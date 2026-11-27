/**
 * Copyright (C) 2015-2025 unfacd works
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

#include <ufnet_msgcore/utils_nonce.h>
#include <hiredis/hiredis.h>
#include <uflib/utils_crypto.h>

#ifndef CONFIG_MAX_NONCE_SZ
  #define	CONFIG_MAX_NONCE_SZ	(SHA_DIGEST_LENGTH + UINT64_LONGEST_STR_SZ)
#endif

//returns incremented counter
#define REDIS_CMD_REGONONCE_INC_COUNTER	"INCRBY NONCE_REGO_COUNTER 1"

#define EDIS_CMD_REGONONCE_EXPIRE	"EXPIRE %s %d"
//set nonce sesnid with expiry vlue
#define EDIS_CMD_REGONONCE_SET	"SET %s:%s	%lu EX %lu"
#define EDIS_CMD_REGONONCE_GET	"GET %s:%s"
#define REDIS_CMD_REGONONCE_DEL	"DEL %s:%s"

extern __thread ThreadContext ufsrv_thread_context;

/**
 * 	@brief generate a nonce with expiry time
 * 	@param sesn_ptr_carrier: represents the currently in-service session that is invoking the command. If Null, the request is
 * 	done by ufsrv worker, as opposed to session worked
 * 	@param prefix specify a domain for the nonce counter. Currently not in use. All nonce use the same domain 'NONCE_REGO_COUNTER '
 * 	@param value additional identifier to add to the nonce key. If empty current session's id is used
 *
 * 	@dynamic_memory OUT ALLOCATES new string which the user must free
 */
char *
BackEndGenerateNonce(Session *sesn_ptr_carrier, time_t expiry_in, const char *prefix, const char *value)
{
  redisReply *redis_ptr;
  time_t expiry;

  PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(ufsrv_thread_context);

  if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_REGONONCE_INC_COUNTER))) {
    syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR COULD NOT INC NONCE COUNTER: BACKEND CONNECTIVITY ERROR", __func__, pthread_self());

    return NULL;
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' ): ERROR COULD NOT INC NONCE COUNTER: REPLY ERROR '%s'", __func__, pthread_self(),  redis_ptr->str);

    freeReplyObject(redis_ptr);

    return NULL;
  }

  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s(pid:'%lu' ): ERROR COULD NOT INC NONCE COUNTER: REPLY NIL '%s'", __func__, pthread_self(), redis_ptr->str);

    freeReplyObject(redis_ptr);

    return NULL;
  }

  long long nonce_counter = redis_ptr->integer;
  freeReplyObject(redis_ptr);

  char nonce_hashed[CONFIG_MAX_NONCE_SZ + 1];
  memset(nonce_hashed, 0, sizeof(nonce_hashed));
  char *nonce_unhashed;

  asprintf(&nonce_unhashed, "%lld%lu", nonce_counter, time(NULL));

  ComputeSHA1((unsigned char *)nonce_unhashed, strlen(nonce_unhashed), nonce_hashed, sizeof(nonce_hashed), 0);

#ifdef __UF_FULLDEBUG
  syslog(LOG_DEBUG, "%s (pid:'%lu' o:'%p' cid='%lu'): GENERATED SHA-1:'%s' for NONCE:'%s' for COUNTER:'%lld'", __func__, pthread_self(), sesn_ptr_target, SESSION_ID(sesn_ptr_target),
			nonce_hashed, nonce_unhashed, nonce_counter);
#endif

  if (strlen(nonce_hashed) == 0) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' ): ERROR: COULD NO GENERATE SHA-1 for NONCE:'%s' for COUNTER:'%lld'", __func__, pthread_self(), nonce_unhashed, nonce_counter);

    free(nonce_unhashed);

    return NULL;
  }

  free(nonce_unhashed);

  if (IS_PRESENT(value)) {
    redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, "SET %s:%s %s EX %lu"/*REDIS_CMD_REGONONCE_SET*/, prefix, nonce_hashed, value, expiry_in <= 0? 5L : expiry_in);
  } else if IS_PRESENT(sesn_ptr_carrier) {
    redis_ptr = (*pers_ptr->send_command)(sesn_ptr_carrier, pers_ptr, "SET %s:%s %lu EX %lu"/*REDIS_CMD_REGONONCE_SET*/, prefix, nonce_hashed, SESSION_ID(sesn_ptr_carrier), expiry_in <= 0? 5L : expiry_in);
  }

  if (redis_ptr == NULL) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' ): ERROR COULD ISSUE SET COMMAND: BACKEND CONNECTIVITY ERROR", __func__, pthread_self());

    return NULL;
  }

  if (strcasecmp(redis_ptr->str, "ok") == 0) {
    freeReplyObject(redis_ptr);

    char *nonce_hashed_copy = strndup(nonce_hashed, sizeof(nonce_hashed));

    syslog(LOG_DEBUG, "%s (pid:'%lu' ): SUCCESS: RETURNING NONCE:'%s' WITH EXPIRY:'%lu'", __func__, pthread_self(), nonce_hashed_copy, expiry_in <= 0? 5 : expiry_in);

    return nonce_hashed_copy;
  } else {
    syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR SET NONCE COMMAND FAILED: '%s' REPLY CODE:'%d'", __func__, pthread_self(), redis_ptr->str, redis_ptr->type);

    freeReplyObject(redis_ptr);

    return NULL;
  }

  return NULL;
}

/**
 * 	@brief
 * 	@returns 0 on success or <0 on error
 *
 * 	@dynamic_memory: ALLOCATES and frees redisReply *
 */
int
BackEndDeleteNonce(Session *sesn_ptr_carrier, const char *nonce, const char *prefix)
{
  PersistenceBackend 	*pers_ptr;
  redisReply 					*redis_ptr;

  if (!IS_STR_LOADED(nonce)) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' o:'%p'): ERROR: NONCE VALUE NOT SET", __func__, pthread_self(), sesn_ptr_carrier);

    return -1;
  }

  pers_ptr = THREAD_CONTEXT_SESSION_CACHEBACKEND;

  if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_REGONONCE_DEL, prefix, nonce))) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' o:'%p'): ERROR COULD NOT DEL NONCE: BACKEND CONNECTIVITY ERROR", __func__, pthread_self(), sesn_ptr_carrier);

    return -2;
  }

  __success_block:
  if (strcasecmp(redis_ptr->str, "ok") == 0) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' o:'%p'): SUCCESS NONCE:'%s' DELETED...", __func__, pthread_self(), sesn_ptr_carrier, nonce);

    freeReplyObject(redis_ptr);

    return 0;
  } else {
    syslog(LOG_DEBUG, "%s (pid:'%lu' o:'%p'): ERROR COULD NOT DEL NONCE: REPLY ERROR '%s'", __func__, pthread_self(), sesn_ptr_carrier, redis_ptr->str);

    freeReplyObject(redis_ptr);

    return -2;
  }

  return -1;

}

/**
 * @brief simplified frontend to BackEndGetNonce
 * @dynamic_memory EXPORTS char * if stored_value_return is non NULL
 */
bool
IsNonceValid(const char *nonce, const char *prefix, char **stored_value_return)
{
  return (BackEndGetNonce(nonce, prefix, stored_value_return) == 0);
}

/**
 * 	@brief
 * 	@param stored_value_return [out] if set, the value stored for the nonce key will returned. Currently, only c-strings are processed.
 * 	@returns 0 on success or <0 on error
 *
 * 	@dynamic_memory: ALLOCATES and frees redisReply *
 * 	@dynamic_memory EXPORTS char * if stored_value_return is non NULL
 */
int
BackEndGetNonce(const char *nonce, const char *prefix, char **stored_value_return)
{
  PersistenceBackend 	*pers_ptr;
  redisReply 					*redis_ptr;

  if (!IS_STR_LOADED(nonce)) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' o): ERROR: NONCE VALUE NOT SET", __func__, pthread_self());
    return -1;
  }

  pers_ptr = THREAD_CONTEXT_SESSION_CACHEBACKEND;

  char *tmp;
  asprintf(&tmp, "GET %s:%s", prefix, nonce);

  if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, tmp)))  {
    //if (!(redis_ptr=(*pers_ptr->send_command)(sesn_ptr_target, EDIS_CMD_REGONONCE_GET), prefix, nonce))//buggy cause string corruption
    syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR COULD NOT GET NONCE: BACKEND CONNECTIVITY ERROR", __func__, pthread_self());

    free(tmp);
    return -2;
  }

  free(tmp);

  if (redis_ptr->type == REDIS_REPLY_STRING) {
    syslog(LOG_DEBUG, "%s (pid:'%lu'): SUCCESS NONCE:'%s' RECEIVED. Stored value:'%s'", __func__, pthread_self(), nonce, redis_ptr->str);
    if (IS_PRESENT(stored_value_return)) *stored_value_return = strdup(redis_ptr->str);
    freeReplyObject(redis_ptr);

    return 0;
  }

  if (IS_PRESENT(stored_value_return)) stored_value_return = NULL;

  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' ): ERROR COULD NOT GET NONCE: REPLY ERROR '%s'", __func__, pthread_self(), redis_ptr->str);

    freeReplyObject(redis_ptr);

    return -2;
  }

  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s(pid:'%lu' ): ERROR COULD NOT GET STORED NONCE: REPLY NIL '%s'", __func__, pthread_self(), redis_ptr->str);

    freeReplyObject(redis_ptr);

    return -2;
  }

  return -1;

}
