#
# tests/http/CMakeLists.txt — HTTP callback dispatch typing tests
#
# Guards the two indirect-call sites UBSan's function checker reported:
# onion_handler_handle() calling handler->handler, and the dictionary walk
# calling its visitor.  Both callbacks are now registered without casts, so a
# mismatched callback fails to compile; a subtle one still trips
# -fsanitize=function at runtime when these tests run.
#
# The HTTP TUs under test are compiled directly rather than linked out of
# libufnetcorelib.a: the archive also carries the session/worker code, whose
# config accessors are supplied by the consumer application.  The response
# write path is stubbed — these tests never reach it.
#
# The calls themselves are made from a C harness: the session headers use
# Blocks, which the C++ gtest translation unit cannot see.
#

if(TARGET ufsrvcorelib::ufsrvcorelib_static)
    set(_http_primitive_libraries ufsrvcorelib::ufsrvcorelib_static)
else()
    set(_http_primitive_sources
        ${_UFSRVCORELIB_CFG_ROOT}/src/ufsrv_core/http/dictionary.c
        ${_UFSRVCORELIB_CFG_ROOT}/src/ufsrv_core/http/vector.c
        ${_UFSRVCORELIB_CFG_ROOT}/src/ufsrv_core/http/h_codecs.c
    )
endif()

add_c_test(
    NAME http_callback_dispatch_tests
    TEST_SOURCE http_callback_dispatch_tests.cpp
    C_SOURCES
        ${_http_primitive_sources}
        ${_UFSRVCORELIB_CFG_ROOT}/src/ufsrv_core/http/h_handler.c
        http_callback_harness.c
        http_callback_stubs.c
    INCLUDE_DIRS
        ${CMAKE_BINARY_DIR}                              # config_ufnetcorelib.h
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/include/ufnetcorelib
        ${CMAKE_SOURCE_DIR}/packages                    # <hiredis/hiredis.h>
        $<TARGET_PROPERTY:ufnetcorelib,INCLUDE_DIRECTORIES>
    LIBRARIES
        uflib::uflib
        PkgConfig::UTF8PROC
        ${_http_primitive_libraries}
)
