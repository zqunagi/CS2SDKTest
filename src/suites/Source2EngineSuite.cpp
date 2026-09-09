#include "suites/Source2EngineSuite.h"

#include <cstddef>
#include <cstdint>

#include <appframework/IAppSystem.h>
#include <eiface.h>

#include "core/Log.h"
#include "testkit/StructInspector.h"

namespace cs2st
{
namespace
{

/** The store AppID, which is what GetAppID reports. */
constexpr unsigned int kCounterStrike2AppId = 730;

/**
 * GetStatsAppID reports the *content* app instead, not the store app - CS2 is
 * built out of a separate set of apps from the 730 storefront entry, and the
 * exact one shifts between branches. Observed: 2347771.
 */
const unsigned int kStatsAppIds[] = {2347771, 2347772, 2347773};

bool IsKnownStatsAppId(unsigned int app_id)
{
	for (const unsigned int known : kStatsAppIds)
	{
		if (app_id == known)
		{
			return true;
		}
	}
	return false;
}

/** k_EUniversePublic. Anything else means we are not on a retail build. */
constexpr int kUniversePublic = 1;

/** Player slot used for the read-only per-player queries. */
constexpr int kProbeSlot = 0;

/**
 * A spawn group handle that is not expected to be loaded. The point is not the
 * answer but whether the call survives being handed a handle it does not know.
 */
constexpr SpawnGroupHandle_t kUnknownSpawnGroup = 0;

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

bool Source2EngineSuite::Acquire(TestContext& context)
{
	engine_ = static_cast<IVEngineServer2*>(
		context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));

	if (engine_ == nullptr)
	{
		// Worth distinguishing "engine2 is not loaded" from "engine2 is loaded
		// but no longer answers this version": the second one is the finding.
		const Module& engine_module = context.GetModule("engine2");
		if (!engine_module.IsValid())
		{
			context.UnavailableCall("IVEngineServer2", "engine2 is not loaded");
		}
		else
		{
			context.UnavailableCall("IVEngineServer2",
				"engine2 is loaded but does not answer '%s'",
				INTERFACEVERSION_VENGINESERVER);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(engine_),
		INTERFACEVERSION_VENGINESERVER);
	return true;
}

void Source2EngineSuite::ExerciseCalls(TestContext& context)
{
	globals_ = nullptr;

	ExerciseAppSystem(context);
	ExerciseSource2Engine(context);
	ExerciseEngineState(context);
	ExercisePlayerQueries(context);
	ExerciseSpawnGroups(context);
	InspectGlobalVars(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots of the vtable
// -----------------------------------------------------------------------------

void Source2EngineSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	// Every one of these has a value the engine can only sensibly return one
	// answer for, which is what makes them a vtable alignment check rather than
	// just a liveness check. A GetTier() that comes back as 71 is not a weird
	// tier, it is a different function.

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = engine_->GetTier();
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
		const bool singleton = engine_->IsSingleton();
		context.Detail("-> %s", singleton ? "true" : "false");
		if (!singleton)
		{
			context.Fail("the engine is a singleton; false here suggests a slot shift");
		}
	});

	// GetBuildType is slot 10, added on CS2. The SDK's own CBaseAppSystem does
	// not implement it, so a wrong answer here is the canary for that whole
	// family of vtable problems.
	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = engine_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = engine_->GetDependencies();
		if (deps == nullptr)
		{
			context.Detail("-> none");
			return;
		}

		// Terminated by a {nullptr, nullptr} entry. Capped because walking a
		// garbage pointer until it happens to hit two nulls is how a probe
		// turns into a hang instead of a crash.
		int count = 0;
		while (count < 64 && deps[count].m_pModuleName != nullptr)
		{
			++count;
		}
		context.Detail("-> %p, %d entr%s%s", static_cast<const void*>(deps),
			count, count == 1 ? "y" : "ies", count == 64 ? " (capped)" : "");
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface", {
		// Asking an interface for itself is the one query guaranteed to be
		// meaningful without knowing what else this object implements.
		void* self = engine_->QueryInterface(INTERFACEVERSION_VENGINESERVER);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(engine_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run engine startup");
	context.SkipCall("IAppSystem::Init", "would re-run engine startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin engine shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the engine down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down engine interfaces");
}

// -----------------------------------------------------------------------------
//  ISource2Engine
// -----------------------------------------------------------------------------

void Source2EngineSuite::ExerciseSource2Engine(TestContext& context)
{
	context.Note("[ISource2Engine]");

	CS2ST_CALL(context, "ISource2Engine::IsPaused", {
		context.Detail("-> %s", engine_->IsPaused() ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Engine::GetTimescale", {
		const float timescale = engine_->GetTimescale();
		context.Detail("-> %.4f", timescale);
		// host_timescale is clamped well inside this on any real server, so a
		// value outside it means we read a float out of the wrong place.
		if (!(timescale > 0.0f && timescale <= 100.0f))
		{
			context.Fail("implausible timescale");
		}
	});

	CS2ST_CALL(context, "ISource2Engine::GetStatsAppID", {
		const uint32 app_id = engine_->GetStatsAppID();
		context.Detail("-> %u", app_id);
		if (!IsKnownStatsAppId(app_id))
		{
			context.Fail("not a known CS2 content app id (expected one of %u-%u)",
				kStatsAppIds[0], kStatsAppIds[2]);
		}
	});

	context.SkipCall("ISource2Engine::FindOrCreateWorldSession",
		"creates a world session if one is missing");
	context.SkipCall("ISource2Engine::GetEntityLumpForTemplate",
		"may instantiate an entity lump; parameter meanings are unconfirmed");
	context.SkipCall("ISource2Engine::UnknownFunc1", "unidentified in the SDK");
	context.SkipCall("ISource2Engine::UnknownFunc2", "unidentified in the SDK");
}

// -----------------------------------------------------------------------------
//  IVEngineServer2 - server and host state
// -----------------------------------------------------------------------------

void Source2EngineSuite::ExerciseEngineState(TestContext& context)
{
	context.Note("[IVEngineServer2] server state");

	CS2ST_CALL(context, "IVEngineServer2::GetAppID", {
		// The store app, unlike GetStatsAppID above which reports the content
		// app. Two different numbers from two neighbouring calls is correct.
		const uint32 app_id = engine_->GetAppID();
		context.Detail("-> %u", app_id);
		if (app_id != kCounterStrike2AppId && !IsKnownStatsAppId(app_id))
		{
			context.Fail("expected the store app %u", kCounterStrike2AppId);
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::GetBuildVersion", {
		const int build = engine_->GetBuildVersion();
		context.Detail("-> %d", build);
		if (build <= 0)
		{
			context.Fail("build version should be positive");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::GetSteamUniverse", {
		const int universe = static_cast<int>(engine_->GetSteamUniverse());
		context.Detail("-> %d%s", universe,
			universe == kUniversePublic ? " (public)" : "");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsDedicatedServer", {
		context.Detail("-> %s", engine_->IsDedicatedServer() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsHLTVRelay", {
		context.Detail("-> %s", engine_->IsHLTVRelay() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsServerLocalOnly", {
		context.Detail("-> %s", engine_->IsServerLocalOnly() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsLowViolence", {
		context.Detail("-> %s", engine_->IsLowViolence() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsAnyClientLowViolence", {
		context.Detail("-> %s", engine_->IsAnyClientLowViolence() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsLogEnabled", {
		context.Detail("-> %s", engine_->IsLogEnabled() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetGameDir", {
		// Heap allocation disabled: a fault mid-call skips every destructor
		// between there and the guard, and a stack-only buffer cannot leak.
		CBufferStringN<256> game_dir(false);
		engine_->GetGameDir(game_dir);
		context.Detail("-> \"%s\"", game_dir.Get());
		if (game_dir.Get()[0] == '\0')
		{
			context.Fail("game directory came back empty");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::GetFrameTimeAmnesty", {
		const char* amnesty = engine_->GetFrameTimeAmnesty(true);
		context.Detail("-> %s", amnesty != nullptr ? amnesty : "(null)");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetServerGlobals", {
		globals_ = engine_->GetServerGlobals();
		if (globals_ == nullptr)
		{
			context.Fail("engine always has globals once it is running");
			return;
		}
		context.Detail("-> %p tick=%d maxClients=%d curtime=%.2f",
			static_cast<void*>(globals_), globals_->tickcount,
			globals_->maxClients, globals_->curtime);
	});

	CS2ST_CALL(context, "IVEngineServer2::GetGameServerSteamID", {
		const CSteamID steam_id = engine_->GetGameServerSteamID();
		context.Detail("-> %llu",
			static_cast<unsigned long long>(steam_id.ConvertToUint64()));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetSharedEdictChangeInfo", {
		context.Detail("-> %p", static_cast<void*>(engine_->GetSharedEdictChangeInfo()));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetAchievementMgr", {
		context.Detail("-> %p", static_cast<void*>(engine_->GetAchievementMgr()));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetGamestatsData", {
		context.Detail("-> %p", static_cast<void*>(engine_->GetGamestatsData()));
	});

	CS2ST_CALL(context, "IVEngineServer2::IsMapValid", {
		// A name no map will ever have: a true here means the function is not
		// the one we think it is.
		const int valid = engine_->IsMapValid("cs2sdktest_no_such_map");
		context.Detail("-> %d for a nonexistent map", valid);
		if (valid != 0)
		{
			context.Fail("a map that does not exist reported valid");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::IsGenericPrecached", {
		context.Detail("-> %s for a nonexistent resource",
			engine_->IsGenericPrecached("cs2sdktest/no_such_resource") ? "true" : "false");
	});
}

// -----------------------------------------------------------------------------
//  IVEngineServer2 - per-player reads
// -----------------------------------------------------------------------------

void Source2EngineSuite::ExercisePlayerQueries(TestContext& context)
{
	context.Note("[IVEngineServer2] player queries (slot %d)", kProbeSlot);

	if (globals_ == nullptr)
	{
		// Without globals there is no way to know the slot range is even sane,
		// and handing the engine an out-of-range slot is a good way to crash
		// for a reason that says nothing about the SDK.
		context.SkipCall("IVEngineServer2::<player queries>",
			"GetServerGlobals did not return; slot range unknown");
		return;
	}
	if (globals_->maxClients <= kProbeSlot)
	{
		context.SkipCall("IVEngineServer2::<player queries>",
			"maxClients is %d; slot %d is out of range", globals_->maxClients, kProbeSlot);
		return;
	}

	// The slot need not be occupied. These are all reads, and the answer for an
	// empty slot is as good a probe as the answer for a full one - what is
	// being checked is that the call returns at all and returns something of
	// the right shape.
	const CPlayerSlot slot(kProbeSlot);

	CS2ST_CALL(context, "IVEngineServer2::GetPlayerUserId", {
		context.Detail("-> %d", engine_->GetPlayerUserId(slot).Get());
	});

	CS2ST_CALL(context, "IVEngineServer2::GetPlayerNetworkIDString", {
		const char* id = engine_->GetPlayerNetworkIDString(slot);
		context.Detail("-> %s", id != nullptr ? id : "(null)");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetPlayerNetInfo", {
		context.Detail("-> %p", static_cast<void*>(engine_->GetPlayerNetInfo(slot)));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientSteamID", {
		const CSteamID* id = engine_->GetClientSteamID(slot);
		context.Detail("-> %p", static_cast<const void*>(id));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientXUID", {
		context.Detail("-> %llu",
			static_cast<unsigned long long>(engine_->GetClientXUID(slot)));
	});

	CS2ST_CALL(context, "IVEngineServer2::IsClientFullyAuthenticated", {
		context.Detail("-> %s",
			engine_->IsClientFullyAuthenticated(slot) ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsClientLowViolence", {
		context.Detail("-> %s", engine_->IsClientLowViolence(slot) ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsSplitScreenPlayer", {
		context.Detail("-> %s", engine_->IsSplitScreenPlayer(slot) ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetNumSplitScreenUsersAttachedToEdict", {
		context.Detail("-> %d", engine_->GetNumSplitScreenUsersAttachedToEdict(slot));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetSplitScreenPlayerAttachToEdict", {
		context.Detail("-> %p",
			static_cast<void*>(engine_->GetSplitScreenPlayerAttachToEdict(slot)));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientConVarValue", {
		const char* value = engine_->GetClientConVarValue(slot, "name");
		context.Detail("-> name=%s", value != nullptr ? value : "(null)");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientListening", {
		context.Detail("-> %s",
			engine_->GetClientListening(slot, slot) ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsUserIDInUse", {
		// -1 is never a live user id, so false is the only correct answer.
		const bool in_use = engine_->IsUserIDInUse(-1);
		context.Detail("-> %s for userID -1", in_use ? "true" : "false");
		if (in_use)
		{
			context.Fail("userID -1 reported in use");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::GetLoadingProgressForUserID", {
		context.Detail("-> %d for userID -1", engine_->GetLoadingProgressForUserID(-1));
	});

	context.SkipCall("IVEngineServer2::GetPlayerInfo",
		"needs a protobuf message to fill; not wired up yet");
	context.SkipCall("IVEngineServer2::Message_DetermineMulticastRecipients",
		"needs a live PVS origin to mean anything");
}

// -----------------------------------------------------------------------------
//  IVEngineServer2 - spawn groups
// -----------------------------------------------------------------------------

void Source2EngineSuite::ExerciseSpawnGroups(TestContext& context)
{
	context.Note("[IVEngineServer2] spawn groups");

	CS2ST_CALL(context, "IVEngineServer2::IsSpawnGroupLoaded", {
		context.Detail("-> %s for handle %u",
			engine_->IsSpawnGroupLoaded(kUnknownSpawnGroup) ? "true" : "false",
			kUnknownSpawnGroup);
	});

	CS2ST_CALL(context, "IVEngineServer2::IsSpawnGroupLoading", {
		context.Detail("-> %s for handle %u",
			engine_->IsSpawnGroupLoading(kUnknownSpawnGroup) ? "true" : "false",
			kUnknownSpawnGroup);
	});

	CS2ST_CALL(context, "IVEngineServer2::FindSpawnGroupByName", {
		context.Detail("-> %u for a nonexistent name",
			engine_->FindSpawnGroupByName("cs2sdktest_no_such_group"));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetPVSForSpawnGroup", {
		context.Detail("-> %p", engine_->GetPVSForSpawnGroup(kUnknownSpawnGroup));
	});

	context.SkipCall("IVEngineServer2::UnloadSpawnGroup", "unloads level content");
	context.SkipCall("IVEngineServer2::MakeSpawnGroupActive", "changes what is live");
	context.SkipCall("IVEngineServer2::SynchronouslySpawnGroup", "spawns entities");
	context.SkipCall("IVEngineServer2::SynchronizeAndBlockUntilLoaded", "blocks the server");
	context.SkipCall("IVEngineServer2::SetSpawnGroupDescription", "mutates spawn group state");
}

// -----------------------------------------------------------------------------
//  CGlobalVars - the one struct this interface hands out
// -----------------------------------------------------------------------------

void Source2EngineSuite::InspectGlobalVars(TestContext& context)
{
	context.Note("[CGlobalVars] struct layout - two passes, one per declaring "
	             "class: %zu bytes of CGlobalVarsBase then %zu of CGlobalVars",
		sizeof(CGlobalVarsBase), sizeof(CGlobalVars));

	if (globals_ == nullptr)
	{
		context.SkipCall("CGlobalVars", "GetServerGlobals did not return an instance");
		return;
	}

	// One inspector per declaring class, base first. Splitting them is what
	// puts CGlobalVarsBase in the struct-coverage registry under its own name -
	// merged into the derived pass it is invisible there, and the next suite to
	// need it cannot tell it is already covered.
	//
	// The base pass is aimed at the CGlobalVarsBase subobject through a cast
	// rather than at globals_ directly. Same address here (single inheritance,
	// primary base), but the cast is what stays correct if the chain ever grows
	// a second base.
	StructInspector base(context, "CGlobalVarsBase",
		static_cast<const CGlobalVarsBase*>(globals_), sizeof(CGlobalVarsBase));

	// Fields are listed in declaration order so the log reads down the struct;
	// that is what makes "the first failure is where it diverged" legible.
	// Offsets come from offsetof() on purpose - see StructInspector.h.

	base.Float ("realtime",       offsetof(CGlobalVarsBase, realtime));
	base.Int32 ("framecount",     offsetof(CGlobalVarsBase, framecount), 0, INT32_MAX);
	base.Float ("absoluteframetime",
	                                 offsetof(CGlobalVarsBase, absoluteframetime));
	base.Float ("absoluteframestarttimestddev",
	                                 offsetof(CGlobalVarsBase, absoluteframestarttimestddev));
	// A client count cannot be negative - that is the type talking, not the
	// game. No upper bound here: server limits are a policy question, and
	// encoding one would make this check stop being about layout.
	base.Int32 ("maxClients",     offsetof(CGlobalVarsBase, maxClients), 0, INT32_MAX);

	// The five m_unk fields are reverse-engineering placeholders: somebody knew
	// how many bytes sat between two identified fields and filled the gap with
	// ints. Reported, never judged - they have no type to be wrong about, and
	// this is exactly the region where an inserted field would hide.
	base.Int32 ("m_unk001",       offsetof(CGlobalVarsBase, m_unk001));
	base.Int32 ("m_unk002",       offsetof(CGlobalVarsBase, m_unk002));
	base.Int32 ("m_unk003",       offsetof(CGlobalVarsBase, m_unk003));
	base.Int32 ("m_unk004",       offsetof(CGlobalVarsBase, m_unk004));
	base.Int32 ("m_unk005",       offsetof(CGlobalVarsBase, m_unk005));

	base.Pointer("m_pfnWarningFunc",
	                                 offsetof(CGlobalVarsBase, m_pfnWarningFunc));

	base.Float ("curtime",        offsetof(CGlobalVarsBase, curtime));
	base.Float ("frametime",      offsetof(CGlobalVarsBase, frametime));
	base.Float ("m_unk101",       offsetof(CGlobalVarsBase, m_unk101));
	base.Float ("m_unk102",       offsetof(CGlobalVarsBase, m_unk102));

	base.Bool  ("m_bInSimulation",
	                                 offsetof(CGlobalVarsBase, m_bInSimulation));
	// Observed holding 0xFF on a live server. CGlobalVarsBase's constructor is
	// empty, so any field the engine never writes keeps whatever the allocator
	// left there - and a release server has no reason to touch an assertion
	// switch. The surrounding fields are the evidence that this is stale memory
	// rather than misalignment: tickcount at 0x44 and mapname at 0x60 both read
	// correctly, and a layout that had slipped at 0x41 could not repair itself
	// by 0x44.
	base.Bool  ("m_bEnableAssertions",
	                                 offsetof(CGlobalVarsBase, m_bEnableAssertions),
	                                 kMayBeUninitialized);

	base.Int32 ("tickcount",      offsetof(CGlobalVarsBase, tickcount), 0, INT32_MAX);
	base.Int32 ("m_unk201",       offsetof(CGlobalVarsBase, m_unk201));
	base.Int32 ("m_unk202",       offsetof(CGlobalVarsBase, m_unk202));

	base.Float ("m_flSubtickFraction",
	                                 offsetof(CGlobalVarsBase, m_flSubtickFraction));
	base.Bool  ("m_unk301",       offsetof(CGlobalVarsBase, m_unk301));
	base.Raw   ("m_nThreadId",    offsetof(CGlobalVarsBase, m_nThreadId),
	                                 sizeof(ThreadId_t));

	base.Summarize();

	// The derived pass carries only what CGlobalVars declares itself. string_t
	// on this branch is a struct wrapping a single const char*, so its memory is
	// a pointer and String() applies.
	StructInspector inspect(context, "CGlobalVars", globals_, sizeof(CGlobalVars));
	inspect.String("mapname",        offsetof(CGlobalVars, mapname));
	inspect.String("startspot",      offsetof(CGlobalVars, startspot));
	inspect.Enum  ("eLoadType",      offsetof(CGlobalVars, eLoadType),
	                                 MapLoad_NewGame, MapLoad_Background);
	inspect.Bool  ("mp_teamplay",    offsetof(CGlobalVars, mp_teamplay));
	inspect.Int32 ("maxEntities",    offsetof(CGlobalVars, maxEntities), 0, INT32_MAX);
	inspect.Int32 ("serverCount",    offsetof(CGlobalVars, serverCount), 0, INT32_MAX);

	inspect.Summarize();

	// Printed last, and only here: this dump covers the whole object, so the
	// base pass has no reason to repeat its prefix.
	inspect.HexDump();
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void Source2EngineSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[IVEngineServer2] not called - would change server state");

	// Listed rather than omitted. A reader needs to be able to tell "this was
	// checked and is fine" from "nobody ever looked at this", and an absent
	// row cannot say which.
	context.SkipCall("IVEngineServer2::ChangeLevel", "changes the map");
	context.SkipCall("IVEngineServer2::ServerCommand", "executes console commands");
	context.SkipCall("IVEngineServer2::ClientCommand", "executes commands as a client");
	context.SkipCall("IVEngineServer2::ClientCommandKeyValues", "sends a command to a client");
	context.SkipCall("IVEngineServer2::ClientPrintf", "writes to a player's console");
	context.SkipCall("IVEngineServer2::CreateFakeClient", "adds a bot");
	context.SkipCall("IVEngineServer2::DisconnectClient", "kicks a player");
	context.SkipCall("IVEngineServer2::KickClient", "kicks a player");
	context.SkipCall("IVEngineServer2::BanClient", "bans a player");
	context.SkipCall("IVEngineServer2::SetHLTVChatBan", "mutes an HLTV slot");
	context.SkipCall("IVEngineServer2::LogPrint", "writes to the server log");
	context.SkipCall("IVEngineServer2::PrecacheGeneric", "loads a resource");
	context.SkipCall("IVEngineServer2::SetTimescale", "changes game speed");
	context.SkipCall("IVEngineServer2::SetFrameTimeAmnesty", "changes frame timing");
	context.SkipCall("IVEngineServer2::SetClientUpdateRate", "changes a client's rate");
	context.SkipCall("IVEngineServer2::SetClientListening", "changes voice routing");
	context.SkipCall("IVEngineServer2::SetClientProximity", "changes voice routing");
	context.SkipCall("IVEngineServer2::SetFakeClientConVarValue", "mutates a bot's convars");
	context.SkipCall("IVEngineServer2::SetGamestatsData", "replaces the stats container");
	context.SkipCall("IVEngineServer2::SetAchievementMgr", "replaces the achievement manager");
	context.SkipCall("IVEngineServer2::SetDedicatedServerBenchmarkMode", "changes tick pacing");
	context.SkipCall("IVEngineServer2::ShowFrameTimeReport", "parameter meanings unconfirmed");
	context.SkipCall("IVEngineServer2::DumpNetStats", "parameter meanings unconfirmed");

	context.Note("plus 30 unnamed slots (unk001-unk609) the SDK has not identified");
}

} // namespace cs2st
