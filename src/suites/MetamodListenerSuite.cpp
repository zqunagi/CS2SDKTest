#include "suites/MetamodListenerSuite.h"

#include <cstddef>
#include <cstring>

#include <engine2/isource2serverconfig.h>
#include <engine2/ivengineserver2.h>
#include <tier0/convar.h>

#include "hook/HookArg.h"
#include "hook/HookRecorder.h"
#include "version_gen.h"

// g_SMAPI / g_PLAPI / g_PLID / KHook::__exported__khook live in Plugin.cpp,
// where PLUGIN_EXPOSE defines them; this is the declaration side. Unlike the
// other suites nothing here needs KHook's machinery - Metamod calls a
// registered listener directly - but the same globals are how AddListener
// finds our plugin record.
PLUGIN_GLOBALVARS();

namespace cs2st
{
namespace
{

// Hook ids. Used as both the log label and the recorder key, so they are
// written once here rather than repeated at each call site.
constexpr const char* kOnLevelInit          = "IMetamodListener::OnLevelInit";
constexpr const char* kOnLevelShutdown      = "IMetamodListener::OnLevelShutdown";
constexpr const char* kOnPluginLoad         = "IMetamodListener::OnPluginLoad";
constexpr const char* kOnPluginUnload       = "IMetamodListener::OnPluginUnload";
constexpr const char* kOnPluginPause        = "IMetamodListener::OnPluginPause";
constexpr const char* kOnPluginUnpause      = "IMetamodListener::OnPluginUnpause";
constexpr const char* kOnEngineQuery        = "IMetamodListener::OnEngineQuery";
constexpr const char* kOnPhysicsQuery       = "IMetamodListener::OnPhysicsQuery";
constexpr const char* kOnFileSystemQuery    = "IMetamodListener::OnFileSystemQuery";
constexpr const char* kOnGameDLLQuery       = "IMetamodListener::OnGameDLLQuery";
constexpr const char* kOnMetamodQuery       = "IMetamodListener::OnMetamodQuery";
constexpr const char* kOnVSPListening       = "IMetamodListener::OnVSPListening";
constexpr const char* kOnUnlinkConCommandBase = "IMetamodListener::OnUnlinkConCommandBase";
constexpr const char* kOnUnlinkConCommand   = "IMetamodListener::OnUnlinkConCommand";
constexpr const char* kOnUnlinkConVar       = "IMetamodListener::OnUnlinkConVar";

/** Number of ids declared above; InstallHooks reports it. */
constexpr int kListenerCallbackCount = 15;

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool MetamodListenerSuite::Acquire(TestContext& context)
{
	// Unlike every other suite there is no CreateInterface to call: Metamod
	// handed us its API in Load() and there is no second way to ask for it. If
	// this is null the plugin could not have got this far, so the check is
	// really about saying so in the log rather than about recovery.
	smm_ = g_SMAPI;

	if (smm_ == nullptr)
	{
		context.UnavailableCall("ISmmAPI", "g_SMAPI is null; PLUGIN_SAVEVARS never ran");
		return false;
	}
	if (g_PLAPI == nullptr)
	{
		context.UnavailableCall("ISmmAPI",
			"g_PLAPI is null; AddListener needs it to find our plugin record");
		return false;
	}

	context.Note("ISmmAPI at %p, plugin id %d, KHook detour interface at %p",
		static_cast<void*>(smm_), static_cast<int>(g_PLID),
		static_cast<void*>(KHook::__exported__khook));
	return true;
}

void MetamodListenerSuite::ExerciseCalls(TestContext& context)
{
	ExerciseVersions(context);
	ExerciseFactories(context);
	ExerciseMetaFactory(context);
	ExerciseFormatting(context);
	ExerciseUserMessages(context);
	NoteSideEffectingCalls(context);
	NoteListenerExpectations(context);
}

// -----------------------------------------------------------------------------
//  Versions - the numbers every other assumption rests on
// -----------------------------------------------------------------------------

void MetamodListenerSuite::ExerciseVersions(TestContext& context)
{
	context.Note("[ISmmAPI] versions - these decide whether this binary is even "
	             "allowed to run");

	CS2ST_CALL(context, "ISmmAPI::GetApiVersions", {
		// Not named major/minor: glibc's <sys/sysmacros.h> defines both as
		// function-like macros, and it gets pulled in behind <sys/types.h>.
		int api_major = -1;
		int api_minor = -1;
		int plvers = -1;
		int plmin = -1;
		smm_->GetApiVersions(api_major, api_minor, plvers, plmin);
		context.Detail("-> api %d.%d, plugin api %d (min %d); built against %d",
			api_major, api_minor, plvers, plmin, PLUGIN_MM_PLAPI_VERSION);

		// The interesting comparison is not "did it return numbers" but "is the
		// version this binary was compiled against still inside the window the
		// running Metamod accepts". Outside it, the loader would have refused
		// us - so if we are here and this fails, the accept window and the
		// loader's check have drifted apart.
		if (plmin > PLUGIN_MM_PLAPI_VERSION || plvers < PLUGIN_MM_PLAPI_VERSION)
		{
			context.Fail("built against PLAPI %d, which is outside [%d, %d]",
				PLUGIN_MM_PLAPI_VERSION, plmin, plvers);
		}
	});

	// GetShVersions is gone with SourceHook itself (PLAPI 18). Its replacement
	// is the detour interface below - one pointer whose non-nullness is now the
	// "hook layer is alive" fact the old version pair used to carry.
	CS2ST_CALL(context, "ISmmAPI::GetDetourInterface", {
		void* detour = smm_->GetDetourInterface(g_PLID);
		context.Detail("-> %p; the KHook global this plugin hooks through is %p",
			detour, static_cast<void*>(KHook::__exported__khook));

		// PLUGIN_SAVEVARS fetched this exact pointer at load time. Null means
		// every KHook::Virtual in this plugin is inert - the same failure the
		// old GetShVersions row was there to catch, one rename later.
		if (detour == nullptr)
		{
			context.Fail("the detour interface is what hooks resolve through; "
			             "null means no hook in this plugin can attach");
		}
		else if (detour != static_cast<void*>(KHook::__exported__khook))
		{
			context.Fail("differs from the KHook global this plugin hooks through");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::GetSourceEngineBuild", {
		const int build = smm_->GetSourceEngineBuild();
		context.Detail("-> %d (SOURCE_ENGINE_CS2 = %d)", build, SOURCE_ENGINE_CS2);
		if (build != SOURCE_ENGINE_CS2)
		{
			// One correct answer: the Source 2 provider returns this constant
			// unconditionally when compiled for CS2. Anything else means the
			// Metamod build under us is not the one this plugin assumes.
			context.Fail("expected SOURCE_ENGINE_CS2 (%d)", SOURCE_ENGINE_CS2);
		}
	});

	CS2ST_CALL(context, "ISmmAPI::GetGameDLLVersion", {
		context.Detail("-> %d", smm_->GetGameDLLVersion());
	});

	CS2ST_CALL(context, "ISmmAPI::GetBaseDir", {
		const char* dir = smm_->GetBaseDir();
		char scratch[64];
		context.Detail("-> \"%.255s\"", SafeStringArg(dir, scratch));
		if (dir == nullptr || dir[0] == '\0')
		{
			context.Fail("the mod path is how Metamod found us; it cannot be empty");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::GetCGlobals", {
		CGlobalVars* globals = smm_->GetCGlobals();
		context.Detail("-> %p%s", static_cast<void*>(globals),
			globals == nullptr ? " (not set yet)" : "");
		// Deliberately not a failure: Metamod stores whatever the bridge passed
		// at DLLInit, and the layout of what it points at is Source2EngineSuite's
		// job. All that is being recorded here is whether Metamod has one.
	});

	CS2ST_CALL(context, "ISmmAPI::GetVSPVersion", {
		context.Detail("-> %d (VSP not requested by this plugin)",
			smm_->GetVSPVersion());
	});

	CS2ST_CALL(context, "ISmmAPI::GetVSPInfo", {
		int version = -1;
		IServerPluginCallbacks* vsp = smm_->GetVSPInfo(&version);
		context.Detail("-> %p, version %d", static_cast<void*>(vsp), version);
		// Null is the expected answer: the VSP bridge only comes up if some
		// plugin called EnableVSPListener, and this one never does.
	});
}

// -----------------------------------------------------------------------------
//  Factories
//
//  Metamod hands out two flavours of each factory: the engine's real one, and
//  a wrapper that offers the query to every listener first. The difference
//  between them is exactly the mechanism behind OnEngineQuery and friends, so
//  comparing the two pointers is a direct check that the mechanism is still
//  wired up - before a single callback has had a chance to fire.
// -----------------------------------------------------------------------------

void MetamodListenerSuite::ExerciseFactories(TestContext& context)
{
	context.Note("[ISmmAPI] factories - syn=true is the listener-aware wrapper");

	CreateInterfaceFn engine_raw = nullptr;
	CreateInterfaceFn physics_raw = nullptr;
	CreateInterfaceFn filesystem_raw = nullptr;

	CS2ST_CALL(context, "ISmmAPI::GetEngineFactory", {
		CreateInterfaceFn wrapped = smm_->GetEngineFactory(true);
		engine_raw = smm_->GetEngineFactory(false);
		context.Detail("-> wrapper %p, real %p",
			reinterpret_cast<void*>(wrapped), reinterpret_cast<void*>(engine_raw));
		if (wrapped == nullptr || engine_raw == nullptr)
		{
			context.Fail("the game DLL could not have started without these");
		}
		else if (wrapped == engine_raw)
		{
			context.Fail("wrapper equals the real factory; OnEngineQuery can never fire");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::GetPhysicsFactory", {
		CreateInterfaceFn wrapped = smm_->GetPhysicsFactory(true);
		physics_raw = smm_->GetPhysicsFactory(false);
		context.Detail("-> wrapper %p, real %p",
			reinterpret_cast<void*>(wrapped), reinterpret_cast<void*>(physics_raw));
	});

	CS2ST_CALL(context, "ISmmAPI::GetFileSystemFactory", {
		CreateInterfaceFn wrapped = smm_->GetFileSystemFactory(true);
		filesystem_raw = smm_->GetFileSystemFactory(false);
		context.Detail("-> wrapper %p, real %p",
			reinterpret_cast<void*>(wrapped), reinterpret_cast<void*>(filesystem_raw));
	});

	CS2ST_CALL(context, "ISmmAPI::GetServerFactory", {
		CreateInterfaceFn wrapped = smm_->GetServerFactory(true);
		CreateInterfaceFn real = smm_->GetServerFactory(false);
		context.Detail("-> wrapper %p, real %p",
			reinterpret_cast<void*>(wrapped), reinterpret_cast<void*>(real));
		if (real == nullptr)
		{
			context.Fail("Metamod resolved the game DLL through this; it cannot be null");
		}
	});

	// On Source 2 the loader has only one factory to work with: it intercepts
	// ISource2ServerConfig::Connect and copies that single pointer into all
	// three bridge slots. So all three *raw* factories being the same object is
	// correct here, and three distinct ones would mean the bridge changed shape.
	CS2ST_CALL(context, "ISmmAPI (engine/physics/filesystem raw factories agree)", {
		const bool have_all = engine_raw != nullptr && physics_raw != nullptr
			&& filesystem_raw != nullptr;
		const bool same = (engine_raw == physics_raw) && (engine_raw == filesystem_raw);
		context.Detail("engine=%p physics=%p filesystem=%p -> %s",
			reinterpret_cast<void*>(engine_raw),
			reinterpret_cast<void*>(physics_raw),
			reinterpret_cast<void*>(filesystem_raw),
			!have_all ? "incomplete, see the three calls above"
			          : (same ? "identical, as Source 2 expects" : "different"));

		// Guarded on have_all so a fault in one of the three getters above
		// reports itself once, on its own line, instead of a second time here
		// as a bogus mismatch.
		if (have_all && !same)
		{
			context.Fail("the Source 2 bridge fills all three from one Connect() argument");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::VInterfaceMatch", {
		// min = -1 means "try the string exactly as given first", so a name we
		// know exists costs one factory call rather than a sweep up to 999.
		void* found = smm_->VInterfaceMatch(smm_->GetEngineFactory(false),
			INTERFACEVERSION_VENGINESERVER, -1);
		context.Detail("-> %p for '%s'", found, INTERFACEVERSION_VENGINESERVER);
		if (found == nullptr)
		{
			context.Fail("the engine answers this version string directly");
		}
	});

	context.SkipCall("ISmmAPI::InterfaceSearch",
		"sweeps a factory with up to 999 speculative version strings; "
		"VInterfaceMatch above covers the same code path with one query");
}

// -----------------------------------------------------------------------------
//  MetaFactory - Metamod handing out its own internals
// -----------------------------------------------------------------------------

void MetamodListenerSuite::ExerciseMetaFactory(TestContext& context)
{
	context.Note("[ISmmAPI] MetaFactory");

	CS2ST_CALL(context, "ISmmAPI::MetaFactory(ISourceHook)", {
		int ret = -1;
		PluginId id = 0;
		void* sh = smm_->MetaFactory(MMIFACE_SOURCEHOOK, &ret, &id);

		// PLAPI 18 removed the built-in branch that answered this name: khook
		// replaced SourceHook, and the pointer now travels through
		// GetDetourInterface instead. A name-only lookup therefore falls
		// through to the listener walk and answers nothing - which is the new
		// correct behavior, not a broken factory. The row stays because it is
		// how an update that changes this again gets noticed.
		context.Detail("-> %p (ret %d); the hook layer now travels through "
		               "GetDetourInterface (%p)",
			sh, ret,
			static_cast<void*>(KHook::__exported__khook));

		if (sh != nullptr)
		{
			context.Suspect("some listener claimed ISourceHook after Metamod "
			                "stopped providing it; worth knowing who");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::MetaFactory(IPluginManager)", {
		int ret = -1;
		void* mgr = smm_->MetaFactory(MMIFACE_PLMANAGER, &ret, nullptr);
		context.Detail("-> %p (ret %d)", mgr, ret);
		if (mgr == nullptr)
		{
			context.Fail("the plugin manager is a Metamod built-in");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::MetaFactory(unknown)", {
		// The negative case matters as much as the positive: a factory that
		// answers a name nobody implements is answering with something wrong.
		// This also walks every registered listener's OnMetamodQuery, which is
		// why the name is deliberately absurd - a real one might be claimed.
		int ret = META_IFACE_OK;
		void* nothing = smm_->MetaFactory("ICs2SdkTestNoSuchInterface999", &ret, nullptr);
		context.Detail("-> %p (ret %d)", nothing, ret);
		if (nothing != nullptr)
		{
			context.Fail("some listener claimed an interface that does not exist");
		}
		else if (ret != META_IFACE_FAILED)
		{
			context.Fail("expected META_IFACE_FAILED (%d)", static_cast<int>(META_IFACE_FAILED));
		}
	});
}

// -----------------------------------------------------------------------------
//  String helpers
//
//  Pure functions over buffers we own, so they are the cheapest calls here and
//  the only ones with a fully predictable answer.
// -----------------------------------------------------------------------------

void MetamodListenerSuite::ExerciseFormatting(TestContext& context)
{
	context.Note("[ISmmAPI] string helpers");

	CS2ST_CALL(context, "ISmmAPI::FormatIface", {
		// Documented behaviour: bump the trailing 3-digit version in place.
		char buffer[64] = "Source2EngineToServer001";
		const int version = smm_->FormatIface(buffer, sizeof(buffer));
		context.Detail("-> %d, buffer now \"%.63s\"", version, buffer);
		if (version != 2 || std::strcmp(buffer, "Source2EngineToServer002") != 0)
		{
			context.Fail("expected 2 and \"Source2EngineToServer002\"");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::Format", {
		char buffer[64] = {};
		const std::size_t written = smm_->Format(buffer, sizeof(buffer), "%s-%d", "cs2", 2);
		context.Detail("-> %zu, \"%.63s\"", written, buffer);
		if (written != 5 || std::strcmp(buffer, "cs2-2") != 0)
		{
			context.Fail("expected 5 and \"cs2-2\"");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::PathFormat", {
		// Separators come out normalised for the platform, so the check is on
		// length rather than on an exact string.
		char buffer[128] = {};
		const std::size_t written = smm_->PathFormat(buffer, sizeof(buffer),
			"%s/addons/metamod", smm_->GetBaseDir());
		context.Detail("-> %zu, \"%.127s\"", written, buffer);
		if (written == 0 || written >= sizeof(buffer))
		{
			context.Fail("the path was truncated or empty");
		}
	});
}

// -----------------------------------------------------------------------------
//  User messages
//
//  Stubbed on Source 2: the base provider returns -1/null and the Source 2
//  provider does not override it. Recording that is the point - the day these
//  start answering, Metamod grew a user-message table and plugins can use it.
// -----------------------------------------------------------------------------

void MetamodListenerSuite::ExerciseUserMessages(TestContext& context)
{
	context.Note("[ISmmAPI] user messages - expected to be stubbed on Source 2");

	CS2ST_CALL(context, "ISmmAPI::GetUserMessageCount", {
		const int count = smm_->GetUserMessageCount();
		context.Detail("-> %d%s", count, count == -1 ? " (stub, as expected)" : "");
		if (count != -1)
		{
			context.Suspect("no longer the -1 stub; Metamod may have gained a "
			                "Source 2 user-message table");
		}
	});

	CS2ST_CALL(context, "ISmmAPI::FindUserMessage", {
		int size = -1;
		const int index = smm_->FindUserMessage("TextMsg", &size);
		context.Detail("-> index %d, size %d", index, size);
	});

	CS2ST_CALL(context, "ISmmAPI::GetUserMessage", {
		int size = -1;
		const char* name = smm_->GetUserMessage(0, &size);
		char scratch[64];
		context.Detail("-> \"%.63s\", size %d", SafeStringArg(name, scratch), size);
	});
}

// -----------------------------------------------------------------------------
//  Everything deliberately not called
// -----------------------------------------------------------------------------

void MetamodListenerSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.Note("[ISmmAPI] not called - side effects");

	context.SkipCall("ISmmAPI::AddListener",
		"installed by 'cst hooks install metamod', not by a call sweep");
	context.SkipCall("ISmmAPI::EnableVSPListener",
		"would bring up the VSP bridge for every plugin on the server");
	context.SkipCall("ISmmAPI::RegisterConCommandBase", "would register a command");
	context.SkipCall("ISmmAPI::UnregisterConCommandBase", "would remove a live command");
	context.SkipCall("ISmmAPI::RegisterConCommand", "would register a command");
	context.SkipCall("ISmmAPI::RegisterConVar", "would register a cvar");
	context.SkipCall("ISmmAPI::UnregisterConCommand",
		"would remove a live command; exercised for real in Plugin::Unload");
	context.SkipCall("ISmmAPI::UnregisterConVar", "would remove a live cvar");
	context.SkipCall("ISmmAPI::LogMsg", "would write to Metamod's log");
	context.SkipCall("ISmmAPI::ConPrint", "console noise, no information gained");
	context.SkipCall("ISmmAPI::ConPrintf", "console noise, no information gained");
	context.SkipCall("ISmmAPI::ClientConPrintf", "would print into a player's console");
	context.SkipCall("ISmmAPI::FormatArgs",
		"the va_list form of Format, which is covered above");
}

// -----------------------------------------------------------------------------
//  What the listener can and cannot be expected to report
//
//  Written into the log as notes rather than left in this file's comments: the
//  person reading a dump six months from now has the log, not the source, and
//  "never" needs an interpretation next to it.
// -----------------------------------------------------------------------------

void MetamodListenerSuite::NoteListenerExpectations(TestContext& context)
{
	context.Note("[IMetamodListener] what to expect in a hook dump");
	context.Note("  OnLevelInit/OnLevelShutdown  once per map; Metamod synthesises");
	context.Note("    them from ILoopMode::LoopInit/LoopShutdown, so 'never' after a");
	context.Note("    map change means that hook inside Metamod stopped working");
	context.Note("  OnLevelInit's mapEntities is always \"\" on Source 2 - the other");
	context.Note("    arguments come out of the loop-mode KeyValues, not the map lump");
	context.Note("  OnPlugin*  only for *other* plugins; Metamod skips the plugin the");
	context.Note("    event is about, so unloading this one will not show up here");
	context.Note("  On*Query  fires whenever anyone resolves an interface through");
	context.Note("    Metamod's wrapped factories, mostly while plugins load");
	context.Note("  OnPhysicsQuery  Source 2 has no separate physics factory, so this");
	context.Note("    only fires if someone asks the shared factory through that path");
	context.Note("  OnVSPListening  never, unless some plugin calls EnableVSPListener");
	context.Note("  OnUnlinkConCommandBase  never at PLAPI >= 17: Metamod picks the");
	context.Note("    Base form only for plugins declaring an older API version. If it");
	context.Note("    ever fires, our GetApiVersion() is being read as < 17");
	context.Note("  OnUnlinkConCommand/ConVar  when another plugin unloads and its");
	context.Note("    commands are taken back out");
}

// -----------------------------------------------------------------------------
//  Install / remove
// -----------------------------------------------------------------------------

int MetamodListenerSuite::InstallHooks(TestContext& context)
{
	if (smm_ == nullptr || g_PLAPI == nullptr)
	{
		return 0;
	}

	HookRecorder& recorder = HookRecorder::Get();

	// Declared before registering so a callback that never fires still shows up
	// in the dump as "never". For this suite that matters more than most: over
	// half of these are expected to be silent on a dedicated server, and the
	// value is in seeing which ones.
	recorder.Declare(kOnLevelInit, Id());
	recorder.Declare(kOnLevelShutdown, Id());
	recorder.Declare(kOnPluginLoad, Id());
	recorder.Declare(kOnPluginUnload, Id());
	recorder.Declare(kOnPluginPause, Id());
	recorder.Declare(kOnPluginUnpause, Id());
	recorder.Declare(kOnEngineQuery, Id());
	recorder.Declare(kOnPhysicsQuery, Id());
	recorder.Declare(kOnFileSystemQuery, Id());
	recorder.Declare(kOnGameDLLQuery, Id());
	recorder.Declare(kOnMetamodQuery, Id());
	recorder.Declare(kOnVSPListening, Id());
	recorder.Declare(kOnUnlinkConCommandBase, Id());
	recorder.Declare(kOnUnlinkConCommand, Id());
	recorder.Declare(kOnUnlinkConVar, Id());

	if (!listener_registered_)
	{
		// One-way door. ISmmAPI has no RemoveListener - Metamod drops a
		// plugin's listeners only when the plugin unloads - so this happens at
		// most once per load, and `recording_` is what install/remove actually
		// move afterwards.
		smm_->AddListener(g_PLAPI, this);
		listener_registered_ = true;
	}
	else
	{
		context.Note("already registered with Metamod (AddListener cannot be "
		             "undone); re-enabling recording instead");
	}

	recording_ = true;
	return kListenerCallbackCount;
}

void MetamodListenerSuite::RemoveHooks(TestContext& context)
{
	if (!recording_)
	{
		return;
	}

	recording_ = false;

	// Worth saying out loud, because it is not what "hooks remove" does for any
	// other suite: the callbacks keep arriving, they just stop being counted.
	// The listener itself only goes away when Metamod unloads this plugin,
	// which is also what keeps `this` valid for as long as it is registered.
	context.Note("recording stopped; the listener stays registered until unload "
	             "(ISmmAPI has no RemoveListener)");
}

// -----------------------------------------------------------------------------
//  Callbacks
//
//  Unlike a SourceHook pre/post pair, we are the callee here, so entering and
//  returning are both inside this function. Both are still recorded: it keeps
//  the dump's balance column meaningful across suites, and an enter with no
//  leave still names the handler the process died inside.
// -----------------------------------------------------------------------------

void MetamodListenerSuite::OnLevelInit(char const* map_name,
                                       char const* map_entities,
                                       char const* old_level,
                                       char const* landmark_name,
                                       bool load_game,
                                       bool background)
{
	if (!recording_)
	{
		return;
	}

	// Four const char* in a row is the widest string surface any callback in
	// this plugin has, and it is the one most likely to be handed something
	// that is not a string: Metamod fills these from loop-mode KeyValues, so a
	// renamed key yields whatever GetString's default is rather than a fault.
	char map_scratch[48];
	char entities_scratch[48];
	char old_scratch[48];
	char landmark_scratch[48];

	// Vetted first, then inspected. Reading entities[0] straight off the
	// argument would be the one dereference in this file that happens before
	// the address has been looked at.
	const char* entities = SafeStringArg(map_entities, entities_scratch);
	const char* entities_note = (entities[0] == '\0')
		? "\"\" (empty, as Source 2 always gives)"
		: entities;

	CS2ST_HOOK_ENTER_ARGS(kOnLevelInit,
		"map=\"%.63s\" entities=%.63s old=\"%.63s\" landmark=\"%.63s\" load=%d bg=%d",
		SafeStringArg(map_name, map_scratch),
		entities_note,
		SafeStringArg(old_level, old_scratch),
		SafeStringArg(landmark_name, landmark_scratch),
		load_game ? 1 : 0, background ? 1 : 0);
	CS2ST_HOOK_LEAVE(kOnLevelInit);
}

void MetamodListenerSuite::OnLevelShutdown()
{
	if (!recording_)
	{
		return;
	}
	CS2ST_HOOK_ENTER(kOnLevelShutdown);
	CS2ST_HOOK_LEAVE(kOnLevelShutdown);
}

void MetamodListenerSuite::RecordPluginEvent(const char* hook_id, PluginId id)
{
	if (!recording_)
	{
		return;
	}
	CS2ST_HOOK_ENTER_ARGS(hook_id, "id=%d", static_cast<int>(id));
	CS2ST_HOOK_LEAVE(hook_id);
}

void MetamodListenerSuite::OnPluginLoad(PluginId id)
{
	RecordPluginEvent(kOnPluginLoad, id);
}

void MetamodListenerSuite::OnPluginUnload(PluginId id)
{
	RecordPluginEvent(kOnPluginUnload, id);
}

void MetamodListenerSuite::OnPluginPause(PluginId id)
{
	RecordPluginEvent(kOnPluginPause, id);
}

void MetamodListenerSuite::OnPluginUnpause(PluginId id)
{
	RecordPluginEvent(kOnPluginUnpause, id);
}

void* MetamodListenerSuite::DeclineQuery(const char* hook_id, const char* iface, int* ret)
{
	// Answer exactly as the IMetamodListener default does. Returning anything
	// non-null here would short-circuit the factory search for every other
	// caller on the server - the difference between watching and interfering.
	if (ret != nullptr)
	{
		*ret = META_IFACE_FAILED;
	}

	if (recording_)
	{
		char scratch[48];
		CS2ST_HOOK_ENTER_ARGS(hook_id, "iface=\"%.63s\"", SafeStringArg(iface, scratch));
		CS2ST_HOOK_LEAVE(hook_id);
	}

	return nullptr;
}

void* MetamodListenerSuite::OnEngineQuery(const char* iface, int* ret)
{
	return DeclineQuery(kOnEngineQuery, iface, ret);
}

void* MetamodListenerSuite::OnPhysicsQuery(const char* iface, int* ret)
{
	return DeclineQuery(kOnPhysicsQuery, iface, ret);
}

void* MetamodListenerSuite::OnFileSystemQuery(const char* iface, int* ret)
{
	return DeclineQuery(kOnFileSystemQuery, iface, ret);
}

void* MetamodListenerSuite::OnGameDLLQuery(const char* iface, int* ret)
{
	return DeclineQuery(kOnGameDLLQuery, iface, ret);
}

void* MetamodListenerSuite::OnMetamodQuery(const char* iface, int* ret)
{
	return DeclineQuery(kOnMetamodQuery, iface, ret);
}

void MetamodListenerSuite::OnVSPListening(IServerPluginCallbacks* iface)
{
	if (!recording_)
	{
		return;
	}
	CS2ST_HOOK_ENTER_ARGS(kOnVSPListening, "iface=%p", static_cast<void*>(iface));
	CS2ST_HOOK_LEAVE(kOnVSPListening);
}

void MetamodListenerSuite::OnUnlinkConCommandBase(PluginId id, ConCommandBase* command)
{
	if (!recording_)
	{
		return;
	}

	// ConCommandBase is only forward-declared on Source 2, so there is nothing
	// to read here even if this did fire - and at PLAPI >= 17 it should not.
	// Recording the bare fact is the whole point: this row firing means Metamod
	// took us for an older plugin.
	CS2ST_HOOK_ENTER_ARGS(kOnUnlinkConCommandBase, "id=%d cmd=%p (deprecated form!)",
		static_cast<int>(id), static_cast<void*>(command));
	CS2ST_HOOK_LEAVE(kOnUnlinkConCommandBase);
}

void MetamodListenerSuite::OnUnlinkConCommand(PluginId id, ProviderConCommand* command)
{
	if (!recording_)
	{
		return;
	}

	// Metamod calls this *before* the command is actually unregistered, so the
	// ref is still resolvable. Still guarded twice over: an invalid ref, and a
	// lookup that comes back null, both mean "print the handle only".
	const char* name = "<unresolved>";
	char scratch[48];
	if (command != nullptr && command->IsValidRef())
	{
		const ConCommandData* data = command->GetRawData();
		if (data != nullptr)
		{
			name = SafeStringArg(data->GetName(), scratch);
		}
	}

	CS2ST_HOOK_ENTER_ARGS(kOnUnlinkConCommand, "id=%d access=%u \"%.63s\"",
		static_cast<int>(id),
		command != nullptr ? static_cast<unsigned>(command->GetAccessIndex()) : 0u,
		name);
	CS2ST_HOOK_LEAVE(kOnUnlinkConCommand);
}

void MetamodListenerSuite::OnUnlinkConVar(PluginId id, ProviderConVar* var)
{
	if (!recording_)
	{
		return;
	}

	// No name here. ConVarRefAbstract::GetName() dereferences a cached
	// ConVarData pointer with no null check of its own, and there is no public
	// way to ask whether that pointer is set - so the handle is all this reads.
	CS2ST_HOOK_ENTER_ARGS(kOnUnlinkConVar, "id=%d access=%u registered=%d",
		static_cast<int>(id),
		var != nullptr ? static_cast<unsigned>(var->GetAccessIndex()) : 0u,
		var != nullptr ? var->GetRegisteredIndex() : -1);
	CS2ST_HOOK_LEAVE(kOnUnlinkConVar);
}

} // namespace cs2st
