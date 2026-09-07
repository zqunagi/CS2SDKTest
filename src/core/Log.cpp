#include "core/Log.h"

#include <cstdarg>
#include <cstdio>

#include <ISmmPlugin.h>

#include "core/FileSystemUtil.h"
#include "version_gen.h"

// g_SMAPI is defined once by PLUGIN_EXPOSE in Plugin.cpp; every other
// translation unit only needs the declarations.
PLUGIN_GLOBALVARS();

namespace cs2st
{
namespace
{

LogLevel g_min_level = LogLevel::kInfo;
std::FILE* g_log_file = nullptr;
std::string g_log_path;

const char* LevelPrefix(LogLevel level)
{
	switch (level)
	{
		case LogLevel::kDebug:   return "[" PLUGIN_LOGTAG "/dbg] ";
		case LogLevel::kWarning: return "[" PLUGIN_LOGTAG "/warn] ";
		case LogLevel::kError:   return "[" PLUGIN_LOGTAG "/ERROR] ";
		case LogLevel::kInfo:
		default:                 return "[" PLUGIN_LOGTAG "] ";
	}
}

void WriteToFile(const char* text)
{
	if (g_log_file == nullptr)
	{
		return;
	}
	std::fputs(text, g_log_file);
	// Flushed every line on purpose - see the header. A buffered log loses
	// exactly the lines that matter when a call takes the process down.
	std::fflush(g_log_file);
}

void WriteToConsole(const char* text)
{
	if (g_SMAPI != nullptr)
	{
		// The text is already formatted; passing it as "%s" stops a '%' inside
		// an interface or symbol name from being read as a conversion.
		g_SMAPI->ConPrintf("%s", text);
	}
	else
	{
		// Before Load() and after Unload() there is no Metamod to talk to.
		std::fputs(text, stdout);
		std::fflush(stdout);
	}
}

void Emit(const char* prefix, const char* format, va_list args, bool to_console)
{
	char body[4096];
	if (std::vsnprintf(body, sizeof(body), format, args) < 0)
	{
		return;
	}

	char line[4224];
	if (prefix != nullptr)
	{
		std::snprintf(line, sizeof(line), "%s%s", prefix, body);
	}
	else
	{
		std::snprintf(line, sizeof(line), "%s", body);
	}

	if (to_console)
	{
		WriteToConsole(line);
	}
	WriteToFile(line);
}

} // namespace

void SetLogLevel(LogLevel level)
{
	g_min_level = level;
}

LogLevel GetLogLevel()
{
	return g_min_level;
}

bool OpenSessionLog(const std::string& path, std::string& error)
{
	CloseSessionLog();

	const std::string directory = fs::DirectoryName(path);
	if (!directory.empty() && !fs::CreateDirectories(directory))
	{
		error = "cannot create log directory '" + directory + "'";
		return false;
	}

	g_log_file = std::fopen(path.c_str(), "wb");
	if (g_log_file == nullptr)
	{
		error = "cannot open '" + path + "' for writing";
		return false;
	}

	g_log_path = path;
	error.clear();
	return true;
}

void CloseSessionLog()
{
	if (g_log_file != nullptr)
	{
		std::fclose(g_log_file);
		g_log_file = nullptr;
	}
	g_log_path.clear();
}

const std::string& SessionLogPath()
{
	return g_log_path;
}

void LogMessage(LogLevel level, const char* format, ...)
{
	if (static_cast<int>(level) < static_cast<int>(g_min_level))
	{
		return;
	}

	va_list args;
	va_start(args, format);
	Emit(LevelPrefix(level), format, args, true);
	va_end(args);
}

void LogRaw(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	Emit(nullptr, format, args, true);
	va_end(args);
}

void LogToFile(const char* format, ...)
{
	if (g_log_file == nullptr)
	{
		return;
	}

	va_list args;
	va_start(args, format);
	Emit(nullptr, format, args, false);
	va_end(args);
}

} // namespace cs2st
