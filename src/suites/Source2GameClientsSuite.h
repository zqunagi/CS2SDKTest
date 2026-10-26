/**
 * Exercises the player lifecycle interface.
 *
 * "Source2GameClients001" resolves to ISource2GameClients (the SDK also
 * typedefs it as IServerGameClients). It is the second thing every Metamod
 * plugin hooks, after ISource2Server, and unlike that one it is dominated by
 * *arguments* rather than by the fact of being called: ClientConnect,
 * ClientPutInServer, ClientDisconnect and ClientCommand all carry player
 * identity, and the shape of what they carry has changed more than once
 * (CPlayerSlot replacing edict_t*, ENetworkDisconnectionReason arriving as a
 * protobuf enum, CBufferString* replacing char[]).
 *
 * That makes the argument snapshots worth more here than anywhere else in this
 * plugin. A hook whose signature has drifted still fires and still counts - the
 * evidence that anything is wrong is entirely in the values, so several of the
 * handlers below do not just print an argument, they check it:
 *
 *   - the disconnect reason goes through the protobuf enum's own IsValid
 *   - CCommand's argument count is bounds-checked before any argument is read
 *   - the reject-reason CBufferString is inspected as a struct, not read as a
 *     string: it is an engine-constructed instance of a type whose layout the
 *     SDK only asserts, and this is a free look at whether that layout holds
 *
 * The call side is aimed at queries and at a slot nobody is in. Notifications
 * and writers are called on that empty slot, where the game has no player to
 * act on and only its guard clauses run; the connection-lifecycle calls that
 * would create a controller (ClientConnect, ClientPutInServer, ...) stay
 * skipped. If hooks are installed, the suite's own calls show up in the dump.
 */

#ifndef CS2SDKTEST_SUITES_SOURCE2GAMECLIENTSSUITE_H_
#define CS2SDKTEST_SUITES_SOURCE2GAMECLIENTSSUITE_H_

// The other suites forward-declare their interface and keep the SDK out of the
// header. This one cannot: the hook handlers below have to spell out the exact
// parameter types, and those are CPlayerSlot, ENetworkDisconnectionReason (a
// protobuf enum), CBufferString and CCommand - which is the whole point of the
// suite. eiface.h is where that set is assembled; picking transitive headers
// out of it one by one would just be guessing at which one supplies what.
#include <engine2/isource2server.h>
#include <khook.hpp>

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
		return "Player lifecycle; the hooks whose argument signatures drift most";
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
	// Each hook fires pre and post: pre records the entry and the arguments,
	// post records that the call came back. Unequal counts mean the original
	// did not return, which is a different failure from "the engine stopped
	// calling" and only the pair can tell them apart.
	//
	// Neither side ever returns anything but Ignore. ClientConnect's return
	// value decides whether a player gets on the server; supersede it by
	// accident and this diagnostic plugin becomes an outage - Ignore discards
	// the `ret` field so the game's own answer stands.

	KHook::Return<void> Pre_OnClientConnected(ISource2GameClients*, CPlayerSlot slot, const char* name, uint64 xuid,
	                                          const char* network_id, const char* address, bool fake_player);
	KHook::Return<void> Post_OnClientConnected(ISource2GameClients*, CPlayerSlot slot, const char* name, uint64 xuid,
	                                           const char* network_id, const char* address, bool fake_player);

	KHook::Return<bool> Pre_ClientConnect(ISource2GameClients*, CPlayerSlot slot, const char* name, uint64 xuid,
	                                      const char* network_id, bool unk1, CBufferString* reject_reason);
	KHook::Return<bool> Post_ClientConnect(ISource2GameClients*, CPlayerSlot slot, const char* name, uint64 xuid,
	                                       const char* network_id, bool unk1, CBufferString* reject_reason);

	KHook::Return<void> Pre_ClientPutInServer(ISource2GameClients*, CPlayerSlot slot, const char* name, ClientPutInServerType_t type, uint64 xuid);
	KHook::Return<void> Post_ClientPutInServer(ISource2GameClients*, CPlayerSlot slot, const char* name, ClientPutInServerType_t type, uint64 xuid);

	KHook::Return<void> Pre_ClientActive(ISource2GameClients*, CPlayerSlot slot, bool load_game, const char* name, uint64 xuid);
	KHook::Return<void> Post_ClientActive(ISource2GameClients*, CPlayerSlot slot, bool load_game, const char* name, uint64 xuid);

	KHook::Return<void> Pre_ClientFullyConnect(ISource2GameClients*, CPlayerSlot slot);
	KHook::Return<void> Post_ClientFullyConnect(ISource2GameClients*, CPlayerSlot slot);

	KHook::Return<void> Pre_ClientDisconnect(ISource2GameClients*, CPlayerSlot slot, ENetworkDisconnectionReason reason,
	                                         const char* name, uint64 xuid, const char* network_id);
	KHook::Return<void> Post_ClientDisconnect(ISource2GameClients*, CPlayerSlot slot, ENetworkDisconnectionReason reason,
	                                          const char* name, uint64 xuid, const char* network_id);

	KHook::Return<void> Pre_ClientCommand(ISource2GameClients*, CPlayerSlot slot, const CCommand& args);
	KHook::Return<void> Post_ClientCommand(ISource2GameClients*, CPlayerSlot slot, const CCommand& args);

	KHook::Return<void> Pre_ClientSettingsChanged(ISource2GameClients*, CPlayerSlot slot);
	KHook::Return<void> Post_ClientSettingsChanged(ISource2GameClients*, CPlayerSlot slot);

	KHook::Return<void> Pre_ClientVoice(ISource2GameClients*, CPlayerSlot slot);
	KHook::Return<void> Post_ClientVoice(ISource2GameClients*, CPlayerSlot slot);

	KHook::Return<void> Pre_ClientCommandKeyValues(ISource2GameClients*, CPlayerSlot slot, KeyValues* kv);
	KHook::Return<void> Post_ClientCommandKeyValues(ISource2GameClients*, CPlayerSlot slot, KeyValues* kv);

	KHook::Return<void> Pre_ClientSvcUserMessage(ISource2GameClients*, CPlayerSlot slot, int type, uint32 size, const void* buffer);
	KHook::Return<void> Post_ClientSvcUserMessage(ISource2GameClients*, CPlayerSlot slot, int type, uint32 size, const void* buffer);

	// ---- the hooks themselves ------------------------------------------------
	//
	// One KHook::Virtual per virtual, bound in the constructor below; template
	// parameters are the interface, the return type, then the SDK's argument
	// list. ClientConnect returns bool, which is why its handlers return
	// KHook::Return<bool>.
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, const char*, uint64, const char*, const char*, bool> hook_OnClientConnected_;
	KHook::Virtual<ISource2GameClients, bool, CPlayerSlot, const char*, uint64, const char*, bool, CBufferString*> hook_ClientConnect_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, const char*, ClientPutInServerType_t, uint64> hook_ClientPutInServer_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, bool, const char*, uint64> hook_ClientActive_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot> hook_ClientFullyConnect_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*> hook_ClientDisconnect_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, const CCommand&> hook_ClientCommand_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot> hook_ClientSettingsChanged_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot> hook_ClientVoice_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, KeyValues*> hook_ClientCommandKeyValues_;
	KHook::Virtual<ISource2GameClients, void, CPlayerSlot, int, uint32, const void*> hook_ClientSvcUserMessage_;

	Source2GameClientsSuite();

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

	bool hooks_installed_ = false;
};

} // namespace cs2st

#endif /* CS2SDKTEST_SUITES_SOURCE2GAMECLIENTSSUITE_H_ */
