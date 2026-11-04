#ifndef CS2SDKTEST_SUITES_LOOPMODESUITE_H_
#define CS2SDKTEST_SUITES_LOOPMODESUITE_H_

#include <cstddef>

#include "testkit/ITestSuite.h"

class IEngineServiceMgr;
class ILoopMode;
class ILoopModeFactory;

namespace cs2st
{

/**
 * ILoopMode / ILoopModeFactory.
 *
 * Neither interface is reachable through QueryInterface: the game DLL
 * implements them and hands them to IEngineServiceMgr::RegisterLoopMode once,
 * during startup, and the registry has no getter. The only copy of the
 * (name, factory, global-pointer) triples that outlives startup is the singly
 * linked list the server module iterates in CLoopModeRegistry::RegisterLoopModes.
 *
 * So the suite finds that list: it scans the server module for the registry
 * function (Linux only), decodes the RIP-relative address of the list head from
 * the `mov rbx, [rip+disp]` inside it, and walks the nodes (bounded). Each
 * node is { next, name, factory, ppGlobalPointer }; *ppGlobalPointer is the
 * live ILoopMode while that loop is the active one.
 *
 * Oracles:
 *  - when the registration has no global pointer, the instance comes from the
 *    engine's own loop object, checked by its factory matching the registration;
 *  - the registered name for the active loop equals
 *    IEngineServiceMgr::GetActiveLoopName, and only that node has a live
 *    ILoopMode (Suspect);
 *  - GetLoopModeType is inside LoopModeType_t (Fail);
 *  - the vtable slots line up: HandleInputEvent ignores an empty event, and
 *    AddViewsToSceneSystem hands back the buffer it was given.
 *
 * The node layout and the signature are reverse-engineered from the 2026-11
 * Linux libserver.so, not from the SDK; if they stop matching, the suite
 * reports that and calls nothing.
 */
class LoopModeSuite : public ITestSuite
{
public:
	const char* Id() const override { return "loopmode"; }
	const char* DisplayName() const override { return "ILoopMode"; }
	const char* Description() const override
	{
		return "ILoopMode / ILoopModeFactory reached through the server module's "
		       "loop mode registration list";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	// One registration, as the engine side of RegisterLoopMode receives it.
	struct Registration
	{
		const char* name = nullptr;
		ILoopModeFactory* factory = nullptr;
		ILoopMode** global_pointer = nullptr;
	};

	static constexpr int kMaxRegistrations = 16;

	bool FindRegistrations(TestContext& context);
	// Reads the engine's own loop object for `registration`; null when not found.
	ILoopMode* FindEngineInstance(TestContext& context, const Registration& registration);
	void ExerciseFactory(TestContext& context, const Registration& registration);
	void ExerciseLoopMode(TestContext& context, const Registration& registration,
		ILoopMode* loop_mode);

	IEngineServiceMgr* manager_ = nullptr;
	Registration registrations_[kMaxRegistrations];
	int registration_count_ = 0;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_LOOPMODESUITE_H_ */
