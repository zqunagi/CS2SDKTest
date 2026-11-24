/**
 * Unattended mode: run the call sweep once the map is up, then quit.
 *
 * Off unless CST_AUTORUN=1 is in the server's environment - everything else in
 * this plugin is on-demand, and that stays true when the variable is absent.
 *
 *   CST_AUTORUN=1          enable
 *   CST_AUTORUN_SUITE=id   one suite instead of all of them
 *   CST_AUTORUN_DELAY=n    seconds after the plugin is armed before sweeping
 *                          (default 30). It has to cover the map load.
 *   CST_AUTORUN_QUIT=0     leave the server running afterwards
 *   CST_RESULT_DIR=path    where results.json(l) go (see ResultSink)
 *
 * Timing: no engine vtable is hooked. A timer thread sleeps for the delay and
 * then queues `cst autorun-fire` on the engine's command buffer, which runs the
 * sweep on the main thread. The delay is counted from when the plugin is armed,
 * so it must be long enough for the map to finish loading; nothing checks that
 * the map is actually simulating.
 *
 * This is not a watchdog. A hang or a crash the fault guard cannot catch takes
 * the process with it, and only something outside the process can notice -
 * results.jsonl is what that something reads afterwards.
 */

#ifndef CS2SDKTEST_PLUGIN_AUTORUN_H_
#define CS2SDKTEST_PLUGIN_AUTORUN_H_

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

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

	/** Reads the settings, resolves the engine interface, starts the timer. */
	bool Start();

	/** Runs the sweep. Main thread only; reached through `cst autorun-fire`. */
	void Fire();

	/** Cancels the timer and joins its thread. Safe to call twice. */
	void Stop();

private:
	enum class State
	{
		kIdle,
		kWaiting,   ///< timer running, waiting for the delay
		kRunning,
		kDone,
	};

	void TimerMain();
	void Execute();
	void Quit();

	IVEngineServer2* engine_ = nullptr;

	std::thread timer_;
	std::mutex mutex_;
	std::condition_variable cv_;
	bool cancel_ = false;

	State state_ = State::kIdle;
	std::string only_;
	int delay_seconds_ = 30;
	bool quit_when_done_ = true;
};

/** Entry point for the `cst autorun-fire` command; no-op when autorun is not armed. */
void FireAutorun();

} // namespace cs2st

#endif /* CS2SDKTEST_PLUGIN_AUTORUN_H_ */
