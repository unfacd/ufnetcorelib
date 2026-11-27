add_c_test(
    NAME console_delegator_channel_tests
    TEST_SOURCE console_delegator_channel_tests.cpp
    C_SOURCES ${CMAKE_SOURCE_DIR}/src/ufnet_msgcore/session_delegator/session_delegator_command.c
              ${CMAKE_SOURCE_DIR}/../uflib/src/buffer_descriptor/buffer_descriptor.c
    INCLUDE_DIRS ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/include/ufnetcorelib ${CMAKE_SOURCE_DIR}/../uflib/include ${CMAKE_SOURCE_DIR}/../uflib/include/uflib ${CMAKE_SOURCE_DIR}/../ufsrvcorelib/include
    LIBRARIES pthread
)
target_compile_definitions(console_delegator_channel_tests PRIVATE _GNU_SOURCE)
set_tests_properties(console_delegator_channel_tests PROPERTIES TIMEOUT 10)
