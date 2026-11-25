#ifndef CS2SDKTEST_SUITES_SOURCE2GAMECLIENTSSUITE_H_
#define CS2SDKTEST_SUITES_SOURCE2GAMECLIENTSSUITE_H_

#include <engine2/isource2server.h>

#include "testkit/ITestSuite.h"

class IVEngineServer2;

namespace cs2st
{

class Source2GameClientsSuite : public ITestSuite
{
public:
	const char* Id() const override { return "clients"; }
	const char* DisplayName() const override { return "ISource2GameClients (IServerGameClients)"; }
	const char* Description() const override
	{
		return "Player lifecycle: connect, command, settings, voice and disconnect";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

private:
	void ExerciseAppSystem(TestContext& context);
	void ExercisePlayerQueries(TestContext& context);
	void ExerciseHltv(TestContext& context);
	void ExerciseEmptySlot(TestContext& context, CPlayerSlot slot);
	void ExerciseMessageCalls(TestContext& context, CPlayerSlot slot);
	void NoteSideEffectingCalls(TestContext& context);

	ISource2GameClients* clients_ = nullptr;

	/** Resolved with globals_; only used to fetch a slot's userinfo handle. */
	IVEngineServer2* engine_ = nullptr;

	/**
	 * Resolved in Acquire through IVEngineServer2, only for maxClients. Without
	 * it there is no way to know a probe slot is in range, and handing the game
	 * an out-of-range slot crashes for a reason that says nothing about the SDK.
	 */
	CGlobalVars* globals_ = nullptr;

};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SOURCE2GAMECLIENTSSUITE_H_ */
