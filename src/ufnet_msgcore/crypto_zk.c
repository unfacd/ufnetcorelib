/**
 * @file crypto_zk.c
 * @brief Initialise the server certificate and credential issuance parameters.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 * Created by ayman on 18/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <uflib/standard_defs.h>
#include <uflib/utils_base64.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h>

#include "ufnet_core/config_nonpublic.h"
#include "ufnet_msgcore/crypto_zk.h"
#include "ufnet_msgcore/zkgroup_utils/zkgroup_server_params_type.h"

static int
sInitServerCertificates(void)
{
  int            size_out = 0;
  unsigned char *key_raw;

  //todo: these should be defined out of config file. Private key can be fed via command line
  key_raw = base64_decode((const unsigned char*)SERVER_PRIVATEKEY, strlen(SERVER_PRIVATEKEY), &size_out);
  if (IS_EMPTY(key_raw)) {
    syslog(LOG_ERR, "%s: ERROR (size: '%d'): COULD NOT DECODE PRIVATE KEY PARAMS: TERMINATING...", __func__,
           size_out);
    return -1;
  }
  UfsrvConfigSetCryptoPrivateKeyServer(&(UfsrvConfigItem){
                                         .value = {.stored_value.blob_val = key_raw, .type_allocation_sz = size_out}
                                       }, UFSRV_CONFIG_SOURCE_RUNTIME);
  memset(key_raw, '\0', size_out);
  free(key_raw);

  key_raw = base64_decode((const unsigned char*)SERVER_PUBLICKEY, strlen(SERVER_PUBLICKEY), &size_out);
  if (IS_EMPTY(key_raw)) {
    syslog(LOG_ERR, "%s: ERROR (size: '%d'): COULD NOT DECODE SERVER_PUBLICKEY PARAMS: TERMINATING...",
           __func__, size_out);
    return -1;
  }
  UfsrvConfigSetCryptoPublicKeyServer(&(UfsrvConfigItem){
                                        .value = {.stored_value.blob_val = key_raw, .type_allocation_sz = size_out}
                                      }, UFSRV_CONFIG_SOURCE_RUNTIME);
  free(key_raw);

  key_raw = base64_decode((const unsigned char*)SERVER_PUBLICKEY_SERIALISED, strlen(SERVER_PUBLICKEY_SERIALISED),
                          &size_out);
  if (IS_EMPTY(key_raw)) {
    syslog(
      LOG_ERR,
      "%s: ERROR (size: '%d'): COULD NOT DECODE SERVER_PUBLICKEY_SERIALISED PARAMS: TERMINATING...",
      __func__, size_out);
    return -1;
  }

  UfsrvConfigSetCryptoPublicKeyServerSerialised(&(UfsrvConfigItem){
                                                  .value = {
                                                    .stored_value.blob_val = key_raw, .type_allocation_sz = size_out
                                                  }
                                                }, UFSRV_CONFIG_SOURCE_RUNTIME);
  free(key_raw);

  UfsrvConfigSetServerKeyid(&(UfsrvConfigItem){.value = {.stored_value.int_val = SERVER_KEYID}},
                            UFSRV_CONFIG_SOURCE_RUNTIME);

  return 0;
}

static void
sInitCredentialsIssuanceServerParams(void)
{
  int            size_out = 0;
  unsigned char *decoded;

  decoded = base64_decode((const unsigned char*)PRIVATE_SERVER_PARAM, strlen(PRIVATE_SERVER_PARAM), &size_out);
  if (size_out != SERVER_SECRET_PARAMS_SIZE) {
    syslog(LOG_ERR, "%s: ERROR (size: '%d'): COULD NOT DECODE PRIVATE SERVER PARAMS: TERMINATING...",
           __func__, size_out);

    free(decoded);
    exit(-1);
  }
  UfsrvConfigSetCryptoPrivateServerParams(&(UfsrvConfigItem){
                                            .value = {
                                              .stored_value.blob_val = decoded,
                                              .type_allocation_sz    = SERVER_SECRET_PARAMS_SIZE
                                            }
                                          }, UFSRV_CONFIG_SOURCE_RUNTIME);

  free(decoded);

  size_out = 0;
  decoded  = base64_decode((const unsigned char*)PUBLIC_SERVER_PARAM, strlen(PUBLIC_SERVER_PARAM), &size_out);
  if (size_out != SERVER_PUBLIC_PARAMS_SIZE) {
    syslog(LOG_ERR, "%s: ERROR (size: '%d'): COULD NOT DECODE PUBLIC SERVER PARAMS: TERMINATING...",
           __func__, size_out);

    free(decoded);
    exit(-1);
  }
  UfsrvConfigSetCryptoPublicServerParams(&(UfsrvConfigItem){
                                           .value = {
                                             .stored_value.blob_val = decoded,
                                             .type_allocation_sz    = SERVER_PUBLIC_PARAMS_SIZE
                                           }
                                         }, UFSRV_CONFIG_SOURCE_RUNTIME);
  free(decoded);
}

void
InitialiseUfsrvCrypto(void)
{
  sInitServerCertificates();
  sInitCredentialsIssuanceServerParams();
}
