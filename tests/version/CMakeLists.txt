#
# tests/version/CMakeLists.txt — dynamic version accessor tests
#
# Compiles the generated version.c (produced by the `ufnetcorelib_version`
# custom target into the top-level build dir) and exercises the
# UfNetCoreLibVersion*() accessors. version.c is generated, so it is marked
# GENERATED and the test is made to depend on the generator target.

set(_generated_version_c ${CMAKE_BINARY_DIR}/version.c)
set_source_files_properties(${_generated_version_c} PROPERTIES GENERATED TRUE)

add_c_test(
    NAME version_tests
    TEST_SOURCE version_tests.cpp
    C_SOURCES ${_generated_version_c}
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include/ufnetcorelib      # <version.h>
        ${CMAKE_SOURCE_DIR}/include
    LIBRARIES
)

# version.c only exists once the generator target has run.
add_dependencies(version_tests ufnetcorelib_version)
