/**
 * @file ufsrv_delegator_utils.c
 * @brief ufsrv_delegator_utils
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 * Created by ayman on 18/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>

#include <hiredis/hiredis.h>
#include <json-c/json.h>
#include <uflib/db/db_sql.h>
#include <ufsrvcorelib/ufsrv_core/cache_backend/persistence.h>
#include <ufsrvcorelib/ufsrv_core/cache_backend/redis.h>
#include <ufsrvcorelib/ufsrv_core/protocol/protocol.h>
#include <ufsrvcorelib/ufsrv_core/ratelimit_v2/ratelimit_backend.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h>

#include "ufnet_core/ufsrv_worker_user_data_type.h"
#include "ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue.h"

static void
DestroyPersistenceHandle(PersistenceBackend *backend)
{
  if (!backend) return;
  if (backend->backend_handle_ptr) redisFree(backend->backend_handle_ptr);
  free((char *)backend->backend_config.con_tcp.host);
  free((char *)backend->backend_config.backend_label);
  free(backend);
}

static void
DestroyWorkerSlots(UfsrvWorkerUserData *slots, size_t count)
{
  if (!slots) return;
  for (size_t i = 0; i < count; i++) {
    UfsrvWorkerUserData *slot = &slots[i];
    RateLimitBackendDestroy(slot->ratelimit_handle.backend);
    if (slot->msgqueue_backend) {
      if (slot->msgqueue_backend->persistance_agent) redisFree(slot->msgqueue_backend->persistance_agent);
      free(slot->msgqueue_backend);
    }
    DestroyPersistenceHandle(slot->fence_cachebackend);
    DestroyPersistenceHandle(slot->usrmsg_cachebackend);
    DestroyPersistenceHandle(slot->persistence_backend);
  }
  free(slots);
}

void
DestroyUfsrvDelegatorWorkersUserData(UfsrvDelegatorWorkersUserData *data)
{
  DestroyWorkerSlots(data->session_worker, data->session_worker_count);
  DestroyWorkerSlots(data->job_worker, data->job_worker_count);
  DestroyPersistenceHandle(data->persistence_backend);
  *data = (UfsrvDelegatorWorkersUserData){0};
}

static PersistenceBackend *
InitialiseConfiguredPersistence(void)
{
  return InitialisePersistenceBackend(NULL, UfsrvConfigPersistenceBackendAddress(),
                                      UfsrvConfigPersistenceBackendPort(),
                                      UfsrvConfigServerDescriptiveName(), false);
}

PersistenceBackend *
InitialiseUfsrvMainThreadBackends(RateLimitHandle *listener_rate_limit)
{
  PersistenceBackend *persistence = InitialiseConfiguredPersistence();
  if (!persistence) return NULL;

  UserMessageCacheBackend *usrmsg = InitialiseCacheBackendUserMessage(
      NULL, UfsrvConfigCacheBackendAddressUsrmsg(), UfsrvConfigCacheBackendPortUsrmsg(),
      UfsrvConfigServerDescriptiveName(), false);
  if (!usrmsg || RedisVerifyScriptLoaded(persistence, REDIS_SCRIPT_SHA1_UNIQUE_ID, "UNIQUE ID GENERATION") != 0) {
    DestroyPersistenceHandle(usrmsg);
    DestroyPersistenceHandle(persistence);
    return NULL;
  }

  RateLimitHandleBindRedis(listener_rate_limit, usrmsg, UfsrvConfigRatelimitPolicyAsPtr());
  UfsrvConfigSetUsrmsgCachebackend(&(UfsrvConfigItem){.value.stored_value.ptr_val = usrmsg},
                                  UFSRV_CONFIG_SOURCE_RUNTIME);
  UfsrvConfigSetPersistenceBackend(&(UfsrvConfigItem){.value.stored_value.ptr_val = persistence},
                                   UFSRV_CONFIG_SOURCE_RUNTIME);

  return persistence;
}

static void
AddHandleAddress(struct json_object *record, const char *key, const void *handle)
{
  char address[sizeof(uintptr_t) * 2 + 3];
  if (handle)
    snprintf(address, sizeof(address), "0x%" PRIxPTR, (uintptr_t)handle);
  json_object_object_add(record, key, handle ? json_object_new_string(address) : NULL);
}

static struct json_object *
WorkerSlotRecord(const UfsrvWorkerUserData *slot, const ThreadContext *thread)
{
  struct json_object *record = json_object_new_object();
  if (!record) return NULL;

  json_object_object_add(record, "pool", json_object_new_string(slot->is_job_worker ? "job" : "session"));
  json_object_object_add(record, "slot_index", json_object_new_uint64(slot->slot_index));
  AddHandleAddress(record, "user_data", thread ? thread->user_data : slot);
  AddHandleAddress(record, "persistence_backend", thread ? thread->persistence_backend : slot->persistence_backend);
  AddHandleAddress(record, "instrumentation_backend", slot->instrumentation_backend);
  AddHandleAddress(record, "usrmsg_cachebackend", thread ? thread->usrmsg_cachebackend : slot->usrmsg_cachebackend);
  AddHandleAddress(record, "fence_cachebackend", thread ? thread->fence_cachebackend : slot->fence_cachebackend);
  AddHandleAddress(record, "msgqueue_backend", thread ? thread->msgqueue_backend : slot->msgqueue_backend);
  AddHandleAddress(record, "db_backend", thread ? thread->db_backend : slot->db_backend);
  AddHandleAddress(record, "ratelimit_backend",
                   thread ? thread->ratelimit_handle.backend : slot->ratelimit_handle.backend);
  return record;
}

static void
LogWorkerSlot(int priority, const char *event, const UfsrvWorkerUserData *slot, const ThreadContext *thread)
{
  struct json_object *record = WorkerSlotRecord(slot, thread);
  if (!record) return;

  json_object_object_add(record, "event", json_object_new_string(event));
  json_object_object_add(record, "pid", json_object_new_int64(getpid()));
  if (thread) {
    json_object_object_add(record, "thread_id", json_object_new_uint64((unsigned long)pthread_self()));
    AddHandleAddress(record, "thread_context", thread);
  }
  syslog(priority, "%s", json_object_to_json_string_ext(record, JSON_C_TO_STRING_PLAIN));
  json_object_put(record);
}

static void
LogWorkerSlotInventory(const UfsrvDelegatorWorkersUserData *data)
{
  struct json_object *record = json_object_new_object();
  if (!record) return;
  struct json_object *slots = json_object_new_array();
  if (!slots) {
    json_object_put(record);
    return;
  }

  json_object_object_add(record, "event", json_object_new_string("worker_slot_inventory"));
  json_object_object_add(record, "pid", json_object_new_int64(getpid()));
  json_object_object_add(record, "thread_id", json_object_new_uint64((unsigned long)pthread_self()));
  json_object_object_add(record, "session_worker_count", json_object_new_uint64(data->session_worker_count));
  json_object_object_add(record, "job_worker_count", json_object_new_uint64(data->job_worker_count));
  AddHandleAddress(record, "delegator_persistence_backend", data->persistence_backend);
  json_object_object_add(record, "slots", slots);

  const UfsrvWorkerUserData *session = data->session_worker;
  const UfsrvWorkerUserData *job = data->job_worker;
  for (size_t i = 0; i < data->session_worker_count; i++)
    json_object_array_add(slots, WorkerSlotRecord(&session[i], NULL));
  for (size_t i = 0; i < data->job_worker_count; i++)
    json_object_array_add(slots, WorkerSlotRecord(&job[i], NULL));

  syslog(LOG_INFO, "%s", json_object_to_json_string_ext(record, JSON_C_TO_STRING_PLAIN));
  json_object_put(record);
}

static bool
InitialiseWorkerSlots(UfsrvWorkerUserData *slots, size_t count, bool publish, bool is_job_worker)
{
  const char *label = UfsrvConfigServerDescriptiveName();
  for (size_t i = 0; i < count; i++) {
    UfsrvWorkerUserData *slot = &slots[i];
    slot->slot_index = i;
    slot->is_job_worker = is_job_worker;

    slot->persistence_backend = InitialiseConfiguredPersistence();
    if (!slot->persistence_backend) goto failed;

    slot->usrmsg_cachebackend = InitialiseCacheBackendUserMessage(NULL, UfsrvConfigCacheBackendAddressUsrmsg(),
                                                                  UfsrvConfigCacheBackendPortUsrmsg(), label, false);
    if (!slot->usrmsg_cachebackend) goto failed;

    slot->fence_cachebackend = InitialiseCacheBackendFence(NULL, UfsrvConfigCacheBackendAddressFence(),
                                                           UfsrvConfigCacheBackendPortFence(), label, false);
    if (!slot->fence_cachebackend) goto failed;

    RateLimitHandleBindRedis(&slot->ratelimit_handle, slot->usrmsg_cachebackend, UfsrvConfigRatelimitPolicyAsPtr());
    if (!slot->ratelimit_handle.backend) goto failed;

    if (publish) {
      slot->msgqueue_backend = BuildConnectionHandleForMessageQueueBackend(NULL);
      if (!slot->msgqueue_backend) goto failed;
    }
    continue;

  failed:
    LogWorkerSlot(LOG_ERR, "worker_slot_initialisation_failed", slot, NULL);
    return false;
  }
  return true;
}

static void
BindWorkerBackends(ThreadContext *thread)
{
  UfsrvWorkerUserData *slot = thread->user_data;

  /* The database connector initializes MariaDB state on the calling thread. */
  slot->db_backend = InitialiseDbBackend();
  if (!slot->db_backend) {
    LogWorkerSlot(LOG_ERR, "worker_db_initialisation_failed", slot, thread);
    exit(EXIT_FAILURE);
  }

  /* Existing consumers still use the ThreadContext backend accessors. */
  thread->db_backend = slot->db_backend;
  thread->persistence_backend = slot->persistence_backend;
  thread->usrmsg_cachebackend = slot->usrmsg_cachebackend;
  thread->fence_cachebackend = slot->fence_cachebackend;
  thread->msgqueue_backend = slot->msgqueue_backend;
  thread->ratelimit_handle = slot->ratelimit_handle;
  LogWorkerSlot(LOG_INFO, "worker_slot_bound", slot, thread);
}

bool
InitialiseUfsrvDelegatorWorkersUserData(UfsrvDelegatorWorkersUserData *data)
{
  if (!data) return false;

  UfsrvDelegatorWorkersUserData backends = {
    .session_worker_count = UfsrvConfigSessionWorkersThreadPool(),
    .job_worker_count = UfsrvConfigUfsrvWorkersThreadPool(),
    .slot_size = sizeof(UfsrvWorkerUserData),
    .on_worker_start = BindWorkerBackends,
  };

  if (!backends.session_worker_count || backends.session_worker_count >= INT_MAX) {
    syslog(LOG_ERR, "%s: Invalid session worker count %zu", __func__, backends.session_worker_count);
    return false;
  }

  backends.session_worker = calloc(backends.session_worker_count, backends.slot_size);
  if (backends.job_worker_count)
    backends.job_worker = calloc(backends.job_worker_count, backends.slot_size);

  if (!backends.session_worker ||
      (backends.job_worker_count && !backends.job_worker)) {
    syslog(LOG_ERR, "%s: Could not allocate worker backend slots", __func__);
    goto failed;
  }

  backends.persistence_backend = InitialiseConfiguredPersistence();
  if (!backends.persistence_backend) {
    syslog(LOG_ERR, "%s: Could not initialise the delegator persistence backend", __func__);
    goto failed;
  }
  bool session_publisher = ProtocolRegistryPublishesMessages();
  if (!InitialiseWorkerSlots(backends.session_worker, backends.session_worker_count, session_publisher, false) ||
      !InitialiseWorkerSlots(backends.job_worker, backends.job_worker_count, true, true))
    goto failed;

  LogWorkerSlotInventory(&backends);
  *data = backends;
  return true;

failed:
  DestroyUfsrvDelegatorWorkersUserData(&backends);
  return false;
}
