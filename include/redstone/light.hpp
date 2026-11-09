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
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
  const LightSectionSnapshot *section(int sectionY) const;
};

struct LightChunkSnapshot {
  int chunkX = 0;
  int chunkZ = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  std::uint64_t generation = 0;
  // Lifecycle rejects unload/reload results; revision distinguishes completed
  // results within one loaded lifetime.
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
  // L-2a may complete the block layer while sky propagation and protocol
  // publication remain unfinished.
  bool blockLightComplete = false;
  // TODO(L-2b): sky propagation remains unfinished.
  bool propagationComplete = false;
  // TODO(L-3): protocol encoding consumes only complete published snapshots.
  bool protocolReady = false;
  std::uint32_t unsupportedCells = 0;
  std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;

  int minLightSectionY() const { return minBlockSectionY - 1; }
  int lightSectionCount() const { return blockSectionCount + 2; }
  const LightSectionSnapshot *section(int sectionY) const;
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

struct LightWorkerOptions {
  std::size_t batchSize = 1000;
  std::size_t maxQueuedCommands = 4096;
  bool startPausedForTesting = false;
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
  bool unloadChunk(int chunkX, int chunkZ);

  // Main-thread publication handoff. Reading a snapshot never waits for the
  // worker and the shared object is immutable.
  void drainPublished();
  std::shared_ptr<const LightChunkSnapshot> latestChunk(int chunkX,
                                                        int chunkZ) const;
  void setPublishCallback(
      std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback);

  Stats stats() const;
  std::string workerError() const;
  bool waitIdleForTesting();
  void pauseForTesting();
  void resumeForTesting();
  void close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redstone
