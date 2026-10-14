#include "suites/ServerConfigSuite.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <appframework/iappsystem.h>
#include <const.h>
#include <engine2/isource2serverconfig.h>
#include <icvar.h>
#include <interfaces/interfaces.h>
#include <steam/steamuniverse.h>
#include <tier0/buffer_string.h>
#include <tier0/utlstring.h>
#include <tier0/utlvector.h>

#include "core/Log.h"
#include "testkit/PrivateField.h"

// The one private member this suite reads directly. CUtlString's Get() folds
// null and empty together (both render as ""); reading m_pString itself is
// the only way to tell "the callee did not write" from "the callee wrote an
// empty list" - two different facts about the same out-parameter.
CS2ST_PRIVATE_FIELD(kUtlStringM_pString, CUtlString, char*, m_pString);

namespace cs2st
{
namespace
{

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

const char* LoopModeName(HostStateLoopModeType_t mode)
{
	switch (mode)
	{
		case HOST_STATE_LOOP_MODE_IDLE:             return "IDLE";
		case HOST_STATE_LOOP_MODE_GAME:             return "GAME";
		case HOST_STATE_LOOP_MODE_SOURCETV_RELAY:   return "SOURCETV_RELAY";
		default:                                    return "<out of range>";
	}
}

/**
 * GetHostStateLoopModeInfo for one loop mode, as its own guarded row. The mode
 * goes into the row label as well as the detail: three rows sharing one label
 * would be indistinguishable in a diff, and which mode was being asked is the
 * one fact that separates them.
 *
 * The out-parameters get real destinations, declared inside the guarded body:
 * a callee that writes them writes to our stack, and one that reads them
 * finds zeros rather than a sentinel we invented. A fake pointer here would
 * make any crash in the row ours rather than the callee's - the opposite of
 * what a probe is for.
 */
void ProbeLoopMode(TestContext& context, ISource2ServerConfig* config,
                   HostStateLoopModeType_t mode)
{
	// Built outside the macro argument: CS2ST_CALL is a two-argument macro, so
	// a function call inside the target string's argument list would split on
	// its comma before the compiler ever saw the format string. check-macro-
	// commas.sh exists for exactly this shape.
	char label[96];
	std::snprintf(label, sizeof(label),
		"ISource2ServerConfig::GetHostStateLoopModeInfo [%s]", LoopModeName(mode));

	CS2ST_CALL(context, label, {
		CUtlString loop_mode_name;
		KeyValues* loop_mode_options = nullptr;
		config->GetHostStateLoopModeInfo(mode, loop_mode_name, &loop_mode_options);
		context.Detail("-> name \"%s\", options %p",
			loop_mode_name.Get() != nullptr ? loop_mode_name.Get() : "(null)",
			static_cast<void*>(loop_mode_options));
	});
}

/**
 * ICvar as a tool, not a subject (ConVarSuite owns it): the tick-rate convar
 * is the second source for the tick-interval cross-check. tier0 first then
 * anywhere, mirroring the lookup order the other "tool" uses share.
 */
ICvar* Cvar(TestContext& context)
{
	ICvar* cvar = static_cast<ICvar*>(
		context.QueryInterface("tier0", CVAR_INTERFACE_VERSION));
	if (cvar == nullptr)
	{
		cvar = static_cast<ICvar*>(context.QueryInterfaceAnywhere(CVAR_INTERFACE_VERSION));
	}
	return cvar;
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool ServerConfigSuite::Acquire(TestContext& context)
{
	config_ = static_cast<ISource2ServerConfig*>(
		context.QueryInterface("server", INTERFACEVERSION_SERVERCONFIG));

	if (config_ == nullptr)
	{
		const Module& server_module = context.GetModule("server");
		if (!server_module.IsValid())
		{
			context.UnavailableCall("ISource2ServerConfig", "the server module is not loaded");
		}
		else
		{
			context.UnavailableCall("ISource2ServerConfig",
				"server is loaded but does not answer '%s'",
				INTERFACEVERSION_SERVERCONFIG);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(config_),
		INTERFACEVERSION_SERVERCONFIG);
	return true;
}

void ServerConfigSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseConfigQueries(context);
	ExerciseTailSlots(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void ServerConfigSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = config_->GetTier();
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
		const bool singleton = config_->IsSingleton();
		context.Detail("-> %s", singleton ? "true" : "false");
		if (!singleton)
		{
			context.Fail("the server config is a singleton; false here suggests a "
			             "slot shift");
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = config_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = config_->GetDependencies();
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
		void* self = config_->QueryInterface(INTERFACEVERSION_SERVERCONFIG);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(config_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run game DLL startup");
	context.SkipCall("IAppSystem::Init", "would re-run game DLL startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin game DLL shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the game DLL down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down game DLL interfaces");
}

// -----------------------------------------------------------------------------
//  The config queries
//
//  The split is the same one every suite here draws: type-level contracts are
//  judged (a tick interval is a finite positive subsecond float; a player
//  limit is a count inside [1, ABSOLUTE_PLAYER_LIMIT]), semantic guesses are
//  not (which bool the game DLL returns for "allow takeover of bots" is the
//  game's policy, and the SDK attaches no contract to it).
// -----------------------------------------------------------------------------

void ServerConfigSuite::ExerciseConfigQueries(TestContext& context)
{
	context.Note("[ISource2ServerConfig] the startup questions; the tick interval "
	             "is what NetworkServerSuite's GetTime formula consumes");

	CS2ST_CALL(context, "ISource2ServerConfig::GetGameDescription", {
		const char* description = config_->GetGameDescription();
		context.Detail("-> \"%s\"", description != nullptr ? description : "(null)");
		if (description == nullptr)
		{
			// Not Fail: the SDK only promises "a string describing the .dll",
			// and an empty-at-startup description is the engine's business,
			// not a broken declaration. Null is still worth a row of its own.
			context.Suspect("no game description; the browser list shows one");
		}
	});

	CS2ST_CALL(context, "ISource2ServerConfig::GetNetworkVersion", {
		// No second source for this number exists in the SDK, so it is
		// recorded rather than judged. A change in it after an update is the
		// row doing its job.
		const int version = config_->GetNetworkVersion();
		context.Detail("-> %d", version);
	});

	CS2ST_CALL(context, "ISource2ServerConfig::GetPlayerLimits", {
		// Seeded so "did not write" is distinguishable from "wrote a zero":
		// counts have exactly one out-of-domain value, and it is negative.
		int min_players = -31337;
		int max_players = -31337;
		int default_max = -31337;
		bool is_multiplayer = false;
		config_->GetPlayerLimits(min_players, max_players, default_max, is_multiplayer);
		context.Detail("-> min %d, max %d, default %d, multiplayer %s",
			min_players, max_players, default_max,
			is_multiplayer ? "yes" : "no");

		const bool wrote_counts =
			min_players != -31337 && max_players != -31337 && default_max != -31337;
		if (!wrote_counts)
		{
			context.Fail("at least one limit out-parameter was not written");
			return;
		}
		if (min_players < 1 || max_players < 1 || default_max < 1)
		{
			context.Fail("a player limit below 1 is not a limit the engine can act on");
		}
		else if (min_players > ABSOLUTE_PLAYER_LIMIT || max_players > ABSOLUTE_PLAYER_LIMIT
			|| default_max > ABSOLUTE_PLAYER_LIMIT)
		{
			context.Fail("a player limit above ABSOLUTE_PLAYER_LIMIT (%d) exceeds the "
			             "engine-wide slot budget", ABSOLUTE_PLAYER_LIMIT);
		}
	});

	CS2ST_CALL(context, "ISource2ServerConfig::GetMaxSplitscreenPlayers", {
		const int splits = config_->GetMaxSplitscreenPlayers();
		context.Detail("-> %d", splits);
		// [1, ABSOLUTE_PLAYER_LIMIT] is the type-level envelope for any player
		// count; 1 == "no splits", which the SDK documents as the expected
		// value on CS2. Exact value is policy, not judged.
		if (splits < 1 || splits > ABSOLUTE_PLAYER_LIMIT)
		{
			context.Fail("a splitscreen slot count outside [1, %d] is not a count",
				ABSOLUTE_PLAYER_LIMIT);
		}
	});

	CS2ST_CALL(context, "ISource2ServerConfig::GetMaxHumanPlayers", {
		const int humans = config_->GetMaxHumanPlayers();
		// The SDK documents -1 as "can't determine or don't care" - the one
		// sentinel this interface has, which makes it part of the contract.
		context.Detail("-> %d%s", humans, humans == -1 ? " (can't determine)" : "");
		if (humans != -1 && (humans < 1 || humans > ABSOLUTE_PLAYER_LIMIT))
		{
			context.Fail("a human-slot count outside {-1} ∪ [1, %d] is not a count "
			             "this SDK describes", ABSOLUTE_PLAYER_LIMIT);
		}
	});

	CS2ST_CALL(context, "ISource2ServerConfig::ShouldNotifyLocalClientConnectionStateChanges", {
		context.Detail("-> %s",
			config_->ShouldNotifyLocalClientConnectionStateChanges() ? "true" : "false");
	});

	// One row per loop mode rather than one row looping over all three: a
	// fault mid-loop abandons the remaining iterations with no row for them,
	// and which mode was being asked when something went wrong is exactly the
	// attribution a guarded call exists to provide.
	ProbeLoopMode(context, config_, HOST_STATE_LOOP_MODE_IDLE);
	ProbeLoopMode(context, config_, HOST_STATE_LOOP_MODE_GAME);
	ProbeLoopMode(context, config_, HOST_STATE_LOOP_MODE_SOURCETV_RELAY);

	CS2ST_CALL(context, "ISource2ServerConfig::GetConVarPrefixesToResetToDefaults", {
		CUtlString prefixes;
		config_->GetConVarPrefixesToResetToDefaults(prefixes);
		// CUtlString::Get() folds "never written" (m_pString null) and "wrote
		// an empty list" (m_pString -> "") into the same "" - and they are
		// different facts: the second means the callee ran and had nothing to
		// say, the first that this row probably did not reach it. Reading the
		// pointer itself through the SDK's own declaration keeps them apart,
		// the same trick ConVarSuite uses on ConVarData's private members.
		const std::size_t m_pString_offset =
			PrivateFieldOffset<kUtlStringM_pString>(&prefixes);
		if (m_pString_offset == static_cast<std::size_t>(-1))
		{
			// Only possible if the explicit instantiation went missing; the
			// comparison below needs the raw pointer, so say so and stop.
			context.Detail("-> \"%s\" (m_pString unreachable: field not captured)",
				prefixes.Get());
			return;
		}
		const char* raw = nullptr;
		std::memcpy(&raw, reinterpret_cast<const unsigned char*>(&prefixes)
			+ m_pString_offset, sizeof(raw));
		context.Detail("-> \"%s\" (m_pString %p, %s)",
			raw != nullptr ? raw : "",
			static_cast<const void*>(raw),
			raw == nullptr ? "not written" : "written");
		if (raw == nullptr)
		{
			// Not Fail: the SDK promises nothing about the garbage value, and the
			// engine may honestly have no prefixes to reset. But "returned
			// without writing" and "wrote an empty list" deserve different
			// colours, and only the pointer can tell them apart.
			context.Suspect("the out-parameter was not written; an empty reset "
			                "list would have been a written empty string");
		}
	});

	CS2ST_CALL(context, "ISource2ServerConfig::AllowDedicatedServers", {
		// The most plugin-adjacent method on the interface: the Metamod loader
		// intercepts this very interface's Connect() and consults this method
		// (TEST_TARGETS.md item 11's whole reason for existing). On a
		// dedicated server there is exactly one right answer for the public
		// universe, and we know which world we are in - this plugin runs on
		// one, or the sweep would not be running under these conditions.
		const bool allowed = config_->AllowDedicatedServers(k_EUniversePublic);
		context.Detail("-> %s for k_EUniversePublic", allowed ? "true" : "false");
		if (!allowed)
		{
			context.Fail("the game DLL does not allow dedicated servers in the public "
			             "universe, yet this sweep is running on one");
		}
	});

	CS2ST_CALL(context, "ISource2ServerConfig::AllowSaveRestore", {
		context.Detail("-> %s", config_->AllowSaveRestore() ? "true" : "false");
	});
}

// -----------------------------------------------------------------------------
//  The tail slots (22-27) and the factory slot (28)
//
//  Slots 23/24 carry names in the SDK since PR #430 (IsCommandQueueEnabled /
//  GetCommandQueueDilationPercentage), reversing evidence from this repo's own
//  notes: the implementations read the cq_enable / cq_dilation_percentage
//  convars through holders near unk_29B3800 / unk_29B37F0, and the runtime
//  probe agreed (true / 5.0 = the convar defaults). The rest stay unnamed -
//  their implementations are byte-identical mov eax,1 stubs whose identity
//  would have to come from engine-side call sites, which is out of this
//  suite's reach. Each row records the value so a future build repurposing a
//  slot shows up as a diff rather than a silent shift.
//
//  Slot 28 is the ensure-and-register hook: resolve GameEventSystem via the
//  caller's factory on first call (cached in a libserver.so global, ~200
//  readers), then register this DLL's game events (the sub_1779D60 tail).
//  The interface pointer never leaves the DLL - the runtime probe got 0x1
//  back, a bool success code - so the SDK's void* era is over; the current
//  declaration passes no factory at all. Calling it re-runs the registration
//  walk only (idempotent, observed harmless with a warm cache), but it is
//  engine-init territory, so the row records a SkipCall rather than a probe.
// -----------------------------------------------------------------------------

void ServerConfigSuite::ExerciseTailSlots(TestContext& context)
{
	CS2ST_CALL(context, "ISource2ServerConfig::unk101 [slot 22, bool]", {
		context.Detail("-> %s", config_->unk101() ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2ServerConfig::IsCommandQueueEnabled [slot 23, bool]", {
		context.Detail("-> %s", config_->IsCommandQueueEnabled() ? "true" : "false");
	});

	// The one float in the tail. A wrong slot mapping here reads a stale XMM0
	// (the "clean 0.0" from the reversing notes), so the raw bits go into the
	// detail line too: a plausible float with bits 0x00000000 means the callee
	// never touched XMM0. 5.0 is cq_dilation_percentage's default.
	CS2ST_CALL(context, "ISource2ServerConfig::GetCommandQueueDilationPercentage [slot 24, float]", {
		const float value = config_->GetCommandQueueDilationPercentage();
		unsigned int bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		context.Detail("-> %f (bits 0x%08X)", value, bits);
	});

	// Slots 25-27 were unk201-203 until the 2026-10 SDK named them. The row
	// labels keep no memory of that; a diff against an old log shows renames.
	CS2ST_CALL(context, "ISource2ServerConfig::ShouldMapCommandChangeLevel [slot 25, bool]", {
		context.Detail("-> %s", config_->ShouldMapCommandChangeLevel() ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2ServerConfig::ShouldAllocateNewClients [slot 26, bool]", {
		context.Detail("-> %s", config_->ShouldAllocateNewClients() ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2ServerConfig::Uses64TickInterval [slot 27, bool]", {
		context.Detail("-> %s", config_->Uses64TickInterval() ? "true" : "false");
	});

	context.SkipCall("ISource2ServerConfig::InitGameEvents [slot 28]",
		"the game-event ensure-and-register hook; the probe era established it "
		"caches GameEventSystemServerV001 in a server-DLL global and returns a "
		"bool, but its body also walks the event dictionary, which is engine "
		"init - not something a probe should re-run for a value it already "
		"knows how to interpret");
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void ServerConfigSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.SkipCall("ISource2ServerConfig::OnClientFullyConnect",
		"an engine-to-game callback; calling it would tell the game a client "
		"finished connecting when none did");
}

} // namespace cs2st
