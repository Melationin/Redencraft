#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "light/common.hpp"
#include "light/spsc_ring.hpp"

namespace redstone::light_internal {

struct BlockPropagatorScratch {
  FlatSet<LightPos, LightPosHash> checks;
  TaskQueue<LightPos> checkQueue;
  TaskQueue<DecreaseTask> decreaseQueue;
  TaskQueue<IncreaseTask> increaseQueue;
  TaskQueue<DecreaseTask> currentDecreaseQueue;
  TaskQueue<IncreaseTask> currentIncreaseQueue;
  FlatMap<SectionPos3, LightValues, SectionPos3Hash> lightValues;
  FlatSet<SectionPos3, SectionPos3Hash> dirtySections;

  void clear() {
    checks.clear();
    checkQueue.clear();
    decreaseQueue.clear();
    increaseQueue.clear();
    currentDecreaseQueue.clear();
    currentIncreaseQueue.clear();
    lightValues.clear();
    dirtySections.clear();
  }
};

class BlockLightPropagator {
public:
  BlockLightPropagator(LightEngineHost &light,
                       FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> &workingChunks,
                       const FlatSet<std::uint64_t, UInt64Hash> &removedChunks,
                       BlockPropagatorScratch &scratchBuffers);

  void enqueueCheck(LightPos position);
  void seedChunk(std::uint64_t key);
  void seedIncoming(std::uint64_t key, std::uint8_t directionMask = 0b111100);
  void seedOutgoing(std::uint64_t key, std::uint8_t directionMask = 0b111100);
  void clearIncomingFrom(std::uint64_t removedKey);
  void run();
  void commitLayers();
  bool isComplete() const { return !incomplete; }

private:
  struct ChunkStateCacheEntry {
    std::uint64_t key = 0;
    const LightChunkSnapshot *snapshot = nullptr;
    bool valid = false;
  };
  struct SectionCacheEntry {
    SectionPos3 section{};
    std::uint64_t epoch = 0;
    const LightSectionSnapshot *snapshot = nullptr;
    LightValues *values = nullptr;
  };

  static std::size_t sectionL1Slot(SectionPos3 s) {
    return static_cast<std::size_t>(((s.x & 3) << 4) | ((s.y & 3) << 2) |
                                    (s.z & 3));
  }
  std::uint64_t cacheEpoch() const {
    return lightValues.rehashEpoch() + snapshotCacheEpoch_;
  }
  const LightChunkSnapshot *stateFor(std::uint64_t key) const;
  const LightChunkSnapshot *stateFor(int chunkX, int chunkZ) const;
  WorkingChunk *ensureWorking(std::uint64_t key);
  const LightSectionSnapshot *sectionAt(LightPos position) const;
  const SectionCacheEntry &lookupSection(LightPos position) const;
  const BlockLightProperties &stateAt(LightPos position) const;
  int storedLevel(LightPos position) const;
  bool setStoredLevel(LightPos position, int level);
  void checkNode(LightPos position);
  void propagateIncrease(const IncreaseTask &task);
  void propagateDecrease(const DecreaseTask &task);
  static SectionPos3 sectionOf(LightPos position) {
    return {position.x >> 4, position.y >> 4, position.z >> 4};
  }
  void enqueueIncrease(LightPos position, std::uint8_t level,
                       std::uint8_t directions, bool fromEmpty,
                       bool fromEmission);
  void enqueueDecrease(LightPos position, std::uint8_t level,
                       std::uint8_t directions);

  LightEngineHost &owner;
  FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> &working;
  const FlatSet<std::uint64_t, UInt64Hash> &removed;
  FlatSet<LightPos, LightPosHash> &checks;
  TaskQueue<LightPos> &checkQueue;
  TaskQueue<DecreaseTask> &decreaseQueue;
  TaskQueue<IncreaseTask> &increaseQueue;
  TaskQueue<DecreaseTask> &currentDecreaseQueue;
  TaskQueue<IncreaseTask> &currentIncreaseQueue;
  std::optional<SectionPos3> activeDecreaseSection;
  std::optional<SectionPos3> activeIncreaseSection;
  FlatMap<SectionPos3, LightValues, SectionPos3Hash> &lightValues;
  FlatSet<SectionPos3, SectionPos3Hash> &dirtySections;
  mutable std::array<ChunkStateCacheEntry, 64> chunkStateCache_{};
  mutable std::array<SectionCacheEntry, 64> sectionCache_{};
  mutable std::uint64_t snapshotCacheEpoch_ = 1;
  mutable bool incomplete = false;
};

} // namespace redstone::light_internal
