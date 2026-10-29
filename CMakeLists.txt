cmake_minimum_required(VERSION 3.21)

# =============================================================================
#  CS2SDKTest - a Metamod:Source exercise harness for s2sdk (cs2 branch)
#
#  Behaves like an ordinary plugin, but its job is to use as much of the SDK as
#  possible: call the engine interfaces, hook the callbacks, and log what
#  actually happened. After a game update, the log is what tells you which
#  parts of s2sdk no longer match the game.
# =============================================================================

project(cs2sdktest
  VERSION 0.1.0
  DESCRIPTION "s2sdk surface diagnostics for Counter-Strike 2"
  LANGUAGES CXX
)

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
set(CS2SDKTEST_METAMOD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/metamod-source"
    CACHE PATH "Path to the metamod-source checkout")
set(CS2SDKTEST_GAME_DIR    ""
    CACHE PATH "Optional: <csgo> directory to install the built plugin into")

# Off by default, and not just for convenience: on MSVC the SDK's
# memoverride.cpp does not currently link. AlliedModders disabled the
# _recalloc_base *definition* in it (#if 0, "causes crashes on library unload")
# but left _recalloc_crt calling it, so the symbol is unresolved. It also
# redefines malloc/free and collides with the CRT's own copies.
#
# This plugin does not need it: it reads from the engine and allocates only for
# its own containers, so no allocation crosses the module boundary. Turn it on
# if that ever stops being true - and expect to sort out the MSVC link first.
option(CS2SDKTEST_USE_MEMOVERRIDE
       "Route new/delete through tier0's allocator (see comment; MSVC link is broken)" OFF)
# Not really optional: public/eiface.h includes network_connection.pb.h, and
# eiface.h is reached from ISmmPlugin.h, so every source file needs it. The
# switch exists so a future SDK that drops the dependency can turn it off.
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
# compiled into the plugin rather than exported by tier0. With this off that
# suite reports itself unavailable.
option(CS2SDKTEST_TIER1_KEYVALUES3
       "Also compile tier1/keyvalues3.cpp into the plugin" ON)
# ON by default since NetworkMessagesSuite: its serialization round trip needs
# bf_write/bf_read, whose constructors are exported by no prebuilt library
# (tier0 contains only netadr_t::Serialize/Unserialize). With this off that
# suite reports every serialization method as SKIP.
option(CS2SDKTEST_TIER1_BITBUF
       "Also compile tier1/bitbuf.cpp into the plugin" ON)
option(CS2SDKTEST_WARNINGS_AS_ERRORS
       "Treat warnings in this project's own sources as errors" OFF)

# Plugin identity, surfaced through ISmmPlugin and written into every log header.
set(CS2SDKTEST_DISPLAY_NAME "CS2 SDK Test")
set(CS2SDKTEST_DESCRIPTION  "${PROJECT_DESCRIPTION}")
set(CS2SDKTEST_AUTHOR       "unagi")
set(CS2SDKTEST_URL          "https://github.com/alliedmodders/metamod-source")
set(CS2SDKTEST_LICENSE      "MIT")
set(CS2SDKTEST_LOGTAG       "SDKTEST")
set(CS2SDKTEST_ALIAS        "cs2sdktest")
set(CS2SDKTEST_TARGET_NAME  "cs2sdktest")

# -----------------------------------------------------------------------------
#  Dependencies
# -----------------------------------------------------------------------------
include(S2SDK)
include(Metamod)
include(PluginVersion)
if(CS2SDKTEST_WITH_PROTOBUF)
  include(Protobuf)
endif()

if(WIN32)
  set(CS2SDKTEST_PLATFORM "windows")
else()
  set(CS2SDKTEST_PLATFORM "linux")
endif()

cs2sdktest_resolve_git_revision(CS2SDKTEST_GIT_REVISION)
cs2sdktest_resolve_sdk_revision("${CS2SDKTEST_S2SDK_DIR}" CS2SDKTEST_SDK_REVISION)
set(CS2SDKTEST_FULL_VERSION "${PROJECT_VERSION}-${CS2SDKTEST_GIT_REVISION}")

# -----------------------------------------------------------------------------
#  Generated sources
# -----------------------------------------------------------------------------
set(CS2SDKTEST_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/templates/version_gen.h.in"
  "${CS2SDKTEST_GENERATED_DIR}/version_gen.h"
  @ONLY
)

set(CS2SDKTEST_PROTO_SOURCES "")
if(CS2SDKTEST_WITH_PROTOBUF)
  cs2sdktest_generate_protobuf(CS2SDKTEST_PROTO_SOURCES ${CS2SDKTEST_PROTO_FILES})
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/configs/cs2sdktest.vdf.in"
  "${CMAKE_CURRENT_BINARY_DIR}/package/addons/metamod/${CS2SDKTEST_ALIAS}.vdf"
  @ONLY
)

# GamedataSuite reads this from <plugin_dir>/configs beside the binary, so the
# package layout and the installed layout both have to carry it.
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/configs/gamedata.toml"
  "${CMAKE_CURRENT_BINARY_DIR}/package/addons/${CS2SDKTEST_ALIAS}/configs/gamedata.toml"
  COPYONLY
)

# -----------------------------------------------------------------------------
#  The plugin
# -----------------------------------------------------------------------------
set(CS2SDKTEST_SOURCES
  src/plugin/Plugin.cpp
  src/plugin/Commands.cpp
  src/plugin/Autorun.cpp

  src/core/Log.cpp
  src/core/Module.cpp
  src/core/FileSystemUtil.cpp
  src/core/TomlData.cpp
  src/core/SignatureScan.cpp

  src/data/GameModules.cpp

  src/testkit/CallGuard.cpp
  src/testkit/StructInspector.cpp
  src/testkit/TestContext.cpp
  src/testkit/TestRunner.cpp
  src/testkit/ResultSink.cpp

  src/hook/HookRecorder.cpp

  src/suites/Source2EngineSuite.cpp
  src/suites/Source2ServerSuite.cpp
  src/suites/Source2GameClientsSuite.cpp
  src/suites/ConVarSuite.cpp
  src/suites/SchemaSystemSuite.cpp
  src/suites/GameEntitySystemSuite.cpp
  src/suites/GameEventSuite.cpp
  src/suites/NetworkServerSuite.cpp
  src/suites/NetworkMessagesSuite.cpp
  src/suites/MetamodListenerSuite.cpp
  src/suites/ServerConfigSuite.cpp
  src/suites/EngineServiceMgrSuite.cpp
  src/suites/StringTableSuite.cpp
  src/suites/GameEntitiesSuite.cpp
  src/suites/FileSystemSuite.cpp
  src/suites/GamedataSuite.cpp
  src/suites/Tier0ContainerSuite.cpp
  src/suites/KeyValues3Suite.cpp
)

set(CS2SDKTEST_HEADERS
  src/plugin/Plugin.h
  src/plugin/Commands.h
  src/plugin/Autorun.h

  src/core/Platform.h
  src/core/Log.h
  src/core/Module.h
  src/core/FileSystemUtil.h
  src/core/TomlData.h
  src/core/SignatureScan.h

  src/data/GameModules.h

  src/testkit/CallGuard.h
  src/testkit/ITestSuite.h
  src/testkit/PrivateField.h
  src/testkit/StructInspector.h
  src/testkit/TestContext.h
  src/testkit/TestRunner.h
  src/testkit/ResultSink.h

  src/hook/HookArg.h
  src/hook/HookRecorder.h

  src/suites/Source2EngineSuite.h
  src/suites/Source2ServerSuite.h
  src/suites/Source2GameClientsSuite.h
  src/suites/ConVarSuite.h
  src/suites/SchemaSystemSuite.h
  src/suites/GameEntitySystemSuite.h
  src/suites/GameEventSuite.h
  src/suites/NetworkServerSuite.h
  src/suites/NetworkMessagesSuite.h
  src/suites/MetamodListenerSuite.h
  src/suites/ServerConfigSuite.h
  src/suites/EngineServiceMgrSuite.h
  src/suites/StringTableSuite.h
  src/suites/GameEntitiesSuite.h
  src/suites/FileSystemSuite.h
  src/suites/GamedataSuite.h
  src/suites/Tier0ContainerSuite.h
  src/suites/KeyValues3Suite.h
)

# Upstream Source 2 plugins compile these SDK translation units directly:
# memoverride.cpp hands allocation to tier0 so memory crossing the plugin
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
if(CS2SDKTEST_USE_MEMOVERRIDE)
  list(APPEND CS2SDKTEST_SDK_SOURCES "${S2SDK_ROOT}/public/tier0/memoverride.cpp")
endif()

add_library(cs2sdktest MODULE
  ${CS2SDKTEST_SOURCES}
  ${CS2SDKTEST_HEADERS}
  ${CS2SDKTEST_SDK_SOURCES}
  ${CS2SDKTEST_PROTO_SOURCES}
)

# memoverride.cpp and protoc output are both foreign code, and both are noisy.
# Source-level options are appended after the target's, so a blanket disable
# here wins over the /W4 set further down without affecting our own files.
set(_foreign_sources ${CS2SDKTEST_SDK_SOURCES} ${CS2SDKTEST_PROTO_SOURCES})
if(_foreign_sources)
  if(MSVC)
    set(_foreign_warning_flags "/W0")
  else()
    set(_foreign_warning_flags "-w")
  endif()
  set_source_files_properties(${_foreign_sources} PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS "${_foreign_warning_flags}"
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

target_link_libraries(cs2sdktest PRIVATE
  metamod::metamod
  s2sdk::s2sdk
)

# Upstream s2sdk builds itself as C++20 (AMBuildScript, Oct 2026); matching it
# keeps this plugin and the SDK headers under the same language rules.
target_compile_features(cs2sdktest PRIVATE cxx_std_20)

set_target_properties(cs2sdktest PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  # Plain cs2sdktest.so / cs2sdktest.dll. Metamod tries "<file>.x64.<ext>"
  # first and falls back to "<file>.<ext>", which is the one we produce.
  PREFIX ""
  OUTPUT_NAME "${CS2SDKTEST_TARGET_NAME}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/addons/${CS2SDKTEST_ALIAS}/bin"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/package/addons/${CS2SDKTEST_ALIAS}/bin"
)

if(WIN32)
  set_target_properties(cs2sdktest PROPERTIES SUFFIX ".dll")
else()
  set_target_properties(cs2sdktest PROPERTIES SUFFIX ".so")
endif()

# Multi-config generators append the config name to the output directory unless
# told otherwise; the package layout has to stay stable for the vdf to work.
foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(cs2sdktest PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_config} "${CMAKE_CURRENT_BINARY_DIR}/package/addons/${CS2SDKTEST_ALIAS}/bin"
    RUNTIME_OUTPUT_DIRECTORY_${_config} "${CMAKE_CURRENT_BINARY_DIR}/package/addons/${CS2SDKTEST_ALIAS}/bin"
  )
endforeach()

# Warnings for our own sources only - the SDK's are silenced in s2sdk::headers.
if(MSVC)
  target_compile_options(cs2sdktest PRIVATE /W4)
  # C4355 fires on every suite constructor: the KHook::Virtual initializers
  # take `this` as their context argument. The hook ctor only stores the
  # pointer (it never dereferences it or calls back before construction
  # finishes), so the "this used in member initializer list" warning is a
  # false positive for this binding pattern.
  target_compile_options(cs2sdktest PRIVATE /wd4355)
  if(CS2SDKTEST_WARNINGS_AS_ERRORS)
    target_compile_options(cs2sdktest PRIVATE /WX)
  endif()
else()
  target_compile_options(cs2sdktest PRIVATE -Wall -Wextra)
  if(CS2SDKTEST_WARNINGS_AS_ERRORS)
    target_compile_options(cs2sdktest PRIVATE -Werror)
  endif()
  # cs2.json sets uses_system_cxxlib=false: the game ships an old libstdc++ and
  # loading a plugin that expects a newer one crashes on the first std:: symbol.
  #
  # Do NOT add -Wl,--exclude-libs,ALL here. It makes every symbol coming from a
  # static archive local, and CreateInterface - the entry point Metamod looks
  # for - is defined in the SDK's interfaces.a. Hiding it does not produce a
  # missing-symbol error: dlsym walks the dependency chain and finds
  # libtier0.so's CreateInterface instead, which knows nothing about
  # "ISmmPlugin", so the plugin fails to load with "Failed to get API".
  target_link_options(cs2sdktest PRIVATE
    -static-libstdc++
    -static-libgcc
  )
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src"
  FILES ${CS2SDKTEST_SOURCES} ${CS2SDKTEST_HEADERS})

# -----------------------------------------------------------------------------
#  Packaging / installation
# -----------------------------------------------------------------------------
install(TARGETS cs2sdktest
  LIBRARY DESTINATION "addons/${CS2SDKTEST_ALIAS}/bin"
  RUNTIME DESTINATION "addons/${CS2SDKTEST_ALIAS}/bin"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/package/addons/metamod/${CS2SDKTEST_ALIAS}.vdf"
  DESTINATION "addons/metamod"
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/configs/gamedata.toml"
  DESTINATION "addons/${CS2SDKTEST_ALIAS}/configs"
)

# `cmake --build . --target deploy` drops the package straight into a server.
if(CS2SDKTEST_GAME_DIR)
  add_custom_target(deploy
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_BINARY_DIR}/package"
            "${CS2SDKTEST_GAME_DIR}"
    DEPENDS cs2sdktest
    COMMENT "Deploying ${CS2SDKTEST_ALIAS} to ${CS2SDKTEST_GAME_DIR}"
    VERBATIM
  )
endif()

message(STATUS "cs2sdktest ${CS2SDKTEST_FULL_VERSION} (${CS2SDKTEST_PLATFORM})")
