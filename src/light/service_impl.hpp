#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "light/block_light_engine.hpp"
#include "light/common.hpp"
#include "light/sky_light_engine.hpp"
#include "light/spsc_ring.hpp"
#include "redstone/light/service.hpp"

namespace redstone {

class LightService::Impl {
public:
  Impl(int minSection, int sectionCount, LightWorkerOptions workerOptions);
  ~Impl();

  std::shared_ptr<const LightOpticalTable> opticalTablePtr() const {
    return opticalTable;
  }

  std::uint64_t submitChunk(LightChunkInput input);
  std::uint64_t reportBlockCell(LightCellUpdate update);
  std::uint64_t reportBlockCells(std::span<const LightCellUpdate> updates);
  bool unloadChunk(int chunkX, int chunkZ);

  void drainPublished();
  std::shared_ptr<const LightChunkSnapshot> latestLayerChunk(LightLayer layer,
                                                             int x,
                                                             int z) const;
  std::optional<std::array<int, 256>>
  skyColumnSourcesForTesting(int x, int z) const;
  std::shared_ptr<const LightOpticalTable> opticalTableForTesting() const {
    return opticalTable;
  }
  std::optional<BlockLightProperties>
  opticalPropertiesForTesting(int x, int y, int z) const;
  void setPublishCallback(
      std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback);
  Stats stats() const;
  ChunkStatus chunkStatus(int chunkX, int chunkZ) const;
  bool flushAndWait(std::chrono::milliseconds timeout);
  void resetMeasurementStats();
  std::string workerError() const;
  bool waitIdleForTesting();
  bool waitWorkerSleepingForTesting();
  void resumeForTesting();
  void pauseForTesting();
  bool processChunkBatchForTesting(int chunkX, int chunkZ);
  void close();

private:
  struct RawCellResult {
    bool opticalChanged = false;
    bool emptyChanged = false;
  };

  struct DrainResultKey {
    LightLayer layer = LightLayer::Block;
    std::uint64_t key = 0;
    std::uint64_t epoch = 0;
    bool operator==(const DrainResultKey &) const = default;
  };

  struct DrainResultKeyHash {
    std::size_t operator()(const DrainResultKey &value) const noexcept {
      std::uint64_t h = value.key * 0x9e3779b97f4a7c15ULL;
      h ^= (value.epoch + 0x517cc1b727220a95ULL) + (h << 6) + (h >> 2);
      h ^= static_cast<std::uint64_t>(value.layer) * 0xbf58476d1ce4e5b9ULL;
      return static_cast<std::size_t>(h ^ (h >> 30));
    }
  };

  struct ChunkVersion {
    std::uint64_t epoch = 0;
    std::uint64_t requestRevision = 0;
    std::uint64_t submittedGeneration = 0;
    // Merged from the former inactive set: block-change reporting probed it
    // with a second hash lookup on every write.
    bool inactive = false;
  };

  bool enqueueControl(light_internal::PendingChunk state);
  std::uint64_t nextRequestGeneration();
  const LightOpticalClass &opticalClass(std::uint16_t id) const {
    return (*opticalTable)[static_cast<std::uint8_t>(id)];
  }
  std::uint8_t classForState(BlockState::Id stateId) const {
    return opticalTable->classForState(stateId);
  }
  void loadGeometry(std::uint64_t key, LightChunkInput &input);
  GeometrySection *geometrySection(std::uint64_t key, int sectionY);
  const GeometrySection *geometrySection(std::uint64_t key, int sectionY) const;
  std::optional<LightSectionView> readDirectSection(std::uint64_t key,
                                                    int sectionY) const;
  struct SectionMetaSummary {
    bool empty = true;
    std::uint16_t unknown = 0;
  };
  SectionMetaSummary sectionMetaSummary(std::uint64_t key, int sectionY) const;
  const std::shared_ptr<LightChunkSnapshot> &
  storedLayerSnapshot(LightLayer layer, std::uint64_t key) const;
  std::shared_ptr<LightChunkSnapshot>
  makeRawLayerShell(std::uint64_t key, int chunkX, int chunkZ,
                    std::uint64_t generation, std::uint64_t epoch) const;
  void syncRawLayerGeometry(std::shared_ptr<LightChunkSnapshot> &snapshot,
                            std::uint64_t key) const;
  void applyPersistedChunkLight(std::uint64_t key,
                                const PersistedChunkLight &persisted,
                                std::uint64_t generation,
                                std::uint64_t epoch,
                                std::shared_ptr<LightChunkSnapshot> &blockShell,
                                std::shared_ptr<LightChunkSnapshot> &skyShell) const;
  RawCellResult applyRawGeometryCell(std::uint64_t key, int sectionY,
                                     std::uint16_t localIndex,
                                     BlockState::Id stateId);
  void notifyWorkerSleeping();
  void signalWorkerLocked(bool batchWindow = false);
  bool applyOwnedCellLocked(std::uint64_t sequence,
                            const LightCellUpdate &update);
  static void mergeChangedSections(
      std::vector<LightChunkSnapshot::LayerChange> &dest,
      const std::vector<LightChunkSnapshot::LayerChange> &extra);
  bool validSection(int sectionY) const;
  bool validInput(const LightChunkInput &input) const;
  static std::uint32_t changeSlot(int sectionY, std::uint16_t localIndex) {
    return (static_cast<std::uint32_t>(sectionY) << 12) | localIndex;
  }
  void pushCellInput(std::uint64_t sequence, LightCellUpdate update);
  bool ringEmpty() const;
  bool orderedInputEmptyLocked() const;
  void applyOrderedControlLocked(light_internal::PendingChunk control);
  void drainCellChangesLocked();
  void notePendingLocked();
  std::size_t pendingChunkCountLocked() const;
  void noteHighWaterLocked();
  void awaitWork(std::unique_lock<std::mutex> &lock);
  void run();

  light_internal::PreparedBatch
  prepareBatch(std::vector<light_internal::PendingChunk> &batch);
  light_internal::LayerWorkspace
  layerWorkspace(const light_internal::PreparedBatch &prepared,
                 LightLayer layer);
  light_internal::LayerOutcome
  executeBlockLayer(const light_internal::PreparedBatch &prepared);
  light_internal::LayerOutcome
  executeSkyLayer(const light_internal::PreparedBatch &prepared);
  void publishResult(light_internal::LayerResult result);
  void publishIndependentLayer(const light_internal::PreparedBatch &prepared,
                               light_internal::LayerOutcome &outcome,
                               LightLayer layer);
  std::uint8_t persistedBoundaryMask(
      std::uint64_t key,
      const light_internal::FlatSet<std::uint64_t, light_internal::UInt64Hash>
          &removed,
      const light_internal::FlatMap<std::uint64_t,
                                    std::shared_ptr<LightChunkSnapshot>,
                                    light_internal::UInt64Hash> &ready) const;
  void executeLayers(const light_internal::PreparedBatch &prepared);
  void processBatch(std::vector<light_internal::PendingChunk> &batch);

  int minBlockSectionY;
  int blockSectionCount;
  LightWorkerOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable idle;
  std::condition_variable batchDone;
  std::condition_variable sleepState;
  std::unordered_map<std::uint64_t, light_internal::PendingChunk> pending;
  std::shared_ptr<const LightOpticalTable> opticalTable;
  light_internal::FlatMap<std::uint64_t, light_internal::GeometryChunk,
                          light_internal::UInt64Hash>
      geometry;
  light_internal::FlatMap<std::uint64_t, light_internal::DirectChunkMeta,
                          light_internal::UInt64Hash>
      directChunks;
  std::unique_ptr<
      light_internal::GrowableSpscRing<light_internal::GeometryInput>>
      inputQueue;
  std::unique_ptr<
      light_internal::GrowableSpscRing<light_internal::PendingChunk>>
      controlQueue;
  std::unique_ptr<light_internal::GrowableSpscRing<light_internal::LayerResult>>
      resultQueue;
  light_internal::FlatMap<std::uint64_t, ChunkVersion,
                          light_internal::UInt64Hash>
      versions;
  // Worker-maintained lifecycle view consumed with the SPSC control stream.
  light_internal::FlatMap<std::uint64_t, ChunkVersion,
                          light_internal::UInt64Hash>
      workerVersions;
  // Main-thread-only lifecycle view used by result draining; never touched by
  // the worker, so publication consumption cannot wait on input merging.
  light_internal::FlatMap<std::uint64_t, ChunkVersion,
                          light_internal::UInt64Hash>
      mainVersions;
  light_internal::FlatMap<std::uint64_t, light_internal::SkyColumnSources,
                          light_internal::UInt64Hash>
      skyColumnSources;
  light_internal::FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                          light_internal::UInt64Hash>
      latestBlock;
  light_internal::FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                          light_internal::UInt64Hash>
      latestSky;
  light_internal::FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                          light_internal::UInt64Hash>
      lastReadyBlock;
  light_internal::FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                          light_internal::UInt64Hash>
      lastReadySky;
  // Loaded chunks whose current light still matches persisted isLightOn data
  // without dynamic edits or propagation modifications.
  light_internal::FlatSet<std::uint64_t, light_internal::UInt64Hash>
      persistedLightChunks;
  // Last network-visible complete metadata and dirty sections accumulated
  // while one layer was incomplete.
  light_internal::FlatMap<std::uint64_t, light_internal::ProtocolBaseMeta,
                          light_internal::UInt64Hash>
      protocolBaseBlock;
  light_internal::FlatMap<std::uint64_t, light_internal::ProtocolBaseMeta,
                          light_internal::UInt64Hash>
      protocolBaseSky;
  light_internal::BlockPropagatorScratch blockScratch;
  light_internal::SkyPropagatorScratch skyScratch;
  std::uint64_t nextBlockLayerRevision = 0;
  std::uint64_t nextSkyLayerRevision = 0;
  std::function<void(std::shared_ptr<const LightChunkSnapshot>)> publishCallback;
  std::thread worker;
  bool accepting = true;
  std::atomic<bool> closing{false};
  bool busy = false;
  std::size_t busyChunks = 0;
  std::uint64_t nextPublicationRevision = 0;
  bool paused = false;
  bool pauseAfterBatch = false;
  bool flushRequested = false;
  std::chrono::steady_clock::time_point nextBatchCollectAt{};
  std::optional<std::uint64_t> forcedChunk;
  std::string error;

  // Separate producer-written and worker-written atomics onto distinct cache
  // lines so worker stats updates never invalidate main-thread control flags
  // or submission counters.
  alignas(64) std::atomic<std::uint64_t> nextGeneration{0};
  std::atomic<bool> acceptingCells{true};
  mutable std::atomic<std::uint64_t> statsSubmitted{0};
  mutable std::atomic<std::uint64_t> statsRejected{0};
  mutable std::atomic<std::uint64_t> statsDroppedGeometry{0};

  alignas(64) std::atomic<std::uint64_t> wakeSequence{0};
  // Only the worker sets this under mutex before waiting; producers read it
  // with seq_cst after advancing wakeSequence.
  alignas(64) std::atomic<bool> workerSleeping{false};

  alignas(64) mutable std::atomic<std::uint64_t> statsBatches{0};
  mutable std::atomic<std::uint64_t> statsChanges{0};
  mutable std::atomic<std::uint64_t> statsPublished{0};
  mutable std::atomic<std::uint64_t> statsStale{0};
  mutable std::atomic<std::uint64_t> statsHighWater{0};
  mutable std::atomic<std::uint64_t> statsLightQueueEntries{0};
  mutable std::atomic<std::uint64_t> statsSkyChanges{0};
  mutable std::atomic<std::uint64_t> statsBlockChanges{0};
  mutable std::atomic<std::uint64_t> statsReceivedSkySections{0};
  mutable std::atomic<std::uint64_t> statsReceivedBlockSections{0};
  mutable std::atomic<std::uint64_t> statsUnloadedNeighborSkips{0};
  mutable std::atomic<double> statsWorkerCpuMs{0};
  mutable std::atomic<double> statsBlockWorkerCpuMs{0};
  mutable std::atomic<double> statsSkyWorkerCpuMs{0};
  mutable std::atomic<std::uint64_t> statsWorkerAffinity{0};
  mutable std::atomic<std::uint32_t> statsWorkerAffinityError{0};
  mutable std::atomic<std::uint64_t> statsBlockWorkerAffinity{0};
  mutable std::atomic<std::uint32_t> statsBlockWorkerAffinityError{0};
  mutable std::atomic<std::uint64_t> statsSkyWorkerAffinity{0};
  mutable std::atomic<std::uint32_t> statsSkyWorkerAffinityError{0};
};

} // namespace redstone
