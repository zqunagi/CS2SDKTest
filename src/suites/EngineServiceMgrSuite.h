#ifndef CS2SDKTEST_SUITES_ENGINESERVICEMGRSUITE_H_
#define CS2SDKTEST_SUITES_ENGINESERVICEMGRSUITE_H_

#include "testkit/ITestSuite.h"

class IEngineServiceMgr;
struct ActiveLoop_t;

namespace cs2st
{

class EngineServiceMgrSuite : public ITestSuite
{
public:
	const char* Id() const override { return "engineservicemgr"; }
	const char* DisplayName() const override { return "IEngineServiceMgr"; }
	const char* Description() const override
	{
		return "The engine service / loop mode registry: active loop, "
		       "service lookup, addon list, loop-switch state";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseLoop(TestContext& context);
	void InspectActiveLoop(TestContext& context);
	void ExerciseServices(TestContext& context);
	void ExerciseAddons(TestContext& context);
	void ExerciseEngineWindow(TestContext& context);
	void ExerciseTailSlots(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	IEngineServiceMgr* manager_ = nullptr;
	// Filled by GetActiveLoop, read back by InspectActiveLoop; never freed.
	ActiveLoop_t* active_loop_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_ENGINESERVICEMGRSUITE_H_ */
