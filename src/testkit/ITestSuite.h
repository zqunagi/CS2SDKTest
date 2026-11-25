/**
 * A test suite: one group of SDK surface that gets exercised together.
 *
 * The natural unit is one engine interface - IVEngineServer2, ISchemaSystem,
 * IFileSystem - because acquiring it is a single step and everything that can
 * be called on it shares that prerequisite. A suite that cannot acquire its
 * interface reports that once and skips its calls, instead of producing a
 * screenful of identical failures.
 *
 * Note the vocabulary: a *module* in this codebase is always a loaded shared
 * library (see core/Module.h and data/GameModules.h), matching how the Source
 * engine uses the word. What a suite tests lives inside one of those.
 *
 * Lifecycle over one run:
 *
 *     Acquire(ctx)         -> false: log why, skip the rest
 *     ExerciseCalls(ctx)   -> the simulated calls, one CS2ST_CALL each
 *
 * A sweep is a one-shot thing you run and read. Nothing is hooked or left
 * installed in the game afterwards.
 */

#ifndef CS2SDKTEST_TESTKIT_ITESTSUITE_H_
#define CS2SDKTEST_TESTKIT_ITESTSUITE_H_

#include "testkit/TestContext.h"

namespace cs2st
{

class ITestSuite
{
public:
	virtual ~ITestSuite() = default;

	/** Stable short id, used in commands and log sections. */
	virtual const char* Id() const = 0;

	/** Human-readable name for the log section header. */
	virtual const char* DisplayName() const = 0;

	/** One line on what this suite covers. */
	virtual const char* Description() const { return ""; }

	/**
	 * Resolves whatever this suite needs before it can call anything.
	 * Returning false skips ExerciseCalls; log the reason via the context.
	 */
	virtual bool Acquire(TestContext& context) = 0;

	/** The simulated calls. One CS2ST_CALL per thing being exercised. */
	virtual void ExerciseCalls(TestContext& context) = 0;
};

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_ITESTSUITE_H_ */
