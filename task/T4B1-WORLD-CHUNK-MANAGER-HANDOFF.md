# T-4b-1 World Chunk Manager Handoff

Date: 2026-11-12 (Asia/Shanghai)

## Status and scope

T-4b-1 adds an opt-in WorldChunkManager that owns TicketSourceManager and
ChunkLifecycleController and performs real terrain chunk load, main-thread World
install, block-edit tracking, background save, actual unload, and reload.

W-3a later supersedes the no-saved-tick restriction documented below; see
task/W3A-TICK-PERSISTENCE-HANDOFF.md.

The existing World offline path is unchanged. World::loadChunk/unloadChunk,
offline import, server startup, and native --world behavior are not switched to
this manager. No native option was added.

This stage is terrain-only. Chunks containing saved or runtime block/fluid ticks,
block entities, piston block entities, block events, or terrain entities are
rejected or blocked from save/unload instead of being silently discarded.

## Files

Added:

- include/redstone/world_chunk_manager.hpp
- src/world_chunk_manager.cpp
- tests/world_chunk_manager_tests.cpp
- validation/vanilla26_1/TerrainWorldCycleFixtureProbe.java
- validation/vanilla26_1/run_world_cycle_fixture.sh
- task/T4B1-WORLD-CHUNK-MANAGER-HANDOFF.md

Changed:

- include/redstone/engine.hpp
- src/engine.cpp
- CMakeLists.txt

Existing uncommitted work was retained. No commit or push was made.

## Opt-in API

WorldChunkManager(World&, regionDirectory, viewDistance, simulationDistance) or
the injected RawChunkStore overload enables the path only for that manager.

The manager provides:

- TicketSourceManager access and player/view-distance/forced wrappers.
- applyTicketUpdate for externally produced TicketSystemUpdate.
- nonblocking pump().
- setBlock/rawSetBlock wrappers.
- managedStatus/lifecycleStatus/isManaged.
- retrySave.
- close returning ChunkShutdownResult, plus lastShutdownResult.

World still permits direct offline load/unload. Managed chunks must be routed
through the manager in a later native wiring stage.

## Ticket demand and loading

Ticket loadingChanges call ChunkLifecycleController::applyLoadingChanges.
Missing or unsupported chunks remain Missing/Failed. No generation or empty air
chunk is created.

The worker reads and decodes a FULL chunk. Main receives moved section and tick
views. A terrain-only install performs:

1. Reject nonempty block ticks, fluid ticks, block entities, or entities.
2. Range-check each decoded section against World minY/maxY.
3. Convert a one-entry palette to a zero-storage World single value.
4. Convert a palette of at most 256 entries to World SectionStorage8.
5. Convert larger palettes to SectionStorage16.
6. Expand packed NBT cells once into World's indices arrays.
7. Prime heightmaps from the newly built local sections.
8. Insert the completed World Chunk and call loadChunk once, which adds both
   tick containers and submits one light snapshot.

The manager tracks a monotonic per-position light lifecycle epoch across actual
erasure and reload. Tests observe a newer published epoch after reload.

## Main-thread conversion payload

A nonuniform 8-bit section uses:

- fixed World indices: 4096 bytes;
- fixed palette capacity: 256 * 4 = 1024 bytes;
- section header/shared_ptr overhead.

A nonuniform 16-bit section uses:

- fixed World indices: 8192 bytes;
- fixed palette capacity: 4096 * 4 = 16384 bytes;
- section header/shared_ptr overhead.

These are World-owned fixed storage, not extra copies of the NBT DOM. The moved
decoded palette supplies state IDs. The direct suite covers a multi-entry 8-bit
palette; the 16-bit path compiles but was not separately stress-fixtured.

Heightmaps are recomputed by reading the installed local sections. No stale NBT
heightmap is copied.

## Edit tracking and background save

World has an opt-in managedBlockMutationObserver, empty by default. Every
successful setBlock/rawSetBlock state change, including nested World calls,
reports the previous and new state.

For managed chunks the observer:

- computes NBT section Y and local cell index;
- records one latest BlockEdit per block position;
- increments dirtyVersion;
- calls lifecycle markDirty.

Edits in sections absent from the NBT template or states outside the persistent
codec support set a blocker. The World edit remains in memory, but save and
unload are refused rather than writing a partial terrain result.

pump starts background saves for released dirty chunks. Save commands carry the
exact snapshot version and move a small BlockEdit vector to the lifecycle worker.
The worker retains the original NBT root, patches it in place, encodes it, and
writes through RegionFileStore.

If new edits arrive during save v1, manager retains pending v2 edits. The old ack
cannot clear v2. Manager submits v2 after the v1 ack. Tests verify both writes and
reload both edited cells.

Save failure leaves the World chunk resident and dirty. manager.retrySave clears
the failed snapshot marker and retries the retained patch. A successful retry
with no demand proceeds to unload.

Workers own the NBT root and encoded output. Main only keeps small BlockEdit
records and a small retry copy.

## Actual unload and light/tick lifecycle

World has a manager-only real erase path, separate from legacy unloadChunk:

1. Refuse while World is ticking.
2. Refuse if the chunk has block/fluid ticks, block events, piston block entities
   or tickers, or any runtime terrain entity.
3. Reset the direct chunk/section read cache before storage destruction.
4. Emit the existing light unload tombstone and lifecycle observer edge.
5. Remove block and fluid tick containers and their owned storage.
6. Remove pending block-event state for the chunk.
7. Erase the World Chunk node and leave the cache clear.

Clean no-demand chunks unload without writing. Dirty chunks unload only after a
clean save ack and a fresh no-demand check.

The real test verifies:

- getBlock stone after install;
- setBlock glass;
- background save;
- actual World node erase and cache invalidation;
- tick container removal;
- one light load and unload edge;
- reload glass with a newer light epoch;
- unchanged gameTime;
- retained untouched NBT extension.

## Explicit safe shutdown

WorldChunkManager::close() is an explicit draining shutdown:

1. Set closing and reject new ticket/demand/write work.
2. Reject load completions that arrive during shutdown instead of installing.
3. Save every dirty installed managed chunk, including held-ticket chunks.
4. Wait for accepted saves. For v1 in flight plus v2 pending, observe ack v1,
   retain dirty v2, then submit v2 before stopping I/O.
5. Wait until all accepted and pending save versions complete or fail.
6. If all managed chunks are clean, remove demand, retry eviction, and actually
   unload them.
7. Stop/join the lifecycle worker, drain final results, then clear the World
   mutation observer.
8. Return ChunkShutdownResult with complete, workerStopped, savedChunks, and
   unsavedChunks. Each issue contains position, state, dirty/saved versions, and
   error text.

Save failures and encoding/dynamic blockers set complete=false. Their World
chunks remain resident. The destructor calls the same logic through a noexcept
best-effort wrapper; explicit close is the inspectable path.

Normal pump remains nonblocking. close is the only manager operation expected to
wait for disk. Atomic closing/closed flags let main observe that new work was
rejected while a separate close call waits.

## Dynamic persistence boundary

Saved chunks with block/fluid ticks, block entities, or entities are rejected at
install and never written.

Runtime scheduled ticks, block events, piston block entities/tickers, or terrain
entities set a persistent blocker. Such chunks remain resident and appear in
shutdown results if close cannot save them.

Terrain entity-region persistence, block-entity serialization, scheduled-tick
serialization, generation, persistent chunk tickets, level.dat, and tick gates
remain later work.

The manager exercises current World light lifecycle/publication hooks, but no
native network residency consumer is enabled in this stage.

## Direct verification

The suite uses fake clean/dynamic/blocking/failing/updated stores and a real
World with lighting enabled plus a Java 26.1 NbtIo/RegionFile terrain-only
fixture.

Covered:

- direct multi-palette to World SectionStorage8 install;
- dynamic NBT rejection without World air chunks;
- stale read cannot install over a reissued epoch;
- setBlock plus rawSetBlock while save v1 is blocked, both persisted;
- save failure stays resident/dirty and explicit retry succeeds before unload;
- runtime sand scheduled tick blocks save and unload;
- real region load/edit/save/actual erase/reload;
- light lifecycle edges and monotonic reload epoch;
- unchanged gameTime and retained unknown NBT;
- tick container and read-cache cleanup;
- held-ticket edit plus explicit close, then new manager reads from disk;
- blocked v1 plus pending v2, close persists v2 before stopping;
- save failure and dynamic blocker close reports;
- late read rejected during close.

Commands:

    WORLD_CYCLE_FIXTURE_OUT=/home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-world-fixture-20261112 \
      bash validation/vanilla26_1/run_world_cycle_fixture.sh

    /home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build/world_chunk_manager_tests \
      /home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-close-20261112-scratch-disk1 \
      /home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-world-fixture-20261112/region

Result:

    world chunk manager tests passed (with real World disk fixture)

The real suite passed ten consecutive times. engine_tests was built and run after
the rawSetBlock fast-path correction and exited 0 with empty stdout.

No CTest or light suite was run.

## Logs and hashes

- Build: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-close-20261112-build1.log
  SHA-256 214effc68ae6b1ba5b1311bba7d38f789d2bfa2720dc335d4bd7d0e52382f747
- World direct test: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-close-20261112-test-disk1.log
  SHA-256 936988b0ec2318a17af306fa3c424709237a2d37a0567434b2dee3796f1ce543
- Ten-run stability: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-close-20261112-stability.log
  SHA-256 83c8e65a75ddeb3d69399554c8d08a2efb292ad6be9e46c83a57ffb8548bc34a
- engine_tests: /home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-close-20261112-engine-test.log
  SHA-256 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  (empty stdout; process exited 0)

Current source/binary hashes:

    include/redstone/world_chunk_manager.hpp  34527227c7b6644e0dc24b08a1daf3f9b2a1e7e15b0e41d45ce198d5adfc5813
    src/world_chunk_manager.cpp               077a5933666478e770e86b1a9f825ba2cdb24f3c795cf45381f3e6da36826341
    tests/world_chunk_manager_tests.cpp       e3f1de80ccc262e709fd45935b753c02c4338b0ac0b574173dc001e36c024f64
    include/redstone/engine.hpp               5f059d41e430b3feeeb23bdb24fe18a3b52ada658581e5b24e9ded27527a7a11
    src/engine.cpp                            e7950a774a841af7416a88508523495daa0f2d6348f4fcd4210db89926a9f581
    world_chunk_manager_tests binary          df2fc8459f35225e6a8681f6210300a2de7c6a1642e44f590e6f0094e33da17d

Official fixture region:
/home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-world-fixture-20261112/region/r.-1.-1.mca
SHA-256 005e94eb0797e097bfa4d23d92f7120d6dc3c9450ccbe25ed1355914261baa75

The test copies this file before writing; the fixture source is read only.

## Remaining limits

- No native --world/server activation.
- No generation of absent chunks.
- No terrain entity-region or generic block-entity persistence.
- No saved/runtime scheduled-tick persistence; such chunks are blocked.
- No persistent chunk ticket file or level.dat.
- No native network residency consumer beyond existing World light observers.
- Dynamic blockers are sticky in this stage.
- One World can have one managed chunk owner and the manager must outlive World.
- rawSetBlock's no-manager path no longer constructs an old BlockState; it reuses
  Section::set's previous ID. TickQueue comment matches immediate unique_ptr
  storage erase.
