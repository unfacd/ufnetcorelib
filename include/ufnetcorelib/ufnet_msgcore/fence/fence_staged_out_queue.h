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

#ifndef SRC_INCLUDE_FENCE_STAGED_OUT_QUEUE_H_
#define SRC_INCLUDE_FENCE_STAGED_OUT_QUEUE_H_

#include <stddef.h>
#include <ufnetcorelib_defs.h>
#include <ufsrv_core/ufsrvresult_type.h>
#include <ufnet_core/session/session_type.h>
#include <ufnet_core/staged_message_descriptor_type.h>

PUBLIC_API UFSRVResult *StoreFenceOutQueueMessage(const StagedMessageDescriptor *staged_message_descriptor_ptr);
PUBLIC_API UFSRVResult *ExtractFenceOutQueueMessage(unsigned long            fid, unsigned long eid,
                                                    StagedMessageDescriptor *staged_message_descriptor_ptr_out);

#endif
