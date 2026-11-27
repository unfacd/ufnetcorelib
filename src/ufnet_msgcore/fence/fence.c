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
#include <uflib/recycler/instance_type.h>
#include <uflib/utils_threads.h>
#include <ufnet_msgcore/fence/fence.h>
#include <ufnet_msgcore/fence/utils_fence.h>
#include <ufnet_msgcore/utils_location.h>
#include "ufnet_core/user_prefs/user_prefs_fence.h"
#include <ufnet_msgcore/fence/utils_fence_db.h>
#include <ufnet_msgcore/fence/fence_permission.h>
#include <ufnet_msgcore/fence/typepool_context_data_fence_type.h>
#include <ufnet_msgcore/attachments.h>
#include <ufsrv_core/delegator_sessions/http_request_context_type.h>
#include <ufsrv_core/cache_backend/persistence.h>

#include <uflib/recycler/recycler.h>
#include <ufnet_msgcore/user/user_backend.h>
#include <ufsrv_core/system/thread_utils.h>
#include <uflib/ufsrvuid.h>
#include <ufnet_msgcore/json_attribute_names_fence.h>

#include "ufnet_msgcore/fence/fence_cachbackend_commands_literal.h"
#include "ufnet_msgcore/type_providers/type_provider_fence.h"
#include "ufnetcorelib/ufnet_msgcore/fence/pair_of_userid_user_name_type.h"
#include <ufnet_msgcore/json_attribute_names_account.h>

#include "ufsrv_core/cache_backend/redis.h"
#include "ufnet_msgcore/type_providers/session_provider.h"
#include "ufnet_msgcore/user/user_cachebackend.h"
#include <ufnetcorelib/ufnet_msgcore/session_result_macros.h>

//todo fix me ufsrvwebsock -> ufnetcorelib porting forward delralarions

//assigned when the typepool is initialised
static RecyclerPoolHandle *FenceStateDescriptorPoolHandle;

static int	TypePoolInitCallback_FenceStateDescriptor(ClientContextData *data_ptr, size_t oid);
static int	TypePoolGetInitCallback_FenceStateDescriptor(InstanceHolder *data_ptr, ContextData *context_data, size_t oid, unsigned long call_flags);
static int	TypePoolPutInitCallback_FenceStateDescriptor(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);
static char	*TypePoolPrintCallback_FenceStateDescriptor(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);
static int	TypePoolDestructCallback_FenceStateDescriptor(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);

static RecyclerPoolOps ops_fence_state_descriptor = {
		TypePoolInitCallback_FenceStateDescriptor,
		TypePoolGetInitCallback_FenceStateDescriptor,
		TypePoolPutInitCallback_FenceStateDescriptor,
		TypePoolPrintCallback_FenceStateDescriptor,
		TypePoolDestructCallback_FenceStateDescriptor
};

/////>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>


extern __thread ThreadContext ufsrv_thread_context;

static HashTable FenceRegistryIdHashTable;
static HashTable FenceRegistryCanonicalNameHashTable;

static size_t _CrossCheckInvitedListsForUser(InstanceHolderForSession *instance_sesn_ptr);
static List *_GetUserListForSessionByFenceCollectionType(Session *sesn_ptr, EnumFenceCollectionType fence_collection_type);
static UFSRVResult *_CheckForDuplicateUsername(Session *sesn_ptr_carrier, Fence *f_ptr, redisReply *current_user, redisReply **processed_users_collection, size_t collection_sz);
static void _ClearInvalidUserId(Session *sesn_ptr_carrier, Fence *f_ptr, redisReply *redis_ptr_invalid);
static Fence *_InstateUsersListCacheRecordsForFence(Session *sesn_ptr_target, List *fence_user_list_ptr, EnumFenceCollectionType user_fence_list_type, InstanceHolderForFence *instance_f_ptr, CollectionDescriptorPair *collection_pair_ptr, unsigned call_flags);

//todo this needs to be protected from being exported outside of this file
HashTable *
FenceRegistryIdHashTableProvide()
{
  return &FenceRegistryIdHashTable;
}

HashTable *
FenceRegistryCanonicalNameHashTableProvide()
{
  return &FenceRegistryCanonicalNameHashTable;
}

// ---START LOCKING--------------------

 inline UFSRVResult *
 FenceEventsLockRDCtx(ThreadContext *thread_ctx_ptr, Fence *f_ptr, int try_flag, UFSRVResult *res_ptr, const char *func)
 {
 	int lock_state;

 	if (IS_PRESENT(thread_ctx_ptr)) {
		if (IsObjectInLockedObjectsStore(thread_ctx_ptr->ht_ptr, f_ptr)) {
			lock_state = 0;
			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p, try:'%i', func:'%s'): NOTICE: NOT LOCKING: ALREADY IN STORE", __func__, pthread_self(), f_ptr, thread_ctx_ptr, try_flag, func);
			goto return_already_locked_by_this_thread;
		}
	}

 	if (try_flag) {//The calling thread acquires the read lock if a writer does not hold the lock and there are no writers blocked on the lock.
 		lock_state = pthread_rwlock_tryrdlock(&(f_ptr->fence_events.rwlock));
 		if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
 			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p, try:'1', func:'%s'): SUCCESS: TRY-READ lock for Fence events acquired...",__func__, pthread_self(), f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, func);
#endif
 			goto return_locked;
 		} else {
 			char *err_str = thread_error(errno);
 			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p', try:'1', func:'%s'): ERROR: COULD NOT acquire TRY-READ lock for Fence events (errno='%d'): '%s'", __func__, pthread_self(), f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, func, errno, err_str);
 		}
 	} else {
 		lock_state = pthread_rwlock_rdlock(&(f_ptr->fence_events.rwlock));
 		if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p', func:'%s'): SUCCESS: READ lock for Fence events acquired...", __func__, pthread_self(), f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, func);
#endif
			goto return_locked;
		} else {
 			char *err_str = thread_error(errno);
 			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p', func:'%s'): ERROR: COULD NOT acquire READ lock for Fence events (errno='%d'): '%s'", __func__, pthread_self(), f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, func, errno, err_str);
 		}
 	}

 	return_wont_lock:
	_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_ERR, RESCODE_PROG_WONTLOCK)

	return_locked:
	if (IS_PRESENT(thread_ctx_ptr)) PutIntoLockedObjectsStore (thread_ctx_ptr->ht_ptr, (void *)f_ptr);
	_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_LOCKED)

	return_already_locked_by_this_thread:
	_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_LOCKED_BY_THIS_THREAD)

 }

inline UFSRVResult *
FenceEventsLockRWCtx(ThreadContext *thread_ctx_ptr, Fence *f_ptr, int try_flag, UFSRVResult *res_ptr, const char *func)
{
	int lock_state;

	if (IS_PRESENT(thread_ctx_ptr)) {
		if (IsObjectInLockedObjectsStore(thread_ctx_ptr->ht_ptr, f_ptr)) {
			lock_state = 0;
			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p', try:'%i', func:'%s'): NOTICE: NOT LOCKING: ALREADY IN STORE", __func__, pthread_self(),  f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, try_flag, func);
			goto return_already_locked_by_this_thread;
		}
	}

	if (try_flag) {
		lock_state = pthread_rwlock_trywrlock(&(f_ptr->fence_events.rwlock));
		if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
      syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p', try:'1', func:'%s'): SUCCESS: TRY-WRITE/READ lock for Fence events acquired...", __func__, pthread_self(), f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, func);
#endif
			goto return_locked;
		} else {
			char *err_str = thread_error(errno);
			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p', try:'1', func:'%s'): ERROR: COULD NOT acquire TRY-WRITE/READ lock for Fence events (errno='%d'): '%s'", __func__, pthread_self(),  f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, func, errno, err_str);
		}
	} else {
		lock_state = pthread_rwlock_wrlock(&(f_ptr->fence_events.rwlock));
		if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
      syslog (LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p', func:'%s'): SUCCESS: WRITE/READ lock for Fence events acquired...",__func__, pthread_self(), f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, func);
#endif
			goto return_locked;
		} else {
			char *err_str = thread_error(errno);
			syslog (LOG_DEBUG, "%s (pid:'%lu', o:'%p', ctx:'%p', func:'%s'): ERROR: COULD NOT acquire WRITE/READ lock for Fence events (errno='%d'): '%s'", __func__, pthread_self(), f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, func, errno, err_str);
		}
	}

	return_wont_lock:
	_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_ERR, RESCODE_PROG_WONTLOCK)

	return_locked:
	if (IS_PRESENT(thread_ctx_ptr)) PutIntoLockedObjectsStore (thread_ctx_ptr->ht_ptr, (void *)f_ptr);
	_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_LOCKED)

	return_already_locked_by_this_thread:
	_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_LOCKED_BY_THIS_THREAD)

}

inline UFSRVResult *
FenceEventsUnLockCtx(ThreadContext *thread_ctx_ptr, Fence *f_ptr, UFSRVResult *res_ptr)
{
	int lock_state = pthread_rwlock_unlock(&(f_ptr->fence_events.rwlock));
	if (lock_state == 0) {
#ifdef __UF_FULLDEBUG
    syslog(LOG_DEBUG, "%s: (pid:'%lu', o:'%p', ctx:'%p'): SUCCESS: RELEASED WRITE/READ lock for Fence events...", __func__, pthread_self(), f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0);
#endif
		if (IS_PRESENT(thread_ctx_ptr)) RemoveFromLockedObjectsStore(thread_ctx_ptr->ht_ptr, (void *)f_ptr);
		_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_LOCKED)
	} else {
		char *err_str = thread_error(errno);
		syslog(LOG_DEBUG, "%s: (pid:'%lu', o:'%p', ctx:'%p'): ERROR: COULD NOT RELEASE WRITE/READ lock for Fence events (errno='%d'): '%s'", __func__, pthread_self(),  f_ptr, IS_PRESENT(thread_ctx_ptr)?thread_ctx_ptr:0, errno, err_str);
	}

	_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_ERR, RESCODE_PROG_WONTLOCK)

}

//---END LOCKING-------------------------

//static entity at the base of the Base Fence Registry.
Fence *broadcast_fence_ptr;

static Fence *_FindFenceInUserListByCanonicalName(const List *const lst_ptr, const char *fence_canonical_name);

static void SummariseFenceConfiguration(List *, const char *) __attribute__((unused));

static bool _ExtractUserIdUserNameFromListCacheRecord(char *raw_cache_field, PairOfUserIdUserName *pair);
static FenceRawSessionList *_ConstructRawUsersListForFence(Session *sesn_ptr, Fence *f_ptr, List *fence_user_list_ptr, unsigned long fence_call_flags, FenceRawSessionList *raw_sesn_list_ptr);

#ifdef _LOCAL_FENCE_EVENT_QUEUE
static void * _DestructFenceMessageQueue (Fence *);
static void *_m_AddMessageToFenceQueue (SessionService *, Fence *, Message *const);
#endif

static size_t _CleanUpFaultyUserEntriesForFence(Session *sesn_ptr, Fence *f_ptr, unsigned long userid_loaded_for, CollectionDescriptor *collection_ptr,  EnumFenceCollectionType list_type);

static Fence *_RemoveUserFromBaseFence(InstanceHolderForSession *, Fence *, unsigned long);//unsigned long);
static Fence *_RemoveUserFromUserFence(InstanceHolderForSession *, Fence *, unsigned long);//unsigned long);
static InstanceHolderForFenceStateDescriptor * _RemoveFenceFromList(List *, unsigned long);

//-----------------------------------------------------------

void
InitialiseMaserFenceRegistries(void)
{
  if (HashTableLockingInstantiate(&(FenceRegistryIdHashTable), (offsetof(Fence, fence_id)), sizeof(unsigned long), HASH_ITEM_NOT_PTR_TYPE, "FenceRegistry", RecyclerGetClientContextDataFromItemContainer)) {
    HashTableSetUniqueKeys(&(FenceRegistryIdHashTable), true);

    syslog(LOG_INFO, "%s: SUCCESS: FenceRegistryId HashTable Instantiated: key_offset: '%ld'. key_size: '%ld'", __func__, FenceRegistryIdHashTable.fKeyOffset, FenceRegistryIdHashTable.fKeySize);
  } else {
    syslog(LOG_ERR, "%s: ERROR (errno: '%d'): COULD NOT INITIALISE FenceRegistryId HashTable: TERMINATING...", __func__, errno);

    exit(-1);
  }

  if (HashTableLockingInstantiate(&(FenceRegistryCanonicalNameHashTable), (offsetof(Fence, fence_location.canonical_name)), KEY_SIZE_ZERO, HASH_ITEM_IS_PTR_TYPE, "FenceRegistryCanonicalName", RecyclerGetClientContextDataFromItemContainer)) {
    HashTableSetUniqueKeys(&(FenceRegistryCanonicalNameHashTable), true);

    syslog(LOG_INFO, "%s: SUCCESS: FenceRegistryCanonicalName HashTable Instantiated: key_offset: '%ld'. key_size: '%ld'", __func__, FenceRegistryIdHashTable.fKeyOffset, FenceRegistryIdHashTable.fKeySize);
  } else {
    syslog(LOG_ERR, "%s: ERROR (errno: '%d'): COULD NOT INITIALISE FenceRegistryCanonicalName HashTable: TERMINATING...", __func__, errno);

    exit(-1);
  }

	Fence *f_ptr = NULL;

	InstanceHolder *instance_holder_ptr = FenceGetInstance((ContextData *)NULL, FENCE_CALLFLAG_BASEFENCE);
	if (unlikely(IS_EMPTY(instance_holder_ptr))) {
		syslog(LOG_DEBUG, "%s: Failed to create Broadcast Fence. Aborting", __func__);
		exit (-1);
	}

	f_ptr = GetInstance(instance_holder_ptr);

	f_ptr->fence_location.display_banner_name	=	strdup("broadcast");
	f_ptr->fence_location.canonical_name			=	strdup("unfacd Broadcast Fence");
	f_ptr->when																=	time(NULL);
	f_ptr->fence_id														=	1;

	broadcast_fence_ptr												=	f_ptr;

	syslog(LOG_DEBUG, "%s: Global Broadcast Fence '%s' successfully created...", __func__, f_ptr->fence_location.display_banner_name );

	return;

}

UFSRVResult  * __attribute__((nonnull(1), access(read_only, 1)))
CacheBackendUpdateFenceKey(Fence *f_ptr, ZKGroupMasterKey *fence_key)
{
  unsigned char *key;
  BufferDescriptor buffer_descriptor = {.size = GROUP_MASTER_KEY_SIZE};

  if (fence_key == NULL) key = FENCE_KEY(f_ptr);
  else key = fence_key->raw.bytes;
  buffer_descriptor.data = (char *)key;

  CacheBackendSetFenceAttributeBinary(FENCE_ID(f_ptr), "fence_key", &buffer_descriptor);

  if (unlikely(THREAD_CONTEXT_UFSRV_RESULT_TYPE_ERR))	{
    syslog(LOG_ERR, "%s {pid:'%lu', th_ctx:'%p', fo:'%p', fid:'%lu'} ERROR: COULD NOT UPDATE FENCE KEY", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, FENCE_ID(f_ptr));
    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, THREAD_CONTEXT_UFSRV_RESULT_CODE_)
  }

  exit_error:
  THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, RECODE_NONE)
}

const unsigned char * __attribute__((const))
ProvideEmptyFenceKey()
{
  static unsigned char EmptyFenceKey[GROUP_MASTER_KEY_SIZE] = {0};
  return EmptyFenceKey;
}

bool __attribute__((const, nonnull(1)))
IsFenceKeyEmpty(const unsigned char *fence_key)
{
  return (memcmp(fence_key, ProvideEmptyFenceKey(), GROUP_MASTER_KEY_SIZE) == 0);
}

static void __attribute__((nonnull(1, 2), access(read_only, 1)))
_GenerateFenceKeyIfNecessary(Fence *f_ptr, unsigned char *key_provided)
{
  if (memcmp(key_provided, ProvideEmptyFenceKey(), GROUP_MASTER_KEY_SIZE) == 0) {
    GenerateZKGroupParams(f_ptr);
    CacheBackendUpdateFenceKey(f_ptr, NULL);
    DbBackendUpdateFenceKey(FENCE_ID(f_ptr), FENCE_KEY(f_ptr));
  } else {
    memcpy(FENCE_KEY(f_ptr), key_provided, GROUP_MASTER_KEY_SIZE);
  }
}

/**
 * Text encode the fence key using Base64
 * @param fence_key Binary representation of key
 * @param buffer_provided Optionally pass a pre-allocated storage buffer for encoded key
 * @dynamic_memory ALLOCATES 'ZKGroupMasterKey *' (only if no preallocated buffer was provided)
 * @return storage buffer containing encoded key
 */
ZKGroupMasterKey *
EncodeFenceKey(const unsigned char *fence_key, ZKGroupMasterKey *buffer_provided)
{
  ZKGroupMasterKey *buffer;
  if (!IS_EMPTY(buffer_provided)) buffer = buffer_provided;
  else buffer = calloc(1, sizeof(ZKGroupMasterKey));

  base64_encode(fence_key, GROUP_MASTER_KEY_SIZE, (unsigned char *)buffer->encoded.bytes);

  return buffer;
}

char *
MakeCanonicalSelfZonedFenceName(unsigned long userid, const char *fence_name, char *cname_out)
{
	char *canonical_name  = NULL;

	if (IS_PRESENT(cname_out)) {
		canonical_name = cname_out;
		sprintf(canonical_name, "%s:%s:%s:%lu:%s", "", "", "", userid, fence_name);
	} else {
		asprintf(&canonical_name, "%s:%s:%s:%lu:%s", "", "", "", userid, fence_name);
	}

	return canonical_name;

}

/**
 * 	@brief Check if  the user-provided baseloc is valid, based on current known location.
 * 	Also, check if the localisation tokens are structured correctly.
 * 	template: 'x:y:z:user_self_zone_private_cloud:'
 * 	variations: 'x::::', ':::user:', etc...
 * 	baseloc prefix should include the full qualified base(i.e. each zone suffixed by ':') less the terminating fence name
 */
bool
IsHomebaseGeoLocValid(Session *sesn_ptr, const char *baseloc)
{
#define Z1_COUNTRY	0
#define Z2_ADMINA		1
#define	Z3_LOCALITY	2
#define Z4_USER			3
	User *u_ptr	=	&(sesn_ptr->ufnet_session.sservice.user);

	if (unlikely(IS_EMPTY(u_ptr) || IS_EMPTY(baseloc)))	return false;

	if (IS_STR_LOADED(u_ptr->user_details.homebase_geoloc_dynamic)) {
		if((strcmp(u_ptr->user_details.homebase_geoloc_dynamic, baseloc)) == 0) return true;
	}

	if (IS_STR_LOADED(u_ptr->user_details.user_preferences.homebase_geoloc)) {
		if((strcmp(u_ptr->user_details.user_preferences.homebase_geoloc, baseloc)) == 0) return true;
	}

	int iterations = 0;
	char *start, *aux;
	char *zones[4] = {0};
	char *baseloc_copy = strdupa(baseloc);
	start = baseloc_copy;

	while (iterations++ < 4) {
		if((aux = strchr(start, ':'))) {
			*aux = '\0'; zones[iterations - 1] = start;
			aux++;
			start = aux;
			continue;
		}

		break;
	}

	//253 3846 129
	if (iterations-1 != 4) {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s {uname:'%s', token_sz:'%d'}: Wrong number of tokens...", __func__, u_ptr->user_details.user_name, iterations-1);
#endif
		return false;
	}

#ifdef __UF_TESTING
	  syslog(LOG_DEBUG, "%s {uname:'%s', country:'%s', admin:'%s', locality:'%s', user:'%s'}: Toknised baseloc...", __func__, u_ptr->user_details.user_name, zones[0]?zones[0]:"", zones[1]?zones[1]:"", zones[2]?zones[2]:"", zones[3]?zones[3]:"");
#endif

	  LocationDescription *ld_ptr = NULL;
	  if (u_ptr->user_details.user_location_initialised) ld_ptr = &u_ptr->user_details.user_location;
	  else
	  if (u_ptr->user_details.user_location_by_server_initialised) ld_ptr = &u_ptr->user_details.user_location_by_server;

	  if (!ld_ptr) {
		  syslog(LOG_DEBUG, "%s {uname:'%s'}: ERROR: COULD NOT NOT FIND VALID LOCATION DESCRIPTION DEFINED FOR USER ", __func__, u_ptr->user_details.user_name);

		  return false;
	  }


	  //don't allow empty country zone, although technically this is correct condition, meaning global scope
	  if((*zones[Z1_COUNTRY] == 0) || (strcmp(zones[Z1_COUNTRY], ld_ptr->country) != 0)) {
		  syslog(LOG_INFO, "%s: INVALID COUNTRY assignment in baseloc: '%s'. LocationDescription contains: '%s'",   __func__, zones[Z1_COUNTRY], ld_ptr->country);

		  return false;
	  }


	  if ((*zones[Z4_USER])) {// && strtoul(zones[Z4_USER], NULL, 10)!=u_ptr->user_details.user_id)
	  	unsigned long selfzoned_uid = strtoul(zones[Z4_USER], NULL, 10);
	  	if (selfzoned_uid != 0 && selfzoned_uid != UfsrvUidGetSequenceId(&(u_ptr->user_details.uid))) {
	  		syslog(LOG_INFO, "%s: INVALID USER assignment in self-zoned baseloc: '%s'", __func__, zones[Z4_USER]);

	  		return false;
	  	}
	  }

  UpdateHomebaseGeoLocDynamicAssignment(sesn_ptr, baseloc, CALL_FLAG_WRITEBACK_SESSION_DATA_TO_BACKEND | CALL_FLAG_BROADCAST_SESSION_EVENT);

	  return true;

}

char *
MakeCanonicalFenceName(Session *sesn_ptr, const char *fdname, unsigned  flag_selfzoned, char *cname_out)
{
	return (_MakeCanonicalFenceName(&(sesn_ptr->ufnet_session.sservice.user.user_details.user_location), SESSION_USERID(sesn_ptr), fdname, flag_selfzoned, cname_out));
}

/**
 * 	@brief: Calculate maximum possible string size to house fully loaded canonical fence name. Location information
 * 	is based on Session. Also, caters for self-zoned possibility. We overshoot slightly.
 */
__attribute__ ((const)) size_t
SizeofCanonicalFenceName(Session *sesn_ptr, const char *fdname)
{
	const LocationDescription *ld_ptr = SESSION_SERVICE(sesn_ptr)->user.user_details.user_location_initialised?
                                      &(SESSION_SERVICE(sesn_ptr)->user.user_details.user_location) :
                                      &(SESSION_SERVICE(sesn_ptr)->user.user_details.user_location_by_server);
  size_t len = 0;

  if (IS_STR_LOADED(ld_ptr->country))     len += strlen(ld_ptr->country);
  if (IS_STR_LOADED(ld_ptr->admin_area))  len += strlen(ld_ptr->admin_area);
  if (IS_STR_LOADED(ld_ptr->locality))    len += strlen(ld_ptr->locality);
  len += sizeof(UINT64_LONGEST_STR) + 4 + 1; //uid + 4 for ':' seperators + for null char

  if (IS_PRESENT(fdname))	len += strlen(fdname);

  return len;
}

/**
 * 	@brief:	Populate the Fence in the context of the Session owner sesn_ptr who created the fence (if location_ptr is NULL),
 * 	eg. it inherits its location attributes.
 * 	@param location_ptr:	Location descriptor touse forthe fence, could be the Session's or some other designation (not implemented well)
 * 	@userfence_canonical_name_in
 *
 * 	@dynamic_memory userfence_canonical_name_in: this is a reference to previously constructed local copy. Must be allocated on the heap.
 *
 */
Fence *
PopulateFenceData(Fence *f_ptr, Session *sesn_ptr, LocationDescription *location_ptr, const char *fence_banner, char *userfence_canonical_name_in, bool flag_fuzz_location)
{
	LocationDescription *ld_ptr = NULL;
	SessionService 		  *ss_ptr = &(sesn_ptr->ufnet_session.sservice);
	static LocationDescription location_descriptor_fallback = {
			.longitude=0.0,
			.latitude=0.0,
			.locality="_locality_",
			.admin_area="_adminarea_",
			.country="_country_",
			.selfzone="0"
	};

	if (IS_PRESENT(location_ptr))	ld_ptr = location_ptr;
	else {
		//use Session's location, or fallback
		 ld_ptr = ss_ptr->user.user_details.user_location_initialised?
					 (&ss_ptr->user.user_details.user_location):(ss_ptr->user.user_details.user_location_by_server_initialised?
							 (&(ss_ptr->user.user_details.user_location_by_server)):(&location_descriptor_fallback));
	}

	 char *userfence_canonical_name = NULL;
	 if (userfence_canonical_name_in) {
		userfence_canonical_name = strdup(userfence_canonical_name_in);//previously dynamically allocated and safe to use
	 } else {
		 //resolved above
		 //ld_ptr=ss_ptr->user.user_details.user_location_initialised?
			// (&ss_ptr->user.user_details.user_location):(&ss_ptr->user.user_details.user_location_by_server);

		 //this is specific to user fences, which limits the utility of this function
		 userfence_canonical_name = _MakeCanonicalFenceName(ld_ptr, SESSION_USERID(sesn_ptr), fence_banner, USER_ATTRIBUTE_IS_SET(sesn_ptr, USERATTRIBUTE_DEFINES_USERZONE), NULL);
	 }

	 //we should not need to strdup if we invoked MakeCanonicalFenceName
	 f_ptr->fence_location.canonical_name				=	userfence_canonical_name;
	 f_ptr->fence_location.display_banner_name	=	strdup(fence_banner);
	 FENCE_OWNER_UID(f_ptr)											=	UfsrvUidGetSequenceId(&(ss_ptr->user.user_details.uid));
	 {
		 int len = strlen(f_ptr->fence_location.canonical_name);//xxx:yyy:zzz
		 int len2= strlen(f_ptr->fence_location.display_banner_name);//zzz
		 int len3= len - len2;//11-3=8

		 f_ptr->fence_location.base_location = malloc(len3+1);//9 extra for null
		 memcpy(f_ptr->fence_location.base_location, f_ptr->fence_location.canonical_name, len3);
		 *(f_ptr->fence_location.base_location + (len3)) = 0;

#ifdef __UF_FULLDEBUG
		 syslog(LOG_DEBUG, "%s: BASELOC IS: '%s'", __func__, f_ptr->fence_location.base_location);
#endif
	 }

	 f_ptr->fence_location.fence_location.latitude	=	ld_ptr->latitude;
	 f_ptr->fence_location.fence_location.longitude	=	ld_ptr->longitude;

	 if (flag_fuzz_location) FuzzGeoLocation(&(FENCE_LOCATION(f_ptr).fence_location), _CONFIGDEFAULT_GEOLOC_FUZZFACTOR);

	 //TODO: populate the rest from ld_ptr

	 return f_ptr;
}
/**
 * @brief Build a canonical fence name whilst respecting current triggered zone state
 * @param cname_out user provided buffer
 * @return fully formed canonical name or NULL if
 */
char *
_MakeCanonicalFenceNameForTriggeredZone(LocationDescription *ld_ptr, unsigned long userid, const char *userfence_banner, unsigned  flag_selfzoned, char *cname_out)
{
  if (!IS_STR_LOADED(ld_ptr->country)) {
    syslog(LOG_ERR, "%s {pid:'%lu'}: WILL NOT create a canonical for NoCountryDefined: '%s'", __func__, pthread_self(), IS_STR_LOADED(userfence_banner)? userfence_banner : CONFIG_DEFAULT_PREFS_STRING_VALUE);
    return NULL;
  }

  char *canonical_name  = NULL;
  if (LOCATION_GEOLOC_TRIGGER(ld_ptr) == GEOLOC_TRIGGER_NEIGHBOURHOOD) {
    if (IS_PRESENT(cname_out)) {
      canonical_name = cname_out;
      sprintf(canonical_name, "%s:%s:%s:%lu:%s",
              ld_ptr->country? ld_ptr->country : "",
              ld_ptr->admin_area? ld_ptr->admin_area : "",
              ld_ptr->locality? ld_ptr->locality : "",
              flag_selfzoned? userid : 0,
              userfence_banner? userfence_banner : "");
    } else {
      asprintf(&canonical_name, "%s:%s:%s:%lu:%s",
               ld_ptr->country? ld_ptr->country : "",
               ld_ptr->admin_area? ld_ptr->admin_area : "",
               ld_ptr->locality? ld_ptr->locality : "",
               flag_selfzoned? userid : 0,
               userfence_banner? userfence_banner : "");
    }
  } else if (LOCATION_GEOLOC_TRIGGER(ld_ptr) == GEOLOC_TRIGGER_REGIONHOOD) {
    if (IS_PRESENT(cname_out)) {
      canonical_name = cname_out;
      sprintf(canonical_name, "%s:%s:%s:%lu:%s",
              ld_ptr->country? ld_ptr->country : "",
              ld_ptr->admin_area? ld_ptr->admin_area : "",
               "",
              flag_selfzoned? userid : 0,
              userfence_banner? userfence_banner : "");
    } else {
      asprintf(&canonical_name, "%s:%s:%s:%lu:%s",
               ld_ptr->country? ld_ptr->country : "",
               ld_ptr->admin_area? ld_ptr->admin_area : "",
                "",
               flag_selfzoned? userid : 0,
               userfence_banner? userfence_banner : "");
    }
  } else if (LOCATION_GEOLOC_TRIGGER(ld_ptr) == GEOLOC_TRIGGER_COUNTRYHOOD) {
    if (IS_PRESENT(cname_out)) {
      canonical_name = cname_out;
      sprintf(canonical_name, "%s:%s:%s:%lu:%s",
              ld_ptr->country? ld_ptr->country : "",
              "",
              "",
              flag_selfzoned? userid : 0,
              userfence_banner? userfence_banner : "");
    } else {
      asprintf(&canonical_name, "%s:%s:%s:%lu:%s",
               ld_ptr->country? ld_ptr->country : "",
               "",
               "",
               flag_selfzoned? userid : 0,
               userfence_banner? userfence_banner : "");
    }
  }

  if (canonical_name) {
#ifdef __UF_FULLDEBUG
    syslog(LOG_DEBUG, "%s {pid:'%lu'}: Produced: '%s'", __func__, pthread_self(), canonical_name);
#endif
  } else syslog(LOG_DEBUG, "%s {pid:'%lu', triggered_zone:'%d'}: COULD NOT PRODUCE CANONICAL NAME: '%s'! Was there a valid triggered zone?", __func__, pthread_self(), LOCATION_GEOLOC_TRIGGER(ld_ptr), IS_STR_LOADED(userfence_banner)? userfence_banner : CONFIG_DEFAULT_PREFS_STRING_VALUE);

  return canonical_name;

}

/**
 * 	@brief:	produces final fully qualified user fence name, incorporating all known location information.
 * 	if userfence_banner is null it only create homebase_geoloc_dynamic value.
 *	This is intended for USER FENCE only, but it dovetails with _f_CheckIntraBaseFencesAndAdd which handles BaseFences.
 *
 * 	@dynamic_memory: EXPORTS char * which the user must free
 */
char *
_MakeCanonicalFenceName(LocationDescription *ld_ptr, unsigned long userid, const char *userfence_banner, unsigned  flag_selfzoned, char *cname_out)
{
	if (!IS_STR_LOADED(ld_ptr->country)) {
		syslog(LOG_ERR, "%s {pid:'%lu'}: WILL NOT create a canonical for NoCountryDefined: '%s'", __func__, pthread_self(), IS_STR_LOADED(userfence_banner)? userfence_banner : CONFIG_DEFAULT_PREFS_STRING_VALUE);
		return NULL;
	}

	char *canonical_name  = NULL;

	if (IS_PRESENT(cname_out)) {
		canonical_name = cname_out;
		sprintf(canonical_name, "%s:%s:%s:%lu:%s",
						ld_ptr->country? ld_ptr->country : "",
						ld_ptr->admin_area? ld_ptr->admin_area : "",
						ld_ptr->locality? ld_ptr->locality : "",
						flag_selfzoned? userid : 0,
						userfence_banner? userfence_banner : "");
	} else {
    asprintf(&canonical_name, "%s:%s:%s:%lu:%s",
                        ld_ptr->country? ld_ptr->country : "",
                        ld_ptr->admin_area? ld_ptr->admin_area : "",
                        ld_ptr->locality? ld_ptr->locality : "",
                        flag_selfzoned? userid : 0,
                        userfence_banner? userfence_banner : "");
	}

	if (canonical_name) {
#ifdef __UF_FULLDEBUG
		syslog(LOG_DEBUG, "%s {pid:'%lu'}: Produced: '%s'", __func__, pthread_self(), canonical_name);
#endif
	} else syslog(LOG_DEBUG, "%s {pid:'%lu'}: ERROR: COULD NOT PRODUCE CANONICAL NAME: '%s'", __func__, pthread_self(), IS_STR_LOADED(userfence_banner)? userfence_banner : CONFIG_DEFAULT_PREFS_STRING_VALUE);

	return canonical_name;

}

void
UpdateLocationIfDifferent(FenceLocationDescription *location_ptr_target, FenceLocationDescription *location_ptr_source)
{
	if (IS_STR_LOADED(location_ptr_target->display_banner_name)) {
		if (!(strcasecmp(location_ptr_target->display_banner_name, location_ptr_source->display_banner_name) == 0)) {
			free (location_ptr_target->display_banner_name);
			location_ptr_target->display_banner_name = strdup(location_ptr_source->display_banner_name);
		}
	}
	else location_ptr_target->display_banner_name = strdup(location_ptr_source->display_banner_name);

	if (IS_STR_LOADED(location_ptr_target->canonical_name)) {
		if (!(strcasecmp(location_ptr_target->canonical_name, location_ptr_source->canonical_name) == 0)) {
			free (location_ptr_target->canonical_name);
			location_ptr_target->canonical_name = strdup(location_ptr_source->canonical_name);
		}
	} else location_ptr_target->canonical_name = strdup (location_ptr_source->canonical_name);

	if (IS_STR_LOADED(location_ptr_target->base_location)) {
		if (!(strcasecmp(location_ptr_target->base_location, location_ptr_source->base_location) == 0)) {
			free (location_ptr_target->base_location);
			location_ptr_target->base_location = strdup(location_ptr_source->base_location);
		}
	} else location_ptr_target->base_location = strdup(location_ptr_source->base_location);

}

LocationDescription *
MapFenceLocationDescription(const Fence *f_ptr, char *canonical_name_buffer, LocationDescription *location_ptr_out)
{
	//country:admin:locality:username-self_zone:dname
	char *walker,
				*orig=canonical_name_buffer;

	if ((walker=strchr(orig, ':')))
	{
		*walker++='\0';
		location_ptr_out->country=orig;
		orig=walker;

		if ((walker=strchr(orig, ':')))
		{
			*walker++='\0';
			location_ptr_out->admin_area=orig;
			orig=walker;

			if ((walker=strchr(orig, ':')))
			{
				*walker++='\0';
				location_ptr_out->locality=orig;
				orig=walker;

				if ((walker=strchr(orig, ':')))
				{
					*walker++='\0';
					location_ptr_out->selfzone=orig;
					orig=walker;

					return location_ptr_out;
				}
			}
		}
	}

	return NULL;
}

// ------------------- REMOVE & DESTRUCT ROUTINES -------------------------------------

/**
 * 	@brief:
 * 	Frontline line function removes individual named fence from Session's fence list
 *
 */
int
FenceRemoveUserByFenceId(Session *sesn_ptr_this, Session *sesn_ptr_target, unsigned long fence_id)
{
#if 0
	//require work to function properly

	if (fence_id<=0)
	{
		syslog(LOG_ERR, "%s (pid:'%lu' cid:'%lu'): INVALID fence_id parameter passed: RETURNING", __func__, pthread_self(), SESSION_ID(sesn_ptr_target));

		return -2;
	}

	//FENCE_CALLFLAG_SEARCH_BACKEND unset. We only remove if user is local to this server
	Fence *f_ptr_hash=FindFenceById(NULL, fence_id, 0);
	if (!f_ptr_hash)
	{
		syslog(LOG_DEBUG, "%s (pid:'%lu' cid='%lu'): ERROR: COULD NOT FIND fence_id: '%lu' in Hash.", __func__, pthread_self(), SESSION_ID(sesn_ptr_target), fence_id);

		return -3;
	}

	return (RemoveUserFromMembersListForFence(sesn_ptr_this, sesn_ptr_target, f_ptr_hash, 0));
#endif

	return 0;
}

/**
 * 	@brief: Clears Session instance only and doesn't touch referencial links related to Fences at system level, as this is
 * 	an isolated instance with its own copy of the relationships.
 * 	This is useful when the session was created as a snapshot and not actual live one.
 */
unsigned
RemoveUserFromAllFencesSessionInstanceOnly(Session *sesn_ptr, unsigned long call_flags)
{
	Fence 					*f_ptr		=	NULL;
	ListEntry 			*eptr			=	NULL;

	//NEVER USE FOR LOOP CONSTRUCT FOR ITERATION
	FenceStateDescriptor	*fence_state_descriptor = NULL;

	while (SESSION_FENCE_LIST_SIZE(sesn_ptr) != 0) {
		eptr = SESSION_FENCE_LIST(sesn_ptr).head;
		if (IS_PRESENT(eptr)) {
      InstanceHolderForFenceStateDescriptor *instance_fstate_ptr = (InstanceHolderForFenceStateDescriptor *)eptr->whatever;
			fence_state_descriptor = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);
			f_ptr = FENCESTATE_FENCE(fence_state_descriptor);
			RemoveFenceFromSessionFenceList (SESSION_FENCE_LIST_PTR(sesn_ptr), f_ptr);
			FenceDecrementReference (FENCESTATE_INSTANCE_HOLDER(fence_state_descriptor), 1);
			//kill both
      RecyclerPut(FencePoolTypeNumber(), (RecyclerClientData *)FENCESTATE_INSTANCE_HOLDER(fence_state_descriptor), (ContextData *)NULL, CALLFLAGS_EMPTY);
			RecyclerPut(FenceStateDescriptorPoolTypeNumber(), instance_fstate_ptr, (ContextData *)NULL, CALLFLAGS_EMPTY);
		}
	}

	while (SESSION_INVITED_FENCE_LIST_SIZE(sesn_ptr) != 0) {
		eptr = SESSION_INVITED_FENCE_LIST(sesn_ptr).head;
		if (IS_PRESENT(eptr)) {
      InstanceHolderForFenceStateDescriptor *instance_fstate_ptr = (InstanceHolderForFenceStateDescriptor *)eptr->whatever;
			fence_state_descriptor = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);
			f_ptr = FENCESTATE_FENCE(fence_state_descriptor);
			RemoveFenceFromSessionInvitedFenceList(SESSION_INVITED_FENCE_LIST_PTR(sesn_ptr), FENCE_ID(f_ptr));
			FenceDecrementReference(FENCESTATE_INSTANCE_HOLDER(fence_state_descriptor), 1);
			//kill both
      RecyclerPut(FencePoolTypeNumber(), (RecyclerClientData *)FENCESTATE_INSTANCE_HOLDER(fence_state_descriptor), (ContextData *)NULL, CALLFLAGS_EMPTY);
			RecyclerPut(FenceStateDescriptorPoolTypeNumber(), (RecyclerClientData *)instance_fstate_ptr, (ContextData *)NULL, CALLFLAGS_EMPTY);
		}
	}

	return 1;//all good

}

/**
 * @brief Remove user from LinkJoining list for fence.
 * @param instance_sesn_ptr
 * @param f_ptr_instance
 * @param fe_ptr_out
 * @param fence_call_flags
 * @return
 */
int
RemoveUserFromLinkJoinedList(InstanceHolderForSession *sesn_ptr_instance, InstanceHolderForSession *f_ptr_instance, unsigned long fence_call_flags)
{
  int		rc			= 0;
  Session *sesn_ptr = SessionOffInstanceHolder(sesn_ptr_instance);
  Fence *f_ptr = FenceOffInstanceHolder(f_ptr_instance);

    if (RemoveThisFromList(FENCE_LINK_JOINING_USER_SESSIONS_LIST_PTR(f_ptr), sesn_ptr_instance)) {
      SessionDecrementReference(sesn_ptr_instance, 1);
      rc++;
#ifdef __UF_FULLDEBUG
      syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fid:'%lu', list_sz:'%u'} Session removed from Fences's LinkJoining list...", __func__, pthread_self(), sesn_ptr, FENCE_ID(f_ptr), FENCE_LINK_JOINING_USER_SESSIONS_LIST_SIZE(f_ptr));
#endif
    }

    return rc;
}

static void
_DestructUserFenceSnapshot(Session *sesn_ptr, Fence *f_ptr, unsigned long call_flags);

/**
 * 	@brief: In snapshot mode, we dont connect users
 */
static void
_DestructUserFenceSnapshot(Session *sesn_ptr, Fence *f_ptr, unsigned long call_flags)
{
//	if (IS_PRESENT(FENCE_CNAME(f_ptr)))	 free (FENCE_CNAME(f_ptr));
//	f_ptr->attrs		=	0;
//	FENCE_ID(f_ptr)	=	0;
	DePopulateFenceData(f_ptr, 0);

}

//#if 1
//
//destructive: all resources associated with _a Fence_ are removed and completely unlinked.
//DO NOT CALL ON A LOOP WITH  ListEntry->next s you'd be shooting yourself in the foot as you cycle through loop iteration
//because the structure of the list will have changed
//assumes USers are in Fence and will loop unlink destruct each
//user service routines to unlink
//
/**
 * 	@brief This is designed to work with the type pool, being invoked from PUT and DESTRUCT callbacks. It will need a
 * 	slight adaptation to operate as a standalone.
 *
 * 	@sesn_ptr: the session owner, who is (most likely) in the Fence's user session list
 * 	@brief: Deep destructor: all resources associated with Fence are removed, especially if SELF_DESTRUCT is SET
 * 	@locks f_ptr: if FENCE_CALLFLAG_LOCK_FENCE is set
 * 	@unlocks f_ptr: unless FENCE_CALLFLAG_KEEP_FENCE_LOCKED is set
 * 	@locked sesn_ptr: must be locked in the calling environment
 */
void
DestructUserFence(Session *sesn_ptr, InstanceContextForFence *instance_ctx_ptr, unsigned long call_flags)
{
	if ((call_flags&FENCE_CALLFLAG_SNAPSHOT_INSTANCE) || (F_ATTR_IS_SET(instance_ctx_ptr->f_ptr->attrs, F_ATTR_SNAPSHOT)))	return (_DestructUserFenceSnapshot(sesn_ptr, instance_ctx_ptr->f_ptr, call_flags));

	bool fence_lock_already_owned = false;

	if (call_flags&FENCE_CALLFLAG_LOCK_FENCE) {
		FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, instance_ctx_ptr->f_ptr, _LOCK_TRY_FLAG_FALSE, SESSION_RESULT_PTR(sesn_ptr), __func__);

		if (SESSION_RESULT_TYPE_EQUAL(sesn_ptr, RESULT_TYPE_ERR)) return;

		fence_lock_already_owned = (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD));
	}

#ifdef __UF_TESTING
	syslog(LOG_DEBUG, "%s: {pid:'%lu', fo:'%p', bid:'%lu', fcname: '%s'} has (user_count='%d') Sessions...", __func__, pthread_self(), instance_ctx_ptr->f_ptr, instance_ctx_ptr->f_ptr->fence_id, instance_ctx_ptr->f_ptr->fence_location.canonical_name, instance_ctx_ptr->f_ptr->fence_user_sessions_list.nEntries);
#endif

	ListEntry *eptr = NULL;

	bool		dangling_session_status	=	false;
	bool    lock_already_owned			= false;
	Session *sesn_ptr_other_user		=	NULL;
	InstanceHolderForSession *instance_sesn_ptr_other_user;

	while (instance_ctx_ptr->f_ptr->fence_user_sessions_list.nEntries != 0) {
		if ((eptr = instance_ctx_ptr->f_ptr->fence_user_sessions_list.head)) {
			sesn_ptr_other_user = SessionOffInstanceHolder(eptr->whatever);

			//since sesn_ptr is already locked, we skip it
			if (NOT_TRUE(SESSION_ID(sesn_ptr) == (SESSION_ID(sesn_ptr_other_user)))) {
				SessionLockRWCtx(THREAD_CONTEXT_PTR, sesn_ptr_other_user, _LOCK_TRY_FLAG_TRUE, __func__);
				if (_RESULT_TYPE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), RESULT_TYPE_ERR)) {
					//lock failed: Session will still be removed from theFence's List but Fence will remain on the user's List of fences, although marked as dangling
					dangling_session_status = true;
				}
			}

			lock_already_owned = (_RESULT_CODE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), RESCODE_PROG_LOCKED_BY_THIS_THREAD));

			RemoveUserFromUserFenceAndUnlinkUser(instance_ctx_ptr->f_ptr, (InstanceHolderForSession *)eptr->whatever, dangling_session_status);

			if (NOT_TRUE(SESSION_ID(sesn_ptr) == (SESSION_ID(sesn_ptr_other_user)))) {
				if (!lock_already_owned)	SessionUnLockCtx (THREAD_CONTEXT_PTR, sesn_ptr_other_user, __func__);
			}

			dangling_session_status = false;
		}
	}//while

	//TODO: do the same for invited list/blocked

	RemoveFromHash(&FenceRegistryIdHashTable, (void *)instance_ctx_ptr->instance_f_ptr);
	RemoveFromHash(&FenceRegistryCanonicalNameHashTable, (void *) instance_ctx_ptr->instance_f_ptr);

	ResetFencePermissions(instance_ctx_ptr->f_ptr);

	DePopulateFenceData(instance_ctx_ptr->f_ptr, call_flags);

	if ((call_flags&FENCE_CALLFLAG_LOCK_FENCE) && !(call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED)) {
		if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, instance_ctx_ptr->f_ptr, SESSION_RESULT_PTR(sesn_ptr));
	}

	//TODO: LOST LOCK OWNERSHIP remove FENCE_CALLFLAG_KEEP_FENCE_LOCKED

	if (unlikely(call_flags&FENCE_CALLFLAG_SELF_DESTRUCT)) {
		pthread_rwlockattr_destroy(&(instance_ctx_ptr->f_ptr->fence_events.rwattr));

		free(instance_ctx_ptr->f_ptr);
	} else {
		//TODO: this cannot be called, because this is function is callback handler for put
		//RecyclerPut(FencePoolTypeNumber(), (RecyclerClientData *)f_ptr, (ContextData *)NULL, 0);
	}

}

/**
 * 	@brief:	Resets some of the data structure associated with a Fence in a way that suitable for reuse in the Recycler. This doesn't clean up the Fence-Session linking
 *
 * 	@locked f_ptr: must be locked in the calling environment
 */
void
DePopulateFenceData(Fence *f_ptr, unsigned long call_flags)
{
	DestructFenceLocationDescription(&(f_ptr->fence_location));

	f_ptr->when					= 0;
	f_ptr->time_to_live	= 0;
	f_ptr->fence_id			= 0;
	f_ptr->attrs				= 0;
	f_ptr->max_users		= 0;
	f_ptr->owner_uid		= 0;

	if (IS_PRESENT(FENCE_AVATAR(f_ptr)))	{free (FENCE_AVATAR(f_ptr)); FENCE_AVATAR(f_ptr) = NULL;}
  if (IS_STR_LOADED(FENCE_DESCRIPTION(f_ptr))) {free(FENCE_DESCRIPTION(f_ptr)); FENCE_DESCRIPTION(f_ptr) = NULL;}

}

void
DestructFenceLocationDescription(FenceLocationDescription *location_ptr)
{
	if (!IS_EMPTY(location_ptr->canonical_name)) 			{free(location_ptr->canonical_name); location_ptr->canonical_name = NULL;}
	if (!IS_EMPTY(location_ptr->display_banner_name))	{free(location_ptr->display_banner_name); location_ptr->display_banner_name = NULL;}
	if (!IS_EMPTY(location_ptr->banner_name))					{free(location_ptr->banner_name); location_ptr->banner_name = NULL;}
	if (!IS_EMPTY(location_ptr->base_location))				{free(location_ptr->base_location); location_ptr->base_location = NULL;}

	DestructLocationDescription(&(location_ptr->fence_location));
}

/**
 * 	@brief: This is here for semantical clarity only.
 */
InstanceHolderForFenceStateDescriptor *
RemoveFenceFromSessionInvitedFenceList(List *sesn_fence_list, unsigned long fid)
{
	return _RemoveFenceFromList(sesn_fence_list, fid);
}

/**
 * 	@brief: Unlinks Fence from User's session list Pure data manipulation.
 */
InstanceHolderForFenceStateDescriptor *
RemoveFenceFromSessionFenceList(List *sesn_fence_list, Fence *f_ptr)
{
	return _RemoveFenceFromList(sesn_fence_list, FENCE_ID(f_ptr));
}

/**
 * 	@brief introspects into the Session's FenceStateDescriptor List to find/remove the underlying Fence. This is necessary because
 * 	 FenceStateDescriptor is not exported and would be double handling to do it through the standard opaque 'RemoveThisFromList'
 */
static inline InstanceHolderForFenceStateDescriptor *
_RemoveFenceFromList(List *sesn_fence_list, unsigned long fid)
{
	int 		found	= 0;
	ListEntry	*lptr	= NULL,
				    *prev	= NULL;
	FenceStateDescriptor *fence_state_descriptor	= NULL;
  InstanceHolderForFenceStateDescriptor *instance_fstate_ptr = NULL;

	lptr = sesn_fence_list->head;

	while ((lptr != NULL) && (!found)) {
    instance_fstate_ptr = (InstanceHolder *)lptr->whatever;
		fence_state_descriptor = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);

		if (FENCE_ID(FENCESTATE_FENCE(fence_state_descriptor)) == fid) {
			found = 1;
		} else {
			prev = lptr;
			lptr = lptr->next;
		}
	}

	if (!found)  return NULL;

	if (prev == NULL) {
		sesn_fence_list->head = lptr->next;
	} else if (lptr->next == NULL) {
		sesn_fence_list->tail = prev;
		sesn_fence_list->tail->next = NULL;
	} else {
		prev->next = lptr->next;
	}

	sesn_fence_list->nEntries--;

	memset(lptr, 0, sizeof(ListEntry));
	free(lptr);

	return instance_fstate_ptr;

}

/**
 * 	@brief: non destructive removal of referenced Fence and Session entities from each other's respective lists
 * 	@param dangling_session: session has not been able to be locked so we only remove it from the fence's list
 * 	@locked: both Fence andSession must be locked by the caller
 * 	@locks: Directly done: but decrementing refrences locks type recycler resources associated with Fence and Session
 */
Fence *
RemoveUserFromUserFenceAndUnlinkUser(Fence *f_ptr, InstanceHolderForSession *instance_sesn_ptr, bool dangling_session)
{
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);
	SessionService *ss_ptr = SESSION_SERVICE(sesn_ptr);

	//TODO: this should be done somewhere else
	//if we are unlinking the user's baseloc fence, that's no longer valid relationship as we are removing the Fence
	if (ss_ptr->user.user_details.base_fence_local_id == FENCE_ID(f_ptr))	ss_ptr->user.user_details.base_fence_local_id = 0;

	if (IS_FALSE(dangling_session)) {
    InstanceHolderForFenceStateDescriptor *instance_fstate_ptr = RemoveFenceFromSessionFenceList(SESSION_FENCE_LIST_PTR(sesn_ptr), f_ptr);
    if (IS_PRESENT(instance_fstate_ptr)) {
      FenceStateDescriptor *fence_state_descriptor = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);
      if (IS_PRESENT(fence_state_descriptor)) {
        FenceDecrementReference(fence_state_descriptor->instance_holder_fence, 1);
        RecyclerPut(FenceStateDescriptorPoolTypeNumber(), instance_fstate_ptr, (ContextData *) NULL, 0);
      }
    } else {
      syslog(LOG_WARNING, "%s (pid:'%lu', cid_removed:'%lu', uid_removed:'%lu' fence_count='%d'): DATA INCONSISTENCY: FENCE WASN'T IN USER'S FENCE LIST (bid='%lu' user_count='%d') >> CHECK IF SESSION IS FENCE'S SESSION LIST.", __func__, pthread_self(), SESSION_ID(sesn_ptr), UfsrvUidGetSequenceId(&(ss_ptr->user.user_details.uid)), SESSION_FENCE_LIST_SIZE(sesn_ptr), FENCE_ID(f_ptr), f_ptr->fence_user_sessions_list.nEntries);
    }
	} else {
		//TODO
		//we can't remove it from the Session because we couldn't lock it so we may atomically mark  it as Fence statedescription as dangling
	}

	//do locking at higher level
	if (RemoveThisFromList(&(f_ptr->fence_user_sessions_list), instance_sesn_ptr)) {
    SessionDecrementReference(instance_sesn_ptr, 1);

#ifdef __UF_TESTING
    syslog(LOG_DEBUG, "%s (pid:'%lu', cid_removed:'%lu', uid_removed:'%lu' fence_count='%d'): SUCCESS: REMOVED  from (bid='%lu' user_count='%d')", __func__, pthread_self(), SESSION_ID(sesn_ptr), UfsrvUidGetSequenceId(&(ss_ptr->user.user_details.uid)), SESSION_FENCE_LIST_SIZE(sesn_ptr), FENCE_ID(f_ptr), f_ptr->fence_user_sessions_list.nEntries);
#endif
  } else {
    syslog(LOG_WARNING, "%s (pid:'%lu', cid_removed:'%lu', uid_removed:'%lu' fence_count='%d'): DATA INCONSISTENCY: SESSION WASN'T IN FENCE'S SESSION LIST (bid='%lu' user_count='%d')", __func__, pthread_self(), SESSION_ID(sesn_ptr), UfsrvUidGetSequenceId(&(ss_ptr->user.user_details.uid)), SESSION_FENCE_LIST_SIZE(sesn_ptr), FENCE_ID(f_ptr), f_ptr->fence_user_sessions_list.nEntries);
  }

	return f_ptr;

}

// ---------- --------------- END OF REMOVE ROUTINES ---------------------

/**
 * @brief: This is a data integrity check for invite lists that is performed after Session's been loaded from cachebackend.
 * @param sesn_ptr
 * @return how many list elements that were affected.
 * @locked sesn_ptr by caller
 * @locks f_ptr
 * @unlocks f_ptr
 */
static size_t
_CrossCheckInvitedListsForUser(InstanceHolderForSession *instance_sesn_ptr)
{
  size_t processed = 0;
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

  if (SESSION_FENCE_LIST_SIZE(sesn_ptr) > 0 && SESSION_INVITED_FENCE_LIST_SIZE(sesn_ptr) > 0) {
    ListEntry *eptr = NULL;
    ListEntry *eptr_invited = NULL;
    FenceStateDescriptor *fence_state_descriptor = NULL;
    FenceStateDescriptor *fence_state_descriptor_invited = NULL;

    FenceStateDescriptor *rouge_fences[SESSION_FENCE_LIST_SIZE(sesn_ptr)];
    memset(rouge_fences, 0, sizeof(rouge_fences));

    size_t idx = 0;
    for (eptr = SESSION_FENCE_LIST(sesn_ptr).head; eptr; eptr = eptr->next) {
      fence_state_descriptor = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *) eptr->whatever);

      for (eptr_invited = SESSION_INVITED_FENCE_LIST(sesn_ptr).head; eptr_invited; eptr_invited = eptr_invited->next) {
        fence_state_descriptor_invited = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *) eptr_invited->whatever);
        if (FENCE_ID(FENCESTATE_FENCE(fence_state_descriptor)) == FENCE_ID(FENCESTATE_FENCE(fence_state_descriptor_invited))) {
          rouge_fences[idx++] = fence_state_descriptor_invited;
        }
      }
    }

    size_t idx_processed = idx;
    bool fence_lock_already_owned;

    idx = 0;
    while (idx++ < idx_processed) {
      FenceEvent fence_event = {.event_type=EVENT_TYPE_FENCE_USER_LIST_CORRECTED};

      FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, FENCESTATE_FENCE(rouge_fences[idx]), _LOCK_TRY_FLAG_FALSE, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), __func__);

      if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_ERR) continue;

      syslog(LOG_DEBUG, "%s {pid:'%lu', th_ctx:'%p, o:'%p', fo:'%p', fid:'%lu'}: ERROR: DATA INTEGRITY: FENCE ON INVITE LIST, BUT USER ALREADY JOINED ", __func__, pthread_self(), THREAD_CONTEXT_PTR, sesn_ptr, rouge_fences[idx], FENCE_ID(FENCESTATE_FENCE(rouge_fences[idx])));

      fence_lock_already_owned = THREAD_CONTEXT_UFSRV_RESULT_CODE_EQUAL(ufsrv_thread_context, RESCODE_PROG_LOCKED_BY_THIS_THREAD);
      RemoveUserFromInvitedList(instance_sesn_ptr, rouge_fences[idx], &fence_event, FENCE_CALLFLAG_WRITEBACK_DATA_TO_BACKEND);
      if (!fence_lock_already_owned)  FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, FENCESTATE_FENCE(rouge_fences[idx]), THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
    }

    return idx_processed;
  }

  return processed;
}


/**
 *	@brief Using the Fences's Sessions List check for the presence of the given session. As opposed to "_f_CrossCheckUserInFenceAndFenceInUser()", which
 *	performs pointer comparison, this function uses the logical session_id value, which is more correct in handling remote sessions.
 *	Another function IsUserMemberOfThisFence() uses the Session's fence's list.
 *
 *	@locked: f_ptr must be locked in caller environment
 *
 *	@blocks_on: NONE
 *
 *	@dynamic_memory: NONE
 */
inline int
CrossCheckSessionInFenceBySessionId(Fence *f_ptr, unsigned long session_id)
{
   ListEntry *eptr = NULL;

   for (eptr=f_ptr->fence_user_sessions_list.head; eptr; eptr=eptr->next) {

	   if (SESSION_ID(SessionOffInstanceHolder((InstanceHolderForSession *)eptr->whatever)) == session_id) {
		   return 1;
	   }
   }

   return 0;

}

/**
 * 	This is generalised implementation of CrossCheckSessionInFenceBySessionId (Fence *f_ptr, unsigned long session_id)
 *	@brief Using the Fences's provided user List check for the presence of the given session. As opposed to "_f_CrossCheckUserInFenceAndFenceInUser()", which
 *	performs pointer comparison, this function uses the logical user_id value, which is more correct in handling remote sessions.
 *	Another function IsUserMemberOfThisFence() uses the Session's fence's list.
 *
 *	@locked: f_ptr must be locked in caller environment
 *
 *	@blocks_on: NONE
 *
 *	@dynamic_memory: NONE
 */
int
CrossCheckSessionInFenceUserListByUserId(Fence *f_ptr, List *fence_user_list_ptr, unsigned long user_id)
{
  ListEntry *eptr = NULL;
  InstanceHolder *instance_holder_ptr = NULL;

  for (eptr=fence_user_list_ptr->head; eptr; eptr=eptr->next) {
    instance_holder_ptr = (InstanceHolder *)eptr->whatever;
    if (SESSION_USERID(((Session *)GetInstance(instance_holder_ptr))) == user_id) {
     return 1;
    }
  }

return 0;

}

/**
 *	@brief Using the Session;s Fences List check for the presence of the given fence. As opposed to "_f_CrossCheckUserInFenceAndFenceInUser()", which
 *	performs pointer comparison, this function uses the logical fence_id value, which is more correct in handling remote sessions.
 *	Another comparable function IsUserMemberOfThisFence().
 *
 *	@locked sesn_ptr: must be locked in caller environment
 *	@locks: None
 *	@blocks_on: NONE
 *
 *	@dynamic_memory: NONE
 */
inline int
CrossCheckFenceInSessionByFenceId(Session *sesn_ptr, unsigned long fence_id)
{
   ListEntry *eptr;
   FenceStateDescriptor *fence_descriptor;

   for (eptr=SESSION_FENCE_LIST(sesn_ptr).head; eptr; eptr=eptr->next) {
	   fence_descriptor = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *)(eptr->whatever));
	   if (FENCESTATE_FENCE(fence_descriptor)->fence_id == fence_id) {
		   return 1;
	   }
   }

   return 0;

}

/**
 * This is a generalisation of CrossCheckFenceInSessionByFenceId (Session *sesn_ptr, unsigned long fence_id)
 *	@brief Using the provided Session's Fences List check for the presence of the given fence. As opposed to "_f_CrossCheckUserInFenceAndFenceInUser()", which
 *	performs pointer comparison, this function uses the logical fence_id value, which is more correct in handling remote sessions.
 *	Another comparable function IsUserMemberOfThisFence().
 *
 *	@locked sesn_ptr: must be locked in caller environment
 *	@locks: None
 *	@blocks_on: NONE
 *
 *	@dynamic_memory: NONE
 */
int
CrossCheckFenceInSessionFenceListByFenceId(Session *sesn_ptr, List *sesn_fence_ist_ptr, unsigned long fence_id)
{
   ListEntry *eptr;
   FenceStateDescriptor *fence_descriptor_ptr;

   for (eptr=sesn_fence_ist_ptr->head; eptr; eptr=eptr->next) {
	   fence_descriptor_ptr = (FenceStateDescriptor *)GetInstance(((InstanceHolder *)eptr->whatever));
	   if (FENCESTATE_FENCE(fence_descriptor_ptr)->fence_id == fence_id) {
		   return 1;
	   }
   }

   return 0;

}

inline InstanceHolderForFenceStateDescriptor *
FindFenceStateInSessionFenceListByFenceId(Session *sesn_ptr, List *sesn_fence_ist_ptr, unsigned long fence_id)
{
   ListEntry *eptr;
   FenceStateDescriptor *fence_descriptor;

   for (eptr=sesn_fence_ist_ptr->head; eptr; eptr=eptr->next) {
     fence_descriptor = FenceStateDescriptorOffInstanceHolder((InstanceHolder *)eptr->whatever);
	   if (FENCESTATE_FENCE(fence_descriptor)->fence_id == fence_id) {
		   return (InstanceHolderForFenceStateDescriptor *)eptr->whatever;
	   }
   }

   return NULL;

}

inline static void _JsonFormatFenceTypeSpecs(const Fence *f_ptr, json_object *jobj);

struct json_object *
JsonFormatFenceForDbBackend(Session *sesn_ptr_carrier, Fence *f_ptr, enum DigestMode digest_mode, unsigned long fence_call_flags)
{
	bool 			fence_lock_already_owned	=	false;
	bool    	is_uid_hashed_locally 		= false;
	char 		  uid_encoded[CONFIG_MAX_UFSRV_ID_ENCODED_SZ+1] = {0};
	UfsrvUid	uid = {0};

	if (fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE) {
		FenceEventsLockRDCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_TRUE, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), __func__);

		if (_RESULT_TYPE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), RESULT_TYPE_ERR))	return NULL;

		fence_lock_already_owned = (_RESULT_CODE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), RESCODE_PROG_LOCKED_BY_THIS_THREAD));
	}

	struct json_object *jobj = json_object_new_object();

	if (IS_STR_LOADED(FENCE_DNAME(f_ptr))) {
    char *fname_safe = h_escape_string(THREAD_CONTEXT_DB_BACKEND, FENCE_DNAME(f_ptr));
    json_object_object_add(jobj, FENCE_JSONATTR_FNAME, json_object_new_string(fname_safe));
    free(fname_safe);
  }

  json_object_object_add(jobj, FENCE_JSONATTR_ID, 			json_object_new_int64(FENCE_ID(f_ptr)));
	json_object_object_add(jobj, FENCE_JSONATTR_EID, 			json_object_new_int64(f_ptr->fence_events.event_id));
	if (FENCE_OWNER_UID(f_ptr) != 1) {
		UfsrvUid *uid_ptr = GetUfsrvUid(FENCE_OWNER_UID(f_ptr), &uid, true, NULL);
		if (IS_PRESENT(uid_ptr)) {
      UfsrvUidConvertSerialise(uid_ptr, uid_encoded);
			json_object_object_add(jobj, "owner_ufsrvid", json_object_new_string(uid_encoded));
		}
	} else {
		json_object_object_add(jobj, "owner_ufsrvuid", json_object_new_string(UFSRV_SYSTEMUSER_UID));
	}
//	todo: enable
//	_JsonFormatFenceTypeSpecs (f_ptr, jobj);

	if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BASEFENCE))	json_object_object_add(jobj,"type", 			json_object_new_int(FENCE_RECORD__FENCE_TYPE__GEO));
	else 																								json_object_object_add(jobj,"type", 			json_object_new_int(FENCE_RECORD__FENCE_TYPE__USER));

	if	(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_PRIVATE))		json_object_object_add(jobj,"privacy_mode", 			json_object_new_int(FENCE_RECORD__PRIVACY_MODE__PRIVATE));
	else	json_object_object_add (jobj,"privacy_mode", 			json_object_new_int(FENCE_RECORD__PRIVACY_MODE__PUBLIC));

	if	(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BROADCAST))		json_object_object_add(jobj,"delivery_mode", 			json_object_new_int(FENCE_RECORD__DELIVERY_MODE__BROADCAST));
	else if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BROADCAST_ONEWAY))	json_object_object_add(jobj,"delivery_mode", 			json_object_new_int(FENCE_RECORD__DELIVERY_MODE__BROADCAST_ONEWAY));
	else json_object_object_add (jobj,"delivery_mode", 			json_object_new_int(FENCE_RECORD__DELIVERY_MODE__MANY)); //defaults to F_ATTR_MANY_TO_MANY

	if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_OPEN))
	{
		if (!F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_KEY))	json_object_object_add(jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN));
		else json_object_object_add (jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN_WITH_KEY));
	}
	else if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_INVITE_ONLY))
	{
		if (!F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_KEY))	json_object_object_add(jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__INVITE));
		else json_object_object_add (jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__INVITE_WITH_KEY));
	}
	else	json_object_object_add (jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN)); //defaults to open

	if (IS_STR_LOADED(FENCE_CNAME(f_ptr))) {
    char *fcname_safe = h_escape_string(THREAD_CONTEXT_DB_BACKEND, FENCE_CNAME(f_ptr));
    json_object_object_add(jobj, "fcname", 		json_object_new_string(fcname_safe));
    free(fcname_safe);
  }

	json_object_object_add (jobj,"msg_expiry", 		json_object_new_int64(FENCE_MSG_EXPIRY(f_ptr)));

	if (digest_mode == DIGESTMODE_BRIEF) goto record_ready;//digest mode

	{
		//add more attributes for full
		if (IS_STR_LOADED(FENCE_BASELOC(f_ptr))) {
      char *baseloc_safe = h_escape_string(THREAD_CONTEXT_DB_BACKEND, FENCE_BASELOC(f_ptr));
      json_object_object_add(jobj, "baseloc", json_object_new_string(baseloc_safe));
      free(baseloc_safe);
    }

    if (IS_STR_LOADED(FENCE_AVATAR(f_ptr)))		json_object_object_add (jobj, FENCE_JSONATTR_AVATAR, 		json_object_new_string(FENCE_AVATAR(f_ptr)));
//		if (IS_STR_LOADED(FENCE_BNAME(f_ptr)))		json_object_object_add (jobj, "bname", 		json_object_new_string(FENCE_BNAME(f_ptr))); //to be phased out
    if (IS_STR_LOADED(FENCE_DESCRIPTION(f_ptr)))		json_object_object_add (jobj, FENCE_JSONATTR_DESCRIPTION, 		json_object_new_string(FENCE_DESCRIPTION(f_ptr)));
		json_object_object_add(jobj,"when_created", 		json_object_new_int64(FENCE_WHEN_CREATED(f_ptr)));
		json_object_object_add(jobj,"when_modified", 		json_object_new_int64(FENCE_WHEN_MODIFIED(f_ptr)));
		json_object_object_add(jobj,"max_members", 		json_object_new_int(FENCE_MAX_MEMBERS(f_ptr)));
		json_object_object_add(jobj,"ttl", 		json_object_new_int64(FENCE_TTL(f_ptr)));
		json_object_object_add(jobj,ACCOUNT_JSONATTR_LONGITUDE, 		json_object_new_double(FENCE_LONGITUDE(f_ptr)));
		json_object_object_add(jobj,ACCOUNT_JSONATTR_LATITUDE, 		json_object_new_double(FENCE_LATITUDE(f_ptr)));
	}

	record_ready:
	if (fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE) {
		if (!fence_lock_already_owned) FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
	}

	return jobj;

}

inline static void
_JsonFormatFenceTypeSpecs(const Fence *f_ptr, json_object *jobj)
{
	if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BASEFENCE))	json_object_object_add(jobj,"type", 			json_object_new_int(FENCE_RECORD__FENCE_TYPE__GEO));
	else 																								json_object_object_add(jobj,"type", 			json_object_new_int(FENCE_RECORD__FENCE_TYPE__USER));

	if	(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_PRIVATE))		json_object_object_add(jobj,"privacy_mode", 			json_object_new_int(FENCE_RECORD__PRIVACY_MODE__PRIVATE));
	else	json_object_object_add (jobj,"privacy_mode", 			json_object_new_int(FENCE_RECORD__PRIVACY_MODE__PUBLIC));

	if	(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BROADCAST))		json_object_object_add(jobj,"delivery_mode", 			json_object_new_int(FENCE_RECORD__DELIVERY_MODE__BROADCAST));
	else if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BROADCAST_ONEWAY))	json_object_object_add(jobj,"delivery_mode", 			json_object_new_int(FENCE_RECORD__DELIVERY_MODE__BROADCAST_ONEWAY));
	else json_object_object_add (jobj,"delivery_mode", 			json_object_new_int(FENCE_RECORD__DELIVERY_MODE__MANY)); //defaults to F_ATTR_MANY_TO_MANY

	if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_OPEN))
	{
		if (!F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_KEY))	json_object_object_add(jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN));
		else json_object_object_add (jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN_WITH_KEY));
	}
	else if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_INVITE_ONLY))
	{
		if (!F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_KEY))	json_object_object_add(jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__INVITE));
		else json_object_object_add (jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__INVITE_WITH_KEY));
	}
	else	json_object_object_add (jobj,"join_mode", json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN)); //defaults to open

}

/**
 *  @param mode: 1 digest,2full
 *
 *  @dynamic_memory: EXPORTS josn_object tree (array) which the user must free
 *  @locks: locks Fences in user's list in iteratios
 *  @unlocks: unlocks fenceslocked within itreation
 *  @locked: Session must be locked by the calling environment
 */
struct json_object *
JsonFormatSessionFenceList(Session *sesn_ptr, enum DigestMode digest_mode)
{
  if (SESSION_FENCE_LIST_SIZE(sesn_ptr) == 0) {
   syslog(LOG_DEBUG, "%s {pid:'%lu' o:'%p', cid:'%lu'}: COULD NOT MAKE FENCE LIST FOR SESSION: No Fences found in Session", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));

   return NULL;
  }

  bool fence_lock_already_owned = false;
  Fence 				*f_ptr	= NULL;
  ListEntry		*eptr		  = NULL;
  json_object	*jarray	  = NULL;
  json_object	*jobj		  = NULL;
  FenceStateDescriptor	*fence_state_descriptor	= NULL;

  char 		  uid_encoded[CONFIG_MAX_UFSRV_ID_ENCODED_SZ + 1] = {0};
  UfsrvUid	uid 																					= {0};
  ZKGroupMasterKey fence_key                              = {0};

   jarray = json_object_new_array();

   for (eptr=SESSION_FENCE_LIST(sesn_ptr).head; eptr; eptr=eptr->next) {
	   fence_state_descriptor = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *)eptr->whatever);
	   f_ptr = FENCESTATE_FENCE(fence_state_descriptor);

	   FenceEventsLockRDCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_TRUE, SESSION_RESULT_PTR(sesn_ptr), __func__);

		 if (SESSION_RESULT_TYPE_EQUAL(sesn_ptr, RESULT_TYPE_ERR))	continue;

		 fence_lock_already_owned = (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD));

		 if (IsFenceKeyEmpty(FENCE_KEY(f_ptr))) {
#ifdef __UF_TESTING
       syslog(LOG_ERR, "%s (pid:'%lu', o:'%p', fo:'%p', fid:'%lu'): ERROR: FENCE_KEY UNALLOCATED...",		__func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr));
#endif
       if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));
       continue;
		 }

		 jobj = json_object_new_object();
		 memset(&fence_key, 0, sizeof(ZKGroupMasterKey));

		 json_object_object_add(jobj,FENCE_JSONATTR_USERCOUNT,	json_object_new_int(FENCE_USERS_COUNT(f_ptr)));
     json_object_object_add(jobj, FENCE_JSONATTR_FENCE_LINKJOIN_LIST_USER_COUNT,	json_object_new_int(FENCE_LINK_JOINING_USER_SESSION_LIST_SIZE(f_ptr))); //user can separately fetch the user list is count > 0
     json_object_object_add(jobj, FENCE_JSONATTR_FENCE_BANNED_LIST_USER_COUNT,	json_object_new_int(FENCE_BANNED_USER_SESSION_LIST_SIZE(f_ptr))); //user can separately fetch the user list is count > 0
		 if (IS_STR_LOADED(FENCE_DNAME(f_ptr)))		json_object_object_add(jobj, FENCE_JSONATTR_FNAME, 		json_object_new_string(FENCE_DNAME(f_ptr)));
     if (IS_STR_LOADED(FENCE_DESCRIPTION(f_ptr)))		json_object_object_add(jobj, FENCE_JSONATTR_DESCRIPTION, 		json_object_new_string(FENCE_DESCRIPTION(f_ptr)));
		 json_object_object_add(jobj, FENCE_JSONATTR_ID, json_object_new_int64(FENCE_ID(f_ptr)));
     EncodeFenceKey(FENCE_KEY(f_ptr), &fence_key);
		 json_object_object_add(jobj, FENCE_JSONATTR_FKEY, json_object_new_string((const char *)fence_key.encoded.bytes));
		 json_object_object_add(jobj, FENCE_JSONATTR_EID, json_object_new_int64(f_ptr->fence_events.event_id));

		if (FENCE_OWNER_UID(f_ptr) != 1) {
			UfsrvUid *uid_ptr = GetUfsrvUid(FENCE_OWNER_UID(f_ptr), &uid, true, NULL);
			if (IS_PRESENT(uid_ptr)) {
        UfsrvUidConvertSerialise(uid_ptr, uid_encoded);
				json_object_object_add(jobj, FENCE_JSONATTR_MSG_EXPIRY, json_object_new_string(uid_encoded));
			}
		} else {
			json_object_object_add(jobj, FENCE_JSONATTR_MSG_EXPIRY, json_object_new_string(UFSRV_SYSTEMUSER_UID));
		}

		json_object_object_add (jobj, FENCE_JSONATTR_MSG_EXPIRY, json_object_new_int64(FENCE_MSG_EXPIRY(f_ptr)));

		if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BASEFENCE))	json_object_object_add(jobj, FENCE_JSONATTR_FTYPE, json_object_new_int(FENCE_RECORD__FENCE_TYPE__GEO));
		else 																								json_object_object_add(jobj, FENCE_JSONATTR_FTYPE, json_object_new_int(FENCE_RECORD__FENCE_TYPE__USER));

		if	(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_PRIVATE))		json_object_object_add(jobj, FENCE_JSONATTR_PRIVACY_MODE, json_object_new_int(FENCE_RECORD__PRIVACY_MODE__PRIVATE));
		else	                                              json_object_object_add(jobj,FENCE_JSONATTR_PRIVACY_MODE, json_object_new_int(FENCE_RECORD__PRIVACY_MODE__PUBLIC));

		if	(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BROADCAST))		          json_object_object_add(jobj, FENCE_JSONATTR_DELIVERY_MODE, json_object_new_int(FENCE_RECORD__DELIVERY_MODE__BROADCAST));
		else if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BROADCAST_ONEWAY))	json_object_object_add(jobj, FENCE_JSONATTR_DELIVERY_MODE, json_object_new_int(FENCE_RECORD__DELIVERY_MODE__BROADCAST_ONEWAY));
		else                                                            json_object_object_add(jobj, FENCE_JSONATTR_DELIVERY_MODE, json_object_new_int(FENCE_RECORD__DELIVERY_MODE__MANY)); //defaults to F_ATTR_MANY_TO_MANY

		if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_OPEN)) {
			if (!F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_KEY))	json_object_object_add(jobj, FENCE_JSONATTR_JOIN_MODE, json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN));
			else                                                    json_object_object_add(jobj, FENCE_JSONATTR_JOIN_MODE, json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN_WITH_KEY));
		} else if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_INVITE_ONLY)) {
			if (!F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_JOINMODE_KEY))	json_object_object_add(jobj, FENCE_JSONATTR_JOIN_MODE, json_object_new_int(FENCE_RECORD__JOIN_MODE__INVITE));
			else                                                    json_object_object_add(jobj, FENCE_JSONATTR_JOIN_MODE, json_object_new_int(FENCE_RECORD__JOIN_MODE__INVITE_WITH_KEY));
		} else	json_object_object_add(jobj, FENCE_JSONATTR_JOIN_MODE, json_object_new_int(FENCE_RECORD__JOIN_MODE__OPEN)); //defaults to open


		if (IS_STR_LOADED(FENCE_CNAME(f_ptr)))		json_object_object_add(jobj, FENCE_JSONATTR_CNAME, 	json_object_new_string(FENCE_CNAME(f_ptr)));

		//todo: disable this, as fence statesync contains protobuf representation of this data
    struct json_object *jobj_array_prefs = JsonFormatFenceUserPreferences(sesn_ptr, fence_state_descriptor);
    if (IS_PRESENT(jobj_array_prefs)) json_object_object_add(jobj, FENCE_JSONATTR_FENCE_PREFS, jobj_array_prefs);

     struct json_object *jobj_array_fence_permissions = JsonFormatFencePermissions(sesn_ptr, f_ptr);
     if (IS_PRESENT(jobj_array_fence_permissions)) json_object_object_add(jobj, FENCE_JSONATTR_FENCE_PERMS, jobj_array_fence_permissions);

		if (digest_mode == DIGESTMODE_BRIEF) goto record_ready;

		{
			//add more attributes for full
		}

		record_ready:
		if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));

		json_object_array_add(jarray, jobj);
	}

   //edge case where we originally had one fence but we could not lock it, so we end up with zero size array
   if (json_object_array_length(jarray) == 0) {
	   json_object_put(jarray);

	   return NULL;
   }

   return jarray;

}

/**
 *  @brief Format list of fences to which this user has been invited to (not list of invited members of a given fence)
 *  @param mode: 1 digest, 2full
 *
 *  @dynamic_memory: EXPORTS josn_object tree (array) which the user must free
 *  @locks: locks Fences in user's list in iteratios
 *  @unlocks: unlocks fenceslocked within itreation
 *  @locked: Session must be locked by the calling environment
 */
struct json_object *
JsonFormatSessionInvitedToFenceList(Session *sesn_ptr, enum DigestMode digest_mode)
{
   if (SESSION_INVITED_FENCE_LIST_SIZE(sesn_ptr) == 0) {
	   return NULL;
   }

   bool									fence_lock_already_owned = false;
   Fence 								*f_ptr									= NULL;
   ListEntry						*eptr										= NULL;
   FenceStateDescriptor	*fstate_ptr             = NULL;
   struct json_object		*jarray									= NULL;
   struct json_object		*jobj										= NULL;
   char invited_by[CONFIG_MAX_UFSRV_ID_ENCODED_SZ+1];
  ZKGroupMasterKey      fence_key               = {0};


   jarray = json_object_new_array();

   for (eptr=SESSION_INVITED_FENCE_LIST(sesn_ptr).head; eptr; eptr=eptr->next) {
     fstate_ptr = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *)eptr->whatever);
	   f_ptr = FenceOffInstanceHolder(FENCESTATE_INSTANCE_HOLDER(fstate_ptr));

	   FenceEventsLockRDCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_TRUE, SESSION_RESULT_PTR(sesn_ptr), __func__);

		 if (SESSION_RESULT_TYPE_EQUAL(sesn_ptr, RESULT_TYPE_ERR))	continue;

		 fence_lock_already_owned = (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD));

		//TODO: We can probably get away without locking if we dont include userCount, fname, fcname
		jobj = json_object_new_object();
		memset(&fence_key, 0, sizeof(ZKGroupMasterKey));
    EncodeFenceKey(FENCE_KEY(f_ptr), &fence_key);
    json_object_object_add(jobj, "fkey", json_object_new_string((const char *)fence_key.encoded.bytes));
		json_object_object_add (jobj,"userCount",			json_object_new_int(FENCE_USERS_COUNT(f_ptr)));
		if (IS_STR_LOADED(FENCE_DNAME(f_ptr)))				json_object_object_add (jobj,"fname", 				json_object_new_string(FENCE_DNAME(f_ptr)));
		json_object_object_add (jobj,"fid", 					json_object_new_int64(FENCE_ID(f_ptr)));
		if (IS_STR_LOADED(FENCE_CNAME(f_ptr)))				json_object_object_add (jobj,"fcname", 				json_object_new_string(FENCE_CNAME(f_ptr)));

		memset (invited_by, '\0', CONFIG_MAX_UFSRV_ID_ENCODED_SZ+1);
		UfsrvUidConvertSerialise(&(fstate_ptr->invited_by), invited_by);

    json_object_object_add(jobj,"invited_by",		json_object_new_string(invited_by));
		json_object_object_add(jobj,"invited_when",	json_object_new_int64(fstate_ptr->when_invited));

		_JsonFormatFenceTypeSpecs (f_ptr, jobj);

		if (digest_mode == DIGESTMODE_BRIEF) goto record_ready;//digest mode

		{
			//add more attributes for full
		}

		record_ready:
		if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));

		json_object_array_add(jarray, jobj);
	}

   //edge case where we originally had one fence but we could not lock it, so we end up with zero size array
   if (json_object_array_length(jarray) == 0) {
	   json_object_put(jarray);

	   return NULL;
   }

   return jarray;

}

/**
 *  @brief Return a list of fences in which user is on the LinkJoin request list ie user linkjoin requested to join fence
 *  @param mode: 1 digest, 2full
 *
 *  @dynamic_memory: EXPORTS josn_object tree (array) which the user must free
 *  @locks: locks Fences in user's list in iteratios
 *  @unlocks: unlocks fenceslocked within itreation
 *  @locked: Session must be locked by the calling environment
 */
struct json_object *
JsonFormatSessionLinkJoinRequestedFenceList(Session *sesn_ptr, enum DigestMode digest_mode)
{
  return NULL;
  /*if (SESSION_INVITED_FENCE_LIST_SIZE(sesn_ptr) == 0) {
    return NULL;
  }

  bool									fence_lock_already_owned = false;
  Fence 								*f_ptr									= NULL;
  ListEntry						*eptr										  = NULL;
  FenceStateDescriptor	*fstate_ptr             = NULL;
  struct json_object		*jarray									= NULL;
  struct json_object		*jobj										= NULL;
  char invited_by[CONFIG_MAX_UFSRV_ID_ENCODED_SZ+1];
  ZKGroupMasterKey      fence_key               = {0};


  jarray = json_object_new_array();

  for (eptr=SESSION_LINK_FENCE_LIST(sesn_ptr).head; eptr; eptr=eptr->next) {
    fstate_ptr = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *)eptr->whatever);
    f_ptr = FenceOffInstanceHolder(FENCESTATE_INSTANCE_HOLDER(fstate_ptr));

    FenceEventsLockRDCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_TRUE, SESSION_RESULT_PTR(sesn_ptr), __func__);

    if (SESSION_RESULT_TYPE_EQUAL(sesn_ptr, RESULT_TYPE_ERR))	continue;

    fence_lock_already_owned = (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD));

    //TODO: We can probably get away without locking if we dont include userCount, fname, fcname
    jobj = json_object_new_object();
    memset(&fence_key, 0, sizeof(ZKGroupMasterKey));
    EncodeFenceKey(FENCE_KEY(f_ptr), &fence_key);
    json_object_object_add(jobj, "fkey", json_object_new_string((const char *)fence_key.encoded.bytes));
    json_object_object_add (jobj,"userCount",			json_object_new_int(FENCE_USERS_COUNT(f_ptr)));
    if (IS_STR_LOADED(FENCE_DNAME(f_ptr)))				json_object_object_add (jobj,"fname", 				json_object_new_string(FENCE_DNAME(f_ptr)));
    json_object_object_add (jobj,"fid", 					json_object_new_int64(FENCE_ID(f_ptr)));
    if (IS_STR_LOADED(FENCE_CNAME(f_ptr)))				json_object_object_add (jobj,"fcname", 				json_object_new_string(FENCE_CNAME(f_ptr)));

    memset (invited_by, '\0', CONFIG_MAX_UFSRV_ID_ENCODED_SZ+1);
    UfsrvUidConvertSerialise(&(fstate_ptr->invited_by), invited_by);

    json_object_object_add(jobj,"invited_by",		json_object_new_string(invited_by));
    json_object_object_add(jobj,"invited_when",	json_object_new_int64(fstate_ptr->when_invited));

    _JsonFormatFenceTypeSpecs (f_ptr, jobj);

    if (digest_mode == DIGESTMODE_BRIEF) goto record_ready;//digest mode

    {
      //add more attributes for full
    }

    record_ready:
    if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));

    json_object_array_add(jarray, jobj);
  }

  //edge case where we originally had one fence but we could not lock it, so we end up with zero size array
  if (json_object_array_length(jarray) == 0) {
    json_object_put(jarray);

    return NULL;
  }

  return jarray;*/

}

/**
 * 	@brief: Generically constructs a UsersList from sessions attached to a given fence.
 * 	@refcount: Increment refcount for Session * if included in the returned collection
 *	@param sesn_ptr: the session context under which request is being made
 * 	@dynamic_memory: EXPORTS dynamically created array which the user must deallocate
 * 	@returns: raw session references
 *
 * 	@locked f_ptr: must be locked by the caller
 */
static inline FenceRawSessionList *
_ConstructRawUsersListForFence(Session *sesn_ptr, Fence *f_ptr, List *fence_user_list_ptr, unsigned long fence_call_flags, FenceRawSessionList *raw_sesn_list_ptr)
{
	//just to gurad against this  being used as a general utility: request has to be associted with a serverd Session
	if (unlikely(IS_EMPTY(sesn_ptr)))						return NULL;
	if (unlikely(IS_EMPTY(raw_sesn_list_ptr)))	return NULL;

	if (fence_user_list_ptr->nEntries == 0) {
		raw_sesn_list_ptr->sessions_sz = 0;
		return raw_sesn_list_ptr;
	}

	raw_sesn_list_ptr->sessions = calloc(fence_user_list_ptr->nEntries, sizeof(InstanceHolderForSession *));

	size_t		i			=	0;
	ListEntry	*eptr	= NULL;

	for (eptr=fence_user_list_ptr->head; eptr; eptr=eptr->next) {
	   InstanceHolderForSession *instance_sesn_ptr_aux = (InstanceHolderForSession *)eptr->whatever;
     Session *sesn_ptr_aux = SessionOffInstanceHolder(instance_sesn_ptr_aux);
	   if (SESNSTATUS_IS_SET(sesn_ptr_aux->ufsrv_session.stat, SESNSTATUS_REMOTE)) {
		   if (!(fence_call_flags&FENCE_CALLFLAG_INCLUDE_REMOTE_SESSIONS)) {
#ifdef __UF_FULLDEBUG
			   syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fo:'%p', fid:'%lu', o_remote:'%p', cid_remote:'%lu'}: FOUND REMOTE SESSION: IGNORING...", __func__, pthread_self(),
		   			sesn_ptr, f_ptr, FENCE_ID(f_ptr), sesn_ptr_aux, SESSION_ID(sesn_ptr_aux), f_ptr);
#endif
			   continue;
		   }

		   //else control flows below to fill attributes
	   }

	   raw_sesn_list_ptr->sessions[i++] = instance_sesn_ptr_aux;
	   SessionIncrementReference (instance_sesn_ptr_aux, 1);
	}

	raw_sesn_list_ptr->sessions_sz = i;

	return raw_sesn_list_ptr;

}


/**
 * @brief Conveniently retrieve a session for fence owner without session locking semantics.
 * @param sesn_ptr
 * @param fence_owner_uid containing identifier for foence owner
 * @return
 */
InstanceHolderForSession *
GetSessionForFenceOwnerNonBlocking(Session *sesn_ptr, unsigned long fence_owner_userid)
{
  bool		lock_already_owned = false;
  unsigned long sesn_call_flags	=	(CALL_FLAG_HASH_SESSION_LOCALLY|CALL_FLAG_HASH_UID_LOCALLY| CALL_FLAG_HASH_USERNAME_LOCALLY|
                                     CALL_FLAG_ATTACH_FENCE_LIST_TO_SESSION|CALL_FLAG_REMOTE_SESSION);
  GetSessionForThisUserByUserId(sesn_ptr,  fence_owner_userid, NULL, sesn_call_flags);

  if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_EQUALS(THREAD_CONTEXT, RESULT_TYPE_SUCCESS))  return (InstanceHolderForSession *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;

  return NULL;
}

/**
 *
 * @param sesn_ptr caller session
 * @param fence_owner_userid
 * @param lock_state return a state indicator if lock was already owned by caller
 * @return
 */
InstanceHolderForSession *
GetSessionForFenceOwner(Session *sesn_ptr, unsigned long fence_owner_userid, bool *lock_state)
{
  bool		lock_already_owned = false;
  unsigned long sesn_call_flags	=	(CALL_FLAG_LOCK_SESSION| CALL_FLAG_LOCK_SESSION_BLOCKING|
                                   CALL_FLAG_HASH_SESSION_LOCALLY|CALL_FLAG_HASH_UID_LOCALLY| CALL_FLAG_HASH_USERNAME_LOCALLY|
                                   CALL_FLAG_ATTACH_FENCE_LIST_TO_SESSION|CALL_FLAG_REMOTE_SESSION);
  GetSessionForThisUserByUserId(sesn_ptr, fence_owner_userid, lock_state, sesn_call_flags);

  if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_EQUALS(THREAD_CONTEXT, RESULT_TYPE_SUCCESS))  return (InstanceHolderForSession *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;

  return NULL;
}

/**
 * 	@locked f_ptr: must be locked by the caller, or here with the FENCE_CALLFLAG_LOCK_FENCE callflag
 *
 * 	@call_fag FENCE_CALLFLAG_INCLUDE_REMOTE_SESSIONS:
 * 	@call_flag FENCE_CALLFLAG_LOCK_FENCE:
 * 	@call_flag FENCE_CALLFLAG_KEEP_FENCE_LOCKED: Only considered if FENCE_CALLFLAG_LOCK_FENCE was flagged as well
 *
 *	@refcount: Increments refcount for Session *
 *
 * 	@return NULL: on error. Zero members is not an error list is still returned
 * 	@return FenceRawSessionList *: containing zero or more elements...
 *
 * 	@worker: sesnworker
 */
FenceRawSessionList *
GetRawMemberUsersListForFence(Session *sesn_ptr, InstanceHolderForFence *instance_f_ptr, unsigned long fence_call_flags, FenceRawSessionList *raw_sesn_list_ptr_in)
{
  Fence *f_ptr = FenceOffInstanceHolder(instance_f_ptr);

	//TODO: change FenceRawList to CollectionDescriptor type
	if (fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE) {
		FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_FALSE, SESSION_RESULT_PTR(sesn_ptr), __func__);

		if (SESSION_RESULT_TYPE_EQUAL(sesn_ptr, RESULT_TYPE_ERR))	return NULL;
	}

	bool fence_lock_already_owned = (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD));

	FenceRawSessionList *raw_sesn_list_ptr = NULL;

	if (IS_EMPTY(raw_sesn_list_ptr_in))	raw_sesn_list_ptr = calloc(1, sizeof(FenceRawSessionList));
	else																raw_sesn_list_ptr = raw_sesn_list_ptr_in;

	if (unlikely(FENCE_USERS_COUNT(f_ptr) == 0))	{raw_sesn_list_ptr->sessions = NULL;	raw_sesn_list_ptr->sessions_sz = 0; goto exit_unlock;}

	//won't load from cache backend unless F_ATTR_DIRTY || F_ATTR_SESSNLIST_LAZY set
  InstateMembersListCacheRecordForFence(sesn_ptr, instance_f_ptr, UNSPECIFIED_UID, UNSPECIFIED_FENCE_LISTTYPE,
                                        UNSPECIFIED_FENCE_LISTTYPE, FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE);
	if (SESSION_RESULT_TYPE_EQUAL(sesn_ptr, RESULT_TYPE_ERR)) {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fo:'%p', bid:'%lu', fence_members_sz:'%d'}: ERROR: COULD NOT LOAD FENCE'S USERS LIST", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), FENCE_USERS_COUNT(f_ptr));
#endif

		raw_sesn_list_ptr->sessions = NULL;
		raw_sesn_list_ptr->sessions_sz = 0;

		goto exit_error;
	}

	if (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_BACKEND_DATA_EMPTYSET)) {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fo:'%p', bid:'%lu', fence_members_sz:'%d'}: NOTICE: COULD NOT LOAD FENCE'S USERS LIST: ZERO MEMBERS", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), FENCE_USERS_COUNT(f_ptr));
#endif

		raw_sesn_list_ptr->sessions = NULL;
		raw_sesn_list_ptr->sessions_sz = 0;

		goto exit_unlock;
	}

	_ConstructRawUsersListForFence(sesn_ptr, f_ptr, &(f_ptr->fence_user_sessions_list), fence_call_flags, raw_sesn_list_ptr);

	exit_unlock:
	if (fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE && !fence_lock_already_owned) {
		FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));
	}

#ifdef __UF_TESTING
	syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fo:'%p', fid:'%lu', members_sz:'%lu'}: Finished Session list construction for Fence", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), raw_sesn_list_ptr->sessions_sz);
#endif

	return raw_sesn_list_ptr;

	exit_error:
	if (IS_EMPTY(raw_sesn_list_ptr_in))	free(raw_sesn_list_ptr);
	if (fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE && !fence_lock_already_owned) {
		FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));
	}

	return NULL;

}

/**
 * 	@locked f_ptr: must be locked by the caller, or here with the FENCE_CALLFLAG_LOCK_FENCE callflag
 *
 * 	@call_fag FENCE_CALLFLAG_INCLUDE_REMOTE_SESSIONS:
 * 	@call_flag FENCE_CALLFLAG_LOCK_FENCE:
 * 	@call_flag FENCE_CALLFLAG_KEEP_FENCE_LOCKED: Only considered if FENCE_CALLFLAG_LOCK_FENCE was flagged as well
 *
 *	@refcount: Increments refcount for Session *
 *
 * 	@return NULL: on error. Zero members is not an error list is still returned
 * 	@return FenceRawSessionList *: containing zero or more elements...
 *
 * 	@worker: sesnworker
 */
FenceRawSessionList *
GetRawInvitedUsersListForFence(Session *sesn_ptr, InstanceContextForFence *instance_fence_ctx, unsigned long fence_call_flags, FenceRawSessionList *raw_sesn_list_ptr_in)
{
	//TODO: change FenceRawList to CollectionDescriptor type
	Fence *f_ptr = instance_fence_ctx->f_ptr;

	if (fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE) {
		FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_FALSE, SESSION_RESULT_PTR(sesn_ptr), __func__);

		if (SESSION_RESULT_TYPE_EQUAL(sesn_ptr, RESULT_TYPE_ERR))	return NULL;
	}

  instance_fence_ctx->is_locked          = true;
  instance_fence_ctx->lock_already_owned = (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD));

	FenceRawSessionList *raw_sesn_list_ptr = NULL;

	if (IS_EMPTY(raw_sesn_list_ptr_in))	raw_sesn_list_ptr = calloc(1, sizeof(FenceRawSessionList));
	else																raw_sesn_list_ptr = raw_sesn_list_ptr_in;

	if (unlikely(FENCE_USERS_COUNT(f_ptr) == 0))	goto exit_unlock;

	//won't load from cache backend unless F_ATTR_DIRTY || F_ATTR_SESSNLIST_LAZY set
  InstateInvitedMembersListCacheRecordForFence(sesn_ptr, instance_fence_ctx->instance_f_ptr, 0,
                                               UNSPECIFIED_FENCE_LISTTYPE, UNSPECIFIED_FENCE_LISTTYPE,
                                               FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE);
	if (SESSION_RESULT_TYPE_ERROR(sesn_ptr)) {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fo:'%p', bid:'%lu', members_sz:'%d'}: ERROR: COULD NOT LOAD FENCE'S SESSION LISTFENCE", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), FENCE_USERS_COUNT(f_ptr));
#endif

		goto exit_error;
	}

	if (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_BACKEND_DATA_EMPTYSET)) {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fo:'%p', bid:'%lu', members_sz:'%d'}: NOTICE: COULD NOT LOAD FENCE'S SESSION LISTFENCE: ZERO MEMBERS", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), FENCE_USERS_COUNT(f_ptr));
#endif

		return raw_sesn_list_ptr;
	}

	//increments refcount for Session. You must destruct this with DestructFenceRawSessionList()
	_ConstructRawUsersListForFence(sesn_ptr, f_ptr, &(f_ptr->fence_user_sessions_invited_list), fence_call_flags, raw_sesn_list_ptr);

	exit_unlock:
	if ((fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE) && !(fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED)) {
		if (!instance_fence_ctx->lock_already_owned)	{
      FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));
      instance_fence_ctx->is_locked = false;
    }
	}

#ifdef __UF_TETING
	syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fo:'%p', fid:'%lu', members_sz:'%lu'}: Finished Session list construction for Fence", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), raw_sesn_list_ptr->sessions_sz);
#endif

	return raw_sesn_list_ptr;

	exit_error:
	if (IS_EMPTY(raw_sesn_list_ptr_in))	free(raw_sesn_list_ptr);
	if ((fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE) && !(fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED)) {
		if (!instance_fence_ctx->lock_already_owned)	{
      FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));
      instance_fence_ctx->is_locked = false;
    }
	}
	return NULL;

}

void
DestructFenceRawSessionList(FenceRawSessionList *raw_sesn_list_ptr, bool self_destruct)
{
	if (unlikely(IS_EMPTY(raw_sesn_list_ptr)))	return;

	if (!IS_EMPTY(raw_sesn_list_ptr->sessions)) {
		if (raw_sesn_list_ptr->sessions_sz>0) {
			size_t i=0;
			for (; i<raw_sesn_list_ptr->sessions_sz; i++) {
				SessionDecrementReference(raw_sesn_list_ptr->sessions[i], 1);
			}
		}

		free (raw_sesn_list_ptr->sessions);
	}

	if (self_destruct)	free (raw_sesn_list_ptr);
}

// START START CACHE RECORD AND USERLIST FOR FENCE \\\\\

static inline void _CacheRecordPopulateFence(Fence *f_ptr, redisReply *redis_ptr);

/**
 * 	@brief: Pupulate fence's fields from raw cache record
 */
static inline void
_CacheRecordPopulateFence(Fence *f_ptr, redisReply *redis_ptr)
{
	f_ptr->fence_id																	=	strtoul(redis_ptr->element[REDIS_KEY_FENCE_ID]->str, NULL, 10);
  f_ptr->attrs																		=	strtoul(redis_ptr->element[REDIS_KEY_FENCE_TYPE]->str, NULL, 10);
	f_ptr->when																			=	strtoul(redis_ptr->element[REDIS_KEY_FENCE_WHEN_CREATED]->str, NULL, 10);
	FENCE_OWNER_UID(f_ptr)													=	strtoul(redis_ptr->element[REDIS_KEY_FENCE_USERID]->str, NULL, 10);
	f_ptr->fence_location.base_location							=	strdup(redis_ptr->element[REDIS_KEY_FENCE_BASELOC]->str);
	f_ptr->fence_location.canonical_name						=	strdup(redis_ptr->element[REDIS_KEY_FENCE_CNAME]->str);
	f_ptr->fence_location.display_banner_name				=	strdup(redis_ptr->element[REDIS_KEY_FENCE_DNAME]->str);
	f_ptr->fence_location.banner_name								=	strdup(redis_ptr->element[REDIS_KEY_FENCE_BNAME]->str);
	f_ptr->fence_location.fence_location.longitude	=	strtof(redis_ptr->element[REDIS_KEY_FENCE_LONG]->str, NULL);
	f_ptr->fence_location.fence_location.latitude		=	strtof(redis_ptr->element[REDIS_KEY_FENCE_LAT]->str, NULL);
	f_ptr->max_users																=	strtoul(redis_ptr->element[REDIS_KEY_MAXUSERS]->str, NULL, 10);
	f_ptr->time_to_live															=	strtoul(redis_ptr->element[REDIS_KEY_TTL]->str, NULL, 10);
	f_ptr->fence_events.event_id								    =	strtoul(redis_ptr->element[REDIS_KEY_EVENT_COUNTER]->str, NULL, 10);

	if (redis_ptr->element[REDIS_KEY_FENCE_AVATAR]->str && *redis_ptr->element[REDIS_KEY_EVENT_COUNTER]->str != '*')
		FENCE_AVATAR(f_ptr)														=	strdup(redis_ptr->element[REDIS_KEY_FENCE_AVATAR]->str);

	if (redis_ptr->element[REDIS_KEY_FENCE_MSGEXPIRY]->str)	FENCE_MSG_EXPIRY(f_ptr) = strtoul(redis_ptr->element[REDIS_KEY_FENCE_MSGEXPIRY]->str, NULL, 10);

	if (redis_ptr->element[REDIS_KEY_LIST_SEMANTICS]->str)
    MapPermissionsListSemanticsFromBackendPersistence(f_ptr, (int) strtoul(redis_ptr->element[REDIS_KEY_LIST_SEMANTICS]->str, NULL, 10));

	//todo this is a temporary treatment to retrospectively populate geo fences with keys
	_GenerateFenceKeyIfNecessary(f_ptr, (unsigned char *)redis_ptr->element[REDIS_KEY_FENCE_KEY]->str);
//	memcpy(FENCE_KEY(f_ptr), redis_ptr->element[REDIS_KEY_FENCE_KEY]->str, GROUP_MASTER_KEY_SIZE); //todo to be re-enabled in place of above when ready
}

/**
 * @brief Fetch the full fence record as stored in the Cache Backend.
 * @dynamic_memory redisReply *: EXPORTS
 */
UFSRVResult *CacheBackendLoadRecordForFence(unsigned long fence_id)
{
	int rescode	=	RESCODE_BACKEND_DATA;
	PersistenceBackend 	*pers_ptr;
	redisReply					*redis_ptr;

	pers_ptr = THREAD_CONTEXT_FENCE_CACHEBACKEND;
	if (IS_EMPTY((redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_FENCE_RECORD_GET_ALL, fence_id))))	{rescode = RESCODE_BACKEND_CONNECTION;	goto return_connection_error;}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_reply_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_nil_error;
	if (!(redis_ptr->element[0]->str))			  goto return_empty_set; //HMGET command will still return empty array

	 return_success:
//	 _RETURN_RESULT_SESN(sesn_ptr, redis_ptr, RESULT_TYPE_SUCCESS, rescode);
	 THREAD_CONTEXT_RETURN_RESULT_SUCCESS(redis_ptr, rescode);

	 return_empty_set:
	 rescode = RESCODE_BACKEND_DATA_EMPTYSET;
	 syslog(LOG_DEBUG, "%s {pid:'%lu', th_ctx:'%p', fid;'%lu'}: ERROR: EMPTY SET.",  __func__, pthread_self(), THREAD_CONTEXT_PTR, fence_id);
	 goto return_error;

	 return_nil_error:
	 syslog(LOG_DEBUG, "%s {pid:'%lu', th_ctx:'%p', fid;'%lu'}: ERROR: NIL SET",  __func__, pthread_self(), THREAD_CONTEXT_PTR, fence_id);
	 goto return_error;

	 return_reply_error:
	 rescode = RESCODE_BACKEND_COMMAND;
	 syslog(LOG_DEBUG, "%s {pid:'%lu', th_ctx:'%p', fid:'%lu', error:'%s'}: ERROR: COMMAND ERROR.", __func__, pthread_self(), THREAD_CONTEXT_PTR, fence_id, redis_ptr->str);
	 goto return_error;

	 return_error:
	 freeReplyObject(redis_ptr);
//	 _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)
  THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, rescode);

	 return_connection_error:
	 syslog(LOG_DEBUG, "%s {pid:'%lu', th_ctx:'%p'}: ERROR RESPONSE for fence_id '%lu'", __func__, pthread_self(), THREAD_CONTEXT_PTR, fence_id);
//	 _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)
  THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, rescode);
}

/**
 *	@brief Load and build a Fence object from its cache backend record.
 *
 *	This function can be called solely to build a fence, or as a side-effect of building a user's session, which includes references to fences of which
 *	the user is a member. The @param call_flags can vary accordingly.
 *	There is an extra check which is currently complicating the logic: whether the @param uid is seen in the member's list for given fence's list, potentially
 *	signalling a referencial integrity problem, as the user should be present in every fence that is on the session's list of fences.
 *
 *	@param sesn_ptr_this can be NULL if no lock is installed on Fence
 *	@param uid user id to look up when loading fence lists, typically the requesting session. Only when FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER is passed.
 *	empty value set to '0'.
 *  @returns InstanceHolderForFence
 *	@call_flag FENCE_CALLFLAG_HASH_FENCE_LOCALLY:
 *	@call_flag FENCE_CALLFLAG_KEEP_FENCE_LOCKED: only honored if HASH_LOCALLY is set
 *	@call_flag FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE:
 *	@call_flag FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER: stop loading the fence record if user in the provided session was not on members list.
 *	Currently this test only works if the fence had zero members in it. We don't checked for named users in fences list.
 *
 *	@locks f_ptr:
 *	Fence RW events. If FENCE_CALLFLAG_KEEP_FENCE_LOCKED  we let the calling environment to unlock the fence.
 *	@unlocks f_ptr: if hash local and FENCE_CALLFLAG_KEEP_FENCE_LOCKED is NOT set
 *
 *	@worker: sesn_worker
 *
 */
UFSRVResult *//CALL_FLAG_HASH_SESSION_LOCALLY
InstateCacheRecordForFence(Session *sesn_ptr_this, EnumFenceCollectionType list_type_context, unsigned long fence_id, unsigned long uid, bool *fence_lock_state, unsigned long call_flags)
{
	Fence 							    *f_ptr = NULL;
	InstanceHolderForFence  *instance_f_ptr = NULL;

  CacheBackendLoadRecordForFence(fence_id);
	if (!THREAD_CONTEXT_UFSRV_RESULT_IS_SUCCESS_WITH_BACKEND_DATA)	goto return_error_null;
	redisReply 	*redis_ptr	=	(redisReply *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;

	unsigned fence_attrs = strtoul(redis_ptr->element[REDIS_KEY_FENCE_TYPE]->str, NULL, 10);
	if (F_ATTR_IS_SET(fence_attrs, F_ATTR_BASEFENCE)) {
    instance_f_ptr = RecyclerGet(FencePoolTypeNumber(), (ContextData *)NULL, FENCE_CALLFLAG_BASEFENCE);/*won't generate id*/
	} else if (F_ATTR_IS_SET(fence_attrs, F_ATTR_USERFENCE)) {
    instance_f_ptr = RecyclerGet(FencePoolTypeNumber(), (ContextData *)NULL, FENCE_CALLFLAG_USERFENCE);/*won't generate id*/
	}

	if (unlikely(IS_EMPTY(instance_f_ptr)))	goto return_error;

	f_ptr = FenceOffInstanceHolder(instance_f_ptr);

	//Do it early as there is no point in loading the fence for the user if user is not on its list

  bool fence_lock_already_owned = false;
  DbOpDescriptor db_op_descriptor = {.ctx_data=instance_f_ptr};

	if (call_flags&FENCE_CALLFLAG_HASH_FENCE_LOCALLY) {
		FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_FALSE, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), __func__);
		if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_ERR)	goto return_error;

		fence_lock_already_owned = (THREAD_CONTEXT_UFSRV_RESULT_CODE_EQUAL(THREAD_CONTEXT, RESCODE_PROG_LOCKED_BY_THIS_THREAD));

		_CacheRecordPopulateFence(f_ptr, redis_ptr);

		bool  claim_inserted   = false;
		bool  adopted_resident = false;
		void *claim_resident   = NULL;

		if (!AddToHashUniquely(&FenceRegistryCanonicalNameHashTable, instance_f_ptr, &claim_inserted, &claim_resident) || !claim_inserted) {
			if (IS_EMPTY(claim_resident))	goto return_error_unlock;

			/* already filed under this name: nothing to claim, nothing to release */
			if (claim_resident == (void *)instance_f_ptr)	goto canonical_settled;

			/* another instance owns the name: this record was never stored, so release it and continue
			 * against the instance that does own it, as the caller's request is still valid */
			syslog(LOG_INFO, "%s {pid:'%lu', o:'%p', fid_new:'%lu'}: NOTICE: FENCE NAME HELD BY INSTANCE:'%p': ADOPTING IT", __func__, pthread_self(), instance_f_ptr, FENCE_ID(f_ptr), claim_resident);

			if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
			FenceReturnToRecycler(instance_f_ptr, NULL, CALLFLAGS_EMPTY);

			instance_f_ptr = (InstanceHolderForFence *)claim_resident;
			f_ptr          = FenceOffInstanceHolder(instance_f_ptr);

			FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_FALSE, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), __func__);
			if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_ERR) {
				freeReplyObject(redis_ptr);

				goto return_error_null;
			}

			fence_lock_already_owned = (THREAD_CONTEXT_UFSRV_RESULT_CODE_EQUAL(THREAD_CONTEXT, RESCODE_PROG_LOCKED_BY_THIS_THREAD));

			/* the resident could be re-keyed or recycled between the refusal and this lock */
			if (FENCE_ID(f_ptr) != fence_id || HashLookup(&FenceRegistryCanonicalNameHashTable, (void *)FENCE_CNAME(f_ptr), true) != instance_f_ptr) {
				syslog(LOG_ERR, "%s {pid:'%lu', o:'%p', fid:'%lu'}: ERROR: ADOPTED INSTANCE NO LONGER HOLDS THE CANONICAL NAME", __func__, pthread_self(), instance_f_ptr, FENCE_ID(f_ptr));

				if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
				freeReplyObject(redis_ptr);

				goto return_error_null;
			}

			adopted_resident = true;
		}

	canonical_settled: ;
		if (!adopted_resident) {
			if (!AddToHashUniquely(&FenceRegistryIdHashTable, instance_f_ptr, &claim_inserted, &claim_resident) || !claim_inserted) {
				if (claim_resident != (void *)instance_f_ptr) {
					RemoveFromHash(&FenceRegistryCanonicalNameHashTable, (void *) instance_f_ptr);

					goto return_error_unlock;
				}
			}
		}
	} else {
		_CacheRecordPopulateFence(f_ptr, redis_ptr);
	}

  DbBackendLoadFenceDescription(instance_f_ptr);

	 if (call_flags&FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE) {
		 F_ATTR_SET(f_ptr->attrs, F_ATTR_DIRTY);

		 if (call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) {
       InstateMembersListCacheRecordForFence(sesn_ptr_this, instance_f_ptr, uid, MEMBER_FENCES, list_type_context, FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE | FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER);
		 } else {
       InstateMembersListCacheRecordForFence(sesn_ptr_this, instance_f_ptr, UNSPECIFIED_UID, MEMBER_FENCES, list_type_context, FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE);
		 }

     if (SESSION_RESULT_TYPE_ERROR(sesn_ptr_this) ||
         (SESSION_RESULT_TYPE_SUCCESS(sesn_ptr_this) && SESSION_RESULT_CODE_EQUAL(sesn_ptr_this, RESCODE_BACKEND_DATA_EMPTYSET))) {
       if ((call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) && (list_type_context == MEMBER_FENCES))	{
         goto return_error_unhashed;
       }
     }

		 unsigned long fence_call_flags_invited = FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE;

		 F_ATTR_SET(f_ptr->attrs, F_ATTR_DIRTY);
		 if (call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) {
			 fence_call_flags_invited |= FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER;
       InstateInvitedMembersListCacheRecordForFence(sesn_ptr_this, instance_f_ptr, uid, INVITED_FENCES, list_type_context, fence_call_flags_invited);
		 } else {
       InstateInvitedMembersListCacheRecordForFence(sesn_ptr_this, instance_f_ptr, 0, INVITED_FENCES, list_type_context, fence_call_flags_invited);
		 }

		 if (SESSION_RESULT_TYPE_ERROR(sesn_ptr_this) ||
				 (SESSION_RESULT_TYPE_SUCCESS(sesn_ptr_this) && SESSION_RESULT_CODE_EQUAL(sesn_ptr_this, RESCODE_BACKEND_DATA_EMPTYSET))) {
			 if ((call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) && (list_type_context == INVITED_FENCES)) {
         goto return_error_unhashed;
			 }
		 }

     F_ATTR_SET(f_ptr->attrs, F_ATTR_DIRTY);
     InstateLinkJoiningMembersListCacheRecordForFence(sesn_ptr_this, instance_f_ptr, UNSPECIFIED_UID, call_flags);

     //todo 7/5/24 devops: implement banned list
//     F_ATTR_SET(f_ptr->attrs, F_ATTR_DIRTY);
//     GetLinkBannedMembersListCacheRecordForFence(sesn_ptr_this,  instance_f_ptr, 0, LINKJOINED_FENCES);

		 //more lists.....

		 F_ATTR_UNSET(f_ptr->attrs, F_ATTR_DIRTY);
	 }
	 else	F_ATTR_SET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY); //load list on demand

	 freeReplyObject(redis_ptr);

	 if (call_flags&FENCE_CALLFLAG_HASH_FENCE_LOCALLY) {
		 if (!(call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED)) {//not transferring lock ownership
			 if (!fence_lock_already_owned) {
         FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
       }

			 if (IS_PRESENT(fence_lock_state))	*fence_lock_state = fence_lock_already_owned;
			 _RETURN_RESULT_SESN(sesn_ptr_this, instance_f_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)
		 }

		 if (IS_PRESENT(fence_lock_state))	*fence_lock_state = fence_lock_already_owned;
		 _RETURN_RESULT_SESN(sesn_ptr_this, instance_f_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)
	 } else {
		 //lock never obtained
		 _RETURN_RESULT_SESN(sesn_ptr_this, instance_f_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)
	 }

	return_error:
	if (IS_PRESENT(f_ptr)) FenceReturnToRecycler(instance_f_ptr, NULL, CALLFLAGS_EMPTY);
	freeReplyObject(redis_ptr);
	goto return_error_null;

	return_error_unlock:
	if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
	FenceReturnToRecycler(instance_f_ptr, NULL, CALLFLAGS_EMPTY);
	freeReplyObject(redis_ptr);
  goto return_error_null;

  return_error_unhashed:
  freeReplyObject(redis_ptr);
  if (call_flags&FENCE_CALLFLAG_HASH_FENCE_LOCALLY) {
    FenceReturnToRecycler(instance_f_ptr,
                          (ContextData *)&((TypePoolContextDataFence){.is_fence_locked=fence_lock_already_owned, .sesn_ptr=sesn_ptr_this, .fence_data.instance_f_ptr=instance_f_ptr}),
                          FENCE_CALLFLAG_UNLOCK_FENCE);
  } else {
    //locking only happens if FENCE_CALLFLAG_HASH_FENCE_LOCALLY was passed
    FenceReturnToRecycler(instance_f_ptr, NULL, CALLFLAGS_EMPTY);
  }

	return_error_null:
	_RETURN_RESULT_SESN(sesn_ptr_this, NULL, RESULT_TYPE_ERR, RESCODE_PROG_NULL_POINTER)

}

UFSRVResult *
CacheBackendGetFencesListSizeForUser(unsigned long userid)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_SESSION_CACHEBACKEND;
	redisReply 					*redis_ptr	=	NULL;

	if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_USER_FENCE_LIST_SIZE, userid)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_INTEGER) {
		size_t fences_list_sz = (size_t)redis_ptr->integer; //shouldn't have problems with negative as we don't store them in this context
		freeReplyObject(redis_ptr);

		if (fences_list_sz < CONFIG_MAX_FENCELIST_SZ) {
      THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *)(uintptr_t)fences_list_sz, rescode)

    } else {
			syslog(LOG_DEBUG, "%s {pid:'%lu, userid:'%lu', fenceslist_sz:'%lu'}: ERROR: FENCE LIST SIZE EXCEEED MAX...", __func__, pthread_self(), userid, fences_list_sz);
      THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_INCONSISTENT_STATE)
		}
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, userid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), userid);
    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)
	}
	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, userid:'%lu'}: ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), userid, redis_ptr->str);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}
	if (redis_ptr->type == REDIS_REPLY_NIL) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, userid:'%lu'}: ERROR: NIL SET",  __func__, pthread_self(), userid);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}

	return_error:
	freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode)

}

/**
 * 	@brief: Get the number of fences for which user is invited
 */
UFSRVResult *
CacheBackendGetInvitedToFencesListSizeForUser(Session *sesn_ptr, unsigned long uid)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_SESSION_CACHEBACKEND;
	redisReply 					*redis_ptr	=	NULL;

	if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr, pers_ptr, REDIS_CMD_INVITED_FENCES_FOR_USER_LIST_SIZE, uid)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_INTEGER) {
		size_t fences_list_sz = (size_t)redis_ptr->integer; //shouldn't have problems with negative as we don't store them in this context
		freeReplyObject(redis_ptr);

		if (fences_list_sz < CONFIG_MAX_FENCELIST_SZ) {
      _RETURN_RESULT_SESN(sesn_ptr, (void *) (uintptr_t) fences_list_sz, RESULT_TYPE_SUCCESS, rescode)
    } else {
			syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu', _fenceslist_sz:'%lu'}: ERROR: FENCE LIST SIZE EXCEEED MAX...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), fences_list_sz);
			_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, RESCODE_PROG_INCONSISTENT_STATE)
		}
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));
	 _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)
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

/**
 * 	@brief: return the number of invitees for a fence using backend cache source.
 */
UFSRVResult *
CacheBackendGetFenceMembersListSize(unsigned long fid, EnumFenceCollectionType list_type)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_SESSION_CACHEBACKEND;
	redisReply 					*redis_ptr	=	NULL;

  const char *prebuilt_command_template = NULL;

  if 			(list_type == MEMBER_FENCES)        prebuilt_command_template  =  REDIS_CMD_MEMBER_USERS_FOR_FENCE_LIST_SIZE;
  else if (list_type == INVITED_FENCES)       prebuilt_command_template  =  REDIS_CMD_INVITED_USERS_FOR_FENCE_LIST_SIZE;
  else {THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_MISSING_PARAM)}


  if (!(redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, prebuilt_command_template, fid)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_INTEGER) {
		size_t fences_list_sz = (size_t)redis_ptr->integer; //shouldn't have problems with negative as we dont store them in this context
		freeReplyObject(redis_ptr);

    THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *) (uintptr_t) fences_list_sz, rescode);
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, fid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), fid);
    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode);
	}
	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, fid:'%lu'}: ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), fid, redis_ptr->str);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}
	if (redis_ptr->type == REDIS_REPLY_NIL) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, fid:'%lu'}: ERROR: NIL SET",  __func__, pthread_self(), fid);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}

	return_error:
	freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, rescode);

}

static char *_BuildAttributesValuesString(CollectionDescriptor *collection_attributes, CollectionDescriptor *collection_values, CollectionDescriptorPair *collection_argv_argvlen);

/**
 * 	@dynamic_memory char *: allocate a one chunk  memory pool to carve out the individual argv elements. User must free.
 */
static char *
_BuildAttributesValuesString(CollectionDescriptor *collection_attributes, CollectionDescriptor *collection_values, CollectionDescriptorPair *collection_argv_argvlen)
{
#define REDIS_COMMAND_OFFSET	2 //the first two slots are for redis command and its length
#define PAIR_FACTOR						2	//well pair means '2'
	if (collection_attributes->collection_sz != collection_values->collection_sz)	return NULL;

	size_t total_sz = 0;
	size_t len_attributes[collection_attributes->collection_sz],
					len_values[collection_values->collection_sz];

	//process the elements that we know of already: attribute names and their values
	for (size_t i=0; i<collection_attributes->collection_sz; i++) {
		len_attributes[i]	=	strlen((char *)collection_attributes->collection[i]); total_sz += len_attributes[i];
		len_values[i]			=	strlen((char *)collection_values->collection[i]);	total_sz += len_values[i];
	}

	total_sz += (collection_values->collection_sz + 1); //for nulls


	char 		*built_str	=	calloc(total_sz, sizeof(char));
	char		*walker			=	built_str;

	for (size_t i=1; i<=collection_attributes->collection_sz; i++) {//we skipped the [0] & [1] which holds the actual comand/arg + len
		collection_argv_argvlen->first.collection[(i * PAIR_FACTOR)]			=	(collection_t *)(const char *)collection_attributes->collection[i-1];
		collection_argv_argvlen->second.collection[(i * PAIR_FACTOR)]			=	(collection_t *)(const size_t)len_attributes[i-1];

		collection_argv_argvlen->first.collection[(i * PAIR_FACTOR) + 1]	=	(collection_t *)(const char *)collection_values->collection[i-1];
		collection_argv_argvlen->second.collection[(i * PAIR_FACTOR) + 1]	=	(collection_t *)(const size_t)len_values[i-1];
	}

//
//	char 		*built_str	=	calloc(total_sz, sizeof(char));
//	char		*walker			=	built_str;
//
//	for (size_t i=(0+REDIS_COMMAND_OFFSET); i<collection_attributes->collection_sz; i++)
//	{
//		collection_combined->collection[i]			=	(collection_t *)walker;
//		collection_combined_len->collection[i]	=	(collection_t *)len_attributes[i]+len_values[i]+1;//+1 for space
//
//		sprintf (walker, "%s %s", (char *)collection_attributes->collection[i], (char *)collection_values->collection[i]);
//		walker+=(len_attributes[i]+len_values[i]+2);//+2 advance beyon the null
//	}

	return built_str;
}

UFSRVResult *
CacheBackendSetFenceAttributesByCollection(Session *sesn_ptr, unsigned long fence_id, CollectionDescriptor *collection_attributes, CollectionDescriptor *collection_values, CollectionDescriptorPair *collection_argv_argvlen)// *collection_combined, CollectionDescriptor *collection_combined_len)
{
#define REDIS_COMMAND_IDX 		0
#define REDIS_COMMAND_ARG_IDX 1

	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	char								redis_cmdarg[sizeof(UINT64_LONGEST_STR)+sizeof("BID:")+1];
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_FENCE_CACHEBACKEND;//persistance_backend;
	redisReply 					*redis_ptr	=	NULL;

	char *built_str=_BuildAttributesValuesString(collection_attributes, collection_values, collection_argv_argvlen);

	collection_argv_argvlen->first.collection[REDIS_COMMAND_IDX]		=	(collection_t *)(const char *)"HMSET";
	collection_argv_argvlen->second.collection[REDIS_COMMAND_IDX]		=	(collection_t *)(const size_t)5;

	snprintf(redis_cmdarg, (sizeof(UINT64_LONGEST_STR)+sizeof("BID:")+1), "BID:%lu", fence_id);
	collection_argv_argvlen->first.collection[REDIS_COMMAND_ARG_IDX]		=	(collection_t *)(const char *)redis_cmdarg;
	collection_argv_argvlen->second.collection[REDIS_COMMAND_ARG_IDX]		=	(collection_t *)(const size_t)strlen(redis_cmdarg);

	if (!(redis_ptr = RedisSendCommandWithCollection((UfsrvSession *)sesn_ptr, pers_ptr, collection_argv_argvlen)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_STATUS && (strcasecmp(redis_ptr->str, "OK") == 0)) {
		return_success:
		free (built_str);
		freeReplyObject(redis_ptr);
		_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_SUCCESS, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	free (built_str);
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));
	 _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)
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
	_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode);

}

/**
 * 	@brief: Set a single value for  fence
 */
UFSRVResult *
CacheBackendSetFenceAttribute(Session *sesn_ptr, unsigned long fence_id, const char *attribute_name, const char *attribute_value)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_FENCE_CACHEBACKEND;
	redisReply 					*redis_ptr	=	NULL;

	if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr, THREAD_CONTEXT_FENCE_CACHEBACKEND, REDIS_CMD_FENCE_RECORD_SET_ATTRIBUTE, fence_id, attribute_name, attribute_value)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_STATUS && (strcasecmp(redis_ptr->str, "OK") == 0)) {
		return_success:
		freeReplyObject(redis_ptr);
		_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_SUCCESS, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));
	 _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)
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

UFSRVResult * __attribute__((nonnull(2, 3), access(read_only, 2)))
CacheBackendSetFenceAttributeBinary(unsigned long fence_id, const char *attribute_name, BufferDescriptor *buffer_attribute_value)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_FENCE_CACHEBACKEND;
	redisReply 					*redis_ptr	=	NULL;

	if (IS_EMPTY((redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, REDIS_CMD_FENCE_RECORD_SET_ATTRIBUTE_BINARY, fence_id, attribute_name, buffer_attribute_value->data, buffer_attribute_value->size))))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_STATUS && (strcasecmp(redis_ptr->str, "OK") == 0)) {
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

/**
 * 	@brief: Get a single value for  fence
 * 	@dynamic_memory redis_ptr: EXPORTS
 */
UFSRVResult *
CacheBackendLoadFenceAttribute(Session *sesn_ptr, unsigned long fence_id, const char *attribute_name)
{
	unsigned 						rescode			=	RESCODE_BACKEND_DATA;
	PersistenceBackend	*pers_ptr		=	THREAD_CONTEXT_FENCE_CACHEBACKEND;//persistance_backend;
	redisReply 					*redis_ptr	=	NULL;

	if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr, THREAD_CONTEXT_FENCE_CACHEBACKEND, REDIS_CMD_FENCE_RECORD_GET_ATTRIBUTE, fence_id, attribute_name)))	goto return_redis_error;

	if (redis_ptr->type == REDIS_REPLY_ARRAY && (redis_ptr->elements > 0)) {
		_RETURN_RESULT_SESN(sesn_ptr, redis_ptr, RESULT_TYPE_ERR, rescode);
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;

	return_redis_error:
	if (IS_EMPTY(redis_ptr)) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));
	 _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), redis_ptr->str);
	 rescode = RESCODE_BACKEND_DATA; goto return_error;
	}
	if (redis_ptr->type == REDIS_REPLY_NIL) {
	 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', cid:'%lu'}: ERROR: NIL SET",  __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));
	 rescode=RESCODE_BACKEND_DATA; goto return_error;
	}

	return_error:
	freeReplyObject(redis_ptr);
	_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)

}

/**
 * 	@brief Retrieve raw CacheBackend userid records for a given fence's members.
 * 	@dynamic_memroy: EXPORTS redis_ptr
 */
UFSRVResult *
CacheBackendLoadUserIdsForFence(unsigned long fid)
{
	PersistenceBackend	*pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);

	redisReply *redis_ptr;

  if (!((redis_ptr = (*pers_ptr->send_command)(NO_SESSION, pers_ptr, REDIS_CMD_FENCE_USERS_LIST_GET, fid)))) {
     syslog(LOG_DEBUG, "%s (pid:'%lu) ERROR RESPONSE for bid:'%lu': ZRANGE MEMBER_USERS_FOR_FENCE:%lu", __func__, pthread_self(), fid, fid);

     THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_CONNECTION)
  }

   if (redis_ptr->type == REDIS_REPLY_ERROR) {
     syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: REDIS RESULT SET for RESPONSE for fid:'%lu'. Error: '%s'",  __func__, pthread_self(), fid, redis_ptr->str);

     goto return_error;
   }
   if (redis_ptr->type == REDIS_REPLY_NIL) {
     syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: EMPTY SET FOR fid:'%lu'",  __func__, pthread_self(), fid);
     goto return_error;
   }

   if (redis_ptr->elements > 0) {
     THREAD_CONTEXT_RETURN_RESULT_SUCCESS(redis_ptr, RESCODE_BACKEND_DATA)
   } else {
     freeReplyObject(redis_ptr);
     THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, RESCODE_BACKEND_DATA_EMPTYSET)
   }

	 return_error:
	 freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)

}

/**
 *	@brief Scan the returned raw userids for the matching session owner
 */
bool
IsUserIdInCacheRecordForFence(Session *sesn_ptr,  unsigned long fid)
{
  CacheBackendLoadUserIdsForFence(fid);
	if (THREAD_CONTEXT_UFSRV_RESULT_IS_SUCCESS_WITH_BACKEND_DATA) {
		bool				return_value = false;
		redisReply *redis_ptr = (redisReply *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;

		for (size_t i=0; i < redis_ptr->elements; ++i) {
			unsigned long 	user_id		= strtoul(redis_ptr->element[i]->str, NULL, 10);
			if (user_id == SESSION_USERID(sesn_ptr)) {
				return_value = true;
				break;
			}
		}

		freeReplyObject(redis_ptr);
		return return_value;
	 }

	return false;
}

/**
 * 	@brief: Resolve link-back integrity issues for Fence/Session memberships. It is assumed at least one part of the relationship is
 * 	established. The caller must request sync from connected clients if necessary.
 * 	@locked sesn_ptr:
 * 	@locked f_ptr:
 */
UFSRVResult *
RepairFenceMembershipForUser(InstanceHolderForSession *instance_sesn_ptr, InstanceHolderForFence *instance_f_ptr, EnumImpairedFenceMembershipType impairment_type)
{
	bool is_not_impaired;
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);
  Fence *f_ptr = FenceOffInstanceHolder(instance_f_ptr);

	if (impairment_type == ImpairedFenceMembershipFence) {
		//fence's memory store did not contain reference session: refer back to CacheBackend for Fence and retrieve list of session ids for its members
		is_not_impaired = IsUserIdInCacheRecordForFence (sesn_ptr,  FENCE_ID(f_ptr));
		if (is_not_impaired) {
			AddThisToList (FENCE_SESSIONS_LIST_PTR(f_ptr), CLIENT_CTX_DATA(instance_sesn_ptr));
			SessionIncrementReference (instance_sesn_ptr, 1);

			_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_FENCE_SESSION_INTEGRITY)
		}
	} else if (impairment_type == ImpairedFenceMembershipSession) {
		unsigned long uid_inviter = 0;

		//session's memory store did not contain reference to fence: refer back to CacheBack end for Session and retrieve list of fence ids this user is memer of
		is_not_impaired = IsFenceIdInCacheRecordForUser(SESSION_USERID(sesn_ptr), FENCE_ID(f_ptr), &uid_inviter);
		if (is_not_impaired) {
			InstanceHolder *instance_holder_ptr = RecyclerGet(FenceStateDescriptorPoolTypeNumber(), (ContextData *)instance_f_ptr, CALLFLAGS_EMPTY);
			if (unlikely(IS_EMPTY(instance_holder_ptr))) goto return_allocation_error;

      FenceStateDescriptor *fence_state_ptr = GetInstance(instance_holder_ptr);
      GetUfsrvUid(uid_inviter, &(fence_state_ptr->invited_by), false, NULL);

			AddThisToList(SESSION_FENCE_LIST_PTR(sesn_ptr), instance_holder_ptr);
			FenceIncrementReference(instance_holder_ptr, 1);

			_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_SESSION_FENCE_INTEGRITY)

		}
	} else goto return_error;

	return_allocation_error:
	syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu'}: ERROR: COULD NOT ALLOCATE FenceStateDescriptr type", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr));
	_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, RESCODE_FENCE_LIST_SELFLOADED);

	return_error:
	syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu', type:'%i'}: ERROR: COULD NOT determine impairment type", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr), impairment_type);
	_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, RESCODE_PROG_INCONSISTENT_STATE);

}

/**
 *      @brief: This is very specific clean up function used to resolve referencial user-fence integrity issues as seen from the
 *      Fences side; ie: the Fence's members List contained a reference to a user who did not reference that fence on its Fence List.
 *      @param sesn_ptr Current session under processing
 *      @param collection_ptr: collection of userids
 *
 *      @param raw_records_collection: The collection may contain less items than indicated by resultset_sz. Always check for payload.
 *
 *      @returns: how many users who did not reference the fence
 *      @locked f_ptr:
 *      @locked sesn_ptr:
 *
 */
static size_t
_CleanUpFaultyUserEntriesForFence(Session *sesn_ptr, Fence *f_ptr, unsigned long userid_loaded_for, CollectionDescriptor *collection_ptr,  EnumFenceCollectionType list_type)
{
	size_t 	removed_users_sz = 0;

	//todo fixme ufnetcorelib migration: function disabled due to hard dependency (Intra/Inter broadcast in ufsrv application)
#if 0
	if (collection_ptr->collection_sz > 0) {
		if (list_type == MEMBER_FENCES) {
			for (size_t i=0; i<collection_ptr->collection_sz; i++) {
				redisReply *redis_ptr_user = (redisReply *)collection_ptr->collection[i];
				unsigned long uid = UfsrvUidGetSequenceId((UfsrvUid *)redis_ptr_user->element[REDIS_KEY_USER_UID]->str);

				if (!IsFenceIdInCacheRecordForUser(uid, FENCE_ID(f_ptr), NULL)) {
					//user doesn't have fence in its List of fences: so we remove this user from the Fences Members list
					redisReply 					*redis_ptr;
					PersistenceBackend 	*pers_ptr						= THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);

					if (unlikely(IS_EMPTY(UpdateBackendFenceData(sesn_ptr, f_ptr, (void *)uid, EVENT_TYPE_FENCE_MEMBERSHIP_REPAIRED, &((FenceEvent){})))))
										continue;

						//TODO: inter broadcast invalidate event (should happen in UpdateBackendFenceData())

						syslog (LOG_NOTICE, "%s (pid:'%lu', th_ctx:'%p', uid:'%lu, fid:'%lu'): FOUND USER THAT DID NOT REFERENCE FENCE IN ITS LIST: DELETEING...", __func__, pthread_self(), THREAD_CONTEXT_PTR, uid, FENCE_ID(f_ptr));

						Session *sesn_ptr_removed;
						InstanceHolderForSession *instance_sesn_ptr_removed;
						bool 		dangling_session 					= false;
						bool 		lock_already_owned 				= false;

						//update memory store for both direction, although it is likely the user doesn't have the fence on its list
						if (uid == userid_loaded_for) {// && SESSION_USERID(sesn_ptr_carrier) == uid) {
							//already locked. Unlikely scenario???
							InstanceHolderForSession *instance_sesn_ptr_this_user = LocallyLocateSessionByUserId(uid);
							RemoveUserFromUserFenceAndUnlinkUser(f_ptr, instance_sesn_ptr_this_user, false);
						} else {
							instance_sesn_ptr_removed = LocallyLocateSessionByUserId(uid);
							if (IS_PRESENT(instance_sesn_ptr_removed)) {
							  sesn_ptr_removed = SessionOffInstanceHolder(instance_sesn_ptr_removed);

								SessionLockRWCtx(THREAD_CONTEXT_PTR, sesn_ptr_removed, _LOCK_TRY_FLAG_FALSE, __func__);
								if (_RESULT_TYPE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), RESULT_TYPE_SUCCESS)) {
									if (_RESULT_CODE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), RESCODE_PROG_LOCKED_BY_THIS_THREAD)) lock_already_owned = true;
								} else	{
									dangling_session = true;//lock failed
								}

								RemoveUserFromUserFenceAndUnlinkUser(f_ptr, instance_sesn_ptr_removed, dangling_session);

								if (!dangling_session && !lock_already_owned)	SessionUnLockCtx(THREAD_CONTEXT_PTR, sesn_ptr_removed, __func__);
								dangling_session = false;
								lock_already_owned = false;
							} else {
								//this is tolerable, as we may not have session for that user locally
								syslog(LOG_NOTICE, "%s (pid:'%lu', th_ctx:'%p' uid:'%lu, fid:'%lu'): FOUND USER THAT DID NOT REFERENCE FENCE IN ITS LIST: BUT NO LOCAL SESSION FOR THIS FOUND", __func__, pthread_self(), THREAD_CONTEXT_PTR, uid, FENCE_ID(f_ptr));
							}
						}

						//remove record from received collection so called doesn;t process for this user, even though we may not have been able to successfull update memory store
						removed_users_sz++;
						collection_ptr->collection[i] = NULL;
						freeReplyObject(redis_ptr_user);
				}
			}
		} else if (list_type == INVITED_FENCES) {
			//TODO: implement _CleanUpFaultyUserEntriesForFence for other fence membership types
			syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p', fid:'%lu'): WARNING: NOT IMPLEMENTED...", __func__, pthread_self(), THREAD_CONTEXT_PTR, FENCE_ID(f_ptr));
		}

	}//end if
#endif

	return removed_users_sz;

}

/**
 * 	@brief: retrieve a given Fence's users member list and build/initialise it into Fence's Session list
 * 	FenceGetUserListFromBackend -> LOOP( _RetrieveSessionRecordFromRawBackend)
 * 									 -> FenceAttachRawSessionsList
 *
 * 	@WARNING: Fence MAY NOT BE POPULATED DONT ASSUME ANYTHING ABOUT FENEC DATA EXCEPT ID AND WHETHET DIRT FLAG IS SET
 * 	TODO: refactor so the session creation happens here
 *
 *	@param userid_loaded_for: where fence is being loaded for a specific user and if flag FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER is present: abort loading if fence user list doess dnot contain user
 *	@param list_type_context: context information indicating which fence list type the provided fence was loaded from, for example this fence
 *	could have been originally referenced in users invited tyle fence list. This helps with ABORTING callflag
 * 	@call_flag CALL_FLAG_ATTACH_USER_LIST_TO_FENCE:
 *
 * 	@locks: none
 *
 * 	@locked f_ptr: must be locked in the calling environment
 * 	@locked sesn_ptr_target: must be locked in the calling environment
 *
 * 	@dynamic_memory redis_ptr: Redis reply is created and destroyed
 * 	@dynamic_memory sesn_ptr_list: redisReply **sesn_ptr_list array of redis replys created and destroyed

 *	@call_flag: FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE
 *	@call_flag FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER: if present, the userid_loaded_for param can used to decided whether to contiue with loading fence
 *
 * 	@worker_thread: session worker, ufsrv worker
 */
UFSRVResult *
InstateMembersListCacheRecordForFence(Session *sesn_ptr_target, InstanceHolderForFence *instance_f_ptr, unsigned long userid_loaded_for, EnumFenceCollectionType list_type_target, EnumFenceCollectionType list_type_context, unsigned long fence_call_flags)
{
  Fence *f_ptr = FenceOffInstanceHolder(instance_f_ptr);

	//we need at least one of these to be set to proceed...
	if (!(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_DIRTY)) && !(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY))) {
#ifdef __UF_TESTING
		 syslog(LOG_DEBUG, "%s (pid:'%lu, th_ctx:'%p', fo:'%p', fid:'%lu') NOT FETCHING SESSION LIST FOR FENCE (lazy:'%d', dirty:'%d')", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, FENCE_ID(f_ptr), F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY), F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_DIRTY));
#endif

		_RETURN_RESULT_SESN(sesn_ptr_target, f_ptr, RESULT_TYPE_SUCCESS, RESCODE_FENCE_LIST_SELFLOADED)
	}

	redisReply 	*redis_ptr;
  CacheBackendLoadUserIdsForFence(FENCE_ID(f_ptr));//TODO: this backend loading is heavy handed

	if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_SUCCESS) {
		if (THREAD_CONTEXT_UFSRV_RESULT_CODE_EQUAL(THREAD_CONTEXT, RESCODE_BACKEND_DATA_EMPTYSET))	_RETURN_RESULT_SESN(sesn_ptr_target, NULL, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA_EMPTYSET)

		redis_ptr = (redisReply 	*)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
	} else	_RETURN_RESULT_SESN(sesn_ptr_target, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_DATA)

	{
	 //>> we now have a list of UID's

#ifdef __UF_TESTING
	   syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p', fo:'%p', fid:'%lu'}: RESULT: Fence contains (%lu) users in it", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, FENCE_ID(f_ptr), redis_ptr->elements);
#endif

	   bool 					userid_found						=	false;
	   size_t					userids_preprocessed_sz	=	redis_ptr->elements;
	   unsigned long 	userids_preprocessed[userids_preprocessed_sz]; //TODO: warning: stackoverflow

	   memset (userids_preprocessed, 0, sizeof(userids_preprocessed));

		 for (size_t i=0; i < userids_preprocessed_sz; ++i) {
			 userids_preprocessed[i] = strtoul(redis_ptr->element[i]->str, NULL, 10);
			 if (fence_call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) if (userids_preprocessed[i] == userid_loaded_for)	userid_found = true;
		 }

		 //where user was'nt found we want to continue process if the fence for which we are processing the user list was being loaded into
		 //the missing user; ie there is a referential problem: if I was loading the invited fences list for user and this fence was referenced
		 //that list we should see the user referenced here
		 if (!userid_found && (fence_call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) && (list_type_target == list_type_context)) {
			 syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p', fo:'%p', fid:'%lu', userid_loadedfor:'%lu'}: ERROR: REFERENTIAL ERROR: FENCE MEMBERS LIST DOESNNT REFER TO USER BEING LOADED FOR", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, FENCE_ID(f_ptr), userid_loaded_for);
			 freeReplyObject(redis_ptr);

			 _RETURN_RESULT_SESN(sesn_ptr_target, NULL, RESULT_TYPE_ERR, RESCODE_FENCE_MEMBERSHIP)
		 }

	   size_t				i, success_counter = 0;
	   redisReply		*redis_ptr_session_records[userids_preprocessed_sz];

	   for (i=0; i < userids_preprocessed_sz; ++i)	redis_ptr_session_records[i] = NULL;

	   //retrieve the raw user cache record for each uid and index it into list
	   for (i=0; i < userids_preprocessed_sz; ++i) {
		   unsigned long 	user_id		=  	userids_preprocessed[i];
       CacheBackendGetRawSessionRecordByUserId(user_id, CALLFLAGS_EMPTY, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));

		   if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_SUCCESS) {
			   redisReply *redis_ptr_user = (redisReply *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
			   *(redis_ptr_session_records + success_counter++) = redis_ptr_user;
		   } else {
			   //TODO: this a stale UID which must be cleansed. First check in the DbBackend if the user has uninstalled. perhaps log it in a list for out of band processing?
			   syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p', fo:'%p', fid:'%lu', uid:'%lu'): ERROR: FOUND POTENTIALLY STALE UID --> TODO: IMPELMENT CLEANSING", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, FENCE_ID(f_ptr), user_id);

			   //TODO: implement fully.. currently an anchor point and must check the db backend
			   ClearBackendCacheForSessionlessInvalidUserId_TO_BE_PORTED_FROM_UFSRVWEBSOCK(user_id, CALLFLAGS_EMPTY, CALLFLAGS_EMPTY);
		   }
	   }

	   //we now have a list of user record each entry represented by raw redisReply *
	   if (success_counter) {
	  	 __unused size_t users_removed = 0;
       //filter out user's who don't reference this Fence in their Fence list...
       CollectionDescriptor collection_users				={.collection=(collection_t **)redis_ptr_session_records, success_counter};
       users_removed = _CleanUpFaultyUserEntriesForFence(sesn_ptr_target, f_ptr, userid_loaded_for, &collection_users,
                                                         MEMBER_FENCES);

		   if (fence_call_flags&FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE) {
		  	 //IMPORTANT: THIS DOES NOT LOAD FENCE LIST FOR SESSION; instead LAZY flag will be set
		  	 CollectionDescriptorPair collection_pair				={.first={(collection_t **)redis_ptr_session_records, success_counter}, .second={0}};
		  	 _InstateUsersListCacheRecordsForFence(sesn_ptr_target, &(f_ptr->fence_user_sessions_list), MEMBER_FENCES, instance_f_ptr,  &collection_pair, CALL_FLAG_REMOTE_SESSION);

			   //we are not writing back fence data or broadcasting users/sessions in this list
		   }
	   }

	   if (success_counter < userids_preprocessed_sz) {
		   syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p', fo:'%p'): ERROR: Received '%lu' elements: BUT ONLY PROCESED '%lu' WITH SUCCESS...", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, redis_ptr->elements, success_counter);
	   }

	   good_finish:
	   for (i=0; i < success_counter; ++i)	 if (IS_PRESENT(redis_ptr_session_records[i]))	freeReplyObject(redis_ptr_session_records[i]);

	   freeReplyObject(redis_ptr);

	   _RETURN_RESULT_SESN(sesn_ptr_target, f_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)
	}//redis block

	return_error:
	freeReplyObject(redis_ptr);
	_RETURN_RESULT_SESN(sesn_ptr_target, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_DATA)

	return_error_unlock:
	freeReplyObject(redis_ptr);
	_RETURN_RESULT_SESN(sesn_ptr_target, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_DATA)

}

/**
 * 	@brief: retrieve a given Fence's invited users member list and build/initialise it into Fence's Session list
 * 	FenceGetUserListFromBackend -> LOOP( _RetrieveSessionRecordFromRawBackend)
 * 									 -> FenceAttachRawSessionsList
 * 	TODO: refactor so the session creation happens here
 *
 * 	@WARNING: Fence MAY NOT BE POPULATED DONT ASSUME ANYTHING ABOUT FENEC DATA EXCEPT ID AND WHETHET DIRT FLAG IS SET
 *
 *	@param sesn_ptr: must have full backend access context
 *
 * 	@call_flag CALL_FLAG_ATTACH_USER_LIST_TO_FENCE:
 *
 * 	@locks: none
 *
 * 	@locked f_ptr: must be locked in the calling environment
 * 	@locked sesn_ptr: must be locked in the calling environment
 *
 * 	@dynamic_memory redis_ptr: Redis reply is created and destroyed
 * 	@dynamic_memory sesn_ptr_list: redisReply **sesn_ptr_list array of redis replys created and destroyed

 *	@call_flag: FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE
 *
 * 	@worker_thread: session worker, ufsrv worker
 */
UFSRVResult *
InstateInvitedMembersListCacheRecordForFence(Session *sesn_ptr, InstanceHolderForFence *instance_f_ptr, unsigned long userid_loaded_for, EnumFenceCollectionType list_type_target, EnumFenceCollectionType list_type_context, unsigned long fence_call_flags)
{
  Fence *f_ptr = FenceOffInstanceHolder(instance_f_ptr);

	//we need at lease one of these to be set to proceed...
	if (!(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_DIRTY)) && !(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY))) {
#ifdef __UF_TESTING
		 syslog(LOG_DEBUG, "%s (pid:'%lu, o:'%p', fo:'%p', fid:'%lu') NOT FETCHING SESSION LIST FOR FENCE (lazy:'%d', dirty:'%d')", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY), F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_DIRTY));
#endif

		 _RETURN_RESULT_SESN(sesn_ptr, f_ptr, RESULT_TYPE_SUCCESS, RESCODE_FENCE_LIST_SELFLOADED)
	}

	int									rescode = RESCODE_BACKEND_RESOURCE_NULL;
	PersistenceBackend	*pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);
	redisReply 					*redis_ptr	=	NULL;

#if 1
	//returns list of userids
	if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr, pers_ptr, REDIS_CMD_INVITED_USERS_FOR_FERNCE_GET_ALL, FENCE_ID(f_ptr))))	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_ERROR)	goto return_redis_error;
	if (redis_ptr->type == REDIS_REPLY_NIL)		goto return_redis_error;
  if (redis_ptr->elements == 0) {
#ifdef __UF_FULLDEBUG
 	 syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p, fo:'%p', fid:'%lu'): NOTICE: EMPTY SET FOR FENCE",  __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr));
#endif

 	 _RETURN_RESULT_SESN(sesn_ptr, f_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA_EMPTYSET)
  }
#endif

	   //we now have a list of UID's
#ifdef __UF_TESTING
   syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', fo:'%p', fid:'%lu'}: RESULT: Fence contains (%lu) invited users in it", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), redis_ptr->elements);
#endif

   {//block necessary because of variable sized array. Cannot exit with goto from this block
		 bool 					userid_found						=	false;
		 size_t					userids_preprocessed_sz	=	redis_ptr->elements;
		 PairOfUserIdUserName	uids_unames_preprocessed[userids_preprocessed_sz];

		 memset(uids_unames_preprocessed, 0, sizeof(uids_unames_preprocessed));

		 for (size_t i=0; i < userids_preprocessed_sz; ++i) {
			 if (NOT_TRUE(_ExtractUserIdUserNameFromListCacheRecord(redis_ptr->element[i]->str, &uids_unames_preprocessed[i]))) {
				syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', cid:'%lu', idx:'%lu', field(maybe modified):'%s'): ERROR: COULD NOT PARSE FIELD", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), i, redis_ptr->element[i]->str);
				continue;
			 }

			 if (fence_call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) if (uids_unames_preprocessed[i].uid==userid_loaded_for)	userid_found=true;
		 }

		 //where user wasn't found we want to continue process if the fence for which we are processing the user list was being loaded into
		 //the missing user; ie there is a referential problem: if I was loading the invited fences list for user and this fence was referenced
		 //that list we should see the user referenced here
		 if (!userid_found && (fence_call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) && (list_type_target == list_type_context)) {
			 syslog(LOG_DEBUG, "%s {pid:'%lu, o:'%p', fo:'%p', fid:'%lu', userid_loadedfor:'%lu'}: ERROR: REFERENTIAL ERROR: FENCE MEMBERS LIST DOESN'T REFER TO USER BEING LOADED FOR", __func__, pthread_self(), sesn_ptr, f_ptr, FENCE_ID(f_ptr), userid_loaded_for);
			 freeReplyObject(redis_ptr);

			 _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, RESCODE_FENCE_MEMBERSHIP)
		 }

		 //retrieve the raw user cache record for each uid and index it into list
		 //IMPORTANT: we are loading temporary session regardless if the users exists locally in order to perform data integrity check
		 //on members reflected in the returned cache backend.It is expensive.
		 size_t				success_counter =	0;
		 PairOfUserIdUserName			*uids_unames_processed[userids_preprocessed_sz];//filtered out list based on successfully fetched user records
		 redisReply								*redis_ptr_session_records[userids_preprocessed_sz];

		 memset(uids_unames_processed, 0, sizeof(uids_unames_processed));
		 memset(redis_ptr_session_records, 0, sizeof(redis_ptr_session_records));

		 for (size_t i=0; i < userids_preprocessed_sz; ++i) {
			 unsigned long 	user_id		= uids_unames_preprocessed[i].uid;
       CacheBackendGetRawSessionRecordByUserId(user_id, CALLFLAGS_EMPTY, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));

			 if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_SUCCESS) {
				 redisReply *redis_ptr_user = (redisReply *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
				 *(uids_unames_processed + success_counter) = &uids_unames_preprocessed[i];
				 *(redis_ptr_session_records + success_counter++) = redis_ptr_user;
			 } else {
				 //TODO: this a stale UID which must cleanse. perhaps log it in a list for out of band processing?
				 syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu', uid:'%lu'): ERROR: FOUND POTENTIALLY STALE UID --> TODO: IMPELMENT CLEANSING", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr), user_id);
			 }
		 }

		 //we now have a list of user record each entry represented by raw redisReply *
		 if (success_counter) {
			 if (fence_call_flags&FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE) {
				 CollectionDescriptorPair collection_pair				={ .first={(collection_t **)redis_ptr_session_records, success_counter},
						 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 	 .second={(collection_t **)uids_unames_processed, success_counter}};
				 _InstateUsersListCacheRecordsForFence(sesn_ptr, &(f_ptr->fence_user_sessions_invited_list), INVITED_FENCES, instance_f_ptr, &collection_pair, CALL_FLAG_REMOTE_SESSION);
				 //we are not writing back fence data or broadcasting users/sessions in this list
			 }
		 }

		 if (success_counter < redis_ptr->elements) {
			 syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', fo:'%p'): ERROR: Received '%lu' elements: BUT ONLY PROCESED '%lu' WITH SUCCESS...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, redis_ptr->elements, success_counter);
		 }

		 good_finish:
		 for (size_t i=0; i < success_counter; ++i)	 freeReplyObject(redis_ptr_session_records[i]);
		 freeReplyObject(redis_ptr);

		 _RETURN_RESULT_SESN(sesn_ptr, f_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)
   }

   return_redis_error:
   if (IS_EMPTY(redis_ptr)) {
	   syslog(LOG_DEBUG, "%s (pid:'%lu, o:'%p', cid:'%lu', fo:'%p', fid:'%lu') ERROR: CACHE BACKEND: NO REPLY RECEIVED...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr));_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_CONNECTION);
   }
   if (redis_ptr->type == REDIS_REPLY_ERROR) {
	   syslog(LOG_DEBUG, "%s (pid:'%lu, o:'%p', cid:'%lu', fo:'%p', fid:'%lu'): ERROR: REDIS RESULTSET. Error: '%s'", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr), redis_ptr->str);
	   rescode = RESCODE_BACKEND_DATA; goto return_error;
   }
   if (redis_ptr->type == REDIS_REPLY_NIL) {
	   syslog(LOG_DEBUG, "%s (pid:'%lu, o:'%p', cid:'%lu', fo:'%p', fid:'%lu': ERROR: NIL SET FOR FENCE",  __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr));
	   rescode = RESCODE_BACKEND_DATA; goto return_error;
   }

   return_error:
   freeReplyObject(redis_ptr);
   _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_ERR, rescode)

}

/**
 * @brief Fully load LinkJoining members using a user list retrieved  from CachBackend.
 * @param sesn_ptr_target Session representing the calling user. Only relevant if cleanup is invoked.
 * @param instance_f_ptr Target fence
 * @param userid_loaded_for
 * @param fence_call_flags
 * @return
 */
UFSRVResult *
InstateLinkJoiningMembersListCacheRecordForFence(Session *sesn_ptr_target, InstanceHolderForFence *instance_f_ptr, unsigned long userid_loaded_for, unsigned long fence_call_flags)
{
  Fence *f_ptr = FenceOffInstanceHolder(instance_f_ptr);

  //we need at least one of these to be set to proceed...
  if (!(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_DIRTY)) && !(F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY))) {
#ifdef __UF_TESTING
    syslog(LOG_DEBUG, "%s (pid:'%lu, th_ctx:'%p', fo:'%p', fid:'%lu') NOT FETCHING LINKJOIN SESSION LIST FOR FENCE (lazy:'%d', dirty:'%d')", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, FENCE_ID(f_ptr), F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY), F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_DIRTY));
#endif

    THREAD_CONTEXT_RETURN_RESULT_SUCCESS(f_ptr, RESCODE_FENCE_LIST_SELFLOADED)
  }

  redisReply 	*redis_ptr;
  CacheBackendLoadLinkJoinUserIdsForFence(FENCE_ID(f_ptr));

  if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_SUCCESS) {
    if (THREAD_CONTEXT_UFSRV_RESULT_CODE_EQUAL(THREAD_CONTEXT, RESCODE_BACKEND_DATA_EMPTYSET))	THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, RESCODE_BACKEND_DATA_EMPTYSET)

    redis_ptr = (redisReply *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
  } else	THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)

  {
    //>> we now have a raw list of UID's

#ifdef __UF_TESTING
    syslog(LOG_DEBUG, "%s {pid:'%lu, th_ctx:'%p', fo:'%p', fid:'%lu'}: RESULT: Fence contains (%lu) users in it", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, FENCE_ID(f_ptr), redis_ptr->elements);
#endif

   __unused  bool 					  userid_found						=	false;
    size_t					userids_preprocessed_sz	=	redis_ptr->elements;
    unsigned long 	userids_preprocessed[userids_preprocessed_sz]; //TODO: warning: stackoverflow

    memset(userids_preprocessed, 0, sizeof(userids_preprocessed));

    //is this logic relevant for this collection type?
    for (size_t i=0; i < userids_preprocessed_sz; ++i) {
      userids_preprocessed[i] = strtoul(redis_ptr->element[i]->str, NULL, 10);
      if (fence_call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER) if (userids_preprocessed[i] == userid_loaded_for)	userid_found = true;
    }

    size_t				i, success_counter = 0;
    redisReply		*redis_ptr_session_records[userids_preprocessed_sz];

    for (i=0; i < userids_preprocessed_sz; ++i)	redis_ptr_session_records[i] = NULL;

    //retrieve the raw user cache record for each uid and index it into list
    for (i=0; i < userids_preprocessed_sz; ++i) {
      unsigned long 	user_id		=  	userids_preprocessed[i];
      CacheBackendGetRawSessionRecordByUserId(user_id, CALLFLAGS_EMPTY, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));

      if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_SUCCESS) {
        redisReply *redis_ptr_user = (redisReply *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
        *(redis_ptr_session_records + success_counter++) = redis_ptr_user;
      } else {
        //TODO: this a stale UID which must be cleansed. First check in the DbBackend if the user has uninstalled. perhaps log it in a list for out of band processing?
        syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p', fo:'%p', fid:'%lu', uid:'%lu'): ERROR: FOUND POTENTIALLY STALE UID --> TODO: IMPELMENT CLEANSING", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, FENCE_ID(f_ptr), user_id);

        //TODO: implement fully.. currently an anchor point and must check the db backend
        ClearBackendCacheForSessionlessInvalidUserId_TO_BE_PORTED_FROM_UFSRVWEBSOCK(user_id, CALLFLAGS_EMPTY, CALLFLAGS_EMPTY);
      }
    }

    //we now have a list of user cachebackend records, each entry represented by raw redisReply *
    if (success_counter) {
      if (fence_call_flags&FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE) {
        //IMPORTANT: THIS DOES NOT LOAD FENCE LIST FOR SESSION; instead LAZY flag will be set
        CollectionDescriptorPair collection_pair				={.first={(collection_t **)redis_ptr_session_records, success_counter}, .second={0}};
        _InstateUsersListCacheRecordsForFence(sesn_ptr_target, FENCE_LINK_JOINING_USER_SESSIONS_LIST_PTR(f_ptr), LINKJOINED_FENCES, instance_f_ptr,  &collection_pair, CALL_FLAG_REMOTE_SESSION);

        //we are not writing back fence data or broadcasting users/sessions in this list
      }
    }

    if (success_counter < userids_preprocessed_sz) {
      syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p', fo:'%p'): ERROR: Received '%lu' elements: BUT ONLY PROCESSED '%lu' WITH SUCCESS...", __func__, pthread_self(), THREAD_CONTEXT_PTR, f_ptr, redis_ptr->elements, success_counter);
    }

    good_finish:
    for (i=0; i < success_counter; ++i)	 if (IS_PRESENT(redis_ptr_session_records[i]))	freeReplyObject(redis_ptr_session_records[i]);

    freeReplyObject(redis_ptr);

    THREAD_CONTEXT_RETURN_RESULT_SUCCESS(f_ptr, RESCODE_BACKEND_DATA)
  }//redis block

  return_error:
  freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)

  return_error_unlock:
  freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)

}

/**
 * 	@brief: Cache backend query function, returning raw ids for a given fence user's LinkJoin membership
 * 	@dynamic_memroy: EXPORTS redis_ptr
 */
UFSRVResult *
CacheBackendLoadLinkJoinUserIdsForFence(unsigned long fid)
{
  PersistenceBackend	*pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);

  redisReply *redis_ptr;

  if (!(redis_ptr = (*pers_ptr->send_command)(NO_SESSION, pers_ptr, REDIS_CMD_LINKJOINING_USERS_FOR_FERNCE_GET_ALL, fid))) {
    syslog(LOG_DEBUG, "%s (pid:'%lu) ERROR RESPONSE for bid:'%lu': ZRANGE LINKJOINING_USERS_FOR_FENCE:%lu 0 -1", __func__, pthread_self(), fid, fid);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_CONNECTION)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: REDIS RESULT SET for RESPONSE for fid:'%lu'. Error: '%s'",  __func__, pthread_self(), fid, redis_ptr->str);

    goto return_error;
  }
  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s (pid:'%lu'): ERROR: EMPTY SET FOR fid:'%lu'",  __func__, pthread_self(), fid);
    goto return_error;
  }

  if (redis_ptr->elements > 0) {
    THREAD_CONTEXT_RETURN_RESULT_SUCCESS(redis_ptr, RESCODE_BACKEND_DATA)
  } else {
    freeReplyObject(redis_ptr);
    THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, RESCODE_BACKEND_DATA_EMPTYSET)
  }

  return_error:
  freeReplyObject(redis_ptr);
  THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)

}

/**
 * @brief: helper routine
 * 	@ALREAT: modifies passed string
 */
static inline bool
_ExtractUserIdUserNameFromListCacheRecord(char *raw_cache_field, PairOfUserIdUserName *pair)
{
	//"307:xxx@gmail.com:335", includes extra field at the end(inviter uid)
	char *uid_str = raw_cache_field;
	char *uid_by  = strrchr(uid_str, ':'); *uid_by = '\0'; uid_by++;
	char *uname   = strchr(uid_str, ':');
	if (IS_PRESENT(uname)) {
		*uname      = '\0';
		pair->uname = ++uname;
		pair->uid   = strtoul(uid_str, NULL, 10);
		pair->aux   = uid_by;

		return true;
	}

	return false;
}

//END BUILD USER LIST FOR FENCE

// -------------------------- FIND AND FETCH ROUTINES ------------------------------
#if 1

/**
 *  @brief Search locally and network-wide for existence of fence based on it unique canonical name.
 *  @returns InstanceHolderForFence
 * 	@locks RW Fence *:  WHEN  the flag HASH_LOCALLY is set OR when the fence is hashed locally and FENCE_CALLFLAG_KEEP_FENCE_LOCKED was set
 * 	@unlocks Fence *: UNLESS (FENCE_CALLFLAG_HASH_FENCE_LOCALLY|
 *
 * 	@call_flag FENCE_CALLFLAG_SEARCH_BACKEND: if local search find none backend will be searched
 * 	@call_flag FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE: SET BY DEFAULT
 * 	@call_flag FENCE_CALLFLAG_HASH_FENCE_LOCALLY: SET BY DEFULT
 * 	@call_flag FENCE_CALLFLAG_KEEP_FENCE_LOCKED: Fence returned in locked state. In case of backend search works in combination with FENCE_CALLFLAG_HASH_FENCE_LOCALLY
 * 	@call_flag	FENCE_CALLFLAG_SNAPSHOT_INSTANCE: Used in conjunction with BACKEND, restricting the amount of data populated for the Fence. User
 * 	must return the instance back to recycler
 *
 */
UFSRVResult *
FindFenceByCanonicalName(Session *sesn_ptr_this, const char *fence_canonical_name, bool *fence_lock_state, unsigned long fence_call_flags)
{
	Fence 							*f_ptr		= NULL;
  InstanceHolderForFence *instance_f_ptr = NULL;
	PersistenceBackend	*pers_ptr	= NULL;

  instance_f_ptr = (InstanceHolderForFence *)HashLookup(&FenceRegistryCanonicalNameHashTable, (void *)fence_canonical_name, true);

	if (!IS_EMPTY(instance_f_ptr)) {
	  f_ptr = FenceOffInstanceHolder(instance_f_ptr);
		if (fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED) {
			FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_FALSE, SESSION_RESULT_PTR(sesn_ptr_this), __func__);
			if (likely(IS_PRESENT(fence_lock_state)))	*fence_lock_state = (SESSION_RESULT_CODE_EQUAL(sesn_ptr_this, RESCODE_PROG_LOCKED_BY_THIS_THREAD));
		}

		SESSION_RETURN_RESULT(sesn_ptr_this, instance_f_ptr, RESULT_TYPE_SUCCESS, RESCODE_PROG_RESOURCE_CACHED)
	}

	if ((IS_EMPTY(f_ptr)) && (!(fence_call_flags&FENCE_CALLFLAG_SEARCH_BACKEND)))	{SESSION_RETURN_RESULT(sesn_ptr_this, NULL, RESULT_TYPE_ERR, RESCODE_PROG_NULL_POINTER)}

	pers_ptr = THREAD_CONTEXT_FENCE_CACHEBACKEND;

	redisReply *redis_ptr;
	bool fence_lock_already_owned = false;

  if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr_this, THREAD_CONTEXT_FENCE_CACHEBACKEND, REDIS_CMD_MATCHING_FENCES_GET, fence_canonical_name, fence_canonical_name))) {
    syslog(LOG_DEBUG, "%s: ERROR RESPONSE for baseloc '%s' " REDIS_CMD_MATCHING_FENCES_GET, __func__, fence_canonical_name, fence_canonical_name, fence_canonical_name);
    SESSION_RETURN_RESULT(sesn_ptr_this, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_CONNECTION)
  }

  if (redis_ptr->type == REDIS_REPLY_ERROR) {
    syslog(LOG_DEBUG, "%s: ERROR: REDIS RESULTSET for baseloc '%s'. Error: '%s'", __func__, fence_canonical_name, redis_ptr->str);
    SESSION_RETURN_RESULT(sesn_ptr_this, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_CONNECTION)
  }

  if (redis_ptr->type == REDIS_REPLY_NIL) {
    syslog(LOG_DEBUG, "%s: ERROR: EMPTY SET FOR BASELOC '%s'",  __func__, fence_canonical_name);
    SESSION_RETURN_RESULT(sesn_ptr_this, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_RESOURCE_NULL)
  }
#endif

#ifdef __UF_FULLDEBUG
   syslog(LOG_DEBUG, "%s (pid:'%lu'): RESULT: Received (%lu) elements", __func__, pthread_self(), redis_ptr->elements);
#endif
   int i, j = 0;

   for (i=0; i < redis_ptr->elements; ++i) {
     char *fence_id_str = strrchr(redis_ptr->element[i]->str, ':');
     *fence_id_str = '\0'; fence_id_str++;

     if ((strcasecmp(redis_ptr->element[i]->str, fence_canonical_name) == 0)) {
       if (fence_call_flags&FENCE_CALLFLAG_SNAPSHOT_INSTANCE) {
         instance_f_ptr = FenceGetInstance(NULL, FENCE_CALLFLAG_USERFENCE|FENCE_CALLFLAG_SNAPSHOT_INSTANCE);
         if (unlikely(IS_EMPTY(instance_f_ptr))) {
           freeReplyObject(redis_ptr);
           SESSION_RETURN_RESULT(sesn_ptr_this, NULL, RESULT_TYPE_ERR, RESCODE_PROG_INCONSISTENT_STATE)
         }

         f_ptr = FenceOffInstanceHolder(instance_f_ptr);

         FENCE_ID(f_ptr) = strtoul(fence_id_str, NULL, 10);
         goto return_success;
       } else {
         unsigned long fence_call_flags_final = FENCE_CALLFLAG_HASH_FENCE_LOCALLY|FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE;

         if (fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED)	fence_call_flags_final |= FENCE_CALLFLAG_KEEP_FENCE_LOCKED;

         InstateCacheRecordForFence(sesn_ptr_this, UNSPECIFIED_FENCE_LISTTYPE, strtoul(fence_id_str, NULL, 10), UNSPECIFIED_UID, &fence_lock_already_owned, fence_call_flags_final);

         instance_f_ptr = (InstanceHolderForFence *)SESSION_RESULT_USERDATA(sesn_ptr_this);

         if (IS_PRESENT(instance_f_ptr))	goto return_success;
       }
     }
   }

   //we did not find it
  freeReplyObject(redis_ptr);

  syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p'): COULD NOT FIND Fence '%s' ", __func__, pthread_self(), sesn_ptr_this, fence_canonical_name);

  SESSION_RETURN_RESULT(sesn_ptr_this, NULL, RESULT_TYPE_ERR, RESCODE_PROG_NULL_POINTER)

  return_success:
  freeReplyObject(redis_ptr);
  if (IS_PRESENT(fence_lock_state))	*fence_lock_state = fence_lock_already_owned;
  SESSION_RETURN_RESULT(sesn_ptr_this, instance_f_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA) //fence returned in RW locked state if FENCE_CALLFLAG_KEEP_FENCE_LOCKED set

}

/**
 *
 * 	@locks f_ptr: RW LOCKED downstream if HASH_LOCALLY|KEEP LOCKED ARE set
 * 	@unlocks f_ptr: returned unlocked if fence was not of BaseFence type and keeplocked was set
 * 	@locked sesn_ptr_this:
 * 	@call_flag
 */
InstanceHolder *
FindBaseFenceByCanonicalName(Session *sesn_ptr, const char *fence_canonical_name, bool *fence_already_locked, unsigned long fence_call_flags)
{
	Fence *f_ptr = NULL;

	FindFenceByCanonicalName (sesn_ptr, fence_canonical_name, fence_already_locked, fence_call_flags);
	InstanceHolder *instance_holder_ptr = SESSION_RESULT_USERDATA(sesn_ptr);

	if (IS_EMPTY(instance_holder_ptr))	return NULL;

	f_ptr = FenceOffInstanceHolder(instance_holder_ptr);

	if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BASEFENCE)) {
		return instance_holder_ptr;
	} else {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s {pid:'%lu' o:'%p'}: FENCE '%s' IS NOT OF BASE TYPE", __func__, pthread_self(), sesn_ptr, FENCE_CNAME(f_ptr));
#endif

		//since the retrieval function  above is oblivion to fence type if these two flags are set it will return a locked fence
		//but since our client is only interested in type specific fence, we transparently unlock on their behalf, because the retrieval
		//has essentially failed from their perspective, hence NULL return--they have no way of unlocking a NULL reference!
		if (((fence_call_flags&FENCE_CALLFLAG_HASH_FENCE_LOCALLY) && (fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED)) || (fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED))
			if (!(*fence_already_locked))	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));

		return NULL;
	}

	return NULL;

}

//
//Searches Hash. Canonical names only apply to network created base fences
//>>RD LOCKS
//
/**
 * 	locks: None, unless fence was fetched from backend and the flags HASH_LOCALLY|FENCE_CALLFLAG_KEEP_FENCE_LOCKED were set
 * 			which has the effect of returning the fence locked state
 */
InstanceHolderForFence *
FindUserFenceByCanonicalName(Session *sesn_ptr, const char *fence_canonical_name, bool *fence_already_locked, unsigned long fence_call_flags)
{
	Fence *f_ptr = NULL;

	FindFenceByCanonicalName (sesn_ptr, fence_canonical_name, fence_already_locked, fence_call_flags);
	InstanceHolderForFence *instance_f_ptr = SESSION_RESULT_USERDATA(sesn_ptr);

	if (IS_EMPTY(instance_f_ptr))	return NULL;

	f_ptr = FenceOffInstanceHolder(instance_f_ptr);

	if (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_USERFENCE)) {
		return instance_f_ptr;
	} else {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s {pid:'%lu' o:'%p'}: FENCE '%s' IS NOT OF USER TYPE", __func__, pthread_self(), sesn_ptr, FENCE_CNAME(f_ptr));
#endif

		//since the retrieval function is oblivion to fence type if these two flags are set it will return a locked fence
		//but since our client is only interested in type specific fence, we transparently unlock on their behalf, because the retrieval
		//has essentially failed from their perspective, hence NULL return
		if (((fence_call_flags&FENCE_CALLFLAG_HASH_FENCE_LOCALLY) && (fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED)) || (fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED))
			if (!(*fence_already_locked))	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, SESSION_RESULT_PTR(sesn_ptr));

		return NULL;
	}

}

/**
 * 	@brief Searches locally and network-wide based on fence id. Relays all call_flags.
 * 	@returns InstanceHolderForFence
 * 	@locks: RW f_ptr
 * 	@locked Session
 * 	@call_flag FENCE_CALLFLAG_SEARCH_BACKEND: if fence is not found locally, perform search on backend. IMPORTANT sesn_ptr must be provided.
 * 	@call_flag FENCE_CALLFLAG_KEEP_FENCE_LOCKED: for backend search works when hash-locally is set
 */
UFSRVResult *
FindFenceById(Session *sesn_ptr, const unsigned long fence_id, unsigned long fence_call_flags)
{
	int 	res_code 	= RESCODE_PROG_LOCKED;
	ThreadContext *thread_ctx_ptr;
	UFSRVResult 	*res_ptr;

	if (IS_EMPTY(sesn_ptr)) {
		thread_ctx_ptr  = THREAD_CONTEXT_PTR;
		res_ptr         = THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT);
	} else {
		thread_ctx_ptr = THREAD_CONTEXT_PTR;
		res_ptr        =  SESSION_RESULT_PTR(sesn_ptr);
	}

	InstanceHolderForFence *instance_f_ptr = (InstanceHolder *)HashLookup(&FenceRegistryIdHashTable, (void *)&fence_id, true);

	if (!IS_EMPTY(instance_f_ptr)) {
    Fence *f_ptr = FenceOffInstanceHolder(instance_f_ptr);

		if (fence_call_flags&FENCE_CALLFLAG_KEEP_FENCE_LOCKED) {
			if (!(fence_call_flags&FENCE_CALLFLAG_LOCK_FENCE_BLOCKING))	FenceEventsLockRWCtx(thread_ctx_ptr, f_ptr, 1, res_ptr, __func__);
			else 																												FenceEventsLockRWCtx(thread_ctx_ptr, f_ptr, 0, res_ptr, __func__);

			if (_RESULT_TYPE_EQUAL(res_ptr, RESULT_TYPE_SUCCESS)) {
				if (_RESULT_CODE_EQUAL(res_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD)) res_code = RESCODE_PROG_LOCKED_BY_THIS_THREAD;
			} else	{_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_ERR, RESCODE_PROG_WONTLOCK)}
		}

		return_success:
		_RETURN_RESULT_RES(res_ptr, instance_f_ptr, RESULT_TYPE_SUCCESS, res_code)
	}

	if (IS_EMPTY(instance_f_ptr) && !(fence_call_flags&FENCE_CALLFLAG_SEARCH_BACKEND)) {_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_ERR, RESCODE_FENCE_DOESNT_EXIST)}

	//this honours FENCE_CALLFLAG_KEEP_FENCE_LOCKED
	bool fence_lock_already_owned = false;
  InstateCacheRecordForFence(sesn_ptr, UNSPECIFIED_FENCE_LISTTYPE, fence_id, SESSION_USERID(sesn_ptr), &fence_lock_already_owned, fence_call_flags);

	instance_f_ptr = SESSION_RESULT_USERDATA(sesn_ptr);

	if (IS_EMPTY(instance_f_ptr))	{_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_RESOURCE_NULL)}

	return_backend_success:
	_RETURN_RESULT_RES(res_ptr, instance_f_ptr, RESULT_TYPE_SUCCESS, fence_lock_already_owned ? RESCODE_PROG_LOCKED_BY_THIS_THREAD : RESCODE_BACKEND_DATA)

}

/**
 * 	@brief: just a decorator function.
 *
 * 	@locks: NONE
 * 	@locked: NONE
 */
Fence *
IsUserMemberOfFenceByCanonicalName(const List *const lst_ptr, const char *fence_canonical_name)

{
	return (_FindFenceInUserListByCanonicalName(lst_ptr, fence_canonical_name));

}

/**
 * 	@brief: Frontend function performs local fence search to find the Fence * to use for searching Session's list
 * 	@returns InstanceHolderForFenceStateDescriptor
 * 	@locks RD Fence *: if flagged
 */
InstanceHolderForFenceStateDescriptor *
IsUserMemberOfFenceByFenceId(const List *const lst_ptr_sesn, const unsigned long fence_id, bool lock_flag)
{
	if (unlikely(fence_id <= 0)) {
		syslog(LOG_ERR, "%s (pid:'%lu', fid:'%lu'): INVALID fence_id parameter <=0 ", __func__, pthread_self(), fence_id);
		return NULL;
	}

	if (lst_ptr_sesn->nEntries == 0) {
		return NULL;
	}

	UFSRVResult *res_ptr = FindFenceById(NULL, fence_id, CALLFLAGS_EMPTY);//local search only
  InstanceHolderForFence *instance_f_ptr = (InstanceHolderForFence *)_RESULT_USERDATA(res_ptr);

	if (IS_PRESENT(instance_f_ptr)) {
	  Fence *f_ptr_hash = FenceOffInstanceHolder(instance_f_ptr);
		InstanceHolderForFenceStateDescriptor *instance_fstate_ptr	=	IsUserMemberOfThisFence(lst_ptr_sesn, f_ptr_hash, lock_flag);
		if (IS_PRESENT(instance_fstate_ptr))		return instance_fstate_ptr;
		else																  return NULL;
	}
	else return NULL;

}

/*
 * 	@brief: Searches Session's FenceList for instance of supplied fence id
 * 	@returns InstanceHolderForFenceStateDescriptor
 *	@locks f_ptr: RD locked  unless the lock_flag is off
 *	@unlocks f_ptr: if lock_flag was on
 *	@locked Session *: underlying Session must be locked by caller to prevent changes to the list being searched
 *
 *	//TODO: IS this RD LOCK  USELESS in this context?
 */
InstanceHolderForFenceStateDescriptor *
IsUserMemberOfThisFence(const List *const lst_ptr_sesn, Fence *f_ptr, bool lock_flag)
{
	if (lst_ptr_sesn->nEntries == 0) {
		return NULL;
	}

	InstanceHolderForFenceStateDescriptor *instance_fstate_ptr = NULL;

	if (lock_flag) {
		FenceEventsLockRDCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_FALSE, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), __func__);
		if (_RESULT_CODE_EQUAL(ufsrv_thread_context.res_ptr, RESCODE_PROG_WONTLOCK))	return NULL;

		bool lock_already_owned = (_RESULT_CODE_EQUAL(ufsrv_thread_context.res_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD));

    instance_fstate_ptr = FindFenceInUserListByID(lst_ptr_sesn, FENCE_ID(f_ptr));

		if (!lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));

		return instance_fstate_ptr;
	} else {
    instance_fstate_ptr = FindFenceInUserListByID(lst_ptr_sesn, FENCE_ID(f_ptr));

		return instance_fstate_ptr;
	}

}

//
//searches in any List, but it designed to be used in searching Session's list of fences.
//looks up immutable field fence_id so there is no need to lock f_ptr
//user list is on average of small size
//Fence LOCKED at higher level
//SESSION MUST BE LOCKED
//
InstanceHolderForFenceStateDescriptor *
FindFenceInUserListByID(const List *const lst_ptr_sesn, unsigned long fid)
{
	ListEntry							*eptr										= NULL;
	FenceStateDescriptor	*fence_state_descriptor	= NULL;
	InstanceHolderForFenceStateDescriptor *instance_fstate_ptr    = NULL;

	for (eptr=lst_ptr_sesn->head; eptr; eptr=eptr->next) {
    instance_fstate_ptr = (InstanceHolder *)eptr->whatever;
		fence_state_descriptor = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);
	   if (FENCESTATE_FENCE(fence_state_descriptor)->fence_id == fid) {
		   return instance_fstate_ptr;
		}
	}

	return NULL;

}

/**
 * 	@brief: Searches the Sessions' FenceList for instance of the given fence cname. It does extra integrity check by also
 * 	fetching the hashed instance of theFence and compares it with what's found in the user's list.
 *
 * 	@locks: NONE
 * 	@locked sesn_ptr: assumed locked by the caller
 */
 //TODO Phase out _FindFenceInUserListByCanonicalName?
inline static Fence *
_FindFenceInUserListByCanonicalName(const List *const lst_ptr, const char *fence_canonical_name)
{
	ListEntry				*eptr 					= NULL;
	FenceStateDescriptor	*fence_state_descriptor	= NULL;

	if (lst_ptr->nEntries == 0 || !fence_canonical_name) {
		syslog(LOG_ERR, "%s  (pid:'%lu'): INVALID canonical name parameter was passed or List entries = 0 ", __func__, pthread_self());

		return (Fence *)NULL;
	}

	//TODO: this needs to be protected by read lock
	Fence *f_ptr_hash = NULL;
  f_ptr_hash = (Fence *)HashLookup(&FenceRegistryCanonicalNameHashTable, (void *)fence_canonical_name, true);

  for (eptr=lst_ptr->head; eptr; eptr=eptr->next) {
      fence_state_descriptor=(FenceStateDescriptor *)eptr->whatever;

     if (strncasecmp(fence_canonical_name, FENCESTATE_FENCE(fence_state_descriptor)->fence_location.canonical_name, strlen(FENCESTATE_FENCE(fence_state_descriptor)->fence_location.canonical_name)) == 0) {
       //cross check with hash
       if (f_ptr_hash && (strncasecmp(fence_canonical_name, f_ptr_hash->fence_location.canonical_name, strlen(f_ptr_hash->fence_location.canonical_name)) == 0)) {
         syslog(LOG_ERR, "%s (pid:'%lu'): SUCCESS: FOUND USER in bid='%lu' fcname='%s'", __func__, pthread_self(), f_ptr_hash->fence_id, f_ptr_hash->fence_location.canonical_name);

         return f_ptr_hash;
       } else {
         syslog(LOG_ERR, "%s (pid:'%lu', cname:'%s'): SEVERE ERROR: FOUND FENCE IN USER'S SESSION'S FENCE-LIST BUT NOT IN HASH)", __func__, pthread_self(), fence_canonical_name);
         return NULL;
       }
    }
  }

	//not found
	return NULL;

}

size_t
FenceListIterator(const List *const lst_ptr, void (^on_list_item)(ClientContextData *))
{
  for (ListEntry *eptr=lst_ptr->head; eptr; eptr=eptr->next) {
    on_list_item(eptr->whatever);
  }

  return lst_ptr->nEntries;
}

/**
 * 	@brief: Search Fence's userlist for the occurrence of a give session id.
 * 	@param lst_ptr_sesn: any list that hold Sessions entities
 * 	@param f_ptr: reference to actual fence entity
 * 	@returns: InstanceHolderForSession
 * 	@locked f_ptr:
 *
 */
inline InstanceHolderForSession *
FindUserInFenceSessionListByID(const List *const lst_ptr_sesn, Fence *f_ptr, unsigned long cid)
{
	ListEntry							*eptr										= NULL;
	InstanceHolderForSession *instance_sesn_ptr;
	Session 							*sesn_ptr_in_list;

	for (eptr=lst_ptr_sesn->head; eptr; eptr=eptr->next) {
    instance_sesn_ptr = (InstanceHolderForSession *)eptr->whatever;
    sesn_ptr_in_list = SessionOffInstanceHolder(instance_sesn_ptr);
		if (SESSION_ID(sesn_ptr_in_list) == cid)	return instance_sesn_ptr;
	}

#ifdef __UF_TESTING
		   syslog(LOG_DEBUG, "%s (pid:'%lu', of:'%p', fid:'%lu', cid:'%lu'): COULD NOT FIND Session in Fence List", __func__, pthread_self(), f_ptr, FENCE_ID(f_ptr), cid);
#endif

	return NULL;

}

/**
 *
 *	@locked RD|RW f_ptr:
 */
bool
IsUserOnFenceInvitedList(Fence *f_ptr, unsigned long uid)
{
  ListEntry *eptr;
  Session *sesn_ptr;

  for (eptr=f_ptr->fence_user_sessions_invited_list.head; eptr; eptr=eptr->next) {
    sesn_ptr = SessionOffInstanceHolder((InstanceHolderForSession *) eptr->whatever);

    if (SESSION_USERID(sesn_ptr) == uid) {
#ifdef __UF_TESTING
      syslog(LOG_DEBUG, "%s {pid:'%lu', uid:'%lu', cname:'%s', invitation_list_sz:'%d'}: User already on Fence's invitation list", __func__, pthread_self(), uid, FENCE_CNAME(f_ptr), f_ptr->fence_user_sessions_invited_list.nEntries);
#endif
      return true;
    }
  }

  return false;

}

/**
 *
 *	@locked RD|RW f_ptr:
 */
bool
IsUserOnFenceLinkJoinList(Fence *f_ptr, unsigned long uid)
{
  ListEntry *eptr;
  Session *sesn_ptr;
  for (eptr=FENCE_LINK_JOINING_USER_SESSION_LIST(f_ptr).head; eptr; eptr=eptr->next) {
    sesn_ptr = SessionOffInstanceHolder((InstanceHolderForSession *) eptr->whatever);

    if (SESSION_USERID(sesn_ptr) == uid) {
#ifdef __UF_TESTING
      syslog(LOG_DEBUG, "%s {pid:'%lu', uid:'%lu', fid:'%lu', requesting_list_sz:'%d'}: User's already on Fence's list", __func__, pthread_self(), uid, FENCE_ID(f_ptr), FENCE_LINK_JOINING_USER_SESSION_LIST_SIZE(f_ptr));
#endif
      return true;
    }
  }

  return false;

}

/**
 *
 *	@locked RD|RW f_ptr:
 */
bool
IsUserOnFenceBannedList(Fence *f_ptr, unsigned long uid)
{
  ListEntry *eptr;
  Session *sesn_ptr;
  for (eptr=FENCE_BANNED_USER_SESSION_LIST(f_ptr).head; eptr; eptr=eptr->next) {
    sesn_ptr = SessionOffInstanceHolder((InstanceHolderForSession *) eptr->whatever);

    if (SESSION_USERID(sesn_ptr) == uid) {
#ifdef __UF_TESTING
      syslog(LOG_DEBUG, "%s {pid:'%lu', uid:'%lu', cname:'%s', requesting_list_sz:'%d'}: User already on Fence's invitation list", __func__, pthread_self(), uid, FENCE_CNAME(f_ptr), FENCE_BANNED_USER_SESSION_LIST_SIZE(f_ptr));
#endif
      return true;
    }
  }

  return false;

}
// -------------------------- END OF FIND ROUTINES ------------------


//------------------ MESSAGE QUEUE ROUTINES --------------------------


//-------------------------- END OF MESSAGE QUEUE ROUTINES ------


//-------------- START backend fence instantiation FOR USER ---//
#if 1
static UFSRVResult *_GetFencesListCacheRecordForUser(Session *sesn_ptr_this, unsigned long call_flags, UFSRVResult *res_ptr_in);
static UFSRVResult * _GetFencesListCacheRecordForUserId(unsigned long uid, unsigned long call_flags, UFSRVResult *res_ptr_in);
static UFSRVResult *_InstateFencesListCacheRecordForUser(InstanceHolderForSession *instance_sesn_ptr_this, redisReply *redis_ptr_list, EnumFenceCollectionType, unsigned long sesn_call_flags, unsigned long fence_call_flags, UFSRVResult *res_ptr);
static FenceStateDescriptor *_InstateFenceIntoSessionFromCacheRecord(InstanceHolderForSession *instance_sesn_ptr, EnumFenceCollectionType list_type, unsigned long score, unsigned long fence_id, const char *uid_by, unsigned long fence_call_flags);
static FenceStateDescriptor *_InstateFenceForSnapshotSession(InstanceHolderForSession *instance_sesn_ptr, EnumFenceCollectionType list_type, unsigned long fence_id, const char *uid_by);
static UFSRVResult *_GetInvitedFencesListCacheRecordForUser(Session *sesn_ptr, unsigned long call_flags, UFSRVResult *res_ptr_in);
static UFSRVResult *_GetInvitedFencesListCacheRecordForUserWithScores(Session *sesn_ptr,  unsigned long call_flags, UFSRVResult *res_ptr_in);


/**
 * 	@brief Wrapper function for building a sessions's fence list
 * 	@param sesn_ptr: Must have full backend access context
 */
Session *
InstateMembersFenceListForUser(InstanceHolderForSession *instance_sesn_ptr, unsigned long sesn_call_flags, unsigned long fence_call_flags)
{
	UFSRVResult res;
	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

	_GetFencesListCacheRecordForUser(sesn_ptr, SESSION_CALLFLAGS_EMPTY, &res);
	if (res.result_type == RESULT_TYPE_SUCCESS) {
	  if (res.result_code == RESCODE_BACKEND_DATA) {
      redisReply *redis_ptr = (redisReply *)(res.result_user_data);

      _InstateFencesListCacheRecordForUser(instance_sesn_ptr, redis_ptr, MEMBER_FENCES, sesn_call_flags, fence_call_flags, &res);
      if (res.result_type == RESULT_TYPE_SUCCESS) {
        freeReplyObject(redis_ptr);
      }
		}

    return sesn_ptr;
	} else {
		syslog(LOG_DEBUG, "%s {pid:'%lu', o_target:'%p', cid_target:'%lu'}: ERROR: COULD NOT INSTANTIATE FENCE LIST FOR SESSION", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));

		return NULL;
	}

	return NULL;

}

/**
 * 	@brief:
 * 	Wrapper function for building a sessions's invited-for fence list
 * 	@param fence_call_flags FENCE_CALLFLAG_FENCE_LIST_WITH_SCORES: retrieve additional context information
 * 	@param sesn_ptr: Must have full backend access context
 */
Session *
InstateInvitedFenceListForUser(InstanceHolderForSession *instance_sesn_ptr, unsigned long sesn_call_flags, unsigned long fence_call_flags)
{
	UFSRVResult res;
	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

	//<fid:cname>
	if (fence_call_flags&FENCE_CALLFLAG_FENCE_LIST_WITH_SCORES)
	  _GetInvitedFencesListCacheRecordForUserWithScores(sesn_ptr, CALLFLAGS_EMPTY, &res);
	else
		_GetInvitedFencesListCacheRecordForUser(sesn_ptr, CALLFLAGS_EMPTY, &res);

	if (res.result_type == RESULT_TYPE_SUCCESS) {
		redisReply *redis_ptr = ((redisReply *)(res.result_user_data));

		_InstateFencesListCacheRecordForUser(instance_sesn_ptr, redis_ptr, INVITED_FENCES, sesn_call_flags, fence_call_flags, &res);
		if (res.result_type == RESULT_TYPE_SUCCESS) {
			freeReplyObject(redis_ptr);

			return sesn_ptr;
		}

		freeReplyObject(redis_ptr);
	} else {
		syslog(LOG_DEBUG, "%s {pid:'%lu', o_target:'%p', cid_target:'%lu'}: ERROR: COULD NOT INSTANTIATE FENCE LIST FOR SESSION", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));

		return NULL;
	}

	return NULL;

}

void
DestructFenceCollection(CollectionDescriptor *fence_collection_ptr, bool flag_self_destruct)
{
	if (!(IS_EMPTY(fence_collection_ptr))) {
		if (!(IS_EMPTY(fence_collection_ptr->collection))) free(fence_collection_ptr->collection);

		if (flag_self_destruct) {
			free(fence_collection_ptr);
			fence_collection_ptr = NULL;
		}

	}
}

/**
 * @brief: Primary point for fully loading user fences to session. Typically, invoked when SESNSTATUS_FENCELIST_LAZY
 * flag is set on Session.
 *	@param sesn_ptr: As this session may not always be a connected one, FULL ACCESS CONTEXT must be loaded for it by the caller
 * 	@param list_types_encoded: uses EnumFenceCollectionType
 */
int
InstateFenceListsForUser(InstanceHolderForSession *instance_sesn_ptr, unsigned long sesn_call_flags, FenceTypes fence_type, bool flag_abort_on_failure)
{
	unsigned int failed_lists = 0;

		//we want full fence list for this session + for each session full list of session
	if (fence_type&MEMBER_FENCE) {
		if (!(InstateMembersFenceListForUser(instance_sesn_ptr, sesn_call_flags, FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE|FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER))) {
			failed_lists |= (MEMBER_FENCE);
			if (flag_abort_on_failure)	return failed_lists;
		}
	}

	if (fence_type&INVITED_FENCE) {
		if (!(InstateInvitedFenceListForUser(instance_sesn_ptr, sesn_call_flags, FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE|FENCE_CALLFLAG_FENCE_LIST_WITH_SCORES|FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER))) {
			failed_lists |= (INVITED_FENCE);
			if (flag_abort_on_failure)	return failed_lists;
		}
	}

	_CrossCheckInvitedListsForUser(instance_sesn_ptr);

	return failed_lists;
}

/**
 * 	@brief: Queries the backend cache and retrieves the desired fence category as a Collection of fence ids.
 *
 * 	@sesn_ptr:	target user session. Must have full access context. Could be ephemeral, connected, or carriered
 *
 * 	@dynamic_memory redisReply *: IMPORTS when _GetFenceListFromBackendRaw() returns success, even if list is empty.
 * 	must be deallocated here.
 *
 * 	@dynamic_memory CollectionDescriptor *: EXPORTS if the value of 'fence_collection_ptr_in *' is NULL,
 * 	in which case the caller must deallocate
 *
 * 	@dynamic_memory CollectionDescriptor:collection_t: EXPORTS the actual collection
 *
 * 	@returns CollectionDescriptor *: which could user allocated/provided.
 * 	@return NULL: on empty set.
 */
CollectionDescriptor *
GetFenceCollectionForUser(Session *sesn_ptr, CollectionDescriptor *fence_collection_ptr_in, CollectionDescriptor *overflow_collection_ptr_in, EnumFenceCollectionType collection_type)
{
	UFSRVResult						res;
	CollectionDescriptor	*fence_collection_ptr			=	NULL;
	CollectionDescriptor	*overflow_collection_ptr	=	NULL;

	switch (collection_type)
	{
	case MEMBER_FENCES:
		_GetFencesListCacheRecordForUser(sesn_ptr, 0/*call_flags*/, &res);
		break;

	case INVITED_FENCES:
		_GetInvitedFencesListCacheRecordForUser(sesn_ptr, 0/*call_flags*/, &res);
		break;

	case BLOCKED_FENCES:
	case LIKED_FENCES:
	case FAVED_FENCES:
	default:
		return NULL;

	}

	if ((res.result_type == RESULT_TYPE_SUCCESS) && !(IS_EMPTY(res.result_user_data))) {
		if (res.result_code == RESCODE_BACKEND_DATA_EMPTYSET) {
			return NULL;//fence_collection_ptr;
		}

		if 		(!IS_EMPTY(fence_collection_ptr_in))	fence_collection_ptr = fence_collection_ptr_in;
		else																				fence_collection_ptr = calloc(1, sizeof(CollectionDescriptor));

		size_t			i = 0,
								processed = 0;
		char				*bid_str,
								*uid_inviter;
		redisReply	*redis_ptr_list	=	((redisReply *)(res.result_user_data));

		fence_collection_ptr->collection = calloc(redis_ptr_list->elements, sizeof(unsigned long));
		unsigned long *fence_ids		=	(unsigned long *)fence_collection_ptr->collection;
		unsigned long *by_ids				=	NULL;

		//this holds the reminder of raw data fter it is stripped of fenceid. Mostly *_by type information reflecting userid
		//TODO: impelemnt this as context with callback
		if (IS_PRESENT(overflow_collection_ptr_in))	{
			overflow_collection_ptr	=	overflow_collection_ptr_in;
			overflow_collection_ptr	=	calloc(redis_ptr_list->elements, sizeof(unsigned long));
			by_ids									=	(unsigned long *)overflow_collection_ptr->collection;
		}

		for (; i < redis_ptr_list->elements; ++i) {
			//"1804689344:1234"
			if (IS_STR_LOADED(uid_inviter = strrchr(redis_ptr_list->element[i]->str, ':'))) {
				*uid_inviter = '\0';	uid_inviter++;
				fence_ids[processed] = strtoul(redis_ptr_list->element[i]->str, NULL, 10);
				if (IS_PRESENT(by_ids))	by_ids[processed] = strtoul(uid_inviter, NULL, 10);
				processed++;
			} else {
				syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p',  cid:'%lu'): ERROR: COULD NOT EXTRACT BID UID value from redis set '%s': ignoring...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), redis_ptr_list->element[i]->str);
				continue;
			}

#if 0
			//"1804689344:Australia:New South Wales:Auburn:1234"
			bid_str=redis_ptr_list->element[i]->str;
			uid_inviter=strrchr (bid_str, ':'); *uid_inviter='\0'; uid_inviter++;
			fence_cname=strchr(bid_str, ':');
			if (fence_cname)
			{
				*fence_cname='\0';
				fence_cname++;
				fence_ids[processed++]=strtoul(bid_str, NULL, 10);

#ifdef __UF_TESTING
				syslog(LOG_DEBUG, "%s (pid='%lu', cid='%lu'): BACKEND RESULTSET CONTAINs FENCE: bid='%lu' CNAME: '%s' ", __func__, pthread_self(), SESSION_ID(sesn_ptr), fence_ids[processed-1], fence_cname);
#endif
			}
			else
			{
				syslog(LOG_DEBUG, "%s (pid='%lu' cid='%lu'): ERROR: COULD NOT EXTRACT BID UID value from redis set '%s': ignoring...", __func__, pthread_self(), SESSION_ID(sesn_ptr), redis_ptr_list->element[i]->str);
				continue;
			}
#endif
		}//for

		fence_collection_ptr->collection_sz = processed;
		if (IS_PRESENT(by_ids))	overflow_collection_ptr->collection_sz = processed;

		freeReplyObject(redis_ptr_list);

		return fence_collection_ptr;
	} else {
		syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p'}: ERROR: COULD NOT RETRIEVE RAW FENCE LIST...", __func__, pthread_self(), sesn_ptr);
	}

	return NULL;
}

/**
 *	@brief: Scan the returned raw fenceids in users raw Session CacheRecord  for the matching fid
 *	@para uid_inviter: returnthe inviter value if present
 *	@locks: none
 *	@locked sesn_ptr_carrier: could be locked if belongs to a real user
 *	@unlocks: none
 */
bool
IsFenceIdInCacheRecordForUser(unsigned long uid, unsigned long fid, unsigned long *uid_inviter)
{
	bool				return_value = false;
	redisReply *redis_ptr		=	NULL;

	_GetFencesListCacheRecordForUserId(uid, FENCE_CALLFLAG_EMPTY, NULL);

	if (THREAD_CONTEXT_UFSRV_RESULT_TYPE_SUCCESS) {
		redis_ptr = (redisReply *)THREAD_CONTEXT_UFSRV_RESULT_USERDATA; //can be NULL for empty set

		if (THREAD_CONTEXT_UFSRV_RESULT_CODE_EQUAL(THREAD_CONTEXT, RESCODE_BACKEND_DATA)) {
			size_t i = 0;
			unsigned long fid_processed;
			char *uid_inviter_processed;

			for (; i < redis_ptr->elements; ++i) {
				//"1804689344:1234"
				if (IS_STR_LOADED(uid_inviter_processed=strrchr(redis_ptr->element[i]->str, ':'))) {
					*uid_inviter_processed = '\0';	uid_inviter_processed++;
					fid_processed = strtoul(redis_ptr->element[i]->str, NULL, 10);
					if (fid == fid_processed) {
						return_value = true;
						if (IS_PRESENT(uid_inviter))	*uid_inviter = strtoul(uid_inviter_processed, NULL, 10);
						goto return_success;
					}
				} else {
					syslog(LOG_DEBUG, "%s (pid:'%lu', th_ctx:'%p', uid:'%lu', bid:'%lu'): ERROR: COULD NOT EXTRACT BID UID value from redis set(%s): ignoring...", __func__, pthread_self(), THREAD_CONTEXT_PTR, uid, fid, redis_ptr->element[i]->str);
				}
			}
	  }

		goto return_success;
	 }

	return false;

	return_success:
	if (IS_PRESENT(redis_ptr))  freeReplyObject(redis_ptr);
	return return_value;

}

/**
 * 	@brief:
 * 	Retrieve the user's list of fences from the backend in raw redis format.
 *
 * 	@param sesn_ptr_this:
 * 	current session being serviced or NULL if called from within ufrsv worker
 *
 * 	@param sesn_ptr_target:
 * 	session for which list is built, could be teh same as sesn_ptr_this
 *
 * 	@returns:
 * 	UFSRVResult * with res_ptr->userdata set to the resulting redisReply.
 *
 * 	@dynamic_memory:
 * 	user must free returned redisReply object
 *
 *	@locks: none as no local objects are manipulated
 *
 *	@call_flags: NONE
 */
static UFSRVResult *
_GetFencesListCacheRecordForUser(Session *sesn_ptr,  unsigned long call_flags, UFSRVResult *res_ptr_in)
{
	return _GetFencesListCacheRecordForUserId(SESSION_USERID(sesn_ptr), call_flags, res_ptr_in);

}

static UFSRVResult *
_GetFencesListCacheRecordForUserId(unsigned long uid, unsigned long call_flags, UFSRVResult *res_ptr_in)
{
	PersistenceBackend	*pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);
	redisReply					*redis_ptr;
	UFSRVResult					*res_ptr;

	if (!res_ptr_in)	res_ptr = THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT);
	else							res_ptr = res_ptr_in;

	if (!(redis_ptr = (*pers_ptr->send_command)(NO_SESSION, pers_ptr, REDIS_CMD_USER_FENCE_LIST_GET_ALL, uid))) {
		syslog(LOG_DEBUG, "%s: ERROR COULD NOT GET REDIS RESPONSE for CID '%lu'", __func__, uid);
		_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_BACKENDERR, RESCODE_BACKEND_CONNECTION)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	   syslog(LOG_DEBUG, "%s: REDIS_REPLY_ERROR COULD NOT GET REDIS RESPONSE for CID '%lu'", __func__, uid);

	   freeReplyObject(redis_ptr);

	   _RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_DATA)
	}

	if (redis_ptr->type == REDIS_REPLY_NIL || redis_ptr->elements == 0) {
#ifdef __UF_TESTING
	   syslog(LOG_DEBUG, "%s {pid:'%lu', th_ctx:'%p', uid:'%lu'}: COULD NOT RETRIEVE RECORD: EMPTY SET",  __func__, pthread_self(), THREAD_CONTEXT_PTR, uid);
#endif

    freeReplyObject(redis_ptr);

	   _RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA_EMPTYSET)
	}

#ifdef __UF_TESTING
	syslog(LOG_DEBUG, "%s (pid:'%lu'): UID '%lu': FOUND '%lu' fences", __func__, pthread_self(), uid, redis_ptr->elements);
#endif

	_RETURN_RESULT_RES(res_ptr, redis_ptr,  RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)

}

/**
 * 	@brief:
 * 	Retrieve the user's list of fences from the backend in raw redis format.
 *
 * 	@param sesn_ptr_this:
 * 	current session being serviced or NULL if called from within ufrsv worker
 *
 * 	@param sesn_ptr_target:
 * 	session for which list is built, could be teh same as sesn_ptr_this
 *
 * 	@returns:
 * 	UFSRVResult * with res_ptr->userdata set to the resulting redisReply.
 *
 * 	@dynamic_memory redisReply *: EXPORTS
 * 	user must free returned redisReply object
 *
 *	@locks: none as no local objects are manipulated
 *
 *	@call_flags: NONE
 */
static UFSRVResult *
_GetInvitedFencesListCacheRecordForUser(Session *sesn_ptr,  unsigned long call_flags, UFSRVResult *res_ptr_in)

{
	if (unlikely(IS_EMPTY(sesn_ptr)))
	{
		if (IS_PRESENT(res_ptr_in))	_RETURN_RESULT_RES(res_ptr_in, NULL, RESULT_TYPE_ERR, RESCODE_BACKEND_DATA)
		else						            return _ufsrv_result_generic_error;
	}

	UFSRVResult 		    *res_ptr;
	PersistenceBackend 	*pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);
	redisReply 			    *redis_ptr;

	if (!res_ptr_in)	res_ptr = THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT);
	else				      res_ptr = res_ptr_in;

	if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr, pers_ptr, REDIS_CMD_INVITED_FENCES_FOR_USER_GET_ALL, SESSION_USERID(sesn_ptr)))) {
		syslog(LOG_DEBUG, "%s: ERROR COULD NOT GET REDIS RESPONSE for CID '%lu'", __func__, SESSION_USERID(sesn_ptr));
		_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_BACKENDERR, RESCODE_BACKEND_CONNECTION)
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	   syslog(LOG_DEBUG, "%s: REDIS_REPLY_ERROR COULD NOT GET REDIS RESPONSE for CID '%lu'", __func__, SESSION_USERID(sesn_ptr));

	   freeReplyObject(redis_ptr);

	   _RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_BACKENDERR, RESCODE_BACKEND_DATA);;
	}

	if ((redis_ptr->type == REDIS_REPLY_NIL) || (redis_ptr->elements == 0)) {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', uid:'%lu'}: COULD NOT RETRIEVE RECORD: EMPTY SET",  __func__, pthread_self(), sesn_ptr, SESSION_USERID(sesn_ptr));
#endif

	   _RETURN_RESULT_RES(res_ptr, redis_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA_EMPTYSET)
	}

#ifdef __UF_TESTING
	syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p'): UID '%lu': FOUND '%lu' fences", __func__, pthread_self(), sesn_ptr, SESSION_USERID(sesn_ptr), redis_ptr->elements);
#endif

	_RETURN_RESULT_RES(res_ptr, redis_ptr,  RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)

}

/**
 * 	@brief:
 *
 * 	Similra to its cousine _GetInvitedFencesListCacheRecordForUserWithScores(), except it also retrieve
 * 	the user's list of fences from the backend in raw redis format, including original score values, which
 * 	translate to 'invited when timestamp' value for each fence.
 *
 */
static UFSRVResult *
_GetInvitedFencesListCacheRecordForUserWithScores(Session *sesn_ptr,  unsigned long call_flags, UFSRVResult *res_ptr_in)
{
	UFSRVResult 				*res_ptr;
	PersistenceBackend 	*pers_ptr = THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);
	redisReply 					*redis_ptr;

	if (!res_ptr_in)	res_ptr = THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT);
	else				      res_ptr = res_ptr_in;

	if (!(redis_ptr = (*pers_ptr->send_command)(sesn_ptr, pers_ptr, REDIS_CMD_INVITED_FENCES_FOR_USER_GET_ALL_WITHSCORES, SESSION_USERID(sesn_ptr)))) {
		syslog(LOG_DEBUG, "%s: ERROR COULD NOT GET REDIS RESPONSE for CID '%lu'", __func__, SESSION_USERID(sesn_ptr));
		_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_BACKENDERR, RESCODE_BACKEND_CONNECTION);
	}

	if (redis_ptr->type == REDIS_REPLY_ERROR) {
	   syslog(LOG_DEBUG, "%s: REDIS_REPLY_ERROR COULD NOT GET REDIS RESPONSE for CID '%lu'", __func__, SESSION_USERID(sesn_ptr));

	   freeReplyObject(redis_ptr);

	   _RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_BACKENDERR, RESCODE_BACKEND_DATA)
	}

	if ((redis_ptr->type == REDIS_REPLY_NIL) || (redis_ptr->elements == 0)) {
#ifdef __UF_FULLDEBUG
		syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', uid:'%lu'}: COULD NOT RETRIEVE RECORD: EMPTY SET",  __func__, pthread_self(), sesn_ptr, SESSION_USERID(sesn_ptr));
#endif

	   _RETURN_RESULT_RES(res_ptr, redis_ptr, RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA_EMPTYSET)
	}

#ifdef __UF_FULLDEBUG
	syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p'): UID '%lu': FOUND '%lu' fences", __func__, pthread_self(), sesn_ptr, SESSION_USERID(sesn_ptr), redis_ptr->elements);
#endif

	_RETURN_RESULT_RES(res_ptr, redis_ptr,  RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)

}

static void	_BuildFenceListWithScores(InstanceHolderForSession *instance_sesn_ptr, redisReply *redis_ptr_list, EnumFenceCollectionType list_type, unsigned long fence_call_flags);
static void	_BuildFenceList(InstanceHolderForSession *instance_sesn_ptr, redisReply *redis_ptr_list, EnumFenceCollectionType list_type, unsigned long fence_call_flags);
static void _CleanUpFaultyFenceEntriesForUser(Session *sesn_ptr, CollectionDescriptor *collection_ptr, size_t resultset_sz,  EnumFenceCollectionType list_type);


/**
 *      @brief: This is very specific clean up function used to resolve referencial user-fence integrity issues as seen from the
 *      User side; ie: the user's Fence List contained a reference to a Fence where that fenece did not have the user on its Members List.
 *
 *      @param raw_records_collection: The collection may contain less items than indicated by resultset_sz. Always check for payload.
 *
 */
static inline void
_CleanUpFaultyFenceEntriesForUser(Session *sesn_ptr, CollectionDescriptor *collection_ptr, size_t resultset_sz,  EnumFenceCollectionType list_type)//, char **raw_records_collection, size_t collection_sz, EnumFenceCollectionType list_type)
{
	if (resultset_sz > 0) {
		const char *prebuilt_command_template = NULL;

		if 			(list_type == MEMBER_FENCES)        prebuilt_command_template  =  REDIS_CMD_USER_FENCE_LIST_REM_PREBUILT;
		else if (list_type == INVITED_FENCES)       prebuilt_command_template  =  REDIS_CMD_INVITED_FENCES_FOR_USER_REM_PREBUILT;
		else return;

		size_t	actually_processed      	= collection_ptr->collection_sz+2;
		char    **raw_records_collection	=	(char **)collection_ptr->collection;
		PersistenceBackend      *pers_ptr	=THREAD_CONTEXT_PERSISTENCE_CACHEBACKEND(THREAD_CONTEXT);

		(*pers_ptr->send_command_multi)(sesn_ptr, pers_ptr, "MULTI");
		for (size_t i=0; i<collection_ptr->collection_sz; i++) {
			if (IS_PRESENT(raw_records_collection[i])) {
				(*pers_ptr->send_command_multi)(sesn_ptr, pers_ptr, prebuilt_command_template, SESSION_USERID(sesn_ptr), raw_records_collection[i]);
				syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', collection_sz:'%lu', resultset_sz:'%lu', arg:'%s'): ERROR: User's Fences List contained referential errors (user wasn't referenced)...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), collection_ptr->collection_sz, resultset_sz, raw_records_collection[i]);
			}
			else actually_processed--;
		}
		(*pers_ptr->send_command_multi)(sesn_ptr, pers_ptr, "EXEC");

		size_t          commands_successful     =       actually_processed;
		redisReply      *replies[actually_processed];

		memset (replies, 0, sizeof(replies));

		for (size_t i=0; i<actually_processed; i++) {
			if ((RedisGetReply((UfsrvSession *)sesn_ptr, pers_ptr, (void *)&replies[i])) != REDIS_OK) {
				commands_successful--;

				if ((replies[i] != NULL)) {
					syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', idex:'%lu'): ERROR PROCESSING MULTI SET COMMAND. ERROR: '%s'", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), i, replies[i]->str);
				} else {
					syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', i:'%lu'): ERROR COULD ISSUE GET COMMAND: BACKEND CONNECTIVITY ERROR", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), i);
				}
			}
		}

    //diagnostics
    if (commands_successful != actually_processed) {
			for (size_t i=0; i<actually_processed; i++)     if (IS_PRESENT(replies[i]))     freeReplyObject(replies[i]);

			return;
    }

    //verification block
    {
			//we only want to keep the last one which contains array of redisReply * corresponding with the number of commands issued, less exec/multi
			//the rest will return value of type REDIS_REPLY_STATUS
#define EXEC_COOMAND_IDX actually_processed-1

			for (size_t i=0; i<EXEC_COOMAND_IDX; i++)   if (IS_PRESENT(replies[i]))     freeReplyObject(replies[i]);

			if (IS_EMPTY(replies[EXEC_COOMAND_IDX])) {//idx for EXEC, which is last
				syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu'): ERROR: REDIS TRANSCTION ERROR: NULL COMMAND ARRAY RESPONSE", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr));

				return;
			}

			if (replies[EXEC_COOMAND_IDX]->elements == actually_processed - 2) {
				for (size_t i=0; i<replies[EXEC_COOMAND_IDX]->elements; i++) {
					redisReply *reply = replies[EXEC_COOMAND_IDX]->element[i];
					//TODO: CHECK CODE
				}
			} else {
				//Only remaining element is EXEC
				syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p', cid:'%lu', dispatched:'%lu', received:'%lu'): ERROR: REDIS TRANSCTION ERROR: DISPATCHED/RECEIVED COMMANDS COUNT MISMATCH", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), actually_processed-2, replies[EXEC_COOMAND_IDX]->elements);
			}

			if (IS_PRESENT(replies[EXEC_COOMAND_IDX]))      freeReplyObject(replies[EXEC_COOMAND_IDX]);
		}
	}
}

/**
 * 	@brief: Helper class to iterate over raw fence list for a user. This's strictly a utility class.
 * 	@param redis_ptr_list: raw redis collection in the form of "<fid>:<uid_invited_by>" which originates from
 * 	user's fences store. We should be able to detect data referential issues and recover from that if the target fence did not contain
 * 	the user in fences user's store and delete that fence from user's collection.
 * 	@locked: None
 */
static inline void
_BuildFenceList(InstanceHolderForSession *instance_sesn_ptr, redisReply *redis_ptr_list, EnumFenceCollectionType list_type, unsigned long fence_call_flags)
{
	int 						i								=	0;
	size_t          raw_records_sz	= 0;
	unsigned long		fence_id;
	char 						*uid_by;
	char 						*raw_records[redis_ptr_list->elements]; memset (raw_records, 0, sizeof(raw_records)); //TODO: warning: stackoverflow

	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

	for (; i < redis_ptr_list->elements; ++i) {
		//"1804689344:1234"
		raw_records[i]	=	strdupa(redis_ptr_list->element[i]->str);

		if (IS_STR_LOADED(uid_by = strrchr(redis_ptr_list->element[i]->str, ':'))) {
			*uid_by = '\0';	uid_by++;
			fence_id = strtoul(redis_ptr_list->element[i]->str, NULL, 10);
#ifdef __UF_FULLDEBUG
			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p',  cid:'%lu'): BACKEND RESULTSET CONTAINs FENCE: fid:'%lu' uid_inviter:'%s", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), fence_id, uid_by);
#endif
		} else {
			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p',  cid:'%lu'): ERROR: COULD NOT EXTRACT BID UID value from redis set '%s': ignoring...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), redis_ptr_list->element[i]->str);
			continue;
		}

		if (!(SESNSTATUS_IS_SET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_SNAPSHOT))) {
			if (IS_PRESENT(_InstateFenceIntoSessionFromCacheRecord(instance_sesn_ptr, list_type, 0/*score*/, fence_id, uid_by, fence_call_flags)))	raw_records[i]	= NULL;
			else raw_records_sz++;
		} else {
			_InstateFenceForSnapshotSession(instance_sesn_ptr, list_type, fence_id, uid_by);
		}
	}

	_CleanUpFaultyFenceEntriesForUser(sesn_ptr, &((CollectionDescriptor){.collection=(collection_t **)raw_records, .collection_sz=redis_ptr_list->elements}), raw_records_sz, list_type);
}

/**
 * 	@brief: Helper class to iterate over raw fence list, containing SCORES info for a user.
 * 	This strictly utility class.
 */
static inline void
_BuildFenceListWithScores(InstanceHolderForSession *instance_sesn_ptr, redisReply *redis_ptr_list, EnumFenceCollectionType list_type, unsigned long fence_call_flags)
{
	size_t          raw_records_sz  =       0;
	unsigned long		fence_id,
									score;
	char 						*uid_by;
	char 						*raw_records[redis_ptr_list->elements / 2]; memset (raw_records, 0, sizeof(raw_records)); // elements/2 because list contain scores as well as useful payload
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

	for (size_t i=0; i < redis_ptr_list->elements; i+=2) {
		score	=	strtoul(redis_ptr_list->element[i+1]->str, NULL, 10);//score at odd idx
		raw_records[i/2]	=	strdupa(redis_ptr_list->element[i]->str); //this is the payload always at even index

		//"1804689344:1234"
		if (IS_STR_LOADED(uid_by = strrchr(redis_ptr_list->element[i]->str, ':'))) {
			*uid_by = '\0';
			uid_by++;
			fence_id = strtoul(redis_ptr_list->element[i]->str, NULL, 10);
		} else {
			raw_records_sz++;
			syslog(LOG_DEBUG, "%s (pid:'%lu', o:'%p',  cid:'%lu', list_type:'%d'): ERROR: COULD NOT EXTRACT BID UID value from redis set '%s': ignoring...", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), list_type, redis_ptr_list->element[i]->str);

			continue;
		}

		if (!(SESNSTATUS_IS_SET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_SNAPSHOT))) {
			//_InstateFenceForSession (sesn_ptr, list_type, score, fence_id, uid_by, fence_call_flags);
			if (IS_PRESENT(_InstateFenceIntoSessionFromCacheRecord(instance_sesn_ptr, list_type, score, fence_id, uid_by, fence_call_flags)))	raw_records[i/2]	= NULL;
			else raw_records_sz++;
		} else {
			_InstateFenceForSnapshotSession(instance_sesn_ptr, list_type, fence_id,  uid_by);
		}
	}

	_CleanUpFaultyFenceEntriesForUser(sesn_ptr, &((CollectionDescriptor){.collection=(collection_t **)raw_records, .collection_sz=redis_ptr_list->elements/2}), raw_records_sz, list_type);

}

/**
 * 	@brief
 * 	In the context of initialising a User: Retrieve the user's list of fences from the backend previously returned in list
 * 	(output of REDIS_CMD_USER_FENCE_LIST_GET_ALL %bid:%uid) and build it into User's Session
 * 	We don't change the core state of the session we just reflect what is already in the backend, update local cache. Therefore,
 * 	no msgqueue broadcast is performed.
 *
 * 	@param redis_ptr:
 * 	Raw redis record reply, containing list of fences in the form: '%bid:%uid'. List type is indicated by list_type
 *
 * 	@param fence_call_flag FENCE_CALLFLAG_FENCE_LIST_WITH_SCORES:
 * 	@param call_flag:
 * 	(NOT IMPLEMENTED)CALL_FLAG_SESSION_FENCE_APPEND	append to existing user fence list
 *
 *	@param call_flag:
 * 	(NOT IMLEMENTED)CALL_FLAG_SESSION_FENCE_REBUILD	rebuild user fence list freeing all memory associated with old list, but dont remove fences from rego
 *
 * 	@dynamic_memory: caller must free IMPORTED redisReply instance
 *
 *	@locks: RW each fence in the list as the fence's list is updated to remove user session from it
 */
static UFSRVResult *
_InstateFencesListCacheRecordForUser(InstanceHolderForSession *instance_sesn_ptr, redisReply *redis_ptr_list, EnumFenceCollectionType list_type, unsigned long sesn_call_flags, unsigned long fence_call_flags, UFSRVResult *res_ptr_in)
{
	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);
	UFSRVResult			*res_ptr = NULL;

	if (IS_EMPTY(res_ptr_in))	res_ptr = SESSION_RESULT_PTR(sesn_ptr);
	else											res_ptr = res_ptr_in;

	//SESSION LOCKED (if flag set)

	if (SESSION_FENCE_LIST_COUNT(sesn_ptr) > 0) {
		if (sesn_call_flags&CALL_FLAG_SESSION_FENCE_REBUILD) {
			//free existing list
#ifdef __UF_TESTING
			syslog(LOG_DEBUG, "%s (cid='%lu'): SESSION CONTAINS '%d' IVITED-FOR FENCES: releasing before rebuilding...(NOT IMPLEMENTED)", __func__, SESSION_ID(sesn_ptr), SESSION_FENCE_LIST_COUNT(sesn_ptr));
#endif
			//no state change is intended therefore no msgqueue broadcast is necessary: we are just rebuilding internal data structure to
			//mirror established state in backend
			//RemoveUserFromMembersListForFence
		}
	}

#ifdef __UF_FULLDEBUG
	syslog(LOG_DEBUG, "%s {pid:'%lu' o:'%p', cid:'%lu', list_sz:'%lu'}: Fence CacheRecord from CacheBackend", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), redis_ptr_list->elements);
#endif

	if (redis_ptr_list->elements > 0) {
		if (fence_call_flags&FENCE_CALLFLAG_FENCE_LIST_WITH_SCORES) {
			_BuildFenceListWithScores(instance_sesn_ptr, redis_ptr_list, list_type, fence_call_flags);
		} else {
			_BuildFenceList(instance_sesn_ptr, redis_ptr_list, list_type, fence_call_flags);
		}
	}

	SESNSTATUS_UNSET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_FENCELIST_LAZY);

	_RETURN_RESULT_RES(res_ptr, sesn_ptr,  RESULT_TYPE_SUCCESS, RESCODE_BACKEND_DATA)

}

/**
 * 	@brief: Instate Fence for snapshot session (not hashed, locked, etc...). In this context we only one-way link Session <- Fence with
 * 	light-on fence data. This should be paired with ResetSessionData(), which has test for SNAPSHOT session type and knows how
 * 	to clean what's being instated here.
 * 	@return InstanceHolderForFenceStateDescriptor
 */
static inline FenceStateDescriptor *
_InstateFenceForSnapshotSession(InstanceHolderForSession *instance_sesn_ptr, EnumFenceCollectionType list_type,  unsigned long fence_id,  const char *uid_by)
{
	List				 					*user_fence_list_ptr;
	Fence									*f_ptr;
	FenceStateDescriptor	*fstate_ptr;

	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

  InstateCacheRecordForFence(sesn_ptr, list_type, fence_id, UNSPECIFIED_UID, NULL, FENCE_CALLFLAG_EMPTY);
  InstanceHolderForFence *instance_f_ptr = SESSION_RESULT_USERDATA(sesn_ptr);
	if (unlikely(IS_EMPTY(instance_f_ptr)))	return NULL;

	InstanceHolderForFenceStateDescriptor *instance_fstate_ptr = FenceStateDescriptorGetInstance((ContextData *)instance_f_ptr, FENCE_CALLFLAG_EMPTY);
	if (unlikely(IS_EMPTY(instance_fstate_ptr))) {
		FenceReturnToRecycler(instance_f_ptr, NULL, 0);
		return NULL;
	}

	f_ptr = FenceOffInstanceHolder(instance_f_ptr);
	fstate_ptr = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);

	F_ATTR_SET(f_ptr->attrs, F_ATTR_SNAPSHOT);
	F_ATTR_SET(f_ptr->attrs, F_ATTR_USERFENCE);

  GetUfsrvUid(strtoll(uid_by, NULL, 10), &(fstate_ptr->invited_by), false, NULL);

	switch (list_type)
	{
	case MEMBER_FENCES:
		user_fence_list_ptr = SESSION_FENCE_LIST_PTR(sesn_ptr);

		AddThisToList(user_fence_list_ptr, instance_fstate_ptr);
		FenceIncrementReference(instance_f_ptr, 1);

		InitialiseFenceUserPreferences (&((PairedSessionFenceState){fstate_ptr, sesn_ptr}));

		//f_state_ptr->when_joined=time(NULL);
		return fstate_ptr;

	case INVITED_FENCES:
		user_fence_list_ptr = SESSION_INVITED_FENCE_LIST_PTR(sesn_ptr);
		AddThisToList(user_fence_list_ptr, instance_fstate_ptr);
		FenceIncrementReference(instance_f_ptr, 1);

		return fstate_ptr;

  case LINKJOINED_FENCES:
    //Not applicable
    break;
	case BLOCKED_FENCES:
		//user_fence_list_ptr=&(sesn_ptr->ufnet_session.sservice.session_user_blocked_fence_list);
		break;

	default:
		;
	}

	FenceStateDescriptorReturnToRecycler(instance_fstate_ptr, (ContextData *)f_ptr, 0);

	return NULL;

}

#endif
//-------------- END backend fence instantiation FOR USER ---//

__attribute__ ((const)) bool
IsGeoFence(const Fence *f_ptr)
{
	return (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_BASEFENCE));
}

__attribute__ ((const)) bool
IsUserFence (const Fence *f_ptr)
{
	return (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_USERFENCE));
}

__attribute__ ((const)) bool
IsFenceSticky(const Fence *f_ptr)
{
	return (F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_STICKY));
}

__attribute__ ((const)) bool
IsFenceReferencedByUsers(InstanceHolderForFence *instance_f_ptr)
{
	size_t refcount_fence = RecyclerTypeGetReferenceCount(FencePoolTypeNumber(), instance_f_ptr);
	return (refcount_fence > 2);

}

//practically owner left, but fence has invitees who have not accepted invitation
__attribute__ ((const)) bool
IsFenceReferencedByInviteMembersOnly(const Fence *f_ptr)
{
	return (!F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_DIRTY) 					&&
					!F_ATTR_IS_SET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY) &&
					FENCE_SESSIONS_LIST_SIZE(f_ptr)==0 									&&
					FENCE_INVITED_LIST_SIZE(f_ptr)>0);
}

__pure bool
IsFenceOwnedByUser(Session *sesn_ptr, Fence *f_ptr)
{
	if (SESSION_USERID(sesn_ptr) == FENCE_OWNER_UID(f_ptr))	return true;

	return false;
}

static void _AssignFenceListTypeForInvited(Session *sesn_ptr, FenceStateDescriptor *fence_state_ptr, ClientContextData *context_ptr);

static void
_AssignFenceListTypeForInvited(Session *sesn_ptr, FenceStateDescriptor *fence_state_ptr, ClientContextData *context_ptr)
{
	PairOfUserIdUserName *pair_ptr = (PairOfUserIdUserName *)context_ptr;
  GetUfsrvUid(strtoul(pair_ptr->aux, NULL, 10), &(fence_state_ptr->invited_by), false, NULL);
//	fence_state_ptr->when_invited=pair_ptr->
}

__attribute__((const)) FenceListTypeDescriptor *
GetFenceListTypeDescriptor(EnumFenceCollectionType list_type)
{
  ///< maintain order as per \ref EnumFenceCollectionType
	static FenceListTypeDescriptor fence_list_types[] = {
			{0, 							  {NULL}},
			{MEMBER_FENCES, 	  {NULL}},
			{INVITED_FENCES, 	  {_AssignFenceListTypeForInvited}},
			{BLOCKED_FENCES, 	  {NULL}},
      {LINKJOINED_FENCES, {NULL}},
			{LIKED_FENCES, 		  {NULL}},
      {FAVED_FENCES, 		  {NULL}},
	};

	if (list_type >= MEMBER_FENCES && list_type < ALL_FENCES)	return &fence_list_types[list_type];

	return NULL;
}

//----------- Recycer Type Pool Fence---- //

////end typePool Fence /////////////////////


//----------- Recycer Type Pool FenceState ---- //
void InitFenceStateDescriptorRecyclerTypePool(size_t block_allocation_sz)
{
	#define _THIS_EXPANSION_THRESHOLD (1024 * 100)
	FenceStateDescriptorPoolHandle = RecyclerInitTypePool("FenceStateDescriptor", sizeof(FenceStateDescriptor), block_allocation_sz/*_CONF_SESNMEMSPECS_ALLOC_GROUPS(masterptr)*/,
                                                        _THIS_EXPANSION_THRESHOLD/*_CONF_SESNMEMSPECS_ALLOC_GROUP_SZ(masterptr)*/,
                                                        &ops_fence_state_descriptor);

	syslog(LOG_INFO, "%s: Initialised TypePool: '%s'. TypeNumber:'%d', Block Size:'%lu'", __func__, FenceStateDescriptorPoolHandle->type_name, FenceStateDescriptorPoolHandle->type, FenceStateDescriptorPoolHandle->blocksz);
}

/**
 * 	@brief: "constructor" type intialiser for newly instantiated objects just before attaching them to the recycler.
 * 	One off for the object's lifetime. No InstanceHolder ref yet.
 *
 */
static int
TypePoolInitCallback_FenceStateDescriptor(ClientContextData *data_ptr, size_t oid)
{
  FenceStateDescriptor *fstate_ptr = (FenceStateDescriptor *)data_ptr;

	return 0;//success
}

/**
 * 	@param ContextData: whatever  context data we might have passed to the recycler when we issued Get().
 */
static int
TypePoolGetInitCallback_FenceStateDescriptor(InstanceHolderForFenceStateDescriptor *data_ptr, ContextData *context_data, size_t oid, unsigned long call_flags)
{
  FenceStateDescriptor *fstate_ptr = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *)data_ptr);
	InstanceHolderForFence *instance_f_ptr = (InstanceHolderForFence *)context_data;

	fstate_ptr->instance_holder_fence = instance_f_ptr;

	return 0;//success
}

/**
 * 	@param ContextData: whatever  context data we might havepassed to the recycler when we issued Put In this instance Fence *
 */
static int
TypePoolPutInitCallback_FenceStateDescriptor(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
  FenceStateDescriptor *fstate_ptr = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *)data_ptr);
	Fence *f_ptr = (Fence *)context_data;

	return 0;//success
}

static char *
TypePoolPrintCallback_FenceStateDescriptor(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
	FenceStateDescriptor *fstate_ptr = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *)data_ptr);

	return NULL;
}

static int
TypePoolDestructCallback_FenceStateDescriptor(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
  FenceStateDescriptor *fstate_ptr = FenceStateDescriptorOffInstanceHolder((InstanceHolderForFenceStateDescriptor *)data_ptr);

	return 0;//success

}

void
FenceStateDescriptorIncrementReference(FenceStateDescriptor *descriptor_ptr, int multiples)
{
	RecyclerTypeReferenced(2, (RecyclerClientData *)descriptor_ptr, multiples);
}

void
FenceStateDescriptorDecrementReference(FenceStateDescriptor *descriptor_ptr, int multiples)
{
	RecyclerTypeUnReferenced(2, (RecyclerClientData *)descriptor_ptr, multiples);
}

inline unsigned
FenceStateDescriptorPoolTypeNumber()
{
	return FenceStateDescriptorPoolHandle->type;
}

/**
 * @brief Retrive a fresh instance from the recycler
 * @param ctx_data_ptr Context data to be passed to the initi function for Get
 * @param call_flags
 * @return
 */
InstanceHolderForFenceStateDescriptor *
FenceStateDescriptorGetInstance(ContextData *ctx_data_ptr, unsigned long call_flags)
{
	InstanceHolderForFenceStateDescriptor *instance_fstate_ptr = (InstanceHolderForFenceStateDescriptor *)RecyclerGet(FenceStateDescriptorPoolTypeNumber(), ctx_data_ptr, call_flags);
	if (unlikely(IS_EMPTY(instance_fstate_ptr)))	goto return_error;

	return instance_fstate_ptr;

	return_error:
	syslog(LOG_DEBUG, LOGSTR_INCONSISTENT_STATE, __func__, pthread_self(), (void *)0, 0UL, LOGCODE_PROTO_INCONSISTENT_STATE, "Could not get FenceStateDescriptor instance");
	return NULL;

}

int
FenceStateDescriptorReturnToRecycler(InstanceHolder *instance_older_ptr, ContextData *ctx_data_ptr, unsigned long call_flags)
{
	return RecyclerPut(FenceStateDescriptorPoolTypeNumber(), instance_older_ptr, (ContextData *)ctx_data_ptr, call_flags);
}
////end typePool  /////////////////////

void
SummariseUserFenceConfiguration(void)
{
	//MASTER_USER_FENCE_RDLOCK(SummariseUserFenceConfiguration);
	//SummariseFenceConfiguration (master_fence_registry_ptr, "UserFence");
	//MASTER_USER_FENCE_RWUNLOCK(SummariseUserFenceConfiguration);
}

void
SummariseBaseFenceConfiguration(void)
{
	//MASTER_BASE_FENCE_RDLOCK(SummariseBaseFenceConfiguration);
	//SummariseFenceConfiguration (master_base_fence_registry_ptr, "BaseFence");
	//MASTER_BASE_FENCE_RWUNLOCK(SummariseBaseFenceConfiguration);
}

//cross referenced reporting of users and fencesto check on the referencial integrity of lists across the network
static void SummariseFenceConfiguration(List *lst_ptr, const char *label)
{
	ListEntry *eptr;
	Fence *f_ptr;

	if (lst_ptr->nEntries == 0) {
		syslog (LOG_INFO, "%s: List has empty Fences in it '%d'", label, lst_ptr->nEntries);
		return;
	}

	syslog (LOG_INFO, ":::- Summary of %s Configuration Network wide -:::", label);
	syslog (LOG_INFO, "%s: List has '%d' Fences in it:", label, lst_ptr->nEntries);

	for (eptr=lst_ptr->head; eptr; eptr=eptr->next)
	{
	 f_ptr=(Fence *)eptr->whatever;
	 syslog (LOG_INFO, "%s: Fence: '%s'. ID: '%lu'. Number of Users: '%u' ",
			 label, f_ptr->fence_location.canonical_name, f_ptr->fence_id, f_ptr->fence_user_sessions_list.nEntries );
	 {//block1: fence list of users
		 ListEntry *eptr2;
		 SessionService *ss_ptr;
		 Session *sesn_ptr;
		 for (eptr2=f_ptr->fence_user_sessions_list.head; eptr2; eptr2=eptr2->next)
		 {
			 sesn_ptr=SessionOffInstanceHolder((InstanceHolderForSession *)eptr2->whatever);
			 ss_ptr=&(sesn_ptr->ufnet_session.sservice);
			 syslog (LOG_INFO, "`-- User name: '%s'. User's Fence count: '%u'",
					 ss_ptr->user.user_details.user_name, SESSION_FENCE_LIST_SIZE(sesn_ptr));
			 {//block2 user list of fences
				 ListEntry *eptr3;
				 Fence *f_ptr2;
				 for (eptr3=SESSION_FENCE_LIST(sesn_ptr).head; eptr3; eptr3=eptr3->next)
				 {
					 f_ptr2=FenceOffInstanceHolder(FenceStateDescriptorOffInstanceHolder(((InstanceHolderForFenceStateDescriptor *)(eptr3->whatever)))->instance_holder_fence);
					 syslog (LOG_INFO, "`---- Fence: '%s'. Fence's User count: '%u' ",
							 f_ptr2->fence_location.canonical_name, f_ptr2->fence_user_sessions_list.nEntries);
				 }

			 }//block2

		 }
	 }//block 1

	}

}

/**
 * 	@brief: This is a referenced userid, but for which there is no valid userbackend record, so no Session container
 * 	could be built for it
 *
 */
UFSRVResult * //todo fixme backport ufsrvwebsock -> ufnetcorelib
ClearBackendCacheForSessionlessInvalidUserId_TO_BE_PORTED_FROM_UFSRVWEBSOCK(unsigned long userid, unsigned long sesn_call_flags, unsigned long fence_call_flags)
{
	/*//check _ClearInvalidUserId, which RELIES ON STATUS BEING SET TO 0
	//check ClearBackendCacheForInvalidUserId
	if (false && !IsUserCloudRegistered(NULL, THREAD_CONTEXT_HTTP_REQUEST_CONTEXT)) { //todo implement properly, as it currently assumes session context
		//check cache backend for cookie based on uid and check if cookie can be signed on -> user may have unregistered but they are coming back. Also check for freshness
		//get all fences ->remove reciprocally
		//get all invited fences ->remove reciprocally
		//duplicate some of the del operations from ClearBackendCacheForInvalidUserId()
	}*/

	THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, RESCODE_BACKEND_DATA)
}

static List *
_GetUserListForSessionByFenceCollectionType(Session *sesn_ptr, EnumFenceCollectionType fence_collection_type)
{
  switch (fence_collection_type)
  {
  case 	MEMBER_FENCES:	return SESSION_FENCE_LIST_PTR(sesn_ptr);
  case	INVITED_FENCES:	return SESSION_INVITED_FENCE_LIST_PTR(sesn_ptr);

  default:
    ;
  }

  return NULL;
}

/**
 * 	@brief: helper function to detect if raw cache records list of users contain the a specific user. This is extra safety check to
 * 	detect at "run-time" if the list being processed contains the same user twice.
 *
 * 	@returns if returned userdata is NULL and rescode is  RESCODE_USER_INVALID_UID that indicates both uids are invalid, because
 * 	we detected two uids sharing the same username
 */
static inline UFSRVResult *
_CheckForDuplicateUsername(Session *sesn_ptr_carrier, Fence *f_ptr, redisReply *current_user, redisReply **processed_users_collection, size_t collection_sz)
{
	size_t idx = 0;

	for (; idx<collection_sz; idx++) {
		if (IS_EMPTY(processed_users_collection[idx]))	continue;

//		if ((strcasecmp(current_user->element[REDIS_KEY_USER_USER_NAME]->str, processed_users_collection[idx]->element[REDIS_KEY_USER_USER_NAME]->str)==0))
		if ((memcmp(current_user->element[REDIS_KEY_USER_UID]->str, processed_users_collection[idx]->element[REDIS_KEY_USER_UID]->str, CONFIG_MAX_UFSRV_ID_SZ)==0)) {
			syslog(LOG_ERR, "%s: (pid='%lu', o:'%p', fid:'%lu', fo:'%p', uname:'%s', uid_redis:'%p', cid_redis:'%s'): ERROR: DUPLICATE USERNAME IN FENCE LIST", __func__,
					pthread_self(), sesn_ptr_carrier, FENCE_ID(f_ptr), f_ptr, current_user->element[REDIS_KEY_USER_USER_NAME]->str, processed_users_collection[idx]->element[REDIS_KEY_USER_UID]->str, processed_users_collection[idx]->element[REDIS_KEY_USER_CID]->str);

			bool				userid_current_valid		= false,
									userid_processed_valid	= false;
			unsigned		userid_current_rescode		__attribute__((unused)),
									userid_processed_rescode	__attribute__((unused)); //use to check for returned errors

			unsigned long	userid_current	 = UfsrvUidGetSequenceId((UfsrvUid *)current_user->element[REDIS_KEY_USER_UID]->str);
			unsigned long	userid_processed = UfsrvUidGetSequenceId((UfsrvUid *)processed_users_collection[idx]->element[REDIS_KEY_USER_UID]->str);

			DbValidateUserId(sesn_ptr_carrier, userid_current);
			if (SESSION_RESULT_TYPE_SUCCESS(sesn_ptr_carrier))	userid_current_valid = true;
			userid_current_rescode = SESSION_RESULT_CODE(sesn_ptr_carrier);

			DbValidateUserId(sesn_ptr_carrier, userid_processed);
			if (SESSION_RESULT_TYPE_SUCCESS(sesn_ptr_carrier))	userid_processed_valid = true;
			userid_processed_rescode = SESSION_RESULT_CODE(sesn_ptr_carrier);

			if (userid_current_valid && userid_processed_valid) {
				syslog(LOG_ERR, "%s: (pid:'%lu', o:'%p', fid:'%lu', fo:'%p', uid_current:'%lu', uid_processed:'%lu'): ERROR: FOUND TWO VALID UIDs WITH THE SAME USERNAME", __func__, pthread_self(), sesn_ptr_carrier, FENCE_ID(f_ptr), f_ptr, userid_current, userid_processed);

				_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_ERR, RESCODE_USER_INVALID_UID);
			}

			if (userid_current_valid) {
				syslog(LOG_ERR, "%s: (pid:'%lu', o:'%p', fid:'%lu', fo:'%p', uid_current:'%lu', uid_invalid:'%lu'): ERROR: FOUND INVALID UID", __func__, pthread_self(), sesn_ptr_carrier, FENCE_ID(f_ptr), f_ptr, userid_current, userid_processed);

				_RETURN_RESULT_SESN(sesn_ptr_carrier, processed_users_collection[idx], RESULT_TYPE_ERR, RESCODE_USER_INVALID_UID);
			}

			if (userid_processed_valid) {
				syslog(LOG_ERR, "%s: (pid:'%lu', o:'%p', fid:'%lu', fo:'%p', uid_processed:'%lu', uid_invalid:'%lu'): ERROR: CURRENT UID  INVALID UID", __func__, pthread_self(), sesn_ptr_carrier, FENCE_ID(f_ptr), f_ptr, userid_processed, userid_current);

				_RETURN_RESULT_SESN(sesn_ptr_carrier, current_user, RESULT_TYPE_ERR, RESCODE_USER_INVALID_UID);
			}
		}
	}

	//all erroneous exit points are inlined above
	_RETURN_RESULT_SESN(sesn_ptr_carrier, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_NULL_POINTER);
}

/**
 * 	@brief: Helper to validate the associated userid with the fence and return the session if valid.
 * 	it also removes the session from hte system and the fence (single-sided).
 *
 * 	Designed to be called within _InstateUsersListCacheRecordsForFence() type processing.
 *
 * 	@param sesn_ptr: just a carrier session, not target for operation
 * 	@param:redis_ptr_invalid: raw redis record containing the invalid userid
 *
 * 	@type_pool: instantiates and returns temporary Session
 * 	@ref_count: does not increment
 *
 * 	@worker: session worker,ufsrv worker
 */
static void
_ClearInvalidUserId(Session *sesn_ptr_carrier, Fence *f_ptr, redisReply *redis_ptr_invalid)
{
  //todo: this can prove problematic as clearing cache entries require more data about the user and fence state for exampel invited by
  InstanceHolderForSession *instance_sesn_ptr_invalid = SessionLightlyInstantiateFromBackendRaw(sesn_ptr_carrier, NULL, redis_ptr_invalid, CALLFLAGS_EMPTY);
  if (!IS_EMPTY(instance_sesn_ptr_invalid)) {
    ClearBackendCacheForInvalidUserId(sesn_ptr_carrier, SessionOffInstanceHolder(instance_sesn_ptr_invalid), f_ptr, CALLFLAGS_EMPTY);
    SessionReturnToRecycler(instance_sesn_ptr_invalid, (ContextData *)NULL, CALLFLAGS_EMPTY);
  }
}

/**
 * 	@brief: Helper routine to instate and connect individual fence into a given session in the context of previously freshly retrieved
 * 	cache fence backend records of type list_type, eg. invited. the key point is we should not load the fence if it did not
 * 	have the user in it. Hence, why we need to keep the context of what list_type we are working with, because this function works
 * 	with multiple fence types (especially downstream). As a side effect of loading the fence, we'l be loading various other fence lists
 * 	it should not be an error if the user was not on members list if the context of the fence load was invited list.
 * 	the above rules only apply if FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER was set.
 * 	@param score: If available that will have been retrieved from the backend and is interpreted relative to list context
 *	@fence_call_flag FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER:
 * 	@locks RW f_ptr: where found
 * 	@unlocks f_ptr: where previously found
 */
static FenceStateDescriptor *
_InstateFenceIntoSessionFromCacheRecord(InstanceHolderForSession *instance_sesn_ptr, EnumFenceCollectionType list_type, unsigned long score, unsigned long fence_id, const char *uid_by, unsigned long fence_call_flags)
{
	bool		lock_already_owned 				= false,
					fence_lock_already_owned 	= false;
	Fence 	*f_ptr_hashed;
	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

	FindFenceById(sesn_ptr, fence_id, CALLFLAGS_EMPTY);//local search only
	InstanceHolder *instance_f_ptr_hashed = (InstanceHolderForFence *)SESSION_RESULT_USERDATA(sesn_ptr);

	if (IS_PRESENT(instance_f_ptr_hashed)) {
	  f_ptr_hashed = FenceOffInstanceHolder(instance_f_ptr_hashed);
		FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, f_ptr_hashed, _LOCK_TRY_FLAG_FALSE, SESSION_RESULT_PTR(sesn_ptr), __func__);
		if (SESSION_RESULT_TYPE_EQUAL(sesn_ptr, RESULT_TYPE_ERR))	return NULL;
		fence_lock_already_owned = (SESSION_RESULT_CODE_EQUAL(sesn_ptr, RESCODE_PROG_LOCKED_BY_THIS_THREAD));
	} else {
#ifdef __UF_TESTING
		syslog(LOG_DEBUG, "%s (pid'%lu', o:'%p', cid:%lu', list_type:'%d'): COULD NOT FIND  FENCE bid='%lu' LOCALLY: RETRIEVING FROM BACKEND", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), list_type, fence_id);
#endif

		unsigned fence_call_flags_final = 0;

		fence_call_flags_final = (FENCE_CALLFLAG_HASH_FENCE_LOCALLY|FENCE_CALLFLAG_KEEP_FENCE_LOCKED|FENCE_CALLFLAG_LOCK_FENCE_BLOCKING);
		if (fence_call_flags&FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE)				fence_call_flags_final |= FENCE_CALLFLAG_ATTACH_USER_LIST_TO_FENCE;
		if (fence_call_flags&FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER)	fence_call_flags_final |= FENCE_CALLFLAG_ABORT_RECORD_IF_USER_NOT_MEMBER;


		//we request to keep fence locked upon return
		//CALL_FLAG_ATTACH_USER_LIST_TO_FENCE does not add this user to list. we do it here
    InstateCacheRecordForFence(sesn_ptr, list_type, fence_id, SESSION_USERID(sesn_ptr), &fence_lock_already_owned, fence_call_flags_final);
    instance_f_ptr_hashed = (InstanceHolderForFence *)SESSION_RESULT_USERDATA(sesn_ptr);

		if (IS_EMPTY(instance_f_ptr_hashed)) {
			syslog(LOG_DEBUG, "%s (pid:'%lu' o:'%p', cid:'%lu', fid:'%lu', list_type:'%d'): ERROR COULD NOT INSTATE FENCE RECORD FROM BACKEND: MEMBERS LIST COULD HAVE BEEN EMPTY", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), fence_id, list_type);

			return NULL;
		}
	}

	f_ptr_hashed = FenceOffInstanceHolder(instance_f_ptr_hashed);

	//OK we now have a locked fence instance in the local cache. add and cross link this session<->fence
#define BACKEND_WRITEBACK 0
	//no write back because we are reflecting an already existing backend state
	//AddUserToExistingFenceAndLinkToUser(f_ptr_hashed, sesn_ptr, CALL_FLAG_FENCE_LIST_CHECK_DUP_SESSION|CALL_FLAG_SESSION_LIST_CHECK_DUP_FENCE);//BACKEND_WRITEBACK);

	List				 					*user_fence_list_ptr,
												*fence_user_list_ptr;
	FenceStateDescriptor	*f_state_ptr = NULL;
	InstanceHolderForFenceStateDescriptor *instance_fstate_ptr = NULL;

	switch (list_type)
	{
    case MEMBER_FENCES:
      user_fence_list_ptr = SESSION_FENCE_LIST_PTR(sesn_ptr);
      fence_user_list_ptr = &(f_ptr_hashed->fence_user_sessions_list);
      instance_fstate_ptr = AddUserToThisFenceListWithLinkback(instance_sesn_ptr, instance_f_ptr_hashed, user_fence_list_ptr, fence_user_list_ptr, 0/*event_type*/, CALL_FLAG_FENCE_LIST_CHECK_DUP_SESSION|CALL_FLAG_SESSION_LIST_CHECK_DUP_FENCE);
      if (IS_PRESENT(instance_fstate_ptr)) {
        f_state_ptr = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);
        InitialiseFenceUserPreferences (&((PairedSessionFenceState){f_state_ptr, sesn_ptr}));
        GetUfsrvUid(strtoll(uid_by, NULL, 10), &(f_state_ptr->invited_by), false, NULL);
      }
      //f_state_ptr->when_joined=time(NULL);
      break;

    case INVITED_FENCES:
      user_fence_list_ptr = SESSION_INVITED_FENCE_LIST_PTR(sesn_ptr);
      fence_user_list_ptr = &(f_ptr_hashed->fence_user_sessions_invited_list);
      instance_fstate_ptr = AddUserToThisFenceListWithLinkback(instance_sesn_ptr, instance_f_ptr_hashed, user_fence_list_ptr, fence_user_list_ptr,  0, CALL_FLAG_FENCE_LIST_CHECK_DUP_SESSION|CALL_FLAG_SESSION_LIST_CHECK_DUP_FENCE);
      if (IS_PRESENT(instance_fstate_ptr)) {
        f_state_ptr = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);
        GetUfsrvUid(strtoll(uid_by, NULL, 10), &(f_state_ptr->invited_by), false, NULL);
        //InitialiseFenceUserPreferences (&((PairedSessionFenceState){f_state_ptr, sesn_ptr})); //only di=o it when user actually joined
        f_state_ptr->when_invited = score;
      }
      break;

    default:
      ;
	}

#undef BACKEND_WRITEBACK

	if (!fence_lock_already_owned)	FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr_hashed, SESSION_RESULT_PTR(sesn_ptr));

	return f_state_ptr;
}
/**
*	@brief: 1)Preserves invit-by from the invite list if exists 2)Links the session into the Fence 3)links the Fence into Session
*
*	Fences are linked to sessions through a FenceStateDescriptor, a proxy that holds fence state relative to a Session. The state
*	is a set of properties that reflects the Session's relation to this fence: when he joined, who invited him, his permission-level
*	in the fence context. FenceStateDescriptors are stored in the Session's fence list.
*
*	When a link is created between Session and Fence, both sides receive entries. The Session gets a FenceStateDescriptor (a proxy for the
*	Fence) in its fence list, and the Fence receives a ClientContextData (a proxy for the Session) attached to
*	its relevant user list. References are incremented on both sides to prevent premature reclamation.
*
*	CALL_FLAG_WRITEBACK_FENCE_DATA_TO_BACKEND (REMOVED. For historical reference only): update and broadcast the event across the network
*
*	@locks: None directly, but locks associated with getting type instances from the type pool are engaged
*	@locked sesn_ptr: must be locked in the caller's environment
*	@locked RW f_ptr: must be locked in the caller's environment
*	@unlocks: None.
*
*	@blocks_on: None
*
*	@dynamic_memory: Implicitly creates ListEntry * in both Session and Fence which are freed else where as part of removing user from fence
*
*	@call_flags CALL_FLAG_SESSION_LIST_CHECK_DUP_FENCE: checks for the existence of the Fence in the user's Session
*	@call_flags CALL_FLAG_FENCE_LIST_CHECK_DUP_SESSION: checks for the existence of user session in Fence's list
*	@call_flags CALL_FLAG_WRITEBACK_FENCE_DATA_TO_BACKEND (REMOVED. For historical reference only): update and broadcast the event across the network
*
 */

/**
 * 	This is a generalised implementation of AddUserToExistingFenceAndLinkToUser(Fence *f_ptr, Session *sesn_ptr, unsigned call_flags)
 *
 * 	@brief: establish two-way link between a user-fence list. User-Session Fences have many lists, but they all have the same semantics
 * 	IMPORTANT NO integrity check is done if user is already in Fence. This should be done in the calling environment.
*
*	@param call_flags:
*	CALL_FLAG_WRITEBACK_FENCE_DATA_TO_BACKEND update backend and broadcast event through msgqueue
*
*	@locks: None directly, but locks associated with getting type instances from the type pool are engaged
*	@locked sesn_ptr: must be locked in the caller's environment
*	@locked RW f_ptr: must be locked in the caller's environment
*	@unlocks: None.
*
*	@blocks_on: None
*
*	@dynamic_memory: Implicitly creates ListEntry * in both Session and Fence which are freed else where as part of removing user from fence
*
*	@call_flags CALL_FLAG_SESSION_LIST_CHECK_DUP_FENCE: checks for the existence of the Fence in the user's Session
*	@call_flags CALL_FLAG_FENCE_LIST_CHECK_DUP_SESSION: checks for the existence of user session in Fence's list
*	@call_flags CALL_FLAG_WRITEBACK_FENCE_DATA_TO_BACKEND (REMOVED. For historical reference only): update and broadcast the event across the network
*
 */
InstanceHolderForFenceStateDescriptor *
AddUserToThisFenceListWithLinkback(InstanceHolderForSession *instance_sesn_ptr, InstanceHolderForFence *instance_f_ptr, List *user_fence_list_ptr, List *fence_user_list_ptr,  int event_type, unsigned call_flags)
{
	//integrity check
	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);
	Fence *f_ptr = FenceOffInstanceHolder(instance_f_ptr);

	if (call_flags&CALL_FLAG_FENCE_LIST_CHECK_DUP_SESSION) {
		if (CrossCheckSessionInFenceUserListByUserId(f_ptr, fence_user_list_ptr, SESSION_USERID(sesn_ptr))) {
			syslog(LOG_NOTICE, "%s {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu'}: DUPLICATE: CROSS CHECKING SESSION IN FENCE LIST FOUND SESSION: RETURNING INSTANCE FROM user_fence_list", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr));

			return (FindFenceStateInSessionFenceListByFenceId(sesn_ptr, user_fence_list_ptr, FENCE_ID(f_ptr)));
		}
	}

	//integrity check
	if (call_flags&CALL_FLAG_SESSION_LIST_CHECK_DUP_FENCE) {
		if (CrossCheckFenceInSessionFenceListByFenceId(sesn_ptr, user_fence_list_ptr, FENCE_ID(f_ptr))) {
			syslog(LOG_NOTICE, "%s (pid:'%lu' o:'%p', cid:'%lu', fo:'%p', fid:'%lu'): DUPLICATE: CROSS CHECKING FENCE IN SESSION LIST FOUND FENCE", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr));

			return NULL;
		}
	}

	InstanceHolder *instance_fstate_ptr = RecyclerGet(FenceStateDescriptorPoolTypeNumber(), (ContextData *)instance_f_ptr, CALLFLAGS_EMPTY);
	if (unlikely(IS_EMPTY(instance_fstate_ptr))) {
		syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu'}: ERROR: COULD NOT ALLOCATE FenceStateDescriptor type", __func__, pthread_self(), sesn_ptr, SESSION_ID(sesn_ptr), f_ptr, FENCE_ID(f_ptr));

		return NULL;
	}

  FenceStateDescriptor *fence_state_ptr = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);
  fence_state_ptr->instance_holder_fence = instance_f_ptr; //no need as it is done at GetInit by RecyclerGet()

	//preserve invited-by on this fence
  InstanceHolder *instance_fstate_ptr_invite = IsUserMemberOfThisFence(SESSION_INVITED_FENCE_LIST_PTR(sesn_ptr), f_ptr, false/*FLAG_FENCE_LOCK_FALSE*/);
	if (IS_PRESENT(instance_fstate_ptr_invite)) {
    FenceStateDescriptor *fstate_ptr_invite = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr_invite);
    memcpy(&(fence_state_ptr->invited_by), &(fstate_ptr_invite->invited_by), sizeof(UfsrvUid));
    fence_state_ptr->when_invited = fstate_ptr_invite->when_invited;
  }

	//2)link this Fence to User's List of Fences
	//fence is added by a proxy. The same fence can be in multiple instances of FenceStates across many users.
	//References are incremented every time a fence is referenced like this
	//The fence instance will live on for as long as it is referenced in a user's session list, until they explicitly leave
	//or inactivity rules are breached
	AddThisToList(user_fence_list_ptr, instance_fstate_ptr);
	FenceIncrementReference(instance_f_ptr, 1);

	//3)add this Session to Fence's List
	AddThisToList(fence_user_list_ptr, CLIENT_CTX_DATA(instance_sesn_ptr));
	SessionIncrementReference(instance_sesn_ptr, 1);

#ifdef __UF_TESTING
	syslog(LOG_DEBUG, "%s {pid:'%lu', o:'%p', fo:'%p'}: SUCCESS: LINKED (uid:'%lu' fence_count:'%d') to (fid:'%lu' user_count:'%d')...", __func__, pthread_self(), sesn_ptr, f_ptr, SESSION_USERID(sesn_ptr), user_fence_list_ptr->nEntries, FENCE_ID(f_ptr), fence_user_list_ptr->nEntries);
#endif

	return instance_fstate_ptr;

}

/**
 * 	@brief Load specified collection type into the \ref Fence object.
 *
 *  Fences hold a number of collection types, typically collection of users. Collection types are defined in \ref EnumFenceCollectionType.
 *
 * 	For each (redis-raw) user session record contained in the list:
 * 	1)instantiate a remote type session 2)attach it to the Fences Session list.
 * 	Designed to work within FenceGetUserListFromBackend()
 *
 *	@param sesn_ptr_target mostly functions as carrier Session, but it also reflects the current user Session for whom list is
 *	being processed, especially when check for uid invalidity is performed
 *  @param fence_user_list_ptr The collection object to load into
 *	@param collection_pair_ptr Contains the actual list of raw records. Additionally, collection_pair_ptr.second may contain additional
 *	indexed records list depending on context
 *
 *	DOESN'T NOT RESET FENCE USER LIST BEFORE ADDING, but no duplicates are allowed in user or fence list as they are cross checked
 *	@dynamic_memory: Caller is responsible for deallocating 'redisReplay **'
 *
 *	@locked f_ptr *: must locked by the caller
 *
 *	@worker: session worker, ufsrv worker
 */
static Fence *
_InstateUsersListCacheRecordsForFence(Session *sesn_ptr_target, List *fence_user_list_ptr, EnumFenceCollectionType user_fence_list_type, InstanceHolderForFence *instance_f_ptr, CollectionDescriptorPair *collection_pair_ptr, unsigned call_flags)
{
	size_t i = 0;

	unsigned					instan_flags		= 0;
	redisReply 				**redis_ptr_session_records 	= (redisReply **)collection_pair_ptr->first.collection;
	ClientContextData **contex_records 							= collection_pair_ptr->second.collection;
	size_t 						list_count										=	collection_pair_ptr->first.collection_sz;
	redisReply				*processed_users[list_count];
	UfsrvUid					*uid_ptr;

	Fence *f_ptr = FenceOffInstanceHolder(instance_f_ptr);

	for (; i<list_count; i++)	processed_users[i] = NULL;

#ifdef __UF_TESTING
	syslog(LOG_DEBUG, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu'}: Attaching '%lu' Users to Fence", __func__, pthread_self(), sesn_ptr_target, SESSION_ID(sesn_ptr_target), f_ptr, FENCE_ID(f_ptr), list_count);
#endif

	instan_flags |= call_flags&CALL_FLAG_REMOTE_SESSION? CALL_FLAG_REMOTE_SESSION : 0;
	instan_flags |= (CALL_FLAG_LOCK_SESSION|CALL_FLAG_HASH_SESSION_LOCALLY|CALL_FLAG_HASH_UID_LOCALLY|CALL_FLAG_HASH_USERNAME_LOCALLY);//no CALL_FLAG_ATTACH_FENCE_LIST_TO_SESSION

	for (i=0; i<list_count; i++) {
		redisReply *redis_ptr = *(redis_ptr_session_records + i);

		if (IS_EMPTY(redis_ptr)) {
			syslog(LOG_DEBUG, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu'}: ERROR: User at index '%lu' is NULL: RECEIVED '%lu' elements to attach to FENCE", __func__, pthread_self(), sesn_ptr_target, SESSION_ID(sesn_ptr_target), f_ptr, FENCE_ID(f_ptr), i, list_count);

			processed_users[i] = NULL;
			continue;
		}

		//IMPORTANT RELIES ON STATUS BEING SET TO 0
		if ((strtoul(redis_ptr->element[REDIS_KEY_USER_STATUS]->str, NULL, 10)) == 0) {
			const char *username_invalid = (!IS_EMPTY(redis_ptr->element[REDIS_KEY_USER_USER_NAME]->str))? redis_ptr->element[REDIS_KEY_USER_USER_NAME]->str : "";
			uid_ptr											 = (!IS_EMPTY(redis_ptr->element[REDIS_KEY_USER_UID]->str) && *(redis_ptr->element[REDIS_KEY_USER_UID]->str) != 0) ? (UfsrvUid *)redis_ptr->element[REDIS_KEY_USER_UID]->str : NULL;
//			const char *uid_invalid		=(!IS_EMPTY(redis_ptr->element[REDIS_KEY_USER_UID]->str))?redis_ptr->element[REDIS_KEY_USER_UID]->str:"";

			syslog(LOG_NOTICE, "%s: {pid:'%lu', o:'%p', cid:'%lu', fo:'%p', fid:'%lu', unamed_invalid:'%s', uid_ptr:'%p'}: WARNING: ENTIRE USER CACHEBACKEND RECORD WILL BE BLOWN OFF: User at index '%lu' is NULL: RECIEVED '%lu' elements to attach to FENCE", __func__, pthread_self(), sesn_ptr_target, SESSION_ID(sesn_ptr_target), f_ptr, FENCE_ID(f_ptr), username_invalid, uid_ptr, i, list_count);

			_ClearInvalidUserId(sesn_ptr_target, f_ptr, redis_ptr);

			continue;
		}

		//ensure the user is not in this list twice
		_CheckForDuplicateUsername(sesn_ptr_target, f_ptr, redis_ptr, processed_users, i + 1);

		if (unlikely(SESSION_RESULT_TYPE_ERROR(sesn_ptr_target))) {
			redisReply *redis_ptr_invalid = SESSION_RESULT_USERDATA(sesn_ptr_target);

			if (!(IS_EMPTY(redis_ptr_invalid))) {
				UfsrvUid *uid_ptr_invalid	=	(UfsrvUid *)redis_ptr_invalid->element[REDIS_KEY_USER_UID]->str;
				unsigned long userid_invalid = UfsrvUidGetSequenceId(uid_ptr_invalid);

				//some other dude with invalid userid
				if (SESSION_USERID(sesn_ptr_target) != userid_invalid) {
					_ClearInvalidUserId(sesn_ptr_target, f_ptr, redis_ptr_invalid);

					continue;//process next user
				} else {
					//kind of impossible?
					syslog(LOG_ERR, "%s: (pid:'%lu', o:'%p', cid:'%lu', uid_invalid:'%lu', fid:'%lu', fo:'%p'): SEVERE ERROR: CURRENT CONNECTED USER REPORTED WITH INVALID UID", __func__, pthread_self(), sesn_ptr_target, SESSION_ID(sesn_ptr_target), userid_invalid,  FENCE_ID(f_ptr), f_ptr);
					return NULL;
				}
			}

			if (IS_EMPTY(redis_ptr_invalid)) {
				//both userids share the same username
				syslog(LOG_ERR, "%s: (pid:'%lu', o:'%p', cid:'%lu', uid:'%lu', uname:'%s', fid:'%lu', fo:'%p'): SEVERE ERROR: TWO USERIDs SHARE THE SAME USERNAME...", __func__, pthread_self(), sesn_ptr_target, SESSION_ID(sesn_ptr_target), SESSION_USERID(sesn_ptr_target),  SESSION_USERNAME(sesn_ptr_target), FENCE_ID(f_ptr), f_ptr);
				return NULL;
			}
		}

		if (IS_EMPTY(redis_ptr->element[REDIS_KEY_USER_UID]->str) || *(redis_ptr->element[REDIS_KEY_USER_UID]->str) == 0 || IS_EMPTY(redis_ptr->element[REDIS_KEY_USER_USER_NAME]->str)) {
			if (!IS_EMPTY(redis_ptr->element[REDIS_KEY_USER_USER_NAME]->str))	processed_users[i] = redis_ptr;
			else processed_users[i] = NULL;

			syslog(LOG_DEBUG, "%s: (pid:'%lu', o:'%p', cid:'%lu'): ERROR: ELEMENT at index '%lu' HAS NULL UID OR USERNAME 'bid='%lu'", __func__, pthread_self(), sesn_ptr_target, SESSION_ID(sesn_ptr_target), i, FENCE_ID(f_ptr));

			continue;
		}

		if (UfsrvUidIsEqual(&SESSION_UFSRVUIDSTORE(sesn_ptr_target), (UfsrvUid *)redis_ptr->element[REDIS_KEY_USER_UID]->str)) {
			//session id sould be the same too as UID:%uid record will have been updated before this call after authentication
			//OK this is me: this potentially belongs to stale old entry, maybe server crashed and did not update properly
			//since we'll be added properly to this fence (not s a remote session) we can ignore this reference

			processed_users[i] = redis_ptr;

			continue;
		}

		//at this stage we don't know which of these sessions are local
		//TODO: NOTICE: for the same session id and different session pointer the has WILL SUCCEED as the same is will hash to the same slot number,
		//but due to collision one will be incremented: so we have one id, two session objects. We must ensure the id is not in the hash before adding

		//check for existence in local hash
		bool lock_already_owned = false;
		InstanceHolderForSession  *instance_sesn_ptr_remote;
    Session						        *sesn_ptr_remote;

		if ((instance_sesn_ptr_remote = LocallyLocateSessionById(strtoul(redis_ptr->element[REDIS_KEY_USER_CID]->str, NULL, 10)))) {
#ifdef __UF_FULLDEBUG
			syslog(LOG_DEBUG, "%s: (pid='%lu'): FOUND USER IN SESSION HASH (cid='%s') (LOCAL OR REMOTE): NOT INSTANTIATING SESSION", __func__, pthread_self(), redis_ptr->element[REDIS_KEY_USER_CID]->str);
#endif
      sesn_ptr_remote = SessionOffInstanceHolder(instance_sesn_ptr_remote);

			SessionLockRWCtx(THREAD_CONTEXT_PTR, sesn_ptr_remote, _LOCK_TRY_FLAG_TRUE, __func__);
			if (_RESULT_TYPE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), RESULT_TYPE_ERR)) {
				processed_users[i] = NULL;

				continue;
			}

			lock_already_owned = _RESULT_CODE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT), RESCODE_PROG_LOCKED_BY_THIS_THREAD);
		} else {
      if ((instance_sesn_ptr_remote = CacheBackendInstantiateRawSessionRecord(sesn_ptr_target, redis_ptr, instan_flags | CALL_FLAG_LOAD_DB_BACKEND_FOR_SESSION, NULL))) {
        sesn_ptr_remote = SessionOffInstanceHolder(instance_sesn_ptr_remote);
      } else {
        sesn_ptr_remote = NULL;
      }
		}

		if (!IS_EMPTY(sesn_ptr_remote)) {
			processed_users[i] = redis_ptr;
		} else {
			//could be because it is a duplicate local session, because hash will refuse rehahshing that
			syslog(LOG_DEBUG, "%s: (pid:'%lu', o:'%p', cid:'%lu'): ERROR: COULD NOT INSTANTIATE REMOTE SESSION: 'cid_redis='%s' to Fence: 'bid='%lu'",	__func__, pthread_self(), sesn_ptr_target, SESSION_ID(sesn_ptr_target), redis_ptr->element[REDIS_KEY_USER_CID]->str, FENCE_ID(f_ptr));

			processed_users[i] = NULL;

			continue; //process next record in list
		}

    InstanceHolderForFenceStateDescriptor *instance_fstate_ptr;
    FenceStateDescriptor *fstate_ptr;

    List *user_fence_list_ptr = _GetUserListForSessionByFenceCollectionType(sesn_ptr_remote, user_fence_list_type);//this is only relevant when reciprocal and referential links are to be maintained Fence <-> Session; currently, only members and invited types suport this
    if (IS_PRESENT(user_fence_list_ptr)) {
      instance_fstate_ptr = AddUserToThisFenceListWithLinkback(instance_sesn_ptr_remote, instance_f_ptr, user_fence_list_ptr, fence_user_list_ptr, EVENT_TYPE_UNDEFINED/*event_type*/, CALL_FLAG_FENCE_LIST_CHECK_DUP_SESSION | CALL_FLAG_SESSION_LIST_CHECK_DUP_FENCE);
      fstate_ptr = FenceStateDescriptorOffInstanceHolder(instance_fstate_ptr);

      FenceListTypeDescriptor *list_descriptor_ptr = GetFenceListTypeDescriptor(user_fence_list_type);
      if (IS_PRESENT(list_descriptor_ptr->type_ops.on_user_attached))
        (*list_descriptor_ptr->type_ops.on_user_attached)(sesn_ptr_remote, fstate_ptr, *(contex_records + i));
    } else {
      AddThisToList(FENCE_LINK_JOINING_USER_SESSIONS_LIST_PTR(f_ptr), CLIENT_CTX_DATA(instance_sesn_ptr_remote));
      SessionIncrementReference(instance_sesn_ptr_remote, 1);
    }

    if (!lock_already_owned)	SessionUnLockCtx(THREAD_CONTEXT_PTR, sesn_ptr_remote, __func__);

	}//for

	F_ATTR_UNSET(f_ptr->attrs, F_ATTR_SESSNLIST_LAZY);
	F_ATTR_UNSET(f_ptr->attrs, F_ATTR_DIRTY);

	return f_ptr;

}