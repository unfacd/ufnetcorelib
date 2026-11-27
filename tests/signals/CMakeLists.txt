#
# tests/signals/CMakeLists.txt — signal-handling design tests (Stage 1)
#
# Compiles the real misc.c (InitSignals / UfsrvSignalControlSet) into the test
# executable and stubs the unrelated deprecated Lua helpers (see signals_stubs.c)
# so we avoid transitively linking lua / uflib / ufsrvcorelib.
#

add_c_test(
    NAME signals_tests
    TEST_SOURCE signals_tests.cpp
    C_SOURCES
        ${CMAKE_SOURCE_DIR}/src/ufnet_msgcore/misc.c
        signals_stubs.c
    INCLUDE_DIRS
        ${CMAKE_BINARY_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/ufnetcorelib
        ${CMAKE_SOURCE_DIR}/packages
        ${CMAKE_SOURCE_DIR}/packages/ufsrvcorelib/include
    LIBRARIES
        uflib::uflib
)
