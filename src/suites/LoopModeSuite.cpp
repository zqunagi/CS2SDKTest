#include "suites/LoopModeSuite.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <engine2/engineevents.h>
#include <engine2/iengineservicemgr.h>
#include <inputsystem/inputenums.h>
#include <interfaces/interfaces.h>
#include <rendersystem/rendersystemtypes.h>

#include "core/Platform.h"
#include "core/SignatureScan.h"

namespace cs2st
{
namespace
{

// CLoopModeRegistry::RegisterLoopModes in the 2026-11 Linux libserver.so. It
// walks the registration list and calls IEngineServiceMgr slot 13
// (RegisterLoopMode) for each node. The bytes up to and including the call are
// 54 long; the two ?? runs are the RIP-relative displacements of the list head
// (at +10) and of the engine service manager global (at +22).
const char kRegistryPattern[] =
	"55 48 89 E5 41 54 53 48 8B 1D ? ? ? ? 48 85 DB 74 2B 4C 8D 25 ? ? ? ? "
	"66 0F 1F 44 00 00 49 8B 3C 24 48 8B 4B 18 48 8B 53 10 48 8B 73 08 48 8B "
	"07 FF 50 68";

// `mov rbx, [rip+disp32]`: opcode bytes at +7, displacement at +10, the next
// instruction at +14.
constexpr std::size_t kHeadDispOffset = 10;
constexpr std::size_t kHeadNextInstruction = 14;

// A node of the registration list, as the loop at +0x1d..+0x39 reads it.
struct RegistrationNode
{
	RegistrationNode* next;       // +0x00
	const char* name;             // +0x08
	ILoopModeFactory* factory;    // +0x10
	ILoopMode** global_pointer;   // +0x18
};

const char* Str(const char* text)
{
	return text != nullptr ? text : "(null)";
}

const char* LoopModeTypeName(int type)
{
	switch (type)
	{
		case LOOP_MODE_TYPE_INVALID: return "INVALID";
		case LOOP_MODE_TYPE_SIMPLE:  return "SIMPLE";
		case LOOP_MODE_TYPE_CLIENT:  return "CLIENT";
		case LOOP_MODE_TYPE_SERVER:  return "SERVER";
		case LOOP_MODE_TYPE_HOST:    return "HOST";
		default:                     return "<out of range>";
	}
}

} // namespace

// -----------------------------------------------------------------------------
//  Acquire
// -----------------------------------------------------------------------------

bool LoopModeSuite::Acquire(TestContext& context)
{
#if !CS2ST_WINDOWS
	const Module& server = context.GetModule("server");
	if (!server.IsValid())
	{
		context.UnavailableCall("ILoopModeFactory", "the server module is not loaded");
		return false;
	}

	// Optional: only used to tell which registration is the active loop.
	manager_ = static_cast<IEngineServiceMgr*>(
		context.QueryInterface("engine2", ENGINESERVICEMGR_INTERFACE_VERSION));
	context.Note("server module %s at %p; IEngineServiceMgr %s", server.name().c_str(),
		server.base(), manager_ != nullptr ? "available" : "not available");
	return true;
#else
	context.UnavailableCall("ILoopModeFactory",
		"the registry signature is Linux-only; no Windows pattern has been derived");
	return false;
#endif
}

// -----------------------------------------------------------------------------
//  Finding the registrations
// -----------------------------------------------------------------------------

bool LoopModeSuite::FindRegistrations(TestContext& context)
{
	registration_count_ = 0;
	const Module& server = context.GetModule("server");

	bool found = false;
	CS2ST_CALL(context, "CLoopModeRegistry list head (signature scan)", {
		BytePattern pattern;
		const char* parse_error = ParseBytePattern(kRegistryPattern, pattern);
		if (parse_error != nullptr)
		{
			context.Fail("pattern does not parse: %s", parse_error);
			return;
		}

		const PatternScanResult scan = ScanModule(server, pattern);
		if (scan.matches != 1)
		{
			context.Detail("%zu-byte pattern matches %zu time(s) in %s", pattern.size(),
				scan.matches, server.name().c_str());
			context.Fail("signature %s; it must match exactly once, so no loop mode can be reached",
				scan.matches == 0 ? "matches nothing" : "matches more than once");
			return;
		}

		const unsigned char* function = static_cast<const unsigned char*>(scan.first);
		std::int32_t displacement = 0;
		std::memcpy(&displacement, function + kHeadDispOffset, sizeof(displacement));
		RegistrationNode* const* head = reinterpret_cast<RegistrationNode* const*>(
			function + kHeadNextInstruction + displacement);

		const RegistrationNode* node = *head;
		context.Detail("function %p (rva 0x%zx), list head %p -> first node %p", scan.first,
			static_cast<std::size_t>(function - static_cast<const unsigned char*>(server.base())),
			static_cast<const void*>(head), static_cast<const void*>(node));

		while (node != nullptr && registration_count_ < kMaxRegistrations)
		{
			Registration& slot = registrations_[registration_count_++];
			slot.name = node->name;
			slot.factory = node->factory;
			slot.global_pointer = node->global_pointer;
			node = node->next;
		}
		if (node != nullptr)
		{
			context.Suspect("more than %d registrations; the rest are ignored", kMaxRegistrations);
		}
		found = true;
	});
	return found && registration_count_ > 0;
}

// -----------------------------------------------------------------------------
//  Calls
// -----------------------------------------------------------------------------

void LoopModeSuite::ExerciseCalls(TestContext& context)
{
	if (!FindRegistrations(context))
	{
		context.UnavailableCall("ILoopModeFactory", "no registration list could be read");
		return;
	}

	const char* active_name = nullptr;
	if (manager_ != nullptr)
	{
		CS2ST_CALL(context, "IEngineServiceMgr::GetActiveLoopName (for matching)", {
			active_name = manager_->GetActiveLoopName();
			context.Detail("-> \"%s\"", Str(active_name));
		});
	}

	int live_modes = 0;
	for (int i = 0; i < registration_count_; ++i)
	{
		const Registration& registration = registrations_[i];
		const std::string label = std::string("[") + Str(registration.name) + "]";

		context.Note("registration %d %s: factory %p, global pointer %p", i, Str(registration.name),
			static_cast<void*>(registration.factory),
			static_cast<void*>(registration.global_pointer));

		if (registration.name == nullptr || registration.factory == nullptr)
		{
			context.Suspect("registration %d has a null name or factory", i);
			continue;
		}

		ExerciseFactory(context, registration);

		// Two routes to the live ILoopMode. The registration's global pointer is
		// optional (the game loop has none, 2026-11 Linux). The engine's own
		// per-loop object always holds the instance it created.
		ILoopMode* loop_mode = nullptr;
		if (registration.global_pointer != nullptr)
		{
			CS2ST_CALL(context, "ILoopMode global pointer " + label, {
				loop_mode = *registration.global_pointer;
				context.Detail("-> %p", static_cast<void*>(loop_mode));
			});
		}
		if (loop_mode == nullptr && manager_ != nullptr)
		{
			CS2ST_CALL(context, "ILoopMode engine-side instance " + label, {
				loop_mode = FindEngineInstance(context, registration);
				context.Detail("-> %p", static_cast<void*>(loop_mode));
			});
		}
		if (loop_mode == nullptr && registration.global_pointer == nullptr)
		{
			context.SkipCall("ILoopMode " + label,
				"no global pointer and the engine-side instance was not found");
			continue;
		}

		const bool is_active = active_name != nullptr
			&& std::strcmp(active_name, registration.name) == 0;
		if (loop_mode != nullptr)
		{
			++live_modes;
			if (active_name != nullptr && !is_active)
			{
				context.Suspect("\"%s\" has a live ILoopMode but the active loop is \"%s\"",
					registration.name, active_name);
			}
			ExerciseLoopMode(context, registration, loop_mode);
		}
		else if (is_active)
		{
			context.Suspect("the active loop \"%s\" has no ILoopMode in its global pointer",
				registration.name);
			context.SkipCall("ILoopMode " + label, "no live instance to call");
		}
		else
		{
			context.SkipCall("ILoopMode " + label, "not the active loop; no live instance");
		}
	}
	context.Note("%d of %d registered loop mode(s) had a live ILoopMode", live_modes,
		registration_count_);

	// The factory methods that build or tear down loop modes, and every
	// ILoopMode method that drives a loop transition, are skipped per
	// registration inside ExerciseFactory / ExerciseLoopMode.
}

// -----------------------------------------------------------------------------
//  The engine's own record of a loop mode
// -----------------------------------------------------------------------------

// CEngineServiceMgr::RegisterLoopMode (2026-11 Linux libengine2.so) keeps one
// object per registered loop, in an array of pointers at this+56 with the
// count at this+48; the object's name is at +16 (that is what GetActiveLoopName
// returns). Each loop object stores up to three {factory, instance, global
// pointer} triples, one per LoopModeType_t, starting at +144, +168 and +192.
// A triple belongs to the registration when its factory is the registered one,
// and that is the check: nothing is taken on offset alone.
// Reverse-engineered, not from the SDK; a mismatch returns null.
ILoopMode* LoopModeSuite::FindEngineInstance(TestContext& context, const Registration& registration)
{
	const unsigned char* mgr = reinterpret_cast<const unsigned char*>(manager_);
	const int count = *reinterpret_cast<const int*>(mgr + 48);
	const unsigned char* const* entries =
		*reinterpret_cast<const unsigned char* const* const*>(mgr + 56);
	if (count <= 0 || count > 64 || entries == nullptr)
	{
		context.Suspect("engine loop table has count %d at %p; the layout probably changed",
			count, static_cast<const void*>(entries));
		return nullptr;
	}

	static const std::size_t kTripleOffsets[] = { 144, 168, 192 };
	for (int i = 0; i < count; ++i)
	{
		const unsigned char* entry = entries[i];
		if (entry == nullptr)
		{
			continue;
		}
		const char* name = *reinterpret_cast<const char* const*>(entry + 16);
		if (name == nullptr || std::strcmp(name, registration.name) != 0)
		{
			continue;
		}
		for (const std::size_t offset : kTripleOffsets)
		{
			ILoopModeFactory* factory = *reinterpret_cast<ILoopModeFactory* const*>(entry + offset);
			if (factory != registration.factory)
			{
				continue;
			}
			ILoopMode* instance = *reinterpret_cast<ILoopMode* const*>(entry + offset + 8);
			context.Detail("loop object %p, triple at +%zu, factory matches",
				static_cast<const void*>(entry), offset);
			return instance;
		}
		context.Suspect("loop object %p is named \"%s\" but none of its factories is %p",
			static_cast<const void*>(entry), name, static_cast<void*>(registration.factory));
		return nullptr;
	}
	context.Detail("no engine loop object is named \"%s\" (%d examined)", registration.name, count);
	return nullptr;
}

void LoopModeSuite::ExerciseFactory(TestContext& context, const Registration& registration)
{
	const std::string label = std::string("[") + Str(registration.name) + "]";
	ILoopModeFactory* factory = registration.factory;

	CS2ST_CALL(context, "ILoopModeFactory::GetLoopModeType " + label, {
		const int type = static_cast<int>(factory->GetLoopModeType());
		context.Detail("-> %s (%d)", LoopModeTypeName(type), type);
		if (type <= LOOP_MODE_TYPE_INVALID || type >= LOOP_MODE_TYPE_COUNT)
		{
			context.Fail("type %d is outside LoopModeType_t; slot 4 is probably not GetLoopModeType",
				type);
		}
	});

	context.SkipCall("ILoopModeFactory::Init " + label,
		"hands the factory its ILoopType; re-running it would re-add engine services");
	context.SkipCall("ILoopModeFactory::Shutdown " + label,
		"takes the registry lock and tears the factory down");
	context.SkipCall("ILoopModeFactory::CreateLoopMode " + label,
		"constructs a second CLoopModeGame (logs, allocates, touches game state)");
	context.SkipCall("ILoopModeFactory::DestroyLoopMode " + label,
		"only valid for an object CreateLoopMode made, and that is skipped");
}

void LoopModeSuite::ExerciseLoopMode(TestContext& context, const Registration& registration,
	ILoopMode* loop_mode)
{
	const std::string label = std::string("[") + Str(registration.name) + "]";

	CS2ST_CALL(context, "ILoopMode::HandleInputEvent " + label, {
		InputEvent_t event;
		std::memset(&event, 0, sizeof(event));
		const InputHandlerResult_t result = loop_mode->HandleInputEvent(event, CSplitScreenSlot(0));
		context.Detail("zeroed event -> %d", static_cast<int>(result));
		if (result != IHR_INPUT_IGNORED && result != IHR_INPUT_CONSUMED)
		{
			context.Fail("result %d is outside InputHandlerResult_t", static_cast<int>(result));
		}
		else if (result == IHR_INPUT_CONSUMED)
		{
			context.Suspect("a dedicated server's loop mode consumed an empty input event");
		}
	});

	CS2ST_CALL(context, "ILoopMode::AddViewsToSceneSystem " + label, {
		EngineLoopState_t state;
		std::memset(&state, 0, sizeof(state));
		RenderViewport_t viewport;
		std::memset(&viewport, 0, sizeof(viewport));
		int sentinel = 0;
		RHBackColorBuffer_t buffer;
		std::memset(&buffer, 0, sizeof(buffer));
		buffer.m_pView = reinterpret_cast<ISceneView*>(&sentinel);
		const RHBackColorBuffer_t result =
			loop_mode->AddViewsToSceneSystem(state, 0.0, 0.0, viewport, buffer);
		context.Detail("buffer view %p -> view %p", static_cast<void*>(buffer.m_pView),
			static_cast<void*>(result.m_pView));
		if (result.m_pView != buffer.m_pView)
		{
			context.Suspect("a server loop mode should pass the back buffer through unchanged");
		}
	});

	CS2ST_CALL(context, "ILoopMode::unk001 " + label, {
		// "True only while loading a level"; the tests run on a loaded server.
		const bool loading = loop_mode->unk001();
		context.Detail("-> %s", loading ? "true" : "false");
	});

	context.SkipCall("ILoopMode::LoopInit " + label, "initialises the loop's game systems again");
	context.SkipCall("ILoopMode::LoopShutdown " + label, "shuts the running loop down");
	context.SkipCall("ILoopMode::OnLoopActivate " + label,
		"re-activates the loop and needs the engine's event dispatcher");
	context.SkipCall("ILoopMode::OnLoopDeactivate " + label,
		"deactivates the running loop and needs the engine's event dispatcher");
	context.SkipCall("ILoopMode::RegisterEventMap " + label,
		"adds or removes the loop's event handlers on the engine's dispatcher");
}

} // namespace cs2st
