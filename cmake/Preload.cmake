# =============================================================================
#  Preload.cmake
#
#  The Metamod-free build: one shared object loaded with LD_PRELOAD. Included
#  from CMakeLists.txt after the Metamod target is defined; Linux only, and
#  only when CS2SDKTEST_BUILD_PRELOAD is ON.
#
#  Phase 1 compiled the shared kit plus a single suite; phase 2 adds suites in
#  batches. Nothing here comes from metamod-source. The suite list grows by editing TestRunner::CreateDefault()
#  under CS2SDKTEST_PRELOAD and the source list below, one batch at a time.
# =============================================================================

if(NOT CS2SDKTEST_BUILD_PRELOAD)
  return()
endif()

if(WIN32)
  message(FATAL_ERROR "CS2SDKTEST_BUILD_PRELOAD is Linux only (it is loaded with LD_PRELOAD).")
endif()

set(CS2SDKTEST_PRELOAD_SOURCES
  src/preload/Entry.cpp
  src/plugin/Sweep.cpp

  src/core/Log.cpp
  src/core/Module.cpp
  src/core/FileSystemUtil.cpp
  src/core/TomlData.cpp
  src/core/SignatureScan.cpp

  src/data/GameModules.cpp

  src/testkit/CallGuard.cpp
  src/testkit/CrashRecorder.cpp
  src/testkit/StructInspector.cpp
  src/testkit/TestContext.cpp
  src/testkit/TestRunner.cpp
  src/testkit/ResultSink.cpp
  src/testkit/BaselineLookup.cpp

  src/hook/HookRecorder.cpp

  src/suites/Source2EngineSuite.cpp
  src/suites/SchemaSystemSuite.cpp
  src/suites/ServerConfigSuite.cpp
  src/suites/EngineServiceMgrSuite.cpp
  src/suites/StringTableSuite.cpp
  src/suites/GameEntitiesSuite.cpp
  src/suites/HostStateMgrSuite.cpp
  src/suites/LoopModeSuite.cpp
  src/suites/NetChannelSuite.cpp
  src/suites/ResourceSystemSuite.cpp
  src/suites/LocalizeSuite.cpp
  src/suites/GameTypesSuite.cpp
  src/suites/SerializedEntitiesSuite.cpp
  src/suites/Tier0PlatformSuite.cpp
  src/suites/SchemaDiffSuite.cpp
  src/suites/FileSystemSuite.cpp
  src/suites/KeyValues3Suite.cpp
  src/suites/FlattenedSerializersSuite.cpp
  src/suites/NetworkMessagesSuite.cpp
  src/suites/GameEntitySystemSuite.cpp
  src/suites/GamedataSuite.cpp
  src/suites/NetworkServerSuite.cpp
  src/suites/ConVarSuite.cpp
  src/suites/GameEventSuite.cpp
  src/suites/Source2ServerSuite.cpp
  src/suites/Source2GameClientsSuite.cpp
  src/suites/Tier0ContainerSuite.cpp
)

add_library(cs2sdktest_preload SHARED
  ${CS2SDKTEST_PRELOAD_SOURCES}
  ${CS2SDKTEST_SDK_SOURCES}
  ${CS2SDKTEST_PROTO_SOURCES}
)

# The foreign-code warning flags set for the Metamod target are per source file
# and directory-wide, so they already apply to the same files here.

target_include_directories(cs2sdktest_preload PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CS2SDKTEST_GENERATED_DIR}"
)
if(CS2SDKTEST_WITH_PROTOBUF)
  target_include_directories(cs2sdktest_preload SYSTEM PRIVATE "${CS2SDKTEST_PROTO_OUT_DIR}")
endif()

# GamedataSuite compares the SDK's declared slots against gamedata with
# KHook::GetVtableIndex, which is member-pointer arithmetic in a header. Only the
# header is used: nothing from khook is linked or called at run time, and
# KHook::__exported__khook (the pointer Metamod fills in) is never touched.
set(CS2SDKTEST_KHOOK_INCLUDE "${CS2SDKTEST_METAMOD_DIR}/third_party/khook/include")
if(NOT EXISTS "${CS2SDKTEST_KHOOK_INCLUDE}/khook.hpp")
  message(FATAL_ERROR "khook.hpp not found at '${CS2SDKTEST_KHOOK_INCLUDE}'; "
    "run: git submodule update --init --recursive metamod-source")
endif()
target_include_directories(cs2sdktest_preload SYSTEM PRIVATE "${CS2SDKTEST_KHOOK_INCLUDE}")

target_compile_definitions(cs2sdktest_preload PRIVATE CS2SDKTEST_PRELOAD=1)
if(CS2SDKTEST_TIER1_KEYVALUES3)
  target_compile_definitions(cs2sdktest_preload PRIVATE CS2SDKTEST_WITH_KEYVALUES3=1)
endif()
if(CS2SDKTEST_TIER1_BITBUF)
  target_compile_definitions(cs2sdktest_preload PRIVATE CS2SDKTEST_WITH_BITBUF=1)
endif()

add_dependencies(cs2sdktest_preload cs2sdktest_revision)

# s2sdk::s2sdk, not metamod::metamod: the headers and prebuilt libraries only.
# std::thread needs -pthread on glibc older than 2.34.
find_package(Threads REQUIRED)
target_link_libraries(cs2sdktest_preload PRIVATE s2sdk::s2sdk Threads::Threads)

target_compile_features(cs2sdktest_preload PRIVATE cxx_std_20)
set_target_properties(cs2sdktest_preload PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  PREFIX ""
  OUTPUT_NAME "cs2sdktest_preload"
  SUFFIX ".so"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/preload"
)

target_compile_options(cs2sdktest_preload PRIVATE -Wall -Wextra)
if(CS2SDKTEST_WARNINGS_AS_ERRORS)
  target_compile_options(cs2sdktest_preload PRIVATE -Werror)
endif()

# Symbol hygiene is the point of this block. LD_PRELOAD puts this object ahead
# of every other library in the process, so any symbol it exports wins lookup
# for the engine too. The SDK's interfaces.a defines CreateInterface (and the
# g_p* globals); exported from here, the engine's own libtier0.so would resolve
# to ours. The version script exports nothing, and --exclude-libs hides what
# the static archives would otherwise leak. Nothing in this object needs to be
# found by name: the constructor is called by the loader, not looked up.
set(CS2SDKTEST_PRELOAD_EXPORTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/preload.map")
set_target_properties(cs2sdktest_preload PROPERTIES
  LINK_DEPENDS "${CS2SDKTEST_PRELOAD_EXPORTS}")
target_link_options(cs2sdktest_preload PRIVATE
  -static-libstdc++
  -static-libgcc
  -Wl,--version-script=${CS2SDKTEST_PRELOAD_EXPORTS}
  -Wl,--exclude-libs,ALL
  -Wl,-Bsymbolic
)

# Fails the build when the object exports anything. Cheap, and the only thing
# standing between a mistake here and an engine that fails to start.
add_custom_command(TARGET cs2sdktest_preload POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -DSO=$<TARGET_FILE:cs2sdktest_preload>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPreloadExports.cmake
  COMMENT "Checking that cs2sdktest_preload exports nothing"
  VERBATIM
)

message(STATUS "cs2sdktest_preload: Metamod-free LD_PRELOAD build enabled")
