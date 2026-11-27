/**
 * @file utils_email.c
 * @brief Outbound e-mail delivery through the provider the config store names.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 * Created by ayman on 4/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "ufnet_core/email/utils_email.h"
#include "ufsrv_core/http/http_request.h"
#include "ufnet_msgcore/misc.h"
#include "uflib/utils.h"
#include "uflib/utils_urls.h"
#include "ufsrv_core/delegator_sessions/thread_context_type.h"
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_secret.h>

extern __thread ThreadContext ufsrv_thread_context;

/**
 * @brief Outbound e-mail API endpoint, from the config store (ufsrv.email.api_url).
 *
 * @return the configured URL, or NULL when the field is unset — callers must
 *         treat that as a hard failure rather than POSTing to an empty string.
 */
static const char *
sProvideEmailApiUrl(void)
{
  const char *api_url = UfsrvConfigEmailApiUrl();

  if (!IS_STR_LOADED(api_url)) {
    syslog(LOG_ERR, "%s: email.api_url is not configured", __func__);
    return NULL;
  }

  return api_url;
}


/**
 * @brief Decrypt ufsrv.email.api_key, the enc:v1 envelope held by the config store.
 *
 * @return the plaintext credential in 'user:password' form, or NULL when the
 *         field is unset or cannot be decrypted with the application secret key.
 *
 * @dynamic_memory EXPORTS char * — caller frees
 */
static char *
sProvideEmailApiKey(void)
{
  char *api_key_encrypted = UfsrvConfigEmailApiKey();
  char *plaintext         = NULL;

  if (!IS_STR_LOADED(api_key_encrypted)) {
    syslog(LOG_ERR, "%s: email.api_key is not configured", __func__);
    return NULL;
  }

  if (SecretConfigDecrypt(api_key_encrypted, UfsrvConfigUfsrvSecretKeyAsPtr(), &plaintext) != 0 || !IS_PRESENT(plaintext)) {
    syslog(LOG_ERR, "%s: FAILED to decrypt email.api_key", __func__);
    return NULL;
  }

  return plaintext;
}


/**
 * @brief Percent-encode one value for the provider's form-encoded body.
 *
 * The provider takes the message as application/x-www-form-urlencoded, so every
 * value has to reach it as data. A link carrying "&state=..." would otherwise
 * end the field it belongs to, and the recipient would get a truncated message.
 *
 * @param value NUL-terminated value.
 * @return heap-allocated encoded text, or NULL when allocation fails.
 * @dynamic_memory EXPORTS returned string which the caller must free
 */
static char *
sEncodeFormValue(const char *value)
{
  if (value == NULL) return NULL;

  size_t  value_len = strlen(value);
  size_t  buffer_sz = (value_len * 3) + 1;
  char   *encoded   = malloc(buffer_sz);

  if (encoded == NULL) return NULL;

  if (UrlEncodeComponent(value, encoded, buffer_sz) < 0) {
    free(encoded);

    return NULL;
  }

  return encoded;
}


/**
 * @brief Post one composed message to the configured provider.
 *
 * Kept apart from the two message builders so the field encoding, the post and
 * the provider's answer are written once: each builder owns its prose, this owns
 * the transport.
 *
 * @param to_email Recipient address.
 * @param from     Sender as "Name <address>".
 * @param subject  Subject line.
 * @param html     HTML body.
 * @param text     Plain-text body.
 * @return 0 when the provider queued the message; -1 or -2 otherwise.
 */
int
SendEmailMessage(const char *to_email, const char *from, const char *subject, const char *html, const char *text)
{
  if (!IS_STR_LOADED(to_email) || !IS_STR_LOADED(from) || !IS_STR_LOADED(subject) ||
      !IS_STR_LOADED(html) || !IS_STR_LOADED(text)) {
    return -1;
  }

  const char *provider_url_str = sProvideEmailApiUrl();
  if (!IS_PRESENT(provider_url_str)) return -1;

  char *api_key = sProvideEmailApiKey();
  if (!IS_PRESENT(api_key)) return -1;

  char *enc_to      = sEncodeFormValue(to_email);
  char *enc_from    = sEncodeFormValue(from);
  char *enc_subject = sEncodeFormValue(subject);
  char *enc_html    = sEncodeFormValue(html);
  char *enc_text    = sEncodeFormValue(text);

  char *post_fields_str = NULL;

  if (enc_to && enc_from && enc_subject && enc_html && enc_text) {
    asprintf(&post_fields_str, "from=%s&to=%s&h:List-Unsubscribe=&subject=%s&html=%s&text=%s",
             enc_from, enc_to, enc_subject, enc_html, enc_text);
  }

  free(enc_to);
  free(enc_from);
  free(enc_subject);
  free(enc_html);
  free(enc_text);

  if (!post_fields_str) {
    syslog(LOG_ERR, "%s: could not compose the message body", __func__);
    free(api_key);

    return -1;
  }

  HttpRequestContext *http_ptr = THREAD_CONTEXT_HTTP_REQUEST_CONTEXT;

  int result = HttpRequestPostUrl(http_ptr, provider_url_str, post_fields_str, api_key, NULL, NULL, 0L);

  if (result == 0) {
    syslog(LOG_ERR, "%s: the provider refused the message", __func__);
    free(post_fields_str);
    free(api_key);

    return -1;
  }

  free(post_fields_str);
  free(api_key);

  //we rely on the fact that SessionServiceGetUrl above has invoked the reset routine simple html buffer fetch
  do {
    http_ptr->jobj = json_tokener_parse_ex(http_ptr->jtok, http_ptr->rb.memory, strlen(http_ptr->rb.memory));
  } while ((http_ptr->jerr = json_tokener_get_error(http_ptr->jtok)) == json_tokener_continue);

  if (http_ptr->jerr != json_tokener_success) {
    syslog(LOG_NOTICE, "%s {pid:'%lu'}: ERROR JSON TOKENISER: '%s' ", __func__, pthread_self(), json_tokener_error_desc(http_ptr->jerr));

    //no cleanup necessary we rely on subsequent get url invoking reset
    return -2;
  }

  const char *message = json_object_get_string(json__get(http_ptr->jobj, "message"));

  if (IS_STR_LOADED(message)) {
    if (IS_PRESENT(strstr(message, "Queued."))) {
      syslog(LOG_DEBUG, "%s {pid:'%lu'}: message queued: '%s' ", __func__, pthread_self(),
             json_object_get_string(json__get(http_ptr->jobj, "id")));

      return 0;
    }

    syslog(LOG_DEBUG, "%s {pid:'%lu' }: JSON response: '%s'", __func__, pthread_self(),
           json_object_to_json_string(http_ptr->jobj));
  }

  return -1;
}

