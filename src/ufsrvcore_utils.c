/**
 * @file ufsrvcore_utils.c
 * @brief ufsrvcore_utils
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


#include <syslog.h>

#include <ufsrvcorelib_defs.h>
#include <adt/adt_minheap.h>
#include <uflib/scheduled_jobs/scheduled_jobs.h>
#include <uflib/utils_time.h>
#include <uflib/adt/adt_hashtable.h>
#include <uflib/db/db_sql.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_utils.h>
#include <uflib/recycler/recycler.h>
#include <ufnetcorelib/ufnet_core/ufsrvcore_utils.h>

#include <ufsrv_core/delegator_sessions/sessions_delegator_type.h>

#include "config_defs_ufnetcorelib.h"
#include "ufnet_msgcore/session.h"
#include "ufsrvcorelib/ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h"

/**
 * @brief Instantiate the delegator's four session tables from descriptors.
 *
 * One descriptor per table, each handed to
 * SessionsDelegatorInitialiseHashtable(). Only the key differs between tables:
 * its offset within the composed Session, its width, and whether the key field
 * holds a pointer or the key inline.
 */
PUBLIC_API void
SessionDelegatorInitHashTables()
{
  UfsrvSessionSetSuspensionPublisher(PublishSuspendedSession);

  HashtableSpecs specs_sessions = {
      .table_name              = "SessionsHashTable",
      .fKeyOffset              = offsetof(Session, ufsrv_session.session_id),
      .fKeySize                = sizeof(unsigned long),
      .fKeyIsPtr               = HASH_ITEM_NOT_PTR_TYPE,
      .item_extractor_callback = RecyclerGetClientContextDataFromItemContainer,
  };
  SessionsDelegatorInitialiseHashtable(&(SessionsDelegatorPtr()->hashed_sessions.hashtable), &specs_sessions);

  /* the cookie is a char array, so the key is inline and NUL-terminated */
  HashtableSpecs specs_cookies = {
      .table_name              = "CookiesHashTable",
      .fKeyOffset              = offsetof(Session, ufsrv_session.session_cookie),
      .fKeySize                = KEY_SIZE_ZERO,
      .fKeyIsPtr               = HASH_ITEM_NOT_PTR_TYPE,
      .item_extractor_callback = RecyclerGetClientContextDataFromItemContainer,
  };
  SessionsDelegatorInitialiseHashtable(&(SessionsDelegatorPtr()->hashed_cookies.hashtable), &specs_cookies);

  HashtableSpecs specs_userids = {
      .table_name              = "UserIdHashTable",
      .fKeyOffset              = offsetof(Session, ufnet_session.sservice.user.user_details.user_id),
      .fKeySize                = sizeof(unsigned long),
      .fKeyIsPtr               = HASH_ITEM_NOT_PTR_TYPE,
      .item_extractor_callback = RecyclerGetClientContextDataFromItemContainer,
  };
  SessionsDelegatorInitialiseHashtable(&(SessionsDelegatorPtr()->hashed_userids.hashtable), &specs_userids);

  HashtableSpecs specs_usernames = {
      .table_name              = "UsernamesHashtable",
      .fKeyOffset              = offsetof(Session, ufnet_session.sservice.user.user_details.user_name),
      .fKeySize                = KEY_SIZE_ZERO,
      .fKeyIsPtr               = HASH_ITEM_IS_PTR_TYPE,
      .item_extractor_callback = RecyclerGetClientContextDataFromItemContainer,
  };
  SessionsDelegatorInitialiseHashtable(&(SessionsDelegatorPtr()->hashed_usernames.hashtable), &specs_usernames);
}

#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_secret.h>

char *
ProvideDbBackendPassword(void)
{
    char *db_password_encrypted = UfsrvConfigDbBackendPassword();
    char *plaintext             = NULL;
    if (SecretConfigDecrypt(db_password_encrypted, UfsrvConfigUfsrvSecretKeyAsPtr(), &plaintext) != 0 || !plaintext) {
        syslog(LOG_ERR, ">>> %s: FAILED to decrypt %s", __func__, db_password_encrypted);
        return NULL;
    }

    return plaintext;
}

static pthread_once_t mysql_client_once = PTHREAD_ONCE_INIT;

/**
 *  @brief Executed in a ufsrv thread context, so issuing mysql lib call unsigned long mysql_thread_id(MYSQL *mysql) returns
 *  client thread associated with this ufsrv thread. or "SELECT CONNECTION_ID();"
 */
struct _h_connection *
InitialiseDbBackend(void)
{
    pthread_once(&mysql_client_once, InitMysql);
    BackoffAlgorithmStatus_t  retry_status       = BackoffAlgorithmSuccess;
    BackoffAlgorithmContext_t retry_params       = {0};
    uint16_t                  next_retry_backoff = 0;
    BackoffAlgorithm_InitializeParamsExponential(&retry_params, EXPOBACKOFF_RETRY_BACKOFF_BASE_MS,
                                                 EXPOBACKOFF_RETRY_MAX_BACKOFF_DELAY_MS, EXPOBACKOFF_RETRY_MAX_ATTEMPTS);

    //ANNOTATE_IGNORE_READS_BEGIN();
    //__vdrd_AnnotateIgnoreReadsBegin();
    struct _h_connection *db_ptr = NULL;
    //ANNOTATE_IGNORE_READS_END();
    //__vdrd_AnnotateIgnoreReadsEnd();

    char *db_password = ProvideDbBackendPassword();
    do {
        db_ptr = h_connect_mariadb(UfsrvConfigDbBackendAddress(), UfsrvConfigDbBackendUsername(), db_password,
                                   CONFIG_DBBACKEND_DBNAME, UfsrvConfigDbBackendPort(), NULL);
        if (IS_PRESENT(db_ptr)) {
            SqlServerDisplayConnectedUsers(db_ptr);
            free(db_password);
            return db_ptr;
        }

        retry_status = BackoffAlgorithm_GetNextBackoff(&retry_params, 0, &next_retry_backoff);
        syslog(LOG_WARNING, "%s (pid: '%lu', current_attempt: %d'): RETRYING DB CONNECTION IN %u milliseconds....",
               __func__, pthread_self(), retry_params.attemptsDone, next_retry_backoff);
        ThreadSleepMs(next_retry_backoff);
    }
    while ((!IS_PRESENT(db_ptr)) && (retry_status != BackoffAlgorithmRetriesExhausted));

    free(db_password);
    return NULL;
}

__attribute__ ((pure)) ScheduledJobType *
GetScheduledJobTypeForSessionTimeout(void)
{
	static ScheduledJobType job_type_session_timeout={
			.type_name				=	"Session Timeout",
			.type_id					=	0,//gets assigned by type registry
			.frequency_mode		=	PERIODIC,
			.concurrency_mode	=	SINGLE_INSTANCE,
			.frequency				=	_CONFIGDEFAULT_SESSION_TIMEOUT_CHECK_FREQUENCY, //1min
			.callbacks					=	{
					.on_compare_keys	= (CallbackOnCompareKeys)TimeValueComparator,
					.on_error				=	NULL,
					.on_run					=	CheckSessionIdleTime,
					.on_get_time    = GetTimeNowInMicros
			}
	};

	return &job_type_session_timeout;

}

/**
 * 	@brief: Since this job type does not allow concurrent scheduling, ie one job of this type can ever exist in the scheduler
 * 	we can get away with allocating a single static reference.
 */
ScheduledJob *
GetScheduledJobForSessionTimeout(void)
{
	static ScheduledJob job_session_timeout;

	job_session_timeout.job_type_ptr = GetScheduledJobTypeForSessionTimeout();

	return &job_session_timeout;
}

void
InitialiseScheduledJobTypeForSessionsTimeouts(void)
{
  ScheduledJobs *jobs = GetScheduledJobsStore();
  RegisterScheduledJobType(jobs, GetScheduledJobTypeForSessionTimeout());
  InsertScheduledJob(jobs, GetScheduledJobForSessionTimeout());
}
