#include "plugin/Sweep.h"

#include "core/FileSystemUtil.h"
#include "core/Log.h"
#include "testkit/CallGuard.h"
#include "testkit/TestContext.h"
#include "testkit/TestRunner.h"
#include "version_gen.h"

namespace cs2st
{

ScopedRunLog::ScopedRunLog(const char* prefix)
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

ScopedRunLog::~ScopedRunLog()
{
	if (owned_)
	{
		CS2ST_LOG_INFO("log written to %s\n", SessionLogPath().c_str());
		CloseSessionLog();
	}
}

void WriteLogHeader(const char* what)
{
	LogToFile("=== %s %s ===\n", PLUGIN_DISPLAY_NAME, PLUGIN_FULL_VERSION);
	LogToFile("what      : %s\n", what);
	LogToFile("time      : %s\n", fs::TimestampUtc().c_str());
#ifdef CS2SDKTEST_PRELOAD
	LogToFile("platform  : %s, SDK '%s' (SOURCE_ENGINE=%d), preload build, no Metamod\n",
		PLUGIN_BUILD_PLATFORM, PLUGIN_SDK_ENGINE, PLUGIN_SDK_ENGINE_CODE);
#else
	LogToFile("platform  : %s, SDK '%s' (SOURCE_ENGINE=%d), Metamod PLAPI %d\n",
		PLUGIN_BUILD_PLATFORM, PLUGIN_SDK_ENGINE,
		PLUGIN_SDK_ENGINE_CODE, PLUGIN_MM_PLAPI_VERSION);
#endif
	LogToFile("build     : %s (%s)\n", PLUGIN_GIT_REVISION, PLUGIN_BUILD_TYPE);
	LogToFile("guard     : %s\n", GuardDescription());
	LogToFile("\n");
}

RunTally RunCallSweep(TestRunner& runner, const std::string& only, const char* log_prefix)
{
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

} // namespace cs2st
