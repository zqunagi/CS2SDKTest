#include "core/FileSystemUtil.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "core/Platform.h"

#if CS2ST_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <Windows.h>
	#include <direct.h>
	#include <sys/stat.h>
#else
	#include <dlfcn.h>
	#include <sys/stat.h>
	#include <sys/types.h>
	#include <unistd.h>
#endif

namespace cs2st
{
namespace fs
{
namespace
{

bool IsSeparator(char ch)
{
	return ch == '/' || ch == '\\';
}

bool CreateSingleDirectory(const std::string& path)
{
#if CS2ST_WINDOWS
	if (::_mkdir(path.c_str()) == 0)
	{
		return true;
	}
#else
	if (::mkdir(path.c_str(), 0755) == 0)
	{
		return true;
	}
#endif
	return errno == EEXIST;
}

std::string FormatLocalTime(const char* format)
{
	const std::time_t now = std::time(nullptr);

	std::tm utc {};
#if CS2ST_WINDOWS
	::gmtime_s(&utc, &now);
#else
	::gmtime_r(&now, &utc);
#endif

	char buffer[64];
	if (std::strftime(buffer, sizeof(buffer), format, &utc) == 0)
	{
		return std::string();
	}
	return buffer;
}

} // namespace

std::string Join(const std::string& base, const std::string& leaf)
{
	if (base.empty())
	{
		return leaf;
	}
	if (leaf.empty())
	{
		return base;
	}

	std::string out = base;
	if (!IsSeparator(out.back()))
	{
		out.push_back(CS2ST_PATH_SEP);
	}

	std::size_t start = 0;
	while (start < leaf.size() && IsSeparator(leaf[start]))
	{
		++start;
	}
	out.append(leaf, start, std::string::npos);
	return out;
}

std::string DirectoryName(const std::string& path)
{
	for (std::size_t i = path.size(); i > 0; --i)
	{
		if (IsSeparator(path[i - 1]))
		{
			return path.substr(0, i - 1);
		}
	}
	return std::string();
}

std::string BaseName(const std::string& path)
{
	for (std::size_t i = path.size(); i > 0; --i)
	{
		if (IsSeparator(path[i - 1]))
		{
			return path.substr(i);
		}
	}
	return path;
}

bool Exists(const std::string& path)
{
	if (path.empty())
	{
		return false;
	}
	struct stat info;
	return ::stat(path.c_str(), &info) == 0;
}

bool CreateDirectories(const std::string& path)
{
	if (path.empty())
	{
		return false;
	}
	if (Exists(path))
	{
		return true;
	}

	// Walk forward creating each component; the leading separator on POSIX is
	// the root itself and must not be created.
	for (std::size_t i = 1; i <= path.size(); ++i)
	{
		if (i != path.size() && !IsSeparator(path[i]))
		{
			continue;
		}

		const std::string prefix = path.substr(0, i);
		if (prefix.empty() || Exists(prefix))
		{
			continue;
		}
#if CS2ST_WINDOWS
		// "C:" is not a directory that can be created.
		if (prefix.size() == 2 && prefix[1] == ':')
		{
			continue;
		}
#endif
		if (!CreateSingleDirectory(prefix))
		{
			return false;
		}
	}

	return Exists(path);
}

bool IsSafeFileName(const std::string& name, std::string& error)
{
	if (name.empty())
	{
		error = "name is empty";
		return false;
	}
	if (name.size() > 128)
	{
		error = "name is longer than 128 characters";
		return false;
	}
	if (name == "." || name == "..")
	{
		error = "name refers to a directory";
		return false;
	}

	for (const char ch : name)
	{
		if (IsSeparator(ch) || ch == ':')
		{
			error = "name must not contain a path separator or drive letter";
			return false;
		}
		if (static_cast<unsigned char>(ch) < 0x20)
		{
			error = "name contains a control character";
			return false;
		}
	}

	if (name.find("..") != std::string::npos)
	{
		error = "name must not contain '..'";
		return false;
	}

	error.clear();
	return true;
}

std::string EnsureExtension(const std::string& name, const std::string& extension)
{
	if (name.size() >= extension.size() &&
		name.compare(name.size() - extension.size(), extension.size(), extension) == 0)
	{
		return name;
	}
	return name + extension;
}

std::string PluginBinaryPath()
{
#if CS2ST_WINDOWS
	HMODULE self = nullptr;
	if (::GetModuleHandleExA(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&PluginBinaryPath),
			&self) == 0 || self == nullptr)
	{
		return std::string();
	}

	std::vector<char> buffer(1024);
	const DWORD length =
		::GetModuleFileNameA(self, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (length == 0 || length >= buffer.size())
	{
		return std::string();
	}
	return std::string(buffer.data(), length);
#else
	Dl_info info;
	if (::dladdr(reinterpret_cast<const void*>(&PluginBinaryPath), &info) == 0 ||
		info.dli_fname == nullptr)
	{
		return std::string();
	}
	return std::string(info.dli_fname);
#endif
}

std::string LogDirectory()
{
	const std::string binary = PluginBinaryPath();
	if (binary.empty())
	{
		// Falling back to the working directory keeps the commands usable on a
		// layout we did not anticipate; the run always prints the full path of
		// the log it opened, so where it went is never a mystery.
		return "logs";
	}

	// <mod>/addons/<alias>/bin/<binary>  ->  <mod>/addons/<alias>/logs
	const std::string bin_dir = DirectoryName(binary);
	const std::string plugin_dir = DirectoryName(bin_dir);
	if (plugin_dir.empty())
	{
		return "logs";
	}
	return Join(plugin_dir, "logs");
}

std::string TimestampForFileName()
{
	const std::string stamp = FormatLocalTime("%Y%m%d-%H%M%S");
	return stamp.empty() ? "00000000-000000" : stamp;
}

std::string TimestampUtc()
{
	const std::string stamp = FormatLocalTime("%Y-%m-%dT%H:%M:%SZ");
	return stamp.empty() ? "1970-01-01T00:00:00Z" : stamp;
}

} // namespace fs
} // namespace cs2st
