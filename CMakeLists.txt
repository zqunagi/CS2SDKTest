cmake_minimum_required(VERSION 3.21)

# =============================================================================
#  CS2SDKTest - an exercise harness for s2sdk (cs2 branch)
#
#  One shared library, injected into the dedicated server: with LD_PRELOAD on
#  Linux, and on Windows by cst_run.exe, which uses Microsoft Detours to start
#  cs2.exe with the DLL in its import table. Its job
#  is to use as much of the SDK as possible: call the engine interfaces and log
#  what actually happened. After a game update, the log is what tells you which
#  parts of s2sdk no longer match the game.
# =============================================================================

project(cs2sdktest
  VERSION 0.1.0
  DESCRIPTION "s2sdk surface diagnostics for Counter-Strike 2"
  LANGUAGES CXX
)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT WIN32)
  message(FATAL_ERROR "cs2sdktest builds for Linux (LD_PRELOAD) and Windows (Detours) only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Source 2 is 64-bit only; configure a x64 toolchain.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# -----------------------------------------------------------------------------
#  Options
# -----------------------------------------------------------------------------
set(CS2SDKTEST_S2SDK_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/s2sdk"
    CACHE PATH "Path to the s2sdk checkout (cs2 branch)")
# Off by default: this library reads from the engine and allocates only for its
# own containers, so no allocation crosses the module boundary. Turn it on if
# that ever stops being true.
option(CS2SDKTEST_USE_MEMOVERRIDE
       "Route new/delete through tier0's allocator (see comment)" OFF)
# Not really optional: public/eiface.h includes network_connection.pb.h, so
# every source file that reaches it needs the generated header. The switch
# exists so a future SDK that drops the dependency can turn it off.
option(CS2SDKTEST_WITH_PROTOBUF
       "Generate and link the SDK's protobuf sources (required by eiface.h)" ON)

# Protos to run through protoc. network_connection is the one the SDK headers
# demand; netmessages (+ its imports networkbasetypes, source2_steam_stats and
# valveextensions) is what NetworkMessagesSuite needs to walk real net
# messages - the generated CNETMsg_Tick is the round-trip subject.
set(CS2SDKTEST_PROTO_FILES "network_connection;netmessages"
    CACHE STRING "s2sdk/common proto files to generate (without .proto)")

# ON by default since KeyValues3Suite: it reads back a KeyValues3 the engine
# wrote (ISource2Server::WriteStatusJson), and the KeyValues3 implementation is
# compiled into the library rather than exported by tier0. With this off that
# suite reports itself unavailable.
option(CS2SDKTEST_TIER1_KEYVALUES3
       "Also compile tier1/keyvalues3.cpp into the library" ON)
# ON by default since NetworkMessagesSuite: its serialization round trip needs
# bf_write/bf_read, whose constructors are exported by no prebuilt library
# (tier0 contains only netadr_t::Serialize/Unserialize). With this off that
# suite reports every serialization method as SKIP.
option(CS2SDKTEST_TIER1_BITBUF
       "Also compile tier1/bitbuf.cpp into the library" ON)
option(CS2SDKTEST_WARNINGS_AS_ERRORS
       "Treat warnings in this project's own sources as errors" OFF)

# Library identity, written into every log header.
set(CS2SDKTEST_DISPLAY_NAME "CS2 SDK Test")
set(CS2SDKTEST_DESCRIPTION  "${PROJECT_DESCRIPTION}")
set(CS2SDKTEST_AUTHOR       "unagi")
set(CS2SDKTEST_URL          "https://github.com/zqunagi/CS2SDKTest")
set(CS2SDKTEST_LICENSE      "MIT")
set(CS2SDKTEST_LOGTAG       "SDKTEST")

# -----------------------------------------------------------------------------
#  Dependencies
# -----------------------------------------------------------------------------
include(S2SDK)
include(PluginVersion)
if(CS2SDKTEST_WITH_PROTOBUF)
  include(Protobuf)
endif()
if(WIN32)
  include(Detours)
endif()

# CS2SDKTEST_BIN_DIR is the name of the server's own bin subdirectory, which the
# package mirrors; the library works out <csgo> from it (see fs::GameDirectory).
if(WIN32)
  set(CS2SDKTEST_PLATFORM "windows")
  set(CS2SDKTEST_BIN_DIR  "win64")
else()
  set(CS2SDKTEST_PLATFORM "linux")
  set(CS2SDKTEST_BIN_DIR  "linuxsteamrt64")
endif()

cs2sdktest_resolve_git_revision("${CMAKE_CURRENT_SOURCE_DIR}" CS2SDKTEST_GIT_REVISION)
cs2sdktest_resolve_git_commit("${CMAKE_CURRENT_SOURCE_DIR}" CS2SDKTEST_GIT_COMMIT)
cs2sdktest_resolve_sdk_revision("${CS2SDKTEST_S2SDK_DIR}" CS2SDKTEST_SDK_REVISION)
cs2sdktest_full_version("${PROJECT_VERSION}" "${CS2SDKTEST_GIT_REVISION}" CS2SDKTEST_FULL_VERSION)

# -----------------------------------------------------------------------------
#  Package layout
#
#  The package is the contents of the server's game/csgo directory, so it is
#  unzipped straight into <server>/game/csgo:
#
#    bin/linuxsteamrt64/libcs2sdktest.so  the library (Linux)
#    bin/linuxsteamrt64/cst_run.sh        starts a server with the library preloaded
#    bin/win64/cs2sdktest.dll             the library (Windows)
#    bin/win64/cst_run.exe                starts a server with the library injected
#    configs/gamedata.toml                its data file
#    configs/cs2sdktest-meta.json         which build this is, for a run that died
#                                         before the library wrote anything
#
#  Run logs and results are not part of the package: the library writes them to
#  <csgo>/logs at run time (see fs::GameDirectory).
# -----------------------------------------------------------------------------
set(CS2SDKTEST_PACKAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/package")
set(CS2SDKTEST_PACKAGE_BIN_DIR "${CS2SDKTEST_PACKAGE_DIR}/bin/${CS2SDKTEST_BIN_DIR}")

# -----------------------------------------------------------------------------
#  Generated sources
# -----------------------------------------------------------------------------
set(CS2SDKTEST_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/templates/version_gen.h.in"
  "${CS2SDKTEST_GENERATED_DIR}/version_gen.h"
  @ONLY
)

# The revision header is rewritten by a target that runs on every build, so
# the reported version follows commits and the dirty flag without a re-configure.
# Generated here as well so the first compile has a header before that runs.
set(CS2SDKTEST_REVISION_HEADER "${CS2SDKTEST_GENERATED_DIR}/revision_gen.h")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/templates/revision_gen.h.in"
  "${CS2SDKTEST_REVISION_HEADER}"
  @ONLY
)

# The build metadata travels in the package. Whoever runs the package (the runner) can
# then say which build it was even when the process died before the library
# wrote anything itself: the engine going down first leaves no results file to
# take the version from.
set(CS2SDKTEST_PROJECT_NAME "${PROJECT_NAME}")
set(CS2SDKTEST_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
set(CS2SDKTEST_META_FILE "${CS2SDKTEST_PACKAGE_DIR}/configs/cs2sdktest-meta.json")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/templates/meta.json.in"
  "${CS2SDKTEST_META_FILE}"
  @ONLY
)

add_custom_target(cs2sdktest_revision
  COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DS2SDK_DIR=${CS2SDKTEST_S2SDK_DIR}
    -DPROJECT_VERSION=${PROJECT_VERSION}
    -DTEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/templates/revision_gen.h.in
    -DOUTPUT=${CS2SDKTEST_REVISION_HEADER}
    -DMETA_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/templates/meta.json.in
    -DMETA_OUTPUT=${CS2SDKTEST_META_FILE}
    -DCS2SDKTEST_PROJECT_NAME=${CS2SDKTEST_PROJECT_NAME}
    -DCS2SDKTEST_PLATFORM=${CS2SDKTEST_PLATFORM}
    -DCS2SDKTEST_BUILD_TYPE=$<CONFIG>
    -DS2SDK_ENGINE_NAME=${S2SDK_ENGINE_NAME}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateRevision.cmake
  BYPRODUCTS "${CS2SDKTEST_REVISION_HEADER}" "${CS2SDKTEST_META_FILE}"
  COMMENT "Refreshing the version header"
  VERBATIM
)

set(CS2SDKTEST_PROTO_SOURCES "")
if(CS2SDKTEST_WITH_PROTOBUF)
  cs2sdktest_generate_protobuf(CS2SDKTEST_PROTO_SOURCES ${CS2SDKTEST_PROTO_FILES})
endif()

# GamedataSuite reads this from <csgo>/configs, so the package carries it there.
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/configs/gamedata.toml"
  "${CS2SDKTEST_PACKAGE_DIR}/configs/gamedata.toml"
  COPYONLY
)

if(NOT WIN32)
  # The launcher sits next to the library: it finds the server from there, and the
  # library finds the game directory from the same place. Copied with the execute
  # bit spelled out rather than trusting the checkout to have kept it.
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cst_run.sh"
    DESTINATION "${CS2SDKTEST_PACKAGE_BIN_DIR}"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                     GROUP_READ GROUP_EXECUTE
                     WORLD_READ WORLD_EXECUTE
  )
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cst_run.sh")
endif()

# -----------------------------------------------------------------------------
#  The library
# -----------------------------------------------------------------------------
set(CS2SDKTEST_SOURCES
  src/Entry.cpp
  src/plugin/Sweep.cpp

  src/core/Log.cpp
  src/core/Module.cpp
  src/core/Process.cpp
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


  src/suites/Source2EngineSuite.cpp
  src/suites/Source2ServerSuite.cpp
  src/suites/Source2GameClientsSuite.cpp
  src/suites/ConVarSuite.cpp
  src/suites/SchemaSystemSuite.cpp
  src/suites/SchemaDiffSuite.cpp
  src/suites/GameEntitySystemSuite.cpp
  src/suites/GameEventSuite.cpp
  src/suites/NetworkServerSuite.cpp
  src/suites/NetworkMessagesSuite.cpp
  src/suites/ServerConfigSuite.cpp
  src/suites/EngineServiceMgrSuite.cpp
  src/suites/StringTableSuite.cpp
  src/suites/GameEntitiesSuite.cpp
  src/suites/HostStateMgrSuite.cpp
  src/suites/LoopModeSuite.cpp
  src/suites/NetChannelSuite.cpp
  src/suites/ResourceSystemSuite.cpp
  src/suites/FileSystemSuite.cpp
  src/suites/GamedataSuite.cpp
  src/suites/Tier0ContainerSuite.cpp
  src/suites/Tier0PlatformSuite.cpp
  src/suites/KeyValues3Suite.cpp
  src/suites/LocalizeSuite.cpp
  src/suites/GameTypesSuite.cpp
  src/suites/SerializedEntitiesSuite.cpp
  src/suites/FlattenedSerializersSuite.cpp
  src/testkit/BaselineLookup.cpp
)

set(CS2SDKTEST_HEADERS
  src/Entry.h
  src/plugin/Sweep.h

  src/core/Platform.h
  src/core/Log.h
  src/core/Module.h
  src/core/Process.h
  src/core/FileSystemUtil.h
  src/core/TomlData.h
  src/core/SignatureScan.h
  src/core/VtableIndex.h

  src/data/GameModules.h

  src/testkit/CallGuard.h
  src/testkit/ITestSuite.h
  src/testkit/PrivateField.h
  src/testkit/StructInspector.h
  src/testkit/TestContext.h
  src/testkit/TestRunner.h
  src/testkit/ResultSink.h

  src/hook/HookArg.h

  src/suites/Source2EngineSuite.h
  src/suites/Source2ServerSuite.h
  src/suites/Source2GameClientsSuite.h
  src/suites/ConVarSuite.h
  src/suites/SchemaSystemSuite.h
  src/suites/SchemaDiffSuite.h
  src/suites/GameEntitySystemSuite.h
  src/suites/GameEventSuite.h
  src/suites/NetworkServerSuite.h
  src/suites/NetworkMessagesSuite.h
  src/suites/ServerConfigSuite.h
  src/suites/EngineServiceMgrSuite.h
  src/suites/StringTableSuite.h
  src/suites/GameEntitiesSuite.h
  src/suites/HostStateMgrSuite.h
  src/suites/LoopModeSuite.h
  src/suites/NetChannelSuite.h
  src/suites/ResourceSystemSuite.h
  src/suites/FileSystemSuite.h
  src/suites/GamedataSuite.h
  src/suites/Tier0ContainerSuite.h
  src/suites/Tier0PlatformSuite.h
  src/suites/KeyValues3Suite.h
  src/suites/LocalizeSuite.h
  src/suites/GameTypesSuite.h
  src/suites/SerializedEntitiesSuite.h
  src/suites/FlattenedSerializersSuite.h
  src/testkit/BaselineLookup.h
)

# Source 2 libraries built on s2sdk compile these SDK translation units directly:
# memoverride.cpp hands allocation to tier0 so memory crossing the module
# boundary is freed by the allocator that made it.
# entity2/*.cpp are the SDK's own implementations of GetEntityIdentity,
# FindClassByName and the EntityInstance*Iter_t walkers - pure struct-offset
# arithmetic with no engine symbol behind it. GameEntitySystemSuite calls them
# on purpose: they are where a moved field produces a wrong answer rather than
# a crash. They need a definition of GameEntitySystem(), which the suite
# supplies.
set(CS2SDKTEST_SDK_SOURCES
  "${S2SDK_ROOT}/entity2/entitysystem.cpp"
  "${S2SDK_ROOT}/entity2/entityidentity.cpp"
)
# entitysystem.cpp's CreateEntityKeyValues / SpawnEntities construct and release
# CEntityKeyValues, which entitykeyvalues.cpp defines on top of CKV3Arena from
# tier0_perproject/keyvalues3.cpp (CS2SDKTEST_TIER1_KEYVALUES3). Without that
# option those symbols stay undefined.
if(CS2SDKTEST_TIER1_KEYVALUES3)
  list(APPEND CS2SDKTEST_SDK_SOURCES "${S2SDK_ROOT}/entity2/entitykeyvalues.cpp")
else()
  message(WARNING "CS2SDKTEST_TIER1_KEYVALUES3 is OFF: entity2/entitysystem.cpp references CEntityKeyValues, which will be undefined")
endif()
if(CS2SDKTEST_USE_MEMOVERRIDE)
  list(APPEND CS2SDKTEST_SDK_SOURCES "${S2SDK_ROOT}/public/tier0/memoverride.cpp")
endif()

# DllMain is the Windows loader entry; on Linux the constructor in Entry.cpp is.
if(WIN32)
  list(APPEND CS2SDKTEST_SOURCES src/DllMain.cpp)
endif()

add_library(cs2sdktest SHARED
  ${CS2SDKTEST_SOURCES}
  ${CS2SDKTEST_HEADERS}
  ${CS2SDKTEST_SDK_SOURCES}
  ${CS2SDKTEST_PROTO_SOURCES}
)

# The .def file is passed to the linker as /DEF:. Added here rather than to
# CS2SDKTEST_SOURCES because source_group(TREE src) below only takes files under src.
if(WIN32)
  target_sources(cs2sdktest PRIVATE cmake/exports.def)
endif()

# memoverride.cpp and protoc output are both foreign code, and both are noisy.
# Source-level options are appended after the target's, so a blanket disable
# here wins over the warning set further down without affecting our own files.
set(_foreign_sources ${CS2SDKTEST_SDK_SOURCES} ${CS2SDKTEST_PROTO_SOURCES})
if(_foreign_sources)
  set_source_files_properties(${_foreign_sources} PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "-w"
  )
endif()

target_include_directories(cs2sdktest PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CS2SDKTEST_GENERATED_DIR}"
)

if(CS2SDKTEST_WITH_PROTOBUF)
  # The SDK includes it as "network_connection.pb.h", unqualified.
  target_include_directories(cs2sdktest SYSTEM PRIVATE "${CS2SDKTEST_PROTO_OUT_DIR}")
endif()

# NetworkMessagesSuite's round trip compiles in only when bitbuf is present;
# the define is how the .cpp knows which half of itself to build.
# KeyValues3Suite is only meaningful with the SDK implementation compiled in.
if(CS2SDKTEST_TIER1_KEYVALUES3)
  target_compile_definitions(cs2sdktest PRIVATE CS2SDKTEST_WITH_KEYVALUES3=1)
endif()
if(CS2SDKTEST_TIER1_BITBUF)
  target_compile_definitions(cs2sdktest PRIVATE CS2SDKTEST_WITH_BITBUF=1)
endif()

add_dependencies(cs2sdktest cs2sdktest_revision)

# std::thread needs -pthread on glibc older than 2.34.
find_package(Threads REQUIRED)
target_link_libraries(cs2sdktest PRIVATE s2sdk::s2sdk Threads::Threads)

# Upstream s2sdk builds itself as C++20 (AMBuildScript, Oct 2026); matching it
# keeps this library and the SDK headers under the same language rules.
target_compile_features(cs2sdktest PRIVATE cxx_std_20)
set_target_properties(cs2sdktest PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME "cs2sdktest"
  LIBRARY_OUTPUT_DIRECTORY "${CS2SDKTEST_PACKAGE_BIN_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CS2SDKTEST_PACKAGE_BIN_DIR}"
)

# Multi-config generators append the config name to the output directory unless
# told otherwise; the package layout has to stay as laid out above.
foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(cs2sdktest PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_config} "${CS2SDKTEST_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_${_config} "${CS2SDKTEST_PACKAGE_BIN_DIR}"
  )
endforeach()

# Warnings for our own sources only - the SDK's are silenced in s2sdk::headers.
if(MSVC)
  target_compile_options(cs2sdktest PRIVATE /W4)
  if(CS2SDKTEST_WARNINGS_AS_ERRORS)
    target_compile_options(cs2sdktest PRIVATE /WX)
  endif()
else()
  target_compile_options(cs2sdktest PRIVATE -Wall -Wextra)
  if(CS2SDKTEST_WARNINGS_AS_ERRORS)
    target_compile_options(cs2sdktest PRIVATE -Werror)
  endif()
endif()

if(WIN32)
  # Windows has no global symbol interposition, so there is no hygiene problem to
  # solve: a DLL's exports are looked up in that DLL only. What it does need is
  # ordinal 1, which Detours puts into cs2.exe's import table (exports.def).
  set(CS2SDKTEST_EXPORTS_DEF "${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.def")
  set_target_properties(cs2sdktest PROPERTIES LINK_DEPENDS "${CS2SDKTEST_EXPORTS_DEF}")
else()
  # Symbol hygiene is the point of this block. LD_PRELOAD puts this object ahead
  # of every other library in the process, so any symbol it exports wins lookup
  # for the engine too. The SDK's interfaces.a defines CreateInterface (and the
  # g_p* globals); exported from here, the engine's own libtier0.so would resolve
  # to ours. The version script exports nothing, and --exclude-libs hides what
  # the static archives would otherwise leak. Nothing in this object needs to be
  # found by name: the constructor is called by the loader, not looked up.
  #
  # cs2.json sets uses_system_cxxlib=false: the game ships an old libstdc++ and
  # loading a library that expects a newer one crashes on the first std:: symbol,
  # hence the static libstdc++/libgcc.
  set(CS2SDKTEST_EXPORTS_MAP "${CMAKE_CURRENT_SOURCE_DIR}/cmake/exports.map")
  set_target_properties(cs2sdktest PROPERTIES
    LINK_DEPENDS "${CS2SDKTEST_EXPORTS_MAP}")
  target_link_options(cs2sdktest PRIVATE
    -static-libstdc++
    -static-libgcc
    -Wl,--version-script=${CS2SDKTEST_EXPORTS_MAP}
    -Wl,--exclude-libs,ALL
    -Wl,-Bsymbolic
  )

  # Fails the build when the object exports anything. Cheap, and the only thing
  # standing between a mistake here and an engine that fails to start.
  add_custom_command(TARGET cs2sdktest POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DSO=$<TARGET_FILE:cs2sdktest>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckExports.cmake
    COMMENT "Checking that cs2sdktest exports nothing"
    VERBATIM
  )
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src"
  FILES ${CS2SDKTEST_SOURCES} ${CS2SDKTEST_HEADERS})

# -----------------------------------------------------------------------------
#  The Windows launcher
#
#  Windows has no LD_PRELOAD, so cst_run.exe does what the shell script does on
#  Linux and, in place of the environment variable, has Detours inject the DLL
#  into cs2.exe as it starts. It sits next to the DLL and uses neither the SDK
#  nor the SDK's compiler settings. Static CRT (S2SDK.cmake), so it runs on a
#  machine without the VC redistributable.
# -----------------------------------------------------------------------------
if(WIN32)
  add_executable(cst_run tools/cst_run/main.cpp)
  target_link_libraries(cst_run PRIVATE detours::detours)
  target_compile_features(cst_run PRIVATE cxx_std_17)
  target_compile_options(cst_run PRIVATE /W4)
  if(CS2SDKTEST_WARNINGS_AS_ERRORS)
    target_compile_options(cst_run PRIVATE /WX)
  endif()
  set_target_properties(cst_run PROPERTIES
    OUTPUT_NAME "cst_run"
    RUNTIME_OUTPUT_DIRECTORY "${CS2SDKTEST_PACKAGE_BIN_DIR}"
  )
  foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(cst_run PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY_${_config} "${CS2SDKTEST_PACKAGE_BIN_DIR}")
  endforeach()
  # The launcher is only useful with the library beside it.
  add_dependencies(cst_run cs2sdktest)
endif()

# -----------------------------------------------------------------------------
#  Packaging / installation
# -----------------------------------------------------------------------------
install(TARGETS cs2sdktest
  LIBRARY DESTINATION "bin/${CS2SDKTEST_BIN_DIR}"
  RUNTIME DESTINATION "bin/${CS2SDKTEST_BIN_DIR}"
)
if(WIN32)
  install(TARGETS cst_run RUNTIME DESTINATION "bin/${CS2SDKTEST_BIN_DIR}")
else()
  install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cst_run.sh"
    DESTINATION "bin/${CS2SDKTEST_BIN_DIR}"
  )
endif()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/configs/gamedata.toml"
  DESTINATION "configs"
)

message(STATUS "cs2sdktest ${CS2SDKTEST_FULL_VERSION} (${CS2SDKTEST_PLATFORM})")
