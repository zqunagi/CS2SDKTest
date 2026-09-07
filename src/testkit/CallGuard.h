/**
 * The wrapper every probing call goes through.
 *
 * Calling into an interface whose signature moved is not a graceful failure -
 * it is a segfault, or a return value read out of the wrong register, or a
 * silently corrupted stack. Since that is precisely what this plugin exists to
 * find, every call has to be made through one place that can eventually catch
 * it and turn it into a log line instead of a dead server.
 *
 * Right now that catching is NOT implemented: InvokeGuarded calls straight
 * through and IsGuardAvailable() returns false. The seam exists so the calls
 * being written today do not have to be rewritten when it lands.
 *
 * When it does land, the shape is:
 *   Windows - __try/__except around the invocation. Note that MSVC forbids
 *             objects with destructors in a function using SEH, which is why
 *             the callable is type-erased into std::function here rather than
 *             templated: the __try must sit in a helper that holds nothing.
 *   Linux   - a scoped SIGSEGV/SIGBUS/SIGILL handler plus sigsetjmp/siglongjmp.
 *             The engine installs its own handlers, so ours has to be saved
 *             and restored around each sweep rather than left in place.
 */

#ifndef CS2SDKTEST_TESTKIT_CALLGUARD_H_
#define CS2SDKTEST_TESTKIT_CALLGUARD_H_

#include <functional>

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

/** True once the platform fault guard is implemented and armed. */
bool IsGuardAvailable();

/**
 * Runs `body`, returning kOk normally or kCrashed if the guard caught a fault.
 * Without a guard a fault takes the process down, as it would anywhere else.
 */
CallOutcome InvokeGuarded(const std::function<void()>& body);

} // namespace cs2st

#endif /* CS2SDKTEST_TESTKIT_CALLGUARD_H_ */
