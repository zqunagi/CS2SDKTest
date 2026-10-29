#include "suites/Source2EngineSuite.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include <appframework/iappsystem.h>
#include <engine2/iscreenshotcallback.h>
#include <engine2/isource2engine.h>
#include <engine2/ivengineserver2.h>
#include <engine2/gameshareddefs.h>
#include <network_connection.pb.h>
#include <networkbasetypes.pb.h>

#include "core/Log.h"
#include "testkit/StructInspector.h"
#include "testkit/UtlVectorGuard.h"

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

/**
 * Handed to ISource2Engine::RequestScreenshot. The engine may keep the pointer
 * and call back later, so it has static storage duration instead of living on
 * the stack of the call that queued it. Every slot declines the data (false),
 * which tells the engine it still owns and frees the buffer.
 */
class CountingScreenshotCallback final : public IScreenshotCallback
{
public:
	bool Screenshot(int, int, int, uint8*, int, int64) override { ++calls; return false; }
	bool ScreenshotJPEG(int, int, void*, int, void*) override { ++calls; return false; }
	bool ScreenshotPNG(int, int, void*, int, void*) override { ++calls; return false; }
	bool Screenshot_GPUFrame(uint32, uint32, int, const uint8*, size_t, int64, uint32) override
	{
		++calls;
		return false;
	}
	bool IsDoneWithData(void*) override { ++calls; return true; }

	// Atomic rather than volatile: a deferred callback may arrive on an engine thread.
	std::atomic<int> calls{0};
};

CountingScreenshotCallback g_screenshot_callback;

/** Number of set bits in a player bit vector, read through its raw bytes. */
int CountSetBits(const void* data, size_t size)
{
	const uint8* bytes = static_cast<const uint8*>(data);
	int count = 0;
	for (size_t i = 0; i < size; ++i)
	{
		for (int bit = 0; bit < 8; ++bit)
		{
			count += (bytes[i] >> bit) & 1;
		}
	}
	return count;
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
	ExerciseWorldGroups(context);
	ExercisePlayerQueries(context);
	ExerciseSpawnGroups(context);
	InspectGlobalVars(context);
	InspectAppSystemInfo(context);
	NoteSideEffectingCalls(context);
}

// -----------------------------------------------------------------------------
//  AppSystemInfo_t - what IAppSystem::GetDependencies returns
//
//  The one registration for this type: every other suite's AppSystem walks the
//  same array and only reads its two strings.
// -----------------------------------------------------------------------------

void Source2EngineSuite::InspectAppSystemInfo(TestContext& context)
{
	context.Note("[AppSystemInfo_t] struct layout - first entry of GetDependencies");

	if (dependencies_ == nullptr)
	{
		context.SkipCall("AppSystemInfo_t",
			"no reachable instance: GetDependencies returned no entries");
		return;
	}

	StructInspector inspect(context, "AppSystemInfo_t", dependencies_, sizeof(AppSystemInfo_t));
	inspect.String("m_pModuleName",    offsetof(AppSystemInfo_t, m_pModuleName));
	inspect.String("m_pInterfaceName", offsetof(AppSystemInfo_t, m_pInterfaceName));
	inspect.Summarize();
	inspect.HexDump();
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
		if (count > 0)
		{
			dependencies_ = deps;
		}
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

	// SetTimescale is only exercised when the timescale is already 1: GetTimescale
	// reports the value multiplied with host_timescale, so writing back any other
	// reading could compound the two and leave the server at a different speed
	// for every suite after this one.
	CS2ST_CALL(context, "IVEngineServer2::SetTimescale", {
		const float before = engine_->GetTimescale();
		if (std::fabs(before - 1.0f) > 0.0001f)
		{
			context.SkipCall("IVEngineServer2::SetTimescale",
				"timescale is %.4f, not 1; writing it back could compound with host_timescale",
				before);
			return;
		}
		engine_->SetTimescale(1.0f);
		const float after = engine_->GetTimescale();
		context.Detail("1.0 -> read back %.4f", after);
		if (std::fabs(after - 1.0f) > 0.001f)
		{
			context.Suspect("SetTimescale(1.0) did not read back as 1.0");
		}
	});

	context.SkipCall("ISource2Engine::FindOrCreateWorldSession",
		"creates a world session and leaves it registered; the second and third "
		"parameters are unidentified, so no argument is known to be safe");
	context.SkipCall("ISource2Engine::GetEntityLumpForTemplate",
		"returns an entity lump the engine allocates for the caller; five unidentified "
		"parameters and no known release path");
	// Last in the interface because a dedicated server has no renderer: these two
	// are the calls most likely to be answered by a null where a render context
	// should be, and the fault guard is what turns that into a logged result.
	CS2ST_CALL(context, "ISource2Engine::RequestScreenshot", {
		const int before = g_screenshot_callback.calls;
		engine_->RequestScreenshot(&g_screenshot_callback, nullptr);
		context.Detail("queued; callback entered %d time(s) during the call",
			g_screenshot_callback.calls - before);
	});

	CS2ST_CALL(context, "ISource2Engine::WriteJpegScreenshot", {
		engine_->WriteJpegScreenshot("cs2sdktest_probe.jpg", 50, 16, 16, false);
		context.Detail("returned (no result value; the file is only written with a renderer)");
	});

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

	// Precaching is after the IsGenericPrecached probe above on purpose: that
	// query is only meaningful while the name is still unknown to the engine.
	CS2ST_CALL(context, "IVEngineServer2::PrecacheGeneric", {
		context.Detail("-> %d for a nonexistent resource (no preload)",
			engine_->PrecacheGeneric("cs2sdktest/no_such_resource", false));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetMapName", {
		const char* name = engine_->GetMapName();
		context.Detail("-> \"%s\"", name != nullptr ? name : "(null)");
		if (name == nullptr)
		{
			context.Fail("GetMapName returned null instead of an empty string");
			return;
		}
		// Second path to the same fact: CGlobalVars::mapname.
		if (globals_ != nullptr)
		{
			const char* global_name = globals_->mapname.ToCStr();
			if (std::strcmp(name, global_name) != 0)
			{
				context.Suspect("GetMapName \"%s\" != CGlobalVars::mapname \"%s\"",
					name, global_name);
			}
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::GetMainLoopTimes", {
		// The SDK documents two floats: time before the frame ran, and the frame.
		const float* times = engine_->GetMainLoopTimes();
		if (times == nullptr)
		{
			context.Detail("-> null");
			return;
		}
		const float before_frame = times[0];
		const float frame = times[1];
		context.Detail("-> %p before=%.6f frame=%.6f", static_cast<const void*>(times),
			before_frame, frame);
		if (!std::isfinite(before_frame) || !std::isfinite(frame)
			|| before_frame < 0.0f || frame < 0.0f)
		{
			context.Suspect("a loop time is negative or not finite");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::ShouldForceMaxFrametimeToTickInterval", {
		context.Detail("-> %s",
			engine_->ShouldForceMaxFrametimeToTickInterval() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetLongFrameCount", {
		context.Detail("-> %u", engine_->GetLongFrameCount());
	});

	CS2ST_CALL(context, "IVEngineServer2::WasShutDownRequested", {
		const bool requested = engine_->WasShutDownRequested();
		context.Detail("-> %s", requested ? "true" : "false");
		if (requested)
		{
			context.Suspect("a shutdown is already pending while the suite is running");
		}
	});

	// ---- demo / TV recording: reads only. Starting or stopping a recording is a
	// ---- one-way change (StopRecordingDemo needs a CGameInfo we cannot build).

	CS2ST_CALL(context, "IVEngineServer2::IsTvRecording", {
		context.Detail("-> %s", engine_->IsTvRecording() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsRecordingDemo", {
		context.Detail("-> %s", engine_->IsRecordingDemo() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetTvRecordingDemoFilename", {
		const char* filename = engine_->GetTvRecordingDemoFilename();
		context.Detail("-> \"%s\"", filename != nullptr ? filename : "(null)");
		if (filename == nullptr)
		{
			context.Fail("the SDK promises an empty string, not null, when not recording");
			return;
		}
		// Oracle: IsTvRecording false <=> filename empty (SDK-documented).
		if (!engine_->IsTvRecording() && filename[0] != '\0')
		{
			context.Fail("a filename is reported while IsTvRecording is false");
		}
	});

	// ---- HLTV replay: global state reads.

	CS2ST_CALL(context, "IVEngineServer2::IsHltvReplayBufferAvailable", {
		context.Detail("-> %s", engine_->IsHltvReplayBufferAvailable() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::IsAnyHltvReplayActive", {
		context.Detail("-> %s", engine_->IsAnyHltvReplayActive() ? "true" : "false");
	});

	// ---- round trips: write back exactly what was just read, so the end state is
	// ---- the start state and the second read is the oracle.

	CS2ST_CALL(context, "IVEngineServer2::SetGamestatsData", {
		CGamestatsData* before = engine_->GetGamestatsData();
		engine_->SetGamestatsData(before);
		CGamestatsData* after = engine_->GetGamestatsData();
		context.Detail("%p -> %p", static_cast<void*>(before), static_cast<void*>(after));
		if (before != after)
		{
			context.Fail("GetGamestatsData did not return what SetGamestatsData stored");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::SetAchievementMgr", {
		IAchievementMgr* before = engine_->GetAchievementMgr();
		engine_->SetAchievementMgr(before);
		IAchievementMgr* after = engine_->GetAchievementMgr();
		context.Detail("%p -> %p", static_cast<void*>(before), static_cast<void*>(after));
		if (before != after)
		{
			context.Fail("GetAchievementMgr did not return what SetAchievementMgr stored");
		}
	});

	// Benchmark mode is off on any server this runs against; writing false is the
	// identity.
	CS2ST_CALL(context, "IVEngineServer2::SetDedicatedServerBenchmarkMode", {
		engine_->SetDedicatedServerBenchmarkMode(false);
		context.Detail("set to false (the normal state)");
	});

	// A zero-length amnesty is the only kind that cannot excuse a real long frame.
	CS2ST_CALL(context, "IVEngineServer2::SetFrameTimeAmnesty", {
		engine_->SetFrameTimeAmnesty("cs2sdktest", 0, 0.0f);
		const char* amnesty = engine_->GetFrameTimeAmnesty(false);
		context.Detail("set \"cs2sdktest\"/0.0s -> GetFrameTimeAmnesty(false) = %s",
			amnesty != nullptr ? amnesty : "(null)");
	});

	CS2ST_CALL(context, "IVEngineServer2::SetFramePerformanceTag", {
		engine_->SetFramePerformanceTag("cs2sdktest", 0, 0, 0.0f);
		context.Detail("tag set with a zero duration");
	});

	// ---- output: harmless text.

	CS2ST_CALL(context, "IVEngineServer2::LogPrint", {
		engine_->LogPrint("cs2sdktest: Source2EngineSuite LogPrint probe\n");
		context.Detail("-> IsLogEnabled=%s", engine_->IsLogEnabled() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::ServerCommand", {
		engine_->ServerCommand("echo cs2sdktest: Source2EngineSuite ServerCommand probe\n");
		context.Detail("queued an echo");
	});

	// ---- HLTV relay / chat ban: arguments chosen to be no-ops.

	CS2ST_CALL(context, "IVEngineServer2::SetHLTVChatBan", {
		// Un-banning an invalid SteamID: nothing to change.
		engine_->SetHLTVChatBan(CSteamID(), false);
		context.Detail("un-ban of an invalid SteamID");
	});

	CS2ST_CALL(context, "IVEngineServer2::AddHltvRelayProxyWhitelist", {
		// 192.0.2.1/32 is TEST-NET-1 (RFC 5737): never routable, so whitelisting it
		// cannot admit a real relay.
		engine_->AddHltvRelayProxyWhitelist(192, 0, 2, 1, 32);
		context.Detail("whitelisted 192.0.2.1/32 (TEST-NET-1)");
	});

	CS2ST_CALL(context, "IVEngineServer2::StopAllHltvReplays", {
		engine_->StopAllHltvReplays();
		context.Detail("-> IsAnyHltvReplayActive=%s",
			engine_->IsAnyHltvReplayActive() ? "true" : "false");
		if (engine_->IsAnyHltvReplayActive())
		{
			context.Fail("a replay is still active after StopAllHltvReplays");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::StashHltvReplay", {
		// Zero seconds of replay to stash, under an id nothing else uses.
		context.Detail("-> %s", engine_->StashHltvReplay(0xC52C52u, 0.0f) ? "true" : "false");
	});
}

// -----------------------------------------------------------------------------
//  IVEngineServer2 - world groups
// -----------------------------------------------------------------------------

void Source2EngineSuite::ExerciseWorldGroups(TestContext& context)
{
	context.Note("[IVEngineServer2] world groups");

	CS2ST_CALL(context, "IVEngineServer2::FindWorldGroupByName", {
		const WorldGroupId_t id = engine_->FindWorldGroupByName("cs2sdktest_no_such_group");
		context.Detail("-> 0x%08x for a nonexistent name", id.m_nId);
		if (id != INVALID_WORLD_GROUP_ID)
		{
			context.Suspect("a nonexistent group name did not resolve to the invalid id");
		}
	});

	// Both sides, because the high bit marks client-side groups and a slot shift
	// would show up as the two answers swapping.
	CS2ST_CALL(context, "IVEngineServer2::GetFirstWorldGroupId", {
		context.Detail("-> server 0x%08x, client 0x%08x",
			engine_->GetFirstWorldGroupId(false).m_nId,
			engine_->GetFirstWorldGroupId(true).m_nId);
	});

	CS2ST_CALL(context, "IVEngineServer2::MaxWorldGroupId", {
		context.Detail("-> server 0x%08x, client 0x%08x",
			engine_->MaxWorldGroupId(false).m_nId,
			engine_->MaxWorldGroupId(true).m_nId);
	});

	CS2ST_CALL(context, "IVEngineServer2::IsWorldGroupValid", {
		const bool invalid_is_valid = engine_->IsWorldGroupValid(INVALID_WORLD_GROUP_ID);
		const WorldGroupId_t first = engine_->GetFirstWorldGroupId(false);
		context.Detail("-> invalid id: %s, first server group 0x%08x: %s",
			invalid_is_valid ? "valid" : "invalid", first.m_nId,
			engine_->IsWorldGroupValid(first) ? "valid" : "invalid");
		if (invalid_is_valid)
		{
			context.Fail("INVALID_WORLD_GROUP_ID reported valid");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::GetWorldGroupName", {
		const WorldGroupId_t first = engine_->GetFirstWorldGroupId(false);
		if (!engine_->IsWorldGroupValid(first))
		{
			// Nothing loaded yet; asking for the name of a group that does not
			// exist is an out-of-range table read, which tests nothing.
			context.SkipCall("IVEngineServer2::GetWorldGroupName",
				"no valid server world group (first id 0x%08x)", first.m_nId);
			return;
		}
		const char* name = engine_->GetWorldGroupName(first);
		context.Detail("-> \"%s\" for 0x%08x", name != nullptr ? name : "(null)", first.m_nId);
		if (name == nullptr || name[0] == '\0')
		{
			return;
		}
		// Oracle: the name must resolve back to the group it came from. Suspect,
		// not Fail: nothing in the SDK says names are unique.
		const WorldGroupId_t round_trip = engine_->FindWorldGroupByName(name);
		if (round_trip != first)
		{
			context.Suspect("FindWorldGroupByName(\"%s\") = 0x%08x, expected 0x%08x",
				name, round_trip.m_nId, first.m_nId);
		}
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

	CS2ST_CALL(context, "IVEngineServer2::GetSplitScreenPlayerAttachToEdict", {
		// The SDK reworked the split-screen trio in the Oct 2026 update: this
		// slot now returns the attached player's slot instead of an edict_t*,
		// and GetNumSplitScreenUsersAttachedToEdict is gone from the interface.
		// An unattached slot answers with an invalid slot - the value shape the
		// SDK's own CPlayerSlot API promises.
		const CPlayerSlot attached = engine_->GetSplitScreenPlayerAttachToEdict(slot);
		context.Detail("-> slot %d, valid=%s", attached.Get(),
			attached.IsValid() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetSplitScreenPlayerForEdict", {
		// The other survivor of the trio, same reworked shape: a CPlayerSlot
		// back instead of an edict_t*.
		const CPlayerSlot split =
			engine_->GetSplitScreenPlayerForEdict(slot, 0);
		context.Detail("-> slot %d, valid=%s", split.Get(),
			split.IsValid() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientConVarValue", {
		const char* value = engine_->GetClientConVarValue(slot, "name");
		context.Detail("-> name=%s", value != nullptr ? value : "(null)");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientListening", {
		context.Detail("-> %s",
			engine_->GetClientListening(slot, slot) ? "true" : "false");
	});

	// IsUserIDInUse / GetLoadingProgressForUserID were TERROR-era leftovers
	// the 2026-10 SDK removed; GetClientDeltaTick is the named resident of
	// that vtable neighbourhood now. -1 is the documented invalid-slot answer.
	CS2ST_CALL(context, "IVEngineServer2::GetClientDeltaTick", {
		context.Detail("-> %d for the empty slot (SDK documents -1 as invalid)",
			engine_->GetClientDeltaTick(slot));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientSignonState", {
		context.Detail("-> %d", static_cast<int>(engine_->GetClientSignonState(slot)));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientHltvReplayDelay", {
		context.Detail("-> %d", engine_->GetClientHltvReplayDelay(slot));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetClientUserInfo", {
		// A one-pointer handle; the data behind it is the engine's.
		const ConVarUserInfoSet_t info = engine_->GetClientUserInfo(slot);
		context.Detail("-> data=%p", static_cast<const void*>(info.m_pData));
	});

	CS2ST_CALL(context, "IVEngineServer2::GetEntityTransmitBitsForClient", {
		context.Detail("-> %p",
			static_cast<const void*>(engine_->GetEntityTransmitBitsForClient(slot)));
	});

	CS2ST_CALL(context, "IVEngineServer2::CanStartHltvReplay", {
		context.Detail("-> %s for delay 0", engine_->CanStartHltvReplay(slot, 0) ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::GetPlayerInfo", {
		// The engine fills this message with its own protobuf code and heap, so its
		// string fields are not ours to free: destroying the message here runs our
		// CRT's free on engine-owned memory and aborts on Windows (heap corruption in
		// ~CMsgPlayerInfo). Build it in static storage and never destroy it; the
		// leak is one small object for the whole run.
		alignas(CMsgPlayerInfo) static unsigned char storage[sizeof(CMsgPlayerInfo)];
		static CMsgPlayerInfo* const info_ptr = new (storage) CMsgPlayerInfo();
		CMsgPlayerInfo& info = *info_ptr;
		const bool filled = engine_->GetPlayerInfo(slot, info);
		context.Detail("-> %s name=\"%s\" userid=%d fakeplayer=%s", filled ? "true" : "false",
			info.name().c_str(), info.userid(), info.fakeplayer() ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::Message_DetermineMulticastRecipients", {
		// usepas=false skips the PVS lookup, so the origin does not matter and no
		// live level is needed; the output is a local bit vector. Pre-zeroed so a
		// call that writes nothing reads as 0 rather than as stack garbage.
		CPlayerBitVec recipients;
		std::memset(static_cast<void*>(&recipients), 0, sizeof(recipients));
		engine_->Message_DetermineMulticastRecipients(false, Vector(0.0f, 0.0f, 0.0f), recipients);
		context.Detail("-> %d recipient bit(s) set", CountSetBits(&recipients, sizeof(recipients)));
	});

	// -------------------------------------------------------------------------
	//  Slot-mutating calls. Only made on a slot that is provably empty: no net
	//  channel and no user id. The slot is in range (checked above), so the
	//  engine indexes its client table safely and finds nothing to act on.
	// -------------------------------------------------------------------------

	bool slot_empty = false;
	CS2ST_CALL(context, "IVEngineServer2::<probe slot occupancy>", {
		slot_empty = engine_->GetPlayerNetInfo(slot) == nullptr
			&& engine_->GetPlayerUserId(slot).Get() < 0;
		context.Detail("slot %d is %s", kProbeSlot, slot_empty ? "empty" : "occupied");
	});

	if (!slot_empty)
	{
		context.SkipCall("IVEngineServer2::<empty-slot mutators>",
			"slot %d is occupied; they would act on a real client", kProbeSlot);
		return;
	}

	CS2ST_CALL(context, "IVEngineServer2::ClientPrintf", {
		engine_->ClientPrintf(slot, "cs2sdktest: ClientPrintf probe\n");
		context.Detail("sent to the empty slot");
	});

	CS2ST_CALL(context, "IVEngineServer2::ClientCommand", {
		engine_->ClientCommand(slot, "echo cs2sdktest");
		context.Detail("sent to the empty slot");
	});

	CS2ST_CALL(context, "IVEngineServer2::StartHltvReplay", {
		const HltvReplayParams_t params;
		const bool started = engine_->StartHltvReplay(slot, params);
		context.Detail("-> %s for the empty slot", started ? "true" : "false");
		if (started)
		{
			context.Suspect("a replay started for a slot with no client");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::ForceStopHltvReplay", {
		engine_->ForceStopHltvReplay(slot);
		context.Detail("returned for the empty slot");
	});

	CS2ST_CALL(context, "IVEngineServer2::ResetHltvReplayRequestTime", {
		engine_->ResetHltvReplayRequestTime(slot);
		context.Detail("returned for the empty slot");
	});

	CS2ST_CALL(context, "IVEngineServer2::SetClientUpdateRate", {
		engine_->SetClientUpdateRate(slot, 64.0f);
		context.Detail("returned for the empty slot");
	});

	CS2ST_CALL(context, "IVEngineServer2::SetClientJitterBadThresholdUp", {
		engine_->SetClientJitterBadThresholdUp(slot, 100.0f);
		context.Detail("returned for the empty slot");
	});

	CS2ST_CALL(context, "IVEngineServer2::SetRecyclePlayerSlot", {
		engine_->SetRecyclePlayerSlot(slot, true);
		context.Detail("returned for the empty slot");
	});

	// Written back unchanged, so the voice routing ends where it began and the
	// re-read is the oracle.
	CS2ST_CALL(context, "IVEngineServer2::SetClientListening", {
		const bool before = engine_->GetClientListening(slot, slot);
		const bool result = engine_->SetClientListening(slot, slot, before);
		const bool after = engine_->GetClientListening(slot, slot);
		context.Detail("%s -> returned %s, read back %s", before ? "true" : "false",
			result ? "true" : "false", after ? "true" : "false");
		if (before != after)
		{
			context.Suspect("writing the current listening state back changed it");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::SetClientProximity", {
		const bool result = engine_->SetClientProximity(slot, slot, false);
		context.Detail("-> %s for the empty slot", result ? "true" : "false");
	});

	CS2ST_CALL(context, "IVEngineServer2::DisconnectClient", {
		engine_->DisconnectClient(slot, NETWORK_DISCONNECT_INVALID, "cs2sdktest");
		context.Detail("returned for the empty slot");
	});

	CS2ST_CALL(context, "IVEngineServer2::KickClient", {
		engine_->KickClient(slot, "cs2sdktest", NETWORK_DISCONNECT_INVALID);
		context.Detail("returned for the empty slot");
	});
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

	CS2ST_CALL(context, "IVEngineServer2::IsSpawnGroupLoadedForClient", {
		// An invalid slot has no client, so the engine answers false before it
		// looks at the handle.
		const bool loaded = engine_->IsSpawnGroupLoadedForClient(CPlayerSlot(-1), kUnknownSpawnGroup);
		context.Detail("-> %s for slot -1", loaded ? "true" : "false");
		if (loaded)
		{
			context.Fail("an invalid slot has no client to have loaded anything");
		}
	});

	CS2ST_CALL(context, "IVEngineServer2::GetAllSpawnGroupsWithPVS", {
		// Both vectors are outputs and local, so this is a read of engine state.
		CUtlVector<SpawnGroupHandle_t> groups;
		CUtlVector<IPVS*> pvs;
		// The engine grows both; see UtlVectorGuard for the Windows heap mismatch.
		UtlVectorGuard<decltype(groups)> groups_guard(groups);
		UtlVectorGuard<decltype(pvs)> pvs_guard(pvs);
		engine_->GetAllSpawnGroupsWithPVS(&groups, &pvs);
		context.Detail("-> %d group(s), %d PVS", groups.Count(), pvs.Count());
		// Two outputs of one call describe the same set of groups.
		if (groups.Count() != pvs.Count())
		{
			context.Suspect("%d groups but %d PVS pointers", groups.Count(), pvs.Count());
		}
	});

	context.SkipCall("IVEngineServer2::UnloadSpawnGroup", "unloads level content");
	context.SkipCall("IVEngineServer2::LoadSpawnGroup",
		"loads level content into a live spawn group; the Oct 2026 rework "
		"added the slot, and the SpawnGroupDesc_t it takes is not safe to "
		"fabricate");
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
	base.Float ("absoluteframetime_unbounded",
	                                 offsetof(CGlobalVarsBase, absoluteframetime_unbounded));
	// A client count cannot be negative - that is the type talking, not the
	// game. No upper bound here: server limits are a policy question, and
	// encoding one would make this check stop being about layout.
	base.Int32 ("maxClients",     offsetof(CGlobalVarsBase, maxClients), 0, INT32_MAX);

	// The five placeholder-named fields (renamed upstream as their meanings
	// were recovered) report the region between maxClients and
	// m_pfnWarningFunc where an inserted field would hide. Reported, never
	// judged - their types are still guesses upstream.
	base.Int32 ("m_nCurrentTickThisFrame",
	                                 offsetof(CGlobalVarsBase, m_nCurrentTickThisFrame));
	base.Int32 ("m_nTotalTicksThisFrame",
	                                 offsetof(CGlobalVarsBase, m_nTotalTicksThisFrame));
	base.Float ("m_flUsercmdTickInterval",
	                                 offsetof(CGlobalVarsBase, m_flUsercmdTickInterval));
	base.Double("m_flUsercmdTickStartTime",
	                                 offsetof(CGlobalVarsBase, m_flUsercmdTickStartTime));

	base.Pointer("m_pfnWarningFunc",
	                                 offsetof(CGlobalVarsBase, m_pfnWarningFunc));

	base.Float ("curtime",        offsetof(CGlobalVarsBase, curtime));
	base.Float ("frametime",      offsetof(CGlobalVarsBase, frametime));
	base.Float ("interpolation_amount",
	                                 offsetof(CGlobalVarsBase, interpolation_amount));
	base.Float ("m_flUsercmdTickFraction",
	                                 offsetof(CGlobalVarsBase, m_flUsercmdTickFraction));

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
	base.Int32 ("m_nClientTickCount",
	                                 offsetof(CGlobalVarsBase, m_nClientTickCount));
	base.Float ("m_flClientTime", offsetof(CGlobalVarsBase, m_flClientTime));

	base.Float ("m_flSubtickFraction",
	                                 offsetof(CGlobalVarsBase, m_flSubtickFraction));
	base.Bool  ("m_bIsOncePerFrameAsyncWorkPhase",
	                                 offsetof(CGlobalVarsBase, m_bIsOncePerFrameAsyncWorkPhase));
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
	context.Note("[IVEngineServer2] not called - no argument is known that leaves "
	             "state the later suites rely on untouched");

	// Listed rather than omitted. A reader needs to be able to tell "this was
	// checked and is fine" from "nobody ever looked at this", and an absent
	// row cannot say which. Everything whose arguments can be made harmless
	// (empty slot, written-back values, TEST-NET addresses) is called above.
	context.SkipCall("IVEngineServer2::ChangeLevel",
		"changes the map out from under the other suites");
	context.SkipCall("IVEngineServer2::ClientCommandKeyValues",
		"needs a KeyValues built with the engine's allocator; this module's heap "
		"is not the engine's (CS2SDKTEST_USE_MEMOVERRIDE is off)");
	context.SkipCall("IVEngineServer2::CreateFakeClient",
		"adds a bot that occupies a slot and a player count");
	context.SkipCall("IVEngineServer2::CreateClient",
		"creates a connectionless client whose slot stays locked (SDK: unaffected "
		"by SetRecyclePlayerSlot), so it cannot be cleaned up");
	context.SkipCall("IVEngineServer2::DisconnectAllClients", "kicks every connected player");
	context.SkipCall("IVEngineServer2::BanClient",
		"writes to the persistent ban list; both overloads");
	context.SkipCall("IVEngineServer2::SetFakeClientConVarValue",
		"acts on a bot, and no bot is created here");
	context.SkipCall("IVEngineServer2::ShowFrameTimeReport",
		"first parameter is an unidentified pointer the engine dereferences");
	context.SkipCall("IVEngineServer2::DumpNetStats",
		"first parameter is an unidentified pointer the engine dereferences");
	context.SkipCall("IVEngineServer2::StartAutoRecording", "starts a demo file on disk");
	context.SkipCall("IVEngineServer2::RecordDemo", "starts a demo file on disk");
	context.SkipCall("IVEngineServer2::StopRecordingDemo",
		"needs a CGameInfo the SDK gives no way to construct");
	context.SkipCall("IVEngineServer2::BroadcastEvent",
		"needs an INetworkMessageInternal and a matching CNetMessage; would be "
		"sent to every client");
	context.SkipCall("IVEngineServer2::SendReserveCheckResponse",
		"extends matchmaking reservation timeouts and sends packets to reserved clients");
}

} // namespace cs2st
