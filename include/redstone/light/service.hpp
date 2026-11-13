#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "redstone/light/snapshot.hpp"
#include "redstone/light/types.hpp"

namespace redstone {

struct LightWorkerOptions {
  std::size_t batchSize = 1000;
  std::size_t maxQueuedCommands = 4096;

  bool startPausedForTesting = false;
  bool hasSkyLight = true;
  bool collectDiagnostics = false;
  // Affinity for the single light worker. Block and sky run sequentially on it.
  std::uintptr_t workerAffinityMask = 0;

  // Worker batch collection window in milliseconds: coalesces and deduplicates
  // queued cell updates across ticks every 30 ms before propagating (0 runs
  // immediately).
  int batchWindowMs = 30;
  // Geometry input uses a 256 KiB circular SPSC ring by default and
  // doubles automatically on burst overflow (like std::vector). Tests may
  // provide a tiny explicit initial capacity.
  std::size_t cellQueueCapacity = 0;
  std::size_t geometryQueueBytes = std::size_t{256} << 10;
  // One worker publishes complete block and sky layer results into this SPSC
  // ring buffer. It also doubles automatically if a burst exceeds 256 KiB.
  std::size_t resultQueueBytes = std::size_t{256} << 10;
  // Deterministic test seam invoked once per enabled layer, before propagation.
  std::function<void(LightLayer)> beforeLayerForTesting;
};

// Main-thread submission and worker-thread owner. No method on the worker
// borrows the live World: every input is a value snapshot. Publications are
// drained on the main thread; the callback never runs on the light worker.
class LightService {
public:
  struct Stats {
    std::uint64_t submitted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t processedBatches = 0;
    std::uint64_t processedBlockChanges = 0;
    std::uint64_t droppedGeometryTasks = 0;
    std::uint64_t published = 0;
    std::uint64_t stalePublications = 0;
    std::uint64_t queueHighWater = 0;
    std::uint64_t pendingChunks = 0;
    std::uint64_t pendingPublications = 0;
    std::uint64_t lightQueueEntries = 0;
    std::uint64_t skySectionChanges = 0;
    std::uint64_t blockSectionChanges = 0;
    std::uint64_t receivedSkySections = 0;
    std::uint64_t receivedBlockSections = 0;
    std::uint64_t unloadedNeighborSkips = 0;
    double workerCpuMs = 0;
    std::uint64_t workerAffinityRequested = 0;
    std::uint64_t workerAffinityMask = 0;
    std::uint32_t workerAffinityError = 0;
    std::uint64_t workerCount = 0;
    double blockWorkerCpuMs = 0;
    double skyWorkerCpuMs = 0;
    std::uint64_t blockWorkerAffinityRequested = 0;
    std::uint64_t blockWorkerAffinityMask = 0;
    std::uint32_t blockWorkerAffinityError = 0;
    std::uint64_t skyWorkerAffinityRequested = 0;
    std::uint64_t skyWorkerAffinityMask = 0;
    std::uint32_t skyWorkerAffinityError = 0;
  };

  explicit LightService(int minBlockSectionY, int blockSectionCount,
                        LightWorkerOptions options = {});
  ~LightService();
  LightService(const LightService &) = delete;
  LightService &operator=(const LightService &) = delete;

  // The immutable state-id to optical-class table used for direct World-section
  // conversion. It is fully built before the service starts its workers.
  std::shared_ptr<const LightOpticalTable> opticalTable() const;
  // Valid updates are coalesced per chunk and are not rejected by queue
  // capacity. Returns 0 only when the service is closing or input is invalid.
  std::uint64_t submitChunk(LightChunkInput input);
  // Owned hot-path hand-off. The record contains every optical value the worker
  // needs. A full ring merges the latest owned record into pending work instead
  // of dropping it. Returns the assigned sequence number (0 if rejected).
  std::uint64_t reportBlockCell(LightCellUpdate update);
  std::uint64_t reportBlockCells(std::span<const LightCellUpdate> updates);
  bool unloadChunk(int chunkX, int chunkZ);

  // Main-thread publication handoff. Headers are copied at drain, while layer
  // descriptors and atomic 2048-byte pages are shared with the worker. Mixed
  // values may update in place after publication and readers may observe a
  // mixture of old and new nibbles.
  void drainPublished();
  // Returns the independently saved layer envelope. Block always exists; sky
  // is null when that layer is absent.
  std::shared_ptr<const LightChunkSnapshot> latestLayerChunk(
      LightLayer layer, int chunkX, int chunkZ) const;
  // Test-only snapshot of the sky worker's 256 lowest direct-source heights.
  // Returns nullopt unless the service is currently idle.
  std::optional<std::array<int, 256>> skyColumnSourcesForTesting(
      int chunkX, int chunkZ) const;
  // Test-only read of the coordinator's frozen optical mirror. Production
  // light snapshots intentionally contain no secondary optical geometry.
  std::optional<BlockLightProperties> opticalPropertiesForTesting(
      int x, int y, int z) const;
  void setPublishCallback(
      std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback);

  Stats stats() const;
  struct ChunkStatus {
    bool loaded = false;
    bool pending = false;
    bool blockComplete = false;
    bool skyComplete = false;
    bool complete = false;
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t submittedGeneration = 0;
    std::uint64_t publishedBlockGeneration = 0;
    std::uint64_t publishedSkyGeneration = 0;
    // Bits 0..3 correspond to -Z, +Z, -X, +X neighbors missing or unloaded.
    std::uint8_t unloadedNeighborMask = 0;
  };
  ChunkStatus chunkStatus(int chunkX, int chunkZ) const;
  bool flushAndWait(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
  // Resets monotonic counters after initial-lighting warmup. Pending work is
  // left visible; benchmark callers must invoke it only after an idle drain.
  void resetMeasurementStats();
  std::string workerError() const;
  bool waitIdleForTesting();
  // Waits until the worker has published its pre-sleep state. The following
  // report exercises the exact notify-before-wait window without a timing poll.
  bool waitWorkerSleepingForTesting();
  void pauseForTesting();
  void resumeForTesting();
  // Lets a paused service choose exactly one pending chunk for the worker's
  // next batch and pause again after it completes. Used to exercise publication
  // ordering without depending on unordered_map iteration.
  bool processChunkBatchForTesting(int chunkX, int chunkZ);
  void close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redstone
