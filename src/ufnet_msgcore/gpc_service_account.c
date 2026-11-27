/**
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <ufnet_msgcore/gpc_service_account.h>

#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_secret.h>

extern char *UfsrvConfigCloudServiceAccountsGpcFirebaseMessaging(void);
extern char *UfsrvConfigCloudServiceAccountsGpcIntegrity(void);
#include <uflib/standard_defs.h>

#include <json-c/json.h>
#include <syslog.h>

typedef struct GpcServiceAccountSigningKey {
  JwtKeyset     *keyset;
  char          *key_id;
  pthread_once_t once;
  int            status;
} GpcServiceAccountSigningKey;

static GpcServiceAccountSigningKey s_gpc_service_accounts[GPC_SERVICE_ACCOUNT_COUNT] = {
  [GPC_SERVICE_ACCOUNT_FIREBASE_MESSAGING] = { .once = PTHREAD_ONCE_INIT, .status = -1 },
  [GPC_SERVICE_ACCOUNT_INTEGRITY]          = { .once = PTHREAD_ONCE_INIT, .status = -1 }
};

static const char *
sServiceAccountName(enum GpcServiceAccount service_account)
{
  switch (service_account) {
    case GPC_SERVICE_ACCOUNT_FIREBASE_MESSAGING: return "gpc_firebase_messaging";
    case GPC_SERVICE_ACCOUNT_INTEGRITY:          return "gpc_integrity";
    default:                                     return "unknown";
  }
}

static json_object *
sServiceAccountField(json_object *document, const char *name)
{
  json_object *field = NULL;

  if (!json_object_object_get_ex(document, name, &field) || !json_object_is_type(field, json_type_string)) return NULL;

  return field;
}

static void
_LoadServiceAccountSigningKey(enum GpcServiceAccount service_account, const char *envelope)
{
  GpcServiceAccountSigningKey *entry_ptr    = &s_gpc_service_accounts[service_account];
  char                        *document_str = NULL;
  json_object                 *document     = NULL;
  json_object                 *field        = NULL;
  const char                  *key_id       = NULL;
  const char                  *pem          = NULL;
  char                         err[256]     = {0};

  if (!IS_STR_LOADED(envelope)) {
    syslog(LOG_ERR, "%s: cloud_service_accounts.%s is not configured", __func__, sServiceAccountName(service_account));

    return;
  }

  if (SecretConfigDecrypt(envelope, UfsrvConfigUfsrvSecretKeyAsPtr(), &document_str) != 0 || !IS_STR_LOADED(document_str)) {
    syslog(LOG_ERR, "%s: cloud_service_accounts.%s could not be decrypted", __func__, sServiceAccountName(service_account));

    free(document_str);

    return;
  }

  document = json_tokener_parse(document_str);

  free(document_str);

  if (document == NULL || !json_object_is_type(document, json_type_object)) {
    syslog(LOG_ERR, "%s: cloud_service_accounts.%s does not hold a JSON document", __func__, sServiceAccountName(service_account));

    if (document != NULL) json_object_put(document);

    return;
  }

  field  = sServiceAccountField(document, "private_key_id");
  key_id = field != NULL ? json_object_get_string(field) : NULL;
  field  = sServiceAccountField(document, "private_key");
  pem    = field != NULL ? json_object_get_string(field) : NULL;

  if (!IS_STR_LOADED(key_id) || !IS_STR_LOADED(pem)) {
    syslog(LOG_ERR, "%s: cloud_service_accounts.%s carries no private_key/private_key_id", __func__, sServiceAccountName(service_account));

    json_object_put(document);

    return;
  }

  entry_ptr->keyset = JwtKeysetNew();

  if (entry_ptr->keyset == NULL) {
    syslog(LOG_ERR, "%s: cloud_service_accounts.%s: could not allocate a key set", __func__, sServiceAccountName(service_account));

    json_object_put(document);

    return;
  }

  if (JwtKeysetAddPem(entry_ptr->keyset, pem, strlen(pem), key_id, err, sizeof err) != JWT_STATUS_OK) {
    syslog(LOG_ERR, "%s: cloud_service_accounts.%s: %s", __func__, sServiceAccountName(service_account), err);

    JwtKeysetFree(entry_ptr->keyset);
    entry_ptr->keyset = NULL;
    json_object_put(document);

    return;
  }

  entry_ptr->key_id = strdup(key_id);

  if (!IS_STR_LOADED(entry_ptr->key_id)) {
    syslog(LOG_ERR, "%s: cloud_service_accounts.%s: could not retain the key id", __func__, sServiceAccountName(service_account));

    JwtKeysetFree(entry_ptr->keyset);
    entry_ptr->keyset = NULL;
    json_object_put(document);

    return;
  }

  entry_ptr->status = 0;

  json_object_put(document);
}

static void
_LoadFirebaseMessagingSigningKey(void)
{
  _LoadServiceAccountSigningKey(GPC_SERVICE_ACCOUNT_FIREBASE_MESSAGING, UfsrvConfigCloudServiceAccountsGpcFirebaseMessaging());
}

static void
_LoadIntegritySigningKey(void)
{
  _LoadServiceAccountSigningKey(GPC_SERVICE_ACCOUNT_INTEGRITY, UfsrvConfigCloudServiceAccountsGpcIntegrity());
}

static void (*const s_gpc_service_account_loaders[GPC_SERVICE_ACCOUNT_COUNT])(void) = {
  [GPC_SERVICE_ACCOUNT_FIREBASE_MESSAGING] = _LoadFirebaseMessagingSigningKey,
  [GPC_SERVICE_ACCOUNT_INTEGRITY]          = _LoadIntegritySigningKey
};

int
ProvideGpcServiceAccountSigningKey(enum GpcServiceAccount service_account, JwtKeyset **keyset_out, const char **key_id_out)
{
  GpcServiceAccountSigningKey *entry_ptr = NULL;

  if (service_account < 0 || service_account >= GPC_SERVICE_ACCOUNT_COUNT) return -1;

  entry_ptr = &s_gpc_service_accounts[service_account];
  pthread_once(&entry_ptr->once, s_gpc_service_account_loaders[service_account]);

  if (entry_ptr->status != 0) return -1;

  if (keyset_out != NULL) *keyset_out = entry_ptr->keyset;
  if (key_id_out != NULL) *key_id_out = entry_ptr->key_id;

  return 0;
}
