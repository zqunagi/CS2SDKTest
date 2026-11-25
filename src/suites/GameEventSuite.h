#ifndef CS2SDKTEST_SUITES_GAMEEVENTSUITE_H_
#define CS2SDKTEST_SUITES_GAMEEVENTSUITE_H_

#include <cstddef>
#include <string>

#include <engine2/igameeventsystem.h>

#include "testkit/ITestSuite.h"

class IGameEventManager2;

namespace cs2st
{

class GameEventSuite : public ITestSuite
{
public:
	const char* Id() const override { return "event"; }
	const char* DisplayName() const override { return "IGameEventSystem / IGameEventManager2"; }
	const char* Description() const override
	{
		return "The engine's event bus, and the legacy manager CS2 stopped exporting";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseEventSystem(TestContext& context);
	void ResolveEventMessages(TestContext& context);
	void InspectNetMessageInfo(TestContext& context);
	void ReportManagerProbe(TestContext& context);
	void ExerciseGameEventManager(TestContext& context);
	void AccountForGameEvent(TestContext& context);

	IGameEventSystem* event_system_ = nullptr;

	/**
	 * Null on every build seen so far; see the file comment. Kept as a real
	 * member rather than a constant so that the day a CS2 update starts
	 * answering one of the probed version strings, the manager section runs
	 * instead of reporting N/A.
	 */
	IGameEventManager2* manager_ = nullptr;

	/** Which version string answered, and from which module. Null/empty if none. */
	const char* manager_version_ = nullptr;
	std::string manager_module_;

	/**
	 * The registry entry for CMsgSource1LegacyGameEvent, resolved through
	 * INetworkMessages. This is what every IGameEventManager2::FireEvent ends
	 * up travelling inside, and the only handle this suite has on the
	 * descriptor struct behind a game event.
	 *
	 * These objects are the engine's per-message-type registry entries, not
	 * per-event temporaries, so they outlive the call that produced them. They
	 * are still re-vetted before use.
	 */
	NetMessageInfo_t* legacy_event_message_ = nullptr;

};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_GAMEEVENTSUITE_H_ */
