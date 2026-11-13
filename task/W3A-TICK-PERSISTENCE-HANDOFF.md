# W-3a Managed Block/Fluid Tick Persistence Handoff

Date: 2026-11-12 (Asia/Shanghai)

## Status

W-3a removes the T-4b-1 restriction that rejected every managed chunk with
saved block_ticks or fluid_ticks. Managed terrain chunks can now install, age,
save, execute, pause across unload, reload, and shut down with their per-chunk
block and fluid tick lists intact.

Terrain block entities and entities remain unsupported and are still rejected.
No entity-region persistence, generation, tick gate, level.dat, persistent ticket
file, or native --world activation was added.

## Files

Changed:

- include/redstone/engine.hpp
- src/engine.cpp
- include/redstone/terrain_codec.hpp
- src/terrain_codec.cpp
- include/redstone/chunk_lifecycle.hpp
- src/chunk_lifecycle.cpp
- include/redstone/world_chunk_manager.hpp
- src/world_chunk_manager.cpp
- tests/world_chunk_manager_tests.cpp
- tests/terrain_codec_tests.cpp
- task/T4B1-WORLD-CHUNK-MANAGER-HANDOFF.md (stage note only)

Added:

- validation/vanilla26_1/TerrainTickWorldCycleFixtureProbe.java
- validation/vanilla26_1/TerrainTickReadProbe.java
- validation/vanilla26_1/run_tick_world_fixture.sh
- task/W3A-TICK-PERSISTENCE-HANDOFF.md

Existing uncommitted work was preserved. Nothing was committed or pushed.

## Original semantics

- SavedTick.unpack computes triggerTick as Java wrapping addition of current
  game time and saved delay.
- LevelChunkTicks.unpack assigns one independent -N..-1 subTick sequence to the
  saved block list and another to the saved fluid list. Duplicate saved entries
  use scheduleUnchecked and remain duplicated in the heap.
- ScheduledTick.DRAIN_ORDER is triggerTick, then priority, then subTickOrder.
- ScheduledTick.toSavedTick removes game time and casts the long difference to
  a 32-bit signed delay. W-3a performs the same low-32-bit conversion without
  saturation.
- Vanilla chunk NBT has no SubTick field. W-3a writes only i/x/y/z/t/p.

## World per-chunk tick API

World now provides:

    ChunkTickSnapshot snapshotChunkTicks(int chunkX, int chunkZ);
    ChunkTickSnapshot takeChunkTicks(int chunkX, int chunkZ);
    std::size_t chunkTickCount(int chunkX, int chunkZ);

These look up only the requested chunk's block and fluid containers. No full
dimension scheduledTicks scan is used for persistence.

Snapshots contain duplicates and are sorted in ScheduledTick.DRAIN_ORDER.
takeChunkTicks clears only that chunk's containers and leaves other chunks
untouched.

## Install and pause/resume

For saved ticks belonging to the requested chunk:

- block and fluid lists receive separate -N..-1 order bases;
- delays are added to the current World gameTime with wrapping long arithmetic;
- priority is clamped to -3..3;
- duplicate entries are restored through scheduleUnchecked;
- block/fluid tick containers and data are installed before World::loadChunk
  publishes the light lifecycle/publication edge.

On unload/save, main snapshots the current per-chunk lists and converts absolute
trigger time back to saved delay using the snapshot game time.

Game-time passage while a resident managed chunk has ticks marks it dirty at the
main-thread tick boundary. Therefore unload stores remaining delays rather than
the original full delay. Time spent unloaded is not subtracted on reload.

## Dirty tracking and save payload

World now exposes tickMutationObserver. Schedule, restore, execution/removal,
and explicit take operations report the affected position.

WorldChunkManager keeps dirtyVersion for both terrain edits and tick changes.
Schedule or execution without any setBlock marks the chunk dirty and saves the
current complete tick lists.

ChunkLifecycleController save payload now supports:

    ChunkSavePayload {
      vector<BlockEdit> edits;
      optional<ChunkTickSaveData> ticks;
    }

    ChunkTickSaveData {
      int64 snapshotTime;
      TickListPatch ticks;
    }

TickListPatch has independent optional block and fluid vectors:

- nullopt leaves that list unchanged;
- engaged empty vector clears the list;
- engaged nonempty vector replaces the full list.

The manager normally engages both lists so every current list, including an
empty one, has explicit on-disk state. The worker applies tick patches directly
to its owned NBT root, then applies block edits, encodes, and writes. Main never
exports the full World and transfers only the affected chunk's tick records.

If a save fails, the World chunk remains resident and tick changes remain in the
live containers. retrySave captures a fresh tick snapshot at the retry time and
uses the same exact lifecycle snapshot version where permitted.

## Block events

Block events have no vanilla chunk NBT format here and are not fabricated.

They are split from persistent-data blockers:

- Save is allowed while a transient block event is pending.
- Actual unload is temporarily refused while that chunk's event remains.
- The blocker is not sticky. After World::tick processes the event, pump retries
  eviction.

Moving-piston block entities and terrain entities remain sticky persistence
blockers and still prevent lossy save/unload.

## Explicit shutdown

Safe close behavior from T-4b-1 is retained. close observes game time first,
saves held-ticket dirty chunks, waits through v1/pending v2, writes complete
current tick lists, and only then stops I/O. Shutdown results report any save or
dynamic blocker as unsaved/resident.

## Direct coverage

Codec:

- patchChunkTicks replaces only block_ticks/fluid_ticks;
- engaged empty list clears, nullopt leaves unchanged;
- signed delay and priority survive;
- no SubTick is emitted.

Scheduler/manager:

- per-chunk snapshot/take does not enumerate another chunk;
- duplicate saved block entries load;
- block and fluid orders are independently -3/-2/-1 and -1;
- same-trigger order survives save/reload;
- resident time passage marks tick lists dirty and stores reduced delay;
- time unloaded does not reduce remaining delay;
- tick-only schedule is dirty without setBlock;
- later tick edit during save is not cleared by old ack;
- execution/removal is dirty and saves remaining or empty lists;
- extreme trigger-to-int delay uses Java low-32-bit truncation;
- save failure retains and retries pending tick data;
- pending block event allows save but delays unload until processed;
- real Java fixture load/edit/save/unload/reload succeeds.

Real Java fixture checks include three block ticks (two duplicates), one fluid
tick, no SubTick, stable delay/priority, and retained unknown NBT. The fixture
was generated with official 26.1 NbtIo/RegionFile; Java read the C++-written
scratch region.

## Commands and logs

Fixture generation:

    TICK_FIXTURE_OUT=/home/zhddsj/.local/state/redstone-cpp/wsl-w3a-tick-fixture-20261112 \
      bash validation/vanilla26_1/run_tick_world_fixture.sh write

Manager direct test:

    world_chunk_manager_tests <scratch> <terrain-region> <tick-region>

Java readback:

    TICK_FIXTURE_OUT=/home/zhddsj/.local/state/redstone-cpp/wsl-w3a-tick-fixture-20261112 \
      bash validation/vanilla26_1/run_tick_world_fixture.sh read \
      /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-manager-final3/real-tick-region

Outputs:

    world chunk manager tests passed (with real World disk fixture)
    fluid tick tests passed
    chunk lifecycle tests passed (with real disk fixture)
    terrain codec tests passed
    JAVA_READ_CPP_TICKS_OK

engine_tests exited 0 with empty stdout.

Logs:

- Build: /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-build-current.log
  SHA-256 8c35b1682f73592d30612d7b0fabdb384fcfff35cb5a076cb48b85b1368aa794
- Manager: /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-manager-final3.log
  SHA-256 936988b0ec2318a17af306fa3c424709237a2d37a0567434b2dee3796f1ce543
- Java readback: /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-java-final3.log
  SHA-256 f3e3b726b12444229ccec0b64ac58c44c5ff3cc6a1c02c40478388b0740f9077
- fluid ticks: /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-fluid-tick-final.log
  SHA-256 5ab5394e85f57303c8266cc195770a385d0f1149feb364095ee74cf207ff12f5
- lifecycle: /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-lifecycle-final.log
  SHA-256 0f1e8020c8cdef75b48e47504de4b14a507f29447511345344c304faa0ef3c0c
- terrain codec: /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-codec-final.log
  SHA-256 df70095f12414f5da75f5c95c4d1c3f51fc14231e670fb8cee169e899a931efb
- engine: /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-engine-final.log
  SHA-256 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  (empty stdout; exit 0)
- Ten-run manager stability: /home/zhddsj/.local/state/redstone-cpp/wsl-w3a-20261112-final-stability.log
  SHA-256 83c8e65a75ddeb3d69399554c8d08a2efb292ad6be9e46c83a57ffb8548bc34a

Current source/binary hashes:

    include/redstone/engine.hpp                 0f6df4a4fc7c71a17867d2fe166b15cfa11cf3a16c32d65ba2850f5e2542e5d8
    src/engine.cpp                              6a5ce2bff8e6ab14a3f405d053e007dde1d16585e73eb79c90eb7e62c7996686
    include/redstone/terrain_codec.hpp          b06d1e450d714d8c2fd6f7648b68706c1ca614be398631e7d94efa0de1c381c5
    src/terrain_codec.cpp                       c728d117b8d924433f7416ddac3056cb670740197be7189e3d9a1e33b027742f
    include/redstone/chunk_lifecycle.hpp        973d340583e7abe277bfbd75017ddbbe736c41db75d005652f714c0793c97c35
    src/chunk_lifecycle.cpp                     0001fb1eefd30e8be554d38b854ab78ce4da3cf9b101159ede7984c288fac79e
    include/redstone/world_chunk_manager.hpp    e89bac1de7856f8f0a4f2fcd6d66b8dd6125030db6eb4e7a70b6051b463d3ca4
    src/world_chunk_manager.cpp                 5638c5b924aa4885fd0780a798a6394adaff31ecf9a1f572c737f16032fe7f76
    tests/world_chunk_manager_tests.cpp         83a3cdf90ca46046732033ff1eb157997fa8f2019703da55793213405ae622ce
    tests/terrain_codec_tests.cpp               6bc29803d6b91ca533588470db5a15affe575aa22d6c86bf129b188357798b4f
    world_chunk_manager_tests binary            cfcef1fdecce3325f59f6e10f4bc9a8e5bdbe1bc077db091f801f15eb5a6cedb

Java tick fixture:
/home/zhddsj/.local/state/redstone-cpp/wsl-w3a-tick-fixture-20261112/region/r.-1.-1.mca
SHA-256 beec85d3a75890eb648789df92fd970e785bef819f3ccc92b5a3727af429fd9b

The manager test copies the fixture before writing; the source fixture remains
read only.

## Remaining limits

- No block-entity or entity persistence.
- No entity-region load/save.
- No generation, persistent tickets, level.dat, tick gate, or native server
  activation.
- Block events remain transient unload blockers until processed.
- Movement of dynamic blocker state to later W-3 stages is unchanged.
- No CTest, light suite, commit, or push was performed.
