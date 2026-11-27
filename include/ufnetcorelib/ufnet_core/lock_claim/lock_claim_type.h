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

#ifndef UFSRV_FENCE_LOCK_CLAIM_TYPE_H
#define UFSRV_FENCE_LOCK_CLAIM_TYPE_H

#include <stdbool.h>

#define FENCE_CLAIM_DEFAULT_TTL_MS	5000
#define FENCE_CLAIM_TOKEN_SIZE			64

typedef enum FenceClaimStatus {
	FENCE_CLAIM_GRANTED = 0,
	FENCE_CLAIM_ALREADY_LOCKED,
	FENCE_CLAIM_NO_SUCH_FENCE,
	FENCE_CLAIM_NOT_HOLDER,
	FENCE_CLAIM_RELEASED,
	FENCE_CLAIM_BACKEND_ERROR,
	FENCE_CLAIM_BAD_REPLY
} FenceClaimStatus;

/* holder_* is only meaningful on FENCE_CLAIM_ALREADY_LOCKED and FENCE_CLAIM_GRANTED: on the former it
 * names the instance holding the record, so a refused caller can log who holds it and back off. */
typedef struct GlobalFenceClaim {
	FenceClaimStatus		status;
	unsigned long				fid;
	char							*token;
	unsigned long long	deadline;
	int									holder_serverid;
	int									holder_geogroup;
	unsigned long				holder_reqid;
	bool								owned;	/* module-internal: storage was allocated here, not caller-supplied */
} GlobalFenceClaim;

#endif //UFSRV_FENCE_LOCK_CLAIM_TYPE_H
