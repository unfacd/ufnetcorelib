/*

 Copyright (c) 2015-2026 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#ifndef UFSRV_DELEGATOR_MSGQUEUE_SUBSCRIBER_THREAD_H
#define UFSRV_DELEGATOR_MSGQUEUE_SUBSCRIBER_THREAD_H

#include <stdbool.h>
#include <ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue_type.h>
#include "UfsrvMessageQueue.pb-c.h"
#include <ufnet_msgcore/msgqueue_backend/ufsrv_command_broadcast_type.h>

struct Protocol;
MessageQueueBackend *CreateMessageQueueSubscriberListenerThread(const struct Protocol *protocol);
void DestructMessageQueueMsgPayload(MessageQueueMsgPayload *mqp_ptr, MessageQueueMessage *mqm_ptr, bool flag_self_destruct);
MessageQueueMsgPayload *InitialiseMessageQueueMsgPayload_m(char *verb, UfsrvCommandBroadcast *ufsrv_broadcast_ptr, unsigned char *payload, size_t payload_sz, unsigned delegator_type);

#endif //UFSRV_DELEGATOR_MSGQUEUE_SUBSCRIBER_THREAD_H
