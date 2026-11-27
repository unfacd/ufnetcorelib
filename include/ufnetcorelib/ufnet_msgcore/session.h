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

#ifndef SESSION_H
# define SESSION_H

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <uflib/utils_threads.h>
#include <ufsrv_core/session/sessions_utils.h>
#include <ufnet_core/session/session_type.h>
#include <ufsrv_core/delegator_sessions/sessions_delegator_type.h>
#include <ufnet_msgcore/user/user_backend.h>
#include <ufsrv_core/delegator_sessions/http_request_context_type.h>
#include <pthread.h>
#include <ufsrv_core/delegator_sessions/thread_context_type.h>
#include <ufsrv_core/system/thread_utils.h>
#include <hiredis/hiredis.h>
#include <uflib/scheduled_jobs/scheduled_jobs.h>
#include <uflib/ufsrvuid_type.h>
#include <uflib/recycler/recycler_type.h>
#include <ufnetcorelib/ufnet_msgcore/session_result_macros.h>

//profile_key not in use and can be reassigned
enum REDIS_UID_FIELDS {
REDIS_KEY_USER_SID=0, 					REDIS_KEY_USER_CID,
REDIS_KEY_USER_STATUS, 					REDIS_KEY_USER_UID,
REDIS_KEY_USER_USER_NAME, 			REDIS_KEY_USER_PROFILE_KEY,
REDIS_KEY_USER_HADDRESS, 				REDIS_KEY_USER_BASELOC, /* automatically calculated by backend */
REDIS_KEY_USER_WHEN,						REDIS_KEY_USER_WHEN_SERVICED,
REDIS_KEY_USER_WHEN_SUSPENDED, 	REDIS_KEY_USER_WHEN_RESUMED,
REDIS_KEY_USER_COOKIE, 					REDIS_KEY_USER_GEOGROUP,
REDIS_KEY_USER_EVENT_COUNTER,   REDIS_KEY_USER_NICKNAME,
REDIS_KEY_CURRENT_GEOFENCE, 		REDIS_KEY_PAST_GEOFENCE,
REDIS_KEY_LOCATION_USER,				REDIS_KEY_LOCATION_SERVER,
REDIS_KEY_HOME_BASELOC, /* set by user */					REDIS_KEY_USER_AVATAR,
REDIS_KEY_GUARDIAN_ID,
REDIS_KEY_MAX
};

#define REDIS_CMD_USER_SESSION_RECORD_SET_ALL	"HMSET UID:%lu cid %lu sid %d uid %b profile_key %b when %lu haddress %s baseloc %s status 1 when_serviced %lu when_suspended %lu when_resumed %lu uname %s cookie %s geogroup %d events_counter %lu nickname %s geofid_current %lu geofid_past %lu location_byu %s location_bys %s home_baseloc %s avatar %s guardian_uid %lu"

//#define REDIS_CMD_FULL_USER_SESSION_RECORD_GET	"HVALS UID:%lu"
//keep fields indexed inline with enum REDIS_UID_FIELDS
#define REDIS_CMD_USER_SESSION_RECORD_GET_ALL	"HMGET UID:%lu sid cid status uid uname profile_key haddress baseloc when when_serviced when_suspended when_resumed cookie geogroup events_counter nickname geofid_current geofid_past location_byu location_bys home_baseloc avatar guardian_uid"

#define REDIS_CMD_USER_SESSION_RECORD_DEL_ALL "DEL UID:%lu"

#define REDIS_CMD_USER_SESSION_SERVICE_TIMING_GET	"HMGET UID:%lu uid status when_serviced when_suspended"

#define REDIS_CMD_USER_GET_UFSRVUID	"HMGET UID:%lu uid"
#define REDIS_CMD_USER_SESSION_UFSRVUID_SET "HSET UID:%lu uid %b"

#define REDIS_CMD_USER_SESSION_STATUS_SET "HSET UID:%lu status %d"
#define REDIS_CMD_USER_SESSION_STATUS_SUSPEND "HMSET UID:%lu status 2 when_suspended %lu"
#define REDIS_CMD_USER_SESSION_PROFILE_KEY_SET "HSET UID:%lu profile_key %b"
#define REDIS_CMD_SESSION_PUBLISH_MSG "PUBLISH UFSRV:SESSION %s"

//%scookie %cid. Setting the same key again will override with new value cid:uid
#define REDIS_CMD_COOKIE_SESSION_SET "SET COOKIE:%s %lu:%lu"


//HSET USERNAME_TO_USERID <username> <userid>
#define REDIS_CMD_USERNAME_UID_SET "HSET USERNAME_TO_USERID %s %lu"
//#define REDIS_CMD_USERNAME_UID_DEL "HDEL USERNAME_TO_USERID %s"

//#define REDIS_CMD_USERNAME_USERID_MAPPING_SET "HSET USERNAME_TO_USERID %s %lu"
#define REDIS_CMD_USERNAME_USERID_MAPPING_GET "HGET USERNAME_TO_USERID %s"
#define REDIS_CMD_USERNAME_USERID_MAPPING_DEL "HDEL USERNAME_TO_USERID %s"

#define REDIS_CMD_COOKIE_SESSION_GET "GET COOKIE:%s"

#define REDIS_CMD_COOKIE_SESSION_DEL "DEL COOKIE:%s"

#define REDIS_CMD_SESSION_INC_EVENT_COUNTER	"HINCRBY UID:%lu events_counter 1"
#define REDIS_CMD_SESSION_EVENT_COUNTER_GET	"HGET BID:%lu events_counter"

//geo fences
#define REDIS_CMD_USER_GEOFIDS_GET	"HMGET UID:%lu geofid_current geofid_past"
#define REDIS_CMD_USER_GEOFIDS_SET "HMSET UID:%lu geofid_current %lu geofid_past %lu"

//GEOHASH <%long> <%lat> <%uid>:<0>: last colon is empty placeholder
//
#define REDIS_CMD_USER_GEOHASH_ADD "GEOADD USERS_GEO %f %f %lu:%d:"

#define REDIS_CMD_USER_LOCATION_SET	"HMSET UID:%lu %s %s"
//-------------


//<uid> <bitoffset:[0...n>0]> <value:0|1>
//in redis offset 0 starts at the right most end
//refere to enum UserPrefsOffsets "user_type.h", which provides user-friendly name inline with actual offsets
//also, refer to macro SESSION_USERPREF_ONOFF(x, y, z)
#define REDIS_CMD_USERPREF_ONOFF 			"SETBIT USER_PREFS_ONOFF:%lu %d %d"
#define REDIS_CMD_USERPREF_ONOFF_ON 	"SETBIT USER_PREFS_ONOFF:%lu %d 1"
#define REDIS_CMD_USERPREF_ONOFF_OFF 	"SETBIT USER_PREFS_ONOFF:%lu %d 0"
#define REDIS_CMD_USERPREF_ONOFF_GET 	"GETBIT USER_PREFS_ONOFF:%lu %d"
#define REDIS_CMD_USERPREF_GETALL			"GETRANGE USER_PREFS_ONOFF:%lu 0 -1"
#define REDIS_CMD_USERPREF_REMALL			"DEL USER_PREFS_ONOFF:%lu"
#define REDIS_CMD_USERPREF_GETRANGE		"GETRANGE USER_PREFS_ONOFF:%lu %d %d"
//<uid> <byte-offset> <vlue>
#define REDIS_CMD_USERPREF_SETRANGE		"SETRANGE USER_PREFS_ONOFF:%lu %d %s"

enum {//protocol types
  PROTO_TYPE_WEBSOCKET=0, PROTO_TYPE_RAW
};

#define HARD_SUSPENSE 1

Session *GetUfsrvSystemUser(void);
Session * CloneUfsrvSystemUser(InstanceHolderForSession *holder);
void ResetClonedUfsrvSystemUser(Session *sesn_ptr, unsigned long sesn_call_flags);
void *FetchRecycledObject(void);

unsigned long GenerateSessionIdGlobally(void);

Session *SessionGet(int protocol_id, unsigned call_flas);
Session *InstantiateSessionObject(Socket *ssptr, Socket *dsptr, unsigned call_flags, int protocol_id);
type_instantiator GetSessionInstantiator();
int InstantiateSessionObject2(ContextData **sesn_ptr_in, unsigned long call_flags, int protocol_id);
InstanceHolderForSession *InstantiateCarrierSession(InstanceHolderForSession *sesn_ptr_out, enum WorkerType worker, unsigned long flags, const Protocol *protocol);
int SuspendSession(InstanceHolderForSession *, unsigned);
void PublishSuspendedSession(InstanceHolderForSession *);

UfsrvUid *GetUfsrvUid(unsigned long user_id, UfsrvUid *uid_ptr_out, bool flag_by_ref, bool *flag_is_local);
InstanceHolderForSession *LocallyLocateSessionByUserId(unsigned long user_id);
InstanceHolderForSession *LocallyLocateSessionByUfsrvUid(const UfsrvUid *ui_ptr);
InstanceHolderForSession *LocallyLocateSessionByUsername(const char *username);
InstanceHolderForSession *LocallyLocateSessionById(unsigned long);

int CheckSessionIdleTime(ScheduledJob *, void *);
bool IsCheckSessionIdelTimeRunning();

UFSRVResult *GetSessionForThisUser(Session *sesn_ptr, const char *username, bool *lock_state, unsigned long call_flags);
UFSRVResult *GetSessionForThisUserByUserId(Session *sesn_ptr, unsigned long uid, bool *lock_state, unsigned long call_flags_sesn);
InstanceHolderForSession *SessionInstantiateFromCacheBackendWithDbFallback(Session *sesn_ptr_this, const UfsrvUid *ufsrvuid, unsigned long sesn_call_flags, const Protocol *protocol);
UFSRVResult *CacheBackendGetRawSessionRecordByCookie(const char *session_cookie, unsigned long call_flags);
InstanceHolderForSession *CacheBackendInstantiateRawSessionRecord(Session *sesn_ptr_this, redisReply *redis_ptr, unsigned long sesn_call_flags, InstanceHolderForSession *instance_sesn_ptr_out);
InstanceHolder *SessionLightlyInstantiateFromBackendRaw(Session *sesn_ptr_this, InstanceHolder *, redisReply *redis_ptr, unsigned long sesn_call_flags);
Session *InstantiateFromBackendRaw(Session *sesn_ptr_this, Session *sesn_ptr_in, redisReply *redis_ptr, unsigned long sesn_call_flags);
UFSRVResult *CacheBackendGetRawSessionRecordByUserId(unsigned long user_id, unsigned long call_flags, UFSRVResult *res_ptr_in);
UfsrvUid *CacheBackendInstateUfsrvUid(unsigned long user_id, unsigned long sesn_call_flags, UfsrvUid *uid_ptr_out);

UFSRVResult *IsSessionLocalAndCompareStatus(unsigned long session_id, unsigned status, unsigned unlock_flag, UFSRVResult *res_ptr);

UFSRVResult *DbBackendInstantiateSession(Session *sesn_ptr_transient, const UfsrvUid *ufrsvuid);
void TransferBasicSessionDbBackendData(Session *sesn_ptr, AuthenticatedAccount *authacct_ptr);
void TransferBasicSessionDbBackendDataFromSession(Session *sesn_ptr_target, Session *sesn_ptr_source);
int ClearLocalSessionCache(InstanceHolderForSession *instance_sesn_ptr_target, unsigned long sesn_call_flags);
void ResetSessionData(InstanceHolderForSession *instance_sesn_ptr_target);
UFSRVResult *RefreshBackendCacheForSession(Session *sesn_ptr, const char *old_cookie, unsigned long sesn_call_flags);
UFSRVResult *CacheBackendUpdateCookie(Session *sesn_ptr, const char *old_cookie);

int SessionLoadEphemeralMode(Session *sesn_ptr) __attribute__((deprecated("NO BACKEND HANDLES LOADED THIS IS NOOP")));
int SessionUnLoadEphemeralMode(Session *sesn_ptr) __attribute__((deprecated("NO BACKEND HANDLES UNLOADED THIS IS NOOP")));

void ThreadContextTransferAccessContextForSession(Session *sesn_ptr) __attribute__((deprecated("NO BACKEND HANDLES TRANSFERED THIS IS NOOP")));
void ThreadContextResetAccessContextForSession(Session *sesn_ptr) __attribute__((deprecated("NO BACKEND HANDLES UNTRANSFERED THIS IS NOOP")));

UFSRVResult *RegisterUfsrvEvent(Session *sesn_ptr, EnumEventType event_type, unsigned event_instance_type, void *event_payload, UfsrvEvent *event_ptr_out);

/**
 * @brief Allocate the next sequential event id for the user behind @p sesn_ptr.
 *
 * Increments the user's @c events_counter and returns the new value, without creating an event
 * record. Use it when only the id is needed, e.g. to stamp a client-bound command that is staged
 * for delivery before it is sent.
 *
 * @param sesn_ptr Session whose user receives the id.
 *
 * @return The new eid, or zero when the session is missing or the backend did not answer.
 *
 * @locked sesn_ptr
 */
PUBLIC_API unsigned long GenerateUserEventId(Session *sesn_ptr);

HttpRequestContext *GetHttpRequestContext(Session *sesn_ptr);
HttpRequestContext *GetHttpRequestContextUfsrvWorker(Session *sesn_ptr);

ScheduledJobType *GetScheduledJobTypeForSessionTimeout(void);
ScheduledJob *GetScheduledJobForSessionTimeout(void);

UFSRVResult *DbBackendBootstrapIntoBrandNewSession(Session *sesn_ptr_transient, unsigned long userid_provided, bool flag_auth_data_loaded);

  static inline bool
  IsSessionConnected(Session *sesn_ptr, time_t time_now)
  {
    return ((SESNSTATUS_IS_SET(sesn_ptr->ufsrv_session.stat, SESNSTATUS_CONNECTED)) && (time_now-sesn_ptr->ufsrv_session.when_serviced_end>_CONFIGDEFAULT_IDLETIME_THRESHOLD));
  }

  /* @return: the state is returned on the thread's own UFSRvresul*/
  

  

  

//----------------------------

  

  

  static inline bool IsSessionListInitialisedFences(Session *sesn_ptr) {
    return SESSION_LISTS_INIT_STATE_FENCES(sesn_ptr);
  }

  static inline bool IsSessionListInitialisedProfile(Session *sesn_ptr) {
    return SESSION_LISTS_INIT_STATE_PROFILE(sesn_ptr);
  }

  static inline bool IsSessionListInitialisedLocation(Session *sesn_ptr) {
    return SESSION_LISTS_INIT_STATE_LOCATION(sesn_ptr);
  }

  static inline bool IsSessionListInitialisedNetstate(Session *sesn_ptr) {
    return SESSION_LISTS_INIT_STATE_NETSTATE(sesn_ptr);
  }

  static inline bool IsSessionListInitialisedContacts(Session *sesn_ptr) {
    return SESSION_LISTS_INIT_STATE_CONTACTS(sesn_ptr);
  }

#endif