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

#include "ufnet_msgcore/type_providers/type_provider_fence.h"
#include "ufsrv_core/system/log_message_literals.h"
#include "uflib/recycler/recycler.h"
#include <json-c/json_object.h>

#include "ufnetcorelib/ufnet_msgcore/fence/fence.h"
#include "ufnet_msgcore/fence/fence.h"
#include "ufnet_msgcore/fence/fence_permission.h"
#include "ufnetcorelib/ufnet_msgcore/fence/typepool_context_data_fence_type.h"
#include "ufnetcorelib/ufnet_msgcore/fence/utils_fence.h"
#include "ufsrv_core/cache_backend/redis.h"

extern __thread ThreadContext ufsrv_thread_context;

static RecyclerPoolHandle *FencePoolHandle;

static int	TypePoolInitCallback_Fence(ClientContextData *data_ptr, size_t oid);
static int	TypePoolGetInitCallback_Fence(InstanceHolder *data_ptr, ContextData *context_data, size_t oid, unsigned long call_flags);
static int	TypePoolPutInitCallback_Fence(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);
static char	*TypePoolPrintCallback_Fence(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);
static int	TypePoolDestructCallback_Fence(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags);

static RecyclerPoolOps ops_fence = {
        TypePoolInitCallback_Fence,
        TypePoolGetInitCallback_Fence,
        TypePoolPutInitCallback_Fence,
        TypePoolPrintCallback_Fence,
        TypePoolDestructCallback_Fence
};

void InitFenceRecyclerTypePool(size_t block_allocation_sz)
{
#define _THIS_FENCE_EXPANSION_THRESHOLD (1024 * 10)
  FencePoolHandle = RecyclerInitTypePool("Fence", sizeof(Fence), block_allocation_sz,
                                         _THIS_FENCE_EXPANSION_THRESHOLD/*_CONF_SESNMEMSPECS_ALLOC_GROUP_SZ(masterptr)*/,
                                         &ops_fence);

  syslog(LOG_INFO, "%s: Initialised TypePool: '%s'. TypeNumber:'%d', Block Size:'%lu'", __func__, FencePoolHandle->type_name, FencePoolHandle->type, FencePoolHandle->blocksz);
}

static void _MarshalPoolOperationsCallbackIfProvided(RecyclerPoolOps *ops_fence_ptr_provided)
{
  if (IS_PRESENT(ops_fence_ptr_provided)) {
    if (IS_PRESENT(ops_fence_ptr_provided->poolop_init_callback))     ops_fence.poolop_init_callback = ops_fence_ptr_provided->poolop_init_callback;
    if (IS_PRESENT(ops_fence_ptr_provided->poolop_initget_callback))  ops_fence.poolop_initget_callback = ops_fence_ptr_provided->poolop_initget_callback;
    if (IS_PRESENT(ops_fence_ptr_provided->poolop_initput_callback))  ops_fence.poolop_initput_callback = ops_fence_ptr_provided->poolop_initput_callback;
    if (IS_PRESENT(ops_fence_ptr_provided->poolop_print_callback))    ops_fence.poolop_print_callback = ops_fence_ptr_provided->poolop_print_callback;
    if (IS_PRESENT(ops_fence_ptr_provided->poolop_destruct_callback)) ops_fence.poolop_destruct_callback = ops_fence_ptr_provided->poolop_destruct_callback;
  }
}

void InitFenceRecyclerTypePoolWithProvider(RecyclerPoolOps *ops_fence_ptr_provided, size_t block_allocation_sz)
{
#define _THIS_FENCE_EXPANSION_THRESHOLD (1024 * 10)


  _MarshalPoolOperationsCallbackIfProvided(ops_fence_ptr_provided);
  FencePoolHandle = RecyclerInitTypePool("Fence", sizeof(Fence), block_allocation_sz,
                                         _THIS_FENCE_EXPANSION_THRESHOLD/*_CONF_SESNMEMSPECS_ALLOC_GROUP_SZ(masterptr)*/,
                                         &ops_fence);

  syslog(LOG_INFO, "%s: Initialised TypePool: '%s'. TypeNumber:'%d', Block Size:'%lu'", __func__, FencePoolHandle->type_name, FencePoolHandle->type, FencePoolHandle->blocksz);
}


/**
 * 	@brief: "constructor" type intialiser for newly instantiated objects just before attaching them to the recycler.
 * 	One off for the object's lifetime. No InstanceHolder ref yet.
 *
 */
static int
TypePoolInitCallback_Fence(ClientContextData *data_ptr, size_t oid)
{
  Fence *f_ptr = (Fence *)data_ptr;

  pthread_rwlockattr_init(&(f_ptr->fence_events.rwattr));

  int rc = pthread_rwlock_init(&(f_ptr->fence_events.rwlock), &(f_ptr->fence_events.rwattr));//==0 on success

  if (unlikely(rc != 0)) {
    char error_str[250] = {0};
    strerror_r(errno, error_str, 250);

    syslog(LOG_ERR, "%s: ERROR: (pid:'%lu', errno: '%d', error:'%s'): COULD NOT INITIALISE fence_events.rwlock...", __func__, pthread_self(), errno, error_str);

    return 1;//this will return NULL fence to the caller
  }

  InitialiseFencePermissionsTypes(f_ptr);
  InitialiseFencePermissionsSpecs(f_ptr);

  return 0;//success
}

/**
 * 	@param ContextData: whatever  context data we might have passed to the recycler when we issued Get In this instance Fence *
 */
static int
TypePoolGetInitCallback_Fence(InstanceHolder *data_ptr, ContextData *context_data, size_t oid, unsigned long call_flags)
{
  Fence *f_ptr = FenceOffInstanceHolder((InstanceHolderForFence *)data_ptr);

  if (call_flags&FENCE_CALLFLAG_GENERATE_ID) {
    FenceCacheBackend *fence_backend = (FenceCacheBackend *)context_data;

    if (IS_EMPTY(fence_backend)) {
      syslog (LOG_ERR, "%s {pid:'%lu', fo:'%p'}: ERROR: NO FENCE BACKEND SUPPLIED FOR ID ALLOCATION", __func__, pthread_self(), f_ptr);
      return -1;
    }

    if ((FENCE_ID(f_ptr) = GenerateCacheBackendId(fence_backend)) <= 0) {
      syslog (LOG_ERR, "%s {pid:'%lu', fo:'%p'}: ERROR: COULD NOT ALLOCATE FENCE ID", __func__, pthread_self(), f_ptr);
      return -1;
    }

    f_ptr->when = time(NULL);
  }

  if (call_flags&FENCE_CALLFLAG_SNAPSHOT_INSTANCE)	F_ATTR_SET(f_ptr->attrs, F_ATTR_SNAPSHOT);

  if (call_flags&FENCE_CALLFLAG_LOCK_FENCE) {
    FenceEventsLockRWCtx(THREAD_CONTEXT_PTR, f_ptr, _LOCK_TRY_FLAG_FALSE, THREAD_CONTEXT_UFSRV_RESULT(ufsrv_thread_context), __func__);
    if (_RESULT_TYPE_EQUAL(THREAD_CONTEXT_UFSRV_RESULT(ufsrv_thread_context), RESULT_TYPE_ERR))	return 0;
  }

  if (call_flags&FENCE_CALLFLAG_BASEFENCE) {
    F_ATTR_SET(f_ptr->attrs, F_ATTR_BASEFENCE);
    FENCE_OWNER_UID(f_ptr) = CONFIG_UFSRV_UID;

  } else if (call_flags&FENCE_CALLFLAG_USERFENCE) {
    F_ATTR_SET(f_ptr->attrs, F_ATTR_USERFENCE);

    //now done lazily
//		if (!InitialiseFencePermissions(f_ptr))
//		{
//			if (call_flags&FENCE_CALLFLAG_LOCK_FENCE) FenceEventsUnLock(f_ptr);
//
//			syslog(LOG_ERR, "%s: ERROR: (pid:'%lu'): COULD NOT INITIALISE FENCE PERMISSION", __func__, pthread_self());
//
//			return -1;//this will return NULL fence to the caller
//		}
  }

  //this will cause it to be fully fetched from backend
  F_ATTR_SET(f_ptr->attrs, F_ATTR_DIRTY);

  return 0;//success
}

/**
 * 	@param ContextData: whatever  context data we might have passed to the recycler when we issued Put. In this instance Fence *
 */
static int
TypePoolPutInitCallback_Fence(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
  //perhaps early in the initialisation we failed so there nothing to destruct
  if (IS_EMPTY(context_data))	return 0;

  Fence *f_ptr = FenceOffInstanceHolder(data_ptr);
  TypePoolContextDataFence *data = context_data;

  if (call_flags&FENCE_CALLFLAG_WRITEBACK_TO_DBBACKEND) {
    struct json_object 	*jobj_fence	= JsonFormatFenceForDbBackend(data->sesn_ptr, f_ptr, DIGESTMODE_FULL, 0/*lock*/);
    const char 					*jstr_fence	=	json_object_to_json_string(jobj_fence);

    DbBackendInsertFenceRecord(f_ptr, jstr_fence);
    json_object_put(jobj_fence);
  }

  if (call_flags&FENCE_CALLFLAG_EVICT_FROM_CACHEBACKEND) {
    CacheBackendRemCacheRecordForFence(data->sesn_ptr, f_ptr);
    // InterBroadcastFenceDestruct(data->sesn_ptr, f_ptr, NULL, 0);
  }

  DestructUserFence(data->sesn_ptr, &((InstanceContextForFence){data->fence_data.instance_f_ptr, FenceOffInstanceHolder(data->fence_data.instance_f_ptr)}), call_flags);

  //not relevant to check if lock owned already
  //note: if FENCE_CALLFLAG_LOCK was passed, _Destruct takes care of unlocking
  if (call_flags&FENCE_CALLFLAG_UNLOCK_FENCE) {
    FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, f_ptr, THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
  }

  return 0;//success
}

static char *
TypePoolPrintCallback_Fence(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
  Fence *f_ptr = FenceOffInstanceHolder((InstanceHolderForFence *)data_ptr);

  return NULL;
}

static int
TypePoolDestructCallback_Fence(InstanceHolder *data_ptr, ContextData *context_data, unsigned long call_flags)
{
  if (IS_EMPTY(context_data))	return 0;

  TypePoolContextDataFence *data = (TypePoolContextDataFence *)context_data;

  DestructUserFence(data->sesn_ptr,
                     &((InstanceContextForFence){data->fence_data.instance_f_ptr, FenceOffInstanceHolder(data->fence_data.instance_f_ptr)}),
                     call_flags);//expecting CALL_FLAG_SELF_DESTRUCT

  //not relevant to check if lock owned already
  //note: if FENCE_CALLFLAG_LOCK was passed, _Destruct takes care of unlocking
  if (call_flags&FENCE_CALLFLAG_UNLOCK_FENCE) {
    FenceEventsUnLockCtx(THREAD_CONTEXT_PTR, FenceOffInstanceHolder(data->fence_data.instance_f_ptr), THREAD_CONTEXT_UFSRV_RESULT(THREAD_CONTEXT));
  }

  return 0;//success

}

/**
 * @brief Retrieve a fresh instance from the recycler
 * @param ctx_data_ptr Context data to be passed to the init function for Get
 * @param call_flags
 * @return
 */
InstanceHolderForFence *
FenceGetInstance(ContextData *ctx_data_ptr, unsigned long call_flags)
{
  InstanceHolderForFence *instance_f_ptr = RecyclerGet(FencePoolTypeNumber(), ctx_data_ptr, call_flags);
  if (unlikely(IS_EMPTY(instance_f_ptr)))	goto return_error;

  return instance_f_ptr;

  return_error:
  syslog(LOG_DEBUG, LOGSTR_INCONSISTENT_STATE, __func__, pthread_self(), (void *)0, 0UL, LOGCODE_PROTO_INCONSISTENT_STATE, "Could not get Fence instance");
  return NULL;

}

int
FenceReturnToRecycler(InstanceHolderForFence *instance_f_ptr, ContextData *ctx_data_ptr, unsigned long call_flags)
{
  return RecyclerPut(FencePoolTypeNumber(), instance_f_ptr, (ContextData *)ctx_data_ptr, call_flags);
}

void
FenceIncrementReference(InstanceHolderForFence *instance_f_ptr, int multiples)
{
  RecyclerTypeReferenced(FencePoolTypeNumber(), (RecyclerClientData *)instance_f_ptr, multiples);
#if UFNET_DEBUG_BUILD_L3
  syslog(LOG_DEBUG, "%s: {pid:'%lu', fo:'%p'} ...", __func__, pthread_self(), instance_f_ptr);
#endif
}

void
FenceDecrementReference(InstanceHolderForFence *instance_f_ptr, int multiples)
{
  RecyclerTypeUnReferenced (FencePoolTypeNumber(), (RecyclerClientData *)instance_f_ptr, multiples);
#if UFNET_DEBUG_BUILD_L3
  syslog(LOG_DEBUG, "%s: {pid:'%lu', fo:'%p'} ...", __func__, pthread_self(), instance_f_ptr);
#endif
}

__pure inline unsigned
FencePoolTypeNumber()
{
  return FencePoolHandle->type;
}

