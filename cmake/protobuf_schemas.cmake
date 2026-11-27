# cmake/protobuf_schemas.cmake — the maintainer targets that produce the committed
# .pb-c pairs from protobuf/*.proto.
#
# ufnetcorelib owns the schemas, and the generated pairs are committed: the ordinary
# build compiles them as they are. These targets regenerate them on demand:
#
#   cmake --build <dir> --target protobuf_uf      UnfacdService, WebSocketMessage
#   cmake --build <dir> --target protobuf_ufsrv   UfsrvMessageQueue
#   cmake --build <dir> --target protobuf_clean   empty protobuf/generated
#
# protobuf_uf and protobuf_ufsrv compile into protobuf/generated, which git ignores,
# then copy the pair to its committed destination after rewriting the includes
# protoc-c cannot know about. They need the vendored generator: configure with
# -DUFNETCORELIB_PROTOBUF_C_GENERATOR=ON (which needs libprotoc-dev).

set(_protobuf_dir     "${CMAKE_CURRENT_SOURCE_DIR}/protobuf")
set(_protobuf_staging "${_protobuf_dir}/generated")

add_custom_target(protobuf_clean
  COMMAND ${CMAKE_COMMAND} -DSTAGING=${_protobuf_staging}
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/protobuf_clean.cmake"
  USES_TERMINAL
  COMMENT "Emptying protobuf/generated")

if(NOT UFNETCORELIB_PROTOBUF_C_GENERATOR)
  foreach(target protobuf_uf protobuf_ufsrv)
    add_custom_target(${target}
      COMMAND ${CMAKE_COMMAND} -E echo
              "protobuf: reconfigure with -DUFNETCORELIB_PROTOBUF_C_GENERATOR=ON (needs libprotoc-dev)"
      COMMAND ${CMAKE_COMMAND} -E false
      VERBATIM
      COMMENT "protobuf: generator is not built")
  endforeach()
  return()
endif()

find_package(Protobuf REQUIRED)

# One schema family: generate every name into the staging area and copy the results to
# the directories their consumers include them from.
function(_ufnetcorelib_protobuf_family target header_dir source_dir include_prefix)
  # one -D argument: a semicolon list would be split by the build shell
  string(REPLACE ";" "," _names "${ARGN}")
  add_custom_target(${target}
    COMMAND ${CMAKE_COMMAND}
            -DPROTOC=${Protobuf_PROTOC_EXECUTABLE}
            -DGENERATOR=$<TARGET_FILE:protoc-gen-c>
            -DPROTO_DIR=${_protobuf_dir}
            -DSTAGING=${_protobuf_staging}
            -DHEADER_DIR=${header_dir}
            -DSOURCE_DIR=${source_dir}
            -DINCLUDE_PREFIX=${include_prefix}
            -DIMPORT_PREFIX=ufnet_msgcore
            "-DNAMES=${_names}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/protobuf_generate.cmake"
    VERBATIM
    USES_TERMINAL
    COMMENT "protobuf: ${target}")
  add_dependencies(${target} protoc-gen-c)
endfunction()

_ufnetcorelib_protobuf_family(protobuf_uf
  "${CMAKE_CURRENT_SOURCE_DIR}/include/ufnetcorelib/ufnet_msgcore"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_msgcore"
  "ufnet_msgcore"
  UnfacdService WebSocketMessage)

_ufnetcorelib_protobuf_family(protobuf_ufsrv
  "${CMAKE_CURRENT_SOURCE_DIR}/include/ufnetcorelib/ufnet_msgcore/msgqueue_backend"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/ufnet_msgcore/msgqueue_backend"
  "ufnet_msgcore/msgqueue_backend"
  UfsrvMessageQueue)
