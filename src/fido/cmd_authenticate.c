/*
 * This file is part of the Pico FIDO distribution (https://github.com/polhenarejos/pico-fido).
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

#include "fido.h"
#include "pico_keys.h"
#include "apdu.h"
#include "ctap.h"
#include "random.h"
#include "files.h"
#include "credential.h"

int cmd_authenticate() {
    CTAP_AUTHENTICATE_REQ *req = (CTAP_AUTHENTICATE_REQ *) apdu.data;
    CTAP_AUTHENTICATE_RESP *resp = (CTAP_AUTHENTICATE_RESP *) res_APDU;
    //if (scan_files(true) != CCID_OK)
    //    return SW_EXEC_ERROR();
    if (apdu.nc < CTAP_CHAL_SIZE + CTAP_APPID_SIZE + 1 + 1) {
        return SW_WRONG_DATA();
    }
    if (req->keyHandleLen < KEY_HANDLE_LEN) {
        return SW_INCORRECT_PARAMS();
    }
    if (P1(apdu) == CTAP_AUTH_ENFORCE && wait_button_pressed() == true) {
        return SW_CONDITIONS_NOT_SATISFIED();
    }

    mbedtls_ecdsa_context key;
    mbedtls_ecdsa_init(&key);
    int ret = 0;
    uint8_t *tmp_kh = (uint8_t *) calloc(1, req->keyHandleLen);
    memcpy(tmp_kh, req->keyHandle, req->keyHandleLen);
    if (credential_verify(tmp_kh, req->keyHandleLen, req->appId) == 0) {
        ret = fido_load_key(FIDO2_CURVE_P256, req->keyHandle, &key);
    }
    else {
        ret = derive_key(req->appId, false, req->keyHandle, MBEDTLS_ECP_DP_SECP256R1, &key);
        if (verify_key(req->appId, req->keyHandle, &key) != 0) {
            mbedtls_ecdsa_free(&key);
            return SW_INCORRECT_PARAMS();
        }
    }
    free(tmp_kh);
    if (ret != CCID_OK) {
        mbedtls_ecdsa_free(&key);
        return SW_EXEC_ERROR();
    }
    if (P1(apdu) == CTAP_AUTH_CHECK_ONLY) {
        mbedtls_ecdsa_free(&key);
        return SW_CONDITIONS_NOT_SATISFIED();
    }
    resp->flags = 0;
    resp->flags |= P1(apdu) == CTAP_AUTH_ENFORCE ? CTAP_AUTH_FLAG_TUP : 0x0;
    uint32_t ctr = get_sign_counter();
    resp->ctr[0] = ctr >> 24;
    resp->ctr[1] = ctr >> 16;
    resp->ctr[2] = ctr >> 8;
    resp->ctr[3] = ctr & 0xff;
    uint8_t hash[32], sig_base[CTAP_APPID_SIZE + 1 + 4 + CTAP_CHAL_SIZE];
    memcpy(sig_base, req->appId, CTAP_APPID_SIZE);
    memcpy(sig_base + CTAP_APPID_SIZE, &resp->flags, sizeof(uint8_t));
    memcpy(sig_base + CTAP_APPID_SIZE + 1, resp->ctr, 4);
    memcpy(sig_base + CTAP_APPID_SIZE + 1 + 4, req->chal, CTAP_CHAL_SIZE);
    ret =
        mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), sig_base, sizeof(sig_base), hash);
    if (ret != 0) {
        mbedtls_ecdsa_free(&key);
        return SW_EXEC_ERROR();
    }
    size_t olen = 0;
    ret = mbedtls_ecdsa_write_signature(&key,
                                        MBEDTLS_MD_SHA256,
                                        hash,
                                        32,
                                        (uint8_t *) resp->sig,
                                        CTAP_MAX_EC_SIG_SIZE,
                                        &olen,
                                        random_gen,
                                        NULL);
    mbedtls_ecdsa_free(&key);
    if (ret != 0) {
        return SW_EXEC_ERROR();
    }
    res_APDU_size = 1 + 4 + olen;

    ctr++;
    flash_write_data_to_file(ef_counter, (uint8_t *) &ctr, sizeof(ctr));
    low_flash_available();
    return SW_OK();
}
