/**
 * What the `cst` command and the autorun path share.
 */

#ifndef CS2SDKTEST_PLUGIN_COMMANDS_H_
#define CS2SDKTEST_PLUGIN_COMMANDS_H_

#include <string>

#include "testkit/TestContext.h"

namespace cs2st
{

/**
 * The call sweep behind `cst run`: opens a log (unless one is already open),
 * writes its header, runs the suites, prints the tally. The same entry point
 * serves the autorun path, so a CI run and a manual run produce identical
 * logs and results.json.
 *
 * `only` limits it to one suite id, empty runs everything. The caller has
 * already checked that the id exists.
 */
RunTally RunCallSweep(const std::string& only, const char* log_prefix);

} // namespace cs2st

#endif /* CS2SDKTEST_PLUGIN_COMMANDS_H_ */
