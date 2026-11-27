# src/ufnet_core/CMakeLists.txt
# Contributes the ufnet_core sources to the ufnetcorelib OBJECT target.
#
# Explicit list (no globbing): adding or removing a file here is a reviewable
# diff and, because the CMakeLists.txt text changes, triggers an automatic
# CLion/CMake reload so the new file lands in the model. Paths are relative to
# this directory (CMP0076).

target_sources(${_PROJECT_NAME}
    PRIVATE
        # top level
        lock_claim/lock_claim.c
        main_utils_ratelimit.c
        ufsrv_delegator_utils.c
        # cache_backend
        # command_console (to be moved into future ufsrvcorelib)
        # email
        email/utils_email.c
        protocol_sandbox/protocol_sandbox.c
        # session
        session/session_utils.c
)
