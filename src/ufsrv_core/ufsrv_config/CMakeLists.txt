# src/ufsrv_core/CMakeLists.txt
# Contributes the ufsrv_core sources to the ufnetcorelib OBJECT target.
#
# Explicit list (no globbing) — see src/ufnet_core/CMakeLists.txt for rationale.

target_sources(${_PROJECT_NAME}
    PRIVATE
        ufsrv_config_stubs_as_string.c
)
