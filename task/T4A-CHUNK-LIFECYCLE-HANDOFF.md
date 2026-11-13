# T-4a Chunk Lifecycle Skeleton Handoff

Date: 2026-11-12 (Asia/Shanghai)

## Status and scope

T-4a adds an independent asynchronous chunk lifecycle controller with one I/O
worker and an injectable raw store/resident callback boundary. It is not wired
into World or the server tick loop.

Implemented:

- Main-thread demand, epoch, dirty-version, residency, and callback state.
- Worker-owned decoded NBT save template.
- One worker for all commands handled by a controller.
- RegionFileStore for real W-2a/W-2b disk reads and writes.
- Blocking/failing injected store tests.
- Isolated official W-2 fixture: read, edit, save, evict, and read back.

Not implemented in T-4a:

- World::loadChunk/unloadChunk or tick gate integration.
- Original chunk generation or empty-chunk creation.
- Entity-region serialization.
- level.dat or persistent chunk ticket storage.
- An actual vanilla server restart differential.
- Light installation or unload notification.
- Multiple controllers sharing one region directory. T-4b must own one
  controller per directory/region owner; this controller has exactly one worker
  and never calls the static region API concurrently.

## Files

Added:

- include/redstone/chunk_lifecycle.hpp
- src/chunk_lifecycle.cpp
- tests/chunk_lifecycle_tests.cpp
- task/T4A-CHUNK-LIFECYCLE-HANDOFF.md

Changed:

- CMakeLists.txt: adds chunk_lifecycle to redstone_core and registers
  chunk_lifecycle_tests with a binary-directory scratch argument.

No existing light, ticket, World, storage, README, or PLAN file was changed for
T-4a. Existing uncommitted work was retained.

## Ownership and queues

ChunkLifecycleController owns a pimpl containing:

- one std::thread I/O worker;
- a mutex-protected command deque;
- a mutex-protected completed-result deque;
- main-thread-only Entry bookkeeping;
- WorkerTemplate NBT roots used by saves.

Load commands contain only position and epoch. Save commands move either a small
std::vector<terrain::BlockEdit> or an owned nbt::RootTag. Loaded results contain
LifecycleLoadedChunk with moved sections, both tick lists, block entities, and
entities; its copy constructor is deleted. The decoded root is not included in
the install event.

Normal main-thread request methods only populate the queue. They do not read or
write disk. close() is the explicit shutdown point: it rejects new commands,
lets the worker drain accepted commands, joins, and still leaves completed
results available for a final drain().

## Loading

updateDemand(position, loadingLevel) accepts the effective level from
ChunkLevelGraph. applyLoadingChanges(span<const ChunkLevelChange>) applies every
newLevel in the loading graph batch.

Data demand is level <= ticket_levels::FullChunkLevel (33). Higher levels release
data demand. This stage does not treat source levels 34..44 as resident FULL
data and never synthesizes missing chunks.

Read sequence:

1. Main sets Reading and submits Load{position, epoch}.
2. The worker calls RawChunkStore::readChunk through region::readChunkNbt.
3. Missing remains Missing.
4. Unsupported compression/version/status remains Failed with
   ChunkLoadFailure::Unsupported.
5. Corrupt/truncated data remains Failed with Corrupt. No empty chunk is made.
6. A successful decode also verifies xPos/zPos against the requested position.
7. The worker moves decoded.root into WorkerTemplate and moves decoded section,
   tick, block-entity, and entity views into LifecycleLoadedChunk.
8. Main drain verifies epoch and demand, then calls the install callback.
9. Only a true install return promotes Reading to Resident.

A stale load result cannot install: demand removal increments the epoch and
queues an Invalidate command; a subsequent demand re-add gets a newer epoch and
Load command.

## Saving and eviction

The primary save path is:

    requestSave(position, snapshotVersion, std::move(smallEdits))

snapshotVersion identifies the exact payload supplied by the caller. It is not
replaced with the current dirty maximum. The worker retains the load-time NBT
root, applies the moved edits with the W-2b in-place patch API, encodes NBT once,
and calls RawChunkStore::writeChunk. Region compression, packing, and file I/O
remain on the worker.

requestSaveOwnedRoot is the ownership-handoff fallback for a caller that already
has a validated decoded root. It moves the root into the worker. The worker
checks the root xPos/zPos match the requested position before writing.

Version rules:

- Every save command carries epoch and exact snapshotVersion.
- Completion is ignored when its epoch is not the current main generation.
- savedDirtyVersion advances to max(saved, ack.snapshotVersion).
- A snapshot older than lastAcceptedSnapshotVersion or workerWrittenVersion is
  rejected on the main thread.
- The worker independently rejects a successful-write version regression before
  replacing or writing its root.
- A save in flight does not prevent markDirty from advancing dirtyVersion.
- An old ack leaves newer dirty bits intact and returns Resident/Saving without
  clearing the newer version.
- Multiple saves for a chunk are FIFO. Older worker commands cannot execute
  after a newer generation that appeared earlier in the same queue.
- Save failure leaves the chunk Resident, dirty, and retryable. The worker keeps
  the patched root, so a retry can use an empty edit vector.
- A clean chunk with no demand enters EvictPending. The evict callback runs; only
  true confirmation changes the main state to Absent.
- A dirty chunk is never evicted before a save ack clears its dirty version.
- If demand returns while Saving, evictAfterSave is cleared and the successful
  ack leaves the chunk Resident.
- A failed save cannot evict. A later successful retry at the same snapshot
  version remains allowed and proceeds to evict after the ack.
- requestSaveOwnedRoot validates coordinates, DataVersion/FULL, palette schema,
  packed lengths, and indices before replacing the worker template. Invalid
  input leaves the previous good root intact.
- retryEviction(position) explicitly retries one EvictPending entry after the
  resident callback declined. No graph or global scan is used.
- close() releases all worker-owned roots after join. The final main drain can
  complete resident status cleanup without enqueueing another DropTemplate.

## Serialization

Each controller has one worker, so all reads and writes through its RawChunkStore
are serialized. The test observer asserts maximum active store calls == 1 across
reads and writes. Same-chunk save commands are FIFO; a two-write test checks that
the payload order is air then stone, with stone as the final disk value.

RegionFileStore uses only the validated W-2a single-slot APIs. No static region
object is shared with a second thread.

## Copy and payload inventory

No main-thread chunk decode and no complete NBT DOM copy occurs in the production
load/save path.

Load:

- Region raw compressed bytes -> one decompressed vector on the worker.
- decodeChunkNbt creates the worker owned DOM plus decoded views.
- The DOM moves into WorkerTemplate; views move into the install result. Neither
  is copied into that result.
- Main receives only the moved decoded views and then owns them.

Save, worker-retained template plus small edits:

- Main copies/moves one BlockEdit record per dirty cell, currently 24 bytes per
  edit on this ABI (section byte, uint16 index, 16-byte BlockState plus padding).
- Worker patches the retained root; W-2b uses one 8 KiB index work array, a 512 B
  per-cell bitset, and each affected section's packed 1-6 KiB array.
- Worker allocates one complete encoded raw NBT byte vector for RegionFileStore.
  This is the file payload being written, not an extra DOM.
- RegionFileStore compresses that payload before disk write.

Owned-root fallback:

- Main hands off one complete RootTag by move.
- The worker replaces its template with that moved root. No duplicate root is
  retained.

Shutdown/drop:

- DropTemplate erases the worker owned root.
- Result queues own moved decoded payloads; stale load results are discarded and
  their templates dropped where the controller is still accepting commands.

## Direct test coverage

The final binary was run ten times with the real fixture.

Covered:

- Demand through applyLoadingChanges.
- Missing: state Missing, zero installs.
- Unsupported and corrupt: explicit Failed kinds, zero installs.
- Install callback rejection: Failed/InstallRejected, never Resident.
- Blocking read followed by demand remove and re-add: only the new epoch installs.
- Blocking save plus demand remove and return: eviction is cancelled.
- markDirty(2), then requestSave(snapshot 1) while demand is lost: ack 1 leaves
  dirty 2 and does not evict.
- Save failure: Resident/dirty retained, no eviction; same-version retry
  persists, then evicts.
- Successful owned-root version 2 followed by owned-root version 1: the older
  request is rejected and performs no disk write.
- Wrong-position, bad-DataVersion, non-FULL, and unsupported-palette owned roots
  are worker-rejected before template replacement; the same-version edit retry
  succeeds against the retained good root.
- Eviction callback decline followed by retryEviction reaches Absent; the same
  retry also completes after close without a live worker.
- Same-chunk two-write FIFO and maximum one active store call.
- Owned-root save path.
- close(): rejects new demand, drains an accepted blocked read, and leaves the
  completed resident event for main drain.
- Real copied W-2 fixture:
  read stone -> install -> move edit to air -> save -> no-demand evict ->
  reopen controller -> read air from disk.

No synthetic empty chunk is returned for missing, unsupported, corrupt, or
install-rejected input.

## Commands and evidence

Build:

    cmake --build /home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build --target chunk_lifecycle_tests --parallel 6

Final direct test:

    /home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build/chunk_lifecycle_tests \
      /home/zhddsj/.local/state/redstone-cpp/wsl-t4a-20261112-scratch-final2 \
      /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-terrain-java-20261112-review-final/region

Result:

    chunk lifecycle tests passed (with real disk fixture)

The real test copies r.-1.-1.mca into its scratch directory before writing; the
source W-2 fixture is read only.

Logs:

- Build: /home/zhddsj/.local/state/redstone-cpp/wsl-t4a-20261112-build-final2.log
  SHA-256 7aef98bda2cfa7e899c76bda0ec119b35f4230dbd026b936216a34b6147e1594
- Final test: /home/zhddsj/.local/state/redstone-cpp/wsl-t4a-20261112-test-final2.log
  SHA-256 0f1e8020c8cdef75b48e47504de4b14a507f29447511345344c304faa0ef3c0c
- Ten-run stability: /home/zhddsj/.local/state/redstone-cpp/wsl-t4a-20261112-stability.log
  SHA-256 1dbcbe00ebcddc3ca6f6e496b4f7b18844b968b66f8adff54741aa3496cf4001

Source/binary hashes:

    include/redstone/chunk_lifecycle.hpp  657c152ed11e6f08332d3b7505a2112c6736a17018d67e977c21edc2ce358cda
    src/chunk_lifecycle.cpp               0ef53b59b8d75bb3a25dd17aa8a3a878432013c886f96fca6a2a7fc00a52cfd3
    tests/chunk_lifecycle_tests.cpp       dab6d417f2e291cb6d7bcd9b9798785e9426a4cb904d8112fa1c9af3e9f2ee01
    CMakeLists.txt                        a263c23700188ce7453cc1113ec1986eb571163237a8a85cd0ff7af634dfd781
    chunk_lifecycle_tests binary          589927281bd01852ef35375349f47bdf4d30bfdbe817673540d420180932734f

## Handoff boundary

The next integration stage (T-4b) must install moved decoded data into World on the
main thread, acknowledge only after installation, route World unload through the
evict callback, provide real dirty edits/atomic handoff, and own exactly one
lifecycle controller per region directory. Entity-region, generation, persistent
tickets, level.dat, lighting lifecycle, and tick readiness remain separate work.

No CTest, light test, unrelated benchmark, commit, push, World modification, or
Minecraft client/server launch was performed.

## Review-fix evidence (current)

The current candidate was built after all three review fixes. The lifecycle
suite passed with the copied real W-2 region and then passed ten consecutive
iterations. The affected terrain codec suite was also run directly once to check
the new read-only owned-root validator.

Logs:

- Build: /home/zhddsj/.local/state/redstone-cpp/wsl-t4a-review-20261112-build2.log
  SHA-256 14161753245e28af64d6a9a91dc3edb6f939df97f4c72cb2f11a0313e87b2321
- Lifecycle final: /home/zhddsj/.local/state/redstone-cpp/wsl-t4a-review-20261112-test1.log
  SHA-256 0f1e8020c8cdef75b48e47504de4b14a507f29447511345344c304faa0ef3c0c
- Affected terrain codec: /home/zhddsj/.local/state/redstone-cpp/wsl-t4a-review-20261112-codec.log
  SHA-256 df70095f12414f5da75f5c95c4d1c3f51fc14231e670fb8cee169e899a931efb
- Ten-run lifecycle stability: /home/zhddsj/.local/state/redstone-cpp/wsl-t4a-review-20261112-stability.log
  SHA-256 1dbcbe00ebcddc3ca6f6e496b4f7b18844b968b66f8adff54741aa3496cf4001

Current source/binary hashes supersede the pre-review values above:

    include/redstone/chunk_lifecycle.hpp  bb1162ee11c8f108e5d4b33756271c169a18e6ec32c3ed6b3c3aa14073775c2c
    src/chunk_lifecycle.cpp               20bff441e0281228dd1828bf883fba7a3d551a3b7855e4ec0318cdb6e3611d65
    include/redstone/terrain_codec.hpp    956f0972030b769922930d3dcb10d98a0c100eef0971057a43b31e751f6df26a
    src/terrain_codec.cpp                 9e5a74ef490c66170be2de77aa197f5c830a229312c854153ec3e480aab07d03
    tests/chunk_lifecycle_tests.cpp       0881975aed4b8c00ddfa164e2c548ee534b295f48e676c5cee8ccfa095ae48e8
    chunk_lifecycle_tests binary           50887f9c579ebd0b52b7b4787c066cc02e49266ef4106a83b304b2a42ab9ffcb

No CTest, light test, commit, push, World change, or Minecraft client/server
launch was performed for this review fix.
