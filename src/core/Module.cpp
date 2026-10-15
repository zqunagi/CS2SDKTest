#include "core/Module.h"

#include <cstring>

#if CS2ST_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
	#include <TlHelp32.h>
#else
	#include <dlfcn.h>
	#include <link.h>
#endif

namespace cs2st
{
namespace
{

#if CS2ST_WINDOWS

// SizeOfImage is read straight out of the PE headers rather than through
// psapi's GetModuleInformation, so the plugin needs no extra import library.
std::size_t ImageSizeFromHeaders(const void* base)
{
	const auto* dos = static_cast<const IMAGE_DOS_HEADER*>(base);
	if (dos == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		return 0;
	}

	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
		static_cast<const unsigned char*>(base) + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
	{
		return 0;
	}

	return static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
}

/**
 * The game's own copy of a module whose name Metamod also uses: its loader is
 * built as server.dll, and GetModuleHandle returns whichever same-named image
 * was mapped first. A copy under an addons\ directory is never the game's, so
 * it is the answer only when no other module carries the name. Toolhelp is
 * kernel32's, so this needs no extra import library either.
 */
HMODULE FindPreferredModule(const std::string& file_name)
{
	HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
	if (snapshot == INVALID_HANDLE_VALUE)
	{
		return ::GetModuleHandleA(file_name.c_str());
	}

	HMODULE fallback = nullptr;
	HMODULE preferred = nullptr;
	MODULEENTRY32 entry{};
	entry.dwSize = sizeof(entry);
	for (BOOL more = ::Module32First(snapshot, &entry); more && preferred == nullptr;
	     more = ::Module32Next(snapshot, &entry))
	{
		if (::_stricmp(entry.szModule, file_name.c_str()) != 0)
		{
			continue;
		}
		char lowered[MAX_PATH] = {0};
		for (std::size_t i = 0; i + 1 < sizeof(lowered) && entry.szExePath[i] != '\0'; ++i)
		{
			const char c = entry.szExePath[i];
			lowered[i] = (c == '/') ? '\\' : static_cast<char>((c >= 'A' && c <= 'Z') ? c + 32 : c);
		}
		if (std::strstr(lowered, "\\addons\\") != nullptr)
		{
			if (fallback == nullptr)
			{
				fallback = entry.hModule;
			}
			continue;
		}
		preferred = entry.hModule;
	}
	::CloseHandle(snapshot);

	return (preferred != nullptr) ? preferred : fallback;
}

#else

struct ImageBounds
{
	const char* wanted = nullptr;
	const void* base = nullptr;
	std::size_t size = 0;
	std::string path;
	bool found = false;
};

const char* BaseName(const char* path)
{
	const char* slash = std::strrchr(path, '/');
	return (slash != nullptr) ? slash + 1 : path;
}

int CollectImageBounds(struct dl_phdr_info* info, std::size_t /*info_size*/, void* user)
{
	auto* out = static_cast<ImageBounds*>(user);
	if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0')
	{
		return 0;
	}
	if (std::strcmp(BaseName(info->dlpi_name), out->wanted) != 0)
	{
		return 0;
	}

	// Two mapped objects can share a file name: Metamod's loader is itself
	// built as libserver.so and is mapped before the game's. A copy under an
	// addons/ directory is never the game's own module, so it only wins when
	// nothing else answers to the name - and the walk goes on looking.
	const bool under_addons = std::strstr(info->dlpi_name, "/addons/") != nullptr;
	if (out->found && under_addons)
	{
		return 0;
	}

	// The mapped size is the span of the PT_LOAD segments; there is no single
	// field for it the way there is in a PE optional header.
	ElfW(Addr) lo = static_cast<ElfW(Addr)>(-1);
	ElfW(Addr) hi = 0;
	bool saw_load = false;
	for (int i = 0; i < info->dlpi_phnum; ++i)
	{
		const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
		if (phdr.p_type != PT_LOAD)
		{
			continue;
		}
		if (phdr.p_vaddr < lo)
		{
			lo = phdr.p_vaddr;
		}
		if (phdr.p_vaddr + phdr.p_memsz > hi)
		{
			hi = phdr.p_vaddr + phdr.p_memsz;
		}
		saw_load = true;
	}

	out->base = reinterpret_cast<const void*>(info->dlpi_addr);
	out->size = (saw_load && hi > lo) ? static_cast<std::size_t>(hi - lo) : 0;
	out->path = info->dlpi_name;
	out->found = true;
	return under_addons ? 0 : 1; // stop only once the game's own copy is found
}

#endif // CS2ST_WINDOWS

bool LooksDecorated(const std::string& name)
{
	const std::string suffix = CS2ST_LIB_SUFFIX;
	return name.size() > suffix.size() &&
	       name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

Module::~Module()
{
	Reset();
}

Module::Module(Module&& other) noexcept
{
	*this = static_cast<Module&&>(other);
}

Module& Module::operator=(Module&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		handle_ = other.handle_;
		owns_handle_ = other.owns_handle_;
		name_ = static_cast<std::string&&>(other.name_);
		path_ = static_cast<std::string&&>(other.path_);
		base_ = other.base_;
		size_ = other.size_;
		factory_ = other.factory_;

		other.handle_ = nullptr;
		other.owns_handle_ = false;
		other.base_ = nullptr;
		other.size_ = 0;
		other.factory_ = nullptr;
	}
	return *this;
}

void Module::Reset()
{
#if !CS2ST_WINDOWS
	if (handle_ != nullptr && owns_handle_)
	{
		dlclose(handle_);
	}
#endif
	handle_ = nullptr;
	owns_handle_ = false;
	base_ = nullptr;
	size_ = 0;
	factory_ = nullptr;
}

std::string Module::Decorate(const std::string& stem)
{
	if (LooksDecorated(stem))
	{
		return stem;
	}
	return std::string(CS2ST_LIB_PREFIX) + stem + CS2ST_LIB_SUFFIX;
}

Module Module::Find(const std::string& stem)
{
	return FindExact(Decorate(stem));
}

Module Module::FindExact(const std::string& file_name)
{
	Module module;
	module.name_ = file_name;

#if CS2ST_WINDOWS
	// GetModuleHandle does not take a reference, which is exactly what we
	// want: the engine owns these modules and we are only looking.
	HMODULE handle = FindPreferredModule(file_name);
	if (handle == nullptr)
	{
		return module;
	}

	module.handle_ = reinterpret_cast<ModuleHandle>(handle);
	module.owns_handle_ = false;
	module.base_ = reinterpret_cast<const void*>(handle);
	module.size_ = ImageSizeFromHeaders(module.base_);

	char path[MAX_PATH] = {0};
	if (::GetModuleFileNameA(handle, path, static_cast<DWORD>(sizeof(path))) > 0)
	{
		module.path_ = path;
	}
#else
	// The object is chosen from the loader's list first, then opened by its
	// full path: dlopen by bare name returns whichever same-named object the
	// loader met first, which for libserver.so is Metamod's shim, and the
	// handle (dlsym) and the bounds (scans) must describe the same image.
	ImageBounds bounds;
	bounds.wanted = file_name.c_str();
	::dl_iterate_phdr(&CollectImageBounds, &bounds);

	// RTLD_NOLOAD returns a handle only if the library is already mapped, so
	// this stays a query rather than turning into a load.
	void* handle = ::dlopen(bounds.found ? bounds.path.c_str() : file_name.c_str(),
		RTLD_NOW | RTLD_NOLOAD);
	if (handle == nullptr)
	{
		return module;
	}

	module.handle_ = handle;
	module.owns_handle_ = true; // RTLD_NOLOAD still bumps the refcount

	if (bounds.found)
	{
		module.base_ = bounds.base;
		module.size_ = bounds.size;
		module.path_ = bounds.path;
	}
#endif

	module.factory_ =
		reinterpret_cast<CreateInterfaceFnRaw>(module.FindSymbol("CreateInterface"));

	return module;
}

void* Module::FindSymbol(const char* symbol) const
{
	if (handle_ == nullptr || symbol == nullptr)
	{
		return nullptr;
	}

#if CS2ST_WINDOWS
	FARPROC proc = ::GetProcAddress(reinterpret_cast<HMODULE>(handle_), symbol);
	return reinterpret_cast<void*>(proc);
#else
	return ::dlsym(handle_, symbol);
#endif
}

void* Module::QueryInterface(const char* version) const
{
	if (factory_ == nullptr || version == nullptr)
	{
		return nullptr;
	}

	int return_code = kInterfaceFailed;
	void* iface = factory_(version, &return_code);

	// Source modules are not consistent about which of the two they set on
	// failure, so both have to agree before the result is trusted.
	if (iface == nullptr || return_code != kInterfaceOk)
	{
		return nullptr;
	}
	return iface;
}

} // namespace cs2st
