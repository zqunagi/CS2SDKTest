// REG_RIP and friends come from <ucontext.h> only under _GNU_SOURCE. The
// compilers define it for C++ on Linux already; this makes it independent of
// that, and has to precede the first system header.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "testkit/CrashRecorder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/FileSystemUtil.h"
#include "core/Log.h"
#include "core/Platform.h"

#include <atomic>

#if CS2ST_POSIX
#include <cerrno>
#include <csignal>
#include <ctime>

#include <execinfo.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#if CS2ST_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
	#include <ctime>
#endif

#include "testkit/CallGuard.h"

namespace cs2st
{

namespace
{

// State and helpers both platforms use. Everything the handlers touch is static and
// sized up front: they may be running because the heap is what broke.
constexpr int kMaxFrames = 48;
constexpr std::size_t kModuleLen = 256;

struct Frame
{
	const void* pc;
	unsigned long long base;
	char module[kModuleLen];
	bool resolved;
};

std::atomic<const char*> g_phase{"init"};
char g_path[1024];
char g_suite[128];
char g_call[192];

char g_report[32 * 1024];
char g_console[3 * 1024];
Frame g_frames[kMaxFrames];

/** Bounded, allocation-free text builder. Drops what does not fit. */
class Writer
{
public:
	Writer(char* buffer, std::size_t capacity) : buf_(buffer), cap_(capacity)
	{
		if (cap_ > 0)
		{
			buf_[0] = '\0';
		}
	}

	void Put(char c)
	{
		if (len_ + 1 < cap_)
		{
			buf_[len_++] = c;
			buf_[len_] = '\0';
		}
	}

	void Raw(const char* s)
	{
		for (; s != nullptr && *s != '\0'; ++s)
		{
			Put(*s);
		}
	}

	void Dec(long long value)
	{
		unsigned long long u = value < 0
			? 0ULL - static_cast<unsigned long long>(value)
			: static_cast<unsigned long long>(value);
		char digits[24];
		int n = 0;
		do
		{
			digits[n++] = static_cast<char>('0' + (u % 10));
			u /= 10;
		} while (u != 0);
		if (value < 0)
		{
			Put('-');
		}
		while (n > 0)
		{
			Put(digits[--n]);
		}
	}

	void Hex(unsigned long long value)
	{
		static const char kDigits[] = "0123456789abcdef";
		char digits[20];
		int n = 0;
		do
		{
			digits[n++] = kDigits[value & 0xF];
			value >>= 4;
		} while (value != 0);
		Raw("0x");
		while (n > 0)
		{
			Put(digits[--n]);
		}
	}

	/** A JSON string. Anything outside printable ASCII becomes '?'. */
	void Quoted(const char* s)
	{
		Put('"');
		for (; s != nullptr && *s != '\0'; ++s)
		{
			const unsigned char c = static_cast<unsigned char>(*s);
			if (c == '"' || c == '\\')
			{
				Put('\\');
				Put(static_cast<char>(c));
			}
			else if (c < 0x20 || c >= 0x7F)
			{
				Put('?');
			}
			else
			{
				Put(static_cast<char>(c));
			}
		}
		Put('"');
	}

	void QuotedHex(unsigned long long value)
	{
		Put('"');
		Hex(value);
		Put('"');
	}

	const char* data() const { return buf_; }
	std::size_t size() const { return len_; }

private:
	char* buf_;
	std::size_t cap_;
	std::size_t len_ = 0;
};

void CopyTruncated(char* destination, std::size_t capacity, const char* source)
{
	if (capacity == 0)
	{
		return;
	}
	std::size_t i = 0;
	if (source != nullptr)
	{
		for (; source[i] != '\0' && i + 1 < capacity; ++i)
		{
			destination[i] = source[i];
		}
	}
	destination[i] = '\0';
}

/** The last path component, whichever separator the platform uses. */
const char* BaseNameAny(const char* path)
{
	const char* base = path;
	for (const char* p = path; *p != '\0'; ++p)
	{
		if (*p == '/' || *p == '\\')
		{
			base = p + 1;
		}
	}
	return base;
}

} // namespace

#if CS2ST_POSIX

namespace
{

// Everything the handler touches is static and sized up front: it may be
// running because the heap is what broke.
const int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr int kSignalCount = static_cast<int>(sizeof(kSignals) / sizeof(kSignals[0]));

struct sigaction g_previous[kSignalCount];
std::atomic<bool> g_installed{false};

// The first thread to get here writes the report; everything after that only
// waits for it to finish and lets the process die.
std::atomic<int> g_entered{0};
std::atomic<int> g_done{0};

int IndexOf(int signal_number)
{
	for (int i = 0; i < kSignalCount; ++i)
	{
		if (kSignals[i] == signal_number)
		{
			return i;
		}
	}
	return -1;
}

const char* SignalName(int signal_number)
{
	switch (signal_number)
	{
		case SIGSEGV: return "SIGSEGV";
		case SIGBUS:  return "SIGBUS";
		case SIGILL:  return "SIGILL";
		case SIGFPE:  return "SIGFPE";
		case SIGABRT: return "SIGABRT";
		default:      return "signal";
	}
}

/** Signals that carry a meaningful faulting address in si_addr. */
bool HasFaultAddress(int signal_number)
{
	return signal_number == SIGSEGV || signal_number == SIGBUS
		|| signal_number == SIGILL || signal_number == SIGFPE;
}

const char* BaseName(const char* path)
{
	const char* base = path;
	for (const char* p = path; *p != '\0'; ++p)
	{
		if (*p == '/')
		{
			base = p + 1;
		}
	}
	return base;
}

void RestoreDefault(int signal_number)
{
	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_handler = SIG_DFL;
	sigemptyset(&action.sa_mask);
	::sigaction(signal_number, &action, nullptr);
}

// -----------------------------------------------------------------------------
//  Module lookup, from /proc/self/maps
// -----------------------------------------------------------------------------

unsigned long long ParseHex(const char*& p)
{
	unsigned long long value = 0;
	for (;; ++p)
	{
		const char c = *p;
		if (c >= '0' && c <= '9')
		{
			value = (value << 4) | static_cast<unsigned long long>(c - '0');
		}
		else if (c >= 'a' && c <= 'f')
		{
			value = (value << 4) | static_cast<unsigned long long>(c - 'a' + 10);
		}
		else if (c >= 'A' && c <= 'F')
		{
			value = (value << 4) | static_cast<unsigned long long>(c - 'A' + 10);
		}
		else
		{
			return value;
		}
	}
}

/**
 * One line of /proc/self/maps: `start-end perms offset dev inode   path`.
 *
 * A module's base is the start of its first mapping, which maps lists first
 * and with offset 0. Lines without a path (bss, anonymous) are skipped over
 * rather than ending the module, since they sit between a library's segments.
 */
void HandleMapsLine(const char* line, int frame_count,
	char* current_path, std::size_t current_capacity, unsigned long long* current_base)
{
	const char* p = line;
	const unsigned long long start = ParseHex(p);
	if (*p != '-')
	{
		return;
	}
	++p;
	const unsigned long long end = ParseHex(p);

	// perms, offset, dev, inode
	for (int field = 0; field < 4; ++field)
	{
		while (*p == ' ')
		{
			++p;
		}
		while (*p != '\0' && *p != ' ')
		{
			++p;
		}
	}
	while (*p == ' ')
	{
		++p;
	}
	const char* path = p;
	const bool is_file = (path[0] == '/');

	if (is_file && std::strcmp(path, current_path) != 0)
	{
		CopyTruncated(current_path, current_capacity, path);
		*current_base = start;
	}

	for (int i = 0; i < frame_count; ++i)
	{
		Frame& frame = g_frames[i];
		const unsigned long long pc = reinterpret_cast<unsigned long long>(frame.pc);
		if (frame.resolved || pc < start || pc >= end)
		{
			continue;
		}
		frame.resolved = true;
		if (is_file)
		{
			CopyTruncated(frame.module, kModuleLen, current_path);
			frame.base = *current_base;
		}
		else
		{
			// [stack], [heap], anonymous: say what it is rather than nothing.
			CopyTruncated(frame.module, kModuleLen, path);
			frame.base = 0;
		}
	}
}

void ResolveFrames(int frame_count)
{
	const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return;
	}

	char chunk[4096];
	char line[640];
	std::size_t line_length = 0;
	char current_path[kModuleLen];
	current_path[0] = '\0';
	unsigned long long current_base = 0;

	for (;;)
	{
		const ssize_t got = ::read(fd, chunk, sizeof(chunk));
		if (got < 0 && errno == EINTR)
		{
			continue;
		}
		if (got <= 0)
		{
			break;
		}
		for (ssize_t i = 0; i < got; ++i)
		{
			const char c = chunk[i];
			if (c != '\n')
			{
				if (line_length + 1 < sizeof(line))
				{
					line[line_length++] = c;
				}
				continue;
			}
			line[line_length] = '\0';
			HandleMapsLine(line, frame_count, current_path, sizeof(current_path), &current_base);
			line_length = 0;
		}
	}
	::close(fd);
}

// -----------------------------------------------------------------------------
//  The report
// -----------------------------------------------------------------------------

void AddFrame(int index, const void* pc)
{
	Frame& frame = g_frames[index];
	frame.pc = pc;
	frame.base = 0;
	frame.module[0] = '\0';
	frame.resolved = false;
}

struct ExecRange
{
	unsigned long long start;
	unsigned long long end;
};
ExecRange g_exec[768];

/** Executable mappings, so a stack word can be tested for "could be a return address". */
int CollectExecRanges()
{
	const int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		return 0;
	}
	int count = 0;
	char chunk[4096];
	char line[640];
	std::size_t length = 0;
	for (;;)
	{
		const ssize_t got = ::read(fd, chunk, sizeof(chunk));
		if (got < 0 && errno == EINTR)
		{
			continue;
		}
		if (got <= 0)
		{
			break;
		}
		for (ssize_t i = 0; i < got; ++i)
		{
			if (chunk[i] != 0x0A)
			{
				if (length + 1 < sizeof(line))
				{
					line[length++] = chunk[i];
				}
				continue;
			}
			line[length] = 0;
			length = 0;
			const char* p = line;
			const unsigned long long start = ParseHex(p);
			if (*p != '-')
			{
				continue;
			}
			++p;
			const unsigned long long end = ParseHex(p);
			while (*p == ' ')
			{
				++p;
			}
			// perms are "rwxp": the third character
			if (p[0] != 0 && p[1] != 0 && p[2] == 'x' && count < 768)
			{
				g_exec[count].start = start;
				g_exec[count].end = end;
				++count;
			}
		}
	}
	::close(fd);
	return count;
}

/**
 * Fallback for code without frame pointers or unwind data, where backtrace()
 * stops at the faulting function: look through the words just above the stack
 * pointer for values that point into executable memory. Those are mostly real
 * return addresses, with some stale ones mixed in, so it is a lead and not a proof.
 */
void ScanStack(void* context, int* frame_count)
{
#if defined(__linux__) && defined(__x86_64__)
	if (context == nullptr)
	{
		return;
	}
	const unsigned long long sp = static_cast<unsigned long long>(
		static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RSP]);
	if (sp == 0 || (sp & 7) != 0)
	{
		return;
	}
	const int ranges = CollectExecRanges();
	const unsigned long long* words = reinterpret_cast<const unsigned long long*>(sp);
	for (int i = 0; i < 192 && *frame_count < 16; ++i)
	{
		const unsigned long long value = words[i];
		for (int r = 0; r < ranges; ++r)
		{
			if (value >= g_exec[r].start && value < g_exec[r].end)
			{
				AddFrame((*frame_count)++, reinterpret_cast<const void*>(value));
				break;
			}
		}
	}
#else
	(void)context;
	(void)frame_count;
#endif
}

void WriteAll(int fd, const char* data, std::size_t size)
{
	while (size > 0)
	{
		const ssize_t written = ::write(fd, data, size);
		if (written < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return;
		}
		data += written;
		size -= static_cast<std::size_t>(written);
	}
}

void WriteReport(int signal_number, const siginfo_t* info, void* context)
{
	const void* pc = nullptr;
#if defined(__linux__) && defined(__x86_64__)
	if (context != nullptr)
	{
		pc = reinterpret_cast<const void*>(static_cast<unsigned long long>(
			static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]));
	}
#else
	(void)context;
#endif

	// The unwinder starts inside this handler, so the first frames are ours and
	// the signal trampoline. Start at the frame that is the interrupted pc; if
	// it is not found (a jump to garbage), say where it died and skip the two.
	void* raw[kMaxFrames + 4];
	const int raw_count = ::backtrace(raw, kMaxFrames + 4);
	int first = -1;
	if (pc != nullptr)
	{
		for (int i = 0; i < raw_count; ++i)
		{
			if (raw[i] == pc)
			{
				first = i;
				break;
			}
		}
	}
	int frame_count = 0;
	if (first >= 0)
	{
		for (int i = first; i < raw_count && frame_count < kMaxFrames; ++i)
		{
			AddFrame(frame_count++, raw[i]);
		}
	}
	else
	{
		if (pc != nullptr)
		{
			AddFrame(frame_count++, pc);
		}
		for (int i = (raw_count > 2 ? 2 : 0); i < raw_count && frame_count < kMaxFrames; ++i)
		{
			AddFrame(frame_count++, raw[i]);
		}
	}
	if (frame_count < 4)
	{
		ScanStack(context, &frame_count);
	}
	ResolveFrames(frame_count);

	const bool has_address = (info != nullptr) && HasFaultAddress(signal_number);
	const unsigned long long address = has_address
		? reinterpret_cast<unsigned long long>(info->si_addr) : 0ULL;
	const char* phase = g_phase.load();

	Writer json(g_report, sizeof(g_report));
	json.Raw("{\"version\":1,\"signal\":");
	json.Dec(signal_number);
	json.Raw(",\"signal_name\":");
	json.Quoted(SignalName(signal_number));
	json.Raw(",\"si_code\":");
	json.Dec(info != nullptr ? info->si_code : 0);
	if (has_address)
	{
		json.Raw(",\"fault_address\":");
		json.QuotedHex(address);
	}
	if (pc != nullptr)
	{
		json.Raw(",\"pc\":");
		json.QuotedHex(reinterpret_cast<unsigned long long>(pc));
	}
	json.Raw(",\"pid\":");
	json.Dec(static_cast<long long>(::getpid()));
	json.Raw(",\"tid\":");
	json.Dec(static_cast<long long>(::syscall(SYS_gettid)));
	json.Raw(",\"time\":");
	json.Dec(static_cast<long long>(::time(nullptr)));
	json.Raw(",\"phase\":");
	json.Quoted(phase);
	if (g_call[0] != '\0')
	{
		json.Raw(",\"call\":{\"suite\":");
		json.Quoted(g_suite);
		json.Raw(",\"call\":");
		json.Quoted(g_call);
		json.Put('}');
	}
	json.Raw(",\"frames\":[");
	for (int i = 0; i < frame_count; ++i)
	{
		const Frame& frame = g_frames[i];
		if (i > 0)
		{
			json.Put(',');
		}
		json.Raw("{\"pc\":");
		json.QuotedHex(reinterpret_cast<unsigned long long>(frame.pc));
		if (frame.module[0] != '\0')
		{
			json.Raw(",\"module\":");
			json.Quoted(frame.module);
			if (frame.base != 0)
			{
				json.Raw(",\"offset\":");
				json.QuotedHex(reinterpret_cast<unsigned long long>(frame.pc) - frame.base);
			}
		}
		json.Put('}');
	}
	json.Raw("]}\n");

	const int fd = ::open(g_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd >= 0)
	{
		WriteAll(fd, json.data(), json.size());
		::close(fd);
	}

	// The same facts on the console, so console.log tells the story on its own
	// when the file could not be written (or the reader only has the log).
	Writer text(g_console, sizeof(g_console));
	text.Raw("[SDKTEST] FATAL ");
	text.Raw(SignalName(signal_number));
	text.Raw(" (si_code ");
	text.Dec(info != nullptr ? info->si_code : 0);
	text.Put(')');
	if (has_address)
	{
		text.Raw(" at ");
		text.Hex(address);
	}
	text.Raw(", plugin phase: ");
	text.Raw(phase);
	if (g_call[0] != '\0')
	{
		text.Raw(", call: ");
		text.Raw(g_suite);
		text.Raw(" / ");
		text.Raw(g_call);
	}
	text.Put('\n');
	for (int i = 0; i < frame_count && i < 12; ++i)
	{
		const Frame& frame = g_frames[i];
		text.Raw("[SDKTEST]   #");
		text.Dec(i);
		text.Put(' ');
		if (frame.module[0] != '\0')
		{
			text.Raw(BaseName(frame.module));
			if (frame.base != 0)
			{
				text.Raw("+");
				text.Hex(reinterpret_cast<unsigned long long>(frame.pc) - frame.base);
			}
		}
		else
		{
			text.Hex(reinterpret_cast<unsigned long long>(frame.pc));
		}
		text.Put('\n');
	}
	WriteAll(2, text.data(), text.size());
}

void FatalHandler(int signal_number, siginfo_t* info, void* context);

/** Hands the signal on to whoever had it before us, or lets the default happen. */
void Chain(int index, int signal_number, siginfo_t* info, void* context)
{
	const struct sigaction& previous = g_previous[index];

	if ((previous.sa_flags & SA_SIGINFO) != 0)
	{
		if (previous.sa_sigaction != nullptr && previous.sa_sigaction != &FatalHandler)
		{
			previous.sa_sigaction(signal_number, info, context);
			return;
		}
	}
	else if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN
		&& previous.sa_handler != nullptr)
	{
		previous.sa_handler(signal_number);
		return;
	}

	// Default, or ignored: ignoring a synchronous fault would just re-fault
	// forever, so both mean "die the normal way". The instruction re-executes
	// when this returns, and now lands in the default action.
	RestoreDefault(signal_number);
}

void FatalHandler(int signal_number, siginfo_t* info, void* context)
{
	const int saved_errno = errno;
	const int index = IndexOf(signal_number);

	int expected = 0;
	if (g_entered.compare_exchange_strong(expected, 1))
	{
		WriteReport(signal_number, info, context);
		g_done.store(1);
		if (index >= 0)
		{
			Chain(index, signal_number, info, context);
		}
		else
		{
			RestoreDefault(signal_number);
		}
		errno = saved_errno;
		return;
	}

	// Already reported: either another thread is writing it right now (give it
	// a moment, dying first would cut the file short), or the chain above came
	// back around to us. Either way the process should now die for real.
	for (int waited = 0; waited < 200 && g_done.load() == 0; ++waited)
	{
		struct timespec pause;
		pause.tv_sec = 0;
		pause.tv_nsec = 5 * 1000 * 1000;
		::nanosleep(&pause, nullptr);
	}
	RestoreDefault(signal_number);
	errno = saved_errno;
}

} // namespace

#endif // CS2ST_POSIX

#if CS2ST_WINDOWS

namespace
{

std::atomic<bool> g_installed{false};
std::atomic<int> g_first_done{0};      // the vectored handler has written
std::atomic<int> g_unhandled_done{0};  // the unhandled-exception filter has written
PVOID g_veh = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = nullptr;
char g_log_path[1024];

bool IsFatalCode(DWORD code)
{
	switch (code)
	{
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_IN_PAGE_ERROR:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_STACK_OVERFLOW:
		case 0xC0000374: // STATUS_HEAP_CORRUPTION
		case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN
			return true;
		default:
			return false;
	}
}

const char* CodeName(DWORD code)
{
	switch (code)
	{
		case EXCEPTION_ACCESS_VIOLATION:    return "ACCESS_VIOLATION";
		case EXCEPTION_IN_PAGE_ERROR:       return "IN_PAGE_ERROR";
		case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
		case EXCEPTION_PRIV_INSTRUCTION:    return "PRIV_INSTRUCTION";
		case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "INT_DIVIDE_BY_ZERO";
		case EXCEPTION_STACK_OVERFLOW:      return "STACK_OVERFLOW";
		case 0xC0000374:                    return "HEAP_CORRUPTION";
		case 0xC0000409:                    return "STACK_BUFFER_OVERRUN";
		default:                            return "EXCEPTION";
	}
}

// A separate function: __try cannot share one with objects that need unwinding.
bool ReadQword(const void* address, DWORD64* value)
{
	__try
	{
		*value = *static_cast<const DWORD64*>(address);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

/**
 * Walks the stack from the faulting context with the unwind tables (.pdata), the
 * same data the OS uses, so frames are right without frame pointers. A function
 * with no entry is a leaf: its return address is at the top of the stack.
 */
int CaptureFrames(const CONTEXT& start)
{
#if defined(_M_X64)
	CONTEXT context = start;
	int count = 0;
	while (count < kMaxFrames)
	{
		const DWORD64 pc = context.Rip;
		if (pc == 0)
		{
			break;
		}

		Frame& frame = g_frames[count++];
		frame.pc = reinterpret_cast<const void*>(pc);
		frame.base = 0;
		frame.module[0] = '\0';
		frame.resolved = true;
		PVOID base = nullptr;
		if (::RtlPcToFileHeader(reinterpret_cast<PVOID>(pc), &base) != nullptr && base != nullptr)
		{
			frame.base = reinterpret_cast<unsigned long long>(base);
			char path[MAX_PATH];
			const DWORD length = ::GetModuleFileNameA(static_cast<HMODULE>(base), path, MAX_PATH);
			if (length > 0 && length < MAX_PATH)
			{
				CopyTruncated(frame.module, kModuleLen, path);
			}
		}

		DWORD64 image_base = 0;
		PRUNTIME_FUNCTION entry = ::RtlLookupFunctionEntry(pc, &image_base, nullptr);
		if (entry == nullptr)
		{
			DWORD64 return_address = 0;
			if (context.Rsp == 0 || !ReadQword(reinterpret_cast<const void*>(context.Rsp), &return_address))
			{
				break;
			}
			context.Rip = return_address;
			context.Rsp += 8;
		}
		else
		{
			PVOID handler_data = nullptr;
			DWORD64 establisher = 0;
			const DWORD64 previous_sp = context.Rsp;
			::RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, entry, &context,
				&handler_data, &establisher, nullptr);
			if (context.Rsp <= previous_sp && context.Rip == pc)
			{
				break; // no progress
			}
		}
	}
	return count;
#else
	(void)start;
	return 0;
#endif
}

void WriteToFile(const char* path, const char* data, std::size_t size, bool append)
{
	const HANDLE file = ::CreateFileA(path,
		append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return;
	}
	DWORD written = 0;
	::WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr);
	::CloseHandle(file);
}

void WriteReport(const EXCEPTION_POINTERS* info, bool unhandled)
{
	const EXCEPTION_RECORD& record = *info->ExceptionRecord;
	const DWORD code = record.ExceptionCode;
	const bool is_access = (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
		&& record.NumberParameters >= 2;
	const unsigned long long address = is_access ? static_cast<unsigned long long>(record.ExceptionInformation[1]) : 0ULL;
	const unsigned long long pc = reinterpret_cast<unsigned long long>(record.ExceptionAddress);
	const char* phase = g_phase.load();

	const int frame_count = CaptureFrames(*info->ContextRecord);

	Writer json(g_report, sizeof(g_report));
	json.Raw("{\"version\":1,\"signal\":");
	json.Dec(static_cast<long long>(code));
	json.Raw(",\"signal_name\":");
	json.Quoted(CodeName(code));
	json.Raw(",\"exception_code\":");
	json.QuotedHex(code);
	// false: seen first, before anything had a chance to handle it, so a handler
	// further up could still have dealt with it. true: nothing did.
	json.Raw(",\"unhandled\":");
	json.Raw(unhandled ? "true" : "false");
	if (is_access)
	{
		json.Raw(",\"access\":");
		json.Quoted(record.ExceptionInformation[0] == 0 ? "read"
			: (record.ExceptionInformation[0] == 1 ? "write" : "execute"));
		json.Raw(",\"fault_address\":");
		json.QuotedHex(address);
	}
	json.Raw(",\"pc\":");
	json.QuotedHex(pc);
	json.Raw(",\"pid\":");
	json.Dec(static_cast<long long>(::GetCurrentProcessId()));
	json.Raw(",\"tid\":");
	json.Dec(static_cast<long long>(::GetCurrentThreadId()));
	json.Raw(",\"time\":");
	json.Dec(static_cast<long long>(::time(nullptr)));
	json.Raw(",\"phase\":");
	json.Quoted(phase);
	if (g_call[0] != '\0')
	{
		json.Raw(",\"call\":{\"suite\":");
		json.Quoted(g_suite);
		json.Raw(",\"call\":");
		json.Quoted(g_call);
		json.Put('}');
	}
	json.Raw(",\"frames\":[");
	for (int i = 0; i < frame_count; ++i)
	{
		const Frame& frame = g_frames[i];
		if (i > 0)
		{
			json.Put(',');
		}
		json.Raw("{\"pc\":");
		json.QuotedHex(reinterpret_cast<unsigned long long>(frame.pc));
		if (frame.module[0] != '\0')
		{
			json.Raw(",\"module\":");
			json.Quoted(frame.module);
			if (frame.base != 0)
			{
				json.Raw(",\"offset\":");
				json.QuotedHex(reinterpret_cast<unsigned long long>(frame.pc) - frame.base);
			}
		}
		json.Put('}');
	}
	json.Raw("]}\n");
	WriteToFile(g_path, json.data(), json.size(), false);

	// The same facts as text, in the run log and on stderr: the dedicated server's
	// console window closes with the process, so the log file is what survives.
	Writer text(g_console, sizeof(g_console));
	text.Raw("[SDKTEST] FATAL ");
	text.Raw(CodeName(code));
	text.Raw(" (");
	text.Hex(code);
	text.Put(')');
	if (is_access)
	{
		text.Raw(record.ExceptionInformation[0] == 0 ? " reading " : (record.ExceptionInformation[0] == 1 ? " writing " : " executing "));
		text.Hex(address);
	}
	text.Raw(" at ");
	text.Hex(pc);
	text.Raw(unhandled ? ", unhandled" : ", first chance");
	text.Raw(", phase: ");
	text.Raw(phase);
	if (g_call[0] != '\0')
	{
		text.Raw(", call: ");
		text.Raw(g_suite);
		text.Raw(" / ");
		text.Raw(g_call);
	}
	text.Put('\n');
	for (int i = 0; i < frame_count && i < 12; ++i)
	{
		const Frame& frame = g_frames[i];
		text.Raw("[SDKTEST]   #");
		text.Dec(i);
		text.Put(' ');
		if (frame.module[0] != '\0')
		{
			text.Raw(BaseNameAny(frame.module));
			text.Raw("+");
			text.Hex(reinterpret_cast<unsigned long long>(frame.pc) - frame.base);
		}
		else
		{
			text.Hex(reinterpret_cast<unsigned long long>(frame.pc));
		}
		text.Put('\n');
	}
	if (g_log_path[0] != '\0')
	{
		WriteToFile(g_log_path, text.data(), text.size(), true);
	}
	const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
	if (err != nullptr && err != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		::WriteFile(err, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
	}
}

/**
 * Sees every exception first, including the ones a __try/__except further down
 * will handle. So it only reports a fatal-class one that happens outside a
 * CallGuard scope, where a sweep call's faults are caught on purpose and are not
 * crashes. It never handles anything: the search continues.
 */
LONG CALLBACK VectoredHandler(PEXCEPTION_POINTERS info)
{
	if (IsFatalCode(info->ExceptionRecord->ExceptionCode) && !FaultGuardScope::IsActive())
	{
		int expected = 0;
		if (g_first_done.compare_exchange_strong(expected, 1))
		{
			WriteReport(info, false);
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

/** Reached only when nothing handled the exception: the process is about to die. */
LONG WINAPI UnhandledFilter(PEXCEPTION_POINTERS info)
{
	int expected = 0;
	if (g_unhandled_done.compare_exchange_strong(expected, 1))
	{
		WriteReport(info, true);
	}
	if (g_previous_filter != nullptr && g_previous_filter != &UnhandledFilter)
	{
		return g_previous_filter(info);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

#endif // CS2ST_WINDOWS

// -----------------------------------------------------------------------------
//  Public interface
// -----------------------------------------------------------------------------

#if CS2ST_POSIX

bool CrashRecorder::Install()
{
	if (g_installed.load())
	{
		return true;
	}

	const char* env_dir = std::getenv("CST_RESULT_DIR");
	std::string directory = (env_dir != nullptr && env_dir[0] != '\0')
		? std::string(env_dir) : fs::LogDirectory();
	if (!fs::CreateDirectories(directory))
	{
		CS2ST_LOG_WARN("crash recorder: cannot create '%s'; a crash will not be recorded\n",
			directory.c_str());
		return false;
	}

	const std::string path = fs::Join(directory, "crash.json");
	if (path.size() >= sizeof(g_path))
	{
		CS2ST_LOG_WARN("crash recorder: path too long; a crash will not be recorded\n");
		return false;
	}
	std::memcpy(g_path, path.c_str(), path.size() + 1);

	// Whatever is there is from an earlier run; "no crash.json" has to mean none.
	std::remove(g_path);

	// backtrace() loads libgcc on its first call, which is not something to be
	// doing from a signal handler.
	void* warm[8];
	::backtrace(warm, 8);

	g_entered.store(0);
	g_done.store(0);

	struct sigaction action;
	std::memset(&action, 0, sizeof(action));
	action.sa_sigaction = &FatalHandler;
	// SA_ONSTACK only has an effect on threads that have an alternate stack
	// (CallGuard sets one during a sweep); it makes a stack overflow reportable.
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset(&action.sa_mask);

	for (int i = 0; i < kSignalCount; ++i)
	{
		if (::sigaction(kSignals[i], &action, &g_previous[i]) != 0)
		{
			for (int j = 0; j < i; ++j)
			{
				::sigaction(kSignals[j], &g_previous[j], nullptr);
			}
			CS2ST_LOG_WARN("crash recorder: sigaction failed; a crash will not be recorded\n");
			return false;
		}
	}

	g_installed.store(true);
	CS2ST_LOG_INFO("crash recorder: armed, a fatal signal writes %s\n", g_path);
	return true;
}

void CrashRecorder::Uninstall()
{
	if (!g_installed.exchange(false))
	{
		return;
	}

	// Only take our handler out if it is still the current one. If the engine
	// installed its own on top and saved ours as "previous", pulling ours out
	// from under it would not help and putting the old one back would drop theirs.
	for (int i = 0; i < kSignalCount; ++i)
	{
		struct sigaction current;
		if (::sigaction(kSignals[i], nullptr, &current) == 0
			&& (current.sa_flags & SA_SIGINFO) != 0
			&& current.sa_sigaction == &FatalHandler)
		{
			::sigaction(kSignals[i], &g_previous[i], nullptr);
		}
	}
}

void CrashRecorder::SetPhase(const char* phase)
{
	g_phase.store(phase != nullptr ? phase : "");
}

void CrashRecorder::SetCall(const char* suite, const char* call)
{
	// Not atomic: a crash racing this may see a suite from one call and the name
	// of the next. The call name is written last, and it is the one that counts.
	CopyTruncated(g_suite, sizeof(g_suite), suite);
	CopyTruncated(g_call, sizeof(g_call), call);
}

void CrashRecorder::ClearCall()
{
	g_call[0] = '\0';
	g_suite[0] = '\0';
}

#else // CS2ST_WINDOWS

bool CrashRecorder::Install()
{
	if (g_installed.load())
	{
		return true;
	}

	const char* env_dir = std::getenv("CST_RESULT_DIR");
	std::string directory = (env_dir != nullptr && env_dir[0] != '\0')
		? std::string(env_dir) : fs::LogDirectory();
	if (!fs::CreateDirectories(directory))
	{
		CS2ST_LOG_WARN("crash recorder: cannot create '%s'; a crash will not be recorded\n",
			directory.c_str());
		return false;
	}

	const std::string path = fs::Join(directory, "crash.json");
	if (path.size() >= sizeof(g_path))
	{
		CS2ST_LOG_WARN("crash recorder: path too long; a crash will not be recorded\n");
		return false;
	}
	std::memcpy(g_path, path.c_str(), path.size() + 1);
	CopyTruncated(g_log_path, sizeof(g_log_path), SessionLogPath().c_str());

	// Whatever is there is from an earlier run; "no crash.json" has to mean none.
	std::remove(g_path);

	g_first_done.store(0);
	g_unhandled_done.store(0);

	g_veh = ::AddVectoredExceptionHandler(1, &VectoredHandler);
	g_previous_filter = ::SetUnhandledExceptionFilter(&UnhandledFilter);
	if (g_veh == nullptr)
	{
		CS2ST_LOG_WARN("crash recorder: AddVectoredExceptionHandler failed; only unhandled exceptions "
		               "will be recorded\n");
	}

	g_installed.store(true);
	CS2ST_LOG_INFO("crash recorder: armed, a fatal exception writes %s\n", g_path);
	return true;
}

void CrashRecorder::Uninstall()
{
	if (!g_installed.exchange(false))
	{
		return;
	}
	if (g_veh != nullptr)
	{
		::RemoveVectoredExceptionHandler(g_veh);
		g_veh = nullptr;
	}
	// Only take our filter out if it is still the current one. If the engine
	// installed its own on top, theirs stays.
	LPTOP_LEVEL_EXCEPTION_FILTER current = ::SetUnhandledExceptionFilter(g_previous_filter);
	if (current != &UnhandledFilter)
	{
		::SetUnhandledExceptionFilter(current);
	}
}

void CrashRecorder::SetPhase(const char* phase)
{
	g_phase.store(phase != nullptr ? phase : "");
}

void CrashRecorder::SetCall(const char* suite, const char* call)
{
	CopyTruncated(g_suite, sizeof(g_suite), suite);
	CopyTruncated(g_call, sizeof(g_call), call);
}

void CrashRecorder::ClearCall()
{
	g_call[0] = '\0';
	g_suite[0] = '\0';
}

#endif // CS2ST_POSIX / CS2ST_WINDOWS

} // namespace cs2st
