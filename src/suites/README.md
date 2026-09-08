Test suites live here — one per SDK interface being exercised.

  Source2EngineSuite       IVEngineServer2 / ISource2Engine / IAppSystem (calls + CGlobalVars layout)
  Source2ServerSuite       ISource2Server (= IServerGameDLL)             (calls + 11 hooks)
  Source2GameClientsSuite  ISource2GameClients (= IServerGameClients)    (calls + 11 hooks)
  MetamodListenerSuite     ISmmAPI / IMetamodListener                    (calls + 15 listener callbacks)

MetamodListenerSuite is the odd one out: it tests Metamod, not hl2sdk, and it
is here because LevelInit/LevelShutdown reach a Source 2 plugin only through
IMetamodListener. It is also the only suite whose "hooks" are not SourceHook —
ISmmAPI::AddListener has no inverse, so removing them stops the recording but
not the callbacks.

Vocabulary: a *module* in this codebase is always a loaded shared library
(engine2.so, server.so — see data/GameModules.h). A *suite* is one of these
files: our test code for one interface that lives inside such a module.

See `src/testkit/ITestSuite.h` for the interface, `docs/TEST_TARGETS.md` for
what to write next, and the repository README for a worked example. Adding one
is: write the file, register it in `TestRunner::CreateDefault()`, add it to
`CS2SDKTEST_SOURCES` in CMakeLists.txt.
