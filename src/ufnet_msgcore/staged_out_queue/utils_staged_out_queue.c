/**
 * @file utils_staged_out_queue.c
 * @brief Shared machinery for the fence and user staged out queues.
 *
 * Both queues store the same record shape and differ only in what the queue belongs to, which
 * backend it lives on, and what the second prefix field means (@c gid for the fence queue, @c fid
 * for the user queue). This file owns the prefix grammar, the redis round trip and the
 * discard/log decisions; each queue keeps a thin wrapper that supplies its backend and validation.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
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

#include <main_ufnetcorelib.h>
#include <errno.h>
#include <stdarg.h>
#include <ufsrv_core/delegator_sessions/thread_context_type.h>
#include <ufsrv_core/cache_backend/redis.h>
#include <ufnet_msgcore/staged_out_queue/utils_staged_out_queue.h>
#include <uflib/utils.h>

extern __thread ThreadContext ufsrv_thread_context;

/**
 * @brief Append one formatted field to a record prefix.
 *
 * @param buffer_ptr Destination prefix buffer.
 * @param buffer_sz  Total size of @p buffer_ptr.
 * @param offset     Write position, maintained by the caller across calls.
 * @param format     printf-style field format.
 * @param ...        Arguments for @p format.
 *
 * @return Bytes written, or zero when the field did not fit or formatting failed.
 */
size_t
StagedOutQueuePrefixWrite(char *buffer_ptr, size_t buffer_sz, size_t offset, const char *format, ...)
{
  va_list args;
  int     written;

  va_start(args, format);
  written = vsnprintf(buffer_ptr + offset, buffer_sz - offset, format, args);
  va_end(args);

  if (written < 0 || (size_t)written >= (buffer_sz - offset)) return 0;

  return (size_t)written;
}

/**
 * @brief Read one decimal field from a record and advance past its ':' separator.
 *
 * @param cursor_ptr Position in the record.
 * @param end_ptr    One past the end of the record.
 * @param value_out  Receives the parsed value.
 * @param next_ptr   Receives the position just after the separator.
 *
 * @return @c true only when a non-empty decimal field terminated by ':' was consumed entirely
 *         inside the record; otherwise @c false.
 */
bool
StagedOutQueueParseField(const char *cursor_ptr, const char *end_ptr, size_t *value_out, const char **next_ptr)
{
  char *        end_of_number = NULL;
  unsigned long value         = 0;

  if (cursor_ptr >= end_ptr) return false;

  errno = 0;
  value = strtoul(cursor_ptr, &end_of_number, 10);

  if (errno != 0 || end_of_number == cursor_ptr || end_of_number >= end_ptr || *end_of_number != ':') return false;

  *value_out = (size_t)value;
  *next_ptr  = end_of_number + 1;

  return true;
}

/**
 * @brief Release a heap buffer at the end of the enclosing scope.
 *
 * @param ptr Address of the buffer pointer; a NULL pointer or a NULL buffer is ignored.
 */
void
StagedOutQueueCleanupFree(void *ptr)
{
  free(*(void **)ptr);
}

/**
 * @brief Release a redis reply at the end of the enclosing scope.
 *
 * @param reply_ptr Address of the reply pointer; a NULL pointer or a NULL reply is ignored.
 */
void
StagedOutQueueCleanupReply(redisReply **reply_ptr)
{
  if (IS_PRESENT(reply_ptr) && IS_PRESENT(*reply_ptr)) freeReplyObject(*reply_ptr);
}

/**
 * @brief Stage one record on a queue.
 *
 * @param spec                        Queue identity: names, commands and second-field meaning.
 * @param pers_ptr                    Backend the queue lives on.
 * @param key_a                       First key part (fid or uid), substituted into the templates.
 * @param key_b                       Second key part (eid).
 * @param staged_message_descriptor_ptr Event identity, target snapshot and compressed payload.
 *
 * @return Through the thread context, success with the resulting queue depth as userdata, or an
 *         error code.
 *
 * @dynamic_memory allocates the prefix and the record buffer; both are released before returning.
 * @locks none
 */
UFSRVResult *
StagedOutQueueStore(const StagedOutQueueSpec *spec, PersistenceBackend *pers_ptr, unsigned long key_a,
                    unsigned long key_b, const StagedMessageDescriptor *staged_message_descriptor_ptr)
{
  if (unlikely(IS_EMPTY(spec) || IS_EMPTY(spec->add_command))) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_NULL_POINTER)
  if (unlikely(IS_EMPTY(pers_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_NULL_POINTER)
  if (unlikely(IS_EMPTY(staged_message_descriptor_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_NULL_POINTER)
  if (unlikely(staged_message_descriptor_ptr->userid == 0)) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_MISSING_PARAM)

  bool record_has_targets = (spec->shape != STAGED_OUT_QUEUE_SHAPE_SINGLE);
  bool record_has_second  = (spec->shape == STAGED_OUT_QUEUE_SHAPE_TARGETS);

  if (record_has_targets
      && (IS_EMPTY(staged_message_descriptor_ptr->payload.uids)
          || (staged_message_descriptor_ptr->payload.uids_sz == 0))) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_MISSING_PARAM)
  if (unlikely(IS_EMPTY(staged_message_descriptor_ptr->payload.payload_ptr)) || (staged_message_descriptor_ptr->payload.
    payload_sz == 0) || (staged_message_descriptor_ptr->payload.raw_sz == 0)) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_MISSING_PARAM)

  const unsigned long *uids        = staged_message_descriptor_ptr->payload.uids;
  size_t               uids_sz     = staged_message_descriptor_ptr->payload.uids_sz;
  const void *         payload_ptr = staged_message_descriptor_ptr->payload.payload_ptr;
  size_t               payload_sz  = staged_message_descriptor_ptr->payload.payload_sz;
  size_t               raw_sz      = staged_message_descriptor_ptr->payload.raw_sz;
  unsigned long        eid         = staged_message_descriptor_ptr->eid;
  unsigned long        second      = spec->second_field_is_fid ? staged_message_descriptor_ptr->fid
                                                               : staged_message_descriptor_ptr->gid;
  unsigned long        userid      = staged_message_descriptor_ptr->userid;
  unsigned             attempts    = staged_message_descriptor_ptr->attempts;

  size_t prefix_sz  = (uids_sz * STAGED_OUT_QUEUE_UID_DIGITS) + STAGED_OUT_QUEUE_PREFIX_EXTRA;
  char * prefix_ptr = malloc(prefix_sz);
  if (unlikely(IS_EMPTY(prefix_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_MEMORY_EXHAUSTED)

  size_t prefix_len;

  if (!record_has_targets) {
    prefix_len = StagedOutQueuePrefixWrite(prefix_ptr, prefix_sz, 0, "%lu:%lu:", eid, userid);
  } else if (record_has_second) {
    prefix_len = StagedOutQueuePrefixWrite(prefix_ptr, prefix_sz, 0, "%lu:%lu:%lu:%zu:", eid, second, userid, uids_sz);
  } else {
    prefix_len = StagedOutQueuePrefixWrite(prefix_ptr, prefix_sz, 0, "%lu:%lu:%zu:", eid, userid, uids_sz);
  }

  if (prefix_len == 0) {
    free(prefix_ptr);
    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_INCONSISTENT_DATA)
  }

  for (size_t i = 0; record_has_targets && i < uids_sz; i++) {
    size_t written = StagedOutQueuePrefixWrite(prefix_ptr, prefix_sz, prefix_len, "%lu:", uids[i]);

    if (written == 0) {
      free(prefix_ptr);
      THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_INCONSISTENT_DATA)
    }

    prefix_len += written;
  }

  {
    size_t written = StagedOutQueuePrefixWrite(prefix_ptr, prefix_sz, prefix_len, "%zu:", raw_sz);

    if (written == 0) {
      free(prefix_ptr);
      THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_INCONSISTENT_DATA)
    }

    prefix_len += written;
  }

  {
    size_t written = StagedOutQueuePrefixWrite(prefix_ptr, prefix_sz, prefix_len, "%u:", attempts);

    if (written == 0) {
      free(prefix_ptr);
      THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_INCONSISTENT_DATA)
    }

    prefix_len += written;
  }

  size_t         record_sz  = prefix_len + payload_sz;
  unsigned char *record_ptr = malloc(record_sz);

  if (unlikely(IS_EMPTY(record_ptr))) {
    free(prefix_ptr);
    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_MEMORY_EXHAUSTED)
  }

  memcpy(record_ptr, prefix_ptr, prefix_len);
  memcpy(record_ptr + prefix_len, payload_ptr, payload_sz);

  free(prefix_ptr);

  redisReply *redis_ptr = (*pers_ptr->send_command)(NULL, pers_ptr, spec->add_command, key_a, key_b, record_ptr,
                                                    record_sz);

  free(record_ptr);

  if (unlikely(IS_EMPTY(redis_ptr))) {
    syslog(
      LOG_DEBUG,
      "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', uids:'%zu'): ERROR: COULD ISSUE STAGED OUT QUEUE COMMAND: BACKEND CONNECTIVITY ERROR",
      __func__, pthread_self(), spec->queue_name, key_a, key_b, uids_sz);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_CONNECTION)
  }

  long long queue_sz = (redis_ptr->type == REDIS_REPLY_INTEGER) ? (long long)redis_ptr->integer : -1;

  freeReplyObject(redis_ptr);

  if (unlikely(queue_sz < 0)) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)

  THREAD_CONTEXT_RETURN_RESULT_SUCCESS((void *)(uintptr_t)queue_sz, RESCODE_BACKEND_DATA_SETCREATED)
}

/**
 * @brief Pop one record from a queue and hand back its parsed fields.
 *
 * @param spec                        Queue identity: names, commands and second-field meaning.
 * @param pers_ptr                    Backend the queue lives on.
 * @param key_a                       First key part (fid or uid).
 * @param key_b                       Second key part (eid), checked against the record.
 * @param[out] staged_message_descriptor_ptr_out Receives the parsed descriptor.
 *
 * @return Through the thread context, success carrying @p staged_message_descriptor_ptr_out as
 *         userdata, or an error code.
 *
 * @dynamic_memory the caller owns @c payload.uids and @c payload.payload_ptr on success.
 * @locks none
 */
UFSRVResult *
StagedOutQueueExtract(const StagedOutQueueSpec *spec, PersistenceBackend *pers_ptr, unsigned long key_a,
                      unsigned long key_b, StagedMessageDescriptor *staged_message_descriptor_ptr_out)
{
  if (unlikely(IS_EMPTY(spec) || IS_EMPTY(spec->get_command))) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_NULL_POINTER)
  if (unlikely(IS_EMPTY(pers_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_NULL_POINTER)
  if (unlikely(IS_EMPTY(staged_message_descriptor_ptr_out))) THREAD_CONTEXT_RETURN_RESULT_ERROR(
    NULL, RESCODE_PROG_NULL_POINTER)

  __attribute__((cleanup(StagedOutQueueCleanupReply))) redisReply *redis_ptr = (*pers_ptr->send_command)(
    NULL, pers_ptr, spec->get_command, key_a, key_b);

  if (unlikely(IS_EMPTY(redis_ptr))) THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_CONNECTION)

  if (redis_ptr->type == REDIS_REPLY_NIL) THREAD_CONTEXT_RETURN_RESULT_SUCCESS(NULL, RESCODE_BACKEND_DATA_EMPTYSET)

  if (redis_ptr->type != REDIS_REPLY_STRING || IS_EMPTY(redis_ptr->str) || (redis_ptr->len == 0)) {
    syslog(
      LOG_ERR,
      "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', reply_type:'%d', reply_len:'%zu'): ERROR: DISCARDED STAGED RECORD: UNREADABLE REPLY PAYLOAD",
      __func__, pthread_self(), spec->queue_name, key_a, key_b, redis_ptr->type, (size_t)redis_ptr->len);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA)
  }

  const char *cursor_ptr = redis_ptr->str;
  const char *end_ptr    = redis_ptr->str + redis_ptr->len;

  bool record_has_targets = (spec->shape != STAGED_OUT_QUEUE_SHAPE_SINGLE);
  bool record_has_second  = (spec->shape == STAGED_OUT_QUEUE_SHAPE_TARGETS);

  size_t record_eid = 0, record_second = 0, record_userid = 0, uids_sz = 0, raw_sz = 0, record_attempts = 0;

  if (!StagedOutQueueParseField(cursor_ptr, end_ptr, &record_eid, &cursor_ptr) || (record_eid != (size_t)key_b)) {
    syslog(
      LOG_ERR,
      "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', record_len:'%zu', record_eid:'%zu', head:'%.*s'): ERROR: DISCARDED STAGED RECORD: EVENT ID FIELD UNPARSEABLE OR DOES NOT MATCH THE KEY",
      __func__, pthread_self(), spec->queue_name, key_a, key_b, (size_t)redis_ptr->len, record_eid,
      STAGED_OUT_QUEUE_HEAD_LOG_SZ, redis_ptr->str);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA_TRANSFORMATION)
  }

  if (record_has_second && !StagedOutQueueParseField(cursor_ptr, end_ptr, &record_second, &cursor_ptr)) {
    syslog(
      LOG_ERR,
      "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', record_len:'%zu', head:'%.*s'): ERROR: DISCARDED STAGED RECORD: FENCE IDENTITY FIELD UNPARSEABLE",
      __func__, pthread_self(), spec->queue_name, key_a, key_b, (size_t)redis_ptr->len,
      STAGED_OUT_QUEUE_HEAD_LOG_SZ, redis_ptr->str);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA_TRANSFORMATION)
  }

  if (!StagedOutQueueParseField(cursor_ptr, end_ptr, &record_userid, &cursor_ptr)) {
    syslog(
      LOG_ERR,
      "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', record_len:'%zu', head:'%.*s'): ERROR: DISCARDED STAGED RECORD: ORIGINATOR FIELD UNPARSEABLE",
      __func__, pthread_self(), spec->queue_name, key_a, key_b, (size_t)redis_ptr->len,
      STAGED_OUT_QUEUE_HEAD_LOG_SZ, redis_ptr->str);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA_TRANSFORMATION)
  }

  if (record_has_targets
      && (!StagedOutQueueParseField(cursor_ptr, end_ptr, &uids_sz, &cursor_ptr) || (uids_sz == 0))) {
    syslog(
      LOG_ERR,
      "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', record_len:'%zu', uids_sz:'%zu', head:'%.*s'): ERROR: DISCARDED STAGED RECORD: TARGET COUNT UNPARSEABLE",
      __func__, pthread_self(), spec->queue_name, key_a, key_b, (size_t)redis_ptr->len, uids_sz,
      STAGED_OUT_QUEUE_HEAD_LOG_SZ, redis_ptr->str);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA_TRANSFORMATION)
  }

  __attribute__((cleanup(StagedOutQueueCleanupFree))) unsigned long *uids = NULL;

  if (record_has_targets) {
    uids = calloc(uids_sz, sizeof(unsigned long));

    if (unlikely(IS_EMPTY(uids))) {
      syslog(
        LOG_ERR,
        "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', uids_sz:'%zu'): ERROR: DISCARDED STAGED RECORD: COULD NOT ALLOCATE TARGET LIST",
        __func__, pthread_self(), spec->queue_name, key_a, key_b, uids_sz);

      THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_MEMORY_EXHAUSTED)
    }

    for (size_t i = 0; i < uids_sz; i++) {
      size_t target_uid = 0;

      if (!StagedOutQueueParseField(cursor_ptr, end_ptr, &target_uid, &cursor_ptr)) {
        syslog(
          LOG_ERR,
          "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', target_idx:'%zu', uids_sz:'%zu'): ERROR: DISCARDED STAGED RECORD: TARGET FIELD UNPARSEABLE",
          __func__, pthread_self(), spec->queue_name, key_a, key_b, i, uids_sz);

        THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA_TRANSFORMATION)
      }

      uids[i] = (unsigned long)target_uid;
    }
  } else {
    /* the key names the single recipient; the record only says who caused the event */
    uids = malloc(sizeof(unsigned long));

    if (unlikely(IS_EMPTY(uids))) {
      syslog(
        LOG_ERR,
        "%s (pid:'%lu', queue:'%s', key:'%lu:%lu'): ERROR: DISCARDED STAGED RECORD: COULD NOT ALLOCATE RECIPIENT",
        __func__, pthread_self(), spec->queue_name, key_a, key_b);

      THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_MEMORY_EXHAUSTED)
    }

    uids[0] = key_a;
    uids_sz  = 1;
  }

  if (!StagedOutQueueParseField(cursor_ptr, end_ptr, &raw_sz, &cursor_ptr) || (raw_sz == 0) || !
    StagedOutQueueParseField(cursor_ptr, end_ptr, &record_attempts, &cursor_ptr) || (cursor_ptr >= end_ptr)) {
    syslog(
      LOG_ERR,
      "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', raw_sz:'%zu', attempts:'%zu', record_len:'%zu'): ERROR: DISCARDED STAGED RECORD: TRAILER FIELDS UNPARSEABLE OR NO PAYLOAD",
      __func__, pthread_self(), spec->queue_name, key_a, key_b, raw_sz, record_attempts, (size_t)redis_ptr->len);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_BACKEND_DATA_TRANSFORMATION)
  }

  size_t payload_sz = (size_t)(end_ptr - cursor_ptr);

  __attribute__((cleanup(StagedOutQueueCleanupFree))) unsigned char *payload_ptr = malloc(payload_sz);

  if (unlikely(IS_EMPTY(payload_ptr))) {
    syslog(
      LOG_ERR,
      "%s (pid:'%lu', queue:'%s', key:'%lu:%lu', payload_sz:'%zu'): ERROR: DISCARDED STAGED RECORD: COULD NOT ALLOCATE PAYLOAD",
      __func__, pthread_self(), spec->queue_name, key_a, key_b, payload_sz);

    THREAD_CONTEXT_RETURN_RESULT_ERROR(NULL, RESCODE_PROG_MEMORY_EXHAUSTED)
  }

  memcpy(payload_ptr, cursor_ptr, payload_sz);

  /* the second prefix field is the gid on the fence queue and the fid on the user queue; when it is
     not the fid, the fence id is the first key part. Only the TARGETS shape carries it. */
  staged_message_descriptor_ptr_out->fid                 = record_has_second
                                                             ? (spec->second_field_is_fid
                                                                  ? (unsigned long)record_second
                                                                  : key_a)
                                                             : 0;
  staged_message_descriptor_ptr_out->gid                 = (record_has_second && !spec->second_field_is_fid)
                                                             ? (unsigned long)record_second
                                                             : 0;
  staged_message_descriptor_ptr_out->eid                 = (unsigned long)record_eid;
  staged_message_descriptor_ptr_out->userid              = (unsigned long)record_userid;
  staged_message_descriptor_ptr_out->attempts            = (unsigned)record_attempts;
  staged_message_descriptor_ptr_out->payload.uids        = uids;
  staged_message_descriptor_ptr_out->payload.uids_sz     = uids_sz;
  staged_message_descriptor_ptr_out->payload.payload_ptr = payload_ptr;
  staged_message_descriptor_ptr_out->payload.payload_sz  = payload_sz;
  staged_message_descriptor_ptr_out->payload.raw_sz      = raw_sz;

  uids        = NULL;
  payload_ptr = NULL;

  THREAD_CONTEXT_RETURN_RESULT_SUCCESS(staged_message_descriptor_ptr_out, RESCODE_BACKEND_DATA)
}
