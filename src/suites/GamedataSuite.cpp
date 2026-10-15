#include "suites/GamedataSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <engine2/igameresourceservice.h>
#include <engine2/isource2server.h>
#include <entity2/entityinstance.h>
#include <entity2/entitysystem.h>
#include <interfaces/interfaces.h>
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
//  vtable slots - the two an SDK declaration can compute, then the rest
// -----------------------------------------------------------------------------

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
