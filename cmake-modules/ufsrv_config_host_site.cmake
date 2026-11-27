#
# cmake-modules/ufsrv_config_host_site.cmake
#
# Site declaration for ufnetcorelib's ufsrv_config self-test composition.
# Data only — no logic.  Consumed by the canonical orchestrator at
#   ufsrvcorelib/cmake-modules/ufsrv_config/host/ufsrv_config_compose_host.cmake
# via -DUFSRV_CONFIG_HOST_SITE_FILE=<this file>.
#

# This file is included TWICE: at configure time (from CMakeLists.txt) and at
# compose time (from the orchestrator, under `cmake -P`).  In script mode
# CMAKE_SOURCE_DIR is the BUILD dir, not the project root — so resolve the root
# once, here, from whichever variable is authoritative at the time.  Everything
# below reads UFSRV_CONFIG_SOURCE_DIR.
if(NOT DEFINED UFSRV_CONFIG_SOURCE_DIR)
    set(UFSRV_CONFIG_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
endif()

# Composition order.  The first entry owns the toolchain (generator, verifier,
# composer, join script); the order follows the config enum order.
set(UFSRV_CONFIG_HOST_LIBRARIES "ufsrvcorelib;ufnetcorelib")

# Per-library source mode.  Each library resolves to exactly ONE checkout:
#   local:<path>        use that checkout directly (no clone, no network)
#   git:<repo>[#<ref>]  shallow-clone, refreshed when the remote tip advances
#
# ufnetcorelib is the host: it is composed from its own working tree, never a
# clone (G17).  ufsrvcorelib is the external dependency.
set(UFSRV_CONFIG_HOST_SOURCE_ufsrvcorelib "local:${UFSRV_CONFIG_SOURCE_DIR}/../ufsrvcorelib")
set(UFSRV_CONFIG_HOST_SOURCE_ufnetcorelib "local:${UFSRV_CONFIG_SOURCE_DIR}")

# Invariant sources + shim come from the same ufsrvcorelib checkout the composer
# reads — one source, not two (G16).
set(UFSRV_CONFIG_HOST_CANONICAL_SUB "${UFSRV_CONFIG_SOURCE_DIR}/../ufsrvcorelib")

# Deep-tree destinations.  Public headers land under include/<lib>/ — the
# tracked deep tree already lives at include/ufsrvcorelib/ufsrv_core/ufsrv_config
# (it carries ufsrvcorelib's name; include/ufnetcorelib/ufsrv_core/ufsrv_config
# is an empty .gitkeep anchor).  See design §5.3 A1.
set(UFSRV_CONFIG_HOST_PRIVATE_ROOT "${UFSRV_CONFIG_SOURCE_DIR}/src/ufsrv_core/ufsrv_config")
set(UFSRV_CONFIG_HOST_PUBLIC_ROOT  "${UFSRV_CONFIG_SOURCE_DIR}/include/ufsrvcorelib/ufsrv_core/ufsrv_config")

# ufnetcorelib ships no host symbol inventory (G14) — omit the argument.
set(UFSRV_CONFIG_HOST_SYMBOLS_HEADER "")
