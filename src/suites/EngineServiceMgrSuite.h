/**
 * Exercises IEngineServiceMgr - the engine's registry of engine services and
 * loop modes, and the owner of the loop-switch / addon bookkeeping.
 *
 * "EngineServiceMgr001" @ engine2.
 *
 * The interface has no enumeration call, so "which services does the engine
 * provide" is answered by probing FindService with a list of well-known
 * service names. The found / not-found pattern is recorded, never judged: it
 * is the diff material for the next CS2 update. What is judged is that a
 * service the registry does hand out agrees with itself - GetName on the
 * object returns the name it was found under, and a name nobody registered
 * finds nothing.
 *
 * Most of the interface is client-side machinery (window, swap chain, video
 * mode, frame rendering). A dedicated server answers those with null or zero,
 * which is recorded; the ones that start, stop or register something are a
 * one-line SkipCall with the reason.
 *
 * Oracles specific to this suite:
 *
 *   - FindService(name)->GetName() against name (Fail: the registry key is
 *     the name the service reports, see NetworkServerSuite's round trip).
 *   - GetAddonCount against GetAddon(i) / IsAddonMounted / GetAddonsString:
 *     every listed addon is mounted and appears in the addon string
 *     (Suspect: the link comes from reversing, not from the SDK).
 *   - GetActiveLoopClientServerMode inside ClientServerMode_t (Fail: the
 *     enum is the SDK's declaration of the only values it can take).
 *   - GetActiveLoop null-ness against GetActiveLoopName (Suspect).
 */

#ifndef CS2SDKTEST_SUITES_ENGINESERVICEMGRSUITE_H_
#define CS2SDKTEST_SUITES_ENGINESERVICEMGRSUITE_H_

#include "testkit/ITestSuite.h"

// No SDK headers in the header: this suite installs no hooks, so nothing
// outside the .cpp needs the interface's complete type.
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
