#
# cmake-modules/ufsrv_config_module.cmake
#
# ufsrv_config self-test module — canonical composition pipeline.
#
# Contract:
#   Include this file with the guard clause already satisfied:
#     - UFSRVCORELIB_MAINTAINER_MODE=ON
#     - UFSRVCORELIB_USE_SYSTEM=OFF
#     - a site file declaring the canonical ufsrvcorelib checkout
#   The _PROJECT_NAME target must already exist.
#
# Division of labour (design contract §1.1):
#   P2 — composition is an EXPLICIT act.  Nothing here wires the compose target
#        into the default build.  It is invoked deliberately.
#   P3 — the pipeline is DORMANT.  The committed deep-tree artefacts are the
#        product and are compiled directly; a build regenerates nothing.
#
# Targets defined here:
#   ufsrv_config_compose_host  — runs the orchestrator (manual, idempotent)
#   ufsrv_config_check_host    — integrity guard, fails loudly, never composes
#
# Removed relative to the previous N=1 module: the configure-time stub seeder,
# the inherited ufsrv_config_install path, and the ufsrv_config_rewrite_generated
# target.  Those were the P2/P3 violations.
#

message(STATUS "ufsrv_config: self-test module ON — canonical composition pipeline")

# Generator binary needs lua.h for the catalogue <lua.h> include
find_path(LUA_INCLUDE_DIR lua.h PATHS /opt/include REQUIRED)

# ── Site-declared roots ────────────────────────────────────────────────
# The toolchain root is the SAME ufsrvcorelib checkout the composer reads, so
# configure-time and compose-time never diverge (G16), and the host's own
# library is composed from its working tree (G17).
if(NOT DEFINED UFSRV_CONFIG_HOST_CANONICAL_SUB)
    message(FATAL_ERROR "ufsrv_config: no site file loaded — UFSRV_CONFIG_HOST_CANONICAL_SUB is unset.\n"
                        "  Expected cmake-modules/ufsrv_config_host_site.cmake at the project root.")
endif()
set(_UFSRVCORELIB_ROOT "${UFSRV_CONFIG_HOST_CANONICAL_SUB}")
if(NOT IS_DIRECTORY "${_UFSRVCORELIB_ROOT}")
    message(FATAL_ERROR "ufsrv_config: canonical ufsrvcorelib checkout not found: ${_UFSRVCORELIB_ROOT}")
endif()

list(PREPEND CMAKE_MODULE_PATH "${_UFSRVCORELIB_ROOT}/cmake-modules")

set(_HOST_MODULES "${_UFSRVCORELIB_ROOT}/cmake-modules/ufsrv_config/host")

# ── Compile-time indicator for self-test mode ──────────────────────────
target_compile_definitions(${_PROJECT_NAME} PRIVATE UFSRV_CONFIG_MODULE_UFNETCORELIB)

# ── Include paths for deep-layout compilation ──────────────────────────
target_include_directories(${_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/ufsrvcorelib
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_core/ufsrv_config
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_core/ufsrv_config/generated
)
target_include_directories(${_PROJECT_NAME} PUBLIC
    ${_UFSRVCORELIB_ROOT}/include
    ${_UFSRVCORELIB_ROOT}/include/ufsrvcorelib
)

# ── Sources — compiled from the committed deep tree ────────────────────
# Invariant copies (untracked, refreshed only by an explicit compose)
target_sources(${_PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_core/ufsrv_config/ufsrv_config.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_core/ufsrv_config/ufsrv_config_generic_callbacks.c"
)
# The master include aggregates the three generated .c fragments; compiling
# them separately would duplicate every symbol (G6).  Evidence: the host pattern
# lists exactly these three — ufsrvwebsock/src/CMakeLists.txt:168-171.
set(_MASTER_GEN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_core/ufsrv_config/generated/ufsrv_config_getters_setters_generated.c")
target_sources(${_PROJECT_NAME} PRIVATE "${_MASTER_GEN_SRC}")
set_source_files_properties("${_MASTER_GEN_SRC}" PROPERTIES GENERATED TRUE)

# ── Composition targets ────────────────────────────────────────────────
# Compose is standalone: no add_dependencies edge points at it, so no build
# ever regenerates as a side effect (P2).  The guard IS wired in, so a build
# against an incomplete deep tree fails loudly rather than silently (P3).
add_custom_target(ufsrv_config_compose_host
    COMMAND ${CMAKE_COMMAND}
        "-DUFSRV_CONFIG_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DUFSRV_CONFIG_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DUFSRV_CONFIG_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DUFSRV_CONFIG_HOST_SITE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake-modules/ufsrv_config_host_site.cmake"
        -P "${_HOST_MODULES}/ufsrv_config_compose_host.cmake"
    COMMENT "ufsrv_config: compose (resolve → generate → compose → deep-install → merge → rewrite → verify)"
    VERBATIM)

add_custom_target(ufsrv_config_check_host
    COMMAND ${CMAKE_COMMAND}
        "-DUFSRV_CONFIG_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DUFSRV_CONFIG_HOST_PRIVATE_ROOT=${UFSRV_CONFIG_HOST_PRIVATE_ROOT}"
        "-DUFSRV_CONFIG_HOST_PUBLIC_ROOT=${UFSRV_CONFIG_HOST_PUBLIC_ROOT}"
        -P "${_HOST_MODULES}/ufsrv_config_check_host.cmake"
    COMMENT "ufsrv_config: integrity check (deep tree complete)"
    VERBATIM)
add_dependencies(${_PROJECT_NAME} ufsrv_config_check_host)
