/**
 * Small filesystem helpers.
 *
 * Deliberately built on stdio and the raw OS calls rather than <filesystem>:
 * the plugin is compiled with _GLIBCXX_USE_CXX11_ABI=0 to match the SDK's
 * prebuilt libraries, and std::filesystem is exactly the kind of heavily
 * ABI-entangled component that makes that mismatch show up as a link error on
 * some toolchains and a crash on others.
 */

#ifndef CS2SDKTEST_CORE_FILESYSTEMUTIL_H_
#define CS2SDKTEST_CORE_FILESYSTEMUTIL_H_

#include <string>

namespace cs2st
{
namespace fs
{

/** Joins two path fragments with the platform separator. */
std::string Join(const std::string& base, const std::string& leaf);

/** Everything before the final separator, or "" when there is none. */
std::string DirectoryName(const std::string& path);

/** Everything after the final separator. */
std::string BaseName(const std::string& path);

bool Exists(const std::string& path);

/** Creates a directory and any missing parents. Returns true if it now exists. */
bool CreateDirectories(const std::string& path);

/**
 * Validates a user-supplied file name.
 *
 * The name arrives from a console command, so it is untrusted input that ends
 * up in a path: anything with a separator, a drive letter, or a ".." component
 * is rejected outright rather than normalised.
 */
bool IsSafeFileName(const std::string& name, std::string& error);

/** Appends `extension` (including the dot) when the name does not have it. */
std::string EnsureExtension(const std::string& name, const std::string& extension);

/** Absolute path of this plugin's own binary, or "" if it cannot be resolved. */
std::string PluginBinaryPath();

/**
 * Directory run logs are written to: <mod>/addons/<alias>/logs, derived from
 * the plugin's own location so it works the same on a dedicated server and a
 * local test install.
 */
std::string LogDirectory();

/** "20260907-141032", for building a default log file name. */
std::string TimestampForFileName();

/** ISO-8601 UTC, e.g. "2026-09-07T14:10:32Z", for log headers. */
std::string TimestampUtc();

} // namespace fs
} // namespace cs2st

#endif /* CS2SDKTEST_CORE_FILESYSTEMUTIL_H_ */
