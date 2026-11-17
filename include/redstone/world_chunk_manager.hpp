#pragma once
// 受管区块生命周期编排器（WorldChunkManager）：负责加载票驱动的异步加载/淘汰、
// 增量脏追踪与非阻塞异步自动保存、tick 就绪状态判定以及跨区块实体迁移交接。
// 存在原因：在不破坏 World 原有离线 loadChunk/unloadChunk 行为的前提下，
// 为在线服务端提供接近原版 ServerChunkCache/ChunkMap 的驻留与安全持久化协调层。

#include "redstone/chunk_lifecycle.hpp"
#include "redstone/engine.hpp"
#include "redstone/tickets.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <map>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace redstone {

// 单个受管区块的聚合观测视图，汇总生命周期状态、光照状态、保存阻塞原因与错误文本。
struct ManagedChunkInfo {
  ChunkLifecycleStatus lifecycle;
  std::optional<LightService::ChunkStatus> light;
  std::string blocker;
  std::string error;
};

// 关服排空时未能成功保存的区块诊断信息，供上层决定告警或人工介入。
struct ChunkShutdownIssue {
  ChunkTicketPos position;
  ChunkResidencyState state = ChunkResidencyState::Absent;
  std::uint64_t dirtyVersion = 0;
  std::uint64_t savedDirtyVersion = 0;
  std::string error;
};

// 显式 close() 的汇总结果，区分正常排空与未落盘区块列表。
struct ChunkShutdownResult {
  bool complete = false;
  bool workerStopped = false;
  std::vector<ChunkTicketPos> savedChunks;
  std::vector<ChunkShutdownIssue> unsavedChunks;
  std::string fatalError;
};

// 在线积极保存（eager-save）限额配置。队列每真实服务端刻最多检查指定数量的候选
// 区块并提交上限数量的写盘任务，绝不阻塞等待 I/O 工作线程，同时限制总在途写盘数量。
// 保存成功的区块进入原版 10 秒冷却期；失败请求则在退避后重新尝试其保留补丁。
struct AutosaveLimits {
  std::size_t maxCandidateChecks = 64;
  std::size_t maxSubmissions = 4;
  std::size_t maxInFlightSaves = 32;
  std::chrono::milliseconds candidateTimeBudget{2};
  std::chrono::milliseconds cooldown{10000};
  std::chrono::milliseconds failureBackoff{1000};
};

// 记录最近一次在线自动保存与单次 pump() 的调度统计，便于测试与背压观测。
struct AutosaveStats {
  std::size_t candidateChecks = 0;
  std::size_t submissions = 0;
  std::size_t inFlight = 0;
  std::size_t queued = 0;
};

struct PumpSaveStats {
  std::size_t candidateChecks = 0;
  std::size_t submissions = 0;
  std::size_t inFlight = 0;
};

enum class EntityChunkDataStatus {
  Empty,
  NonEmpty,
  Unsupported,
  Error,
  Unavailable,
};


// 描述区块当前的 tick 就绪度：区分加载等级、模拟等级以及方块刻/实体刻放行条件，
// 确保尚未加载完实体数据或等级不足的区块不会提前推进计划刻或实体。
struct ChunkTickReadiness {
  int loadingLevel = ticket_levels::NoTicketSource;
  int simulationLevel = ticket_levels::SimulationTrackerMax;
  bool blockTicking = false;
  bool entityTicking = false;
  bool entityDataReady = false;
  std::string limitation;
};

// 可选接入的 T-4b-1 生命周期管理器；未构造此对象时，World 仍保持原有离线
// loadChunk/unloadChunk 行为。
//
// 当前受支持的 FULL 区块可携带地形、方块/流体计划刻、活动移动活塞方块实体以及
// 当前支持的物品实体类型；其他已保存方块实体与实体类型仍会被拒绝。物品跨区块
// 移动通过按 UUID 维护的紧凑交接账本协调，确保目标区块持久化落盘确认后，才会
// 清理旧源区块的实体列表。
class WorldChunkManager {
public:
  using EntityUuid = std::array<std::int32_t, 4>;
  static constexpr std::size_t compactEditBytes =
      sizeof(terrain::CompactBlockEdit);
  WorldChunkManager(World &world, std::filesystem::path regionDirectory,
                    int viewDistance = 3, int simulationDistance = 10,
                    EntityChunkDataProbe entityProbe = {},
                    std::shared_ptr<RawEntityStore> entityStore = {});
  WorldChunkManager(World &world, std::shared_ptr<RawChunkStore> store,
                    int viewDistance = 3, int simulationDistance = 10,
                    EntityChunkDataProbe entityProbe = {},
                    std::shared_ptr<RawEntityStore> entityStore = {});
  ~WorldChunkManager() noexcept;

  WorldChunkManager(const WorldChunkManager &) = delete;
  WorldChunkManager &operator=(const WorldChunkManager &) = delete;

  TicketSourceManager &ticketSources() { return tickets_; }
  const TicketSourceManager &ticketSources() const { return tickets_; }

  // 各类加载票来源更新接口：同步修改 TicketSourceManager 并将等级差异转发至生命周期控制器。
  TicketSourceUpdate setPlayer(PlayerTicketId id, bool present,
                               ChunkTicketPos center);
  TicketSourceUpdate addPlayer(PlayerTicketId id, ChunkTicketPos center);
  TicketSourceUpdate movePlayer(PlayerTicketId id, ChunkTicketPos center);
  TicketSourceUpdate removePlayer(PlayerTicketId id);
  TicketSourceUpdate setViewDistance(int distance);
  TicketSourceUpdate setSimulationDistance(int distance);
  TicketSystemUpdate setForced(const ChunkTicketPos &position, bool forced);
  TicketSystemUpdate addPortalTicket(const ChunkTicketPos &position,
                                     int radius = 3);
  TicketSystemUpdate addEnderPearlTicket(const ChunkTicketPos &position,
                                          int radius = 2);
  // 启动阶段批量恢复：按精确的票据标识与剩余刻数安装所有校验通过的持久化加载票，
  // 并将加载等级变化转发给生命周期控制器。
  TicketSystemUpdate restorePersistentTickets(
      std::span<const std::pair<ChunkTicketPos, Ticket>> tickets);

  void applyTicketUpdate(const TicketSystemUpdate &update);
  // 排空一批已完成的加载/保存任务，推送光照发布通知，并启动待处理的脏块保存或
  // 已释放区块的淘汰。此方法绝不阻塞等待 I/O。
  void pump();
  // 推进一个真实服务端游戏刻。加载票超时衰减仅在此处发生，对齐原版 ServerChunkCache
  // 的 runsNormally/tickChunks 条件；pump() 永不老化加载票。
  void serverTick(bool tickChunks = true, bool runsNormally = true);

  // 通过 World 进行的所有写入仍正常工作。管理器会观察成功的 World::setBlock/rawSetBlock
  // 修改（包含嵌套写入），并仅记录落入受管区块中的变更。
  // 主线程受管区块回调：install 与 evict 均在对应的 World 驻留状态变更之后触发，
  // 两者均不执行磁盘 I/O。
  std::function<void(const ChunkTicketPos &)> managedChunkInstalled;
  std::function<void(const ChunkTicketPos &)> managedChunkRemoved;
  std::vector<std::pair<int, int>> managedChunks() const;
  const AutosaveStats &lastAutosaveStats() const { return autosaveStats_; }
  const PumpSaveStats &lastPumpSaveStatsForTest() const {
    return pumpSaveStats_;
  }
  std::uint64_t tickTimeEntriesVisitedForTest() const {
    return tickTimeEntriesVisited_;
  }
  std::size_t pendingEditEntriesForTest(
      const ChunkTicketPos &position) const {
    const auto found = managed_.find(position);
    return found == managed_.end() ? 0 : found->second.pendingEdits.size();
  }
  std::size_t pendingEditPayloadBytesForTest(
      const ChunkTicketPos &position) const {
    return pendingEditEntriesForTest(position) * compactEditBytes;
  }
  std::size_t retainedPatchEntriesForTest(
      const ChunkTicketPos &position) const {
    const auto found = managed_.find(position);
    return found != managed_.end() && found->second.retainedPatch
               ? found->second.retainedPatch->size()
               : 0;
  }
  std::size_t retainedPatchPayloadBytesForTest(
      const ChunkTicketPos &position) const {
    return retainedPatchEntriesForTest(position) * compactEditBytes;
  }
  std::size_t inFlightPatchEntriesForTest(
      const ChunkTicketPos &position) const {
    const auto found = managed_.find(position);
    return found != managed_.end() && found->second.inFlightPatch
               ? found->second.inFlightPatch->size()
               : 0;
  }
  void setAutosaveLimitsForTest(AutosaveLimits limits) {
    autosaveLimits_ = std::move(limits);
  }
  bool setBlock(Pos pos, BlockState state, int flags = 3, int limit = 512);
  void rawSetBlock(Pos pos, BlockState state);
  // 手动重新排入保存失败的受管区块，尝试恢复落盘。
  bool retrySave(const ChunkTicketPos &position);
  std::optional<ManagedChunkInfo>
  managedStatus(const ChunkTicketPos &position) const;
  ChunkTickReadiness tickReadiness(const ChunkTicketPos &position) const;
  bool isManaged(const ChunkTicketPos &position) const;
  std::optional<ChunkLifecycleStatus>
  lifecycleStatus(const ChunkTicketPos &position) const;

  // 显式关服流程可等待已接收的 I/O 完成。会先拒绝新需求与迟到的加载安装，随后
  // 保存所有脏的受管区块（包括仍持有加载票的区块）。存在保存失败或阻塞的区块会
  // 保持驻留并在 unsavedChunks 中返回。常规 pump() 仍保持非阻塞。
  ChunkShutdownResult close();
  const ChunkShutdownResult &lastShutdownResult() const {
    return lastShutdownResult_;
  }
  // 测试可见的所有权检查：每次读取的实体元数据在失败、过期或完成后必须归零；
  // 就绪度缓存也仅限于当前驻留的受管中心区块，而非保留所有历史访问过的邻居记录。
  std::size_t entityReadMetadataCountForTest() const;
  std::size_t readinessCacheSizeForTest() const { return readiness_.size(); }
  std::uint64_t migrationWorkVisitedForTest() const {
    return migrationWorkVisited_;
  }
  std::size_t migrationWorkQueuedForTest() const {
    return entityMigrationWork_.size();
  }
  std::size_t entityPositionLedgerSizeForTest() const {
    return entityLastPositions_.size();
  }
  std::size_t lifecycleCoordinateCountForTest() const {
    return managed_.size();
  }
  bool accepting() const {
    return !closing_.load(std::memory_order_acquire) &&
           !closed_.load(std::memory_order_acquire);
  }

private:
  struct SaveCandidate {
    ChunkTicketPos position;
    std::uint64_t epoch = 0;
  };

  // 单个驻留受管区块的增量修改与保存状态机。
  // 存在原因：通过 epoch、dirtyVersion、pendingEdits 与 retainedPatch 将主线程模拟修改
  // 与后台 I/O 快照解耦，避免保存期间的新写入丢失或阻塞主线程。
  struct ManagedChunk {
    std::uint64_t epoch = 0;
    std::uint64_t dirtyVersion = 0;
    std::uint64_t inFlightVersion = 0;
    std::uint64_t failedVersion = 0;
    std::int64_t lastObservedGameTime = 0;
    std::int64_t transientEventRetryTime = 0;
    bool transientEventRetryScheduled = false;
    bool entityDataReady = false;
    bool entitySlotPresent = false;
    std::string entityDataDetail;
    // 压缩的 sectionY/localIndex 键 -> 进程内稳定的 BlockState ID。
    std::map<std::uint32_t, BlockState::Id> pendingEdits;
    ChunkLifecycleController::ChunkEditPatch retainedPatch;
    ChunkLifecycleController::ChunkEditPatch inFlightPatch;
    std::chrono::steady_clock::time_point nextSaveAttempt{};
    bool cooldownScheduled = false;
    bool dynamicBlocked = false;
    std::set<int> writableSections;
    std::string blocker;
    bool initialLightComplete = false;
    bool initialLightBaselineRecorded = false;
    bool needsLightSave = false;
    std::uint64_t savedBlockLightRevision = 0;
    std::uint64_t savedSkyLightRevision = 0;
  };

  // 回调绑定与世界驻留安装/淘汰（在加载安装和卸载阶段协调 World 与控制器状态）。
  void initializeCallbacks();
  void installWorldCallbacks();
  void detachWorldCallbacks();
  bool installLoaded(LifecycleLoadedChunk &&loaded);
  bool evictLoaded(const ChunkTicketPos &position, std::uint64_t epoch);
  void recordBlockMutation(Pos pos, const BlockState &oldState,
                           const BlockState &newState);
  void recordTickMutation(Pos pos);
  void recordPistonMutation(Pos pos);
  void recordLightPublished(int chunkX, int chunkZ);
  void processLightSaveCompletions();

  // 跨区块实体迁移交接账本：跟踪目标区块确认写盘前不得清理的源区块清单，
  // 防止异步存盘乱序造成物品实体复制或永久丢失。
  struct EntityMigration {
    std::optional<ChunkTicketPos> target;
    std::uint64_t targetEpoch = 0;
    std::uint64_t targetVersion = 0;
    bool targetAcknowledged = false;
    bool removalObserved = false;
    std::set<ChunkTicketPos> clearSources;
    std::map<ChunkTicketPos, std::pair<std::uint64_t, std::uint64_t>>
        sourceVersions;
  };

  void recordEntityMutation(
      Vec3 oldPosition, Vec3 newPosition,
      const EntityUuid &uuid, bool removed);
  void noteEntityMigrationSave(
      const ChunkTicketPos &position, std::uint64_t epoch,
      std::uint64_t version,
      const std::set<EntityUuid> &presentUuids);
  void entityMigrationSaveCompleted(const ChunkTicketPos &position,
                                    std::uint64_t version, bool success);
  void setMigrationTarget(const EntityUuid &uuid, EntityMigration &migration,
                          const ChunkTicketPos &target);
  void addMigrationSource(const EntityUuid &uuid,
                          EntityMigration &migration,
                          const ChunkTicketPos &source);
  void removeMigrationSource(const EntityUuid &uuid,
                             EntityMigration &migration,
                             const ChunkTicketPos &source);
  void eraseMigration(const EntityUuid &uuid);
  void enqueueMigrationWork(const EntityUuid &uuid);
  void processMigrationWork(std::size_t maxEntries);
  bool entityMigrationAllowsSave(const ChunkTicketPos &position) const;
  bool entityMigrationPinned(const ChunkTicketPos &position) const;
  // 异步保存队列调度、冷却退避与阻塞检测（区分可重试瞬态阻塞与永久持久化阻塞）。
  void observeTickTime();
  void processSaveCompletions();
  void processDemandSaveCandidates(std::size_t maxEntries);
  void processEvictionRetries(std::size_t maxEntries);
  bool processAutosaveCandidate(const ChunkTicketPos &position,
                                std::chrono::steady_clock::time_point now);
  void processAutosaveBudget();
  void processNoDemandSaveBudget(std::size_t maxEntries);
  void enqueueDynamicRetry(const ChunkTicketPos &position);
  void reclassifyDynamicRetry(const ChunkTicketPos &position);
  void reclassifyCooldown(const ChunkTicketPos &position);
  void enqueueDemandSaveCandidate(const ChunkTicketPos &position);
  void enqueueEvictionRetry(const ChunkTicketPos &position);
  void enqueueAutosaveCandidate(const ChunkTicketPos &position);
  void deferSaveCandidate(const ChunkTicketPos &position,
                          std::chrono::milliseconds delay);
  void noteDynamicBlocker(const ChunkTicketPos &position,
                          std::string reason);
  bool scheduleSave(const ChunkTicketPos &position, ManagedChunk &chunk,
                    bool forceRetry);
  bool persistenceBlocker(const ChunkTicketPos &position,
                          std::string &reason) const;
  bool transientUnloadBlocker(const ChunkTicketPos &position,
                              std::string &reason) const;
  // 根据加载等级、模拟等级与实体数据就绪状态重算并缓存邻居范围的 tick 就绪度。
  ChunkTickReadiness computeReadiness(const ChunkTicketPos &position) const;
  void refreshReadinessAround(const ChunkTicketPos &position);
  void refreshReadinessForChanges(std::span<const ChunkLevelChange> loading,
                                  std::span<const ChunkLevelChange> simulation);

  // 在未单独提供 RawEntityStore 时，于地形读盘阶段同步探查实体区块可读性的包装存储。
  class EntityReadStore;
  World &world_;
  TicketSourceManager tickets_;
  std::shared_ptr<RawChunkStore> terrainStore_;
  std::shared_ptr<RawEntityStore> entityStore_;
  std::unique_ptr<ChunkLifecycleController> controller_;
  std::map<ChunkTicketPos, ManagedChunk> managed_;
  // 由驻留代际共享的单调生命周期令牌。不按坐标保留历史标记；失败的安装仅在
  // 本地消耗一个令牌，不会创建账本条目。
  std::uint64_t nextLightLifecycleEpoch_ = 1;
  std::map<ChunkTicketPos, ChunkTickReadiness> readiness_;
  std::map<ChunkTicketPos, std::string> errors_;
  std::deque<SaveCandidate> autosaveCandidates_;
  std::set<ChunkTicketPos> autosaveQueued_;
  std::deque<SaveCandidate> demandSaveCandidates_;
  std::set<ChunkTicketPos> demandSaveQueued_;
  std::deque<SaveCandidate> evictionRetries_;
  std::set<ChunkTicketPos> evictionRetryQueued_;
  std::set<ChunkTicketPos> savingInFlight_;
  std::deque<SaveCandidate> heldDynamicRetries_;
  std::set<ChunkTicketPos> heldDynamicQueued_;
  std::deque<SaveCandidate> noDemandDynamicRetries_;
  std::set<ChunkTicketPos> noDemandDynamicQueued_;
  std::set<ChunkTicketPos> pendingLightSave_;
  std::set<ChunkTicketPos> lightNotificationQueue_;
  std::set<ChunkTicketPos> tickTimeSensitive_;
  std::map<EntityUuid, EntityMigration> entityMigrations_;
  std::map<ChunkTicketPos, std::set<EntityUuid>> entityMigrationTargets_;
  std::map<ChunkTicketPos, std::set<EntityUuid>> entityMigrationSources_;
  std::map<EntityUuid, Vec3> entityLastPositions_;
  std::deque<EntityUuid> entityMigrationWork_;
  std::set<EntityUuid> entityMigrationWorkQueued_;
  std::set<std::pair<std::chrono::steady_clock::time_point,
                     ChunkTicketPos>>
      heldSaveCooldowns_;
  std::set<std::pair<std::chrono::steady_clock::time_point,
                     ChunkTicketPos>>
      noDemandSaveCooldowns_;
  AutosaveLimits autosaveLimits_;
  AutosaveStats autosaveStats_;
  PumpSaveStats pumpSaveStats_;
  bool preferHeldDynamic_ = false;
  std::int64_t lastObservedTickTime_ =
      std::numeric_limits<std::int64_t>::min();
  std::uint64_t tickTimeEntriesVisited_ = 0;
  std::uint64_t migrationWorkVisited_ = 0;
  std::atomic<bool> closing_{false};
  std::atomic<bool> closed_{false};
  ChunkShutdownResult lastShutdownResult_;
};

} // namespace redstone
