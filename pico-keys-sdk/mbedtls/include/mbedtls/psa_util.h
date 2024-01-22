/**
 * \file psa_util.h
 *
 * \brief Utility functions for the use of the PSA Crypto library.
 */
/*
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
 */

#ifndef MBEDTLS_PSA_UTIL_H
#define MBEDTLS_PSA_UTIL_H
#include "mbedtls/private_access.h"

#include "mbedtls/build_info.h"

#include "psa/crypto.h"

#if defined(MBEDTLS_PSA_CRYPTO_C)

/* Expose whatever RNG the PSA subsystem uses to applications using the
 * mbedtls_xxx API. The declarations and definitions here need to be
 * consistent with the implementation in library/psa_crypto_random_impl.h.
 * See that file for implementation documentation. */


/* The type of a `f_rng` random generator function that many library functions
 * take.
 *
 * This type name is not part of the Mbed TLS stable API. It may be renamed
 * or moved without warning.
 */
typedef int mbedtls_f_rng_t(void *p_rng, unsigned char *output, size_t output_size);

#if defined(MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG)

/** The random generator function for the PSA subsystem.
 *
 * This function is suitable as the `f_rng` random generator function
 * parameter of many `mbedtls_xxx` functions. Use #MBEDTLS_PSA_RANDOM_STATE
 * to obtain the \p p_rng parameter.
 *
 * The implementation of this function depends on the configuration of the
 * library.
 *
 * \note Depending on the configuration, this may be a function or
 *       a pointer to a function.
 *
 * \note This function may only be used if the PSA crypto subsystem is active.
 *       This means that you must call psa_crypto_init() before any call to
 *       this function, and you must not call this function after calling
 *       mbedtls_psa_crypto_free().
 *
 * \param p_rng         The random generator context. This must be
 *                      #MBEDTLS_PSA_RANDOM_STATE. No other state is
 *                      supported.
 * \param output        The buffer to fill. It must have room for
 *                      \c output_size bytes.
 * \param output_size   The number of bytes to write to \p output.
 *                      This function may fail if \p output_size is too
 *                      large. It is guaranteed to accept any output size
 *                      requested by Mbed TLS library functions. The
 *                      maximum request size depends on the library
 *                      configuration.
 *
 * \return              \c 0 on success.
 * \return              An `MBEDTLS_ERR_ENTROPY_xxx`,
 *                      `MBEDTLS_ERR_PLATFORM_xxx,
 *                      `MBEDTLS_ERR_CTR_DRBG_xxx` or
 *                      `MBEDTLS_ERR_HMAC_DRBG_xxx` on error.
 */
int mbedtls_psa_get_random(void *p_rng,
                           unsigned char *output,
                           size_t output_size);

/** The random generator state for the PSA subsystem.
 *
 * This macro expands to an expression which is suitable as the `p_rng`
 * random generator state parameter of many `mbedtls_xxx` functions.
 * It must be used in combination with the random generator function
 * mbedtls_psa_get_random().
 *
 * The implementation of this macro depends on the configuration of the
 * library. Do not make any assumption on its nature.
 */
#define MBEDTLS_PSA_RANDOM_STATE NULL

#else /* !defined(MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG) */

#if defined(MBEDTLS_CTR_DRBG_C)
#include "mbedtls/ctr_drbg.h"
typedef mbedtls_ctr_drbg_context mbedtls_psa_drbg_context_t;
static mbedtls_f_rng_t *const mbedtls_psa_get_random = mbedtls_ctr_drbg_random;
#elif defined(MBEDTLS_HMAC_DRBG_C)
#include "mbedtls/hmac_drbg.h"
typedef mbedtls_hmac_drbg_context mbedtls_psa_drbg_context_t;
static mbedtls_f_rng_t *const mbedtls_psa_get_random = mbedtls_hmac_drbg_random;
#endif
extern mbedtls_psa_drbg_context_t *const mbedtls_psa_random_state;

#define MBEDTLS_PSA_RANDOM_STATE mbedtls_psa_random_state

#endif /* !defined(MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG) */

/** \defgroup psa_tls_helpers TLS helper functions
 * @{
 */
#if defined(PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY)
#include <mbedtls/ecp.h>

/** Convert an ECC curve identifier from the Mbed TLS encoding to PSA.
 *
 * \param grpid         An Mbed TLS elliptic curve identifier
 *                      (`MBEDTLS_ECP_DP_xxx`).
 * \param[out] bits     On success the bit size of the curve; 0 on failure.
 *
 * \return              If the curve is supported in the PSA API, this function
 *                      returns the proper PSA curve identifier
 *                      (`PSA_ECC_FAMILY_xxx`). This holds even if the curve is
 *                      not supported by the ECP module.
 * \return              \c 0 if the curve is not supported in the PSA API.
 */
psa_ecc_family_t mbedtls_ecc_group_to_psa(mbedtls_ecp_group_id grpid,
                                          size_t *bits);

/** Convert an ECC curve identifier from the PSA encoding to Mbed TLS.
 *
 * \param family        A PSA elliptic curve family identifier
 *                      (`PSA_ECC_FAMILY_xxx`).
 * \param bits          The bit-length of a private key on \p curve.
 *
 * \return              If the curve is supported in the PSA API, this function
 *                      returns the corresponding Mbed TLS elliptic curve
 *                      identifier (`MBEDTLS_ECP_DP_xxx`).
 * \return              #MBEDTLS_ECP_DP_NONE if the combination of \c curve
 *                      and \p bits is not supported.
 */
mbedtls_ecp_group_id mbedtls_ecc_group_from_psa(psa_ecc_family_t family,
                                                size_t bits);
#endif /* PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY */

/**
 * \brief           This function returns the PSA algorithm identifier
 *                  associated with the given digest type.
 *
 * \param md_type   The type of digest to search for. Must not be NONE.
 *
 * \warning         If \p md_type is \c MBEDTLS_MD_NONE, this function will
 *                  not return \c PSA_ALG_NONE, but an invalid algorithm.
 *
 * \warning         This function does not check if the algorithm is
 *                  supported, it always returns the corresponding identifier.
 *
 * \return          The PSA algorithm identifier associated with \p md_type,
 *                  regardless of whether it is supported or not.
 */
static inline psa_algorithm_t mbedtls_md_psa_alg_from_type(mbedtls_md_type_t md_type)
{
    return PSA_ALG_CATEGORY_HASH | (psa_algorithm_t) md_type;
}

/**
 * \brief           This function returns the given digest type
 *                  associated with the PSA algorithm identifier.
 *
 * \param psa_alg   The PSA algorithm identifier to search for.
 *
 * \warning         This function does not check if the algorithm is
 *                  supported, it always returns the corresponding identifier.
 *
 * \return          The MD type associated with \p psa_alg,
 *                  regardless of whether it is supported or not.
 */
static inline mbedtls_md_type_t mbedtls_md_type_from_psa_alg(psa_algorithm_t psa_alg)
{
    return (mbedtls_md_type_t) (psa_alg & PSA_ALG_HASH_MASK);
}

/**@}*/

#endif /* MBEDTLS_PSA_CRYPTO_C */
#endif /* MBEDTLS_PSA_UTIL_H */
