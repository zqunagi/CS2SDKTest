#include "core/Process.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/Platform.h"

#if CS2ST_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
#else
	#include <sys/mman.h>
	#include <sys/types.h>
	#include <unistd.h>
#endif

namespace cs2st
{
namespace proc
{

bool IsExecutable(const char* stem)
{
	if (stem == nullptr || stem[0] == '\0')
	{
		return false;
	}
	const std::size_t stem_length = std::strlen(stem);

#if CS2ST_WINDOWS
	char path[MAX_PATH];
	const DWORD length = ::GetModuleFileNameA(nullptr, path, static_cast<DWORD>(sizeof(path)));
	if (length == 0 || length >= sizeof(path))
	{
		return false;
	}

	// ...\<stem>.exe
	const std::size_t suffix = stem_length + 4;
	if (length < suffix + 1 || path[length - suffix - 1] != '\\')
	{
		return false;
	}
	return ::_strnicmp(path + length - suffix, stem, stem_length) == 0 &&
	       ::_stricmp(path + length - 4, ".exe") == 0;
#else
	char path[512];
	const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (length <= 0)
	{
		return false;
	}
	path[length] = '\0';

	// .../<stem>
	const std::size_t size = static_cast<std::size_t>(length);
	return size > stem_length && path[size - stem_length - 1] == '/' &&
	       std::strcmp(path + size - stem_length, stem) == 0;
#endif
}

bool PatchPointerSlot(void** slot, void* value)
{
#if CS2ST_WINDOWS
	DWORD previous = 0;
	if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == 0)
	{
		return false;
	}
	*reinterpret_cast<void* volatile*>(slot) = value;
	DWORD ignored = 0;
	::VirtualProtect(slot, sizeof(void*), previous, &ignored);
	return true;
#else
	const long page_size = ::sysconf(_SC_PAGESIZE);
	void* page = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(slot) &
	                                     ~static_cast<std::uintptr_t>(page_size - 1));
	if (::mprotect(page, static_cast<std::size_t>(page_size), PROT_READ | PROT_WRITE) != 0)
	{
		return false;
	}
	__atomic_store_n(slot, value, __ATOMIC_SEQ_CST);
	::mprotect(page, static_cast<std::size_t>(page_size), PROT_READ);
	return true;
#endif
}

std::size_t FunctionSize(const void* fn)
{
#if CS2ST_WINDOWS && defined(_M_X64)
	DWORD64 image_base = 0;
	const RUNTIME_FUNCTION* entry =
		::RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(fn), &image_base, nullptr);
	if (entry != nullptr && image_base + entry->BeginAddress == reinterpret_cast<DWORD64>(fn))
	{
		return static_cast<std::size_t>(entry->EndAddress - entry->BeginAddress);
	}

	// No unwind entry of its own: a leaf function (no frame, no calls) needs none,
	// so the tables say nothing. MSVC pads between functions with int3, and no
	// function body contains two in a row, so the first pair marks the end. Capped,
	// so a function that is not followed by padding still gets a bounded scan.
	const unsigned char* code = static_cast<const unsigned char*>(fn);
	constexpr std::size_t kCap = 0x300;
	for (std::size_t i = 0; i + 1 < kCap; ++i)
	{
		if (code[i] == 0xCC && code[i + 1] == 0xCC)
		{
			return i;
		}
	}
	return kCap;
#else
	(void)fn;
	return 0;
#endif
}

[[noreturn]] void Terminate(int code)
{
#if CS2ST_WINDOWS
	::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(code));
	for (;;)
	{
		::Sleep(INFINITE);
	}
#else
	::_exit(code);
#endif
}

} // namespace proc
} // namespace cs2st
