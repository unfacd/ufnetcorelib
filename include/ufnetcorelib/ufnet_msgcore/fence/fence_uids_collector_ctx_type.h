/**
 * @file fence_uids_collector_ctx_type.h
 * @brief fence_uids_collector_ctx_type
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 * Created by ayman on 16/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFNETCORELIB_FENCE_UIDS_COLLECTOR_CTX_TYPE_H
#define UFNETCORELIB_FENCE_UIDS_COLLECTOR_CTX_TYPE_H

#include <stdbool.h>

/* Per-call state. It is the only channel between
 * FenceMembersCollectUids() and the collector callbacks, and it is mutable, which is why the
 * filter lives here and not in the (shared, const) descriptors. */
typedef struct FenceUidsCollectorCtx
{
  /* ---- policy, supplied by the caller ------------------------------------------- */
  bool                 use_filter;   /* false = accept every well-formed member */
  const unsigned long *uids;         /* filter keys, borrowed for the duration of the call */
  size_t               uids_sz;      /* must be > 0 when use_filter is true */
  bool                 exclude;      /* false: keep only uids[], true: keep everything except uids[] */

  /* ---- results, written by the call --------------------------------------------- */
  size_t               n_seen;       /* items the source yielded, before any filtering */
  size_t               n_malformed;  /* items that were not a well-formed uid */
  size_t               n_filtered;   /* well-formed items dropped by the filter */
} FenceUidsCollectorCtx;

#endif //UFNETCORELIB_FENCE_UIDS_COLLECTOR_CTX_TYPE_H
