# =============================================================================
#  Detours.cmake
#
#  Microsoft Detours, as the imported target detours::detours (STATIC).
#
#  Windows has no LD_PRELOAD. cst_run.exe starts cs2.exe suspended with
#  DetourCreateProcessWithDllEx, which adds cs2sdktest.dll to the new process's
#  import table before it runs a single instruction. That is the only part of
#  Detours this project uses; the library itself hooks nothing.
#
#  Detours is built from source here rather than through its nmake makefiles, so
#  it picks up this project's compiler, architecture and static CRT. The list of
#  translation units is the one its own makefile builds; the per-architecture
#  disassemblers are guarded by DETOURS_<ARCH> and compile to nothing on the
#  others.
#
#  CS2SDKTEST_DETOURS_DIR points at an existing checkout (offline builds, a
#  patched copy). Left empty, the pinned release is fetched.
# =============================================================================

if(TARGET detours::detours)
  return()
endif()

set(CS2SDKTEST_DETOURS_DIR "" CACHE PATH
    "Path to a Microsoft Detours checkout; empty fetches the pinned release")
set(CS2SDKTEST_DETOURS_TAG "v4.0.1" CACHE STRING "Detours release to fetch")

if(NOT CS2SDKTEST_DETOURS_DIR)
  include(FetchContent)
  # Detours has no CMakeLists.txt of its own to add; SOURCE_SUBDIR points at a
  # directory that does not exist so that FetchContent only downloads it.
  FetchContent_Declare(detours
    GIT_REPOSITORY https://github.com/microsoft/Detours.git
    GIT_TAG        ${CS2SDKTEST_DETOURS_TAG}
    GIT_SHALLOW    ON
    SOURCE_SUBDIR  cs2sdktest-no-cmake
  )
  FetchContent_MakeAvailable(detours)
  set(_detours_root "${detours_SOURCE_DIR}")
else()
  set(_detours_root "${CS2SDKTEST_DETOURS_DIR}")
endif()

set(_detours_src "${_detours_root}/src")
if(NOT EXISTS "${_detours_src}/detours.h")
  message(FATAL_ERROR "Detours not found: '${_detours_src}/detours.h' does not exist")
endif()

add_library(detours STATIC
  "${_detours_src}/detours.cpp"
  "${_detours_src}/modules.cpp"
  "${_detours_src}/disasm.cpp"
  "${_detours_src}/image.cpp"
  "${_detours_src}/creatwth.cpp"
  "${_detours_src}/disolx86.cpp"
  "${_detours_src}/disolx64.cpp"
  "${_detours_src}/disolia64.cpp"
  "${_detours_src}/disolarm.cpp"
  "${_detours_src}/disolarm64.cpp"
)
add_library(detours::detours ALIAS detours)

target_include_directories(detours SYSTEM PUBLIC "${_detours_src}")
target_compile_features(detours PRIVATE cxx_std_17)
# Foreign code, compiled but not maintained here.
target_compile_options(detours PRIVATE /w)
set_target_properties(detours PROPERTIES FOLDER "third_party")

message(STATUS "Detours: ${_detours_root}")
