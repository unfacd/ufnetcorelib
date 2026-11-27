/**
 * Copyright (C) 2015-2024 unfacd works
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

#include <ufnet_msgcore/gpc_http_utils.h>
#include <ufnet_msgcore/gpc_service_account.h>
#include <ufsrv_core/delegator_sessions/thread_context_type.h>
#include <ufsrv_core/http/http_request.h>
#include <ufnet_msgcore/misc.h>
#include <ufsrv_core/jwt/jwt.h>

#include <json-c/json.h>
#include <openssl/evp.h>

#include <uflib/utils_crypto.h>

#include "ufnet_core/config_nonpublic.h"

extern __thread ThreadContext ufsrv_thread_context;

/**
 * @brief Perform standard GPC OAuth2.0 flow to get an access token for API use authorization.
 *
 * \image html https://developers.google.com/static/identity/protocols/oauth2/images/flows/jwt.png "GPC access token request flow"
 * @param[in] http_ptr Http handling context containing data objects necessary for handling http request, response and processing of response
 * @return fully formed access token
 * @dynamic_memory EXPORTS string
 */
char *
GetGoogleAccessCodeAuthorization(HttpRequestContext *http_ptr, const GpcServiceRequestDescriptor *const gpc_request_descriptor_ptr)
{
  char *gpc_access_token = NULL;

  char *jwt_final = GenerateSignedJWT(NULL, gpc_request_descriptor_ptr);
  if (IS_PRESENT(jwt_final)) {
    gpc_access_token = RequestGoogleAccessToken(http_ptr, jwt_final);
    if (IS_PRESENT(gpc_access_token)) {
      //NOOP
    } else {
      syslog(LOG_ERR, "%s {pid:'%lu', th_ctx:'%p'}: ERROR COULD NOT REQUEST GPC ACCESS TOKEN: JWT:'%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, jwt_final);
    }
    free(jwt_final);
  }

  return gpc_access_token;
}

static void _ConfirmNoError(HttpRequestContext *http_ptr, void(^on_no_error)(void));

/**
 * '{ "error": "invalid_grant", "error_description": "java.security.SignatureException: Invalid signature for token: eyJhbGciOiJSU0=.eyAiaXTZ9.btweB7Q==" }'
 * '{ "access_token": "ya29.c.c0", "expires_in": 3599, "token_type": "Bearer" }'
 * @param http_tr
 */
static void
_ConfirmNoError(HttpRequestContext *http_ptr, void(^on_no_error)(void))
{
  const char *error = json_object_get_string(json__get(http_ptr->jobj, "error"));
  if (!IS_STR_LOADED(error)) {
    on_no_error();
    return;
  }

  syslog(LOG_NOTICE, "%s {pid:'%lu'}: ERROR GCP ACCESS TOKEN: error:'%s', error_description:'%s'", __func__, pthread_self(), error, json_object_get_string(json__get(http_ptr->jobj, "error_description")));
}

/**
 * @brief Request a GPC access token, authorising ufsrv to use service account on GPC which was previously configured to use Play Integrity API.
 * @param http_ptr[IN] Http handling context containing data objects necessary for handling http request, response and processing of response.
 * @param jwt_encoded[IN] previously form JWT token, base64 encoded and ready for insertion into HTTP request.
 * @dynamic_memory user must delallocate returned string
 * {
  "access_token": "1/8xbJqaOZXSUZbHLl5EOtu1pxz3fmmetKx9W8CV4t79M",
  "scope": "https://www.googleapis.com/auth/prediction"
  "token_type": "Bearer",
  "expires_in": 3600
}
 */
char *
RequestGoogleAccessToken(HttpRequestContext *http_ptr, const char *jwt_encoded)
{
#define GOOGLE_TOKEN_ACCESS_SERVER "https://oauth2.googleapis.com/token"
  char *post_data = NULL;

  asprintf(&post_data, "grant_type=%s&assertion=%s", "urn:ietf:params:oauth:grant-type:jwt-bearer", jwt_encoded);
  int result = HttpRequestPostUrl(http_ptr, GOOGLE_TOKEN_ACCESS_SERVER, post_data, APITOKEN_SMSVOICE_PROD, NULL, "application/x-www-form-urlencoded", 0L);
  if (result == 0) {
    syslog(LOG_ERR, "%s (pid:'%lu'): ERROR: COULD NOT POST URL '%s'", __func__, pthread_self(), post_data);
    free(post_data);
    return NULL;
  }

  free(post_data);

  do {
    http_ptr->jobj = json_tokener_parse_ex(http_ptr->jtok, http_ptr->rb.memory, strlen(http_ptr->rb.memory));
  } while ((http_ptr->jerr = json_tokener_get_error(http_ptr->jtok)) == json_tokener_continue);

  if (http_ptr->jerr != json_tokener_success) {
    syslog(LOG_NOTICE, "%s {pid:'%lu'}: ERROR JSON TOKENISER: '%s' ", __func__, pthread_self(), json_tokener_error_desc(http_ptr->jerr));

    return NULL;
  }

  const char *json_str_access_token = json_object_to_json_string(http_ptr->jobj);
  __block char *access_token = NULL;
  syslog(LOG_NOTICE, "%s {pid:'%lu'}: RECEIVED GCP ACCESS TOKEN RESPONSE: '%s'", __func__, pthread_self(), json_str_access_token);
  _ConfirmNoError(http_ptr, ^(){
      access_token = strdup(json_object_get_string(json__get(http_ptr->jobj, "access_token")));
  });

  return access_token;

}

/**
 * @brief Build and sign the service-account assertion that GPC exchanges for an access token.
 *
 * The claim set comes from the descriptor's template; the header, the algorithm
 * and the key id come from the JWT module.  GPC matches the key id against the
 * fingerprint published for the service account's key.
 *
 * @param http_ptr unused; retained for the established call shape
 * @param gpc_request_descriptor_ptr service account request parameters
 * @dynamic_memory returned str must be freed by user
 * @return the compact assertion, or NULL
 */
char *
GenerateSignedJWT(HttpRequestContext *http_ptr, const GpcServiceRequestDescriptor *const gpc_request_descriptor_ptr)
{
#define ONE_HOUR_IN_SECONDS (60 * 60)
  time_t       issued_at     = time(NULL);
  time_t       expires_at    = issued_at + ONE_HOUR_IN_SECONDS;
  JwtKeyset   *keyset        = NULL;
  const char  *key_id        = NULL;
  json_object *claims        = NULL;
  char        *claim_set_str = NULL;
  char        *assertion     = NULL;
  char         err[256]      = {0};

  (void)http_ptr;

  if (gpc_request_descriptor_ptr == NULL) {
    syslog(LOG_ERR, "%s: no service request descriptor was supplied", __func__);

    return NULL;
  }

  if (asprintf(&claim_set_str, gpc_request_descriptor_ptr->claim_set_template,
               gpc_request_descriptor_ptr->is_refresh_token ? "offline" : "online",
               (unsigned long)expires_at, (unsigned long)issued_at) < 0) {
    return NULL;
  }

  claims = json_tokener_parse(claim_set_str);
  free(claim_set_str);

  if (claims == NULL || !json_object_is_type(claims, json_type_object)) {
    syslog(LOG_ERR, "%s: the claim set template did not render a JSON object", __func__);

    if (claims != NULL) json_object_put(claims);

    return NULL;
  }

  if (ProvideGpcServiceAccountSigningKey(gpc_request_descriptor_ptr->service_account, &keyset, &key_id) != 0) {
    json_object_put(claims);

    return NULL;
  }

  assertion = JwtMint(keyset, key_id, claims, err, sizeof err);

  if (assertion == NULL) {
    syslog(LOG_ERR, "%s: the assertion could not be signed: %s", __func__, err);
  }

  json_object_put(claims);

  return assertion;
}

/**
 * @brief This implementation uses the new style of openssl digest generation, where the underlying engine is abstracted out.
 * See legacy implementation \ref _ComputeSHA256Digest
 * @param message message to be digested
 * @param message_sz size of message
 * @param message_hashed_out user allocated buffer of size EVP_MAX_MD_SIZE
 * @return
 */
void
ComputeMessageDigest(unsigned char *message, size_t message_sz, unsigned char *message_hashed_out, unsigned int *message_hashed_out_sz)
{
  // Create a Message Digest Context for the operations
  const EVP_MD *md = EVP_get_digestbyname("SHA256");
  EVP_MD_CTX *mdctx = EVP_MD_CTX_new();

  // Sets up the Message Digest Context to be used with the engine, in this case
  // NULL which means the default implmentation for the Message Digest Type will be used
  ENGINE* engine = NULL;
  EVP_DigestInit_ex(mdctx, md, engine);

  // Hash the passed in message and add it to mdctx->md_data
  EVP_DigestUpdate(mdctx, message, message_sz);//this can be called again on another message

  int ret_code = EVP_DigestFinal(mdctx, message_hashed_out, message_hashed_out_sz);//mdctx is automatically cleaned up by EVP_DigestFinal
  if (ret_code == 1) {
    unsigned char *message_digest_hexed = hex_print(message_hashed_out, EVP_MAX_MD_SIZE, NULL);
    syslog(LOG_INFO, "%s (pid:'%lu', md_type:'%d', md_len:'%i', EVP_MD_CTX_size:'%d', EVP_MD_size:'%d'):digest: '%s' ", __func__, pthread_self(), EVP_MD_type(md), *message_hashed_out_sz, EVP_MD_CTX_size(mdctx), EVP_MD_size(md), message_digest_hexed);
    free(message_digest_hexed);
  }

  EVP_MD_CTX_free(mdctx);
}
