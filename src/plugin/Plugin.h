/**
 * The Metamod:Source plugin entry point.
 *
 * Deliberately thin: it owns the test runner and nothing else. Everything that
 * actually touches the SDK lives behind ITestSuite, so this file should not
 * need to change as coverage is filled in.
 */

#ifndef CS2SDKTEST_PLUGIN_PLUGIN_H_
#define CS2SDKTEST_PLUGIN_PLUGIN_H_

#include <ISmmPlugin.h>

#include "testkit/TestRunner.h"
#include "version_gen.h"

namespace cs2st
{

// Not an IMetamodListener. The listener callbacks are a thing under test like
// any other, so they live in MetamodListenerSuite, which registers itself with
// ISmmAPI::AddListener when that suite's hooks are installed. Inheriting here
// as well would mean a second listener nobody registers - and a reader would
// have to check which of the two Metamod is actually calling.
class CS2SDKTestPlugin : public ISmmPlugin
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
	bool Unload(char* error, size_t maxlen) override;
	void AllPluginsLoaded() override;

	TestRunner& tests() { return tests_; }
	const TestRunner& tests() const { return tests_; }

	/** True once Load() has completed; commands refuse to run before that. */
	bool ready() const { return ready_; }

	const char* GetAuthor() override      { return PLUGIN_AUTHOR; }
	const char* GetName() override        { return PLUGIN_DISPLAY_NAME; }
	const char* GetDescription() override { return PLUGIN_DESCRIPTION; }
	const char* GetURL() override         { return PLUGIN_URL; }
	const char* GetLicense() override     { return PLUGIN_LICENSE; }
	const char* GetVersion() override     { return PLUGIN_FULL_VERSION; }
	const char* GetDate() override        { return __DATE__; }
	const char* GetLogTag() override      { return PLUGIN_LOGTAG; }

private:
	TestRunner tests_;
	bool ready_ = false;
};

/** The single plugin instance; commands reach the runner through this. */
extern CS2SDKTestPlugin g_Plugin;

} // namespace cs2st

PLUGIN_GLOBALVARS();

#endif /* CS2SDKTEST_PLUGIN_PLUGIN_H_ */
