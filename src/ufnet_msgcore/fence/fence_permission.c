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

#include <main_ufnetcorelib.h>
#include <ufnet_msgcore/fence/fence_permission.h>
#include <uflib/recycler/recycler.h>
#include <ufnet_msgcore/fence/fence.h>
#include <json-c/json_object.h>
#include <ufsrv_core/cache_backend/redis.h>
#include <uflib/lzf/lzf.h>
#include <uflib/ufsrvuid.h>

#include "ufnet_core/ufsrv_worker_user_data_type.h"
#include "ufnet_msgcore/session.h"
#include "ufnet_msgcore/type_providers/session_provider.h"
#include <ufnetcorelib/ufnet_msgcore/session_result_macros.h>

extern __thread ThreadContext ufsrv_thread_context;

//Align with enum.
//IMPORTANT: THESE ARE USED FOR REDIS SET: DO NOT CHANGE
static const char *permission_names[] = {
		"", "perm_presentation", 	"perm_membership", "perm_messaging", "perm_attaching", "perm_calling"
};

static UFSRVResult *_CacheBackendPermissionMembersSetGetAll(Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *permission_ptr, CollectionDescriptor *collection_ptr_out);
static struct json_object *_JsonFormatFencePermissionByCacheRecord(Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *permission_ptr);

__unused bool
InitialiseFencePermissions(Fence *f_ptr)
{
	return 	((InitialiseFencePermission(FENCE_PERMISSIONS_PRESENTATION_PTR(f_ptr), PERM_PRESENTATION) == 0)	&&
					(InitialiseFencePermission(FENCE_PERMISSIONS_MEMBERSHIP_PTR(f_ptr), PERM_MEMBERSHIP) == 0)			&&
					(InitialiseFencePermission(FENCE_PERMISSIONS_MESSAGING_PTR(f_ptr), PERM_MESSAGING) == 0)				&&
					(InitialiseFencePermission(FENCE_PERMISSIONS_ATTACHING_PTR(f_ptr), PERM_ATTACHING) == 0)				&&
					(InitialiseFencePermission(FENCE_PERMISSIONS_CALLING_PTR(f_ptr), PERM_CALLING) == 0));

}

void
InitialiseFencePermissionsTypes(Fence *f_ptr)
{
  FENCE_PERMISSION_TYPE(FENCE_PERMISSIONS_PRESENTATION_PTR(f_ptr))  = PERM_PRESENTATION;
  FENCE_PERMISSION_TYPE(FENCE_PERMISSIONS_MEMBERSHIP_PTR(f_ptr))    = PERM_MEMBERSHIP;
  FENCE_PERMISSION_TYPE(FENCE_PERMISSIONS_MESSAGING_PTR(f_ptr))     = PERM_MESSAGING;
  FENCE_PERMISSION_TYPE(FENCE_PERMISSIONS_ATTACHING_PTR(f_ptr))     = PERM_ATTACHING;
  FENCE_PERMISSION_TYPE(FENCE_PERMISSIONS_CALLING_PTR(f_ptr))       = PERM_CALLING;

}

//TODO: these can be removed from Fence type and provided as global macros, because they are constants as far as all fence permissions are concerned
void InitialiseFencePermissionsSpecs(Fence *f_ptr)
{
//	FENCE_PERMISSIONS_PFACTOR(f_ptr)=HOPSCOTCH_INIT_BSIZE_FACTOR;
//	FENCE_PERMISSIONS_KEYLEN(f_ptr)=sizeof(unsigned long);
//	FENCE_PERMISSIONS_KEYOFFSET(f_ptr)=offsetof(Session, ufnet_session.sservice.user.user_details.user_id);
}

void
ResetFencePermissions(Fence *f_ptr)
{
	if (FENCE_PERMISSION_CONFIG_INIT((FENCE_PERMISSIONS_PRESENTATION_PTR(f_ptr))))	ResetFencePermission(FENCE_PERMISSIONS_PRESENTATION_PTR(f_ptr));
	if (FENCE_PERMISSION_CONFIG_INIT((FENCE_PERMISSIONS_MEMBERSHIP_PTR(f_ptr))))	ResetFencePermission(FENCE_PERMISSIONS_MEMBERSHIP_PTR(f_ptr));
	if (FENCE_PERMISSION_CONFIG_INIT((FENCE_PERMISSIONS_MESSAGING_PTR(f_ptr))))	ResetFencePermission(FENCE_PERMISSIONS_MESSAGING_PTR(f_ptr));
	if (FENCE_PERMISSION_CONFIG_INIT((FENCE_PERMISSIONS_ATTACHING_PTR(f_ptr))))	ResetFencePermission(FENCE_PERMISSIONS_ATTACHING_PTR(f_ptr));
	if (FENCE_PERMISSION_CONFIG_INIT((FENCE_PERMISSIONS_CALLING_PTR(f_ptr))))	ResetFencePermission(FENCE_PERMISSIONS_CALLING_PTR(f_ptr));
}

/**
 * 	@brief: deallocates storage associated with hashtable
 */
void
ResetFencePermission(FencePermission *ht_ptr_permission)
{
	hopscotch_release(FENCE_PERMISSION_PERMITTED_USERS_PTR(ht_ptr_permission));
	memset(ht_ptr_permission, 0, sizeof(FencePermission));

}

/**
 * 	@brief: This only initialises the storage and type (type is also initialised when type pool is used)
 */
int
InitialiseFencePermission(FencePermission *permission_ptr, EnumFencePermissionType permission_type)
{
	if (!FENCE_PERMISSION_CONFIG_INIT(permission_ptr)) {
		if (IS_PRESENT(hopscotch_init_with_offset(FENCE_PERMISSION_PERMITTED_USERS_PTR(permission_ptr), CONFIG_FENCE_PERMISSIONS_PFACTOR))) {
			FENCE_PERMISSION_TYPE(permission_ptr) = permission_type;
			FENCE_PERMISSION_CONFIG_INIT(permission_ptr) = true;
			return 0;
		} else return -1;
	}

	return 0;//already allocated anyway
}

static size_t _FormatUsersWithPermissionForPersistence (Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *ht_ptr_permission, BufferDescriptor *buffer_stored, BufferDescriptor *buffer_out);

int
LoadListSemanticsForBackendPersistence(const Fence *f_ptr)
{
	int list_semantics = 0;

	if (FENCE_PERMISSIONS_PRESENTATION(f_ptr).config.whitelist)	list_semantics |= (0x1<<PERM_PRESENTATION);
	if (FENCE_PERMISSIONS_MEMBERSHIP(f_ptr).config.whitelist)		list_semantics |= (0x1<<PERM_MEMBERSHIP);
	if (FENCE_PERMISSIONS_MESSAGING(f_ptr).config.whitelist)		list_semantics |= (0x1<<PERM_MESSAGING);
	if (FENCE_PERMISSIONS_ATTACHING(f_ptr).config.whitelist)		list_semantics |= (0x1<<PERM_ATTACHING);
	if (FENCE_PERMISSIONS_CALLING(f_ptr).config.whitelist)			list_semantics |= (0x1<<PERM_CALLING);

	return list_semantics;
}

/**
 * 	@brief: Decode listSemantics value as stored in cache backend
 * 	All permissions list semantics default to "blacklist" unless \ref FencePermission.config.whitelist bit is turned on.
 * 	When stored in backend cache, the @param list_semantics_persisted will have various bits on and/or off. Where it's on
 * 	when set "whitelist" semantics on for a given permission type.
 *
 */
void
MapPermissionsListSemanticsFromBackendPersistence(Fence *f_ptr, int list_semantics_persisted)
{
	if (list_semantics_persisted&(0x1<<PERM_PRESENTATION))	FENCE_PERMISSIONS_PRESENTATION(f_ptr).config.whitelist = true;
	if (list_semantics_persisted&(0x1<<PERM_MEMBERSHIP))		FENCE_PERMISSIONS_MEMBERSHIP(f_ptr).config.whitelist = true;
	if (list_semantics_persisted&(0x1<<PERM_MESSAGING))			FENCE_PERMISSIONS_MESSAGING(f_ptr).config.whitelist = true;
	if (list_semantics_persisted&(0x1<<PERM_ATTACHING))			FENCE_PERMISSIONS_ATTACHING(f_ptr).config.whitelist = true;
	if (list_semantics_persisted&(0x1<<PERM_CALLING))				FENCE_PERMISSIONS_CALLING(f_ptr).config.whitelist = true;
}

//
//UFSRVResult *
//InstateFencePersmissionMembers (Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *permission_ptr, unsigned long fence_call_flags)
//{
//	unsigned					instan_flags									= 0;
//	redisReply 				**redis_ptr_session_records 	= (redisReply **)collection_ptr->collection;
//	size_t 						list_count										=	collection_ptr->collection_sz;
//	redisReply				*processed_users[list_count]; //@WARNING: STACK OVERFLOW RISK
//
//	memset (processed_users, 0, sizeof(processed_users));
//
//	instan_flags|=fence_call_flags&CALL_FLAG_REMOTE_SESSION?CALL_FLAG_REMOTE_SESSION:0;
//	instan_flags|=(CALL_FLAG_LOCK_SESSION|CALL_FLAG_HASH_SESSION_LOCALLY|CALL_FLAG_HASH_UID_LOCALLY|CALL_FLAG_HASH_USERNAME_LOCALLY);//no CALL_FLAG_ATTACH_FENCE_LIST_TO_SESSION
//
//	unsigned long userid_member;
//	Session 			*sesn_ptr_member;
//	for (size_t i=0; i<list_count; i++)
//	{
//		redisReply *redis_ptr=*(redis_ptr_session_records+i);
//
//		if (IS_EMPTY(redis_ptr))
//		{
//			syslog(LOG_DEBUG, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu'}: ERROR: User at index '%lu' is NULL: RECIEVED '%lu' elements to attach to FENCE", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), f_ptr, FENCE_ID(f_ptr), i, list_count);
//
//			processed_users[i]=NULL;
//			continue;
//		}
//
//		if ((userid_member=strtoul(redis_ptr->str, NULL, 10))==0)
//		{
//			syslog(LOG_NOTICE, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu', uid_invalid:'%s'}: WARNING: (NOT IMPLEMENTED) ENTIRE USER CACHEBACKEND RECORD WILL BE BLOWN OFF: User at index '%lu' is NULL: RECIEVED '%lu' elements to attach to FENCE", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), f_ptr, FENCE_ID(f_ptr), redis_ptr->str, i, list_count);
//
////			_ClearInvalidUserId (sesn_ptr_carrier, f_ptr, redis_ptr); //todo: look into invalid uid
//
//			continue;
//		}
//
//		if (IS_PRESENT((sesn_ptr_member=GetSessionForThisUserByUserId(sesn_ptr_carrier, userid_member, instan_flags))))
//		{
//			struct ValueMapper value_mapper={.value=SESSION_USERID(sesn_ptr_member)};
//
//			if ((hopscotch_insert(FENCE_PERMISSION_PERMITTED_USERS_PTR(permission_ptr), value_mapper.value_mapped, (void *)sesn_ptr_member))!=0)//TODO:  refcount
//			{
//				processed_users[i]=NULL;
//				syslog(LOG_NOTICE, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu', uid_member:'%lu'}: ERROR: COULD NOT HASH MMBR INTO PRMISSION SET", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), f_ptr, FENCE_ID(f_ptr), SESSION_USERID(sesn_ptr_member));
//			}
//		}
//	}//for
//
//}


//TODO: perhaps consider protobuf for array encoding
__unused static size_t
_FormatUsersWithPermissionForPersistence(Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *ht_ptr_permission, BufferDescriptor *buffer_stored, BufferDescriptor *buffer_persisted_users)
{
	size_t stored_counter  = 0;
	unsigned long *stored_out = (unsigned long *)buffer_stored->data;
  struct hopscotch_bucket *buckets = ht_ptr_permission->permitted_users.buckets;

	for (size_t i = 0; i < (1ULL << ht_ptr_permission->permitted_users.pfactor); i++ ) {
		if (IS_PRESENT(buckets[i].data)) {
			Session *sesn_ptr_stored = SessionOffInstanceHolder((InstanceHolderForSession *)buckets[i].data);
			stored_out[stored_counter++] = SESSION_USERID(sesn_ptr_stored);
		}
	}

	if (stored_counter > 0) {
		buffer_persisted_users->size = lzf_compress(stored_out, buffer_stored->size_max, buffer_persisted_users->data, buffer_persisted_users->size_max);
	}

	return stored_counter;
}

/**
 *
 * @param permission_ptr
 * @param executor
 * @param ctx_data_ptr
 * @param is_initialise
 * @return count of how many units were executed (regardless of success state)
 */
size_t
InvokeFencePermissionListIteratorExecutor(FencePermission *permission_ptr, CallbackExecutor executor, ClientContextData *ctx_data_ptr, bool is_initialise)
{
  return hopscotch_iterator_executor_with_offset(&(permission_ptr->permitted_users), executor, ctx_data_ptr);
}

UFSRVResult *
CacheBackendPermissionMembersSetAdd(Fence *f_ptr, FencePermission *permission_ptr, unsigned long userid_target)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	UfsrvWorkerUserData *worker_data = THREAD_CONTEXT.user_data;
	PersistenceBackend *pers_ptr = worker_data ? worker_data->fence_cachebackend : THREAD_CONTEXT_FENCE_CACHEBACKEND;
	redisReply 					*redis_ptr	=	NULL;

	if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_FENCE_PERMISSION_SET_ADD_USER, FENCE_ID(f_ptr), permission_names[permission_ptr->type], userid_target)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_INTEGER) {
		return_success:
		freeReplyObject(redis_ptr);
    _RETURN_RESULT_RES(THREAD_CONTEXT.res_ptr, NULL, RESULT_TYPE_SUCCESS, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), THREAD_CONTEXT_PTR);
    _RETURN_RESULT_RES(THREAD_CONTEXT.res_ptr, NULL, RESULT_TYPE_ERR, rescode)
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
  _RETURN_RESULT_RES(THREAD_CONTEXT.res_ptr, NULL, RESULT_TYPE_ERR, rescode)

}

UFSRVResult *
CacheBackendPermissionMembersSetRem(Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *permission_ptr, unsigned long userid_target)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_FENCE_CACHEBACKEND;
	redisReply 					*redis_ptr	=	NULL;

	if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr_carrier, THREAD_CONTEXT_FENCE_CACHEBACKEND, REDIS_CMD_FENCE_PERMISSION_SET_DEL_USER, FENCE_ID(f_ptr), permission_names[permission_ptr->type], userid_target)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_INTEGER) {
		return_success:
		freeReplyObject(redis_ptr);
		_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_SUCCESS, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier));
	 _RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), redis_ptr->str);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}
	if (redis_ptr->type == REDIS_REPLY_NIL) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: NIL SET",  __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier));
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}

	return_error:
	freeReplyObject(redis_ptr);
	_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, rescode)

}

/**
 * @brief Query if user is present on a given fence's permission members list.
 * @param fid the fence id
 * @param permission_ptr the permission set being queried
 * @param userid_target user being queried
 * @return UfsrvResul with result poyload: 1 being user present
 */
UFSRVResult *
CacheBackendPermissionMembersIsMember(long fid, FencePermission *permission_ptr, unsigned long userid_target)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_FENCE_CACHEBACKEND;
	redisReply 					*redis_ptr	=	NULL;

	if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_FENCE_PERMISSION_SET_ISMEMBER, fid, permission_names[permission_ptr->type], userid_target)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_INTEGER) {
		long long int is_member = redis_ptr->integer;
		return_success:
		freeReplyObject(redis_ptr);
    THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *)is_member, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, uid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), userid_target);
   THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, userid:'%lu'}: ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), userid_target, redis_ptr->str);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}
	if (redis_ptr->type == REDIS_REPLY_NIL) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, yserid:'%lu'}: ERROR: NIL SET",  __func__, pthread_self(), userid_target);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}

	return_error:
	freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)

}

UFSRVResult *_CacheBackendPermissionMembersGetAll(Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *permission_ptr, CollectionDescriptor *collection_ptr_out);

/**
 * 	@brief: It is caller's responsibility to ensure hash table for the permission is in true uninitialised state, no elements
 * 	are tested for existence before insertion.
 * 	This function does not check if the list is initialised already (use _InitialiseMembersIfNecessary() for that)
 * 	@dynamic_memory redis_ptr: IMPORTS/DEALLOCATES
 */
UFSRVResult *
InstateFencePermissionMembers(Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *permission_ptr, unsigned long fence_call_flags)
{
	CollectionDescriptor collection = {0};
	_CacheBackendPermissionMembersSetGetAll(sesn_ptr_carrier, f_ptr, permission_ptr, &collection);

	if (SESSION_RESULT_TYPE_SUCCESS(sesn_ptr_carrier)) {
		if (collection.collection_sz == 0) {
			FENCE_PERMISSION_CONFIG_USERS_LOADED(permission_ptr) = true;
			freeReplyObject((redisReply *)collection.collection);
			_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_NULL_POINTER);
		}

		unsigned long userid_member;
		unsigned long instan_flags = 0;
		Session				*sesn_ptr_member;
		redisReply 		*redis_ptr = (redisReply *)collection.collection;
		redisReply		*processed_users[collection.collection_sz]; //@WARNING: STACK OVERFLOW RISK

		instan_flags |= fence_call_flags&CALL_FLAG_REMOTE_SESSION ? CALL_FLAG_REMOTE_SESSION : 0;
		instan_flags |= (CALL_FLAG_HASH_SESSION_LOCALLY|CALL_FLAG_HASH_UID_LOCALLY|CALL_FLAG_HASH_USERNAME_LOCALLY);//no CALL_FLAG_ATTACH_FENCE_LIST_TO_SESSION

		memset(processed_users, 0, sizeof(processed_users));

		for (size_t i=0; i<collection.collection_sz; i++) {
			redisReply *redis_ptr_member = redis_ptr->element[i];

			if (IS_EMPTY(redis_ptr_member)) {
				syslog(LOG_DEBUG, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu'}: ERROR: User at index '%lu' is NULL", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), f_ptr, FENCE_ID(f_ptr), i);

				processed_users[i] = NULL;
				continue;
			}

			if ((userid_member = strtoul(redis_ptr_member->str, NULL, 10)) == 0) {
				syslog(LOG_NOTICE, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu', uid_invalid:'%s'}: WARNING: (NOT IMPLEMENTED) ENTIRE USER CACHEBACKEND RECORD WILL BE BLOWN OFF: User at index '%lu' is NULL", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), f_ptr, FENCE_ID(f_ptr), redis_ptr->str, i);

	//			_ClearInvalidUserId (sesn_ptr_carrier, f_ptr, redis_ptr); //todo: look into invalid uid

				continue;
			}

			GetSessionForThisUserByUserId(sesn_ptr_carrier, userid_member, NULL/*lock_state*/, instan_flags);
			InstanceHolderForSession *instance_sesn_ptr_member = (InstanceHolderForSession *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
			if (IS_PRESENT(instance_sesn_ptr_member)) {
			  sesn_ptr_member = SessionOffInstanceHolder(instance_sesn_ptr_member);

				if ((hopscotch_insert_with_offset(FENCE_PERMISSION_PERMITTED_USERS_PTR(permission_ptr),
                                          PermissionListItemExtractorCallback, (void *) instance_sesn_ptr_member,
                                          CONFIG_FENCE_PERMISSIONS_KEYOFFSET(Session, ufnet_session.sservice.user.user_details.user_id))) != 0) {//TODO:  refcount
					processed_users[i] = NULL;
					syslog(LOG_NOTICE, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu', uid_member:'%lu'}: ERROR: COULD NOT HASH MMBR INTO PRMISSION SET", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), f_ptr, FENCE_ID(f_ptr), SESSION_USERID(sesn_ptr_member));
				}
			}
		}

		return_success:
		FENCE_PERMISSION_CONFIG_USERS_LOADED(permission_ptr) = true;
		freeReplyObject(redis_ptr);
		_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_NULL_POINTER)
	}

	_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, RESCODE_PROG_NULL_POINTER)

}

/**
 * @brief Retrieve members set associated with a give fence permission type.
 * @param[OUT] collection_ptr_out A pre allocated collection to save permission's members into. Collection will contain raw redisReply elements.
 * @dynamic_memory redis_ptr: EXPORTS
 */
static UFSRVResult *
_CacheBackendPermissionMembersSetGetAll(Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *permission_ptr, CollectionDescriptor *collection_ptr_out)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_FENCE_CACHEBACKEND;//persistance_backend;
	redisReply 					*redis_ptr	=	NULL;

	if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr_carrier, THREAD_CONTEXT_FENCE_CACHEBACKEND, REDIS_CMD_FENCE_PERMISSION_SET_GET_USERS, FENCE_ID(f_ptr), permission_names[permission_ptr->type])))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_ARRAY) {
		return_success:
		collection_ptr_out->collection = (collection_t **)redis_ptr;
		collection_ptr_out->collection_sz = redis_ptr->elements;
		_RETURN_RESULT_SESN(sesn_ptr_carrier, collection_ptr_out, RESULT_TYPE_SUCCESS, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier));
	 _RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), redis_ptr->str);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}

	if (redis_ptr->type == REDIS_REPLY_NIL) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: NIL SET",  __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier));
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}

	return_error:
	freeReplyObject(redis_ptr);
	_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, rescode)

}

/**
 * @brief Convenient grouping of all fence permissions json formatters returned as a json array.
 * @param sesn_ptr_carrier
 * @param f_ptr fence containing permissions data.
 * @return json_object containing binary representation of the permissions
 * @dynamic_memory EXPORTS json_object *
 */
json_object *
JsonFormatFencePermissions(Session *sesn_ptr_carrier, Fence *f_ptr)
{
  json_object	*jarray_permission = json_object_new_array();
  json_object *jobj_permission;

  jobj_permission = _JsonFormatFencePermissionByCacheRecord(sesn_ptr_carrier, f_ptr, FENCE_PERMISSIONS_PRESENTATION_PTR(f_ptr));
  if (IS_PRESENT(jobj_permission)) json_object_array_add(jarray_permission, jobj_permission);

  jobj_permission = _JsonFormatFencePermissionByCacheRecord(sesn_ptr_carrier, f_ptr, FENCE_PERMISSIONS_MEMBERSHIP_PTR(f_ptr));
  if (IS_PRESENT(jobj_permission)) json_object_array_add(jarray_permission, jobj_permission);

  jobj_permission = _JsonFormatFencePermissionByCacheRecord(sesn_ptr_carrier, f_ptr, FENCE_PERMISSIONS_MESSAGING_PTR(f_ptr));
  if (IS_PRESENT(jobj_permission)) json_object_array_add(jarray_permission, jobj_permission);

  jobj_permission = _JsonFormatFencePermissionByCacheRecord(sesn_ptr_carrier, f_ptr, FENCE_PERMISSIONS_ATTACHING_PTR(f_ptr));
  if (IS_PRESENT(jobj_permission)) json_object_array_add(jarray_permission, jobj_permission);

  return jarray_permission;
}

/**
 * @brief Retrieve permission type configuration set for a fence set from the cache backend and format it in json.
 * @returns: json array containing all fence permission members of a given type {type:"", members["xx", ...]}
 */
static struct json_object *
_JsonFormatFencePermissionByCacheRecord(Session *sesn_ptr_carrier, Fence *f_ptr, FencePermission *permission_ptr)
{
  json_object	*jarray_permission_members;
  json_object	*jobj_permission		= NULL;

  CollectionDescriptor collection = {0};
  _CacheBackendPermissionMembersSetGetAll(sesn_ptr_carrier, f_ptr, permission_ptr, &collection);

  if (SESSION_RESULT_TYPE_SUCCESS(sesn_ptr_carrier)) {
    if (collection.collection_sz == 0) {
      FENCE_PERMISSION_CONFIG_USERS_LOADED(permission_ptr) = true;
      freeReplyObject((redisReply *)collection.collection);
      return NULL;
    }

    unsigned long userid_member;
    redisReply 		*redis_ptr = (redisReply *)collection.collection;
    redisReply		*processed_users[collection.collection_sz]; //@WARNING: STACK OVERFLOW RISK

    memset (processed_users, 0, sizeof(processed_users));

    jobj_permission = json_object_new_object();
    jarray_permission_members	= json_object_new_array();
    char ufsrvuid_encoded[CONFIG_MAX_UFSRV_ID_ENCODED_SZ+1] = {0};

    json_object_object_add(jobj_permission, "type", json_object_new_string(permission_names[permission_ptr->type]));

    for (size_t i=0; i<collection.collection_sz; i++) {
      redisReply *redis_ptr_member = redis_ptr->element[i];

      if (IS_EMPTY(redis_ptr_member)) {
        syslog(LOG_DEBUG, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu'}: ERROR: User at index '%lu' is NULL", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), f_ptr, FENCE_ID(f_ptr), i);

        processed_users[i] = NULL;
        continue;
      }

      if ((userid_member = strtoul(redis_ptr_member->str, NULL, 10)) == 0) {
        syslog(LOG_NOTICE, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu', uid_invalid:'%s'}: WARNING: (NOT IMPLEMENTED) ENTIRE USER CACHEBACKEND RECORD WILL BE BLOWN OFF: User at index '%lu' is NULL", __func__, pthread_self(), sesn_ptr_carrier, SESSION_ID(sesn_ptr_carrier), f_ptr, FENCE_ID(f_ptr), redis_ptr->str, i);

        continue;
      }

      CacheBackendGetRawSessionRecordByUserId(userid_member, CALLFLAGS_EMPTY, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
      if (THREAD_CONTEXT_UFSRV_RESULT_IS_SUCCESS_WITH_BACKEND_DATA) {
        redisReply *redis_ptr_user = ((redisReply *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA);
        InstanceHolderForSession *instance_sesn_ptr_member = SessionLightlyInstantiateFromBackendRaw(sesn_ptr_carrier, NULL, redis_ptr_user, CALLFLAGS_EMPTY);
        if (!IS_EMPTY(instance_sesn_ptr_member)) {
          Session *sesn_ptr_member = SessionOffInstanceHolder(instance_sesn_ptr_member);
          UfsrvUidConvertSerialise(&SESSION_UFSRVUIDSTORE(sesn_ptr_member), ufsrvuid_encoded);
          json_object_array_add(jarray_permission_members, json_object_new_string(ufsrvuid_encoded));

          memset(ufsrvuid_encoded, 0, sizeof(ufsrvuid_encoded));

          SessionReturnToRecycler(instance_sesn_ptr_member, (ContextData *) NULL, CALLFLAGS_EMPTY);
          freeReplyObject(redis_ptr_user);
        }
      }
    }

    json_object_object_add(jobj_permission, "members", jarray_permission_members);

    return_success:
    freeReplyObject(redis_ptr);
    return jobj_permission;
  }

  return NULL;

}

ClientContextData *
PermissionListItemExtractorCallback(ItemContainer *item_container_ptr)
{
  return GetClientContextData((InstanceHolder *)item_container_ptr);
}