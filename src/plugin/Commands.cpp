/**
 * The `cst` console command.
 *
 * Registered without FCVAR_CLIENT_CAN_EXECUTE on purpose: these subcommands
 * call into engine interfaces, install hooks, and write files. That is
 * server-operator territory, not something a connected player should be able
 * to trigger.
 */

#include <string>

#include <ISmmPlugin.h>
#include <tier0/convar.h>

#include "core/FileSystemUtil.h"
#include "core/Log.h"
#include "core/Module.h"
#include "data/GameModules.h"
#include "hook/HookRecorder.h"
#include "plugin/Commands.h"
#include "plugin/Plugin.h"
#include "testkit/CallGuard.h"
#include "testkit/TestContext.h"
#include "testkit/TestRunner.h"
#include "version_gen.h"

namespace cs2st
{
namespace
{

std::string ArgAt(const CCommand& args, int index)
{
	if (index < 0 || index >= args.ArgC())
	{
		return std::string();
	}
	const char* value = args.Arg(index);
	return (value != nullptr) ? value : "";
}

bool IsEqual(const std::string& lhs, const char* rhs)
{
	return lhs == rhs;
}

void PrintUsage()
{
	LogRaw("\n%s %s - %s\n\n", PLUGIN_DISPLAY_NAME, PLUGIN_FULL_VERSION, PLUGIN_DESCRIPTION);
	LogRaw("  cst help                    this text\n");
	LogRaw("  cst suites                  test suites this build ships\n");
	LogRaw("  cst targets                 game modules (shared libraries) and their state\n");
	LogRaw("  cst run [suite]             call sweep; writes a log\n");
	LogRaw("  cst hooks install [suite]   install hooks and leave them running\n");
	LogRaw("  cst hooks remove [suite]    take them back out\n");
	LogRaw("  cst hooks dump              what has fired so far\n");
	LogRaw("  cst hooks reset             zero the counters, keep the hooks\n");
	LogRaw("  cst log open [name]         start writing everything to a file\n");
	LogRaw("  cst log close               stop\n");
	LogRaw("  cst log status              where the log is going\n");
	LogRaw("  cst log color <on|off|console>\n");
	LogRaw("                                    ANSI colour; 'console' keeps it out of files\n");
	LogRaw("\n  logs are written to %s\n\n", fs::LogDirectory().c_str());
}

void PrintSuites()
{
	const TestRunner& runner = g_Plugin.tests();

	if (runner.size() == 0)
	{
		LogRaw("\n  no test suites registered in this build.\n");
		LogRaw("  add one in TestRunner::CreateDefault() - see src/testkit/ITestSuite.h\n\n");
		return;
	}

	LogRaw("\n  %zu test suite(s):\n", runner.size());
	for (const auto& suite : runner.suites())
	{
		LogRaw("    %-16s %-6s %s\n",
			suite->Id(),
			suite->HooksInstalled() ? "hooked" : "",
			suite->Description());
	}
	LogRaw("\n");
}

void PrintTargets()
{
	std::size_t count = 0;
	const GameModuleDesc* table = GetGameModules(count);

	TestContext context;

	LogRaw("\n  %-24s %-28s %-9s %s\n", "ID", "FILE", "STATE", "NOTE");
	std::size_t loaded = 0;

	for (std::size_t i = 0; i < count; ++i)
	{
		const GameModuleDesc& desc = table[i];
		const Module& module = context.GetModule(desc.id);

		const char* state = "-";
		if (module.IsValid())
		{
			++loaded;
			// A module with no CreateInterface is still loaded, it just has
			// nothing to hand out; worth distinguishing from plain absence.
			state = (module.factory() != nullptr) ? "loaded" : "no-iface";
		}

		LogRaw("  %-24s %-28s %-9s %s\n",
			desc.id, PlatformModuleName(desc).c_str(), state, desc.note);
	}

	LogRaw("\n  %zu/%zu loaded\n\n", loaded, count);
}

/** Opens a log for the duration of one command when none is already open. */
class ScopedRunLog
{
public:
	explicit ScopedRunLog(const char* prefix)
	{
		if (!SessionLogPath().empty())
		{
			// A session log is already open; write into that instead of
			// interrupting it, and leave it open afterwards.
			return;
		}

		const std::string name =
			std::string(prefix) + "-" + fs::TimestampForFileName() + ".log";
		const std::string path = fs::Join(fs::LogDirectory(), name);

		std::string error;
		if (OpenSessionLog(path, error))
		{
			owned_ = true;
		}
		else
		{
			CS2ST_LOG_WARN("could not open log file: %s (console only)\n", error.c_str());
		}
	}

	~ScopedRunLog()
	{
		if (owned_)
		{
			CS2ST_LOG_INFO("log written to %s\n", SessionLogPath().c_str());
			CloseSessionLog();
		}
	}

	ScopedRunLog(const ScopedRunLog&) = delete;
	ScopedRunLog& operator=(const ScopedRunLog&) = delete;

private:
	bool owned_ = false;
};

void WriteLogHeader(const char* what)
{
	LogToFile("=== %s %s ===\n", PLUGIN_DISPLAY_NAME, PLUGIN_FULL_VERSION);
	LogToFile("what      : %s\n", what);
	LogToFile("time      : %s\n", fs::TimestampUtc().c_str());
	LogToFile("platform  : %s, SDK '%s' (SOURCE_ENGINE=%d), Metamod PLAPI %d\n",
		PLUGIN_BUILD_PLATFORM, PLUGIN_SDK_ENGINE,
		PLUGIN_SDK_ENGINE_CODE, PLUGIN_MM_PLAPI_VERSION);
	LogToFile("build     : %s (%s, %s)\n",
		PLUGIN_VERSION, PLUGIN_GIT_REVISION, PLUGIN_BUILD_TYPE);
	LogToFile("guard     : %s\n", GuardDescription());
	LogToFile("\n");
}

void CommandRun(const CCommand& args)
{
	TestRunner& runner = g_Plugin.tests();

	const std::string only = ArgAt(args, 2);
	if (!only.empty() && runner.Find(only) == nullptr)
	{
		CS2ST_LOG_ERROR("no test suite named '%s'; try 'cst suites'\n", only.c_str());
		return;
	}

	if (runner.size() == 0)
	{
		CS2ST_LOG_WARN("no test suites registered in this build; nothing to run\n");
		return;
	}

	RunCallSweep(only, "run");
}

void CommandHooks(const CCommand& args)
{
	const std::string action = ArgAt(args, 2);
	const std::string only = ArgAt(args, 3);

	TestRunner& runner = g_Plugin.tests();
	TestContext context;

	if (IsEqual(action, "install"))
	{
		if (!only.empty() && runner.Find(only) == nullptr)
		{
			CS2ST_LOG_ERROR("no test suite named '%s'\n", only.c_str());
			return;
		}

		// Entry/exit tracing goes to the session log and nowhere else, so
		// without one the most valuable thing hooks produce - the last line
		// before a fault - is thrown away. Open one rather than let that
		// happen silently.
		if (SessionLogPath().empty())
		{
			const std::string path = fs::Join(fs::LogDirectory(),
				"hooks-" + fs::TimestampForFileName() + ".log");
			std::string error;
			if (OpenSessionLog(path, error))
			{
				WriteLogHeader("hook session");
				CS2ST_LOG_INFO("tracing to %s\n", path.c_str());
			}
			else
			{
				CS2ST_LOG_WARN("no log file (%s); entry/exit tracing will be lost\n",
					error.c_str());
			}
		}

		const int installed = runner.InstallHooks(context, only);
		CS2ST_LOG_INFO("%d hook(s) installed; play a round, then 'cst hooks dump'\n",
			installed);
	}
	else if (IsEqual(action, "remove"))
	{
		const int touched = runner.RemoveHooks(context, only);
		CS2ST_LOG_INFO("hooks removed from %d suite(s)\n", touched);
	}
	else if (IsEqual(action, "reset"))
	{
		HookRecorder::Get().ResetCounts();
		CS2ST_LOG_INFO("hook counters zeroed\n");
	}
	else if (action.empty() || IsEqual(action, "dump"))
	{
		ScopedRunLog log("hooks");
		WriteLogHeader("hook dump");

		HookRecorder& recorder = HookRecorder::Get();
		LogRaw("\n");
		recorder.Dump();
		LogRaw("\n  %zu of %zu declared hook(s) have fired\n",
			recorder.fired(), recorder.declared());

		const std::size_t unbalanced = recorder.unbalanced();
		if (unbalanced > 0)
		{
			// Entered more often than returned. Either the original function
			// faulted, or another plugin superseded the call so the post-hook
			// chain never ran - both worth chasing, and neither visible from
			// a single count.
			CS2ST_LOG_WARN("%zu hook(s) entered more often than they returned\n",
				unbalanced);
		}
		LogRaw("\n");
	}
	else
	{
		CS2ST_LOG_ERROR("unknown hooks action '%s'; try 'cst help'\n", action.c_str());
	}
}

void CommandLog(const CCommand& args)
{
	const std::string action = ArgAt(args, 2);

	if (IsEqual(action, "open"))
	{
		std::string name = ArgAt(args, 3);
		if (name.empty())
		{
			name = "session-" + fs::TimestampForFileName() + ".log";
		}
		else
		{
			std::string error;
			if (!fs::IsSafeFileName(name, error))
			{
				CS2ST_LOG_ERROR("invalid log name: %s\n", error.c_str());
				return;
			}
			name = fs::EnsureExtension(name, ".log");
		}

		const std::string path = fs::Join(fs::LogDirectory(), name);
		std::string error;
		if (!OpenSessionLog(path, error))
		{
			CS2ST_LOG_ERROR("%s\n", error.c_str());
			return;
		}

		WriteLogHeader("session log");
		CS2ST_LOG_INFO("logging to %s\n", path.c_str());
	}
	else if (IsEqual(action, "close"))
	{
		if (SessionLogPath().empty())
		{
			CS2ST_LOG_INFO("no session log is open\n");
			return;
		}
		CS2ST_LOG_INFO("closing %s\n", SessionLogPath().c_str());
		CloseSessionLog();
	}
	else if (IsEqual(action, "color") || IsEqual(action, "colour"))
	{
		const std::string value = ArgAt(args, 3);
		if (IsEqual(value, "on"))
		{
			SetColorEnabled(true, true);
		}
		else if (IsEqual(value, "off"))
		{
			SetColorEnabled(false, false);
		}
		else if (IsEqual(value, "console"))
		{
			// Escape sequences in the file are fine under `less -R` and awful
			// in an editor that does not interpret them; this is the middle.
			SetColorEnabled(true, false);
		}
		else
		{
			CS2ST_LOG_ERROR("usage: cst log color <on|off|console>\n");
			return;
		}
		CS2ST_LOG_INFO("colour: console %s, file %s\n",
			IsConsoleColorEnabled() ? "on" : "off",
			IsFileColorEnabled() ? "on" : "off");
	}
	else if (action.empty() || IsEqual(action, "status"))
	{
		if (SessionLogPath().empty())
		{
			LogRaw("\n  no session log open; 'run' and 'hooks dump' write their own.\n");
			LogRaw("  log directory: %s\n", fs::LogDirectory().c_str());
		}
		else
		{
			LogRaw("\n  session log: %s\n", SessionLogPath().c_str());
		}
		LogRaw("  colour     : console %s, file %s\n\n",
			IsConsoleColorEnabled() ? "on" : "off",
			IsFileColorEnabled() ? "on" : "off");
	}
	else
	{
		CS2ST_LOG_ERROR("unknown log action '%s'; try 'cst help'\n", action.c_str());
	}
}

void Dispatch(const CCommand& args)
{
	if (!g_Plugin.ready())
	{
		CS2ST_LOG_ERROR("plugin is not initialised yet\n");
		return;
	}

	const std::string command = ArgAt(args, 1);

	if (command.empty() || IsEqual(command, "help"))
	{
		PrintUsage();
	}
	else if (IsEqual(command, "suites"))
	{
		PrintSuites();
	}
	else if (IsEqual(command, "targets"))
	{
		PrintTargets();
	}
	else if (IsEqual(command, "run"))
	{
		CommandRun(args);
	}
	else if (IsEqual(command, "hooks"))
	{
		CommandHooks(args);
	}
	else if (IsEqual(command, "log"))
	{
		CommandLog(args);
	}
	else
	{
		CS2ST_LOG_ERROR("unknown command '%s'; try 'cst help'\n", command.c_str());
	}
}

} // namespace
} // namespace cs2st

CON_COMMAND_F(cst,
	"CS2 SDK exercise harness: call and hook engine interfaces, log what happens.",
	FCVAR_RELEASE | FCVAR_GAMEDLL)
{
	(void)context;
	cs2st::Dispatch(args);
}

namespace cs2st
{

RunTally RunCallSweep(const std::string& only, const char* log_prefix)
{
	TestRunner& runner = g_Plugin.tests();

	const std::string what =
		only.empty() ? std::string("call sweep (all suites)") : "call sweep: " + only;

	ScopedRunLog log(log_prefix);
	WriteLogHeader(what.c_str());

	TestContext context;
	const RunTally tally = runner.RunCalls(context, only);

	LogRaw("\n  %zu call(s): %zu ok, %zu crashed, %zu failed, %zu suspect, "
	       "%zu unavailable, %zu skipped\n\n",
		tally.Total(), tally.ok, tally.crashed, tally.failed, tally.suspect,
		tally.unavailable, tally.skipped);
	return tally;
}

void UnregisterCommands()
{
	if (g_SMAPI == nullptr || g_PLAPI == nullptr)
	{
		return;
	}

	// Not META_UNREGCMD: that macro passes the ConCommand by value, while
	// ISmmAPI::UnregisterConCommand takes a ConCommandRef*. ConCommand derives
	// from ConCommandRef, so taking the address here is the registration the
	// macro was written for, minus the stale signature.
	//
	// cst_command is the static object CON_COMMAND_F declared above,
	// which is why this has to live in this file.
	g_SMAPI->UnregisterConCommand(g_PLAPI, &cst_command);
}

} // namespace cs2st
