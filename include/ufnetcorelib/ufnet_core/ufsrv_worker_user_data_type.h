
/**
 * @file ufsrv_worker_user_data_type.h
 * @brief ufsrv_worker_user_data_type
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

#ifndef UFNETCORELIB_UFSRV_WORKER_USER_DATA_TYPE_H
#define UFNETCORELIB_UFSRV_WORKER_USER_DATA_TYPE_H


#include <ufsrvcorelib/ufsrv_core/delegator_sessions/thread_context_type.h>

typedef struct UfsrvWorkerUserData {
  PersistenceBackend             *persistence_backend;      ///< session data loaded from thread-specific data at service time
  InstrumentationBackend         *instrumentation_backend;  ///< loaded from thread-specific data at service time
  UserMessageCacheBackend        *usrmsg_cachebackend;
  FenceCacheBackend              *fence_cachebackend;
  MessageQueueBackend            *msgqueue_backend;         ///< msg queue publisher loaded from thread-specific data at service time
  DbBackend                      *db_backend; ///< Opened by the worker startup callback.
  RateLimitHandle                 ratelimit_handle;
  size_t                          slot_index; ///< Zero-based index within the worker pool for diagnostics.
  bool                            is_job_worker; ///< Identifies the pool that owns this slot.
} UfsrvWorkerUserData;

/**
 * @brief Allocate the worker slots, their Redis handles, and the delegator connection.
 * @param data Output package, expected to be empty.
 * @return true when the package is ready to pass to InitialiseDelegator().
 * @code{.c}
 * UfsrvDelegatorWorkersUserData data = {0};
 * if (InitialiseUfsrvDelegatorWorkersUserData(&data)) specs.ufsrv_delegator_userdata = &data;
 * @endcode
 */
PUBLIC_API bool InitialiseUfsrvDelegatorWorkersUserData(UfsrvDelegatorWorkersUserData *data);

/**
 * @brief Release the package after startup failure or after all threads join.
 * @code{.c}
 * DestroyUfsrvDelegatorWorkersUserData(&data);
 * @endcode
 */
PUBLIC_API void DestroyUfsrvDelegatorWorkersUserData(UfsrvDelegatorWorkersUserData *data);

/**
 * @brief Open main-thread connections and publish them as runtime configuration.
 * @param listener_rate_limit Receives the rate-limit handle bound to the main user-message cache.
 * @return The dedicated main-thread persistence connection, or NULL on failure.
 * @code{.c}
 * specs.on_init_main_thread_backends = InitialiseUfsrvMainThreadBackends;
 * @endcode
 */
PUBLIC_API PersistenceBackend *InitialiseUfsrvMainThreadBackends(RateLimitHandle *listener_rate_limit);

#endif //UFNETCORELIB_UFSRV_WORKER_USER_DATA_TYPE_H
