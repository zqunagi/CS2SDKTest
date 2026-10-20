#include "plugin/Autorun.h"

#include <cstdlib>
#include <cstring>

#include <appframework/iappsystem.h>
#include <engine2/isource2server.h>
#include <engine2/ivengineserver2.h>

#include "core/Log.h"
#include "plugin/Commands.h"
#include "plugin/Plugin.h"
#include "testkit/TestContext.h"
#include "testkit/TestRunner.h"

namespace cs2st
{
namespace
{

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
	server_ = static_cast<ISource2Server*>(
		context.QueryInterface("server", INTERFACEVERSION_SERVERGAMEDLL));
	engine_ = static_cast<IVEngineServer2*>(
		context.QueryInterface("engine2", INTERFACEVERSION_VENGINESERVER));

	if (server_ == nullptr)
	{
		CS2ST_LOG_ERROR("autorun: ISource2Server not available, cannot hook GameFrame; "
		                "nothing will run\n");
		return false;
	}
	if (engine_ == nullptr && quit_when_done_)
	{
		CS2ST_LOG_WARN("autorun: IVEngineServer2 not available, the server will not quit by itself\n");
	}

	hook_GameFrame_.Add(server_);
	hooked_ = true;
	state_ = State::kWaiting;

	CS2ST_LOG_INFO("autorun: armed (suite=%s, delay=%ds, quit=%s)\n",
		only_.empty() ? "all" : only_.c_str(), delay_seconds_, quit_when_done_ ? "yes" : "no");
	return true;
}

void Autorun::Stop()
{
	if (hooked_)
	{
		hook_GameFrame_.Remove(server_);
		hooked_ = false;
	}
	state_ = State::kIdle;
}

void Autorun::OnFrame(bool simulating)
{
	if (state_ != State::kWaiting || !simulating)
	{
		return;
	}

	const auto now = std::chrono::steady_clock::now();
	if (!clock_started_)
	{
		clock_started_ = true;
		clock_start_ = now;
		CS2ST_LOG_INFO("autorun: map is simulating, sweeping in %ds\n", delay_seconds_);
	}

	if (now - clock_start_ < std::chrono::seconds(delay_seconds_))
	{
		return;
	}

	// Set first: the sweep runs inside this frame and may take a while, and a
	// second GameFrame must never get here while it does.
	state_ = State::kRunning;
	Execute();
	state_ = State::kDone;

	if (quit_when_done_)
	{
		Quit();
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
	CS2ST_LOG_INFO("autorun: quitting\n");
	engine_->ServerCommand("quit\n");
}

KHook::Return<void> Autorun::Pre_GameFrame(ISource2Server*, bool simulating, bool, bool)
{
	OnFrame(simulating);
	return { KHook::Action::Ignore };
}

KHook::Return<void> Autorun::Post_GameFrame(ISource2Server*, bool, bool, bool)
{
	return { KHook::Action::Ignore };
}

// convar.h ends with `#pragma warning(default : 4355)`, which re-enables the warning
// for the rest of the translation unit; `this` as the hook context is intentional.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4355)
#endif
Autorun::Autorun() :
	hook_GameFrame_(&ISource2Server::GameFrame, this,
		&Autorun::Pre_GameFrame,
		&Autorun::Post_GameFrame)
{
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

Autorun::~Autorun()
{
	Stop();
}

} // namespace cs2st
