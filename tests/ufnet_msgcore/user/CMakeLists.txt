add_c_test(
    NAME user_redis_share_list_tests
    TEST_SOURCE user_redis_share_list_tests.cpp
    C_SOURCES ${CMAKE_SOURCE_DIR}/src/ufnet_msgcore/user/user_redis_backend.c
    INCLUDE_DIRS $<TARGET_PROPERTY:ufnetcorelib,INCLUDE_DIRECTORIES>
)
set_tests_properties(user_redis_share_list_tests PROPERTIES TIMEOUT 10)
