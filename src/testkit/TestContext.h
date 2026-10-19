/**
 * The per-run context handed to every test module.
 *
 * It owns three things a module should not have to re-do:
 *   - a cache of the game's loaded modules and their CreateInterface factories,
 *   - the single place a probing call is made and its outcome recorded,
 *   - the running tallies that become the summary at the end of a log.
 *
 * The intended way to write a test is the CS2ST_CALL macro at the bottom:
 *
 *     CS2ST_CALL(ctx, "IVEngineServer2::GetServerGlobals", {
 *         void* globals = engine->GetServerGlobals();
 *         ctx.Detail("-> %p", globals);
 *     });
 *
 * which logs one aligned line, routes the body through the fault guard, and
 * keeps the counters right - so filling in coverage stays a matter of writing
 * the call itself and nothing else.
 */

#ifndef CS2SDKTEST_TESTKIT_TESTCONTEXT_H_
#define CS2SDKTEST_TESTKIT_TESTCONTEXT_H_

#include <functional>
#include <map>
#include <string>

#include "core/Module.h"
#include "core/Platform.h"
#include "testkit/CallGuard.h"

namespace cs2st
{

struct RunTally
{
	std::size_t ok = 0;
	std::size_t crashed = 0;
	std::size_t skipped = 0;
	std::size_t unavailable = 0;
	std::size_t failed = 0;
	std::size_t suspect = 0;

	std::size_t Total() const
	{
		return ok + crashed + skipped + unavailable + failed + suspect;
	}
	void Count(CallOutcome outcome);
};

class TestContext
{
public:
	TestContext() = default;
	TestContext(const TestContext&) = delete;
	TestContext& operator=(const TestContext&) = delete;

	// ---- module and interface access ---------------------------------------

	/**
	 * Returns an already-loaded game module by undecorated name ("engine2"),
	 * caching the lookup. Never null; check IsValid().
	 *
	 * Nothing here ever *loads* a library: by the time the plugin runs the
	 * engine has mapped everything worth looking at, and forcing a load would
	 * re-run static initialisers.
	 */
	const Module& GetModule(const std::string& stem);

	/**
	 * Asks a module's factory for an interface version string.
	 * Returns nullptr when the module is absent or does not implement it.
	 */
	void* QueryInterface(const std::string& module_stem, const char* version);

	/**
	 * Asks every module in the table, in order, and returns the first that
	 * answers. Use when the SDK does not say which module owns an interface.
	 */
	void* QueryInterfaceAnywhere(const char* version, std::string* found_in = nullptr);

	// ---- section structure -------------------------------------------------

	/** Starts a titled section in the log; one per test module. */
	void BeginSection(const char* id, const char* display_name);
	void EndSection();

	/** Writes a free-form line inside the current section. */
	void Note(const char* format, ...) CS2ST_PRINTF_FMT(2, 3);

	// ---- calls -------------------------------------------------------------

	/**
	 * Runs one probing call through the fault guard and logs the result.
	 * Prefer the CS2ST_CALL macro, which reads better at the call site.
	 */
	CallOutcome RunCall(const std::string& target, const std::function<void()>& body);

	/** Records a call that was not attempted, with a reason. */
	void SkipCall(const std::string& target, const char* format, ...) CS2ST_PRINTF_FMT(3, 4);

	/** Records a call whose prerequisite interface was missing. */
	void UnavailableCall(const std::string& target, const char* format, ...) CS2ST_PRINTF_FMT(3, 4);

	/**
	 * Sets the detail text for the call currently running.
	 * Called from inside a RunCall body; the last call wins.
	 */
	void Detail(const char* format, ...) CS2ST_PRINTF_FMT(2, 3);

	/**
	 * Marks the in-flight call as having returned something the SDK's
	 * declaration says it should not have - a null where a singleton is
	 * promised, a count that cannot be right. Cheaper to spot here than in a
	 * log read three weeks later.
	 */
	void Fail(const char* format, ...) CS2ST_PRINTF_FMT(2, 3);

	/**
	 * Like Fail, but for a value that is outside what the SDK implies *and*
	 * has a known benign reason - a field the engine never initialises, say.
	 *
	 * Still recorded and still printed, so the same field failing later for a
	 * different reason is not swallowed by yesterday's excuse. Fail wins if
	 * both are set.
	 */
	void Suspect(const char* format, ...) CS2ST_PRINTF_FMT(2, 3);

	// ---- results -----------------------------------------------------------

	const RunTally& tally() const { return tally_; }
	const std::map<std::string, Module>& modules() const { return modules_; }

private:
	void Record(const std::string& target, CallOutcome outcome, const std::string& detail);

	std::map<std::string, Module> modules_;
	RunTally tally_;

	std::string section_;
	std::string pending_detail_;
	/** Last SkipCall reason set inside a RunCall body, or empty. */
	std::string pending_skip_;
	std::string pending_failure_;
	std::string pending_suspect_;
	bool in_call_ = false;
};

} // namespace cs2st

/**
 * Wraps one probing call. `body` is a braced block; `ctx.Detail(...)` inside it
 * supplies the text logged next to the outcome.
 */
#define CS2ST_CALL(context, target, body) \
	(context).RunCall((target), [&]() body)

#endif /* CS2SDKTEST_TESTKIT_TESTCONTEXT_H_ */
