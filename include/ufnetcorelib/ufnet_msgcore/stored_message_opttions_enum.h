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

#ifndef UFSRV_STORED_MESSAGE_OPTTIONS_ENUM_H
#define UFSRV_STORED_MESSAGE_OPTTIONS_ENUM_H

enum StoredMessageOptions {
    MSGOPT_GET_FIRST 			= (0x1<<1),
    MSGOPT_GET_LAST				= (0x1<<2),
    MSGOPT_GETALL					=	(0x1<<3),
    MSGOPT_GETNAMED				=	(0x1<<4),
    MSGOPT_REMOVE					= (0x1<<5),
    MSGOPT_REMOVENAMED		= (0x1<<6),
    MSGOPT_GET_REM_FIRST	=	((0x1<<1)|(0x1<<5)),
    MSGOPT_GET_REM_LAST		=	((0x1<<2)|(0x1<<5)),
    MSGOPT_GET_REM_ALL		=	((0x1<<3)|(0x1<<5)),
};

#endif //UFSRV_STORED_MESSAGE_OPTTIONS_ENUM_H
