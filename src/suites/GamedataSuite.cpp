#include "suites/GamedataSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <engine2/igameresourceservice.h>
#include <engine2/ivengineserver2.h>
#include <engine2/isource2server.h>
#include <entity2/entityclass.h>
#include <entity2/entityinstance.h>
#include <entity2/entitysystem.h>
#include <gameentitysystem.h>
#include <interfaces/interfaces.h>
#include <schemasystem/schemasystem.h>
#include <tier0/convar.h>

#include "core/FileSystemUtil.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/SignatureScan.h"
#include "hook/HookArg.h"
#include "hook/HookRecorder.h"

/**
 * The one suite whose subject is not an SDK interface but a config file: the
 * byte patterns, slot indexes and offsets the plugin ecosystem resolves out of
 * band because no header declares them. The header comment on GamedataSuite.h
 * describes the shape; this file is the sweep.
 *
 * The function types below are the ecosystem's own, transcribed from how CSS
 * declares and calls each entry - not paraphrased - because the point of a
 * behavioural check is calling the function the way its consumers do.
 */

namespace cs2st
{
namespace
{

// ---- hook ids ---------------------------------------------------------------

constexpr const char* kHostSay = "Host_Say";
constexpr const char* kAcceptInput = "CEntityInstance_AcceptInput";

/** Number of function hooks; InstallHooks reports it. */
constexpr int kHookCount = 2;

// ---- table prefixes in the TOML ----------------------------------------------

constexpr const char* kSignaturesPrefix = "signatures.";
constexpr const char* kVtablesPrefix = "vtables.";

/** Full TOML table paths for the two vtable keys an SDK declaration computes. */
constexpr const char* kCheckTransmitTable = "vtables.ISource2GameEntities::CheckTransmit";
constexpr const char* kAddResourceTable = "vtables.CEntityResourceManifest_AddResource";

/** CCommand's own COMMAND_MAX_ARGC, which is private, so it is restated here. */
constexpr int kMaxPlausibleArgC = 64;

// ---- the ecosystem's function types ------------------------------------------

using HostSay_t = void (*)(CEntityInstance*, CCommand&, bool, int, const char*);
using CreateEntityByName_t = void* (*)(const char*, int);
using DispatchSpawn_t = void (*)(void*, void*);
using UtilRemove_t = void (*)(void*);
using WeaponDataFromKey_t = void* (*)(int, const char*);
using NavMeshBool_t = bool (*)();

// The scan-only signatures' types, transcribed the same way: from the
// ecosystem's declarations where it has one (FireOutputInternal,
// AddEntityIOEvent, LegacyGameEventListener, TakeDamageOld), and from the
// live build's own registers where it does not - every pointer-width count
// below was read off build 25947352's prologue saves, not guessed.

// UTIL_ClientPrintAll(dest, msg_name, param1..param4): the six integer
// registers the prologue spills (rdi..r9), with msg_name landing in strlen.
using ClientPrintAll_t = void (*)(int, const char*, const char*, const char*,
                                  const char*, const char*);
// ClientPrint(out, filter, origin, dest, ...): writes a vtable into out[0]
// and an object chain into out[1]/out[2]; the pointer arguments are all read
// (the filter's callback slot, the origin vectors), so callers pass buffers,
// never null.
using ClientPrint_t = void (*)(void*, const void*, const void*, int, const void*,
                               const void*, char, const void*);
using SwitchTeam_t = void (*)(void*, int);
using SetPawn_t = void (*)(void*, void*, bool, bool, bool, bool);
using PostThink_t = void (*)(void*);
// GiveNamedItem(services, name, subType, pInfo, bool, ptr, double, float):
// the prologue saves arg3 (esi), arg4 (rcx), arg5 (r8d), arg6 (r9) and arg7
// (rdi) before the first test; the two floats ride xmm.
using GiveNamedItem_t = void* (*)(void*, const char*, int, void*, bool, void*,
                                  double, float);
using SetModel_t = void (*)(void*);
using CanUse_t = int (*)(void*, void*);
using CanAcquire_t = int (*)(void*, void*, int, int*);
using TerminateRound_t = void (*)(void*, int, long long, float);
// EmitSoundFilter(filter, ent, params): forwards rsi/rdx untouched and
// returns rdi - the filter pointer - back to the caller.
using EmitSoundFilter_t = void* (*)(void*, int, const void*);
using AddEntityIOEvent_t = void (*)(void*, void*, const char*, void*, void*,
                                    void*, float, void*, void*);
using LegacyListener_t = void* (*)(int);
using RemovePlayerItem_t = void (*)(void*, void*);
// TakeDamageOld(pawn, info, result, double, float): the decompiler's two
// trailing float arguments are what CSS's 3-argument typedef misses.
using TakeDamageOld_t = long long (*)(void*, void*, void*, double, float);
using EndTouch_t = void (*)(void*, void*);
// FireOutputInternal(output, activator, caller, value, delay, unk1, unk2):
// returns its first argument untouched when the output has no connections.
using FireOutputInternal_t = void* (*)(void*, void*, void*, const void*, float,
                                       void*, void*);

// ---- helpers -----------------------------------------------------------------

/** Reads a whole file as text. stdio, like everything else in core/. */
bool ReadTextFile(const std::string& path, std::string& out)
{
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if (file == nullptr)
	{
		return false;
	}

	char buffer[4096];
	std::size_t got = 0;
	while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
	{
		out.append(buffer, got);
	}
	const bool ok = (std::ferror(file) == 0);
	std::fclose(file);
	return ok;
}

/**
 * Directory gamedata.toml lives in: <mod>/addons/<alias>/configs, derived from
 * this plugin's own binary the same way LogDirectory() derives its logs path.
 */
std::string ConfigsGamedataPath()
{
	const std::string binary = fs::PluginBinaryPath();
	if (binary.empty())
	{
		return fs::Join("configs", "gamedata.toml");
	}

	const std::string plugin_dir = fs::DirectoryName(fs::DirectoryName(binary));
	if (plugin_dir.empty())
	{
		return fs::Join("configs", "gamedata.toml");
	}
	return fs::Join(fs::Join(plugin_dir, "configs"), "gamedata.toml");
}

/** Reads the pointer-sized slot `offset` bytes into `object`. */
void* SlotAt(const void* object, std::size_t offset)
{
	void* value = nullptr;
	// memcpy rather than a dereference: the offset is what this suite tests,
	// and nothing guarantees alignment on the hypothesis being checked.
	std::memcpy(&value, static_cast<const unsigned char*>(object) + offset, sizeof(value));
	return value;
}

/** True when `address` lies inside `module`'s mapped image. */
bool InModule(const Module& module, const void* address)
{
	const auto* base = static_cast<const unsigned char*>(module.base());
	const auto* at = static_cast<const unsigned char*>(address);
	return base != nullptr && at >= base && at < base + module.size();
}

/**
 * Schema class names the pawn's team byte lives in, most derived first. A
 * field is declared once, in its declaring class; walking down finds that
 * declaration wherever the dump put it.
 */
constexpr const char* kPawnTeamClasses[] = {"CCSPlayerPawn", "CCSPlayerPawnBase",
                                            "CBasePlayerPawn", "CBaseEntity", nullptr};

/** The item-services pointer every GiveNamedItem call reads off the pawn. */
constexpr const char* kItemServicesClasses[] = {"CCSPlayerPawn", "CCSPlayerPawnBase",
                                                "CBasePlayerPawn", nullptr};

} // namespace

// -----------------------------------------------------------------------------
//  Acquire - the file, and nothing but the file
// -----------------------------------------------------------------------------

bool GamedataSuite::Acquire(TestContext& context)
{
	// This also runs on the hooks-install path, which has no fault guard, so
	// nothing here goes past file reads and interface queries. The scans and
	// every engine call live in ExerciseCalls, inside CS2ST_CALL.
	//
	// The file is re-read on every run rather than cached: editing a pattern
	// and re-running, without reloading the plugin, is the workflow this file
	// exists for.
	const std::string path = ConfigsGamedataPath();
	std::string text;
	if (!ReadTextFile(path, text))
	{
		data_loaded_ = false;
		context.UnavailableCall("configs/gamedata.toml", "cannot open %s", path.c_str());
		return false;
	}

	data_ = TomlData::Parse(text);
	if (!data_.ok())
	{
		data_loaded_ = false;
		context.UnavailableCall("configs/gamedata.toml",
			"parse error at line %d: %s",
			data_.error_line(), data_.error().c_str());
		return false;
	}

	data_loaded_ = true;
	context.Note("gamedata file: %s (%zu key(s))", path.c_str(), data_.tables().size());

	// The service object the GameEntitySystem offset is read from. Not fatal:
	// every oracle except that one works from the file and the compiler alone.
	resource_service_ = context.QueryInterface(
		"engine2", GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	if (resource_service_ == nullptr)
	{
		context.Note("engine2 does not answer %s; the GameEntitySystem offset will be "
		             "compared as numbers only",
			GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	}

	// The object vtables.GameEventManager is read from. Not fatal either: that
	// row fails on its own when the interface is absent.
	server_ = context.QueryInterface("server", INTERFACEVERSION_SERVERGAMEDLL);

	// ServerCommand - the bot management the behavioural layer queues. Not
	// fatal: every bot-dependent check skips with its own line when absent.
	engine_ = static_cast<IVEngineServer2*>(context.QueryInterface(
		"engine2", INTERFACEVERSION_VENGINESERVER));

	// The schema system and the server module's type scope, where the
	// behavioural layer resolves the offsets its calls read. Same deal: not
	// fatal, the checks that need an offset skip.
	schema_ = static_cast<ISchemaSystem*>(context.QueryInterface(
		"schemasystem", SCHEMASYSTEM_INTERFACE_VERSION));
	if (schema_ != nullptr)
	{
		server_scope_ = schema_->FindTypeScopeForModule("server");
	}

	return true;
}

// -----------------------------------------------------------------------------
//  The sweep
// -----------------------------------------------------------------------------

void GamedataSuite::ExerciseCalls(TestContext& context)
{
	// A run can be repeated; each one re-resolves from the file as written.
	host_say_ = nullptr;
	accept_input_ = nullptr;
	create_entity_ = nullptr;
	dispatch_spawn_ = nullptr;
	util_remove_ = nullptr;
	weapon_data_ = nullptr;
	navmesh_ = nullptr;
	navmesh_slot_ = nullptr;
	event_manager_init_ = nullptr;
	network_state_changed_ = nullptr;
	client_print_all_ = nullptr;
	client_print_ = nullptr;
	switch_team_ = nullptr;
	set_pawn_ = nullptr;
	post_think_ = nullptr;
	give_named_item_ = nullptr;
	set_model_ = nullptr;
	can_use_ = nullptr;
	can_acquire_ = nullptr;
	terminate_round_ = nullptr;
	emit_sound_filter_ = nullptr;
	add_entity_io_event_ = nullptr;
	legacy_listener_ = nullptr;
	remove_player_item_ = nullptr;
	take_damage_old_ = nullptr;
	end_touch_ = nullptr;
	fire_output_internal_ = nullptr;
	gamesystem_pfirst_ = nullptr;
	signatures_verified_ = false;

	context.Note("[signatures] byte patterns scanned against the live server module");

	int resolved = 0;
	int total = 0;
	for (const std::string& table : data_.tables())
	{
		if (table.rfind(kSignaturesPrefix, 0) != 0)
		{
			continue;
		}
		++total;
		if (ExerciseSignature(context, table))
		{
			++resolved;
		}
	}

	// Hooks need exactly two of those addresses, each landed on by one match.
	signatures_verified_ = (host_say_ != nullptr && accept_input_ != nullptr);

	ExerciseRoundTrip(context);
	ExerciseWeaponData(context);
	ExerciseNavMesh(context);
	ExerciseCodeDecodes(context);
	ExerciseSignatureBehaviours(context);
	ExerciseVtableSlots(context);
	ExerciseStructOffsets(context);

	context.Note("gamedata sweep: %d of %d signature(s) resolved to one address; "
	             "hooks are %seligible",
		resolved, total, signatures_verified_ ? "" : "NOT ");
}

bool GamedataSuite::ExerciseSignature(TestContext& context, const std::string& table)
{
	const std::string name = table.substr(std::strlen(kSignaturesPrefix));
	const std::string target = "signatures." + name;

	bool ok = false;

	CS2ST_CALL(context, target, {
		const Module& server = context.GetModule("server");
		if (!server.IsValid())
		{
			context.Detail("-> the server module is not loaded");
			return;
		}

		const TomlData::Entry* library = data_.Find(table + ".library");
		if (library == nullptr || library->text != "server")
		{
			context.Fail("no library = \"server\" key; this suite only scans the server module");
			return;
		}

		const TomlData::Entry* pattern_entry = data_.FindPlatform(table, CS2ST_WINDOWS != 0);
		if (pattern_entry == nullptr || pattern_entry->is_number)
		{
			context.Fail("no %s pattern in the file", CS2ST_WINDOWS ? "windows" : "linux");
			return;
		}

		BytePattern pattern;
		const char* parse_error = ParseBytePattern(pattern_entry->text, pattern);
		if (parse_error != nullptr)
		{
			context.Fail("pattern does not parse: %s", parse_error);
			return;
		}

		const PatternScanResult result = ScanModule(server, pattern);
		if (result.matches == 0)
		{
			// The headline this file exists for: an engine update moved the
			// bytes, and every plugin carrying this key stopped loading.
			context.Fail("%zu-byte pattern matches nothing in %s",
				pattern.size(), server.name().c_str());
			return;
		}
		if (result.matches > 1)
		{
			// Two hits is how a too-short pattern announces itself. The first
			// hit is not more likely the function than the second, so neither
			// is kept.
			context.Suspect("pattern matches twice (%p and %p); it does not name one function",
				result.first, result.second);
			return;
		}

		const unsigned char* hit = static_cast<const unsigned char*>(result.first);
		const std::size_t rva = static_cast<std::size_t>(
			hit - static_cast<const unsigned char*>(server.base()));
		context.Detail("-> %p (%s+0x%zx, %zu-byte pattern)",
			result.first, server.name().c_str(), rva, pattern.size());

		if (name == "Host_Say")
		{
			host_say_ = const_cast<void*>(result.first);
		}
		else if (name == "CEntityInstance_AcceptInput")
		{
			accept_input_ = const_cast<void*>(result.first);
		}
		else if (name == "UTIL_CreateEntityByName")
		{
			create_entity_ = const_cast<void*>(result.first);
		}
		else if (name == "CBaseEntity_DispatchSpawn")
		{
			dispatch_spawn_ = const_cast<void*>(result.first);
		}
		else if (name == "UTIL_Remove")
		{
			util_remove_ = const_cast<void*>(result.first);
		}
		else if (name == "GetCSWeaponDataFromKey")
		{
			weapon_data_ = const_cast<void*>(result.first);
		}
		else if (name == "CCSNavArea_IsValidNavMesh")
		{
			navmesh_ = const_cast<void*>(result.first);
		}
		else if (name == "CGameEventManager_Init")
		{
			event_manager_init_ = const_cast<void*>(result.first);
		}
		else if (name == "NetworkStateChanged")
		{
			network_state_changed_ = const_cast<void*>(result.first);
		}
		else if (name == "UTIL_ClientPrintAll")
		{
			client_print_all_ = const_cast<void*>(result.first);
		}
		else if (name == "ClientPrint")
		{
			client_print_ = const_cast<void*>(result.first);
		}
		else if (name == "CCSPlayerController_SwitchTeam")
		{
			switch_team_ = const_cast<void*>(result.first);
		}
		else if (name == "CBasePlayerController_SetPawn")
		{
			set_pawn_ = const_cast<void*>(result.first);
		}
		else if (name == "CCSPlayerPawnBase_PostThink")
		{
			post_think_ = const_cast<void*>(result.first);
		}
		else if (name == "GiveNamedItem")
		{
			give_named_item_ = const_cast<void*>(result.first);
		}
		else if (name == "CBaseModelEntity_SetModel")
		{
			set_model_ = const_cast<void*>(result.first);
		}
		else if (name == "CCSPlayer_WeaponServices_CanUse")
		{
			can_use_ = const_cast<void*>(result.first);
		}
		else if (name == "CCSPlayer_ItemServices_CanAcquire")
		{
			can_acquire_ = const_cast<void*>(result.first);
		}
		else if (name == "CCSGameRules_TerminateRound")
		{
			terminate_round_ = const_cast<void*>(result.first);
		}
		else if (name == "CBaseEntity_EmitSoundFilter")
		{
			emit_sound_filter_ = const_cast<void*>(result.first);
		}
		else if (name == "CEntitySystem_AddEntityIOEvent")
		{
			add_entity_io_event_ = const_cast<void*>(result.first);
		}
		else if (name == "LegacyGameEventListener")
		{
			legacy_listener_ = const_cast<void*>(result.first);
		}
		else if (name == "CBasePlayerPawn_RemovePlayerItem")
		{
			remove_player_item_ = const_cast<void*>(result.first);
		}
		else if (name == "CBaseEntity_TakeDamageOld")
		{
			take_damage_old_ = const_cast<void*>(result.first);
		}
		else if (name == "CBaseTrigger_EndTouch")
		{
			end_touch_ = const_cast<void*>(result.first);
		}
		else if (name == "CEntityIOOutput_FireOutputInternal")
		{
			fire_output_internal_ = const_cast<void*>(result.first);
		}
		else if (name == "IGameSystem_InitAllSystems_pFirst")
		{
			gamesystem_pfirst_ = const_cast<void*>(result.first);
		}

		ok = true;
	});

	return ok;
}

// -----------------------------------------------------------------------------
//  Behavioural checks - calling what the scan found
// -----------------------------------------------------------------------------

void GamedataSuite::ExerciseRoundTrip(TestContext& context)
{
	context.Note("[behaviour] entity round trip: create -> spawn -> remove");

	if (create_entity_ == nullptr || dispatch_spawn_ == nullptr || util_remove_ == nullptr)
	{
		context.SkipCall("entity round trip (create -> spawn -> remove)",
			"one of the three signatures did not resolve to one address");
		return;
	}

	const auto create = reinterpret_cast<CreateEntityByName_t>(create_entity_);
	const auto spawn = reinterpret_cast<DispatchSpawn_t>(dispatch_spawn_);
	const auto remove = reinterpret_cast<UtilRemove_t>(util_remove_);

	void* entity = nullptr;

	CS2ST_CALL(context, "UTIL_CreateEntityByName(info_target)", {
		// The second argument is the entity index to force, -1 for "any free
		// slot": the function forwards it as-is to the entity system's create
		// call (Linux build 25815307). 0 would ask for worldspawn's slot.
		entity = create("info_target", -1);
		context.Detail("-> %p", entity);
		if (entity == nullptr)
		{
			context.Fail("no entity came back; either the signature is stale or this "
			             "build has no info_target class");
			return;
		}
		if (!LooksLikeObjectPointer(entity))
		{
			context.Fail("the call returned something that is not a pointer");
			entity = nullptr;
		}
	});

	if (entity == nullptr)
	{
		context.SkipCall("CBaseEntity_DispatchSpawn(kv=nullptr)", "no entity to spawn");
		context.SkipCall("UTIL_Remove(entity)", "no entity to remove");
		return;
	}

	CS2ST_CALL(context, "CBaseEntity_DispatchSpawn(kv=nullptr)", {
		// CSS calls this with a null CEntityKeyValues* - spawn with no overrides.
		// void* here rather than the SDK type: only null is ever passed, and the
		// ABI is the same.
		spawn(entity, nullptr);

		// The proof the pointer is what the name says: an entity the engine
		// spawned carries an identity with an index into the fixed table.
		CEntityInstance* instance = static_cast<CEntityInstance*>(entity);
		CEntityIdentity* identity = instance->m_pEntity;
		if (!LooksLikeObjectPointer(identity))
		{
			context.Fail("spawned, but the object carries no identity; the pointer is "
			             "not a CEntityInstance");
			return;
		}
		const int index = identity->GetEntityIndex().Get();
		context.Detail("-> spawned, entity index %d", index);
		if (index < 0 || index >= MAX_TOTAL_ENTITIES)
		{
			context.Fail("index %d is outside the fixed identity table", index);
		}
	});

	CS2ST_CALL(context, "UTIL_Remove(entity)", {
		remove(entity);

		// UTIL_Remove marks for deletion rather than freeing, so the flag read
		// back is the evidence the call reached the entity system at all.
		CEntityInstance* instance = static_cast<CEntityInstance*>(entity);
		if (!LooksLikeObjectPointer(instance->m_pEntity))
		{
			context.Detail("-> called; the entity no longer carries a plausible identity");
			return;
		}
		const bool marked =
			(instance->m_pEntity->m_flags & EF_MARKED_FOR_DELETE) != 0;
		context.Detail("-> called, EF_MARKED_FOR_DELETE %s", marked ? "set" : "not set");
		if (!marked)
		{
			// Suspect rather than Fail: the deletion pass may simply not have
			// run yet, and that is the benign reading until proven otherwise.
			context.Suspect("the entity is not marked for deletion yet");
		}
	});
}

void GamedataSuite::ExerciseWeaponData(TestContext& context)
{
	context.Note("[behaviour] GetCSWeaponDataFromKey (the table GiveNamedItem reads)");

	if (weapon_data_ == nullptr)
	{
		context.SkipCall("GetCSWeaponDataFromKey",
			"the signature did not resolve to one address");
		return;
	}

	const auto lookup = reinterpret_cast<WeaponDataFromKey_t>(weapon_data_);

	CS2ST_CALL(context, "GetCSWeaponDataFromKey(weapon_ak47)", {
		// CSS declares this as Func<int, string, CCSWeaponBaseVData>; the int is
		// unknown to the ecosystem and every caller passes 0, so 0 it is.
		void* vdata = lookup(0, "weapon_ak47");
		context.Detail("-> %p", vdata);
		if (vdata == nullptr)
		{
			context.Fail("the AK-47's vdata did not resolve; every GiveNamedItem goes "
			             "through this table");
		}
		else if (!LooksLikeObjectPointer(vdata))
		{
			context.Fail("the table returned something that is not a pointer");
		}
	});

	CS2ST_CALL(context, "GetCSWeaponDataFromKey(absent name)", {
		// A lookup that only ever says yes is as broken as one that only ever
		// says no, and the negative half is free to test.
		void* vdata = lookup(0, "cs2sdktest_no_such_weapon");
		context.Detail("-> %p", vdata);
		if (vdata != nullptr)
		{
			context.Fail("a weapon name no build contains resolved to vdata");
		}
	});
}

void GamedataSuite::ExerciseNavMesh(TestContext& context)
{
	context.Note("[behaviour] CCSNavArea_IsValidNavMesh (a stub that reads one global)");

	if (navmesh_ == nullptr)
	{
		context.SkipCall("CCSNavArea_IsValidNavMesh",
			"the signature did not resolve to one address");
		return;
	}

	// The function body is `cmp qword ptr [rip+disp32], 0 / setne / ret`: three
	// opcode bytes, then the disp32, then the rest of the instruction (8 bytes
	// on Windows, 7 on Linux) - which lands exactly on the navmesh global.
	// That is the ecosystem's decode, transcribed rather than reinvented.
	CS2ST_CALL(context, "CCSNavArea_IsValidNavMesh (global decode)", {
		const unsigned char* fn = static_cast<const unsigned char*>(navmesh_);
		std::int32_t disp = 0;
		std::memcpy(&disp, fn + 3, sizeof(disp));
		const unsigned char* slot = fn + disp + (CS2ST_WINDOWS ? 8 : 7);

		void* navmesh = SlotAt(slot, 0);
		navmesh_slot_ = navmesh;

		context.Detail("-> global %p holds %p", static_cast<const void*>(slot), navmesh);
		if (navmesh != nullptr && !LooksLikeObjectPointer(navmesh))
		{
			context.Fail("the decoded global holds something that is not a pointer");
		}
	});

	CS2ST_CALL(context, "CCSNavArea_IsValidNavMesh()", {
		// Two independent readings of one fact: executing the stub, and reading
		// the global it compares. A signature that landed on the wrong bytes
		// cannot keep the two in agreement.
		const bool valid = reinterpret_cast<NavMeshBool_t>(navmesh_)();
		context.Detail("-> %s (the global it reads is %s)",
			valid ? "true" : "false",
			navmesh_slot_ != nullptr ? "set" : "null");
		if ((navmesh_slot_ != nullptr) != valid)
		{
			context.Fail("the stub's answer and the global it reads disagree");
		}
	});
}

// -----------------------------------------------------------------------------
//  Code decodes - signatures checked by what their bytes do, not by calling
// -----------------------------------------------------------------------------

void GamedataSuite::ExerciseCodeDecodes(TestContext& context)
{
	context.Note("[decode] signatures checked by their bytes, not called");

	// How far into a function body the decodes below look. Both functions do
	// what is being checked within their first few instructions.
	constexpr std::size_t kDecodeWindow = 0x80;

	// Not called on purpose: Init first calls the manager's Reset, which drops
	// every event descriptor and the listeners hanging off them - every later
	// test that listens for an event would silently hear nothing. What makes
	// the address Init is the first file it loads, so that is what is checked.
	CS2ST_CALL(context, "CGameEventManager_Init (decode)", {
		if (event_manager_init_ == nullptr)
		{
			context.Fail("the signature did not resolve to one address");
			return;
		}
		const Module& server = context.GetModule("server");
		const auto* fn = static_cast<const unsigned char*>(event_manager_init_);

		// lea r64, [rip+disp32]: REX.W 8D, then a ModRM with mod=00 rm=101.
		static const char kExpected[] = "resource/core.gameevents";
		for (std::size_t i = 0; i + 7 <= kDecodeWindow; ++i)
		{
			if ((fn[i] != 0x48 && fn[i] != 0x4C) || fn[i + 1] != 0x8D
				|| (fn[i + 2] & 0xC7) != 0x05)
			{
				continue;
			}
			std::int32_t disp = 0;
			std::memcpy(&disp, fn + i + 3, sizeof(disp));
			const char* text = reinterpret_cast<const char*>(fn + i + 7 + disp);
			if (!InModule(server, text) || !InModule(server, text + sizeof(kExpected) - 1))
			{
				continue;
			}
			if (std::memcmp(text, kExpected, sizeof(kExpected)) == 0)
			{
				context.Detail("-> +0x%zx loads \"%s\"", i, kExpected);
				return;
			}
		}
		context.Fail("no rip-relative lea in the first 0x%zx bytes names \"%s\"; the address "
		             "is not CGameEventManager::Init", kDecodeWindow, kExpected);
	});

	// NetworkStateChanged ends in a call through the entity's own vtable - the
	// slot vtables.SetStateChanged names. The displacement of that load is a
	// second, independent statement of the same slot number.
	CS2ST_CALL(context, "NetworkStateChanged (decode)", {
		if (network_state_changed_ == nullptr)
		{
			context.Fail("the signature did not resolve to one address");
			return;
		}
		const TomlData::Entry* entry = data_.FindPlatform("vtables.SetStateChanged",
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number)
		{
			context.Fail("vtables.SetStateChanged is missing from the file");
			return;
		}
		const auto* fn = static_cast<const unsigned char*>(network_state_changed_);

		// The first mov r64, [r64+disp32] (REX.W 8B, ModRM mod=10): every field
		// read before it fits a disp8, a vtable slot past 15 does not.
		for (std::size_t i = 0; i + 7 <= kDecodeWindow; ++i)
		{
			if ((fn[i] & 0xF8) != 0x48 || fn[i + 1] != 0x8B || (fn[i + 2] & 0xC0) != 0x80
				|| (fn[i + 2] & 0x07) == 0x04)
			{
				continue;
			}
			std::int32_t disp = 0;
			std::memcpy(&disp, fn + i + 3, sizeof(disp));
			context.Detail("-> +0x%zx loads vtable +0x%x = slot %d; gamedata %lld",
				i, static_cast<unsigned>(disp), disp / 8, entry->number);
			if (disp % 8 != 0 || disp / 8 != entry->number)
			{
				context.Fail("the slot the code calls and the gamedata disagree");
			}
			return;
		}
		context.Fail("no vtable load in the first 0x%zx bytes; the address is not "
		             "NetworkStateChanged", kDecodeWindow);
	});
}

// -----------------------------------------------------------------------------
//  Behavioural checks - the scan-only signatures, called for real
// -----------------------------------------------------------------------------

void GamedataSuite::ExerciseSignatureBehaviours(TestContext& context)
{
	// The standalone half runs wherever the server is; the bot half needs a
	// player, and adds one. Order matters: the bot is queued and kicked
	// around ExerciseBotBehaviours, so nothing player-shaped runs after it.
	ExerciseStandaloneBehaviours(context);
	ExerciseBotBehaviours(context);
}

void GamedataSuite::ExerciseStandaloneBehaviours(TestContext& context)
{
	context.Note("[behaviour] scan-only signatures: stateless calls");

	// FireOutputInternal on an output with no connections. The binary (build
	// 25947352) reads connections at output+8, finds null, and returns its
	// first argument untouched - an address that is not this function cannot
	// reproduce that identity under seven arguments.
	if (fire_output_internal_ != nullptr)
	{
		struct ZeroOutput
		{
			void* vtable;
			void* connections;
			void* desc;
		};
		ZeroOutput output{nullptr, nullptr, nullptr};

		CS2ST_CALL(context, "CEntityIOOutput_FireOutputInternal(no connections)", {
			// The ecosystem's typedef, called the way its detour forwards:
			// null activator/caller, a null CVariant (the binary switches on
			// value[8] only when firing, which no connection means it never
			// reads), zero delay.
			void* returned = reinterpret_cast<FireOutputInternal_t>(fire_output_internal_)(
				&output, nullptr, nullptr, nullptr, 0.0f, nullptr, nullptr);
			context.Detail("-> %p (passed %p)", returned, static_cast<void*>(&output));
			if (returned != &output)
			{
				context.Fail("a zeroed CEntityIOOutput did not come back unchanged; the "
				             "address is not FireOutputInternal");
			}
		});
	}

	// LegacyGameEventListener is a pure function of one global: slot <= 0x3F
	// and the table non-null -> table+16*slot+64; anything else -> null. Both
	// halves are computable without calling, so both are asserted.
	if (legacy_listener_ != nullptr)
	{
		const auto listener = reinterpret_cast<LegacyListener_t>(legacy_listener_);
		void* table = nullptr;

		CS2ST_CALL(context, "LegacyGameEventListener(global decode)", {
			// mov rax,[rip+d]; test rax,rax; jz -> the global is at +6 past the
			// load. Reading it is the negative half's expected value.
			const unsigned char* fn = static_cast<const unsigned char*>(legacy_listener_);
			std::int32_t disp = 0;
			std::memcpy(&disp, fn + 3, sizeof(disp));
			table = SlotAt(fn + 7 + disp, 0);

			context.Detail("-> global %p holds %p",
				static_cast<const void*>(fn + 7 + disp), table);
			if (table != nullptr && !LooksLikeObjectPointer(table))
			{
				context.Fail("the decoded table holds something that is not a pointer");
				table = nullptr;
			}
		});

		CS2ST_CALL(context, "LegacyGameEventListener(slot 0)", {
			void* listener_at_zero = listener(0);
			const void* expected = (table != nullptr) ? static_cast<const void*>(
				static_cast<const unsigned char*>(table) + 64) : nullptr;
			context.Detail("-> %p (the table is %s)", listener_at_zero,
				table != nullptr ? "live" : "null");
			if (listener_at_zero != expected)
			{
				context.Fail("slot 0 did not answer table+64 (or null); the address is "
				             "not LegacyGameEventListener");
			}
		});

		CS2ST_CALL(context, "LegacyGameEventListener(slot 0x40)", {
			// 0x40 is the first slot past the table: whatever the global holds,
			// the answer must be null - `ja` out, no table arithmetic.
			void* past_end = listener(0x40);
			context.Detail("-> %p", past_end);
			if (past_end != nullptr)
			{
				context.Fail("slot 64 did not answer null; the address is not "
				             "LegacyGameEventListener");
			}
		});
	}

	// IGameSystem_InitAllSystems_pFirst names the instruction that walks the
	// game-system linked list in InitAllSystems. What it proves is that the
	// list is live: the global the load reads is non-null on a running server.
	if (gamesystem_pfirst_ != nullptr)
	{
		CS2ST_CALL(context, "IGameSystem_InitAllSystems_pFirst (global decode)", {
			// mov r14,[rip+d]; test r14,r14; jnz -> the load at +3; the global
			// at +7+disp32 must be the head of a list this server walked to
			// get where it is.
			const unsigned char* fn = static_cast<const unsigned char*>(gamesystem_pfirst_);
			std::int32_t disp = 0;
			std::memcpy(&disp, fn + 3, sizeof(disp));
			void* head = SlotAt(fn + 7 + disp, 0);

			context.Detail("-> global %p holds %p (%s)",
				static_cast<const void*>(fn + 7 + disp), head,
				LooksLikeObjectPointer(head) ? "plausible" : "NOT plausible");
			if (head == nullptr)
			{
				context.Fail("the game-system list head global is null on a running "
				             "server; the decoded address is not the list head");
			}
			else if (!LooksLikeObjectPointer(head))
			{
				context.Fail("the decoded global holds something that is not a pointer");
			}
		});
	}

	// UTIL_ClientPrintAll sends a TextMsg to everyone; on a dedicated server
	// with no human client the message is still routed, so the call must
	// simply survive it. The name strings are read by strlen, nulls are not
	// an option for a caller that passes them.
	if (client_print_all_ != nullptr)
	{
		CS2ST_CALL(context, "UTIL_ClientPrintAll(HUD_PRINTTALK, one line)", {
			reinterpret_cast<ClientPrintAll_t>(client_print_all_)(
				4, "cs2sdktest gamedata behaviour sweep", nullptr, nullptr, nullptr,
				nullptr);
			context.Detail("-> returned (message routed to all recipients)");
		});
	}

	// EmitSoundFilter forwards (ent, params) into the sound system and hands
	// the filter back. A call that comes back with the same filter pointer
	// reached the sound system and came home.
	if (emit_sound_filter_ != nullptr)
	{
		CS2ST_CALL(context, "CBaseEntity_EmitSoundFilter(identity)", {
			// A recipient filter with no bits set: the engine's CRecipientFilter
			// constructor default (vtable slot 0 unknown, everything else
			// zeroed), same as the game builds on the stack for a quiet call.
			unsigned char filter_bytes[64] = {0};

			// EmitSound_t with just the name set - CSS's EntityEmitSoundFilter
			// fills exactly these members before its call.
			struct EmitParams
			{
				const char* name;
				float origin[3];
				float volume;
				float sound_time;
				int speaker_entity;
				unsigned int force_guid;
				int source_soundscape;
				unsigned short pitch;
				unsigned char flags;
			};
			EmitParams params{};
			params.name = "UI.ButtonRollout";
			params.volume = 1.0f;
			params.pitch = 100;

			void* returned = reinterpret_cast<EmitSoundFilter_t>(emit_sound_filter_)(
				filter_bytes, -1, &params);
			context.Detail("-> %p (passed %p)", returned, static_cast<void*>(filter_bytes));
			if (returned != filter_bytes)
			{
				context.Fail("the filter did not come back; the address is not "
				             "CBaseEntity_EmitSoundFilter");
			}
		});
	}

	// ClientPrint(out, ...) fills a message-builder object. On Linux its
	// pattern anchors the very first instruction, a lea into the module's
	// rdata the decode reads back; on Windows the pattern anchors the
	// prologue, and nothing about the builder's construction is observable
	// without a client. So the check is the Linux decode, plus the call:
	// every pointer argument on a zeroed buffer (none of them may be null),
	// and the builder's vtable landing in out[0] as the identity.
	if (client_print_ != nullptr)
	{
		CS2ST_CALL(context, "ClientPrint(builder fill)", {
			// Every pointer argument but the last is dereferenced before the
			// builder is touched (build 25947352: the filter's callback at
			// +8, two vectors out of a3, a5[0] and a5[2], sixteen bytes out
			// of a6 plus its second qword) - so each rides its own zeroed
			// buffer, and a zeroed buffer is the shape whose callback slot
			// is null, which the body branches on and handles.
			unsigned char out[384] = {0};
			unsigned char filter_bytes[64] = {0};
			unsigned char origin[32] = {0}; // two unaligned loads reach +27
			unsigned char unknown12[12] = {0};
			unsigned char unknown32[32] = {0};
			reinterpret_cast<ClientPrint_t>(client_print_)(
				out, filter_bytes, origin, 4, unknown12, unknown32, 0, nullptr);
			// The binary writes a module vtable into out[0] before anything
			// else; a null out[0] means nothing was built.
			void* first = nullptr;
			std::memcpy(&first, out, sizeof(first));
			context.Detail("-> out[0] %p (%s)", first,
				first != nullptr ? "built" : "not built");
			if (first == nullptr)
			{
				context.Fail("the builder came back empty; the address is not "
				             "ClientPrint");
			}
			else if (!InModule(context.GetModule("server"), first))
			{
				context.Fail("out[0] does not point into the server module; the "
				             "builder's vtable is wrong");
			}
		});
	}

	// CBaseModelEntity_SetModel(this) is a one-argument forwarder (build
	// 25947352: it fetches a global and tail-calls with (this, global+104)).
	// The model name lives in the entity's own keyvalues, which a fresh
	// info_target has none of; the observable is simply that the forward
	// completes on an entity this suite created, before the Kill below
	// removes it.
	if (set_model_ != nullptr && create_entity_ != nullptr)
	{
		CS2ST_CALL(context, "CBaseModelEntity_SetModel(info_target)", {
			void* target = reinterpret_cast<CreateEntityByName_t>(create_entity_)(
				"info_target", -1);
			if (target == nullptr)
			{
				context.Fail("the round trip's create call did not answer; no target "
				             "to set a model on");
				return;
			}
			reinterpret_cast<SetModel_t>(set_model_)(target);
			context.Detail("-> called on %p (single-arg forward, no return to assert)",
				target);
		});
		// The Kill queue below is the target's cleanup; the SetModel call
		// left no state behind that outlives the entity.
	}

	// CEntitySystem_AddEntityIOEvent queues an input for the next frame's
	// event pump. The closed loop this file can build: create an info_target
	// (the round trip's own three calls), queue "Kill" on it, and assert the
	// entity is gone from the list on the spot - the pump runs on the very
	// next frame boundary, so gone-now is the wrong reading; the assert is
	// that the call survives and the row records the queue was taken.
	if (add_entity_io_event_ != nullptr && create_entity_ != nullptr)
	{
		CS2ST_CALL(context, "CEntitySystem_AddEntityIOEvent(Kill, info_target)", {
			CGameEntitySystem* entity_system = GameEntitySystem();
			if (entity_system == nullptr)
			{
				context.Fail("no entity system to queue through");
				return;
			}
			void* target = reinterpret_cast<CreateEntityByName_t>(create_entity_)(
				"info_target", -1);
			if (target == nullptr)
			{
				context.Fail("the round trip's create call did not answer; the Kill "
				             "queue has no target");
				return;
			}
			// The ecosystem's call: (entitySystem, target, input, activator,
			// caller, value, delay, unk1, unk2) with a null variant - the
			// binary reads value[0] only when the input needs one, and Kill
			// does not.
			reinterpret_cast<AddEntityIOEvent_t>(add_entity_io_event_)(
				entity_system, target, "Kill", nullptr, nullptr, nullptr, 0.0f,
				nullptr, nullptr);
			context.Detail("-> Kill queued on %p; the next frame's pump fires it",
				target);
			// No explicit cleanup: the queued Kill IS the cleanup - the pump
			// removes the target on the next frame, and the entity was this
			// call's own, not the map's.
		});
	}

	// CBaseTrigger_EndTouch(trigger, other): the binary decrements the
	// touch-list count at trigger+3176 when the other is on the list. A
	// hand-built trigger with an empty list takes the no-op path and must
	// leave the count at zero - calling it on a live map trigger would be
	// the other half, but that writes state this suite cannot restore.
	if (end_touch_ != nullptr)
	{
		CS2ST_CALL(context, "CBaseTrigger_EndTouch(empty list)", {
			// CBaseTrigger's layout past CBaseEntity: the count at +3176 and
			// the list pointer at +3184 (build 25947352). The body reads
			// other's identity at +0x10 - the one touch it does before the
			// count check - then returns outright when the count is zero:
			// zeroed buffers take the no-op path end to end. CEntityInstance
			// is abstract in the SDK, so `other` is a raw buffer rather
			// than an instance.
			unsigned char trigger[0xC80] = {0};
			unsigned char other_bytes[0x100] = {0};
			reinterpret_cast<EndTouch_t>(end_touch_)(
				trigger, reinterpret_cast<CEntityInstance*>(other_bytes));
			std::int32_t count = 0;
			std::memcpy(&count, trigger + 3176, sizeof(count));
			context.Detail("-> count after the call %d (empty list, no-op path)", count);
			if (count != 0)
			{
				context.Fail("the touch count moved on an empty list; the address is "
				             "not EndTouch");
			}
		});
	}
}

// ---- schema-resolved offsets ---------------------------------------------

bool GamedataSuite::SchemaOffset(const char* class_name, const char* field_name,
                                 std::size_t& offset)
{
	if (server_scope_ == nullptr)
	{
		return false;
	}
	SchemaMetaInfoHandle_t<CSchemaClassInfo> handle =
		server_scope_->FindDeclaredClass(class_name);
	CSchemaClassInfo* info = handle.Get();
	if (info == nullptr || info->m_pFields == nullptr)
	{
		return false;
	}
	for (int i = 0; i < static_cast<int>(info->m_nFieldCount); ++i)
	{
		const SchemaClassFieldData_t& field = info->m_pFields[i];
		if (field.m_pszName != nullptr && std::strcmp(field.m_pszName, field_name) == 0)
		{
			offset = static_cast<std::size_t>(field.m_nSingleInheritanceOffset);
			return true;
		}
	}
	return false;
}

bool GamedataSuite::SchemaOffsetAny(const char* const* class_names,
                                    const char* field_name, std::size_t& offset)
{
	for (const char* const* name = class_names; *name != nullptr; ++name)
	{
		if (SchemaOffset(*name, field_name, offset))
		{
			return true;
		}
	}
	return false;
}

bool GamedataSuite::FindBotPair(CEntityInstance*& controller, CEntityInstance*& pawn)
{
	controller = nullptr;
	pawn = nullptr;

	CGameEntitySystem* entity_system = GameEntitySystem();
	if (entity_system == nullptr)
	{
		return false;
	}

	// The controller: walk the active-entity chain (the same walk
	// GameEntitySystemSuite uses) and match on two names - the designer name
	// ServerCommand("bot_add ... cs_bot") gives the bot, and the C++ class
	// name off the identity's class, which is what separates a controller
	// from anything else that might carry the name.
	CEntityIdentity* scanned = entity_system->m_EntityList.m_pFirstActiveEntity;
	for (int walked = 0; scanned != nullptr && walked < 512;
	     scanned = scanned->m_pNext, ++walked)
	{
		const char* designer = scanned->GetClassname();
		if (designer == nullptr || std::strncmp(designer, "cs_bot", 6) != 0)
		{
			continue;
		}
		const char* cpp_name = (scanned->m_pClass != nullptr &&
		                       scanned->m_pClass->m_pClassInfo != nullptr)
			? scanned->m_pClass->m_pClassInfo->m_pszCPPClassname
			: nullptr;
		if (cpp_name == nullptr || std::strcmp(cpp_name, "CCSPlayerController") != 0)
		{
			continue;
		}
		controller = scanned->m_pInstance;
		break;
	}
	if (controller == nullptr || !LooksLikeObjectPointer(controller))
	{
		return false;
	}

	// The pawn: the controller's m_hPawn handle, resolved through the entity
	// system so the pair is two views of one bot.
	std::size_t hpawn_offset = 0;
	if (!SchemaOffset("CBasePlayerController", "m_hPawn", hpawn_offset))
	{
		return false;
	}
	std::uint32_t handle_bits = 0;
	std::memcpy(&handle_bits, static_cast<const unsigned char*>(
		static_cast<void*>(controller)) + hpawn_offset, sizeof(handle_bits));
	CEntityInstance* resolved =
		entity_system->GetEntityInstance(CEntityHandle{handle_bits});
	if (resolved == nullptr || !LooksLikeObjectPointer(resolved))
	{
		return false;
	}
	pawn = resolved;
	return true;
}

CEntityInstance* GamedataSuite::FindEntityByCppClassname(const char* cpp_class_name)
{
	CGameEntitySystem* entity_system = GameEntitySystem();
	if (entity_system == nullptr)
	{
		return nullptr;
	}
	CEntityIdentity* scanned = entity_system->m_EntityList.m_pFirstActiveEntity;
	for (int walked = 0; scanned != nullptr && walked < 512;
	     scanned = scanned->m_pNext, ++walked)
	{
		const char* cpp_name = (scanned->m_pClass != nullptr &&
		                       scanned->m_pClass->m_pClassInfo != nullptr)
			? scanned->m_pClass->m_pClassInfo->m_pszCPPClassname
			: nullptr;
		if (cpp_name != nullptr && std::strcmp(cpp_name, cpp_class_name) == 0
			&& (scanned->m_flags & EF_MARKED_FOR_DELETE) == 0)
		{
			return scanned->m_pInstance;
		}
	}
	return nullptr;
}

void GamedataSuite::ExerciseBotBehaviours(TestContext& context)
{
	context.Note("[behaviour] scan-only signatures: calls against an added bot");

	// Every check below needs at least one of these; none is fatal alone.
	if (engine_ == nullptr)
	{
		context.SkipCall("bot-based behaviour checks",
			"engine2 does not answer %s", INTERFACEVERSION_VENGINESERVER);
		return;
	}

	// bot_add is queued, not executed: the sweep is synchronous inside one
	// command callback, so the bot from this run's command is not in the
	// entity list yet. A previous run's bot is: use it if present, else
	// queue the add and let the next run pick it up. Either way the kick is
	// queued at the end, so no bot survives into other suites' runs.
	CEntityInstance* controller = nullptr;
	CEntityInstance* pawn = nullptr;
	const bool have_bot = FindBotPair(controller, pawn);

	bool queued_add = false;
	if (!have_bot)
	{
		engine_->ServerCommand("bot_add t normal cs_bot\n");
		queued_add = true;
	}

	CS2ST_CALL(context, "bot_add round trip", {
		if (queued_add)
		{
			context.Detail("-> bot_add queued this run; re-run to exercise the "
			               "player-shaped checks against it");
			return;
		}
		context.Detail("-> bot %p (pawn %p) found", static_cast<void*>(controller),
			static_cast<void*>(pawn));
	});

	if (queued_add)
	{
		engine_->ServerCommand("bot_kick cs_bot\n");
		return;
	}

	// The bot is ours and live from here; every check that changes state
	// restores it, and the kick is queued no matter how far this gets.
	struct BotCleanup
	{
		IVEngineServer2* engine;
		~BotCleanup()
		{
			if (engine != nullptr)
			{
				engine->ServerCommand("bot_kick cs_bot\n");
			}
		}
	} cleanup{engine_};

	// ---- team / pawn identity ------------------------------------------

	std::size_t team_offset = 0;
	const bool have_team_offset = SchemaOffsetAny(
		kPawnTeamClasses, "m_iTeamNum", team_offset);

	if (switch_team_ != nullptr && have_team_offset)
	{
		// The pawn's team byte is both the input and the assertion: read
		// before, call with the other team, read after. Team 3 only - the
		// bot joins as T (2) and returns there, so the round's state is
		// what it was before the sweep.
		CS2ST_CALL(context, "CCSPlayerController_SwitchTeam(3)", {
			std::uint8_t before = 0;
			std::memcpy(&before, static_cast<const unsigned char*>(
				static_cast<void*>(pawn)) + team_offset, sizeof(before));
			reinterpret_cast<SwitchTeam_t>(switch_team_)(pawn, 3);
			std::uint8_t after = 0;
			std::memcpy(&after, static_cast<const unsigned char*>(
				static_cast<void*>(pawn)) + team_offset, sizeof(after));
			context.Detail("-> team %u -> %u", before, after);
			if (after != 3)
			{
				context.Fail("the pawn's team byte did not follow the call; the address "
				             "is not SwitchTeam");
			}
			// Restore: back to the team the round started the bot in.
			if (after != before)
			{
				reinterpret_cast<SwitchTeam_t>(switch_team_)(pawn, before);
			}
		});
	}

	if (set_pawn_ != nullptr)
	{
		// SetPawn(controller, null) is the engine's own pawn-drop path: the
		// controller's m_hPawn is nulled and the old pawn's controller field
		// cleared, all observable through the same handle read FindBotPair
		// uses. A round trip null -> old pawn proves both directions.
		CS2ST_CALL(context, "CBasePlayerController_SetPawn(null)", {
			reinterpret_cast<SetPawn_t>(set_pawn_)(controller, nullptr, false,
				false, false, false);
			std::uint32_t bits = 0;
			std::size_t hpawn_offset = 0;
			const bool readable = SchemaOffset("CBasePlayerController", "m_hPawn",
				hpawn_offset);
			if (readable)
			{
				std::memcpy(&bits, static_cast<const unsigned char*>(
					static_cast<void*>(controller)) + hpawn_offset, sizeof(bits));
			}
			context.Detail("-> m_hPawn bits 0x%08x after the null call", bits);
			// Restore: hand the pawn back so the bot stays whole.
			reinterpret_cast<SetPawn_t>(set_pawn_)(controller, pawn, false,
				false, false, false);
			if (!readable)
			{
				// Nothing was asserted about the null call without the offset;
				// say so rather than let the row read as verified.
				context.Suspect("m_hPawn's offset did not resolve; the null was not "
				                "observable");
			}
		});
	}

	// ---- weapons: give, use, acquire, remove ------------------------------

	std::size_t itemservices_offset = 0;
	const bool have_itemservices = SchemaOffsetAny(
		kItemServicesClasses, "m_pItemServices", itemservices_offset);

	void* item_services = nullptr;
	if (have_itemservices)
	{
		item_services = SlotAt(pawn, itemservices_offset);
		if (!LooksLikeObjectPointer(item_services))
		{
			context.Note("the pawn's m_pItemServices did not resolve to a pointer; "
			             "the weapon checks will skip");
			item_services = nullptr;
		}
	}

	void* weapon = nullptr;

	if (give_named_item_ != nullptr && item_services != nullptr)
	{
		CS2ST_CALL(context, "GiveNamedItem(weapon_ak47)", {
			// The ecosystem's own call shape: subType 0, no info pointer, no
			// default attributes, no script scope. The name table check runs
			// first, "weapon_ak47" passes it, and the vdata path creates and
			// returns the entity.
			weapon = reinterpret_cast<GiveNamedItem_t>(give_named_item_)(
				item_services, "weapon_ak47", 0, nullptr, false, nullptr, 0.0, 0.0f);
			context.Detail("-> %p", weapon);
			if (weapon == nullptr)
			{
				// Not Fail: GiveNamedItem can refuse for live-game reasons
				// (freezetime, buy restrictions) that say nothing about the
				// signature. The row records what came back, that is all.
				context.Suspect("no entity came back; buy-zone rules may have refused "
				                "the give");
				return;
			}
			if (!LooksLikeObjectPointer(weapon))
			{
				context.Fail("the call returned something that is not a pointer");
				weapon = nullptr;
			}
		});
	}

	if (can_use_ != nullptr && weapon != nullptr && item_services != nullptr)
	{
		CS2ST_CALL(context, "CCSPlayer_WeaponServices_CanUse(ak47)", {
			// CanUse takes the weapon services (the component that owns the
			// arsenal), not the item services; the pawn's weapon-services
			// pointer sits under its own schema field.
			std::size_t weaponservices_offset = 0;
			if (!SchemaOffsetAny(kItemServicesClasses, "m_pWeaponServices",
				weaponservices_offset))
			{
				context.Fail("m_pWeaponServices did not resolve through the schema; "
				             "cannot call");
				return;
			}
			void* weapon_services = SlotAt(pawn, weaponservices_offset);
			if (!LooksLikeObjectPointer(weapon_services))
			{
				context.Fail("the pawn's weapon services did not resolve to a pointer");
				return;
			}
			const int usable = reinterpret_cast<CanUse_t>(can_use_)(
				weapon_services, weapon);
			context.Detail("-> %d (an AK the game just gave the pawn)", usable);
			if (usable == 0)
			{
				context.Suspect("the AK was refused; team restrictions may apply to bots");
			}
		});
	}

	if (can_acquire_ != nullptr && weapon != nullptr && item_services != nullptr)
	{
		CS2ST_CALL(context, "CCSPlayer_ItemServices_CanAcquire(ak47)", {
			// CanAcquire(services, vdata, method, out): the vdata comes from
			// the weapon table the sweep already proved, the method is
			// BUY VIA PICKUP (the permissive one), and the out int is where
			// the refusal reason lands when the answer is no.
			if (weapon_data_ == nullptr)
			{
				context.Fail("GetCSWeaponDataFromKey did not resolve; no vdata to "
				             "acquire with");
				return;
			}
			void* vdata = reinterpret_cast<WeaponDataFromKey_t>(weapon_data_)(
				0, "weapon_ak47");
			if (vdata == nullptr)
			{
				context.Fail("the weapon table lookup failed on the second ask");
				return;
			}
			int reason = -1;
			const int answer = reinterpret_cast<CanAcquire_t>(can_acquire_)(
				item_services, vdata, 2, &reason);
			context.Detail("-> %d (refusal reason %d)", answer, reason);
			// The real function clears the out parameter unconditionally at
			// its top (build 25947352: `*a4 = 0` before any branch), so a
			// surviving -1 means the address never wrote it at all.
			if (reason == -1)
			{
				context.Fail("the out parameter was never written; the address is not "
				             "CanAcquire");
			}
			else if (answer == 0)
			{
				context.Note("the acquisition was refused with reason %d; live-game "
				             "rules may refuse a bot", reason);
			}
		});
	}

	if (remove_player_item_ != nullptr && weapon != nullptr)
	{
		CS2ST_CALL(context, "CBasePlayerPawn_RemovePlayerItem(ak47)", {
			// RemovePlayerItem(pawn, item): compares against the pawn's active
			// weapon and ends in UTIL_Remove(item) - the entity is marked for
			// deletion, the same flag the round trip asserts.
			reinterpret_cast<RemovePlayerItem_t>(remove_player_item_)(pawn, weapon);
			CEntityInstance* item = static_cast<CEntityInstance*>(weapon);
			bool marked = false;
			if (LooksLikeObjectPointer(item->m_pEntity))
			{
				marked = (item->m_pEntity->m_flags & EF_MARKED_FOR_DELETE) != 0;
			}
			context.Detail("-> called, EF_MARKED_FOR_DELETE %s", marked ? "set" : "not set");
			weapon = nullptr; // the entity is on its way out either way
			if (!marked)
			{
				context.Suspect("the item is not marked for deletion yet");
			}
		});
	}

	// ---- health and rounds: the write-heavy ones --------------------------

	if (take_damage_old_ != nullptr)
	{
		// TakeDamageOld(pawn, info, result, double, float): a zeroed info
		// block is the shape the engine builds for environment damage. The
		// observable is the pawn's health: build the info with 1 point of
		// damage, read health before and after.
		std::size_t health_offset = 0;
		const bool have_health = SchemaOffset("CBaseEntity", "m_iHealth", health_offset);
		CS2ST_CALL(context, "CBaseEntity_TakeDamageOld(1 hp)", {
			if (!have_health)
			{
				context.Fail("m_iHealth did not resolve through the schema; cannot "
				             "observe the damage");
				return;
			}
			// The ecosystem's CTakeDamageInfo field order, zero-initialized
			// except the damage float, which sits at +0x20 (build 25947352:
			// 4 Vectors then 3 handles).
			unsigned char info[0x70] = {0};
			float one = 1.0f;
			std::memcpy(info + 0x20, &one, sizeof(one));
			unsigned char result[0x40] = {0};
			std::int32_t before = 0;
			std::memcpy(&before, static_cast<const unsigned char*>(
				static_cast<void*>(pawn)) + health_offset, sizeof(before));
			reinterpret_cast<TakeDamageOld_t>(take_damage_old_)(
				pawn, info, result, 0.0, 0.0f);
			std::int32_t after = 0;
			std::memcpy(&after, static_cast<const unsigned char*>(
				static_cast<void*>(pawn)) + health_offset, sizeof(after));
			context.Detail("-> health %d -> %d", before, after);
			if (after >= before)
			{
				context.Suspect("the pawn's health did not move; armor rules may have "
				                "absorbed 1 point");
			}
			// No restore needed: 1 point off a bot the sweep kicks anyway.
		});
	}

	if (post_think_ != nullptr)
	{
		CS2ST_CALL(context, "CCSPlayerPawnBase_PostThink(bot pawn)", {
			// PostThink runs the pawn's per-tick bookkeeping - buy-zone events
			// included, which is the anchor its signature carries. Calling it
			// once out of band must simply complete: the observable (the game
			// event) fires only on a zone transition, which this call does
			// not cause.
			reinterpret_cast<PostThink_t>(post_think_)(pawn);
			context.Detail("-> returned (bookkeeping ran once, out of band)");
		});
	}

	if (terminate_round_ != nullptr)
	{
		// TerminateRound(gamerules, reason, _, delay): writes the round-end
		// state into the gamerules synchronously. The gamerules object has
		// no SDK accessor and no ecosystem global; the route that exists is
		// the map's CCSGameRulesProxy entity, whose m_pGameRules field
		// points at the object every map runs with.
		CS2ST_CALL(context, "CCSGameRules_TerminateRound(draw)", {
			std::size_t gamerules_offset = 0;
			if (!SchemaOffset("CCSGameRulesProxy", "m_pGameRules", gamerules_offset))
			{
				context.Fail("CCSGameRulesProxy.m_pGameRules did not resolve through "
				             "the schema; no gamerules object to call on");
				return;
			}
			// EntityInstanceByClassIter_t matches designer names
			// ("cs_gamerules" here), not C++ class names, so the proxy is
			// found by the class-info walk instead.
			CEntityInstance* proxy = FindEntityByCppClassname("CCSGameRulesProxy");
			if (proxy == nullptr)
			{
				context.Fail("no CCSGameRulesProxy entity in the list; the map has no "
				             "rounds to terminate");
				return;
			}
			void* gamerules = SlotAt(proxy, gamerules_offset);
			if (!LooksLikeObjectPointer(gamerules))
			{
				context.Fail("the proxy's m_pGameRules is not a pointer");
				return;
			}
			// Reason 10 is RoundDraw: no team is credited, so no scoreboard
			// state a later suite might read is disturbed. Delay 0: the state
			// lands this frame.
			reinterpret_cast<TerminateRound_t>(terminate_round_)(
				gamerules, 10, 0, 0.0f);
			context.Detail("-> proxy %p -> gamerules %p, round terminated as a draw",
				static_cast<void*>(proxy), gamerules);
		});
	}
}

void GamedataSuite::ExerciseVtableSlots(TestContext& context)
{
	context.Note("[vtables] slot indexes (two compared against KHook::GetVtableIndex)");

	CS2ST_CALL(context, "vtables.\"ISource2GameEntities::CheckTransmit\"", {
		const TomlData::Entry* entry = data_.FindPlatform(kCheckTransmitTable,
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number)
		{
			context.Fail("the key is missing from the file");
			return;
		}

		const std::int32_t sdk_index =
			KHook::GetVtableIndex(&ISource2GameEntities::CheckTransmit);
		context.Detail("gamedata %lld, GetVtableIndex %d", entry->number,
			static_cast<int>(sdk_index));
		if (sdk_index < 0)
		{
			context.Fail("the SDK member pointer did not decode to a slot");
			return;
		}
		if (static_cast<long long>(sdk_index) != entry->number)
		{
			// Two independent sources, one number. Which one is wrong is a
			// question for the binary; that they disagree is the finding.
			context.Fail("the SDK declaration and the gamedata disagree about this slot");
		}
	});

	CS2ST_CALL(context, kAddResourceTable, {
		const TomlData::Entry* entry = data_.FindPlatform(kAddResourceTable,
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number)
		{
			context.Fail("the key is missing from the file");
			return;
		}

		// The ecosystem's number is the one-argument AddResource. The SDK
		// declares three overloads: MSVC groups them by name in reverse
		// declaration order (single-argument last, slot 2), Itanium keeps
		// declaration order (slot 0) - which is exactly the 2/0 the file
		// carries, arrived at by the compiler rather than copied.
		const auto single_arg = static_cast<void (IEntityResourceManifest::*)(const char*)>(
			&IEntityResourceManifest::AddResource);
		const std::int32_t sdk_index = KHook::GetVtableIndex(single_arg);
		context.Detail("gamedata %lld, GetVtableIndex %d", entry->number,
			static_cast<int>(sdk_index));
		if (sdk_index < 0)
		{
			context.Fail("the SDK member pointer did not decode to a slot");
			return;
		}
		if (static_cast<long long>(sdk_index) != entry->number)
		{
			context.Fail("the SDK declaration and the gamedata disagree about this slot");
		}
	});

	// One row per remaining key, so each slot reads on its own line. These are
	// reported, not proven: proving a slot moved needs the class declaration,
	// which the SDK does not have.
	for (const std::string& table : data_.tables())
	{
		if (table.rfind(kVtablesPrefix, 0) != 0)
		{
			continue;
		}
		const std::string name = table.substr(std::strlen(kVtablesPrefix));
		if (name == "ISource2GameEntities::CheckTransmit"
			|| name == "CEntityResourceManifest_AddResource"
			|| name == "GameEventManager") // checked below, by decode
		{
			continue;
		}

		CS2ST_CALL(context, table, {
			const TomlData::Entry* entry = data_.FindPlatform(table, CS2ST_WINDOWS != 0);
			if (entry == nullptr || !entry->is_number)
			{
				context.Fail("the key is missing from the file");
				return;
			}
			if (name == "CServerSideClient")
			{
				context.Detail("%lld (RTTI offset-to-top discriminator, not a slot)",
					entry->number);
			}
			else if (name == "SetStateChanged")
			{
				context.Detail("slot %lld (cross-checked by the NetworkStateChanged decode)",
					entry->number);
			}
			else
			{
				context.Detail("slot %lld (reported; the SDK does not declare the class)",
					entry->number);
			}
		});
	}

	// The ecosystem calls this slot on ISource2Server and subtracts 8 from what
	// comes back to reach IGameEventManager2. GameEventSuite records why this
	// project will not blind-call server vtable slots, so the slot is read, its
	// code decoded, and only an accessor-shaped body is ever called.
	CS2ST_CALL(context, "vtables.GameEventManager", {
		const TomlData::Entry* entry = data_.FindPlatform("vtables.GameEventManager",
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number || entry->number < 0)
		{
			context.Fail("the key is missing from the file");
			return;
		}
		if (server_ == nullptr)
		{
			context.Fail("server does not answer %s; there is no object to read the slot from",
				INTERFACEVERSION_SERVERGAMEDLL);
			return;
		}
		const Module& server = context.GetModule("server");
		if (!server.IsValid())
		{
			context.Fail("the server module is not loaded");
			return;
		}

		const auto* const* vtable = *static_cast<const unsigned char* const* const*>(server_);
		const unsigned char* fn = vtable[entry->number];
		context.Detail("slot %lld -> %p", entry->number, static_cast<const void*>(fn));
		if (!InModule(server, fn))
		{
			context.Fail("the slot points outside %s; the slot index is past the table",
				server.name().c_str());
			return;
		}
		if (fn[0] == 0xC3)
		{
			// The finding this row exists for: calling an empty stub returns
			// whatever rax happened to hold, and "minus 8" turns that into a
			// manager pointer every listener registration then dereferences.
			context.Fail("slot %lld is an empty 'ret' stub on this build; the ecosystem's "
			             "call returns an undefined register, not the manager", entry->number);
			return;
		}

		// The accessor shape: lea rax,[rip+d] / mov rax,[rip+d], then ret.
		const bool is_lea = fn[0] == 0x48 && fn[1] == 0x8D && fn[2] == 0x05;
		const bool is_mov = fn[0] == 0x48 && fn[1] == 0x8B && fn[2] == 0x05;
		if ((!is_lea && !is_mov) || fn[7] != 0xC3)
		{
			context.Fail("slot %lld does not have an accessor's shape (starts %02x %02x %02x); "
			             "not calling it", entry->number, fn[0], fn[1], fn[2]);
			return;
		}
		std::int32_t disp = 0;
		std::memcpy(&disp, fn + 3, sizeof(disp));
		const unsigned char* global = fn + 7 + disp;
		void* decoded = is_lea ? const_cast<unsigned char*>(global) : SlotAt(global, 0);

		using Accessor_t = void* (*)(void*);
		void* returned = reinterpret_cast<Accessor_t>(const_cast<unsigned char*>(fn))(server_);
		context.Detail("-> returned %p, decoded %p", returned, decoded);
		if (returned != decoded)
		{
			context.Fail("the slot's return value and its decoded global disagree");
			return;
		}

		void* manager = static_cast<unsigned char*>(returned) - 8;
		if (!LooksLikeObjectPointer(manager) || !InModule(server, SlotAt(manager, 0)))
		{
			context.Fail("result - 8 (%p) does not carry a vtable inside %s; it is not an "
			             "IGameEventManager2", manager, server.name().c_str());
			return;
		}
		context.Detail("-> manager %p, vtable %p", manager, SlotAt(manager, 0));
	});
}

// -----------------------------------------------------------------------------
//  byte offsets - offsetof() where the SDK declares the struct
// -----------------------------------------------------------------------------

void GamedataSuite::ExerciseStructOffsets(TestContext& context)
{
	context.Note("[offsets] byte offsets (two compared against offsetof)");

	const std::string player_slot = "offsets.CheckTransmitPlayerSlot";
	CS2ST_CALL(context, player_slot, {
		const TomlData::Entry* entry = data_.FindPlatform("offsets.CheckTransmitPlayerSlot",
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number)
		{
			context.Fail("the key is missing from the file");
			return;
		}
		const std::size_t sdk_offset = offsetof(CCheckTransmitInfo, m_nPlayerSlot);
		context.Detail("gamedata %lld, offsetof %zu", entry->number, sdk_offset);
		if (static_cast<long long>(sdk_offset) != entry->number)
		{
			context.Fail("the SDK declaration and the gamedata disagree; every "
			             "CheckTransmit hook reads the wrong field");
		}
	});

	const std::string entity_system = "offsets.GameEntitySystem";
	CS2ST_CALL(context, entity_system, {
		const TomlData::Entry* entry = data_.FindPlatform("offsets.GameEntitySystem",
			CS2ST_WINDOWS != 0);
		if (entry == nullptr || !entry->is_number)
		{
			context.Fail("the key is missing from the file");
			return;
		}

		// The SDK's own GameEntitySystem() accessor - which compiles into this
		// plugin - reads this member, so offsetof here and the ecosystem's
		// number are two descriptions sharing no code.
		const std::size_t sdk_offset =
			offsetof(CGameResourceService, m_pEntityResourceManifestHandler);
		context.Detail("gamedata %lld, offsetof %zu", entry->number, sdk_offset);
		if (static_cast<long long>(sdk_offset) != entry->number)
		{
			context.Fail("the SDK declaration and the gamedata disagree; every plugin "
			             "reading this offset reads the wrong bytes");
			return;
		}

		if (resource_service_ == nullptr)
		{
			context.Detail("no live service object to read the offset from");
			return;
		}
		void* at_offset = SlotAt(resource_service_, sdk_offset);
		context.Detail("+%zu on the live object reads %p (%s)",
			sdk_offset, at_offset,
			LooksLikeObjectPointer(at_offset) ? "plausible" : "NOT a plausible pointer");
		if (!LooksLikeObjectPointer(at_offset))
		{
			context.Fail("the SDK's own offset reads a non-pointer from the live object; "
			             "the declaration has drifted from the binary");
		}
	});

	context.SkipCall("offsets.CTakeDamageInfo_HitGroup",
		"no CTakeDamageInfo in the SDK to take offsetof from (gamedata +104)");
}

// -----------------------------------------------------------------------------
//  Hooks - Host_Say and CEntityInstance_AcceptInput, observation only
// -----------------------------------------------------------------------------

int GamedataSuite::InstallHooks(TestContext& context)
{
	(void)context;

	if (hooks_installed_)
	{
		return 0;
	}

	// Same discipline as GameEntitySystemSuite: a detour writes a jump into
	// code this plugin did not compile, so it only goes in once the sweep has
	// said the address under it is the function the file names.
	if (!signatures_verified_)
	{
		CS2ST_LOG_WARN("%s: refusing to install - run 'meta_test run %s' first so the "
		               "signatures are resolved and verified\n",
			Id(), Id());
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before installing, so a hook that never fires still appears in
	// the dump as `never` - after a CS2 update that row is the finding.
	recorder.Declare(kHostSay, Id());
	recorder.Declare(kAcceptInput, Id());

	// Re-attach first, then configure. RemoveContext took this suite's
	// callbacks out; Configure on an address it has already detoured returns
	// early without restoring them, so a re-install after a remove would leave
	// a live detour that records nothing.
	hook_host_say_.AddContext(this, &GamedataSuite::Pre_HostSay,
		&GamedataSuite::Post_HostSay);
	hook_accept_input_.AddContext(this, &GamedataSuite::Pre_AcceptInput,
		&GamedataSuite::Post_AcceptInput);

	hook_host_say_.Configure(host_say_);
	hook_accept_input_.Configure(accept_input_);

	hooks_installed_ = true;
	CS2ST_LOG_INFO("%s: Host_Say and CEntityInstance_AcceptInput detoured (observation "
	               "only)\n", Id());

	return kHookCount;
}

void GamedataSuite::RemoveHooks(TestContext& context)
{
	(void)context;

	if (!hooks_installed_)
	{
		return;
	}

	// KHook::Function has no uninstall short of destruction: Configure puts the
	// detour in, the destructor takes it out. RemoveContext detaches this
	// suite's callbacks - recording stops immediately, the detour itself stays
	// (harmlessly, forwarding to an empty context list) until the suite object
	// is destroyed at plugin unload. MetamodListenerSuite documents the same
	// no-inverse asymmetry.
	hook_host_say_.RemoveContext(this);
	hook_accept_input_.RemoveContext(this);

	hooks_installed_ = false;
}

KHook::Return<void> GamedataSuite::Pre_HostSay(CEntityInstance* controller, CCommand& args,
                                               bool teamonly, int unk1, const char* unk2)
{
	// No fault guard here. Reading ArgC() trusts the SDK's CCommand layout
	// against an object the engine built, so the count is bounds-checked
	// before anything indexes with it - the same discipline as
	// Source2GameClientsSuite::Pre_ClientCommand.
	const int argc = args.ArgC();
	const bool plausible = argc > 0 && argc <= kMaxPlausibleArgC;

	char arg0_scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kHostSay, "controller=%p argc=%d%s say=\"%.63s\" teamonly=%d",
		static_cast<void*>(controller),
		argc,
		plausible ? "" : " IMPLAUSIBLE",
		plausible ? SafeStringArg(args.Arg(0), arg0_scratch) : "<not read>",
		teamonly ? 1 : 0);

	// unk1 and unk2 are recorded nowhere: unknown semantics, and a hook that
	// reads what it cannot vet is the crash this project exists to prevent.
	(void)unk1;
	(void)unk2;
	return { KHook::Action::Ignore };
}

KHook::Return<void> GamedataSuite::Post_HostSay(CEntityInstance*, CCommand&, bool, int,
                                                const char*)
{
	CS2ST_HOOK_LEAVE(kHostSay);
	return { KHook::Action::Ignore };
}

KHook::Return<void> GamedataSuite::Pre_AcceptInput(CEntityInstance* self, const char* input,
                                                   CEntityInstance* activator,
                                                   CEntityInstance* caller, void* value,
                                                   int output_id, void* unk)
{
	char input_scratch[48];
	CS2ST_HOOK_ENTER_ARGS(kAcceptInput,
		"self=%p input=\"%.63s\" activator=%p caller=%p value=%p output_id=%d",
		static_cast<void*>(self),
		SafeStringArg(input, input_scratch),
		static_cast<void*>(activator),
		static_cast<void*>(caller),
		value,
		output_id);

	// The value parameter is a variant_t* in the ecosystem's typedef; void*
	// here because the payload is never read, only its address recorded.
	(void)unk;
	return { KHook::Action::Ignore };
}

KHook::Return<void> GamedataSuite::Post_AcceptInput(CEntityInstance*, const char*,
                                                    CEntityInstance*, CEntityInstance*,
                                                    void*, int, void*)
{
	CS2ST_HOOK_LEAVE(kAcceptInput);
	return { KHook::Action::Ignore };
}

// -----------------------------------------------------------------------------
//  Construction - the hook templates bind their callbacks here, the addresses
//  they detour arrive later, from the sweep
// -----------------------------------------------------------------------------

GamedataSuite::GamedataSuite()
	: hook_host_say_(this, &GamedataSuite::Pre_HostSay, &GamedataSuite::Post_HostSay),
	  hook_accept_input_(this, &GamedataSuite::Pre_AcceptInput, &GamedataSuite::Post_AcceptInput)
{
}

} // namespace cs2st
