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

#ifndef UFSRV_BROADCAST_SEMANTICS_ENUM_H
#define UFSRV_BROADCAST_SEMANTICS_ENUM_H

enum BroadcastSemantics {
    INTER_SEMANTICS												=	0, //pure INTER broadcast	receiver only modify local data model
    INTRA_SEMANTICS,													//pure INTRA broadcast	receiver expected to change backend data model
    INTRA_WITH_INTER_SEMANTICS,		//broadcast from outside server class Backend data model has already taken place, therefore treat message with INTER semantics
    INTER_WITH_INTRA_SEMANTICS
};

#endif //UFSRV_BROADCAST_SEMANTICS_ENUM_H
