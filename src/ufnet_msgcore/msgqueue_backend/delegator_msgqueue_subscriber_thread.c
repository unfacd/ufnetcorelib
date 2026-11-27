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
#include <config_ufnetcorelib.h>
#endif

#include <sys/prctl.h>//for naming thread
#include "ufnet_msgcore/msgqueue_backend/delegator_msgqueue_subscriber_thread.h"

#include <adt/adt_minheap.h>
#include <scheduled_jobs/scheduled_jobs_type.h>

#include <ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue.h>
#include <ufsrvcorelib/ufsrv_core/protocol/protocol.h>
#include <ufnet_msgcore/msgqueue_backend/ufsrv_command_broadcast_type.h>

#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_shim.h>
#include <ufsrv_core/delegator_sessions/delegator_session_worker_thread.h>

#include "hiredis.h"
#include <ufsrvcorelib/ufsrv_core/jobworkers/worker_job_descriptor_type.h>

static pthread_t 					th_msgqueue_sub;
UfsrvCommandBroadcast     *GetBroadcastDescriptorByName(const char *topic);

static void *ThreadMessageQueueSub(void *);
static int _CreateMessageQueueTopics(MessageQueueBackend *, const Protocol *);

typedef struct {
  MessageQueueBackend *backend;
  const Protocol *protocol;
} MessageQueueSubscriberStart;

static char *subpub_verb = "message";

/**
 * 	@brief arguments are assumed to be dynamically allocated by the calling environment.
 */
MessageQueueMsgPayload *
InitialiseMessageQueueMsgPayload_m(char *verb, UfsrvCommandBroadcast *ufsrv_broadcast_ptr, unsigned char *payload, size_t payload_sz, unsigned delegator_type)
{
  MessageQueueMsgPayload *mqp_ptr;

  mqp_ptr = calloc(1, sizeof(MessageQueueMsgPayload));
  if (mqp_ptr) {
    mqp_ptr->verb = verb;
    mqp_ptr->topic = ufsrv_broadcast_ptr->topic_name;//topic;
    //mqp_ptr->msg=msg;
    mqp_ptr->payload = payload;
    mqp_ptr->payload_sz = payload_sz;
    mqp_ptr->delegator_type = delegator_type;

    return mqp_ptr;
  }

  return NULL;
}

void
DestructMessageQueueMsgPayload(MessageQueueMsgPayload *mqp_ptr, MessageQueueMessage *mqm_ptr, bool flag_self_destruct)
{
  if (likely(mqp_ptr != NULL)) {
    if (IS_PRESENT(mqm_ptr))	message_queue_message__free_unpacked(mqm_ptr, NULL);

    //if (mqp_ptr->msg)	free(mqp_ptr->msg);

    if (mqp_ptr->payload && mqp_ptr->payload_sz > 0)	free(mqp_ptr->payload);

    if (flag_self_destruct) {
      free(mqp_ptr);
      mqp_ptr = NULL;
    }
  }
}

//subscriber thread for reading
/**
 * @brief performs very rudimentary parsing of the stream. Hardcoded to parse 3-argument or 4-argument message only
 * it doesn't do proper checking on the fragmentation of message based on known packet size.
 *
 */
static void * __attribute__((nonnull(1)))
ThreadMessageQueueSub(void *ptr)
{
	MessageQueueBackend *mq_ptr;
  WorkersConfigDescriptor *jobworkers_config  = GetJobWorkersConfigurationDescriptor();

	{
		#define MAX_NAME_LEN 15
		char proc_name [MAX_NAME_LEN + 1];	/* Name must be <= 15 characters + a null */

		strncpy(proc_name, "ufMsgQueueSub", MAX_NAME_LEN);
		proc_name [MAX_NAME_LEN] = 0;
		prctl(PR_SET_NAME, (unsigned long)&proc_name);
		#undef MAX_NAME_LEN
	}

	MessageQueueSubscriberStart *start = ptr;
  mq_ptr = start->backend;
  const Protocol *protocol = start->protocol;
  free(start);

	if ((_CreateMessageQueueTopics(mq_ptr, protocol)) != 0)	return(0);//exit thread

	while (1) {
		int x, read_len;
		fd_set fd,
				xfd;
		char msg[X11BUF];

		again:
		FD_ZERO(&fd);
		FD_ZERO(&xfd);

		FD_SET(((redisContext *)mq_ptr->persistance_agent)->fd, &fd);

		x = select(((redisContext *)mq_ptr->persistance_agent)->fd + 1, &fd, NULL, NULL, NULL);

		if (x > 0) {
			read_len = read(((redisContext *)mq_ptr->persistance_agent)->fd, msg, X10BUF);
			if (read_len > 0) {
				msg[read_len-1] = 0;
				//syslog(LOG_ERR, "MessageQueueSub: received publish: '%s' length: '%d'", msg, read_len);
#if 0
				'*3/r/n $7/r/n message/r/n $13/r/n UFSRV:SESSION/r/n $2/r/n hi/r/n'
								1					2						3
#endif
				if (msg[0] == '*') {
					char *verb, *topic;
					char *aux,  *tmsg;
					size_t len = 0;

					aux = msg + 1;//skip past '*'
					*(aux + 1) = '\0'; //kill '/r' so atoi sees termination point for the digit, probably not necessary Assumes single digit
					int nargs = atoi(aux);
					aux += 4;//skip past \r\n$ assumes single digit argument, which is not unreasonable

					//7/r/n
						while(*aux != '\r') {
							len = (len * 10) + (*aux - '0');
							aux++;
						}

						aux += 2; //move past \n$
						verb = aux;
						*(verb + len) = 0; //kills \r
						//syslog(LOG_ERR, "MessageQueueSub:  length: '%d' verb: '%s'...", len, verb);
						aux = verb + (len + 3);
						len = 0;

						while (*aux != '\r') {
							len = (len * 10) + (*aux - '0');
							aux++;
						}
						aux += 2; //move past \n$
						topic = aux;
						*(topic + len) = 0;//kills \r

						//syslog(LOG_ERR, "MessageQueueSub: length: '%d' topic: '%s'...", len, topic);
						aux = topic + (len + 3);//at size marker
						len = 0;

						while (*aux != '\r') {
							len = (len * 10) + (*aux - '0');
							aux++;
						}

						aux += 2; //move past '\n '
						tmsg = aux;
						*(tmsg + len) = 0;//kills '\r' <-- not necessary as we memcopy len_amount + 1 extra for null

						//if (nargs==3)
						{
							syslog(LOG_ERR, "%s {nargs:'%d', len:'%lu', verb:'%s', topic:'%s', message:'...'}", __func__, nargs, len, verb, topic);

              pthread_mutex_lock(&jobworkers_config->work_queue_mutex);

              QueueEntry *qe_ptr = NULL;
              qe_ptr = AddQueue(&(jobworkers_config->ufsrv_work_queue));//remember this is mutex protected

              unsigned char *payload = calloc(1, len + 1);//extra space for null because the buffer is multiplex between binary and text payloads
              memcpy(payload, tmsg, len);
              qe_ptr->whatever = InitialiseMessageQueueMsgPayload_m(subpub_verb, GetBroadcastDescriptorByName(topic), payload, len, DELEGTYPE_MSGQUEUE);

              pthread_cond_broadcast(&jobworkers_config->queue_not_empty_cond);
              pthread_mutex_unlock(&jobworkers_config->work_queue_mutex);
						}
					}
						//server_id:cmd_idx:arg1:arg2:arg3
			}
			else if (read_len == 0) {
				syslog(LOG_ERR, "%s: received '0' length message: CLIENT DISCONNECTED: RE-ESTABLISHING CONNECTION", __func__);

				if (SetupMessageQueueSubscriber(1))	_CreateMessageQueueTopics(UfsrvConfigMsgqueueSub()/*masterptr->msgqueue_sub*/, protocol);
			} else {

			}

		} else if (x == 0) {
      //timeout
		} else if (x == -1 && errno != EINTR) {
			syslog(LOG_ERR, "ThreadCommandConsoleClient: ERROR: select");

			//goto again;
		}

		//goto again;
	}

	return NULL;

}

static size_t _CalculateTopicsStringSize(CollectionDescriptor *collection_topics);

static size_t
_CalculateTopicsStringSize(CollectionDescriptor *collection_topics)
{
	size_t total_sz = 0;

	for (size_t i=0; i<collection_topics->collection_sz; i++)	total_sz += strlen(collection_topics->collection[i]);

	return total_sz;
}

/**
 * @brief the channels to which we listen for updates. Add new channels here. There is no interface to dynamically achieve that, so all calls
 * must be hard coded here in. Channels stay on for the lifetime of the server and are pretty much fixed per given protocol.
 *
 * As the parser above is under-developed, any topic added here, must have a corresponding parser block in the function ParseMessageQueueCommand().
 *
 */
static int
_CreateMessageQueueTopics(MessageQueueBackend *mq_ptr, const Protocol *protocol)
{
	UFSRVResult result={0};
#define REDIS_COMMAND_SUBSCRIBE "SUBSCRIBE "

  if (!protocol->protocol_callbacks.msgqueue_topics_callback ||
      !protocol->protocol_callbacks.msgqueue_topics_callback(&result)) goto exit_msgqueue_disabled;

	CollectionDescriptor *collection_topics = result.result_user_data;

	if (IS_PRESENT(collection_topics) && (collection_topics->collection_sz > 0)) {
		size_t topics_string_sz;
		if ((topics_string_sz=_CalculateTopicsStringSize(collection_topics)) <= 0)	goto exit_error;

		char 	topics_string[topics_string_sz +
												(collection_topics->collection_sz) +//nulls
												(sizeof(REDIS_COMMAND_SUBSCRIBE) * collection_topics->collection_sz)//this already contains nulls due sizeof
											 ];
		char *subscribe_commands[collection_topics->collection_sz];
		char	*walker			=	topics_string;


		for (size_t i=0; i<collection_topics->collection_sz; i++) {
			size_t topic_sz = strlen(collection_topics->collection[i]);
			sprintf(walker, REDIS_COMMAND_SUBSCRIBE "%s", (const char *)collection_topics->collection[i]);
			subscribe_commands[i] = walker;
			walker += (topic_sz + sizeof(REDIS_COMMAND_SUBSCRIBE));//+1);//+1 skip over space
		}

		for (size_t i=0; i<collection_topics->collection_sz; i++) {
			redisReply *redis_ptr = (*mq_ptr->send_command_for_subscriber)(mq_ptr, subscribe_commands[i]);

			if (IS_PRESENT(redis_ptr)) {
				syslog(LOG_INFO, "%s: Subscribed to: '%s'. Total subscriptions for this instance: '%lld'", __func__, redis_ptr->element[1]->str, redis_ptr->element[2]->integer);

				freeReplyObject(redis_ptr);
			}
			else	goto exit_redis_error;
		}

		exit_success:
		return 0;

	}

	exit_msgqueue_disabled:
	syslog (LOG_WARNING, "%s (protocol_id:'%u'): MessageQueue Listener shutdown: Not supported by protocol", __func__, protocol->protocol_id);
	return 1;

	exit_error:
	return -1;

	exit_redis_error:
	syslog (LOG_WARNING, "%s (protocol_id:'%u'): MessageQueue Listener shutdown: COULDN'T ISSUE SUBSCRIBE COMMAND", __func__, protocol->protocol_id);
	return -2;
}

/*
 * @brief only one listener per usfrv instance
 */
MessageQueueBackend *
CreateMessageQueueSubscriberListenerThread(const Protocol *protocol)
{
	const SessionsDelegator *delegator = SessionsDelegatorPtr();
	if (!protocol || !protocol->protocol_controls.msgqueue_subscriber || !delegator || !delegator->msgqueue_executor) {
		syslog(LOG_ERR, "%s: Message queue subscriber requires a registered executor", __func__);
		return NULL;
	}

  MessageQueueSubscriberStart *start = malloc(sizeof(*start));
  if (!start) return NULL;

  MessageQueueBackend *mq_ptr = SetupMessageQueueSubscriber(0);
  if (!mq_ptr) {
    free(start);
    return NULL;
  }
  *start = (MessageQueueSubscriberStart){.backend = mq_ptr, .protocol = protocol};
  if (pthread_create(&th_msgqueue_sub, NULL, ThreadMessageQueueSub, start) != 0) {
    free(start);
    return NULL;
  }
  return mq_ptr;
}



