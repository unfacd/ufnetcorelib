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


#include "ufnet_msgcore/staged_messages_ufsrv.h"
#include <ufsrv_core/delegator_sessions/thread_context_type.h>
#include <uflib/utils.h>
#include <ufsrv_core/cache_backend/redis.h>
#include <ufsrv_core/system/log_message_literals.h>

#include "ufnet_msgcore/redis_commands_staged_messages.h"
#include <ufnetcorelib/ufnet_msgcore/session_result_macros.h>

extern __thread ThreadContext ufsrv_thread_context;

/**
 * 	@brief:retrieve a record and delete its presence in the backend as one transaction
 * 	@para payloadname: for retrieving a specific record (not implemented)
 * 	@return redisReply *[2]. The actual retrieved payload is at idx=0, which is another redisReplay *
 * 	@dynamic_memory redisreply *: EXPORTS
 */
UFSRVResult *
GetRemStagedMessageCacheRecordForIntraCommand(Session *sesn_ptr_carrier, ParsedMessageDescriptor *msg_desc_ptr,  const char *payload_name, enum StoredMessageOptions msg_opts)
{
  int 		rescode										= RESCODE_PROG_NULL_POINTER;

  if (unlikely(IS_EMPTY(sesn_ptr_carrier)))			goto return_generic_error;
  //if (unlikely(IS_EMPTY(storage_id)))		goto return_final;

  const char 					*command_template_get,
          *command_template_rem;
  PersistenceBackend	*pers_ptr	= THREAD_CONTEXT_USRMSG_CACHEBACKEND;
  redisReply 					*redis_ptr;

  if (msg_opts == MSGOPT_GET_REM_FIRST) {
    command_template_get = REDIS_CMD_INTRAMESSAGE_LIST_GET_EARLIEST;
    command_template_rem = REDIS_CMD_INTRAMESSAGE_LIST_REM_EARLIEST;
  }
  else if (msg_opts == MSGOPT_GET_REM_LAST) {
    command_template_get = REDIS_CMD_INTRAMESSAGE_LIST_GET_EARLIEST;
    command_template_rem = REDIS_CMD_INTRAMESSAGE_LIST_REM_EARLIEST;
  }
  else if (msg_opts == MSGOPT_GET_REM_ALL) goto return_generic_error;//TODO IMPLEMENT

  (*pers_ptr->send_command_multi)(sesn_ptr_carrier, THREAD_CONTEXT_USRMSG_CACHEBACKEND, "MULTI");
  (*pers_ptr->send_command_multi)(sesn_ptr_carrier, THREAD_CONTEXT_USRMSG_CACHEBACKEND, command_template_get, msg_desc_ptr->instance_descriptor_ptr->server_class, msg_desc_ptr->instance_descriptor_ptr->ufsrv_geogroup);
  (*pers_ptr->send_command_multi)(sesn_ptr_carrier, THREAD_CONTEXT_USRMSG_CACHEBACKEND, command_template_rem, msg_desc_ptr->instance_descriptor_ptr->server_class, msg_desc_ptr->instance_descriptor_ptr->ufsrv_geogroup);
  (*pers_ptr->send_command_multi)(sesn_ptr_carrier, THREAD_CONTEXT_USRMSG_CACHEBACKEND, "EXEC");

#define COMMAND_SET_SIZE	4
#define EXEC_COOMAND_IDX (COMMAND_SET_SIZE - 1)

  size_t					commands_successful	= COMMAND_SET_SIZE;
  redisReply			*replies[COMMAND_SET_SIZE]; memset (replies, 0, sizeof(replies));

  for (size_t i=0; i<COMMAND_SET_SIZE; i++) {
    if ((RedisGetReply((UfsrvSession *)sesn_ptr_carrier, pers_ptr, (void *)&replies[i])) != REDIS_OK) {
      --commands_successful;

      if ((replies[i] != NULL)) {
        syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', reqid:'%lu', idex:'%lu'): ERROR PROCESSING MULTI SET COMMAND. ERROR: '%s'", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), msg_desc_ptr->instance_descriptor_ptr->reqid, i, replies[i]->str);
      } else {
        syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', reqid:'%lu', i:'%lu'): ERROR COULD ISSUE GET COMMAND: BACKEND CONNECTIVITY ERROR", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), msg_desc_ptr->instance_descriptor_ptr->reqid, i);
      }
    }
  }//for

  if (unlikely(commands_successful != COMMAND_SET_SIZE)) {
    syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', reqid:'%lu'): ERROR: REDIS TRANSCTION ERROR: NOT ALL COMMAND SUCCESSED: REVOING TRANSACTION", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), msg_desc_ptr->instance_descriptor_ptr->reqid);

    for (size_t i=0; i<COMMAND_SET_SIZE; i++)	if (IS_PRESENT(replies[i]))	freeReplyObject(replies[i]);

    _RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_DATA)
  }

  //remove replies upto  EXEC
  for (size_t i=0; i<COMMAND_SET_SIZE-1; i++)	if (IS_PRESENT(replies[i]))	freeReplyObject(replies[i]);

  if (unlikely(IS_EMPTY(replies[EXEC_COOMAND_IDX]))) {
    syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', reqid:'%lu'): ERROR: REDIS TRANSCTION ERROR: NULL COMMAND ARRAY RESPONSE FOR EXEC ELEMENT", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), msg_desc_ptr->instance_descriptor_ptr->reqid);

    _RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_DATA)
  }

#define GET_COMMAND_IDX	0
#define REM_COMMAND_IDX 1

  if (likely(replies[EXEC_COOMAND_IDX]->elements == COMMAND_SET_SIZE-2)) {
    if (replies[EXEC_COOMAND_IDX]->element[GET_COMMAND_IDX]->elements == 1) {//this contains the payload in redis array
      if (replies[EXEC_COOMAND_IDX]->element[REM_COMMAND_IDX]->integer == 1) {
        return_success:
        _RETURN_RESULT_SESN(sesn_ptr_carrier, replies[EXEC_COOMAND_IDX], RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)
      }
    }

    goto return_error_exec_command;
  }

  return_error_exec_command:
  syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', reqid:'%lu', rem_redis_int:'%llu'): ERROR: EXEC command error", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), msg_desc_ptr->instance_descriptor_ptr->reqid, replies[EXEC_COOMAND_IDX]->element[REM_COMMAND_IDX]->integer);
  freeReplyObject(replies[EXEC_COOMAND_IDX]);
  _RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_RESOURCE_NULL)

  return_generic_error:
  _RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_RESOURCE_NULL)


#undef COMMAND_SET_SIZE
#undef EXEC_COOMAND_IDX
#undef GET_COMMAND_IDX
#undef REM_COMMAND_IDX
}


/**
 * 	@brief: Remove individual cache record from staged storage index and hash
 */
UFSRVResult *
DeleteStagedMessageCacheRecordForUser(Session *sesn_ptr, StagedMessageDescriptor *staged_message_descriptor_ptr)
{
  int 		rescode										= RESCODE_PROG_NULL_POINTER;

  if (unlikely(IS_EMPTY(sesn_ptr)))			goto return_generic_error;

  PersistenceBackend	*pers_ptr	= THREAD_CONTEXT_USRMSG_CACHEBACKEND;
  redisReply 					*redis_ptr;

  (*pers_ptr->send_command_multi)(sesn_ptr, THREAD_CONTEXT_USRMSG_CACHEBACKEND, "MULTI");
  (*pers_ptr->send_command_multi)(sesn_ptr, THREAD_CONTEXT_USRMSG_CACHEBACKEND, REDIS_CMD_STAGED_OUTMSG_MSG_RECORD_DEL, staged_message_descriptor_ptr->userid, staged_message_descriptor_ptr->fid, staged_message_descriptor_ptr->gid);
  (*pers_ptr->send_command_multi)(sesn_ptr, THREAD_CONTEXT_USRMSG_CACHEBACKEND, REDIS_CMD_STAGED_OUTMSG_EVENT_RECORD_REM, staged_message_descriptor_ptr->userid, staged_message_descriptor_ptr->fid, staged_message_descriptor_ptr->gid);
  (*pers_ptr->send_command_multi)(sesn_ptr, THREAD_CONTEXT_USRMSG_CACHEBACKEND, "EXEC");

  size_t		commands_processed = 4,
          commands_successful= 4;

  //TODO: error recovery not done well...
  {
    size_t 			i;
    redisReply	*replies[commands_processed];

    //TODO: we need error recover for intermediate errors
    for (i=0; i<commands_processed; i++) {
      if ((RedisGetReply((UfsrvSession *)sesn_ptr, pers_ptr, (void*)&replies[i])) != REDIS_OK) {
        commands_successful--;

        if ((replies[i] != NULL)) {
          syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', uid:'%lu', idex:'%lu'): ERROR PROCESSING MULTI SET COMMAND. ERROR: '%s'", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), staged_message_descriptor_ptr->userid, i, replies[i]->str);
        } else {
          syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', i:'%lu'): ERROR COULD ISSUE GET COMMAND: BACKEND CONNECTIVITY ERROR", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), i);
        }
      }

      if (!IS_EMPTY(replies[i]))	freeReplyObject(replies[i]);
    }
  }

  if (commands_successful!=commands_processed)	{rescode = RESCODE_BACKEND_DATA_PARTIALSET; goto return_final;}

  return_success:
  _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA_SETCREATED);

  return_final:
  _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode);

  return_generic_error:
  syslog(LOG_DEBUG, LOGSTR_NULL_PARAM, __func__, pthread_self(), LOGCODE_PROTO_MISSING_PARAM, "Target Session *");
  return _ufsrv_result_generic_error;

}


UFSRVResult *
GetStageMessageCacheBackendListSize(Session *sesn_ptr, unsigned long userid)
{
  unsigned 						rescode			=	RESCODE_BACKEND_DATA;
  PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_USRMSG_CACHEBACKEND;
  redisReply 					*redis_ptr	=	NULL;

  if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr, pers_ptr, REDIS_CMD_STAGED_OUTMSG_EVENT_COUNT, userid)))	goto return_redis_error;

  if (redis_ptr->type == REDIS_REPLY_INTEGER) {
    size_t list_sz = (size_t)redis_ptr->integer; //shouldn't have problems with negative as we dont store them in this context
    freeReplyObject(redis_ptr);

    _RETURN_RESULT_SESN(sesn_ptr, (void *) (uintptr_t) list_sz, RESULT_TYPE_SUCCESS, rescode)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
  if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

  return_redis_error:
  if (IS_EMPTY(redis_ptr)) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));
    _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode);
  }
  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), redis_ptr->str);
    rescode = RESCODE_BACKEND_DATA; goto return_error;
  }
  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: NIL SET",  __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));
    rescode = RESCODE_BACKEND_DATA; goto return_error;
  }

  return_error:
  freeReplyObject(redis_ptr);
  _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)

}
