#include <main_ufnetcorelib.h>
#include <ufnet_core/protocol/protocol_io.h>
#include <ufnet_core/protocol.h>
#include <assert.h>
#include <sys/socket.h>

static const Protocol protocol;
static unsigned suspended;

const Protocol *const ProtocolGet(unsigned id) { return &protocol; }
int RemoveSessionToMonitoredWorkEvents(InstanceHolderForSession *holder) { return 0; }
int SuspendSession(InstanceHolderForSession *holder, unsigned flags)
{
  Session *session = SessionOffInstanceHolder(holder);
  assert(pthread_mutex_trylock(&session->message_queue_in.mutex) == 0);
  assert(pthread_mutex_unlock(&session->message_queue_in.mutex) == 0);
  ++suspended;
  return 0;
}
void DestructSocketMessage(SocketMessage *message)
{
  if (message->raw_msg_size > 0) free(message->_raw_msg);
  memset(message, 0, sizeof *message);
}
void ProtocolIoTestEofLockOrder(void)
{
  Session session = {0};
  InstanceHolderForSession holder = {.holder.instance = &session};
  session.ssptr = calloc(1, sizeof *session.ssptr);
  session.protocol_type_data = (void *)&protocol;
  assert(pthread_mutex_init(&session.message_queue_in.mutex, NULL) == 0);
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
  SESSION_SOCKETFD((&session)) = sockets[0];
  SESSION_SOCKETBLOCKSZ((&session)) = 64;
  assert(close(sockets[1]) == 0);
  suspended = 0;
  assert(ReadFromSocket(&holder, SESSION_INSOCKMSG_TRANS_PTR((&session)), SOCKMSG_READSOCKET) == -1);
  assert(suspended == 1);
  assert(pthread_mutex_trylock(&session.message_queue_in.mutex) == 0);
  assert(pthread_mutex_unlock(&session.message_queue_in.mutex) == 0);
  assert(pthread_mutex_destroy(&session.message_queue_in.mutex) == 0);
  close(sockets[0]);
  free(session.ssptr);
}
unsigned UfsrvConfigMainListenerProtocolId(void) { return 0; }
size_t UfsrvConfigBufferSizesIncomingBufferSize(void) { return 64; }
size_t UfsrvConfigBufferSizesIncomingBufferMaxsize(void) { return 65536; }

static void *sBleed(void *argument)
{
  Session *session = argument;
  assert(pthread_rwlock_trywrlock(&session->session_events.rwlock) == EBUSY);
  assert(pthread_mutex_lock(&session->message_queue_in.mutex) == 0);
  assert(ReadIncomingQueueLocked(session) == 64);
  assert(pthread_mutex_unlock(&session->message_queue_in.mutex) == 0);
  return NULL;
}

void ProtocolIoTestBusyQueue(void)
{
  Session session = {0};
  session.ssptr = calloc(1, sizeof *session.ssptr);
  assert(pthread_mutex_init(&session.message_queue_in.mutex, NULL) == 0);
  assert(pthread_rwlock_init(&session.session_events.rwlock, NULL) == 0);
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets) == 0);
  SESSION_SOCKETFD((&session)) = sockets[0];
  SESSION_SOCKETBLOCKSZ((&session)) = 64;
  unsigned char payload[129];
  for (size_t i = 0; i < sizeof payload; ++i) payload[i] = i;
  assert(write(sockets[1], payload, sizeof payload) == sizeof payload);
  assert(pthread_rwlock_wrlock(&session.session_events.rwlock) == 0);
  pthread_t reader;
  assert(pthread_create(&reader, NULL, sBleed, &session) == 0);
  assert(pthread_join(reader, NULL) == 0);
  assert(session.message_queue_in.queue.nEntries == 1);
  assert(pthread_rwlock_unlock(&session.session_events.rwlock) == 0);
  assert(pthread_mutex_lock(&session.message_queue_in.mutex) == 0);
  assert(ReadIncomingQueueLocked(&session) == 64);
  assert(ReadIncomingQueueLocked(&session) == 1);
  assert(ReadIncomingQueueLocked(&session) == 0);
  assert(session.message_queue_in.queue.nEntries == 3);
  UFSRVResult result = {0};
  ConsolidateSocketMessageQueue(&session, SOCKMSG_CONSOLIDATE_INSESSION, &result);
  assert(result.result_type == RESULT_TYPE_SUCCESS);
  assert(session.ssptr->socket_msg.raw_msg_size == sizeof payload);
  assert(memcmp(session.ssptr->socket_msg._raw_msg, payload, sizeof payload) == 0);
  DestructSocketMessage(&session.ssptr->socket_msg);
  assert(close(sockets[1]) == 0);
  assert(ReadIncomingQueueLocked(&session) == SOCKMSG_SMERRNO_PEERCLOSED);
  ConsolidateSocketMessageQueue(&session, SOCKMSG_CONSOLIDATE_INSESSION, &result);
  assert(session.ssptr->socket_msg.sm_errno == SOCKMSG_SMERRNO_PEERCLOSED);
  free(session.ssptr->socket_msg._raw_msg);
  assert(pthread_mutex_unlock(&session.message_queue_in.mutex) == 0);
  assert(pthread_mutex_destroy(&session.message_queue_in.mutex) == 0);
  assert(pthread_rwlock_destroy(&session.session_events.rwlock) == 0);
  close(sockets[0]);
  free(session.ssptr);
}
