/**
 * The wrapper every probing call goes through.
 *
 * Calling into an interface whose signature moved is not a graceful failure -
 * it is a segfault, or a return value read out of the wrong register, or a
 * silently corrupted stack. Since that is precisely what this plugin exists to
 * find, faults are caught and turned into a log line instead of a dead server.
 *
 *   Windows - __try/__except around the invocation.
 *   Linux   - a scoped SIGSEGV/SIGBUS/SIGILL/SIGFPE handler plus
 *             sigsetjmp/siglongjmp.
 *
 * What this does NOT give you:
 *
 *   - Nothing is unwound. A caught fault skips every destructor between the
 *     fault and the guard, so whatever the call had allocated is leaked and
 *     whatever it had locked stays locked. That is an acceptable trade for a
 *     diagnostic sweep and would not be for normal code.
 *   - The process is only *probably* still healthy. A call that faulted
 *     halfway through may have left engine state inconsistent. Treat a run
 *     with crashes as a run whose later results are suspect, and restart the
 *     server before trusting it again.
 *   - Corruption that does not fault is invisible. A call that scribbles over
 *     valid memory returns kOk here and takes the server down ten minutes
 *     later.
 *
 * On Linux the handlers are installed only inside a FaultGuardScope, because
 * the engine installs its own and ours has to give them back.
 */

#ifndef CS2SDKTEST_TESTKIT_CALLGUARD_H_
#define CS2SDKTEST_TESTKIT_CALLGUARD_H_

#include <functional>
#include <string>

namespace cs2st
{

enum class CallOutcome
{
	kOk,             ///< returned normally
	kCrashed,        ///< faulted; caught by the guard
	kSkipped,        ///< not attempted (prerequisite missing)
	kUnavailable,    ///< the interface or symbol it needed was not there
	kFailed,         ///< returned, but the result was not what the SDK implies
};

const char* CallOutcomeName(CallOutcome outcome);

/** What was caught, when something was. */
struct FaultInfo
{
	bool faulted = false;

	/** POSIX signal number, or the Windows SEH exception code. */
	unsigned long code = 0;

	/** Instruction that faulted, when the platform reports it. */
	const void* instruction = nullptr;

	/** Address that was accessed, for faults that have one. */
	const void* address = nullptr;

	/** Ready-to-log summary, e.g. "SIGSEGV reading 0x0 at 0x7f2a4c001200". */
	std::string description;
};

/**
 * True when a fault during a call will be caught rather than killing the
 * process. On Linux this is only true inside a FaultGuardScope.
 */
bool IsGuardAvailable();

/**
 * One line naming the mechanism on this platform, for log headers.
 *
 * Distinct from IsGuardAvailable(), which answers "is it armed right now" -
 * on Linux that is false everywhere except inside a sweep, so it is the wrong
 * thing to print at load time.
 */
const char* GuardDescription();

/**
 * Installs the fault handlers for as long as it is alive, and puts back
 * whatever was there before.
 *
 * Wrap one sweep in one scope rather than one call: installing handlers is a
 * process-wide change, and doing it per call would both cost more and widen
 * the window in which the engine's own handlers are not in place.
 *
 * Nesting is safe; only the outermost scope installs and restores.
 */
class FaultGuardScope
{
public:
	FaultGuardScope();
	~FaultGuardScope();

	FaultGuardScope(const FaultGuardScope&) = delete;
	FaultGuardScope& operator=(const FaultGuardScope&) = delete;

	/** True if this scope (or an enclosing one) actually armed the guard. */
	static bool IsActive();

private:
	bool installed_ = false;
};

/**
 * Runs `body`, returning kOk normally or kCrashed if a fault was caught.
 * When `fault` is given it is filled in on a crash and cleared otherwise.
 */
CallOutcome InvokeGuarded(const std::function<void()>& body, FaultInfo* fault = nullptr);

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_CALLGUARD_H_ */
