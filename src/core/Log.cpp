#include "core/Log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <ISmmPlugin.h>

#include "core/FileSystemUtil.h"
#include "version_gen.h"

#if CS2ST_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
#endif

// g_SMAPI (and the KHook global) are defined once by PLUGIN_EXPOSE in
// Plugin.cpp; every other translation unit only needs the declarations.
PLUGIN_GLOBALVARS();

namespace cs2st
{
namespace
{

LogLevel g_min_level = LogLevel::kInfo;
std::FILE* g_log_file = nullptr;
std::string g_log_path;

bool g_console_color = true;
bool g_file_color = false;

constexpr const char* kColorReset = "\033[0m";

const char* ColorEscape(LogColor color)
{
	switch (color)
	{
		case LogColor::kRed:       return "\033[31m";
		case LogColor::kBrightRed: return "\033[1;31m";
		case LogColor::kYellow:    return "\033[33m";
		case LogColor::kNone:
		default:                   return nullptr;
	}
}

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

/**
 * Wraps `line` in a colour escape.
 *
 * The reset goes *before* the trailing newline: leaving it after means the
 * colour is still active while the terminal moves to the next line, which some
 * terminals render as a coloured gap and which corrupts the next line entirely
 * if the process dies in between.
 */
std::string Colorize(const char* escape, const char* line)
{
	const std::size_t length = std::strlen(line);
	std::size_t body = length;
	while (body > 0 && (line[body - 1] == '\n' || line[body - 1] == '\r'))
	{
		--body;
	}

	std::string out;
	out.reserve(length + 16);
	out.append(escape);
	out.append(line, body);
	out.append(kColorReset);
	out.append(line + body, length - body);
	return out;
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

void Emit(LogColor color, const char* prefix, const char* format, va_list args, bool to_console)
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

	const char* escape = ColorEscape(color);

	if (to_console)
	{
		if (escape != nullptr && g_console_color)
		{
			WriteToConsole(Colorize(escape, line).c_str());
		}
		else
		{
			WriteToConsole(line);
		}
	}

	if (g_log_file != nullptr)
	{
		if (escape != nullptr && g_file_color)
		{
			WriteToFile(Colorize(escape, line).c_str());
		}
		else
		{
			WriteToFile(line);
		}
	}
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

void InitLogging()
{
#if CS2ST_WINDOWS
	// Windows consoles only interpret ANSI escapes once virtual-terminal
	// processing is enabled, and older ones cannot do it at all. Ask, and fall
	// back to plain text rather than printing raw escape sequences.
	bool vt_enabled = false;

	const HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
	if (out != nullptr && out != INVALID_HANDLE_VALUE)
	{
		DWORD mode = 0;
		if (::GetConsoleMode(out, &mode) != 0)
		{
			if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
			{
				vt_enabled = true;
			}
			else if (::SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
			{
				vt_enabled = true;
			}
		}
	}

	g_console_color = vt_enabled;
#else
	g_console_color = true;
#endif

	g_file_color = false;
}

void SetColorEnabled(bool console, bool file)
{
	g_console_color = console;
	g_file_color = file;
}

bool IsConsoleColorEnabled()
{
	return g_console_color;
}

bool IsFileColorEnabled()
{
	return g_file_color;
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

	LogColor color = LogColor::kNone;
	if (level == LogLevel::kError)
	{
		color = LogColor::kRed;
	}
	else if (level == LogLevel::kWarning)
	{
		color = LogColor::kYellow;
	}

	va_list args;
	va_start(args, format);
	Emit(color, LevelPrefix(level), format, args, true);
	va_end(args);
}

void LogRaw(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	Emit(LogColor::kNone, nullptr, format, args, true);
	va_end(args);
}

void LogRawColored(LogColor color, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	Emit(color, nullptr, format, args, true);
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
	Emit(LogColor::kNone, nullptr, format, args, false);
	va_end(args);
}

} // namespace cs2st
