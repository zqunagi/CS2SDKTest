#include "suites/Source2ServerSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <appframework/iappsystem.h>
#include <engine2/isource2server.h>
#include <steam/steamclientpublic.h>
#include <tier0/buffer_string.h>
#include <tier0/utlbuffer.h>
#include <tier0/utlstring.h>
#include <tier0/utlvector.h>

#include "core/Log.h"
#include "hook/HookArg.h"
#include "hook/HookRecorder.h"
#include "testkit/UtlVectorGuard.h"

// The SH_DECL_HOOK block used to live here; under KHook the hook templates are
// declared as members in the header instead, bound to the SDK member pointers
// in the constructor at the bottom of this file.

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
	ExerciseLookups(context);
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

	CS2ST_CALL(context, "ISource2Server::IsValveDS", {
		// unk_001 until the 2026-10 SDK named it. We are not a Valve dedicated
		// server, so false is the expected answer - and the one cross-check this
		// row has: true on a community box would mean the slot moved.
		context.Detail("-> %s", server_->IsValveDS() ? "true" : "false");
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
		// The (char*, size_t) era ended in the 2026-10 SDK: the pair of tag
		// getters now share one shape, CBufferString, with GetMatchmakingGameData
		// below - same heap-allocation-off trick too.
		CBufferStringN<512> tags(false);
		server_->GetMatchmakingTags(tags);
		context.Detail("-> \"%s\"", tags.Get());
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

	CS2ST_CALL(context, "ISource2Server::GetBugReportAttachment", {
		// GetSpawnDebugInterface until the 2026-10 SDK reworked the slot into
		// the bug-report pipeline. Index 0 is attachment slot zero, not an
		// entity; the callee decides whether it has anything to attach. The
		// out-strings are read-only to us, and CUtlBuffer's stack form cannot
		// leak, so the probe stays honest without any structure of ours.
		CUtlBuffer buf;
		CUtlString name;
		CUtlString description;
		const bool have = server_->GetBugReportAttachment(0, buf, name, description);
		context.Detail("-> %s, name \"%s\", description \"%s\", %d byte(s)",
			have ? "true" : "false",
			name.Get() != nullptr ? name.Get() : "(null)",
			description.Get() != nullptr ? description.Get() : "(null)",
			buf.TellPut());
	});

	CS2ST_CALL(context, "ISource2Server::GetToolGameSimulationAPI", {
		context.Detail("-> %p", static_cast<void*>(server_->GetToolGameSimulationAPI()));
	});

	// Returns a forward-declared-only type, so the pointer is all this row can
	// honestly say - same shape as GetToolGameSimulationAPI above it.
	CS2ST_CALL(context, "ISource2Server::GetDebugOverlays", {
		context.Detail("-> %p",
			static_cast<void*>(server_->GetDebugOverlays()));
	});

	CS2ST_CALL(context, "ISource2Server::GetEconItemSystem", {
		context.Detail("-> %p", server_->GetEconItemSystem());
	});

	// The GC identity getters: nullary, read-only, and only meaningful with a GC
	// connection this sweep's community box does not have - 0 is the expected
	// answer, which is exactly what makes a non-zero answer a finding.
	CS2ST_CALL(context, "ISource2Server::GetMatchID", {
		context.Detail("-> %llu", static_cast<unsigned long long>(server_->GetMatchID()));
	});

	CS2ST_CALL(context, "ISource2Server::GetSteamGroupAccountID", {
		context.Detail("-> %u", server_->GetSteamGroupAccountID());
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

	CS2ST_CALL(context, "ISource2Server::GetHitGroupInfo", {
		// GetHitGroupEnumInfo until the 2026-10 SDK renamed it; parameters and
		// vtable slot are unchanged, so the row keeps comparing against old logs.
		CUtlVector<int> values;
		UtlVectorGuard<decltype(values)> values_guard(values);
		CUtlVector<CUtlString> names;
		UtlVectorGuard<decltype(names)> names_guard(names);
		server_->GetHitGroupInfo(values, names);

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
		UtlVectorGuard<decltype(activities)> activities_guard(activities);
		server_->GetAnimationActivityList(activities);
		context.Detail("-> %d activit%s", activities.Count(),
			activities.Count() == 1 ? "y" : "ies");
		NoteListPreview(context, activities);
	});

	CS2ST_CALL(context, "ISource2Server::GetAnimationEventList", {
		CUtlVector<CUtlString> events;
		UtlVectorGuard<decltype(events)> events_guard(events);
		server_->GetAnimationEventList(events);
		context.Detail("-> %d event(s)", events.Count());
		NoteListPreview(context, events);
	});

	// GetCharacterList and the whole econ/model-name family were removed from
	// the interface in the Oct 2026 SDK rework (moved behind the save-game
	// slots that now close the vtable); the tail-of-vtable section below
	// carries their successors.
}

// -----------------------------------------------------------------------------
//  Lookups, policy questions and no-op notifications with inert arguments
//
//  Every name below is one no game content will have, every id one no entity or
//  client holds, so each call answers from its rules alone and changes nothing
//  later suites look at. A hook, if installed, will see the ones that are hooked.
// -----------------------------------------------------------------------------

void Source2ServerSuite::ExerciseLookups(TestContext& context)
{
	context.Note("[ISource2Server] lookups with names and ids that match nothing");

	constexpr const char* kNoSuchName = "cs2sdktest_no_such_name";
	const CEntityIndex no_entity(-1);
	const CSteamID no_steam_id;

	CS2ST_CALL(context, "ISource2Server::GetEntity2Networkable", {
		Entity2Networkable_t out;
		std::memset(static_cast<void*>(&out), 0, sizeof(out));
		const bool found = server_->GetEntity2Networkable(no_entity, &out);
		context.Detail("-> %s for entity index -1", found ? "true" : "false");
		if (found)
		{
			context.Suspect("an invalid entity index resolved to a networkable");
		}
	});

	CS2ST_CALL(context, "ISource2Server::GetEntityUniqueHammerID", {
		const char* id = server_->GetEntityUniqueHammerID(no_entity);
		context.Detail("-> %s for entity index -1", id != nullptr ? id : "(null)");
	});

	CS2ST_CALL(context, "ISource2Server::PrintStatus", {
		CBufferStringN<512> output(false);
		server_->PrintStatus(no_entity, output);
		context.Detail("-> %d byte(s) for entity index -1: \"%.100s\"",
			output.Length(), output.Get());
		if (output.Length() > output.AllocatedNum())
		{
			context.Fail("length exceeds the allocation; the layout does not hold");
		}
	});

	// ---- VData / subclass scopes: a null scope matches any, so a nonexistent
	// ---- name is what makes the answer small and checkable.

	CS2ST_CALL(context, "ISource2Server::GetVDataClassName", {
		const char* name = server_->GetVDataClassName(kNoSuchName, nullptr);
		context.Detail("-> %s", name != nullptr ? name : "(null)");
	});

	CS2ST_CALL(context, "ISource2Server::GetVDataClassNameByDataType", {
		const char* name = server_->GetVDataClassNameByDataType(kNoSuchName, kNoSuchName);
		context.Detail("-> %s", name != nullptr ? name : "(null)");
	});

	// Observed: both of these SIGSEGV at address 0, for a nonexistent scope and
	// for the empty one the SDK documents as "matches any". The SDK declares a
	// reference return, and reading through a null one is exactly that fault -
	// but the fault could equally be inside the engine, and a reference cannot be
	// null-checked (the compiler may assume it is not). So the same slot is called
	// through a pointer-returning signature, identical in the ABI, and the pointer
	// is checked before anything reads it. If this still crashes, it is inside the
	// engine and not our read.
	using SubclassNamesFn = const CUtlVector<CUtlString>* (ISource2Server::*)(const char*);

	CS2ST_CALL(context, "ISource2Server::GetSubclassNamesInScope", {
		const SubclassNamesFn call = reinterpret_cast<SubclassNamesFn>(
			&ISource2Server::GetSubclassNamesInScope);
		const CUtlVector<CUtlString>* names = (server_->*call)("");
		if (names == nullptr)
		{
			context.Detail("-> null reference for the empty (any) scope");
			return;
		}
		context.Detail("-> %d name(s) for the empty (any) scope", names->Count());
	});

	CS2ST_CALL(context, "ISource2Server::GetSubclassNamesInScopeByDataType", {
		const SubclassNamesFn call = reinterpret_cast<SubclassNamesFn>(
			&ISource2Server::GetSubclassNamesInScopeByDataType);
		const CUtlVector<CUtlString>* names = (server_->*call)("");
		if (names == nullptr)
		{
			context.Detail("-> null reference for the empty (any) data type");
			return;
		}
		context.Detail("-> %d name(s) for the empty (any) data type", names->Count());
	});

	CS2ST_CALL(context, "ISource2Server::GetAllSubclassNames", {
		CUtlVector<CUtlString> names;
		UtlVectorGuard<decltype(names)> names_guard(names);
		server_->GetAllSubclassNames(names);
		context.Detail("-> %d subclass name(s)", names.Count());
		NoteListPreview(context, names);
	});

	CS2ST_CALL(context, "ISource2Server::GetSubclassDesignerName", {
		const char* name = server_->GetSubclassDesignerName(kNoSuchName);
		context.Detail("-> %s", name != nullptr ? name : "(null)");
	});

	CS2ST_CALL(context, "ISource2Server::GetDesignerNamesForClass", {
		const CUtlVector<CUtlString>& names = server_->GetDesignerNamesForClass(kNoSuchName);
		context.Detail("-> %d name(s) for a nonexistent class", names.Count());
	});

	CS2ST_CALL(context, "ISource2Server::GetNativeClassForScriptClass", {
		const char* name = server_->GetNativeClassForScriptClass(kNoSuchName);
		context.Detail("-> %s", name != nullptr ? name : "(null)");
	});

	CS2ST_CALL(context, "ISource2Server::GetScriptClassForDesignerName", {
		CEntityClass* entity_class = server_->GetScriptClassForDesignerName(kNoSuchName);
		context.Detail("-> %p", static_cast<void*>(entity_class));
		if (entity_class != nullptr)
		{
			context.Suspect("a nonexistent designer name resolved to a class");
		}
	});

	CS2ST_CALL(context, "ISource2Server::IsScriptClassDerivedFrom", {
		const bool derived = server_->IsScriptClassDerivedFrom(kNoSuchName, kNoSuchName);
		context.Detail("-> %s for two nonexistent classes", derived ? "true" : "false");
	});

	// ---- save pipeline: queries, and a path computation, on a save that does
	// ---- not exist. Nothing here starts, drives or finishes a save.

	bool pending = false;
	CS2ST_CALL(context, "ISource2Server::IsAsyncSaveInProgress", {
		context.Detail("-> %s", server_->IsAsyncSaveInProgress() ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Server::HasPendingSaveRequest", {
		pending = server_->HasPendingSaveRequest();
		context.Detail("-> %s", pending ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Server::ProcessPendingSaveRequest", {
		if (pending)
		{
			context.SkipCall("ISource2Server::ProcessPendingSaveRequest",
				"a save request is pending; processing it would start a real save");
			return;
		}
		context.Detail("-> %s with nothing pending",
			server_->ProcessPendingSaveRequest() ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Server::SaveGame_CalcFileName", {
		CUtlString file_name;
		const bool ok = server_->SaveGame_CalcFileName(kNoSuchName, file_name);
		context.Detail("-> %s \"%s\"", ok ? "true" : "false",
			file_name.Get() != nullptr ? file_name.Get() : "(null)");
	});

	CS2ST_CALL(context, "ISource2Server::GetRequiredAddonsFromSaveFile", {
		CUtlString addons;
		const bool ok = server_->GetRequiredAddonsFromSaveFile(kNoSuchName, addons);
		context.Detail("-> %s for a save that does not exist (\"%s\")", ok ? "true" : "false",
			addons.Get() != nullptr ? addons.Get() : "(null)");
	});

	CS2ST_CALL(context, "ISource2Server::GetAddonForMap", {
		CUtlString addon;
		const bool ok = server_->GetAddonForMap(kNoSuchName, addon);
		context.Detail("-> %s for a nonexistent map (\"%s\")", ok ? "true" : "false",
			addon.Get() != nullptr ? addon.Get() : "(null)");
	});

	// ---- policy questions about nobody.

	CS2ST_CALL(context, "ISource2Server::GetClientConnectRejectReason", {
		const int reason = static_cast<int>(server_->GetClientConnectRejectReason(no_steam_id));
		context.Detail("-> %d for an invalid SteamID", reason);
	});

	CS2ST_CALL(context, "ISource2Server::ClientConnectionValidatePreNetChan", {
		const char* answer = server_->ClientConnectionValidatePreNetChan(no_steam_id, "cs2sdktest");
		context.Detail("-> %s for an invalid SteamID (null means accepted)",
			answer != nullptr ? answer : "(null)");
	});

	CS2ST_CALL(context, "ISource2Server::ShouldHoldGameServerReservation", {
		context.Detail("-> %s at 0s without clients",
			server_->ShouldHoldGameServerReservation(0.0f) ? "true" : "false");
	});

	// ---- notifications with a payload that carries nothing.

	CS2ST_CALL(context, "ISource2Server::ServerConVarChanged", {
		server_->ServerConVarChanged("cs2sdktest_no_such_cvar", "0");
		context.Detail("returned for a convar that does not exist");
	});

	CS2ST_CALL(context, "ISource2Server::LogForHTTPListeners", {
		const bool delivered = server_->LogForHTTPListeners("cs2sdktest: LogForHTTPListeners probe");
		context.Detail("-> %s", delivered ? "true" : "false");
	});

	CS2ST_CALL(context, "ISource2Server::OnSteamAuthWarning", {
		server_->OnSteamAuthWarning("cs2sdktest: OnSteamAuthWarning probe");
		context.Detail("returned");
	});

	// SDK: all four do nothing in CS2. Called anyway - a slot that starts doing
	// something is the kind of change a rename in the SDK would otherwise hide.
	CS2ST_CALL(context, "ISource2Server::unk_301", { server_->unk_301(); context.Detail("returned"); });
	CS2ST_CALL(context, "ISource2Server::unk_302", { server_->unk_302(); context.Detail("returned"); });
	CS2ST_CALL(context, "ISource2Server::unk_303", { server_->unk_303(); context.Detail("returned"); });
	CS2ST_CALL(context, "ISource2Server::unk_304", { server_->unk_304(); context.Detail("returned"); });
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

	// Everything not listed here is called, with inert arguments, in
	// ExerciseLookups / ExerciseServerState. What remains needs a type the SDK
	// only forward-declares, an object from the engine's heap, a live player, or
	// would change state the later suites read.
	context.SkipCall("ISource2Server::GetNavMeshData",
		"CNavData is only forward-declared, so there is nothing to size an out-struct with");
	context.SkipCall("ISource2Server::GetTaggedConVarList",
		"needs a KeyValues from the engine's allocator; this module's heap is not "
		"the engine's (CS2SDKTEST_USE_MEMOVERRIDE is off)");
	context.SkipCall("ISource2Server::GetLevelsFromSaveFile",
		"CCreateGameServerLoadInfo is only forward-declared, so the out-vector "
		"cannot be built");
	context.SkipCall("ISource2Server::FormatSerializerFieldValue",
		"FlattenedSerializerSpewField_t is only forward-declared");
	context.SkipCall("ISource2Server::WriteStatusJson",
		"needs a live KeyValues3 from the engine's allocator");
	context.SkipCall("ISource2Server::ClearSaveDirectory",
		"deletes the save directory's contents");
	context.SkipCall("ISource2Server::PreSaveGameLoaded",
		"runs the game's save-loaded path out of band");
	context.SkipCall("ISource2Server::AppendSaveGameResources",
		"grows a live resource manifest");
	context.SkipCall("ISource2Server::AppendTransitionResources",
		"grows a live resource manifest");
	context.SkipCall("ISource2Server::SaveGame",
		"writes a save file; SaveGameParams_t is only forward-declared");
	context.SkipCall("ISource2Server::FinishAsyncSave",
		"completes a save this suite never started");
	context.SkipCall("ISource2Server::UpdateGCInformation",
		"pushes GC state updates");
	context.SkipCall("ISource2Server::ReportGCQueuedMatchStart",
		"feeds a GC match-start reservation with account ids");
	context.SkipCall("ISource2Server::unk_101",
		"unidentified; two void* arguments the game may dereference");
	context.SkipCall("ISource2Server::unk_201",
		"unidentified; a void* argument the game may dereference");
	context.SkipCall("ISource2Server::OnBroadcastRelayRequestSucceeded",
		"engine-to-game notification taking an unidentified pointer");
	context.SkipCall("ISource2Server::OnClientHltvReplayStart",
		"engine-to-game notification that indexes a client by slot; this suite has "
		"no proven-empty slot (Source2GameClientsSuite does)");
	context.SkipCall("ISource2Server::OnClientHltvReplayStop",
		"engine-to-game notification that indexes a client by slot; this suite has "
		"no proven-empty slot (Source2GameClientsSuite does)");
	context.SkipCall("ISource2Server::SendServerFrameTime",
		"feeds the game's frame-time tracking out of band");
	context.SkipCall("ISource2Server::CaptureUserCommands",
		"the capture path the engine drives at frame time");
	context.SkipCall("ISource2Server::CreateUserCommandsMessage",
		"returns an engine allocation that would need freeing, and belongs to the "
		"capture path the engine drives at frame time");
	context.SkipCall("ISource2Server::ProcessClientStringCommand",
		"executes a client command; needs a slot this suite has not proven empty");
	context.SkipCall("ISource2Server::OnPreMatchInterfaceCommand",
		"executes a pre-match command for a GC account id");
	context.SkipCall("ISource2Server::SetPlayerTeammatePreferredColor",
		"changes GC color state for an account id");
	context.SkipCall("ISource2Server::UpdateCompTeammateColors",
		"recomputes GC teammate colors");
	context.SkipCall("ISource2Server::RegisterClientNetMessageHandlers",
		"mutates a live net channel's handler table");
	context.SkipCall("ISource2Server::OnNetworkGameServerActivated",
		"an engine-to-game activation callback");
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

	// Each hook object carries its pre and post handler from the constructor;
	// Add installs the detour on the server instance, Remove takes it off.
	hook_GameFrame_.Add(server_);
	hook_PreWorldUpdate_.Add(server_);
	hook_UpdateWhenNotInGame_.Add(server_);
	hook_ServerHibernationUpdate_.Add(server_);
	hook_GameServerSteamAPIActivated_.Add(server_);
	hook_GameServerSteamAPIDeactivated_.Add(server_);
	hook_ApplyGameSettings_.Add(server_);
	hook_ServerConVarChanged_.Add(server_);
	hook_OnHostNameChanged_.Add(server_);
	hook_ShouldTimeoutClient_.Add(server_);
	hook_ShouldHideFromMasterServer_.Add(server_);

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

	hook_GameFrame_.Remove(server_);
	hook_PreWorldUpdate_.Remove(server_);
	hook_UpdateWhenNotInGame_.Remove(server_);
	hook_ServerHibernationUpdate_.Remove(server_);
	hook_GameServerSteamAPIActivated_.Remove(server_);
	hook_GameServerSteamAPIDeactivated_.Remove(server_);
	hook_ApplyGameSettings_.Remove(server_);
	hook_ServerConVarChanged_.Remove(server_);
	hook_OnHostNameChanged_.Remove(server_);
	hook_ShouldTimeoutClient_.Remove(server_);
	hook_ShouldHideFromMasterServer_.Remove(server_);

	hooks_installed_ = false;
	(void)context;
}

// -----------------------------------------------------------------------------
//  Hook handlers
//
//  These run inside the server's own frame - GameFrame at the tick rate - so
//  they do the minimum: bump a counter, and format arguments only until the
//  sample quota is full (HookRecorder stops before formatting once it is).
//  Every one answers Action::Ignore, the KHook equivalent of MRES_IGNORED.
// -----------------------------------------------------------------------------

KHook::Return<void> Source2ServerSuite::Pre_GameFrame(ISource2Server*, bool simulating, bool first_tick, bool last_tick)
{
	CS2ST_HOOK_ENTER_ARGS(kHookGameFrame, "simulating=%d first=%d last=%d",
		simulating ? 1 : 0, first_tick ? 1 : 0, last_tick ? 1 : 0);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_GameFrame(ISource2Server*, bool, bool, bool)
{
	CS2ST_HOOK_LEAVE(kHookGameFrame);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Pre_PreWorldUpdate(ISource2Server*, bool simulating)
{
	CS2ST_HOOK_ENTER_ARGS(kHookPreWorldUpdate, "simulating=%d", simulating ? 1 : 0);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_PreWorldUpdate(ISource2Server*, bool)
{
	CS2ST_HOOK_LEAVE(kHookPreWorldUpdate);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Pre_UpdateWhenNotInGame(ISource2Server*, float frame_time)
{
	CS2ST_HOOK_ENTER_ARGS(kHookUpdateNotInGame, "frametime=%.4f",
		static_cast<double>(frame_time));
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_UpdateWhenNotInGame(ISource2Server*, float)
{
	CS2ST_HOOK_LEAVE(kHookUpdateNotInGame);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Pre_ServerHibernationUpdate(ISource2Server*, bool hibernating)
{
	CS2ST_HOOK_ENTER_ARGS(kHookHibernation, "hibernating=%d", hibernating ? 1 : 0);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_ServerHibernationUpdate(ISource2Server*, bool)
{
	CS2ST_HOOK_LEAVE(kHookHibernation);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Pre_GameServerSteamAPIActivated(ISource2Server*)
{
	CS2ST_HOOK_ENTER(kHookSteamActivated);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_GameServerSteamAPIActivated(ISource2Server*)
{
	CS2ST_HOOK_LEAVE(kHookSteamActivated);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Pre_GameServerSteamAPIDeactivated(ISource2Server*)
{
	CS2ST_HOOK_ENTER(kHookSteamDeactivated);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_GameServerSteamAPIDeactivated(ISource2Server*)
{
	CS2ST_HOOK_LEAVE(kHookSteamDeactivated);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Pre_ApplyGameSettings(ISource2Server*, KeyValues* settings)
{
	// Only the pointer value - walking into a KeyValues tree from a hook is
	// exactly the kind of thing that turns a diagnostic into an outage.
	CS2ST_HOOK_ENTER_ARGS(kHookApplyGameSettings, "kv=%p",
		static_cast<void*>(settings));
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_ApplyGameSettings(ISource2Server*, KeyValues*)
{
	CS2ST_HOOK_LEAVE(kHookApplyGameSettings);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Pre_ServerConVarChanged(ISource2Server*, const char* name, const char* value)
{
	// Both arguments are vetted before anything dereferences them, and the
	// lengths are capped so a missing terminator cannot walk off a page.
	char name_scratch[32];
	char value_scratch[32];
	CS2ST_HOOK_ENTER_ARGS(kHookConVarChanged, "%.63s = \"%.63s\"",
		SafeStringArg(name, name_scratch),
		SafeStringArg(value, value_scratch));
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_ServerConVarChanged(ISource2Server*, const char*, const char*)
{
	CS2ST_HOOK_LEAVE(kHookConVarChanged);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Pre_OnHostNameChanged(ISource2Server*, const char* hostname)
{
	char scratch[32];
	CS2ST_HOOK_ENTER_ARGS(kHookHostNameChanged, "hostname=\"%.63s\"",
		SafeStringArg(hostname, scratch));
	return { KHook::Action::Ignore };
}

KHook::Return<void> Source2ServerSuite::Post_OnHostNameChanged(ISource2Server*, const char*)
{
	CS2ST_HOOK_LEAVE(kHookHostNameChanged);
	return { KHook::Action::Ignore };
}

KHook::Return<bool> Source2ServerSuite::Pre_ShouldTimeoutClient(ISource2Server*, int user_id, float time_since_last_received)
{
	CS2ST_HOOK_ENTER_ARGS(kHookShouldTimeout, "userID=%d since=%.2f",
		user_id, static_cast<double>(time_since_last_received));
	// Action::Ignore means the `ret` field here is discarded and the game's own
	// result stands - which is what makes this observation rather than
	// interference.
	return { KHook::Action::Ignore, false };
}

KHook::Return<bool> Source2ServerSuite::Post_ShouldTimeoutClient(ISource2Server*, int, float)
{
	CS2ST_HOOK_LEAVE(kHookShouldTimeout);
	return { KHook::Action::Ignore, false };
}

KHook::Return<bool> Source2ServerSuite::Pre_ShouldHideFromMasterServer(ISource2Server*, bool server_has_password)
{
	CS2ST_HOOK_ENTER_ARGS(kHookShouldHide, "hasPassword=%d",
		server_has_password ? 1 : 0);
	return { KHook::Action::Ignore, false };
}

KHook::Return<bool> Source2ServerSuite::Post_ShouldHideFromMasterServer(ISource2Server*, bool)
{
	CS2ST_HOOK_LEAVE(kHookShouldHide);
	return { KHook::Action::Ignore, false };
}

// -----------------------------------------------------------------------------
//  Hook binding
//
//  The KHook::Virtual constructor resolves the vtable slot from the member
//  pointer, so this list is the plugin's ABI contract with the SDK: the day a
//  hook stops firing after an update, these lines are what to re-check first.
// -----------------------------------------------------------------------------

// convar.h ends with `#pragma warning(default : 4355)`, which re-enables the warning
// for the rest of the translation unit and overrides the /wd4355 on the command
// line. Passing `this` as the hook context is intentional (stored, not used).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4355)
#endif
Source2ServerSuite::Source2ServerSuite() :
	hook_GameFrame_(&ISource2Server::GameFrame, this,
		&Source2ServerSuite::Pre_GameFrame,
		&Source2ServerSuite::Post_GameFrame),
	hook_PreWorldUpdate_(&ISource2Server::PreWorldUpdate, this,
		&Source2ServerSuite::Pre_PreWorldUpdate,
		&Source2ServerSuite::Post_PreWorldUpdate),
	hook_UpdateWhenNotInGame_(&ISource2Server::UpdateWhenNotInGame, this,
		&Source2ServerSuite::Pre_UpdateWhenNotInGame,
		&Source2ServerSuite::Post_UpdateWhenNotInGame),
	hook_ServerHibernationUpdate_(&ISource2Server::ServerHibernationUpdate, this,
		&Source2ServerSuite::Pre_ServerHibernationUpdate,
		&Source2ServerSuite::Post_ServerHibernationUpdate),
	hook_GameServerSteamAPIActivated_(&ISource2Server::GameServerSteamAPIActivated, this,
		&Source2ServerSuite::Pre_GameServerSteamAPIActivated,
		&Source2ServerSuite::Post_GameServerSteamAPIActivated),
	hook_GameServerSteamAPIDeactivated_(&ISource2Server::GameServerSteamAPIDeactivated, this,
		&Source2ServerSuite::Pre_GameServerSteamAPIDeactivated,
		&Source2ServerSuite::Post_GameServerSteamAPIDeactivated),
	hook_ApplyGameSettings_(&ISource2Server::ApplyGameSettings, this,
		&Source2ServerSuite::Pre_ApplyGameSettings,
		&Source2ServerSuite::Post_ApplyGameSettings),
	hook_ServerConVarChanged_(&ISource2Server::ServerConVarChanged, this,
		&Source2ServerSuite::Pre_ServerConVarChanged,
		&Source2ServerSuite::Post_ServerConVarChanged),
	hook_OnHostNameChanged_(&ISource2Server::OnHostNameChanged, this,
		&Source2ServerSuite::Pre_OnHostNameChanged,
		&Source2ServerSuite::Post_OnHostNameChanged),
	hook_ShouldTimeoutClient_(&ISource2Server::ShouldTimeoutClient, this,
		&Source2ServerSuite::Pre_ShouldTimeoutClient,
		&Source2ServerSuite::Post_ShouldTimeoutClient),
	hook_ShouldHideFromMasterServer_(&ISource2Server::ShouldHideFromMasterServer, this,
		&Source2ServerSuite::Pre_ShouldHideFromMasterServer,
		&Source2ServerSuite::Post_ShouldHideFromMasterServer)
{
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

} // namespace cs2st
