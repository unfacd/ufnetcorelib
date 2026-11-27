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


#ifndef UFSRV_GUARDIAN_H
#define UFSRV_GUARDIAN_H

#include <ufsrv_core/ufsrvresult_type.h>
#include <ufnet_core/session/session_type.h>
#include <ufnet_core/guardian_record_descriptor.h>

//<uid> <uid>
#define REDIS_CMD_GUARDIAN_ADD "SADD GUARDIAN_FOR_%lu %lu"
#define REDIS_CMD_GUARDIAN_REM 			"SREM GUARDIAN_FOR_%lu %lu"
#define REDIS_CMD_FENCE_GUARDIAN_MEMBERS	"SMEMBERS GUARDIAN_FOR_%lu"
#define REDIS_CMD_FENCE_GUARDIAN_ISMEMBER	"SISMEMBER GUARDIAN_FOR_%lu %lu"

char *GenerateGuardianNonce(Session *sesn_ptr, const char *value);
unsigned long IsGuardianLinkNonceValid(const char *nonce, unsigned long value);
UFSRVResult *DbBackendInsertGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr, bool force_data);
UFSRVResult *DbBackendGetGuardianRecord(GuardianRecordDescriptor *descriptor_ptr);
UFSRVResult *DbBackendGetGuardianRecords(GuardianRecordDescriptor *descriptor_ptr_guardian, CollectionDescriptor *collection_ptr_out);
UFSRVResult *DbBackendGetGuardianRecordForOriginator(GuardianRecordDescriptor *descriptor_ptr);
UFSRVResult *DbBackendDeleteGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr);
UFSRVResult *CacheBackendAddGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr);
UFSRVResult *CacheBackendRemGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr);
UFSRVResult *CacheBackendGetGuardianRecord(const GuardianRecordDescriptor *descriptor_ptr);
bool IsUserGuardianFor(const GuardianRecordDescriptor *);

#endif //UFSRV_GUARDIAN_H
