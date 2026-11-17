#pragma once
// 区块加载票据系统、双距离传播图（Loading / Simulation）与需求源管理，
// 对齐原版 26.1 `TicketStorage`、`ChunkLevel`、`ChunkTracker` 与 `DistanceManager`。
// 将“区块应该加载到什么等级”的需求计算与实际磁盘 I/O（`chunk_lifecycle.hpp`）及 `World` 内存安装解耦。

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace redstone {

// 区块二维坐标 `(x, z)`，支持按 `(x, z)` 字典序排序及打包为原版 `ChunkPos.asLong()` 风格的 64 位键。
struct ChunkTicketPos {
  int x = 0;
  int z = 0;

  friend bool operator==(const ChunkTicketPos &, const ChunkTicketPos &) = default;
  friend auto operator<=>(const ChunkTicketPos &, const ChunkTicketPos &) = default;
  std::int64_t pack() const {
    return static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
        static_cast<std::uint32_t>(z));
  }
};

// 票据行为标志位（对应原版 `TicketType.Flags`）：控制持久化、加载图注入、模拟图注入、维度保活与卸载过期策略。
enum class TicketFlags : std::uint32_t {
  None = 0,
  Persist = 1,
  Loading = 2,
  Simulation = 4,
  KeepDimensionActive = 8,
  CanExpireIfUnloaded = 16,
};

constexpr std::uint32_t ticketFlag(TicketFlags flag) {
  return static_cast<std::uint32_t>(flag);
}
constexpr std::uint32_t operator|(TicketFlags left, TicketFlags right) {
  return ticketFlag(left) | ticketFlag(right);
}
constexpr std::uint32_t operator|(std::uint32_t left, TicketFlags right) {
  return left | ticketFlag(right);
}

// 票据类型描述符：定义静态超时 tick 数与标志位集合，决定同类票据对双图和维度的语义影响。
struct TicketType {
  std::string_view name;
  std::int64_t timeout = 0;
  std::uint32_t flags = 0;

  bool hasTimeout() const { return timeout != 0; }
  bool persist() const { return (flags & ticketFlag(TicketFlags::Persist)) != 0; }
  bool doesLoad() const { return (flags & ticketFlag(TicketFlags::Loading)) != 0; }
  bool doesSimulate() const {
    return (flags & ticketFlag(TicketFlags::Simulation)) != 0;
  }
  bool shouldKeepDimensionActive() const {
    return (flags & ticketFlag(TicketFlags::KeepDimensionActive)) != 0;
  }
  bool canExpireIfUnloaded() const {
    return (flags & ticketFlag(TicketFlags::CanExpireIfUnloaded)) != 0;
  }
  friend bool operator==(const TicketType &, const TicketType &) = default;
};

// 原版 26.1 内置票据类型注册表，flags 掩码与超时刻数严格对齐原版定义。
namespace ticket_types {
inline constexpr TicketType PlayerSpawn{"minecraft:player_spawn", 20, 2};
inline constexpr TicketType SpawnSearch{"minecraft:spawn_search", 1, 2};
inline constexpr TicketType Dragon{"minecraft:dragon", 0, 6};
inline constexpr TicketType PlayerLoading{"minecraft:player_loading", 0, 2};
inline constexpr TicketType PlayerSimulation{"minecraft:player_simulation", 0, 12};
inline constexpr TicketType Forced{"minecraft:forced", 0, 15};
inline constexpr TicketType Portal{"minecraft:portal", 300, 15};
inline constexpr TicketType EnderPearl{"minecraft:ender_pearl", 40, 14};
inline constexpr TicketType Unknown{"minecraft:unknown", 1, 18};
} // namespace ticket_types

// 挂载在某个区块坐标上的单张加载票据实例，持有类型、源等级 `level_` 与剩余寿命 `ticksLeft_`。
class Ticket {
public:
  Ticket(const TicketType &type, int level)
      : type_(type), level_(level), ticksLeft_(type.timeout) {}
  Ticket(const TicketType &type, int level, std::int64_t ticksLeft)
      : type_(type), level_(level), ticksLeft_(ticksLeft) {}

  const TicketType &type() const { return type_; }
  int level() const { return level_; }
  std::int64_t ticksLeft() const { return ticksLeft_; }
  void resetTicksLeft() { ticksLeft_ = type_.timeout; }
  void decreaseTicksLeft() {
    if (type_.hasTimeout())
      --ticksLeft_;
  }
  bool isTimedOut() const {
    return type_.hasTimeout() && ticksLeft_ < 0;
  }

private:
  TicketType type_;
  int level_;
  std::int64_t ticksLeft_;
};

// 区分两条独立的需求传播图：
// 玩家视野（viewDistance）只拉高 `Loading` 需求（加载并发送区块给客户端，但不 ticking），
// 而模拟距（simulationDistance）与强加载票据同时驱动 `Simulation` 需求（决定方块/实体刻是否执行）。
enum class TicketDemand {
  Loading,
  Simulation,
};

// `TicketStorage` 发出的与原版 26.1 一致的两类监听更新。`sourceLevel` 为经 `ChunkTracker` 截断前的
// 原始源等级（加载图与模拟图在无源时均为 45）。重复添加相同身份的票据只会续期，不会产生源等级更新。
struct TicketSourceChange {
  TicketDemand demand = TicketDemand::Loading;
  ChunkTicketPos position;
  int sourceLevel = 45;
  bool onlyDecreased = false;
};

enum class TicketOperationKind {
  Add,
  Remove,
};

struct TicketOperation {
  TicketOperationKind kind = TicketOperationKind::Add;
  ChunkTicketPos position;
  Ticket ticket;

  static TicketOperation add(ChunkTicketPos position, Ticket ticket) {
    return {TicketOperationKind::Add, position, std::move(ticket)};
  }
  static TicketOperation remove(ChunkTicketPos position, Ticket ticket) {
    return {TicketOperationKind::Remove, position, std::move(ticket)};
  }
};

struct TicketStorageChange {
  bool inserted = false;
  bool renewed = false;
  bool removed = false;
  bool changed = false;
  bool dirty = false;
  std::vector<TicketSourceChange> sourceChanges;
};

struct TicketHolderState {
  bool present = false;
  bool readyForSaving = false;
};
using TicketHolderLookup =
    std::function<TicketHolderState(const ChunkTicketPos &position)>;

// 区块票据存储中心：维护各区块上的活跃票据、关闭时停用的票据以及 `/forceload` 强制加载集合。
// 之所以集中管理并提供 `applyBatch`，是因为玩家移动或区域切换往往同时产生大量增删操作，
// 汇总后只输出净源等级变化可避免图传播产生无效的中间态震荡。
class TicketStorage {
public:
  bool addTicket(const ChunkTicketPos &position, Ticket ticket,
                 TicketStorageChange *out = nullptr);
  bool removeTicket(const ChunkTicketPos &position, const Ticket &ticket,
                    TicketStorageChange *out = nullptr);
  // 按顺序执行全部操作，然后仅报告每个位置最终的源等级净差异。
  // 若一批操作后某位置的源等级回到原值，即使中途续期了票据对象，也不会触发传播图更新。
  TicketStorageChange applyBatch(std::span<const TicketOperation> operations);

  const std::vector<Ticket> &getTickets(const ChunkTicketPos &position) const;
  const std::vector<Ticket> &
  getDeactivatedTickets(const ChunkTicketPos &position) const;
  int getTicketLevelAt(const ChunkTicketPos &position,
                       TicketDemand demand) const;

  bool hasTickets() const { return !tickets_.empty(); }
  bool shouldKeepDimensionActive() const;
  bool hasForcedTicket(const ChunkTicketPos &position) const;

  // 每次调用执行一轮原版 26.1 的超时清理。若对应区块 holder 存在但尚未准备好保存，
  // 对未设置 `CanExpireIfUnloaded` 标志的票据会暂停倒计时（避免正在刷盘或过渡中的区块过早失活）。
  TicketStorageChange purgeTimedTickets(const TicketHolderLookup &holders);

  TicketStorageChange deactivateTicketsOnClosing();
  TicketStorageChange activateAllDeactivatedTickets();
  TicketStorageChange replaceTicketLevelOfType(const TicketType &type,
                                                int newLevel);

  std::vector<std::pair<ChunkTicketPos, Ticket>>
  persistentTickets() const;
  // 仅用于启动阶段恢复持久化票据。每条输入直接追加而不做身份去重或超时续期，
  // 并在末尾一次性为两张传播图生成汇总源差异。
  TicketStorageChange restorePersistentTickets(
      std::span<const std::pair<ChunkTicketPos, Ticket>> tickets);

private:
  using TicketMap = std::map<ChunkTicketPos, std::vector<Ticket>>;

  static int getTicketLevelAt(const std::vector<Ticket> &tickets,
                              TicketDemand demand);
  static bool sameIdentity(const Ticket &left, const Ticket &right);

  TicketStorageChange removeIf(
      const std::function<bool(Ticket &, const ChunkTicketPos &)> &predicate,
      TicketMap *removedOutput = nullptr);
  void updateForcedChunks();

  TicketMap tickets_;
  TicketMap deactivatedTickets_;
  std::set<ChunkTicketPos> forcedChunks_;
};

// 完整区块的活跃状态阶梯（与原版 `FullChunkStatus` 一致）：等级数字越小，活跃度越高。
enum class FullChunkStatus {
  Inaccessible,
  Full,
  BlockTicking,
  EntityTicking,
};

// 世界生成/加载阶段枚举：覆盖从空占位到最终 `Full` 的 12 个原版阶段（对应等级 33..44）。
enum class GenerationStatus {
  Empty,
  StructureStarts,
  StructureReferences,
  Biomes,
  Noise,
  Surface,
  Carvers,
  Features,
  InitializeLight,
  Light,
  Spawn,
  Full,
};

// 原版区块等级常量与映射工具：
// 数值越小优先级越高（31 实体刻 -> 32 方块刻 -> 33 完整区块 -> 34..44 外围生成缓冲 -> 45 无源）。
// 票据向 8 邻域扩散时每格等级 +1，使核心加载区块周围自然形成满足邻居依赖（光照/结构/特征）的缓冲环。
namespace ticket_levels {
inline constexpr int FullChunkLevel = 33;
inline constexpr int BlockTickingLevel = 32;
inline constexpr int EntityTickingLevel = 31;
inline constexpr int RadiusAroundFullChunk = 11;
inline constexpr int MaxLevel = FullChunkLevel + RadiusAroundFullChunk;
inline constexpr int NoTicketSource = MaxLevel + 1;
inline constexpr int SimulationTrackerMax = 33;

constexpr bool isLoaded(int level) { return level <= MaxLevel; }
constexpr bool isBlockTicking(int level) {
  return level <= BlockTickingLevel;
}
constexpr bool isEntityTicking(int level) {
  return level <= EntityTickingLevel;
}
constexpr FullChunkStatus fullStatus(int level) {
  if (level <= EntityTickingLevel)
    return FullChunkStatus::EntityTicking;
  if (level <= BlockTickingLevel)
    return FullChunkStatus::BlockTicking;
  if (level <= FullChunkLevel)
    return FullChunkStatus::Full;
  return FullChunkStatus::Inaccessible;
}
std::optional<GenerationStatus> generationStatus(int level);
int generationLevel(GenerationStatus status);
const char *generationStatusName(GenerationStatus status);
const char *fullStatusName(FullChunkStatus status);
} // namespace ticket_levels

struct ChunkLevelChange {
  ChunkTicketPos position;
  int oldLevel = 0;
  int newLevel = 0;

  bool decreased() const { return newLevel < oldLevel; }
};

// 仅限主线程运行的区块等级传播图（对应 `ChunkTracker`）：
// 源节点由票据系统断言给定；每条边连接切比雪夫距离为 1 的八个周围区块且传播代价为 +1 等级。
// 仅当存在源变化时执行局部增量重算；在无源变化时不会扫描全图。
class ChunkLevelGraph {
public:
  explicit ChunkLevelGraph(TicketDemand demand);

  TicketDemand demand() const { return demand_; }
  int noSourceLevel() const;
  int storedBelow() const;
  int levelAt(const ChunkTicketPos &position) const;
  std::optional<int> sourceLevel(const ChunkTicketPos &position) const;

  std::vector<ChunkLevelChange>
  applySourceChanges(std::span<const TicketSourceChange> changes);
  std::uint64_t recomputeCount() const { return recomputeCount_; }
  std::size_t lastRecomputedCells() const { return lastRecomputedCells_; }
  std::size_t lastChangedCells() const { return lastChangedCells_; }
  std::size_t storedNodeCount() const { return levels_.size(); }

private:
  TicketDemand demand_;
  std::map<ChunkTicketPos, int> sources_;
  std::map<ChunkTicketPos, int> levels_;
  std::uint64_t recomputeCount_ = 0;
  std::size_t lastRecomputedCells_ = 0;
  std::size_t lastChangedCells_ = 0;
};

enum class ChunkDataResidency {
  // 当前仅建模“需求”（demand）本身。`holderFullStatus` 与 ticking 投影
  // 并不意味着区块磁盘数据已经驻留或就绪（实际驻留由 `ChunkLifecycleController` 跟踪）。
  NotTracked,
};

struct ChunkDemandSnapshot {
  int loadingLevel = ticket_levels::NoTicketSource;
  int simulationLevel = ticket_levels::SimulationTrackerMax;
  bool schedulingScope = false;
  FullChunkStatus holderFullStatus = FullChunkStatus::Inaccessible;
  std::optional<GenerationStatus> generationStatus;
  bool blockTickingDemand = false;
  bool entityTickingDemand = false;
  ChunkDataResidency dataResidency = ChunkDataResidency::NotTracked;
};

struct TicketSystemUpdate {
  TicketStorageChange storage;
  std::vector<ChunkLevelChange> loadingChanges;
  std::vector<ChunkLevelChange> simulationChanges;

  bool changed() const {
    return storage.changed || !loadingChanges.empty() ||
           !simulationChanges.empty();
  }
};

// 主线程票据等级组合系统：把 `TicketStorage` 与 `Loading` / `Simulation` 两张传播图绑定为单一原子状态。
// 它刻意不持有 `World`、`ChunkHolder`、磁盘或 ticking 钩子：`holderFullStatus` 只是票据等级投影，
// 并非实际数据驻留状态，使需求传播逻辑能够独立验证与测试。
class TicketLevelSystem {
public:
  bool addTicket(const ChunkTicketPos &position, Ticket ticket,
                 TicketSystemUpdate *out = nullptr);
  bool removeTicket(const ChunkTicketPos &position, const Ticket &ticket,
                    TicketSystemUpdate *out = nullptr);
  TicketSystemUpdate applyBatch(std::span<const TicketOperation> operations);
  TicketSystemUpdate restorePersistentTickets(
      std::span<const std::pair<ChunkTicketPos, Ticket>> tickets);
  TicketSystemUpdate purgeTimedTickets(const TicketHolderLookup &holders);
  TicketSystemUpdate deactivateTicketsOnClosing();
  TicketSystemUpdate activateAllDeactivatedTickets();
  TicketSystemUpdate replaceTicketLevelOfType(const TicketType &type,
                                               int newLevel);

  ChunkDemandSnapshot query(const ChunkTicketPos &position) const;
  const TicketStorage &storage() const { return storage_; }
  const ChunkLevelGraph &loadingGraph() const { return loading_; }
  const ChunkLevelGraph &simulationGraph() const { return simulation_; }

private:
  TicketSystemUpdate applyStorageChange(TicketStorageChange change);

  TicketStorage storage_;
  ChunkLevelGraph loading_{TicketDemand::Loading};
  ChunkLevelGraph simulation_{TicketDemand::Simulation};
};

// 判定在线玩家是否应为周围区块贡献加载/模拟需求（仅在 Playing 且未死亡/未断线时有效）。
constexpr bool ticketPlayerDemandActive(bool sessionPlaying, bool dead,
                                        bool closing, bool closed) {
  return sessionPlaying && !dead && !closing && !closed;
}

using PlayerTicketId = std::uint64_t;

struct PlayerTicketSource {
  ChunkTicketPos center;
};

struct TicketSourceUpdate {
  TicketSystemUpdate tickets;
  bool stateChanged = false;
  std::size_t loadingPresenceChanges = 0;
  std::size_t simulationPresenceChanges = 0;
};

// 主线程需求源记账管理器（对应原版 `DistanceManager` 中的玩家区域与强加载源跟踪）。
// 维护每个区块被多少玩家视距/模拟距覆盖的引用计数（`loadingCoverage_` / `simulationCoverage_`），
// 仅在覆盖数从 0->1 或 1->0 时向 `TicketLevelSystem` 发射批量票据变更，避免多玩家重叠移动时重复操作图。
// 本类只负责计算票据需求，不直接加载区块、搜索出生点、创建异步任务或驱动 tick 门控。
class TicketSourceManager {
public:
  static constexpr int MinViewDistance = 2;
  static constexpr int MaxViewDistance = 32;
  static constexpr int PlayerLoadingLevel = 31;
  static constexpr int PlayerSpawnRadius = 3;
  static constexpr int SpawnSearchRadius = 0;

  TicketSourceManager(int viewDistance = 3, int simulationDistance = 10);

  int viewDistance() const { return viewDistance_; }
  int simulationDistance() const { return simulationDistance_; }
  int playerSimulationLevel() const;
  const std::map<PlayerTicketId, PlayerTicketSource> &players() const {
    return players_;
  }
  std::size_t loadingCoverage(const ChunkTicketPos &position) const;
  std::size_t simulationCoverage(const ChunkTicketPos &position) const;

  TicketSourceUpdate addPlayer(PlayerTicketId id, ChunkTicketPos center);
  TicketSourceUpdate movePlayer(PlayerTicketId id, ChunkTicketPos center);
  // 精确存在性对账：在线则添加或移动，离线则移除。可在服务端每轮主循环中幂等调用。
  TicketSourceUpdate setPlayer(PlayerTicketId id, bool present,
                               ChunkTicketPos center);
  TicketSourceUpdate removePlayer(PlayerTicketId id);
  TicketSourceUpdate setViewDistance(int viewDistance);
  TicketSourceUpdate setSimulationDistance(int simulationDistance);

  TicketSystemUpdate setForced(const ChunkTicketPos &position, bool forced);
  TicketSystemUpdate restorePersistentTickets(
      std::span<const std::pair<ChunkTicketPos, Ticket>> tickets);
  TicketSystemUpdate requestPlayerSpawn(const ChunkTicketPos &center);
  TicketSystemUpdate requestSpawnSearch(const ChunkTicketPos &center);
  TicketSystemUpdate tick(const TicketHolderLookup &holders = {});

  TicketLevelSystem &tickets() { return tickets_; }
  const TicketLevelSystem &tickets() const { return tickets_; }
  ChunkDemandSnapshot query(const ChunkTicketPos &position) const {
    return tickets_.query(position);
  }

private:
  static bool inSquare(const ChunkTicketPos &center,
                       const ChunkTicketPos &position, int radius);
  TicketSourceUpdate applyPlayerSources(std::vector<TicketOperation> operations,
                                        bool stateChanged,
                                        std::size_t loadingChanges,
                                        std::size_t simulationChanges);
  std::map<ChunkTicketPos, std::size_t> rebuildLoadingCoverage(int radius);

  int viewDistance_;
  int simulationDistance_;
  TicketLevelSystem tickets_;
  std::map<PlayerTicketId, PlayerTicketSource> players_;
  std::map<ChunkTicketPos, std::size_t> loadingCoverage_;
  std::map<ChunkTicketPos, std::size_t> simulationCoverage_;
  std::set<ChunkTicketPos> forced_;
};

} // namespace redstone
