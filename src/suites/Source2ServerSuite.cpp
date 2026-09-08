#include "suites/Source2ServerSuite.h"

#include <cstddef>
#include <cstdint>

#include <ISmmPlugin.h>
#include <appframework/IAppSystem.h>
#include <eiface.h>
#include <sourcehook.h>
#include <tier1/utlstring.h>
#include <tier1/utlvector.h>

#include "core/Log.h"
#include "hook/HookRecorder.h"

// SH_ADD_HOOK expands to code that reaches g_SHPtr and g_PLID; these are the
// declarations for them. They are defined once by PLUGIN_EXPOSE in Plugin.cpp.
PLUGIN_GLOBALVARS();

// Hook declarations. The third argument is the method's cv-qualifier list, so
// SH_NOATTRIB is "no qualifiers" and a const method would put `const` there.
// The fourth is the overload index; none of these are overloaded.
SH_DECL_HOOK3_void(ISource2Server, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK1_void(ISource2Server, PreWorldUpdate, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK1_void(ISource2Server, UpdateWhenNotInGame, SH_NOATTRIB, 0, float);
SH_DECL_HOOK1_void(ISource2Server, ServerHibernationUpdate, SH_NOATTRIB, 0, bool);
SH_DECL_HOOK0_void(ISource2Server, GameServerSteamAPIActivated, SH_NOATTRIB, 0);
SH_DECL_HOOK0_void(ISource2Server, GameServerSteamAPIDeactivated, SH_NOATTRIB, 0);
SH_DECL_HOOK1_void(ISource2Server, ApplyGameSettings, SH_NOATTRIB, 0, KeyValues*);
SH_DECL_HOOK2_void(ISource2Server, ServerConVarChanged, SH_NOATTRIB, 0, const char*, const char*);
SH_DECL_HOOK1_void(ISource2Server, OnHostNameChanged, SH_NOATTRIB, 0, const char*);
SH_DECL_HOOK2(ISource2Server, ShouldTimeoutClient, SH_NOATTRIB, 0, bool, int, float);
SH_DECL_HOOK1(ISource2Server, ShouldHideFromMasterServer, SH_NOATTRIB, 0, bool, bool);

namespace cs2st
{
namespace
{

// Hook ids. Used as both the log label and the recorder key, so they are
// written once here rather than repeated at each call site.
constexpr const char* kHookGameFrame        = "ISource2Server::GameFrame";
constexpr const char* kHookPreWorldUpdate   = "ISource2Server::PreWorldUpdate";
constexpr const char* kHookUpdateNotInGame  = "ISource2Server::UpdateWhenNotInGame";
constexpr const char* kHookHibernation      = "ISource2Server::ServerHibernationUpdate";
constexpr const char* kHookSteamActivated   = "ISource2Server::GameServerSteamAPIActivated";
constexpr const char* kHookSteamDeactivated = "ISource2Server::GameServerSteamAPIDeactivated";
constexpr const char* kHookApplyGameSettings = "ISource2Server::ApplyGameSettings";
constexpr const char* kHookConVarChanged    = "ISource2Server::ServerConVarChanged";
constexpr const char* kHookHostNameChanged  = "ISource2Server::OnHostNameChanged";
constexpr const char* kHookShouldTimeout    = "ISource2Server::ShouldTimeoutClient";
constexpr const char* kHookShouldHide       = "ISource2Server::ShouldHideFromMasterServer";

/**
 * Renders a const char* hook argument without risking a fault on a bad one.
 *
 * This is the one genuinely dangerous thing the hook callbacks do, and it is
 * dangerous exactly where it matters most: a changed parameter signature is a
 * primary thing this plugin exists to catch, and when it happens the value
 * arriving in a `const char*` slot is not a pointer at all. A null check does
 * not help - a float's bit pattern is rarely null - so the address is vetted
 * before anything dereferences it.
 *
 * Deliberately not shared with StructInspector's pointer check: that one is
 * about struct layout and requires 8-alignment, which is wrong for strings,
 * and this runs on the tick path where it should stay a handful of compares
 * with no cross-module coupling.
 *
 * Returns something safe to hand to "%s". `scratch` holds the rendering of a
 * rejected pointer, so it must outlive the format call.
 */
template <std::size_t N>
const char* SafeStringArg(const char* text, char (&scratch)[N])
{
	if (text == nullptr)
	{
		return "(null)";
	}

	const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(text);

	// The first 64 KiB is the null guard region, and user-space addresses on
	// x86-64 stay under 2^47. No alignment requirement: string literals sit
	// packed in .rodata on arbitrary bytes.
	if (value < 0x10000 || value >= (std::uintptr_t(1) << 47))
	{
		std::snprintf(scratch, N, "<bad ptr 0x%llx>",
			static_cast<unsigned long long>(value));
		return scratch;
	}

	// A plausible address can still be unmapped, and there is no portable way
	// to find out short of reading it. Callers cap the length with a precision
	// specifier so a missing terminator cannot walk off the page.
	return text;
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

/** Logs the first few entries of a string list the game filled in for us. */
void NoteListPreview(TestContext& context, const CUtlVector<CUtlString>& list,
                     int max_shown = 6)
{
	const int count = list.Count();
	for (int i = 0; i < count && i < max_shown; ++i)
	{
		context.Note("    [%d] %s", i, list[i].Get());
	}
	if (count > max_shown)
	{
		context.Note("    ... and %d more", count - max_shown);
	}
}

} // namespace

bool Source2ServerSuite::Acquire(TestContext& context)
{
	server_ = static_cast<ISource2Server*>(
		context.QueryInterface("server", INTERFACEVERSION_SERVERGAMEDLL));

	if (server_ == nullptr)
	{
		const Module& server_module = context.GetModule("server");
		if (!server_module.IsValid())
		{
			context.UnavailableCall("ISource2Server", "the server module is not loaded");
		}
		else
		{
			context.UnavailableCall("ISource2Server",
				"server is loaded but does not answer '%s'",
				INTERFACEVERSION_SERVERGAMEDLL);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(server_),
		INTERFACEVERSION_SERVERGAMEDLL);
	return true;
}

void Source2ServerSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseServerState(context);
	ExerciseEnumerations(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void Source2ServerSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = server_->GetTier();
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
		const bool singleton = server_->IsSingleton();
		context.Detail("-> %s", singleton ? "true" : "false");
		if (!singleton)
		{
			context.Fail("the game DLL is a singleton; false here suggests a slot shift");
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = server_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = server_->GetDependencies();
		if (deps == nullptr)
		{
			context.Detail("-> none");
			return;
		}

		// Capped: walking a garbage pointer until it happens to hit two nulls
		// turns a probe into a hang instead of a crash.
		int count = 0;
		while (count < 64 && deps[count].m_pModuleName != nullptr)
		{
			++count;
		}
		context.Detail("-> %p, %d entr%s%s", static_cast<const void*>(deps),
			count, count == 1 ? "y" : "ies", count == 64 ? " (capped)" : "");
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface", {
		void* self = server_->QueryInterface(INTERFACEVERSION_SERVERGAMEDLL);
		context.Detail("-> %p%s", self,
			(self == static_cast<void*>(server_)) ? " (itself)" : "");
	});

	context.SkipCall("IAppSystem::Connect", "would re-run game DLL startup");
	context.SkipCall("IAppSystem::Init", "would re-run game DLL startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin game DLL shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the game DLL down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down game DLL interfaces");
}

// -----------------------------------------------------------------------------
//  Read-only server state
// -----------------------------------------------------------------------------

void Source2ServerSuite::ExerciseServerState(TestContext& context)
{
	context.Note("[ISource2Server] server state");

	CS2ST_CALL(context, "ISource2Server::IsPaused", {
		context.Detail("-> %s", server_->IsPaused() ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Server::GetActiveWorldName", {
		const char* world = server_->GetActiveWorldName();
		context.Detail("-> %s", world != nullptr ? world : "(null)");
	});

	CS2ST_CALL(context, "ISource2Server::GetServerGameDLLFlags", {
		context.Detail("-> 0x%08x", server_->GetServerGameDLLFlags());
	});

	CS2ST_CALL(context, "ISource2Server::unk_001", {
		// Unidentified in the SDK, but const and nullary so it is safe to ask.
		// Recording the answer is the point: if it ever changes, somebody has a
		// starting clue about what it does.
		context.Detail("-> %s (unidentified in the SDK)",
			server_->unk_001() ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Server::ShouldHideFromMasterServer", {
		context.Detail("-> %s (asked as if the server had no password)",
			server_->ShouldHideFromMasterServer(false) ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Server::ShouldTimeoutClient", {
		// userID -1 is never a live client, so this is a query about nobody.
		context.Detail("-> %s for userID -1 at 0s",
			server_->ShouldTimeoutClient(-1, 0.0f) ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Server::GetMatchmakingTags", {
		char tags[256] = {0};
		server_->GetMatchmakingTags(tags, sizeof(tags));
		context.Detail("-> \"%s\"", tags);
	});

	CS2ST_CALL(context, "ISource2Server::GetMatchmakingGameData", {
		// Heap allocation disabled: a fault mid-call skips every destructor
		// between there and the guard, and a stack-only buffer cannot leak.
		CBufferStringN<512> data(false);
		server_->GetMatchmakingGameData(data);
		context.Detail("-> \"%s\"", data.Get());
	});

	CS2ST_CALL(context, "ISource2Server::GetServerGCLobby", {
		context.Detail("-> %p", static_cast<void*>(server_->GetServerGCLobby()));
	});

	CS2ST_CALL(context, "ISource2Server::GetEntityInfo", {
		context.Detail("-> %p", server_->GetEntityInfo());
	});

	CS2ST_CALL(context, "ISource2Server::GetEntity2Networkables", {
		context.Detail("-> %p", static_cast<const void*>(server_->GetEntity2Networkables()));
	});

	CS2ST_CALL(context, "ISource2Server::GetAllServerClasses", {
		CUtlVector<ServerClass*>* classes = server_->GetAllServerClasses();
		if (classes == nullptr)
		{
			context.Detail("-> (null)");
			return;
		}
		context.Detail("-> %p, %d class(es)", static_cast<void*>(classes),
			classes->Count());
	});

	CS2ST_CALL(context, "ISource2Server::GetSpawnDebugInterface", {
		context.Detail("-> %p", server_->GetSpawnDebugInterface());
	});

	CS2ST_CALL(context, "ISource2Server::GetToolGameSimulationAPI", {
		context.Detail("-> %p", static_cast<void*>(server_->GetToolGameSimulationAPI()));
	});

	CS2ST_CALL(context, "ISource2Server::GetEconItemSystem", {
		context.Detail("-> %p", server_->GetEconItemSystem());
	});

	CS2ST_CALL(context, "ISource2Server::FilterPlayerCounts", {
		// Writes through its arguments rather than to server state, so the
		// only thing mutated is our own stack.
		int humans = 0;
		int human_slots = 0;
		int bots = 0;
		server_->FilterPlayerCounts(&humans, &human_slots, &bots);
		context.Detail("-> humans=%d slots=%d bots=%d", humans, human_slots, bots);
	});
}

// -----------------------------------------------------------------------------
//  List-returning calls
//
//  These are the most valuable calls in the interface for this project's
//  purpose. Each one makes the game enumerate a set the SDK has a matching
//  constant or enum for - hit groups, activities, characters - so the *contents*
//  are a direct read of something a game update can change out from under a
//  plugin, without any interface version moving at all.
// -----------------------------------------------------------------------------

void Source2ServerSuite::ExerciseEnumerations(TestContext& context)
{
	context.Note("[ISource2Server] enumerations - contents are the finding here");

	CS2ST_CALL(context, "ISource2Server::GetHitGroupEnumInfo", {
		CUtlVector<int> values;
		CUtlVector<CUtlString> names;
		server_->GetHitGroupEnumInfo(values, names);

		context.Detail("-> %d value(s), %d name(s)", values.Count(), names.Count());
		if (values.Count() != names.Count())
		{
			context.Fail("values and names disagree in length");
			return;
		}
		for (int i = 0; i < values.Count() && i < 16; ++i)
		{
			context.Note("    %2d = %s", values[i], names[i].Get());
		}
	});

	CS2ST_CALL(context, "ISource2Server::GetAnimationActivityList", {
		CUtlVector<CUtlString> activities;
		server_->GetAnimationActivityList(activities);
		context.Detail("-> %d activit%s", activities.Count(),
			activities.Count() == 1 ? "y" : "ies");
		NoteListPreview(context, activities);
	});

	CS2ST_CALL(context, "ISource2Server::GetAnimationEventList", {
		CUtlVector<CUtlString> events;
		server_->GetAnimationEventList(events);
		context.Detail("-> %d event(s)", events.Count());
		NoteListPreview(context, events);
	});

	CS2ST_CALL(context, "ISource2Server::GetCharacterList", {
		CUtlVector<CUtlString> characters;
		server_->GetCharacterList(characters);
		context.Detail("-> %d character(s)", characters.Count());
		NoteListPreview(context, characters);
	});
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void Source2ServerSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[ISource2Server] not called - would change server state");

	// Listed rather than omitted: a reader needs to tell "checked and fine"
	// from "nobody ever looked", and an absent row cannot say which.
	context.SkipCall("ISource2Server::GameFrame",
		"advances the world by a tick - hooked instead");
	context.SkipCall("ISource2Server::PreWorldUpdate",
		"advances the world - hooked instead");
	context.SkipCall("ISource2Server::UpdateWhenNotInGame",
		"advances the idle loop - hooked instead");
	context.SkipCall("ISource2Server::SetGlobals",
		"replaces the engine's CGlobalVars pointer");
	context.SkipCall("ISource2Server::GameCreateNetworkStringTables",
		"creates string tables");
	context.SkipCall("ISource2Server::WriteSignonMessages",
		"writes into a network buffer");
	context.SkipCall("ISource2Server::ApplyGameSettings",
		"applies lobby settings - hooked instead");
	context.SkipCall("ISource2Server::ServerHibernationUpdate",
		"changes hibernation state - hooked instead");
	context.SkipCall("ISource2Server::SetNavMeshData", "replaces the nav mesh");
	context.SkipCall("ISource2Server::RegisterNavListener", "mutates the listener chain");
	context.SkipCall("ISource2Server::UnregisterNavListener", "mutates the listener chain");
	context.SkipCall("ISource2Server::GameServerSteamAPIActivated",
		"changes Steam API state - hooked instead");
	context.SkipCall("ISource2Server::GameServerSteamAPIDeactivated",
		"changes Steam API state - hooked instead");
	context.SkipCall("ISource2Server::OnHostNameChanged",
		"changes the host name - hooked instead");
	context.SkipCall("ISource2Server::PreFatalShutdown", "prepares the server to die");
	context.SkipCall("ISource2Server::ServerConVarChanged",
		"runs convar change handling - hooked instead");
	context.SkipCall("ISource2Server::unk_201", "unidentified in the SDK");

	// Callable in principle, but they need a structure we cannot honestly
	// build yet. Separated from the side-effecting list so the reason is clear.
	context.SkipCall("ISource2Server::GetNavMeshData", "needs a CNavData to fill");
	context.SkipCall("ISource2Server::GetTaggedConVarList", "needs a KeyValues to fill");
	context.SkipCall("ISource2Server::PrintStatus", "needs a valid entity index");
	context.SkipCall("ISource2Server::GetEconItemNamesForModel", "needs a real model name");
	context.SkipCall("ISource2Server::GetEconItemNamesForCharacter",
		"needs a real character name");
	context.SkipCall("ISource2Server::GetEconItemsInfoForModel", "needs a real model name");
	context.SkipCall("ISource2Server::GetEconItemsInfoForCharacter",
		"needs a real character name");
	context.SkipCall("ISource2Server::GetDefaultScaleForModel", "needs a real model name");
	context.SkipCall("ISource2Server::GetDefaultScaleForCharacter",
		"needs a real character name");
	context.SkipCall("ISource2Server::GetDefaultControlPointAutoUpdates",
		"needs a real particle system name");
	context.SkipCall("ISource2Server::GetCharacterNameForModel", "needs a real model name");
	context.SkipCall("ISource2Server::GetModelNameForCharacter",
		"needs a real character name");
	context.SkipCall("ISource2Server::GetDefaultChoreoDirForModel", "needs a real model name");
}

// -----------------------------------------------------------------------------
//  Hooks
// -----------------------------------------------------------------------------

int Source2ServerSuite::InstallHooks(TestContext& context)
{
	if (server_ == nullptr || hooks_installed_)
	{
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before installing so a hook that never fires still shows up in
	// the dump as "never". That row - a callback the engine has stopped making
	// - is the single most informative line after an update, and a silently
	// missing one says nothing at all.
	recorder.Declare(kHookGameFrame, Id());
	recorder.Declare(kHookPreWorldUpdate, Id());
	recorder.Declare(kHookUpdateNotInGame, Id());
	recorder.Declare(kHookHibernation, Id());
	recorder.Declare(kHookSteamActivated, Id());
	recorder.Declare(kHookSteamDeactivated, Id());
	recorder.Declare(kHookApplyGameSettings, Id());
	recorder.Declare(kHookConVarChanged, Id());
	recorder.Declare(kHookHostNameChanged, Id());
	recorder.Declare(kHookShouldTimeout, Id());
	recorder.Declare(kHookShouldHide, Id());

	// Each one twice: pre (false) records the entry, post (true) records the
	// return. See the header for why both.
#define CS2ST_ADD_PAIR(method) \
	SH_ADD_HOOK(ISource2Server, method, server_, \
		SH_MEMBER(this, &Source2ServerSuite::Pre_##method), false); \
	SH_ADD_HOOK(ISource2Server, method, server_, \
		SH_MEMBER(this, &Source2ServerSuite::Post_##method), true)

	CS2ST_ADD_PAIR(GameFrame);
	CS2ST_ADD_PAIR(PreWorldUpdate);
	CS2ST_ADD_PAIR(UpdateWhenNotInGame);
	CS2ST_ADD_PAIR(ServerHibernationUpdate);
	CS2ST_ADD_PAIR(GameServerSteamAPIActivated);
	CS2ST_ADD_PAIR(GameServerSteamAPIDeactivated);
	CS2ST_ADD_PAIR(ApplyGameSettings);
	CS2ST_ADD_PAIR(ServerConVarChanged);
	CS2ST_ADD_PAIR(OnHostNameChanged);
	CS2ST_ADD_PAIR(ShouldTimeoutClient);
	CS2ST_ADD_PAIR(ShouldHideFromMasterServer);

#undef CS2ST_ADD_PAIR

	hooks_installed_ = true;
	(void)context;
	return 11;
}

void Source2ServerSuite::RemoveHooks(TestContext& context)
{
	if (server_ == nullptr || !hooks_installed_)
	{
		return;
	}

#define CS2ST_REMOVE_PAIR(method) \
	SH_REMOVE_HOOK(ISource2Server, method, server_, \
		SH_MEMBER(this, &Source2ServerSuite::Pre_##method), false); \
	SH_REMOVE_HOOK(ISource2Server, method, server_, \
		SH_MEMBER(this, &Source2ServerSuite::Post_##method), true)

	CS2ST_REMOVE_PAIR(GameFrame);
	CS2ST_REMOVE_PAIR(PreWorldUpdate);
	CS2ST_REMOVE_PAIR(UpdateWhenNotInGame);
	CS2ST_REMOVE_PAIR(ServerHibernationUpdate);
	CS2ST_REMOVE_PAIR(GameServerSteamAPIActivated);
	CS2ST_REMOVE_PAIR(GameServerSteamAPIDeactivated);
	CS2ST_REMOVE_PAIR(ApplyGameSettings);
	CS2ST_REMOVE_PAIR(ServerConVarChanged);
	CS2ST_REMOVE_PAIR(OnHostNameChanged);
	CS2ST_REMOVE_PAIR(ShouldTimeoutClient);
	CS2ST_REMOVE_PAIR(ShouldHideFromMasterServer);

#undef CS2ST_REMOVE_PAIR

	hooks_installed_ = false;
	(void)context;
}

// -----------------------------------------------------------------------------
//  Hook handlers
//
//  These run inside the server's own frame - GameFrame at the tick rate - so
//  they do the minimum: bump a counter, and format arguments only until the
//  sample quota is full (HookRecorder stops before formatting once it is).
//  Every one ends in RETURN_META / RETURN_META_VALUE, which is how SourceHook
//  hands control back.
// -----------------------------------------------------------------------------

void Source2ServerSuite::Pre_GameFrame(bool simulating, bool first_tick, bool last_tick)
{
	CS2ST_HOOK_ENTER_ARGS(kHookGameFrame, "simulating=%d first=%d last=%d",
		simulating ? 1 : 0, first_tick ? 1 : 0, last_tick ? 1 : 0);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_GameFrame(bool, bool, bool)
{
	CS2ST_HOOK_LEAVE(kHookGameFrame);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Pre_PreWorldUpdate(bool simulating)
{
	CS2ST_HOOK_ENTER_ARGS(kHookPreWorldUpdate, "simulating=%d", simulating ? 1 : 0);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_PreWorldUpdate(bool)
{
	CS2ST_HOOK_LEAVE(kHookPreWorldUpdate);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Pre_UpdateWhenNotInGame(float frame_time)
{
	CS2ST_HOOK_ENTER_ARGS(kHookUpdateNotInGame, "frametime=%.4f",
		static_cast<double>(frame_time));
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_UpdateWhenNotInGame(float)
{
	CS2ST_HOOK_LEAVE(kHookUpdateNotInGame);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Pre_ServerHibernationUpdate(bool hibernating)
{
	CS2ST_HOOK_ENTER_ARGS(kHookHibernation, "hibernating=%d", hibernating ? 1 : 0);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_ServerHibernationUpdate(bool)
{
	CS2ST_HOOK_LEAVE(kHookHibernation);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Pre_GameServerSteamAPIActivated()
{
	CS2ST_HOOK_ENTER(kHookSteamActivated);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_GameServerSteamAPIActivated()
{
	CS2ST_HOOK_LEAVE(kHookSteamActivated);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Pre_GameServerSteamAPIDeactivated()
{
	CS2ST_HOOK_ENTER(kHookSteamDeactivated);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_GameServerSteamAPIDeactivated()
{
	CS2ST_HOOK_LEAVE(kHookSteamDeactivated);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Pre_ApplyGameSettings(KeyValues* settings)
{
	// Only the pointer value - walking into a KeyValues tree from a hook is
	// exactly the kind of thing that turns a diagnostic into an outage.
	CS2ST_HOOK_ENTER_ARGS(kHookApplyGameSettings, "kv=%p",
		static_cast<void*>(settings));
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_ApplyGameSettings(KeyValues*)
{
	CS2ST_HOOK_LEAVE(kHookApplyGameSettings);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Pre_ServerConVarChanged(const char* name, const char* value)
{
	// Both arguments are vetted before anything dereferences them, and the
	// lengths are capped so a missing terminator cannot walk off a page.
	char name_scratch[32];
	char value_scratch[32];
	CS2ST_HOOK_ENTER_ARGS(kHookConVarChanged, "%.63s = \"%.63s\"",
		SafeStringArg(name, name_scratch),
		SafeStringArg(value, value_scratch));
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_ServerConVarChanged(const char*, const char*)
{
	CS2ST_HOOK_LEAVE(kHookConVarChanged);
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Pre_OnHostNameChanged(const char* hostname)
{
	char scratch[32];
	CS2ST_HOOK_ENTER_ARGS(kHookHostNameChanged, "hostname=\"%.63s\"",
		SafeStringArg(hostname, scratch));
	RETURN_META(MRES_IGNORED);
}

void Source2ServerSuite::Post_OnHostNameChanged(const char*)
{
	CS2ST_HOOK_LEAVE(kHookHostNameChanged);
	RETURN_META(MRES_IGNORED);
}

bool Source2ServerSuite::Pre_ShouldTimeoutClient(int user_id, float time_since_last_received)
{
	CS2ST_HOOK_ENTER_ARGS(kHookShouldTimeout, "userID=%d since=%.2f",
		user_id, static_cast<double>(time_since_last_received));
	// MRES_IGNORED means the value here is discarded and the game's own result
	// stands - which is what makes this observation rather than interference.
	RETURN_META_VALUE(MRES_IGNORED, false);
}

bool Source2ServerSuite::Post_ShouldTimeoutClient(int, float)
{
	CS2ST_HOOK_LEAVE(kHookShouldTimeout);
	RETURN_META_VALUE(MRES_IGNORED, false);
}

bool Source2ServerSuite::Pre_ShouldHideFromMasterServer(bool server_has_password)
{
	CS2ST_HOOK_ENTER_ARGS(kHookShouldHide, "hasPassword=%d",
		server_has_password ? 1 : 0);
	RETURN_META_VALUE(MRES_IGNORED, false);
}

bool Source2ServerSuite::Post_ShouldHideFromMasterServer(bool)
{
	CS2ST_HOOK_LEAVE(kHookShouldHide);
	RETURN_META_VALUE(MRES_IGNORED, false);
}

} // namespace cs2st
