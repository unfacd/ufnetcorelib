#ifndef UFNETCORELIB_UFSRV_TYPE_H
#define UFNETCORELIB_UFSRV_TYPE_H

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <stdalign.h>
#include <uflib/standard_net_includes.h>
#include <uflib/file_loader_type.h>
#include <uflib/file_loader_service_concurrent/file_loader_service_concurrent_type.h>
#include <ufsrv_core/ufsrvresult_type.h>
#include <ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue_type.h>
#include <ufnet_msgcore/utils_curve.h>

#include <ufnet_core/standard_lua_includes.h>
#include <ufsrv_core/delegator_sessions/sessions_delegator_type.h>
#include <ufnet_msgcore/cloud_authorization_token_type.h>
#include "uflib/adt/adt_distinct_array_type.h"
#include <ufsrvcorelib/ufsrv_core/system/server_run_mode_enum.h>
#include <ufnetcorelib/ufsrv_type.h>

/*
enum {
  LOG_MODE_SYSLOG, LOG_MODE_OWN
 }; //struct msredird.log_mode#1#

 enum {
  RUNNING_MODE_DAEMON, RUNNING_MODE_TERM
 };

 enum {
  REDIRECTION_ACTIVE, REDIRECTION_DISABLED
 }; //status#1#
 */

/*
typedef enum ServerRunMode {
	RUNMODE_NORMAL, RUNMODE_SHADOW, RUNMODE_INVALID
} ServerRunMode;
*/

#define IS_RUNMODE_VALID(x) (x<RUNMODE_INVALID && x>=RUNMODE_NORMAL)


#include <ufsrvcorelib/ufsrv_core/cache_backend/server_registry_redis_defs.h>

//see zk params below
//todo ufnetcorelib migration hardcoded
// #define SERVER_SECRET_PARAMS_SIZE  SignalSERVER_SECRET_PARAMS_LEN //1537
#define SERVER_PUBLIC_PARAMS_SIZE  SignalSERVER_PUBLIC_PARAMS_LEN //289


#endif //UFNETCORELIB_UFSRV_TYPE_H
