/**
 * Unattended mode: run the call sweep once the map is up, then quit.
 *
 * Off unless CST_AUTORUN=1 is in the server's environment - everything else in
 * this plugin is on-demand, and that stays true when the variable is absent.
 *
 *   CST_AUTORUN=1          enable
 *   CST_AUTORUN_SUITE=id   one suite instead of all of them
 *   CST_AUTORUN_DELAY=n    seconds of simulated game time to wait after the
 *                          map is up before sweeping (default 10)
 *   CST_AUTORUN_QUIT=0     leave the server running afterwards
 *   CST_RESULT_DIR=path    where results.json(l) go (see ResultSink)
 *
 * Timing: the sweep needs a map that is actually running (the entity system,
 * game rules, a bot to poke), so it is triggered from ISource2Server::GameFrame
 * rather than from a load callback. The first frame with `simulating` set
 * starts the clock; the sweep runs on the first frame after the delay.
 *
 * This is not a watchdog. A hang or a crash the fault guard cannot catch takes
 * the process with it, and only something outside the process can notice -
 * results.jsonl is what that something reads afterwards.
 */

#ifndef CS2SDKTEST_PLUGIN_AUTORUN_H_
#define CS2SDKTEST_PLUGIN_AUTORUN_H_

#include <chrono>
#include <string>

#include <khook.hpp>

class ISource2Server;
class IVEngineServer2;

namespace cs2st
{

class Autorun
{
public:
	Autorun();
	~Autorun();

	Autorun(const Autorun&) = delete;
	Autorun& operator=(const Autorun&) = delete;

	/** True when CST_AUTORUN=1 is set. Cheap; call before constructing. */
	static bool RequestedByEnvironment();

	/** Reads the settings, resolves the interfaces, installs the frame hook. */
	bool Start();

	/** Removes the frame hook. Safe to call twice. */
	void Stop();

private:
	enum class State
	{
		kIdle,
		kWaiting,   ///< hooked, waiting for the map and the delay
		kRunning,
		kDone,
	};

	KHook::Return<void> Pre_GameFrame(ISource2Server*, bool simulating, bool first_tick, bool last_tick);
	KHook::Return<void> Post_GameFrame(ISource2Server*, bool simulating, bool first_tick, bool last_tick);

	void OnFrame(bool simulating);
	void Execute();
	void Quit();

	KHook::Virtual<ISource2Server, void, bool, bool, bool> hook_GameFrame_;

	ISource2Server* server_ = nullptr;
	IVEngineServer2* engine_ = nullptr;
	bool hooked_ = false;

	State state_ = State::kIdle;
	std::string only_;
	int delay_seconds_ = 10;
	bool quit_when_done_ = true;

	bool clock_started_ = false;
	std::chrono::steady_clock::time_point clock_start_;
};

} // namespace cs2st

#endif /* CS2SDKTEST_PLUGIN_AUTORUN_H_ */
