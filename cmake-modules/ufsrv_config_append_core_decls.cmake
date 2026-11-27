#
# cmake-modules/ufsrv_config_append_core_decls.cmake
#
# Appends ufsrvcorelib's generated accessor declarations to ufnetcorelib's
# own generated declarations header, wrapped in #ifdef UFSRV_CONFIG_MODULE_UFNETCORELIB.
# Idempotent — strips any previously appended block before writing.
#
# Invoked via cmake -P:
#   cmake -DSRC=<ufsrvcorelib-decls> -DDST=<ufnetcorelib-decls>
#         -P cmake-modules/ufsrv_config_append_core_decls.cmake
#

cmake_minimum_required(VERSION 3.20)

if(NOT EXISTS "${SRC}")
    message(WARNING "ufsrv_config_append_core_decls: SRC='${SRC}' not found — skipping")
    return()
endif()

file(READ "${SRC}" _core_decls)

# Strip include guard lines (exact match)
string(REPLACE "#ifndef UFSRV_CORE_UFSRV_CONFIG_ACCESSOR_DECLS_GENERATED_H\n" "" _core_decls "${_core_decls}")
string(REPLACE "#define UFSRV_CORE_UFSRV_CONFIG_ACCESSOR_DECLS_GENERATED_H\n" "" _core_decls "${_core_decls}")
string(REPLACE "\n#endif /* UFSRV_CORE_UFSRV_CONFIG_ACCESSOR_DECLS_GENERATED_H */\n" "" _core_decls "${_core_decls}")

# Strip trailing newline from core decls
string(STRIP "${_core_decls}" _core_decls)

# Read destination and strip any previous append blocks
set(_MARKER "/* UFSRV_CONFIG_APPEND_BLOCK_BEGIN */")
file(READ "${DST}" _dst_content)
string(FIND "${_dst_content}" "${_MARKER}" _marker_pos)
while(_marker_pos GREATER -1)
    string(SUBSTRING "${_dst_content}" 0 ${_marker_pos} _dst_content)
    string(STRIP "${_dst_content}" _dst_content)
    string(FIND "${_dst_content}" "${_MARKER}" _marker_pos)
endwhile()

file(WRITE "${DST}"
    "${_dst_content}\n\n"
    "/* UFSRV_CONFIG_APPEND_BLOCK_BEGIN */\n"
    "/* ── Forward declarations: ufsrvcorelib core accessor methods ──\n"
    " *\n"
    " * This section was appended by the ufnetcorelib self-test composition\n"
    " * pipeline (ufsrv_config_append_core_decls.cmake).  It forward-declares\n"
    " * getter/setter methods generated from the ufsrvcorelib ufsrv_config\n"
    " * catalogue that are not part of ufnetcorelib's own catalogue.\n"
    " *\n"
    " * These declarations take effect only when UFSRV_CONFIG_MODULE_UFNETCORELIB\n"
    " * is defined, which happens when ufnetcorelib is configured with:\n"
    " *\n"
    " *   cmake --preset dev-ufnetcorelib-ufsrvconfig\n"
    " *   cmake -DUFSRV_CONFIG_MODULE_UFNETCORELIB=ON -DUFSRVCORELIB_MAINTAINER_MODE=ON\n"
    " *\n"
    " * In natural state (UFSRV_CONFIG_MODULE_UFNETCORELIB=OFF) these\n"
    " * declarations are invisible — zero ufsrv_config symbols are compiled\n"
    " * into the library.\n"
    " */\n"
    "#ifdef UFSRV_CONFIG_MODULE_UFNETCORELIB\n"
    "${_core_decls}\n"
    "#endif /* UFSRV_CONFIG_MODULE_UFNETCORELIB */\n"
)

message(STATUS "ufsrv_config_append_core_decls: appended ufsrvcorelib declarations to ${DST}")
