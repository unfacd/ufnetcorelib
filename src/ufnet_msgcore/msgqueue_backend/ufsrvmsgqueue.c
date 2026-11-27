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

/**
 * MessageQueue Backend is a subsystem for sending a variety of message based inter-thread and inter-process communications. The queue backend
 * is a standalone redis instance running subscriber-publisher semantics.
 */
#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <main_ufnetcorelib.h>

#include <ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue.h>
#include <ufsrvcorelib/ufsrv_core/cache_backend/redis.h>
#include <ufsrvcorelib/ufsrv_core/cache_backend/backendconfig_type.h>
#include <uflib/net/utils_net.h>
#include <ufsrvcorelib/ufsrv_core/delegator_sessions/sessions_delegator_type.h>
#include <ufnet_core/session/session_type.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_shim.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_value_type.h>
#include <ufsrvcorelib/ufsrv_core/delegator_sessions/delegator_session_worker_thread.h>
#include <ufsrvcorelib/ufsrv_core/delegator_sessions/thread_context_type.h>

extern __thread ThreadContext ufsrv_thread_context;
static void *MQSubRedisSendCommand(MessageQueueBackend *mq_ptr, const char *format, ...);
static void *MQRedisSendCommand(SessionType *sesn_type_ptr, MessageQueueBackend *mq_ptr, const char *format, ...);

/**
 * @brief Create a connection handle for the MessageQueue backend
 * @param in_per_ptr When passing in_per_ptr we assume we are re-initialising an existing backend: free context and reallocate
 * @return Allocate / initialised backend connection handle
 */
MessageQueueBackend *
BuildConnectionHandleForMessageQueueBackend(MessageQueueBackend *in_per_ptr)
{
	BackendConfig config;
	struct timeval tv;
	char resolved_ip[SBUF] = {0};

	if ((GenericDnsResolve(UfsrvConfigMsgqueueAddress()/*masterptr->ufsrvmsgqueue_address*/, resolved_ip, sizeof(resolved_ip))) < 0)	 config.con_tcp.host = "127.0.0.1";
	else	 config.con_tcp.host = resolved_ip;
	config.con_tcp.port = UfsrvConfigMsgqueuePort();//masterptr->ufsrvmsgqueue_port;
	config.type = SOCK_TCP;
	tv.tv_sec = 0;
	tv.tv_usec = 500000;//0.5 sec
	config.con_tcp.timeout = tv;
	config.backend_label = UfsrvConfigServerDescriptiveName();//masterptr->server_descriptive_name;

	MessageQueueBackend *mq_ptr = NULL;
	mq_ptr = (MessageQueueBackend *) BuildConnectionHandleForRedisBackend((RedisBackend *)in_per_ptr, &config);
	if (IS_PRESENT(mq_ptr)) {
		mq_ptr->send_command								= MQRedisSendCommand;
		mq_ptr->send_command_multi					= RedisSendCommandMulti;
		mq_ptr->send_command_for_subscriber	=	MQSubRedisSendCommand;
		mq_ptr->init_connection							= (MessageQueueBackend *(*)(MessageQueueBackend *))BuildConnectionHandleForMessageQueueBackend;
	}
	return mq_ptr;

}

/**
 * @brief Create a connection handle for the MessageQueue backend, which is a standalone redis instance, running
 * subscriber-publisher semantics. Only one subscriber listener per ufsrv instance that listens for broadcasts by other ufsrv publishers
 * @param restart_flag  reinitialise the connection context after disconnect
 *
 */
MessageQueueBackend *
SetupMessageQueueSubscriber(int restart_flag)
{
	MessageQueueBackend *mq_ptr;

	syslog(LOG_INFO, "%s: Initialising MessageQueue Subscriber Listener Backend. RESTART_FLAG: '%d'...", __func__, restart_flag);

	if (restart_flag) {
		if ((mq_ptr = BuildConnectionHandleForMessageQueueBackend(UfsrvConfigMsgqueueSub()/*masterptr->msgqueue_sub*/))) {
			mq_ptr->send_command_for_subscriber = MQSubRedisSendCommand; //this is session agnostic hence why a different send command function
		} else {
			syslog(LOG_INFO, "%s: ERROR: COULD NOT RE-INITIALISE MessageQueue Subscriber Listening Session...", __func__);
			goto exit_and_return_null;
		}
	} else {
		mq_ptr = BuildConnectionHandleForMessageQueueBackend(NULL);
		if (mq_ptr) {
			syslog(LOG_INFO, "%s: Initialising MessageQueue Subscriber Listening Session...", __func__);

			Socket *s_ptr = calloc(1, sizeof(Socket));
			s_ptr->type = SOCK_UFSRVQUEUESUB;
			s_ptr->sock = ((redisContext *)(mq_ptr->persistance_agent))->fd;
			strcpy(s_ptr->address, "localhost");
			strcpy(s_ptr->haddress, UfsrvConfigMsgqueueAddress()); //masterptr->ufsrvmsgqueue_address
			UfsrvConfigSetMsgqueueSub(&(UfsrvConfigItem){.value.stored_value.ptr_val=mq_ptr}, UFSRV_CONFIG_SOURCE_RUNTIME);
			mq_ptr->send_command_for_subscriber = MQSubRedisSendCommand;
		} else {
			syslog(LOG_INFO, "%s: ERROR: COULD NOT INITIALISE MessageQueue Subscriber Listening Session...", __func__);
			goto exit_and_return_null;
		}
	}

	return mq_ptr;

	exit_and_return_null:
	return NULL;

}

/**
 * 	@brief: Worker specific send function for intra publishing via msg queue bus. Each worker maintains its own instance
 * 	of the connection.
 */
static void *
MQRedisSendCommand(SessionType *sesn_type_ptr, MessageQueueBackend *mq_ptr, const char *format, ...)
{
	MessageQueueBackend *pers_ptr = mq_ptr;
	Session *sesn_ptr = AS_SESSION(sesn_type_ptr);

	if (!pers_ptr || !pers_ptr->persistance_agent) {
		syslog(LOG_ERR, "%s (pid='%lu'): ERROR: COULD NOT FIND Persistence object...", __func__, pthread_self());

		return NULL;
	}

	va_list ap;
	void *_reply = NULL;
	redisReply *reply;

	va_start(ap,format);
	_reply = redisvCommand(pers_ptr->persistance_agent, format, ap);
	va_end(ap);

	if (!_reply) {
		syslog(LOG_ERR, "%s (oid='%p'): ERROR ('%d'): REDIS COMMAND ERROR '%s': Attempting reconnection...", __func__, sesn_ptr, ((redisContext *)(pers_ptr->persistance_agent))->err, ((redisContext *)(pers_ptr->persistance_agent))->errstr);

		if (IS_PRESENT(((*pers_ptr->init_connection)(pers_ptr)))) {
			va_start(ap, format);
			_reply = redisvCommand(pers_ptr->persistance_agent, format, ap);
			va_end(ap);

			if (IS_PRESENT(_reply))	return _reply;
		}

		return NULL;
	}

	return _reply;

}

/**
 * 	@brief: Subscriber specific send function. Currently only one thread uses this to listen in for sub/pub traffic
 */
static void *
MQSubRedisSendCommand(MessageQueueBackend *mq_ptr, const char *format, ...)
{
	void *_reply = NULL;

	if (mq_ptr) {
		va_list ap;
		redisReply *reply;

		va_start(ap,format);
		_reply = redisvCommand(mq_ptr->persistance_agent, format, ap);
		va_end(ap);

		if (!_reply) {
			syslog(LOG_ERR, "%s : ERROR ('%d'): REDIS COMMAND ERROR '%s': Attempting reconnection...", __func__,
					((redisContext *)(mq_ptr->persistance_agent))->err, ((redisContext *)(mq_ptr->persistance_agent))->errstr);

			if (IS_PRESENT(((*mq_ptr->init_connection)(mq_ptr)))) {
				va_start(ap, format);
				_reply = redisvCommand(mq_ptr->persistance_agent, format, ap);
				va_end(ap);

				if (IS_PRESENT(_reply))	return _reply;
			}

			return NULL;
		}
	}

	return _reply;

}