/**
 * The call sweep, and the run log around it. Shared by the `cst` commands
 * (Metamod build) and autorun, and the only part of the old command file the
 * preload build keeps.
 */

#ifndef CS2SDKTEST_PLUGIN_SWEEP_H_
#define CS2SDKTEST_PLUGIN_SWEEP_H_

#include <string>

#include "testkit/TestContext.h"

namespace cs2st
{

class TestRunner;

/** Opens a log for the duration of one command when none is already open. */
class ScopedRunLog
{
public:
	explicit ScopedRunLog(const char* prefix);
	~ScopedRunLog();

	ScopedRunLog(const ScopedRunLog&) = delete;
	ScopedRunLog& operator=(const ScopedRunLog&) = delete;

private:
	bool owned_ = false;
};

/** The header every run log starts with: version, platform, build, guard. */
void WriteLogHeader(const char* what);

/**
 * The call sweep behind `cst run`: opens a log (unless one is already open),
 * writes its header, runs the suites, prints the tally. The same entry point
 * serves the autorun path, so a CI run and a manual run produce identical
 * logs and results.json.
 *
 * `only` limits it to one suite id, empty runs everything. The caller has
 * already checked that the id exists.
 */
RunTally RunCallSweep(TestRunner& runner, const std::string& only, const char* log_prefix);

} // namespace cs2st

#endif /* CS2SDKTEST_PLUGIN_SWEEP_H_ */
