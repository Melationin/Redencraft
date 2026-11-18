#pragma once

// 异步光照服务公开接口：负责主线程区块/方块变更投递、后台单工作线程调度以及主线程快照结果回收与状态查询。

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

#include "redstone/light/snapshot.hpp"
#include "redstone/light/types.hpp"
#include "redstone/periodic_cleanup.hpp"

namespace redstone {

struct LightSectionView;

// 异步光照工作线程的配置选项（批处理窗口、环形队列初始容量、直读世界回调及测试钩子）。
struct LightWorkerOptions {
  // 直读低内存模式下的世界几何回调源：允许工作线程在需要时直接从实时 `World` 区块段借用只读视图，避免维护整份区块几何镜像。
  struct DirectWorldGeometrySource {
    const void *context = nullptr;
    // 仅在 `readSection` 获取区块段指针并增加引用计数的极短窗口内以共享模式加锁；`World` 仅在底层 `chunks_` 或存储区扩容时获取独占锁。
    std::shared_mutex *resizeMutex = nullptr;
    LightSectionView (*readSection)(const void *context, int chunkX, int chunkZ,
                                    int sectionY) = nullptr;
  };

  std::size_t batchSize = 1000;
  std::size_t maxQueuedCommands = 4096;

  bool startPausedForTesting = false;
  bool hasSkyLight = true;
  bool collectDiagnostics = false;
  // 单个光照工作线程的 CPU 亲和性掩码；方块光与天空光在该线程上顺序执行。
  std::uintptr_t workerAffinityMask = 0;

  // 工作线程合批收集窗口（毫秒）：在触发传播前按 30 ms 窗口合并并去重跨 tick 积压的方块单元更新（设为 0 则立即执行）。
  int batchWindowMs = 30;
  // 几何变更输入队列默认采用 256 KiB 的环形 SPSC 缓冲区，突发溢出时自动成倍扩容（类似 `std::vector`）；测试代码可传入极小的显式初始容量。
  std::size_t cellQueueCapacity = 0;
  std::size_t geometryQueueBytes = std::size_t{256} << 10;
  // 单工作线程通过该 SPSC 环形缓冲区发布已完成的方块光与天空光层结果；突发溢出 256 KiB 时同样自动倍增。
  std::size_t resultQueueBytes = std::size_t{256} << 10;
  DirectWorldGeometrySource directWorld{};
  // 确定性测试注入钩子：在每个已启用光照层开始传播前调用一次。
  std::function<void(LightLayer)> beforeLayerForTesting;
  // 世界生成期：邻区块几何未就绪时推迟本区块，并在邻居就绪后让受影响的邻居
  // 重新传播一次（LightEngine.checkEdge 的等价物）。默认关闭以保持既有语义；
  // 必须放在结构体末尾，避免破坏既有聚合初始化的字段顺序。
  bool deferUntilNeighborsReady = false;
  // 3x3 九色分片并行的 worker 数：0/1 表示保持现有单 worker 行为（默认关闭）。
  // 光传播半径 15 <= 1 个区块，因此同色分片（间隔 >= 3 个区块）之间不存在
  // 跨分片传播，可以安全并行而无需原子操作。
  int tiledParallelism = 0;
  // 几何新就绪时是否把已处理过的邻居置为“未计算”，让它重跑一次边界播种
  // （原版 LightEngine.checkEdge 的完整等价物）。默认关闭：跨区块光本来就会由
  // 新块自身的传播经 ensureWorking 推进已发布邻居，再反向拉一次是重复劳动，
  // 20x20 基准实测 wall +72%。需要更保守的边界收敛时再打开。
  bool borderTriggers = false;
};

// 主线程投递入口与后台光照工作线程的所有者（Pimpl 隔离实现细节）。
// 存在原因：将昂贵的光照 BFS 传播从主游戏刻线程剥离到独立工作线程，同时保证工作线程绝不直接持有可变 `World` 引用——所有输入要么是值快照，要么受 `resizeMutex` 与 `atomic_ref` 保护；结果发布始终在主线程调用 `drainPublished()` 时完成，确保发布回调永远不会在工作线程上执行。
class LightService {
public:
  // 服务运行与性能诊断统计快照（合并原子计数器与队列瞬时深度）。
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

  // 返回用于直读模式 `World` 区块段转换的不可变 `stateId -> opticalClass` 查找表；在服务启动工作线程前即已构建完毕。
  std::shared_ptr<const LightOpticalTable> opticalTable() const;
  // 提交区块加载或重载输入；有效更新会按区块合并，不会因队列容量受限而拒收，仅在服务正在关闭或输入非法时返回 0。
  std::uint64_t submitChunk(LightChunkInput input);
  // 把已注册几何、停在“未计算”的区块推进到“完整”：登记一次首次计算请求，
  // 返回请求序号（0 表示区块未注册或服务正在关闭）。与 `submitChunk` 一样只在
  // 主线程调用；真正的播种与传播由工作线程在下一批执行。
  std::uint64_t requestChunkCompute(int chunkX, int chunkZ);
  // 热路径上的无锁方块单元变更投递入口，记录内仅含坐标与原始状态 ID，工作线程负责一切光学解析；环形缓冲区满时会自动扩容并合并到待处理工作中而不会静默丢弃。返回分配的单调请求序号（被拒绝时返回 0）。
  std::uint64_t reportBlockCell(LightCellUpdate update);
  std::uint64_t reportBlockCells(std::span<const LightCellUpdate> updates);
  // 通知工作线程卸载指定区块并推进其生命周期代次，丢弃该区块遗留的过期光照状态。
  bool unloadChunk(int chunkX, int chunkZ);
  // 低内存模式：把已注册几何的区块切换为“直读 World 几何”。调用方负责保证此刻
  // World 区块的方块内容与提交时的几何完全一致（安装完成后的第一次调用即满足）；
  // 不推进代次、不改光照值、不触发重算，只释放工作线程持有的整份几何镜像。
  bool adoptDirectGeometry(int chunkX, int chunkZ);

  // 主线程结果排空与发布交接。排空时仅复制区块快照头部并触发回调，层描述符与 2048 字节原子页仍与工作线程共享；因此混合区块段中的半字节可能在发布后继续原地更新，并发读取方可能观察到新旧半字节的混合状态（但单字节内无撕裂风险）。
  void drainPublished();
  // 返回主线程上各自独立保存的最新单层区块快照；方块光层在已就绪时总是非空，天空光层在未启用天空光时为空指针。
  std::shared_ptr<const LightChunkSnapshot> latestLayerChunk(
      LightLayer layer, int chunkX, int chunkZ) const;
  // 仅限测试：读取天空光引擎缓存的 256 列最低直射光源高度；仅当服务当前处于空闲状态时返回有效值。
  std::optional<std::array<int, 256>> skyColumnSourcesForTesting(
      int chunkX, int chunkZ) const;
  // 仅限测试：读取工作线程侧冻结光学镜像中指定世界坐标的方块光学属性（生产环境光照快照刻意不携带二级光学几何）。
  std::optional<BlockLightProperties> opticalPropertiesForTesting(
      int x, int y, int z) const;
  void setPublishCallback(
      std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback);

  Stats stats() const;
  // 单个区块在光照服务中的加载、收敛与邻域完整度状态，供主线程判断区块是否已可发送给客户端。
  struct ChunkStatus {
    bool loaded = false;
    bool pending = false;
    // 三态模型：`loaded == false` 为“不可访问”；`needsCompute`（几何已注册、
    // 从未算过）或 `needsUpdate`（已完整、边界变脏待重算）为“未计算”；
    // 两者皆假且 `pending == false` 为“完整”。
    bool needsCompute = false;
    bool needsUpdate = false;
    bool blockComplete = false;
    bool skyComplete = false;
    bool complete = false;
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t submittedGeneration = 0;
    std::uint64_t publishedBlockGeneration = 0;
    std::uint64_t publishedSkyGeneration = 0;
    // 第 0..3 位分别对应 -Z、+Z、-X、+X 方向邻居缺失或未加载。
    std::uint8_t unloadedNeighborMask = 0;
  };
  ChunkStatus chunkStatus(int chunkX, int chunkZ) const;
  // 立即打破合批等待窗口并阻塞等待工作线程处理完所有已提交任务（`timeout == 0` 表示无限等待）。
  bool flushAndWait(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
  // 只等待"提交序号 <= generation 的区块光照处理完成"，不把调用线程拖到整个
  // 队列排空；世界生成阶段用它替代 flushAndWait() 的全局屏障。
  // 该区块的光照是否已收敛（自身输入处理完、无待处理条目）。允许之后被邻居
  // 的增量传播继续更新——光照是可变数据，不是一次性快照。
  bool isChunkSettled(int chunkX, int chunkZ) const;
  bool waitForChunk(
      int chunkX, int chunkZ, std::uint64_t generation,
      std::chrono::milliseconds timeout = std::chrono::milliseconds{0});
  // 检查后台传播器的 TaskQueue 是否存在过多空位可收缩，并在空闲/批间触发一次异步回缩。
  bool needsTaskQueueCompact() const noexcept;
  bool requestTaskQueueCompact(CleanupExecutionGate::Lease lease = {});
  std::size_t scratchTaskQueueBytesForTesting() const noexcept;
  // 在初始预热照明完成后重置单调性能计数器；仍保留待处理工作计数，基准测试调用方必须在空闲排空后再调用。
  void resetMeasurementStats();
  std::string workerError() const;
  bool waitIdleForTesting();
  // 等待工作线程发布“即将休眠”状态；随后的投递可直接命中“唤醒前通知”竞争窗口，无需依赖定时轮询。
  bool waitWorkerSleepingForTesting();
  void pauseForTesting();
  void resumeForTesting();
  // 让处于暂停状态的服务精确挑选一个待处理区块作为工作线程下一批次，并在完成后再次暂停；用于在测试中构造确定性的发布顺序，避免依赖 `unordered_map` 的迭代顺序。
  bool processChunkBatchForTesting(int chunkX, int chunkZ);
  // 停止接收新请求并等待工作线程安全退出。
  void close();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace redstone
