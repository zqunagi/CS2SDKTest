# Run after linking cs2sdktest: fails if the object exports a symbol.
# Usage: cmake -DSO=<path to .so> -P CheckExports.cmake
if(NOT SO OR NOT EXISTS "${SO}")
  message(FATAL_ERROR "CheckExports: '${SO}' does not exist")
endif()

find_program(_NM nm)
if(NOT _NM)
  message(WARNING "nm not found; cannot verify that ${SO} exports nothing")
  return()
endif()

execute_process(
  COMMAND "${_NM}" -D --defined-only "${SO}"
  OUTPUT_VARIABLE _exports
  RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "nm -D failed on ${SO}")
endif()

string(STRIP "${_exports}" _exports)
if(_exports)
  message(FATAL_ERROR
    "${SO} exports symbols; with LD_PRELOAD these would override the engine's own:\n${_exports}")
endif()
