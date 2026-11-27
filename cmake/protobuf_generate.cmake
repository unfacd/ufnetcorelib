# cmake/protobuf_generate.cmake — compile one schema family into the staging area and
# copy the pair to its committed destination.
#
# Invoked by the protobuf_uf / protobuf_ufsrv targets (cmake/protobuf_schemas.cmake),
# never included: the caller passes everything through -D.
#
#   PROTOC         protoc executable
#   GENERATOR      protoc-gen-c plugin built from packages/protobuf-c
#   PROTO_DIR      directory holding the .proto inputs
#   STAGING        protobuf/generated, ignored by git
#   HEADER_DIR     destination of the generated headers
#   SOURCE_DIR     destination of the generated sources
#   INCLUDE_PREFIX include prefix the generated source uses for its own header
#   IMPORT_PREFIX  include prefix a generated header uses for an imported schema
#   NAMES          schema names to generate, comma-separated, without .proto
#
# protoc-c writes includes that do not hold in this tree, so they are rewritten before
# the copy: the source names its header bare, a header names an imported schema bare,
# and every header includes the system <protobuf-c/protobuf-c.h> where the build uses
# uflib's copy.

file(MAKE_DIRECTORY "${STAGING}")
string(REPLACE "," ";" NAMES "${NAMES}")

foreach(name IN LISTS NAMES)
  set(source "${STAGING}/${name}.pb-c.c")
  set(header "${STAGING}/${name}.pb-c.h")

  execute_process(
    COMMAND "${PROTOC}" "--plugin=protoc-gen-c=${GENERATOR}" "--c_out=${STAGING}"
            "-I${PROTO_DIR}" "${PROTO_DIR}/${name}.proto"
    RESULT_VARIABLE result)

  if(NOT result EQUAL 0 OR NOT EXISTS "${source}" OR NOT EXISTS "${header}")
    message(FATAL_ERROR "protobuf: generating ${name}.proto failed (${result})")
  endif()

  file(READ "${header}" text)
  string(REPLACE "#include <protobuf-c/protobuf-c.h>"
                 "#include <uflib/protobuf-c/protobuf-c.h>" text "${text}")
  string(REGEX REPLACE "#include \"([A-Za-z0-9_]+)\\.pb-c\\.h\""
                       "#include <${IMPORT_PREFIX}/\\1.pb-c.h>" text "${text}")
  file(WRITE "${header}" "${text}")

  file(READ "${source}" text)
  string(REPLACE "#include \"${name}.pb-c.h\""
                 "#include \"${INCLUDE_PREFIX}/${name}.pb-c.h\"" text "${text}")
  file(WRITE "${source}" "${text}")

  file(COPY "${header}" DESTINATION "${HEADER_DIR}")
  file(COPY "${source}" DESTINATION "${SOURCE_DIR}")

  message(STATUS "protobuf: ${name}.pb-c.{c,h} -> ${HEADER_DIR}, ${SOURCE_DIR}")
endforeach()
