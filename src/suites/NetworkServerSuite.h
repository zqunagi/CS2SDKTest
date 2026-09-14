/**
 * Exercises the server-side networking services.
 *
 * "NetworkServerService_001" @ engine2 resolves to INetworkServerService, and
 * that one pointer opens four levels of vtable:
 *
 *     IAppSystem              11 slots, at the front, as everywhere
 *     IEngineService          11 more - this is the only suite that covers them
 *     INetworkServerService   the service itself
 *     CNetworkGameServerBase  handed out by GetIGameServer(), and the object
 *                             every plugin means when it says "the server"
 *
 * This suite has almost no layout work and an unusual amount of *cross-checking*
 * instead, because of what these interfaces are: they do not own facts, they
 * re-expose facts that other interfaces also expose. That is normally a reason
 * to skip something. Here it is the whole point - the map name, the tick, the
 * client limit, the player ids and the globals pointer are all reachable both
 * through this suite and through IVEngineServer2 or CGlobalVars, and two
 * unrelated interfaces disagreeing about the tick count is a finding that
 * neither interface could produce on its own.
 *
 * Fifteen such pairs are checked. The strongest of them do not go through this
 * plugin's own arithmetic at all:
 *
 *   - GetGlobals() against IVEngineServer2::GetServerGlobals() - the same
 *     singleton, fetched down two vtables that share no code.
 *   - GetTime() against GetServerTick() * tick_interval, which is a formula the
 *     SDK writes out in a comment rather than one this suite invented.
 *   - IEngineService::GetName() fed back to IEngineServiceMgr::FindService(),
 *     which has to return this very pointer.
 *
 * The one struct with a layout worth checking is netadr_t, and it is a better
 * target than it looks: its fields are public and its accessors are compiled
 * into tier0. Reading `port` by offsetof and calling GetPort() are two
 * descriptions of one byte pair that share nothing, so agreement is a
 * cross-module ABI check rather than a self-consistency check. Its IsValid()
 * and IsBaseAdrValid() even come with their rule written down ("ip & port != 0"
 * and "ip != 0"), which turns two more getters into oracles.
 *
 * ⚠️ CNetworkGameServerBase is rebuilt when the level changes. Hooks are
 * therefore installed on INetworkServerService, which is a process-lifetime
 * singleton, and not on the game server - a hook on an object the engine
 * replaces stops firing silently, which reads exactly like "the engine stopped
 * calling" and is the trap GameEntitySystemSuite already walked into. The
 * replacement is measured rather than asserted: the suite remembers the pointer
 * it saw last sweep and reports when it changes.
 */

#ifndef CS2SDKTEST_SUITES_NETWORKSERVERSUITE_H_
#define CS2SDKTEST_SUITES_NETWORKSERVERSUITE_H_

#include "testkit/ITestSuite.h"

// Deliberately no SDK headers here: every hooked method takes POD parameters
// (bool, int) or none, so the handler signatures need nothing declared. The
// SDK types live in the .cpp, where SH_DECL_HOOK needs the complete interface.
class INetworkServerService;
class CNetworkGameServerBase;
class CGlobalVars;

namespace cs2st
{

class NetworkServerSuite : public ITestSuite
{
public:
	const char* Id() const override { return "netserver"; }
	const char* DisplayName() const override
	{
		return "INetworkServerService / INetworkGameServer";
	}
	const char* Description() const override
	{
		return "The server object plugins mean by \"the server\", cross-checked "
		       "against the engine interface that reports the same facts";
	}

	bool Acquire(TestContext& context) override;
	void ExerciseCalls(TestContext& context) override;

	int InstallHooks(TestContext& context) override;
	void RemoveHooks(TestContext& context) override;
	bool HooksInstalled() const override { return hooks_installed_; }

	// ---- hook handlers -----------------------------------------------------
	//
	// Public because SH_MEMBER takes their addresses from outside the class.
	// Pre/post pairs, every one returning MRES_IGNORED.
	//
	// All six are on INetworkServerService, which lives as long as the process.
	// Between them they cover the level-load lifecycle - the thing this suite
	// can observe that a call sweep cannot, because a sweep only ever sees one
	// instant and these are transitions.

	void Pre_StartChangeLevel();
	void Post_StartChangeLevel();

	void Pre_SetGameLoadStarted();
	void Post_SetGameLoadStarted();

	void Pre_SetGameLoadFailed(bool bFailed);
	void Post_SetGameLoadFailed(bool bFailed);

	void Pre_DisconnectGameNow(int nReason);
	void Post_DisconnectGameNow(int nReason);

	void Pre_SetFinalSimulationTickThisFrame(int nTick);
	void Post_SetFinalSimulationTickThisFrame(int nTick);

	void Pre_PreserveSteamID();
	void Post_PreserveSteamID();

private:
	void ExerciseAppSystem(TestContext& context);
	void ExerciseEngineService(TestContext& context);
	void ExerciseServerService(TestContext& context);
	void ResolveGameServer(TestContext& context);
	void ExerciseGameServer(TestContext& context);
	void ExerciseGameServerBase(TestContext& context);
	void InspectNetworkAddress(TestContext& context);
	void NoteInheritanceShape(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	INetworkServerService* service_ = nullptr;

	/**
	 * Resolved inside ExerciseCalls, never in Acquire: it takes an engine call,
	 * and Acquire also runs on the hook-install path, which has no fault guard.
	 * Null whenever no level is loaded, which is a normal state and not a
	 * failure.
	 */
	CNetworkGameServerBase* game_server_ = nullptr;

	/**
	 * The game server pointer the previous sweep saw. Comparing the two is how
	 * "the engine replaces this object on level change" stops being a claim in
	 * a comment and becomes a line in the log.
	 */
	CNetworkGameServerBase* previous_game_server_ = nullptr;
	bool have_previous_game_server_ = false;

	/** From IVEngineServer2, for the cross-checks. Null if the engine moved. */
	CGlobalVars* globals_ = nullptr;

	bool hooks_installed_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_NETWORKSERVERSUITE_H_ */
