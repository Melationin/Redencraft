#pragma once

// 天空光传播引擎内部声明：实现对齐原版 `SkyLightEngine` 的最低直射高度表、空段惰性继承与跨空段跳跃传播。

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

// 单区块 16x16=256 列的最低直射天空光起始 Y 高度表（对应原版 `ChunkSkyLightSources`）。
using SkyColumnSources = std::array<int, 256>;

// 天空光传播器跨批次复用的工作缓冲池。
// 存在原因：保留队列、稀疏工作段容器与存储资格集合的已分配内存，减少每批重构开销。
struct SkyPropagatorScratch {
  static constexpr int kUnknownInheritedSection =
      std::numeric_limits<int>::min();

  // 单个区块在当前批次中的天空光稀疏工作层数组。
  // 存在原因：一个区块每个光照段最多拥有一个工作层入口，空段（无存储资格）保持空指针，从而无需为所有缺失层分配存储；`inherited[]` 仅缓存上方最近一个已存储段的段 Y 坐标，其底面值始终从当前工作层或不可变原始层读取。
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
  bool anyQueueNeedsCompact() const noexcept {
    return checkQueue.needsCompact() || decreaseQueue.needsCompact() ||
           increaseQueue.needsCompact() || currentDecreaseQueue.needsCompact() ||
           currentIncreaseQueue.needsCompact();
  }
  bool compactQueues() {
    bool shrunk = false;
    shrunk |= checkQueue.compact();
    shrunk |= decreaseQueue.compact();
    shrunk |= increaseQueue.compact();
    shrunk |= currentDecreaseQueue.compact();
    shrunk |= currentIncreaseQueue.compact();
    return shrunk;
  }
  std::size_t totalQueueBytes() const noexcept {
    return checkQueue.capacityBytes() + decreaseQueue.capacityBytes() +
           increaseQueue.capacityBytes() + currentDecreaseQueue.capacityBytes() +
           currentIncreaseQueue.capacityBytes();
  }
};

// 天空光（`LightLayer::Sky`）单批次传播器。
// 存在原因：实现原版天空光独有的两项优化与语义——（1）利用 `SkyColumnSources` 在垂直无遮挡柱内直接赋予 15 级直射光而不逐格 BFS；（2）仅在 3x3x3 邻域存在非空方块的区块段中实际分配光照层，其余空段通过 `inheritedBase` 向下继承上方最近层底面值，并在跨越空段边界时通过 `propagateFromEmptySections` 直接向下方可见段批处理传播。
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
  // 为新加载区块计算直射高度表、初始化工作层并向水平邻柱播种侧向扩散。
  void seedChunk(std::uint64_t key);
  // 为带持久化 `isLightOn` 数据的区块仅重建 `SkyColumnSources` 高度表，保留已有光照页。
  void initPersistedSources(std::uint64_t key);
  void seedIncoming(std::uint64_t key, std::uint8_t directionMask = 0b111100);
  void seedOutgoing(std::uint64_t key, std::uint8_t directionMask = 0b111100);
  void clearIncomingFrom(std::uint64_t removedKey);
  // 依据 3x3x3 方块区块段非空状态重新计算受影响区块各段的存储资格：新获得资格的段按继承值实例化，失去资格的段释放并回退为 `Missing`。
  void syncStorageEligibility();
  bool hasPendingWork() const {
    return !checkQueue.empty() || !increaseQueue.empty() ||
           !decreaseQueue.empty() || !currentIncreaseQueue.empty() ||
           !currentDecreaseQueue.empty();
  }
  int columnSource(std::uint64_t key, int localX, int localZ);
  SkyColumnSources &sourcesForWorking(std::uint64_t key, WorkingChunk &chunk);
  // 仅增量重算某列因 `changedY` 处方块变动引起的最低直射光源高度变化。
  int updateColumnSource(std::uint64_t key, int localX, int localZ,
                         int changedY);
  // 根据新旧直射光源高度差，批量点亮新暴露柱或熄灭新遮挡柱并触发 BFS 扩散。
  void applyColumnSource(std::uint64_t key, int localX, int localZ,
                         int oldSource, int newSource);
  void run();
  // 按从上到下的顺序（`y` 降序）提交脏区块段，确保上方层就绪后再刷新依赖继承的下方层。
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
  // 当某区块新增或移除工作段时，作废该区块所有段的 `inherited[]` 缓存。
  void invalidateInheritedBases(std::uint64_t chunkKeyValue) const;
  LightValues *insertWorkingLayer(std::uint64_t chunkKeyValue, int sectionY,
                                  LightValues values);
  void dropWorkingLayer(std::uint64_t chunkKeyValue, int sectionY);
  void dropAllWorkingLayers(std::uint64_t chunkKeyValue);
  // 查找并缓存 `sectionY` 上方最近一个非缺失已存储段的 Y 编号（无则返回 `kNoInheritedSection`）。
  int inheritedBase(WorkingLayers &layers, const LightChunkSnapshot *chunk,
                    int sectionY) const;
  // 为新分配工作层构造初始光照值：优先复制已有非缺失段，否则从 `inheritedBase` 底面整柱向下继承。
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
  bool isSectionStoredEligible(int sectionX, int sectionY, int sectionZ) const;
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
  // 若当前节点位于段底边缘且处于水平边界，则统计其正下方连续不具存储资格的空段数量，用于跨空段快速垂直投射。
  int countEmptySectionsBelowIfAtBorder(LightPos position) const;
  static bool crossedSkySectionEdge(int direction, int localX, int localZ);
  // 当光沿水平方向跨入另一区块且目标段底部紧邻空段时，直接向下穿透这些连续空段并把边柱写入更下方的有存储资格段，等价于原版空段继承传播。
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
