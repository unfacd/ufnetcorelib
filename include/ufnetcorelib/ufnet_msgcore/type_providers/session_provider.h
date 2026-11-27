/**
 * Copyright (C) 2015-2025 unfacd works
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

#ifndef UFSRV_SESSION_PROVIDER_H
#define UFSRV_SESSION_PROVIDER_H

#include <uflib/recycler/recycler_type.h>
#include <ufnet_core/session/session_type.h>
#include <ufnetcorelib/ufnet_core/session/session_layout.h>

int SessionReturnToRecycler(InstanceHolderForSession *instance_sesn_ptr, ContextData *ctx_data_ptr, unsigned long call_falgs);
size_t SessionGetReferenceCount(InstanceHolderForSession *instance_sesn_ptr);
void SessionIncrementReference(InstanceHolderForSession *instance_sesn_ptr, int multiples);
void SessionDecrementReference(InstanceHolderForSession *instance_sesn_ptr, int multiples);
void SessionDecrementReferenceByOne(InstanceHolderForSession *instance_sesn_ptr);
void SessionIncrementReferenceByOne(InstanceHolderForSession *instance_sesn_ptr);
RecyclerPoolHandle *InitSessionRecyclerTypePool(type_instantiator instantiator, size_t group_allocation_sz, size_t expansion_threshold);
PUBLIC_API RecyclerPoolHandle *InitSessionRecyclerTypePoolWithLayout(type_instantiator instantiator,
                                                                    SessionStorageLayout layout,
                                                                    size_t group_allocation_sz,
                                                                    size_t expansion_threshold);

#endif //UFSRV_SESSION_PROVIDER_H
