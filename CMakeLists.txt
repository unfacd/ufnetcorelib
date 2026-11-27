
# ufnetcorelib — Merged ufnet_core + ufnet_msgcore
#
# Dual-mode (mirrors uflib pattern):
#   IS_ROOT_PROJECT=ON  → STATIC library + install + pkg-config + CPack
#   IS_ROOT_PROJECT=OFF → OBJECT library (compiled into parent via add_subdirectory)
#
# Usage:
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#   cmake --install build --prefix /usr/local
#   cmake --build build --target package    # .deb

cmake_minimum_required(VERSION 3.20)

set(_PROJECT_NAME "ufnetcorelib")
string(TOUPPER ${_PROJECT_NAME} _PROJECT_NAME_X)

MACRO(getVersionPatchLevel name)
    SET(VERSION_REGEX "^#define ${name} (.+)$")
    FILE(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/ufnetcorelib/version.h"
            VERSION_BIT REGEX ${VERSION_REGEX})
    STRING(REGEX REPLACE ${VERSION_REGEX} "\\1" ${name} "${VERSION_BIT}")
ENDMACRO(getVersionPatchLevel)

getVersionPatchLevel(${_PROJECT_NAME_X}_MAJOR)
getVersionPatchLevel(${_PROJECT_NAME_X}_MINOR)
getVersionPatchLevel(${_PROJECT_NAME_X}_PATCH)
getVersionPatchLevel(${_PROJECT_NAME_X}_INTERNAL)
SET(${_PROJECT_NAME_X}_VERSION "${${_PROJECT_NAME_X}_MAJOR}.${${_PROJECT_NAME_X}_MINOR}.${${_PROJECT_NAME_X}_PATCH}")
# The incremental counter is optional: a repo that does not define <LIB>_INTERNAL
# in version.h keeps the three-component form rather than gaining a trailing dot.
if(NOT "${${_PROJECT_NAME_X}_INTERNAL}" STREQUAL "")
    SET(${_PROJECT_NAME_X}_VERSION "${${_PROJECT_NAME_X}_VERSION}.${${_PROJECT_NAME_X}_INTERNAL}")
endif()
MESSAGE("Detected ${_PROJECT_NAME_X} version: ${${_PROJECT_NAME_X}_VERSION}")

project(${_PROJECT_NAME}
    VERSION ${${_PROJECT_NAME_X}_VERSION}
    LANGUAGES C
)

# ── ufsrvcorelib: determine if packages/ufsrvcorelib is a genuine git subtree ──
# A real git subtree carries merge-commit metadata (git-subtree-dir:).  An
# accidental git add of a FetchContent clone does not.  Check commit history
# for the canonical subtree marker injected by "git subtree add".
execute_process(
    COMMAND ${GIT_EXECUTABLE} log -1 --format=%H
        --grep="git-subtree-dir: packages/ufsrvcorelib"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _UFSRVCORELIB_SUBTREE_COMMIT
    ERROR_QUIET
)
if(_UFSRVCORELIB_SUBTREE_COMMIT STREQUAL "")
    set(UFSRVCORELIB_IS_SUBTREE 1)   # not a genuine subtree
else()
    set(UFSRVCORELIB_IS_SUBTREE 0)   # genuine subtree — metadata found
endif()

# ── ufsrvcorelib: build dependency ──────────────────────────────────────────
# ufnetcorelib compiles against ufsrvcorelib headers and consumers link it.
# Resolution order: a site-declared checkout, then a git subtree carried in the
# tree, then the installed library, then (maintainer mode only) a fresh fetch.
# This keeps the monorepo self-contained while a standalone clone uses the
# installed package.
option(UFSRVCORELIB_MAINTAINER_MODE
    "Allow an in-source ufsrvcorelib fetch for standalone development"
    OFF)
option(UFSRVCORELIB_USE_SYSTEM
    "Use the installed ufsrvcorelib when no in-source checkout is carried"
    ON)

# ── ufsrv_config: site declaration (optional, data only) ──────────────────────
# Included before dependency detection so that a site which names its own
# ufsrvcorelib checkout suppresses the configure-time fetch and the destructive
# REMOVE_RECURSE below.  The developer owns which revision is in play.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake-modules/ufsrv_config_host_site.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake-modules/ufsrv_config_host_site.cmake")
endif()

if(DEFINED UFSRV_CONFIG_HOST_CANONICAL_SUB AND IS_DIRECTORY "${UFSRV_CONFIG_HOST_CANONICAL_SUB}")
    # Site-declared checkout — used in place.  No clone, no deletion, no network.
    set(UFSRVCORELIB_USE_SYSTEM OFF)
    set(UFSRVCORELIB_USE_IN_SOURCE_SITE TRUE)
    set(UFSRVCORELIB_DEPENDENCY_MODE "site")
elseif(UFSRVCORELIB_IS_SUBTREE EQUAL 0)
    set(UFSRVCORELIB_USE_SYSTEM OFF)
    set(UFSRVCORELIB_USE_IN_SOURCE_SUBTREE TRUE)
    set(UFSRVCORELIB_DEPENDENCY_MODE "subtree")
elseif(UFSRVCORELIB_USE_SYSTEM)
    if(UFSRV_CONFIG_MODULE_UFNETCORELIB)
        message(FATAL_ERROR "UFSRV_CONFIG_MODULE_UFNETCORELIB requires in-source ufsrvcorelib (UFSRVCORELIB_USE_SYSTEM=OFF)")
    endif()
    find_package(ufsrvcorelib REQUIRED)
    set(UFSRVCORELIB_DEPENDENCY_MODE "system")
elseif(UFSRVCORELIB_MAINTAINER_MODE)
    # Neither a checkout nor a system install — fetch a fresh copy.
    set(UFSRVCORELIB_USE_IN_SOURCE_PACKAGES TRUE)
    set(UFSRVCORELIB_DEPENDENCY_MODE "packages")
    include(FetchContent)
    FetchContent_Declare(ufsrvcorelib
        GIT_REPOSITORY git.ro:ufsrvcorelib.git
        GIT_TAG master
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/packages/ufsrvcorelib
    )
else()
    message(FATAL_ERROR
        "No ufsrvcorelib found: install it, carry a subtree at "
        "packages/ufsrvcorelib, or reconfigure with -DUFSRVCORELIB_MAINTAINER_MODE=ON")
endif()

message(STATUS "ufsrvcorelib: dependency=${UFSRVCORELIB_DEPENDENCY_MODE}")

# ── Guard: UFSRV_CONFIG_MODULE_UFNETCORELIB preconditions ──────────────────────────
if(UFSRV_CONFIG_MODULE_UFNETCORELIB AND UFSRVCORELIB_USE_SYSTEM)
    message(FATAL_ERROR "UFSRV_CONFIG_MODULE_UFNETCORELIB requires in-source ufsrvcorelib")
endif()

# ── Shared ufsrv build options ───────────────────────────────────────────────
# clang enforcement, base compile flags, and the UFSRV_BUILD_WITH_{ASAN,MSAN,TSAN}
# toggles live in the shared module. Warning flags are THIS repo's own policy —
# set them before including the module. Invoke via a preset: `cmake --preset debug`.
#
# _UFSRV_BUILD_FLAVOUR is set BEFORE the module include so the build digest reports
# the correct output kind (OBJECT/STATIC/SHARED+STATIC) — the module reads it.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(BUILD_SHARED_LIBS)
        set(_UFSRV_BUILD_FLAVOUR "SHARED+STATIC")
    else()
        set(_UFSRV_BUILD_FLAVOUR "STATIC")
    endif()
else()
    set(_UFSRV_BUILD_FLAVOUR "OBJECT")
endif()

list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake-modules)
set(UFSRV_C_WARNING_FLAGS
    -Wall -Wimplicit-fallthrough -Wno-unused-label -Wno-unused-variable
    -Wno-comment -Wno-nullability-completeness -Wno-unknown-attributes)
set(UFSRV_C_DEBUG_FLAGS_DEFAULT -ggdb3)   # this repo's Debug-info scope (override per profile via -DUFSRV_C_DEBUG_FLAGS=...)

include(ufsrv_build_options)

add_definitions(-DHAVE_CONFIG_UFNETCORELIB_H)

# Probe system capabilities before generating config_ufnetcorelib.h
include(CheckIncludeFile)
include(CheckStructHasMember)
include(CheckFunctionExists)
check_struct_has_member("struct sockaddr" sa_len sys/socket.h HAVE_SA_LEN)
check_function_exists(hstrerror HAVE_HSTRERROR)
check_function_exists(inet_pton HAVE_INET_PTON)
check_function_exists(inet_ntop HAVE_INET_NTOP)
check_function_exists(recvmmsg HAVE_RECVMMSG)
# Valgrind — probe system path first, then /opt/include (OPT install)
find_path(VALGRIND_INCLUDE_DIR valgrind/valgrind.h
    HINTS ${VALGRIND_ROOT}/include /opt/include)
include(CheckIncludeFile)
if(VALGRIND_INCLUDE_DIR)
    set(CMAKE_REQUIRED_INCLUDES "${VALGRIND_INCLUDE_DIR}")
endif()
check_include_file(valgrind/valgrind.h HAVE_VALGRIND)
check_include_file(valgrind/drd.h HAVE_VALGRIND_DRD)
check_include_file(valgrind/memcheck.h HAVE_VALGRIND_MEMCHECK)
unset(CMAKE_REQUIRED_INCLUDES)

option(UF_TESTING "Enable testing mode" ON)
option(UF_DEV "Enable development mode" ON)
option(UFNET_DEBUG_BUILD_L2 "Verbose level-2 debug logging" OFF)
option(UFNET_DEBUG_BUILD_L3 "Very verbose level-3 debug logging (per-object traces)" OFF)
option(UFSRV_CONFIG_MODULE_UFNETCORELIB "Enable ufsrv_config self-test mode (N=1 composition)" OFF)
option(UFNETCORELIB_PROTOBUF_C_GENERATOR
       "Build protoc-gen-c for protobuf/ schemas (maintainer-only; needs libprotoc-dev)" OFF)
if(UFSRV_CONFIG_MODULE_UFNETCORELIB)
    set(UFSRV_CONFIG_GENERATOR_ON ON CACHE BOOL "Run ufsrv_config_generator at build time" FORCE)
endif()
if(UF_TESTING)
    set(__UF_TESTING 1)
else()
    set(__UF_TESTING 0)
endif()
if(UF_DEV)
    set(__UF_DEV 1)
else()
    set(__UF_DEV 0)
endif()
# L3 implies L2, and either implies the base debug build switch.
if(UFNET_DEBUG_BUILD_L3)
    set(UFNET_DEBUG_L3 1)
    set(UFNET_DEBUG_L2 1)
else()
    set(UFNET_DEBUG_L3 0)
    if(UFNET_DEBUG_BUILD_L2)
        set(UFNET_DEBUG_L2 1)
    else()
        set(UFNET_DEBUG_L2 0)
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(UFNET_DEBUG_BUILD 1)
else()
    set(UFNET_DEBUG_BUILD 0)
endif()
if(UFNET_DEBUG_L2 OR UFNET_DEBUG_L3)
    set(UFNET_DEBUG_BUILD 1)
endif()
add_compile_definitions(UFNET_DEBUG_BUILD=${UFNET_DEBUG_BUILD}
                        UFNET_DEBUG_BUILD_L2=${UFNET_DEBUG_L2}
                        UFNET_DEBUG_BUILD_L3=${UFNET_DEBUG_L3})

# Generate config_ufnetcorelib.h from template — substituted at configure time
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config_ufnetcorelib.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config_ufnetcorelib.h
    @ONLY
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IS_ROOT_PROJECT ON)
else()
    set(IS_ROOT_PROJECT OFF)
endif()

option(_PACKAGE_TESTS "Enable test targets" OFF)
option(_PACKAGE_INSTALL "Enable install target" ${IS_ROOT_PROJECT})

# ── GTest ──
if(_PACKAGE_TESTS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.0
    )
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# ── Doxygen ──
set(DOXYGEN_OUTPUT_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "Doxygen HTML output directory (default: repo root)")
find_package(Doxygen QUIET)
if(Doxygen_FOUND)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Doxyfile.in
                   ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    add_custom_target(docs
        ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation in ${DOXYGEN_OUTPUT_DIR}/html"
        VERBATIM
    )
    set_target_properties(docs PROPERTIES
        ADDITIONAL_CLEAN_FILES "${DOXYGEN_OUTPUT_DIR}/html"
    )
else()
    message(STATUS "Doxygen not found — 'docs' target disabled.")
endif()

# ── Dependencies ──
#
#Guard the call for `uflib` to resolve in-source, so as not to not hard-fail
#on `find_package(uflib)`. It is harmless in every other mode: with a
##system `uflib` the target is absent, so `find_package` runs exactly as before
#
if(NOT TARGET uflib::uflib)
    find_package(uflib REQUIRED)
endif()
find_package(ufsrvrxlib REQUIRED)   # reactive-extensions layer (futures/coroutines/scheduler)
find_package(OpenSSL REQUIRED)

# External libs — discovered through the package system, with the provisioning
# prefix (/opt) offered as a hint rather than a hardcoded path or soname.
list(PREPEND CMAKE_PREFIX_PATH "/opt")
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
pkg_check_modules(MARIADB REQUIRED IMPORTED_TARGET libmariadb)
pkg_check_modules(JEMALLOC REQUIRED IMPORTED_TARGET jemalloc)
find_package(Lua REQUIRED)
find_library(SIGNAL_FFI_LIB signal_ffi HINTS /opt/lib)

set(JSONC_LIB PkgConfig::JSONC)
set(UTF8PROC_LIB PkgConfig::UTF8PROC)
set(MARIADB_LIB PkgConfig::MARIADB)
set(JEMALLOC_LIB PkgConfig::JEMALLOC)
set(LUA_LIB ${LUA_LIBRARIES})

# System libs
find_library(CURL_LIB curl)
find_library(BSD_LIB bsd)
find_library(DL_LIB dl)
find_library(SYSTEMD_LIB systemd)

# Vendored/internal deps — headers from ufsrvapi source (TEMPORARY:
# these need their own packages; for now point at the host source tree)

# Threads
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ── Build target ──
# Always OBJECT — CLion sees one target. Sources are listed explicitly (no globbing)
# by each module via target_sources(); see src/ufnet_core/CMakeLists.txt and
# src/ufnet_msgcore/CMakeLists.txt, wired in below. Create the OBJECT library empty
# first so those subdirectories can populate it — $<TARGET_OBJECTS:> and the tests
# read the collected sources at generate time.
add_library(${_PROJECT_NAME} OBJECT)

if(IS_ROOT_PROJECT)
    # Static archive always built (libufnetcorelib.a).
    add_library(${_PROJECT_NAME}_static STATIC $<TARGET_OBJECTS:${_PROJECT_NAME}>)
    set_target_properties(${_PROJECT_NAME}_static PROPERTIES OUTPUT_NAME ${_PROJECT_NAME})

    if(BUILD_SHARED_LIBS)
        # Interim switch: until every public symbol carries PUBLIC_API, exporting
        # all globals keeps the shared library usable by consumers. Set OFF to
        # enforce the annotation sweep; a gap then fails at link time.
        option(UFNETCORELIB_SHARED_EXPORT_ALL
            "Export every global from the shared library (interim; disable once PUBLIC_API coverage is complete)" ON)

        if(UFNETCORELIB_SHARED_EXPORT_ALL)
            set(_UFNETCORELIB_SHARED_VISIBILITY default)
        else()
            set(_UFNETCORELIB_SHARED_VISIBILITY hidden)
        endif()

        # Sources compile once, in the OBJECT target, so visibility must be set
        # there; a preset on the shared target would be inert.
        set_target_properties(${_PROJECT_NAME} PROPERTIES
            C_VISIBILITY_PRESET ${_UFNETCORELIB_SHARED_VISIBILITY})

        # Shared library from the same object files (must already be PIC — see below).
        add_library(${_PROJECT_NAME}_shared SHARED $<TARGET_OBJECTS:${_PROJECT_NAME}>)
        set_target_properties(${_PROJECT_NAME}_shared PROPERTIES
            OUTPUT_NAME ${_PROJECT_NAME}
            VERSION   ${${_PROJECT_NAME_X}_VERSION}
            SOVERSION ${${_PROJECT_NAME_X}_MAJOR})
        add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME}_shared)
    else()
        add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME}_static)
    endif()
else()
    add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME})
endif()

ufsrv_apply_build_options(${_PROJECT_NAME})
ufsrv_build_summary(${_PROJECT_NAME})   # digest target ordered after the core OBJECT lib
if(IS_ROOT_PROJECT)
    ufsrv_apply_build_options(${_PROJECT_NAME}_static)
    ufsrv_build_summary(${_PROJECT_NAME}_static)   # …and after the STATIC rollup, so it prints last
    if(BUILD_SHARED_LIBS)
        ufsrv_apply_build_options(${_PROJECT_NAME}_shared)
        ufsrv_build_summary(${_PROJECT_NAME}_shared)
    endif()
endif()

# ── Include directories ──
target_include_directories(${_PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>        # generated config_ufnetcorelib.h (must come first)
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/ufnetcorelib> # internal <header.h> → include/ufnetcorelib/
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/packages>
        $<INSTALL_INTERFACE:include>
        $<INSTALL_INTERFACE:include/ufnetcorelib>
    PRIVATE
        ${VALGRIND_INCLUDE_DIR}
        ${LUA_INCLUDE_DIR}                                  # <lua.h> from the resolved Lua
        ${CMAKE_CURRENT_SOURCE_DIR}/src                     # <ufnet_core/...> → src/ufnet_core/...
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_core
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_msgcore
        # Subdirectory paths for internal resolution
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_core/cache_backend
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_core/jobworkers
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_core/oauth
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_core/protocol
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_core/protocol_sandbox
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_core/ratelimit_v2
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_core/session
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_msgcore/donations
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_msgcore/fence
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_msgcore/msgqueue_backend
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_msgcore/type_providers
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_msgcore/user
        # External headers (/opt)
        /opt/include
        /opt/include/mariadb
        # Vendored deps, extracted to packages/ (each carries its own CMakeLists).
        # Some headers use <dir/file.h>, others use just <file.h>.
        ${CMAKE_CURRENT_SOURCE_DIR}/packages                # <hiredis/hiredis.h>, <curve25519/...>, <curve25519/ed25519/...>
        ${CMAKE_CURRENT_SOURCE_DIR}/packages/hiredis        # <hiredis.h>
        ${CMAKE_CURRENT_SOURCE_DIR}/packages/curve25519     # <curve25519-donna.h>
        src/ufsrv_msgcore/zkgroup_utils       # <utils_zkgroup.h>  (TODO: vendored — need extraction)
        # TODO: ${UFSRVAPI_SRC}/include — user_prefs, etc. — needs resolution
)

# ── ufsrvcorelib: fetch fresh copy from git (packages mode, configure-time only) ──
if(UFSRVCORELIB_MAINTAINER_MODE AND UFSRVCORELIB_USE_IN_SOURCE_PACKAGES)
    file(REMOVE_RECURSE ${CMAKE_CURRENT_SOURCE_DIR}/packages/ufsrvcorelib)
    file(REMOVE_RECURSE "${CMAKE_CURRENT_BINARY_DIR}/_deps/ufsrvcorelib-subbuild")
    set(_PACKAGE_TESTS OFF CACHE BOOL "" FORCE)
    set(_PACKAGE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(ufsrvcorelib)
    if(NOT UFSRVCORELIB_IS_SUBTREE EQUAL 0)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rm --cached -r --ignore-unmatch
                packages/ufsrvcorelib/
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ERROR_QUIET
        )
    endif()
endif()

# ── Link dependencies ──
# One list per visibility class: the OBJECT target uses both, and so does the
# shared library's own link (see "Link closure" below) — naming the set twice by
# hand is how the two drift apart.
set(_UFNETCORELIB_PUBLIC_LIBS
    uflib::uflib
    ufsrvrxlib::ufsrvrxlib
    ${_PROJECT_NAME}_packages
    OpenSSL::SSL
    OpenSSL::Crypto
)
set(_UFNETCORELIB_PRIVATE_LIBS
    ${JSONC_LIB}
    ${CURL_LIB}
    ${LUA_LIB}
    ${UTF8PROC_LIB}
    ${MARIADB_LIB}
    ${SIGNAL_FFI_LIB}
    ${JEMALLOC_LIB}
    ${CMAKE_THREAD_LIBS_INIT}
    ${BSD_LIB}
    ${DL_LIB}
    ${SYSTEMD_LIB}
    BlocksRuntime
)
target_link_libraries(${_PROJECT_NAME}
    PUBLIC ${_UFNETCORELIB_PUBLIC_LIBS}
    PRIVATE ${_UFNETCORELIB_PRIVATE_LIBS}
)
# wire in ufsrvcorelib whenever it is resolved (system install or in-source)
if(TARGET ufsrvcorelib::ufsrvcorelib)
    target_link_libraries(${_PROJECT_NAME} PRIVATE ufsrvcorelib::ufsrvcorelib)
endif()

# ── Link closure for the shared library ─────────────────────────────────────
# libufnetcorelib.so is built from $<TARGET_OBJECTS:...>, which carries no usage
# requirements: the libraries above reach the static archive through its
# INTERFACE exports, and reached the shared object not at all. It recorded no
# DT_NEEDED entry beyond libc and left every openssl, curl, lua, utf8proc,
# mariadb, jemalloc, systemd and uflib call for the consumer to resolve by hand.
# Naming the same set again as PRIVATE closes the object without changing what
# consumers inherit from the installed target.
if(BUILD_SHARED_LIBS AND IS_ROOT_PROJECT)
    target_link_libraries(${_PROJECT_NAME}_shared PRIVATE
        ${_UFNETCORELIB_PUBLIC_LIBS}
        ${_UFNETCORELIB_PRIVATE_LIBS}
    )
    if(TARGET ufsrvcorelib::ufsrvcorelib)
        target_link_libraries(${_PROJECT_NAME}_shared PRIVATE ufsrvcorelib::ufsrvcorelib)
    endif()
endif()
# ── config_ufsrvcorelib.h for the in-source ufsrvcorelib ─────────────────────
# Two gaps this closes, both present in _every_ in-source mode:
#   1) ufsrvcorelib sets HAVE_CONFIG_UFSRVCORELIB_H via add_definitions() in its
#      OWN directory scope (its CMakeLists.txt:77), so the define never reaches
#      THIS target — ufsrv_secret_key_type.h:21 then skips the include and
#      SECRET_CREDENTIAL_ID (:26) expands to a bare undefined identifier.
#   2) In site mode the canonical checkout is never configured, so its generated
#      config_ufsrvcorelib.h does not exist at all.
# Generated from the checkout's own template + defaults module — one source of
# truth, not a second copy (G16).  Only the generated header's directory goes on
# the include path; the checkout's include/ dirs are propagated separately below.
#
# Not gated on UFSRVCORELIB_MAINTAINER_MODE: the header is needed by every
# configuration that compiles against ufsrvcorelib's headers, and natural state
# is one of them (see the include-path block below).
if(NOT UFSRVCORELIB_USE_SYSTEM)
    if(UFSRVCORELIB_USE_IN_SOURCE_SITE)
        set(_UFSRVCORELIB_CFG_ROOT "${UFSRV_CONFIG_HOST_CANONICAL_SUB}")
    else()
        set(_UFSRVCORELIB_CFG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/packages/ufsrvcorelib")
    endif()
    if(EXISTS "${_UFSRVCORELIB_CFG_ROOT}/config_ufsrvcorelib.h.in")
        option(UF_TESTING "Enable testing mode" ON)
        option(UF_DEV "Enable development mode" ON)
        if(UF_TESTING)
            set(__UF_TESTING 1)
        else()
            set(__UF_TESTING 0)
        endif()
        if(UF_DEV)
            set(__UF_DEV 1)
        else()
            set(__UF_DEV 0)
        endif()
        include("${_UFSRVCORELIB_CFG_ROOT}/cmake-modules/config_defaults_definitions.cmake")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/ufsrvcorelib_config")
        configure_file(
            "${_UFSRVCORELIB_CFG_ROOT}/config_ufsrvcorelib.h.in"
            "${CMAKE_BINARY_DIR}/ufsrvcorelib_config/config_ufsrvcorelib.h"
            @ONLY)
        target_compile_definitions(${_PROJECT_NAME} PRIVATE HAVE_CONFIG_UFSRVCORELIB_H)
        target_include_directories(${_PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/ufsrvcorelib_config")
        message(STATUS "ufsrvcorelib: generated config_ufsrvcorelib.h from ${_UFSRVCORELIB_CFG_ROOT}")
    else()
        message(STATUS "ufsrvcorelib: no config_ufsrvcorelib.h.in under ${_UFSRVCORELIB_CFG_ROOT} — skipping")
    endif()
endif()
# ── canonical ufsrvcorelib include paths (natural state only) ────────────────
# 32 files in src/ and include/ #include <ufsrvcorelib/...> unconditionally, and
# the types they pull in (InstrumentationBackend, CommandLineArgsConfig,
# ServerProcessMode, ConfigfileProcessor, …) are real dependencies, not
# self-test specifics.  Without this path the library does not compile at all,
# which is why the default build has been broken since 333f3084 removed it.
#
# That commit's stated reason — these headers "collide with ufnetcorelib's own
# types (server_run_mode_enum.h vs ufsrv_type.h both defining ServerRunMode)" —
# does not reproduce.  The real hazard is different and narrower: this directory
# also holds ufsrvcorelib's OWN ufsrv_core/ufsrv_config/generated/ headers, and
# putting it ahead of the composed deep tree makes the wrong
# ufsrv_config_field_enums_generated.h win, so every composed field identifier
# goes undeclared.
#
# Hence the guard.  In self-test mode ufsrv_config_module.cmake already adds
# these paths at the right position, behind the composed deep tree — leave it to
# do so.  This block exists for the configuration that has no module: natural
# state.
if(NOT UFSRV_CONFIG_MODULE_UFNETCORELIB)
    if(UFSRVCORELIB_USE_IN_SOURCE_SITE)
        set(_corelib_include_root "${UFSRV_CONFIG_HOST_CANONICAL_SUB}")
    else()
        set(_corelib_include_root "${CMAKE_CURRENT_SOURCE_DIR}/packages/ufsrvcorelib")
    endif()
    target_include_directories(${_PROJECT_NAME} PUBLIC
        ${_corelib_include_root}/include
        ${_corelib_include_root}/include/ufsrvcorelib
    )
endif()

# ── Position-independent code ──
set_target_properties(${_PROJECT_NAME} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME ${_PROJECT_NAME}
)

# ── ufsrvcorelib dependency annotation ──
set(UFSRV_BUILD_DIGEST "${UFSRV_BUILD_DIGEST}"
                       "  ufsrvcorelib_dependency=${UFSRVCORELIB_DEPENDENCY_MODE}")

# ── Sources (explicit, per module) ──
# Each module's CMakeLists.txt calls target_sources(${_PROJECT_NAME} PRIVATE ...).
# The OBJECT target created above must already exist before these run.
add_subdirectory(packages/hiredis)
add_subdirectory(packages/curve25519)
# The generator is a maintainer tool: only the protobuf_* targets pull it in.
add_subdirectory(packages/protobuf-c EXCLUDE_FROM_ALL)
include(cmake/protobuf_schemas.cmake)

include(GNUInstallDirs)
add_library(${_PROJECT_NAME}_packages INTERFACE)
target_link_libraries(${_PROJECT_NAME}_packages INTERFACE
    $<BUILD_INTERFACE:hiredis>
    $<BUILD_INTERFACE:curve25519>
    $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/libhiredis.a>
    $<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/libcurve25519.a>
)
add_subdirectory(src/ufnet_core)
add_subdirectory(src/ufnet_msgcore)
add_subdirectory(src/ufsrv_core/ufsrv_config)

# ── Dynamic version (mirrors the uflib version.c.sh pattern) ──
# src/version.c.sh regenerates version.c on every build, stamping the version
# (from version.h) plus a fresh compile timestamp, builder, and host. The custom
# target carries no OUTPUT, so it is always out of date and runs each build;
# version.c is written into the build dir and compiled into the OBJECT library —
# uflib omits this last target_sources() step, so its accessors never link; we
# include it so ufnetcorelibVersion*() resolve. The STATIC wrapper picks up
# version.c.o automatically via $<TARGET_OBJECTS:>.
add_custom_target(${_PROJECT_NAME}_version
    COMMAND chmod 755 ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/version.c
    COMMENT "Generating version.c with a fresh compilation timestamp"
    VERBATIM
)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/version.c PROPERTIES GENERATED TRUE)
target_sources(${_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.c)

# Domain-side helpers kept out of the core library.
target_sources(${_PROJECT_NAME} PRIVATE src/ufsrvcore_utils.c)
add_dependencies(${_PROJECT_NAME} ${_PROJECT_NAME}_version)

# ── Install ──
if(_PACKAGE_INSTALL)
    set(_UFNETCORELIB_EXPORT_LIBS
        uflib::uflib
        ufsrvrxlib::ufsrvrxlib
        ${_PROJECT_NAME}_packages
        OpenSSL::SSL
        OpenSSL::Crypto
        ${JSONC_LIB}
        ${CURL_LIB}
        ${LUA_LIB}
        ${UTF8PROC_LIB}
        ${MARIADB_LIB}
        ${SIGNAL_FFI_LIB}
        ${JEMALLOC_LIB}
        ${CMAKE_THREAD_LIBS_INIT}
        ${BSD_LIB}
        ${DL_LIB}
        ${SYSTEMD_LIB}
        BlocksRuntime)
    target_link_libraries(${_PROJECT_NAME}_static INTERFACE ${_UFNETCORELIB_EXPORT_LIBS})
    target_include_directories(${_PROJECT_NAME}_static INTERFACE /opt/include)
    if(BUILD_SHARED_LIBS)
        target_link_libraries(${_PROJECT_NAME}_shared INTERFACE ${_UFNETCORELIB_EXPORT_LIBS})
        target_include_directories(${_PROJECT_NAME}_shared INTERFACE /opt/include)
    endif()

    install(TARGETS ${_PROJECT_NAME}_static
        EXPORT ${_PROJECT_NAME}-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} ${CMAKE_INSTALL_INCLUDEDIR}/${_PROJECT_NAME}
            ${CMAKE_INSTALL_INCLUDEDIR}/${_PROJECT_NAME}/packages
    )

    if(BUILD_SHARED_LIBS)
        install(TARGETS ${_PROJECT_NAME}_shared
            EXPORT ${_PROJECT_NAME}-targets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} ${CMAKE_INSTALL_INCLUDEDIR}/${_PROJECT_NAME}
                ${CMAKE_INSTALL_INCLUDEDIR}/${_PROJECT_NAME}/packages
        )
    endif()

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY packages/hiredis packages/curve25519
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${_PROJECT_NAME}/packages
        FILES_MATCHING PATTERN "*.h"
    )

    install(TARGETS hiredis curve25519
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    install(TARGETS ${_PROJECT_NAME}_packages
        EXPORT ${_PROJECT_NAME}-targets
    )

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/config_ufnetcorelib.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${_PROJECT_NAME}
    )

    # pkg-config
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_PROJECT_NAME}.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}.pc
        @ONLY
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    # cmake package config
    include(CMakePackageConfigHelpers)
    set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${_PROJECT_NAME})

    install(EXPORT ${_PROJECT_NAME}-targets
        FILE ${_PROJECT_NAME}Targets.cmake
        NAMESPACE ${_PROJECT_NAME}::
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}ConfigVersion.cmake
        VERSION ${${_PROJECT_NAME_X}_VERSION}
        COMPATIBILITY AnyNewerVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}ConfigVersion.cmake
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    # ── IDE source manifest ──────────────────────────────────────────────────
    # The installed package ships headers and a binary, never sources, so a
    # consumer that links it has no definition to navigate to. Publish the
    # translation units this library actually compiles, together with the
    # checkout they were configured from, so a consumer can index them.
    #
    # Paths are absolute and machine-specific by design: the manifest records
    # where the sources were when the package was built. A consumer must
    # tolerate entries that do not exist on its machine and fall back to
    # whatever it can resolve itself (other checkout location, another host).
    get_target_property(_ufsrv_ide_sources ${_PROJECT_NAME} SOURCES)
    set(_ufsrv_ide_manifest
        "set(${_PROJECT_NAME_X}_IDE_SOURCE_DIR \"${CMAKE_CURRENT_SOURCE_DIR}\")\nset(${_PROJECT_NAME_X}_IDE_SOURCES\n")
    foreach(_ufsrv_ide_src IN LISTS _ufsrv_ide_sources)
        if(NOT IS_ABSOLUTE "${_ufsrv_ide_src}")
            set(_ufsrv_ide_src "${CMAKE_CURRENT_SOURCE_DIR}/${_ufsrv_ide_src}")
        endif()
        # Generated sources live in the build tree, not the source checkout:
        # there is no file for a consumer to navigate to.
        string(FIND "${_ufsrv_ide_src}" "${CMAKE_CURRENT_BINARY_DIR}/" _ufsrv_ide_gen)
        if(_ufsrv_ide_gen EQUAL 0)
            continue()
        endif()
        string(APPEND _ufsrv_ide_manifest "    \"${_ufsrv_ide_src}\"\n")
    endforeach()
    string(APPEND _ufsrv_ide_manifest ")\n")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}-ide-sources.cmake"
        "${_ufsrv_ide_manifest}")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}-ide-sources.cmake
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    # archive-uninstall: move currently installed files to /tmp before installing new version
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/archive_uninstall.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/archive_uninstall.cmake
        @ONLY
    )
    add_custom_target(archive-uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/archive_uninstall.cmake
        COMMENT "Archiving currently installed ${_PROJECT_NAME} to /tmp/archived_${_PROJECT_NAME}_${${_PROJECT_NAME_X}_VERSION}"
        VERBATIM
    )

    # Uninstall target
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
        @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    )

    # CPack
    set(CPACK_PACKAGE_NAME "${_PROJECT_NAME}")
    set(CPACK_PACKAGE_VERSION "${${_PROJECT_NAME_X}_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Core library for ufsrv platform (session mgmt, messaging, HTTP, auth)")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "unfacd works")
    set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libssl3")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    include(CPack)
endif()

# ── ufsrv_config: clean target (always available) ─────────────────────
# Removes untracked composition DEBRIS: the two staging directories, the
# composer scratch output, and the toolchain files copied into the generator
# dir.  Preserves tracked generated files.
#
# It deliberately does NOT remove the invariant copies, the shim or .composed.
# Those are BUILD INPUTS, not debris:
#   * ufsrv_config_module.cmake:66 target_sources() the three invariant copies,
#     so removing them makes configure fail outright;
#   * only the compose recreates them, and the compose is a build target, which
#     cannot run without a successful configure.  Removing them left a tree with
#     no way back.
# Before 15f1b8e8 this was safe by accident: ufsrv_config_module.cmake included
# ufsrv_config_bootstrap_ufnetcorelib, which re-copied all four at configure
# time.  That include was removed when composition became explicit and dormant,
# taking the only restore path with it.  Leaving the files costs nothing — they
# are gitignored and refreshed by the compose.
#
# Works regardless of UFSRV_CONFIG_MODULE_UFNETCORELIB.

# Toolchain debris — copied into generator dir by bootstrap (untracked)
set(_CLEAN_TOOLCHAIN_DEBRIS
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufsrv_config_generator.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufsrv_config_verify.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufsrv_config.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufsrv_config.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufsrv_config_generic_callbacks.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufsrv_config_generic_callbacks_priv.h"
)

set(_CLEAN_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E remove_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufnetcorelib"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufsrvcorelib"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_config"
)
foreach(_F ${_CLEAN_TOOLCHAIN_DEBRIS})
    list(APPEND _CLEAN_COMMANDS COMMAND ${CMAKE_COMMAND} -E rm -f "${_F}")
endforeach()
add_custom_target(ufnetcorelib_ufsrv_config_clean
        ${_CLEAN_COMMANDS}
        COMMENT "ufsrv_config: removing staging + composer scratch + toolchain debris (preserves tracked generated files and build inputs)"
        VERBATIM
)

# ── ufsrv_config: self-test composition (N=1) ─────────────────────────
# When UFSRV_CONFIG_MODULE_UFNETCORELIB=ON the toolchain is borrowed from
# packages/ufsrvcorelib/ and composed sources land in src/ufsrv_config/.
if(UFSRV_CONFIG_MODULE_UFNETCORELIB)
    include(ufsrv_config_module)
endif()

# ── Tests ──
# Added last (not inside the early GTest setup block) so test targets can
# reference the fully configured library, its generated version target
# (${_PROJECT_NAME}_version), and resolved dependencies such as uflib::uflib.
if(_PACKAGE_TESTS)
    add_subdirectory(tests)
endif()
