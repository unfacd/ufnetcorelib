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

#ifndef UFSRV_UFSRV_COMMAND_BROADCAST_TYPE_H
#define UFSRV_UFSRV_COMMAND_BROADCAST_TYPE_H

#include <ufsrvcorelib/ufsrv_core/ufsrvresult_type.h>
#include <ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue_type.h>
#include <ufnet_msgcore/msgqueue_backend/UfsrvMessageQueue.pb-c.h>
#include <ufsrvcorelib/ufsrv_core/protocol/protocol_type.h>
#include <ufnet_msgcore/msgqueue_backend/message_queue_verification_codes_enum.h>

#include "ufnet_msgcore/fence/fence_type.h"


typedef UFSRVResult * (*BroadcastHandler)(MessageQueueMsgPayload *, MessageQueueMessage *, UFSRVResult *, unsigned long);
typedef UFSRVResult * (*BroadcastPreHandler)(WireProtocolData *, FenceSessionPair *fence_sesn_pair_ptr, UFSRVResult *res_ptr, int command);

typedef struct UfsrvCommandBroadcastOps {
    unsigned long verifier_callflags;
    int (*broadcast_handler)(MessageQueueMessage *, UFSRVResult *, unsigned long);
    BroadcastPreHandler broadcast_pre_handler;
    enum MessageQueueVerificationCodes  (*broadcast_verifier)(MessageQueueMsgPayload *, MessageQueueMessage **mqm_ptr_out, unsigned long);

} UfsrvCommandBroadcastOps;

typedef struct UfsrvCommandBroadcast {
    int 				topic_id;//enumUfsrvBroadcastTopicIds
   char 				*topic_name;

    UfsrvCommandBroadcastOps				ops;
} UfsrvCommandBroadcast;


#endif //UFSRV_UFSRV_COMMAND_BROADCAST_TYPE_H
