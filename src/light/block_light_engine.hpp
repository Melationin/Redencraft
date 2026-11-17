#pragma once

// 方块光传播引擎内部声明：实现对齐原版 `BlockLightEngine` 的区域优先增亮/减暗 BFS 传播与缓冲复用。

#include <array>
#include <cstdint>
#include <optional>

#include "light/common.hpp"
#include "light/spsc_ring.hpp"

namespace redstone::light_internal {

// 方块光传播器跨批次复用的工作缓冲池。
// 存在原因：将哈希表、任务队列与修改中区块段缓存的底层内存保留在 `LightService::Impl` 内，避免每个批次重新分配与释放大量堆内存。
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

// 方块光（`LightLayer::Block`）单批次传播器。
// 存在原因：在工作线程上复刻原版按区块段聚簇（`activeDecreaseSection`/`activeIncreaseSection`）的检查、减暗与增亮三阶段算法，并在传播收敛后将脏区块段写回 `WorkingChunk` 快照。
class BlockLightPropagator {
public:
  BlockLightPropagator(LightEngineHost &light,
                       FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> &workingChunks,
                       const FlatSet<std::uint64_t, UInt64Hash> &removedChunks,
                       BlockPropagatorScratch &scratchBuffers);

  // 将单个世界坐标加入去重后的待检查队列（用于方块光学变更或边界清理）。
  void enqueueCheck(LightPos position);
  // 对新加载且无持久化光照的区块扫描全部发光方块与已有存储值并播种增亮队列。
  void seedChunk(std::uint64_t key);
  // 从相邻已加载区块边界向当前区块播种跨界入向光。
  void seedIncoming(std::uint64_t key, std::uint8_t directionMask = 0b111100);
  // 将当前区块边界高亮度光向相邻区块播种出向光（常用于持久化区块加载后的边界衔接）。
  void seedOutgoing(std::uint64_t key, std::uint8_t directionMask = 0b111100);
  // 区块被卸载或替换时，将邻接区块朝向该区块的边界单元加入检查队列以消退来自已移除区块的光照。
  void clearIncomingFrom(std::uint64_t removedKey);
  // 顺序执行检查、减暗、增亮三阶段传播直至队列清空。
  void run();
  // 将 `lightValues` 中被修改的区块段折叠/提交回各自 `WorkingChunk` 的快照并标记脏段。
  void commitLayers();
  bool isComplete() const { return !incomplete; }

private:
  // 64 槽直接映射的区块快照 L1 缓存条目，用于加速高频邻域区块查询。
  struct ChunkStateCacheEntry {
    std::uint64_t key = 0;
    const LightChunkSnapshot *snapshot = nullptr;
    bool valid = false;
  };
  // 64 槽直接映射的区块段 L1 缓存条目，同时记录只读快照段与可写 `LightValues` 指针，通过 `epoch` 感知容器重哈希与新写入区块转换。
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
  // 结合 `lightValues` 重哈希代次与 `ensureWorking` 触发的快照表变更代次，使 L1 区块段缓存无需主动遍历失效即可安全过期。
  std::uint64_t cacheEpoch() const {
    return lightValues.rehashEpoch() + snapshotCacheEpoch_;
  }
  const LightChunkSnapshot *stateFor(std::uint64_t key) const;
  const LightChunkSnapshot *stateFor(int chunkX, int chunkZ) const;
  // 按需把邻接只读区块拉入 `working` 映射，仅在真正需要修改邻区亮度时触发。
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
  // 将任务路由至当前活动区块段队列或普通队列，复刻原版同段优先出队以提升缓存局部性。
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
