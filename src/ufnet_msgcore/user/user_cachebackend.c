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

#include <ufnetcorelib/ufnet_msgcore/user/user_cachebackend.h>
#include <ufsrv_core/cache_backend/redis.h>
#include <ufnetcorelib/ufnet_msgcore/session.h>
#include <ufnetcorelib/ufnet_msgcore/fence/fence_cachbackend_commands_literal.h>
#include <ufnetcorelib/ufnet_msgcore/session_result_macros.h>

extern __thread ThreadContext     ufsrv_thread_context;

/**
 * 	@brief: Clear the cache associated with an invalid userid. No network-wide broadcast is performed
 * 	because of the invalidity of theSession.
 *
 *	@param sesn_ptr_carrier: just a carrier Session not target for operation
 *	@param sesn_ptr_invalid: session containin the target invalid uid
 */
UFSRVResult *
ClearBackendCacheForInvalidUserId(Session *sesn_ptr_carrier, Session *sesn_ptr_invalid, Fence *f_ptr, unsigned long call_flags)
{
	size_t				actually_processed = 0;
	PersistenceBackend	*pers_ptr = NULL;

	pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(ufsrv_thread_context);;

	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, "MULTI");

	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_COOKIE_SESSION_DEL, SESSION_COOKIE(sesn_ptr_invalid));
	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_USERNAME_USERID_MAPPING_DEL, SESSION_USERNAME(sesn_ptr_invalid));
	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_USER_SESSION_RECORD_DEL_ALL, SESSION_USERID(sesn_ptr_invalid));
	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_USER_FENCE_LIST_REM_ALL, SESSION_USERID(sesn_ptr_invalid));
	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_NICKNAMES_DIRECTORY_DEL, SESSION_USERNICKNAME(sesn_ptr_invalid)?SESSION_USERNICKNAME(sesn_ptr_invalid):"");
	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_FENCE_USERS_LIST_REM, FENCE_ID(f_ptr), SESSION_USERID(sesn_ptr_invalid));
	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_INVITED_USERS_FOR_FERNCE_REM, FENCE_ID(f_ptr), SESSION_USERID(sesn_ptr_invalid));

	//TODO: THIS NEEDS INVITED BY value. currently set to 0. this command will fail
	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_INVITED_FENCES_FOR_USER_REM, SESSION_USERID(sesn_ptr_invalid), FENCE_ID(f_ptr), 0);

	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, REDIS_CMD_MY_FENCE_INVITED_USERS_REM_ALL, SESSION_USERID(sesn_ptr_invalid));

	//TODO: ADD preferences to cleard UID
	(*pers_ptr->send_command_multi)(sesn_ptr_carrier, pers_ptr, "EXEC");
	actually_processed = 11;

	redisReply	*replies[actually_processed];

	//TODO: we need error recover for intermediate errors
	for (size_t i = 0; i < actually_processed; i++) {
		if ((RedisGetReply((UfsrvSession *)sesn_ptr_carrier, pers_ptr, (void*)&replies[i]) != REDIS_OK)) {
			syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', cid:'%lu', cmd_idx:'%lu', uid_invalid:'%lu'}: ERROR: REDIS COMMAND IN MULTI SET FAILED", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), i, SESSION_USERID(sesn_ptr_invalid));

			if ((replies[i] != NULL) && (replies[i]->type != REDIS_REPLY_NIL)) {
				//syslog (LOG_DEBUG, LOGSTR_BACKENDCACHE_SHARED_CONTACTS, __func__, SESSION_PID(sesn_ptr), sesn_ptr, i, replies[i]->str, LOGCODE_BACKENDCACHE_SHARED_CONTACTS_HIT, "Found shared contact token");
			}
		}

		if (!IS_EMPTY(replies[i]))	freeReplyObject(replies[i]);
	}//for

	_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)

}
