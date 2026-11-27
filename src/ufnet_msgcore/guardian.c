/*

 Copyright (c) 2015-2026 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include "ufnet_msgcore/guardian.h"
#include <ufsrv_core/delegator_sessions/thread_context_type.h>
#include <uflib/utils.h>
#include <ufsrv_core/cache_backend/redis.h>
#include <ufsrv_core/system/log_message_literals.h>
#include <ufnet_msgcore/utils_nonce.h>
#include <json-c/json_tokener.h>
#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>

extern __thread ThreadContext ufsrv_thread_context;

/**
 *
 * @param sesn_ptr
 * @param value uid idenitifying the guarded
 * @return
 */
char *
GenerateGuardianNonce(Session *sesn_ptr, const char *value)
{
  char *attachment_nonce = BackEndGenerateNonce(sesn_ptr, CONFIGDEFAULT_GUARDIAN_NONCE_EXPIRY, CONFIGDEFAULT_GUARDIAN_NONCE_PREFIX, value);

  return attachment_nonce;

}

unsigned long
IsGuardianLinkNonceValid(const char *nonce, unsigned long supplied_value)
{
  PersistenceBackend *pers_ptr;
  redisReply *redis_ptr;

  if (!IS_STR_LOADED(nonce)) {
    syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: NONCE VALUE NOT SET", __func__, pthread_self());
    return 0;
  }

  pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);

  char tmp[LBUF] = {0};
  snprintf(tmp, LBUF-1, "GET %s:%s", CONFIGDEFAULT_GUARDIAN_NONCE_PREFIX, nonce);

  if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, tmp))) {
    syslog(LOG_DEBUG, "%s (pid:'%lu' nonce:'%s'): ERROR COULD NOT GET NONCE: BACKEND CONNECTIVITY ERROR", __func__, pthread_self(), nonce);

    return false;
  }

  __success_block:
  if (redis_ptr->type == REDIS_REPLY_STRING) {
    unsigned long retrieved_value = strtoul(redis_ptr->str, NULL, 10);

    freeReplyObject(redis_ptr);
    return retrieved_value == supplied_value;
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR COULD NOT GET NONCE: REPLY ERROR '%s'", __func__, pthread_self(), redis_ptr->str);

    freeReplyObject(redis_ptr);

    return 0;
  }

  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s(pid:'%lu'): ERROR COULD NOT GET STORED NONCE: REPLY NIL '%s'", __func__, pthread_self(), redis_ptr->str);

    freeReplyObject(redis_ptr);

    return 0;
  }

  return 0;

}

/**
 *  {event_types: '[{type:'%u', fid:'%lu'},{}]'}
 * @param descriptor_ptr
 * @return
 */
UFSRVResult *
DbBackendInsertGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr, bool force_data)
{
#define SQL_INSERT_NEW_GUARDIAN_RECORD "INSERT INTO guardians (guardian, originator, status, gid, timestamp, data) VALUES ('%lu', '%lu', '%u', '%lu', '%llu', '%s') ON DUPLICATE KEY UPDATE timestamp = '%llu', gid = '%lu'"
#define SQL_INSERT_NEW_GUARDIAN_RECORD_NO_DATA "INSERT INTO guardians (guardian, originator, status, gid, timestamp) VALUES ('%lu', '%lu', '%u', '%lu', '%llu') ON DUPLICATE KEY UPDATE timestamp = '%llu', gid = '%lu'"
  char *sql_query_str;
  if (force_data) {
    sql_query_str = mdsprintf(SQL_INSERT_NEW_GUARDIAN_RECORD, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid, descriptor_ptr->status, descriptor_ptr->gid, descriptor_ptr->timestamp, descriptor_ptr->specs.specs_serialised, descriptor_ptr->timestamp, descriptor_ptr->gid);
  } else {
    if (!IS_STR_LOADED(descriptor_ptr->specs.specs_serialised)) {
      sql_query_str = mdsprintf(SQL_INSERT_NEW_GUARDIAN_RECORD_NO_DATA, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid, descriptor_ptr->status, descriptor_ptr->gid, descriptor_ptr->timestamp, descriptor_ptr->timestamp, descriptor_ptr->gid);
    } else {
      sql_query_str = mdsprintf(SQL_INSERT_NEW_GUARDIAN_RECORD, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid, descriptor_ptr->status, descriptor_ptr->gid, descriptor_ptr->timestamp, descriptor_ptr->specs.specs_serialised, descriptor_ptr->timestamp, descriptor_ptr->gid);
    }
  }

#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): GENERATED SQL QUERY: '%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, sql_query_str);
#endif
  int sql_result = h_query_insert(THREAD_CONTEXT_DB_BACKEND, sql_query_str);

  if (sql_result != H_OK) {
    syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): ERROR: COULD NOT EXECUTE QUERY: '%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, sql_query_str);

    free (sql_query_str);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)
  }

  free (sql_query_str);

  struct _h_data *db_data = h_query_last_insert_id(THREAD_CONTEXT_DB_BACKEND);
  if (db_data->type == HOEL_COL_TYPE_INT) {
    int last_id = ((struct _h_type_int *)db_data->t_data)->value;
    h_clean_data_full(db_data);

    THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *)(uintptr_t)last_id, RESCODE_BACKEND_DATA)
  }

  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)

#undef SQL_INSERT_NEW_GUARDIAN_RECORD
}

/**
 * @brief Retrieve the guardian record for a named originator/guardian pair.
 * @param descriptor_ptr
 * @return
 */
UFSRVResult *
DbBackendGetGuardianRecord(GuardianRecordDescriptor *descriptor_ptr)
{
#define SQL_GET_GUARDIAN_RECORD "SELECT * FROM guardians WHERE  guardian = '%lu' AND originator = '%lu'"
#define SQL_GET_GUARDIAN_RECORD_WITH_STATUS "SELECT * FROM guardians WHERE  guardian = '%lu' AND originator = '%lu' AND status = '%u'"
#define COLUMN_ID(x)	    ((struct _h_type_int *)result.data[x][0].t_data)->value //0
#define COLUMN_GUARDIAN(x)	    ((struct _h_type_int *)result.data[x][1].t_data)->value
#define COLUMN_ORIGINATOR(x)	    ((struct _h_type_int *)result.data[x][2].t_data)->value
#define COLUMN_STATUS(x)	    ((struct _h_type_int *)result.data[x][3].t_data)->value
#define COLUMN_TIMESTAMP(x)	    ((struct _h_type_int *)result.data[x][4].t_data)->value
#define COLUMN_GID(x)	    ((struct _h_type_int *)result.data[x][5].t_data)->value
#define COLUMN_DATA_IS_EMPTY(x)          IS_EMPTY(((struct _h_type_blob *)result.data[x][6].t_data))
#define COLUMN_DATA(x)          (char *)(((struct _h_type_blob *)result.data[x][6].t_data)->value)
#define COLUMN_DATA_LEN(x)          ((struct _h_type_blob *)result.data[x][6].t_data)->length

  int 	rescode;
  struct _h_result result;
  char *sql_query_str;

  if (descriptor_ptr->status != GUARDIAN_STATUS_NONE) sql_query_str = mdsprintf(SQL_GET_GUARDIAN_RECORD_WITH_STATUS, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid, descriptor_ptr->status);
  else sql_query_str = mdsprintf(SQL_GET_GUARDIAN_RECORD, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid);

#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): GENERATED SQL QUERY: '%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, sql_query_str);
#endif

  int sql_result = h_query_select(THREAD_CONTEXT_DB_BACKEND, sql_query_str, &result);

  if (sql_result != H_OK)		goto return_db_error;
  if (result.nb_rows == 0)	goto return_empty_set;
  if (COLUMN_DATA_IS_EMPTY(0))	goto return_db_empty_jsonstr;

  const char *account_data_json_str = strndupa(COLUMN_DATA(0), COLUMN_DATA_LEN(0));
  size_t 			jsonstr_sz = strlen(account_data_json_str);

  if (unlikely(jsonstr_sz == 0))	goto return_db_empty_jsonstr;

  enum 		json_tokener_error jerr;
  json_object 	*jobj_account = NULL;
  json_tokener 	*jtok = json_tokener_new();

  do {
    jobj_account = json_tokener_parse_ex(jtok, account_data_json_str, strlen(account_data_json_str));
  } while ((jerr = json_tokener_get_error(jtok)) == json_tokener_continue);

  if (jerr != json_tokener_success)	goto return_error_json_tokniser;

  //originator and guardian values already set in the provided descriptor_ptr
  unsigned long id = COLUMN_ID(0);
  descriptor_ptr->status = COLUMN_STATUS(0);
  descriptor_ptr->timestamp = COLUMN_TIMESTAMP(0);
  descriptor_ptr->gid = COLUMN_GID(0);
  descriptor_ptr->specs.specs_jobj = jobj_account;

  return_success:
  json_tokener_free(jtok);
  h_clean_result(&result);
  free (sql_query_str);
  THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *)(uintptr_t)id, RESCODE_BACKEND_DATA)

  return_db_error:
  syslog(LOG_DEBUG, "%s {th_ctx: '%p'}: ERROR: COULD EXECUTE QUERY: '%s'", __func__, THREAD_CONTEXT_PTR, sql_query_str);
  rescode = RESCODE_BACKEND_CONNECTION;
  goto return_free;

  return_empty_set:
#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): ERROR: COULD FIND CORRESPONDING DB RECORD", __func__, pthread_self(), THREAD_CONTEXT_PTR);
#endif
  rescode = RESCODE_BACKEND_DATA_EMPTYSET;
  goto return_free_sql_handle;

  return_error_json_tokniser:
  syslog(LOG_NOTICE, "%s (pid:'%lu' th_ctx:'%p'): JSON tokeniser Error: '%s'. Terminating.", __func__, pthread_self(), THREAD_CONTEXT_PTR, json_tokener_error_desc(jerr));
  rescode = RESCODE_PROG_JSON_PARSER;
  goto return_free_json_tokeniser;

  return_db_empty_jsonstr:
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): ERROR: DB JSON COLUMN NULL OR STRING SIZE ZERO", __func__, pthread_self(), THREAD_CONTEXT_PTR);
  rescode = RESCODE_BACKEND_RESOURCE_NULL;
  goto return_free_sql_handle;

  return_free_json_tokeniser:
  json_tokener_free(jtok);
  if (IS_PRESENT(jobj_account)) json_object_put(jobj_account);

  return_free_sql_handle:
  h_clean_result(&result);

  return_free:
  free (sql_query_str);

  return_error:
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)

#undef SQL_GET_GUARDIAN_RECORD
#undef COLUMN_ID
#undef COLUMN_GUARDIAN
#undef COLUMN_ORIGINATOR
#undef COLUMN_STATUS
#undef COLUMN_TIMESTAMP
#undef COLUMN_GID
#undef COLUMN_DATA_IS_EMPTY
#undef COLUMN_DATA
#undef COLUMN_DATA_LEN
#undef SQL_GET_GUARDIAN_RECORD
#undef SQL_GET_GUARDIAN_RECORD_WITH_STATUS
}

/**
 * @brief Retrieve all records for the named guardian.
 * @param descriptor_ptr_guardian
 * @param collection_ptr_out Must be allocated by caller. When returned, size will be set > 0
 * @dynamic_memory: EXPORTS GuardianRecordDescriptor **
 * @return
 */
UFSRVResult *
DbBackendGetGuardianRecords(GuardianRecordDescriptor *descriptor_ptr_guardian, CollectionDescriptor *collection_ptr_out)
{
#define SQL_GET_GUARDIAN_RECORD "SELECT * FROM guardians WHERE  guardian = '%lu'"
#define SQL_GET_GUARDIAN_RECORD_WITH_STATUS "SELECT * FROM guardians WHERE  guardian = '%lu' AND status = '%u'"
#define COLUMN_ID(x)	    ((struct _h_type_int *)result.data[x][0].t_data)->value //0
#define COLUMN_GUARDIAN(x)	    ((struct _h_type_int *)result.data[x][1].t_data)->value
#define COLUMN_ORIGINATOR(x)	    ((struct _h_type_int *)result.data[x][2].t_data)->value
#define COLUMN_STATUS(x)	    ((struct _h_type_int *)result.data[x][3].t_data)->value
#define COLUMN_TIMESTAMP(x)	    ((struct _h_type_int *)result.data[x][4].t_data)->value
#define COLUMN_GID(x)	    ((struct _h_type_int *)result.data[x][5].t_data)->value
#define COLUMN_DATA_IS_EMPTY(x)          IS_EMPTY(((struct _h_type_blob *)result.data[x][6].t_data))
#define COLUMN_DATA(x)          (char *)(((struct _h_type_blob *)result.data[x][6].t_data)->value)
#define COLUMN_DATA_LEN(x)          ((struct _h_type_blob *)result.data[x][6].t_data)->length

  int 	rescode;
  struct _h_result result;
  char *sql_query_str;

  if (descriptor_ptr_guardian->status != GUARDIAN_STATUS_NONE) sql_query_str = mdsprintf(SQL_GET_GUARDIAN_RECORD_WITH_STATUS, descriptor_ptr_guardian->guardian.uid, descriptor_ptr_guardian->status);
  else sql_query_str = mdsprintf(SQL_GET_GUARDIAN_RECORD, descriptor_ptr_guardian->guardian.uid);

#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): GENERATED SQL QUERY: '%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, sql_query_str);
#endif

  int sql_result = h_query_select(THREAD_CONTEXT_DB_BACKEND, sql_query_str, &result);

  if (sql_result != H_OK)		goto return_db_error;
  if (result.nb_rows == 0)	goto return_empty_set;
  GuardianRecordDescriptor **guardian_records = calloc(result.nb_rows, sizeof(GuardianRecordDescriptor));

#if defined(HAVE_VALGRIND_MEMCHECK)
  VALGRIND_CREATE_MEMPOOL(guardian_records, 0, 1);
  VALGRIND_MAKE_MEM_NOACCESS(guardian_records, result.nb_rows * (sizeof(GuardianRecordDescriptor)));
#endif

  GuardianRecordDescriptor *descriptor_ptr;
  size_t i = 0;
  for (; i<result.nb_rows; i++) {
#if defined(HAVE_VALGRIND_MEMCHECK)
    VALGRIND_MEMPOOL_ALLOC(guardian_records, (guardian_records + (i * sizeof(GuardianRecordDescriptor))), sizeof(GuardianRecordDescriptor));
#endif
    descriptor_ptr = (GuardianRecordDescriptor *)(guardian_records + (i * sizeof(GuardianRecordDescriptor)));
    descriptor_ptr->status = COLUMN_STATUS(i);
    descriptor_ptr->timestamp = COLUMN_TIMESTAMP(i);
    descriptor_ptr->gid = COLUMN_GID(i);
    descriptor_ptr->originator.uid = COLUMN_ORIGINATOR(i);
    descriptor_ptr->guardian.uid = COLUMN_GUARDIAN(i);
  }

  return_success:
  collection_ptr_out->collection_sz = result.nb_rows;
  collection_ptr_out->collection = (collection_t **) guardian_records;
  h_clean_result(&result);
  free (sql_query_str);
  THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *)collection_ptr_out, RESCODE_BACKEND_DATA)

  return_db_error:
  syslog(LOG_DEBUG, "%s {th_ctx: '%p'}: ERROR: COULD EXECUTE QUERY: '%s'", __func__, THREAD_CONTEXT_PTR, sql_query_str);
  rescode = RESCODE_BACKEND_CONNECTION;
  goto return_free;

  return_empty_set:
#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): ERROR: COULD FIND CORRESPONDING DB RECORD", __func__, pthread_self(), THREAD_CONTEXT_PTR);
#endif
  rescode = RESCODE_BACKEND_DATA_EMPTYSET;
  goto return_free_sql_handle;
  return_free_sql_handle:
  h_clean_result(&result);

  return_free:
  free (sql_query_str);

  return_error:
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)

#undef SQL_GET_GUARDIAN_RECORD
#undef COLUMN_ID
#undef COLUMN_GUARDIAN
#undef COLUMN_ORIGINATOR
#undef COLUMN_STATUS
#undef COLUMN_TIMESTAMP
#undef COLUMN_GID
#undef COLUMN_DATA_IS_EMPTY
#undef COLUMN_DATA
#undef COLUMN_DATA_LEN
#undef SQL_GET_GUARDIAN_RECORD
#undef SQL_GET_GUARDIAN_RECORD_WITH_STATUS
}

UFSRVResult *
DbBackendGetGuardianRecordForOriginator(GuardianRecordDescriptor *descriptor_ptr)
{
#define SQL_GET_ORIGINATOR_RECORD "SELECT * FROM guardians WHERE  originator = '%lu'"
#define SQL_GET_ORIGINATOR_RECORD_WITH_STATUS "SELECT * FROM guardians WHERE  originator = '%lu' AND status = '%u'"
#define COLUMN_ID(x)	    ((struct _h_type_int *)result.data[x][0].t_data)->value //0
#define COLUMN_GUARDIAN(x)	    ((struct _h_type_int *)result.data[x][1].t_data)->value
#define COLUMN_ORIGINATOR(x)	    ((struct _h_type_int *)result.data[x][2].t_data)->value
#define COLUMN_STATUS(x)	    ((struct _h_type_int *)result.data[x][3].t_data)->value
#define COLUMN_TIMESTAMP(x)	    ((struct _h_type_int *)result.data[x][4].t_data)->value
#define COLUMN_GID(x)	    ((struct _h_type_int *)result.data[x][5].t_data)->value
#define COLUMN_DATA_IS_EMPTY(x)          IS_EMPTY(((struct _h_type_blob *)result.data[x][6].t_data))
#define COLUMN_DATA(x)          (char *)(((struct _h_type_blob *)result.data[x][6].t_data)->value)
#define COLUMN_DATA_LEN(x)          ((struct _h_type_blob *)result.data[x][6].t_data)->length

  int 	rescode;
  struct _h_result result;
  char *sql_query_str;

  if (descriptor_ptr->status != GUARDIAN_STATUS_NONE) sql_query_str = mdsprintf(SQL_GET_ORIGINATOR_RECORD_WITH_STATUS, descriptor_ptr->originator.uid, descriptor_ptr->status);
  else sql_query_str = mdsprintf(SQL_GET_ORIGINATOR_RECORD, descriptor_ptr->originator.uid);

#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): GENERATED SQL QUERY: '%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, sql_query_str);
#endif

  int sql_result = h_query_select(THREAD_CONTEXT_DB_BACKEND, sql_query_str, &result);

  if (sql_result != H_OK)		goto return_db_error;
  if (result.nb_rows == 0)	goto return_empty_set;
  if (COLUMN_DATA_IS_EMPTY(0))	goto return_db_empty_jsonstr;

  const char *account_data_json_str = strndupa(COLUMN_DATA(0), COLUMN_DATA_LEN(0));
  size_t 			jsonstr_sz = strlen(account_data_json_str);

  if (unlikely(jsonstr_sz == 0))	goto return_db_empty_jsonstr;

  enum 		json_tokener_error jerr;
  json_object 	*jobj_account = NULL;
  json_tokener 	*jtok = json_tokener_new();

  do {
    jobj_account = json_tokener_parse_ex(jtok, account_data_json_str, strlen(account_data_json_str));
  } while ((jerr = json_tokener_get_error(jtok)) == json_tokener_continue);

  if (jerr != json_tokener_success)	goto return_error_json_tokniser;

  unsigned long id = COLUMN_ID(0);
  //originator value already set in the provided descriptor_ptr
  descriptor_ptr->guardian.uid = COLUMN_GUARDIAN(0);
  descriptor_ptr->status = COLUMN_STATUS(0);
  descriptor_ptr->timestamp = COLUMN_TIMESTAMP(0);
  descriptor_ptr->gid = COLUMN_GID(0);
  descriptor_ptr->specs.specs_jobj = jobj_account;

  return_success:
  json_tokener_free(jtok);
  h_clean_result(&result);
  free (sql_query_str);
  THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *)(uintptr_t)id, RESCODE_BACKEND_DATA)

  return_db_error:
  syslog(LOG_DEBUG, "%s {th_ctx: '%p'}: ERROR: COULD EXECUTE QUERY: '%s'", __func__, THREAD_CONTEXT_PTR, sql_query_str);
  rescode = RESCODE_BACKEND_CONNECTION;
  goto return_free;

  return_empty_set:
#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): ERROR: COULD FIND CORRESPONDING DB RECORD", __func__, pthread_self(), THREAD_CONTEXT_PTR);
#endif
  rescode = RESCODE_BACKEND_DATA_EMPTYSET;
  goto return_free_sql_handle;

  return_error_json_tokniser:
  syslog(LOG_NOTICE, "%s (pid:'%lu' th_ctx:'%p'): JSON tokeniser Error: '%s'. Terminating.", __func__, pthread_self(), THREAD_CONTEXT_PTR, json_tokener_error_desc(jerr));
  rescode = RESCODE_PROG_JSON_PARSER;
  goto return_free_json_tokeniser;

  return_db_empty_jsonstr:
  syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p'): ERROR: DB JSON COLUMN NULL OR STRING SIZE ZERO", __func__, pthread_self(), THREAD_CONTEXT_PTR);
  rescode = RESCODE_BACKEND_RESOURCE_NULL;
  goto return_free_sql_handle;

  return_free_json_tokeniser:
  json_tokener_free(jtok);
  if (IS_PRESENT(jobj_account)) json_object_put(jobj_account);

  return_free_sql_handle:
  h_clean_result(&result);

  return_free:
  free (sql_query_str);

  return_error:
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)

#undef COLUMN_ID
#undef COLUMN_GUARDIAN
#undef COLUMN_ORIGINATOR
#undef COLUMN_STATUS
#undef COLUMN_TIMESTAMP
#undef COLUMN_GID
#undef COLUMN_DATA_IS_EMPTY
#undef COLUMN_DATA
#undef COLUMN_DATA_LEN
#undef SQL_GET_ORIGINATOR_RECORD
#undef SQL_GET_ORIGINATOR_RECORD_WITH_STATUS
}

UFSRVResult *
DbBackendDeleteGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr)
{
#define SQL_DELETE_GUARDIAN_RECORD "DELETE FROM guardians WHERE guardian = '%lu' AND originator = '%lu' AND status = '%u'"

  char *sql_query_str = mdsprintf(SQL_DELETE_GUARDIAN_RECORD, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid, descriptor_ptr->status);

#ifdef __UF_TESTING
  syslog(LOG_DEBUG, "%s {pid:'%lu', th_ctx:'%p'}: GENERATED SQL QUERY: '%s'", __func__,  pthread_self(), THREAD_CONTEXT_PTR, sql_query_str);
#endif

  int sql_result = h_query_delete(THREAD_CONTEXT_DB_BACKEND, sql_query_str);

  if (sql_result != H_OK) {
    syslog(LOG_DEBUG, LOGSTR_BACKENDDB_CONNECTION_ERROR, __func__, pthread_self(), NULL, 0UL, sql_query_str, LOGCODE_BACKENDDB_CONNECTION_ERROR);

    free (sql_query_str);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_CONNECTION)
  }

  free (sql_query_str);

  THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, RESCODE_PROG_NULL_POINTER)

#undef SQL_DELETE_GUARDIAN_RECORD
}

UFSRVResult *
CacheBackendAddGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr)
{
  PersistenceBackend *pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);
  unsigned 						rescode			=	RESCODE_BACKEND_DATA;
  redisReply 					*redis_ptr	=	NULL;

  if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_GUARDIAN_ADD, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid)))	goto return_redis_error;

  if (redis_ptr->type == REDIS_REPLY_INTEGER) {
    return_success:
    freeReplyObject(redis_ptr);
    THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, rescode)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
  if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

  return_redis_error:
  if (IS_EMPTY(redis_ptr)) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), THREAD_CONTEXT_PTR);
    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: REDIS RESULT-SET. Error: '%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, redis_ptr->str);
    rescode = RESCODE_BACKEND_DATA; goto return_error;
  }
  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: NIL SET",  __func__, pthread_self(), THREAD_CONTEXT_PTR);
    rescode = RESCODE_BACKEND_DATA; goto return_error;
  }

  return_error:
  freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)
}

UFSRVResult *
CacheBackendRemGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr)
{
  unsigned 						rescode			=	RESCODE_BACKEND_DATA;
  PersistenceBackend *pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);
  redisReply 					*redis_ptr	=	NULL;

  if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_GUARDIAN_REM, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid)))	goto return_redis_error;

  if (redis_ptr->type == REDIS_REPLY_INTEGER) {
    return_success:
    freeReplyObject(redis_ptr);
    THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, rescode)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
  if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

  return_redis_error:
  if (IS_EMPTY(redis_ptr)) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), THREAD_CONTEXT_PTR);
    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, redis_ptr->str);
    rescode = RESCODE_BACKEND_DATA; goto return_error;
  }
  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: NIL SET",  __func__, pthread_self(), THREAD_CONTEXT_PTR);
    rescode = RESCODE_BACKEND_DATA; goto return_error;
  }

  return_error:
  freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)
}

UFSRVResult *
CacheBackendGetGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr)
{
  PersistenceBackend *pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);
  unsigned 						rescode			=	RESCODE_BACKEND_DATA;
  redisReply 					*redis_ptr	=	NULL;

  if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_FENCE_GUARDIAN_ISMEMBER, descriptor_ptr->guardian.uid, descriptor_ptr->originator.uid)))	goto return_redis_error;

  if (redis_ptr->type == REDIS_REPLY_INTEGER) {
    long long int is_member=redis_ptr->integer;
    return_success:
    freeReplyObject(redis_ptr);
    THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *)is_member, rescode)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
  if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

  return_redis_error:
  if (IS_EMPTY(redis_ptr)) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), THREAD_CONTEXT_PTR);
    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: REDIS RESULT-SET. Error: '%s'", __func__, pthread_self(), THREAD_CONTEXT_PTR, redis_ptr->str);
    rescode = RESCODE_BACKEND_DATA; goto return_error;
  }
  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: NIL SET",  __func__, pthread_self(), THREAD_CONTEXT_PTR);
    rescode = RESCODE_BACKEND_DATA; goto return_error;
  }

  return_error:
  freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)
}

bool
IsUserGuardianFor(const GuardianRecordDescriptor *descriptor_ptr)
{
  CacheBackendGetGuardianRecord(descriptor_ptr);
  if (THREAD_CONTEXT_UFSRV_RESULT_IS_SUCCESS_WITH_BACKEND_DATA) {
    bool is_member = (bool)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
    return is_member;
  }

  return false;
}
