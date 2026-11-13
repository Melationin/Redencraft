#pragma once

#include "redstone/nbt.hpp"
#include "redstone/entity_codec.hpp"
#include "redstone/terrain_codec.hpp"
#include "redstone/tickets.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace redstone {

enum class ChunkResidencyState {
  Absent,
  Reading,
  Resident,
  Saving,
  EvictPending,
  Failed,
  Missing,
};

enum class ChunkLoadFailure {
  None,
  Missing,
  Unsupported,
  Corrupt,
  Io,
  InstallRejected,
};

enum class StoreReadStatus {
  Found,
  Missing,
  Unsupported,
  Corrupt,
  Io,
};

struct EntityChunkDataResult {
  enum class Status { Empty, NonEmpty, Unsupported, Error, Unavailable };
  Status status = Status::Unavailable;
  std::string detail;
};
using EntityChunkDataProbe =
    std::function<EntityChunkDataResult(const ChunkTicketPos &)>;

struct StoreReadResult {
  StoreReadStatus status = StoreReadStatus::Io;
  std::vector<std::uint8_t> nbt;
  std::string detail;
  // Moved with a successful read into its install event. Failed/stale reads
  // release it with the result; no out-of-band per-read token table is needed.
  std::optional<EntityChunkDataResult> entityData;
};

struct StoreWriteResult {
  bool wrote = false;
  bool cleanupPending = false;
  std::string detail;
};

// Raw storage boundary used by the worker. One controller owns one worker, so
// all methods for a store are serialized even when several chunks share a
// region. Implementations must not open static region state concurrently.
class RawChunkStore {
public:
  virtual ~RawChunkStore() = default;
  virtual StoreReadResult readChunk(const ChunkTicketPos &position) = 0;
  virtual StoreWriteResult writeChunk(const ChunkTicketPos &position,
                                      std::span<const std::uint8_t> nbt,
                                      int timestampSeconds) = 0;
};

// The controller executes terrain and entity I/O on the same single worker.
class RawEntityStore {
public:
  virtual ~RawEntityStore() = default;
  virtual StoreReadResult readEntities(const ChunkTicketPos &position) = 0;
  virtual StoreWriteResult writeEntities(
      const ChunkTicketPos &position,
      std::span<const std::uint8_t> nbt, int timestampSeconds) = 0;
};

class RegionEntityStore final : public RawEntityStore {
public:
  explicit RegionEntityStore(std::filesystem::path entityDirectory);
  StoreReadResult readEntities(const ChunkTicketPos &position) override;
  StoreWriteResult writeEntities(
      const ChunkTicketPos &position,
      std::span<const std::uint8_t> nbt, int timestampSeconds) override;

private:
  std::filesystem::path entityDirectory_;
};

class RegionFileStore final : public RawChunkStore {
public:
  explicit RegionFileStore(std::filesystem::path regionDirectory);
  StoreReadResult readChunk(const ChunkTicketPos &position) override;
  StoreWriteResult writeChunk(const ChunkTicketPos &position,
                              std::span<const std::uint8_t> nbt,
                              int timestampSeconds) override;

private:
  std::filesystem::path regionDirectory_;
};

// Decoded views are moved to the main-thread recipient. The original decoded
// NBT root remains owned by the I/O worker as the save template.
struct LifecycleLoadedChunk {
  ChunkTicketPos position;
  std::uint64_t epoch = 0;
  std::optional<EntityChunkDataResult> entityData;
  std::int32_t chunkX = 0;
  std::int32_t chunkZ = 0;
  std::string statusName;
  bool isLightOn = false;
  std::vector<terrain::Section> sections;
  std::vector<terrain::SavedTick> blockTicks;
  std::vector<terrain::SavedTick> fluidTicks;
  std::vector<terrain::MovingPistonEntity> movingPistons;
  bool hasUnsupportedBlockEntities = false;
  std::string blockEntityError;
  std::vector<entity_codec::EntityRecord> entityRecords;
  bool entitySlotPresent = false;
  // Fixture-only raw list. Managed online loads use itemEntities.
  std::vector<nbt::Tag> entities;

  LifecycleLoadedChunk() = default;
  LifecycleLoadedChunk(LifecycleLoadedChunk &&) = default;
  LifecycleLoadedChunk &operator=(LifecycleLoadedChunk &&) = default;
  LifecycleLoadedChunk(const LifecycleLoadedChunk &) = delete;
  LifecycleLoadedChunk &operator=(const LifecycleLoadedChunk &) = delete;
};

struct ChunkResidentCallbacks {
  // Called on the main/drain thread with moved decoded section and tick views.
  // Returning true confirms installation and promotes Reading to Resident.
  std::function<bool(LifecycleLoadedChunk &&)> install;
  // Called on the main/drain thread only after the chunk is clean and still has
  // no demand. Returning true confirms removal; false leaves EvictPending.
  std::function<bool(const ChunkTicketPos &, std::uint64_t epoch)> evict;
};

struct ChunkLifecycleStatus {
  ChunkResidencyState state = ChunkResidencyState::Absent;
  ChunkLoadFailure failure = ChunkLoadFailure::None;
  std::uint64_t epoch = 0;
  int demandLevel = ticket_levels::NoTicketSource;
  bool demand = false;
  std::uint64_t dirtyVersion = 0;
  std::uint64_t savedDirtyVersion = 0;
  std::string detail;
};

// T-4a only: no World, generation, entity-region, level.dat, persistent-ticket,
// or tick-gate integration. updateDemand/drain/status are main-thread APIs.
class ChunkLifecycleController {
public:
  ChunkLifecycleController(std::filesystem::path regionDirectory,
                           ChunkResidentCallbacks callbacks);
  ChunkLifecycleController(
      std::shared_ptr<RawChunkStore> store,
      ChunkResidentCallbacks callbacks,
      std::shared_ptr<RawEntityStore> entityStore = {});
  ~ChunkLifecycleController();

  ChunkLifecycleController(const ChunkLifecycleController &) = delete;
  ChunkLifecycleController &operator=(const ChunkLifecycleController &) = delete;

  // Accepts one loading-tracker result. A level at or below FullChunkLevel
  // creates data demand; higher levels release it.
  bool updateDemand(const ChunkTicketPos &position, int loadingLevel);
  bool applyLoadingChanges(std::span<const ChunkLevelChange> changes);

  // Advances the dirty generation without enqueueing a write. A save ack for
  // an older version can never clear this newer value.
  bool markDirty(const ChunkTicketPos &position, std::uint64_t version);

  struct ChunkTickSaveData {
    std::int64_t snapshotTime = 0;
    terrain::TickListPatch ticks;
  };

  // The worker applies this immutable snapshot before the next write. Keeping
  // the patch shared lets a failed request be retried or merged with newer
  // edits without copying the same vector into retry and command storage.
  using ChunkEditPatch =
      std::shared_ptr<const std::vector<terrain::CompactBlockEdit>>;

  struct ChunkSavePayload {
    ChunkEditPatch edits;
    // nullopt means the saved tick lists are unchanged. An engaged value owns
    // full current block/fluid lists; each engaged inner optional can clear an
    // empty list explicitly.
    std::optional<ChunkTickSaveData> ticks;
    // Engaged means the chunk currently has a validated moving-piston-only BE
    // view. An engaged empty vector explicitly clears the old persisted list.
    std::optional<std::vector<terrain::MovingPistonEntity>> movingPistons;
    // nullopt leaves the entity slot untouched. An engaged list (including
    // empty) replaces Entities in the entity region.
    std::optional<std::vector<entity_codec::EntityRecord>> entityRecords;
    // nullopt leaves existing isLightOn and section light nibbles untouched.
    // Engaged replaces isLightOn and BlockLight/SkyLight arrays atomically.
    std::optional<terrain::ChunkLightPatch> light;
  };

  // snapshotVersion identifies exactly the payload being requested, not the
  // current dirty maximum. The call is rejected if that snapshot is older than
  // the last accepted or successfully written snapshot in this residency
  // generation. A failed snapshot may be retried at the same version.
  bool requestSave(const ChunkTicketPos &position,
                   std::uint64_t snapshotVersion,
                   std::vector<terrain::BlockEdit> edits = {});
  bool requestSavePayload(const ChunkTicketPos &position,
                          std::uint64_t snapshotVersion,
                          ChunkSavePayload payload);
  // Ownership handoff for a complete root from a successful decode. Validation
  // completes before the worker replaces its retained template. The root is
  // moved only after the request passes the snapshot-version checks.
  bool requestSaveOwnedRoot(const ChunkTicketPos &position,
                            std::uint64_t snapshotVersion,
                            nbt::RootTag &&root);

  // Retries one already pending eviction after a resident callback declined.
  // This is a direct position lookup, not a global scan, and remains usable
  // after close so main can finish cleanup without a live worker.
  bool retryEviction(const ChunkTicketPos &position);

  // Non-blocking. Moves completed load/save events into install/evict callbacks
  // and validates their epoch against the current main-thread generation.
  std::size_t drain();

  std::optional<ChunkLifecycleStatus>
  status(const ChunkTicketPos &position) const;
  bool accepting() const;

  // Test-only container counts: successful unloads and invalidated reads must
  // not leave one map node per historical coordinate.
  std::size_t activeEntryCountForTest() const;
  std::size_t workerLedgerCountForTest();
  std::size_t templateCountForTest();

  // Stops new requests, drains already accepted commands, then joins the I/O
  // worker. Normal main-loop operation never waits for disk.
  void close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redstone
