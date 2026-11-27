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

#ifndef PROTOCOL_SANDBOX_PRIV_H_
#define PROTOCOL_SANDBOX_PRIV_H_

#include <ufnetcorelib/ufnet_core/session/session_type.h>
#include <ufsrv_core/protocol/protocol.h>
#include <ufsrv_core/delegator_sessions/http_request_context_type.h>


UFSRVResult *proto_sandbox_protocol_init_callback(const Protocol *);
UFSRVResult *proto_sandbox_init_workers_delegator_callback(void);
UFSRVResult *proto_sandbox_hanshake_callback(InstanceHolder *, SocketMessage *, unsigned, int **);
UFSRVResult *proto_sandbox_post_hanshake_callback(InstanceHolder *, SocketMessage *sock_msg_ptr, unsigned callflags);
UFSRVResult *proto_sandbox_msg_callback(InstanceHolder *, SocketMessage *, unsigned, size_t);
UFSRVResult *proto_sandbox_decode_msg_callback(InstanceHolder *, SocketMessage *sock_msg_ptr, unsigned);
UFSRVResult *proto_sandbox_encode_msg_callback(InstanceHolder *, SocketMessage *sock_msg_ptr, unsigned);
UFSRVResult *proto_sandbox_init_session_callback(ClientContextData *ctx_data_ptr, unsigned callflags);
UFSRVResult *proto_sandbox_reset_callback(InstanceHolder *instance_sesn_ptr, unsigned callflags);
UFSRVResult *proto_sandbox_service_timeout_callback(InstanceHolder *instance_sesn_ptr, time_t now, unsigned long call_flags);
UFSRVResult *proto_sandbox_error_callback(InstanceHolder *, unsigned);
UFSRVResult *proto_sandbox_recycler_error_callback(InstanceHolder *, unsigned);
UFSRVResult *proto_sandbox_close_callback(InstanceHolder *);
UFSRVResult *proto_sandbox_msgqueue_topics_callback(UFSRVResult *);
UFSRVResult *proto_sandbox_generate_session_id_callback(UFSRVResult *res_ptr, ClientContextData *context_data);


#endif /* PROTOCOL_SANDBOX_PRIV_H_ */
