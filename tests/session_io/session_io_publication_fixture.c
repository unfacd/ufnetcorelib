#include <main_ufnetcorelib.h>
#include <session_io_work.h>
#include <ufnet_msgcore/session.h>
#include <ufnet_msgcore/type_providers/session_provider.h>
#include <ufnet_core/delegator_session_worker_thread.h>
#include <assert.h>
#include <stdatomic.h>
#include <sys/epoll.h>
#include <sched.h>

static SessionsDelegator test_delegator;
SessionsDelegator *const sessions_delegator_ptr = &test_delegator;
__thread ThreadContext ufsrv_thread_context;
static InstanceHolderForSession *expected_holder;
static atomic_int references, signals, waiting, delivered;
static int lock_failure;
static SessionIoWork received;

int WorkQueueLock(SessionsDelegator *delegator, int try_flag)
{
  if (lock_failure) return EINVAL;
  return pthread_mutex_lock(&delegator->work_queue_mutex);
}

int WorkQueueUnLock(SessionsDelegator *delegator)
{
  return pthread_mutex_unlock(&delegator->work_queue_mutex);
}

void SessionIncrementReference(InstanceHolderForSession *holder, int count)
{
  assert(holder == expected_holder);
  atomic_fetch_add(&references, count);
}

void SessionDecrementReference(InstanceHolderForSession *holder, int count)
{
  assert(holder == expected_holder);
  assert(atomic_fetch_sub(&references, count) >= count);
}

int __real_pthread_cond_signal(pthread_cond_t *condition);
int __wrap_pthread_cond_signal(pthread_cond_t *condition)
{
  if (condition == &test_delegator.queue_not_empty_cond) atomic_fetch_add(&signals, 1);
  return __real_pthread_cond_signal(condition);
}

#include <session_io_publication_under_test.inc>

static void
sSetupPublication(Session *session, InstanceHolderForSession *holder)
{
  memset(session, 0, sizeof *session);
  SetInstance(holder, session);
  expected_holder = holder;
  atomic_store(&references, 1);
  atomic_store(&signals, 0);
  atomic_store(&waiting, 0);
  atomic_store(&delivered, 0);
  lock_failure = 0;
  session_io_queue = (SessionIoQueue){0};
  test_delegator.up_status = 1;
  assert(pthread_mutex_init(&test_delegator.work_queue_mutex, NULL) == 0);
  assert(pthread_cond_init(&test_delegator.queue_not_empty_cond, NULL) == 0);
  assert(pthread_rwlock_init(&session->session_events.rwlock, NULL) == 0);
  session->io_work.held = true;
  session->io_work.work = (SessionIoWork){.session = holder, .session_id = 11, .fd = 101};
}

static void
sDestroyPublication(Session *session)
{
  assert(pthread_rwlock_destroy(&session->session_events.rwlock) == 0);
  assert(pthread_cond_destroy(&test_delegator.queue_not_empty_cond) == 0);
  assert(pthread_mutex_destroy(&test_delegator.work_queue_mutex) == 0);
}

static SessionIoAcquire
sAcquirePublished(const SessionIoWork *work, void *context)
{
  Session *session = context;
  assert(atomic_load(&references) == 1);
  assert(pthread_rwlock_trywrlock(&session->session_events.rwlock) == 0);
  return SESSION_IO_ACQUIRED;
}

static void
sUnexpectedRelease(const SessionIoWork *work, void *context)
{
  assert(false);
}

static void *
sPublicationWaiter(void *argument)
{
  Session *session = argument;
  assert(WorkQueueLock(&test_delegator, 0) == 0);
  atomic_store(&waiting, 1);
  while (!session_io_queue.count)
    assert(pthread_cond_wait(&test_delegator.queue_not_empty_cond, &test_delegator.work_queue_mutex) == 0);
  assert(SessionIoTake(&session_io_queue, UINT64_MAX, sAcquirePublished, sUnexpectedRelease,
                       session, &received) == SESSION_IO_TAKE_READY);
  assert(WorkQueueUnLock(&test_delegator) == 0);
  assert(received.events == (EPOLLIN | EPOLLOUT));
  assert(received.retry_read && received.retry_write && received.not_before_ns == 50);
  assert(pthread_rwlock_unlock(&session->session_events.rwlock) == 0);
  sAbandonHeld(received.session, &received);
  atomic_store(&delivered, 1);
  return NULL;
}

static void *
sEdgeAfterUnlock(void *argument)
{
  Session *session = argument;
  assert(pthread_rwlock_trywrlock(&session->session_events.rwlock) == 0);
  assert(pthread_rwlock_unlock(&session->session_events.rwlock) == 0);
  SessionIoWork edge = {.session = expected_holder, .session_id = 11, .fd = 101, .events = EPOLLOUT};
  sRestoreReadableIoWork(session, &edge);
  return NULL;
}

void
SessionIoPublicationTestUnlockWindow(void)
{
  Session session;
  InstanceHolderForSession holder;
  sSetupPublication(&session, &holder);
  assert(pthread_rwlock_wrlock(&session.session_events.rwlock) == 0);
  pthread_t waiter, producer;
  assert(pthread_create(&waiter, NULL, sPublicationWaiter, &session) == 0);
  while (!atomic_load(&waiting)) sched_yield();
  SessionIoWork snapshot = {.session = &holder, .session_id = 11, .fd = 101,
    .retry_read = true, .retry_write = true, .not_before_ns = 50};
  SessionIoWork edge = snapshot;
  edge.events = EPOLLIN;
  edge.retry_read = edge.retry_write = false;
  edge.not_before_ns = 0;
  sRestoreReadableIoWork(&session, &edge);
  assert(atomic_load(&signals) == 0 && atomic_load(&references) == 1);
  assert(session.io_work.held && !session.io_work.queued);
  assert(pthread_rwlock_unlock(&session.session_events.rwlock) == 0);
  assert(pthread_create(&producer, NULL, sEdgeAfterUnlock, &session) == 0);
  assert(pthread_join(producer, NULL) == 0);
  assert(atomic_load(&signals) == 0 && atomic_load(&delivered) == 0);
  sFinishHeld(&holder, &snapshot);
  assert(pthread_join(waiter, NULL) == 0);
  assert(atomic_load(&signals) == 1 && atomic_load(&delivered) == 1);
  assert(atomic_load(&references) == 0 && session_io_queue.count == 0);
  assert(!session.io_work.held && !session.io_work.queued);
  sDestroyPublication(&session);
}

void
SessionIoPublicationTestNoAppend(void)
{
  for (int mode = 0; mode < 7; mode++) {
    Session session;
    InstanceHolderForSession holder;
    sSetupPublication(&session, &holder);
    SessionIoWork snapshot = {.session = &holder, .session_id = 11, .fd = 101};
    if (mode == 1) session.io_work.work.session_id++;
    else if (mode == 2) session.io_work.work.fd++;
    else if (mode == 3) session.io_work.held = false;
    else if (mode == 4) {
      test_delegator.up_status = 0;
      session.io_work.work.events = EPOLLIN | EPOLLOUT;
    } else if (mode == 5) lock_failure = 1;
    else if (mode == 6) {
      session.io_work.queued = true;
      session_io_queue.head = session_io_queue.tail = &session.io_work;
      session_io_queue.count = 1;
    }
    SessionIoSlot saved;
    memcpy(&saved, &session.io_work, sizeof saved);
    sFinishHeld(&holder, &snapshot);
    assert(atomic_load(&signals) == 0 && atomic_load(&references) == 0);
    if (mode == 0 || mode == 4) {
      assert(!session.io_work.held && !session.io_work.queued);
      assert(!session.io_work.work.events);
    } else assert(memcmp(&saved, &session.io_work, sizeof saved) == 0);
    lock_failure = 0;
    session_io_queue = (SessionIoQueue){0};
    sDestroyPublication(&session);
  }
}
