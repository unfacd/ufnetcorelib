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


#include <adt/adt_minheap.h>

#include "ufsrv_core/service/timer/delegator_timer_thread.h"
#include "ufnet_core/protocol/protocol_io.h"
#ifdef HAVE_CONFIG_UFSRVCORELIB_H
#include <config_ufsrvcorelib.h>
#endif

#include "protocol_sandbox_priv.h"
#include <uflib/recycler/recycler.h>
#include <uflib/net/utils_net.h>
#include <ufsrv_core/delegator_sessions/base_thread_context_data_type.h>
#include <ufnetcorelib/ufnet_core/ufsrvcore_utils.h>
#include "ufnet_core/ufsrv_worker_user_data_type.h"
#include "ufnet_msgcore/crypto_zk.h"
#include <ufnetcorelib/ufnet_msgcore/type_providers/session_provider.h>
#include <ufsrv_core/cache_backend/persistence.h>
#include <ufsrv_core/http/http_request.h>
#include <ufsrv_core/delegator_sessions/delegator_session_worker_thread.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h>
#include <ufsrv_core/delegator_sessions/workers/jobworkers_utils.h>
#include <ufsrv_core/system/thread_utils.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_utils.h>
#include <ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue.h>
#include <ufsrvcorelib/ufsrv_core/instrumentation/instrumentation_backend.h>
#include <ufsrv_core/ratelimit_v2/ratelimit.h>
#include <ufnetcorelib/ufnet_msgcore/session_result_macros.h>

extern const Protocol 	*const protocols_registry_ptr;
extern __thread ThreadContext ufsrv_thread_context;

extern void InitUFSRV(SessionsDelegator *sd_ptr, SessionsDelegatorInitSpecs *spec);

static UFSRVResult *
sUfsrvWorkerPoolOneoffInitialiser(WorkerPoolDescriptor *pool_descriptor)
{
  return DefaultUfsrvWorkerPoolOneoffInitialiser(pool_descriptor);
}

/**
 * @brief static type provider for ufsrv worker pool. One per server instance.
 */
static WorkerPoolDescriptor *const
sGetUfsrvWorkerPoolDescriptor(oneoff_initialiser on_created) {
  static WorkerPoolDescriptor ufsrvworker_pool;

  ufsrvworker_pool.on_created = on_created;

  return &ufsrvworker_pool;
}

static void
sInitUfsrvScheduledJobsStore(ScheduledJobs *scheduled_jobs, size_t count, void (*scheduled_jobs_startup_callback)(void))
{
	InitScheduledJobsStore(scheduled_jobs, count);
	if (IS_PRESENT(scheduled_jobs_startup_callback)) {
		scheduled_jobs_startup_callback();
	}
	syslog(LOG_INFO, "%s (pid:'%lu', o:'%p', page_sz:'%d', entries_per_page:'%d'): SUCCESS: Initialised ScheduledJobsStore", __func__, pthread_self(), scheduled_jobs, MinHeapPageSize(), MinHeapEntriesPerPage());
}
/**
 * @brief Callback initialiser for a UfsrvWorker thread, applicable to ufsrvwebsock and ufsrvapi class servers.
 * @note This must be called in the context of a running thread, because it initialises thread local ThreadContext object.
 * @param thread_base_ctx_data pre-allocated context
 */
static UFSRVResult *
sUfsrvWorkerThreadDataContextInitialiser(BaseThreadContext *thread_base_ctx_data)
{
  ThreadContext *ufsrv_thread_context_ptr = thread_base_ctx_data->worker_thread_context;

  HopscotchHashtableConfigurable  *locked_objects_store = &(thread_base_ctx_data->locked_objects_store);
  if (InitLockedObjectsStore(locked_objects_store) != 0) {
    syslog(LOG_ERR, "%s: ERROR: COULD NOT INITIALISE the locked-objects store for Ufsrv Worker thread: '%lu'...", __func__, pthread_self());
    _exit(-1);
  }

  ufsrv_thread_context_ptr->ht_ptr = locked_objects_store;
  ufsrv_thread_context.ht_ptr = locked_objects_store;//TBD

  sInitUfsrvScheduledJobsStore(&thread_base_ctx_data->scheduled_jobs_store, 0, NULL);

  ufsrv_thread_context_ptr->res_ptr = &(thread_base_ctx_data->ufsrv_result);
  ufsrv_thread_context.res_ptr = &(thread_base_ctx_data->ufsrv_result);//TBD

  //>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>

  if (IS_PRESENT(InitialiseHttpRequestContext(&(thread_base_ctx_data->http_request_context), 0))) {
    ufsrv_thread_context_ptr->http_request_context = &(thread_base_ctx_data->http_request_context);;
    ufsrv_thread_context.http_request_context = &(thread_base_ctx_data->http_request_context);//TBD
  } else {
    syslog(LOG_ERR, "%s: ERROR: COULD NOT INITIALISE HttpRequestContext for Ufsrv Worker thread: '%lu'...", __func__, pthread_self());
    _exit(-1);
  }

  syslog(LOG_DEBUG, "%s: SUCCESS (http_ptr:'%p'): Initialised HttpRequestContext for Ufsrv Worker thread: '%lu'...", __func__, &(thread_base_ctx_data->http_request_context), pthread_self());

  //>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>

  InstrumentationBackend *instr_ptr = InstrumentationBackendInit(NULL, NULL, NULL, 0);//no namespace
  if (instr_ptr) {
    ufsrv_thread_context_ptr->instrumentation_backend = instr_ptr;
    ufsrv_thread_context.instrumentation_backend = instr_ptr;//TBD
  } else {
    syslog(LOG_NOTICE, "%s: ERROR: COULD NOT INITIALISE INSTRUMENTATION for Ufsrv Worker thread: '%lu'...", __func__, pthread_self());
  }

  syslog(LOG_INFO, "%s: SUCCESS (instr_ptr:'%p'): Initialised Instrumentation Backend for Ufsrv Worker thread: '%lu' (NOT IMPLEMENTED)...", __func__, instr_ptr, pthread_self());

  //>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>

  THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, RECODE_NONE)
}

/**
 * @brief Initialise and launch the job workers subsystem (one per instance).
 * Each thread gets its own ThreadContext object, carved out of a slap allocator fixed for the size of thread pool workers
 * set-size.
 */
static void
sLaunchUfsrvWorkers()
{
  WorkerPoolDescriptor *const pool_descriptor = sGetUfsrvWorkerPoolDescriptor(sUfsrvWorkerPoolOneoffInitialiser);
  size_t pool_sz = UfsrvConfigUfsrvWorkersThreadPool();
  pool_descriptor->workers_pool_config_descriptor.up_status = POOL_STATE_UP;
  pool_descriptor->workers_pool_config_descriptor.pool_sz = pool_sz;
//  pool_descriptor->sessions_delegator = sd_ptr;
  pool_descriptor->thread_handlers.on_instantiated = sUfsrvWorkerThreadDataContextInitialiser;
  RegisterJobWorkersConfigurationDescriptor(&pool_descriptor->workers_pool_config_descriptor);

  if (IS_PRESENT(pool_descriptor->on_created)) {
    pool_descriptor->on_created(pool_descriptor);
  }

  LaunchUfServerWorkerThreads(pool_descriptor, sizeof(ThreadContext));
}

static void
sLaunchTimerManagerThread(void (*scheduled_jobs_startup_callback)(void))
{
	sInitUfsrvScheduledJobsStore(GetScheduledJobsStore(), 0, scheduled_jobs_startup_callback);

	int result = pthread_create(UfsrvConfigThreadHandleTimerDelegatorAsPtr(), NULL, ThreadTimerManager, (void *)GetScheduledJobsStore());
	if (result != 0) {
		char error_str[MEDIUMBUF] = {0};
		strerror_r(errno, error_str, MEDIUMBUF);
		syslog(LOG_ERR, "InitialiseWorkerDelegator: TERMINATING (errno: '%d'): COULD NOT INITIALISE Timer Manager(TM) thread: '%s'...", errno, error_str);

		exit(-1);
	}

	syslog(LOG_INFO, ">> %s: SUCCESSFULLY Initialised TimerManager...'", __func__);
}

UFSRVResult *
proto_sandbox_protocol_init_callback(const Protocol *proto_ptr)
{
  syslog(LOG_INFO, "%s: Initialising protocol: '%s' ...", __func__, proto_ptr->protocol_name);

  RecyclerPoolHandle *pool = InitSessionRecyclerTypePool(
      GetSessionInstantiator(), UfsrvConfigMemorySpecsAllocationGroups(),
      UfsrvConfigMemorySpecsSessionsPerAllocationGroup());
  if (!ProtocolSetSessionPool(proto_ptr, pool)) exit(EXIT_FAILURE);

  UfsrvDelegatorWorkersUserData worker_data = {0};
  if (!InitialiseUfsrvDelegatorWorkersUserData(&worker_data)) {
    syslog(LOG_ERR, "%s: Could not initialise delegator worker backends", __func__);
    exit(EXIT_FAILURE);
  }

  SessionsDelegatorInitSpecs delegator_specs = {
      .ufsrv_delegator_userdata = &worker_data,
      .on_init_crypto = InitialiseUfsrvCrypto,
      .on_init_main_thread_backends = InitialiseUfsrvMainThreadBackends,
      .on_init_hashtable      = SessionDelegatorInitHashTables,
      .build_msgqueue_backend = BuildConnectionHandleForMessageQueueBackend,
  };

  SessionsDelegator *sd_ptr = InitialiseDelegator(&delegator_specs);
  if (!sd_ptr) {
    DestroyUfsrvDelegatorWorkersUserData(&worker_data);
    syslog(LOG_ERR, "%s: Could not initialise delegator", __func__);
    exit(EXIT_FAILURE);
  }

  InitUFSRV((SessionsDelegator *)sd_ptr, &delegator_specs);
  LaunchSessionsDelegatorThread(sd_ptr);

  sLaunchUfsrvWorkers();
	sLaunchTimerManagerThread(InitialiseScheduledJobTypeForSessionsTimeouts);//ensure RegisterJobWorkersConfigurationDescriptor() is called beforehand

	return NULL;
}

//TODO: this may need to be phased out
UFSRVResult *
proto_sandbox_init_workers_delegator_callback(void)
{
	//CreateSessionsDelegatorThread ();

	return _ufsrv_result_generic_success;
}

UFSRVResult *
proto_sandbox_hanshake_callback(InstanceHolder *instance_sesn_ptr, SocketMessage *sock_msg_ptr, unsigned callflags, int **comeback)
{
  Session *sesn_ptr     = SessionOffInstanceHolder(instance_sesn_ptr);
	_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROTOCOL_WSHANDSHAKE)

}

/*
 * 	@brief: Session has just been successfully handshaked both ways and authenticated. This session could be brand new, migrated, unsuspended etc...
 */
UFSRVResult *
proto_sandbox_post_hanshake_callback(InstanceHolder *instance_sesn_ptr, SocketMessage *sock_msg_ptr, unsigned callflags)
{
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

  // UfsrvCommandInvokeUserCommand(&(InstanceContextForSession) {instance_sesn_ptr, sesn_ptr}, NULL, NULL, NULL, NULL, uSTATESYNC_V1_IDX);

	_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_NULL_POINTER)
 
}

//TODO: we kind of mirroring what MarshalServiceCommandToClient is doing. We should consolidate at some stage

//TODO: frame_offset and len parameters are no longer applicable. frame_offset should be used as callflags by the calling environment
UFSRVResult *
proto_sandbox_msg_callback(InstanceHolder *instance_sesn_ptr, SocketMessage *sock_msg_ptr, unsigned frame_offset, size_t len)
{
	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

	ssize_t read_result = ReadFromSocket(instance_sesn_ptr, sock_msg_ptr, frame_offset);//frame_offset is 'callflags' passed by the calling environment

	if (read_result > 0)	goto process_framed_decoded_msg;
	else if (read_result == 0) {//if we are reading a very large frame, the first fragment will be seen by decode_hybi, which will return 0
		//subsequent reads will detect missing size and will continue to report zero until  full frame is recieved up to 65k which is the max frame zize we allowe for Websocket
			syslog(LOG_DEBUG, "%s (pid:'%lu' cid:'%lu'): COULD NOT FIND A COMPLETE FRAME: NO MSG WILL BE PROCESSED: (frame_coundt='%lu' missing_msg_size: '%lu') RETURNING...", __func__, pthread_self(), SESSION_ID(sesn_ptr), sock_msg_ptr->frame_count, sock_msg_ptr->missing_msg_size);

		_RETURN_RESULT_SESN(sesn_ptr, sesn_ptr, RESULT_TYPE_SUCCESS, RESCODE_IO_FRAGMENTATION)
	} else {
		read_error:
		switch (read_result)
		{
		case -1:
			_RETURN_RESULT_SESN(sesn_ptr, sesn_ptr, RESULT_TYPE_ERR, RESCODE_IO_CONNECTIONCLOSED)//suspended

		case -2:
			_RETURN_RESULT_SESN(sesn_ptr, sesn_ptr, RESULT_TYPE_ERR, RESCODE_LOGIC_CANTLOCK)

		case -3://user sent termination in WS
			_RETURN_RESULT_SESN(sesn_ptr, sesn_ptr, RESULT_TYPE_ERR, RESCODE_IO_CONNECTIONCLOSED)

		case -4:
			_RETURN_RESULT_SESN(sesn_ptr, sesn_ptr, RESULT_TYPE_ERR, RESCODE_IO_DECODED)//this fatal couldent base64 decode: suspend

		case -5:
			_RETURN_RESULT_SESN(sesn_ptr, sesn_ptr, RESULT_TYPE_ERR, RESCODE_IO_MISSGINGFRAMEDATA)//benign error couldn't process frame because of incomplete frame data

		default:
			_RETURN_RESULT_SESN(sesn_ptr, NULL,  RESULT_TYPE_ERR, RESCODE_PROG_NULL_POINTER)
		}
	}

	process_framed_decoded_msg:

	//TODO: this may be redundant as read_result==0 condition above should indicate the same condition
	if (sock_msg_ptr->frame_count == 0) {
		syslog(LOG_DEBUG, "%s (pid:'%lu' o:'%p', frameindex_multiples:'%lu'): NO FRAME WAS FOUND: NO MSG WILL BE PROCESSED: (missing_msg_size: '%lu') RETURNING...", __func__, pthread_self(), sesn_ptr, sock_msg_ptr->frame_index_multiples, sock_msg_ptr->missing_msg_size);

		//TODO: FIX: this should be considered an error condition?
		_RETURN_RESULT_SESN(sesn_ptr, sesn_ptr, RESULT_TYPE_SUCCESS, RESCODE_IO_FRAGMENTATION)
	}

	_RETURN_RESULT_SESN(sesn_ptr, sesn_ptr, RESULT_TYPE_SUCCESS, RESCODE_IO_MSGPARSED)

}

UFSRVResult *
proto_sandbox_decode_msg_callback(InstanceHolder *instance_sesn_ptr, SocketMessage *sm_ptr, unsigned frame_offset)
{
	_RETURN_RESULT_SESN(SessionOffInstanceHolder(instance_sesn_ptr), NULL, RESULT_TYPE_SUCCESS, RESCODE_IO_ENCODED)
}

UFSRVResult *
proto_sandbox_encode_msg_callback(InstanceHolderForSession *instance_sesn_ptr, SocketMessage *sock_msg_ptr, unsigned frame_offset)
{

		_RETURN_RESULT_SESN(SessionOffInstanceHolder(instance_sesn_ptr), NULL, RESULT_TYPE_SUCCESS, RESCODE_IO_ENCODED)

}


/**
 * @brief Lifecycle callback for when a new session object is memory allocated. This is the pure data object, no instanceHolder
 *
 * @param call_flag: signifies whether the callback is being invoked from the context of the recycler. if set indicates the object is being initialised from a recycler GET request; ie recurring initiallisation. Otherwise indicates
 * object is being initialised at memory allocation time; ie once-off
 * setup
 *
 */
UFSRVResult *
proto_sandbox_init_session_callback(ClientContextData *ctx_data_ptr, unsigned call_flags)
{
  Session *sesn_ptr = (Session *)ctx_data_ptr;

  //TODO: at the moment the session object needs to be recreated regardless of recycler origin. Future optimisation

  _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROG_NULL_POINTER)
}

/**
 *
 * @param instance_sesn_ptr
 * @param call_flags signifies whether the callback is being invoked from the context of the recycler. If set indicates the object is being initialised from a recycler GET request; ie recurring initiallisation. Otherwise indicates
 * object is being initialised at memory allocation time; ie once-off
 * @return
 */
UFSRVResult *
proto_sandbox_reset_callback(InstanceHolder *instance_sesn_ptr, unsigned call_flags)
{
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

	_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_NOOP, RESCODE_PROG_NULL_POINTER)

}

#define BASELINE_REFCOUNT	3

/**
 * 	@brief: invoked everytime service timeout check is performed. This is invoked outside the normal Session worky context, hence
 * 	why ephemeral mode is loaded for the session.
 *
 * 	@param sens_ptr: this is a locally hashed session, but not necessarily connected. It will have ephemeral mode loaded.
 *
 * 	@locked sesn_ptr: locked by the caller unless the flag SESN_WONTLOCK is passed
 *
 * 	@worker: UfsrvWorker
 * 	@access_context sesn_ptr: loaded in ephemeral mode
 *
 * 	@returns on returning error, session will be suspended by the caller
 */
UFSRVResult *
proto_sandbox_service_timeout_callback(InstanceHolder *instance_sesn_ptr, time_t now, unsigned long call_flags)
{
	bool recycle_flag = false;
	bool suspended_flag = false;
	Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

	_return_noop:
	_RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_NOOP, RESCODE_PROG_NULL_POINTER)

}

UFSRVResult *
proto_sandbox_error_callback(InstanceHolder *instance_sesn_ptr, unsigned call_flags)
{
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

  _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_NOOP, RESCODE_PROG_NULL_POINTER)

}

UFSRVResult *
proto_sandbox_recycler_error_callback(InstanceHolder *instance_sesn_ptr, unsigned call_flags)
{
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

  _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_NOOP, RESCODE_PROG_NULL_POINTER)

}

UFSRVResult *
proto_sandbox_close_callback(InstanceHolder *instance_sesn_ptr)
{
  Session *sesn_ptr = SessionOffInstanceHolder(instance_sesn_ptr);

  _RETURN_RESULT_SESN(sesn_ptr, NULL, RESULT_TYPE_NOOP, RESCODE_PROG_NULL_POINTER)
}

UFSRVResult *
proto_sandbox_msgqueue_topics_callback(UFSRVResult *res_ptr)
{
	_RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_SUCCESS, RESCODE_PROTOCOL_DATA)

}

UFSRVResult *
proto_sandbox_generate_session_id_callback(UFSRVResult *res_ptr, ClientContextData *context_data)
{
  unsigned long session_id = GenerateSessionIdGlobally();
  if (session_id == 0) {
    _RETURN_RESULT_RES(res_ptr, NULL, RESULT_TYPE_ERR, RESCODE_PROTOCOL_DATA)
  } else {
    _RETURN_RESULT_RES(res_ptr, (uintptr_t)(unsigned long)session_id, RESULT_TYPE_SUCCESS, RESCODE_PROTOCOL_DATA)
  }
}