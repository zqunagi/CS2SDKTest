/**
 * The Source 2 modules this plugin knows how to look inside.
 *
 * Interface version constants in the SDK say *what* to ask for but never
 * *which* library to ask, so resolving one means asking modules until somebody
 * answers. This table is that candidate list, in the order worth trying.
 *
 * It is also where platform naming differences are kept: almost everything is
 * "foo.dll" / "libfoo.so", but a handful are not, and burying that in the
 * lookup code would make it the lookup code's problem forever.
 */

#ifndef CS2SDKTEST_DATA_GAMEMODULES_H_
#define CS2SDKTEST_DATA_GAMEMODULES_H_

#include <cstddef>
#include <string>

namespace cs2st
{

struct GameModuleDesc
{
	/** Stable short name used everywhere in logs and commands. */
	const char* id;

	/** Explicit file names, used only where the usual decoration is wrong. */
	const char* windows_name;
	const char* linux_name;

	/** What the module covers, shown when listing modules. */
	const char* note;
};

const GameModuleDesc* GetGameModules(std::size_t& count);

/** Looks up a descriptor by id, or nullptr. */
const GameModuleDesc* FindGameModule(const std::string& id);

/** Platform-appropriate, fully decorated file name for a table entry. */
std::string PlatformModuleName(const GameModuleDesc& desc);

/**
 * Decorated file name for an id, consulting the table first so the entries
 * with irregular names resolve correctly, and falling back to plain
 * lib/.so vs .dll decoration for anything not listed.
 */
std::string ResolveModuleFileName(const std::string& id);

} // namespace cs2st

#endif /* CS2SDKTEST_DATA_GAMEMODULES_H_ */
