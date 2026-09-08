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
 * The call side is thin by comparison, and deliberately so. Almost every method
 * here is the engine telling the game something happened; calling those
 * ourselves would fabricate player events on a live server. Only the genuine
 * queries are called.
 */

#ifndef CS2SDKTEST_SUITES_SOURCE2GAMECLIENTSSUITE_H_
#define CS2SDKTEST_SUITES_SOURCE2GAMECLIENTSSUITE_H_

// The other suites forward-declare their interface and keep the SDK out of the
// header. This one cannot: the hook handlers below have to spell out the exact
// parameter types, and those are CPlayerSlot, ENetworkDisconnectionReason (a
// protobuf enum), CBufferString and CCommand - which is the whole point of the
// suite. eiface.h is where that set is assembled; picking transitive headers
// out of it one by one would just be guessing at which one supplies what.
#include <eiface.h>

#include "testkit/ITestSuite.h"

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
	// Public because SH_MEMBER takes their addresses from outside the class.
	// Every one must end in RETURN_META / RETURN_META_VALUE.
	//
	// Installed in pre/post pairs, as in Source2ServerSuite: pre records the
	// entry and the arguments, post records that the call came back. Unequal
	// counts mean the original did not return, which is a different failure
	// from "the engine stopped calling" and only the pair can tell them apart.
	//
	// Neither side ever returns anything but MRES_IGNORED. ClientConnect's
	// return value decides whether a player gets on the server; supersede it by
	// accident and this diagnostic plugin becomes an outage.

	void Pre_OnClientConnected(CPlayerSlot slot, const char* name, uint64 xuid,
	                           const char* network_id, const char* address, bool fake_player);
	void Post_OnClientConnected(CPlayerSlot slot, const char* name, uint64 xuid,
	                            const char* network_id, const char* address, bool fake_player);

	bool Pre_ClientConnect(CPlayerSlot slot, const char* name, uint64 xuid,
	                       const char* network_id, bool unk1, CBufferString* reject_reason);
	bool Post_ClientConnect(CPlayerSlot slot, const char* name, uint64 xuid,
	                        const char* network_id, bool unk1, CBufferString* reject_reason);

	void Pre_ClientPutInServer(CPlayerSlot slot, const char* name, int type, uint64 xuid);
	void Post_ClientPutInServer(CPlayerSlot slot, const char* name, int type, uint64 xuid);

	void Pre_ClientActive(CPlayerSlot slot, bool load_game, const char* name, uint64 xuid);
	void Post_ClientActive(CPlayerSlot slot, bool load_game, const char* name, uint64 xuid);

	void Pre_ClientFullyConnect(CPlayerSlot slot);
	void Post_ClientFullyConnect(CPlayerSlot slot);

	void Pre_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
	                          const char* name, uint64 xuid, const char* network_id);
	void Post_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason,
	                           const char* name, uint64 xuid, const char* network_id);

	void Pre_ClientCommand(CPlayerSlot slot, const CCommand& args);
	void Post_ClientCommand(CPlayerSlot slot, const CCommand& args);

	void Pre_ClientSettingsChanged(CPlayerSlot slot);
	void Post_ClientSettingsChanged(CPlayerSlot slot);

	void Pre_ClientVoice(CPlayerSlot slot);
	void Post_ClientVoice(CPlayerSlot slot);

	void Pre_ClientCommandKeyValues(CPlayerSlot slot, KeyValues* kv);
	void Post_ClientCommandKeyValues(CPlayerSlot slot, KeyValues* kv);

	void Pre_ClientSvcUserMessage(CPlayerSlot slot, int type, uint32 size, const void* buffer);
	void Post_ClientSvcUserMessage(CPlayerSlot slot, int type, uint32 size, const void* buffer);

private:
	void ExerciseAppSystem(TestContext& context);
	void ExercisePlayerQueries(TestContext& context);
	void ExerciseHltv(TestContext& context);
	void NoteSideEffectingCalls(TestContext& context);

	ISource2GameClients* clients_ = nullptr;

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
