#pragma once
// 基于单槽位状态机（`ChunkHolder`）、传递依赖剪枝、O(1) Bitset 分桶队列与
// 3×3 空间九色无锁波前的多线程区块生成调度器（`WorldGenScheduler`）。
// 仅保留必要的跨区块同步屏障（`StructureReferences`/`Noise`/`Features`/`Light`），
// 其余 `waitRadius=0` 阶段在同一工作线程内联连续推进，同时在进入 `WorldGenRegion`
// 时保留完整的 17×17 结构只读视野契约。

#include "redstone/worldgen/chunk_generator.hpp"
#include "redstone/worldgen/chunk_status.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace redstone::worldgen {

struct WorldGenSchedulerConfig {
  std::size_t threadCount = 0; // 0 = auto hardware_concurrency()
  int colorTurnDispatchLimit = 64; // 每轮颜色波前最大连续派发数，防止高密度区饥饿
};

struct WorldGenSchedulerStats {
  std::size_t chunksCreated = 0;
  std::size_t chunksLoadedFromStore = 0;
  std::size_t stepsExecuted = 0;
  std::size_t inlineSteps = 0;
  std::size_t parallelDispatches = 0;
  std::size_t coloredDispatches = 0;
  std::size_t holdersEvicted = 0;
};

struct ChunkBatchRequest {
  ChunkTicketPos pos;
  ChunkStatusId targetStatus = ChunkStatusId::Full;
};

class WorldGenScheduler {
public:
  WorldGenScheduler(ChunkGenerator &generator, std::int64_t worldSeed,
                    RawChunkStore *diskStore = nullptr,
                    World *residentWorld = nullptr,
                    std::shared_ptr<LightOpticalContext> lightContext = {},
                    GeneratedLightQueue *lightQueue = nullptr,
                    WorldGenSchedulerConfig config = {});
  ~WorldGenScheduler();

  WorldGenScheduler(const WorldGenScheduler &) = delete;
  WorldGenScheduler &operator=(const WorldGenScheduler &) = delete;

  // 将单个目标区块多线程推进至至少 `targetStatus`，完成后返回只读/已就绪访问指针。
  ChunkAccess &ensureStatus(ChunkTicketPos pos, ChunkStatusId targetStatus);

  // 批量提交多个目标区块并合并依赖包络，等待该批次全部到达各自 `targetStatus`。
  void ensureStatusBatch(std::span<const ChunkBatchRequest> requests,
                         ChunkTicketPos priorityAnchor = {0, 0});

  ChunkAccess *findChunk(ChunkTicketPos pos) const;

  // 诊断用：统计/遍历当前仍持有区块的槽位（包含目标区块与它们依赖的邻域区块）。
  // 整批生成结束后这些槽位不会被自动回收，因此它们构成常驻内存的主体。
  std::size_t heldChunkCount() const;
  void forEachHeldChunk(
      const std::function<void(const ChunkAccess &)> &visitor) const;
  // 取走区块的所有权（不移动对象本身，光照引擎可能仍持有它的 shared_ptr）。
  std::shared_ptr<ProtoChunk> takeProtoChunk(ChunkTicketPos pos);

  // 生成至 `Full` 并通过共享安装路径写入运行态 `World`。
  bool generateAndInstallIntoWorld(ChunkTicketPos pos, World &world);

  // 清理已完成且不再处于保留坐标 `keepRadius` 邻域内的空闲槽位与外围半成品，控制内存水位。
  // `evictedOut` 非空时追加被裁掉的坐标，供主线程同步注销光照引擎里的生成期登记。
  std::size_t evictIdleOutsideRadius(std::span<const ChunkTicketPos> keepCenters,
                                     int keepRadius = 12,
                                     std::vector<ChunkTicketPos> *evictedOut = nullptr);

  WorldGenSchedulerStats stats() const;

  // 最近一次批次中第一个生成阶段异常（生成器抛错）的描述；空表示没有失败。
  // 异常不再逃出 worker 线程终止进程，而是在本批次结束时由 ensureStatusBatch 抛出。
  std::string lastError() const;

private:
  struct ChunkHolder;
  struct HolderShard;
  class PriorityBucketQueue;
  struct ReadyEntry;

  std::shared_ptr<ChunkHolder> getOrCreateHolder(ChunkTicketPos pos,
                                                 std::uint8_t distBucket);
  std::shared_ptr<ChunkHolder> findHolder(ChunkTicketPos pos) const;

  void armHoldersIterative(std::vector<std::shared_ptr<ChunkHolder>> workStack);
  void enqueueReady(const std::shared_ptr<ChunkHolder> &holder,
                    std::uint8_t nextStage, std::uint8_t writeRadius,
                    std::uint8_t distBucket, std::uint32_t genId);
  void executeHolder(const std::shared_ptr<ChunkHolder> &holder,
                     std::uint32_t expectedGenId, bool wasColored);
  void runSingleStage(ChunkHolder &holder, ChunkStatusId nextStage,
                      const ChunkPyramid &pyramid);
  void notifyWaiters(std::vector<std::shared_ptr<ChunkHolder>> &waiters);
  void releaseActiveHolder();
  void waitUntilIdle();
  void workerLoop();
  // 生成阶段抛错时释放该 holder 的等待者并记录错误，使批次可以失败返回而不是死锁。
  std::vector<std::shared_ptr<ChunkHolder>>
  recordStageFailure(ChunkHolder &holder, std::string detail);

  ChunkGenerator &generator_;
  std::int64_t worldSeed_ = 0;
  RawChunkStore *diskStore_ = nullptr;
  World *residentWorld_ = nullptr;
  std::shared_ptr<LightOpticalContext> lightContext_;
  GeneratedLightQueue *lightQueue_ = nullptr;
  WorldGenSchedulerConfig config_;

  static constexpr std::size_t kShardCount = 64;
  std::unique_ptr<HolderShard[]> shards_;

  mutable std::mutex storeMutex_;

  std::mutex queueMutex_;
  std::condition_variable queueCv_;
  std::unique_ptr<PriorityBucketQueue> parallelQueue_;
  std::unique_ptr<PriorityBucketQueue[]> coloredQueues_; // 9 colors (3x3)
  std::uint8_t activeColor_ = 0;
  std::size_t activeColorInFlight_ = 0;
  int activeColorDispatched_ = 0;
  bool stop_ = false;

  std::mutex idleMutex_;
  std::condition_variable idleCv_;
  std::atomic<std::size_t> activeHolders_{0};

  std::atomic<std::size_t> chunksCreated_{0};
  std::atomic<std::size_t> chunksLoadedFromStore_{0};
  std::atomic<std::size_t> stepsExecuted_{0};
  std::atomic<std::size_t> inlineSteps_{0};
  std::atomic<std::size_t> parallelDispatches_{0};
  std::atomic<std::size_t> coloredDispatches_{0};
  std::atomic<std::size_t> holdersEvicted_{0};

  mutable std::mutex errorMutex_;
  std::string lastError_;

  std::vector<std::thread> workers_;
};

} // namespace redstone::worldgen
