add_c_test(
    NAME session_io_work_tests
    TEST_SOURCE session_io_work_tests.cpp
    C_SOURCES session_io_work_fixture.c ${CMAKE_SOURCE_DIR}/src/ufnet_core/session_io_work.c
    INCLUDE_DIRS ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src/ufnet_core ${CMAKE_SOURCE_DIR}/include/ufnetcorelib
    LIBRARIES pthread
)
target_compile_options(session_io_work_tests PRIVATE -UNDEBUG -fno-builtin-malloc)
target_link_options(session_io_work_tests PRIVATE -Wl,--wrap=malloc)

set(_publication_source "${CMAKE_SOURCE_DIR}/src/ufnet_core/worker_session_io_thread.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_publication_source}")
file(READ "${_publication_source}" _publication_worker)
set(_publication_functions "")
foreach(_publication_function sRestoreReadableIoWork sAbandonHeld sFinishHeld)
    string(FIND "${_publication_worker}" "static void\n${_publication_function}(" _publication_start)
    if(_publication_start LESS 0)
        message(FATAL_ERROR "Missing production publication function: ${_publication_function}")
    endif()
    string(SUBSTRING "${_publication_worker}" ${_publication_start} -1 _publication_tail)
    string(FIND "${_publication_tail}" "\nstatic " _publication_end)
    if(_publication_end LESS 0)
        message(FATAL_ERROR "Missing publication function boundary: ${_publication_function}")
    endif()
    string(SUBSTRING "${_publication_tail}" 0 ${_publication_end} _publication_body)
    string(APPEND _publication_functions "${_publication_body}\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/session_io_publication_under_test.inc" "${_publication_functions}")

add_c_test(
    NAME session_io_publication_tests
    TEST_SOURCE session_io_publication_tests.cpp
    C_SOURCES session_io_publication_fixture.c ${CMAKE_SOURCE_DIR}/src/ufnet_core/session_io_work.c
    INCLUDE_DIRS ${CMAKE_CURRENT_BINARY_DIR} $<TARGET_PROPERTY:ufnetcorelib,INCLUDE_DIRECTORIES>
    LIBRARIES pthread
)
target_compile_definitions(session_io_publication_tests PRIVATE HAVE_CONFIG_UFNETCORELIB_H HAVE_CONFIG_UFSRVCORELIB_H _GNU_SOURCE)
target_compile_options(session_io_publication_tests PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
target_link_options(session_io_publication_tests PRIVATE -Wl,--gc-sections -Wl,--wrap=pthread_cond_signal)
set_tests_properties(session_io_publication_tests PROPERTIES TIMEOUT 10)

add_c_test(
    NAME protocol_io_queue_tests
    TEST_SOURCE protocol_io_queue_tests.cpp
    C_SOURCES protocol_io_queue_fixture.c ${CMAKE_SOURCE_DIR}/src/ufnet_core/protocol/protocol_io.c
    INCLUDE_DIRS $<TARGET_PROPERTY:ufnetcorelib,INCLUDE_DIRECTORIES>
    LIBRARIES uflib::uflib OpenSSL::SSL OpenSSL::Crypto
)
target_compile_definitions(protocol_io_queue_tests PRIVATE HAVE_CONFIG_UFNETCORELIB_H HAVE_CONFIG_UFSRVCORELIB_H _GNU_SOURCE)
target_compile_options(protocol_io_queue_tests PRIVATE -ffunction-sections -fdata-sections -UNDEBUG)
target_link_options(protocol_io_queue_tests PRIVATE -Wl,--gc-sections)
set_tests_properties(protocol_io_queue_tests PROPERTIES TIMEOUT 5)
