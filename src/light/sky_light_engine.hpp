#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "light/common.hpp"
#include "light/spsc_ring.hpp"

namespace redstone::light_internal {

using SkyColumnSources = std::array<int, 256>;

struct SkyPropagatorScratch {
  static constexpr int kUnknownInheritedSection =
      std::numeric_limits<int>::min();

  // A chunk owns at most one working layer entry per allocated light section.
  // Empty sections stay null, so the direct index does not allocate storage
  // for every missing layer. inherited[] caches only the section number of
  // the nearest stored layer above; its bottom value is always read from the
  // current working or immutable raw layer.
  struct WorkingLayers {
    explicit WorkingLayers(std::size_t count)
        : sections(count), inherited(count, kUnknownInheritedSection) {}

    std::vector<std::unique_ptr<LightValues>> sections;
    mutable std::vector<int> inherited;
  };

  FlatSet<LightPos, LightPosHash> checks;
  TaskQueue<LightPos> checkQueue;
  TaskQueue<DecreaseTask> decreaseQueue;
  TaskQueue<IncreaseTask> increaseQueue;
  TaskQueue<DecreaseTask> currentDecreaseQueue;
  TaskQueue<IncreaseTask> currentIncreaseQueue;
  FlatMap<std::uint64_t, std::unique_ptr<WorkingLayers>, UInt64Hash>
      layersByChunk;
  FlatSet<SectionPos3, SectionPos3Hash> dirtySections;
  FlatSet<SectionPos3, SectionPos3Hash> storedEligible;

  void clear() {
    checks.clear();
    checkQueue.clear();
    decreaseQueue.clear();
    increaseQueue.clear();
    currentDecreaseQueue.clear();
    currentIncreaseQueue.clear();
    layersByChunk.clear();
    dirtySections.clear();
    storedEligible.clear();
  }
};

class SkyLightPropagator {
public:
  SkyLightPropagator(
      LightEngineHost &light,
      FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> &workingChunks,
      const FlatSet<std::uint64_t, UInt64Hash> &removedChunks,
      FlatMap<std::uint64_t, SkyColumnSources, UInt64Hash> &sources,
      const FlatSet<std::uint64_t, UInt64Hash> &affectedChunks,
      SkyPropagatorScratch &scratchBuffers);

  void enqueueCheck(LightPos position);
  void seedChunk(std::uint64_t key);
  void initPersistedSources(std::uint64_t key);
  void seedIncoming(std::uint64_t key);
  void seedOutgoing(std::uint64_t key);
  void clearIncomingFrom(std::uint64_t removedKey);
  void syncStorageEligibility();
  int columnSource(std::uint64_t key, int localX, int localZ);
  SkyColumnSources &sourcesForWorking(std::uint64_t key, WorkingChunk &chunk);
  int updateColumnSource(std::uint64_t key, int localX, int localZ,
                         int changedY);
  void applyColumnSource(std::uint64_t key, int localX, int localZ,
                         int oldSource, int newSource);
  void run();
  void commitLayers();
  bool isComplete() const { return !incomplete; }

private:
  using WorkingLayers = SkyPropagatorScratch::WorkingLayers;

  struct ChunkCacheEntry {
    std::uint64_t key = 0;
    const LightChunkSnapshot *snapshot = nullptr;
    WorkingLayers *layers = nullptr;
    bool snapshotValid = false;
    bool layersValid = false;
  };

  struct SectionCacheEntry {
    SectionPos3 section{};
    std::uint64_t generation = 0;
    const LightChunkSnapshot *chunk = nullptr;
    const LightSectionSnapshot *snapshot = nullptr;
    WorkingLayers *layers = nullptr;
    LightValues *values = nullptr;
  };

  static int HeightmapColumnKey(int x, int z) { return x + z * 16; }
  static std::size_t sectionL1Slot(SectionPos3 s) {
    return static_cast<std::size_t>(((s.x & 3) << 4) | ((s.y & 3) << 2) |
                                    (s.z & 3));
  }
  static constexpr int kUnknownInheritedSection =
      SkyPropagatorScratch::kUnknownInheritedSection;
  static constexpr int kNoInheritedSection =
      std::numeric_limits<int>::max();

  int workingSectionIndex(int sectionY) const;
  WorkingLayers *findWorkingLayers(std::uint64_t chunkKeyValue) const;
  WorkingLayers &workingLayers(std::uint64_t chunkKeyValue) const;
  LightValues *workingLayer(std::uint64_t chunkKeyValue, int sectionY);
  const LightValues *workingLayer(std::uint64_t chunkKeyValue,
                                  int sectionY) const;
  void invalidateInheritedBases(std::uint64_t chunkKeyValue) const;
  LightValues *insertWorkingLayer(std::uint64_t chunkKeyValue, int sectionY,
                                  LightValues values);
  void dropWorkingLayer(std::uint64_t chunkKeyValue, int sectionY);
  void dropAllWorkingLayers(std::uint64_t chunkKeyValue);
  int inheritedBase(WorkingLayers &layers, const LightChunkSnapshot *chunk,
                    int sectionY) const;
  LightValues initialWorkingLayer(std::uint64_t chunkKeyValue,
                                  const LightChunkSnapshot *chunk,
                                  int sectionY) const;
  bool blockSectionNonEmpty(int x, int y, int z) const;
  WorkingChunk *workingChunk(std::uint64_t key);
  const LightChunkSnapshot *stateFor(std::uint64_t key) const;
  WorkingChunk *ensureWorking(std::uint64_t key);
  const SectionCacheEntry &lookupSection(LightPos position) const;
  const LightSectionSnapshot *sectionAt(LightPos position) const;
  const BlockLightProperties &stateAt(LightPos position) const;
  int storedLevel(LightPos position) const;
  bool eligibleHere(int sectionX, int sectionY, int sectionZ);
  LightValues *bulkValues(std::uint64_t chunkKeyValue, int sectionY);
  bool setStoredLevel(LightPos position, int level);
  void allocateSection(std::uint64_t chunkKeyValue, int sectionY);
  const BlockLightProperties &sourcePropertyAt(std::uint64_t key, int x, int y,
                                               int z) const;
  bool sourceEmptyGeometry(const GeometrySection *geometry) const;
  static bool isSourceEdgeOccluded(const BlockLightProperties &top,
                                   const BlockLightProperties &bottom);
  int findSourceBelow(std::uint64_t key, int x, int z, int topY) const;
  int computeSource(std::uint64_t key, int x, int z) const;
  SkyColumnSources computeSources(std::uint64_t key) const;
  void clearLines(WorkingChunk &chunk);
  int sourceFor(int blockX, int blockZ);
  void seedColumn(int chunkX, int chunkZ, int x, int z, int source);
  bool isSource(LightPos position);
  void checkNode(LightPos position);
  void propagateIncrease(const IncreaseTask &task);
  void propagateDecrease(const DecreaseTask &task);
  int countEmptySectionsBelowIfAtBorder(LightPos position) const;
  static bool crossedSkySectionEdge(int direction, int localX, int localZ);
  void propagateFromEmptySections(LightPos to, int direction, int level,
                                  bool increase, int emptySectionsBelow);
  static SectionPos3 sectionOf(LightPos position) {
    return {position.x >> 4, position.y >> 4, position.z >> 4};
  }
  void enqueueIncrease(LightPos position, std::uint8_t level,
                       std::uint8_t directions, bool fromEmpty, bool source);
  void enqueueDecrease(LightPos position, std::uint8_t level,
                       std::uint8_t directions);

  LightEngineHost &owner;
  FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> &working;
  const FlatSet<std::uint64_t, UInt64Hash> &removed;
  FlatMap<std::uint64_t, SkyColumnSources, UInt64Hash> &sourceHeights;
  const FlatSet<std::uint64_t, UInt64Hash> &affected;
  FlatSet<LightPos, LightPosHash> &checks;
  TaskQueue<LightPos> &checkQueue;
  TaskQueue<DecreaseTask> &decreaseQueue;
  TaskQueue<IncreaseTask> &increaseQueue;
  TaskQueue<DecreaseTask> &currentDecreaseQueue;
  TaskQueue<IncreaseTask> &currentIncreaseQueue;
  std::optional<SectionPos3> activeDecreaseSection;
  std::optional<SectionPos3> activeIncreaseSection;
  FlatMap<std::uint64_t, std::unique_ptr<WorkingLayers>, UInt64Hash>
      &layersByChunk;
  FlatSet<SectionPos3, SectionPos3Hash> &dirtySections;
  FlatSet<SectionPos3, SectionPos3Hash> &storedEligible;
  mutable std::array<ChunkCacheEntry, 64> chunkCache_{};
  mutable std::array<SectionCacheEntry, 64> sectionCache_{};
  mutable std::uint64_t sectionCacheGen_ = 1;
  mutable bool eligibleValid = false;
  mutable std::uint64_t eligibleChunkKey = 0;
  mutable int eligibleSectionY = 0;
  mutable bool eligibleResult = false;
  bool storageReady = false;
  mutable bool incomplete = false;
};

} // namespace redstone::light_internal
