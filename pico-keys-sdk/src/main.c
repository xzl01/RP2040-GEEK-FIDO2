/*
 * This file is part of the Pico Keys SDK distribution (https://github.com/polhenarejos/pico-keys-sdk).
 * Copyright (c) 2022 Pol Henarejos.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>

// Pico
#ifndef ENABLE_EMULATION
#include "pico/stdlib.h"
#else
#include <sys/time.h>
#include "emulation.h"
#endif

// For memcpy
#include <string.h>

#ifndef ENABLE_EMULATION
// Include descriptor struct definitions
//#include "usb_common.h"
// USB register definitions from pico-sdk
#include "hardware/regs/usb.h"
// USB hardware struct definitions from pico-sdk
#include "hardware/structs/usb.h"
// For interrupt enable and numbers
#include "hardware/irq.h"
// For resetting the USB controller
#include "hardware/resets.h"

#include "pico/multicore.h"
#endif

#include "random.h"
#include "pico_keys.h"
#include "apdu.h"
#ifdef CYW43_WL_GPIO_LED_PIN
#include "pico/cyw43_arch.h"
#endif
#ifdef PICO_DEFAULT_WS2812_PIN
#include "hardware/pio.h"
#include "hardware/clocks.h"
#define ws2812_wrap_target 0
#define ws2812_wrap 3
#define ws2812_T1 2
#define ws2812_T2 5
#define ws2812_T3 3
static const uint16_t ws2812_program_instructions[] = {
    //     .wrap_target
    0x6221, //  0: out    x, 1            side 0 [2]
    0x1123, //  1: jmp    !x, 3           side 1 [1]
    0x1400, //  2: jmp    0               side 1 [4]
    0xa442, //  3: nop                    side 0 [4]
            //     .wrap
};
static const struct pio_program ws2812_program = {
    .instructions = ws2812_program_instructions,
    .length = 4,
    .origin = -1,
};

static inline pio_sm_config ws2812_program_get_default_config(uint offset) {
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + ws2812_wrap_target, offset + ws2812_wrap);
    sm_config_set_sideset(&c, 1, false, false);
    return c;
}
static inline void ws2812_program_init(PIO pio,
                                       uint sm,
                                       uint offset,
                                       uint pin,
                                       float freq,
                                       bool rgbw) {
    pio_gpio_init(pio, pin);
    pio_sm_set_consecutive_pindirs(pio, sm, pin, 1, true);
    pio_sm_config c = ws2812_program_get_default_config(offset);
    sm_config_set_sideset_pins(&c, pin);
    sm_config_set_out_shift(&c, false, true, rgbw ? 32 : 24);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    int cycles_per_bit = ws2812_T1 + ws2812_T2 + ws2812_T3;
    float div = clock_get_hz(clk_sys) / (freq * cycles_per_bit);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}
#endif

#ifndef ENABLE_EMULATION
#include "usb.h"
#include "hardware/rtc.h"
#include "bsp/board.h"
#endif

extern void do_flash();
extern void low_flash_init();

app_t apps[4];
uint8_t num_apps = 0;

app_t *current_app = NULL;

const uint8_t *ccid_atr = NULL;

int register_app(int (*select_aid)(app_t *), const uint8_t *aid) {
    if (num_apps < sizeof(apps) / sizeof(app_t)) {
        apps[num_apps].select_aid = select_aid;
        apps[num_apps].aid = aid;
        num_apps++;
        return 1;
    }
    return 0;
}

int (*button_pressed_cb)(uint8_t) = NULL;

static uint32_t blink_interval_ms = BLINK_NOT_MOUNTED;

void led_set_blink(uint32_t mode) {
    blink_interval_ms = mode;
}

uint32_t timeout = 0;
void timeout_stop() {
    timeout = 0;
}

void timeout_start() {
    timeout = board_millis();
}

bool is_busy() {
    return timeout > 0;
}

void execute_tasks();

static bool req_button_pending = false;

bool is_req_button_pending() {
    return req_button_pending;
}

uint32_t button_timeout = 15000;
bool cancel_button = false;

#ifdef ENABLE_EMULATION
uint32_t board_millis() {
    struct timeval start;
    gettimeofday(&start, NULL);
    return start.tv_sec * 1000 + start.tv_usec / 1000;
}

#else
bool button_pressed_state = false;
uint32_t button_pressed_time = 0;
uint8_t button_press = 0;
bool wait_button() {
    uint32_t start_button = board_millis();
    bool timeout = false;
    cancel_button = false;
    led_set_blink((1000 << 16) | 100);
    req_button_pending = true;
    while (board_button_read() == false && cancel_button == false) {
        execute_tasks();
        //sleep_ms(10);
        if (start_button + button_timeout < board_millis()) { /* timeout */
            timeout = true;
            break;
        }
    }
    if (!timeout) {
        while (board_button_read() == true && cancel_button == false) {
            execute_tasks();
            //sleep_ms(10);
            if (start_button + 15000 < board_millis()) { /* timeout */
                timeout = true;
                break;
            }
        }
    }
    led_set_blink(BLINK_PROCESSING);
    req_button_pending = false;
    return timeout || cancel_button;
}
#endif

struct apdu apdu;

void led_blinking_task() {
    static uint32_t start_ms = 0;
    static uint8_t led_state = false;
#ifdef PICO_DEFAULT_LED_PIN_INVERTED
    uint32_t interval = !led_state ? blink_interval_ms & 0xffff : blink_interval_ms >> 16;
#else
    uint32_t interval = led_state ? blink_interval_ms & 0xffff : blink_interval_ms >> 16;
#endif
#ifdef PICO_DEFAULT_LED_PIN
    static uint8_t led_color = PICO_DEFAULT_LED_PIN;
#elif defined(PICO_DEFAULT_WS2812_PIN)
#elif defined(CYW43_WL_GPIO_LED_PIN)
    static uint8_t led_color = CYW43_WL_GPIO_LED_PIN;
#endif

    // Blink every interval ms
    if (board_millis() - start_ms < interval) {
        return; // not enough time
    }
    start_ms += interval;

#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(led_color, led_state);
#elif defined(PICO_DEFAULT_WS2812_PIN)
    if (led_state == 0) {
        pio_sm_put_blocking(pio0, 0, 0);
    }
    else {
        pio_sm_put_blocking(pio0, 0, 0xff000000);
    }
#elif defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(led_color, led_state);
#endif
    led_state ^= 1; // toggle
}

void led_off_all() {
#ifdef PIMORONI_TINY2040
    gpio_put(TINY2040_LED_R_PIN, 1);
    gpio_put(TINY2040_LED_G_PIN, 1);
    gpio_put(TINY2040_LED_B_PIN, 1);
#elif defined(PICO_DEFAULT_LED_PIN)
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
#endif
#if (PICO_DEFAULT_WS2812_PIN)
    PIO pio = pio0;
    int sm = 0;
    uint offset = pio_add_program(pio, &ws2812_program);

    ws2812_program_init(pio, sm, offset, PICO_DEFAULT_WS2812_PIN, 800000, true);
#endif
}

void init_rtc() {
#ifndef ENABLE_EMULATION
    rtc_init();
    datetime_t dt = {
        .year  = 2020,
        .month = 1,
        .day   = 1,
        .dotw  = 3,     // 0 is Sunday, so 5 is Friday
        .hour  = 00,
        .min   = 00,
        .sec   = 00
    };
    rtc_set_datetime(&dt);
#endif
}

extern void neug_task();
extern void usb_task();

void execute_tasks() {
    usb_task();
#ifndef ENABLE_EMULATION
    tud_task(); // tinyusb device task
#endif
    led_blinking_task();
}

int main(void) {
#ifndef ENABLE_EMULATION
    usb_init();

    board_init();
    stdio_init_all();

#ifdef PIMORONI_TINY2040
    gpio_init(TINY2040_LED_R_PIN);
    gpio_set_dir(TINY2040_LED_R_PIN, GPIO_OUT);
    gpio_init(TINY2040_LED_G_PIN);
    gpio_set_dir(TINY2040_LED_G_PIN, GPIO_OUT);
    gpio_init(TINY2040_LED_B_PIN);
    gpio_set_dir(TINY2040_LED_B_PIN, GPIO_OUT);
#elif defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_init();
#endif

    led_off_all();

    tusb_init();

    //prepare_ccid();
#else
    emul_init("127.0.0.1", 35963);
#endif

    random_init();

    low_flash_init();

    init_rtc();

    //ccid_prepare_receive(&ccid);

    while (1) {
        execute_tasks();
        neug_task();
        do_flash();
#ifndef ENABLE_EMULATION
        if (board_millis() > 1000 && !is_busy()) { // wait 1 second to boot up
            bool current_button_state = board_button_read();
            if (current_button_state != button_pressed_state) {
                if (current_button_state == false) { // unpressed
                    if (button_pressed_time == 0 || button_pressed_time + 1000 > board_millis()) {
                        button_press++;
                    }
                    button_pressed_time = board_millis();
                }
                button_pressed_state = current_button_state;
            }
            if (button_pressed_time > 0 && button_press > 0 && button_pressed_time + 1000 < board_millis() && button_pressed_state == false) {
                if (button_pressed_cb != NULL) {
                    (*button_pressed_cb)(button_press);
                }
                button_pressed_time = button_press = 0;
            }
        }
#endif
    }

    return 0;
}
