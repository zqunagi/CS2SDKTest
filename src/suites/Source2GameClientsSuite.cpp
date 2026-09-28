#include "suites/Source2GameClientsSuite.h"

#include <cmath>
#include <cstddef>
#include <cstdio>

#include <appframework/IAppSystem.h>
#include <tier1/bufferstring.h>

#include "hook/HookArg.h"
#include "hook/HookRecorder.h"

// The SH_DECL_HOOK block used to live here; under KHook the hook templates are
// declared as members in the header instead, bound to the SDK member pointers
// in the constructor at the bottom of this file. The signatures there are the
// SDK's own, not paraphrased: KHook resolves the vtable slot from the member
// pointer, so a drifted signature is a compile error rather than a silent
// garbage read - which is the drift-detection this suite exists for, moved
// from runtime to build time.

namespace cs2st
{
namespace
{

// Hook ids. Used as both the log label and the recorder key, so they are
// written once here rather than repeated at each call site.
constexpr const char* kOnClientConnected      = "ISource2GameClients::OnClientConnected";
constexpr const char* kClientConnect          = "ISource2GameClients::ClientConnect";
constexpr const char* kClientPutInServer      = "ISource2GameClients::ClientPutInServer";
constexpr const char* kClientActive           = "ISource2GameClients::ClientActive";
constexpr const char* kClientFullyConnect     = "ISource2GameClients::ClientFullyConnect";
constexpr const char* kClientDisconnect       = "ISource2GameClients::ClientDisconnect";
constexpr const char* kClientCommand          = "ISource2GameClients::ClientCommand";
constexpr const char* kClientSettingsChanged  = "ISource2GameClients::ClientSettingsChanged";
constexpr const char* kClientVoice            = "ISource2GameClients::ClientVoice";
constexpr const char* kClientCommandKeyValues = "ISource2GameClients::ClientCommandKeyValues";
constexpr const char* kClientSvcUserMessage   = "ISource2GameClients::ClientSvcUserMessage";

/** Number of hooked methods; InstallHooks reports it. */
constexpr int kHookedMethodCount = 11;

/**
 * Slot the read-only queries are aimed at.
 *
 * It need not be occupied. These are all reads, and an empty slot's answer
 * probes the call just as well as a full one's - what is under test is that the
 * call returns at all and returns something of the right shape.
 */
constexpr int kProbeSlot = 0;

/** CCommand's own COMMAND_MAX_ARGC, which is private, so it is restated here. */
constexpr int kMaxPlausibleArgC = 64;

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
	// nothing about the SDK. It is fetched here rather than in Acquire because
	// Acquire also runs on the `hooks install` path, which has no fault guard
	// around it: an unguarded engine call there would take the server down with
	// no line in the log.
	CS2ST_CALL(context, "IVEngineServer2::GetServerGlobals (for maxClients)", {
		IVEngineServer2* engine = static_cast<IVEngineServer2*>(
			context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));
		if (engine == nullptr)
		{
			context.Detail("-> engine2 does not answer '%s'", INTERFACEVERSION_VENGINESERVER);
			return;
		}
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
		const int tick_base = clients_->GetPlayerTickBase(slot);
		context.Detail("-> %d (tickcount %d)", tick_base, globals_->tickcount);
		if (tick_base < 0)
		{
			context.Fail("a tick base cannot be negative");
		}
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
		CEntityIndex replay_entity(-1);
		const float delay = clients_->GetPlayerHltvDelay(slot, replay_entity);
		context.Detail("-> %.3f s, replay entity %d",
			static_cast<double>(delay), replay_entity.Get());
		if (std::isnan(delay) || std::isinf(delay) || delay < 0.0f)
		{
			context.Fail("a delay must be a finite, non-negative number of seconds");
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

	context.SkipCall("ISource2GameClients::StartHLTVServer", "would start an HLTV server");
	context.SkipCall("ISource2GameClients::HLTVClientFullyConnect",
		"would admit an HLTV client that does not exist");
	context.SkipCall("ISource2GameClients::CanHLTVClientConnect",
		"reads like a query, but it is the admission check for a connection "
		"attempt and needs a real CSteamID to mean anything");
	context.SkipCall("ISource2GameClients::SendHLTVStatusMessage",
		"needs an IHLTVServer and would broadcast");
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
//
//  Most of this interface is the engine telling the game that something
//  happened to a player. Calling those from here would not test them, it would
//  fabricate the event - a ClientDisconnect nobody asked for drops a real
//  player. They are listed anyway: a reader has to be able to tell "checked,
//  fine" from "nobody looked", and a missing line says neither.
// -----------------------------------------------------------------------------

void Source2GameClientsSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[ISource2GameClients] not called - engine-to-game notifications "
	             "(hooked instead)");

	context.SkipCall("ISource2GameClients::OnClientConnected",
		"would announce a connection that did not happen; hooked instead");
	context.SkipCall("ISource2GameClients::ClientConnect",
		"would admit a player; hooked instead");
	context.SkipCall("ISource2GameClients::ClientPutInServer",
		"would spawn a controller for a slot; hooked instead");
	context.SkipCall("ISource2GameClients::ClientActive",
		"would activate a player; hooked instead");
	context.SkipCall("ISource2GameClients::ClientFullyConnect",
		"would advance a connection state machine; hooked instead");
	context.SkipCall("ISource2GameClients::ClientDisconnect",
		"would drop a real player; hooked instead");
	context.SkipCall("ISource2GameClients::ClientCommand",
		"would run a console command as a player; hooked instead");
	context.SkipCall("ISource2GameClients::ClientSettingsChanged",
		"would re-read a player's userinfo cvars; hooked instead");
	context.SkipCall("ISource2GameClients::ClientVoice",
		"would inject a voice event; hooked instead");
	context.SkipCall("ISource2GameClients::ClientCommandKeyValues",
		"would deliver a command nobody sent; hooked instead");
	context.SkipCall("ISource2GameClients::ClientSvcUserMessage",
		"would deliver a user message nobody sent; hooked instead");
	context.SkipCall("ISource2GameClients::ClientSetConVarUserInfoSet",
		"writes a player's userinfo cvar block; ConVarUserInfoSet_t is a bare "
		"uint8* with no documented size, so there is nothing safe to pass");
	context.SkipCall("ISource2GameClients::ClientFullyConnect (engine side)",
		"ISource2ServerConfig::OnClientFullyConnect is the paired call and "
		"belongs to that suite");

	context.Note("[ISource2GameClients] not called and not hooked");

	context.SkipCall("ISource2GameClients::ClientSetupVisibility",
		"runs per player per frame and only carries a vis_info_t the SDK does "
		"not define; a hook here would be cost without evidence");
	context.SkipCall("ISource2GameClients::ProcessUsercmds",
		"takes CNetMessagePB<CCLCMsg_Move> by const reference, so the hook "
		"template needs its size - and netmessages.pb.h is not among the protobuf "
		"files this build generates (see CS2SDKTEST_PROTO_FILES)");
	context.SkipCall("ISource2GameClients::ClientDiagnostic",
		"CCLCMsg_Diagnostic is only forward-declared here; hookable, but there "
		"would be nothing to read out of the pointer");
	context.SkipCall("ISource2GameClients::ReplayLastPlayerKill",
		"second parameter is a void* out block of unknown size");
	context.SkipCall("ISource2GameClients::unk101",
		"unidentified in the SDK and takes a player slot; calling it blind on a "
		"live player is not worth the one line it would produce");
	context.SkipCall("ISource2GameClients::unk201", "unidentified in the SDK");
	context.SkipCall("ISource2GameClients::unk202", "unidentified in the SDK");
	context.SkipCall("ISource2GameClients::unk203", "unidentified in the SDK");
}

// -----------------------------------------------------------------------------
//  Install / remove
// -----------------------------------------------------------------------------

int Source2GameClientsSuite::InstallHooks(TestContext& context)
{
	if (clients_ == nullptr || hooks_installed_)
	{
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before installing so a hook that never fires still shows up in
	// the dump as "never". On a server nobody joins, that is most of this list
	// - which is why the dump has to be read after a round, not after a sweep.
	recorder.Declare(kOnClientConnected, Id());
	recorder.Declare(kClientConnect, Id());
	recorder.Declare(kClientPutInServer, Id());
	recorder.Declare(kClientActive, Id());
	recorder.Declare(kClientFullyConnect, Id());
	recorder.Declare(kClientDisconnect, Id());
	recorder.Declare(kClientCommand, Id());
	recorder.Declare(kClientSettingsChanged, Id());
	recorder.Declare(kClientVoice, Id());
	recorder.Declare(kClientCommandKeyValues, Id());
	recorder.Declare(kClientSvcUserMessage, Id());

	// Each hook object carries its pre and post handler from the constructor;
	// Add installs the detour on the clients instance, Remove takes it off.
	hook_OnClientConnected_.Add(clients_);
	hook_ClientConnect_.Add(clients_);
	hook_ClientPutInServer_.Add(clients_);
	hook_ClientActive_.Add(clients_);
	hook_ClientFullyConnect_.Add(clients_);
	hook_ClientDisconnect_.Add(clients_);
	hook_ClientCommand_.Add(clients_);
	hook_ClientSettingsChanged_.Add(clients_);
	hook_ClientVoice_.Add(clients_);
	hook_ClientCommandKeyValues_.Add(clients_);
	hook_ClientSvcUserMessage_.Add(clients_);

	hooks_installed_ = true;

	context.Note("nothing here fires until a player connects; reconnect once, "
	             "type something in chat, then dump");
	return kHookedMethodCount;
}

void Source2GameClientsSuite::RemoveHooks(TestContext& context)
{
	if (clients_ == nullptr || !hooks_installed_)
	{
		return;
	}

	hook_OnClientConnected_.Remove(clients_);
	hook_ClientConnect_.Remove(clients_);
	hook_ClientPutInServer_.Remove(clients_);
	hook_ClientActive_.Remove(clients_);
	hook_ClientFullyConnect_.Remove(clients_);
	hook_ClientDisconnect_.Remove(clients_);
	hook_ClientCommand_.Remove(clients_);
	hook_ClientSettingsChanged_.Remove(clients_);
	hook_ClientVoice_.Remove(clients_);
	hook_ClientCommandKeyValues_.Remove(clients_);
	hook_ClientSvcUserMessage_.Remove(clients_);

	hooks_installed_ = false;
	(void)context;
}

// -----------------------------------------------------------------------------
//  Hook handlers
//
//  These run in the engine's own frame, where CallGuard is not armed: a fault
//  here kills the server rather than producing a CRASHED line. So every pointer
//  argument is vetted before it is touched, every string is length-capped, and
//  nothing walks a structure it was handed.
//
//  Every one answers Action::Ignore, the KHook equivalent of MRES_IGNORED.
// -----------------------------------------------------------------------------

KHook::Return<void> Source2GameClientsSuite::Pre_OnClientConnected(ISource2GameClients*, CPlayerSlot slot, const char* name,
                                                                   uint64 xuid, const char* network_id,
                                                                   const char* address, bool fake_player)
{
	char name_scratch[48];
	char id_scratch[48];
	char address_scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kOnClientConnected,
		"slot=%d name=\"%.63s\" xuid=%llu id=\"%.63s\" addr=\"%.63s\" fake=%d",
		slot.Get(),
		SafeStringArg(name, name_scratch),
		static_cast<unsigned long long>(xuid),
		SafeStringArg(network_id, id_scratch),
		SafeStringArg(address, address_scratch),
		fake_player ? 1 : 0);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_OnClientConnected(ISource2GameClients*, CPlayerSlot, const char*, uint64,
                                                                    const char*, const char*, bool)
{
	CS2ST_HOOK_LEAVE(kOnClientConnected);
	return { KHook::Action::Ignore };
}

KHook::Return<bool> Source2GameClientsSuite::Pre_ClientConnect(ISource2GameClients*, CPlayerSlot slot, const char* name,
                                                               uint64 xuid, const char* network_id,
                                                               bool unk1, CBufferString* reject_reason)
{
	char name_scratch[48];
	char id_scratch[48];

	// The reject buffer is read as a *structure*, not as a string. At pre-hook
	// time the game has not written into it yet, so there is no text to get -
	// but it is an engine-constructed CBufferString, and its two header fields
	// are self-describing enough to check: a length can never exceed the
	// allocation. That makes this the one place a live CBufferString's layout
	// gets validated against an instance we did not build ourselves.
	char buffer_note[64] = "reject=(null)";
	if (reject_reason != nullptr)
	{
		if (!LooksLikeObjectPointer(reject_reason))
		{
			std::snprintf(buffer_note, sizeof(buffer_note), "reject=<bad ptr %p>",
				static_cast<void*>(reject_reason));
		}
		else
		{
			const int length = reject_reason->Length();
			const int allocated = reject_reason->AllocatedNum();
			std::snprintf(buffer_note, sizeof(buffer_note), "reject=%d/%d%s%s",
				length, allocated,
				reject_reason->IsStackAllocated() ? " stack" : " heap",
				length > allocated ? " LAYOUT-BROKEN" : "");
		}
	}

	CS2ST_HOOK_ENTER_ARGS(kClientConnect,
		"slot=%d name=\"%.63s\" xuid=%llu id=\"%.63s\" unk1=%d %s",
		slot.Get(),
		SafeStringArg(name, name_scratch),
		static_cast<unsigned long long>(xuid),
		SafeStringArg(network_id, id_scratch),
		unk1 ? 1 : 0,
		buffer_note);

	// Action::Ignore means the `ret` field here is discarded and the game's own
	// answer stands. Returning a real false would refuse the connection.
	return { KHook::Action::Ignore, true };
}

KHook::Return<bool> Source2GameClientsSuite::Post_ClientConnect(ISource2GameClients*, CPlayerSlot, const char*, uint64,
                                                                const char*, bool, CBufferString*)
{
	CS2ST_HOOK_LEAVE(kClientConnect);
	return { KHook::Action::Ignore, true };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientPutInServer(ISource2GameClients*, CPlayerSlot slot, const char* name,
                                                                   int type, uint64 xuid)
{
	char name_scratch[48];
	// The SDK documents type as 0 player / 1 bot / 2 unknown, which makes
	// anything else a signal that the parameter is not this parameter.
	CS2ST_HOOK_ENTER_ARGS(kClientPutInServer,
		"slot=%d name=\"%.63s\" type=%d%s xuid=%llu",
		slot.Get(),
		SafeStringArg(name, name_scratch),
		type, (type < 0 || type > 2) ? " OUT-OF-RANGE" : "",
		static_cast<unsigned long long>(xuid));
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientPutInServer(ISource2GameClients*, CPlayerSlot, const char*, int, uint64)
{
	CS2ST_HOOK_LEAVE(kClientPutInServer);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientActive(ISource2GameClients*, CPlayerSlot slot, bool load_game,
                                                              const char* name, uint64 xuid)
{
	char name_scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kClientActive, "slot=%d load=%d name=\"%.63s\" xuid=%llu",
		slot.Get(), load_game ? 1 : 0,
		SafeStringArg(name, name_scratch),
		static_cast<unsigned long long>(xuid));
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientActive(ISource2GameClients*, CPlayerSlot, bool, const char*, uint64)
{
	CS2ST_HOOK_LEAVE(kClientActive);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientFullyConnect(ISource2GameClients*, CPlayerSlot slot)
{
	CS2ST_HOOK_ENTER_ARGS(kClientFullyConnect, "slot=%d", slot.Get());
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientFullyConnect(ISource2GameClients*, CPlayerSlot)
{
	CS2ST_HOOK_LEAVE(kClientFullyConnect);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientDisconnect(ISource2GameClients*, CPlayerSlot slot,
                                                                  ENetworkDisconnectionReason reason,
                                                                  const char* name, uint64 xuid,
                                                                  const char* network_id)
{
	char name_scratch[48];
	char id_scratch[48];

	// The highest-value check in this suite. ENetworkDisconnectionReason is a
	// protobuf enum with a generated IsValid, so the argument can be tested
	// against the real value set rather than against a guess at its range. The
	// name is not resolved: ENetworkDisconnectionReason_Name goes through the
	// descriptor pool and allocates a std::string, which is not something to do
	// from inside the engine's frame when a number says the same thing.
	const int reason_value = static_cast<int>(reason);
	const bool known = ENetworkDisconnectionReason_IsValid(reason_value);

	CS2ST_HOOK_ENTER_ARGS(kClientDisconnect,
		"slot=%d reason=%d%s name=\"%.63s\" xuid=%llu id=\"%.63s\"",
		slot.Get(),
		reason_value, known ? "" : " NOT-A-VALID-REASON",
		SafeStringArg(name, name_scratch),
		static_cast<unsigned long long>(xuid),
		SafeStringArg(network_id, id_scratch));
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientDisconnect(ISource2GameClients*, CPlayerSlot, ENetworkDisconnectionReason,
                                                                   const char*, uint64, const char*)
{
	CS2ST_HOOK_LEAVE(kClientDisconnect);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientCommand(ISource2GameClients*, CPlayerSlot slot, const CCommand& args)
{
	// CCommand's accessors are inline in the SDK header, so reading them trusts
	// the SDK's layout against the object the engine built. ArgC() comes out of
	// a CUtlVector's count field: if that offset is wrong the number is
	// arbitrary, and indexing with it is how a diagnostic becomes a crash. So
	// the count is bounds-checked first and only then used.
	const int argc = args.ArgC();
	const bool plausible = argc > 0 && argc <= kMaxPlausibleArgC;

	char arg0_scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kClientCommand, "slot=%d argc=%d%s cmd=\"%.63s\"",
		slot.Get(), argc,
		plausible ? "" : " IMPLAUSIBLE",
		plausible ? SafeStringArg(args.Arg(0), arg0_scratch) : "<not read>");
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientCommand(ISource2GameClients*, CPlayerSlot, const CCommand&)
{
	CS2ST_HOOK_LEAVE(kClientCommand);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientSettingsChanged(ISource2GameClients*, CPlayerSlot slot)
{
	CS2ST_HOOK_ENTER_ARGS(kClientSettingsChanged, "slot=%d", slot.Get());
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientSettingsChanged(ISource2GameClients*, CPlayerSlot)
{
	CS2ST_HOOK_LEAVE(kClientSettingsChanged);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientVoice(ISource2GameClients*, CPlayerSlot slot)
{
	// Fires per voice packet, so it is the busiest hook in this suite by a wide
	// margin. HookRecorder stops formatting once its sample quota is full,
	// which is what keeps that affordable.
	CS2ST_HOOK_ENTER_ARGS(kClientVoice, "slot=%d", slot.Get());
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientVoice(ISource2GameClients*, CPlayerSlot)
{
	CS2ST_HOOK_LEAVE(kClientVoice);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientCommandKeyValues(ISource2GameClients*, CPlayerSlot slot, KeyValues* kv)
{
	// Only the pointer value - walking into a KeyValues tree from a hook is
	// exactly the kind of thing that turns a diagnostic into an outage.
	CS2ST_HOOK_ENTER_ARGS(kClientCommandKeyValues, "slot=%d kv=%p", slot.Get(),
		static_cast<void*>(kv));
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientCommandKeyValues(ISource2GameClients*, CPlayerSlot, KeyValues*)
{
	CS2ST_HOOK_LEAVE(kClientCommandKeyValues);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Pre_ClientSvcUserMessage(ISource2GameClients*, CPlayerSlot slot, int type,
                                                                      uint32 size, const void* buffer)
{
	// The payload is not decoded: that needs the usermessages protobuf set,
	// which this build does not generate. Type and size are still the useful
	// part - they are what a plugin routing user messages keys off.
	CS2ST_HOOK_ENTER_ARGS(kClientSvcUserMessage, "slot=%d type=%d size=%u buf=%p",
		slot.Get(), type, static_cast<unsigned>(size), buffer);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2GameClientsSuite::Post_ClientSvcUserMessage(ISource2GameClients*, CPlayerSlot, int, uint32, const void*)
{
	CS2ST_HOOK_LEAVE(kClientSvcUserMessage);
	return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
//  Hook binding
//
//  The KHook::Virtual constructor resolves the vtable slot from the member
//  pointer, so this list is the plugin's ABI contract with the SDK - and with
//  it, argument drift moved from a runtime garbage read to a compile error.
// -----------------------------------------------------------------------------

Source2GameClientsSuite::Source2GameClientsSuite() :
	hook_OnClientConnected_(&ISource2GameClients::OnClientConnected, this,
		&Source2GameClientsSuite::Pre_OnClientConnected,
		&Source2GameClientsSuite::Post_OnClientConnected),
	hook_ClientConnect_(&ISource2GameClients::ClientConnect, this,
		&Source2GameClientsSuite::Pre_ClientConnect,
		&Source2GameClientsSuite::Post_ClientConnect),
	hook_ClientPutInServer_(&ISource2GameClients::ClientPutInServer, this,
		&Source2GameClientsSuite::Pre_ClientPutInServer,
		&Source2GameClientsSuite::Post_ClientPutInServer),
	hook_ClientActive_(&ISource2GameClients::ClientActive, this,
		&Source2GameClientsSuite::Pre_ClientActive,
		&Source2GameClientsSuite::Post_ClientActive),
	hook_ClientFullyConnect_(&ISource2GameClients::ClientFullyConnect, this,
		&Source2GameClientsSuite::Pre_ClientFullyConnect,
		&Source2GameClientsSuite::Post_ClientFullyConnect),
	hook_ClientDisconnect_(&ISource2GameClients::ClientDisconnect, this,
		&Source2GameClientsSuite::Pre_ClientDisconnect,
		&Source2GameClientsSuite::Post_ClientDisconnect),
	hook_ClientCommand_(&ISource2GameClients::ClientCommand, this,
		&Source2GameClientsSuite::Pre_ClientCommand,
		&Source2GameClientsSuite::Post_ClientCommand),
	hook_ClientSettingsChanged_(&ISource2GameClients::ClientSettingsChanged, this,
		&Source2GameClientsSuite::Pre_ClientSettingsChanged,
		&Source2GameClientsSuite::Post_ClientSettingsChanged),
	hook_ClientVoice_(&ISource2GameClients::ClientVoice, this,
		&Source2GameClientsSuite::Pre_ClientVoice,
		&Source2GameClientsSuite::Post_ClientVoice),
	hook_ClientCommandKeyValues_(&ISource2GameClients::ClientCommandKeyValues, this,
		&Source2GameClientsSuite::Pre_ClientCommandKeyValues,
		&Source2GameClientsSuite::Post_ClientCommandKeyValues),
	hook_ClientSvcUserMessage_(&ISource2GameClients::ClientSvcUserMessage, this,
		&Source2GameClientsSuite::Pre_ClientSvcUserMessage,
		&Source2GameClientsSuite::Post_ClientSvcUserMessage)
{
}

} // namespace cs2st
