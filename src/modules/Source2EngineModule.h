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
 */

#ifndef CS2SDKTEST_MODULES_SOURCE2ENGINEMODULE_H_
#define CS2SDKTEST_MODULES_SOURCE2ENGINEMODULE_H_

#include "testkit/ITestModule.h"

class IVEngineServer2;
class CGlobalVars;

namespace cs2st
{

class Source2EngineModule : public ITestModule
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
	void ExercisePlayerQueries(TestContext& context);
	void ExerciseSpawnGroups(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	IVEngineServer2* engine_ = nullptr;

	/** Filled by ExerciseEngineState; player queries need maxClients from it. */
	CGlobalVars* globals_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_MODULES_SOURCE2ENGINEMODULE_H_ */
