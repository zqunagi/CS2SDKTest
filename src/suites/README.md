# Test suites

One suite per SDK interface (or other subject) we exercise. Each suite's own
header explains its oracles; this page is only the map.

| Suite | Tests | Headers (under `s2sdk/public` unless noted) |
|---|---|---|
| Source2EngineSuite | IVEngineServer2, ISource2Engine, IAppSystem; CGlobalVars and AppSystemInfo_t layouts | engine2/isource2engine.h, ivengineserver2.h, gameshareddefs.h; appframework/iappsystem.h |
| Source2ServerSuite | ISource2Server; 11 hooks | engine2/isource2server.h |
| Source2GameClientsSuite | ISource2GameClients; 11 hooks | engine2/isource2server.h, ivengineserver2.h |
| ServerConfigSuite | ISource2ServerConfig (the tick interval other suites use) | engine2/isource2serverconfig.h, steam/steamuniverse.h |
| EngineServiceMgrSuite | IEngineServiceMgr: active loop, service probe, addons | engine2/iengineservicemgr.h |
| StringTableSuite | INetworkStringTableContainer / INetworkStringTable; StringUserData_t layout | networkstringtabledefs.h |
| GameEntitiesSuite | ISource2GameEntities: IAppSystem slots, baseline-name query, empty-list transmit / pack callbacks | engine2/isource2server.h |
| NetChannelSuite | INetChannelInfo, INetChannel: a playback channel built with INetworkSystem::CreateNetChannel (no client needed) | inetchannelinfo.h, networksystem/inetchannel.h, inetworksystem.h |
| HostStateMgrSuite | IHostStateMgr: IAppSystem slots, every Request skipped; CHostStateMgr / CHostStateRequest layout | engine2/hoststate.h |
| LocalizeSuite | ILocalize: token lookup by name and index, language state, Construct* formatting | localize/ilocalize.h |
| ConVarSuite | ICvar, ConVar, ConCommand; 5 layouts, 6 hooks | tier0/icvar.h, convar.h |
| SchemaSystemSuite | ISchemaSystem, CSchemaSystemTypeScope; 9 layouts | schemasystem/ |
| GameEntitySystemSuite | CGameEntitySystem / CEntitySystem; 8 layouts | entity2/, engine2/igameresourceservice.h; `s2sdk/game/shared/gameentitysystem.h` |
| GameEventSuite | IGameEventSystem, IGameEventManager2 (not reachable, reported as N/A); 9 hooks | engine2/igameeventsystem.h, igameevents.h |
| NetworkServerSuite | INetworkServerService, INetworkGameServer; 15 cross-checks with other interfaces; 5 hooks | engine2/inetworkserverservice.h, inetworkgameserver.h, networkgameserverbase.h, tier0/netadr.h |
| NetworkMessagesSuite | INetworkMessages, protobuf round trip | networksystem/inetworkmessages.h, inetworkserializer.h; tier0/logging.h |
| FileSystemSuite | IFileSystem, read-only | filesystem/ifilesystem.h |
| GamedataSuite | `configs/gamedata.toml`: signatures, slots, offsets; 2 hooks | none of its own |
| MetamodListenerSuite | ISmmAPI / IMetamodListener (Metamod, not s2sdk) | metamod-source `ISmmPlugin.h` |
| Tier0ContainerSuite | CUtlVector, CUtlString, CUtlBuffer, CBufferString read from engine objects | tier0/utl*.h, buffer_string.h |
| KeyValues3Suite | KeyValues3 filled by the engine; needs `CS2SDKTEST_TIER1_KEYVALUES3` | tier0/keyvalues3.h |

## Suites that differ from the rest

- **MetamodListenerSuite** tests Metamod. Its "hooks" are listeners that cannot be removed, only silenced.
- **ConVarSuite** registers its own convars and commands to have ground truth. Only POD types are used, because `CS2SDKTEST_USE_MEMOVERRIDE` is off and a string convar would put our heap buffer into engine-freed storage.
- **GameEntitySystemSuite** has no interface to query. It finds the entity system at a byte offset inside IGameResourceService and measures that guess. It also writes into the game (`AddListenerEntity`) and compiles `s2sdk/entity2/*.cpp`.
- **GameEventSuite** cannot reach IGameEventManager2 (no version string answers). It records that, and lists the interface's methods as N/A so the day it starts answering is noticed.
- **NetworkServerSuite** is mostly cross-checks: tick, client limit, map name, etc. against IVEngineServer2 and CGlobalVars. Its hooks are on INetworkServerService only, because the engine rebuilds the game server on level change.
- **NetworkMessagesSuite** tests that name, partial name, id and no-create lookups agree. It needs `netmessages.proto` in `CS2SDKTEST_PROTO_FILES` and `CS2SDKTEST_TIER1_BITBUF` (default ON); without bitbuf, serialization rows report SKIP.
- **GamedataSuite** is driven by a file, not an SDK interface. A signature must match exactly one address; zero matches is the headline finding on update day.

## Working with them

- Adding a suite: write it, register it in `TestRunner::CreateDefault()`, add it to `CMakeLists.txt`, add a row above. The interface is in `src/testkit/ITestSuite.h`; what to write next is in `docs/TEST_TARGETS.md`.
- Bumping the s2sdk submodule: `git -C s2sdk diff --stat <old>..<new> -- public game common` lists the moved headers; the table says which suites to re-audit. Method-level check: `.claude/skills/cs2-add-suite/scripts/audit-coverage.sh`.
- A *module* is a loaded shared library (`engine2.so`, `server.so`, see `data/GameModules.h`). A *suite* is one of these files.
