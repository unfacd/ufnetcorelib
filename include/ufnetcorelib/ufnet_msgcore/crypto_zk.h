/**
 * @file crypto_zk.h
 * @brief Server certificate and credential issuance startup.
 */
#ifndef UFNETCORELIB_CRYPTO_ZK_H
#define UFNETCORELIB_CRYPTO_ZK_H

#include <ufnetcorelib/ufnetcorelib_defs.h>

/**
 * @brief Load the server certificate and credential issuance parameters.
 * @code{.c}
 * InitialiseUfsrvCrypto();
 * @endcode
 */
PUBLIC_API void InitialiseUfsrvCrypto(void);

#endif
