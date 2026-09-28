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

#include <khook.hpp>

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
	// KHook::Virtual binds these by member pointer in the constructor, so they
	// can stay private. Every one answers Action::Ignore.
	//
	// Each hook fires pre and post. The pre handler records the entry and the
	// arguments; the post handler records that the call came back. Counts that
	// do not match mean the original function did not return - it faulted, or
	// another plugin superseded it - and the pair is what makes those two
	// distinguishable from "the engine stopped calling".
	//
	// Neither side ever returns anything but Ignore. A pre-hook *can* suppress
	// the original call; this plugin observes and must not. For the two hooks
	// with a bool return, Ignore discards this plugin's `ret` field and lets
	// the game's own result stand - which is what makes this observation
	// rather than interference.

	KHook::Return<void> Pre_GameFrame(ISource2Server*, bool simulating, bool first_tick, bool last_tick);
	KHook::Return<void> Post_GameFrame(ISource2Server*, bool simulating, bool first_tick, bool last_tick);

	KHook::Return<void> Pre_PreWorldUpdate(ISource2Server*, bool simulating);
	KHook::Return<void> Post_PreWorldUpdate(ISource2Server*, bool simulating);

	KHook::Return<void> Pre_UpdateWhenNotInGame(ISource2Server*, float frame_time);
	KHook::Return<void> Post_UpdateWhenNotInGame(ISource2Server*, float frame_time);

	KHook::Return<void> Pre_ServerHibernationUpdate(ISource2Server*, bool hibernating);
	KHook::Return<void> Post_ServerHibernationUpdate(ISource2Server*, bool hibernating);

	KHook::Return<void> Pre_GameServerSteamAPIActivated(ISource2Server*);
	KHook::Return<void> Post_GameServerSteamAPIActivated(ISource2Server*);

	KHook::Return<void> Pre_GameServerSteamAPIDeactivated(ISource2Server*);
	KHook::Return<void> Post_GameServerSteamAPIDeactivated(ISource2Server*);

	KHook::Return<void> Pre_ApplyGameSettings(ISource2Server*, KeyValues* settings);
	KHook::Return<void> Post_ApplyGameSettings(ISource2Server*, KeyValues* settings);

	KHook::Return<void> Pre_ServerConVarChanged(ISource2Server*, const char* name, const char* value);
	KHook::Return<void> Post_ServerConVarChanged(ISource2Server*, const char* name, const char* value);

	KHook::Return<void> Pre_OnHostNameChanged(ISource2Server*, const char* hostname);
	KHook::Return<void> Post_OnHostNameChanged(ISource2Server*, const char* hostname);

	KHook::Return<bool> Pre_ShouldTimeoutClient(ISource2Server*, int user_id, float time_since_last_received);
	KHook::Return<bool> Post_ShouldTimeoutClient(ISource2Server*, int user_id, float time_since_last_received);

	KHook::Return<bool> Pre_ShouldHideFromMasterServer(ISource2Server*, bool server_has_password);
	KHook::Return<bool> Post_ShouldHideFromMasterServer(ISource2Server*, bool server_has_password);

	// ---- the hooks themselves ------------------------------------------------
	//
	// One KHook::Virtual per virtual, bound in the constructor below; template
	// parameters are the interface, the return type, then the SDK's argument
	// list. Two of them return bool, which is why their handlers return
	// KHook::Return<bool>.
	KHook::Virtual<ISource2Server, void, bool, bool, bool> hook_GameFrame_;
	KHook::Virtual<ISource2Server, void, bool> hook_PreWorldUpdate_;
	KHook::Virtual<ISource2Server, void, float> hook_UpdateWhenNotInGame_;
	KHook::Virtual<ISource2Server, void, bool> hook_ServerHibernationUpdate_;
	KHook::Virtual<ISource2Server, void> hook_GameServerSteamAPIActivated_;
	KHook::Virtual<ISource2Server, void> hook_GameServerSteamAPIDeactivated_;
	KHook::Virtual<ISource2Server, void, KeyValues*> hook_ApplyGameSettings_;
	KHook::Virtual<ISource2Server, void, const char*, const char*> hook_ServerConVarChanged_;
	KHook::Virtual<ISource2Server, void, const char*> hook_OnHostNameChanged_;
	KHook::Virtual<ISource2Server, bool, int, float> hook_ShouldTimeoutClient_;
	KHook::Virtual<ISource2Server, bool, bool> hook_ShouldHideFromMasterServer_;

	Source2ServerSuite();

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
