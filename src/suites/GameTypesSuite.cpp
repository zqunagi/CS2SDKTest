#include "suites/GameTypesSuite.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include <appframework/iappsystem.h>
#include <matchmaking/igametypes.h>
#include <tier0/utlstring.h>
#include <tier0/utlstringlist.h>
#include <tier0/utlvector.h>

#include "core/Log.h"

namespace cs2st
{
namespace
{

// Bounds for everything the engine counts.
constexpr int kMaxListed = 8;
constexpr int kMaxWalk = 64;

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

const char* Str(const char* text)
{
	return text != nullptr ? text : "(null)";
}

bool SameText(const char* a, const char* b)
{
	return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

// Prints up to kMaxListed entries of an engine-owned string list.
void DescribeStringList(TestContext& context, const char* label, const CUtlStringList* list)
{
	if (list == nullptr)
	{
		context.Detail("-> %s: null", label);
		return;
	}
	const int count = list->Count();
	context.Detail("-> %s: %d entr%s", label, count, count == 1 ? "y" : "ies");
	if (count < 0 || count > 100000)
	{
		context.Suspect("%s count %d is not a plausible length", label, count);
		return;
	}
	for (int i = 0; i < count && i < kMaxListed; ++i)
	{
		context.Detail("   [%d] \"%s\"", i, Str(list->Element(i)));
	}
}

// The names the int routes return, kept in static storage because the
// CS2ST_CALL bodies below hand them from one call to the next.
char g_type_name[64];
char g_mode_name[64];
char g_group[128];
char g_first_map[128];

// Offset of the extended-server-info KeyValues* in the engine's CGameTypes
// (libmatchmaking.so, slot 73 is `mov rdi, [rdi+118h]; jmp GetInt`).
constexpr std::size_t kExtendedServerInfoOffset = 0x118;

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool GameTypesSuite::Acquire(TestContext& context)
{
	game_types_ = static_cast<IGameTypes*>(
		context.QueryInterface("server", VENGINE_GAMETYPES_VERSION));

	std::string found_in = "server";

	// The list says "server", but a dedicated server does not export it there.
	// Try the other likely owner, then every module we know, and report where
	// it turned up so the next run does not have to guess.
	if (game_types_ == nullptr)
	{
		game_types_ = static_cast<IGameTypes*>(
			context.QueryInterface("matchmaking", VENGINE_GAMETYPES_VERSION));
		found_in = "matchmaking";
	}
	if (game_types_ == nullptr)
	{
		game_types_ = static_cast<IGameTypes*>(
			context.QueryInterfaceAnywhere(VENGINE_GAMETYPES_VERSION, &found_in));
	}

	if (game_types_ == nullptr)
	{
		const bool server_loaded = context.GetModule("server").IsValid();
		const bool matchmaking_loaded = context.GetModule("matchmaking").IsValid();
		context.UnavailableCall("IGameTypes",
			"no known module answers '%s' (server %s, matchmaking %s); a dedicated server may not export it",
			VENGINE_GAMETYPES_VERSION, server_loaded ? "loaded" : "not loaded",
			matchmaking_loaded ? "loaded" : "not loaded");
		return false;
	}

	context.Note("interface at %p (%s) from module \"%s\"", static_cast<void*>(game_types_),
		VENGINE_GAMETYPES_VERSION, found_in.c_str());
	return true;
}

void GameTypesSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseModeTables(context);
	ExerciseCurrentMode(context);
	ExerciseMapGroups(context);
	ExerciseFlags(context);
	NoteSkippedCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots
// -----------------------------------------------------------------------------

void GameTypesSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = game_types_->GetTier();
		const int tier_value = static_cast<int>(tier);
		context.Detail("-> %s (%d)", TierName(tier), tier_value);
		if (tier_value < 0 || tier_value > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside the enum; slot 7 is probably not GetTier",
				tier_value);
		}
	});

	CS2ST_CALL(context, "IAppSystem::IsSingleton", {
		context.Detail("-> %s", game_types_->IsSingleton() ? "true" : "false");
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = game_types_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = game_types_->GetDependencies();
		if (deps == nullptr)
		{
			context.Detail("-> none");
			return;
		}
		int count = 0;
		while (count < 64 && deps[count].m_pModuleName != nullptr)
		{
			++count;
		}
		context.Detail("-> %p, %d entr%s%s", static_cast<const void*>(deps),
			count, count == 1 ? "y" : "ies", count == 64 ? " (capped)" : "");
		for (int i = 0; i < count && i < 16; ++i)
		{
			context.Detail("   [%d] module \"%s\", interface \"%s\"", i,
				deps[i].m_pModuleName, Str(deps[i].m_pInterfaceName));
		}
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface (unknown name)", {
		void* none = game_types_->QueryInterface("CS2SDKTestNoSuchInterface001");
		context.Detail("-> %p", none);
		if (none != nullptr)
		{
			context.Suspect("an interface name nobody registered resolved to a pointer");
		}
	});

	context.SkipCall("IAppSystem::Connect", "would re-run engine startup");
	context.SkipCall("IAppSystem::Init", "would re-run engine startup");
	context.SkipCall("IAppSystem::Reconnect", "would rebind engine interfaces");
	context.SkipCall("IAppSystem::PreShutdown", "would begin engine shutdown");
	context.SkipCall("IAppSystem::Shutdown", "would shut the engine down");
	context.SkipCall("IAppSystem::Disconnect", "would tear down engine interfaces");
}

// -----------------------------------------------------------------------------
//  Name <-> int tables: two independent paths to the same fact
// -----------------------------------------------------------------------------

void GameTypesSuite::ExerciseModeTables(TestContext& context)
{
	context.Note("[IGameTypes] mode tables - alias, name and int routes must agree");

	CS2ST_CALL(context, "IGameTypes::IsInitialized", {
		initialized_ = game_types_->IsInitialized();
		context.Detail("-> %s", initialized_ ? "true" : "false");
		if (!initialized_)
		{
			context.Suspect("the table is not initialized; everything that reads it below may be empty");
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetGameTypeAndModeFromAlias (competitive)", {
		int type = -1;
		int mode = -1;
		const bool found = game_types_->GetGameTypeAndModeFromAlias("competitive", type, mode);
		context.Detail("-> %s, type %d, mode %d", found ? "true" : "false", type, mode);
		if (found)
		{
			alias_type_ = type;
			alias_mode_ = mode;
			alias_ok_ = true;
		}
		else if (initialized_)
		{
			context.Suspect("an initialized table does not know the \"competitive\" alias");
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetGameTypeAndModeFromAlias (unknown alias)", {
		int type = -7;
		int mode = -7;
		const bool found = game_types_->GetGameTypeAndModeFromAlias("cs2sdktest_no_such_alias", type, mode);
		context.Detail("-> %s, type %d, mode %d", found ? "true" : "false", type, mode);
		if (found)
		{
			context.Suspect("an alias nobody defined resolved");
		}
	});

	if (!alias_ok_)
	{
		context.SkipCall("IGameTypes::GetGameTypeFromInt / GetGameModeFromInt / GetGameModeAndTypeIntsFromStrings / "
			"GetGameModeAndTypeNameIdsFromStrings / GetGameTypeFromMode / GetMaxPlayersForTypeAndMode",
			"the \"competitive\" alias did not resolve, so there is no known-good type and mode to probe with");
		return;
	}

	// The strings that come back from the int routes are checked against the
	// int routes of the same strings, so a wrong return type or slot shows up
	// as a disagreement rather than as a crash.
	g_type_name[0] = '\0';
	g_mode_name[0] = '\0';

	CS2ST_CALL(context, "IGameTypes::GetGameTypeFromInt / GetGameModeFromInt", {
		const char* type_text = game_types_->GetGameTypeFromInt(alias_type_);
		const char* mode_text = game_types_->GetGameModeFromInt(alias_type_, alias_mode_);
		context.Detail("-> type \"%s\", mode \"%s\"", Str(type_text), Str(mode_text));
		if (type_text != nullptr)
		{
			std::snprintf(g_type_name, sizeof(g_type_name), "%s", type_text);
		}
		if (mode_text != nullptr)
		{
			std::snprintf(g_mode_name, sizeof(g_mode_name), "%s", mode_text);
		}
		if (type_text == nullptr || mode_text == nullptr)
		{
			context.Suspect("the names for a type and mode the alias table just gave are missing");
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetGameModeAndTypeIntsFromStrings", {
		int type = -1;
		int mode = -1;
		const bool found = game_types_->GetGameModeAndTypeIntsFromStrings(g_type_name, g_mode_name, type, mode);
		context.Detail("-> %s, type %d, mode %d", found ? "true" : "false", type, mode);
		if (g_type_name[0] != '\0' && g_mode_name[0] != '\0'
			&& (!found || type != alias_type_ || mode != alias_mode_))
		{
			context.Suspect("name -> int gave (%d, %d) but the alias route gave (%d, %d)",
				type, mode, alias_type_, alias_mode_);
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetGameModeAndTypeNameIdsFromStrings", {
		const char* type_id = nullptr;
		const char* mode_id = nullptr;
		const bool found = game_types_->GetGameModeAndTypeNameIdsFromStrings(g_type_name, g_mode_name, type_id, mode_id);
		context.Detail("-> %s, type id \"%s\", mode id \"%s\"", found ? "true" : "false",
			Str(type_id), Str(mode_id));
		if (found && (type_id == nullptr || mode_id == nullptr))
		{
			context.Suspect("reported success with a null name id");
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetGameTypeFromMode", {
		const char* resolved_type = nullptr;
		const bool found = game_types_->GetGameTypeFromMode(g_mode_name, &resolved_type);
		context.Detail("-> %s, type \"%s\"", found ? "true" : "false", Str(resolved_type));
		if (found && g_type_name[0] != '\0' && !SameText(resolved_type, g_type_name))
		{
			context.Suspect("mode \"%s\" maps to type \"%s\", expected \"%s\"",
				g_mode_name, Str(resolved_type), g_type_name);
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetMaxPlayersForTypeAndMode", {
		const int max_players = game_types_->GetMaxPlayersForTypeAndMode(alias_type_, alias_mode_, 0);
		context.Detail("-> %d", max_players);
		if (max_players < 0 || max_players > 128)
		{
			context.Suspect("%d is not a plausible player count", max_players);
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetGameTypeFromInt / GetGameModeFromInt (out of range)", {
		const char* type_text = game_types_->GetGameTypeFromInt(-1);
		const char* mode_text = game_types_->GetGameModeFromInt(-1, -1);
		context.Detail("-> type \"%s\", mode \"%s\"", Str(type_text), Str(mode_text));
	});
}

// -----------------------------------------------------------------------------
//  The server's current mode
// -----------------------------------------------------------------------------

void GameTypesSuite::ExerciseCurrentMode(TestContext& context)
{
	context.Note("[IGameTypes] the current mode - read only; an uninitialized table answers with whatever it holds");

	CS2ST_CALL(context, "IGameTypes::GetCurrentGameType / GetCurrentGameMode", {
		const int type = game_types_->GetCurrentGameType();
		const int mode = game_types_->GetCurrentGameMode();
		context.Detail("-> type %d, mode %d", type, mode);
		if (initialized_ && alias_ok_)
		{
			const char* type_text = game_types_->GetGameTypeFromInt(type);
			const char* mode_text = game_types_->GetGameModeFromInt(type, mode);
			context.Detail("   names \"%s\" / \"%s\"", Str(type_text), Str(mode_text));
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetCurrentMapName", {
		context.Detail("-> \"%s\"", Str(game_types_->GetCurrentMapName()));
	});

	CS2ST_CALL(context, "IGameTypes::GetCurrentGameModeName", {
		context.Detail("-> \"%s\"", Str(game_types_->GetCurrentGameModeName()));
	});

	CS2ST_CALL(context, "IGameTypes::GetCurrentMapGroupName", {
		context.Detail("-> \"%s\"", Str(game_types_->GetCurrentMapGroupName()));
	});

	CS2ST_CALL(context, "IGameTypes::GetCurrentGameTypeNameID", {
		context.Detail("-> \"%s\"", Str(game_types_->GetCurrentGameTypeNameID()));
	});

	CS2ST_CALL(context, "IGameTypes::GetCurrentGameModeNameID", {
		context.Detail("-> \"%s\"", Str(game_types_->GetCurrentGameModeNameID()));
	});

	CS2ST_CALL(context, "IGameTypes::GetCurrentGameModeScriptName", {
		context.Detail("-> \"%s\"", Str(game_types_->GetCurrentGameModeScriptName()));
	});

	CS2ST_CALL(context, "IGameTypes::GetWeaponProgressionForCurrentModeCT / T", {
		const auto* ct = game_types_->GetWeaponProgressionForCurrentModeCT();
		const auto* t = game_types_->GetWeaponProgressionForCurrentModeT();
		context.Detail("-> CT %p (%d), T %p (%d)", static_cast<const void*>(ct), ct != nullptr ? ct->Count() : -1,
			static_cast<const void*>(t), t != nullptr ? t->Count() : -1);
		if (ct != nullptr && ct->Count() >= 0 && ct->Count() < 1000)
		{
			for (int i = 0; i < ct->Count() && i < kMaxListed; ++i)
			{
				context.Detail("   CT [%d] \"%s\" kills %d", i, ct->Element(i).m_Name.Get(),
					ct->Element(i).m_Kills);
			}
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetNoResetVoteThresholdForCurrentModeCT / T", {
		context.Detail("-> CT %d, T %d", game_types_->GetNoResetVoteThresholdForCurrentModeCT(),
			game_types_->GetNoResetVoteThresholdForCurrentModeT());
	});

	CS2ST_CALL(context, "IGameTypes::GetCurrentServerNumSlots", {
		const int slots = game_types_->GetCurrentServerNumSlots();
		context.Detail("-> %d", slots);
		if (slots < -1 || slots > 256)
		{
			context.Suspect("%d is not a plausible slot count", slots);
		}
	});

	// The engine's slot 73 is `GetInt(this->m_pExtendedServerInfo, name, default)`
	// with no null check, so with no extended server info it reads a null
	// KeyValues (SIGSEGV at 0x8). The pointer is a member of the engine's
	// concrete class, not of the SDK's abstract IGameTypes, so the offset below
	// comes from libmatchmaking.so (2026-11, build v031), not from a header.
	// Linux only; on another build the read is unchecked, so it only gates the call.
	const void* extended_info = nullptr;
	bool extended_info_read = false;
	CS2ST_CALL(context, "IGameTypes::m_pExtendedServerInfo (read for the guard below)", {
		extended_info = *reinterpret_cast<void* const*>(
			reinterpret_cast<const unsigned char*>(game_types_) + kExtendedServerInfoOffset);
		extended_info_read = true;
		context.Detail("-> %p", extended_info);
	});

	if (!extended_info_read || extended_info == nullptr)
	{
		context.SkipCall("IGameTypes::GetCurrentServerSettingInt",
			"no extended server info is set, and the engine reads the setting through that KeyValues without a null check (SIGSEGV at 0x8)");
	}
	else
	{
		CS2ST_CALL(context, "IGameTypes::GetCurrentServerSettingInt (default for an unknown setting)", {
			const int value = game_types_->GetCurrentServerSettingInt("cs2sdktest_no_such_setting", 12345);
			context.Detail("-> %d", value);
			if (value != 12345)
			{
				context.Suspect("an unknown setting did not return the caller default (12345)");
			}
		});
	}

	CS2ST_CALL(context, "IGameTypes::GetCustomBotDifficulty", {
		context.Detail("-> %d", game_types_->GetCustomBotDifficulty());
	});
}

// -----------------------------------------------------------------------------
//  Map groups and maps
// -----------------------------------------------------------------------------

void GameTypesSuite::ExerciseMapGroups(TestContext& context)
{
	context.Note("[IGameTypes] map groups - walked with first / next, cross-checked against the list and the validity queries");

	CS2ST_CALL(context, "IGameTypes::IsValidMapGroupName / IsWorkshopMapGroup (unknown group)", {
		const bool valid = game_types_->IsValidMapGroupName("cs2sdktest_no_such_group");
		const bool workshop = game_types_->IsWorkshopMapGroup("cs2sdktest_no_such_group");
		context.Detail("-> valid %s, workshop %s", valid ? "true" : "false", workshop ? "true" : "false");
		if (valid)
		{
			context.Suspect("a map group nobody defined is valid");
		}
	});

	// Prefer the group the server is in; fall back to the stock one.
	const char* current_group = nullptr;
	CS2ST_CALL(context, "IGameTypes::GetCurrentMapGroupName (probe group)", {
		current_group = game_types_->GetCurrentMapGroupName();
		context.Detail("-> \"%s\"", Str(current_group));
	});
	std::snprintf(g_group, sizeof(g_group), "%s",
		(current_group != nullptr && current_group[0] != '\0') ? current_group : "mg_active");
	g_first_map[0] = '\0';
	context.Note("probing map group \"%s\"", g_group);

	CS2ST_CALL(context, "IGameTypes::IsValidMapGroupName / IsWorkshopMapGroup", {
		const bool valid = game_types_->IsValidMapGroupName(g_group);
		context.Detail("-> valid %s, workshop %s", valid ? "true" : "false",
			game_types_->IsWorkshopMapGroup(g_group) ? "true" : "false");
		if (!valid)
		{
			context.Detail("   the group is not valid here; the calls below probe an empty answer");
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetMapGroupMapList", {
		DescribeStringList(context, "map list", game_types_->GetMapGroupMapList(g_group));
	});

	CS2ST_CALL(context, "IGameTypes::GetFirstMap", {
		const char* map = game_types_->GetFirstMap(g_group);
		context.Detail("-> \"%s\"", Str(map));
		if (map != nullptr)
		{
			std::snprintf(g_first_map, sizeof(g_first_map), "%s", map);
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetNextMap (walk)", {
		const CUtlStringList* list = game_types_->GetMapGroupMapList(g_group);
		const int expected = list != nullptr ? list->Count() : -1;
		char current[128];
		std::snprintf(current, sizeof(current), "%s", g_first_map);
		int steps = 0;
		while (current[0] != '\0' && steps < kMaxWalk)
		{
			++steps;
			const char* next = game_types_->GetNextMap(g_group, current);
			if (next == nullptr || next[0] == '\0')
			{
				break;
			}
			std::snprintf(current, sizeof(current), "%s", next);
			if (std::strcmp(current, g_first_map) == 0)
			{
				break;
			}
		}
		context.Detail("-> walked %d map(s), list says %d", steps, expected);
		if (expected >= 0 && steps != expected && steps < kMaxWalk && g_first_map[0] != '\0')
		{
			context.Suspect("walking GetNextMap visited %d map(s) but GetMapGroupMapList holds %d",
				steps, expected);
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetRandomMap", {
		const char* random_map = game_types_->GetRandomMap(g_group);
		context.Detail("-> \"%s\"", Str(random_map));
		if (random_map != nullptr && random_map[0] != '\0'
			&& !game_types_->IsValidMapInMapGroup(g_group, random_map))
		{
			context.Suspect("GetRandomMap returned a map IsValidMapInMapGroup rejects");
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetRandomMapGroup", {
		if (!alias_ok_)
		{
			context.Detail("-> not called: no known-good type and mode");
			return;
		}
		context.Detail("-> \"%s\"", Str(game_types_->GetRandomMapGroup(g_type_name, g_mode_name)));
	});

	CS2ST_CALL(context, "IGameTypes::IsValidMapGroupForTypeAndMode", {
		if (!alias_ok_)
		{
			context.Detail("-> not called: no known-good type and mode");
			return;
		}
		context.Detail("-> %s", game_types_->IsValidMapGroupForTypeAndMode(g_group, g_type_name, g_mode_name)
			? "true" : "false");
	});

	CS2ST_CALL(context, "IGameTypes::GetMapGroupNameID", {
		bool found = false;
		const char* id = game_types_->GetMapGroupNameID(g_group, found, false);
		context.Detail("-> \"%s\" (%s)", Str(id), found ? "found" : "not found");
	});

	CS2ST_CALL(context, "IGameTypes::GetAddonForMapGroup", {
		CUtlString addon;
		const bool has = game_types_->GetAddonForMapGroup(g_group, addon);
		context.Detail("-> %s \"%s\"", has ? "true" : "false", addon.Get());
	});

	CS2ST_CALL(context, "IGameTypes::GetGameModeNameID", {
		bool found = false;
		const char* id = game_types_->GetGameModeNameID(g_mode_name[0] != '\0' ? g_mode_name : "competitive", found);
		context.Detail("-> \"%s\" (%s)", Str(id), found ? "found" : "not found");
	});

	if (g_first_map[0] == '\0')
	{
		context.SkipCall("IGameTypes::IsValidMapInMapGroup / GetMapGroupMapName and the per-map getters",
			"the group has no first map to probe with");
		return;
	}

	CS2ST_CALL(context, "IGameTypes::IsValidMapInMapGroup / GetMapGroupMapName", {
		const bool in_group = game_types_->IsValidMapInMapGroup(g_group, g_first_map);
		const bool absent = game_types_->IsValidMapInMapGroup(g_group, "cs2sdktest_no_such_map");
		context.Detail("-> first map in group %s, unknown map in group %s, name \"%s\"",
			in_group ? "true" : "false", absent ? "true" : "false",
			Str(game_types_->GetMapGroupMapName(g_group, g_first_map)));
		if (!in_group)
		{
			context.Suspect("the first map of a group is not in that group");
		}
		if (absent)
		{
			context.Suspect("an unknown map is in the group");
		}
	});

	ExercisePerMap(context, g_first_map);
}

void GameTypesSuite::ExercisePerMap(TestContext& context, const char* map_name)
{
	context.Note("[IGameTypes] per-map data for \"%s\"", map_name);

	CS2ST_CALL(context, "IGameTypes::GetDefaultGameTypeForMap / GetDefaultGameModeForMap", {
		const int type = game_types_->GetDefaultGameTypeForMap(map_name);
		const int mode = game_types_->GetDefaultGameModeForMap(map_name);
		context.Detail("-> type %d, mode %d", type, mode);
		if (initialized_ && type >= 0 && mode >= 0)
		{
			context.Detail("   names \"%s\" / \"%s\"", Str(game_types_->GetGameTypeFromInt(type)),
				Str(game_types_->GetGameModeFromInt(type, mode)));
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetMapInfo", {
		unsigned int rich_presence = 0xDEADBEEFu;
		const bool found = game_types_->GetMapInfo(map_name, rich_presence);
		context.Detail("-> %s, rich presence %u", found ? "true" : "false", rich_presence);
	});

	CS2ST_CALL(context, "IGameTypes::GetMapNameID", {
		bool found = false;
		const char* id = game_types_->GetMapNameID(map_name, found, false);
		context.Detail("-> \"%s\" (%s)", Str(id), found ? "found" : "not found");
	});

	CS2ST_CALL(context, "IGameTypes::GetTModelsForMap", {
		DescribeStringList(context, "T models", game_types_->GetTModelsForMap(map_name));
	});

	CS2ST_CALL(context, "IGameTypes::GetCTModelsForMap", {
		DescribeStringList(context, "CT models", game_types_->GetCTModelsForMap(map_name));
	});

	CS2ST_CALL(context, "IGameTypes::GetHostageModelsForMap", {
		DescribeStringList(context, "hostage models", game_types_->GetHostageModelsForMap(map_name));
	});

	CS2ST_CALL(context, "IGameTypes::GetTViewModelArmsForMap / GetCTViewModelArmsForMap", {
		context.Detail("-> T \"%s\", CT \"%s\"", Str(game_types_->GetTViewModelArmsForMap(map_name)),
			Str(game_types_->GetCTViewModelArmsForMap(map_name)));
	});

	CS2ST_CALL(context, "IGameTypes::GetRequiredAttrForMap / GetRequiredAttrValueForMap", {
		context.Detail("-> attr \"%s\", value %d", Str(game_types_->GetRequiredAttrForMap(map_name)),
			game_types_->GetRequiredAttrValueForMap(map_name));
	});

	CS2ST_CALL(context, "IGameTypes::GetRequiredAttrRewardForMap / GetRewardDropListForMap", {
		context.Detail("-> reward \"%s\", drop list %d", Str(game_types_->GetRequiredAttrRewardForMap(map_name)),
			game_types_->GetRewardDropListForMap(map_name));
	});

	CS2ST_CALL(context, "IGameTypes::GetAddonForMap", {
		CUtlString addon;
		const bool has = game_types_->GetAddonForMap(map_name, addon);
		context.Detail("-> %s \"%s\"", has ? "true" : "false", addon.Get());
	});
}

// -----------------------------------------------------------------------------
//  Flags: set to the value just read, so the state ends as it began
// -----------------------------------------------------------------------------

void GameTypesSuite::ExerciseFlags(TestContext& context)
{
	context.Note("[IGameTypes] flags - each setter is given the value its getter just returned");

	CS2ST_CALL(context, "IGameTypes::GetRunMapWithDefaultGametype / SetRunMapWithDefaultGameType (same value)", {
		const bool before = game_types_->GetRunMapWithDefaultGametype();
		game_types_->SetRunMapWithDefaultGameType(before);
		const bool after = game_types_->GetRunMapWithDefaultGametype();
		context.Detail("-> %s before, %s after", before ? "true" : "false", after ? "true" : "false");
		if (before != after)
		{
			context.Fail("GetRunMapWithDefaultGametype changed after setting its own value");
		}
	});

	CS2ST_CALL(context, "IGameTypes::GetLoadingScreenDataIsCorrect / SetLoadingScreenDataIsCorrect (same value)", {
		const bool before = game_types_->GetLoadingScreenDataIsCorrect();
		game_types_->SetLoadingScreenDataIsCorrect(before);
		const bool after = game_types_->GetLoadingScreenDataIsCorrect();
		context.Detail("-> %s before, %s after", before ? "true" : "false", after ? "true" : "false");
		if (before != after)
		{
			context.Fail("GetLoadingScreenDataIsCorrect changed after setting its own value");
		}
	});
}

// -----------------------------------------------------------------------------
//  Skipped calls
// -----------------------------------------------------------------------------

void GameTypesSuite::NoteSkippedCalls(TestContext& context)
{
	context.Note("[IGameTypes] calls that change the server mode or the table, each with its reason");

	context.SkipCall("IGameTypes::~IGameTypes", "destroys the engine table; the destructor may occupy two vtable slots, which is why no slot is called blindly");
	context.SkipCall("IGameTypes::Initialize", "force-reloads the game type tables from disk");
	context.SkipCall("IGameTypes::SetGameTypeAndMode (names)", "changes the server game type and mode");
	context.SkipCall("IGameTypes::SetGameTypeAndMode (ints)", "changes the server game type and mode");
	context.SkipCall("IGameTypes::SetAndParseExtendedServerInfo", "rewrites the table from a KeyValues we would have to invent");
	context.SkipCall("IGameTypes::CheckShouldSetDefaultGameModeAndType", "may set the default mode as a side effect");
	context.SkipCall("IGameTypes::ApplyConvarsForCurrentMode", "writes the mode convars");
	context.SkipCall("IGameTypes::DisplayConvarsForCurrentMode", "prints to the server console, not a result");
	context.SkipCall("IGameTypes::CreateOrUpdateWorkshopMapGroup", "adds a map group to the table");
	context.SkipCall("IGameTypes::ApplyConvarsForMap", "writes the map convars");
	context.SkipCall("IGameTypes::SetCustomBotDifficulty", "changes bot difficulty and applies convars");
	context.SkipCall("IGameTypes::ApplyConvarsForCurrentBotDifficulty", "writes bot convars");
	context.SkipCall("IGameTypes::AddMapInfo", "adds a map to the table from a KeyValues we would have to invent");
}

} // namespace cs2st
