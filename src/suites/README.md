Test suites live here — one per SDK interface being exercised.

  Source2EngineSuite       IVEngineServer2 / ISource2Engine / IAppSystem (calls + CGlobalVars layout)
  Source2ServerSuite       ISource2Server (= IServerGameDLL)             (calls + 11 hooks)
  Source2GameClientsSuite  ISource2GameClients (= IServerGameClients)    (calls + 11 hooks)
  ConVarSuite              ICvar / ConVar / ConCommand / ConCommandRef   (calls + 5 struct layouts + 6 hooks + 2 native listeners)
  SchemaSystemSuite        ISchemaSystem / CSchemaSystemTypeScope        (93 calls + 9 struct layouts)
  GameEntitySystemSuite    CGameEntitySystem / CEntitySystem             (calls + 8 struct layouts + 4 listener callbacks)
  GameEventSuite           IGameEventSystem / IGameEventManager2         (calls + 1 struct layout + 9 hooks)
  NetworkServerSuite       INetworkServerService / INetworkGameServer    (calls + netadr_t layout + 15 cross-checks + 6 hooks)
  NetworkMessagesSuite     INetworkMessages / protobuf bindings          (calls + LoggingChannel_t layout + protobuf round trip)
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

GameEntitySystemSuite is another odd one. There is no interface to query:
hl2sdk gives IGameResourceService a name and nothing else, so the entity system
is fished out of the service object at a byte offset the ecosystem agrees on —
which the suite treats as a hypothesis and measures. It is also the only suite
that writes into the game (AddListenerEntity), and the only one that compiles
SDK translation units (hl2sdk/entity2/*.cpp) in order to test them; it supplies
the GameEntitySystem() definition those need.

GameEventSuite is the third, and the only suite that reports on an interface it
cannot reach. IGameEventSystem is an ordinary CreateInterface away, but
IGameEventManager2 — the interface plugins actually write against — has no
version constant anywhere in hl2sdk's cs2 branch, and no module answers any name
it has ever had. The CS2 ecosystem gets at it by calling a virtual on
ISource2Server at a gamedata-supplied offset and subtracting 8; this project has
no gamedata, and blind-calling server vtable slots to find that one would hit
PreFatalShutdown on the way past. So the suite asks for every historical version
string, records that nothing answers, and then lists IGameEventManager2's and
IGameEvent's methods one row each as N/A — because the day one of those probes
starts answering is a day worth noticing, and a surface nobody listed cannot be
noticed.

It is also the only suite whose hooks feed its calls. Naming a message means
walking an engine object, which a hook — running unguarded in the engine's own
frame — must not do. So the hooks record the distinct INetworkMessageInternal
pointers they are handed into a fixed array, and the next call sweep resolves
those to names under the fault guard. Install the hooks, play a round, sweep
again.

NetworkServerSuite is the one that mostly does not test its own interfaces.
Almost nothing INetworkGameServer reports is a fact it owns - the tick, the
client limit, the map name, the globals pointer and the per-player ids are all
reachable through IVEngineServer2 or CGlobalVars too. That would normally be a
reason to skip a method; here it is the point, because two unrelated interfaces
disagreeing about the tick count is a finding neither of them could produce
alone. Fifteen such pairs are checked, and the best of them (GetTime against the
tick-times-interval formula the SDK writes out, GetName fed back through
IEngineServiceMgr::FindService) do not go through this plugin's own arithmetic
at all.

It is also the only suite that reaches IEngineService, the 11 slots between
IAppSystem and any engine service, and the only one that touches multiple
inheritance: CNetworkGameServerBase has three bases, so a pointer to it has two
different values depending on which one it is viewed through. That adjustment is
reported and never judged - the compiler derives it from the SDK header, so
there is no second source to check it against, but it is a number, and a number
that moves after an update is a changed declaration.

Its hooks are all on INetworkServerService and none on the game server, which is
a decision rather than an omission: the engine rebuilds CNetworkGameServerBase
on level change, so a hook there stops firing at exactly the moment the
interesting thing happens - and a hook that stopped firing looks identical to a
method the engine no longer calls. The replacement is measured instead: the
suite remembers the pointer it saw last sweep and says so when it changes.

NetworkMessagesSuite is the one that tests an agreement rather than an
interface. A net message is reachable four ways - by name, by partial name, by
id, and by the no-create form of the creating method - and the registry is
healthy exactly when all four hand back the same entry. The deepest check is
the protobuf round trip: an engine-allocated CNETMsg_Tick gets a known tick
value planted through the generated accessor, goes out through
SerializeMessageInternal, and has to come back with the same value through two
different unserialize entry points. A protobuf descriptor change breaks that
before it breaks anything a plugin would notice by other means - which is the
whole reason docs/TEST_TARGETS.md #8 exists.

Two build facts this suite added. netmessages.proto (and its import closure:
networkbasetypes, source2_steam_stats, valveextensions) joined
CS2SDKTEST_PROTO_FILES, and cs2sdktest_generate_protobuf now collects that
closure itself rather than trusting the list to name every file - a generated
header #includes the .pb.h of every .proto it imports, and protoc does not
emit those on its own. And CS2SDKTEST_TIER1_BITBUF now defaults to ON: the
round trip needs bf_write/bf_read, whose constructors are exported by no
prebuilt library (tier0 contains exactly two bitbuf symbols, both
netadr_t::Serialize/Unserialize). A build with it off reports every
serialization method as SKIP rather than pretending.

The write into the protobuf is guarded by a measurement. CNetMessagePB in the
SDK is a stub over the engine's real class, so every write through ToPB<T>()
is an ABI assumption - and the suite checks it first: AsProto() (the engine's
own pointer to the protobuf sub-object) has to equal this build's computed
ToPB address, or set_tick is refused and the round trip reports SKIP with
that reason. The same comparison run as a *finding* rather than a gate is in
the allocation section, where a mismatch is Suspect, printed, never fatal.

Vocabulary: a *module* in this codebase is always a loaded shared library
(engine2.so, server.so — see data/GameModules.h). A *suite* is one of these
files: our test code for one interface that lives inside such a module.

See `src/testkit/ITestSuite.h` for the interface, `docs/TEST_TARGETS.md` for
what to write next, and the repository README for a worked example. Adding one
is: write the file, register it in `TestRunner::CreateDefault()`, add it to
`CS2SDKTEST_SOURCES` in CMakeLists.txt.
