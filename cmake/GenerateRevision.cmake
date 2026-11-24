# =============================================================================
#  GenerateRevision.cmake
#
#  Run with `cmake -P` on every build (see the cs2sdktest_revision target), so
#  the version a binary reports matches the tree it was compiled from without a
#  re-configure. configure_file() leaves the output alone when nothing changed,
#  so an unchanged revision recompiles nothing.
#
#  Inputs (-D): SOURCE_DIR, S2SDK_DIR, PROJECT_VERSION, TEMPLATE, OUTPUT
#
#  Optional, for the meta.json shipped in the package: META_TEMPLATE,
#  META_OUTPUT and the values that template reads (CS2SDKTEST_PROJECT_NAME,
#  CS2SDKTEST_PLATFORM, CS2SDKTEST_BUILD_TYPE, S2SDK_ENGINE_NAME).
# =============================================================================

include("${CMAKE_CURRENT_LIST_DIR}/PluginVersion.cmake")

cs2sdktest_resolve_git_revision("${SOURCE_DIR}" CS2SDKTEST_GIT_REVISION)
cs2sdktest_resolve_git_commit("${SOURCE_DIR}" CS2SDKTEST_GIT_COMMIT)
cs2sdktest_resolve_sdk_revision("${S2SDK_DIR}" CS2SDKTEST_SDK_REVISION)
cs2sdktest_full_version("${PROJECT_VERSION}" "${CS2SDKTEST_GIT_REVISION}" CS2SDKTEST_FULL_VERSION)

configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)

if(META_TEMPLATE AND META_OUTPUT)
  configure_file("${META_TEMPLATE}" "${META_OUTPUT}" @ONLY)
endif()
