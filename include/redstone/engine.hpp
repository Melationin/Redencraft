#pragma once
// 核心世界模拟引擎（`World`）与实体区段空间索引桶（`EntitySectionBucket`）。
// 承载调色板压缩区段存储、按原版 26.1 顺序分离派发的更新链（形状更新、邻居通知、方块/流体计划刻、
// 方块事件、活塞与容器方块实体、随机刻）、实体空间查询与按轴滑移碰撞/单遍推挤，以及光照和托管生命周期观测钩子。
#include "redstone/chunk_section.hpp"
#include "redstone/engine_types.hpp"
#include "redstone/light/chunk_input.hpp"
#include "redstone/periodic_cleanup.hpp"
#include <bit>
#include <type_traits>

namespace redstone {
// 实体区段位平面分类标记：用于在 `EntitySectionBucket` 中按类别在位运算层面快速筛选实体。
enum class EntitySectionFlag : std::uint8_t {
  Active = 1 << 0,
  Pushable = 1 << 1,
  HardCollider = 1 << 2,
  Minecart = 1 << 3,
  Item = 1 << 4,
};

// 按 16x16x16 区块区段组织实体的 SoA（Structure of Arrays）空间桶。
// 之所以采用平行数组（`entities`、`boxes`）+ 64 位位平面（`BitPlane`），是因为碰撞查询、活体推挤与
// 特定实体筛选若直接遍历 `Entity*` 指针会因频繁解引用造成大量缓存未命中；
// 借助位平面按位与 + `std::countr_zero` 跳过不匹配槽位，并仅在连续的 `boxes` 数组相交时才解引用 `Entity*`，可显著加速热路径。
struct EntitySectionBucket {
  // 紧凑 64 位位图：区段内实体数 <= 64 时直接内联于 `inlineWord`，完全避免小桶的堆分配；
  // 超过 64 个实体时再无缝迁移到 `overflowWords`。
  struct BitPlane {
    std::uint64_t inlineWord = 0;
    std::vector<std::uint64_t> overflowWords;
    std::uint32_t count = 0;

    bool empty() const noexcept { return count == 0; }
    std::uint64_t wordAt(std::size_t wordIdx) const noexcept {
      if (overflowWords.empty())
        return wordIdx == 0 ? inlineWord : 0ull;
      return wordIdx < overflowWords.size() ? overflowWords[wordIdx] : 0ull;
    }
    bool test(std::size_t idx) const noexcept {
      return ((wordAt(idx >> 6u) >> (idx & 63u)) & 1ull) != 0ull;
    }
    void set(std::size_t idx, bool value) noexcept {
      const std::size_t wordIdx = idx >> 6u;
      const std::uint64_t mask = 1ull << (idx & 63u);
      std::uint64_t &word =
          overflowWords.empty() ? inlineWord : overflowWords[wordIdx];
      const bool prev = (word & mask) != 0ull;
      if (prev == value)
        return;
      if (value) {
        word |= mask;
        ++count;
      } else {
        word &= ~mask;
        --count;
      }
    }
    void pushBack(bool value, std::size_t oldSize);
    void eraseAt(std::size_t idx, std::size_t oldSize);
    void clear() noexcept {
      inlineWord = 0;
      overflowWords.clear();
      count = 0;
    }
  };

  EntitySectionBucket() = default;
  ~EntitySectionBucket();
  EntitySectionBucket(const EntitySectionBucket &) = delete;
  EntitySectionBucket &operator=(const EntitySectionBucket &) = delete;
  EntitySectionBucket(EntitySectionBucket &&) = delete;
  EntitySectionBucket &operator=(EntitySectionBucket &&) = delete;

  std::size_t size() const noexcept { return entities.size(); }
  std::size_t wordCount() const noexcept {
    return (entities.size() + 63u) >> 6u;
  }
  void setFlag(std::size_t idx, EntitySectionFlag flag, bool value) noexcept {
    switch (flag) {
    case EntitySectionFlag::Active:
      active.set(idx, value);
      break;
    case EntitySectionFlag::Pushable:
      pushable.set(idx, value);
      break;
    case EntitySectionFlag::HardCollider:
      hardCollider.set(idx, value);
      break;
    case EntitySectionFlag::Minecart:
      minecart.set(idx, value);
      break;
    case EntitySectionFlag::Item:
      item.set(idx, value);
      break;
    }
  }
  void append(Entity &entity, const Aabb &box, bool isActive, bool isPushable,
              bool isHardCollider, bool isMinecart, bool isItem);
  void eraseAt(std::size_t idx);

  std::vector<Entity *> entities;
  std::vector<Aabb> boxes;
  BitPlane active;
  BitPlane pushable;
  BitPlane hardCollider;
  BitPlane minecart;
  BitPlane item;
};

// 单维度世界模拟核心（对应原版 `ServerLevel` / `Level`）：
// 管理已加载区块、方块读写与各类原版更新链、计划刻、方块事件、方块实体与实体物理。
class World {
public:
  explicit World(int minY = -64, int maxY = 320,
                 int maxNeighborUpdates = 1000000);
  explicit World(DimensionTypeSpec dimension, int maxNeighborUpdates = 1000000,
                 std::string_view dimensionId = {});
  ~World();
  World(World &&) noexcept;
  World &operator=(World &&) noexcept;
  World(const World &) = delete;
  World &operator=(const World &) = delete;
  // ~registerLight=false~ 只建立区块节点、不向光照引擎提交登记，用于“生成后再
  // 统一登记”的晋升路径（此时方块还没导入，提交进去的会是空几何）。
  // `lightEpoch` 非 0 时沿用调用方提供的加载/光照生命周期（ProtoChunk 晋升路径），
  // 否则按新的加载生命周期递增。
  void loadChunk(int x, int z, bool registerLight = true,
                 std::uint64_t lightEpoch = 0);
  // 让本地区块的生命周期代次对齐引擎里已有的登记。生成路径接管流水线先前的
  // 登记时需要它，否则存档/网络侧会认为光照属于上一个生命周期而永远不就绪。
  void alignChunkLightLifecycle(int x, int z);
  void unloadChunk(int x, int z);
  bool isChunkLoaded(int x, int z) const;
  // 测试可见：区块当前的加载生命周期与光照生命周期代次。
  // 受管安装必须让两者等于生成/读取请求的代次（Proto -> World 不换代）。
  std::uint64_t chunkLoadEpochForTest(int x, int z) const;
  std::uint64_t chunkLightEpochForTest(int x, int z) const;
  // 测试可见：生成期光照请求序号（0 = 安装时由 World 自行登记）。
  std::uint64_t chunkRequiredLightGenerationForTest(int x, int z) const;
  // 测试可见：安装时接管生成期光照登记（而不是重新提交几何）的次数。
  std::uint64_t generatedLightAdoptionCountForTest() const {
    return generatedLightAdoptions_;
  }
  bool isLoaded(Pos pos) const;
  // 显式有序的数据包列表，按优先级从低到高排列。仅在全部新定义构建完成后才原子替换当前世界的标签快照。
  BlockTagRegistry::ReloadResult reloadBlockTags(const std::vector<std::filesystem::path> &packs);
  std::optional<bool> hasBlockTag(const BlockState &state, std::string_view tag) const;
  const BlockTagRegistry *customBlockTags() const { return blockTags_.get(); }
  std::uint64_t blockTagRevision() const { return blockTagRevision_; }
  // 可选启用的后台光照接管（L-1）。当前生产服务端默认关闭；尚未实现完整光照传播与协议级光照快照。
  void enableLighting(LightWorkerOptions options = {});
  void disableLighting();
  void setLowMemoryMode(bool enabled);
  bool lowMemoryMode() const noexcept { return lowMemoryMode_; }
  bool lightingEnabled() const { return lightService_ != nullptr; }
  LightService *lightService() const { return lightService_.get(); }
  void setCleanupManager(std::shared_ptr<PeriodicCleanupManager> manager,
                         bool autoAdvance = true);
  const std::shared_ptr<PeriodicCleanupManager> &cleanupManager() const noexcept {
    return cleanup_.manager;
  }
  // 主线程光照结果移交入口。在世界刻冻结时也可合法调用，使已完成的光照批次能立即同步至玩家会话。
  void drainLightPublications();
  // 查询期按原版语义动态合成亮度：方块光与天空光在存储上保持分离，
  // 本接口读取主线程双层缓存并返回 `max(block, sky - skyDarken)`，无需额外分配合成区段。
  int getBrightness(Pos pos, int skyDarken = 0) const;
  BlockState getBlock(Pos pos) const;
  std::optional<BlockState> getBlockIfLoaded(Pos pos) const;
  // 对应 `Heightmap.Types` 的四张终态高度图。导入区块或原始批量写入后统一初始化，
  // 常规 `setBlock` 严格按 `LevelChunk.setBlockState` 的顺序同步更新它们。
  const Heightmap *heightmap(int chunkX, int chunkZ, HeightmapType type) const;
  int height(HeightmapType type, int blockX, int blockZ) const;
  void primeHeightmaps();
  // 只重建单个区块的四张终态高度图。逐区块导入/晋升时使用，避免对整个已加载
  // 世界做一次全量重扫（installProtoChunkIntoWorld 原先每个区块都会触发该全量扫描）。
  void primeHeightmaps(int chunkX, int chunkZ);
  // 在批量原始写入后，用当前权威区块状态替换已入队的导入前光照快照。未启用光照时为空操作。
  void refreshLightSnapshots();
  // 只把单个区块的几何/持久化光照重新投递给光照引擎。逐区块晋升时必须用它，
  // 不能用 refreshLightSnapshots()（后者会重投所有已加载区块，令 N 个区块的
  // 导入退化为 O(N^2) 次提交）。
  void reloadChunkLight(int x, int z);
  // 生成期光照交接（主线程）：生成 worker 只构造输入并投递，由这里真正提交给光照引擎。
  // 返回请求序号（0 = 未开启光照 / 服务已关闭），安装时据此跳过重复提交。
  std::uint64_t submitGeneratedChunkLight(LightChunkInput input);
  std::uint64_t requestGeneratedChunkLightCompute(int x, int z);
  void setChunkPersistedLight(int x, int z, PersistedChunkLight persisted);
  // 按维度独立的兴趣点索引（对应原版 `PoiManager` / `PoiSection`）。
  PoiStorage &pois() { return *poiStorage_; }
  const PoiStorage &pois() const { return *poiStorage_; }
  // 导出单个区块调色板区段的只读拷贝快照（不暴露内部可变状态）。
  ChunkSnapshot snapshotChunk(int chunkX, int chunkZ) const;
  std::string_view getNoiseBiome(int quartX, int quartY, int quartZ) const;
  void setSectionBiomes(int chunkX, int sectionY, int chunkZ,
                        std::shared_ptr<SectionBiomes> biomes);
  bool setBlock(Pos pos, BlockState state, int flags = 3, int limit = 512);
  // 对应 `Level.removeBlock`：写回该位置当前所属的流体遗留方块而非强制写空气，
  // 从而使含水宿主方块被破坏后能保留水体（`Level.java:276-280`）。`movedByPiston` 与原版一样设置标志位 64。
  bool removeBlock(Pos pos, bool movedByPiston = false);
  void rawSetBlock(Pos pos, BlockState state);
  // 将解码后的单个 16x16x16 区段直接批量导入已加载区块，避免逐格走 `setBlock` 开销。
  void importSectionBlocks(int cx, int cz, int sectionY,
                           std::span<const BlockState::Id> palette,
                           std::span<const std::uint16_t, kLightVolume> indices);
  std::map<Pos, BlockState> allBlocks() const;
  int minY() const { return minY_; }
  int maxY() const { return maxY_; }
  const DimensionTypeSpec &dimension() const noexcept { return dimension_; }
  const std::string &dimensionId() const noexcept { return dimensionId_; }
  void setDimension(DimensionTypeSpec dimension,
                    std::string_view dimensionId = {});
  // `LevelChunkSection` 容量：24 * 16 = 384 格纵向跨度，覆盖默认 `-64..320` 维度。
  // 区块区段表采用该大小的固定内联数组以消除每区段指针跳转，因此构造函数会拒绝更高的世界。
  // 共享区段容量与随机刻微区段布局定义在 chunk_section.hpp，World 保留同名别名。
  static constexpr int kSectionCapacity = redstone::kSectionCapacity;
  static constexpr int kRandomTickMinisectionSize =
      redstone::kRandomTickMinisectionSize;
  static constexpr int kRandomTickMinisectionCount =
      redstone::kRandomTickMinisectionCount;
  static constexpr int kRandomTickLastMinisectionSize =
      redstone::kRandomTickLastMinisectionSize;
  // 对应 `ServerLevelMixin.MAX_COUNT_FOR_BLOCK_SEARCH_AFTER_RANDOM_CHANCE = 4096 * 0.09375`：
  // 低于该阈值走微区段稀疏直达路径，高于该阈值直接按随机索引探测。
  static constexpr int kRandomTickSparseThreshold = 384;
  std::optional<BlockState> coralPlacementState(Pos pos,
                                                const BlockState &selected,
                                                Direction clickedFace);
  // 分离的方块更新与形状更新入口（严禁合并成泛化邻居刷新以保持原版红石时序）：
  BlockState updateFromNeighbourShapes(Pos p, const BlockState &state);
  void updateOrDestroy(Pos p, const BlockState &oldState,
                       const BlockState &newState, int flags, int limit);
  void neighborShapeChanged(Pos p, Direction d, Pos neighbor,
                            const BlockState &state, int flags, int limit);
  void updateNeighbourShapes(Pos p, const BlockState &state, int flags,
                             int limit);
  void updateIndirectNeighbourShapes(Pos p, const BlockState &state, int flags,
                                     int limit);
  void affectNeighborsAfterRemoval(Pos p, const BlockState &state, bool moved);
  void neighborChanged(Pos pos, const BlockRef &source);
  void neighborChanged(const BlockState &state, Pos pos,
                       const BlockRef &source, bool movedByPiston = false);
  void updateNeighborsAt(Pos pos, const BlockRef &source,
                         std::optional<Direction> skip = {});
  void updateNeighborsAtExceptFromFacing(Pos pos, const BlockRef &source,
                                         Direction skip) {
    updateNeighborsAt(pos, source, skip);
  }
  void updateNeighbourForOutputSignal(Pos pos, const BlockRef &changedBlock);
  void scheduleTick(Pos pos, const BlockRef &block, int delay,
                    int priority = 0);
  // 对应 `LevelTicks.schedule`：插入已构造好的计划刻（用于存档恢复或测试夹具），
  // 与 `LevelChunkTicks.schedule` 一致按 `(pos, block)` 去重。
  bool scheduleTick(const ScheduledTick &tick);
  // 对应 `LevelChunkTicks.scheduleUnchecked`：完整保留存档列表中可能已存在的重复项。
  bool restoreScheduledTick(const ScheduledTick &tick);
  bool hasScheduledTick(Pos pos, const BlockRef &block) const;
  bool willTickThisTick(Pos pos, const BlockRef &block) const;
  std::size_t scheduledTickCount() const;
  // 按实际派发（drain）顺序返回确定性快照，供日志、调试工具与测试断言使用。
  std::vector<ScheduledTick> scheduledTicks() const;
  std::vector<PistonSnapshot> snapshotChunkPistons(int chunkX,
                                                    int chunkZ) const;
  // 用于在线持久化的按区块计划刻访问接口：仅访问目标区块自身容器，避免遍历全维度队列。
  ChunkTickSnapshot snapshotChunkTicks(int chunkX, int chunkZ) const;
  ChunkTickSnapshot takeChunkTicks(int chunkX, int chunkZ);
  std::size_t chunkTickCount(int chunkX, int chunkZ) const;
  // 对应 `ServerLevel.fluidTicks`：独立的流体计划刻 `LevelTicks` 容器。
  // 与方块计划刻一样按 `(pos, fluid)` 去重，且两者绝不共享容器状态。
  bool scheduleFluidTick(Pos pos, const FluidRef &fluid, int delay,
                         int priority = 0);
  bool scheduleFluidTick(const ScheduledTick &tick, const FluidRef &fluid);
  // 对应 `LevelChunkTicks.scheduleUnchecked`，用于恢复存档中的流体计划刻列表。
  bool restoreFluidTick(const ScheduledTick &tick, const FluidRef &fluid);
  bool hasFluidTick(Pos pos, const FluidRef &fluid) const;
  std::size_t fluidTickCount() const;
  std::vector<std::pair<ScheduledTick, FluidRef>> fluidTicks() const;
  void blockEvent(Pos pos, const BlockRef &block, int type, int data);
  std::size_t blockEventCount() const;
  std::vector<BlockEvent> pendingBlockEvents() const;
  int getDirectSignal(Pos pos, Direction direction) const;
  int getDirectSignalTo(Pos pos) const;
  int getControlInputSignal(Pos pos, Direction direction, bool diodesOnly) const;
  int getSignal(Pos pos, Direction direction) const;
  int getBestNeighborSignal(Pos pos) const;
  bool hasSignal(Pos pos, Direction direction) const;
  bool hasNeighborSignal(Pos pos) const;
  bool wireShouldSignal() const { return wireShouldSignal_; }
  void setWireShouldSignal(bool value) const { wireShouldSignal_ = value; }
  int comparatorOutput(Pos pos) const;
  void setComparatorOutput(Pos pos, int value);
  struct TorchToggle {
    Pos pos;
    std::int64_t when = 0;
  };
  std::deque<TorchToggle> &recentTorchToggles() { return recentTorchToggles_; }
  void tick();
  std::int64_t gameTime() const { return time_; }
  void setGameTime(std::int64_t value) { time_ = value; }
  bool isHandlingTick() const { return handlingTick_; }
  // 基准测试使用的轻量工作计数器：派发计数包含传给 `tickFluidAt` 的陈旧/空条目，
  // 写入计数统计从已派发流体刻内部成功发起的 `setBlock` 次数。
  std::uint64_t fluidDispatchCount() const { return fluidDispatches_; }
  std::uint64_t fluidWriteCount() const { return fluidWrites_; }
  std::vector<LogEntry> log;
  std::function<void(World &)> blockEntityTicker;
  // 位于 `Level.sendBlockUpdated` / `doBlockEvent` 处的只读观测钩子（供网络广播使用）。
  // 消费方严禁在这些回调内部反向修改模拟状态。
  std::function<void(Pos, const BlockState &, int)> blockUpdateObserver;
  // `FallingBlockEntity` 在成功落地并放置方块后、销毁自身之前向追踪者发送的即时位置/状态修正通知。
  std::function<void(int, Pos, const BlockState &)> fallingBlockLandObserver;
  std::function<void(const BlockEvent &)> blockEventObserver;
  // 在完整光照批次发布到主线程之后、且 `ChunkHolder.broadcastChanges` 排空普通方块更新之前执行。
  std::function<void(std::shared_ptr<const LightChunkSnapshot>)>
      lightPublicationObserver;
  // 显式 `World` 加载/卸载通知（独立于玩家会话视距追踪边界），在光照服务接受生命周期变更后于主线程触发。
  std::function<void(int, int, bool)> lightChunkLifecycleObserver;
  std::function<void()> blockUpdatesBroadcastObserver;
  std::function<void()> entityTrackingBroadcastObserver;
  std::function<void(const Entity &)> entityAddedObserver;
  std::function<void(int)> entityRemovedObserver;
  std::function<void(int collectorId, int targetId, int amount)>
      entityTakenObserver;
  std::function<void()> nightSkippedObserver;
  std::function<void(Vec3, float, int)> explosionObserver;
  std::function<void(Pos, BlockState, bool isFluid)> unsupportedRandomTickObserver;
  // 可选安装的 `WorldChunkManager` 脏标钩子：每次实际改变状态的写入（含嵌套 `setBlock`）都会汇报前后状态。
  std::function<void(Pos, const BlockState &, const BlockState &)>
      managedBlockMutationObserver;
  // 计划刻的调度、恢复、执行或移除时触发，汇报受影响的方块位置以便标脏。
  std::function<void(Pos)> tickMutationObserver;
  // 移动活塞推进、创建或移除时失效所属区块的持久化方块实体视图。
  std::function<void(Pos)> pistonEntityMutationObserver;
  // 运行时实体刻/增删观测器：同时传递旧坐标与新坐标，使托管管理器只需标脏源区块和目标区块；
  // 持久 UUID 用于在跨区块移交时独立于运行时实体 ID 追踪实体归属。
  std::function<void(Vec3, Vec3, const std::array<std::int32_t, 4> &, bool)>
      managedEntityMutationObserver;
  // 可选安装的 `TicketType::EnderPearl` 钩子，由 `WorldChunkManager` 注入以在珍珠落点区块续期加载票。
  std::function<std::int64_t(int chunkX, int chunkZ)> enderPearlTicketObserver;
  // 可选的 26.1 `MINECART_IMPROVEMENTS` 实验性矿车行为开关。
  bool minecartImprovements = false;
  // 当世界机制（如活塞、传送、水流等）在实体自身 tick 之外移动了实体时，立即同步更新其区段空间索引，
  // 并将原索引位置通知管理器，防止区段桶与持久化状态滞后到下一个实体刻才发现位置变化。
  void synchronizeEntityAfterMove(Entity &entity, Vec3 previousPosition);
  // 仅用于测试夹具：模拟两次 tick 之间的显式桶迁移，并用稳定的持久 UUID 触发生产跨边界观测回调。
  void moveEntityForManagedTest(const Entity &entity, Vec3 to);
  // 钩子为空时保持旧版离线测试行为；在线模式下由 `WorldChunkManager` 安装基于票据缓存的位置就绪谓词。
  std::function<bool(Pos)> blockTickPositionReady;
  std::function<bool(Pos)> entityTickPositionReady;
  // 高度图直扫柱面的诊断计数器。生产路径默认关闭，仅测试夹具针对长跨度场景开启以验证跳过空区段效果。
  struct HeightmapScanStats {
    bool enabled = false;
    std::uint64_t scans = 0;
    std::uint64_t visitedSections = 0;
    std::uint64_t skippedSections = 0;
    std::uint64_t readCells = 0;
  };
  HeightmapScanStats heightmapScanStats;

  void setPistonEntity(Pos pos,
                       std::shared_ptr<PistonMovingBlockEntity> entity);
  void removePistonEntity(Pos pos, bool notify = true);
  // 对应 `LevelChunk.blockEntities`：位置映射表是查询（`getBlockEntity`）的权威来源，tick 阶段不直接查它。
  PosMap<std::shared_ptr<PistonMovingBlockEntity>> pistonEntities;
  // 对应 `LevelChunk.tickersInLevel`：仅用于在已有 ticker 的位置重绑，保证同槽位替换后仍保持原列表槽位与顺序。
  PosMap<PistonTickerSlot *> pistonTickerSlots;
  // 对应 `Level.blockEntityTickers` / `pendingBlockEntityTickers`：按注册顺序遍历的扁平列表。
  // 遍历期间新创建的活塞 ticker 先追加到 `pendingPistonTickers`，并在下轮开始时合并；
  // 已移除的槽位在遍历时跳过，随后由 `sweepPistonTickers()` 清理。
  std::vector<std::unique_ptr<PistonTickerSlot>> pistonTickers;
  std::vector<std::unique_ptr<PistonTickerSlot>> pendingPistonTickers;
  bool tickingPistonBlockEntities = false;
  std::size_t pistonTickerCount() const { return pistonTickers.size(); }
  std::size_t pendingPistonTickerCount() const {
    return pendingPistonTickers.size();
  }
  void mergePendingPistonTickers();
  void sweepPistonTickers();

  // 容器/漏斗睡眠优化与传输统计计数器，用于验证空闲漏斗跳过与事件唤醒效果。
  struct ContainerTickStats {
    std::uint64_t activeTicks = 0;
    std::uint64_t sleepingSkippedTicks = 0;
    std::uint64_t sleepTransitions = 0;
    std::uint64_t wakeups = 0;
    std::uint64_t cachedEmptyChecks = 0;
    std::uint64_t cachedFullChecks = 0;
    std::uint64_t cachedNoOpTransfers = 0;
  };
  ContainerTickStats containerTickStats;
  void setContainerEntity(Pos pos, std::shared_ptr<ContainerBlockEntity> entity,
                          bool registerTicker = false);
  void removeContainerEntity(Pos pos, bool dropContents = false,
                             bool notify = true);
  std::shared_ptr<ContainerBlockEntity> containerEntity(Pos pos) const;
  void registerContainerTicker(Pos pos,
                               std::shared_ptr<ContainerBlockEntity> entity);
  void mergePendingContainerTickers();
  void sweepContainerTickers();
  void wakeHopperBlockWatchers(Pos pos);
  void wakeHopperItemWatchers(Vec3 pos);
  void watchHopperBlock(Pos pos, const std::shared_ptr<HopperBlockEntity> &hopper);
  void watchHopperItemSection(std::uint64_t sectionKey,
                              const std::shared_ptr<HopperBlockEntity> &hopper);
  PosMap<std::shared_ptr<ContainerBlockEntity>> containerEntities;
  PosMap<ContainerTickerSlot *> containerTickerSlots;
  std::vector<std::unique_ptr<ContainerTickerSlot>> containerTickers;
  std::vector<std::unique_ptr<ContainerTickerSlot>> pendingContainerTickers;
  bool tickingContainerBlockEntities = false;
  std::size_t currentContainerTickerIndex_ = 0;
  std::unordered_map<std::uint64_t, std::vector<Pos>> containerPositionsByChunk_;
  std::unordered_map<std::uint64_t, std::vector<std::weak_ptr<HopperBlockEntity>>>
      hopperItemWatchersBySection_;
  PosMap<std::vector<std::weak_ptr<HopperBlockEntity>>> hopperBlockWatchersByPos_;
  // 实体阶段：对应原版 `EntityTickList` 按插入序遍历活跃映射表的语义。
  // 遍历期间新加入的实体在下一 tick 才开始执行；`tickEntities()` 遍历当前快照并跳过已移除实体。
  std::shared_ptr<Entity> addEntity(std::shared_ptr<Entity> entity);
  std::shared_ptr<Entity> addEntityWithId(std::shared_ptr<Entity> entity,
                                          int fixedId);
  // 服务端协议玩家与 `World` 实体共享这一单调递增的 ID 命名空间，
  // 确保在实体移除与玩家断线重连后运行时实体 ID 仍保持唯一。
  int allocateEntityId();
  void removeEntity(const std::shared_ptr<Entity> &entity);
  bool detachEntityForDimensionTransfer(const std::shared_ptr<Entity> &entity);
  struct PendingPortalTransfer {
    std::shared_ptr<Entity> entity;
    Pos portalEntryPos{};
  };
  void queuePortalTransfer(const Entity &entity, Pos entryPos);
  std::vector<PendingPortalTransfer> takePendingPortalTransfers();
  // 对应 `EntitySectionStorage.getEntities`：仅访问与包围盒重叠的区段，结果保持区段内部插入顺序。
  std::vector<Entity *> entitiesIn(const Aabb &box) const;
  // 按位平面掩码（`EntitySectionFlag`）就地访问重叠区段内的匹配实体，回调返回 false 可提前中止。
  // 之所以提供基于位平面掩码的零分配回调，是为了避免高频碰撞/查询构造临时 `std::vector<Entity*>`。
  template <class F>
  bool forEachEntityInFlags(const Aabb &box, std::uint8_t anyFlagsMask,
                            F &&visitor) const {
    struct Dispatch {
      static bool call(void *ctx, Entity &entity) {
        if constexpr (std::is_same_v<std::invoke_result_t<F &, Entity &>,
                                     bool>) {
          return (*static_cast<F *>(ctx))(entity);
        } else {
          (*static_cast<F *>(ctx))(entity);
          return true;
        }
      }
    };
    return visitEntitiesInFlags(box, anyFlagsMask, &visitor, &Dispatch::call);
  }
  const EntitySectionBucket *findEntitySectionBucket(std::uint64_t key) const {
    auto it = entitySections_.find(key);
    return it == entitySections_.end() ? nullptr : it->second.get();
  }
  // 查询区块范围 `[minX,maxX] x [minZ,maxZ]` 全高度内的实体并按实体 ID（生成顺序）排序。
  // 使 `ChunkMap` 追踪器只需扫描玩家可见范围内的区块，而不必遍历整个世界的实体列表。
  std::vector<Entity *> entitiesInSectionRange(int minX, int maxX, int minZ,
                                               int maxZ) const;
  struct ItemEntitySnapshotResult {
    std::vector<entity_codec::EntityRecord> records;
    bool hasUnsupported = false;
    std::string detail;
  };
  ItemEntitySnapshotResult snapshotChunkItemEntities(int chunkX,
                                                      int chunkZ) const;
  // 仅用于测试的视图：返回单个区块内紧凑实体序列的持久 UUID 顺序。
  std::vector<std::array<std::int32_t, 4>>
  chunkEntityOrderForTest(int chunkX, int chunkZ) const;
  bool hasEntityUuid(const std::array<std::int32_t, 4> &uuid) const;
  std::vector<std::shared_ptr<Entity>> entities;
  int nextEntityId = 1;
  Random random;
  std::uint64_t seed = 0;
  void setSeed(std::uint64_t value);
  std::vector<VoxelShape> blockCollisions(const Aabb &box);
  // 与 `blockCollisions` 查询等价，但直接将世界空间包围盒写入 `out`，
  // 避免在高频物理路径中为每个格子构造临时 `VoxelShape` 向量（原版 `BlockCollisions` 迭代器开销）。
  void collectCollisionBoxes(const Aabb &box, std::vector<ShapeBox> &out);
  // 实体向下运动时的快速地面支撑检查：先直接探测脚下中心格是否为整格实体方块，
  // 若已贴合顶面则立即返回 `0.0`，使地面行走/静止实体跳过完整的体素碰撞盒收集。
  std::optional<double> tryCollideSupportingBlockY(const Aabb &box,
                                                   double movementY) const;
  // 按单轴滑移碰撞裁剪移动距离：直接沿目标轴步进扫掠网格与移动活塞，
  // 随碰随缩窄步长区间，从而省去预先物化整个包围盒内所有碰撞体素向量的分配与遍历开销。
  double collideBlocksOnAxis(int axis, const Aabb &box, double distance) const;
  // 自动上台阶（step-up）的快速拒止预检：检查水平移动前方覆盖的整数格子柱是否全为整格阻挡，
  // 若已被整墙完全挡死，则提前放弃昂贵的台阶候选高度收集与二次轴向重算。
  bool isStepUpFullyBlocked(const Aabb &groundedBox, float maxStep,
                            Vec3 movementStep) const;
  // 单遍 `LivingEntity` 挤压（cramming）统计与双向推挤计算：
  // 借助 `EntitySectionBucket` 的 `active & pushable` 位平面交集在单次区段扫描中同时统计拥挤实体数
  // 并将互推冲量累加到自身（`selfAccX`/`selfAccZ`）与非载具邻居，避免先收集列表再逐个推挤的二次遍历与堆分配。
  int pushLivingEntities(Entity &self, const Aabb &box, bool applyPush,
                         bool selfCanReceivePush, bool selfPassenger,
                         const Entity *selfRoot, double sx, double sz,
                         double &selfAccX, double &selfAccZ,
                         bool &anySelfPush) const;
  std::vector<ShapeBox> getEntityCollisions(const Entity *source,
                                            const Aabb &box) const;
  void collectEntityCollisions(const Entity *source, const Aabb &box,
                               std::vector<ShapeBox> &out) const;
  bool hasEntityCollision(const Entity *source, const Aabb &box) const;
  std::optional<Pos> findSupportingBlock(const Entity &source,
                                         const Aabb &box);
  std::optional<Vec3> findFreePosition(const Entity *source,
                                       const Aabb &allowedCenters,
                                       Vec3 preferredCenter, double sizeX,
                                       double sizeY, double sizeZ);
  bool noCollision(const Aabb &box);
  bool noCollision(const Entity *source, const Aabb &box);
  bool clip(Vec3 from, Vec3 to);
  BlockHitResult clipBlock(Vec3 from, Vec3 to, bool fluidsOnly = false);
  std::optional<EntityHitResult>
  clipEntity(const Entity *source, Vec3 from, Vec3 to, const Aabb &searchBox,
             const std::function<bool(const Entity &)> &filter = {}) const;
  void tickEntities();
  // 实体生成辅助接口：在安装真实实体的同时记录可观测的生成日志条目。
  void spawnItem(Pos pos, const std::string &item, int count = 1,
                 bool defaultPickupDelay = true);
  void spawnItemAt(Vec3 pos, const std::string &item, int count = 1,
                   bool defaultPickupDelay = true);
  void spawnPrimedTnt(Vec3 pos, int fuse = 80);
  void spawnFallingBlock(Vec3 pos, const BlockState &state);
  void spawnMinecart(Vec3 pos);
  // 对应 `TntBlock.prime` 后接 `Level.removeBlock(pos, false)`。
  bool primeTntBlock(Pos pos);
  // 对应原版 `ServerExplosion`：包含 TNT 威力、伤害计算与方块破坏交互。
  // `source` 可为空；仅在计算击退过滤时需要直接伤害源实体身份。
  void explode(Entity *source, Vec3 center, float radius, bool fire);
  void explodeTriggerBlock(Entity *source, Vec3 center, float radius);
  // `ServerExplosion.calculateExplodedPositions` 的工作计数器（按 `World` 生命周期累计）。
  // 模拟逻辑本身从不读取这些计数器；它们仅用于测试夹具量化单次爆炸实际触发了多少次方块探测、
  // 产生了多少候选条目，以及最终排序去重丢弃了多少冗余条目。
  struct ExplosionStats {
    std::uint64_t explosions = 0;
    // 未命中直映缓存而真正读取方块状态的探测次数（probe）。
    std::uint64_t probes = 0;
    // 去重前追加到待破坏列表的候选条目数（射线步进进入新位置且仍有剩余能量时记入）。
    std::uint64_t candidates = 0;
    // 经 sort + unique 后实际送入 `interactWithBlocks` 的最终破坏数。
    std::uint64_t destroyed = 0;
  };
  ExplosionStats explosionStats;
  // `BlockGetter.traverseBlocks` 射线步进统计计数器（用途同上：量化射线裁剪调用次数与探测格数）。
  std::uint64_t clipCalls = 0;
  std::uint64_t clipCells = 0;
  // 对应 `PistonMovingBlockEntity` 的 `ThreadLocal` `NOCLIP` 守卫。
  // 置位期间，运动方向匹配的移动活塞在碰撞查询中对被推动实体忽略其已移动状态的碰撞形状。
  std::optional<Direction> pistonNoClip;
  // 可选测试/观测钩子：在移动活塞实体完成推进并被移除后触发。
  std::function<void(World &, Pos)> pistonCompletionObserver;
  bool tntExplodes = true;
  // 对应 `GameRules.WATER_SOURCE_CONVERSION` / `LAVA_SOURCE_CONVERSION`。
  // 原版 26.1 默认水开启、熔岩关闭；由 `FlowingFluid.canConvertToSource` 读取。
  bool waterSourceConversion = true;
  bool lavaSourceConversion = false;
  bool spreadVines = true;
  int fireSpreadRadiusAroundPlayer = 128;
  bool waterEvaporates = false;
  bool fastLava = false;
  std::vector<Vec3> fireSpreadPlayerPositions;
  int randomTickSpeed = 3;
  bool traceUpdates = false;
  bool anyPlayerCloseEnoughForFireSpread(Pos pos) const;
  struct RandomTickStats {
    std::uint64_t chunksVisited = 0;
    std::uint64_t sectionsVisited = 0;
    std::uint64_t sectionsSparsePath = 0;
    std::uint64_t sectionsDensePath = 0;
    std::uint64_t positionsProbed = 0;
    std::uint64_t blocksTicked = 0;
    std::uint64_t fluidsTicked = 0;
    std::uint64_t ticksApplied = 0;
    std::uint64_t ticksUnsupported = 0;
    std::uint64_t lastTickNs = 0;
    void reset() { *this = {}; }
  };
  RandomTickStats randomTickStats;
  // 从已发布的 `LightService` 快照或持久化区块光照数据中读取局部原始总亮度 `max(block, sky)` 与方块光 `LightLayer::BLOCK`。
  std::optional<int> rawBrightness(Pos pos) const;
  std::optional<int> blockBrightness(Pos pos) const;
  std::uint16_t sectionTickingBlockCountForTest(int chunkX, int sectionY,
                                                int chunkZ) const;
  std::uint8_t sectionStorageBitsForTest(int chunkX, int sectionY,
                                         int chunkZ) const;
  std::array<std::uint8_t, kRandomTickMinisectionCount>
  sectionRandomTickMinisectionsForTest(int chunkX, int sectionY,
                                       int chunkZ) const;

private:
  std::shared_ptr<const BlockTagRegistry> blockTags_;
  std::uint64_t blockTagRevision_ = 0;
  struct Update {
    enum class Kind { Simple, Full, Multi, Shape } kind;
    Pos pos;
    // 更新队列条目仅保存源方块的轻量身份句柄（`BlockRef`），而非持有字符串拷贝。
    BlockRef source;
    std::optional<Direction> skip;
    int index = 0;
    Direction direction = Direction::Down;
    BlockState neighbor;
    BlockState capturedState;
    bool movedByPiston = false;
    Pos neighborPos;
    int flags = 0, limit = 0;
  };
  void enqueue(Update update);
  void runUpdates();
  bool runNext(Update &update);
  void executeNeighbor(Pos pos, const BlockState &state,
                       const BlockRef &source, bool movedByPiston);
  BlockState updateShapeState(Pos pos, const BlockState &state,
                              Direction direction, Pos neighborPos,
                              const BlockState &neighbor);
  void observerNotify(Pos pos, const BlockState &state);
  void shapeUpdate(Pos pos, Direction direction, Pos neighborPos,
                   const BlockState &neighbor, int flags, int limit);
  void tickBlockAt(Pos pos, const Block *expected);
  void tickFluidAt(Pos pos, const Fluid *expected);
  std::unique_ptr<LightChunkInput> makeLightChunkInput(int x, int z) const;
  void runEvents();
  // 统一区段：World 与 ProtoChunk 共用 chunk_section.hpp 的 Section 存储与计数。
  using Section = redstone::BlockSection;
  using SectionStorage = redstone::SectionStorage;
  using SectionStorage8 = redstone::SectionStorage8;
  using SectionStorage16 = redstone::SectionStorage16;

  static std::uint8_t minBitsForPaletteSize(std::size_t paletteSize) noexcept {
    return redstone::minBitsForPaletteSize(paletteSize);
  }
  static std::shared_ptr<SectionStorage>
  makeSectionStorageForBits(std::uint8_t bits) {
    return redstone::makeSectionStorageForBits(bits);
  }
  template <typename StoragePtr, typename Fn>
  static decltype(auto) visitSectionStorage(StoragePtr *base, Fn &&fn) {
    return redstone::visitSectionStorage(base, std::forward<Fn>(fn));
  }
  static BlockState::Id readSectionCell(const Section &sec, int cell) noexcept {
    if (!sec.storage)
      return sec.single;
    return visitSectionStorage(
        sec.storage.get(),
        [c = static_cast<std::size_t>(cell)](const auto *blk) {
          return blk->palette[blk->indexAt(c)];
        });
  }
  // World 区块：方块区段与生物群系来自共享 BaseChunk，模拟状态只追加在派生部分。
  struct Chunk : public BaseChunk {
    static constexpr std::uint32_t kInvalidRandomTickIndex =
        std::numeric_limits<std::uint32_t>::max();
    bool loaded = true;
    bool hasPendingLightCell = false;
    std::uint32_t randomTickingSectionMask = 0;
    std::uint32_t randomTickIndex = kInvalidRandomTickIndex;
    // 当前已加载生命周期的 `LightService` 代数令牌；同一地址重载区块会递增它，从而安全丢弃队列中旧生命周期的单元更新。
    std::uint64_t lightLifecycleEpoch = 1;
    // 主线程在该区块上执行光学方块写入后产生的最新 `LightService` 序列号。
    // 持久化检查要求已发布的光照快照代次 >= `requiredLightGeneration` 才允许将区块标记为 `isLightOn=1`。
    std::uint64_t requiredLightGeneration = 0;
    std::optional<PersistedChunkLight> persistedLight;
    std::array<std::int16_t, kSectionCapacity> persistedLightSectionIndex{};
    // 区段数组直接内联在 `Chunk` 中：从区块到区段头只需一次指针解引用，消除逐区段堆分配且所有区段头内存连续。
    // 默认构造的 `Section` 即为空存储的单值空气区段，因此无需额外维护“是否存在”标记；
    // 首次写入非空气方块时 `Section::set` 才会按需物化调色板。
    std::unique_ptr<ChunkHeightmaps> heightmaps;
    void refreshRandomTickMask(int count);
    void refreshPersistedLightIndex(int minSectionY, int numSections);
  };
  static int chunkIndex(int v) {
    // C++20 中有符号算术右移对负坐标执行向下取整。
    return v >> 4;
  }
  static std::uint64_t chunkKey(int x, int z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
           static_cast<std::uint32_t>(z);
  }
  int sectionCount() const { return ((maxY_ - minY_) + 15) / 16; }
  int sectionIndex(int y) const { return (y - minY_) >> 4; }
  Chunk *chunkAt(int x, int z);
  const Chunk *chunkAt(int x, int z) const;
  const Chunk *chunkAtCached(int x, int z) const;
  // 爆炸、碰撞与计划刻容器往往反复访问相邻的少数几个区块；由于 `unordered_map` 节点地址稳定，
  // 维护一个 64 槽的最近区块直映缓存（外加最近访问的区段缓存）可在 `getBlock` 热路径中跳过哈希探针与桶取模。
  struct ChunkCacheSlot {
    std::uint64_t key = ~std::uint64_t{0};
    const Chunk *chunk = nullptr;
  };
  struct BlockReadCache {
    std::array<ChunkCacheSlot, 64> chunks{};
    std::uint64_t sectionKey = 0;
    int sectionIndex = -1;
    const Section *section = nullptr;
    BlockReadCache() = default;
    // 移动 `World` 时必须清空双方读缓存，防止指针悬空指向旧 `World` 的存储；缓存重建开销极低。
    BlockReadCache(BlockReadCache &&other) noexcept { other.reset(); }
    BlockReadCache &operator=(BlockReadCache &&other) noexcept {
      reset();
      other.reset();
      return *this;
    }
    void reset() noexcept {
      chunks = {};
      sectionIndex = -1;
      section = nullptr;
    }
  };
  struct TickingChunkEntry {
    std::uint64_t key = 0;
    Chunk *chunk = nullptr;
    int x = 0;
    int z = 0;
  };
  mutable BlockReadCache blockReadCache_;
  std::unordered_map<std::uint64_t, Chunk> chunks_;
  // 维护所有 `randomTickingSectionMask != 0` 的已加载区块：按 `key` 懒排序以保证随机刻迭代顺序确定，
  // 同时借助 `Chunk::randomTickIndex` 支持 O(1) swap-and-pop 增删。
  std::vector<TickingChunkEntry> randomTickingChunks_;
  std::vector<TickingChunkEntry> randomTickChunkScratch_;
  bool randomTickingDirty_ = false;
  std::uint32_t randValue_ = 0x61c88647u;
  int nextRandomBlockIndex();
  const Section *loadedSectionAt(Pos p) const;
  void addRandomTickingChunk(Chunk &chunk, std::uint64_t key);
  void removeRandomTickingChunk(Chunk &chunk);
  void updateChunkRandomTickMask(Chunk &chunk, std::uint64_t key,
                                 int sectionIdx, bool sectionTicking);
  void reconcilePoiFromChunk(int cx, int cz, const Chunk &chunk);
  void tickChunks();
  void tickSparseRandomTickHit(int chunkX, int sectionBaseY, int chunkZ,
                               Section &section, int nth,
                               RandomTickAccess &access);
  void tickDenseRandomTickSection(int chunkX, int sectionBaseY, int chunkZ,
                                  Section &section, int tickSpeed,
                                  RandomTickAccess &access);
  void tickRandomChunkSection(int chunkX, int sectionY, int chunkZ,
                              Section &section, int tickSpeed,
                              RandomTickAccess &access);
  // 对应原版 `EntitySectionStorage`：以 `SectionPos.asLong` 为键的 16^3 空间分桶；
  // 每个桶内维护同序的 SoA 实体/包围盒数组与 64 位类型/状态位平面，用于高速过滤可推挤与碰撞实体。
  std::map<std::uint64_t, std::unique_ptr<EntitySectionBucket>> entitySections_;
  bool visitEntitiesInFlags(const Aabb &box, std::uint8_t anyFlagsMask,
                            void *ctx, bool (*callback)(void *, Entity &)) const;
  // 托管存档的区块卸载阻塞索引（按区块统计实体数、记录活塞与容器坐标）：
  // 将原本每次卸载/持久化判定时的全维度遍历转化为 O(1) 局部查询，并与主表增删路径同步维护。
  std::unordered_map<std::uint64_t, std::size_t> entityCountsByChunk_;
  std::unordered_map<std::uint64_t, std::vector<Pos>> pistonPositionsByChunk_;
  std::set<std::array<std::int32_t, 4>> entityUuids_;
  void entitySectionAdd(Entity &entity);
  void entitySectionRemove(Entity &entity);
  void entitySectionUpdate(Entity &entity);
  friend class PrimedTnt;
  void notifyEntityRemoved(Entity &entity);
  void broadcastEntityRemovals();
  int minY_, maxY_, maxUpdates_, updateCount_ = 0;
  DimensionTypeSpec dimension_ = overworldDimensionType();
  std::string dimensionId_ = "minecraft:overworld";
  bool handlingTick_ = false;
  bool insideFluidTick_ = false;
  std::uint64_t fluidDispatches_ = 0, fluidWrites_ = 0;
  std::int64_t time_ = 0;
  // 对应 `Level.nextSubTickCount`：单调递增的微刻序号，仅用于同刻同优先级计划刻的确定性排序，不持久化。
  std::int64_t subTickCount_ = 0;
  std::vector<Update> stack_, added_;
  // 对应 `LevelChunkTicks<ScheduledTick>`（按 `DRAIN_ORDER` 排序的逐区块优先队列 + `(pos, target)` 去重哈希集）
  // 与 `LevelTicks<ScheduledTick>`（区块容器索引、`nextTickForContainer` 与 `containersToTick`/`toRunThisTick` 排空状态）。
  // `ServerLevel` 为方块和流体各持有一份独立实例，故在 `engine.cpp` 中以模板完整定义以隐藏容器细节。
  std::unique_ptr<BlockTickQueue> tickQueue_;
  // 对应 `ServerLevel.fluidTicks`：拥有独立 65536 每刻上限的第二个 `LevelTicks` 队列，
  // 在方块计划刻之后、区块随机刻之前收集并执行。
  std::unique_ptr<FluidTickQueue> fluidTickQueue_;
  std::unique_ptr<LightService> lightService_;
  // 生成期光照接管次数（安装路径诊断/测试可见）。
  std::uint64_t generatedLightAdoptions_ = 0;
  struct CleanupRegistryHandle {
    std::shared_ptr<PeriodicCleanupManager> manager;
    bool autoAdvance = true;
    PeriodicCleanupManager::TaskId blockTickTaskId = 0;
    PeriodicCleanupManager::TaskId fluidTickTaskId = 0;
    PeriodicCleanupManager::TaskId lightTaskId = 0;
    ~CleanupRegistryHandle();
    CleanupRegistryHandle() = default;
    CleanupRegistryHandle(const CleanupRegistryHandle &) = delete;
    CleanupRegistryHandle &operator=(const CleanupRegistryHandle &) = delete;
    CleanupRegistryHandle(CleanupRegistryHandle &&) noexcept = default;
    CleanupRegistryHandle &operator=(CleanupRegistryHandle &&other) noexcept;
    void unregisterAll();
  } cleanup_;
  void registerTickCleanupTasks();
  void registerLightCleanupTask();
  void unregisterLightCleanupTask();
  std::vector<std::pair<int, Pos>> pendingPortalTransferIds_;
  std::unique_ptr<PoiStorage> poiStorage_;
  const LightOpticalTable *lightOptical_ = nullptr;
  bool lowMemoryMode_ = false;
  mutable std::unique_ptr<std::shared_mutex> lightDirectMutex_ =
      std::make_unique<std::shared_mutex>();
  std::shared_mutex *directResizeMutex() const noexcept {
    return (lowMemoryMode_ && lightService_) ? lightDirectMutex_.get() : nullptr;
  }
  static LightSectionView sectionToLightView(const Section &section,
                                             bool atomicHeader) noexcept;
  static LightSectionView directReadSection(const void *context, int chunkX,
                                            int chunkZ, int sectionY) noexcept;
  bool inWorldTick_ = false;
  mutable bool wireShouldSignal_ = true;
  PosMap<int> comparatorSignals_;
  std::deque<TorchToggle> recentTorchToggles_;
  std::vector<LightCellUpdate> pendingLightCells_;
  std::vector<std::uint64_t> pendingLightChunkKeys_;
  void flushPendingLightCells();

  friend class WorldChunkManager;
  friend struct detail::WorldBehaviours;
  friend void pistonForgetWorld(World &);
  bool installManagedChunk(
      int x, int z, Chunk &&chunk,
      const std::vector<ScheduledTick> &blockTicks,
      const std::vector<std::pair<ScheduledTick, FluidRef>> &fluidTicks,
      std::span<const std::shared_ptr<PistonMovingBlockEntity>> pistons = {},
      std::span<const std::shared_ptr<Entity>> itemEntities = {},
      std::span<const std::shared_ptr<Entity>> tntEntities = {});
  bool removeManagedChunk(int x, int z, std::string &error,
                          bool allowTransientDrop = false);
  bool managedChunkPersistenceState(int x, int z, std::string &reason) const;
  bool managedChunkTransientUnloadState(int x, int z,
                                        std::string &reason) const;
  std::function<void(int, int)> managedLightPublishedObserver;

  // 对应 `ServerLevel.blockEvents`（`ObjectLinkedOpenHashSet`）与重调度列表。
  struct BlockEventQueue;
  std::unique_ptr<BlockEventQueue> blockEventQueue_;
  std::vector<BlockEvent> blockEventsToReschedule_;
  std::int64_t nextSubTickCount();

  // `ServerExplosion.calculateExplodedPositions` 的逐方块探测记忆化缓存：
  // 爆炸射线壳层会反复穿过同一批方块坐标；缓存跨多次爆炸复用，并以递增的代次戳 `generation` 替代每次清零全表。
  struct ExplosionCacheEntry {
    std::uint64_t key = 0;
    std::uint32_t generation = 0;
    float resistance = 0.0f;
    bool air = true;
    bool fluid = false;
    bool loaded = false;
  };
  static constexpr std::size_t kExplosionCacheSize = 512;
  std::array<ExplosionCacheEntry, kExplosionCacheSize> explosionCache_{};
  std::uint32_t explosionGeneration_ = 0;
  // 复用爆炸目标位置暂存缓冲区，在多次爆炸之间保留已分配容量。
  std::vector<Pos> explosionTargets_;
};
} // namespace redstone
