#pragma once

#include "redstone/chunk_lifecycle.hpp"
#include "redstone/engine.hpp"
#include "redstone/tickets.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <map>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace redstone {

struct ManagedChunkInfo {
  ChunkLifecycleStatus lifecycle;
  std::optional<LightService::ChunkStatus> light;
  std::string blocker;
  std::string error;
};

struct ChunkShutdownIssue {
  ChunkTicketPos position;
  ChunkResidencyState state = ChunkResidencyState::Absent;
  std::uint64_t dirtyVersion = 0;
  std::uint64_t savedDirtyVersion = 0;
  std::string error;
};

struct ChunkShutdownResult {
  bool complete = false;
  bool workerStopped = false;
  std::vector<ChunkTicketPos> savedChunks;
  std::vector<ChunkShutdownIssue> unsavedChunks;
  std::string fatalError;
};

// Online eager-save limits. The queue processes at most this many candidates
// and accepted writes in one real server tick, never waits for the I/O worker,
// and keeps total active writes bounded. Successful chunks receive the vanilla
// ten-second cooldown; a failed request backs off before its retained patch is
// retried.
struct AutosaveLimits {
  std::size_t maxCandidateChecks = 64;
  std::size_t maxSubmissions = 4;
  std::size_t maxInFlightSaves = 32;
  std::chrono::milliseconds candidateTimeBudget{2};
  std::chrono::milliseconds cooldown{10000};
  std::chrono::milliseconds failureBackoff{1000};
};

struct AutosaveStats {
  std::size_t candidateChecks = 0;
  std::size_t submissions = 0;
  std::size_t inFlight = 0;
  std::size_t queued = 0;
};

struct PumpSaveStats {
  std::size_t candidateChecks = 0;
  std::size_t submissions = 0;
  std::size_t inFlight = 0;
};

enum class EntityChunkDataStatus {
  Empty,
  NonEmpty,
  Unsupported,
  Error,
  Unavailable,
};


struct ChunkTickReadiness {
  int loadingLevel = ticket_levels::NoTicketSource;
  int simulationLevel = ticket_levels::SimulationTrackerMax;
  bool blockTicking = false;
  bool entityTicking = false;
  bool entityDataReady = false;
  std::string limitation;
};

// Opt-in T-4b-1 integration. World retains its existing offline loadChunk/
// unloadChunk behavior when this owner is not constructed.
//
// Supported FULL chunks carry terrain, block/fluid scheduled ticks, active
// moving-piston block entities, and the currently supported item entity type.
// Other saved block entities and entity types remain rejected. Item movement
// across chunks uses a compact per-UUID handoff ledger so target durability is
// confirmed before old source lists are cleared.
class WorldChunkManager {
public:
  using EntityUuid = std::array<std::int32_t, 4>;
  static constexpr std::size_t compactEditBytes =
      sizeof(terrain::CompactBlockEdit);
  WorldChunkManager(World &world, std::filesystem::path regionDirectory,
                    int viewDistance = 3, int simulationDistance = 10,
                    EntityChunkDataProbe entityProbe = {},
                    std::shared_ptr<RawEntityStore> entityStore = {});
  WorldChunkManager(World &world, std::shared_ptr<RawChunkStore> store,
                    int viewDistance = 3, int simulationDistance = 10,
                    EntityChunkDataProbe entityProbe = {},
                    std::shared_ptr<RawEntityStore> entityStore = {});
  ~WorldChunkManager() noexcept;

  WorldChunkManager(const WorldChunkManager &) = delete;
  WorldChunkManager &operator=(const WorldChunkManager &) = delete;

  TicketSourceManager &ticketSources() { return tickets_; }
  const TicketSourceManager &ticketSources() const { return tickets_; }

  TicketSourceUpdate setPlayer(PlayerTicketId id, bool present,
                               ChunkTicketPos center);
  TicketSourceUpdate addPlayer(PlayerTicketId id, ChunkTicketPos center);
  TicketSourceUpdate movePlayer(PlayerTicketId id, ChunkTicketPos center);
  TicketSourceUpdate removePlayer(PlayerTicketId id);
  TicketSourceUpdate setViewDistance(int distance);
  TicketSourceUpdate setSimulationDistance(int distance);
  TicketSystemUpdate setForced(const ChunkTicketPos &position, bool forced);
  // One startup batch: installs every validated persistent ticket with exact
  // identity/ticks and forwards loading changes to the lifecycle owner.
  TicketSystemUpdate restorePersistentTickets(
      std::span<const std::pair<ChunkTicketPos, Ticket>> tickets);

  void applyTicketUpdate(const TicketSystemUpdate &update);
  // Drains one completed load/save batch, pumps light publications, and starts
  // pending dirty saves or released evictions. It never waits for I/O.
  void pump();
  // One real server game tick. Ticket timeout decay happens only here, matching
  // ServerChunkCache's runsNormally/tickChunks condition. pump() never ages
  // tickets.
  void serverTick(bool tickChunks = true, bool runsNormally = true);

  // All writes through World still work. The manager observes successful
  // World::setBlock/rawSetBlock mutations, including nested writes, and records
  // only those in managed chunks.
  // Main-thread managed chunk callbacks. install/evict fire after the matching
  // World residency change; neither performs disk I/O.
  std::function<void(const ChunkTicketPos &)> managedChunkInstalled;
  std::function<void(const ChunkTicketPos &)> managedChunkRemoved;
  std::vector<std::pair<int, int>> managedChunks() const;
  const AutosaveStats &lastAutosaveStats() const { return autosaveStats_; }
  const PumpSaveStats &lastPumpSaveStatsForTest() const {
    return pumpSaveStats_;
  }
  std::uint64_t tickTimeEntriesVisitedForTest() const {
    return tickTimeEntriesVisited_;
  }
  std::size_t pendingEditEntriesForTest(
      const ChunkTicketPos &position) const {
    const auto found = managed_.find(position);
    return found == managed_.end() ? 0 : found->second.pendingEdits.size();
  }
  std::size_t pendingEditPayloadBytesForTest(
      const ChunkTicketPos &position) const {
    return pendingEditEntriesForTest(position) * compactEditBytes;
  }
  std::size_t retainedPatchEntriesForTest(
      const ChunkTicketPos &position) const {
    const auto found = managed_.find(position);
    return found != managed_.end() && found->second.retainedPatch
               ? found->second.retainedPatch->size()
               : 0;
  }
  std::size_t retainedPatchPayloadBytesForTest(
      const ChunkTicketPos &position) const {
    return retainedPatchEntriesForTest(position) * compactEditBytes;
  }
  std::size_t inFlightPatchEntriesForTest(
      const ChunkTicketPos &position) const {
    const auto found = managed_.find(position);
    return found != managed_.end() && found->second.inFlightPatch
               ? found->second.inFlightPatch->size()
               : 0;
  }
  void setAutosaveLimitsForTest(AutosaveLimits limits) {
    autosaveLimits_ = std::move(limits);
  }
  bool setBlock(Pos pos, BlockState state, int flags = 3, int limit = 512);
  void rawSetBlock(Pos pos, BlockState state);
  bool retrySave(const ChunkTicketPos &position);
  std::optional<ManagedChunkInfo>
  managedStatus(const ChunkTicketPos &position) const;
  ChunkTickReadiness tickReadiness(const ChunkTicketPos &position) const;
  bool isManaged(const ChunkTicketPos &position) const;
  std::optional<ChunkLifecycleStatus>
  lifecycleStatus(const ChunkTicketPos &position) const;

  // Explicit shutdown can wait for accepted I/O. New demand and late installs
  // are rejected first; all dirty managed chunks are saved, including chunks
  // whose tickets are still held. Save failures/blockers remain resident and
  // are returned in unsavedChunks. Normal pump() remains nonblocking.
  ChunkShutdownResult close();
  const ChunkShutdownResult &lastShutdownResult() const {
    return lastShutdownResult_;
  }
  // Test-visible ownership checks: per-read entity metadata must return to
  // zero after failure/stale/completion, and readiness is bounded to resident
  // managed centers rather than every historically visited neighbor.
  std::size_t entityReadMetadataCountForTest() const;
  std::size_t readinessCacheSizeForTest() const { return readiness_.size(); }
  std::uint64_t migrationWorkVisitedForTest() const {
    return migrationWorkVisited_;
  }
  std::size_t migrationWorkQueuedForTest() const {
    return entityMigrationWork_.size();
  }
  std::size_t entityPositionLedgerSizeForTest() const {
    return entityLastPositions_.size();
  }
  std::size_t lifecycleCoordinateCountForTest() const {
    return managed_.size();
  }
  bool accepting() const {
    return !closing_.load(std::memory_order_acquire) &&
           !closed_.load(std::memory_order_acquire);
  }

private:
  struct SaveCandidate {
    ChunkTicketPos position;
    std::uint64_t epoch = 0;
  };

  struct ManagedChunk {
    std::uint64_t epoch = 0;
    std::uint64_t dirtyVersion = 0;
    std::uint64_t inFlightVersion = 0;
    std::uint64_t failedVersion = 0;
    std::int64_t lastObservedGameTime = 0;
    std::int64_t transientEventRetryTime = 0;
    bool transientEventRetryScheduled = false;
    bool entityDataReady = false;
    bool entitySlotPresent = false;
    std::string entityDataDetail;
    // Packed sectionY/localIndex key -> process-stable BlockState id.
    std::map<std::uint32_t, BlockState::Id> pendingEdits;
    ChunkLifecycleController::ChunkEditPatch retainedPatch;
    ChunkLifecycleController::ChunkEditPatch inFlightPatch;
    std::chrono::steady_clock::time_point nextSaveAttempt{};
    bool cooldownScheduled = false;
    bool dynamicBlocked = false;
    std::set<int> writableSections;
    std::string blocker;
    bool initialLightComplete = false;
    bool initialLightBaselineRecorded = false;
    bool needsLightSave = false;
    std::uint64_t savedBlockLightRevision = 0;
    std::uint64_t savedSkyLightRevision = 0;
  };

  void initializeCallbacks();
  void installWorldCallbacks();
  void detachWorldCallbacks();
  bool installLoaded(LifecycleLoadedChunk &&loaded);
  bool evictLoaded(const ChunkTicketPos &position, std::uint64_t epoch);
  void recordBlockMutation(Pos pos, const BlockState &oldState,
                           const BlockState &newState);
  void recordTickMutation(Pos pos);
  void recordPistonMutation(Pos pos);
  void recordLightPublished(int chunkX, int chunkZ);
  void processLightSaveCompletions();

  struct EntityMigration {
    std::optional<ChunkTicketPos> target;
    std::uint64_t targetEpoch = 0;
    std::uint64_t targetVersion = 0;
    bool targetAcknowledged = false;
    bool removalObserved = false;
    std::set<ChunkTicketPos> clearSources;
    std::map<ChunkTicketPos, std::pair<std::uint64_t, std::uint64_t>>
        sourceVersions;
  };

  void recordEntityMutation(
      Vec3 oldPosition, Vec3 newPosition,
      const EntityUuid &uuid, bool removed);
  void noteEntityMigrationSave(
      const ChunkTicketPos &position, std::uint64_t epoch,
      std::uint64_t version,
      const std::set<EntityUuid> &presentUuids);
  void entityMigrationSaveCompleted(const ChunkTicketPos &position,
                                    std::uint64_t version, bool success);
  void setMigrationTarget(const EntityUuid &uuid, EntityMigration &migration,
                          const ChunkTicketPos &target);
  void addMigrationSource(const EntityUuid &uuid,
                          EntityMigration &migration,
                          const ChunkTicketPos &source);
  void removeMigrationSource(const EntityUuid &uuid,
                             EntityMigration &migration,
                             const ChunkTicketPos &source);
  void eraseMigration(const EntityUuid &uuid);
  void enqueueMigrationWork(const EntityUuid &uuid);
  void processMigrationWork(std::size_t maxEntries);
  bool entityMigrationAllowsSave(const ChunkTicketPos &position) const;
  bool entityMigrationPinned(const ChunkTicketPos &position) const;
  void observeTickTime();
  void processSaveCompletions();
  void processDemandSaveCandidates(std::size_t maxEntries);
  void processEvictionRetries(std::size_t maxEntries);
  bool processAutosaveCandidate(const ChunkTicketPos &position,
                                std::chrono::steady_clock::time_point now);
  void processAutosaveBudget();
  void processNoDemandSaveBudget(std::size_t maxEntries);
  void enqueueDynamicRetry(const ChunkTicketPos &position);
  void reclassifyDynamicRetry(const ChunkTicketPos &position);
  void reclassifyCooldown(const ChunkTicketPos &position);
  void enqueueDemandSaveCandidate(const ChunkTicketPos &position);
  void enqueueEvictionRetry(const ChunkTicketPos &position);
  void enqueueAutosaveCandidate(const ChunkTicketPos &position);
  void deferSaveCandidate(const ChunkTicketPos &position,
                          std::chrono::milliseconds delay);
  void noteDynamicBlocker(const ChunkTicketPos &position,
                          std::string reason);
  bool scheduleSave(const ChunkTicketPos &position, ManagedChunk &chunk,
                    bool forceRetry);
  bool persistenceBlocker(const ChunkTicketPos &position,
                          std::string &reason) const;
  bool transientUnloadBlocker(const ChunkTicketPos &position,
                              std::string &reason) const;
  ChunkTickReadiness computeReadiness(const ChunkTicketPos &position) const;
  void refreshReadinessAround(const ChunkTicketPos &position);
  void refreshReadinessForChanges(std::span<const ChunkLevelChange> loading,
                                  std::span<const ChunkLevelChange> simulation);

  class EntityReadStore;
  World &world_;
  TicketSourceManager tickets_;
  std::shared_ptr<RawChunkStore> terrainStore_;
  std::shared_ptr<RawEntityStore> entityStore_;
  std::unique_ptr<ChunkLifecycleController> controller_;
  std::map<ChunkTicketPos, ManagedChunk> managed_;
  // Monotonic lifecycle token shared by resident generations. No per-coordinate
  // historical marker is retained; failed installs consume a token locally
  // without creating a ledger entry.
  std::uint64_t nextLightLifecycleEpoch_ = 1;
  std::map<ChunkTicketPos, ChunkTickReadiness> readiness_;
  std::map<ChunkTicketPos, std::string> errors_;
  std::deque<SaveCandidate> autosaveCandidates_;
  std::set<ChunkTicketPos> autosaveQueued_;
  std::deque<SaveCandidate> demandSaveCandidates_;
  std::set<ChunkTicketPos> demandSaveQueued_;
  std::deque<SaveCandidate> evictionRetries_;
  std::set<ChunkTicketPos> evictionRetryQueued_;
  std::set<ChunkTicketPos> savingInFlight_;
  std::deque<SaveCandidate> heldDynamicRetries_;
  std::set<ChunkTicketPos> heldDynamicQueued_;
  std::deque<SaveCandidate> noDemandDynamicRetries_;
  std::set<ChunkTicketPos> noDemandDynamicQueued_;
  std::set<ChunkTicketPos> pendingLightSave_;
  std::set<ChunkTicketPos> lightNotificationQueue_;
  std::set<ChunkTicketPos> tickTimeSensitive_;
  std::map<EntityUuid, EntityMigration> entityMigrations_;
  std::map<ChunkTicketPos, std::set<EntityUuid>> entityMigrationTargets_;
  std::map<ChunkTicketPos, std::set<EntityUuid>> entityMigrationSources_;
  std::map<EntityUuid, Vec3> entityLastPositions_;
  std::deque<EntityUuid> entityMigrationWork_;
  std::set<EntityUuid> entityMigrationWorkQueued_;
  std::set<std::pair<std::chrono::steady_clock::time_point,
                     ChunkTicketPos>>
      heldSaveCooldowns_;
  std::set<std::pair<std::chrono::steady_clock::time_point,
                     ChunkTicketPos>>
      noDemandSaveCooldowns_;
  AutosaveLimits autosaveLimits_;
  AutosaveStats autosaveStats_;
  PumpSaveStats pumpSaveStats_;
  bool preferHeldDynamic_ = false;
  std::int64_t lastObservedTickTime_ =
      std::numeric_limits<std::int64_t>::min();
  std::uint64_t tickTimeEntriesVisited_ = 0;
  std::uint64_t migrationWorkVisited_ = 0;
  std::atomic<bool> closing_{false};
  std::atomic<bool> closed_{false};
  ChunkShutdownResult lastShutdownResult_;
};

} // namespace redstone
