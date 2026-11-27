#
# cmake-modules/ufsrv_config_bootstrap.cmake
#
# Bootstrap the uniform composition directory structure at configure time.
# Ensures the deep-tree baseline exists before the generator/composer pipeline
# runs.  Copies invariant files and the canonical shim verbatim from the
# ufsrvcorelib subtree — these are untracked, refreshed on every configure.
#
# Requires:
#   _UFSRVCORELIB_ROOT — path to the ufsrvcorelib subtree / in-source copy
#
# Creates:
#   src/ufsrv_core/ufsrv_config/             — invariant sources + generated/
#   include/ufsrvcorelib/ufsrv_core/ufsrv_config/ — canonical shim
#

set(_DEEP_PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_core/ufsrv_config")
set(_DEEP_PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include/ufsrvcorelib/ufsrv_core/ufsrv_config")

file(MAKE_DIRECTORY "${_DEEP_PRIVATE}/generated")
file(MAKE_DIRECTORY "${_DEEP_PUBLIC}")

# Staging directory must exist before ufsrvcorelib's generator module writes
# _clean_orphans.sh via file(WRITE) at configure time.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/dev/scripts/ufsrv_config_generator/ufnetcorelib/generated")

# .gitkeep — anchors empty directories in git before any files are installed
if(NOT EXISTS "${_DEEP_PRIVATE}/generated/.gitkeep")
    file(WRITE "${_DEEP_PRIVATE}/generated/.gitkeep" "")
    execute_process(COMMAND ${GIT_EXECUTABLE} add "${_DEEP_PRIVATE}/generated/.gitkeep"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" ERROR_QUIET)
endif()
if(NOT EXISTS "${_DEEP_PUBLIC}/.gitkeep")
    file(WRITE "${_DEEP_PUBLIC}/.gitkeep" "")
    execute_process(COMMAND ${GIT_EXECUTABLE} add "${_DEEP_PUBLIC}/.gitkeep"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" ERROR_QUIET)
endif()

# Invariant sources — verbatim from ufsrvcorelib (canonical include paths)
set(_INVARIANT_SRC "${_UFSRVCORELIB_ROOT}/src/ufsrv_core/ufsrv_config")
foreach(_F IN ITEMS ufsrv_config.c ufsrv_config_generic_callbacks.c ufsrv_config_generic_callbacks_priv.h)
    file(COPY "${_INVARIANT_SRC}/${_F}" DESTINATION "${_DEEP_PRIVATE}")
endforeach()

# Canonical shim — instantiated locally so "generated/" resolves to LOCAL tree
file(COPY "${_UFSRVCORELIB_ROOT}/include/ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_shim.h"
     DESTINATION "${_DEEP_PUBLIC}")

message(STATUS "ufsrv_config: deep tree bootstrapped — invariant copies + shim from subtree")
