# =============================================================================
#  Metamod.cmake
#
#  Defines metamod::metamod - the headers a Metamod:Source plugin needs.
#
#  A plugin links nothing from Metamod: ISmmPlugin/ISmmAPI are pure virtual and
#  the KHook entry points resolve through KHook::__exported__khook, which
#  PLUGIN_GLOBALVARS() declares and PLUGIN_SAVEVARS fills from
#  ISmmAPI::GetDetourInterface. So this is headers only, on purpose.
# =============================================================================

if(TARGET metamod::metamod)
  return()
endif()

set(METAMOD_ROOT "${CS2SDKTEST_METAMOD_DIR}" CACHE PATH "Root of the metamod-source checkout")

if(NOT EXISTS "${METAMOD_ROOT}/core/ISmmPlugin.h")
  message(FATAL_ERROR
    "metamod-source not found at '${METAMOD_ROOT}'.\n"
    "Run: git submodule update --init --recursive")
endif()

if(NOT EXISTS "${METAMOD_ROOT}/third_party/khook/include/khook.hpp")
  message(FATAL_ERROR
    "khook not found at '${METAMOD_ROOT}/third_party/khook'.\n"
    "Metamod 2.1 replaced SourceHook with khook; run:\n"
    "  git submodule update --init --recursive metamod-source")
endif()

add_library(metamod_headers INTERFACE)
add_library(metamod::metamod ALIAS metamod_headers)

target_include_directories(metamod_headers SYSTEM INTERFACE
  "${METAMOD_ROOT}/core"
  "${METAMOD_ROOT}/third_party/khook/include"
)

# ISmmPlugin.h branches on this: with META_IS_SOURCE2 it pulls in
# interfaces/interfaces.h, without it tier1/interface.h - a file the cs2 branch
# of s2sdk does not have. Getting this wrong fails at the very first include.
target_compile_definitions(metamod_headers INTERFACE
  META_IS_SOURCE2
)

target_link_libraries(metamod_headers INTERFACE s2sdk::headers)

# Read the Metamod API version out of the headers so the plugin can report the
# version it was built against, and so a Metamod bump is visible in the diff.
#
# It lives in ISmmPluginExt.h today; ISmmPlugin.h only uses it. Both are checked
# so a future reshuffle upstream does not turn into a configure failure here.
set(METAMOD_PLAPI_VERSION "")
foreach(_header "core/ISmmPluginExt.h" "core/ISmmPlugin.h")
  if(METAMOD_PLAPI_VERSION OR NOT EXISTS "${METAMOD_ROOT}/${_header}")
    continue()
  endif()
  file(STRINGS "${METAMOD_ROOT}/${_header}" _mm_plapi_line
    REGEX "^[ \t]*#define[ \t]+METAMOD_PLAPI_VERSION[ \t]+[0-9]+")
  if(_mm_plapi_line MATCHES "METAMOD_PLAPI_VERSION[ \t]+([0-9]+)")
    set(METAMOD_PLAPI_VERSION "${CMAKE_MATCH_1}")
  endif()
endforeach()

if(NOT METAMOD_PLAPI_VERSION)
  message(FATAL_ERROR
    "Could not parse METAMOD_PLAPI_VERSION from the metamod headers under "
    "'${METAMOD_ROOT}/core'.")
endif()

message(STATUS "metamod-source: ${METAMOD_ROOT} (PLAPI version ${METAMOD_PLAPI_VERSION})")
