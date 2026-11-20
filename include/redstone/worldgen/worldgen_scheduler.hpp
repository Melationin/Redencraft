#pragma once
// High-throughput, low-latency multi-threaded chunk generation scheduler.
// Features:
// - Unified single PriorityBucketQueue (64 distance/priority buckets x 12 stages)
// - Fine-grained 3x3 spatial lock (`spatialMutex_`) for writeRadius > 0 stages
// - 4-dispatch inline chaining across compatible dependency pins
// - Parallel lock-free holder initialization (`InitState::Initializing`)
// - Non-blocking streaming demand control plane with bounded WIP, true in-flight
//   cancellation, and parallel worker NBT finalization

#include "redstone/chunk_map.hpp"
#include "redstone/worldgen/chunk_generator.hpp"
#include "redstone/worldgen/chunk_status.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace redstone::worldgen {

struct WorldGenSchedulerStats {
  std::size_t chunksCreated = 0;
  std::size_t chunksLoadedFromStore = 0;
  std::size_t stepsExecuted = 0;
  std::size_t inlineSteps = 0;
  std::size_t parallelDispatches = 0;
  std::size_t coloredDispatches = 0; // counts writeRadius>0 (Features) executions
  std::size_t holdersEvicted = 0;
  std::size_t spatialConflicts = 0;
  std::size_t completedTargets = 0;
  std::uint64_t targetLatencySumUs = 0;
  std::uint64_t targetLatencyMaxUs = 0;
  std::array<std::uint64_t, 32> targetLatencyHistUs{};

  double latencyPercentileMs(double p) const noexcept;
};

struct ChunkBatchRequest {
  ChunkTicketPos pos;
  ChunkStatusId targetStatus = ChunkStatusId::Full;
};

struct DemandRequest {
  ChunkTicketPos pos;
  ChunkStatusId targetStatus = ChunkStatusId::Full;
  bool residency = false;
  bool knownMissingOnDisk = false;
  std::uint64_t epoch = 0;
  std::int64_t gameTime = 0;
};

struct CompletedResidency {
  ChunkTicketPos pos;
  std::uint64_t epoch = 0;
  bool ok = false;
  std::string detail;
  std::shared_ptr<ProtoChunk> proto;
  std::shared_ptr<void> holderPin; // RAII pin keeping holder->generationRefCount > 0 until consumed
  nbt::RootTag root;               // encoded in parallel on worker thread
  std::uint64_t latencyUs = 0;
};

struct StreamingDemandStats {
  std::size_t requested = 0;
  std::size_t dispatched = 0;
  std::size_t completed = 0;
  std::size_t failed = 0;
  std::size_t cancelled = 0;
  std::size_t bufferRequests = 0;
  std::size_t bufferCompleted = 0;
  std::size_t evicted = 0;
};

class WorldGenScheduler {
public:
  // Embedded construction inside `ChunkMap`.
  WorldGenScheduler(ChunkMap &chunkMap, ChunkGenerator &generator,
                    std::int64_t worldSeed, RawChunkStore *diskStore = nullptr,
                    std::shared_ptr<LightOpticalContext> lightContext = {},
                    GeneratedLightQueue *lightQueue = nullptr,
                    WorldGenSchedulerConfig config = {});

  // Standalone construction (tests/benchmarks; shares `residentWorld->chunkMap()` if non-null).
  WorldGenScheduler(ChunkGenerator &generator, std::int64_t worldSeed,
                    RawChunkStore *diskStore = nullptr,
                    World *residentWorld = nullptr,
                    std::shared_ptr<LightOpticalContext> lightContext = {},
                    GeneratedLightQueue *lightQueue = nullptr,
                    WorldGenSchedulerConfig config = {});
  ~WorldGenScheduler();

  WorldGenScheduler(const WorldGenScheduler &) = delete;
  WorldGenScheduler &operator=(const WorldGenScheduler &) = delete;

  ChunkMap &chunkMap() noexcept { return *chunkMap_; }
  const ChunkMap &chunkMap() const noexcept { return *chunkMap_; }

  // Blocking APIs (for tests, probes, benchmarks, and standalone installs).
  BaseChunk &ensureStatus(ChunkTicketPos pos, ChunkStatusId targetStatus);
  void ensureStatusBatch(std::span<const ChunkBatchRequest> requests,
                         ChunkTicketPos priorityAnchor = {0, 0});

  std::shared_ptr<BaseChunk> findChunkShared(ChunkTicketPos pos) const;
  BaseChunk *findChunk(ChunkTicketPos pos) const;

  std::size_t heldChunkCount() const;
  void forEachHeldChunk(
      const std::function<void(const BaseChunk &)> &visitor) const;

  std::shared_ptr<ProtoChunk> takeProtoChunk(ChunkTicketPos pos);
  bool generateAndInstallIntoWorld(ChunkTicketPos pos, World &world);

  std::size_t evictIdleOutsideRadius(
      std::span<const ChunkTicketPos> keepCenters, int keepRadius = 12,
      std::vector<ChunkTicketPos> *evictedOut = nullptr);

  WorldGenSchedulerStats stats() const;
  std::string lastError() const;

  // Non-blocking streaming control plane APIs (for WorldChunkManager).
  void submitDemand(const DemandRequest &request);
  void cancelDemand(ChunkTicketPos pos, bool cancelResidency,
                    std::uint64_t onlyIfEpoch = 0);
  void updatePriorityCenters(std::span<const ChunkTicketPos> centers,
                             int keepRadius);
  void requestAsyncTrim();
  std::vector<CompletedResidency> drainCompletedResidency();
  std::vector<ChunkTicketPos> drainEvictedChunks();
  StreamingDemandStats streamingStats() const;
  void shutdownStreaming();

private:
  class PriorityBucketQueue;
  struct ReadyEntry;
  struct ControlCommand;
  struct DemandEntry;
  struct CancelAction;
  struct AdmitAction;
  struct WaitOutcome;
  class SpatialRegionGuard;

  WaitOutcome resolveWaitingLocked(ChunkHolder::GenTaskState &gen,
                                   std::uint8_t nextStage) noexcept;
  void dispatchWaitingOutcome(
      const std::shared_ptr<ChunkHolder> &holder, const WaitOutcome &outcome,
      std::vector<ChunkHolder::GenTaskState::WaiterRef> *cascadeFailedOut =
          nullptr);
  bool tryAcquireSpatial3x3OrDefer(
      const std::shared_ptr<ChunkHolder> &holder, ChunkHolder::GenTaskState &gen,
      std::array<std::shared_ptr<ChunkHolder>, 9> &regionHolders,
      std::size_t regionCount, std::uint8_t nextStage,
      SpatialRegionGuard &spatialGuard);

  std::shared_ptr<ChunkHolder> getOrCreateHolder(ChunkTicketPos pos,
                                                 std::uint8_t distBucket,
                                                 bool knownMissingOnDisk = false);
  void ensureHolderInitialized(const std::shared_ptr<ChunkHolder> &holder,
                               std::uint8_t distBucket,
                               bool knownMissingOnDisk);
  std::shared_ptr<ChunkHolder> findHolder(ChunkTicketPos pos) const;

  void markActiveLocked(ChunkHolder::GenTaskState &gen) noexcept;
  void markInactiveLocked(ChunkHolder::GenTaskState &gen) noexcept;

  void armHoldersIterative(std::vector<std::shared_ptr<ChunkHolder>> workStack);
  bool armExplicitTarget(const std::shared_ptr<ChunkHolder> &holder,
                         std::uint8_t distBucket, std::uint8_t targetStatus,
                         bool residency, bool registerBatchTarget,
                         std::vector<std::shared_ptr<ChunkHolder>> &workStack);
  void unregisterPinnedDeps(
      const ChunkHolder *waiter, std::uint32_t armSeq,
      std::vector<ChunkHolder::GenTaskState::PinnedDep> &pinned,
      std::vector<std::shared_ptr<ChunkHolder>> &cascadeWork);
  void cancelHolderWorklist(std::vector<std::shared_ptr<ChunkHolder>> worklist,
                            bool applyExplicitToFirst,
                            std::uint8_t newExplicitTarget,
                            bool clearResidencyPath);
  void pushControlCommand(ControlCommand cmd);

  void enqueueReady(const std::shared_ptr<ChunkHolder> &holder,
                    std::uint8_t nextStage, std::uint8_t distBucket,
                    bool residencyPath, std::uint32_t genId,
                    bool isFinalize = false);
  void executeHolder(const std::shared_ptr<ChunkHolder> &holder,
                     std::uint32_t expectedGenId);
  void runSingleStage(ChunkHolder &holder, ChunkStatusId nextStage,
                      const ChunkPyramid &pyramid, bool loadingPath,
                      int waitRadius, std::uint8_t writeRadius);
  void notifyWaiters(
      std::vector<ChunkHolder::GenTaskState::WaiterRef> &waiters);
  void releaseActiveHolder();
  void waitUntilIdle();
  void waitUntilStreamingIdle();
  void decrementFinalizeTasks() noexcept;
  void workerLoop();
  void startWorkers();

  std::vector<ChunkHolder::GenTaskState::WaiterRef>
  recordStageFailure(ChunkHolder &holder, std::string detail);
  void recordTargetLatencyUs(std::uint64_t latencyUs) noexcept;

  // Streaming control plane & finalization helpers.
  void tryDrainControl();
  void applyControlCommandLocked(ControlCommand cmd,
                                 std::vector<CancelAction> &toCancel);
  void selectAdmissionsLocked(std::vector<AdmitAction> &toAdmit);
  void retireResidencyLocked(DemandEntry &entry, bool isComplete, bool isFail,
                             bool isCancel);
  void retireBufferLocked(DemandEntry &entry, bool isComplete, bool isFail);
  void executeAdmissions(const std::vector<AdmitAction> &toAdmit);

  void onHolderProgressedOrIdle(const std::shared_ptr<ChunkHolder> &holder,
                                std::uint8_t reachedStage,
                                std::uint64_t armedNs = 0);
  void onHolderFailed(ChunkTicketPos pos, const std::string &detail,
                      std::uint64_t onlyIfEpoch = 0);
  void finalizeResidencyHolder(const std::shared_ptr<ChunkHolder> &holder);
  void wakeSpatialWaiters(std::vector<ChunkHolder::SpatialWaiter> &woken);

  std::unique_ptr<ChunkMap> ownedChunkMap_;
  ChunkMap *chunkMap_ = nullptr;
  ChunkGenerator &generator_;
  std::int64_t worldSeed_ = 0;
  RawChunkStore *diskStore_ = nullptr;
  std::shared_ptr<LightOpticalContext> lightContext_;
  GeneratedLightQueue *lightQueue_ = nullptr;
  WorldGenSchedulerConfig config_;

  // Parallel holder init CV (waits under holder->gen.mtx).
  std::condition_variable initCv_;

  // Control plane queue (strictly before demandMutex_).
  std::mutex ctrlMutex_;
  std::vector<ControlCommand> ctrlQueue_;
  std::atomic<bool> controlPending_{false};
  std::atomic<bool> controlDrainActive_{false};
  std::atomic<bool> streamingShuttingDownAtomic_{false};

  // Unified demand state (NEVER hold across gen.mtx or I/O!).
  mutable std::mutex demandMutex_;
  phmap::flat_hash_map<std::uint64_t, DemandEntry, ChunkKeyHash> demands_;
  std::vector<ChunkTicketPos> priorityCenters_;
  int keepRadius_ = 15;
  std::size_t residencyInFlight_ = 0;
  std::size_t bufferInFlight_ = 0;
  std::size_t completedSinceTrim_ = 0;
  bool trimRequested_ = false;
  bool streamingShuttingDown_ = false;
  StreamingDemandStats streamingStats_{};

  // Spatial 3x3 / 1x1 lock for Features and parallel NBT encode.
  std::mutex spatialMutex_;

  // Unified ready queue (64 distance/priority buckets x 12 stages).
  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::unique_ptr<PriorityBucketQueue> readyQueue_;
  bool stop_ = false;

  // Leaf locks (never acquire other locks under these).
  std::mutex completedMutex_;
  std::vector<CompletedResidency> completedResidency_;

  std::mutex evictedMutex_;
  std::vector<ChunkTicketPos> evictedChunks_;

  std::mutex batchMutex_;
  std::condition_variable batchCv_;
  phmap::flat_hash_map<std::uint64_t, std::uint8_t, ChunkKeyHash>
      activeBatchTargets_;
  std::size_t batchInFlight_ = 0;
  bool batchFailed_ = false;

  std::mutex idleMutex_;
  std::condition_variable idleCv_;
  std::atomic<std::size_t> activeHolders_{0};
  std::atomic<std::size_t> activeFinalizeTasks_{0};

  mutable std::mutex errorMutex_;
  std::string lastError_;

  // Lock-free telemetry counters.
  std::atomic<std::size_t> chunksCreated_{0};
  std::atomic<std::size_t> chunksLoadedFromStore_{0};
  std::atomic<std::size_t> stepsExecuted_{0};
  std::atomic<std::size_t> inlineSteps_{0};
  std::atomic<std::size_t> parallelDispatches_{0};
  std::atomic<std::size_t> coloredDispatches_{0};
  std::atomic<std::size_t> holdersEvicted_{0};
  std::atomic<std::size_t> spatialConflicts_{0};
  std::atomic<std::size_t> completedTargets_{0};
  std::atomic<std::uint64_t> targetLatencySumUs_{0};
  std::atomic<std::uint64_t> targetLatencyMaxUs_{0};
  std::array<std::atomic<std::uint64_t>, 32> targetLatencyHistUs_{};

  std::vector<std::thread> workers_;
};

} // namespace redstone::worldgen
