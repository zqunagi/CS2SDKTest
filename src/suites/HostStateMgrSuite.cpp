#include "suites/HostStateMgrSuite.h"

#include <cstddef>

#include <appframework/iappsystem.h>
#include <engine2/hoststate.h>
#include <interfaces/interfaces.h>

#include "core/Log.h"
#include "testkit/StructInspector.h"

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

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool HostStateMgrSuite::Acquire(TestContext& context)
{
	manager_ = static_cast<IHostStateMgr*>(
		context.QueryInterface("engine2", HOSTSTATEMGR_INTERFACE_VERSION));

	if (manager_ == nullptr)
	{
		const Module& engine_module = context.GetModule("engine2");
		if (!engine_module.IsValid())
		{
			context.UnavailableCall("IHostStateMgr", "the engine2 module is not loaded");
		}
		else
		{
			context.UnavailableCall("IHostStateMgr",
				"engine2 is loaded but does not answer '%s'", HOSTSTATEMGR_INTERFACE_VERSION);
		}
		return false;
	}

	context.Note("interface at %p (%s)", static_cast<void*>(manager_),
		HOSTSTATEMGR_INTERFACE_VERSION);
	return true;
}

void HostStateMgrSuite::ExerciseCalls(TestContext& context)
{
	ExerciseAppSystem(context);
	ExerciseRequests(context);
	InspectManager(context);
}

// -----------------------------------------------------------------------------
//  IAppSystem - the first 11 slots
// -----------------------------------------------------------------------------

void HostStateMgrSuite::ExerciseAppSystem(TestContext& context)
{
	context.Note("[IAppSystem] base slots - if these are wrong, nothing below means anything");

	CS2ST_CALL(context, "IAppSystem::GetTier", {
		const AppSystemTier_t tier = manager_->GetTier();
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
				deps[i].m_pModuleName,
				deps[i].m_pInterfaceName != nullptr ? deps[i].m_pInterfaceName : "(null)");
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
//  Requests - every IHostStateMgr method queues a state change
// -----------------------------------------------------------------------------

void HostStateMgrSuite::ExerciseRequests(TestContext& context)
{
	context.Note("[IHostStateMgr] every method queues a host state transition and none has a "
		"read-only form; state is read through the CHostStateMgr layout below");

	context.SkipCall("IHostStateMgr::RequestHS_Quit", "shuts the process down");
	context.SkipCall("IHostStateMgr::RequestHS_Idle", "tears down the running game and returns to idle");
	context.SkipCall("IHostStateMgr::RequestHS_Connect", "disconnects and connects to another server");
	context.SkipCall("IHostStateMgr::RequestHS_ChangelevelReconnect", "changes the level on the live server");
	context.SkipCall("IHostStateMgr::RequestHS_LoadSpawnGroup", "loads level content on the live server");
	context.SkipCall("IHostStateMgr::RequestHS_LoadSaveGame", "replaces the running game with a save");
	context.SkipCall("IHostStateMgr::RequestHS_PlayDemo", "replaces the running game with a demo");
	context.SkipCall("IHostStateMgr::RequestHS_PlayBroadcast", "replaces the running game with a broadcast");
	context.SkipCall("IHostStateMgr::RequestHS_DownloadAddons", "starts an addon download and loop switch");
	context.SkipCall("IHostStateMgr::RequestHS_SourceTVRelay", "replaces the running game with a SourceTV relay");
	context.SkipCall("IHostStateMgr::RequestHS_ReloadLastSaveGame", "replaces the running game with the last save");
	context.SkipCall("IHostStateMgr::RequestHS_RestartSpawnGroups", "reloads every spawn group on the live server");
	context.SkipCall("IHostStateMgr::unk001", "unidentified slot; calling it blind is not safe");
	context.SkipCall("IHostStateMgr::unk002", "unidentified slot; calling it blind is not safe");
}

// -----------------------------------------------------------------------------
//  CHostStateMgr / CHostStateRequest layout
// -----------------------------------------------------------------------------

void HostStateMgrSuite::InspectManager(TestContext& context)
{
	// The interface pointer is the CHostStateMgr: IHostStateMgr is its first base.
	const CHostStateMgr* mgr = static_cast<const CHostStateMgr*>(manager_);

	// The SDK comments place the first field at 0x30; another offset means the
	// tier-3 base changed size, which shifts every field after it.
	context.Detail("m_PendingRequest at +0x%zx (SDK comment 0x30)",
		static_cast<std::size_t>(offsetof(CHostStateMgr, m_PendingRequest)));

	StructInspector inspect(context, "CHostStateMgr", mgr, sizeof(CHostStateMgr));
	inspect.Pointer("m_PendingRequest",       offsetof(CHostStateMgr, m_PendingRequest));
	inspect.Pointer("m_CurrentRequest",       offsetof(CHostStateMgr, m_CurrentRequest));
	inspect.String ("m_LoopModeType",         offsetof(CHostStateMgr, m_LoopModeType));
	inspect.Pointer("m_pGameConfigurationKV", offsetof(CHostStateMgr, m_pGameConfigurationKV));
	inspect.String ("m_LoopMode",             offsetof(CHostStateMgr, m_LoopMode));
	inspect.Pointer("m_pConnectKV",           offsetof(CHostStateMgr, m_pConnectKV));
	inspect.String ("m_LoopModeName",         offsetof(CHostStateMgr, m_LoopModeName));
	inspect.Pointer("m_pSourceTVRelayKV",     offsetof(CHostStateMgr, m_pSourceTVRelayKV));
	inspect.String ("m_SaveGame",             offsetof(CHostStateMgr, m_SaveGame));
	inspect.String ("m_LevelName",            offsetof(CHostStateMgr, m_LevelName));
	inspect.String ("m_Addons",               offsetof(CHostStateMgr, m_Addons));
	inspect.Pointer("m_pKV",                  offsetof(CHostStateMgr, m_pKV));
	inspect.Int32  ("m_ID",                   offsetof(CHostStateMgr, m_ID));
	inspect.Summarize();

	const int queued = mgr->m_QueuedRequests.Count();
	context.Detail("m_QueuedRequests: %d request(s)", queued);
	if (queued < 0)
	{
		context.Fail("m_QueuedRequests count is %d", queued);
	}

	// One CHostStateRequest section, from whichever request is reachable: the
	// current one if the engine is in a state, else the pending one, else the
	// head of the queue. A second section for the same type would be a duplicate
	// registration.
	const CHostStateRequest* request = mgr->m_CurrentRequest;
	if (request == nullptr)
	{
		request = mgr->m_PendingRequest;
	}
	if (request == nullptr && queued > 0)
	{
		request = mgr->m_QueuedRequests[0];
	}
	if (request == nullptr)
	{
		context.SkipCall("CHostStateRequest layout", "no current, pending or queued request");
		return;
	}

	StructInspector req(context, "CHostStateRequest", request, sizeof(CHostStateRequest));
	req.Enum   ("m_iType",        offsetof(CHostStateRequest, m_iType), HSR_IDLE, HSR_QUIT);
	req.String ("m_LoopModeType", offsetof(CHostStateRequest, m_LoopModeType));
	req.String ("m_Desc",         offsetof(CHostStateRequest, m_Desc));
	req.Bool   ("m_bActive",      offsetof(CHostStateRequest, m_bActive));
	req.UInt32 ("m_ID",           offsetof(CHostStateRequest, m_ID));
	req.Enum   ("m_iMode",        offsetof(CHostStateRequest, m_iMode), HM_LEVEL_LOAD_SERVER, HM_ADDON_DOWNLOAD);
	req.String ("m_LevelName",    offsetof(CHostStateRequest, m_LevelName));
	req.Bool   ("m_bChangelevel", offsetof(CHostStateRequest, m_bChangelevel));
	req.String ("m_SaveGame",     offsetof(CHostStateRequest, m_SaveGame));
	req.String ("m_Address",      offsetof(CHostStateRequest, m_Address));
	req.String ("m_DemoFile",     offsetof(CHostStateRequest, m_DemoFile));
	req.Bool   ("m_bTimeDemo",    offsetof(CHostStateRequest, m_bTimeDemo));
	req.String ("m_Addons",       offsetof(CHostStateRequest, m_Addons));
	req.Pointer("m_pKV",          offsetof(CHostStateRequest, m_pKV));
	req.Summarize();
}

} // namespace cs2st
