# T-3 Tick Readiness and Ticket Timeout Handoff

Date: 2026-11-12 (Asia/Shanghai)

## Status

T-3 adds opt-in tick readiness for managed chunks and moves timed-ticket aging
to an explicit real-server-tick API.

The legacy offline World path is unchanged. With no WorldChunkManager, all
readiness hooks are empty and World scheduled ticks, block events, and entities
retain their previous behavior.

No generation, entity serialization, native server activation, or lighting
propagation change was made.

## Files

Changed:

- include/redstone/engine.hpp
- src/engine.cpp
- src/entity.cpp
- include/redstone/chunk_lifecycle.hpp
- src/chunk_lifecycle.cpp
- include/redstone/world_chunk_manager.hpp
- src/world_chunk_manager.cpp
- tests/world_chunk_manager_tests.cpp
- task/W3A-TICK-PERSISTENCE-HANDOFF.md (stage note only)

Added:

- task/T3-TICK-READINESS-HANDOFF.md

No commit or push was made. Existing uncommitted work was retained.

## Original semantics followed

ServerChunkCache.isPositionTicking combines the block-ticking distance range
with a successfully promoted ticking ChunkHolder. ServerLevel
isPositionTickingWithEntitiesLoaded additionally requires entities to be
loaded. Entity ticking requires canPositionTick plus the entity-ticking
distance range.

ChunkHolder promotes BLOCK_TICKING with a 1-chunk FULL dependency ring and
ENTITY_TICKING with a 2-chunk FULL dependency ring.

ServerChunkCache purges timed tickets when
tickRateManager().runsNormally() is true, or when chunk ticking is disabled.
The manager mirrors that condition only in serverTick; pump never ages tickets.

Block events for unavailable/unready chunks are deferred in
blockEventsToReschedule, then re-added after the pass.

## World readiness hooks

World now has two optional read-only predicates:

    std::function<bool(Pos)> blockTickPositionReady;
    std::function<bool(Pos)> entityTickPositionReady;

They are empty unless WorldChunkManager installs them.

For scheduled ticks, a due tick is either dispatched, deferred while not
ready, or kept at its existing queue position. Deferral does not delete or
reorder it. When readiness returns, DRAIN_ORDER still applies:
trigger, priority, subTick order.

For block events, an unready position sends the event to the existing
blockEventsToReschedule list; the event is re-added after the pass rather than
dropped.

Entity position iteration consults entityTickPositionReady after the existing
chunk-loaded check. Empty hook preserves the old offline path.

The World tick hot path performs a cached map lookup for readiness. It does not
enumerate world chunks or scan the world per cell.

## Readiness computation and cache

WorldChunkManager::tickReadiness(position) returns:

- loadingLevel;
- simulationLevel;
- blockTicking;
- entityTicking;
- entityDataReady;
- limitation.

The cached rules are:

- block/fluid scheduled tick requires the center chunk installed, entity data
  known empty, simulationLevel <= 32, and every position in the center +1 FULL
  ring installed with loadingLevel <= 33.
- entity position tick requires the center and +2 FULL ring installed and
  simulationLevel <= 31, with entity data known empty.
- loading level 33 vs simulation level 31/32 are separate inputs.

The cache is refreshed only on:

- loading/simulation ticket changes, within their bounded +/-2 area;
- managed install;
- managed actual removal.

It is not recomputed every pump, socket loop, or game tick.

## Entity data gate

This stage still does not deserialize entity-region data.

The RegionFileStore constructor checks the sibling entities region slot on the
same I/O worker before terrain installation. A Missing entity slot means Empty.
An unsupported/corrupt/nonempty entity chunk is reported and installation is
rejected rather than substituting an empty entity set.

Tests cover an injected NonEmpty result and a real sibling entity-region file
that contains one entity. Neither creates a World air chunk.

The entity probe travels with the worker read using a request token. Main drains
the matching token only with that read result; main does not perform entity
region disk I/O.

## Ticket timeout and holder state

WorldChunkManager::serverTick(tickChunks, runsNormally):

1. Ages timed tickets only when runsNormally is true or tickChunks is false.
2. Applies any resulting demand changes.
3. Calls World::tick only when runsNormally && tickChunks.
4. Pumps nonblocking lifecycle and save work.

Repeated pump does not age tickets or advance gameTime.

The ticket holder lookup reports present for Reading, Resident, Saving, and
EvictPending. readyForSaving is true only for clean Resident. A save in flight
is therefore not claimed ready for saving, so non-expire-if-unloaded tickets
pause their countdown as original TicketStorage requires. Missing/Failed holders
are treated as absent.

Tests verify:

- multiple pumps do not reduce ticksLeft;
- paused serverTick neither advances gameTime nor ages tickets;
- one real serverTick ages exactly once;
- an in-flight save pauses aging;
- after a clean save ack, aging resumes.

## Block-event unload behavior

Pending block events permit a terrain/tick save. Actual unload initially waits
for the event. A one-game-tick grace retry is scheduled. If the event still has
no persistence form after that real game tick, the transient queue entries are
discarded as part of actual unload rather than locking the chunk forever.

A returning FULL demand cancels the transient grace schedule.

## Tests and evidence

Direct coverage:

- ready 3x3 install enables block ticking while entity ticking remains false;
- ready 5x5 ring enables entity ticking;
- negative-coordinate center and boundary neighborhoods;
- missing +1 FULL neighbor blocks ticking despite sufficient loading/simulation
  levels;
- due block/fluid ticks stay queued while unready and preserve subTick order;
- after neighborhood install and readiness refresh, due ticks execute in order;
- entity position remains paused without +2 entity readiness;
- nonempty entity-region and injected unsupported entity data reject install;
- no-manager offline tick/block-event behavior remains unchanged;
- due block event is deferred while unready and runs after readiness returns;
- real Java W-3a tick fixture still loads/saves/reloads and Java reads C++ NBT.

Direct outputs:

    fluid tick tests passed
    ticket tests passed
    world chunk manager tests passed (with real World disk fixture)
    JAVA_READ_CPP_TICKS_OK

engine_tests exited 0 with empty stdout. The manager real-fixture suite passed
ten consecutive runs.

## Logs and hashes

- Build: /home/zhddsj/.local/state/redstone-cpp/wsl-t3-20261112-build-final.log
  SHA-256 46b48de364f1cabb4b6bbd3c6cad6dc4867c5c5849153bc2458b3271953e7aa4
- Manager: /home/zhddsj/.local/state/redstone-cpp/wsl-t3-20261112-manager-final.log
  SHA-256 936988b0ec2318a17af306fa3c424709237a2d37a0567434b2dee3796f1ce543
- Java readback: /home/zhddsj/.local/state/redstone-cpp/wsl-t3-20261112-java-final.log
  SHA-256 3d3cb561b81ad06231aa70032fc2f22e717050de6006acee14c183bfbe9cb4e6
- fluid tick: /home/zhddsj/.local/state/redstone-cpp/wsl-t3-20261112-fluid-tick-final.log
  SHA-256 5ab5394e85f57303c8266cc195770a385d0f1149feb364095ee74cf207ff12f5
- tickets: /home/zhddsj/.local/state/redstone-cpp/wsl-t3-20261112-tickets-final.log
  SHA-256 4b34b208967a1a6650afa5a51edcea8cecbe7c62c837752fcdd3aa9825a1e85e
- engine: /home/zhddsj/.local/state/redstone-cpp/wsl-t3-20261112-engine-final.log
  SHA-256 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  (empty stdout; exit 0)
- Ten-run stability: /home/zhddsj/.local/state/redstone-cpp/wsl-t3-20261112-final-stability.log
  SHA-256 83c8e65a75ddeb3d69399554c8d08a2efb292ad6be9e46c83a57ffb8548bc34a

Current source/binary hashes:

    include/redstone/engine.hpp               c3e8b1034a0a8561dbef137268e9649fef828ef1f455661d275ecf52313cd7a6
    src/engine.cpp                            1b94bd00aa96791a8a3e2f917bca24403f571ba0d5b255706c1150bc0a150c67
    src/entity.cpp                            c1a81bad0eef877beea2bffbb5df41474e1dd1f7d5c2c293d988ded9ea6b2b59
    include/redstone/world_chunk_manager.hpp 7469324a11597551f47730b16fe3fc2679c2234aa23122aef040da648a842b8f
    src/world_chunk_manager.cpp               95da24e3e4eab6dc23c93292bbba6b0951ed34cf891253fcefe95f74f23d9ab4
    include/redstone/chunk_lifecycle.hpp      db38f3abcd27460a7a2a5628c0a735b4702533a10eab7ea749b09a7648d59fcc
    src/chunk_lifecycle.cpp                   f2e8920d5c0ef2a2d2502e957e903231f15590ab1de2b6292f6854e4ec2816f2
    tests/world_chunk_manager_tests.cpp       4fe0798409732f0b45c86352fa6984abe80093dd225953893e703bf488926710
    world_chunk_manager_tests binary          0bc93abb7fc736ea538ff32abb04c98d2500dd89f2246788aa3d53cd5db03973

## Remaining limits

- No entity-region installation or generic entity serialization. Nonempty or
  unsupported entity data rejects the managed chunk.
- No block-entity persistence.
- The readiness cache is an in-process manager projection, not an async chunk
  future. A later T-4b-2 integration should connect it to holder promotion and
  World resident callbacks.
- Transient block events have one-game-tick unload grace, then are discarded
  during managed unload because they have no chunk persistence form.
- No generation, persistent tickets, level.dat, native server startup, or CLI
  switch.
- No CTest, light suite, commit, or push was performed.
