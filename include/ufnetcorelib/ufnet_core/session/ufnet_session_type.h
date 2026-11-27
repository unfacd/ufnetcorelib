/**
 * @file ufnet_session_type.h
 * @brief UF-network (messaging domain) partition of the ufsrv session.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef UFNETCORELIB_UFNET_SESSION_TYPE_H
#define UFNETCORELIB_UFNET_SESSION_TYPE_H

#include <stdbool.h>
#include <stddef.h>
#include <ufsrvcorelib/ufsrv_core/session/ufsrv_session_type.h>
#include <ufsrvcorelib/ufsrv_core/http/http_session.h>
#include <ufnet_msgcore/session_service_type.h>
#include <ufnetcorelib/ufnet_msgcore/session_result_macros.h>

/*! UF-network domain partition of the ufsrv session.
 *
 *  Carries the user model: identity, account status, preferences, share
 *  lists, location, fence lists, geo-fence holders, the per-session result
 *  carrier and the per-list initialisation bits.
 *
 *  In the composed session this view starts at offsetof(Session, ufnet_session)
 *  and spans exactly the members that follow it:
 *
 *    AS_UFNET_SESSION(sesn_ptr)  ==  &sesn_ptr->ufnet_session
 *
 *  Its `sservice` member maps onto Session.sservice and its
 *  `lists_init_state` member maps onto Session.lists_init_state. Both
 *  members remain in the original Session definition until the composition
 *  lands, so the existing sesn->ufnet_session.sservice references keep resolving.
 */
#define AS_PROTOCOL_SESSION_DATA(x)	((ProtocolSessionData *)(x))

/*! Authentication failure cause carried out of session routing. */
enum SessionState {
  INVALID_COOKIE = 1,
  INVALID_CREDENTIALS,
  INVALID_USERNAME,
  INVALID_PASSWORD
};

typedef struct UfnetSession {
  SessionService sservice;        ///< User identity, preferences, share lists, fence lists, result carrier

  /*! Per-share-list initialisation tracking. Aligns with enum EnumShareListType. */
  struct {
    bool fences:1,
         profile:1,
         location:1,
         netstate:1,
         contacts:1,
         blocked:1,
         read_receipt:1,
         activity_state:1,
         blocked_fence:1;
  } lists_init_state;
} UfnetSession;

typedef struct Session
{
  UfsrvSession ufsrv_session;
  UfnetSession ufnet_session;
} Session;


#if defined(__cplusplus)
# define UFSRV_SESSION_ASSERT(c, m) static_assert(c, m)
# define UFSRV_SESSION_ALIGNOF(t)   alignof(t)
#else
# define UFSRV_SESSION_ASSERT(c, m) _Static_assert(c, m)
# define UFSRV_SESSION_ALIGNOF(t)   _Alignof(t)
#endif

#define AS_SESSION(x)	((Session *)(x))
#define AS_UFNET_SESSION(x)	(&((Session *)(x))->ufnet_session)

/* Hash tables index these fields by byte offset from the Session pointer
 * (the extractor returns GetInstance(holder)). These assertions pin the
 * composed offsets; a layout change that breaks a key offset fails the build
 * instead of corrupting a table. */
UFSRV_SESSION_ASSERT(offsetof(Session, ufsrv_session.session_id)
                     == offsetof(UfsrvSession, session_id),
                     "sessions hash key offset drifted");
UFSRV_SESSION_ASSERT(offsetof(Session, ufsrv_session.session_cookie)
                     == offsetof(UfsrvSession, session_cookie),
                     "cookies hash key offset drifted");
UFSRV_SESSION_ASSERT(offsetof(Session, ufnet_session.sservice.user.user_details.user_id)
                     == offsetof(Session, ufnet_session)
                      + offsetof(UfnetSession, sservice.user.user_details.user_id),
                     "userids hash key offset drifted");
UFSRV_SESSION_ASSERT(offsetof(Session, ufnet_session.sservice.user.user_details.user_name)
                     == offsetof(Session, ufnet_session)
                      + offsetof(UfnetSession, sservice.user.user_details.user_name),
                     "usernames hash key offset drifted");
UFSRV_SESSION_ASSERT(offsetof(Session, ufnet_session) >= sizeof(UfsrvSession),
                     "domain partition overlaps the base partition");
UFSRV_SESSION_ASSERT(offsetof(Session, ufnet_session) % UFSRV_SESSION_ALIGNOF(UfnetSession) == 0,
                     "domain partition is not aligned for UfnetSession");
UFSRV_SESSION_ASSERT(sizeof(Session) >= offsetof(Session, ufnet_session) + sizeof(UfnetSession),
                     "Session cannot hold both partitions");
UFSRV_SESSION_ASSERT(UFSRV_SESSION_ALIGNOF(UfnetSession) <= UFSRV_SESSION_ALIGNOF(Session),
                     "Session alignment weaker than its domain member");

/* Access the core partition of a composed Session. */
#define SESSION_ID(x) UFSRV_SESSION_ID(&(x)->ufsrv_session)
#define SESSION_EID(x) UFSRV_SESSION_EID(&(x)->ufsrv_session)
#define SESSION_STATUS(x) UFSRV_SESSION_STATUS(&(x)->ufsrv_session)
#define SESSION_DEVICEID(x) UFSRV_SESSION_DEVICEID(&(x)->ufsrv_session)
#define SESSION_COOKIE(x) UFSRV_SESSION_COOKIE(&(x)->ufsrv_session)
#define SESSION_WHEN_SUSPENDED(x) UFSRV_SESSION_WHEN_SUSPENDED(&(x)->ufsrv_session)
#define SESSION_WHEN_SERVICE_STARTED(x) UFSRV_SESSION_WHEN_SERVICE_STARTED(&(x)->ufsrv_session)
#define SESSION_WHEN_SERVICED(x) UFSRV_SESSION_WHEN_SERVICED(&(x)->ufsrv_session)
#define SESSION_HADDRESS(x) UFSRV_SESSION_HADDRESS(&(x)->ufsrv_session)
#define SESSION_HPORT(x) UFSRV_SESSION_HPORT(&(x)->ufsrv_session)
#define SESSION_PROTOCOL_SESSION_DATA(x) UFSRV_SESSION_PROTOCOL_SESSION_DATA(&(x)->ufsrv_session)
#define SESSION_PROTOCOLTYPE(x) UFSRV_SESSION_PROTOCOLTYPE(&(x)->ufsrv_session)
#define SESSION_CUMMULATIVE_TR(x) UFSRV_SESSION_CUMMULATIVE_TR(&(x)->ufsrv_session)
#define SESSION_CUMMULATIVE_RC(x) UFSRV_SESSION_CUMMULATIVE_RC(&(x)->ufsrv_session)
#define SESSION_INCREMENT_TR(x, y) UFSRV_SESSION_INCREMENT_TR(&(x)->ufsrv_session, (y))
#define SESSION_INCREMENT_RC(x, y) UFSRV_SESSION_INCREMENT_RC(&(x)->ufsrv_session, (y))
#define SESSION_CMTOKEN(x) UFSRV_SESSION_CMTOKEN(&(x)->ufsrv_session)
#define SESSION_NETADDRESS_HASH(x) UFSRV_SESSION_NETADDRESS_HASH(&(x)->ufsrv_session)
#define SESSION_UFSRV_GEOGROUP(x) UFSRV_SESSION_GEOGROUP(&(x)->ufsrv_session)
#define SESSION_SOCKET(x) UFSRV_SESSION_SOCKET(&(x)->ufsrv_session)
#define SESSION_SOCKETFD(x) UFSRV_SESSION_SOCKETFD(&(x)->ufsrv_session)
#define SESSION_SOCKETBLOCKSZ(x) UFSRV_SESSION_SOCKETBLOCKSZ(&(x)->ufsrv_session)
#define SESSION_INSOCKMSG_TRANS(x) UFSRV_SESSION_INSOCKMSG_TRANS(&(x)->ufsrv_session)
#define SESSION_INSOCKMSG_TRANS_PTR(x) UFSRV_SESSION_INSOCKMSG_TRANS_PTR(&(x)->ufsrv_session)
#define SESSION_INSOCKMSG_TRANS_EMPTY(x) UFSRV_SESSION_INSOCKMSG_TRANS_EMPTY(&(x)->ufsrv_session)
#define SESSION_OUTSOCKMSG_TRANS(x) UFSRV_SESSION_OUTSOCKMSG_TRANS(&(x)->ufsrv_session)
#define SESSION_OUTSOCKMSG_TRANS_PTR(x) UFSRV_SESSION_OUTSOCKMSG_TRANS_PTR(&(x)->ufsrv_session)
#define SESSION_INSOCKMSG(x) UFSRV_SESSION_INSOCKMSG(&(x)->ufsrv_session)
#define SESSION_INSOCKMSG_PTR(x) UFSRV_SESSION_INSOCKMSG_PTR(&(x)->ufsrv_session)
#define SESSION_INSOCKMSG_QUEUE(x) UFSRV_SESSION_INSOCKMSG_QUEUE(&(x)->ufsrv_session)
#define SESSION_INSOCKMSG_QUEUE_SIZE(x) UFSRV_SESSION_INSOCKMSG_QUEUE_SIZE(&(x)->ufsrv_session)
#define SESSION_INSOCKMSG_QUEUE_PTR(x) UFSRV_SESSION_INSOCKMSG_QUEUE_PTR(&(x)->ufsrv_session)
#define SESSION_EVENT_HANDLER_ON_READ(x) UFSRV_SESSION_EVENT_HANDLER_ON_READ(&(x)->ufsrv_session)
#define SESSION_EVENT_HANDLER_ON_WRITE(x) UFSRV_SESSION_EVENT_HANDLER_ON_WRITE(&(x)->ufsrv_session)
#define SESSION_OUTSOCKMSG(x) UFSRV_SESSION_OUTSOCKMSG(&(x)->ufsrv_session)
#define SESSION_OUTSOCKMSG_PTR(x) UFSRV_SESSION_OUTSOCKMSG_PTR(&(x)->ufsrv_session)
#define SESSION_OUTSOCKMSG_QUEUE(x) UFSRV_SESSION_OUTSOCKMSG_QUEUE(&(x)->ufsrv_session)
#define SESSION_OUTSOCKMSG_QUEUE_SIZE(x) UFSRV_SESSION_OUTSOCKMSG_QUEUE_SIZE(&(x)->ufsrv_session)
#define SESSION_OUTSOCKMSG_QUEUE_PTR(x) UFSRV_SESSION_OUTSOCKMSG_QUEUE_PTR(&(x)->ufsrv_session)
#define SESSION_RESULT_PTR(x) UFSRV_SESSION_RESULT_PTR(&(x)->ufsrv_session)

#define SESSION_HTTPSESN_REQUEST_PTR(x) HttpSessionGetRequest((HttpSession *)SESSION_PROTOCOL_SESSION_DATA(x))
#define SESSION_HTTPSESN_RESPONSE_PTR(x) HttpSessionGetResponse((HttpSession *)SESSION_PROTOCOL_SESSION_DATA(x))

#define SESSION_SERVICE(x) (&(x->ufnet_session.sservice))
#define SESSION_LISTS_INIT_STATE(x)						((x)->ufnet_session.lists_init_state)
#define SESSION_USER_ACCOUNTSTATUS(x) (x->ufnet_session.sservice.user.account_status)
#define SESSION_USER_TTRIBUTES(x)	(x->ufnet_session.sservice.user.user_details.attrs)
#define SESSION_USERID_TEMP(x) (x->ufnet_session.sservice.user.user_details.user_id)
#define SESSION_UFSRVUID(x) (x->ufnet_session.sservice.user.user_details.uid.data)
#define SESSION_UFSRVUIDSTORE(x) (x->ufnet_session.sservice.user.user_details.uid)
#define SESSION_USERNAME(x)	(x->ufnet_session.sservice.user.user_details.user_name)
#define SESSION_USERPASSWORD(x)	(x->ufnet_session.sservice.user.user_details.password)
#define SESSION_USERBOOLPREFS(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.on_off)
#define SESSION_USERNICKNAME(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.nickname)
#define SESSION_USERGUARDIAN_UID(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.guardian_uid)
#define SESSION_USERAVATAR(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.avatar)
#define SESSION_USERE164NUMBER(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.e164number)
#define SESSION_USER_PROFILE_KEY(x) (x)->ufnet_session.sservice.user.user_details.profile_key
#define SESSION_BASEFENCE_LOCAL(x)	(x->ufnet_session.sservice.user.user_details.base_fence_local_id)
#define SESSION_HOMEBASE_GEOLOC_DYNAMIC(x)						(x)->ufnet_session.sservice.user.user_details.homebase_geoloc_dynamic
#define SESSION_USERPREF_HOMEBASE_GEOLOC(x)				(x)->ufnet_session.sservice.user.user_details.user_preferences.homebase_geoloc
#define SESSION_USERPREF_BASELOC_ZONE(x)				(x)->ufnet_session.sservice.user.user_details.user_preferences.baseloc_anchor_zone
#define SESSION_USERPREF_GEOLOC_TRIGGER(x)				(x)->ufnet_session.sservice.user.user_details.user_preferences.geoloc_trigger
#define SESSION_USERPREF_ONOFF_SET(x, y, z)	((x)->ufnet_session.sservice.user.user_details.user_preferences.on_off.y=(z))
#define SESSION_USERPREF_ONOFF_GET(x, y)	((x)->ufnet_session.sservice.user.user_details.user_preferences.on_off.y)
#define SESSION_USERPREF_SHLIST_PROFILE(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_profile)
#define SESSION_USERPREF_SHLIST_PROFILE_PTR(x)	&((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_profile)
#define SESSION_USERPREF_SHLIST_LOCATION(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_location)
#define SESSION_USERPREF_SHLIST_LOCATION_PTR(x)	&((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_location)
#define SESSION_USERPREF_SHLIST_NETSTATE(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_netstate)
#define SESSION_USERPREF_SHLIST_NETSTATE_PTR(x)	&((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_netstate)
#define SESSION_USERPREF_SHLIST_READ_RECEIPT(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_read_receipt)
#define SESSION_USERPREF_SHLIST_READ_RECEIPT_PTR(x)	&((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_read_receipt)
#define SESSION_USERPREF_SHLIST_ACTIVITY_STATE(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_activity_state)
#define SESSION_USERPREF_SHLIST_ACTIVITY_STATE_PTR(x)	&((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_activity_state)
#define SESSION_USERPREF_SHLIST_BLOCKED(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_blocked)
#define SESSION_USERPREF_SHLIST_CONTACTS(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_contacts)
#define SESSION_USERPREF_SHLIST_BLOCKED_FENCE(x)	((x)->ufnet_session.sservice.user.user_details.user_preferences.sharelist_blocked_fence)

#define SESSION_ULOCATION_BYSERVER(x) ((x)->ufnet_session.sservice.user.user_details.user_location_by_server)
#define SESSION_ULOCATION_BYSERVER_PTR(x) (&((x)->ufnet_session.sservice.user.user_details.user_location_by_server))
#define SESSION_ULOCATION_BYSERVER_INITIALISED(x) ((x)->ufnet_session.sservice.user.user_details.user_location_by_server_initialised)
#define SESSION_ULOCATION_BYUSER(x) ((x)->ufnet_session.sservice.user.user_details.user_location)
#define SESSION_ULOCATION_BYUSER_PTR(x) (&((x)->ufnet_session.sservice.user.user_details.user_location))
#define SESSION_ULOCATION_BYUSER_INITIALISED(x) ((x)->ufnet_session.sservice.user.user_details.user_location_initialised)
#define SESSION_GEOFENCE_LAST(x) ((Fence *)GetInstance((x)->ufnet_session.sservice.user.geofence_last))
#define SESSION_GEOFENCE_LAST_INSTANCE(x) ((x)->ufnet_session.sservice.user.geofence_last)
#define SESSION_GEOFENCE_CURRENT(x) ((Fence *)GetInstance((x)->ufnet_session.sservice.user.geofence_current))
#define SESSION_GEOFENCE_CURRENT_INSTANCE(x) ((x)->ufnet_session.sservice.user.geofence_current)
#define SESSION_FENCE_LIST(x)	(x->ufnet_session.sservice.session_user_fence_list)
#define SESSION_FENCE_LIST_PTR(x)	&((x)->ufnet_session.sservice.session_user_fence_list)
#define SESSION_FENCE_LIST_COUNT(x)	(x->ufnet_session.sservice.session_user_fence_list.nEntries)
#define SESSION_FENCE_LIST_SIZE(x)	x->ufnet_session.sservice.session_user_fence_list.nEntries
#define SESSION_INVITED_FENCE_LIST(x)	((x)->ufnet_session.sservice.session_user_invited_fence_list)
#define SESSION_INVITED_FENCE_LIST_PTR(x)	&((x)->ufnet_session.sservice.session_user_invited_fence_list)
#define SESSION_INVITED_FENCE_LIST_SIZE(x)	((x)->ufnet_session.sservice.session_user_invited_fence_list.nEntries)

#define SESSION_LISTS_INIT_STATE_FENCES(x)		(SESSION_LISTS_INIT_STATE(x).fences)
#define SESSION_LISTS_INIT_STATE_PROFILE(x)		(SESSION_LISTS_INIT_STATE(x).profile)
#define SESSION_LISTS_INIT_STATE_LOCATION(x)	(SESSION_LISTS_INIT_STATE(x).location)
#define SESSION_LISTS_INIT_STATE_NETSTATE(x)	(SESSION_LISTS_INIT_STATE(x).netstate)
#define SESSION_LISTS_INIT_STATE_CONTACTS(x)	(SESSION_LISTS_INIT_STATE(x).contacts)
#define SESSION_LISTS_INIT_STATE_BLOCKED(x)	(SESSION_LISTS_INIT_STATE(x).blocked)
#define SESSION_LISTS_INIT_STATE_BLOCKED_FENCE(x)	(SESSION_LISTS_INIT_STATE(x).blocked_fence)
#define SESSION_LISTS_INIT_STATE_READ_RECEIPT(x)	(SESSION_LISTS_INIT_STATE(x).read_receipt)
#define SESSION_LISTS_INIT_STATE_ACTIVITY_STATE(x)	(SESSION_LISTS_INIT_STATE(x).activity_state)
#define SESSION_USERID(x) (UfsrvUidGetSequenceId(&SESSION_UFSRVUIDSTORE(x)))
#define SESSION_USERPREF_SHLIST_BLOCKED_PTR(x)	&(SESSION_USERPREF_SHLIST_BLOCKED((x)))
#define SESSION_USERPREF_SHLIST_CONTACTS_PTR(x)	&(SESSION_USERPREF_SHLIST_CONTACTS((x)))
#define SESSION_USERPREF_SHLIST_BLOCKED_FENCE_PTR(x)	&(SESSION_USERPREF_SHLIST_BLOCKED_FENCE((x)))

#endif //UFNETCORELIB_UFNET_SESSION_TYPE_H
