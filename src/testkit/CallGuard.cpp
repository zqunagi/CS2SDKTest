#include "testkit/CallGuard.h"

namespace cs2st
{

const char* CallOutcomeName(CallOutcome outcome)
{
	switch (outcome)
	{
		case CallOutcome::kOk:          return "OK";
		case CallOutcome::kCrashed:     return "CRASHED";
		case CallOutcome::kSkipped:     return "SKIP";
		case CallOutcome::kUnavailable: return "N/A";
		case CallOutcome::kFailed:      return "FAIL";
		default:                        return "?";
	}
}

bool IsGuardAvailable()
{
	// Flip to true along with a real implementation below. Callers use this to
	// warn before a sweep that a fault will take the server with it.
	return false;
}

CallOutcome InvokeGuarded(const std::function<void()>& body)
{
	if (!body)
	{
		return CallOutcome::kSkipped;
	}

	// Pass-through for now. See CallGuard.h for the intended SEH / sigsetjmp
	// implementations; the signature is already the one they need.
	body();
	return CallOutcome::kOk;
}

} // namespace cs2st
