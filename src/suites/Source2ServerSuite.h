/**
 * Exercises the game DLL's main interface, and installs this project's first
 * hooks.
 *
 * "Source2Server001" resolves to ISource2Server - which the SDK also typedefs
 * as IServerGameDLL, the name most Metamod plugins use. It is the interface the
 * engine calls *into* the game through, which makes it the natural place to
 * hook rather than to call: the interesting question is not what GameFrame
 * returns when we invoke it (we must never invoke it) but whether the engine
 * still calls it, and with what.
 *
 * Note that LevelInit/LevelShutdown are NOT here. On Source 2 those come to a
 * plugin through IMetamodListener, not through this interface.
 */

#ifndef CS2SDKTEST_SUITES_SOURCE2SERVERSUITE_H_
#define CS2SDKTEST_SUITES_SOURCE2SERVERSUITE_H_

#include "testkit/ITestSuite.h"

class ISource2Server;
class KeyValues;

namespace cs2st
{

class Source2ServerSuite : public ITestSuite
{
public:
	const char* Id() const override { return "server"; }
	const char* DisplayName() const override { return "ISource2Server (IServerGameDLL)"; }
	const char* Description() const override
	{
		return "The game DLL interface the engine drives; the main hook surface";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

	int InstallHooks(TestContext& context) override;
	void RemoveHooks(TestContext& context) override;
	bool HooksInstalled() const override { return hooks_installed_; }

	// ---- hook handlers -----------------------------------------------------
	//
	// Public because SH_MEMBER takes their addresses from outside the class.
	// Every one of these must end in RETURN_META / RETURN_META_VALUE.

	void Hook_GameFrame(bool simulating, bool first_tick, bool last_tick);
	void Hook_PreWorldUpdate(bool simulating);
	void Hook_UpdateWhenNotInGame(float frame_time);
	void Hook_ServerHibernationUpdate(bool hibernating);
	void Hook_GameServerSteamAPIActivated();
	void Hook_GameServerSteamAPIDeactivated();
	void Hook_ApplyGameSettings(KeyValues* settings);
	void Hook_ServerConVarChanged(const char* name, const char* value);
	void Hook_OnHostNameChanged(const char* hostname);
	bool Hook_ShouldTimeoutClient(int user_id, float time_since_last_received);
	bool Hook_ShouldHideFromMasterServer(bool server_has_password);

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseServerState(TestContext& context);
	void ExerciseEnumerations(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	ISource2Server* server_ = nullptr;
	bool hooks_installed_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SOURCE2SERVERSUITE_H_ */
