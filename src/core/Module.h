/**
 * Access to shared libraries the game has already mapped into this process.
 *
 * The plugin never *loads* a module: by the time it runs, the engine has
 * already brought up everything worth inspecting, and forcing a fresh load
 * would run static initialisers a second time. So every lookup here is
 * "find it if it is present, otherwise report absent" - and "absent" is itself
 * a finding worth putting in a report.
 */

#ifndef CS2SDKTEST_CORE_MODULE_H_
#define CS2SDKTEST_CORE_MODULE_H_

#include <string>

#include "core/Platform.h"

namespace cs2st
{

class Module
{
public:
	Module() = default;
	~Module();

	Module(const Module&) = delete;
	Module& operator=(const Module&) = delete;
	Module(Module&& other) noexcept;
	Module& operator=(Module&& other) noexcept;

	/**
	 * Finds an already-loaded module by its undecorated name.
	 *
	 * "engine2" resolves to engine2.dll on Windows and libengine2.so on Linux;
	 * pass a name that already carries a prefix/extension to skip decoration.
	 */
	static Module Find(const std::string& stem);

	/** Finds an already-loaded module by its exact file name. */
	static Module FindExact(const std::string& file_name);

	/** Decorates an undecorated module name for the current platform. */
	static std::string Decorate(const std::string& stem);

	bool IsValid() const { return handle_ != nullptr; }

	/** Decorated file name, e.g. "engine2.dll" / "libengine2.so". */
	const std::string& name() const { return name_; }

	/** Absolute path on disk, or empty when the OS would not tell us. */
	const std::string& path() const { return path_; }

	/** Load address of the mapped image, or nullptr if unknown. */
	const void* base() const { return base_; }

	/** Mapped size in bytes, or 0 if unknown. */
	std::size_t size() const { return size_; }

	/** Resolves an exported symbol, or nullptr when it is not exported. */
	void* FindSymbol(const char* symbol) const;

	/**
	 * The module's CreateInterface export.
	 *
	 * Returns nullptr for modules that do not participate in the interface
	 * system (tier0, for instance, exports its API directly).
	 */
	CreateInterfaceFnRaw factory() const { return factory_; }

	/**
	 * Asks this module's factory for an interface by exact version string.
	 * Returns nullptr when the module does not implement it.
	 */
	void* QueryInterface(const char* version) const;

private:
	void Reset();

	ModuleHandle handle_ = nullptr;
	// Windows hands back a borrowed HMODULE; Linux hands back a dlopen
	// reference that has to be released. Only the latter needs a close.
	bool owns_handle_ = false;
	std::string name_;
	std::string path_;
	const void* base_ = nullptr;
	std::size_t size_ = 0;
	CreateInterfaceFnRaw factory_ = nullptr;
};

} // namespace cs2st

#endif /* CS2SDKTEST_CORE_MODULE_H_ */
