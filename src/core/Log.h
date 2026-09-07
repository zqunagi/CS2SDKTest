/**
 * Logging.
 *
 * Everything this plugin produces is a log line - there is no other output
 * format. Lines go to the server console through Metamod and, while a session
 * log is open, to a file as well, so a run can be read after the fact (or after
 * the call that crashed the process).
 *
 * The file is written line-by-line and flushed on every write. That is slower
 * than buffering, and it is the entire point: if a probing call takes the
 * process down, the log has to already contain the line naming that call.
 */

#ifndef CS2SDKTEST_CORE_LOG_H_
#define CS2SDKTEST_CORE_LOG_H_

#include <string>

#include "core/Platform.h"

namespace cs2st
{

enum class LogLevel
{
	kDebug,
	kInfo,
	kWarning,
	kError,
};

void SetLogLevel(LogLevel level);
LogLevel GetLogLevel();

/**
 * Opens a session log file, closing any previous one.
 * Returns false and fills `error` when the file cannot be created.
 */
bool OpenSessionLog(const std::string& path, std::string& error);
void CloseSessionLog();

/** Path of the open session log, or "" when only the console is being written. */
const std::string& SessionLogPath();

void LogMessage(LogLevel level, const char* format, ...) CS2ST_PRINTF_FMT(2, 3);

/**
 * Writes a line with no level prefix and no plugin tag.
 *
 * Result tables are aligned by column; a prefix would shift every row, so the
 * report writers use this and nothing else.
 */
void LogRaw(const char* format, ...) CS2ST_PRINTF_FMT(1, 2);

/** Writes only to the session log, never to the console. */
void LogToFile(const char* format, ...) CS2ST_PRINTF_FMT(1, 2);

} // namespace cs2st

#define CS2ST_LOG_DEBUG(...) ::cs2st::LogMessage(::cs2st::LogLevel::kDebug,   __VA_ARGS__)
#define CS2ST_LOG_INFO(...)  ::cs2st::LogMessage(::cs2st::LogLevel::kInfo,    __VA_ARGS__)
#define CS2ST_LOG_WARN(...)  ::cs2st::LogMessage(::cs2st::LogLevel::kWarning, __VA_ARGS__)
#define CS2ST_LOG_ERROR(...) ::cs2st::LogMessage(::cs2st::LogLevel::kError,   __VA_ARGS__)

#endif /* CS2SDKTEST_CORE_LOG_H_ */
