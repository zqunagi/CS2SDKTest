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

#if CS2ST_WINDOWS && defined(_M_X64)

namespace
{

struct Section
{
	const unsigned char* begin;
	std::size_t size;
	bool executable;
};

// Mapped, non-discardable sections of the image. Discardable ones (.reloc, INIT)
// may already be unmapped, and reading them would fault.
std::vector<Section> ImageSections(const void* base)
{
	std::vector<Section> sections;
	const auto* image = static_cast<const unsigned char*>(base);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		return sections;
	}
	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
	{
		return sections;
	}
	const IMAGE_SECTION_HEADER* header = IMAGE_FIRST_SECTION(nt);
	for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
	{
		const DWORD flags = header[i].Characteristics;
		if ((flags & IMAGE_SCN_MEM_READ) == 0 || (flags & IMAGE_SCN_MEM_DISCARDABLE) != 0)
		{
			continue;
		}
		const std::size_t length = header[i].Misc.VirtualSize;
		if (length != 0)
		{
			sections.push_back({image + header[i].VirtualAddress, length,
				(flags & IMAGE_SCN_MEM_EXECUTE) != 0});
		}
	}
	return sections;
}

// Where the function containing `address` starts, from the unwind tables, or
// nullptr. Only a function with no frame and no calls lacks an entry; one that
// names a string for a call to use does not qualify.
const unsigned char* FunctionStart(const void* address)
{
	DWORD64 image_base = 0;
	const RUNTIME_FUNCTION* entry =
		::RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(address), &image_base, nullptr);
	if (entry == nullptr)
	{
		return nullptr;
	}
	return reinterpret_cast<const unsigned char*>(image_base + entry->BeginAddress);
}

} // namespace

bool FunctionsReferencingString(const void* base, std::size_t size, const char* text,
	std::vector<const void*>& out)
{
	(void)size;
	const std::size_t text_length = std::strlen(text) + 1; // include the NUL
	const std::vector<Section> sections = ImageSections(base);

	// 1. The string as a whole C string: the byte before it must be a NUL, so the
	//    tail of a longer string does not count.
	std::vector<const unsigned char*> strings;
	for (const Section& section : sections)
	{
		if (section.executable || section.size <= text_length)
		{
			continue;
		}
		for (std::size_t i = 1; i + text_length <= section.size; ++i)
		{
			if (section.begin[i - 1] == 0 && section.begin[i] == static_cast<unsigned char>(text[0]) &&
			    std::memcmp(section.begin + i, text, text_length) == 0)
			{
				strings.push_back(section.begin + i);
			}
		}
	}

	// 2. The instructions that point at it, 3. the function each one belongs to.
	std::vector<const void*> starts;
	for (const Section& section : sections)
	{
		if (!section.executable)
		{
			continue;
		}
		for (std::size_t i = 0; i + 7 <= section.size; ++i)
		{
			const unsigned char* code = section.begin + i;
			// lea r64, [rip+disp32]
			if ((code[0] != 0x48 && code[0] != 0x4C) || code[1] != 0x8D || (code[2] & 0xC7) != 0x05)
			{
				continue;
			}
			std::int32_t disp = 0;
			std::memcpy(&disp, code + 3, sizeof(disp));
			const unsigned char* target = code + 7 + disp;
			bool hit = false;
			for (const unsigned char* candidate : strings)
			{
				hit = hit || candidate == target;
			}
			if (!hit)
			{
				continue;
			}
			const unsigned char* start = FunctionStart(code);
			if (start != nullptr)
			{
				starts.push_back(start);
			}
		}
	}

	out = starts;
	return true;
}

#else

bool FunctionsReferencingString(const void*, std::size_t, const char*, std::vector<const void*>&)
{
	return false;
}

#endif

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
