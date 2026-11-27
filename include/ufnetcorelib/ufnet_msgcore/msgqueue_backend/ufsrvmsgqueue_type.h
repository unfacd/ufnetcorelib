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

#ifndef SRC_INCLUDE_UFSRVMSGQUEUE_TYPE_H_
#define SRC_INCLUDE_UFSRVMSGQUEUE_TYPE_H_

#include <ufsrvcorelib/ufsrv_core/cache_backend/persistence_type.h>

typedef void SessionType;

typedef struct MessageQueueBackend MessageQueueBackend;
	struct MessageQueueBackend	{
		void *persistance_agent;//redisConetx *
		void *(*send_command)(SessionType *sesn_ptr, MessageQueueBackend *mq_ptr, const char *format, ...);//TODO CONVERT TO REGULAR BACKEND
		void *(*send_command_multi)(SessionType *sesn_ptr, CacheBackend *pers_ptr, const char *format, ...);//pipelined synchronous redis command
		void *(*send_command_for_subscriber)(MessageQueueBackend *mq_ptr, const char *format, ...);
		MessageQueueBackend *(*init_connection)(MessageQueueBackend *);
	};

#define AS_MESSAGE_QUEUE_BACKEND(x)	((MessageQueueBackend *)(x))

#include <ufsrv_core/jobworkers/message_queue_msg_payload_type.h>

#endif /* SRC_INCLUDE_UFSRVMSGQUEUE_TYPE_H_ */
