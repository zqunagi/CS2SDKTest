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
	void InspectAppSystemInfo(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	IVEngineServer2* engine_ = nullptr;

	/** First entry GetDependencies returned (nullptr if none); read by InspectAppSystemInfo. */
	const void* dependencies_ = nullptr;

	/** Filled by ExerciseEngineState; player queries need maxClients from it. */
	CGlobalVars* globals_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SOURCE2ENGINESUITE_H_ */
