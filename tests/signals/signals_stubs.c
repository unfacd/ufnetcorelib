/**
 * @file signals_stubs.c
 * @brief Empty stubs for the symbols in misc.c that the signals tests never
 *        exercise.
 *
 * misc.c is a grab-bag: it defines InitSignals() / UfsrvSignalControlSet()
 * (the units under test) alongside deprecated Lua config accessors that pull
 * in lua, uflib and ufsrvcorelib. Linking misc.c drags those symbols into the
 * test executable, so we provide trivial, never-invoked definitions rather
 * than transitively linking lua / uflib / ufsrvcorelib.
 *
 * The linker matches symbols by name only, and none of these stubs is ever
 * called by the tests (they belong to the deprecated Lua helpers), so their
 * exact signatures are immaterial.
 */
#include <stddef.h>

/* Opaque types forward-declared so this TU stays free of lua/uflib headers. */
typedef struct lua_State   lua_State;
typedef struct DistinctArray DistinctArray;

/* extern const char *ufsrv_version — read by ValidateRequiredVersion(). */
const char *ufsrv_version = "test";

/* ufsrvcorelib accessor used by the deprecated Lua helpers. */
lua_State *UfsrvConfigLuaHandle(void) { return NULL; }

/* uflib helper. */
char *mystrdup(const char *s) { (void)s; return NULL; }

/* ── lua_* — only referenced by the deprecated Lua helpers ─────────────── */
void        lua_pushstring(lua_State *L, const char *s) { (void)L; (void)s; }
void        lua_gettable(lua_State *L, int idx)          { (void)L; (void)idx; }
int         lua_isstring(lua_State *L, int idx)          { (void)L; (void)idx; return 0; }
const char *lua_tostring(lua_State *L, int idx)          { (void)L; (void)idx; return NULL; }
void        lua_pop(lua_State *L, int n)                 { (void)L; (void)n; }
int         lua_isnumber(lua_State *L, int idx)          { (void)L; (void)idx; return 0; }
double      lua_tonumber(lua_State *L, int idx)          { (void)L; (void)idx; return 0.0; }
void        lua_getglobal(lua_State *L, const char *n)   { (void)L; (void)n; }
int         lua_istable(lua_State *L, int idx)           { (void)L; (void)idx; return 0; }
void        lua_pushnil(lua_State *L)                    { (void)L; }
int         lua_next(lua_State *L, int idx)              { (void)L; (void)idx; return 0; }
int         lua_type(lua_State *L, int idx)              { (void)L; (void)idx; return 0; }

/* uflib DistinctArrayPut — only referenced by LUALoadAllowedOrigins(). */
void DistinctArrayPut(DistinctArray *a, void *idx, void *storage, void *val, size_t n)
{
  (void)a; (void)idx; (void)storage; (void)val; (void)n;
}
