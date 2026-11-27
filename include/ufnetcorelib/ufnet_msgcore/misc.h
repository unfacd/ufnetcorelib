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

#ifndef MISC_H
# define MISC_H

#include <lua.h>
#include "uflib/adt/adt_distinct_array_type.h"

 int ValidateRequiredVersion(const char *);
 char *LUA_GetFieldToString(const char *key);
char *LUA_GetFieldToStringMax(const char *key, size_t max_string_sz);
 int LUA_GetFieldToInteger(const char *key);
size_t LUALoadAllowedOrigins(lua_State *L, DistinctArray * _Nonnull origins_hashmap_ptr);

#include <ufsrv_core/system/utils_json.h>

 #endif

