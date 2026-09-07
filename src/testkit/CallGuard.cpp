#include "testkit/CallGuard.h"

#include <cstdio>
#include <cstring>

#include "core/Platform.h"

#if CS2ST_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
	#include <malloc.h> // _resetstkoflw
#else
	#include <csetjmp>
	#include <csignal>
#endif

namespace cs2st
{
namespace
{

/** Plain data, so it can be filled from inside an SEH filter or a signal handler. */
struct RawFault
{
	unsigned long code = 0;
	const void* instruction = nullptr;
	const void* address = nullptr;
	bool has_address = false;
	int access = -1; // 0 read, 1 write, 8 execute (Windows only)
};

int g_scope_depth = 0;

// -----------------------------------------------------------------------------
//  Windows
// -----------------------------------------------------------------------------
#if CS2ST_WINDOWS

const char* SehCodeName(unsigned long code)
{
	switch (code)
	{
		case EXCEPTION_ACCESS_VIOLATION:         return "ACCESS_VIOLATION";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "ARRAY_BOUNDS_EXCEEDED";
		case EXCEPTION_DATATYPE_MISALIGNMENT:    return "DATATYPE_MISALIGNMENT";
		case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "FLT_DIVIDE_BY_ZERO";
		case EXCEPTION_FLT_INVALID_OPERATION:    return "FLT_INVALID_OPERATION";
		case EXCEPTION_ILLEGAL_INSTRUCTION:      return "ILLEGAL_INSTRUCTION";
		case EXCEPTION_IN_PAGE_ERROR:            return "IN_PAGE_ERROR";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "INT_DIVIDE_BY_ZERO";
		case EXCEPTION_PRIV_INSTRUCTION:         return "PRIV_INSTRUCTION";
		case EXCEPTION_STACK_OVERFLOW:           return "STACK_OVERFLOW";
		case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
		default:                                 return "EXCEPTION";
	}
}

// C++ exceptions are thrown with this code by the MSVC runtime. They are a
// normal control-flow mechanism, not a fault, and swallowing them here would
// hide real errors from the code that meant to catch them.
constexpr unsigned long kCppExceptionCode = 0xE06D7363u;

int SehFilter(unsigned long code, EXCEPTION_POINTERS* pointers, RawFault* out)
{
	if (code == kCppExceptionCode ||
		code == EXCEPTION_BREAKPOINT ||
		code == EXCEPTION_SINGLE_STEP)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}

	out->code = code;

	if (pointers != nullptr && pointers->ExceptionRecord != nullptr)
	{
		const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
		out->instruction = record->ExceptionAddress;

		// For access violations the first parameter is the access type and the
		// second is the address that was touched.
		if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
			record->NumberParameters >= 2)
		{
			out->access = static_cast<int>(record->ExceptionInformation[0]);
			out->address = reinterpret_cast<const void*>(record->ExceptionInformation[1]);
			out->has_address = true;
		}
	}

	return EXCEPTION_EXECUTE_HANDLER;
}

// Kept out of line so the std::function call cannot be inlined into the __try
// frame: MSVC rejects a function that both uses SEH and holds objects needing
// unwinding (C2712), and an inlined operator() would introduce them.
#if defined(_MSC_VER)
__declspec(noinline)
#endif
void CallBody(const std::function<void()>& body)
{
	body();
}

// This function must contain nothing that requires unwinding - see above.
bool InvokeWithSeh(const std::function<void()>& body, RawFault* fault)
{
	__try
	{
		CallBody(body);
		return true;
	}
	__except (SehFilter(GetExceptionCode(), GetExceptionInformation(), fault))
	{
		return false;
	}
}

std::string DescribeFault(const RawFault& fault)
{
	char buffer[256];

	if (fault.has_address)
	{
		const char* verb = "accessing";
		if (fault.access == 0) verb = "reading";
		else if (fault.access == 1) verb = "writing";
		else if (fault.access == 8) verb = "executing";

		std::snprintf(buffer, sizeof(buffer), "%s %s %p at %p",
			SehCodeName(fault.code), verb, fault.address, fault.instruction);
	}
	else
	{
		std::snprintf(buffer, sizeof(buffer), "%s (0x%08lx) at %p",
			SehCodeName(fault.code), fault.code, fault.instruction);
	}

	return buffer;
}

// -----------------------------------------------------------------------------
//  POSIX
// -----------------------------------------------------------------------------
#else

const int kGuardedSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
constexpr std::size_t kGuardedSignalCount =
	sizeof(kGuardedSignals) / sizeof(kGuardedSignals[0]);

struct sigaction g_previous[kGuardedSignalCount];
stack_t g_previous_altstack;
bool g_altstack_installed = false;

// A fault that happens because the stack is exhausted cannot run a handler on
// that same stack, so the handler gets its own.
constexpr std::size_t kAltStackSize = 64 * 1024;
char g_altstack_memory[kAltStackSize];

// Per-thread: another thread faulting while this one is armed must not be
// hijacked into this thread's jump buffer.
thread_local sigjmp_buf t_jump;
thread_local volatile sig_atomic_t t_armed = 0;

// Written by the handler, read after siglongjmp. These have to survive the
// jump, and local variables are not guaranteed to - the compiler may have kept
// them in registers that siglongjmp restores to their sigsetjmp-time values.
thread_local RawFault t_fault;
thread_local FaultInfo* t_fault_out = nullptr;

int SignalIndex(int signal_number)
{
	for (std::size_t i = 0; i < kGuardedSignalCount; ++i)
	{
		if (kGuardedSignals[i] == signal_number)
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

/** Hands a signal we did not arm for back to whoever had the handler. */
void ForwardToPrevious(int signal_number, siginfo_t* info, void* ucontext)
{
	const int index = SignalIndex(signal_number);
	if (index < 0)
	{
		return;
	}

	const struct sigaction& previous = g_previous[index];

	if ((previous.sa_flags & SA_SIGINFO) != 0)
	{
		if (previous.sa_sigaction != nullptr)
		{
			previous.sa_sigaction(signal_number, info, ucontext);
		}
		return;
	}

	if (previous.sa_handler == SIG_IGN)
	{
		return;
	}
	if (previous.sa_handler == SIG_DFL)
	{
		// Put the default back and let the instruction re-fault into it, so the
		// process dies the way it would have without us in the way.
		struct sigaction restore;
		std::memset(&restore, 0, sizeof(restore));
		restore.sa_handler = SIG_DFL;
		sigemptyset(&restore.sa_mask);
		sigaction(signal_number, &restore, nullptr);
		return;
	}
	if (previous.sa_handler != nullptr)
	{
		previous.sa_handler(signal_number);
	}
}

void FaultHandler(int signal_number, siginfo_t* info, void* ucontext)
{
	if (t_armed == 0)
	{
		ForwardToPrevious(signal_number, info, ucontext);
		return;
	}

	t_armed = 0;
	t_fault.code = static_cast<unsigned long>(signal_number);
	t_fault.address = (info != nullptr) ? info->si_addr : nullptr;
	t_fault.has_address = (info != nullptr);
	t_fault.instruction = nullptr;

	// Jumping out of a signal handler is not something the standard blesses,
	// but for a synchronous fault raised by the faulting instruction itself it
	// is the established technique: the kernel has not retried the instruction
	// yet, and sigsetjmp(buf, 1) means siglongjmp restores the signal mask -
	// without which SIGSEGV would stay blocked and the next fault would kill
	// the process outright.
	siglongjmp(t_jump, 1);
}

bool InstallHandlers()
{
	stack_t alt;
	std::memset(&alt, 0, sizeof(alt));
	alt.ss_sp = g_altstack_memory;
	alt.ss_size = sizeof(g_altstack_memory);
	alt.ss_flags = 0;
	g_altstack_installed = (sigaltstack(&alt, &g_previous_altstack) == 0);

	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_sigaction = &FaultHandler;
	action.sa_flags = SA_SIGINFO | SA_RESTART;
	if (g_altstack_installed)
	{
		action.sa_flags |= SA_ONSTACK;
	}
	sigemptyset(&action.sa_mask);

	for (std::size_t i = 0; i < kGuardedSignalCount; ++i)
	{
		if (sigaction(kGuardedSignals[i], &action, &g_previous[i]) != 0)
		{
			// Put back whatever was already replaced rather than leaving the
			// process half-guarded.
			for (std::size_t j = 0; j < i; ++j)
			{
				sigaction(kGuardedSignals[j], &g_previous[j], nullptr);
			}
			if (g_altstack_installed)
			{
				sigaltstack(&g_previous_altstack, nullptr);
				g_altstack_installed = false;
			}
			return false;
		}
	}

	return true;
}

void RestoreHandlers()
{
	for (std::size_t i = 0; i < kGuardedSignalCount; ++i)
	{
		sigaction(kGuardedSignals[i], &g_previous[i], nullptr);
	}
	if (g_altstack_installed)
	{
		sigaltstack(&g_previous_altstack, nullptr);
		g_altstack_installed = false;
	}
}

std::string DescribeFault(const RawFault& fault)
{
	const char* name = "signal";
	switch (static_cast<int>(fault.code))
	{
		case SIGSEGV: name = "SIGSEGV"; break;
		case SIGBUS:  name = "SIGBUS";  break;
		case SIGILL:  name = "SIGILL";  break;
		case SIGFPE:  name = "SIGFPE";  break;
		default: break;
	}

	char buffer[256];
	if (fault.has_address)
	{
		std::snprintf(buffer, sizeof(buffer), "%s at %p", name, fault.address);
	}
	else
	{
		std::snprintf(buffer, sizeof(buffer), "%s (%lu)", name, fault.code);
	}
	return buffer;
}

#endif // CS2ST_WINDOWS

} // namespace

const char* CallOutcomeName(CallOutcome outcome)
{
	switch (outcome)
	{
		case CallOutcome::kOk:          return "OK";
		case CallOutcome::kCrashed:     return "CRASHED";
		case CallOutcome::kSkipped:     return "SKIP";
		case CallOutcome::kUnavailable: return "N/A";
		case CallOutcome::kFailed:      return "FAIL";
		case CallOutcome::kSuspect:     return "SUSPECT";
		default:                        return "?";
	}
}

const char* GuardDescription()
{
#if CS2ST_WINDOWS
	return "SEH __try/__except (always in effect)";
#else
	return "SIGSEGV/SIGBUS/SIGILL/SIGFPE + siglongjmp (armed during a sweep)";
#endif
}

bool IsGuardAvailable()
{
#if CS2ST_WINDOWS
	// SEH needs nothing installed; it is always in effect.
	return true;
#else
	return g_scope_depth > 0;
#endif
}

// -----------------------------------------------------------------------------
//  FaultGuardScope
// -----------------------------------------------------------------------------

FaultGuardScope::FaultGuardScope()
{
#if !CS2ST_WINDOWS
	if (g_scope_depth == 0)
	{
		if (!InstallHandlers())
		{
			return;
		}
	}
	++g_scope_depth;
	installed_ = true;
#else
	// Nothing to install; SEH is per-frame.
	++g_scope_depth;
	installed_ = true;
#endif
}

FaultGuardScope::~FaultGuardScope()
{
	if (!installed_)
	{
		return;
	}

	--g_scope_depth;

#if !CS2ST_WINDOWS
	if (g_scope_depth == 0)
	{
		RestoreHandlers();
	}
#endif
}

bool FaultGuardScope::IsActive()
{
	return g_scope_depth > 0;
}

// -----------------------------------------------------------------------------
//  InvokeGuarded
// -----------------------------------------------------------------------------

CallOutcome InvokeGuarded(const std::function<void()>& body, FaultInfo* fault)
{
	if (fault != nullptr)
	{
		*fault = FaultInfo();
	}
	if (!body)
	{
		return CallOutcome::kSkipped;
	}

#if CS2ST_WINDOWS

	RawFault raw;
	if (InvokeWithSeh(body, &raw))
	{
		return CallOutcome::kOk;
	}

	// After a stack overflow the guard page is gone; without resetting it the
	// next overflow terminates the process instead of raising again.
	if (raw.code == EXCEPTION_STACK_OVERFLOW)
	{
		_resetstkoflw();
	}

	if (fault != nullptr)
	{
		fault->faulted = true;
		fault->code = raw.code;
		fault->instruction = raw.instruction;
		fault->address = raw.address;
		fault->description = DescribeFault(raw);
	}
	return CallOutcome::kCrashed;

#else

	if (!FaultGuardScope::IsActive())
	{
		// No scope, no handlers - run it bare rather than pretending.
		body();
		return CallOutcome::kOk;
	}

	// Reached through the pointer rather than the parameter, because after
	// siglongjmp the parameter's value is not guaranteed to be intact.
	t_fault_out = fault;
	t_fault = RawFault();

	if (sigsetjmp(t_jump, 1) == 0)
	{
		t_armed = 1;
		body();
		t_armed = 0;
		t_fault_out = nullptr;
		return CallOutcome::kOk;
	}

	// Back here from the handler. Nothing between the fault and this point was
	// unwound: destructors were skipped, so whatever the call held is leaked.
	t_armed = 0;

	FaultInfo* out = t_fault_out;
	t_fault_out = nullptr;

	if (out != nullptr)
	{
		out->faulted = true;
		out->code = t_fault.code;
		out->instruction = t_fault.instruction;
		out->address = t_fault.address;
		out->description = DescribeFault(t_fault);
	}
	return CallOutcome::kCrashed;

#endif
}

} // namespace cs2st
