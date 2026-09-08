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
	// Every one must end in RETURN_META / RETURN_META_VALUE.
	//
	// Each hook is installed twice, pre and post. The pre handler records the
	// entry and the arguments; the post handler records that the call came
	// back. Counts that do not match mean the original function did not
	// return - it faulted, or another plugin superseded it - and the pair is
	// what makes those two distinguishable from "the engine stopped calling".
	//
	// Neither side ever returns anything but MRES_IGNORED. A pre-hook *can*
	// suppress the original call; this plugin observes and must not.

	void Pre_GameFrame(bool simulating, bool first_tick, bool last_tick);
	void Post_GameFrame(bool simulating, bool first_tick, bool last_tick);

	void Pre_PreWorldUpdate(bool simulating);
	void Post_PreWorldUpdate(bool simulating);

	void Pre_UpdateWhenNotInGame(float frame_time);
	void Post_UpdateWhenNotInGame(float frame_time);

	void Pre_ServerHibernationUpdate(bool hibernating);
	void Post_ServerHibernationUpdate(bool hibernating);

	void Pre_GameServerSteamAPIActivated();
	void Post_GameServerSteamAPIActivated();

	void Pre_GameServerSteamAPIDeactivated();
	void Post_GameServerSteamAPIDeactivated();

	void Pre_ApplyGameSettings(KeyValues* settings);
	void Post_ApplyGameSettings(KeyValues* settings);

	void Pre_ServerConVarChanged(const char* name, const char* value);
	void Post_ServerConVarChanged(const char* name, const char* value);

	void Pre_OnHostNameChanged(const char* hostname);
	void Post_OnHostNameChanged(const char* hostname);

	bool Pre_ShouldTimeoutClient(int user_id, float time_since_last_received);
	bool Post_ShouldTimeoutClient(int user_id, float time_since_last_received);

	bool Pre_ShouldHideFromMasterServer(bool server_has_password);
	bool Post_ShouldHideFromMasterServer(bool server_has_password);

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
