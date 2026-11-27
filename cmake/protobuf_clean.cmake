# cmake/protobuf_clean.cmake — empty the protobuf staging area.
#
# Invoked by the protobuf_clean target. Everything under protobuf/generated is
# disposable, except the placeholder that keeps the directory in the tree.

file(GLOB staged "${STAGING}/*")

foreach(item IN LISTS staged)
  get_filename_component(name "${item}" NAME)
  if(NOT name STREQUAL ".gitkeep")
    file(REMOVE_RECURSE "${item}")
  endif()
endforeach()

message(STATUS "protobuf: emptied ${STAGING}")
