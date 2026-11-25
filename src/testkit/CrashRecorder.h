/**
 * Records what killed the process, for the faults CallGuard cannot catch.
 *
 * CallGuard turns a fault inside a *probing call* into a CRASHED row. Anything
 * else takes the server down without a word: a hook callback running inside the
 * engine's own call, another thread, an abort() from an assert, a fault during
 * map load before the sweep even starts. The runner then has a dead process, an
 * exit code, and nothing that says where.
 *
 * With this installed, a fatal signal writes crash.json before the process goes
 * on dying:
 *
 *   signal, si_code, fault address, the faulting pc
 *   the phase the plugin had reached (see SetPhase)
 *   the call in flight, when a sweep was running
 *   a backtrace, each frame resolved to module + offset
 *
 * It does not try to survive: after writing, the signal goes to whoever held the
 * handler before (the engine's breakpad, usually) and finally to the default
 * action, so exit codes, minidumps and core files are what they would have been.
 *
 * The handler runs in a signal context, so it uses nothing that allocates or
 * locks: fixed buffers, open/read/write, and a /proc/self/maps walk instead of
 * dladdr (which takes the loader lock the crashing thread may already hold).
 *
 * Linux only. On Windows Install() reports that it is not available; the
 * machine this runs on is a Linux dedicated server.
 */

#ifndef CS2SDKTEST_TESTKIT_CRASHRECORDER_H_
#define CS2SDKTEST_TESTKIT_CRASHRECORDER_H_

namespace cs2st
{

class CrashRecorder
{
public:
	/**
	 * Installs the handlers. crash.json goes to CST_RESULT_DIR when set, <home>/logs
	 * otherwise; a stale one from an earlier run is
	 * removed. Safe to call twice.
	 */
	static bool Install();

	/** Gives the handlers back, if nobody installed theirs on top of ours. */
	static void Uninstall();

	/**
	 * Where the plugin is in its life, for the report. `phase` must outlive the
	 * process (a string literal): the handler reads the pointer, nothing copies it.
	 */
	static void SetPhase(const char* phase);

	/** The call about to run, for the report. Copied; truncated if long. */
	static void SetCall(const char* suite, const char* call);
	static void ClearCall();
};

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_CRASHRECORDER_H_ */
