#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "redstone/block.hpp"

namespace redstone {

// LightLayer matches net.minecraft.world.level.LightLayer. Block first mirrors
// LevelLightEngine.updateSectionStatus's block-then-sky calls.
enum class LightLayer : std::uint8_t { Block = 0, Sky = 1 };
enum class LightDirection : std::uint8_t { Down = 0, Up, North, South, West, East };
inline constexpr std::size_t kLightLayerCount = 2;
inline constexpr int kLightVolume = 16 * 16 * 16;
inline constexpr std::size_t kLightLayerBytes = 2048;

inline std::size_t lightIndex(int x, int y, int z) {
  // DataLayer.getIndex: y << 8 | z << 4 | x.
  return (static_cast<std::size_t>(y & 15) << 8) |
         (static_cast<std::size_t>(z & 15) << 4) |
         static_cast<std::size_t>(x & 15);
}

// Immutable DataLayer value. Missing and uniform data do not allocate; mixing
// allocates the exact 2048-byte nibble array. The lower nibble belongs to the
// even index and the upper nibble to the following index, as DataLayer.java.
class LightLayerSnapshot {
public:
  enum class Storage : std::uint8_t { Missing, Uniform, Mixed };

  LightLayerSnapshot() = default;
  static LightLayerSnapshot missing();
  static LightLayerSnapshot uniform(std::uint8_t value);
  static LightLayerSnapshot mixed(const std::array<std::uint8_t, kLightVolume> &values);
  static LightLayerSnapshot mixedPacked(
      const std::array<std::uint8_t, kLightLayerBytes> &packed);

  Storage storage() const { return storage_; }
  bool isMissing() const { return storage_ == Storage::Missing; }
  bool isUniform() const { return storage_ == Storage::Uniform; }
  bool isMixed() const { return storage_ == Storage::Mixed; }
  std::uint8_t uniformValue() const { return uniform_; }
  std::uint8_t get(int x, int y, int z) const;
  const std::uint8_t *packedData() const;
  std::size_t packedByteCount() const { return packed_.size(); }
  static constexpr std::size_t packedSize() { return kLightLayerBytes; }
  bool operator==(const LightLayerSnapshot &other) const;

private:
  Storage storage_ = Storage::Missing;
  std::uint8_t uniform_ = 0;
  std::vector<std::uint8_t> packed_;
};

// A light section contains both independent layers. Direction masks are stored
// as six 16x16 bit faces; L-1 preserves their validity bit but does not derive
// shape faces, which belongs to propagation/L-2a.
bool hasDifferentLightProperties(const BlockState &oldState,
                                 const BlockState &newState);
bool hasDifferentLightProperties(const BlockLightProperties &oldProperties,
                                 const BlockLightProperties &newProperties);
bool hasDifferentSectionLightProperties(bool wasEmpty, bool isEmpty);
bool lightFaceShapesOcclude(const BlockLightProperties &from,
                            const BlockLightProperties &to,
                            LightDirection direction);

struct LightSectionSnapshot {
  explicit LightSectionSnapshot(
      int sectionY = 0, bool empty = true,
      std::vector<BlockLightProperties> palette = {BlockLightProperties{}},
      std::vector<std::uint16_t> cells = std::vector<std::uint16_t>(kLightVolume,
                                                                   0),
      std::array<LightLayerSnapshot, kLightLayerCount> layers = {});

  int sectionY = 0;
  bool empty = true;
  std::vector<BlockLightProperties> palette;
  std::vector<std::uint16_t> cells;
  std::array<LightLayerSnapshot, kLightLayerCount> layers{};
  std::uint16_t unsupportedCells = 0;

  // An empty cell vector is the uniform representation: every cell uses
  // palette[0]. Non-uniform sections carry exactly kLightVolume indices.
  bool isUniform() const { return cells.empty(); }
  const BlockLightProperties &at(int x, int y, int z) const;
  std::uint8_t light(LightLayer layer, int x, int y, int z) const;
};

struct LightChunkInput {
  int chunkX = 0;
  int chunkZ = 0;
  // Optional world-owned lifecycle token. Leave zero for a standalone service;
  // World supplies a token that changes on unload/reload so queued cell records
  // from the old lifetime cannot be applied to the new one.
  std::uint64_t lifecycleEpoch = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
  const LightSectionSnapshot *section(int sectionY) const;
};

struct LightChunkSnapshot {
  struct LayerChange {
    int sectionY = 0;
    bool sky = false;
    bool block = false;
  };
  int chunkX = 0;
  int chunkZ = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  // Latest input request that touched this chunk.
  std::uint64_t generation = 0;
  // Lifecycle rejects unload/reload results; revision distinguishes completed
  // results within one loaded lifetime and is ordered by the single writer,
  // independently of the unordered batch's input generation.
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
  bool skyLightEnabled = true;
  // L-2a may complete the block layer while sky propagation and protocol
  // publication remain unfinished.
  bool blockLightComplete = false;
  bool skyLightComplete = false;
  bool propagationComplete = false;
  // Set only when every loaded chunk has complete optical metadata and both
  // enabled light layers converged. Protocol encoding never exposes an
  // unpublished or incomplete result.
  bool protocolReady = false;
  std::uint32_t unsupportedCells = 0;
  std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;
  // changedSections is a ready-to-ready delta. deltaBaseRevision identifies
  // the last protocol-ready snapshot it was computed from; incomplete drained
  // revisions do not advance that base.
  std::uint64_t deltaBaseRevision = 0;
  std::vector<LayerChange> changedSections;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
  const LightSectionSnapshot *section(int sectionY) const;
  // Sky queries follow SkyLightSectionStorage: a missing section takes the
  // bottom plane of the nearest stored section above it. Raw layer access stays
  // available through section().Missing sky data therefore differs from a
  // deliberately stored zero layer.
  int lightValue(LightLayer layer, int x, int y, int z) const;
};

struct LightBlockChange {
  int chunkX = 0;
  int chunkZ = 0;
  int sectionY = 0;
  std::uint16_t localIndex = 0;
  BlockLightProperties oldProperties;
  BlockLightProperties newProperties;
  bool wasEmpty = false;
  bool isEmpty = false;
  std::uint64_t generation = 0; // assigned by LightService
};

// One owned block-light input. World fills this from the authoritative state it
// just wrote; the worker never reads a live chunk, section or storage container.
// sequence is assigned by LightService and orders records that take the ring
// and full-ring fallback paths.
struct LightCellUpdate {
  std::uint64_t chunkKey = 0;
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t sequence = 0;
  int sectionY = 0;
  std::uint16_t localIndex = 0;
  BlockLightProperties properties;
  bool sectionEmpty = false;
};

struct LightWorkerOptions {
  std::size_t batchSize = 1000;
  std::size_t maxQueuedCommands = 4096;

  bool startPausedForTesting = false;
  bool hasSkyLight = true;
  bool collectDiagnostics = false;
  std::uintptr_t workerAffinityMask = 0;

  // Extra time the worker waits for more work before it starts a batch. Every
  // section a batch touches is cloned for copy-on-write publication, so
  // coalescing a few milliseconds of writes cuts that clone count; zero
  // disables the window and takes a batch as soon as anything is pending.
  int batchWindowMs = 1;
  // Testing uses a tiny power-of-two ring to exercise the owned overflow path.
  // Production retains the fixed 1<<18-entry capacity unless explicitly set.
  std::size_t cellQueueCapacity = std::size_t{1} << 18;
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
    double workerCpuMs = 0;
    std::uint64_t workerAffinityRequested = 0;
    std::uint64_t workerAffinityMask = 0;
    std::uint32_t workerAffinityError = 0;
  };

  explicit LightService(int minBlockSectionY, int blockSectionCount,
                        LightWorkerOptions options = {});
  ~LightService();
  LightService(const LightService &) = delete;
  LightService &operator=(const LightService &) = delete;

  // Valid updates are coalesced per chunk and are not rejected by queue
  // capacity. Returns 0 only when the service is closing or input is invalid.
  std::uint64_t submitChunk(LightChunkInput input);
  bool reportSectionStatus(int chunkX, int chunkZ, int sectionY, bool empty);
  bool reportBlockChange(LightBlockChange change);
  // Bulk form for callers that coalesce one tick's writes with their own bitmap.
  // The caller already decided which cells need a re-check, so the per-cell
  // property comparison is skipped and the service mutex is taken once for the
  // whole batch. The worker never reads `oldProperties`; it is stored only to
  // keep the merged struct consistent.
  void reportBlockChanges(const std::vector<LightBlockChange> &changes);
  // Owned hot-path hand-off. The record contains every optical value the worker
  // needs. A full ring merges the latest owned record into pending work instead
  // of dropping it.
  void reportBlockCell(LightCellUpdate update);
  bool unloadChunk(int chunkX, int chunkZ);

  // Main-thread publication handoff. A worker batch becomes visible in full;
  // reading a snapshot never waits for the worker and the shared object is
  // immutable.
  void drainPublished();
  std::shared_ptr<const LightChunkSnapshot> latestChunk(int chunkX,
                                                        int chunkZ) const;
  void setPublishCallback(
      std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback);

  Stats stats() const;
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
