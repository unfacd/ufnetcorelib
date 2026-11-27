/**
 * @file fence_redis_backend.c
 * @brief fence_redis_backend
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

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <standard_defs.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis/hiredis.h>
#include <uflib/collection/collection.h>

#include "ufsrv_core/delegator_sessions/thread_context_type.h"
#include "ufnet_msgcore/fence/fence.h"
#include "ufnet_msgcore/fence/fence_redis_backend.h"

extern __thread ThreadContext ufsrv_thread_context;

typedef struct {
  unsigned long  uid;
  char          *raw;      /* owned copy of the Redis string: heap or arena depending on loader */
} FenceUidEntry;

/* Private per-call state handed to CollectorRun: the public ctx is policy/results only, while the
 * source adapter needs the fence id. Keeping them apart stops callers having to supply (or being
 * able to corrupt) the fid through the policy struct. */
typedef struct {
  FenceUidsCollectorCtx *policy;
  unsigned long          fid;
} FenceUidsRunCtx;


//region collector callbacks shared by the heap and arena descriptors
static void *
sFenceAcquire(void *ctx)
{
  const FenceUidsRunCtx *run = ctx;

  CacheBackendLoadUserIdsForFence(run->fid);

  if (!THREAD_CONTEXT_UFSRV_RESULT_IS_SUCCESS_WITH_BACKEND_DATA) return NULL;

  redisReply *reply = THREAD_CONTEXT_UFSRV_RESULT_USERDATA;
  THREAD_CONTEXT_UFSRV_RESULT_USERDATA = NULL;   /* take ownership; on_release frees it */

  return reply;                                  /* NULL here is reported as COLLECT_ERR_ACQUIRE */
}

static void
sFenceRelease(void *src, void *ctx)
{
  (void)ctx;
  if (IS_PRESENT(src)) freeReplyObject((redisReply *)src);
}

static bool
sFenceValidate(const void *src, size_t *size_hint, void *ctx)
{
  (void)ctx;
  const redisReply *r = src;

  if (r->type != REDIS_REPLY_ARRAY || r->elements == 0) return false;

  *size_hint = r->elements;
  return true;
}

static bool
sFenceNext(const void *src, size_t *pos, const void **item, void *ctx)
{
  (void)ctx;
  const redisReply *r = src;

  if (*pos >= r->elements) return false;

  *item = r->element[(*pos)++];                  /* may be NULL: on_process decides */
  return true;
}

/* Is the raw item a well-formed uid we accept at all? true => *uid_out is set. */
static bool
sFenceParseUid(const redisReply *e, unsigned long *uid_out)
{
  if (IS_EMPTY(e) || IS_EMPTY(e->str)) return false;                /* malformed: exclude */

  /* strtoul() silently accepts leading whitespace and +/-: require a digit up front */
  if (!isdigit((unsigned char)e->str[0])) return false;

  char *end = NULL;
  errno = 0;
  unsigned long uid = strtoul(e->str, &end, 10);

  if (errno != 0 || end == e->str || *end != '\0') return false;

  if (uid == 0) return false;                                       /* uid 0 is never a member */

  *uid_out = uid;
  return true;
}

/* Does `uid` pass the caller's key filter? Linear scan: right for the small sets this is used
 * with. If the set grows large, sort it once in the caller and swap this for a bsearch. */
static bool
sFenceFilterMatches(const FenceUidsCollectorCtx *c, unsigned long uid)
{
  if (IS_EMPTY(c) || !c->use_filter) return true;

  bool found = false;
  for (size_t i = 0; i < c->uids_sz; i++) {
    if (c->uids[i] == uid) { found = true; break; }
  }

  return c->exclude ? !found : found;
}

/* The WHOLE include/exclude decision, shared by both flavours so they cannot disagree.
 * The flavours differ only in how they copy the string AFTER this returns true, so every
 * SKIP still happens before any allocation. */
static bool
sFenceAdmit(const redisReply *e, FenceUidsRunCtx *run, unsigned long *uid_out)
{
  FenceUidsCollectorCtx *c = run->policy;
  unsigned long          uid;

  if (IS_PRESENT(c)) c->n_seen++;

  if (!sFenceParseUid(e, &uid))              { if (IS_PRESENT(c)) c->n_malformed++; return false; }
  if (!sFenceFilterMatches(c, uid))          { if (IS_PRESENT(c)) c->n_filtered++;  return false; }

  *uid_out = uid;
  return true;
}

//endregion

//region heap flavour: the string copy is malloc'd and freed per element
/* portable strdup (strdup is not in C17) */
static char *
sFenceDupStr(const char *s)
{
  size_t n = strlen(s) + 1;
  char  *p = malloc(n);
  if (p) memcpy(p, s, n);
  return p;
}

static CollectVerdict
sFenceProcessHeap(const void *item, void *dst, Arena *arena, void *ctx)
{
  (void)arena;
  const redisReply *e = item;
  unsigned long     uid;

  if (!sFenceAdmit(e, ctx, &uid)) return COLLECT_SKIP;

  char *copy = sFenceDupStr(e->str);
  if (!copy) return COLLECT_ABORT;                       /* OOM invalidates the whole set */

  FenceUidEntry *out = dst;
  out->uid = uid;
  out->raw = copy;
  return COLLECT_TAKE;
}

/* Frees one KEPT entry's copy. Called by the collector for rollback and by CollectionResultFree. */
static void
sFenceElemDestroy(void *elem, void *ctx)
{
  (void)ctx;
  free(((FenceUidEntry *)elem)->raw);
}
//endregion

//region arena flavour: the string copy lives in the result's arena
static CollectVerdict
sFenceProcessArena(const void *item, void *dst, Arena *arena, void *ctx)
{
  const redisReply *e = item;
  unsigned long     uid;

  if (IS_EMPTY(arena)) return COLLECT_ABORT;             /* descriptor/process mismatch: programming error */

  if (!sFenceAdmit(e, ctx, &uid)) return COLLECT_SKIP;

  char *copy = ArenaStrdup(arena, e->str);               /* no cleanup duty: a skip/abort rewinds it */
  if (!copy) return COLLECT_ABORT;

  FenceUidEntry *out = dst;
  out->uid = uid;
  out->raw = copy;
  return COLLECT_TAKE;
}
/* Uid-only flavour: the staging path needs the ids, not the raw redis strings, so the element is
 * a plain unsigned long and there is nothing to destroy per element. */
static CollectVerdict
sFenceProcessUidOnly(const void *item, void *dst, Arena *arena, void *ctx)
{
  (void)arena;
  const redisReply *e = item;
  unsigned long     uid;

  if (!sFenceAdmit(e, ctx, &uid)) return COLLECT_SKIP;

  *(unsigned long *)dst = uid;
  return COLLECT_TAKE;
}
//endregion

//region descriptors
/* Everything the two descriptors have in common, so they cannot drift apart. */
#define FENCE_COLLECTION_COMMON_FIELDS            \
    .on_acquire     = sFenceAcquire,              \
    .on_release     = sFenceRelease,              \
    .on_validate    = sFenceValidate,             \
    .on_next        = sFenceNext,                 \
    .allow_empty    = false  /* all entries filtered out => failure, like the original */

static const CollectionDescriptorV2 sFenceSpecHeap = {
  FENCE_COLLECTION_COMMON_FIELDS,
  .elem_sz         = sizeof(FenceUidEntry),
  .on_process      = sFenceProcessHeap,
  .on_elem_destroy = sFenceElemDestroy,
  .use_arena       = false,
};

static const CollectionDescriptorV2 sFenceSpecArena = {
  FENCE_COLLECTION_COMMON_FIELDS,
  .elem_sz         = sizeof(FenceUidEntry),
  .on_process      = sFenceProcessArena,
  /* no .on_elem_destroy: the strings die with the arena */
  .use_arena       = true,
  .arena_chunk_sz  = 4096,
};

/* Plain uid list: out->items is an unsigned long[], ready to be used as a target array. */
static const CollectionDescriptorV2 sFenceSpecUidList = {
  FENCE_COLLECTION_COMMON_FIELDS,
  .elem_sz         = sizeof(unsigned long),
  .on_process      = sFenceProcessUidOnly,
  .use_arena       = false,
};
//endregion

//region public API
/** Heap descriptor: out->items[i].raw is malloc'd and freed by FreeFenceMembersCollectUids(). */
const CollectionDescriptorV2 *
ProvideDefaultFenceMembersCollectionDescriptor(void)
{
  return &sFenceSpecHeap;
}

/** Arena descriptor: out->items[i].raw points into out->arena. Never free() it individually;
 *  it is valid until FreeFenceMembersCollectUids(). */
const CollectionDescriptorV2 *
ProvideArenaFenceMembersCollectionDescriptor(void)
{
  return &sFenceSpecArena;
}

/** Uid-list descriptor: out->items is an unsigned long[], no per-element allocation. */
PUBLIC_API const CollectionDescriptorV2 *
ProvideUidListFenceMembersCollectionDescriptor(void)
{
  return &sFenceSpecUidList;
}

/**
 * Collects the member uids of fence `fid` into out->items (out->count FenceUidEntry), keeping
 * only those that pass `filter` (NULL = keep every well-formed member).
 *
 * `collection_descriptor` selects the memory model: pass one of the Provide*() descriptors, or a
 * copy of one with policy fields adjusted (limit, allow_empty, arena_chunk_sz). Its callbacks are
 * bound to this file's private context, so they must stay as provided.
 *
 * Do not use max_skipped with a selective filter: members the filter excludes count as skipped
 * and would trip the limit on perfectly good data.
 *
 * Returns false if nothing was collected (backend failure, malformed reply, or every member
 * filtered out) or on bad arguments. On success the caller releases `out` with
 * FreeFenceMembersCollectUids(), passing the SAME descriptor. On failure `out` is already clean.
 */
bool
FenceMembersCollectUids(const CollectionDescriptorV2 *collection_descriptor, unsigned long fid, FenceUidsCollectorCtx *ctx, CollectionResult *out)
{
  if (IS_EMPTY(collection_descriptor) || IS_EMPTY(out) || fid == 0) return false;

  FenceUidsCollectorCtx unfiltered_ctx = {0};

  if (!IS_PRESENT(ctx)) ctx = &unfiltered_ctx;

  if (ctx->use_filter && (IS_EMPTY(ctx->uids) || ctx->uids_sz == 0)) return false;

  /* ctx is private, so refuse a descriptor that was not built from ours */
  if (collection_descriptor->on_acquire != sFenceAcquire ||
      collection_descriptor->on_next    != sFenceNext    ||
      collection_descriptor->on_validate!= sFenceValidate) return false;

  ctx->n_seen      = 0;
  ctx->n_malformed = 0;
  ctx->n_filtered  = 0;

  FenceUidsRunCtx run = { .policy = ctx, .fid = fid };
  CollectResult rc = CollectorRun(collection_descriptor, &run, out);

  if (rc == COLLECT_ERR_EMPTY && ctx->n_seen > 0) {
    /* the source yielded members but the filter excluded every one: that is "no targets", not a
       backend failure. `out` stays clean with count 0. */
    return true;
  }

  if (rc != COLLECT_OK) {
    /* log CollectorResultStr(rc), ctx.n_malformed and ctx.n_filtered with your logging macro */
    return false;
  }

  return true;
}

void
FreeFenceMembersCollectUids(const CollectionDescriptorV2 *collection_descriptor, CollectionResult *out)
{
  CollectionResultFree(collection_descriptor, out, NULL);
}
//endregion

/*
 * Usage:
 *
 *   const unsigned long        me[]   = { requesting_uid };
 *   FenceUidsCollectorCtx      ctx    = { .fid = fid, .use_filter = true,
 *                                         .uids = me, .uids_sz = 1, .exclude = true };
 *   const CollectionDescriptorV2 *cd  = ProvideArenaFenceMembersCollectionDescriptor();
 *   CollectionResult             out  = {0};
 *
 *   if (FenceMembersCollectUids(cd, &ctx, &out)) {             // all members except the requester
 *     const FenceUidEntry *entries = out.items;
 *     for (size_t i = 0; i < out.count; i++) use(entries[i].uid, entries[i].raw);
 *     FreeFenceMembersCollectUids(cd, &out);
 *   }
 *
 *   FenceUidsCollectorCtx      all     = { .fid = fid };        // no filtering
 *   FenceMembersCollectUids(cd, &all, &out);
 */