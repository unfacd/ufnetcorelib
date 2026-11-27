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
#include "config_ufnetcorelib.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <stdatomic.h>

#include <time.h>

#include <uflib/exponential_backoff/algorithm.h>
#include <uflib/exponential_backoff/exponential_backoff_type.h>

#include <ufnetcorelib/ufnet_core/lock_claim/lock_claim.h>
#include <ufsrv_core/cache_backend/persistence.h>
#include <ufsrv_core/cache_backend/redis.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h>
#include <ufsrv_core/delegator_sessions/thread_context_type.h>

extern __thread ThreadContext ufsrv_thread_context;

/* EVALSHA <sha> 3 UF:<fid> UF_LOCK_CLAIM UF_LOCK_CLAIM_META:<fid> <mode> <fid> <token> <ttl> <serverid> <geogroup> <reqid> */
#define FENCE_CLAIM_EVALSHA	"EVALSHA %s 3 UF:%lu UF_LOCK_CLAIM UF_LOCK_CLAIM_META:%lu %s %lu %s %lu %d %d %lu"

#define FENCE_CLAIM_REPLY_FIELDS	7

static atomic_ulong sClaimSeq = 0;

static void
_FenceClaimSleepMillis(unsigned millis)
{
	struct timespec ts = {.tv_sec = millis / 1000, .tv_nsec = (long)(millis % 1000) * 1000000L};

	nanosleep(&ts, NULL);
}

static const char *sStatusNames[] = {
	[FENCE_CLAIM_GRANTED]        = "granted",
	[FENCE_CLAIM_ALREADY_LOCKED] = "already_locked",
	[FENCE_CLAIM_NO_SUCH_FENCE]  = "no_such_fence",
	[FENCE_CLAIM_NOT_HOLDER]     = "not_holder",
	[FENCE_CLAIM_RELEASED]       = "released",
	[FENCE_CLAIM_BACKEND_ERROR]  = "backend_error",
	[FENCE_CLAIM_BAD_REPLY]      = "bad_reply",
};

const char *
FenceClaimStatusString(FenceClaimStatus status)
{
	if (status < 0 || status > FENCE_CLAIM_BAD_REPLY)	return "unknown";

	return sStatusNames[status];
}

unsigned
FenceClaimRefusedRescode(GlobalFenceClaim *claim, const char *func, const char *cname)
{
	if (claim == NULL) {
		syslog(LOG_ERR, "%s {pid:'%lu'}: ERROR: FENCE RECORD CLAIM NOT ATTEMPTED", func, pthread_self());

		return RESCODE_BACKEND_CONNECTION;
	}

	switch (claim->status) {
		case FENCE_CLAIM_ALREADY_LOCKED:
			syslog(LOG_NOTICE, "%s {pid:'%lu', fid:'%lu', cname:'%s'}: NOTICE: FENCE RECORD CLAIMED ELSEWHERE: serverid:'%d' geogroup:'%d' reqid:'%lu' until:'%llu'",
						 func, pthread_self(), claim->fid, cname ? cname : "", claim->holder_serverid, claim->holder_geogroup,
						 claim->holder_reqid, claim->deadline);

			return RESCODE_BACKEND_RESOURCE_LOCKED;

		case FENCE_CLAIM_NO_SUCH_FENCE:
			syslog(LOG_ERR, "%s {pid:'%lu', fid:'%lu', cname:'%s'}: ERROR: FENCE RECORD DOES NOT EXIST",
						 func, pthread_self(), claim->fid, cname ? cname : "");

			return RESCODE_FENCE_DOESNT_EXIST;

		case FENCE_CLAIM_GRANTED:
			return RESCODE_PROG_NULL_POINTER;

		default:
			syslog(LOG_ERR, "%s {pid:'%lu', fid:'%lu', cname:'%s'}: ERROR: FENCE RECORD CLAIM NOT GRANTED: '%s'",
						 func, pthread_self(), claim->fid, cname ? cname : "", FenceClaimStatusString(claim->status));

			return RESCODE_BACKEND_DATA;
	}
}

void
FenceClaimReleaseAndFree(Session *sesn_ptr, GlobalFenceClaim *claim, const char *func)
{
	if (claim == NULL)	return;

	GlobalFenceClaim *released_ptr = ReleaseFenceRecordGlobally(sesn_ptr, claim);

	if (IS_EMPTY(released_ptr) || released_ptr->status != FENCE_CLAIM_RELEASED)
		syslog(LOG_ERR, "%s {pid:'%lu', fid:'%lu'}: ERROR: FENCE RECORD CLAIM RELEASE NOT CONFIRMED", func, pthread_self(), claim->fid);

	FenceClaimFree(released_ptr);
	FenceClaimFree(claim);
}

void
FenceClaimFree(GlobalFenceClaim *claim)
{
	if (claim == NULL)	return;

	bool owned = claim->owned;

	free(claim->token);

	if (owned)	free(claim);
}

static FenceClaimStatus
_StatusFromString(const char *status)
{
	if (status == NULL)												return FENCE_CLAIM_BAD_REPLY;
	if (strcmp(status, "granted") == 0)				return FENCE_CLAIM_GRANTED;
	if (strcmp(status, "already_locked") == 0)	return FENCE_CLAIM_ALREADY_LOCKED;
	if (strcmp(status, "no_such_fence") == 0)		return FENCE_CLAIM_NO_SUCH_FENCE;
	if (strcmp(status, "not_holder") == 0)			return FENCE_CLAIM_NOT_HOLDER;
	if (strcmp(status, "released") == 0)				return FENCE_CLAIM_RELEASED;

	return FENCE_CLAIM_BAD_REPLY;
}

static GlobalFenceClaim *
_AllocateClaim(FenceClaimStatus status, unsigned long fid, GlobalFenceClaim *claim_in)
{
	GlobalFenceClaim *claim = claim_in;

	if (claim == NULL) {
		if ((claim = calloc(1, sizeof(GlobalFenceClaim))) == NULL)	return NULL;

		claim->owned = true;
	} else {
		free(claim->token);
		memset(claim, 0, sizeof(GlobalFenceClaim));
	}

	claim->status = status;
	claim->fid    = fid;

	return claim;
}

static GlobalFenceClaim *
_ClaimReply(redisReply *redis_ptr, unsigned long fid, GlobalFenceClaim *claim_in)
{
	if (redis_ptr == NULL)	return _AllocateClaim(FENCE_CLAIM_BACKEND_ERROR, fid, claim_in);

	if (redis_ptr->type == REDIS_REPLY_ERROR) {
		syslog(LOG_ERR, "%s {pid:'%lu', fid:'%lu'}: ERROR: CLAIM SCRIPT: '%s'", __func__, pthread_self(), fid, redis_ptr->str);

		return _AllocateClaim(FENCE_CLAIM_BACKEND_ERROR, fid, claim_in);
	}

	if (redis_ptr->type != REDIS_REPLY_ARRAY || redis_ptr->elements != FENCE_CLAIM_REPLY_FIELDS) {
		syslog(LOG_ERR, "%s {pid:'%lu', fid:'%lu'}: ERROR: CLAIM SCRIPT REPLY TYPE:'%d' ELEMENTS:'%lu'", __func__, pthread_self(), fid, redis_ptr->type, (unsigned long)redis_ptr->elements);

		return _AllocateClaim(FENCE_CLAIM_BAD_REPLY, fid, claim_in);
	}

	GlobalFenceClaim *claim = _AllocateClaim(_StatusFromString(redis_ptr->element[0]->str), fid, claim_in);
	if (claim == NULL)	return NULL;

	if (IS_PRESENT(redis_ptr->element[2]->str))
		claim->token = strndup(redis_ptr->element[2]->str, FENCE_CLAIM_TOKEN_SIZE);

	claim->deadline        = strtoull(redis_ptr->element[3]->str, NULL, 10);
	claim->holder_serverid = (int)strtol(redis_ptr->element[4]->str, NULL, 10);
	claim->holder_geogroup = (int)strtol(redis_ptr->element[5]->str, NULL, 10);
	claim->holder_reqid    = strtoul(redis_ptr->element[6]->str, NULL, 10);

	return claim;
}

/* The caller's identity as it is written into the claim, so a refused caller learns who holds the record.
 * The token is instance-scoped and unique per acquisition: the cluster-unique instance id plus a local
 * sequence, which is also what makes a leaked claim from this very server recognisable. */
static void
_ComposeClaimant(char *token_out, size_t token_sz, int *serverid_out, int *geogroup_out, unsigned long *reqid_out)
{
	int serverid = UfsrvConfigServerInstanceId();
	int geogroup = (int)UfsrvConfigUfsrvGeogroup();

	snprintf(token_out, token_sz, "%d:%d:%lu:%lu", serverid, geogroup, (unsigned long)getpid(), (unsigned long)atomic_fetch_add(&sClaimSeq, 1) + 1);

	*serverid_out = serverid;
	*geogroup_out = geogroup;
	*reqid_out    = 0;
}

static GlobalFenceClaim *
_RunClaimCommand(Session *sesn_ptr, unsigned long fid, const char *mode, const char *token,
								 unsigned long ttl_ms, int serverid, int geogroup, unsigned long reqid, GlobalFenceClaim *claim_in)
{
	if (unlikely(IS_EMPTY(sesn_ptr)))	return NULL;

	redisReply *redis_ptr = (*THREAD_CONTEXT_FENCE_CACHEBACKEND->send_command)(sesn_ptr, THREAD_CONTEXT_FENCE_CACHEBACKEND,
																																					 FENCE_CLAIM_EVALSHA,
																																					 REDIS_SCRIPT_SHA1_FENCE_CLAIM,
																																					 fid, fid, mode, fid,
																																					 token, ttl_ms,
																																					 serverid, geogroup, reqid);

	GlobalFenceClaim *claim = _ClaimReply(redis_ptr, fid, claim_in);

	if (redis_ptr)	freeReplyObject(redis_ptr);

	return claim;
}

GlobalFenceClaim *
ClaimFenceRecordGlobally(Session *sesn_ptr, unsigned long fid, const char *cname, GlobalFenceClaim *claim_in, bool is_retry)
{
	char token[FENCE_CLAIM_TOKEN_SIZE];
	int  serverid, geogroup;
	unsigned long reqid;
	BackoffAlgorithmContext_t backoff_ctx;
	GlobalFenceClaim *claim  = NULL;
	GlobalFenceClaim *storage = claim_in;

	_ComposeClaimant(token, sizeof token, &serverid, &geogroup, &reqid);

	if (is_retry) {
		BackoffAlgorithm_InitializeParamsExponential(&backoff_ctx, FENCE_CLAIM_BACKOFF_BASE_MS,
		                                             FENCE_CLAIM_BACKOFF_MAX_MS, FENCE_CLAIM_MAX_RETRIES);
	}

	for (;;) {
		claim = _RunClaimCommand(sesn_ptr, fid, "claim", token, FENCE_CLAIM_DEFAULT_TTL_MS, serverid, geogroup, reqid, storage);

		if (claim == NULL)	return NULL;

		/* only contention is worth waiting out: a missing record or a failed check will not change */
		if (!is_retry || claim->status != FENCE_CLAIM_ALREADY_LOCKED)	break;

		uint16_t delay = 0;

		if (BackoffAlgorithm_GetNextBackoff(&backoff_ctx, 0, &delay) == BackoffAlgorithmRetriesExhausted)	break;

		syslog(LOG_DEBUG, "%s {pid:'%lu', fid:'%lu'}: NOTICE: FENCE RECORD CLAIMED ELSEWHERE: RETRYING IN '%u' ms", __func__, pthread_self(), fid, delay);

		_FenceClaimSleepMillis(delay);

		storage = claim; /* the reply storage is reused, so the next attempt frees and refills it */
	}

	if (claim->status == FENCE_CLAIM_GRANTED) {
		syslog(LOG_DEBUG, "%s {pid:'%lu', fid:'%lu', cname:'%s', token:'%s', deadline:'%llu'}: NOTICE: CLAIM GRANTED", __func__, pthread_self(), fid, cname ? cname : "", token, claim->deadline);
	} else if (claim->status == FENCE_CLAIM_ALREADY_LOCKED) {
		syslog(LOG_DEBUG, "%s {pid:'%lu', fid:'%lu', cname:'%s'}: NOTICE: CLAIM REFUSED: HELD BY serverid:'%d' geogroup:'%d' reqid:'%lu' until:'%llu'", __func__, pthread_self(), fid, cname ? cname : "", claim->holder_serverid, claim->holder_geogroup, claim->holder_reqid, claim->deadline);
	}

	return claim;
}

GlobalFenceClaim *
RenewFenceRecordGlobally(Session *sesn_ptr, GlobalFenceClaim *claim, unsigned long ttl_ms)
{
	if (claim == NULL || IS_EMPTY(claim->token))	return NULL;

	if (ttl_ms == 0)	ttl_ms = FENCE_CLAIM_DEFAULT_TTL_MS;

	return _RunClaimCommand(sesn_ptr, claim->fid, "renew", claim->token, ttl_ms, claim->holder_serverid, claim->holder_geogroup, claim->holder_reqid, NULL);
}

GlobalFenceClaim *
ReleaseFenceRecordGlobally(Session *sesn_ptr, GlobalFenceClaim *claim)
{
	if (claim == NULL || IS_EMPTY(claim->token))	return NULL;

	return _RunClaimCommand(sesn_ptr, claim->fid, "release", claim->token, 0, claim->holder_serverid, claim->holder_geogroup, claim->holder_reqid, NULL);
}

GlobalFenceClaim *
InspectFenceRecordGlobally(Session *sesn_ptr, unsigned long fid)
{
	return _RunClaimCommand(sesn_ptr, fid, "inspect", "0", 0, 0, 0, 0, NULL);
}
