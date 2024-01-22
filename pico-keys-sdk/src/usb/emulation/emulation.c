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

#include "emulation.h"
#include <stdio.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#include "pico_keys.h"
#include "apdu.h"
#include "usb.h"
#include "ccid/ccid.h"
#include <netinet/tcp.h>

int ccid_sock = 0;
int hid_server_sock = 0;
int hid_client_sock = -1;
extern uint8_t thread_type;
extern const uint8_t *cbor_data;
extern size_t cbor_len;
extern uint8_t cmd;
extern int cbor_parse(uint8_t cmd, const uint8_t *data, size_t len);

int msleep(long msec) {
    struct timespec ts;
    int res;

    if (msec < 0) {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

int emul_init(char *host, uint16_t port) {
    struct sockaddr_in serv_addr;
    fprintf(stderr, "\n Starting emulation envionrment\n");
    if ((ccid_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // Convert IPv4 and IPv6 addresses from text to binary
    // form
    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(ccid_sock);
        return -1;
    }

    if (connect(ccid_sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(ccid_sock);
        return -1;
    }
    int x = fcntl(ccid_sock, F_GETFL, 0);
    fcntl(ccid_sock, F_SETFL, x | O_NONBLOCK);
    int flag = 1;
    setsockopt(ccid_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

    // HID server

    socklen_t yes = 1;
    uint16_t hid_port = port - 1;
    struct sockaddr_in server_sockaddr;

    if ((hid_server_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(hid_server_sock, SOL_SOCKET, SO_REUSEADDR, (void *) &yes, sizeof yes) != 0) {
        perror("setsockopt");
        close(hid_server_sock);
        return 1;
    }

#if HAVE_DECL_SO_NOSIGPIPE
    if (setsockopt(hid_server_sock, SOL_SOCKET, SO_NOSIGPIPE, (void *) &yes, sizeof yes) != 0) {
        perror("setsockopt");
        close(hid_server_sock);
        return 1;
    }
#endif

    memset(&server_sockaddr, 0, sizeof server_sockaddr);
    server_sockaddr.sin_family = PF_INET;
    server_sockaddr.sin_port = htons(hid_port);
    server_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(hid_server_sock, (struct sockaddr *) &server_sockaddr,
             sizeof server_sockaddr) != 0) {
        perror("bind");
        close(hid_server_sock);
        return 1;
    }

    if (listen(hid_server_sock, 0) != 0) {
        perror("listen");
        close(hid_server_sock);
        return 1;
    }
    return 0;
}

uint8_t *driver_prepare_response_emul(uint8_t itf) {
    apdu.rdata = usb_get_tx(itf);
#ifdef USB_ITF_HID
    if (itf == ITF_HID) {
        apdu.rdata += 7;
    }
#endif
    return apdu.rdata;
}

int get_sock_itf(uint8_t itf) {
#ifdef USB_ITF_CCID
    if (itf == ITF_CCID) {
        return ccid_sock;
    }
#endif
#ifdef USB_ITF_HID
    if (itf == ITF_HID) {
        return hid_client_sock;
    }
#endif
    return -1;
}

extern void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len);
const uint8_t *complete_report = NULL;
uint16_t complete_len = 0;
extern bool last_write_result;
extern uint16_t send_buffer_size[ITF_TOTAL];
int driver_write_emul(uint8_t itf, const uint8_t *buffer, size_t buffer_size) {
    uint16_t size = htons(buffer_size);
    int sock = get_sock_itf(itf);
    // DEBUG_PAYLOAD(buffer,buffer_size);
    int ret = 0;
    do {
        ret = send(sock, &size, sizeof(size), 0);
        if (ret < 0) {
            msleep(10);
        }
    } while (ret <= 0);
    do {
        ret = send(sock, buffer, (uint16_t) buffer_size, 0);
        if (ret < 0) {
            msleep(10);
        }
    } while (ret <= 0);
#ifdef USB_ITF_HID
    if (itf == ITF_HID) {
        last_write_result = true;
        complete_report = buffer;
        complete_len = buffer_size;
    }
#endif
    return buffer_size;
}

uint32_t emul_write_offset(uint8_t itf, uint16_t size, uint16_t offset) {
    if (size > 0) {
        //DEBUG_PAYLOAD(usb_get_tx(itf)+offset, size);
        return usb_write_offset(itf, size, offset);
    }
    return 0;
}

uint32_t emul_write(uint8_t itf, uint16_t size) {
    return emul_write_offset(itf, size, 0);
}

void driver_exec_finished_cont_emul(uint8_t itf, size_t size_next, size_t offset) {
#ifdef USB_ITF_HID
    if (itf == ITF_HID) {
        driver_exec_finished_cont_hid(size_next, offset);
    }
#endif
#ifdef USB_ITF_CCID
    if (itf == ITF_CCID) {
        emul_write_offset(itf, size_next, offset);
    }
#endif
}

int driver_process_usb_packet_emul(uint8_t itf, uint16_t len) {
    if (len > 0) {
        uint8_t *data = usb_get_rx(itf), *rdata = usb_get_tx(itf);
#ifdef USB_ITF_CCID
        if (itf == ITF_CCID) {
            if (len == 1) {
                uint8_t c = data[0];
                if (c == 4) {
                    if (ccid_atr) {
                        memcpy(rdata, ccid_atr + 1, ccid_atr[0]);
                    }
                    driver_write_emul(itf, ccid_atr ? ccid_atr + 1 : NULL, ccid_atr ? ccid_atr[0] : 0);
                }
            }
            else {
                size_t sent = 0;
                DEBUG_PAYLOAD(data, len);
                if ((sent = apdu_process(itf, data, len)) > 0) {
                    process_apdu();
                }
                apdu_finish();
                if (sent > 0) {
                    size_t ret = apdu_next();
                    DEBUG_PAYLOAD(rdata, ret);
                    emul_write(itf, ret);
                }
            }
        }
    #endif
    #ifdef USB_ITF_HID
        if (itf == ITF_HID) {
            if (driver_process_usb_packet_hid(len) > 0) {
                if (thread_type == 1) {
                    process_apdu();
                    apdu_finish();
                    finished_data_size = apdu_next();
                }
                else if (thread_type == 2) {
                    apdu.sw = cbor_parse(cmd, cbor_data, cbor_len);
                    if (apdu.sw == 0) {
                        DEBUG_DATA(res_APDU + 1, res_APDU_size);
                    }

                    finished_data_size = res_APDU_size + 1;
                }
                driver_exec_finished_hid(finished_data_size);
            }
        }
#endif
    }
    usb_clear_rx(itf);
    return 0;
}

uint16_t emul_read(uint8_t itf) {
    /* First we look for a client */
#ifdef USB_ITF_HID
    if (itf == ITF_HID) {
        struct sockaddr_in client_sockaddr;
        socklen_t client_socklen = sizeof client_sockaddr;

        int timeout;
        struct pollfd pfd;

        pfd.fd = hid_server_sock;
        pfd.events = POLLIN;
        pfd.revents = 0;

        timeout = (0 * 1000 + 1000 / 1000);

        if (poll(&pfd, 1, timeout) == -1) {
            return 0;
        }

        if (pfd.revents & POLLIN) {
            if (hid_client_sock > 0) {
                close(hid_client_sock);
            }
            hid_client_sock = accept(hid_server_sock, (struct sockaddr *) &client_sockaddr,
                                     &client_socklen);
            printf("hid_client connected!\n");
        }
        if (send_buffer_size > 0) {
            last_write_result = true;
            tud_hid_report_complete_cb(ITF_HID, complete_report, complete_len);
        }
    }
#endif
    int sock = get_sock_itf(itf);
    //printf("get_sockt itf %d - %d\n", itf, sock);
    uint16_t len = 0;
    fd_set input;
    FD_ZERO(&input);
    FD_SET(sock, &input);
    struct timeval timeout;
    timeout.tv_sec  = 0;
    timeout.tv_usec = 0 * 1000;
    int n = select(sock + 1, &input, NULL, NULL, &timeout);
    if (n == -1) {
        //printf("read wrong [itf:%d]\n", itf);
        //something wrong
    }
    else if (n == 0) {
        //printf("read timeout [itf:%d]\n", itf);
    }
    if (FD_ISSET(sock, &input)) {
        int valread = recv(sock, &len, sizeof(len), 0);
        len = ntohs(len);
        if (len > 0) {
            while (true) {
                valread = recv(sock, usb_get_rx(itf), len, 0);
                if (valread > 0) {
                    return valread;
                }
                msleep(10);
            }
        }
    }
    //else
    //    printf("no input for sock %d - %d\n", itf, sock);
    return 0;
}
