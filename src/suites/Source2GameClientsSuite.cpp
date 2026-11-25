#include "suites/Source2GameClientsSuite.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <appframework/iappsystem.h>
#include <engine2/isource2server.h>
#include <engine2/ivengineserver2.h>
#include <netmessages.pb.h>
#include <networksystem/inetworkmessages.h>
#include <scenesystem/iscenesystem.h>
#include <tier0/buffer_string.h>

#include "testkit/NetworkMessageLookup.h"

namespace cs2st
{
namespace
{

/**
 * Slot the read-only queries are aimed at.
 *
 * It need not be occupied. These are all reads, and an empty slot's answer
 * probes the call just as well as a full one's - what is under test is that the
 * call returns at all and returns something of the right shape.
 */
constexpr int kProbeSlot = 0;

/** Number of set bits in a block of memory, read through its raw bytes. */
int CountSetBits(const void* data, size_t size)
{
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	int count = 0;
	for (size_t i = 0; i < size; ++i)
	{
		for (int bit = 0; bit < 8; ++bit)
		{
			count += (bytes[i] >> bit) & 1;
		}
	}
	return count;
}

const char* TierName(AppSystemTier_t tier)
{
	switch (tier)
	{
		case APP_SYSTEM_TIER0:      return "TIER0";
		case APP_SYSTEM_TIER1:      return "TIER1";
		case APP_SYSTEM_TIER2:      return "TIER2";
		case APP_SYSTEM_TIER3:      return "TIER3";
		case APP_SYSTEM_TIER4:      return "TIER4";
		case APP_SYSTEM_TIER_OTHER: return "TIER_OTHER";
		default:                    return "<out of range>";
	}
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool Source2GameClientsSuite::Acquire(TestContext& context)
{
	clients_ = static_cast<ISource2GameClients*>(
		context.QueryInterface("server", INTERFACEVERSION_SERVERGAMECLIENTS));

	if (clients_ == nullptr)
	{
		const Module& server_module = context.GetModule("server");
		if (!server_module.IsValid())
		{
			context.UnavailableCall("ISource2GameClients", "the server module is not loaded");
		}
		else
		{
			context.UnavailableCall("ISource2GameClients",
				"server is loaded but does not answer '%s'",
				INTERFACEVERSION_SERVERGAMECLIENTS);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(clients_),
		INTERFACEVERSION_SERVERGAMECLIENTS);
	return true;
}

void Source2GameClientsSuite::ExerciseCalls(TestContext& context)
{
	globals_ = nullptr;
	engine_ = nullptr;

	ExerciseAppSystem(context);
	ExercisePlayerQueries(context);
	ExerciseHltv(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void Source2GameClientsSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = clients_->GetTier();
		// Compared as int: the enum's underlying type may be unsigned, which
		// would make a "< 0" check on the enum itself vacuously false.
		const int tier_value = static_cast<int>(tier);
		context.Detail("-> %s (%d)", TierName(tier), tier_value);
		if (tier_value < 0 || tier_value > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside the enum; slot 7 is probably not GetTier",
				tier_value);
		}
	});

	CS2ST_CALL(context, "IAppSystem::IsSingleton", {
		const bool singleton = clients_->IsSingleton();
		context.Detail("-> %s", singleton ? "true" : "false");
		if (!singleton)
		{
			context.Fail("the game clients interface is a singleton; "
			             "false here suggests a slot shift");
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = clients_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = clients_->GetDependencies();
		if (deps == nullptr)
		{
			context.Detail("-> none");
			return;
		}

		// Capped: walking a garbage pointer until it happens to hit a null is
		// how a probe turns into a hang instead of a crash.
		int count = 0;
		while (count < 64 && deps[count].m_pModuleName != nullptr)
		{
			++count;
		}
		context.Detail("-> %p, %d entr%s%s", static_cast<const void*>(deps),
			count, count == 1 ? "y" : "ies", count == 64 ? " (capped)" : "");
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface", {
		void* self = clients_->QueryInterface(INTERFACEVERSION_SERVERGAMECLIENTS);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(clients_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run game DLL startup");
	context.SkipCall("IAppSystem::Init", "would re-run game DLL startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin game DLL shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the game DLL down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down game DLL interfaces");
}

// -----------------------------------------------------------------------------
//  The genuine queries
//
//  Everything in this interface that asks rather than tells. There are not many
//  - it is an interface the engine calls into the game through - but they are
//  the only part that can be exercised without inventing a player event.
// -----------------------------------------------------------------------------

void Source2GameClientsSuite::ExercisePlayerQueries(TestContext& context)
{
	context.Note("[ISource2GameClients] player queries (slot %d)", kProbeSlot);

	// Only for maxClients, so that a probe slot can be known to be in range -
	// handing the game an out-of-range slot crashes for a reason that says
	// nothing about the SDK. It is fetched here rather than in Acquire so that
	// the engine call runs inside the fault guard: an unguarded one in Acquire
	// would take the server down with no line in the log.
	CS2ST_CALL(context, "IVEngineServer2::GetServerGlobals (for maxClients)", {
		IVEngineServer2* engine = static_cast<IVEngineServer2*>(
			context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));
		if (engine == nullptr)
		{
			context.Detail("-> engine2 does not answer '%s'", INTERFACEVERSION_VENGINESERVER);
			return;
		}
		engine_ = engine;
		globals_ = engine->GetServerGlobals();
		context.Detail("-> %p, maxClients %d", static_cast<void*>(globals_),
			globals_ != nullptr ? globals_->maxClients : -1);
	});

	if (globals_ == nullptr)
	{
		context.SkipCall("ISource2GameClients::<player queries>",
			"could not reach CGlobalVars through the engine; slot range unknown");
		return;
	}
	if (globals_->maxClients <= kProbeSlot)
	{
		context.SkipCall("ISource2GameClients::<player queries>",
			"maxClients is %d; slot %d is out of range", globals_->maxClients, kProbeSlot);
		return;
	}

	const CPlayerSlot slot(kProbeSlot);

	// Whether the slot is occupied decides how the rest should read, so it goes
	// first and its answer is carried into the notes below.
	bool occupied = false;

	CS2ST_CALL(context, "ISource2GameClients::IsPlayerSlotOccupied", {
		occupied = clients_->IsPlayerSlotOccupied(slot);
		context.Detail("-> %s", occupied ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2GameClients::IsPlayerAlive", {
		const bool alive = clients_->IsPlayerAlive(slot);
		context.Detail("-> %s", alive ? "true" : "false");
		if (alive && !occupied)
		{
			// Not a crash and not obviously wrong-looking, which is exactly why
			// it is worth asserting: an empty slot holding a live player is the
			// signature of two queries that no longer index the same thing.
			context.Fail("slot %d is unoccupied but reports a live player", kProbeSlot);
		}
	});

	CS2ST_CALL(context, "ISource2GameClients::GetPlayerScore", {
		context.Detail("-> %d", clients_->GetPlayerScore(slot));
	});

	CS2ST_CALL(context, "ISource2GameClients::GetPlayerTickBase", {
		// uint32 since the 2026-10 SDK; the old int return made a negative
		// tick base a Fail, but an unsigned one cannot be negative - so the
		// row now records, and the tickcount beside it stays the oracle.
		const uint32 tick_base = clients_->GetPlayerTickBase(slot);
		context.Detail("-> %u (tickcount %d)", tick_base, globals_->tickcount);
	});

	CS2ST_CALL(context, "ISource2GameClients::ClientCanPause", {
		context.Detail("-> %s", clients_->ClientCanPause(slot) ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2GameClients::ClientEarPosition", {
		// Seeded with a value the game would have to overwrite, so "did not
		// touch it" is distinguishable from "wrote a zero".
		Vector ear(-31337.0f, -31337.0f, -31337.0f);
		clients_->ClientEarPosition(slot, &ear);
		context.Detail("-> (%.2f, %.2f, %.2f)",
			static_cast<double>(ear.x), static_cast<double>(ear.y),
			static_cast<double>(ear.z));

		// A float has 2^32 bit patterns and only some of them are usable
		// coordinates - the same reasoning StructInspector applies to struct
		// fields, borrowed here because an out parameter has the same property.
		if (std::isnan(ear.x) || std::isnan(ear.y) || std::isnan(ear.z)
			|| std::isinf(ear.x) || std::isinf(ear.y) || std::isinf(ear.z))
		{
			context.Fail("not a finite position");
		}
	});

	CS2ST_CALL(context, "ISource2GameClients::GetPlayerHltvDelay", {
		// int seconds since the 2026-10 SDK (was float); the replay entity
		// out-parameter is unchanged, so the row keeps both halves.
		CEntityIndex replay_entity(-1);
		const int delay = clients_->GetPlayerHltvDelay(slot, replay_entity);
		context.Detail("-> %d s, replay entity %d", delay, replay_entity.Get());
		if (delay < 0)
		{
			context.Fail("a delay cannot be a negative number of seconds");
		}
	});

	CS2ST_CALL(context, "ISource2GameClients::GetBugReportInfo", {
		// Heap allocation disabled: a fault mid-call skips every destructor
		// between there and the guard, and a stack-only buffer cannot leak.
		//
		// This is also the cheapest cross-module CBufferString test there is -
		// we construct the object, the game DLL fills it, and tier0 owns the
		// growing. Three modules have to agree on the layout for it to work.
		CBufferStringN<512> report(false);
		clients_->GetBugReportInfo(report);
		context.Detail("-> %d/%d bytes: \"%.200s\"",
			report.Length(), report.AllocatedNum(), report.Get());
		if (report.Length() > report.AllocatedNum())
		{
			context.Fail("length exceeds the allocation; the layout does not hold");
		}
	});

	// Everything below is a notification or a writer. Aimed at a slot nobody is
	// in, the game has no player to act on, so each call exercises the real entry
	// point and its guard clauses without fabricating a player event. An occupied
	// slot gets none of it.
	if (occupied)
	{
		context.SkipCall("ISource2GameClients::<empty-slot calls>",
			"slot %d is occupied; they would act on a real player", kProbeSlot);
		return;
	}
	ExerciseEmptySlot(context, slot);
	ExerciseMessageCalls(context, slot);
}

// -----------------------------------------------------------------------------
//  Notifications and writers, aimed at an empty slot
// -----------------------------------------------------------------------------

void Source2GameClientsSuite::ExerciseEmptySlot(TestContext& context, CPlayerSlot slot)
{
	context.Note("[ISource2GameClients] notifications and writers (empty slot %d)", slot.Get());

	CS2ST_CALL(context, "ISource2GameClients::ClientSettingsChanged", {
		clients_->ClientSettingsChanged(slot);
		context.Detail("returned for the empty slot");
	});

	CS2ST_CALL(context, "ISource2GameClients::ClientVoice", {
		clients_->ClientVoice(slot);
		context.Detail("returned for the empty slot");
	});

	// Built from an argv so no CUtlString allocation crosses modules; the game only
	// reads it.
	const char* command_argv[] = {"cs2sdktest_noop"};
	CCommand command(1, command_argv);
	CS2ST_CALL(context, "ISource2GameClients::ClientCommand", {
		clients_->ClientCommand(slot, command);
		context.Detail("returned for the empty slot (\"%s\")", command.Arg(0));
	});

	CS2ST_CALL(context, "ISource2GameClients::ClientSvcUserMessage", {
		// Type 0 with no payload: nothing for a handler to decode.
		clients_->ClientSvcUserMessage(slot, 0, 0, nullptr);
		context.Detail("returned for the empty slot (type 0, 0 bytes)");
	});

	CS2ST_CALL(context, "ISource2GameClients::ClientSetupVisibility", {
		vis_info_t visinfo;
		std::memset(static_cast<void*>(&visinfo), 0, sizeof(visinfo));
		visinfo.m_uVisBitsBufSize = sizeof(visinfo.m_VisBits);
		clients_->ClientSetupVisibility(slot, &visinfo);
		context.Detail("returned; bits buffer %u bytes, %d bit(s) set afterwards",
			visinfo.m_uVisBitsBufSize,
			CountSetBits(&visinfo.m_VisBits, sizeof(visinfo.m_VisBits)));
	});

	CS2ST_CALL(context, "ISource2GameClients::GetPlayerCommandStatus", {
		// Filled with a pattern the game would have to overwrite, as ClientEarPosition
		// does, so "left alone" differs from "wrote zeroes".
		PlayerCommandStatus_t status;
		std::memset(static_cast<void*>(&status), 0xCD, sizeof(status));
		clients_->GetPlayerCommandStatus(slot, &status);
		const unsigned char* raw = reinterpret_cast<const unsigned char*>(&status);
		size_t untouched = 0;
		for (size_t i = 0; i < sizeof(status); ++i)
		{
			untouched += (raw[i] == 0xCD) ? 1 : 0;
		}
		context.Detail("-> executed=%d received=%d margin=%.3f starved=%d discarded=%d; "
		               "%zu/%zu bytes untouched",
			status.m_nLastCmdNumberExecuted, status.m_nLastCmdNumberReceived,
			static_cast<double>(status.m_flLastCmdRecvMargin), status.m_nStarvedCommandTicks,
			status.m_nDiscardedCommandTicks, untouched, sizeof(status));
	});

	// The slot's own userinfo handle handed back unchanged: the write is the
	// identity, and the engine side has already answered what it holds.
	if (engine_ == nullptr)
	{
		context.UnavailableCall("ISource2GameClients::ClientSetConVarUserInfoSet",
			"no IVEngineServer2 to fetch the slot's userinfo handle from");
	}
	else
	{
		CS2ST_CALL(context, "ISource2GameClients::ClientSetConVarUserInfoSet", {
			const ConVarUserInfoSet_t info = engine_->GetClientUserInfo(slot);
			clients_->ClientSetConVarUserInfoSet(slot, info);
			context.Detail("wrote back the slot's own handle (data=%p)",
				static_cast<const void*>(info.m_pData));
		});
	}

	CS2ST_CALL(context, "ISource2GameClients::ClientDiagnostic", {
		// A local message, never handed over: the game only reads it, and an empty
		// slot has no client to attribute it to.
		CCLCMsg_Diagnostic diagnostic;
		clients_->ClientDiagnostic(slot, &diagnostic);
		context.Detail("returned for the empty slot (empty message)");
	});

	CS2ST_CALL(context, "ISource2GameClients::ClientReplayEvent", {
		const ClientReplayEventParams_t params;
		const bool accepted = clients_->ClientReplayEvent(slot, params);
		context.Detail("-> %s for the empty slot", accepted ? "true" : "false");
		if (accepted)
		{
			context.Suspect("a replay event was accepted for a slot with no player");
		}
	});

	// Late on purpose: it announces a departure, so the calls above have already
	// seen the slot as it was. Only safe because the slot is provably empty - on
	// an occupied one this would drop a real player.
	CS2ST_CALL(context, "ISource2GameClients::ClientDisconnect", {
		clients_->ClientDisconnect(slot, NETWORK_DISCONNECT_INVALID, "", 0, "");
		context.Detail("returned for the empty slot (no name, xuid 0)");
	});

	CS2ST_CALL(context, "ISource2GameClients::ShouldKickClientForSteamAuthFailure", {
		const bool kick = clients_->ShouldKickClientForSteamAuthFailure(
			slot, NETWORK_DISCONNECT_INVALID);
		context.Detail("-> %s for the empty slot", kick ? "true" : "false");
	});
}

// -----------------------------------------------------------------------------
//  Calls that need a CNetMessage
//
//  The messages come from the engine's own allocator, through INetworkMessages,
//  and go back to it afterwards: CNetMessagePB is a stub over the engine's class
//  (see NetworkMessagesSuite) and cannot be constructed here.
// -----------------------------------------------------------------------------

void Source2GameClientsSuite::ExerciseMessageCalls(TestContext& context, CPlayerSlot slot)
{
	context.Note("[ISource2GameClients] calls that take a CNetMessage (engine-allocated)");

	INetworkMessages* messages = static_cast<INetworkMessages*>(
		context.QueryInterface("networksystem", NETWORKMESSAGES_INTERFACE_VERSION));
	if (messages == nullptr)
	{
		context.UnavailableCall("ISource2GameClients::ProcessUsercmds",
			"networksystem does not answer '%s'", NETWORKMESSAGES_INTERFACE_VERSION);
		context.UnavailableCall("ISource2GameClients::ShouldSendMessageDuringHltvReplay",
			"networksystem does not answer '%s'", NETWORKMESSAGES_INTERFACE_VERSION);
		context.UnavailableCall("ISource2GameClients::GetMessageEncryptionKey",
			"networksystem does not answer '%s'", NETWORKMESSAGES_INTERFACE_VERSION);
		return;
	}

	// The registry entry is the NetMessageInfo_t itself now; the two calls
	// below that want a descriptor take it as it comes.
	NetMessageInfo_t* info = nullptr;
	CNetMessage* allocated = nullptr;
	CS2ST_CALL(context, "NetMessageInfo_t::AllocateMessage (CCLCMsg_Move)", {
		const char* how = nullptr;
		info = FindRegisteredMessage(messages, "CCLCMsg_Move", -1, &how);
		if (info == nullptr)
		{
			context.Detail("CCLCMsg_Move is not registered");
			return;
		}
		allocated = info->AllocateMessage();
		context.Detail("-> %p (found %s)", static_cast<void*>(allocated), how);
	});

	if (allocated == nullptr)
	{
		context.SkipCall("ISource2GameClients::ProcessUsercmds",
			"no CCLCMsg_Move could be allocated from the engine");
		context.SkipCall("ISource2GameClients::ShouldSendMessageDuringHltvReplay",
			"no message could be allocated from the engine");
		context.SkipCall("ISource2GameClients::GetMessageEncryptionKey",
			"no message could be allocated from the engine");
		return;
	}

	CS2ST_CALL(context, "ISource2GameClients::ProcessUsercmds", {
		// An empty CCLCMsg_Move for an empty slot: no usercmds to decode, so the
		// call can only exercise the guard in front of them.
		const CNetMessagePB<CCLCMsg_Move>* move = allocated->ToPB<CCLCMsg_Move>();
		clients_->ProcessUsercmds(slot, *move, false);
		context.Detail("returned for the empty slot (empty CCLCMsg_Move, not paused)");
	});

	CS2ST_CALL(context, "ISource2GameClients::ShouldSendMessageDuringHltvReplay", {
		context.Detail("-> %s for CCLCMsg_Move",
			clients_->ShouldSendMessageDuringHltvReplay(info, allocated) ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2GameClients::GetMessageEncryptionKey", {
		const int key = static_cast<int>(clients_->GetMessageEncryptionKey(info, allocated));
		context.Detail("-> %d for CCLCMsg_Move", key);
		if (key < kEncryptedMessageKeyType_None || key > kEncryptedMessageKeyType_Public)
		{
			context.Fail("%d is not an EncryptedMessageKeyType_t", key);
		}
	});

	CS2ST_CALL(context, "CNetMessage::~CNetMessage (CCLCMsg_Move)", {
		// DeallocateNetMessageAbstract is gone from the SDK; the virtual
		// destructor frees through the engine's own allocator.
		void* freed = static_cast<void*>(allocated);
		delete allocated;
		context.Detail("-> deleted %p", freed);
	});
}

// -----------------------------------------------------------------------------
//  HLTV
// -----------------------------------------------------------------------------

void Source2GameClientsSuite::ExerciseHltv(TestContext& context)
{
	context.Note("[ISource2GameClients] HLTV");

	CS2ST_CALL(context, "ISource2GameClients::GetHLTVDirector", {
		// Null is the ordinary answer with tv_enable off. What is being
		// recorded is that the slot is still a nullary getter returning a
		// pointer, which a shifted vtable would not be.
		IHLTVDirector* director = clients_->GetHLTVDirector();
		context.Detail("-> %p%s", static_cast<void*>(director),
			director == nullptr ? " (no HLTV server running)" : "");
	});

	// Admission and password checks are questions, not actions: asked about an
	// invalid SteamID they answer from the rules alone and admit nobody.
	CS2ST_CALL(context, "ISource2GameClients::CanHLTVClientConnect", {
		int reject_reason = 0;
		const CSteamID steam_id;
		const bool allowed = clients_->CanHLTVClientConnect(0, steam_id, &reject_reason);
		context.Detail("-> %s for an invalid SteamID (reject reason %d)",
			allowed ? "true" : "false", reject_reason);
	});

	CS2ST_CALL(context, "ISource2GameClients::CheckHltvPasswordMatch", {
		const CSteamID steam_id;
		const bool same = clients_->CheckHltvPasswordMatch(
			"cs2sdktest", "cs2sdktest", steam_id, nullptr);
		const bool different = clients_->CheckHltvPasswordMatch(
			"cs2sdktest", "not-cs2sdktest", steam_id, nullptr);
		context.Detail("-> equal passwords: %s, different passwords: %s",
			same ? "match" : "no match", different ? "match" : "no match");
		// Suspect, not Fail: the SDK does not say whether a server with no HLTV
		// password configured accepts everything.
		if (different)
		{
			context.Suspect("two different passwords matched");
		}
	});

	CS2ST_CALL(context, "ISource2GameClients::ValidateConfigCommands", {
		// Validation only: the string is checked, not run. One benign command and
		// one empty config, so the two answers can be compared.
		CBufferStringN<256> errors_benign(false);
		CBufferStringN<256> errors_empty(false);
		const bool benign = clients_->ValidateConfigCommands(
			"echo cs2sdktest", &errors_benign);
		const bool empty = clients_->ValidateConfigCommands("", &errors_empty);
		context.Detail("-> \"echo cs2sdktest\": %s (errors \"%.100s\"); empty: %s (errors \"%.100s\")",
			benign ? "valid" : "invalid", errors_benign.Get(),
			empty ? "valid" : "invalid", errors_empty.Get());
		if (errors_benign.Length() > errors_benign.AllocatedNum()
			|| errors_empty.Length() > errors_empty.AllocatedNum())
		{
			context.Fail("an error buffer's length exceeds its allocation; the layout does not hold");
		}
	});

	context.SkipCall("ISource2GameClients::StartHLTVServer", "would start an HLTV server");
	context.SkipCall("ISource2GameClients::HLTVClientFullyConnect",
		"would admit an HLTV client that does not exist");
	context.SkipCall("ISource2GameClients::SendHLTVStatusMessage",
		"first parameter is an IHLTVServer the engine dereferences, and the call "
		"broadcasts a status message to every HLTV client");
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
//
//  The connection-lifecycle notifications that would create state - a
//  controller, a connection record - which nothing here could tear down again.
//  They are listed: a reader has to be able to tell "checked, fine" from
//  "nobody looked", and a missing line says neither.
// -----------------------------------------------------------------------------

void Source2GameClientsSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[ISource2GameClients] not called - would create player state "
	             "that nothing here can remove");

	context.SkipCall("ISource2GameClients::OnClientConnected",
		"would announce a connection that did not happen");
	context.SkipCall("ISource2GameClients::ClientConnect",
		"would admit a player");
	context.SkipCall("ISource2GameClients::ClientPutInServer",
		"would spawn a controller for a slot");
	context.SkipCall("ISource2GameClients::ClientActive",
		"would activate a player");
	context.SkipCall("ISource2GameClients::ClientFullyConnect",
		"would advance a connection state machine");
	context.SkipCall("ISource2GameClients::ClientCommandKeyValues",
		"needs a KeyValues built with the engine's allocator; this module's heap "
		"is not the engine's (CS2SDKTEST_USE_MEMOVERRIDE is off), and a null one "
		"is dereferenced");
	context.SkipCall("ISource2GameClients::ClientFullyConnect (engine side)",
		"ISource2ServerConfig::OnClientFullyConnect is the paired call and "
		"belongs to that suite");

	// Everything else in the interface is called, on an empty slot, in
	// ExerciseEmptySlot / ExerciseMessageCalls / ExerciseHltv.
}

} // namespace cs2st
