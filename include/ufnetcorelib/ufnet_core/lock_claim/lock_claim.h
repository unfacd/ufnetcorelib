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
 * Cluster-wide claim on a single fence record, UF:<fid>.
 *
 * One holder per fence across every server instance. A claim is taken before a mutation is decided,
 * so the record a caller reads is the record it later writes; it is released by token, and its
 * deadline in UF_LOCK_CLAIM only reclaims claims whose holder died.
 *
 * The redis script is etc/redis/redis_fence_claim.lua in ufsrvwebsock: load it at startup with
 *   redis-cli -p <port> SCRIPT LOAD "$(cat redis_fence_claim.lua)"
 * and keep REDIS_SCRIPT_SHA1_FENCE_CLAIM in step with the file's sha1.
 */

#ifndef UFSRV_FENCE_LOCK_CLAIM_H
#define UFSRV_FENCE_LOCK_CLAIM_H

#include <ufnetcorelib/ufnetcorelib_defs.h>
#include <ufnetcorelib/ufnet_core/lock_claim/lock_claim_type.h>
#include <ufnetcorelib/ufnet_core/session/session_type.h>


#define FENCE_CLAIM_RETRY_TRUE true

/** @brief Retry attempts when another instance holds the record. */
#define FENCE_CLAIM_MAX_RETRIES			3
/** @brief First backoff delay, in milliseconds. */
#define FENCE_CLAIM_BACKOFF_BASE_MS	500
/** @brief Ceiling on the backoff delay, in milliseconds. */
#define FENCE_CLAIM_BACKOFF_MAX_MS	5000

/**
 * @brief Take the cluster-wide claim on a fence record, UF:<fid>.
 *
 * @param sesn_ptr      Session whose fence backend runs the script.
 * @param fid           Fence to claim.
 * @param cname         Canonical name, for diagnosis only; the claim is keyed on fid.
 * @param claim_in      Caller-supplied storage, or NULL to have one allocated. A supplied struct has its
 *                      previous token released and is refilled in place.
 * @param is_retry      When true, a claim refused because another instance holds the record is retried with
 *                      exponential backoff, up to #FENCE_CLAIM_MAX_RETRIES. Absent records and failed checks
 *                      are never retried.
*                       attempt 1 refused → sleep  500 ms
                        attempt 2 refused → sleep 1000 ms
                        attempt 3 refused → sleep 2000 ms
                        4th call          → RetriesExhausted → return ALREADY_LOCKED
 *
 * @return The claim, or NULL when nothing could be attempted. Never freed by this call.
 * @retval status == FENCE_CLAIM_GRANTED  the record is held for this caller; release it to give it up.
 * @retval status == FENCE_CLAIM_ALREADY_LOCKED  another instance holds it; holder_* names that instance.
 * @retval status == FENCE_CLAIM_NO_SUCH_FENCE  UF:<fid> does not exist.
 * @retval status == FENCE_CLAIM_BACKEND_ERROR or FENCE_CLAIM_BAD_REPLY  the script or the reply failed.
 *
 * @warning Blocks the calling thread while backing off (nanosleep), for at most base+2·base+4·base ms.
 */
PUBLIC_API GlobalFenceClaim *ClaimFenceRecordGlobally(Session *sesn_ptr, unsigned long fid, const char *cname, GlobalFenceClaim *claim_in, bool is_retry);

/**
 * @brief Extend the deadline of a claim this token still owns.
 *
 * @param sesn_ptr  Session whose fence backend runs the script.
 * @param claim     A granted claim; its token identifies the holder.
 * @param ttl_ms    New lifetime in milliseconds, or 0 for #FENCE_CLAIM_DEFAULT_TTL_MS.
 *
 * @return A fresh claim describing the outcome; free with FenceClaimFree().
 * @retval status == FENCE_CLAIM_GRANTED   deadline extended.
 * @retval status == FENCE_CLAIM_NOT_HOLDER  the claim lapsed and was re-taken, so this token no longer owns it.
 */
PUBLIC_API GlobalFenceClaim *RenewFenceRecordGlobally(Session *sesn_ptr, GlobalFenceClaim *claim, unsigned long ttl_ms);

/**
 * @brief Release a claim this token still owns.
 *
 * @param sesn_ptr  Session whose fence backend runs the script.
 * @param claim     The claim to release.
 *
 * @return A fresh claim describing the outcome; free with FenceClaimFree().
 * @retval status == FENCE_CLAIM_RELEASED   released.
 * @retval status == FENCE_CLAIM_NOT_HOLDER  a lapsed claim cannot be released by its former holder.
 */
PUBLIC_API GlobalFenceClaim *ReleaseFenceRecordGlobally(Session *sesn_ptr, GlobalFenceClaim *claim);

/**
 * @brief Read the current holder without claiming.
 *
 * @param sesn_ptr  Session whose fence backend runs the script.
 * @param fid       Fence to inspect.
 *
 * @return A fresh claim describing the outcome; free with FenceClaimFree().
 * @retval status == FENCE_CLAIM_ALREADY_LOCKED  held; holder_* and deadline are populated.
 * @retval status == FENCE_CLAIM_NOT_HOLDER      free, or the deadline already passed.
 */
PUBLIC_API GlobalFenceClaim *InspectFenceRecordGlobally(Session *sesn_ptr, unsigned long fid);

/**
 * @brief Map a claim that was NOT granted to a rescode, logging it once for every controller.
 *
 * @param claim  The refused claim, or NULL when the script could not run.
 * @param func   Calling function, for the log line.
 * @param cname  Canonical name, for the log line; may be NULL.
 *
 * @return RESCODE_BACKEND_RESOURCE_LOCKED when another instance holds the record,
 *         RESCODE_FENCE_DOESNT_EXIST when the record is absent, RESCODE_BACKEND_CONNECTION when the script
 *         could not run, RESCODE_BACKEND_DATA otherwise; RESCODE_PROG_NULL_POINTER for a granted claim.
 */
PUBLIC_API unsigned FenceClaimRefusedRescode(GlobalFenceClaim *claim, const char *func, const char *cname);

/**
 * @brief Release the claim, confirm the release, then free both the reply and the claim.
 *
 * @param sesn_ptr  Session whose fence backend runs the script.
 * @param claim     Claim to release; NULL is ignored.
 * @param func      Calling function, for the log line when the release is not confirmed.
 */
PUBLIC_API void FenceClaimReleaseAndFree(Session *sesn_ptr, GlobalFenceClaim *claim, const char *func);

/**
 * @brief Human-readable name of a claim status, for logs.
 *
 * @param status  Status to name.
 *
 * @return A static string; "unknown" for a value outside the enum.
 */
PUBLIC_API const char *FenceClaimStatusString(FenceClaimStatus status);

/**
 * @brief Free a claim returned by this module.
 *
 * @param claim  Claim to free; NULL is ignored.
 *
 * @note The token is always released; the struct itself only when this module allocated it, so a
 *       caller-supplied claim on the stack is left alone.
 */
PUBLIC_API void FenceClaimFree(GlobalFenceClaim *claim);

#endif //UFSRV_FENCE_LOCK_CLAIM_H
