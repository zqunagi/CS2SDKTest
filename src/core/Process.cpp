#include "core/Process.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/Log.h"
#include "core/Platform.h"

#if CS2ST_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
#else
	#include <link.h>
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

namespace
{

struct Section
{
	const unsigned char* begin;
	std::size_t size;
	bool executable;
};

/**
 * The search itself, the same on both platforms; only how the image is described
 * (sections) and how an address maps to its function (`function_start`) differ.
 *
 *  1. the string, NUL included, wherever it sits. It may be the tail of a longer
 *     string (linkers merge those), and that is fine: step 2 only accepts an
 *     instruction that points exactly at it;
 *  2. the `lea r64, [rip+disp32]` instructions in code that point at it;
 *  3. the function each one belongs to, from the image's own unwind tables.
 */
template <typename FunctionStartFn>
std::vector<const void*> FindReferencingFunctions(const std::vector<Section>& sections,
	const char* text, FunctionStartFn function_start)
{
	const std::size_t text_length = std::strlen(text) + 1; // include the NUL

	std::vector<const unsigned char*> strings;
	for (const Section& section : sections)
	{
		// Every readable section, executable ones included: a Linux linker puts
		// .rodata in the same R-X segment as .text.
		if (section.size <= text_length)
		{
			continue;
		}
		for (std::size_t i = 0; i + text_length <= section.size; ++i)
		{
			if (section.begin[i] == static_cast<unsigned char>(text[0]) &&
			    std::memcmp(section.begin + i, text, text_length) == 0)
			{
				strings.push_back(section.begin + i);
			}
		}
	}

	std::size_t code_bytes = 0;
	std::size_t lea_hits = 0;
	std::vector<const void*> starts;
	for (const Section& section : sections)
	{
		if (!section.executable)
		{
			continue;
		}
		code_bytes += section.size;
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
			++lea_hits;
			if (const unsigned char* start = function_start(code))
			{
				starts.push_back(start);
			}
		}
	}
	CS2ST_LOG_INFO("scan '%s': %d section(s), %d string occurrence(s), %d code byte(s), "
	               "%d reference(s), %d resolved to a function\n",
		text, static_cast<int>(sections.size()), static_cast<int>(strings.size()),
		static_cast<int>(code_bytes), static_cast<int>(lea_hits), static_cast<int>(starts.size()));
	return starts;
}

} // namespace

#if CS2ST_WINDOWS && defined(_M_X64)

namespace
{

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

// .pdata: every function with a frame or a call has an entry. A function that
// names a string for a call to use has both.
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

bool FunctionsReferencingString(const void* base, std::size_t, const char* text,
	const std::vector<const void*>&, std::vector<const void*>& out)
{
	out = FindReferencingFunctions(ImageSections(base), text, &FunctionStart);
	return true;
}

#else

namespace
{

// One object's loadable segments, and its .eh_frame_hdr lookup table: sorted
// (function start, FDE) pairs of 32-bit offsets from the start of the header.
struct ElfImage
{
	const void* base = nullptr;
	bool found = false;
	std::vector<Section> sections;
	const unsigned char* header = nullptr;
	const std::int32_t* table = nullptr;
	std::uint32_t count = 0;
};

int CollectElfImage(struct dl_phdr_info* info, std::size_t, void* user)
{
	auto* image = static_cast<ElfImage*>(user);
	if (reinterpret_cast<const void*>(info->dlpi_addr) != image->base)
	{
		return 0;
	}
	image->found = true;

	for (int i = 0; i < info->dlpi_phnum; ++i)
	{
		const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
		const auto* at = reinterpret_cast<const unsigned char*>(info->dlpi_addr + phdr.p_vaddr);
		if (phdr.p_type == PT_LOAD && (phdr.p_flags & PF_R) != 0 && phdr.p_filesz != 0)
		{
			image->sections.push_back({at, static_cast<std::size_t>(phdr.p_filesz),
				(phdr.p_flags & PF_X) != 0});
		}
		else if (phdr.p_type == PT_GNU_EH_FRAME)
		{
			image->header = at;
		}
	}
	return 1;
}

// Only the layout every toolchain emits is understood: version 1, a pc-relative
// 4- or 8-byte eh_frame pointer, a udata4 count and sdata4 datarel table entries.
// Anything else leaves the table empty, and the lookup reports it cannot answer.
void ReadEhFrameHeader(ElfImage& image)
{
	const unsigned char* header = image.header;
	if (header == nullptr || header[0] != 1 || header[2] != 0x03 || header[3] != 0x3B)
	{
		return;
	}
	const unsigned pointer_size = (header[1] & 7) == 3 ? 4 : ((header[1] & 7) == 4 ? 8 : 0);
	if (pointer_size == 0)
	{
		return;
	}
	std::memcpy(&image.count, header + 4 + pointer_size, sizeof(image.count));
	image.table = reinterpret_cast<const std::int32_t*>(header + 4 + pointer_size + 4);
}

// The greatest function start at or below `address`.
const unsigned char* FunctionStart(const ElfImage& image, const void* address)
{
	const auto* target = static_cast<const unsigned char*>(address);
	std::uint32_t low = 0;
	std::uint32_t high = image.count;
	while (low < high)
	{
		const std::uint32_t middle = low + (high - low) / 2;
		if (image.header + image.table[middle * 2] <= target)
		{
			low = middle + 1;
		}
		else
		{
			high = middle;
		}
	}
	return low == 0 ? nullptr : image.header + image.table[(low - 1) * 2];
}

} // namespace

bool FunctionsReferencingString(const void* base, std::size_t, const char* text,
	const std::vector<const void*>& known_starts, std::vector<const void*>& out)
{
	ElfImage image;
	image.base = base;
	::dl_iterate_phdr(&CollectElfImage, &image);
	if (!image.found)
	{
		return false;
	}
	ReadEhFrameHeader(image);
	CS2ST_LOG_INFO("elf image %p: %d loadable segment(s), eh_frame_hdr %p, %u function(s) in the table\n",
		image.base, static_cast<int>(image.sections.size()),
		static_cast<const void*>(image.header), static_cast<unsigned>(image.count));
	if (image.table == nullptr || image.count == 0)
	{
		return false; // no usable unwind table: cannot tell where functions start
	}

	std::vector<const unsigned char*> known;
	for (const void* start : known_starts)
	{
		known.push_back(static_cast<const unsigned char*>(start));
	}
	std::sort(known.begin(), known.end());

	// The nearer of the two sources wins: a recorded function between a known start
	// and the reference means the reference is not in the known one.
	out = FindReferencingFunctions(image.sections, text,
		[&image, &known](const void* address) -> const unsigned char* {
			const unsigned char* recorded = FunctionStart(image, address);
			const unsigned char* given = nullptr;
			auto above = std::upper_bound(known.begin(), known.end(),
				static_cast<const unsigned char*>(address));
			if (above != known.begin())
			{
				given = *(above - 1);
			}
			return recorded > given ? recorded : given;
		});
	return true;
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
