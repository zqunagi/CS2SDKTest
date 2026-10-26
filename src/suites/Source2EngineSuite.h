/**
 * Exercises the engine interface the game DLL talks to.
 *
 * "Source2EngineToServer001" resolves to IVEngineServer2, which derives from
 * ISource2Engine, which derives from IAppSystem - so this one interface covers
 * three levels of vtable and all of it is exercised here.
 *
 * The order matters. IAppSystem's slots sit at the front of the vtable, so if
 * they are wrong every call after them is meaningless; they are called first
 * and their results sanity-checked hard. This is also where the SDK's known
 * CBaseAppSystem problem would show up - IAppSystem is 11 slots on CS2 (with
 * PreShutdown at 5 and GetBuildType at 10) and the SDK's own helper base class
 * does not implement both.
 *
 * The suite also inspects CGlobalVars, the one struct this interface hands out.
 * That is a different kind of check - the calls above test functions, while
 * CGlobalVars is raw memory read through a layout the SDK only guesses at: five
 * of its fields are m_unkNNN placeholders standing in for bytes nobody has
 * identified. Insert a field there and every offset after it silently shifts,
 * with no crash and no error, just wrong numbers.
 */

#ifndef CS2SDKTEST_SUITES_SOURCE2ENGINESUITE_H_
#define CS2SDKTEST_SUITES_SOURCE2ENGINESUITE_H_

#include "testkit/ITestSuite.h"

class IVEngineServer2;
class CGlobalVars;

namespace cs2st
{

class Source2EngineSuite : public ITestSuite
{
public:
	const char* Id() const override { return "engine"; }
	const char* DisplayName() const override { return "IVEngineServer2 / ISource2Engine"; }
	const char* Description() const override
	{
		return "The engine interface the game DLL talks to, plus its IAppSystem base";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseSource2Engine(TestContext& context);
	void ExerciseEngineState(TestContext& context);
	void ExerciseWorldGroups(TestContext& context);
	void ExercisePlayerQueries(TestContext& context);
	void ExerciseSpawnGroups(TestContext& context);
	void InspectGlobalVars(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	IVEngineServer2* engine_ = nullptr;

	/** Filled by ExerciseEngineState; player queries need maxClients from it. */
	CGlobalVars* globals_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SOURCE2ENGINESUITE_H_ */
