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

#ifndef UFSRV_TYPE_PROVIDER_FENCE_H
#define UFSRV_TYPE_PROVIDER_FENCE_H

#include "uflib/recycler/recycler_type.h"
#include "ufnetcorelib/ufnet_msgcore/fence/fence_type.h"

InstanceHolderForFence *FenceGetInstance(ContextData *ctx_data_ptr, unsigned long call_flags);
int FenceReturnToRecycler(InstanceHolderForFence *instance_holder_ptr, ContextData *ctx_data_ptr, unsigned long call_flags);
void FenceIncrementReference(InstanceHolderForFence *descriptor_ptr, int multiples);
void FenceDecrementReference(InstanceHolderForFence *descriptor_ptr, int multiples);
void InitFenceRecyclerTypePool(size_t block_allocation_sz);
void InitFenceRecyclerTypePoolWithProvider(RecyclerPoolOps *ops_fence_ptr_provided, size_t block_allocation_sz);
unsigned  FencePoolTypeNumber();

#endif //UFSRV_TYPE_PROVIDER_FENCE_H
