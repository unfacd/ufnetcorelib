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

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <main_ufnetcorelib.h>
#include <error.h>

#include <ufnet_msgcore/misc.h>
#include <sched.h>
#include "uflib/adt/adt_distinct_array_type.h"
#include "uflib/adt/adt_distinct_array.h"
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_shim.h>

#define LUA_CTX UfsrvConfigLuaHandle()

 /*
 ** "+x.y" --> c_version >= "x.y"
 ** "-x.y" --> c_version <= "x.y"
 ** "x.y" --> c_version == "x.y"
 ** return 1 if any of the above conditions is true
 */
 int ValidateRequiredVersion(const char *s)
 {
  char per=0;
  const char *p;
  extern const char *ufsrv_version;

	if (IS_STR_LOADED(s)) {
      float cv, /*current version*/
            v;

       p = s;
	   sscanf(p, "%f", &v);
	   sscanf(ufsrv_version, "%f", &cv);
 
		if ((*p=='+') || (*p=='-'))  per = *p++;

		if (!per) {
		   return (!(strcmp(ufsrv_version, p)));
		 } else {
			if (per == '+')  return (cv >= v);
			else  return (cv <= v);
		 }
	 } 

   return 0;

 } /**/

 //caller must free

char * __attribute_deprecated_msg__("Use ufsrv_config module")
LUA_GetFieldToString(const char *key)
{
	lua_pushstring(UfsrvConfigLuaHandle(), key);
	lua_gettable(UfsrvConfigLuaHandle(), -2);  /* get background[key] */

	if (!lua_isstring(UfsrvConfigLuaHandle(), -1)) return "";

	char *result = mystrdup(lua_tostring(UfsrvConfigLuaHandle(), -1));

	lua_pop(UfsrvConfigLuaHandle(), 1);  /* remove entry */

	return result;

}

char * __attribute_deprecated_msg__("Use ufsrv_config module")
LUA_GetFieldToStringMax(const char *key, size_t max_string_sz)
{
  lua_pushstring(UfsrvConfigLuaHandle(), key);
  lua_gettable(UfsrvConfigLuaHandle(), -2);

  if (!lua_isstring(UfsrvConfigLuaHandle(), -1)) return "";

  char *result = strndup(lua_tostring(UfsrvConfigLuaHandle(), -1), max_string_sz);

  lua_pop(UfsrvConfigLuaHandle(), 1);  /* remove entry */

  return result;

}

/* assume that table is on the stack top */
int __attribute_deprecated_msg__("Use ufsrv_config module")
LUA_GetFieldToInteger(const char *key)
{
	int result;

	lua_pushstring(UfsrvConfigLuaHandle(), key);
	lua_gettable(UfsrvConfigLuaHandle(), -2);  /* get background[key] */

	if (!lua_isnumber(UfsrvConfigLuaHandle(), -1))return -1;
	//error(UfsrvConfigLuaHandle(), "LUA_GetFieldToInteger: invalid component in table");

	result = (int)lua_tonumber(UfsrvConfigLuaHandle(), -1);

	lua_pop(UfsrvConfigLuaHandle(), 1);  /* remove number */

	return result;
}

#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#include <sys/syscall.h>
static pid_t gettid (void);
static pid_t gettid (void)
{
    return syscall( __NR_gettid );
}
#endif

/**
 * Reads a Lua file and extracts all string values from the global table "allowed_origins".
 *
 * @param L        Lua state (already opened)
 * @param filename Path to the Lua script
 * @param out_count Pointer to an int that will receive the number of strings found
 * @return Dynamically allocated NULL‑terminated array of strings (char**),
 *         or NULL on error or if the table is missing/empty.
 *         The caller must free each string and the array itself.
 */
size_t __attribute_deprecated_msg__("Use ufsrv_config module")
LUALoadAllowedOrigins(lua_State *L, DistinctArray * _Nonnull origins_hashmap_ptr) {
  lua_getglobal(L, "allowed_origins");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 0;
  }

  size_t count = 0;
  lua_pushnil(L);  // first key
  while (lua_next(L, -2) != 0) {
    if (lua_type(L, -1) == LUA_TSTRING) { // Stack now: table, key, value
      const char *str = strdup(lua_tostring(L, -1));
      DistinctArrayPut(origins_hashmap_ptr, &origins_hashmap_ptr->stored_value_idx, (uint8_t **) &origins_hashmap_ptr->value_block_storage, (uint8_t *)str, strlen(str) + 1); //+1 for null terminator
      count++;
    }
    lua_pop(L, 1); // pop value, keep key for next iteration
  }

  lua_pop(L, 1); //pop the table

  return count;

}
