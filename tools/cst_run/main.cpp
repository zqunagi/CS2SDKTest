/**
 * cst_run.exe - the Windows counterpart of cst_run.sh.
 *
 * Starts the CS2 dedicated server with cs2sdktest.dll injected, lets the library
 * test every interface once, and exits with the server's exit status.
 *
 * Linux has LD_PRELOAD for this; Windows has nothing equivalent, so the DLL is
 * put into the process with Microsoft Detours: DetourCreateProcessWithDllEx
 * starts cs2.exe suspended, adds the DLL to the new process's import table, and
 * resumes it. The loader then maps the DLL (and runs its DllMain) before
 * cs2.exe's own entry point, which is the moment LD_PRELOAD gives you. Nothing
 * on disk is modified: cs2.exe stays as Steam left it.
 *
 * Lives next to the library, in <csgo>\bin\win64, and finds everything else from
 * there. The server's console output stays in this console, and anything you type
 * goes to the engine's console as usual.
 *
 *   cst_run.exe [options] [-- <extra cs2 arguments>]
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <TlHelp32.h>

#include <detours.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace
{

constexpr const wchar_t* kLibraryName = L"cs2sdktest.dll";

constexpr const char* kUsage =
	"Runs the CS2SDKTest sweep against this server's CS2: injects the library into\n"
	"the dedicated server, lets it test every interface once, and exits.\n"
	"\n"
	"  cst_run.exe [options] [-- <extra cs2 arguments>]\n"
	"\n"
	"Options (all optional; the default in brackets):\n"
	"  --delay N         seconds on a simulating map before the tests start  [10]\n"
	"  --suite NAME      run only this suite                                 [all]\n"
	"  --keep-running    leave the server running after the tests finish     [quit]\n"
	"  --server-dir DIR  the server directory, the one that contains game\\   [derived]\n"
	"  --map NAME        map to start on                                     [de_dust2]\n"
	"  --port N          host port                                           [10001]\n"
	"  --build-id ID     the game's Steam build id, copied into results.json [none]\n"
	"  --result-dir DIR  write results.json, ... here                        [<csgo>\\logs]\n"
	"  --                everything after this is appended to the cs2 command line\n"
	"  -h, --help        this text\n";

struct Options
{
	std::wstring delay;
	std::wstring suite;
	bool keep_running = false;
	std::wstring server_dir;
	std::wstring map = L"de_dust2";
	std::wstring port = L"10001";
	std::wstring build_id;
	std::wstring result_dir;
	std::vector<std::wstring> extra;
};

int Fail(int code, const wchar_t* format, ...)
{
	va_list args;
	va_start(args, format);
	std::fputws(L"cst_run.exe: ", stderr);
	std::vfwprintf(stderr, format, args);
	std::fputws(L"\n", stderr);
	va_end(args);
	return code;
}

bool IsNumber(const std::wstring& text)
{
	if (text.empty())
	{
		return false;
	}
	for (const wchar_t ch : text)
	{
		if (ch < L'0' || ch > L'9')
		{
			return false;
		}
	}
	return true;
}

/** Quotes one argument the way CommandLineToArgvW / the CRT will read it back. */
std::wstring QuoteArgument(const std::wstring& arg)
{
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
	{
		return arg;
	}

	std::wstring out = L"\"";
	for (std::size_t i = 0;; ++i)
	{
		std::size_t backslashes = 0;
		while (i < arg.size() && arg[i] == L'\\')
		{
			++i;
			++backslashes;
		}
		if (i == arg.size())
		{
			out.append(backslashes * 2, L'\\');
			break;
		}
		if (arg[i] == L'"')
		{
			out.append(backslashes * 2 + 1, L'\\');
			out.push_back(L'"');
		}
		else
		{
			out.append(backslashes, L'\\');
			out.push_back(arg[i]);
		}
	}
	out.push_back(L'"');
	return out;
}

std::wstring FullPath(const std::wstring& path)
{
	const DWORD needed = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
	if (needed == 0)
	{
		return std::wstring();
	}
	std::wstring out(needed, L'\0');
	const DWORD written = ::GetFullPathNameW(path.c_str(), needed, &out[0], nullptr);
	if (written == 0 || written >= needed)
	{
		return std::wstring();
	}
	out.resize(written);
	return out;
}

bool FileExists(const std::wstring& path)
{
	const DWORD attributes = ::GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path)
{
	const DWORD attributes = ::GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ParentOf(std::wstring path, int levels)
{
	while (levels-- > 0)
	{
		const std::size_t slash = path.find_last_of(L"\\/");
		if (slash == std::wstring::npos)
		{
			return std::wstring();
		}
		path.resize(slash);
	}
	return path;
}

std::wstring ThisExecutable()
{
	std::wstring path(MAX_PATH, L'\0');
	for (;;)
	{
		const DWORD length = ::GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
		if (length == 0)
		{
			return std::wstring();
		}
		if (length < path.size())
		{
			path.resize(length);
			return path;
		}
		path.resize(path.size() * 2);
	}
}

/**
 * DetourCreateProcessWithDllEx takes the DLL path as a narrow string. A path that
 * the ANSI code page cannot represent is retried as its 8.3 short name, which is
 * always ASCII; if that is not available either, there is nothing to pass.
 */
bool ToAnsiPath(const std::wstring& wide, std::string& out)
{
	auto convert = [](const std::wstring& text, std::string& result) {
		BOOL lossy = FALSE;
		const int size = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.c_str(), -1,
			nullptr, 0, nullptr, &lossy);
		if (size <= 0 || lossy)
		{
			return false;
		}
		result.assign(static_cast<std::size_t>(size), '\0');
		::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.c_str(), -1, &result[0], size,
			nullptr, &lossy);
		result.resize(static_cast<std::size_t>(size) - 1);
		return !lossy;
	};

	if (convert(wide, out))
	{
		return true;
	}

	const DWORD needed = ::GetShortPathNameW(wide.c_str(), nullptr, 0);
	if (needed == 0)
	{
		return false;
	}
	std::wstring shortened(needed, L'\0');
	const DWORD written = ::GetShortPathNameW(wide.c_str(), &shortened[0], needed);
	if (written == 0 || written >= needed)
	{
		return false;
	}
	shortened.resize(written);
	return convert(shortened, out);
}

/** Process id of a running cs2.exe that was started from `cs2_path`, or 0. */
DWORD FindRunningServer(const std::wstring& cs2_path)
{
	const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE)
	{
		return 0;
	}

	DWORD found = 0;
	PROCESSENTRY32W entry;
	entry.dwSize = sizeof(entry);
	for (BOOL ok = ::Process32FirstW(snapshot, &entry); ok && found == 0;
	     ok = ::Process32NextW(snapshot, &entry))
	{
		if (::_wcsicmp(entry.szExeFile, L"cs2.exe") != 0)
		{
			continue;
		}
		const HANDLE process =
			::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
		if (process == nullptr)
		{
			continue;
		}
		wchar_t image[MAX_PATH * 2];
		DWORD size = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
		if (::QueryFullProcessImageNameW(process, 0, image, &size) != 0 &&
		    ::_wcsicmp(image, cs2_path.c_str()) == 0)
		{
			found = entry.th32ProcessID;
		}
		::CloseHandle(process);
	}
	::CloseHandle(snapshot);
	return found;
}

BOOL WINAPI OnConsoleControl(DWORD type)
{
	// Ctrl-C goes to cs2.exe as well (same console); this process only has to
	// outlive it long enough to report how it ended.
	return type == CTRL_C_EVENT ? TRUE : FALSE;
}

bool ParseArguments(int argc, wchar_t** argv, Options& options, int& exit_code)
{
	auto need_value = [&](int i) {
		if (i + 1 >= argc || argv[i + 1][0] == L'\0')
		{
			exit_code = Fail(2, L"%ls needs a value", argv[i]);
			return false;
		}
		return true;
	};

	for (int i = 1; i < argc; ++i)
	{
		const std::wstring arg = argv[i];
		if (arg == L"--delay")             { if (!need_value(i)) return false; options.delay = argv[++i]; }
		else if (arg == L"--suite")        { if (!need_value(i)) return false; options.suite = argv[++i]; }
		else if (arg == L"--keep-running") { options.keep_running = true; }
		else if (arg == L"--server-dir")   { if (!need_value(i)) return false; options.server_dir = argv[++i]; }
		else if (arg == L"--map")          { if (!need_value(i)) return false; options.map = argv[++i]; }
		else if (arg == L"--port")         { if (!need_value(i)) return false; options.port = argv[++i]; }
		else if (arg == L"--build-id")     { if (!need_value(i)) return false; options.build_id = argv[++i]; }
		else if (arg == L"--result-dir")   { if (!need_value(i)) return false; options.result_dir = argv[++i]; }
		else if (arg == L"-h" || arg == L"--help")
		{
			std::fputs(kUsage, stdout);
			exit_code = 0;
			return false;
		}
		else if (arg == L"--")
		{
			for (++i; i < argc; ++i)
			{
				options.extra.push_back(argv[i]);
			}
		}
		else
		{
			exit_code = Fail(2, L"unknown option '%ls' (try --help)", argv[i]);
			return false;
		}
	}

	if (!options.delay.empty() && !IsNumber(options.delay))
	{
		exit_code = Fail(2, L"--delay must be a whole number of seconds");
		return false;
	}
	if (!IsNumber(options.port))
	{
		exit_code = Fail(2, L"--port must be a number");
		return false;
	}
	return true;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
	Options options;
	int exit_code = 0;
	if (!ParseArguments(argc, argv, options, exit_code))
	{
		return exit_code;
	}

	// <server>\game\csgo\bin\win64\cst_run.exe  ->  <server>
	const std::wstring here = ParentOf(ThisExecutable(), 1);
	const std::wstring library = FullPath(here + L"\\" + kLibraryName);
	if (library.empty() || !FileExists(library))
	{
		return Fail(2, L"%ls not found next to cst_run.exe; run it from the unpacked package under "
		               L"<csgo>\\bin\\win64", kLibraryName);
	}

	std::wstring server_dir = options.server_dir.empty() ? ParentOf(here, 4) : options.server_dir;
	server_dir = FullPath(server_dir);
	if (server_dir.empty() || !DirectoryExists(server_dir))
	{
		return Fail(2, L"--server-dir does not exist");
	}

	const std::wstring cs2_dir = server_dir + L"\\game\\bin\\win64";
	const std::wstring cs2_path = cs2_dir + L"\\cs2.exe";
	if (!FileExists(cs2_path))
	{
		return Fail(2, L"no cs2.exe at %ls (is %ls the directory that contains game\\?)",
			cs2_path.c_str(), server_dir.c_str());
	}

	// Never start a second server on top of one somebody is already running.
	if (const DWORD running = FindRunningServer(cs2_path))
	{
		return Fail(3, L"a cs2.exe from this server is already running (pid %lu)", running);
	}

	std::string library_ansi;
	if (!ToAnsiPath(library, library_ansi))
	{
		return Fail(2, L"cannot pass '%ls' to Detours (not representable in the ANSI code page); "
		               L"move the server to a plain-ASCII path", library.c_str());
	}

	// What the library reads (see the README). Only set what was asked for; the
	// library has its own defaults for the rest. The child inherits this process's
	// environment.
	::SetEnvironmentVariableW(L"CST_RUN", L"1");
	if (!options.delay.empty())      ::SetEnvironmentVariableW(L"CST_RUN_DELAY", options.delay.c_str());
	if (!options.suite.empty())      ::SetEnvironmentVariableW(L"CST_RUN_SUITE", options.suite.c_str());
	if (options.keep_running)        ::SetEnvironmentVariableW(L"CST_RUN_QUIT", L"0");
	if (!options.build_id.empty())   ::SetEnvironmentVariableW(L"CST_BUILD_ID", options.build_id.c_str());
	if (!options.result_dir.empty()) ::SetEnvironmentVariableW(L"CST_RESULT_DIR", options.result_dir.c_str());

	// The cs2 arguments used by default are the ones the automated runs use.
	std::wstring command_line = QuoteArgument(cs2_path);
	const std::vector<std::wstring> defaults = {
		L"-dedicated", L"+hostport", options.port, L"+game_type", L"0", L"+game_mode", L"1",
		L"+map", options.map, L"+sv_hibernate_when_empty", L"0", L"+sv_cheats", L"0",
		L"+bot_quota", L"1", L"+mp_warmup_pausetimer", L"1", L"+mp_freezetime", L"0",
		L"+mp_autoteambalance", L"0", L"+mp_limitteams", L"0", L"-insecure",
	};
	for (const std::wstring& arg : defaults)
	{
		command_line += L' ' + QuoteArgument(arg);
	}
	for (const std::wstring& arg : options.extra)
	{
		command_line += L' ' + QuoteArgument(arg);
	}

	std::wprintf(L"cst_run.exe: injecting %ls\n", library.c_str());
	std::wprintf(L"cst_run.exe: server %ls, map %ls, port %ls%ls%ls%ls%ls\n", server_dir.c_str(),
		options.map.c_str(), options.port.c_str(),
		options.delay.empty() ? L"" : L", delay ", options.delay.empty() ? L"" : options.delay.c_str(),
		options.suite.empty() ? L"" : L", suite ", options.suite.c_str());
	std::fflush(stdout);

	::SetConsoleCtrlHandler(&OnConsoleControl, TRUE);

	STARTUPINFOW startup;
	PROCESS_INFORMATION process;
	::ZeroMemory(&startup, sizeof(startup));
	::ZeroMemory(&process, sizeof(process));
	startup.cb = sizeof(startup);

	// CreateProcess may write to the command line, so it needs a mutable buffer.
	std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
	mutable_command.push_back(L'\0');

	if (!::DetourCreateProcessWithDllExW(cs2_path.c_str(), mutable_command.data(), nullptr, nullptr,
	                                     TRUE, 0, nullptr, cs2_dir.c_str(), &startup, &process,
	                                     library_ansi.c_str(), nullptr))
	{
		return Fail(1, L"DetourCreateProcessWithDllEx failed (error %lu)", ::GetLastError());
	}

	::CloseHandle(process.hThread);
	::WaitForSingleObject(process.hProcess, INFINITE);

	DWORD status = 1;
	::GetExitCodeProcess(process.hProcess, &status);
	::CloseHandle(process.hProcess);
	return static_cast<int>(status);
}
