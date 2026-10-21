# =============================================================================
#  PluginVersion.cmake
#
#  Derives the version string baked into the binary. A snapshot is only useful
#  if you can tell which build produced it, so the git revision is part of the
#  version rather than a nice-to-have.
# =============================================================================

# The s2sdk checkout has no tags, so its "version" is the commit the submodule
# points at (short hash and commit date). A tarball without git gives "unknown".
function(cs2sdktest_resolve_sdk_revision sdk_dir out_var)
  set(_revision "unknown")

  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${sdk_dir}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" log -1 "--format=%h (%cs)"
      WORKING_DIRECTORY "${sdk_dir}"
      OUTPUT_VARIABLE _described
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _git_result
    )
    if(_git_result EQUAL 0 AND _described)
      set(_revision "${_described}")
    endif()
  endif()

  set(${out_var} "${_revision}" PARENT_SCOPE)
endfunction()

function(cs2sdktest_resolve_git_revision out_var)
  set(_revision "unknown")

  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _described
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _git_result
    )
    if(_git_result EQUAL 0 AND _described)
      set(_revision "${_described}")
    endif()

    # Re-configure when HEAD moves, otherwise the baked revision goes stale
    # without anything noticing.
    foreach(_watch "${CMAKE_SOURCE_DIR}/.git/HEAD" "${CMAKE_SOURCE_DIR}/.git/index")
      if(EXISTS "${_watch}")
        set_property(DIRECTORY "${CMAKE_SOURCE_DIR}"
          APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_watch}")
      endif()
    endforeach()
  endif()

  set(${out_var} "${_revision}" PARENT_SCOPE)
endfunction()
