# =============================================================================
#  HL2SDK.cmake
#
#  Defines the imported targets used to compile against the Source 2 (CS2)
#  branch of s2sdk (formerly hl2sdk; the checkout folder was renamed with the
#  upstream repository, the target names were not):
#
#    hl2sdk::headers   INTERFACE  - include paths + platform/compiler defines
#    hl2sdk::tier1     STATIC     - the handful of tier1 .cpp files that are
#                                   shipped as source rather than as a library
#    hl2sdk::hl2sdk    INTERFACE  - headers + tier1 + the prebuilt libraries
#
#  All flags mirror what alliedmodders' own build does, as encoded in
#  hl2sdk-manifests/manifests/cs2.json and SdkHelpers.ambuild. Keep them in
#  sync when bumping the SDK: a mismatched COMPILER_* or POSIX define does not
#  fail loudly, it silently changes struct layout and calling conventions.
# =============================================================================

if(TARGET hl2sdk::hl2sdk)
  return()
endif()

set(HL2SDK_ROOT "${CS2SDKTEST_HL2SDK_DIR}" CACHE PATH "Root of the s2sdk checkout")

if(NOT EXISTS "${HL2SDK_ROOT}/public/interfaces/interfaces.h")
  message(FATAL_ERROR
    "s2sdk not found at '${HL2SDK_ROOT}'.\n"
    "Run: git submodule update --init --recursive")
endif()

# -----------------------------------------------------------------------------
#  Engine identity
#
#  SOURCE_ENGINE / SE_CS2 come from hl2sdk-manifests/manifests/cs2.json
#  ("code": 25). Metamod headers switch on these, so the value must match the
#  upstream manifest exactly.
# -----------------------------------------------------------------------------
set(HL2SDK_ENGINE_NAME "cs2")
set(HL2SDK_ENGINE_CODE 25)

# -----------------------------------------------------------------------------
#  hl2sdk::headers
# -----------------------------------------------------------------------------
add_library(hl2sdk_headers INTERFACE)
add_library(hl2sdk::headers ALIAS hl2sdk_headers)

# Mirrors "include_paths" in manifests/cs2.json.
set(_hl2sdk_includes
  "${HL2SDK_ROOT}"
  "${HL2SDK_ROOT}/public"
  "${HL2SDK_ROOT}/public/engine"
  "${HL2SDK_ROOT}/public/mathlib"
  "${HL2SDK_ROOT}/public/tier0"
  "${HL2SDK_ROOT}/public/tier1"
  "${HL2SDK_ROOT}/public/entity2"
  "${HL2SDK_ROOT}/public/game/server"
  "${HL2SDK_ROOT}/game/shared"
  "${HL2SDK_ROOT}/game/server"
  "${HL2SDK_ROOT}/common"
  "${HL2SDK_ROOT}/thirdparty/protobuf-3.21.8/src"
)
foreach(_dir IN LISTS _hl2sdk_includes)
  if(EXISTS "${_dir}")
    target_include_directories(hl2sdk_headers SYSTEM INTERFACE "${_dir}")
  endif()
endforeach()

target_compile_definitions(hl2sdk_headers INTERFACE
  SOURCE_ENGINE=${HL2SDK_ENGINE_CODE}
  SE_CS2=${HL2SDK_ENGINE_CODE}
  GAME_DLL
  RAD_TELEMETRY_DISABLED
  # Every Source 2 target is 64-bit; the SDK has no 32-bit libs on this branch.
  X64BITS
  PLATFORM_64BITS
)

if(MSVC)
  # Every prebuilt lib Valve ships (tier0, interfaces, mathlib, libprotobuf) is
  # built against the *release* static CRT. Anything else produces
  # "RuntimeLibrary mismatch" plus a wall of LNK2005 duplicate symbols out of
  # libcpmt/msvcprt, so this is fixed rather than following the build type:
  # there is no debug-CRT build of those libs to match against. The
  # _ALLOW_*_MISMATCH and _HAS_ITERATOR_DEBUGGING defines below exist for
  # exactly this arrangement.
  # include() runs in the caller's scope, so a plain set() reaches the targets
  # defined in CMakeLists.txt afterwards.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

  target_compile_definitions(hl2sdk_headers INTERFACE
    COMPILER_MSVC
    COMPILER_MSVC64
    WIN32
    WIN64
    _CRT_SECURE_NO_DEPRECATE
    _CRT_NONSTDC_NO_DEPRECATE
    _CRT_SECURE_NO_WARNINGS
    _HAS_ITERATOR_DEBUGGING=0
    _ALLOW_RUNTIME_LIBRARY_MISMATCH
    _ALLOW_ITERATOR_DEBUG_LEVEL_MISMATCH
    _ALLOW_MSC_VER_MISMATCH
  )
  target_compile_options(hl2sdk_headers INTERFACE
    /permissive-      # the SDK relies on some MSVC-isms but compiles clean here
    /Zc:__cplusplus
    /wd4005           # macro redefinition (SDK redefines several CRT names)
    /wd4996           # deprecated CRT calls inside the SDK
    /wd4267 /wd4244   # size_t/int narrowing, pervasive in the SDK
  )
  target_link_libraries(hl2sdk_headers INTERFACE legacy_stdio_definitions.lib)
  # Valve ships the .lib files without their .pdb; LNK4099 on every one of them
  # is noise, not a finding.
  target_link_options(hl2sdk_headers INTERFACE /ignore:4099)
else()
  # POSIX must be defined or public/tier0/platform.h hits its #error branch.
  target_compile_definitions(hl2sdk_headers INTERFACE
    COMPILER_GCC
    POSIX
    _LINUX
    LINUX
    GNUC
    _DLL_EXT=.so
    # The SDK's prebuilt libs are built against the pre-C++11 std::string ABI.
    _GLIBCXX_USE_CXX11_ABI=0
    # CRT name shims the SDK sources expect on POSIX.
    stricmp=strcasecmp
    _stricmp=strcasecmp
    strnicmp=strncasecmp
    _strnicmp=strncasecmp
    strcmpi=strcasecmp
    _vsnprintf=vsnprintf
    _alloca=alloca
    _finite=finite
  )
  target_compile_options(hl2sdk_headers INTERFACE
    -fPIC
    -fno-strict-aliasing
    -march=nocona -mmmx -msse -msse2
    # offsetof() on the SDK's non-standard-layout classes is exactly what this
    # project does for a living, so this warning must be off, not just quiet.
    -Wno-invalid-offsetof
    -Wno-unknown-pragmas
    -Wno-overloaded-virtual
    -Wno-non-virtual-dtor
    -Wno-unused
    -Wno-sign-compare
    -Wno-register
    -Wno-narrowing
    -Wno-delete-non-virtual-dtor
  )
endif()

# -----------------------------------------------------------------------------
#  hl2sdk::tier1
#
#  There is no prebuilt tier1 on this branch: public/tier1/*.h is header-only
#  template code, and the few real translation units live in s2sdk/tier1/.
#  convar.cpp is mandatory for anything that registers a ConVar/ConCommand.
# -----------------------------------------------------------------------------
set(_hl2sdk_tier1_sources "${HL2SDK_ROOT}/tier1/convar.cpp")

if(CS2SDKTEST_TIER1_KEYVALUES3)
  list(APPEND _hl2sdk_tier1_sources "${HL2SDK_ROOT}/tier1/keyvalues3.cpp")
endif()
if(CS2SDKTEST_TIER1_BITBUF)
  list(APPEND _hl2sdk_tier1_sources "${HL2SDK_ROOT}/tier1/bitbuf.cpp")
endif()

add_library(hl2sdk_tier1 STATIC ${_hl2sdk_tier1_sources})
add_library(hl2sdk::tier1 ALIAS hl2sdk_tier1)

# Foreign sources, compiled but not maintained here; silenced the same way
# CMakeLists.txt silences the entity2 .cpp files and the protoc output. On
# Linux bitbuf.cpp's `g_ExtraMasks[32] = ~0ul` warns - unsigned long is 64-bit
# there and the truncation to uint32 is exactly the all-ones mask it wants.
if(NOT MSVC)
  set_source_files_properties(${_hl2sdk_tier1_sources} PROPERTIES
    COMPILE_OPTIONS "-w"
  )
endif()

set_target_properties(hl2sdk_tier1 PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  # AMBuildScript in s2sdk builds everything as C++20 now; keep the tier1
  # translation units this plugin compiles under the same standard.
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  FOLDER "third_party"
)
target_link_libraries(hl2sdk_tier1 PUBLIC hl2sdk::headers)

# -----------------------------------------------------------------------------
#  Prebuilt libraries
#
#  Mirrors the "libs" / "postlink_libs" / "dynamic_libs" entries of cs2.json.
#  tier0 and interfaces are non-negotiable: without interfaces the g_p* globals
#  (g_pCVar, g_pSchemaSystem, ...) are undefined symbols, and without tier0 the
#  CUtl* container templates have no allocator.
# -----------------------------------------------------------------------------
add_library(hl2sdk_libs INTERFACE)
add_library(hl2sdk::libs ALIAS hl2sdk_libs)

function(_hl2sdk_require_lib out_var relative_path)
  set(_full "${HL2SDK_ROOT}/${relative_path}")
  if(NOT EXISTS "${_full}")
    message(FATAL_ERROR "s2sdk library missing: ${_full}")
  endif()
  set(${out_var} "${_full}" PARENT_SCOPE)
endfunction()

if(WIN32)
  _hl2sdk_require_lib(_lib_tier0      "lib/public/win64/tier0.lib")
  _hl2sdk_require_lib(_lib_interfaces "lib/public/win64/interfaces.lib")
  _hl2sdk_require_lib(_lib_mathlib    "lib/public/win64/mathlib.lib")

  target_link_libraries(hl2sdk_libs INTERFACE
    "${_lib_tier0}"
    "${_lib_interfaces}"
    "${_lib_mathlib}"
  )

  if(CS2SDKTEST_WITH_PROTOBUF)
    _hl2sdk_require_lib(_lib_protobuf "lib/public/win64/2015/libprotobuf.lib")
    target_link_libraries(hl2sdk_libs INTERFACE "${_lib_protobuf}")
  endif()

  set(HL2SDK_RUNTIME_DEPENDENCIES "" CACHE INTERNAL "")
else()
  _hl2sdk_require_lib(_lib_tier0      "lib/linux64/libtier0.so")
  _hl2sdk_require_lib(_lib_interfaces "lib/linux64/interfaces.a")
  _hl2sdk_require_lib(_lib_mathlib    "lib/linux64/mathlib.a")

  # The static archives must follow our own objects on the link line, hence
  # the explicit ordering here rather than a bare target_link_libraries set.
  target_link_libraries(hl2sdk_libs INTERFACE
    "${_lib_tier0}"
    "${_lib_interfaces}"
    "${_lib_mathlib}"
    m
    dl
  )

  if(CS2SDKTEST_WITH_PROTOBUF)
    _hl2sdk_require_lib(_lib_protobuf "lib/linux64/release/libprotobuf.a")
    target_link_libraries(hl2sdk_libs INTERFACE "${_lib_protobuf}")
  endif()

  # libtier0.so has to sit next to the plugin binary at runtime; the game ships
  # its own copy, so this is only a build-tree convenience.
  set(HL2SDK_RUNTIME_DEPENDENCIES "${_lib_tier0}" CACHE INTERNAL "")
endif()

# -----------------------------------------------------------------------------
#  Umbrella target
# -----------------------------------------------------------------------------
add_library(hl2sdk_all INTERFACE)
add_library(hl2sdk::hl2sdk ALIAS hl2sdk_all)
target_link_libraries(hl2sdk_all INTERFACE
  hl2sdk::headers
  hl2sdk::tier1
  hl2sdk::libs
)

message(STATUS "s2sdk: ${HL2SDK_ROOT} (engine '${HL2SDK_ENGINE_NAME}', SOURCE_ENGINE=${HL2SDK_ENGINE_CODE})")
