#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <session_io_work.h>
#include <ufsrv_core/session/sessions_utils.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>

typedef struct TestRuntime {
  SessionIoQueue queue;
  pthread_mutex_t mutex;
} TestRuntime;

typedef struct TestSession {
  SessionIoSlot slot;
  TestRuntime *runtime;
  pthread_rwlock_t lock;
  unsigned long id;
  int fd;
  int peer;
  unsigned long status;
  bool closed;
  unsigned reads;
  unsigned writes;
  size_t bytes;
  size_t block_size;
  atomic_int references;
  unsigned read_failures;
  unsigned read_error_code;
  uint64_t now_ns;
  uint64_t retry_until_ns;
  bool bleed_reads;
  UFSRVResult read_result;
} TestSession;

static const char response[] = "server-output";
static atomic_bool fail_allocations;

void *__real_malloc(size_t);
void *__wrap_malloc(size_t size)
{
  return atomic_load(&fail_allocations) ? NULL : __real_malloc(size);
}

static void
sInit(TestRuntime *runtime, TestSession *session, unsigned long id)
{
  memset(session, 0, sizeof(*session));
  session->runtime = runtime;
  session->id = id;
  session->status = SESNSTATUS_CONNECTED;
  session->fd = -1;
  session->peer = -1;
  session->block_size = 127;
  atomic_init(&session->references, 0);
  assert(pthread_rwlock_init(&session->lock, NULL) == 0);
}

static void
sSocket(TestRuntime *runtime, TestSession *session, unsigned long id)
{
  int fds[2];
  sInit(runtime, session, id);
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds) == 0);
  session->fd = fds[0];
  session->peer = fds[1];
}

static void
sDestroy(TestSession *session)
{
  assert(atomic_load(&session->references) == 0);
  assert(!session->slot.queued);
  assert(!session->slot.held);
  assert(pthread_rwlock_destroy(&session->lock) == 0);
  if (session->fd >= 0) assert(close(session->fd) == 0);
  if (session->peer >= 0) assert(close(session->peer) == 0);
}

static void
sSubmit(TestSession *session, SessionIoWork work)
{
  assert(pthread_mutex_lock(&session->runtime->mutex) == 0);
  if (SessionIoSubmit(&session->runtime->queue, &session->slot, &work))
    atomic_fetch_add(&session->references, 1);
  assert(pthread_mutex_unlock(&session->runtime->mutex) == 0);
}

static SessionIoWork
sWork(TestSession *session, uint32_t events)
{
  return (SessionIoWork){.session = session, .session_id = session->id, .fd = session->fd, .events = events,
    .generation = SessionIoGeneration(&session->slot)};
}

static void
sRelease(const SessionIoWork *work, void *context)
{
  TestSession *session = work->session;
  assert(atomic_fetch_sub(&session->references, 1) > 0);
}

static bool
sReadable(const SessionIoWork *work)
{
  return (work->events & (EPOLLIN | EPOLLERR)) != 0 ||
         (!work->retry_read && (work->events & EPOLLRDHUP) != 0);
}

static SessionIoAcquire
sAcquire(const SessionIoWork *work, void *context)
{
  TestSession *session = work->session;
  int rc = pthread_rwlock_trywrlock(&session->lock);
  if (rc == EBUSY) {
    if (session->bleed_reads && sReadable(work)) return SESSION_IO_BLEED;
    return SESSION_IO_DEFER;
  }
  assert(rc == 0);
  if (SessionIoGeneration(&session->slot) != work->generation ||
      session->id != work->session_id || session->fd != work->fd || session->fd < 0 ||
      SESNSTATUS_IS_SET(session->status, SESNSTATUS_RECYCLED)) {
    assert(pthread_rwlock_unlock(&session->lock) == 0);
    return SESSION_IO_RETIRED;
  }
  if (SESNSTATUS_IS_SET(session->status, SESNSTATUS_SUSPENDED | SESNSTATUS_INSERVICE)) {
    assert(pthread_rwlock_unlock(&session->lock) == 0);
    return SESSION_IO_DEFER;
  }
  return SESSION_IO_ACQUIRED;
}

static int
sTakeRaw(TestRuntime *runtime, uint64_t now, SessionIoWork *work)
{
  assert(pthread_mutex_lock(&runtime->mutex) == 0);
  int result = SessionIoTake(&runtime->queue, now, sAcquire, sRelease, NULL, work);
  assert(pthread_mutex_unlock(&runtime->mutex) == 0);
  return result;
}

static bool
sTake(TestRuntime *runtime, uint64_t now, SessionIoWork *work)
{
  assert(pthread_mutex_lock(&runtime->mutex) == 0);
  int result = SessionIoTake(&runtime->queue, now, sAcquire, sRelease, NULL, work);
  if (result == SESSION_IO_TAKE_READY) ((TestSession *)work->session)->slot.held = false;
  assert(pthread_mutex_unlock(&runtime->mutex) == 0);
  return result == SESSION_IO_TAKE_READY;
}

static void
sClose(void *context)
{
  TestSession *session = context;
  assert(!session->closed);
  session->closed = true;
}

static SessionIoReadResult
sRead(void *context)
{
  TestSession *session = context;
  unsigned char buffer[127];
  session->reads++;
  if (session->read_failures) {
    session->read_failures--;
    session->read_result = (UFSRVResult){.result_type = RESULT_TYPE_ERR, .result_code = session->read_error_code};
    return SessionIoReadOutcome(&session->read_result,
                                !SESNSTATUS_IS_SET(session->status, SESNSTATUS_SUSPENDED | SESNSTATUS_RECYCLED),
                                false, session->now_ns, session->retry_until_ns);
  }
  ssize_t count = read(session->fd, buffer, session->block_size);
  if (count > 0) {
    for (ssize_t i = 0; i < count; i++) assert(buffer[i] == 'x');
    session->bytes += (size_t)count;
    return (SessionIoReadResult){.live = true, .progress = true};
  }
  if (count == 0) {
    sClose(session);
    return (SessionIoReadResult){0};
  }
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  return (SessionIoReadResult){.live = true};
}

static void
sWrite(void *context)
{
  TestSession *session = context;
  session->writes++;
  assert(!session->closed);
  assert(write(session->fd, response, sizeof(response)) == sizeof(response));
}

static void
sRetryTerminal(void *context, uint32_t events)
{
  TestSession *session = context;
  SessionIoWork work = sWork(session, events);
  work.retry_read = true;
  sSubmit(session, work);
}

static void
sDefer(void *context, uint32_t events, uint64_t retry_at)
{
  TestSession *session = context;
  SessionIoWork work = sWork(session, 0);
  SessionIoContinuation(&work, events, false);
  work.not_before_ns = retry_at;
  sSubmit(session, work);
}

static const SessionIoCallbacks callbacks = {
  .read = sRead, .write = sWrite, .close = sClose, .retry_terminal = sRetryTerminal, .defer = sDefer
};

static void
sDispatch(SessionIoWork *work)
{
  TestSession *session = work->session;
  sRelease(work, NULL);
  SessionIoDispatch(work, &callbacks, session);
  assert(pthread_rwlock_unlock(&session->lock) == 0);
}

void
SessionIoTestStatusFlags(void)
{
  unsigned long status = SESNSTATUS_CONNECTED;
  assert(!SESNSTATUS_IS_SET(status, SESNSTATUS_SUSPENDED | SESNSTATUS_INSERVICE));
  assert(!SESNSTATUS_IS_SET(status, SESNSTATUS_SUSPENDED | SESNSTATUS_RECYCLED));
  SESNSTATUS_SET(status, SESNSTATUS_SUSPENDED | SESNSTATUS_INSERVICE);
  assert(SESNSTATUS_IS_SET(status, SESNSTATUS_SUSPENDED));
  assert(SESNSTATUS_IS_SET(status, SESNSTATUS_INSERVICE));
  SESNSTATUS_UNSET(status, SESNSTATUS_SUSPENDED | SESNSTATUS_INSERVICE);
  assert(status == SESNSTATUS_CONNECTED);
  SESNSTATUS_SET(status, SESNSTATUS_RECYCLED);
  assert(SESNSTATUS_IS_SET(status, SESNSTATUS_SUSPENDED | SESNSTATUS_RECYCLED));
  SESNSTATUS_UNSET(status, SESNSTATUS_RECYCLED);
  assert(!SESNSTATUS_IS_SET(status, SESNSTATUS_SUSPENDED | SESNSTATUS_RECYCLED));
}

void
SessionIoTestPressure(void)
{
  enum { COUNT = 4096 };
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession *sessions = calloc(COUNT, sizeof(*sessions));
  assert(sessions);
  for (int i = 0; i < COUNT; i++) {
    sInit(&runtime, &sessions[i], i + 1);
    sessions[i].fd = 100000 + i;
  }
  atomic_store(&fail_allocations, true);
  assert(malloc(1) == NULL);
  for (int i = 0; i < COUNT; i++) {
    SessionIoWork work = sWork(&sessions[i], EPOLLIN);
    work.retry_read = true;
    sSubmit(&sessions[i], work);
    work.events = EPOLLOUT;
    work.retry_write = true;
    sSubmit(&sessions[i], work);
    memset(&work, 0, sizeof(work));
    assert(atomic_load(&sessions[i].references) == 1);
  }
  assert(runtime.queue.count == COUNT);
  for (int i = 0; i < COUNT; i++) {
    SessionIoWork work;
    assert(sTake(&runtime, 1, &work));
    assert(work.session == &sessions[i] && work.events == (EPOLLIN | EPOLLOUT));
    assert(work.retry_read && work.retry_write);
    sRelease(&work, NULL);
    assert(pthread_rwlock_unlock(&sessions[i].lock) == 0);
  }
  assert(runtime.queue.count == 0);
  atomic_store(&fail_allocations, false);
  for (int i = 0; i < COUNT; i++) {
    sessions[i].fd = -1;
    sDestroy(&sessions[i]);
  }
  free(sessions);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

static void *
sOneTake(void *context)
{
  TestRuntime *runtime = context;
  SessionIoWork work;
  assert(sTake(runtime, 1, &work));
  TestSession *session = work.session;
  assert(session->id == 3);
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&session->lock) == 0);
  return NULL;
}

void
SessionIoTestDeferral(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession sessions[3];
  for (int i = 0; i < 3; i++) {
    sSocket(&runtime, &sessions[i], i + 1);
    sSubmit(&sessions[i], sWork(&sessions[i], EPOLLIN));
  }
  assert(pthread_rwlock_wrlock(&sessions[0].lock) == 0);
  SESNSTATUS_SET(sessions[1].status, SESNSTATUS_SUSPENDED);
  pthread_t worker;
  assert(pthread_create(&worker, NULL, sOneTake, &runtime) == 0);
  assert(pthread_join(worker, NULL) == 0);
  assert(runtime.queue.count == 2);
  assert(atomic_load(&sessions[0].references) == 1 && atomic_load(&sessions[1].references) == 1);
  assert(pthread_rwlock_unlock(&sessions[0].lock) == 0);
  SessionIoWork work;
  assert(!sTake(&runtime, 2, &work));
  assert(sTake(&runtime, 1000001, &work));
  assert(work.session == &sessions[0]);
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&sessions[0].lock) == 0);
  SESNSTATUS_UNSET(sessions[1].status, SESNSTATUS_SUSPENDED);
  assert(sTake(&runtime, 1000002, &work));
  assert(work.session == &sessions[1]);
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&sessions[1].lock) == 0);
  sSubmit(&sessions[0], sWork(&sessions[0], EPOLLHUP));
  sessions[0].id++;
  assert(!sTake(&runtime, 2000000, &work));
  assert(runtime.queue.count == 0);
  for (int i = 0; i < 3; i++) sDestroy(&sessions[i]);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

void
SessionIoTestBusyBleed(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession session;
  sSocket(&runtime, &session, 7);
  session.bleed_reads = true;
  assert(pthread_rwlock_wrlock(&session.lock) == 0);
  sSubmit(&session, sWork(&session, EPOLLIN | EPOLLOUT));
  SessionIoWork work;
  assert(sTakeRaw(&runtime, 1, &work) == SESSION_IO_TAKE_BLEED);
  assert(work.events == (EPOLLIN | EPOLLOUT));
  assert(session.slot.queued);
  assert(session.slot.work.events == EPOLLOUT);
  assert(session.slot.work.retry_read);
  assert(atomic_load(&session.references) == 1);
  assert(runtime.queue.count == 1);
  assert(sTakeRaw(&runtime, 2, &work) == SESSION_IO_TAKE_NONE);
  session.bleed_reads = false;
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  assert(sTake(&runtime, 1000001, &work));
  assert(work.events == EPOLLOUT);
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  assert(runtime.queue.count == 0);
  sSubmit(&session, sWork(&session, EPOLLOUT));
  assert(pthread_rwlock_wrlock(&session.lock) == 0);
  session.bleed_reads = true;
  assert(sTakeRaw(&runtime, 2000000, &work) == SESSION_IO_TAKE_NONE);
  assert(session.slot.work.events == EPOLLOUT);
  assert(atomic_load(&session.references) == 1);
  session.bleed_reads = false;
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  assert(sTake(&runtime, 3000000, &work));
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  assert(pthread_rwlock_wrlock(&session.lock) == 0);
  session.bleed_reads = true;
  sSubmit(&session, sWork(&session, EPOLLIN | EPOLLOUT | EPOLLRDHUP));
  assert(sTakeRaw(&runtime, 4000000, &work) == SESSION_IO_TAKE_BLEED);
  assert(session.slot.work.events == (EPOLLOUT | EPOLLRDHUP));
  assert(session.slot.work.retry_read);
  assert(sTakeRaw(&runtime, 5000000, &work) == SESSION_IO_TAKE_NONE);
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  assert(sTake(&runtime, 6000000, &work));
  assert(work.retry_read && (work.events & EPOLLRDHUP));
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  session.bleed_reads = true;
  assert(pthread_rwlock_wrlock(&session.lock) == 0);
  sSubmit(&session, sWork(&session, EPOLLIN | EPOLLOUT));
  assert(sTakeRaw(&runtime, 7000000, &work) == SESSION_IO_TAKE_BLEED);
  assert(sTakeRaw(&runtime, 8000000, &work) == SESSION_IO_TAKE_NONE);
  sSubmit(&session, sWork(&session, EPOLLIN));
  assert(sTakeRaw(&runtime, 9000000, &work) == SESSION_IO_TAKE_BLEED);
  assert(session.slot.work.events == EPOLLOUT && session.slot.work.retry_read);
  assert(atomic_load(&session.references) == 1);
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  assert(sTake(&runtime, 10000000, &work));
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  sDestroy(&session);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

void
SessionIoTestOwnerKeepsLaterEdges(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession owner, other;
  sSocket(&runtime, &owner, 4);
  sSocket(&runtime, &other, 5);
  sSubmit(&owner, sWork(&owner, EPOLLIN));
  sSubmit(&other, sWork(&other, EPOLLIN));
  assert(pthread_mutex_lock(&runtime.mutex) == 0);
  SessionIoWork work;
  assert(SessionIoTake(&runtime.queue, 1, sAcquire, sRelease, NULL, &work) == SESSION_IO_TAKE_READY);
  assert(work.session == &owner && work.events == EPOLLIN);
  assert(owner.slot.held && !owner.slot.queued && owner.slot.work.events == 0);
  assert(runtime.queue.count == 1);
  SessionIoWork later = sWork(&owner, EPOLLIN | EPOLLOUT);
  assert(!SessionIoSubmit(&runtime.queue, &owner.slot, &later));
  assert(owner.slot.held && !owner.slot.queued);
  assert(owner.slot.work.events == (EPOLLIN | EPOLLOUT));
  assert(atomic_load(&owner.references) == 1 && runtime.queue.count == 1);
  SessionIoWork second;
  assert(SessionIoTake(&runtime.queue, 1, sAcquire, sRelease, NULL, &second) == SESSION_IO_TAKE_READY);
  assert(second.session == &other);
  other.slot.held = false;
  assert(pthread_mutex_unlock(&runtime.mutex) == 0);
  sRelease(&second, NULL);
  assert(pthread_rwlock_unlock(&other.lock) == 0);
  assert(pthread_mutex_lock(&runtime.mutex) == 0);
  SessionIoWork follow;
  assert(SessionIoCollectHeld(&owner.slot, &follow));
  assert(follow.events == (EPOLLIN | EPOLLOUT));
  assert(owner.slot.held && !SessionIoCollectHeld(&owner.slot, &follow));
  later = sWork(&owner, EPOLLOUT);
  assert(!SessionIoSubmit(&runtime.queue, &owner.slot, &later));
  assert(SessionIoFinishHeld(&runtime.queue, &owner.slot, &later, true));
  assert(!owner.slot.held && owner.slot.queued && owner.slot.work.events == EPOLLOUT);
  assert(atomic_load(&owner.references) == 1);
  assert(pthread_mutex_unlock(&runtime.mutex) == 0);
  assert(pthread_rwlock_unlock(&owner.lock) == 0);
  assert(sTake(&runtime, 2, &work));
  assert(work.session == &owner && work.events == EPOLLOUT);
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&owner.lock) == 0);
  sDestroy(&owner);
  sDestroy(&other);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

void
SessionIoTestPublishAndHandoff(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession owner, dest, queued;
  SessionIoWork work, snapshot, edge;
  sSocket(&runtime, &owner, 11);
  sSocket(&runtime, &dest, 12);
  sSocket(&runtime, &queued, 13);
  sSubmit(&owner, sWork(&owner, EPOLLIN));
  assert(pthread_mutex_lock(&runtime.mutex) == 0);
  assert(SessionIoTake(&runtime.queue, 1, sAcquire, sRelease, NULL, &work) == SESSION_IO_TAKE_READY);
  assert(owner.slot.held && atomic_load(&owner.references) == 1);
  snapshot = sWork(&owner, 0);
  snapshot.retry_read = true;
  snapshot.not_before_ns = 40;
  edge = sWork(&owner, EPOLLOUT);
  assert(!SessionIoSubmit(&runtime.queue, &owner.slot, &edge));
  assert(atomic_load(&owner.references) == 1 && owner.slot.held);
  assert(SessionIoSubmit(&runtime.queue, &owner.slot, &snapshot) == false);
  assert(SessionIoFinishHeld(&runtime.queue, &owner.slot, &snapshot, true));
  assert(owner.slot.work.events == EPOLLOUT && owner.slot.work.retry_read);
  assert(owner.slot.work.not_before_ns == 40 && atomic_load(&owner.references) == 1);
  assert(pthread_mutex_unlock(&runtime.mutex) == 0);
  assert(pthread_rwlock_unlock(&owner.lock) == 0);
  assert(sTake(&runtime, 40, &work) && work.session == &owner);
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&owner.lock) == 0);

  sSubmit(&dest, sWork(&dest, EPOLLIN));
  assert(pthread_mutex_lock(&runtime.mutex) == 0);
  assert(SessionIoTake(&runtime.queue, 1, sAcquire, sRelease, NULL, &work) == SESSION_IO_TAKE_READY);
  assert(work.session == &dest);
  dest.slot.work.events = EPOLLIN;
  dest.slot.work.retry_read = true;
  assert(pthread_mutex_unlock(&runtime.mutex) == 0);
  sSubmit(&queued, sWork(&queued, EPOLLOUT));
  queued.slot.work.not_before_ns = 80;
  queued.slot.retry_after_ns = 90;
  queued.slot.work.retry_write = true;
  assert(pthread_mutex_lock(&runtime.mutex) == 0);
  assert(SessionIoHandoffHeld(&runtime.queue, &dest.slot, &queued.slot, &work,
      &(SessionIoWork){.session = &queued, .session_id = queued.id, .fd = queued.fd + 1}) == SESSION_IO_HANDOFF_REJECTED);
  assert(dest.slot.held && dest.slot.work.events == EPOLLIN && dest.slot.work.retry_read);
  assert(queued.slot.queued && !queued.slot.held);
  assert(queued.slot.work.events == EPOLLOUT && queued.slot.work.not_before_ns == 80);
  assert(queued.slot.retry_after_ns == 90 && queued.slot.work.retry_write);
  assert(atomic_load(&dest.references) == 1 && atomic_load(&queued.references) == 1);
  assert(SessionIoHandoffHeld(&runtime.queue, &dest.slot, &queued.slot, &(SessionIoWork){.session = work.session, .session_id = work.session_id, .fd = dest.fd + 5},
      &(SessionIoWork){.session = &queued, .session_id = queued.id, .fd = queued.fd}) == SESSION_IO_HANDOFF_REJECTED);
  assert(queued.slot.queued && queued.slot.work.events == EPOLLOUT && queued.slot.retry_after_ns == 90);
  assert(SessionIoHandoffHeld(&runtime.queue, &dest.slot, &queued.slot, &work,
      &(SessionIoWork){.session = &queued, .session_id = queued.id, .fd = queued.fd}) == SESSION_IO_HANDOFF_DEST_PIN);
  assert(!dest.slot.held && dest.slot.work.events == 0);
  assert(queued.slot.held && !queued.slot.queued);
  assert(queued.slot.work.events == (EPOLLIN | EPOLLOUT));
  assert(queued.slot.work.not_before_ns == 80 && queued.slot.retry_after_ns == 90);
  assert(queued.slot.work.retry_write && queued.slot.work.session == &queued);
  assert(queued.slot.work.session_id == queued.id && queued.slot.work.fd == queued.fd);
  assert(atomic_load(&queued.references) == 1);
  atomic_fetch_sub(&dest.references, 1);
  assert(pthread_mutex_unlock(&runtime.mutex) == 0);
  assert(pthread_rwlock_unlock(&dest.lock) == 0);
  queued.slot.held = false;
  sRelease(&(SessionIoWork){.session = &queued}, NULL);
  sDestroy(&owner);
  sDestroy(&dest);
  sDestroy(&queued);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

void
SessionIoTestIdleHandoffGeneration(void)
{
  for (int mode = 0; mode < 8; mode++) {
    int source_holder, dest_holder;
    SessionIoQueue queue = {0};
    SessionIoSlot source = {.held = true, .work = {.session = &source_holder,
      .session_id = 11, .fd = 101, .events = EPOLLIN, .retry_read = true}};
    SessionIoSlot dest = {.work = {.session = &dest_holder, .session_id = 12, .fd = 102,
      .not_before_ns = 80}, .retry_after_ns = 90};
    if (mode == 0) dest.work.session_id = 9;
    else if (mode == 1) dest.work.fd = 99;
    else {
      dest.work.session_id = 9;
      dest.work.fd = 99;
    }
    if (mode == 3) {
      dest.queued = true;
      queue.head = queue.tail = &dest;
      queue.count = 1;
    } else if (mode == 4) dest.work.events = EPOLLOUT;
    else if (mode == 5) dest.work.retry_read = true;
    else if (mode == 6) dest.work.retry_write = true;
    else if (mode == 7) {
      dest.held = true;
      dest.work.session_id = 12;
      dest.work.fd = 102;
    }
    SessionIoSlot saved_source, saved_dest;
    SessionIoQueue saved_queue;
    memcpy(&saved_source, &source, sizeof source);
    memcpy(&saved_dest, &dest, sizeof dest);
    memcpy(&saved_queue, &queue, sizeof queue);
    SessionIoHandoff result = SessionIoHandoffHeld(&queue, &source, &dest,
      &saved_source.work, &(SessionIoWork){.session = &dest_holder, .session_id = 12, .fd = 102});
    if (mode < 3) {
      assert(result == SESSION_IO_HANDOFF_MOVED);
      assert(!source.held && !source.work.events && !source.work.retry_read);
      assert(dest.held && !dest.queued && queue.count == 0);
      assert(dest.work.session == &dest_holder && dest.work.session_id == 12 && dest.work.fd == 102);
      assert(dest.work.events == EPOLLIN && dest.work.retry_read && !dest.work.retry_write);
      assert(dest.work.not_before_ns == 80 && dest.retry_after_ns == 90);
    } else {
      assert(result == SESSION_IO_HANDOFF_REJECTED);
      assert(memcmp(&source, &saved_source, sizeof source) == 0);
      assert(memcmp(&dest, &saved_dest, sizeof dest) == 0);
      assert(memcmp(&queue, &saved_queue, sizeof queue) == 0);
    }
  }
}

void
SessionIoTestSocketGenerationReuse(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession session;
  SessionIoWork old, current, resolved, taken;
  void *retired;
  sSocket(&runtime, &session, 11);
  old = sWork(&session, 0);
  assert(pthread_mutex_lock(&runtime.mutex) == 0);
  old.generation = SessionIoRegister(&runtime.queue, &session.slot, &old, &retired);
  assert(old.generation && !retired);
  assert(SessionIoResolve(old.generation, &resolved));
  assert(resolved.session == old.session && resolved.fd == old.fd && resolved.generation == old.generation);
  assert(pthread_mutex_unlock(&runtime.mutex) == 0);
  old.events = EPOLLIN | EPOLLHUP | EPOLLRDHUP;
  sSubmit(&session, old);
  assert(atomic_load(&session.references) == 1);
  assert(pthread_mutex_lock(&runtime.mutex) == 0);
  SessionIoUnregister(&session.slot);
  assert(!SessionIoResolve(old.generation, &resolved));
  current = old;
  current.events = EPOLLIN | EPOLLOUT;
  current.generation = SessionIoRegister(&runtime.queue, &session.slot, &current, &retired);
  assert(current.generation && current.generation != old.generation && retired == &session);
  assert(!runtime.queue.count && !session.slot.held && !session.slot.queued);
  sRelease(&old, NULL);
  assert(SessionIoResolve(current.generation, &resolved));
  assert(resolved.session_id == old.session_id && resolved.fd == old.fd);
  assert(!SessionIoResolve(old.generation, &resolved));
  assert(!SessionIoSubmit(&runtime.queue, &session.slot, &old));
  assert(!session.slot.work.events && !runtime.queue.count);
  assert(pthread_mutex_unlock(&runtime.mutex) == 0);
  sSubmit(&session, current);
  assert(sTake(&runtime, 1, &taken));
  assert(taken.events == (EPOLLIN | EPOLLOUT) && taken.generation == current.generation);
  sDispatch(&taken);
  assert(!session.closed && session.reads == 1 && session.writes == 1);
  assert(pthread_mutex_lock(&runtime.mutex) == 0);
  SessionIoUnregister(&session.slot);
  assert(!SessionIoResolve(current.generation, &resolved));
  assert(pthread_mutex_unlock(&runtime.mutex) == 0);
  sDestroy(&session);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

void
SessionIoTestGenerationHandoffAndCompletion(void)
{
  for (int queued = 0; queued < 2; queued++) {
    int source_holder, dest_holder;
    SessionIoQueue queue = {0};
    SessionIoSlot source = {0}, dest = {0};
    SessionIoWork source_id = {.session = &source_holder, .session_id = 11, .fd = 101};
    SessionIoWork dest_id = {.session = &dest_holder, .session_id = 12, .fd = 102};
    void *retired;
    source_id.generation = SessionIoRegister(&queue, &source, &source_id, &retired);
    assert(source_id.generation && !retired);
    dest_id.generation = SessionIoRegister(&queue, &dest, &dest_id, &retired);
    assert(dest_id.generation && !retired);
    source.held = true;
    source.work.events = EPOLLIN;
    source.work.retry_read = true;
    dest.held = true;
    dest.work.events = EPOLLHUP | EPOLLRDHUP;
    SessionIoWork old_dest = dest.work;
    SessionIoUnregister(&dest);
    dest_id.generation = SessionIoRegister(&queue, &dest, &dest_id, &retired);
    assert(dest_id.generation != old_dest.generation && !retired);
    dest_id.events = EPOLLOUT;
    dest_id.retry_write = true;
    dest_id.not_before_ns = 80;
    if (queued) assert(SessionIoSubmit(&queue, &dest, &dest_id));
    else dest.work = dest_id;
    dest.retry_after_ns = 90;
    SessionIoWork wrong_source = source_id;
    wrong_source.generation++;
    SessionIoSlot saved_source = source, saved_dest = dest;
    assert(SessionIoHandoffHeld(&queue, &source, &dest, &wrong_source, &dest_id) == SESSION_IO_HANDOFF_REJECTED);
    assert(!memcmp(&source, &saved_source, sizeof source) && !memcmp(&dest, &saved_dest, sizeof dest));
    assert(SessionIoHandoffHeld(&queue, &source, &dest, &source_id, &old_dest) == SESSION_IO_HANDOFF_REJECTED);
    assert(!memcmp(&source, &saved_source, sizeof source) && !memcmp(&dest, &saved_dest, sizeof dest));
    SessionIoUnregister(&source);
    assert(SessionIoHandoffHeld(&queue, &source, &dest, &source_id, &dest_id) ==
      (queued ? SESSION_IO_HANDOFF_DEST_PIN : SESSION_IO_HANDOFF_MOVED));
    assert(dest.held && !dest.queued && !queue.count && !source.held);
    assert(dest.work.events == (EPOLLIN | EPOLLOUT));
    assert(dest.work.retry_read && dest.work.retry_write);
    assert(dest.work.not_before_ns == 80 && dest.retry_after_ns == 90);
    saved_dest = dest;
    assert(!SessionIoFinishHeld(&queue, &dest, &old_dest, false));
    assert(!SessionIoFinishHeld(&queue, &dest, &old_dest, true));
    assert(!memcmp(&dest, &saved_dest, sizeof dest) && !queue.count);
    SessionIoWork follow;
    assert(SessionIoCollectHeld(&dest, &follow));
    assert(follow.events == (EPOLLIN | EPOLLOUT) && follow.generation == dest_id.generation);
    assert(!SessionIoFinishHeld(&queue, &dest, &dest_id, false));
    SessionIoUnregister(&dest);
    assert(!dest.held && !dest.queued);
  }
}

void
SessionIoTestCompletionOwnership(void)
{
  for (int mode = 0; mode < 10; mode++) {
    int holder, other;
    SessionIoQueue queue = {0}, saved_queue;
    SessionIoSlot slot = {.held = true, .work = {.session = &holder,
      .session_id = 11, .fd = 101, .events = EPOLLOUT, .not_before_ns = 30}, .retry_after_ns = 40};
    SessionIoWork snapshot = {.session = &holder, .session_id = 11, .fd = 101,
      .retry_read = true, .not_before_ns = 50};
    if (mode == 1 || mode == 7) slot.work.session_id++;
    else if (mode == 2) slot.work.fd++;
    else if (mode == 3) slot.work.session = &other;
    else if (mode == 4 || mode == 9) slot.held = false;
    else if (mode == 5) {
      slot.queued = true;
      queue.head = queue.tail = &slot;
      queue.count = 1;
    } else if (mode == 8) {
      slot.work.events = 0;
      snapshot.retry_read = false;
    }
    SessionIoSlot saved_slot;
    memcpy(&saved_slot, &slot, sizeof slot);
    memcpy(&saved_queue, &queue, sizeof queue);
    bool publish = mode != 6 && mode != 7 && mode != 9;
    bool appended = SessionIoFinishHeld(&queue, &slot, &snapshot, publish);
    if (mode == 0) {
      assert(appended && !slot.held && slot.queued);
      assert(queue.head == &slot && queue.tail == &slot && queue.count == 1);
      assert(slot.work.events == EPOLLOUT && slot.work.retry_read);
      assert(slot.work.not_before_ns == 50);
    } else if (mode == 6 || mode == 8) {
      assert(!appended && !slot.held && !slot.queued && queue.count == 0);
      assert(!slot.work.events && !slot.work.retry_read && !slot.work.retry_write);
      if (mode == 6) assert(!slot.work.not_before_ns && !slot.retry_after_ns);
    } else {
      assert(!appended);
      assert(memcmp(&slot, &saved_slot, sizeof slot) == 0);
      assert(memcmp(&queue, &saved_queue, sizeof queue) == 0);
    }
  }
}

void
SessionIoTestSocketDispatch(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession session;
  sSocket(&runtime, &session, 1);
  int poller = epoll_create1(0);
  assert(poller >= 0);
  struct epoll_event interest = {.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP};
  assert(epoll_ctl(poller, EPOLL_CTL_ADD, session.fd, &interest) == 0);
  struct epoll_event event;
  assert(epoll_wait(poller, &event, 1, 1000) == 1 && event.events == EPOLLOUT);
  sSubmit(&session, sWork(&session, event.events));
  memset(&event, 0, sizeof(event));
  SessionIoWork work;
  assert(sTake(&runtime, 1, &work));
  sDispatch(&work);
  assert(session.reads == 0 && session.writes == 1);
  char buffer[512];
  assert(read(session.peer, buffer, sizeof(buffer)) == sizeof(response));
  assert(write(session.peer, "xxx", 3) == 3);
  assert(epoll_wait(poller, &event, 1, 1000) == 1);
  assert(event.events & EPOLLIN);
  sSubmit(&session, sWork(&session, event.events | EPOLLOUT));
  assert(sTake(&runtime, 2, &work));
  sDispatch(&work);
  assert(session.bytes == 3 && session.writes == 2);
  assert(read(session.peer, buffer, sizeof(buffer)) == sizeof(response));
  memset(buffer, 'x', sizeof(buffer));
  while (write(session.fd, buffer, sizeof(buffer)) > 0) {}
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  assert(epoll_wait(poller, &event, 1, 0) == 0);
  while (read(session.peer, buffer, sizeof(buffer)) > 0) {}
  assert(epoll_wait(poller, &event, 1, 1000) == 1 && (event.events & EPOLLOUT));
  sSubmit(&session, sWork(&session, event.events));
  assert(sTake(&runtime, 3, &work));
  sDispatch(&work);
  assert(session.reads == 1 && session.writes == 3);
  assert(read(session.peer, buffer, sizeof(buffer)) == sizeof(response));
  work = sWork(&session, 0);
  sSubmit(&session, work);
  assert(sTake(&runtime, 4, &work));
  sDispatch(&work);
  assert(session.reads == 1 && session.writes == 3);
  work.retry_read = true;
  sSubmit(&session, work);
  assert(sTake(&runtime, 5, &work));
  sDispatch(&work);
  assert(session.reads == 2 && session.writes == 3);
  work = sWork(&session, 0);
  work.retry_write = true;
  sSubmit(&session, work);
  assert(sTake(&runtime, 6, &work));
  sDispatch(&work);
  assert(session.reads == 2 && session.writes == 4);
  work = sWork(&session, EPOLLERR | EPOLLOUT);
  sSubmit(&session, work);
  assert(sTake(&runtime, 7, &work));
  sDispatch(&work);
  assert(session.closed && session.writes == 4 && session.reads == 2);
  assert(close(poller) == 0);
  sDestroy(&session);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

void
SessionIoTestTerminalDrain(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  for (int mode = 0; mode < 3; mode++) {
    TestSession session;
    sSocket(&runtime, &session, mode + 1);
    int poller = epoll_create1(0);
    assert(poller >= 0);
    struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET};
    assert(epoll_ctl(poller, EPOLL_CTL_ADD, session.fd, &event) == 0);
    char payload[8192];
    memset(payload, 'x', sizeof(payload));
    assert(write(session.peer, payload, sizeof(payload)) == sizeof(payload));
    if (mode == 0) assert(shutdown(session.peer, SHUT_WR) == 0);
    else {
      assert(close(session.peer) == 0);
      session.peer = -1;
    }
    assert(epoll_wait(poller, &event, 1, 1000) == 1);
    assert(event.events & (mode == 0 ? EPOLLRDHUP : EPOLLHUP));
    if (mode == 2) event.events |= EPOLLERR;
    sSubmit(&session, sWork(&session, event.events));
    size_t iterations = 0;
    SessionIoWork work;
    while (sTake(&runtime, ++iterations * 1000000, &work)) {
      sDispatch(&work);
      assert(iterations < 100);
    }
    assert(session.closed && session.bytes == sizeof(payload));
    assert(session.reads > 1 && session.writes == 0);
    assert(epoll_wait(poller, &event, 1, 0) == 0);
    assert(close(poller) == 0);
    sDestroy(&session);
  }
  TestSession stalled;
  sSocket(&runtime, &stalled, 4);
  sSubmit(&stalled, sWork(&stalled, EPOLLHUP | EPOLLOUT));
  SessionIoWork work;
  assert(sTake(&runtime, 1, &work));
  sDispatch(&work);
  assert(stalled.closed && stalled.reads == 1 && !stalled.writes);
  sDestroy(&stalled);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

void
SessionIoTestRecoverableRead(void)
{
  const unsigned codes[] = {RESCODE_USER_RATELIMIT_EXCEEDED, RESCODE_IO_SESSIONSTATE, RESCODE_IO_POLL};
  for (unsigned mode = 0; mode < sizeof(codes)/sizeof(codes[0]); mode++) {
    TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
    TestSession session, other;
    sSocket(&runtime, &session, 1);
    sSocket(&runtime, &other, 2);
    int poller = epoll_create1(0);
    assert(poller >= 0);
    struct epoll_event event = {.events = EPOLLIN | EPOLLOUT | EPOLLET};
    assert(epoll_ctl(poller, EPOLL_CTL_ADD, session.fd, &event) == 0);
    assert(write(session.peer, "x", 1) == 1);
    assert(epoll_wait(poller, &event, 1, 1000) == 1);
    assert((event.events & (EPOLLIN | EPOLLOUT)) == (EPOLLIN | EPOLLOUT));
    session.read_failures = 2;
    session.read_error_code = codes[mode];
    session.now_ns = 1;
    session.retry_until_ns = 100;
    sSubmit(&session, sWork(&session, event.events));
    SessionIoWork work;
    assert(sTake(&runtime, 1, &work));
    sSubmit(&session, sWork(&session, EPOLLOUT));
    assert(atomic_load(&session.references) == 2);
    sDispatch(&work);
    assert(session.reads == 1 && !session.bytes && !session.writes);
    assert(session.read_result.result_code == codes[mode]);
    assert(atomic_load(&session.references) == 1 && runtime.queue.count == 1);
    assert(session.slot.work.retry_read && session.slot.work.retry_write);
    sSubmit(&session, sWork(&session, EPOLLOUT));
    assert(!sTake(&runtime, 99, &work));
    assert(runtime.queue.retry_at_ns == 100);
    assert(sTake(&runtime, 100, &work));
    session.now_ns = 100;
    session.retry_until_ns = 200;
    sDispatch(&work);
    assert(session.reads == 2 && !session.bytes && !session.writes);
    assert(session.read_result.result_code == codes[mode]);
    assert(atomic_load(&session.references) == 1);
    sSubmit(&other, sWork(&other, EPOLLOUT));
    assert(sTake(&runtime, 150, &work) && work.session == &other);
    sDispatch(&work);
    assert(other.writes == 1 && !other.reads);
    assert(!sTake(&runtime, 199, &work));
    assert(sTake(&runtime, 200, &work));
    sDispatch(&work);
    assert(session.bytes == 1 && session.reads == 3 && session.writes == 1);
    assert(!session.closed && runtime.queue.count == 0 && atomic_load(&session.references) == 0);
    assert(epoll_wait(poller, &event, 1, 0) == 0);
    assert(close(poller) == 0);
    sDestroy(&session);
    sDestroy(&other);
    assert(pthread_mutex_destroy(&runtime.mutex) == 0);
  }
}

void
SessionIoTestDeferredTerminalRead(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession session;
  sSocket(&runtime, &session, 1);
  assert(write(session.peer, "x", 1) == 1);
  assert(close(session.peer) == 0);
  session.peer = -1;
  session.read_failures = 1;
  session.read_error_code = RESCODE_USER_RATELIMIT_EXCEEDED;
  session.now_ns = 1;
  session.retry_until_ns = 100;
  sSubmit(&session, sWork(&session, EPOLLIN | EPOLLOUT | EPOLLHUP));
  SessionIoWork work;
  assert(sTake(&runtime, 1, &work));
  sDispatch(&work);
  assert(session.slot.work.events == EPOLLHUP && session.slot.work.retry_read && !session.slot.work.retry_write);
  assert(!sTake(&runtime, 99, &work));
  assert(sTake(&runtime, 100, &work));
  sDispatch(&work);
  assert(sTake(&runtime, 101, &work));
  sDispatch(&work);
  assert(session.bytes == 1 && session.closed && !session.writes && !runtime.queue.count);
  sDestroy(&session);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

void
SessionIoTestReadClassification(void)
{
  UFSRVResult result = {.result_type = RESULT_TYPE_ERR, .result_code = RESCODE_USER_RATELIMIT_EXCEEDED};
  SessionIoReadResult read = SessionIoReadOutcome(&result, true, false, 1, 500);
  assert(read.live && read.deferred && !read.progress && read.retry_at_ns == 500);
  result.result_code = RESCODE_IO_SESSIONSTATE;
  read = SessionIoReadOutcome(&result, true, false, 1, 0);
  assert(read.live && read.deferred && read.retry_at_ns == 100000001);
  read = SessionIoReadOutcome(&result, true, false, UINT64_MAX - 1, 0);
  assert(read.deferred && read.retry_at_ns == UINT64_MAX);
  assert(!SessionIoReadOutcome(&result, false, false, 1, 0).live);
  assert(!SessionIoReadOutcome(NULL, true, false, 1, 0).live);
  const unsigned closed[] = {RESCODE_IO_CONNECTIONCLOSED, RESCODE_SESN_SOFTSPENDED, RESCODE_SESN_HARDSPENDED};
  for (unsigned i = 0; i < sizeof(closed)/sizeof(closed[0]); i++) {
    result.result_code = closed[i];
    result.result_type = RESULT_TYPE_SUCCESS;
    read = SessionIoReadOutcome(&result, true, true, 1, 500);
    assert(!read.live && !read.deferred && !read.retry_at_ns);
  }
  result.result_code = RESCODE_SERVICED;
  const unsigned successful[] = {RESULT_TYPE_SUCCESS, RESULT_TYPE_SUCCESS_NOOP, RESULT_TYPE_SUCCESS_INFORM_USER};
  for (unsigned i = 0; i < sizeof(successful)/sizeof(successful[0]); i++) {
    result.result_type = successful[i];
    read = SessionIoReadOutcome(&result, true, true, 1, 500);
    assert(read.live && read.progress && !read.deferred && !read.retry_at_ns);
  }
}

void
SessionIoTestExplicitWriteRetry(void)
{
  SessionIoWork work = {0};
  SessionIoContinuation(&work, EPOLLOUT, false);
  assert(!work.events && !work.retry_read && work.retry_write);
}

void
SessionIoTestContinuations(void)
{
  SessionIoWork work = {0};
  SessionIoContinuation(&work, 0, false);
  assert(!work.events && !work.retry_read && !work.retry_write);
  SessionIoContinuation(&work, EPOLLIN, false);
  assert(!work.events && work.retry_read && !work.retry_write);
  SessionIoContinuation(&work, 0, true);
  assert(!work.events && !work.retry_read && work.retry_write);
  SessionIoContinuation(&work, EPOLLIN, true);
  assert(!work.events && work.retry_read && work.retry_write);
  SessionIoContinuation(&work, EPOLLIN | EPOLLRDHUP, true);
  assert(work.events == EPOLLRDHUP && work.retry_read && !work.retry_write);
  SessionIoContinuation(&work, EPOLLIN | EPOLLHUP | EPOLLERR, true);
  assert(work.events == (EPOLLHUP | EPOLLERR) && work.retry_read && !work.retry_write);
}

void
SessionIoTestGenerationAndCancel(void)
{
  TestRuntime runtime = {.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession session;
  sSocket(&runtime, &session, 10);
  SessionIoWork old = sWork(&session, EPOLLHUP);
  old.not_before_ns = UINT64_MAX;
  sSubmit(&session, old);
  session.id = 11;
  SessionIoWork next = sWork(&session, EPOLLIN);
  sSubmit(&session, next);
  assert(runtime.queue.count == 1 && atomic_load(&session.references) == 1);
  SessionIoWork work;
  assert(sTake(&runtime, 1, &work));
  assert(work.session_id == 11 && work.events == EPOLLIN);
  sRelease(&work, NULL);
  assert(pthread_rwlock_unlock(&session.lock) == 0);
  old = sWork(&session, EPOLLHUP);
  old.fd++;
  sSubmit(&session, old);
  sSubmit(&session, sWork(&session, EPOLLIN));
  assert(session.slot.work.events == EPOLLIN);
  SessionIoCancel(&runtime.queue, sRelease, NULL);
  assert(runtime.queue.count == 0);
  sDestroy(&session);
  assert(pthread_mutex_destroy(&runtime.mutex) == 0);
}

typedef struct StressRuntime {
  TestRuntime runtime;
  atomic_bool stop;
  atomic_uint completed;
} StressRuntime;

static uint64_t
sNow(void)
{
  struct timespec now;
  assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void *
sStressWorker(void *context)
{
  StressRuntime *stress = context;
  while (!atomic_load(&stress->stop)) {
    SessionIoWork work;
    if (sTake(&stress->runtime, sNow(), &work)) {
      assert(work.events == EPOLLIN);
      sDispatch(&work);
      atomic_fetch_add(&stress->completed, 1);
    } else sched_yield();
  }
  return NULL;
}

void
SessionIoTestConcurrent(void)
{
  enum { COUNT = 64, ROUNDS = 128, WORKERS = 4 };
  StressRuntime stress = {.runtime.mutex = PTHREAD_MUTEX_INITIALIZER};
  TestSession sessions[COUNT];
  pthread_t workers[WORKERS];
  int poller = epoll_create1(0);
  assert(poller >= 0);
  for (unsigned i = 0; i < COUNT; i++) {
    sSocket(&stress.runtime, &sessions[i], i + 1);
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.ptr = &sessions[i]};
    assert(epoll_ctl(poller, EPOLL_CTL_ADD, sessions[i].fd, &event) == 0);
  }
  for (unsigned i = 0; i < WORKERS; i++)
    assert(pthread_create(&workers[i], NULL, sStressWorker, &stress) == 0);
  for (unsigned round = 0; round < ROUNDS; round++) {
    uint64_t deadline = sNow() + 5000000000ULL;
    assert(pthread_rwlock_wrlock(&sessions[0].lock) == 0);
    for (unsigned i = 0; i < COUNT; i++) assert(write(sessions[i].peer, "x", 1) == 1);
    unsigned received = 0;
    while (received < COUNT) {
      struct epoll_event events[COUNT];
      int count = epoll_wait(poller, events, COUNT, 1000);
      assert(count > 0 && sNow() < deadline);
      for (int i = 0; i < count; i++) {
        TestSession *session = events[i].data.ptr;
        sSubmit(session, sWork(session, events[i].events));
      }
      received += count;
    }
    while (atomic_load(&stress.completed) < round * COUNT + COUNT - 1) {
      assert(sNow() < deadline);
      sched_yield();
    }
    assert(atomic_load(&sessions[0].references) == 1);
    assert(pthread_rwlock_unlock(&sessions[0].lock) == 0);
    while (atomic_load(&stress.completed) < (round + 1) * COUNT) {
      assert(sNow() < deadline);
      sched_yield();
    }
  }
  atomic_store(&stress.stop, true);
  for (unsigned i = 0; i < WORKERS; i++) assert(pthread_join(workers[i], NULL) == 0);
  assert(stress.runtime.queue.count == 0);
  for (unsigned i = 0; i < COUNT; i++) {
    assert(sessions[i].bytes == ROUNDS && sessions[i].reads == ROUNDS);
    sDestroy(&sessions[i]);
  }
  assert(close(poller) == 0);
  assert(pthread_mutex_destroy(&stress.runtime.mutex) == 0);
}

#ifdef SESSION_IO_STANDALONE
#include <stdio.h>
int main(void)
{
  SessionIoTestStatusFlags();
  SessionIoTestPressure();
  SessionIoTestDeferral();
  SessionIoTestBusyBleed();
  SessionIoTestOwnerKeepsLaterEdges();
  SessionIoTestPublishAndHandoff();
  SessionIoTestIdleHandoffGeneration();
  SessionIoTestCompletionOwnership();
  SessionIoTestSocketGenerationReuse();
  SessionIoTestGenerationHandoffAndCompletion();
  SessionIoTestSocketDispatch();
  SessionIoTestTerminalDrain();
  SessionIoTestGenerationAndCancel();
  SessionIoTestConcurrent();
  SessionIoTestContinuations();
  SessionIoTestExplicitWriteRetry();
  SessionIoTestRecoverableRead();
  SessionIoTestDeferredTerminalRead();
  SessionIoTestReadClassification();
  puts("PASS production queue and dispatcher: allocation failure, 4096 pending sessions, lock deferral, suspension, epoll readiness, backpressure, terminal drain, identity replacement, reference balance, four-worker concurrent epoll stress (8192 deliveries), recoverable read errors, timed retries, explicit writable continuation");
  return 0;
}
#endif
