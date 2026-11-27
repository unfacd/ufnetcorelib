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

/*
**
** MODULEID("$Id: sredird.h,v 1.1 1999/09/01 00:42:00 ayman Exp $")
**
*/

#ifndef NPORTREDIRD_H
#define NPORTREDIRD_H

#include <stdbool.h>
#include <uflib/file_loader_type.h>
#include <uflib/file_loader_service_concurrent/file_loader_concurrent_service.h>
#include <ufsrvcorelib/ufsrv_core/service/file_loader/service_file_loader.h>

#include <ufsrv_core/delegator_sessions/sessions_delegator_type.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_utils.h>
#include <ufnetcorelib/ufnet_core/ufsrv_instance_descriptor_type.h>
#include <ufnetcorelib/ufnet_msgcore/cloud_authorization_token_type.h>
#include <ufsrv_core/delegator_sessions/ratelimit_handle_type.h>
#include <uflib/scheduled_jobs/scheduled_jobs_type.h>

#include <uflib/net/socket_type.h>
#include <ufnetcorelib/ufnet_core/session/session_type.h>

void InitUFSRV(SessionsDelegator *sd_ptr, SessionsDelegatorInitSpecs *spec);
void InitHTTPClient(void);
void InitSSL(void);
void RegisterUfsrvSessionsDelegator(SessionsDelegator *const sessions_delegator);
SessionsDelegator *const GetUfsrvSessionsDelegator(void);
UFSRVResult *const ProvideSuccessResult();
void LaunchTimerManagerThread(void (*scheduled_jobs_startup_callback)(void));
void LaunchFileloaderServiceThread(FileLoaderServiceDescriptor *descriptor);
void InitUfsrvScheduledJobsStore (ScheduledJobs *scheduled_jobs, size_t count, void (*scheduled_jobs_startup_callback)(void));
void InitialiseScheduledJobTypeForSessionsTimeouts(void);

void InitWorkersDelegator(int protocol_id);
UfsrvInstanceDescriptor *GetUfsrvInstance(Session *sesn_ptr, const char *server_class, unsigned geogroup, UfsrvInstanceDescriptor *instance_ptr_out);
bool UfsrvConfigRegisterUfserverActivityWithSession (Session *sesn_ptr, time_t activity_time);
size_t UfsrvConfigGetGeogroupSize(Session *sesn_ptr);
void RegisterUfsrvCloudAuthorizationTokenForFcm(CloudAuthorizationToken *cloud_authorization_token_ptr);
void RegisterUfsrvCloudAuthorizationTokenForIntegrityApi(CloudAuthorizationToken *cloud_authorization_token_ptr);
void InitialiseScheduledJobTypesForGpcAuthorization();

const char *UfsrvGetThisServerOrigin(void);
bool IsOriginAllowed(const char * _Nonnull origin);
size_t GetJobWorkersPoolSize(size_t default_sz);


PUBLIC_API bool UfsrvConfigRegisterUfsrverActivity(PersistenceBackend	*pers_ptr, time_t activity_time);
PUBLIC_API char *ProvideDbBackendPassword(void);

#endif

