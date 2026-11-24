#include "plugin/Autorun.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include <appframework/iappsystem.h>
#include <engine2/ivengineserver2.h>

#include "core/Log.h"
#include "plugin/Commands.h"
#include "plugin/Plugin.h"
#include "testkit/CrashRecorder.h"
#include "testkit/TestContext.h"
#include "testkit/TestRunner.h"

namespace cs2st
{
namespace
{

Autorun* g_instance = nullptr;

const char* EnvOrNull(const char* name)
{
	const char* value = std::getenv(name);
	return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

} // namespace

bool Autorun::RequestedByEnvironment()
{
	const char* value = EnvOrNull("CST_AUTORUN");
	return value != nullptr && std::strcmp(value, "1") == 0;
}

bool Autorun::Start()
{
	if (const char* suite = EnvOrNull("CST_AUTORUN_SUITE"))
	{
		only_ = suite;
	}
	if (const char* delay = EnvOrNull("CST_AUTORUN_DELAY"))
	{
		const int parsed = std::atoi(delay);
		delay_seconds_ = parsed < 0 ? 0 : (parsed > 600 ? 600 : parsed);
	}
	if (const char* quit = EnvOrNull("CST_AUTORUN_QUIT"))
	{
		quit_when_done_ = std::strcmp(quit, "0") != 0;
	}

	TestContext context;
	engine_ = static_cast<IVEngineServer2*>(
		context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));

	if (engine_ == nullptr)
	{
		// The timer can only reach the main thread by queueing a console command.
		CS2ST_LOG_ERROR("autorun: IVEngineServer2 not available, cannot schedule the sweep; "
		                "nothing will run\n");
		return false;
	}

	g_instance = this;
	state_ = State::kWaiting;
	cancel_ = false;
	timer_ = std::thread(&Autorun::TimerMain, this);
	CrashRecorder::SetPhase("autorun_armed");

	CS2ST_LOG_INFO("autorun: armed (suite=%s, delay=%ds, quit=%s)\n",
		only_.empty() ? "all" : only_.c_str(), delay_seconds_, quit_when_done_ ? "yes" : "no");
	return true;
}

void Autorun::Stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		cancel_ = true;
	}
	cv_.notify_all();
	if (timer_.joinable())
	{
		timer_.join();
	}
	if (g_instance == this)
	{
		g_instance = nullptr;
	}
	state_ = State::kIdle;
}

// Runs on the timer thread and does nothing but queue a console command: the
// sweep itself must run on the engine's main thread, and the command buffer is
// how it gets there. No SDK vtable slot is involved.
void Autorun::TimerMain()
{
	std::unique_lock<std::mutex> lock(mutex_);
	if (cv_.wait_for(lock, std::chrono::seconds(delay_seconds_), [this] { return cancel_; }))
	{
		return;
	}
	lock.unlock();
	engine_->ServerCommand("cst autorun-fire\n");
}

void Autorun::Fire()
{
	if (state_ != State::kWaiting)
	{
		return;
	}

	CrashRecorder::SetPhase("map_simulating");
	CS2ST_LOG_INFO("autorun: map is simulating (assumed after the %ds delay), sweeping\n",
		delay_seconds_);

	// Set first so a second "cst autorun-fire" cannot re-enter while the sweep runs.
	state_ = State::kRunning;
	CrashRecorder::SetPhase("sweep");
	Execute();
	state_ = State::kDone;
	CrashRecorder::SetPhase("sweep_done");

	if (quit_when_done_)
	{
		Quit();
	}
}

void FireAutorun()
{
	if (g_instance != nullptr)
	{
		g_instance->Fire();
	}
}

void Autorun::Execute()
{
	TestRunner& runner = g_Plugin.tests();

	if (!only_.empty() && runner.Find(only_) == nullptr)
	{
		CS2ST_LOG_ERROR("autorun: no test suite named '%s'\n", only_.c_str());
		return;
	}
	if (runner.size() == 0)
	{
		CS2ST_LOG_WARN("autorun: no test suites registered in this build\n");
		return;
	}

	CS2ST_LOG_INFO("autorun: starting call sweep\n");
	RunCallSweep(only_, "autorun");
	CS2ST_LOG_INFO("autorun: sweep finished\n");
}

void Autorun::Quit()
{
	if (engine_ == nullptr)
	{
		return;
	}
	CrashRecorder::SetPhase("quitting");
	CS2ST_LOG_INFO("autorun: quitting\n");
	engine_->ServerCommand("quit\n");
}

Autorun::Autorun() = default;

Autorun::~Autorun()
{
	Stop();
}

} // namespace cs2st
