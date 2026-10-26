# =============================================================================
#  S2SDK.cmake
#
#  Defines the imported targets used to compile against the Source 2 (CS2)
#  branch of s2sdk (formerly hl2sdk; the checkout folder, the CMake variables
#  and the target names all follow the upstream rename):
#
#    s2sdk::headers   INTERFACE  - include paths + platform/compiler defines
#    s2sdk::tier1     STATIC     - the handful of tier1 .cpp files that are
#                                 shipped as source rather than as a library
#    s2sdk::s2sdk     INTERFACE  - headers + tier1 + the prebuilt libraries
#
#  All flags mirror what alliedmodders' own build does, as encoded in
#  hl2sdk-manifests/manifests/cs2.json and SdkHelpers.ambuild. Keep them in
#  sync when bumping the SDK: a mismatched COMPILER_* or POSIX define does not
#  fail loudly, it silently changes struct layout and calling conventions.
# =============================================================================

if(TARGET s2sdk::s2sdk)
  return()
endif()

set(S2SDK_ROOT "${CS2SDKTEST_S2SDK_DIR}" CACHE PATH "Root of the s2sdk checkout")

if(NOT EXISTS "${S2SDK_ROOT}/public/interfaces/interfaces.h")
  message(FATAL_ERROR
    "s2sdk not found at '${S2SDK_ROOT}'.\n"
    "Run: git submodule update --init --recursive")
endif()

# -----------------------------------------------------------------------------
#  Engine identity
#
#  SOURCE_ENGINE / SE_CS2 come from hl2sdk-manifests/manifests/cs2.json
#  ("code": 25). Metamod headers switch on these, so the value must match the
#  upstream manifest exactly.
# -----------------------------------------------------------------------------
set(S2SDK_ENGINE_NAME "cs2")
set(S2SDK_ENGINE_CODE 25)

# -----------------------------------------------------------------------------
#  s2sdk::headers
# -----------------------------------------------------------------------------
add_library(s2sdk_headers INTERFACE)
add_library(s2sdk::headers ALIAS s2sdk_headers)

# Mirrors "include_paths" in manifests/cs2.json. The SDK's Oct 2026 reorg
# moved the engine headers to public/engine2 and folded tier1 into tier0
# (Valve's lowercase names); public/engine and public/tier1 are gone.
set(_s2sdk_includes
  "${S2SDK_ROOT}"
  "${S2SDK_ROOT}/public"
  "${S2SDK_ROOT}/public/engine2"
  "${S2SDK_ROOT}/public/mathlib"
  "${S2SDK_ROOT}/public/tier0"
  "${S2SDK_ROOT}/public/entity2"
  "${S2SDK_ROOT}/game/shared"
  "${S2SDK_ROOT}/common"
  "${S2SDK_ROOT}/thirdparty/protobuf-3.21.8/src"
)
foreach(_dir IN LISTS _s2sdk_includes)
  if(EXISTS "${_dir}")
    target_include_directories(s2sdk_headers SYSTEM INTERFACE "${_dir}")
  endif()
endforeach()

target_compile_definitions(s2sdk_headers INTERFACE
  SOURCE_ENGINE=${S2SDK_ENGINE_CODE}
  SE_CS2=${S2SDK_ENGINE_CODE}
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

  target_compile_definitions(s2sdk_headers INTERFACE
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
  target_compile_options(s2sdk_headers INTERFACE
    /permissive-      # the SDK relies on some MSVC-isms but compiles clean here
    /Zc:__cplusplus
    /wd4005           # macro redefinition (SDK redefines several CRT names)
    /wd4996           # deprecated CRT calls inside the SDK
    /wd4267 /wd4244   # size_t/int narrowing, pervasive in the SDK
    /wd4355           # 'this' used in base member initializer list
  )
  target_link_libraries(s2sdk_headers INTERFACE legacy_stdio_definitions.lib)
  # Valve ships the .lib files without their .pdb; LNK4099 on every one of them
  # is noise, not a finding.
  target_link_options(s2sdk_headers INTERFACE /ignore:4099)
else()
  # POSIX must be defined or public/tier0/platform.h hits its #error branch.
  target_compile_definitions(s2sdk_headers INTERFACE
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
  target_compile_options(s2sdk_headers INTERFACE
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
#  s2sdk::tier1
#
#  There is no prebuilt tier1 on this branch: the headers are template-only
#  code and the few real translation units live in s2sdk/tier0_perproject/
#  (moved there from tier1/ in the Oct 2026 reorg). convar.cpp is mandatory
#  for anything that registers a ConVar/ConCommand.
# -----------------------------------------------------------------------------
set(_s2sdk_tier1_sources "${S2SDK_ROOT}/tier0_perproject/convar.cpp")

if(CS2SDKTEST_TIER1_KEYVALUES3)
  list(APPEND _s2sdk_tier1_sources "${S2SDK_ROOT}/tier0_perproject/keyvalues3.cpp")
endif()
if(CS2SDKTEST_TIER1_BITBUF)
  list(APPEND _s2sdk_tier1_sources "${S2SDK_ROOT}/tier0_perproject/bitbuf.cpp")
endif()

add_library(s2sdk_tier1 STATIC ${_s2sdk_tier1_sources})
add_library(s2sdk::tier1 ALIAS s2sdk_tier1)

# Foreign sources, compiled but not maintained here; silenced the same way
# CMakeLists.txt silences the entity2 .cpp files and the protoc output. On
# Linux bitbuf.cpp's `g_ExtraMasks[32] = ~0ul` warns - unsigned long is 64-bit
# there and the truncation to uint32 is exactly the all-ones mask it wants.
if(NOT MSVC)
  set_source_files_properties(${_s2sdk_tier1_sources} PROPERTIES
    COMPILE_OPTIONS "-w"
  )
endif()

set_target_properties(s2sdk_tier1 PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  # AMBuildScript in s2sdk builds everything as C++20 now; keep the tier1
  # translation units this plugin compiles under the same standard.
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  FOLDER "third_party"
)
target_link_libraries(s2sdk_tier1 PUBLIC s2sdk::headers)

# -----------------------------------------------------------------------------
#  Prebuilt libraries
#
#  Mirrors the "libs" / "postlink_libs" / "dynamic_libs" entries of cs2.json.
#  tier0 and interfaces are non-negotiable: without interfaces the g_p* globals
#  (g_pCVar, g_pSchemaSystem, ...) are undefined symbols, and without tier0 the
#  CUtl* container templates have no allocator.
# -----------------------------------------------------------------------------
add_library(s2sdk_libs INTERFACE)
add_library(s2sdk::libs ALIAS s2sdk_libs)

function(_s2sdk_require_lib out_var relative_path)
  set(_full "${S2SDK_ROOT}/${relative_path}")
  if(NOT EXISTS "${_full}")
    message(FATAL_ERROR "s2sdk library missing: ${_full}")
  endif()
  set(${out_var} "${_full}" PARENT_SCOPE)
endfunction()

if(WIN32)
  _s2sdk_require_lib(_lib_tier0      "lib/public/win64/tier0.lib")
  _s2sdk_require_lib(_lib_interfaces "lib/public/win64/interfaces.lib")
  _s2sdk_require_lib(_lib_mathlib    "lib/public/win64/mathlib.lib")

  target_link_libraries(s2sdk_libs INTERFACE
    "${_lib_tier0}"
    "${_lib_interfaces}"
    "${_lib_mathlib}"
  )

  if(CS2SDKTEST_WITH_PROTOBUF)
    _s2sdk_require_lib(_lib_protobuf "lib/public/win64/2015/libprotobuf.lib")
    target_link_libraries(s2sdk_libs INTERFACE "${_lib_protobuf}")
  endif()

  set(S2SDK_RUNTIME_DEPENDENCIES "" CACHE INTERNAL "")
else()
  _s2sdk_require_lib(_lib_tier0      "lib/linux64/libtier0.so")
  _s2sdk_require_lib(_lib_interfaces "lib/linux64/interfaces.a")
  _s2sdk_require_lib(_lib_mathlib    "lib/linux64/mathlib.a")

  # The static archives must follow our own objects on the link line, hence
  # the explicit ordering here rather than a bare target_link_libraries set.
  target_link_libraries(s2sdk_libs INTERFACE
    "${_lib_tier0}"
    "${_lib_interfaces}"
    "${_lib_mathlib}"
    m
    dl
  )

  if(CS2SDKTEST_WITH_PROTOBUF)
    _s2sdk_require_lib(_lib_protobuf "lib/linux64/release/libprotobuf.a")
    target_link_libraries(s2sdk_libs INTERFACE "${_lib_protobuf}")
  endif()

  # libtier0.so has to sit next to the plugin binary at runtime; the game ships
  # its own copy, so this is only a build-tree convenience.
  set(S2SDK_RUNTIME_DEPENDENCIES "${_lib_tier0}" CACHE INTERNAL "")
endif()

# -----------------------------------------------------------------------------
#  Umbrella target
# -----------------------------------------------------------------------------
add_library(s2sdk_all INTERFACE)
add_library(s2sdk::s2sdk ALIAS s2sdk_all)
target_link_libraries(s2sdk_all INTERFACE
  s2sdk::headers
  s2sdk::tier1
  s2sdk::libs
)

message(STATUS "s2sdk: ${S2SDK_ROOT} (engine '${S2SDK_ENGINE_NAME}', SOURCE_ENGINE=${S2SDK_ENGINE_CODE})")
