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

#ifndef UFSRV_ENUM_MESSAGE_QUEUE_VERIFICATION_CODES_H
#define UFSRV_ENUM_MESSAGE_QUEUE_VERIFICATION_CODES_H


enum MessageQueueVerificationCodes {
    MSGQUE_SUCCESS					=	 0,
    MSGQUE_UNPACK_ERROR 		= -1,
    MSGQUE_EMPTY_WIREDATA		= -2,
    MSGQUE_EMPTY_COMMAND		=	-3,
    MSQQUE_SELFPUBLISHED		=	-4,
    MSGQUE_OTHER_TARGET			=	-5,
    MSGQUE_UNKNOWN_COMMAND	=	-6
};

#endif //UFSRV_ENUM_MESSAGE_QUEUE_VERIFICATION_CODES_H
