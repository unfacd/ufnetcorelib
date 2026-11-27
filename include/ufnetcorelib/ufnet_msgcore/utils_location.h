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

#ifndef UFSRV_UTILS_LOCATION_H
#define UFSRV_UTILS_LOCATION_H

#include <ufnet_msgcore/location/location_type.h>
#include <ufnet_core/session/session_type.h>
#include <ufnet_msgcore/session.h>
#include <ufnet_msgcore/fence/fence_type.h>

#include "ufnet_msgcore/UnfacdService.pb-c.h"

int ParseCacheBackendStoredLocationDescription(LocationDescription *location_ptr, const char *location_stored, bool flag_dirty);
void DestructLocationDescription(LocationDescription *);
LocationDescription *ProvideLocationDescriptionFromSerialised(char *location_serialised, LocationDescription *location_ptr_out, bool is_validate_address);
UFSRVResult *UpdateHomebaseGeoLocDynamicAssignment(Session *sesn_ptr, const char *baseloc, unsigned long call_flags_sesn);

void UpdateSessionGeoFenceDataByFid(Session *sesn_ptr, unsigned long fid_current, unsigned long fid_past);
void UpdateSessionGeoFenceData(Session *sesn_ptr, InstanceHolderForFence *instance_f_ptr_current, InstanceHolderForFence *instance_f_ptr_past);
UFSRVResult *UpdateLocationByWireProto(InstanceContextForSession *ctx_ptr, const LocationRecord *location_record_ptr);

bool BuildUserLocationByProto(Session *sesn_ptr, LocationRecord *location_record_ptr);

int GetUfsrvGeoGroupForUser(Session *sesn_ptr, const char *location_name);
UFSRVResult *UpdateBackendSessionGeoJoinData(Session *sesn_ptr, Fence *f_ptr_current, Fence *f_ptr_past);
json_object *JsonFormatUserLocation(Session *sesn_ptr, json_object *jobj_in);
size_t SizeofLocationDescription(const LocationDescription *location_ptr);
UFSRVResult *UpdateHomebaseGeoLocUserPrefAssignment(Session *sesn_ptr, const char *baseloc, unsigned long call_flags_sesn);


void FormatCacheBackendLocationFieldValue(const LocationDescription *location_ptr, BufferDescriptor *buffer_out);
UserPreferenceDescriptor *PrefValidateGeoGroupsRoaming(ClientContextData *ctx_ptr, UserPreferenceDescriptor *pref_ptr);
void *GeocodeLocation(Session *, HttpRequestContext *, LocationDescription *, const char *);

void ResetSessionGeoFenceData(InstanceHolderForSession *instance_sesn_ptr);
void FuzzGeoLocation(LocationDescription *location_ptr, double factor);


static inline LocationDescription *
GetLocationDescription(Session *sesn_ptr)
{
  LocationDescription *ld_ptr = SESSION_SERVICE(sesn_ptr)->user.user_details.user_location_initialised ?
                                &(SESSION_SERVICE(sesn_ptr)->user.user_details.user_location) : &(SESSION_SERVICE(sesn_ptr)->user.user_details.user_location_by_server);

  return ld_ptr;
}

#endif //UFSRV_UTILS_LOCATION_H
