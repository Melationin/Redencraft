# T-3 Memory Fix and T-4b-2 Native Opt-in Handoff

Date: 2026-11-12 (Asia/Shanghai)

## Status

Two T-3 review issues were fixed before native wiring:

- per-read entity readiness metadata no longer lives in a token map;
- readiness caching is bounded to managed resident centers.

T-4b-2 then adds an explicit native --ticketed-world mode. Legacy --world,
--schematic, in-memory, and all existing startup behavior remain on their old
paths unless the new option is supplied.

No commit or push was made. No CTest or full light suite was run.

## T-3 memory corrections

StoreReadResult now carries an optional EntityChunkDataResult. A successful
read moves it into LifecycleLoadedChunk::entityData and then into main's install
callback. Missing, corrupt, unsupported, stale, failed, and shutdown-dropped
reads release it with the read result. There is no token map, counter, mutex, or
side allocation left after completion.

The readiness cache now contains entries only for currently managed resident
center positions. Ticket refreshes still update a bounded affected neighborhood,
but they erase rather than insert entries for missing neighboring chunks.
Managed install computes the installed center, and actual eviction erases it.
tickReadiness for an uncached managed center computes a cheap diagnostic on
demand; missing centers return the default false result without insertion.
Long travel through missing chunks therefore cannot grow the map.

WorldChunkManager exposes only two test counts: entityReadMetadataCountForTest
(now structurally zero) and readinessCacheSizeForTest. Tests burst hundreds of
missing reads and probe hundreds of nonresident positions, then verify no
retained entity metadata and an empty readiness cache.

## Native opt-in mode

Command form:

    redstone_vanilla_server --ticketed-world <dir> [--port N] [--ticks N]
      [--view-distance N] [--ticket-view-distance N]
      [--simulation-distance N] [--light-workers 1] [--no-console]

Help calls out that this mode opens overworld region files for writes.

Mode selection is mutually exclusive with --world and --litematic/--schematic.
--ticketed-world also rejects --chunk, --offset, and --fluid-basin in this
bounded version. Existing --world and schematic parsing remain unchanged.

Startup reads only level.dat and namespaced world-gen seed/time metadata through
loadWorldMetadata. It does not call loadAnvilWorld, scan terrain, or import the
world. The selected overworld region is checked for existence, then owned by a
single WorldChunkManager.

Existing --world continues to use loadAnvilWorld and its old offline chunk set.
No native world option defaults to the old in-memory fixture.

## Session residency

Session has an opt-in managed residency provider. Legacy sessions never install
it and preserve their existing availableChunks logic.

New managed connections receive a snapshot of currently resident managed chunks
so initial/refresh view can include chunks loaded before Play. A newly resident
chunk is added to availableChunks and queued for the ordinary light-ready/batch
path; a managed unload erases availability, expires the chunk generation token,
removes pending/awaiting-light state, and sends forget_level_chunk if it had been
sent.

The provider uses manager residency only for availability. It does not collapse
simulation readiness or client view readiness into the same flag. Initial
chunks still wait for protocol-ready light through the existing async path.

Session managed residency hooks are invoked only from the main server thread by
WorldChunkManager install/removal callbacks. The initial provider is also seeded
on the main thread at connection creation.

## Server loop integration

For --ticketed-world only:

- controlled player presence and movement feed WorldChunkManager::setPlayer;
- disconnected players remove their source;
- manager callbacks mark managed chunks available/unavailable to sessions;
- manager pump runs nonblocking on each server-loop iteration;
- manager serverTick receives the same tickChunks/runsNormally values as the
  old World tick path;
- shutdown publishes TICKETED_WORLD_CLOSED with complete/saved/unsaved/worker
  fields, prints each unsaved chunk, and returns nonzero if shutdown is
  incomplete.

The ordinary --world path continues calling world.tick exactly as before.

## Smoke and validation

The smoke fixture is an isolated temporary world directory. Its terrain region
was copied from the official fixture; level.dat was generated in the scratch
world with DataTime/seed metadata. No original save was written.

Command:

    redstone_vanilla_server --no-console \
      --ticketed-world <scratch>/world --port 0 --ticks 2

Observed:

    LISTENING 127.0.0.1:<ephemeral> protocol=775 allocator=jemalloc workers=1
    TICK tick=43 ... 
    TICKETED_WORLD_CLOSED complete=1 saved=0 unsaved=0 worker_stopped=1
    STOPPED ticks=2 mean_ms=0.8 max_ms=1.7 backpressure_disconnects=0

The no-save result is expected: no forced/player source was supplied, so no
managed chunk had to be installed or dirtied.

Conflict checks:

- --ticketed-world plus --chunk is rejected;
- --ticketed-world plus --light-workers 2 remains rejected by the existing
  single-worker server option.

Direct tests:

    world chunk manager tests passed (with real World disk fixture)
    light protocol tests passed: 227 packets
    fluid tick tests passed
    ticket tests passed
    JAVA_READ_CPP_TICKS_OK

storage_tests and engine_tests exited 0. The manager real-fixture suite passed
ten consecutive runs.

The protocol suite now includes a managed availability case: a current managed
chunk participates in initial view, a nonresident managed-chunk update is an
idempotent erase, and existing light-ready gating remains untouched.

## Logs and hashes

- Native build: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b2-20261112-build-final.log
  SHA-256 725a7499231fbeebb428d429b43f440ae51a08fca76b52e5050b39e2166b914a
- Native smoke: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b2-20261112-native-smoke.log
  SHA-256 a55c71160ed0715d6b6877d5b87c0b0158b2aa0f3e801118624dbb8780cbbc39
- Manager: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b2-20261112-manager-final.log
  SHA-256 936988b0ec2318a17af306fa3c424709237a2d37a0567434b2dee3796f1ce543
- Protocol: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b2-20261112-protocol-final.log
  SHA-256 37535dead350c6828d5d6c94394a5b1f9bfeee78590fa3d9228be3eb97ecf440
- Java tick readback: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b2-20261112-java-final.log
  SHA-256 528309274ca88d3f31851e114e58b9b407fe8ec448b737bad55ac6d5d8b73356
- Ten-run stability: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b2-20261112-stability.log
  SHA-256 83c8e65a75ddeb3d69399554c8d08a2efb292ad6be9e46c83a57ffb8548bc34a

Current source/binary hashes:

    include/redstone/chunk_lifecycle.hpp       7dea897b6fac8042ac7f841c44079b3f65b005efd547596050cc9a03358b3b8e
    src/chunk_lifecycle.cpp                    2636af1dd362b817eed12c2030f92186668ebcbc65d201ec084f88005e8d4fd5
    include/redstone/storage.hpp               453b662bca680fd8b4f7ada4ea0fc4b69f5cc0849c24987382f77d07c114631b
    src/storage.cpp                            3dbd27d25b929d17330c60630ae0a2f0c484c1665a17f18e88cc680a02bd3174
    include/redstone/world_chunk_manager.hpp   c898ce2367994ec4f85a601a52998f4438da20b4541fa166ec89436cb89dc3b6
    src/world_chunk_manager.cpp                47ed49fb8a3198a170016eec9d8292c97e266aa40666a6f4af5197a60c11c028
    include/redstone/net/session.hpp           e6b6edb0b73a37fb7e07c9efa94f1b92918789fb001d1c2bc99db24eb41d82ee
    src/net/session.cpp                        8289467d0a9c3ff76ce7faca9261287e49c3e51ae1771d38269fd7ec4f930e9d
    src/net/server_main.cpp                    6b39ad1d3bc95247221f1a9da1f336904e72846194ea5b6767f730509b90e6e6
    tests/world_chunk_manager_tests.cpp        be516d9f458657a3ea978720ff6c5a7beb81d3e894db785f689b43f30e2a7d3b
    tests/light_protocol_tests.cpp             3e0f8ba70a2f9457793f0b0148a9c5cb95d3e6a43e88339a6e6d04f959570042
    tests/storage_tests.cpp                    2966cb73377e77117970380dc16f4ed7995d2946948243a0d549c0bc603426d7
    redstone_vanilla_server binary             a976f755bfab373406e42a4d8632ce224e1fdf0045133e4fea2eeb03e14b6c73
    world_chunk_manager_tests binary           475d9dff283a0cac5b5dfd6a06a53a8fd5e5737b5cfac59d550417faf75e6a2f

## Reviewer repair round (2026-11-12)

The T-3 / T-4b-2 review fixes are now in the working tree:

- The session managed residency provider defers Play packet encoding until Play. A focused protocol test covers Handshake installation with no Play packets, Configuration-period residency changes, and a join snapshot that includes already-resident chunks.
- TicketedWorldLock acquires session.lock with Linux POSIX F_SETLK before metadata or region access and holds it through manager close. Non-Linux hosts reject this write mode. Isolated probes prove a second native process and a Java FileChannel.tryLock holder are rejected; Java release permits the native lock probe.
- Connection/player IDs are lifecycle-monotonic. A focused ticket test covers two co-located players, a disconnect, a fresh-key reconnect, and final removal without overwriting the remaining player.
- Help lists --ticketed-world and its actual write/unsupported-data boundary. Shutdown prints TICKETED_WORLD_FATAL when present.

The managed TCP smoke edits a real disk chunk and restarts. Round one removes stone at -13,0,-16; round two joins the same disk chunk and places smooth stone. Both rounds require the exact target block update, complete=1, unsaved=0 and no fatal. The restart readback is the persistence proof. A disconnect can release player tickets and save before explicit close, so saved=0 at close is valid; final8 also shows saved=1.

Evidence:

    /home/zhddsj/.local/state/redstone-cpp/wsl-reviewfix-20261112-managed-tcp32.log
    SHA-256 d5beb47290c05872b859cfa76e7fd59b925c020c95614f94a31622c0852138e0
    /home/zhddsj/.local/state/redstone-cpp/wsl-reviewfix-20261112-managed-tcp-final8.log
    SHA-256 241ba4c6c7ec4f287e3edbacd6257f3eccccda492227e8ddbdbf5287d481a542
    /home/zhddsj/.local/state/redstone-cpp/wsl-reviewfix-20261112-managed-tcp-final9.log
    SHA-256 8645e70eadbe6bbc163f50b8c11e66de1655f57e9da6de8d2cb5f24d43f1924c
    /home/zhddsj/.local/state/redstone-cpp/wsl-reviewfix-20261112-managed-tcp-final10.log
    SHA-256 eb990ffe4f1eb4a076651480b6488ce71806ff21db31a4eac292a1c3a4fb1c4c
    /home/zhddsj/.local/state/redstone-cpp/wsl-reviewfix-20261112-java-readback.log
    SHA-256 49686e991303855eb80dc2e1265b324472d8e6036638cf0e2f4d3ec34245f901
    /home/zhddsj/.local/state/redstone-cpp/wsl-reviewfix-20261112-java-edited-readback.log
    SHA-256 ea46db7eba848b33c04cfc5340a4e2fb381f027b0a829d365011498714a0482f

The Java 26.1 RegionFile/NbtIo probes report
JAVA_MANAGED_CHUNK_OK DataVersion=4786 sections=1 and an edited palette that
contains minecraft:stone, minecraft:air and minecraft:smooth_stone. The latter
independently confirms both native rounds in the saved terrain.

Final focused validation:

    light_protocol_tests assets/protocol775 -> 249 packets
    tickets_tests                          -> passed
    storage_tests                          -> exit 0 (silent)
    world_chunk_manager_tests <scratch> <terrain-region> <tick-region>
                                           -> passed (with real World disk fixture)

No CTest, commit, or push was run.

## Explicit limits

- The native smoke uses an isolated generated world directory with the official terrain fixture copied into its overworld region; the original save was not written. It proves native disk edit/save/restart readback, while the manager direct real-region cycle separately covers terrain install/save/reload.
- No generation. Missing chunks remain Missing.
- Nonempty/unsupported entity-region data is rejected; this stage still does not
  deserialize entities.
- Block-entity persistence remains unsupported and blocked.
- The persistent ticket file, level.dat saving, and full ServerLevel restart are not implemented. --ticketed-world persists the overworld region and holds session.lock; it does not claim full vanilla restart equivalence.
- No light propagation change. Existing first-send light readiness remains.
- No CTest, commit, or push.
