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

#ifndef UFSRV_STAGED_MESSAGES_UFSRV_H
#define UFSRV_STAGED_MESSAGES_UFSRV_H

#include <ufsrv_core/ufsrvresult_type.h>
#include <ufnet_core/session/session_type.h>
#include <ufnet_core/incoming_message_descriptor_type.h>
#include <ufnet_core/staged_message_descriptor_type.h>
#include <ufnet_msgcore/stored_message_opttions_enum.h>

UFSRVResult *GetRemStagedMessageCacheRecordForIntraCommand (Session *sesn_ptr, IncomingMessageDescriptor *msg_desc_ptr,  const char *payload_name, enum StoredMessageOptions);
UFSRVResult *DeleteStagedMessageCacheRecordForUser(Session *sesn_ptr, StagedMessageDescriptor *);
UFSRVResult *GetStageMessageCacheBackendListSize(Session *sesn_ptr, unsigned long userid);


#endif //UFSRV_STAGED_MESSAGES_UFSRV_H
