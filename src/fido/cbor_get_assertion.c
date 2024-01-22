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

#include "cbor.h"
#include "ctap.h"
#ifndef ENABLE_EMULATION
#include "bsp/board.h"
#endif
#include "hid/ctap_hid.h"
#include "fido.h"
#include "files.h"
#include "crypto_utils.h"
#include "pico_keys.h"
#include "apdu.h"
#include "cbor_make_credential.h"
#include "credential.h"
#include "mbedtls/sha256.h"
#include "random.h"

int cbor_get_assertion(const uint8_t *data, size_t len, bool next);

bool residentx = false;
Credential credsx[MAX_CREDENTIAL_COUNT_IN_LIST] = { 0 };
uint8_t credentialCounter = 1;
uint8_t numberOfCredentialsx = 0;
uint8_t flagsx = 0;
uint32_t timerx = 0;
uint8_t *datax = NULL;
size_t lenx = 0;

int cbor_get_next_assertion(const uint8_t *data, size_t len) {
    CborError error = CborNoError;
    if (credentialCounter >= numberOfCredentialsx) {
        CBOR_ERROR(CTAP2_ERR_NOT_ALLOWED);
    }
    if (timerx + 30 * 1000 < board_millis()) {
        CBOR_ERROR(CTAP2_ERR_NOT_ALLOWED);
    }
    CBOR_CHECK(cbor_get_assertion(datax, lenx, true));
    timerx = board_millis();
    credentialCounter++;
err:
    if (error != CborNoError || credentialCounter == numberOfCredentialsx) {
        for (int i = 0; i < MAX_CREDENTIAL_COUNT_IN_LIST; i++) {
            credential_free(&credsx[i]);
        }
        if (datax) {
            free(datax);
            datax = NULL;
        }
        lenx = 0;
        residentx = false;
        timerx = 0;
        flagsx = 0;
        credentialCounter = 0;
        numberOfCredentialsx = 0;
        if (error == CborErrorImproperValue) {
            return CTAP2_ERR_CBOR_UNEXPECTED_TYPE;
        }
        return error;
    }
    return 0;
}

int cbor_get_assertion(const uint8_t *data, size_t len, bool next) {
    size_t resp_size = 0;
    uint64_t pinUvAuthProtocol = 0, hmacSecretPinUvAuthProtocol = 1;
    CredOptions options = { 0 };
    CredExtensions extensions = { 0 };
    CborParser parser;
    CborEncoder encoder, mapEncoder, mapEncoder2;
    CborValue map;
    CborError error = CborNoError;
    CborByteString pinUvAuthParam = { 0 }, clientDataHash = { 0 };
    CborCharString rpId = { 0 };
    PublicKeyCredentialDescriptor allowList[MAX_CREDENTIAL_COUNT_IN_LIST] = { 0 };
    Credential creds[MAX_CREDENTIAL_COUNT_IN_LIST] = { 0 };
    size_t allowList_len = 0, creds_len = 0;
    uint8_t *aut_data = NULL;
    bool asserted = false, up = true, uv = false;
    int64_t kty = 2, alg = 0, crv = 0;
    CborByteString kax = { 0 }, kay = { 0 }, salt_enc = { 0 }, salt_auth = { 0 };
    const bool *credBlob = NULL;

    CBOR_CHECK(cbor_parser_init(data, len, 0, &parser, &map));
    uint64_t val_c = 1;
    CBOR_PARSE_MAP_START(map, 1)
    {
        uint64_t val_u = 0;
        CBOR_FIELD_GET_UINT(val_u, 1);
        if (val_c <= 2 && val_c != val_u) {
            CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
        }
        if (val_u < val_c) {
            CBOR_ERROR(CTAP2_ERR_INVALID_CBOR);
        }
        val_c = val_u + 1;
        if (val_u == 0x01) {
            CBOR_FIELD_GET_TEXT(rpId, 1);
        }
        else if (val_u == 0x02) {
            CBOR_FIELD_GET_BYTES(clientDataHash, 1);
        }
        else if (val_u == 0x03) { // excludeList
            CBOR_PARSE_ARRAY_START(_f1, 2)
            {
                PublicKeyCredentialDescriptor *pc = &allowList[allowList_len];
                CBOR_PARSE_MAP_START(_f2, 3)
                {
                    CBOR_FIELD_GET_KEY_TEXT(3);
                    CBOR_FIELD_KEY_TEXT_VAL_BYTES(3, "id", pc->id);
                    CBOR_FIELD_KEY_TEXT_VAL_TEXT(3, "type", pc->type);
                    if (strcmp(_fd3, "transports") == 0) {
                        CBOR_PARSE_ARRAY_START(_f3, 4)
                        {
                            CBOR_FIELD_GET_TEXT(pc->transports[pc->transports_len], 4);
                            pc->transports_len++;
                        }
                        CBOR_PARSE_ARRAY_END(_f3, 4);
                    }
                }
                CBOR_PARSE_MAP_END(_f2, 3);
                allowList_len++;
            }
            CBOR_PARSE_ARRAY_END(_f1, 2);
        }
        else if (val_u == 0x04) { // extensions
            extensions.present = true;
            CBOR_PARSE_MAP_START(_f1, 2)
            {
                CBOR_FIELD_GET_KEY_TEXT(2);
                if (strcmp(_fd2, "hmac-secret") == 0) {
                    extensions.hmac_secret = ptrue;
                    uint64_t ukey = 0;
                    CBOR_PARSE_MAP_START(_f2, 3)
                    {
                        CBOR_FIELD_GET_UINT(ukey, 3);
                        if (ukey == 0x01) {
                            CBOR_CHECK(COSE_read_key(&_f3, &kty, &alg, &crv, &kax, &kay));
                        }
                        else if (ukey == 0x02) {
                            CBOR_FIELD_GET_BYTES(salt_enc, 3);
                        }
                        else if (ukey == 0x03) {
                            CBOR_FIELD_GET_BYTES(salt_auth, 3);
                        }
                        else if (ukey == 0x04) {
                            CBOR_FIELD_GET_UINT(hmacSecretPinUvAuthProtocol, 3);
                        }
                        else {
                            CBOR_ADVANCE(3);
                        }
                    }
                    CBOR_PARSE_MAP_END(_f2, 3);
                    continue;
                }
                CBOR_FIELD_KEY_TEXT_VAL_BOOL(2, "credBlob", credBlob);
                CBOR_FIELD_KEY_TEXT_VAL_BOOL(2, "largeBlobKey", extensions.largeBlobKey);
                CBOR_FIELD_KEY_TEXT_VAL_BOOL(2, "thirdPartyPayment", extensions.thirdPartyPayment);
                CBOR_ADVANCE(2);
            }
            CBOR_PARSE_MAP_END(_f1, 2);
        }
        else if (val_u == 0x05) { // options
            options.present = true;
            CBOR_PARSE_MAP_START(_f1, 2)
            {
                CBOR_FIELD_GET_KEY_TEXT(2);
                CBOR_FIELD_KEY_TEXT_VAL_BOOL(2, "rk", options.rk);
                CBOR_FIELD_KEY_TEXT_VAL_BOOL(2, "up", options.up);
                CBOR_FIELD_KEY_TEXT_VAL_BOOL(2, "uv", options.uv);
                CBOR_ADVANCE(2);
            }
            CBOR_PARSE_MAP_END(_f1, 2);
        }
        else if (val_u == 0x06) { // pinUvAuthParam
            CBOR_FIELD_GET_BYTES(pinUvAuthParam, 1);
        }
        else if (val_u == 0x07) { // pinUvAuthProtocol
            CBOR_FIELD_GET_UINT(pinUvAuthProtocol, 1);
        }
    }
    CBOR_PARSE_MAP_END(map, 1);

    if (rpId.present == false || clientDataHash.present == false) {
        CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
    }

    uint8_t flags = 0;
    uint8_t rp_id_hash[32];
    mbedtls_sha256((uint8_t *) rpId.data, rpId.len, rp_id_hash, 0);

    bool resident = false;
    uint8_t numberOfCredentials = 0;
    Credential *selcred = NULL;
    if (next == false) {
        if (pinUvAuthParam.present == true) {
            if (pinUvAuthParam.len == 0 || pinUvAuthParam.data == NULL) {
                if (check_user_presence() == false) {
                    CBOR_ERROR(CTAP2_ERR_OPERATION_DENIED);
                }
                if (!file_has_data(ef_pin)) {
                    CBOR_ERROR(CTAP2_ERR_PIN_NOT_SET);
                }
                else {
                    CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
                }
            }
            else {
                if (pinUvAuthProtocol == 0) {
                    CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
                }
                if (pinUvAuthProtocol != 1 && pinUvAuthProtocol != 2) {
                    CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
                }
            }
        }
        if (options.present) {
            if (options.uv == ptrue) { //4.3
                CBOR_ERROR(CTAP2_ERR_INVALID_OPTION);
            }
            //if (options.up != NULL) { //4.5
            //    CBOR_ERROR(CTAP2_ERR_INVALID_OPTION);
            //}
            if (options.rk != NULL) {
                CBOR_ERROR(CTAP2_ERR_UNSUPPORTED_OPTION);
            }
            //else if (options.up == NULL) //5.7
            //rup = ptrue;
            if (options.uv != NULL) {
                uv = *options.uv;
            }
            if (options.up != NULL) {
                up = *options.up;
            }
        }

        if (pinUvAuthParam.present == true) { //6.1
            int ret = verify(pinUvAuthProtocol,
                             paut.data,
                             clientDataHash.data,
                             clientDataHash.len,
                             pinUvAuthParam.data);
            if (ret != CborNoError) {
                CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
            }
            if (getUserVerifiedFlagValue() == false) {
                CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
            }
            if (!(paut.permissions & CTAP_PERMISSION_GA)) {
                CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
            }
            if (paut.has_rp_id == true && memcmp(paut.rp_id_hash, rp_id_hash, 32) != 0) {
                CBOR_ERROR(CTAP2_ERR_PIN_AUTH_INVALID);
            }
            flags |= FIDO2_AUT_FLAG_UV;
            // Check pinUvAuthToken permissions. See 6.2.2.4
        }
        if (extensions.present == true && extensions.hmac_secret == ptrue) {
            if (kax.present == false || kay.present == false || crv == 0 || alg == 0 ||
                salt_enc.present == false || salt_auth.present == false) {
                CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
            }
            if (salt_enc.len != 32 + (hmacSecretPinUvAuthProtocol - 1) * IV_SIZE &&
                salt_enc.len != 64 + (hmacSecretPinUvAuthProtocol - 1) * IV_SIZE) {
                CBOR_ERROR(CTAP1_ERR_INVALID_LEN);
            }
        }

        if (allowList_len > 0) {
            for (int e = 0; e < allowList_len; e++) {
                if (allowList[e].type.present == false || allowList[e].id.present == false) {
                    CBOR_ERROR(CTAP2_ERR_MISSING_PARAMETER);
                }
                if (strcmp(allowList[e].type.data, "public-key") != 0) {
                    continue;
                }
                if (credential_load(allowList[e].id.data, allowList[e].id.len, rp_id_hash,
                                    &creds[creds_len]) != 0) {
                    CBOR_FREE_BYTE_STRING(allowList[e].id);
                    credential_free(&creds[creds_len]);
                }
                else {
                    creds_len++;
                }
            }
        }
        else {
            for (int i = 0;
                 i < MAX_RESIDENT_CREDENTIALS && creds_len < MAX_CREDENTIAL_COUNT_IN_LIST;
                 i++) {
                file_t *ef = search_dynamic_file(EF_CRED + i);
                if (!file_has_data(ef) || memcmp(file_get_data(ef), rp_id_hash, 32) != 0) {
                    continue;
                }
                int ret = credential_load(file_get_data(ef) + 32,
                                          file_get_size(ef) - 32,
                                          rp_id_hash,
                                          &creds[creds_len]);
                if (ret != 0) {
                    credential_free(&creds[creds_len]);
                }
                else {
                    creds_len++;
                }
            }
            resident = true;
        }
        for (int i = 0; i < creds_len; i++) {
            if (creds[i].present == true) {
                if (creds[i].extensions.present == true) {
                    if (creds[i].extensions.credProtect == CRED_PROT_UV_REQUIRED &&
                        !(flags & FIDO2_AUT_FLAG_UV)) {
                        credential_free(&creds[i]);
                    }
                    else if (creds[i].extensions.credProtect == CRED_PROT_UV_OPTIONAL_WITH_LIST &&
                             resident == true && !(flags & FIDO2_AUT_FLAG_UV)) {
                        credential_free(&creds[i]);
                    }
                    else {
                        creds[numberOfCredentials++] = creds[i];
                    }
                }
                else {
                    creds[numberOfCredentials++] = creds[i];
                }
            }
        }
        if (numberOfCredentials == 0) {
            CBOR_ERROR(CTAP2_ERR_NO_CREDENTIALS);
        }

        for (int i = 0; i < numberOfCredentials; i++) {
            for (int j = i + 1; j < numberOfCredentials; j++) {
                if (creds[j].creation > creds[i].creation) {
                    Credential tmp = creds[j];
                    creds[j] = creds[i];
                    creds[i] = tmp;
                }
            }
        }

        if (options.up == ptrue || options.present == false || options.up == NULL) { //9.1
            if (pinUvAuthParam.present == true) {
                if (getUserPresentFlagValue() == false) {
                    if (check_user_presence() == false) {
                        CBOR_ERROR(CTAP2_ERR_OPERATION_DENIED);
                    }
                }
            }
            else {
                if (!(flags & FIDO2_AUT_FLAG_UP)) {
                    if (check_user_presence() == false) {
                        CBOR_ERROR(CTAP2_ERR_OPERATION_DENIED);
                    }
                }
            }
            flags |= FIDO2_AUT_FLAG_UP;
            clearUserPresentFlag();
            clearUserVerifiedFlag();
            clearPinUvAuthTokenPermissionsExceptLbw();
        }

        if (extensions.largeBlobKey == pfalse) {
            CBOR_ERROR(CTAP2_ERR_INVALID_OPTION);
        }

        if (up == false && uv == false) {
            selcred = &creds[0];
        }
        else {
            selcred = &creds[0];
            if (numberOfCredentials > 1) {
                asserted = true;
                residentx = resident;
                for (int i = 0; i < MAX_CREDENTIAL_COUNT_IN_LIST; i++) {
                    credsx[i] = creds[i];
                }
                numberOfCredentialsx = numberOfCredentials;
                datax = (uint8_t *) calloc(1, len);
                memcpy(datax, data, len);
                lenx = len;
                flagsx = flags;
                timerx = board_millis();
                credentialCounter = 1;
            }
        }
    }
    else {
        resident = residentx;
        numberOfCredentials = numberOfCredentialsx;
        flags = flagsx;
        selcred = &credsx[credentialCounter];
    }
    mbedtls_ecdsa_context ekey;
    mbedtls_ecdsa_init(&ekey);
    int ret = fido_load_key(selcred->curve, selcred->id.data, &ekey);
    if (ret != 0) {
        if (derive_key(rp_id_hash, false, selcred->id.data, MBEDTLS_ECP_DP_SECP256R1, &ekey) != 0) {
            mbedtls_ecdsa_free(&ekey);
            CBOR_ERROR(CTAP1_ERR_OTHER);
        }
    }

    uint8_t largeBlobKey[32];
    if (extensions.largeBlobKey == ptrue && selcred->extensions.largeBlobKey == ptrue) {
        ret = credential_derive_large_blob_key(selcred->id.data, selcred->id.len, largeBlobKey);
        if (ret != 0) {
            CBOR_ERROR(CTAP2_ERR_PROCESSING);
        }
    }

    size_t ext_len = 0;
    uint8_t ext[512];
    if (extensions.present == true) {
        cbor_encoder_init(&encoder, ext, sizeof(ext), 0);
        int l = 0;
        if (options.up == pfalse) {
            extensions.hmac_secret = NULL;
        }
        if (extensions.hmac_secret != NULL) {
            l++;
        }
        if (credBlob == ptrue) {
            l++;
        }
        if (extensions.thirdPartyPayment != NULL) {
            l++;
        }
        CBOR_CHECK(cbor_encoder_create_map(&encoder, &mapEncoder, l));
        if (credBlob == ptrue) {
            CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder, "credBlob"));
            if (selcred->extensions.credBlob.present == true) {
                CBOR_CHECK(cbor_encode_byte_string(&mapEncoder, selcred->extensions.credBlob.data,
                                                   selcred->extensions.credBlob.len));
            }
            else {
                CBOR_CHECK(cbor_encode_byte_string(&mapEncoder, NULL, 0));
            }
        }
        if (extensions.hmac_secret != NULL) {

            CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder, "hmac-secret"));

            uint8_t sharedSecret[64];
            mbedtls_ecp_point Qp;
            mbedtls_ecp_point_init(&Qp);
            mbedtls_mpi_lset(&Qp.Z, 1);
            if (mbedtls_mpi_read_binary(&Qp.X, kax.data, kax.len) != 0) {
                mbedtls_ecp_point_free(&Qp);
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }
            if (mbedtls_mpi_read_binary(&Qp.Y, kay.data, kay.len) != 0) {
                mbedtls_ecp_point_free(&Qp);
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }
            int ret = ecdh(hmacSecretPinUvAuthProtocol, &Qp, sharedSecret);
            mbedtls_ecp_point_free(&Qp);
            if (ret != 0) {
                mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }
            if (verify(hmacSecretPinUvAuthProtocol, sharedSecret, salt_enc.data, salt_enc.len,
                       salt_auth.data) != 0) {
                mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
                CBOR_ERROR(CTAP2_ERR_EXTENSION_FIRST);
            }
            uint8_t salt_dec[64], poff = (hmacSecretPinUvAuthProtocol - 1) * IV_SIZE;
            ret = decrypt(hmacSecretPinUvAuthProtocol,
                          sharedSecret,
                          salt_enc.data,
                          salt_enc.len,
                          salt_dec);
            if (ret != 0) {
                mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }
            uint8_t cred_random[64], *crd = NULL;
            ret = credential_derive_hmac_key(selcred->id.data, selcred->id.len, cred_random);
            if (ret != 0) {
                mbedtls_platform_zeroize(sharedSecret, sizeof(sharedSecret));
                CBOR_ERROR(CTAP1_ERR_INVALID_PARAMETER);
            }
            if (flags & FIDO2_AUT_FLAG_UV) {
                crd = cred_random + 32;
            }
            else {
                crd = cred_random;
            }
            uint8_t out1[64], hmac_res[80];
            mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                            crd,
                            32,
                            salt_dec,
                            32,
                            out1);
            if (salt_enc.len == 64 + poff) {
                mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                crd,
                                32,
                                salt_dec + 32,
                                32,
                                out1 + 32);
            }
            encrypt(hmacSecretPinUvAuthProtocol, sharedSecret, out1, salt_enc.len - poff, hmac_res);
            CBOR_CHECK(cbor_encode_byte_string(&mapEncoder, hmac_res, salt_enc.len));
        }
        if (extensions.thirdPartyPayment != NULL) {
            CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder, "thirdPartyPayment"));
            if (selcred->extensions.thirdPartyPayment == ptrue) {
                CBOR_CHECK(cbor_encode_boolean(&mapEncoder, true));
            }
            else {
                CBOR_CHECK(cbor_encode_boolean(&mapEncoder, false));
            }
        }

        CBOR_CHECK(cbor_encoder_close_container(&encoder, &mapEncoder));
        ext_len = cbor_encoder_get_buffer_size(&encoder, ext);
        flags |= FIDO2_AUT_FLAG_ED;
    }

    uint32_t ctr = get_sign_counter();

    size_t aut_data_len = 32 + 1 + 4 + ext_len;
    aut_data = (uint8_t *) calloc(1, aut_data_len + clientDataHash.len);
    uint8_t *pa = aut_data;
    memcpy(pa, rp_id_hash, 32); pa += 32;
    *pa++ = flags;
    *pa++ = ctr >> 24;
    *pa++ = ctr >> 16;
    *pa++ = ctr >> 8;
    *pa++ = ctr & 0xff;
    memcpy(pa, ext, ext_len); pa += ext_len;
    if (pa - aut_data != aut_data_len) {
        CBOR_ERROR(CTAP1_ERR_OTHER);
    }

    memcpy(pa, clientDataHash.data, clientDataHash.len);
    uint8_t hash[64], sig[MBEDTLS_ECDSA_MAX_LEN];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (ekey.grp.id == MBEDTLS_ECP_DP_SECP384R1) {
        md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA384);
    }
    else if (ekey.grp.id == MBEDTLS_ECP_DP_SECP521R1) {
        md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    }
    ret = mbedtls_md(md,
                     aut_data,
                     aut_data_len + clientDataHash.len,
                     hash);
    size_t olen = 0;
    ret = mbedtls_ecdsa_write_signature(&ekey,
                                        mbedtls_md_get_type(md),
                                        hash,
                                        mbedtls_md_get_size(md),
                                        sig,
                                        sizeof(sig),
                                        &olen,
                                        random_gen,
                                        NULL);
    mbedtls_ecdsa_free(&ekey);

    uint8_t lfields = 3;
    if (selcred->opts.present == true && selcred->opts.rk == ptrue) {
        lfields++;
    }
    if (numberOfCredentials > 1 && next == false) {
        lfields++;
    }
    if (extensions.largeBlobKey == ptrue && selcred->extensions.largeBlobKey == ptrue) {
        lfields++;
    }
    cbor_encoder_init(&encoder, ctap_resp->init.data + 1, CTAP_MAX_PACKET_SIZE, 0);
    CBOR_CHECK(cbor_encoder_create_map(&encoder, &mapEncoder, lfields));

    CBOR_CHECK(cbor_encode_uint(&mapEncoder, 0x01));
    CBOR_CHECK(cbor_encoder_create_map(&mapEncoder, &mapEncoder2, 2));
    CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder2, "id"));
    CBOR_CHECK(cbor_encode_byte_string(&mapEncoder2, selcred->id.data, selcred->id.len));
    CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder2, "type"));
    CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder2, "public-key"));
    CBOR_CHECK(cbor_encoder_close_container(&mapEncoder, &mapEncoder2));

    CBOR_CHECK(cbor_encode_uint(&mapEncoder, 0x02));
    CBOR_CHECK(cbor_encode_byte_string(&mapEncoder, aut_data, aut_data_len));
    CBOR_CHECK(cbor_encode_uint(&mapEncoder, 0x03));
    CBOR_CHECK(cbor_encode_byte_string(&mapEncoder, sig, olen));

    if (selcred->opts.present == true && selcred->opts.rk == ptrue) {
        CBOR_CHECK(cbor_encode_uint(&mapEncoder, 0x04));
        uint8_t lu = 1;
        if (numberOfCredentials > 1 && allowList_len == 0) {
            if (selcred->userName.present == true) {
                lu++;
            }
            if (selcred->userDisplayName.present == true) {
                lu++;
            }
        }
        CBOR_CHECK(cbor_encoder_create_map(&mapEncoder, &mapEncoder2, lu));
        CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder2, "id"));
        CBOR_CHECK(cbor_encode_byte_string(&mapEncoder2, selcred->userId.data,
                                           selcred->userId.len));
        if (numberOfCredentials > 1 && allowList_len == 0) {
            if (selcred->userName.present == true) {
                CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder2, "name"));
                CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder2, selcred->userName.data));
            }
            if (selcred->userDisplayName.present == true) {
                CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder2, "displayName"));
                CBOR_CHECK(cbor_encode_text_stringz(&mapEncoder2, selcred->userDisplayName.data));
            }
        }
        CBOR_CHECK(cbor_encoder_close_container(&mapEncoder, &mapEncoder2));
    }
    if (numberOfCredentials > 1 && next == false) {
        CBOR_CHECK(cbor_encode_uint(&mapEncoder, 0x05));
        CBOR_CHECK(cbor_encode_uint(&mapEncoder, numberOfCredentials));
    }
    if (extensions.largeBlobKey == ptrue && selcred->extensions.largeBlobKey == ptrue) {
        CBOR_CHECK(cbor_encode_uint(&mapEncoder, 0x07));
        CBOR_CHECK(cbor_encode_byte_string(&mapEncoder, largeBlobKey, sizeof(largeBlobKey)));
    }
    mbedtls_platform_zeroize(largeBlobKey, sizeof(largeBlobKey));
    CBOR_CHECK(cbor_encoder_close_container(&encoder, &mapEncoder));
    resp_size = cbor_encoder_get_buffer_size(&encoder, ctap_resp->init.data + 1);
    ctr++;
    flash_write_data_to_file(ef_counter, (uint8_t *) &ctr, sizeof(ctr));
    low_flash_available();
err:
    CBOR_FREE_BYTE_STRING(clientDataHash);
    CBOR_FREE_BYTE_STRING(pinUvAuthParam);
    CBOR_FREE_BYTE_STRING(rpId);
    if (asserted == false) {
        for (int i = 0; i < MAX_CREDENTIAL_COUNT_IN_LIST; i++) {
            credential_free(&creds[i]);
        }
    }

    for (int m = 0; m < allowList_len; m++) {
        CBOR_FREE_BYTE_STRING(allowList[m].type);
        CBOR_FREE_BYTE_STRING(allowList[m].id);
        for (int n = 0; n < allowList[m].transports_len; n++) {
            CBOR_FREE_BYTE_STRING(allowList[m].transports[n]);
        }
    }
    if (aut_data) {
        free(aut_data);
    }
    if (error != CborNoError) {
        if (error == CborErrorImproperValue) {
            return CTAP2_ERR_CBOR_UNEXPECTED_TYPE;
        }
        return error;
    }
    res_APDU_size = resp_size;
    return 0;
}
