/**
 * A test module: one group of SDK surface that gets exercised together.
 *
 * The natural unit is one engine interface - IVEngineServer2, ISchemaSystem,
 * IFileSystem - because acquiring it is a single step and everything that can
 * be called on it shares that prerequisite. A module that cannot acquire its
 * interface reports that once and skips its calls, instead of producing a
 * screenful of identical failures.
 *
 * Lifecycle over one run:
 *
 *     Acquire(ctx)         -> false: log why, skip the rest
 *     ExerciseCalls(ctx)   -> the simulated calls, one CS2ST_CALL each
 *     InstallHooks(ctx)    -> optional, stays installed after the run
 *     ...game runs, hooks record...
 *     RemoveHooks(ctx)     -> on unload or on request
 *
 * Calls and hooks are deliberately separate phases: a call sweep is a one-shot
 * thing you run and read, while hooks only tell you anything after the game has
 * been played for a while.
 */

#ifndef CS2SDKTEST_TESTKIT_ITESTMODULE_H_
#define CS2SDKTEST_TESTKIT_ITESTMODULE_H_

#include "testkit/TestContext.h"

namespace cs2st
{

class ITestModule
{
public:
	virtual ~ITestModule() = default;

	/** Stable short id, used in commands and log sections. */
	virtual const char* Id() const = 0;

	/** Human-readable name for the log section header. */
	virtual const char* DisplayName() const = 0;

	/** One line on what this module covers. */
	virtual const char* Description() const { return ""; }

	/**
	 * Resolves whatever this module needs before it can call anything.
	 * Returning false skips ExerciseCalls; log the reason via the context.
	 */
	virtual bool Acquire(TestContext& context) = 0;

	/** The simulated calls. One CS2ST_CALL per thing being exercised. */
	virtual void ExerciseCalls(TestContext& context) = 0;

	/**
	 * Installs SourceHook hooks. Returns how many were installed.
	 * Default is none, so a module that only does calls implements nothing.
	 */
	virtual int InstallHooks(TestContext& context)
	{
		(void)context;
		return 0;
	}

	/** Removes whatever InstallHooks installed. Must tolerate being called twice. */
	virtual void RemoveHooks(TestContext& context) { (void)context; }

	/** True while this module's hooks are installed. */
	virtual bool HooksInstalled() const { return false; }
};

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_ITESTMODULE_H_ */
