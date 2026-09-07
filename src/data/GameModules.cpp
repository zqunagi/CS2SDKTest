#include "data/GameModules.h"

#include "core/Module.h"
#include "core/Platform.h"

namespace cs2st
{
namespace
{

// Ordered by how likely a module is to answer an interface query, so the
// common cases resolve on the first or second try rather than after twenty.
const GameModuleDesc kGameModules[] = {
	{"engine2",           nullptr, nullptr, "engine core, Source2Engine* interfaces"},
	{"server",            nullptr, nullptr, "game server DLL, Source2Server* interfaces"},
	{"schemasystem",      nullptr, nullptr, "schema reflection"},
	{"tier0",             nullptr, nullptr, "platform layer, memory, cvars"},
	{"filesystem_stdio",  nullptr, nullptr, "IFileSystem and friends"},
	{"resourcesystem",    nullptr, nullptr, "resource handles and manifests"},
	{"networksystem",     nullptr, nullptr, "network messages and serialisation"},
	{"localize",          nullptr, nullptr, "string localisation"},
	{"vscript",           nullptr, nullptr, "scripting VM"},
	{"soundsystem",       nullptr, nullptr, "audio; absent on dedicated servers"},
	{"materialsystem2",   nullptr, nullptr, "materials; absent on dedicated servers"},
	{"inputsystem",       nullptr, nullptr, "input; client only"},
	{"scenesystem",       nullptr, nullptr, "scene rendering; client only"},
	{"worldrenderer",     nullptr, nullptr, "world rendering; client only"},
	{"animationsystem",   nullptr, nullptr, "animation graph"},
	{"particles",         nullptr, nullptr, "particle systems"},
	{"panorama",          nullptr, nullptr, "UI framework; client only"},
	{"meshsystem",        nullptr, nullptr, "mesh streaming"},
	{"navsystem",         nullptr, nullptr, "navigation meshes"},
	{"host",              nullptr, nullptr, "host loop"},
	{"matchmaking",       nullptr, nullptr, "matchmaking services"},
	{"client",            nullptr, nullptr, "client DLL; listen servers only"},
	{"steamnetworkingsockets", nullptr, nullptr, "Steam datagram transport"},
	// Valve decorates the Steam API differently on each platform, which is
	// what the explicit-name columns are for.
	{"steam_api",         "steam_api64.dll", "libsteam_api.so", "Steam client API"},
};

} // namespace

const GameModuleDesc* GetGameModules(std::size_t& count)
{
	count = sizeof(kGameModules) / sizeof(kGameModules[0]);
	return kGameModules;
}

const GameModuleDesc* FindGameModule(const std::string& id)
{
	for (const GameModuleDesc& desc : kGameModules)
	{
		if (id == desc.id)
		{
			return &desc;
		}
	}
	return nullptr;
}

std::string PlatformModuleName(const GameModuleDesc& desc)
{
#if CS2ST_WINDOWS
	if (desc.windows_name != nullptr)
	{
		return desc.windows_name;
	}
#else
	if (desc.linux_name != nullptr)
	{
		return desc.linux_name;
	}
#endif
	return Module::Decorate(desc.id);
}

std::string ResolveModuleFileName(const std::string& id)
{
	const GameModuleDesc* desc = FindGameModule(id);
	if (desc != nullptr)
	{
		return PlatformModuleName(*desc);
	}
	// Not in the table is fine - a test module may want something we never
	// listed, and plain decoration is right for almost everything.
	return Module::Decorate(id);
}

} // namespace cs2st
