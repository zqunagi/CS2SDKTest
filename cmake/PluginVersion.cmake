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

function(cs2sdktest_resolve_git_revision source_dir out_var)
  set(_revision "unknown")

  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${source_dir}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --always --tags
      WORKING_DIRECTORY "${source_dir}"
      OUTPUT_VARIABLE _described
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _git_result
    )
    if(_git_result EQUAL 0 AND _described)
      set(_revision "${_described}")

      # Not describe --dirty: it also reports a submodule whose working tree
      # differs, and s2sdk / metamod-source checked out under another
      # core.autocrlf (Windows and WSL share one tree) differ in every file.
      # --ignore-submodules=dirty still counts a submodule pointing at another
      # commit. The refresh first, so stale stat data is not read as a change.
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" update-index -q --refresh --ignore-submodules
        WORKING_DIRECTORY "${source_dir}"
        OUTPUT_QUIET ERROR_QUIET
      )
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" diff-index --quiet --ignore-submodules=dirty HEAD --
        WORKING_DIRECTORY "${source_dir}"
        OUTPUT_QUIET ERROR_QUIET
        RESULT_VARIABLE _dirty_result
      )
      if(_dirty_result EQUAL 1)
        string(APPEND _revision "-dirty")
      endif()
    endif()
  endif()

  set(${out_var} "${_revision}" PARENT_SCOPE)
endfunction()

# What the plugin reports as its version. The git description already carries
# the tag (v0.4.3, v0.4.3-2-gabc1234, ...-dirty), so showing PROJECT_VERSION next
# to it only repeats a number that is easy to leave stale. It is the fallback
# for a source tree without git.
function(cs2sdktest_full_version project_version revision out_var)
  if(revision STREQUAL "unknown")
    set(${out_var} "${project_version}" PARENT_SCOPE)
  else()
    set(${out_var} "${revision}" PARENT_SCOPE)
  endif()
endfunction()
