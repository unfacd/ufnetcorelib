#
# cmake-modules/ufsrv_config_rewrite_master_include.cmake
#
# Rewrites angle-bracket includes of generated assets in installed generated
# .c files so they resolve to ufnetcorelib's LOCAL tree, not the ufsrvcorelib
# subtree.  Invoked after ufsrv_config_install, one file at a time.
#
# Rewrite rules (only paths containing "generated/" are touched):
#
#   <ufsrvcorelib/.../generated/*.c>  →  "*.c"
#       Sibling .c files in src/ufsrv_core/ufsrv_config/generated/.
#
#   <ufsrvcorelib/.../generated/*.h>  →  "ufsrv_core/ufsrv_config/generated/*.h"
#       LOCAL public generated headers; resolves via -I include/ufsrvcorelib
#       (PRIVATE include path on the library target).
#
# Canonical type headers (<ufsrvcorelib/.../ufsrv_config.h> — NOT under
# generated/) are left alone: they correctly come from the subtree.
#

cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS "${FILE}")
    message(FATAL_ERROR "ufsrv_config_rewrite: FILE='${FILE}' not found")
endif()

file(READ "${FILE}" _content)

# Rule 1: generated/*.c → quoted basename (sibling in same directory)
string(REGEX REPLACE
    "#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/([^>]+\\.c)>"
    "#include \"\\1\""
    _content "${_content}")

# Rule 2: generated/*.h → quoted LOCAL path
string(REGEX REPLACE
    "#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/([^>]+\\.h)>"
    "#include \"ufsrv_core/ufsrv_config/generated/\\1\""
    _content "${_content}")

file(WRITE "${FILE}" "${_content}")
message(STATUS "ufsrv_config_rewrite: ${FILE} — generated includes → LOCAL")
