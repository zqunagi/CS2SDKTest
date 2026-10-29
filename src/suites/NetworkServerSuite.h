#ifndef CS2SDKTEST_SUITES_NETWORKSERVERSUITE_H_
#define CS2SDKTEST_SUITES_NETWORKSERVERSUITE_H_

#include <khook.hpp>

#include "testkit/ITestSuite.h"

#include <engine2/inetworkserverservice.h>

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
	// All five are on INetworkServerService, which lives as long as the process.
	// Between them they cover the level-load lifecycle - the thing this suite
	// can observe that a call sweep cannot, because a sweep only ever sees one
	// instant and these are transitions.

	KHook::Return<void> Pre_StartChangeLevel(INetworkServerService*, const char* pszMap,
	                                         const char* pszLandmark, void* pUnknown);
	KHook::Return<void> Post_StartChangeLevel(INetworkServerService*, const char* pszMap,
	                                          const char* pszLandmark, void* pUnknown);

	KHook::Return<void> Pre_SetGameLoadStarted(INetworkServerService*);
	KHook::Return<void> Post_SetGameLoadStarted(INetworkServerService*);

	KHook::Return<void> Pre_SetGameLoadFailed(INetworkServerService*, bool bFailed);
	KHook::Return<void> Post_SetGameLoadFailed(INetworkServerService*, bool bFailed);

	// DisconnectGameNow's reason is typed ENetworkDisconnectionReason since
	// the 2026-10 SDK (was a bare int); the handlers keep judging it through
	// the protobuf validator rather than trusting the enum's range.
	KHook::Return<void> Pre_DisconnectGameNow(INetworkServerService*, ENetworkDisconnectionReason nReason);
	KHook::Return<void> Post_DisconnectGameNow(INetworkServerService*, ENetworkDisconnectionReason nReason);

	KHook::Return<void> Pre_PreserveSteamID(INetworkServerService*);
	KHook::Return<void> Post_PreserveSteamID(INetworkServerService*);

	// ---- the hooks themselves ------------------------------------------------
	//
	// One KHook::Virtual per virtual: constructor binds the handlers above,
	// InstallHooks calls Add(service_), RemoveHooks calls Remove(service_).
	// Template parameters are the interface, the return type, then the argument
	// list exactly as the SDK declares it (StartChangeLevel grew map/landmark/
	// unknown parameters in the s2sdk rework; the old zero-arg vtable slot is
	// gone).
	KHook::Virtual<INetworkServerService, void, const char*, const char*, void*> hook_StartChangeLevel_;
	KHook::Virtual<INetworkServerService, void> hook_SetGameLoadStarted_;
	KHook::Virtual<INetworkServerService, void, bool> hook_SetGameLoadFailed_;
	KHook::Virtual<INetworkServerService, void, ENetworkDisconnectionReason> hook_DisconnectGameNow_;
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
