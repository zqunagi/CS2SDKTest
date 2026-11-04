#include "suites/ResourceSystemSuite.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#include <appframework/iappsystem.h>
#include <engine2/igameresourceservice.h>
#include <interfaces/interfaces.h>
#include <resourcesystem/iresourcesystem.h>
#include <tier0/buffer_string.h>
#include <tier0/utlstring.h>
#include <tier0/utlvector.h>

#include "core/Log.h"
#include "testkit/StructInspector.h"
#include "testkit/UtlVectorGuard.h"

namespace cs2st
{
namespace
{

// Resource types are the extension packed into a uint64 (FixupResourceName
// does the same memcpy), so "vmdl" is a type without asking the engine for one.
const char* const kProbeTypes[] = { "vmdl", "vmat", "vtex", "vpcf" };

const char* const kProbeManifestName = "CS2SDKTest";

ResourceType_t MakeType(const char* extension)
{
	ResourceType_t type = 0;
	// No std::min: windows.h defines min as a macro.
	std::size_t length = std::strlen(extension);
	if (length > sizeof(type))
	{
		length = sizeof(type);
	}
	std::memcpy(&type, extension, length);
	return type;
}

const char* Str(const char* text)
{
	return text != nullptr ? text : "(null)";
}

const char* AppSystemTierName(AppSystemTier_t tier)
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

const char* StatusName(ResourceStatus_t status)
{
	switch (status)
	{
		case RESOURCE_STATUS_UNKNOWN:                return "UNKNOWN";
		case RESOURCE_STATUS_KNOWN_BUT_NOT_RESIDENT: return "KNOWN_BUT_NOT_RESIDENT";
		case RESOURCE_STATUS_PARTIALLY_RESIDENT:     return "PARTIALLY_RESIDENT";
		case RESOURCE_STATUS_RESIDENT:               return "RESIDENT";
		default:                                     return "<out of range>";
	}
}

// Registered and removed within one call; counts whatever the engine reports
// in between, which is normally nothing.
class CountingListener : public IResourceEventListener
{
public:
	void OnResourceEvent(const char*, ResourceEvent_t, int) override
	{
		events.fetch_add(1);
	}

	std::atomic<int> events{0};
};

CountingListener g_listener;

void ForcedSynchronizationCallback(void*) {}

/**
 * The 11 IAppSystem slots every engine interface starts with. Shared by both
 * interfaces here; the label carries the interface so the two sets of rows
 * can be told apart.
 */
void ExerciseAppSystemSlots(TestContext& context, IAppSystem* app, const char* iface)
{
	char label[96];

	std::snprintf(label, sizeof(label), "%s::GetTier", iface);
	CS2ST_CALL(context, label, {
		const AppSystemTier_t tier = app->GetTier();
		const int tier_value = static_cast<int>(tier);
		context.Detail("-> %s (%d)", AppSystemTierName(tier), tier_value);
		if (tier_value < 0 || tier_value > static_cast<int>(APP_SYSTEM_TIER_OTHER))
		{
			context.Fail("tier %d is outside the enum; slot 7 is probably not GetTier", tier_value);
		}
	});

	std::snprintf(label, sizeof(label), "%s::IsSingleton", iface);
	CS2ST_CALL(context, label, {
		context.Detail("-> %s", app->IsSingleton() ? "true" : "false");
	});

	std::snprintf(label, sizeof(label), "%s::GetBuildType", iface);
	CS2ST_CALL(context, label, {
		const BuildType_t build = app->GetBuildType();
		context.Detail("-> %d", static_cast<int>(build));
		if (build != kBuildTypeRelease)
		{
			context.Fail("expected kBuildTypeRelease (%d)", static_cast<int>(kBuildTypeRelease));
		}
	});

	std::snprintf(label, sizeof(label), "%s::GetDependencies", iface);
	CS2ST_CALL(context, label, {
		const AppSystemInfo_t* deps = app->GetDependencies();
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

	std::snprintf(label, sizeof(label), "%s::QueryInterface (unknown name)", iface);
	CS2ST_CALL(context, label, {
		void* none = app->QueryInterface("CS2SDKTestNoSuchInterface001");
		context.Detail("-> %p", none);
		if (none != nullptr)
		{
			context.Suspect("an interface name nobody registered resolved to a pointer");
		}
	});

	static const char* const kSkipped[][2] = {
		{ "Connect",     "would re-run engine startup" },
		{ "Init",        "would re-run engine startup" },
		{ "Reconnect",   "would rebind engine interfaces" },
		{ "PreShutdown", "would begin engine shutdown" },
		{ "Shutdown",    "would shut the engine down" },
		{ "Disconnect",  "would tear down engine interfaces" },
	};
	for (const auto& skipped : kSkipped)
	{
		context.SkipCall(std::string(iface) + "::" + skipped[0], "%s", skipped[1]);
	}
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool ResourceSystemSuite::Acquire(TestContext& context)
{
	resource_system_ = static_cast<IResourceSystem*>(
		context.QueryInterface("resourcesystem", RESOURCESYSTEM_INTERFACE_VERSION));
	game_service_ = static_cast<IGameResourceService*>(
		context.QueryInterface("engine2", GAMERESOURCESERVICESERVER_INTERFACE_VERSION));

	if (resource_system_ == nullptr)
	{
		if (!context.GetModule("resourcesystem").IsValid())
		{
			context.UnavailableCall("IResourceSystem", "the resourcesystem module is not loaded");
		}
		else
		{
			context.UnavailableCall("IResourceSystem",
				"resourcesystem is loaded but does not answer '%s'", RESOURCESYSTEM_INTERFACE_VERSION);
		}
	}
	else
	{
		context.Note("IResourceSystem at %p (%s)", static_cast<void*>(resource_system_),
			RESOURCESYSTEM_INTERFACE_VERSION);
	}

	if (game_service_ == nullptr)
	{
		if (!context.GetModule("engine2").IsValid())
		{
			context.UnavailableCall("IGameResourceService", "the engine2 module is not loaded");
		}
		else
		{
			context.UnavailableCall("IGameResourceService",
				"engine2 is loaded but does not answer '%s'", GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
		}
	}
	else
	{
		context.Note("IGameResourceService at %p (%s)", static_cast<void*>(game_service_),
			GAMERESOURCESERVICESERVER_INTERFACE_VERSION);
	}

	return resource_system_ != nullptr || game_service_ != nullptr;
}

void ResourceSystemSuite::ExerciseCalls(TestContext& context)
{
	sample_count_ = 0;

	if (resource_system_ != nullptr)
	{
		context.Note("[IAppSystem] base slots of IResourceSystem - if these are wrong, nothing below means anything");
		ExerciseAppSystemSlots(context, resource_system_, "IResourceSystem");
		ExerciseTypes(context);
		ExerciseEnumeration(context);
		ExerciseHandles(context);
		ExerciseState(context);
		ExerciseResourceSkips(context);
		InspectBinding(context);
	}

	if (game_service_ != nullptr)
	{
		context.Note("[IAppSystem] base slots of IGameResourceService");
		ExerciseAppSystemSlots(context, game_service_, "IGameResourceService");
		ExerciseServiceBase(context);
		ExerciseManifestRoundTrip(context);
		ExerciseServiceSkips(context);
		InspectService(context);
	}
}

// -----------------------------------------------------------------------------
//  IResourceSystem - type tables
// -----------------------------------------------------------------------------

void ResourceSystemSuite::ExerciseTypes(TestContext& context)
{
	context.Note("[IResourceSystem] type tables - a type is its extension packed into a uint64");

	for (const char* extension : kProbeTypes)
	{
		char label[128];
		std::snprintf(label, sizeof(label),
			"IResourceSystem::GetTypeManager / GetErrorResource / GetResourceTypeName [%s]", extension);

		CS2ST_CALL(context, label, {
			const ResourceType_t type = MakeType(extension);
			IResourceTypeManager* manager = resource_system_->GetTypeManager(type);
			ResourceHandle_t error_resource = resource_system_->GetErrorResource(type);
			const char* type_name = resource_system_->GetResourceTypeName(type);
			context.Detail("-> manager %p, error resource %p, type name \"%s\"",
				static_cast<void*>(manager), static_cast<const void*>(error_resource), Str(type_name));

			// The name is the engine's enumerator-style label (RESOURCE_TYPE_MODEL for
			// "vmdl"), not the extension the type is packed from.
			if (type_name != nullptr && std::strncmp(type_name, "RESOURCE_TYPE_", 14) != 0)
			{
				context.Suspect("GetResourceTypeName returned \"%s\" for \"%s\"; expected a RESOURCE_TYPE_ label",
					type_name, extension);
			}
			if (error_resource != nullptr)
			{
				const ResourceType_t error_type = resource_system_->GetResourceType(error_resource);
				if (error_type != type)
				{
					context.Suspect("GetResourceType of the error resource is 0x%llx, not the type asked for (0x%llx)",
						static_cast<unsigned long long>(error_type), static_cast<unsigned long long>(type));
				}
			}
		});
	}

	// Oracle: a type no file ever uses is not installed.
	CS2ST_CALL(context, "IResourceSystem::GetTypeManager (unknown type)", {
		IResourceTypeManager* none = resource_system_->GetTypeManager(MakeType("zzzz"));
		context.Detail("-> %p", static_cast<void*>(none));
		if (none != nullptr)
		{
			context.Suspect("a type nobody installed resolved to a manager");
		}
	});
}

// -----------------------------------------------------------------------------
//  IResourceSystem - enumeration
// -----------------------------------------------------------------------------

void ResourceSystemSuite::ExerciseEnumeration(TestContext& context)
{
	context.Note("[IResourceSystem] enumeration - scans are bounded and run under the call's fault guard");

	int all_count = -1;

	CS2ST_CALL(context, "IResourceSystem::GetAllResources", {
		CUtlVector<ResourceHandle_t> all;
		UtlVectorGuard<decltype(all)> all_guard(all);
		resource_system_->GetAllResources(all, RESOURCE_SYSTEM_INCLUDE_ALL_RESOURCES);
		all_count = all.Count();
		context.Detail("-> %d resource(s) with every flag", all_count);
		if (all_count < 0)
		{
			context.Fail("resource count is %d", all_count);
			return;
		}

		int anonymous = 0;
		int null_handles = 0;
		const int scan = all_count < 4096 ? all_count : 4096;
		for (int i = 0; i < scan; ++i)
		{
			ResourceHandle_t handle = all[i];
			if (handle == nullptr)
			{
				++null_handles;
				continue;
			}
			if ((handle->m_nFlags & RESOURCE_BINDING_ANONYMOUS) != 0)
			{
				++anonymous;
				continue;
			}
			if (sample_count_ < kMaxSamples)
			{
				samples_[sample_count_++] = handle;
			}
		}
		context.Detail("first %d: %d anonymous, %d null, %d named sample(s) kept", scan, anonymous,
			null_handles, sample_count_);
		if (null_handles != 0)
		{
			context.Suspect("%d null handle(s) in the list", null_handles);
		}
	});

	CS2ST_CALL(context, "IResourceSystem::GetAllResources (named and anonymous, no pending)", {
		CUtlVector<ResourceHandle_t> settled;
		UtlVectorGuard<decltype(settled)> settled_guard(settled);
		resource_system_->GetAllResources(settled, RESOURCE_SYSTEM_INCLUDE_NAMED_AND_ANONYMOUS_RESOURCES);
		context.Detail("-> %d resource(s)", settled.Count());
		if (all_count >= 0 && settled.Count() > all_count)
		{
			context.Suspect("a flag subset (%d) is larger than the full set (%d)", settled.Count(), all_count);
		}
	});

	for (const char* extension : kProbeTypes)
	{
		char label[128];
		std::snprintf(label, sizeof(label),
			"IResourceSystem::GetAllResourcesByType / GetAllNamedResourcesByType [%s]", extension);

		CS2ST_CALL(context, label, {
			const ResourceType_t type = MakeType(extension);
			CUtlVector<ResourceHandle_t> by_type;
			UtlVectorGuard<decltype(by_type)> by_type_guard(by_type);
			CUtlVector<ResourceHandle_t> named_by_type;
			UtlVectorGuard<decltype(named_by_type)> named_by_type_guard(named_by_type);
			resource_system_->GetAllResourcesByType(type, by_type, RESOURCE_SYSTEM_INCLUDE_ALL_RESOURCES);
			resource_system_->GetAllNamedResourcesByType(type, named_by_type, RESOURCE_SYSTEM_INCLUDE_ALL_RESOURCES);
			context.Detail("-> %d of the type, %d named", by_type.Count(), named_by_type.Count());
			if (all_count >= 0 && by_type.Count() > all_count)
			{
				context.Suspect("one type (%d) has more resources than exist (%d)", by_type.Count(), all_count);
			}

			// Oracle: the type of every handle in the list is the type asked for.
			const int scan = by_type.Count() < 16 ? by_type.Count() : 16;
			for (int i = 0; i < scan; ++i)
			{
				const ResourceType_t found = resource_system_->GetResourceType(by_type[i]);
				if (found != type)
				{
					context.Suspect("handle %p in the list for \"%s\" reports type 0x%llx",
						static_cast<const void*>(by_type[i]), extension, static_cast<unsigned long long>(found));
					break;
				}
			}
		});
	}

	CS2ST_CALL(context, "IResourceSystem::GetJustInTimeManifestCount / GetJustInTimeManifestResources", {
		const int manifests = resource_system_->GetJustInTimeManifestCount();
		CUtlVector<ResourceHandle_t> jit;
		UtlVectorGuard<decltype(jit)> jit_guard(jit);
		resource_system_->GetJustInTimeManifestResources(jit);
		context.Detail("-> %d manifest(s), %d resource(s)", manifests, jit.Count());
		if (manifests < 0 || jit.Count() < 0)
		{
			context.Fail("negative count");
		}
		else if (manifests == 0 && jit.Count() != 0)
		{
			context.Suspect("no just-in-time manifest, yet %d resource(s) listed", jit.Count());
		}
	});
}

// -----------------------------------------------------------------------------
//  IResourceSystem - per-handle oracles
// -----------------------------------------------------------------------------

void ResourceSystemSuite::ExerciseHandles(TestContext& context)
{
	if (sample_count_ == 0)
	{
		context.Note("[IResourceSystem] no named resource is loaded, per-handle calls have nothing to read");
		context.SkipCall("IResourceSystem::GetResourceName / ResourceHandleToResourceId / FindResourceById",
			"no named resource handle available");
		context.SkipCall("IResourceSystem::GetResourceStatus / GetActualFileName / FindOrRegisterResourceByName",
			"no named resource handle available");
		return;
	}

	context.Note("[IResourceSystem] per-handle calls on %d sampled named resource(s); each answer is checked "
		"against a second path to the same fact", sample_count_);

	for (int index = 0; index < sample_count_; ++index)
	{
		char label[128];
		const ResourceHandle_t handle = samples_[index];

		std::snprintf(label, sizeof(label),
			"IResourceSystem::GetResourceName (both overloads) / GetResourceType [sample %d]", index);
		CS2ST_CALL(context, label, {
			CResourceName resource_name;
			CBufferStringN<260> buffer;
			resource_system_->GetResourceName(handle, &resource_name);
			resource_system_->GetResourceName(handle, &buffer, false);
			const ResourceType_t type = resource_system_->GetResourceType(handle);
			const char* type_name = resource_system_->GetResourceTypeName(type);
			context.Detail("-> \"%s\", buffer \"%s\", type \"%s\"", Str(resource_name.GetName()),
				buffer.Get(), Str(type_name));

			if (std::strcmp(resource_name.GetName(), buffer.Get()) != 0)
			{
				context.Suspect("the CResourceName and CBufferString overloads disagree");
			}
			if (resource_name.IsEmpty())
			{
				context.Suspect("a named resource reports an empty name");
			}
		});

		std::snprintf(label, sizeof(label),
			"IResourceSystem::ResourceHandleToResourceId / FindResourceById [sample %d]", index);
		CS2ST_CALL(context, label, {
			const ResourceId_t id = resource_system_->ResourceHandleToResourceId(handle);
			const ResourceType_t type = resource_system_->GetResourceType(handle);
			ResourceHandle_t found = resource_system_->FindResourceById(id, type);
			context.Detail("-> id 0x%llx, FindResourceById %p (handle %p)",
				static_cast<unsigned long long>(id.m_Value), static_cast<const void*>(found),
				static_cast<const void*>(handle));

			// Two descriptions of the same id: the virtual, and the field the binding declares.
			if (id.m_Value != handle->m_ResourceId.m_Value)
			{
				context.Fail("ResourceHandleToResourceId (0x%llx) and ResourceBindingBase_t::m_ResourceId (0x%llx) "
					"differ", static_cast<unsigned long long>(id.m_Value),
					static_cast<unsigned long long>(handle->m_ResourceId.m_Value));
			}
			if (found != handle)
			{
				context.Suspect("looking the handle up again by its own id returned another handle");
			}
		});

		std::snprintf(label, sizeof(label),
			"IResourceSystem::GetResourceStatus (name / id / handle) [sample %d]", index);
		CS2ST_CALL(context, label, {
			CResourceName resource_name;
			resource_system_->GetResourceName(handle, &resource_name);
			const ResourceStatus_t by_name = resource_system_->GetResourceStatus(resource_name);
			const ResourceStatus_t by_id = resource_system_->GetResourceStatus(resource_system_->ResourceHandleToResourceId(handle));
			const ResourceStatus_t by_handle = resource_system_->GetResourceStatus(handle);
			context.Detail("-> name %s, id %s, handle %s", StatusName(by_name), StatusName(by_id),
				StatusName(by_handle));
			if (by_handle < RESOURCE_STATUS_UNKNOWN || by_handle > RESOURCE_STATUS_RESIDENT)
			{
				context.Fail("status %d is outside the enum", static_cast<int>(by_handle));
			}
			if (by_name != by_handle || by_id != by_handle)
			{
				context.Suspect("the three GetResourceStatus overloads disagree");
			}
		});

		std::snprintf(label, sizeof(label),
			"IResourceSystem::GetActualFileName / FindOrRegisterResourceByName [sample %d]", index);
		CS2ST_CALL(context, label, {
			CBufferStringN<260> file_name;
			resource_system_->GetActualFileName(handle, &file_name, false);

			// A name that is already bound resolves to its binding, it does not make a new one.
			CResourceName resource_name;
			resource_system_->GetResourceName(handle, &resource_name);
			ResourceHandle_t again = resource_system_->FindOrRegisterResourceByName(resource_name, false);
			context.Detail("-> file \"%s\", FindOrRegisterResourceByName %p (handle %p)", file_name.Get(),
				static_cast<const void*>(again), static_cast<const void*>(handle));
			if (again != handle)
			{
				context.Suspect("an already-bound name resolved to another handle");
			}
		});
	}
}

// -----------------------------------------------------------------------------
//  IResourceSystem - state, lock, listeners
// -----------------------------------------------------------------------------

void ResourceSystemSuite::ExerciseState(TestContext& context)
{
	context.Note("[IResourceSystem] state queries and reversible writes");

	CS2ST_CALL(context, "IResourceSystem::HasPendingWork / HasCompletedIORequests", {
		context.Detail("-> pending work %s, completed IO requests %s",
			resource_system_->HasPendingWork() ? "true" : "false",
			resource_system_->HasCompletedIORequests() ? "true" : "false");
	});

	CS2ST_CALL(context, "IResourceSystem::IsInFrameUpdate", {
		const bool in_update = resource_system_->IsInFrameUpdate();
		context.Detail("-> %s", in_update ? "true" : "false");
		if (in_update)
		{
			context.Suspect("a call from the plugin's own thread reports the resource system mid frame update");
		}
	});

	CS2ST_CALL(context, "IResourceSystem::IsShuttingDown", {
		const bool shutting_down = resource_system_->IsShuttingDown();
		context.Detail("-> %s", shutting_down ? "true" : "false");
		if (shutting_down)
		{
			context.Suspect("the resource system reports shutting down while the server is running");
		}
	});

	CS2ST_CALL(context, "IResourceSystem::GetAsyncFileRequestPriority", {
		for (int priority = RESOURCE_MANIFEST_LOAD_PRIORITY_LOW; priority < RESOURCE_MANIFEST_LOAD_PRIORITY_COUNT; ++priority)
		{
			const AsyncRequestPriority_t mapped = resource_system_->GetAsyncFileRequestPriority(
				static_cast<ResourceManifestLoadPriority_t>(priority));
			context.Detail("load priority %d -> async request priority %d", priority, static_cast<int>(mapped));
		}
	});

	CS2ST_CALL(context, "IResourceSystem::GetCodeResourceManifestManager / GetProfiler / GetLeakTracker / GetGenericDataTypeManager", {
		context.Detail("-> code manifest manager %p, profiler %p, leak tracker %p, generic data type manager %p",
			static_cast<void*>(resource_system_->GetCodeResourceManifestManager()),
			static_cast<void*>(resource_system_->GetProfiler()),
			static_cast<void*>(resource_system_->GetLeakTracker()),
			static_cast<void*>(resource_system_->GetGenericDataTypeManager()));
	});

	// Writing back the value just read leaves every mode where it was.
	CS2ST_CALL(context, "IResourceSystem::IsDebugModeEnabled / EnableDebugMode (same value)", {
		for (int mode = 0; mode < RESOURCE_SYSTEM_DEBUG_MODE_COUNT; ++mode)
		{
			const ResourceSystemDebugMode_t debug_mode = static_cast<ResourceSystemDebugMode_t>(mode);
			const bool before = resource_system_->IsDebugModeEnabled(debug_mode);
			resource_system_->EnableDebugMode(debug_mode, before);
			const bool after = resource_system_->IsDebugModeEnabled(debug_mode);
			context.Detail("mode %d: %s, after writing it back: %s", mode, before ? "on" : "off",
				after ? "on" : "off");
			if (before != after)
			{
				context.Suspect("mode %d changed when its own value was written back", mode);
			}
		}
	});

	CS2ST_CALL(context, "IResourceSystem::Lock / Unlock / IsLockedByCurrentThread", {
		const bool before = resource_system_->IsLockedByCurrentThread();
		resource_system_->Lock();
		const bool held = resource_system_->IsLockedByCurrentThread();
		resource_system_->Unlock();
		const bool after = resource_system_->IsLockedByCurrentThread();
		context.Detail("-> before %s, held %s, after %s", before ? "true" : "false",
			held ? "true" : "false", after ? "true" : "false");
		if (!held)
		{
			context.Suspect("IsLockedByCurrentThread is false right after Lock");
		}
		if (before != after)
		{
			context.Suspect("Lock then Unlock did not restore the lock state");
		}
	});

	CS2ST_CALL(context, "IResourceSystem::RegisterForcedSynchronizationCallback / UnregisterForcedSynchronizationCallback", {
		resource_system_->RegisterForcedSynchronizationCallback(&ForcedSynchronizationCallback, nullptr);
		resource_system_->UnregisterForcedSynchronizationCallback(&ForcedSynchronizationCallback, nullptr);
		context.Detail("-> registered and removed a no-op callback");
	});

	CS2ST_CALL(context, "IResourceSystem::RegisterResourceEventListener / UnregisterResourceEventListener", {
		g_listener.events.store(0);
		resource_system_->RegisterResourceEventListener(&g_listener);
		resource_system_->UnregisterResourceEventListener(&g_listener);
		context.Detail("-> registered and removed a counting listener, %d event(s) seen in between",
			g_listener.events.load());
	});
}

void ResourceSystemSuite::ExerciseResourceSkips(TestContext& context)
{
	context.Note("[IResourceSystem] the rest either changes what the server has loaded, blocks the "
		"frame, or needs an object this plugin cannot build safely");

	context.SkipCall("IResourceSystem::InstallTypeManager",
		"installs a type manager into the live resource system");
	context.SkipCall("IResourceSystem::InstallNullTypeManager",
		"registers a type in the live resource system");
	context.SkipCall("IResourceSystem::Update",
		"pumps loading and finalization from a thread that does not own the frame update");
	context.SkipCall("IResourceSystem::UpdateSimple",
		"pumps loading and finalization from a thread that does not own the frame update");
	context.SkipCall("IResourceSystem::RemoveNullTypeManager",
		"removes a type from the live resource system");
	context.SkipCall("IResourceSystem::RemoveResourceTypeManager",
		"removes an engine type manager");
	context.SkipCall("IResourceSystem::LoadResourceManifestFile",
		"queues a manifest load, and a name that exists would stay loaded");
	context.SkipCall("IResourceSystem::LoadResourceManifestGroup",
		"queues a manifest load, and a group that exists would stay loaded");
	context.SkipCall("IResourceSystem::LoadResourceManifestNamed",
		"queues a manifest load, and a name that exists would stay loaded");
	context.SkipCall("IResourceSystem::CreateResourceManifest",
		"creates a manifest that keeps its resources loaded; no resource is known to be safe to load");
	context.SkipCall("IResourceSystem::SetManifestCompletionCallback",
		"needs a manifest handle, and creating one loads content");
	context.SkipCall("IResourceSystem::IsManifestLoaded",
		"needs a manifest handle, and creating one loads content");
	context.SkipCall("IResourceSystem::DestroyResourceManifest",
		"needs a manifest handle, and creating one loads content");
	context.SkipCall("IResourceSystem::GetResourceManifestDebugName",
		"needs a manifest handle, and creating one loads content");
	context.SkipCall("IResourceSystem::ForceSynchronizationAndBlockUntilManifestLoaded",
		"blocks the frame until a manifest loads");
	context.SkipCall("IResourceSystem::FindOrCreateProceduralResource",
		"creates a resource binding that is never freed");
	context.SkipCall("IResourceSystem::MarkErrorResourcesReloaded",
		"tells every user of an error resource it was reloaded");
	context.SkipCall("IResourceSystem::BlockingLoadResourceByName",
		"loads a resource and blocks the frame");
	context.SkipCall("IResourceSystem::BlockingLoadResourceByNameIntoJustInTimeManifest",
		"loads a resource, blocks the frame, and makes the engine load a spawn group for it");
	context.SkipCall("IResourceSystem::FreeJustInTimeManifests",
		"releases engine-owned manifests the engine then unloads a spawn group for");
	context.SkipCall("IResourceSystem::InstallResourceUpdater",
		"needs an IResourceUpdater implementation this plugin does not have");
	context.SkipCall("IResourceSystem::UninstallResourceUpdater",
		"needs an IResourceUpdater implementation this plugin does not have");
	context.SkipCall("IResourceSystem::GetResourcesNamesInManifest",
		"needs a manifest handle, and creating one loads content");
	context.SkipCall("IResourceSystem::GetResourcesInManifest",
		"needs a manifest handle, and creating one loads content");
	context.SkipCall("IResourceSystem::RegisterToolsResourceListener",
		"ignored outside tools mode, and IToolsResourceListener has no implementation here");
	context.SkipCall("IResourceSystem::UnregisterToolsResourceListener",
		"ignored outside tools mode, and IToolsResourceListener has no implementation here");
	context.SkipCall("IResourceSystem::SetResourceTypeManifestPriority",
		"changes the load order of a type in the live resource system");
	context.SkipCall("IResourceSystem::BlockingFinishAllCurrentlyLoadingManifests",
		"blocks the frame until every loading manifest is done");
	context.SkipCall("IResourceSystem::RegisterToolsResourcePreReloadListener",
		"ignored outside tools mode, and IToolsResourcePreReloadListener has no implementation here");
	context.SkipCall("IResourceSystem::UnregisterToolsResourcePreReloadListener",
		"ignored outside tools mode, and IToolsResourcePreReloadListener has no implementation here");
	context.SkipCall("IResourceSystem::InstallTestFilesystem",
		"swaps the file system the resource system reads from");
	context.SkipCall("IResourceSystem::UninstallTestFilesystem",
		"swaps the file system the resource system reads from");
	context.SkipCall("IResourceSystem::QueueFrameUpdateCallback",
		"the callback would run on a later frame and could outlive an unload of this plugin");
}

// -----------------------------------------------------------------------------
//  ResourceBindingBase_t - the object a ResourceHandle_t points at
// -----------------------------------------------------------------------------

void ResourceSystemSuite::InspectBinding(TestContext& context)
{
	if (sample_count_ == 0)
	{
		context.SkipCall("ResourceBindingBase_t layout", "no named resource handle available");
		return;
	}

	const ResourceBindingBase_t* binding = samples_[0];
	StructInspector inspect(context, "ResourceBindingBase_t", binding, sizeof(ResourceBindingBase_t));
	inspect.Pointer("m_pData",              offsetof(ResourceBindingBase_t, m_pData));
	inspect.Pointer("m_Name",               offsetof(ResourceBindingBase_t, m_Name));
	inspect.UInt64 ("m_ResourceId",         offsetof(ResourceBindingBase_t, m_ResourceId));
	inspect.UInt16 ("m_nFlags",             offsetof(ResourceBindingBase_t, m_nFlags));
	inspect.UInt16 ("m_nStreamingResource", offsetof(ResourceBindingBase_t, m_nStreamingResource));
	inspect.Raw    ("m_nResourceType",      offsetof(ResourceBindingBase_t, m_nResourceType), sizeof(ResourceTypeIndex_t));
	inspect.UInt8  ("m_nReloadCounter",     offsetof(ResourceBindingBase_t, m_nReloadCounter));
	inspect.UInt16 ("m_nLoadingResource",   offsetof(ResourceBindingBase_t, m_nLoadingResource));
	inspect.Int32  ("m_nRefCount",          offsetof(ResourceBindingBase_t, m_nRefCount));
	inspect.UInt32 ("m_nExtRefHandle",      offsetof(ResourceBindingBase_t, m_nExtRefHandle));
	inspect.Summarize();
	inspect.HexDump();
}

// -----------------------------------------------------------------------------
//  IGameResourceService - IEngineService slots
// -----------------------------------------------------------------------------

void ResourceSystemSuite::ExerciseServiceBase(TestContext& context)
{
	context.Note("[IEngineService] the service half of IGameResourceService");

	CS2ST_CALL(context, "IEngineService::GetName / IsActive / GetServiceIndex / GetServiceDependencies", {
		const char* name = game_service_->GetName();
		context.Detail("-> name \"%s\", active %s, index %u, dependencies %p", Str(name),
			game_service_->IsActive() ? "true" : "false",
			static_cast<unsigned>(game_service_->GetServiceIndex()), game_service_->GetServiceDependencies());
		if (name == nullptr)
		{
			context.Fail("a service reports no name");
		}
	});

	CS2ST_CALL(context, "IEngineService::ShouldActivate (unknown loop)", {
		const EngineServiceActivateType_t type = game_service_->ShouldActivate("CS2SDKTestNoSuchLoop");
		context.Detail("-> %d", static_cast<int>(type));
		if (type < ENGINE_SERVICE_ACTIVATE_NEVER || type > ENGINE_SERVICE_ACTIVATE_IF_ADDED_BY_LOOP_MODE)
		{
			context.Fail("value %d is outside EngineServiceActivateType_t", static_cast<int>(type));
		}
	});

	context.SkipCall("IEngineService::OnLoopActivate", "would run the service's loop activation on the live server");
	context.SkipCall("IEngineService::OnLoopDeactivate", "would run the service's loop deactivation on the live server");
	context.SkipCall("IEngineService::SetActive", "toggles whether the engine drives the service");
	context.SkipCall("IEngineService::SetName", "the registry keys services on this name");
	context.SkipCall("IEngineService::RegisterEventMap", "would register the service's event handlers a second time");
	context.SkipCall("IEngineService::SetServiceIndex", "the engine indexes its service table with this");
	context.SkipCall("IEngineService::~IEngineService", "destroying the engine's service object");
}

// -----------------------------------------------------------------------------
//  IGameResourceService - an empty manifest, allocated and destroyed
// -----------------------------------------------------------------------------

void ResourceSystemSuite::ExerciseManifestRoundTrip(TestContext& context)
{
	context.Note("[IGameResourceService] one empty manifest, allocated and destroyed within this block; "
		"nothing is loaded into it");

	HGameResourceManifest manifest = nullptr;

	CS2ST_CALL(context, "IGameResourceService::AllocGameResourceManifest", {
		manifest = game_service_->AllocGameResourceManifest(RESOURCE_MANIFEST_LOAD_DEFAULT,
			kProbeManifestName, RESOURCE_MANIFEST_LOAD_PRIORITY_DEFAULT);
		context.Detail("-> %p", manifest);
		if (manifest == nullptr)
		{
			context.Suspect("no manifest was allocated");
		}
	});

	if (manifest == nullptr)
	{
		context.SkipCall("IGameResourceService::GetResourceManifestDebugName / IsManifestLoaded / "
			"DoesManifestHaveFutureDependentResources / GetManifestResourceNames / DestroyResourceManifest",
			"AllocGameResourceManifest returned no manifest");
		return;
	}

	CS2ST_CALL(context, "IGameResourceService::GetResourceManifestDebugName", {
		const char* debug_name = game_service_->GetResourceManifestDebugName(manifest);
		context.Detail("-> \"%s\"", Str(debug_name));
		if (debug_name == nullptr || std::strcmp(debug_name, kProbeManifestName) != 0)
		{
			context.Suspect("the manifest was allocated as \"%s\" but reports \"%s\"", kProbeManifestName,
				Str(debug_name));
		}
	});

	CS2ST_CALL(context, "IGameResourceService::IsManifestLoaded / DoesManifestHaveFutureDependentResources", {
		context.Detail("-> loaded %s, has future dependent resources %s",
			game_service_->IsManifestLoaded(manifest) ? "true" : "false",
			game_service_->DoesManifestHaveFutureDependentResources(manifest) ? "true" : "false");
	});

	CS2ST_CALL(context, "IGameResourceService::GetManifestResourceNames", {
		CUtlVector<CUtlString> names;
		UtlVectorGuard<decltype(names)> names_guard(names);
		game_service_->GetManifestResourceNames(manifest, names);
		context.Detail("-> %d name(s)", names.Count());
		if (names.Count() != 0)
		{
			context.Suspect("a manifest nothing was added to lists %d resource(s)", names.Count());
		}
	});

	CS2ST_CALL(context, "IGameResourceService::DestroyResourceManifest", {
		game_service_->DestroyResourceManifest(manifest);
		context.Detail("-> destroyed %p", manifest);
	});
}

void ResourceSystemSuite::ExerciseServiceSkips(TestContext& context)
{
	context.Note("[IGameResourceService] the rest loads content, blocks, or needs an engine type");

	context.SkipCall("IGameResourceService::~IGameResourceService",
		"destroying the engine's service object");
	context.SkipCall("IGameResourceService::LoadGameResourceManifestNamed",
		"queues a manifest load; a name that exists would stay loaded");
	context.SkipCall("IGameResourceService::LoadGameResourceManifestGroup",
		"queues a manifest load; a group that exists would stay loaded");
	context.SkipCall("IGameResourceService::LoadGameResourceManifest",
		"queues a manifest load (all overloads); a resource that exists would stay loaded");
	context.SkipCall("IGameResourceService::SetManifestCompletionCallback",
		"installs a callback the engine may call after an unload of this plugin");
	context.SkipCall("IGameResourceService::BlockUntilManifestLoaded",
		"blocks the frame until a manifest loads");
	context.SkipCall("IGameResourceService::SetEntityResourceManifestHandler",
		"replaces the handler the engine builds entity manifests with");
	context.SkipCall("IGameResourceService::PrecacheEntitiesAndConfirmResourcesAreLoaded",
		"needs a spawn group and entity spawn info, and loads their resources");
	context.SkipCall("IGameResourceService::DescribeContents",
		"writes to a logging channel and needs a channel id this plugin does not own");
	context.SkipCall("IGameResourceService::CreateGameResourceManifest",
		"needs a CCompressedResourceManifest, which has no definition here");
	context.SkipCall("IGameResourceService::AppendToGameResourceManifest",
		"needs a CCompressedResourceManifest, which has no definition here");
	context.SkipCall("IGameResourceService::BuildCompressedManifest",
		"needs a CCompressedResourceManifest, which has no definition here");
	context.SkipCall("IGameResourceService::LockGameResourceManifest",
		"needs a CGameResourceManifestLock, which has no definition here");
	context.SkipCall("IGameResourceService::AppendToAndCreateGameResourceManifest",
		"needs a spawn group and entity spawn info, and loads their resources");
}

// -----------------------------------------------------------------------------
//  CBaseEngineService<IGameResourceService> / CGameResourceService layout
// -----------------------------------------------------------------------------

void ResourceSystemSuite::InspectService(TestContext& context)
{
	// The interface pointer is the CGameResourceService: IGameResourceService is its first base.
	// GameEntitySystemSuite and GamedataSuite only read m_pEntityResourceManifestHandler by offset.
	const CGameResourceService* service = static_cast<const CGameResourceService*>(game_service_);
	using BaseService = CBaseEngineService<IGameResourceService>;
	const BaseService* base = static_cast<const BaseService*>(service);

	StructInspector base_inspect(context, "CBaseEngineService<IGameResourceService>", base, sizeof(BaseService));
	base_inspect.Pointer("__vftable",       0);
	base_inspect.String ("m_pszName",       offsetof(BaseService, m_pszName));
	base_inspect.Bool   ("m_bActive",       offsetof(BaseService, m_bActive));
	base_inspect.UInt16 ("m_nServiceIndex", offsetof(BaseService, m_nServiceIndex));
	base_inspect.Summarize();

	StructInspector inspect(context, "CGameResourceService", service, sizeof(CGameResourceService));
	inspect.Raw    ("m_unk001",                         offsetof(CGameResourceService, m_unk001), sizeof(bool));
	inspect.Bool   ("m_bIsServer",                      offsetof(CGameResourceService, m_bIsServer));
	inspect.Pointer("m_pEntityResourceManifestHandler", offsetof(CGameResourceService, m_pEntityResourceManifestHandler));
	inspect.Summarize();
	inspect.HexDump();

	// Oracles: each field next to the virtual that reports it.
	CS2ST_CALL(context, "CBaseEngineService fields against IEngineService getters", {
		if (base->m_pszName != game_service_->GetName())
		{
			context.Fail("m_pszName (%s) is not what GetName returns (%s)", Str(base->m_pszName),
				Str(game_service_->GetName()));
		}
		if (base->m_bActive != game_service_->IsActive())
		{
			context.Fail("m_bActive (%d) is not what IsActive returns (%d)", base->m_bActive ? 1 : 0,
				game_service_->IsActive() ? 1 : 0);
		}
		if (base->m_nServiceIndex != game_service_->GetServiceIndex())
		{
			context.Fail("m_nServiceIndex (%u) is not what GetServiceIndex returns (%u)",
				static_cast<unsigned>(base->m_nServiceIndex), static_cast<unsigned>(game_service_->GetServiceIndex()));
		}
		context.Detail("m_pszName, m_bActive, m_nServiceIndex checked against GetName, IsActive, GetServiceIndex");
	});

	CS2ST_CALL(context, "CGameResourceService::m_bIsServer / m_unk101", {
		context.Detail("m_bIsServer %d, m_unk101 holds %d pointer(s)", service->m_bIsServer ? 1 : 0,
			service->m_unk101.Count());
		if (!service->m_bIsServer)
		{
			context.Suspect("GameResourceServiceServerV001 reports m_bIsServer false");
		}
		if (service->m_unk101.Count() < 0)
		{
			context.Fail("m_unk101 count is %d", service->m_unk101.Count());
		}
	});
}

} // namespace cs2st
