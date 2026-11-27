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

#ifndef DATA_PROTOCOL_SANDBOX_PRIV_H
#define DATA_PROTOCOL_SANDBOX_PRIV_H

#include "protocol_sandbox_priv.h"
#include <ufsrv_core/protocol/protocol_type.h>
#include <ufsrv_core/delegator_sessions/workers/worker_session_io_thread.h>

static Protocol ProtocolsRegistry [] = {
	{
		.protocol_name = "Sandbox",
		.protocol_id = 0,
		.protocol_threadx = ThreadWebSockets,
		.protocol_callbacks = {
			proto_sandbox_protocol_init_callback, //proto init
			NULL,                                 //config
			proto_sandbox_init_workers_delegator_callback,//init_workers_delegator_callback -> to be phased out
			proto_sandbox_init_session_callback,
			proto_sandbox_reset_callback,
			proto_sandbox_hanshake_callback,
			proto_sandbox_post_hanshake_callback,
			proto_sandbox_msg_callback,
			NULL,//msg_out
			proto_sandbox_decode_msg_callback,
			proto_sandbox_encode_msg_callback,
			proto_sandbox_service_timeout_callback,
			proto_sandbox_error_callback,
			proto_sandbox_recycler_error_callback,
			proto_sandbox_close_callback,
			proto_sandbox_msgqueue_topics_callback,
			proto_sandbox_generate_session_id_callback,
		},
		.post_handshake = 0,
		.protocol_data = NULL,
		.protocol_controls = {
			1, //unsigned read_blocked_session
			1, //unsigned read_inservice_session
			1, //retain_session_on_error
			1, //_PROTOCOL_CTL_ABORT_IOERROR
			1, //CLIENT_OF_RECYCLER
			1, //cloudmsgonioerror
			1, //pub_session_transitions
			1, //msgqueue subscriber
		},
		.protocol_key = "sandbox",
	}//end of protocol entry

//sentenil entry
,
	{
	 .protocol_name = "", .protocol_id = -1, .protocol_threadx = NULL, .protocol_callbacks = {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}, .post_handshake = 0, .protocol_data = NULL, .protocol_controls = {0}
	}
};

#endif /* DATA_PROTOCOLS_H_ */
