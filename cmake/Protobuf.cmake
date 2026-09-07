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
#  The compiler used is the one shipped inside hl2sdk (devtools/bin), matching
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
    "hl2sdk does not ship a protoc at '${CS2SDKTEST_PROTOC}'.\n"
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
# hl2sdk/common) and returns the generated file list.
function(cs2sdktest_generate_protobuf out_sources_var)
  file(MAKE_DIRECTORY "${CS2SDKTEST_PROTO_OUT_DIR}")

  set(_generated "")
  foreach(_name IN LISTS ARGN)
    set(_input "${CS2SDKTEST_PROTO_DIR}/${_name}.proto")
    if(NOT EXISTS "${_input}")
      message(FATAL_ERROR "no such proto file: ${_input}")
    endif()

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
