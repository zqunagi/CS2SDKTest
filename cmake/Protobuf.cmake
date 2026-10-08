# =============================================================================
#  Protobuf.cmake
#
#  Generates the .pb.h/.pb.cc files the SDK headers expect.
#
#  This is not optional. public/eiface.h includes "network_connection.pb.h"
#  unconditionally, and eiface.h is on the include path of ISmmAPI.h - so every
#  translation unit that includes ISmmPlugin.h needs it. Any Source 2 plugin
#  runs protoc, whether or not it ever touches a network message itself.
#
#  The compiler used is the one shipped inside s2sdk (devtools/bin), matching
#  the manifest's protoc_path. A system protoc is deliberately not accepted:
#  generated code and the prebuilt libprotobuf.a must come from the same
#  protobuf release, and a mismatch shows up as link errors that name mangled
#  internal symbols rather than the actual problem.
# =============================================================================

if(DEFINED CS2SDKTEST_PROTOC)
  return()
endif()

set(CS2SDKTEST_PROTOBUF_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}")

if(WIN32)
  set(_protoc_relative "devtools/bin/protoc.exe")
else()
  set(_protoc_relative "devtools/bin/linux/protoc")
endif()

set(CS2SDKTEST_PROTOC "${HL2SDK_ROOT}/${_protoc_relative}")
if(NOT EXISTS "${CS2SDKTEST_PROTOC}")
  message(FATAL_ERROR
    "s2sdk does not ship a protoc at '${CS2SDKTEST_PROTOC}'.\n"
    "The SDK headers require generated protobuf sources, so this is required.")
endif()

# Fail here with a clear message rather than at build time with "Permission
# denied": a checkout on a filesystem that drops the executable bit is a common
# way to end up with a protoc that exists but cannot run.
execute_process(
  COMMAND "${CS2SDKTEST_PROTOC}" --version
  RESULT_VARIABLE _protoc_result
  OUTPUT_VARIABLE _protoc_version
  ERROR_VARIABLE _protoc_stderr
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _protoc_result EQUAL 0)
  message(FATAL_ERROR
    "Could not run '${CS2SDKTEST_PROTOC}':\n  ${_protoc_stderr}\n"
    "If this is a permissions problem, run: chmod +x ${CS2SDKTEST_PROTOC}")
endif()

message(STATUS "protoc: ${CS2SDKTEST_PROTOC} (${_protoc_version})")

set(CS2SDKTEST_PROTO_DIR "${HL2SDK_ROOT}/common")
set(CS2SDKTEST_PROTO_OUT_DIR "${CMAKE_BINARY_DIR}/generated/protobuf")

# Generates C++ sources for each named .proto (without extension, relative to
# s2sdk/common) and returns the generated file list.
#
# A generated header #includes the .pb.h of every .proto it imports, so the
# imports have to be generated too - protoc does not emit them on its own
# (it only reads them). The closure below collects transitive same-directory
# imports; imports that resolve inside the bundled protobuf tree (the
# well-known types, e.g. google/protobuf/descriptor.proto) are satisfied by
# that tree's headers and are skipped.
function(cs2sdktest_generate_protobuf out_sources_var)
  file(MAKE_DIRECTORY "${CS2SDKTEST_PROTO_OUT_DIR}")

  # Expands one entry of CS2SDKTEST_PROTO_FILES into itself plus the closure
  # of its s2sdk/common imports, appended in dependency order (imports first,
  # so a regenerating build never compiles against a stale imported .pb.h).
  function(_protobuf_closure name out_var)
    set(_result "")
    set(_pending "${name}")
    set(_seen "")
    while(_pending)
      list(POP_FRONT _pending _current)
      if(_current IN_LIST _seen)
        continue()
      endif()
      list(APPEND _seen "${_current}")

      set(_input "${CS2SDKTEST_PROTO_DIR}/${_current}.proto")
      if(NOT EXISTS "${_input}")
        message(FATAL_ERROR "no such proto file: ${_input}")
      endif()

      file(STRINGS "${_input}" _import_lines REGEX "^[ \t]*import")
      foreach(_line IN LISTS _import_lines)
        string(REGEX REPLACE "^[ \t]*import[ \t]+\"([^\"]+)\".*" "\\1" _path "${_line}")
        # Only same-directory imports need generating; anything under a
        # subdirectory (google/...) resolves against the bundled protobuf tree.
        if(NOT _path MATCHES "/")
          # .pb.h include names are extension-less; match that convention.
          string(REGEX REPLACE "\\.proto$" "" _imported "${_path}")
          list(PREPEND _pending "${_imported}")
        endif()
      endforeach()

      list(APPEND _result "${_current}")
    endwhile()
    set(${out_var} "${_result}" PARENT_SCOPE)
  endfunction()

  set(_names "")
  foreach(_name IN LISTS ARGN)
    _protobuf_closure("${_name}" _closed)
    foreach(_entry IN LISTS _closed)
      if(NOT _entry IN_LIST _names)
        list(APPEND _names "${_entry}")
      endif()
    endforeach()
  endforeach()

  set(_generated "")
  foreach(_name IN LISTS _names)
    set(_input "${CS2SDKTEST_PROTO_DIR}/${_name}.proto")

    set(_header "${CS2SDKTEST_PROTO_OUT_DIR}/${_name}.pb.h")
    set(_source "${CS2SDKTEST_PROTO_OUT_DIR}/${_name}.pb.cc")

    add_custom_command(
      OUTPUT "${_header}" "${_source}"
      COMMAND "${CS2SDKTEST_PROTOC}"
              "--proto_path=${CS2SDKTEST_PROTO_DIR}"
              # descriptor.proto and the other well-known types are imported by
              # the SDK's protos and live in the bundled protobuf tree.
              "--proto_path=${HL2SDK_ROOT}/thirdparty/protobuf-3.21.8/src"
              "--cpp_out=${CS2SDKTEST_PROTO_OUT_DIR}"
              "${_input}"
      DEPENDS "${_input}" "${CS2SDKTEST_PROTOC}"
      COMMENT "protoc ${_name}.proto"
      VERBATIM
    )

    list(APPEND _generated "${_source}" "${_header}")
  endforeach()

  set(${out_sources_var} ${_generated} PARENT_SCOPE)
endfunction()
