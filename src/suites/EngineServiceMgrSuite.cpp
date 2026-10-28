#include "suites/EngineServiceMgrSuite.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include <appframework/iappsystem.h>
#include <engine2/iengineservicemgr.h>
#include <interfaces/interfaces.h>

#include "core/Log.h"
#include "testkit/StructInspector.h"

namespace cs2st
{
namespace
{

// Bounds for everything that walks something the engine counts: a garbage
// count must end the loop, not the process.
constexpr int kMaxAddonsListed = 16;

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

const char* ClientServerModeName(int mode)
{
	switch (mode)
	{
		case CLIENTSERVERMODE_NONE:         return "NONE";
		case CLIENTSERVERMODE_SERVER:       return "SERVER";
		case CLIENTSERVERMODE_CLIENT:       return "CLIENT";
		case CLIENTSERVERMODE_LISTENSERVER: return "LISTENSERVER";
		default:                            return "<out of range>";
	}
}

const char* ActivateName(EngineServiceActivateType_t type)
{
	switch (type)
	{
		case ENGINE_SERVICE_ACTIVATE_NEVER:                  return "NEVER";
		case ENGINE_SERVICE_ACTIVATE_ALWAYS:                 return "ALWAYS";
		case ENGINE_SERVICE_ACTIVATE_IF_ADDED_BY_LOOP_MODE:  return "IF_ADDED_BY_LOOP_MODE";
		default:                                             return "<out of range>";
	}
}

const char* Str(const char* text)
{
	return text != nullptr ? text : "(null)";
}

// Names the engine's services are known to register under. The registry cannot
// be enumerated, so this list is the probe; a name missing from the answer, or
// a service missing from this list, is exactly what the log is for. Recorded,
// never judged.
//
// The first thirteen are the exact strings the engine's "simple" loop passes to
// ILoopType::AddEngineService in the 2026-10 Linux engine2 (suffixes and all:
// "_001", "V001" and "001" are not interchangeable). The rest are guesses at
// the network services, whose registered names are not known.
const char* const kProbeServiceNames[] = {
	"GameResourceServiceServerV001",
	"InputService_001",
	"SimpleEngineLoopService_001",
	"StatsService_001",
	"VProfService_001",
	"BugService001",
	"GameResourceServiceClientV001",
	"RenderService_001",
	"ToolService_001",
	"GameUIService_001",
	"ScreenshotService001",
	"SoundService_001",
	"NetworkP2PService_001",
	"NetworkServerService_001",
	"NetworkClientService_001",
	"NetworkService_001",
};

/**
 * FindService for one name, as its own guarded row. The name goes into the row
 * label so a diff shows which names were found; the label is built outside
 * the macro because CS2ST_CALL is a two-argument macro and a call with commas
 * in its target argument would split before the compiler saw it.
 *
 * A found service is asked for its own name, which the registry keys on:
 * disagreement means the registry holds something else under that name.
 */
void ProbeService(TestContext& context, IEngineServiceMgr* manager, const char* name)
{
	char label[160];
	std::snprintf(label, sizeof(label), "IEngineServiceMgr::FindService [%s]", name);

	CS2ST_CALL(context, label, {
		IEngineService* service = manager->FindService(name);
		if (service == nullptr)
		{
			context.Detail("-> not registered");
			return;
		}

		const char* reported = service->GetName();
		context.Detail("-> %p, GetName \"%s\", IsActive %s, index %u",
			static_cast<void*>(service), Str(reported),
			service->IsActive() ? "true" : "false",
			static_cast<unsigned>(service->GetServiceIndex()));
		if (reported == nullptr)
		{
			context.Fail("a service the registry returned reports no name");
		}
		else if (std::strcmp(reported, name) != 0)
		{
			context.Fail("found under \"%s\" but the service reports \"%s\"", name, reported);
		}
	});
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool EngineServiceMgrSuite::Acquire(TestContext& context)
{
	manager_ = static_cast<IEngineServiceMgr*>(
		context.QueryInterface("engine2", ENGINESERVICEMGR_INTERFACE_VERSION));

	if (manager_ == nullptr)
	{
		const Module& engine_module = context.GetModule("engine2");
		if (!engine_module.IsValid())
		{
			context.UnavailableCall("IEngineServiceMgr", "the engine2 module is not loaded");
		}
		else
		{
			context.UnavailableCall("IEngineServiceMgr",
				"engine2 is loaded but does not answer '%s'",
				ENGINESERVICEMGR_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(manager_),
		ENGINESERVICEMGR_INTERFACE_VERSION);
	return true;
}

void EngineServiceMgrSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseLoop(context);
	ExerciseServices(context);
	ExerciseAddons(context);
	ExerciseEngineWindow(context);
	ExerciseTailSlots(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots, same as every other AppSystem interface
// -----------------------------------------------------------------------------

void EngineServiceMgrSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = manager_->GetTier();
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
		context.Detail("-> %s", manager_->IsSingleton() ? "true" : "false");
	});

	CS2ST_CALL(context, "IAppSystem::GetBuildType", {
		const BuildType_t build = manager_->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	CS2ST_CALL(context, "IAppSystem::GetDependencies", {
		const AppSystemInfo_t* deps = manager_->GetDependencies();
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
		for (int i = 0; i < count && i < 16; ++i)
		{
			context.Detail("   [%d] module \"%s\", interface \"%s\"", i,
				deps[i].m_pModuleName, Str(deps[i].m_pInterfaceName));
		}
	});

	CS2ST_CALL(context, "IAppSystem::QueryInterface (unknown name)", {
		void* none = manager_->QueryInterface("CS2SDKTestNoSuchInterface001");
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
//  The active loop
// -----------------------------------------------------------------------------

void EngineServiceMgrSuite::ExerciseLoop(TestContext& context)
{
	context.Note("[IEngineServiceMgr] active loop and loop-switch state");

	CS2ST_CALL(context, "IEngineServiceMgr::GetActiveLoopName", {
		context.Detail("-> \"%s\"", Str(manager_->GetActiveLoopName()));
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetActiveLoop", {
		// An out-parameter the engine fills with Set / SetDirect, which free what the
		// strings held before, so it has to be constructed (empty) first. When no loop
		// is active the engine writes only m_bValid, so nothing else is read unless it is set.
		//
		// Kept on the heap and never destroyed, so InspectActiveLoop can read it
		// after this row; the engine's strings are a few dozen bytes.
		active_loop_ = new ActiveLoop_t();
		ActiveLoop_t& loop = *active_loop_;
		manager_->GetActiveLoop(&loop);
		const bool has_name = manager_->GetActiveLoopName() != nullptr
			&& manager_->GetActiveLoopName()[0] != '\0';
		context.Detail("-> valid %s (loop name %s)", loop.m_bValid ? "true" : "false",
			has_name ? "set" : "empty");
		// Two routes to "is a loop active": the buffer's flag and the name. The
		// link comes from reversing, so a mismatch is Suspect.
		if (loop.m_bValid != has_name)
		{
			context.Suspect("GetActiveLoop and GetActiveLoopName disagree about whether a loop is active");
		}
	});

	InspectActiveLoop(context);

	CS2ST_CALL(context, "IEngineServiceMgr::GetActiveLoopClientServerMode", {
		const int mode = manager_->GetActiveLoopClientServerMode();
		context.Detail("-> %s (%d)", ClientServerModeName(mode), mode);
		if (mode < CLIENTSERVERMODE_NONE || mode > CLIENTSERVERMODE_LISTENSERVER)
		{
			context.Fail("%d is outside ClientServerMode_t", mode);
		}
	});

	CS2ST_CALL(context, "IEngineServiceMgr::IsLoopSwitchQueued", {
		context.Detail("-> %s", manager_->IsLoopSwitchQueued() ? "true" : "false");
	});

	CS2ST_CALL(context, "IEngineServiceMgr::IsLoopSwitchRequested", {
		context.Detail("-> %s", manager_->IsLoopSwitchRequested() ? "true" : "false");
	});

	CS2ST_CALL(context, "IEngineServiceMgr::IsLoadingLevel", {
		context.Detail("-> %s", manager_->IsLoadingLevel() ? "true" : "false");
	});

	CS2ST_CALL(context, "IEngineServiceMgr::IsInGameLoop", {
		context.Detail("-> %s", manager_->IsInGameLoop() ? "true" : "false");
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetEventDispatcher", {
		context.Detail("-> %p", static_cast<void*>(manager_->GetEventDispatcher()));
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetDebugVisualizerMgr", {
		context.Detail("-> %p", manager_->GetDebugVisualizerMgr());
	});

	CS2ST_CALL(context, "IEngineServiceMgr::PrintStatus", {
		// Read-only: prints the manager's status to the console. The text goes
		// to the engine's log, not to this suite's.
		manager_->PrintStatus();
		context.Detail("-> printed to the engine console");
	});
}

// The buffer GetActiveLoop filled, field by field and as raw bytes. Each
// CUtlString is one char* in memory, so String() applies. m_sAddonName is
// null when no addon is mounted, and m_pLoopOptions is the engine's copy of
// SwitchToLoop's options, a KeyValues object and not a string.
void EngineServiceMgrSuite::InspectActiveLoop(TestContext& context)
{
	if (active_loop_ == nullptr)
	{
		context.SkipCall("ActiveLoop_t layout", "GetActiveLoop did not run");
		return;
	}
	if (!active_loop_->m_bValid)
	{
		// Only m_bValid is written for "no loop"; the rest is not the engine's.
		context.SkipCall("ActiveLoop_t layout", "no active loop, the engine writes only m_bValid");
		return;
	}

	StructInspector inspect(context, "ActiveLoop_t", active_loop_, sizeof(ActiveLoop_t));
	inspect.String ("m_sLoopName",     offsetof(ActiveLoop_t, m_sLoopName));
	inspect.String ("m_sAddonName",    offsetof(ActiveLoop_t, m_sAddonName), kMayBeUninitialized);
	inspect.UInt32 ("m_nId",           offsetof(ActiveLoop_t, m_nId));
	inspect.Pointer("m_pLoopOptions",  offsetof(ActiveLoop_t, m_pLoopOptions));
	inspect.Bool   ("m_bValid",        offsetof(ActiveLoop_t, m_bValid));
	inspect.Summarize();
	inspect.HexDump(0);
}

// -----------------------------------------------------------------------------
//  The service registry
// -----------------------------------------------------------------------------

void EngineServiceMgrSuite::ExerciseServices(TestContext& context)
{
	context.Note("[IEngineServiceMgr] service registry - probed by name, the registry has no enumeration");

	for (const char* name : kProbeServiceNames)
	{
		ProbeService(context, manager_, name);
	}

	CS2ST_CALL(context, "IEngineServiceMgr::FindService (unknown name)", {
		IEngineService* none = manager_->FindService("CS2SDKTestNoSuchService");
		context.Detail("-> %p", static_cast<void*>(none));
		if (none != nullptr)
		{
			context.Suspect("a service name nobody registered resolved to an object");
		}
	});

	CS2ST_CALL(context, "IEngineServiceMgr::LookupLocalizationToken", {
		// The same method as ILocalize's; an unknown token is the safe probe.
		context.Detail("-> \"%s\"",
			Str(manager_->LookupLocalizationToken("#CS2SDKTest_NoSuchToken")));
	});
}

// -----------------------------------------------------------------------------
//  Addons
// -----------------------------------------------------------------------------

void EngineServiceMgrSuite::ExerciseAddons(TestContext& context)
{
	context.Note("[IEngineServiceMgr] addon list");

	CS2ST_CALL(context, "IEngineServiceMgr::GetAddonCount / GetAddon / IsAddonMounted / GetAddonsString", {
		const int count = manager_->GetAddonCount();
		const char* joined = manager_->GetAddonsString();
		context.Detail("-> %d addon(s), GetAddonsString \"%s\"", count, Str(joined));
		if (count < 0)
		{
			context.Fail("GetAddonCount returned %d", count);
			return;
		}

		const int shown = count < kMaxAddonsListed ? count : kMaxAddonsListed;
		for (int i = 0; i < shown; ++i)
		{
			const char* addon = manager_->GetAddon(i);
			context.Detail("   [%d] \"%s\"", i, Str(addon));
			if (addon == nullptr)
			{
				context.Suspect("GetAddon(%d) is null below GetAddonCount %d", i, count);
				continue;
			}
			// Each route to the same fact: listed, mounted, and in the string.
			if (!manager_->IsAddonMounted(addon))
			{
				context.Suspect("addon \"%s\" is listed by GetAddon but not mounted", addon);
			}
			if (joined != nullptr && std::strstr(joined, addon) == nullptr)
			{
				context.Suspect("addon \"%s\" is missing from GetAddonsString", addon);
			}
		}
		if (count > shown)
		{
			context.Detail("   ... and %d more", count - shown);
		}
	});

	CS2ST_CALL(context, "IEngineServiceMgr::IsAddonMounted (unknown name)", {
		const bool mounted = manager_->IsAddonMounted("cs2sdktest_no_such_addon");
		context.Detail("-> %s", mounted ? "true" : "false");
		if (mounted)
		{
			context.Suspect("an addon nobody mounted reports as mounted");
		}
	});

	CS2ST_CALL(context, "IEngineServiceMgr::unk202 (addon name in the unk201 list)", {
		// Read-only: tests the name against the list unk201 appends to.
		const bool listed = manager_->unk202("cs2sdktest_no_such_addon");
		context.Detail("-> %s", listed ? "true" : "false");
		if (listed)
		{
			context.Suspect("a name nobody added is in the unk201 list");
		}
	});

	CS2ST_CALL(context, "IEngineServiceMgr::unk203 (mounted addon missing from the unk201 list)", {
		context.Detail("-> %s", manager_->unk203() ? "true" : "false");
	});
}

// -----------------------------------------------------------------------------
//  Window / swap chain / device - client machinery. A dedicated server answers
//  with null or zero; both are recorded.
// -----------------------------------------------------------------------------

void EngineServiceMgrSuite::ExerciseEngineWindow(TestContext& context)
{
	context.Note("[IEngineServiceMgr] window / swap chain / device - client-side, null or zero expected on a dedicated server");

	CS2ST_CALL(context, "IEngineServiceMgr::GetEngineWindow", {
		context.Detail("-> %p", static_cast<const void*>(manager_->GetEngineWindow()));
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetEngineSwapChain", {
		context.Detail("-> %p", static_cast<const void*>(manager_->GetEngineSwapChain()));
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetEngineInputContext", {
		context.Detail("-> %p", static_cast<const void*>(manager_->GetEngineInputContext()));
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetEngineDeviceInfo", {
		context.Detail("-> %p", manager_->GetEngineDeviceInfo());
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetEngineDeviceWidth / GetEngineDeviceHeight", {
		const int width = manager_->GetEngineDeviceWidth();
		const int height = manager_->GetEngineDeviceHeight();
		context.Detail("-> %d x %d", width, height);
		if (width < 0 || height < 0)
		{
			context.Suspect("a device dimension is negative");
		}
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetEngineSwapChainSize", {
		// Real destinations, zeroed: a callee that writes lands on our stack,
		// one that does not leaves zeros we can tell from a sentinel.
		int width = 0;
		int height = 0;
		manager_->GetEngineSwapChainSize(&width, &height);
		context.Detail("-> %d x %d", width, height);
		if (width < 0 || height < 0)
		{
			context.Suspect("a swap chain dimension is negative");
		}
	});
}

// -----------------------------------------------------------------------------
//  The remaining read-only slots
// -----------------------------------------------------------------------------

void EngineServiceMgrSuite::ExerciseTailSlots(TestContext& context)
{
	context.Note("[IEngineServiceMgr] remaining slots");

	CS2ST_CALL(context, "IEngineServiceMgr::unk101", {
		context.Detail("-> %s", manager_->unk101() ? "true" : "false");
	});

	CS2ST_CALL(context, "IEngineServiceMgr::unk301 (prerequisites the queued loop switch waits for)", {
		const int waiting = manager_->unk301();
		context.Detail("-> %d", waiting);
		if (waiting < 0)
		{
			context.Suspect("a prerequisite count is negative");
		}
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetFrameTimeAmnesty (check_cvar = false)", {
		context.Detail("-> \"%s\"", Str(manager_->GetFrameTimeAmnesty(false)));
	});

	CS2ST_CALL(context, "IEngineServiceMgr::GetFrameTimeAmnesty (check_cvar = true)", {
		context.Detail("-> \"%s\"", Str(manager_->GetFrameTimeAmnesty(true)));
	});
}

// -----------------------------------------------------------------------------
//  Everything that registers, starts, stops or changes something
// -----------------------------------------------------------------------------

void EngineServiceMgrSuite::NoteSideEffectingCalls(TestContext& context)
{
	context.SkipCall("IEngineServiceMgr::RegisterEngineService",
		"adds a service to the engine's registry; needs an IEngineService we do not have");
	context.SkipCall("IEngineServiceMgr::UnregisterEngineService",
		"removes a service from the engine's registry");
	context.SkipCall("IEngineServiceMgr::RegisterLoopMode",
		"adds a loop mode; needs an ILoopModeFactory we do not have");
	context.SkipCall("IEngineServiceMgr::UnregisterLoopMode",
		"removes a loop mode from the engine");
	context.SkipCall("IEngineServiceMgr::SwitchToLoop",
		"queues a loop switch: the server leaves its current map");
	context.SkipCall("IEngineServiceMgr::ChangeVideoMode",
		"client video mode; needs a RenderDeviceInfo_t");
	context.SkipCall("IEngineServiceMgr::GetVideoModeChange",
		"returns nothing and is client video-mode state; the effect is not observable");
	context.SkipCall("IEngineServiceMgr::OnFrameRenderingFinished",
		"client frame callback; needs an EventClientOutput_t");
	context.SkipCall("IEngineServiceMgr::unk201",
		"appends a name to the engine's own list; not observable except through unk202 / unk203");
	context.SkipCall("IEngineServiceMgr::InstallSwitchLoopModeStatusNotify",
		"installs a listener the engine keeps; needs an ISwitchLoopModeStatusNotify");
	context.SkipCall("IEngineServiceMgr::UninstallSwitchLoopModeStatusNotify",
		"needs the listener Install would have registered");
	context.SkipCall("IEngineServiceMgr::InstallAddonListChangeNotify",
		"installs a listener the engine keeps; needs an IAddonListChangeNotify");
	context.SkipCall("IEngineServiceMgr::UninstallAddonListChangeNotify",
		"needs the listener Install would have registered");
	context.SkipCall("IEngineServiceMgr::StartEngineWatchdogThread",
		"starts a thread");
	context.SkipCall("IEngineServiceMgr::AddLogCaptureString",
		"appends to the engine's log capture buffer");
	context.SkipCall("IEngineServiceMgr::AddLogCaptureStringV",
		"appends to the engine's log capture buffer; needs a va_list");
	context.SkipCall("IEngineServiceMgr::AddLogCaptureStringF",
		"appends to the engine's log capture buffer");
	context.SkipCall("IEngineServiceMgr::ExitMainLoop",
		"exits the engine");
	context.SkipCall("IEngineServiceMgr::RegisterPrerequisite",
		"makes a queued loop switch wait on a prerequisite; needs an IPrerequisite");
	context.SkipCall("IEngineServiceMgr::SetFrameTimeAmnesty",
		"changes engine frame-time reporting");
	context.SkipCall("IEngineServiceMgr::SetFramePerformanceTag",
		"changes engine frame performance reporting");
#ifdef PLATFORM_LINUX
	context.SkipCall("IEngineServiceMgr::UnregisterPrerequisite",
		"removes a prerequisite from a queued loop switch (Linux-only slot)");
#endif
}

} // namespace cs2st
