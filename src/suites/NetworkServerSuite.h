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
 *     SDK writes out in a comment rather than one this suite invented. (The
 *     interval side came from ISource2ServerConfig::GetTickInterval until the
 *     deployed binary removed that method; sv_tickrate is where the interval
 *     lives now, see ServerConfigSuite.)
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

#include <khook.hpp>

#include "testkit/ITestSuite.h"

// Deliberately no SDK headers beyond the forward declarations below: the
// KHook::Virtual members name INetworkServerService only as a template
// argument (never dereferenced in this header), and every hooked method takes
// POD parameters (bool, int) or none. The complete SDK types live in the .cpp.
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
	// KHook::Virtual binds these by member pointer in the constructor below,
	// so they can stay private. Pre/post pairs, every one answering
	// Action::Ignore.
	//
	// All six are on INetworkServerService, which lives as long as the process.
	// Between them they cover the level-load lifecycle - the thing this suite
	// can observe that a call sweep cannot, because a sweep only ever sees one
	// instant and these are transitions.

	KHook::Return<void> Pre_StartChangeLevel(INetworkServerService*);
	KHook::Return<void> Post_StartChangeLevel(INetworkServerService*);

	KHook::Return<void> Pre_SetGameLoadStarted(INetworkServerService*);
	KHook::Return<void> Post_SetGameLoadStarted(INetworkServerService*);

	KHook::Return<void> Pre_SetGameLoadFailed(INetworkServerService*, bool bFailed);
	KHook::Return<void> Post_SetGameLoadFailed(INetworkServerService*, bool bFailed);

	KHook::Return<void> Pre_DisconnectGameNow(INetworkServerService*, int nReason);
	KHook::Return<void> Post_DisconnectGameNow(INetworkServerService*, int nReason);

	KHook::Return<void> Pre_SetFinalSimulationTickThisFrame(INetworkServerService*, int nTick);
	KHook::Return<void> Post_SetFinalSimulationTickThisFrame(INetworkServerService*, int nTick);

	KHook::Return<void> Pre_PreserveSteamID(INetworkServerService*);
	KHook::Return<void> Post_PreserveSteamID(INetworkServerService*);

	// ---- the hooks themselves ------------------------------------------------
	//
	// One KHook::Virtual per virtual: constructor binds the handlers above,
	// InstallHooks calls Add(service_), RemoveHooks calls Remove(service_).
	// Template parameters are the interface, the return type, then the argument
	// list exactly as the SDK declares it.
	KHook::Virtual<INetworkServerService, void> hook_StartChangeLevel_;
	KHook::Virtual<INetworkServerService, void> hook_SetGameLoadStarted_;
	KHook::Virtual<INetworkServerService, void, bool> hook_SetGameLoadFailed_;
	KHook::Virtual<INetworkServerService, void, int> hook_DisconnectGameNow_;
	KHook::Virtual<INetworkServerService, void, int> hook_SetFinalSimulationTickThisFrame_;
	KHook::Virtual<INetworkServerService, void> hook_PreserveSteamID_;

	NetworkServerSuite();

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
