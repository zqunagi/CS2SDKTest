/**
 * The library's entry point.
 *
 * The library is injected into the cs2 process before the engine has loaded
 * anything: with LD_PRELOAD on Linux, and on Windows by cst_run.exe, which
 * starts cs2.exe suspended with this DLL added to its import table (Microsoft
 * Detours). Either way Start() runs inside the process at that point, so all it
 * does there is start a worker thread. The worker waits for the server module
 * (libserver.so / server.dll), finds
 * ISource2Server::GameFrame in the interface's vtable by what the function
 * does (it references the string "GameFrame") rather than by SDK slot number,
 * and swaps that one slot for ours. From then on the sweep is started from the
 * engine's own main-thread frame.
 *
 * Failure is always visible and always bounded: if the hook cannot be placed,
 * or the game never reaches a simulating map, the reason goes to the console
 * and the process exits, instead of idling until the runner's timeout.
 *
 *   CST_RUN=1         required; without it the library does nothing
 *   CST_RUN_SUITE=id  one suite instead of all of them
 *   CST_RUN_DELAY=n   seconds of simulating map before the sweep (default 10)
 *   CST_RUN_QUIT=0    leave the server running afterwards
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/Platform.h"

#if CS2ST_POSIX
	#include <unistd.h>
#endif

#include <appframework/iappsystem.h>
#include <engine2/ivengineserver2.h>
#include <interfaces/interfaces.h>
#include <tier0/convar.h>
#include <tier0/icvar.h>

#include "Entry.h"
#include "core/FileSystemUtil.h"
#include "core/Log.h"
#include "core/Module.h"
#include "core/Process.h"
#include "plugin/Sweep.h"
#include "testkit/CrashRecorder.h"
#include "testkit/TestContext.h"
#include "testkit/TestRunner.h"
#include "version_gen.h"

namespace cs2st
{
namespace
{

using CreateInterfaceFn = void* (*)(const char*, int*);
using GameFrameFn = void (*)(void* self, bool simulating, bool first_tick, bool last_tick);

constexpr const char* kServerInterface = "Source2Server001";

// How long to wait for things that should be quick. Both end in an exit with a
// message; the runner's own timeout is the backstop, not the plan.
constexpr int kHookWaitSeconds = 120;
constexpr int kSimulatingWaitSeconds = 300;
constexpr int kQuitGraceSeconds = 20;

const char* EnvOrNull(const char* name)
{
	const char* value = std::getenv(name);
	return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

enum class State : int
{
	kWaiting,
	kRunning,
	kDone,
};

GameFrameFn g_original = nullptr;
std::atomic<State> g_state{State::kWaiting};

std::string g_only;
int g_delay_seconds = 10;
bool g_quit_when_done = true;

bool g_clock_started = false;
std::chrono::steady_clock::time_point g_clock_start;

std::atomic<bool> g_hook_placed{false};

bool g_cvar_ready = false;

[[noreturn]] void ExitNow(int code)
{
	// Terminate() does not flush: this also writes out the session log.
	std::fflush(nullptr);
	proc::Terminate(code);
}

// -----------------------------------------------------------------------------
//  Running the sweep (engine main thread, inside GameFrame)
// -----------------------------------------------------------------------------

/**
 * Takes the probe convars and commands back out of ICvar, and lets go of g_pCVar.
 *
 * Each CConVar and ConCommand this build declares at file scope unregisters
 * itself from its destructor, through g_pCVar. Those destructors run when the
 * process exits, and by then the engine has already torn down the cvar system
 * that g_pCVar points at: the first one to call into it faults inside
 * libtier0. Doing the unregistering here, while the cvar system is alive, and
 * then clearing g_pCVar, leaves the destructors with nothing to call. The SDK's
 * unregister path is a no-op for a ref that is already invalid or when
 * g_pCVar is null, so running it twice is harmless.
 *
 * Called once, from the main thread, after the sweep has finished with the
 * convars and before the quit command is queued.
 */
void ReleaseCvarSystem()
{
	if (!g_cvar_ready)
	{
		return;
	}
	ConVar_Unregister();
	g_pCVar = nullptr;
	g_cvar_ready = false;
	CS2ST_LOG_INFO("run: probe convars unregistered, g_pCVar released\n");
}

void Quit()
{
	TestContext context;
	auto* engine = static_cast<IVEngineServer2*>(
		context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));
	CrashRecorder::SetPhase("quitting");
	if (engine == nullptr)
	{
		CS2ST_LOG_WARN("run: IVEngineServer2 not available, the server will not quit by itself\n");
		return;
	}
	CS2ST_LOG_INFO("run: quitting\n");
	engine->ServerCommand("quit\n");
}

void Execute()
{
	static TestRunner runner = TestRunner::CreateDefault();

	if (!g_only.empty() && runner.Find(g_only) == nullptr)
	{
		CS2ST_LOG_ERROR("run: no test suite named '%s'\n", g_only.c_str());
		return;
	}
	if (runner.size() == 0)
	{
		CS2ST_LOG_WARN("run: no test suites registered in this build\n");
		return;
	}

	CS2ST_LOG_INFO("run: starting call sweep\n");
	RunCallSweep(runner, g_only, "run");
	CS2ST_LOG_INFO("run: sweep finished\n");
}

// -----------------------------------------------------------------------------
//  The SDK globals a Metamod plugin gets for free
// -----------------------------------------------------------------------------

/**
 * Sets the SDK's g_pCVar and registers the convars and commands this build
 * declares at file scope (the ConVarSuite probe convars).
 *
 * A plugin framework would do this at load time. Here it only touches the SDK's
 * own registration queue and ICvar, so what it needs is a live ICvar and a
 * thread that is allowed to call it: this runs on the engine's main thread, in
 * the first simulating frame, which is the same context the sweep runs in.
 *
 * The callbacks are null on purpose. ConVar_Register's own RegisterAll already
 * hands every queued convar to ICvar::RegisterConVar; the callbacks are only a
 * way for a framework to learn about each one, and nothing here wants to know.
 */
bool InitCvarSystem()
{
	if (g_cvar_ready)
	{
		return true;
	}

	TestContext context;
	auto* icvar = static_cast<ICvar*>(context.QueryInterface("tier0", CVAR_INTERFACE_VERSION));
	if (icvar == nullptr)
	{
		icvar = static_cast<ICvar*>(context.QueryInterfaceAnywhere(CVAR_INTERFACE_VERSION));
	}
	if (icvar == nullptr)
	{
		CS2ST_LOG_ERROR("run: ICvar (%s) not available; the probe convars cannot be registered\n",
			CVAR_INTERFACE_VERSION);
		return false;
	}

	g_pCVar = icvar;
	ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);
	g_cvar_ready = true;
	CS2ST_LOG_INFO("run: cvar system ready (ICvar at %p), probe convars registered\n",
		static_cast<void*>(icvar));
	return true;
}

void OnFrame(bool simulating)
{
	if (g_state.load() != State::kWaiting || !simulating)
	{
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (!g_clock_started)
	{
		g_clock_started = true;
		g_clock_start = now;
		// First simulating frame: the engine is as ready as it will get, and this is
		// the main thread. The sweep is a delay away, so registration is long done.
		InitCvarSystem();
		CrashRecorder::SetPhase("map_simulating");
		CS2ST_LOG_INFO("run: map is simulating, sweeping in %ds\n", g_delay_seconds);
	}
	if (now - g_clock_start < std::chrono::seconds(g_delay_seconds))
	{
		return;
	}

	// Set first: the sweep runs inside this frame and may take a while, and a
	// second GameFrame must never get here while it does.
	g_state.store(State::kRunning);
	CrashRecorder::SetPhase("sweep");
	Execute();
	g_state.store(State::kDone);
	CrashRecorder::SetPhase("sweep_done");

	if (g_quit_when_done)
	{
		ReleaseCvarSystem();
		Quit();
	}
}

void HookedGameFrame(void* self, bool simulating, bool first_tick, bool last_tick)
{
	OnFrame(simulating);
	g_original(self, simulating, first_tick, last_tick);
}

// -----------------------------------------------------------------------------
//  Finding and patching the GameFrame slot
// -----------------------------------------------------------------------------

/** Is `address` inside the mapped image of `module`? */
bool InModule(const Module& module, const void* address)
{
	const auto* base = static_cast<const unsigned char*>(module.base());
	const auto* p = static_cast<const unsigned char*>(address);
	return p >= base && p < base + module.size();
}

/**
 * Does the code at `fn` take the address of the C string "GameFrame"?
 *
 * Fallback for platforms that cannot say which function a reference belongs to
 * (Linux): a fixed window from the start of the slot's function. It has no idea
 * where the function ends, so a small function sitting just before GameFrame
 * would match too; the caller insists on exactly one match for that reason.
 * Windows does not use this: proc::FunctionsReferencingString works from the
 * string outwards and needs no window.
 */
bool ReferencesGameFrameString(const void* fn, const Module& module)
{
	constexpr std::size_t kWindow = 0x300;
	const unsigned char* code = static_cast<const unsigned char*>(fn);
	for (std::size_t i = 0; i + 7 <= kWindow; ++i)
	{
		// lea r64, [rip+disp32]
		if ((code[i] != 0x48 && code[i] != 0x4C) || code[i + 1] != 0x8D || (code[i + 2] & 0xC7) != 0x05)
		{
			continue;
		}
		int disp = 0;
		std::memcpy(&disp, code + i + 3, sizeof(disp));
		const char* target = reinterpret_cast<const char*>(code + i + 7) + disp;

		if (InModule(module, target) && std::memcmp(target, "GameFrame", sizeof("GameFrame")) == 0)
		{
			return true;
		}
	}
	return false;
}

enum class HookResult
{
	kNotYet,
	kPlaced,
	kFailed,
};

HookResult TryPlaceHook()
{
	const Module server = Module::Find("server");
	if (!server.IsValid())
	{
		return HookResult::kNotYet;
	}
	// Module::Find answers with Metamod's loader (also built as server) when the
	// game's own copy is not mapped yet, and Metamod's is mapped first. It is the
	// wrong module to hook, and failing on it would end the run; wait for the real one.
	const std::string& server_path = server.path();
	if (server_path.find("/addons/") != std::string::npos ||
	    server_path.find("\\addons\\") != std::string::npos)
	{
		return HookResult::kNotYet;
	}
	if (server.base() == nullptr || server.size() == 0)
	{
		CS2ST_LOG_ERROR("run: cannot work out where %s is mapped\n", server.name().c_str());
		return HookResult::kFailed;
	}

	const CreateInterfaceFn create = server.factory();
	if (create == nullptr)
	{
		CS2ST_LOG_ERROR("run: %s has no CreateInterface export\n", server.name().c_str());
		return HookResult::kFailed;
	}

	int code = 0;
	void* iface = create(kServerInterface, &code);
	if (iface == nullptr)
	{
		CS2ST_LOG_ERROR("run: CreateInterface(%s) returned null (rc=%d)\n", kServerInterface, code);
		return HookResult::kFailed;
	}

	void** vtable = *static_cast<void***>(iface);
	if (!InModule(server, vtable[0]))
	{
		CS2ST_LOG_ERROR("run: vtable slot 0 is not inside %s\n", server.name().c_str());
		return HookResult::kFailed;
	}

	// Windows: the functions that reference the string, then the slot that points at
	// one of them. Elsewhere: look inside each slot's function.
	std::vector<const void*> referencing;
	const bool by_string = proc::FunctionsReferencingString(
		server.base(), server.size(), "GameFrame", referencing);

	int match = -1;
	std::string matched;
	int matches = 0;
	for (int i = 0; i < 128; ++i)
	{
		if (!InModule(server, vtable[i]))
		{
			break; // past the end of this class's table
		}
		bool references = false;
		if (by_string)
		{
			for (const void* start : referencing)
			{
				references = references || start == vtable[i];
			}
		}
		else
		{
			references = ReferencesGameFrameString(vtable[i], server);
		}
		if (references)
		{
			match = i;
			++matches;
			matched += (matched.empty() ? "" : ", ") + std::to_string(i);
		}
	}
	if (matches != 1)
	{
		CS2ST_LOG_ERROR("run: %d vtable slot(s) reference \"GameFrame\" (%s), need exactly 1; "
		                "not patching\n",
			matches, matched.c_str());
		return HookResult::kFailed;
	}

	void** slot = &vtable[match];
	g_original = reinterpret_cast<GameFrameFn>(*slot);
	if (!proc::PatchPointerSlot(slot, reinterpret_cast<void*>(&HookedGameFrame)))
	{
		g_original = nullptr;
		CS2ST_LOG_ERROR("run: could not make the vtable page writable\n");
		return HookResult::kFailed;
	}

	CS2ST_LOG_INFO("run: GameFrame is vtable slot %d of %s\n", match, kServerInterface);
	return HookResult::kPlaced;
}

// -----------------------------------------------------------------------------
//  The worker thread
// -----------------------------------------------------------------------------

void WorkerMain()
{
	for (int waited = 0;; ++waited)
	{
		const HookResult result = TryPlaceHook();
		if (result == HookResult::kPlaced)
		{
			break;
		}
		if (result == HookResult::kFailed)
		{
			CS2ST_LOG_ERROR("run: hook not placed, exiting\n");
			ExitNow(3);
		}
		if (waited >= kHookWaitSeconds)
		{
			CS2ST_LOG_ERROR("run: the server module did not appear within %ds, exiting\n", kHookWaitSeconds);
			ExitNow(3);
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	g_hook_placed.store(true);
	CrashRecorder::SetPhase("run_armed");
	CS2ST_LOG_INFO("run: armed (suite=%s, delay=%ds, quit=%s)\n",
		g_only.empty() ? "all" : g_only.c_str(), g_delay_seconds, g_quit_when_done ? "yes" : "no");

	// Watch for a game that never gets going. The sweep itself is not watched
	// here: a hang inside it is the runner's timeout to report.
	for (int waited = 0; g_state.load() == State::kWaiting; ++waited)
	{
		if (waited >= g_delay_seconds + kSimulatingWaitSeconds)
		{
			CS2ST_LOG_ERROR("run: no simulating map after %ds, exiting\n", waited);
			ExitNow(4);
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	// A quit that the engine ignores would leave the server idling until the
	// runner's timeout, which reads as a hang. Results are on disk by now.
	while (g_state.load() != State::kDone)
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	if (g_quit_when_done)
	{
		std::this_thread::sleep_for(std::chrono::seconds(kQuitGraceSeconds));
		CS2ST_LOG_WARN("run: still running %ds after quit, exiting\n", kQuitGraceSeconds);
		ExitNow(0);
	}
}

} // namespace
} // namespace cs2st

namespace cs2st
{

// Runs in whatever process loaded the library: LD_PRELOAD puts it into every
// process the launcher starts, and cst_run.exe only ever injects it into cs2.exe.
// Only the game itself is of interest, and only when the operator asked for a run.
void Start()
{
	const char* enabled = EnvOrNull("CST_RUN");
	if (enabled == nullptr || std::strcmp(enabled, "1") != 0 || !proc::IsExecutable("cs2"))
	{
		return;
	}

	if (const char* suite = EnvOrNull("CST_RUN_SUITE"))
	{
		g_only = suite;
	}
	if (const char* delay = EnvOrNull("CST_RUN_DELAY"))
	{
		const int parsed = std::atoi(delay);
		g_delay_seconds = parsed < 0 ? 0 : (parsed > 600 ? 600 : parsed);
	}
	if (const char* quit = EnvOrNull("CST_RUN_QUIT"))
	{
		g_quit_when_done = std::strcmp(quit, "0") != 0;
	}

	InitLogging();

	// Open the run log now rather than when the sweep starts (ScopedRunLog writes
	// into a session log that is already open). The dedicated server has a console
	// window of its own on Windows that closes with the process, so a run that
	// ends before the sweep, the case this exists for, would otherwise leave
	// nothing to read.
	{
		const std::string log_path =
			fs::Join(fs::LogDirectory(), "run-" + fs::TimestampForFileName() + ".log");
		std::string log_error;
		if (!OpenSessionLog(log_path, log_error))
		{
			CS2ST_LOG_WARN("run: could not open the run log: %s (console only)\n", log_error.c_str());
		}
	}

	CrashRecorder::SetPhase("plugin_load");
	CrashRecorder::Install();
	CrashRecorder::SetPhase("plugin_loaded");

	// Same shape as the Metamod build's line, so the runner's stage detection
	// finds it either way.
	CS2ST_LOG_INFO("%s %s loaded\n", PLUGIN_DISPLAY_NAME, PLUGIN_FULL_VERSION);
	CS2ST_LOG_INFO("built against SDK '%s' (SOURCE_ENGINE=%d)\n", PLUGIN_SDK_ENGINE, PLUGIN_SDK_ENGINE_CODE);

	std::thread(WorkerMain).detach();
}

} // namespace cs2st

#if CS2ST_POSIX
// On Windows the entry point is DllMain (DllMain.cpp).
__attribute__((constructor)) static void CS2SDKTestInit()
{
	cs2st::Start();
}
#endif
