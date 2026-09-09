Test suites live here — one per SDK interface being exercised.

  Source2EngineSuite       IVEngineServer2 / ISource2Engine / IAppSystem (calls + CGlobalVars layout)
  Source2ServerSuite       ISource2Server (= IServerGameDLL)             (calls + 11 hooks)
  Source2GameClientsSuite  ISource2GameClients (= IServerGameClients)    (calls + 11 hooks)
  ConVarSuite              ICvar / ConVar / ConCommand / ConCommandRef   (calls + 5 struct layouts + 6 hooks + 2 native listeners)
  SchemaSystemSuite        ISchemaSystem / CSchemaSystemTypeScope        (93 calls + 9 struct layouts)
  GameEntitySystemSuite    CGameEntitySystem / CEntitySystem             (calls + 8 struct layouts + 4 listener callbacks)
  MetamodListenerSuite     ISmmAPI / IMetamodListener                    (calls + 15 listener callbacks)

MetamodListenerSuite is the odd one out: it tests Metamod, not hl2sdk, and it
is here because LevelInit/LevelShutdown reach a Source 2 plugin only through
IMetamodListener. It is also the only suite whose "hooks" are not SourceHook —
ISmmAPI::AddListener has no inverse, so removing them stops the recording but
not the callbacks.

ConVarSuite is the only suite that registers something of its own in the game in
order to test it: eight convars and two console commands, at load, through the
SDK's own CConVar/ConCommand path. That is what gives it ground truth — every
other suite has to infer what a correct answer looks like, while this one
compares the engine against names, types, defaults, bounds and callbacks it
chose itself. It writes only to those, never to an engine convar, and every
probe is a POD type on purpose: CS2SDKTEST_USE_MEMOVERRIDE is off, so a
string-typed convar would put a buffer from this module's heap into storage the
engine frees. The string type is covered read-only instead.

GameEntitySystemSuite is the other odd one. There is no interface to query:
hl2sdk gives IGameResourceService a name and nothing else, so the entity system
is fished out of the service object at a byte offset the ecosystem agrees on —
which the suite treats as a hypothesis and measures. It is also the only suite
that writes into the game (AddListenerEntity), and the only one that compiles
SDK translation units (hl2sdk/entity2/*.cpp) in order to test them; it supplies
the GameEntitySystem() definition those need.

Vocabulary: a *module* in this codebase is always a loaded shared library
(engine2.so, server.so — see data/GameModules.h). A *suite* is one of these
files: our test code for one interface that lives inside such a module.

See `src/testkit/ITestSuite.h` for the interface, `docs/TEST_TARGETS.md` for
what to write next, and the repository README for a worked example. Adding one
is: write the file, register it in `TestRunner::CreateDefault()`, add it to
`CS2SDKTEST_SOURCES` in CMakeLists.txt.
