#include "plugin/Plugin.h"

#include <tier1/convar.h>

#include "core/FileSystemUtil.h"
#include "core/Log.h"
#include "hook/HookRecorder.h"
#include "testkit/CallGuard.h"

namespace cs2st
{
CS2SDKTestPlugin g_Plugin;
} // namespace cs2st

// Defines g_SMAPI/g_PLAPI/g_PLID/g_SHPtr and the CreateInterface export
// Metamod looks for. Must sit at global scope.
PLUGIN_EXPOSE(CS2SDKTestPlugin, cs2st::g_Plugin);

namespace cs2st
{

// Declared in Commands.cpp, where the ConCommand object actually lives.
void UnregisterCommands();

namespace
{

ICvar* g_icvar = nullptr;

} // namespace

bool CS2SDKTestPlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	// ICvar is the only interface acquired here: without it the console
	// commands cannot register, and a diagnostic plugin nobody can ask a
	// question is useless. Everything the test modules touch is resolved on
	// demand instead - going through engine interface pointers at load time
	// would mean failing to load in exactly the situation this exists to
	// diagnose.
	GET_V_IFACE_CURRENT(GetEngineFactory, g_icvar, ICvar, CVAR_INTERFACE_VERSION);
	g_pCVar = g_icvar;

	// Probes the console for ANSI support before anything is printed, so a
	// console that cannot do colour gets plain text rather than escapes.
	InitLogging();

	tests_ = TestRunner::CreateDefault();

	META_CONVAR_REGISTER(FCVAR_RELEASE | FCVAR_GAMEDLL);

	ready_ = true;

	CS2ST_LOG_INFO("%s %s loaded%s\n",
		PLUGIN_DISPLAY_NAME, PLUGIN_FULL_VERSION, late ? " (late)" : "");
	CS2ST_LOG_INFO("built against SDK '%s' (SOURCE_ENGINE=%d), Metamod PLAPI %d\n",
		PLUGIN_SDK_ENGINE, PLUGIN_SDK_ENGINE_CODE, PLUGIN_MM_PLAPI_VERSION);
	CS2ST_LOG_INFO("%zu test module(s) registered, logs in %s\n",
		tests_.size(), fs::LogDirectory().c_str());

	CS2ST_LOG_INFO("fault guard: %s\n", GuardDescription());
	CS2ST_LOG_INFO("type 'meta_test help' in the console to get started\n");
	return true;
}

bool CS2SDKTestPlugin::Unload(char* error, size_t maxlen)
{
	ready_ = false;

	// Hooks must come out before the plugin's code is unmapped, or the next
	// call through one of them lands in freed memory.
	TestContext context;
	tests_.RemoveHooks(context);
	HookRecorder::Get().Clear();

	UnregisterCommands();
	CloseSessionLog();

	(void)error;
	(void)maxlen;
	return true;
}

void CS2SDKTestPlugin::AllPluginsLoaded()
{
	// Nothing runs automatically. Sweeping interfaces means calling into the
	// engine at a moment nobody chose, and installing hooks changes the
	// server's behaviour - both are on-demand only.
}

} // namespace cs2st
