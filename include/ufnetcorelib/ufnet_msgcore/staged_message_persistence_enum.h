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

#ifndef UFSRV_STAGED_MESSAGE_PERSISTENCE_ENUM_H
#define UFSRV_STAGED_MESSAGE_PERSISTENCE_ENUM_H


/**
 * Parameters to inform post staged message fetch. Staged messages are copies of undelivered messages which users can retrieve via API calls
 */
enum StagedSetPersistance {
    KEEP_ALL_STAGED_SETS = 0, //keep both, index and payload
    KEEP_STAGED_SET_INDEX,
    KEEP_STAGED_SET_PAYLOAD,

    DELETE_ALL_STAGED_SETS,  //delete both, index and payload
    DELETE_STAGED_SET_INDEX,
    DELETE_STAGED_SET_PAYLOAD,
};

#endif //UFSRV_STAGED_MESSAGE_PERSISTENCE_ENUM_H
