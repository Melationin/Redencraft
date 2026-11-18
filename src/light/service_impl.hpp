#pragma once

// `LightService::Impl` 内部实现类声明：协调无锁 SPSC 输入/结果队列、批处理合并、单工作线程调度循环与主线程快照发布状态。

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

// `LightService` 的 Pimpl 私有实现。
// 存在原因：在主线程与单一后台工作线程之间划分严格的数据所有权边界——主线程仅操作 `versions`/`mainVersions`/`latestBlock`/`latestSky` 及三个 SPSC 队列的生产端，工作线程独占 `directChunks`/`lastReady*`/`protocolBase*` 及两个传播器的缓冲池，并通过 `atomic::wait` + 缓存行隔离降低唤醒与统计竞争。
class LightService::Impl {
public:
  Impl(int minSection, int sectionCount, LightWorkerOptions workerOptions);
  ~Impl();

  std::shared_ptr<const LightOpticalTable> opticalTablePtr() const {
    return opticalTable;
  }

  std::uint64_t submitChunk(LightChunkInput input);
  std::uint64_t requestChunkCompute(int chunkX, int chunkZ);
  std::uint64_t reportBlockCell(LightCellUpdate update);
  std::uint64_t reportBlockCells(std::span<const LightCellUpdate> updates);
  bool unloadChunk(int chunkX, int chunkZ);
  bool adoptDirectGeometry(int chunkX, int chunkZ);

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
  // 等待"提交序号 <= generation 的工作全部处理完"，不等待之后提交的工作。
  bool waitForChunk(int chunkX, int chunkZ, std::uint64_t generation,
                    std::chrono::milliseconds timeout);
  bool isChunkSettled(int chunkX, int chunkZ) const;
  bool needsTaskQueueCompact() const noexcept;
  bool requestTaskQueueCompact(CleanupExecutionGate::Lease lease = {});
  std::size_t scratchTaskQueueBytesForTesting() const noexcept;
  void resetMeasurementStats();
  std::string workerError() const;
  bool waitIdleForTesting();
  bool waitWorkerSleepingForTesting();
  void resumeForTesting();
  void pauseForTesting();
  bool processChunkBatchForTesting(int chunkX, int chunkZ);
  void close();

private:
  // 应用单个格点状态变化后的光学影响摘要，用于判断是否需要把该坐标送入传播队列。
  struct RawCellResult {
    bool opticalChanged = false;
    bool emptyChanged = false;
  };

  // `drainPublished()` 合并同批次多次发布结果时的去重键（按层、区块键与生命周期代次区分）。
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

  // 区块生命周期与请求版本记录。
  struct ChunkVersion {
    std::uint64_t epoch = 0;
    std::uint64_t requestRevision = 0;
    std::uint64_t submittedGeneration = 0;
    // 由原先独立的 `inactive` 集合合并而来：原先每次方块变更上报都要额外做一次哈希查找来探测是否已卸载。
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
  std::optional<LightSectionView> readDirectSection(std::uint64_t key,
                                                    int sectionY) const;
  struct SectionMetaSummary {
    bool empty = true;
    std::uint16_t unknown = 0;
  };
  SectionMetaSummary sectionMetaSummary(std::uint64_t key, int sectionY) const;
  const std::shared_ptr<LightChunkSnapshot> &
  storedLayerSnapshot(LightLayer layer, std::uint64_t key) const;
  // 为新加载区块生成只含段元数据与空层槽的原始快照外壳。
  std::shared_ptr<LightChunkSnapshot>
  makeRawLayerShell(std::uint64_t key, int chunkX, int chunkZ,
                    std::uint64_t generation, std::uint64_t epoch) const;
  void syncRawLayerGeometry(std::shared_ptr<LightChunkSnapshot> &snapshot,
                            std::uint64_t key) const;
  // 将磁盘持久化读取到的半字节光照数据直接装填进区块原始快照外壳（仅在 `isLightOn == true` 时启用）。
  void applyPersistedChunkLight(std::uint64_t key,
                                const PersistedChunkLight &persisted,
                                std::uint64_t generation,
                                std::uint64_t epoch,
                                std::shared_ptr<LightChunkSnapshot> &blockShell,
                                std::shared_ptr<LightChunkSnapshot> &skyShell) const;
  RawCellResult applyRawCell(std::uint64_t key, int sectionY,
                                     std::uint16_t localIndex,
                                     BlockState::Id stateId);
  // 配合 `workerSleeping` 与 `wakeSequence` 实现无锁唤醒，避免每次方块变更都抢占互斥锁。
  void notifyWorkerSleeping();
  void signalWorkerLocked(bool batchWindow = false);
  bool applyOwnedCellLocked(std::uint64_t sequence,
                            const LightCellUpdate &update);
  static void mergeChangedSections(
      std::vector<LightChunkSnapshot::LayerChange> &dest,
      const std::vector<LightChunkSnapshot::LayerChange> &extra);
  bool validSection(int sectionY) const;
  bool validInput(const LightChunkInput &input) const;
  // 将 `(sectionY, localIndex[0..4095])` 压缩为单个 32 位键，用于合并同一格的连续变更。
  static std::uint32_t changeSlot(int sectionY, std::uint16_t localIndex) {
    return (static_cast<std::uint32_t>(sectionY) << 12) | localIndex;
  }
  void pushCellInput(std::uint64_t sequence, LightCellUpdate update);
  bool ringEmpty() const;
  bool orderedInputEmptyLocked() const;
  void applyOrderedControlLocked(light_internal::PendingChunk control);
  // 按全局单调序号 `sequence`/`generation` 归并排空 `inputQueue` 与 `controlQueue`，保证区块加载/卸载与离散方块修改的因果顺序不被打乱。
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
  // 计算持久化加载区块需要与非持久化（或新重算）邻区执行边界衔接的方向掩码。
  std::uint8_t persistedBoundaryMask(
      std::uint64_t key,
      const light_internal::FlatSet<std::uint64_t, light_internal::UInt64Hash>
          &removed,
      const light_internal::FlatMap<std::uint64_t,
                                    std::shared_ptr<LightChunkSnapshot>,
                                    light_internal::UInt64Hash> &ready) const;
  void executeLayers(const light_internal::PreparedBatch &prepared);
  // 返回本批摘要，供 `run()` 在持锁状态下维护“未计算”状态集合。
  light_internal::PreparedBatch
  processBatch(std::vector<light_internal::PendingChunk> &batch);
  bool compactScratchTaskQueues();
  void refreshScratchMetrics();

  int minBlockSectionY;
  int blockSectionCount;
  LightWorkerOptions options;
  mutable std::mutex mutex;
  // 生成管线有多个线程会调用 drainPublished()，结果队列是单消费者，必须串行化。
  std::mutex drainMutex;
  std::condition_variable wake;
  std::condition_variable idle;
  std::condition_variable batchDone;
  std::condition_variable sleepState;
  std::unordered_map<std::uint64_t, light_internal::PendingChunk> pending;
  // “未计算”的两个来源（工作线程在 `mutex` 下维护，主线程的状态查询走同一把锁）：
  //   needsCompute —— 几何已注册、从未算过（`Completion::Deferred`）；
  //   needsUpdate  —— 已完整，但邻居边界变了，等待一次边界重算。
  light_internal::FlatSet<std::uint64_t, light_internal::UInt64Hash> needsCompute;
  light_internal::FlatSet<std::uint64_t, light_internal::UInt64Hash> needsUpdate;
  std::shared_ptr<const LightOpticalTable> opticalTable;

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
  // 由工作线程随 SPSC 控制流同步维护的区块生命周期视图。
  light_internal::FlatMap<std::uint64_t, ChunkVersion,
                          light_internal::UInt64Hash>
      workerVersions;
  // 仅主线程访问的生命周期视图，供 `drainPublished()` 校验发布结果；工作线程永不触碰它，因此结果消费无需等待输入合并。
  light_internal::FlatMap<std::uint64_t, ChunkVersion,
                          light_internal::UInt64Hash>
      mainVersions;
  light_internal::FlatMap<std::uint64_t, light_internal::SkyColumnSources,
                          light_internal::UInt64Hash>
      skyColumnSources;
  // “未计算”区块收到、尚未应用的入向光照传播（每层一个收件箱）。
  light_internal::FlatMap<std::uint64_t, light_internal::DeferredChunkInbox,
                          light_internal::UInt64Hash>
      deferredBlockCells;
  light_internal::FlatMap<std::uint64_t, light_internal::DeferredChunkInbox,
                          light_internal::UInt64Hash>
      deferredSkyCells;
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
  // 当前仍保持持久化 `isLightOn` 原始光照状态、尚未发生动态方块修改或传播改动的已加载区块集合。
  light_internal::FlatSet<std::uint64_t, light_internal::UInt64Hash>
      persistedLightChunks;
  // 记录各区块上一次网络可见的完全收敛版本元数据，以及某层未收敛期间累积的待发布脏段列表。
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
  // 当前批次里最小的提交序号；等待接口用它判断本批次是否还有早于目标序号的
  // 工作在跑（对应原版按区块等待，而不是全局排空）。
  std::uint64_t busyMinGeneration = 0;
  // 每个区块最近一次被处理到的提交序号（工作线程写入，等待方在 mutex 下读取）。
  std::unordered_map<std::uint64_t, std::uint64_t> processedGeneration;
  // 几何已经注册的区块（checkEdge 的等价状态）：跨区块边界传播只有在邻区块
  // 几何就绪后才有意义，否则先推迟本区块，等邻居就绪再处理。
  std::unordered_set<std::uint64_t> readyChunks;
  std::unordered_map<std::uint64_t, std::uint32_t> neighborDeferrals;
  bool neighborsReadyFor(std::uint64_t key) const;
  // 本批次新变为"几何就绪"的区块，用于在批次结束后唤醒邻居重传播。
  std::vector<std::uint64_t> justBecameReady;
  bool compacting = false;
  std::size_t busyChunks = 0;
  std::uint64_t nextPublicationRevision = 0;
  bool paused = false;
  bool pauseAfterBatch = false;
  bool flushRequested = false;
  CleanupExecutionGate::Lease compactLease;
  std::chrono::steady_clock::time_point nextBatchCollectAt{};
  std::optional<std::uint64_t> forcedChunk;
  std::string error;

  // 将生产者写入与工作线程写入的原子变量隔离到不同缓存行（64 字节对齐），避免工作线程更新统计计数时使主线程的控制标志或提交计数器缓存行失效。
  alignas(64) std::atomic<std::uint64_t> nextGeneration{0};
  std::atomic<bool> acceptingCells{true};
  std::atomic<bool> compactRequested{false};
  std::atomic<bool> scratchCompactable{false};
  std::atomic<std::size_t> scratchQueueBytes{0};
  mutable std::atomic<std::uint64_t> statsSubmitted{0};
  mutable std::atomic<std::uint64_t> statsRejected{0};
  mutable std::atomic<std::uint64_t> statsDroppedGeometry{0};

  alignas(64) std::atomic<std::uint64_t> wakeSequence{0};
  // 仅由工作线程在持有 `mutex` 时于进入等待前置位；生产者在推进 `wakeSequence` 后以 `seq_cst` 栅栏读取该标志。
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
