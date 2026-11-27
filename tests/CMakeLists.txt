#
# tests/CMakeLists.txt — gtest harness for ufnetcorelib
#
# Provides:
#   gtest_shared_main      OBJECT library linked into every test executable
#   add_c_test()           one‑call test registration (executable + CTest)
#

# ── Shared gtest main ─────────────────────────────────────────────────────

add_library(gtest_shared_main OBJECT main.cpp)
target_link_libraries(gtest_shared_main PUBLIC GTest::gtest GTest::gtest_main)
ufsrv_apply_build_options(gtest_shared_main)

# ── add_c_test() macro ────────────────────────────────────────────────────

function(add_c_test)
    set(options "")
    set(oneValueArgs NAME TEST_SOURCE)
    set(multiValueArgs C_SOURCES INCLUDE_DIRS LIBRARIES)
    cmake_parse_arguments(TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${TEST_NAME}
        ${TEST_TEST_SOURCE}
        $<TARGET_OBJECTS:gtest_shared_main>
        ${TEST_C_SOURCES}
    )
    set_target_properties(${TEST_NAME} PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(${TEST_NAME} PRIVATE
        GTest::gtest GTest::gtest_main stdc++ m
        ${TEST_LIBRARIES}
    )
    ufsrv_apply_build_options(${TEST_NAME})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

# ── Test subdirectories ───────────────────────────────────────────────────

set(UFNETCORELIB_TEST_MODULES
    "version;signals;http;session_io;command_console;ufnet_msgcore/user"
    CACHE STRING "Semicolon-separated test module directories")
foreach(module IN LISTS UFNETCORELIB_TEST_MODULES)
    add_subdirectory(${module})
endforeach()
