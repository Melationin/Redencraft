#pragma once
// 统一区块资源管理器（`ChunkMap`）、单区块生命周期槽位（`ChunkHolder`）与运行态区块（`WorldChunk`）。
// 合并原 `World::chunks_` 与 `WorldGenScheduler` 的双轨分片表：
// - 多线程主表 `holders_`（64 分片并发哈希表）：`u64 -> std::shared_ptr<ChunkHolder>`
// - 主线程快表 `worldIndex_`（无锁扁平哈希表）：`u64 -> WorldChunk*`
// - 成员 `scheduler_`：按需启用的多线程世界生成调度器，直接共享同一张 `holders_` 表。

#include "parallel_hashmap/phmap.h"
#include "redstone/chunk_section.hpp"
#include "redstone/heightmap.hpp"
#include "redstone/light/types.hpp"
#include "redstone/poi.hpp"
#include "redstone/worldgen/chunk_status.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <vector>

namespace redstone {

class ChunkMap;
class LightService;
class RawChunkStore;
class World;

namespace worldgen {
class ChunkGenerator;
class GeneratedLightQueue;
class LightOpticalContext;
class WorldGenScheduler;
struct WorldGenSchedulerConfig {
  std::size_t threadCount = 0; // 0 = auto hardware_concurrency()
  int colorTurnDispatchLimit = 64; // kept for API compatibility (unused by spatial lock)
  std::size_t maxInFlightResidency = 0; // 0 = auto clamp(threadCount*3, 16, 64)
  std::size_t maxInFlightBuffer = 0;    // 0 = auto clamp(threadCount*2, 8, 32)
  std::size_t batchWavefrontLimit = 0;  // 0 = auto clamp(threadCount*4, 32, 128)
  bool asyncWorldGenLight = true;       // true = non-blocking InitializeLight/Light -> LightService
};
} // namespace worldgen

inline std::uint64_t packChunkKey(int x, int z) noexcept {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
         static_cast<std::uint32_t>(z);
}

inline ChunkTicketPos unpackChunkKey(std::uint64_t key) noexcept {
  return ChunkTicketPos{
      static_cast<int>(static_cast<std::int32_t>(key >> 32)),
      static_cast<int>(static_cast<std::int32_t>(key & 0xffffffffu))};
}

// 运行态完整区块（原 `World::Chunk`）：方块区段与生物群系继承自 `BaseChunk`，
// 仅追加主循环模拟、随机刻索引、高度图与持久化光照元数据。
struct WorldChunk : public BaseChunk {
  static constexpr std::uint32_t kInvalidRandomTickIndex =
      std::numeric_limits<std::uint32_t>::max();

  WorldChunk() : BaseChunk(ChunkKind::World) {}

  bool loaded = true;
  bool hasPendingLightCell = false;
  std::uint32_t randomTickingSectionMask = 0;
  std::uint32_t randomTickIndex = kInvalidRandomTickIndex;
  // 当前已加载生命周期的 `LightService` 代数令牌；同一地址重载区块会递增它，
  // 从而安全丢弃队列中旧生命周期的单元更新。
  std::uint64_t lightLifecycleEpoch = 1;
  // 主线程在该区块上执行光学方块写入后产生的最新 `LightService` 序列号。
  std::uint64_t requiredLightGeneration = 0;
  std::optional<PersistedChunkLight> persistedLight;
  std::array<std::int16_t, kSectionCapacity> persistedLightSectionIndex{};
  std::unique_ptr<ChunkHeightmaps> heightmaps;

  void refreshRandomTickMask(int count);
  void refreshPersistedLightIndex(int minSectionY, int numSections);
  int getHeight(HeightmapType type, int x, int z) const;
};

enum class HeldChunkKind : std::uint8_t {
  Empty = 0,
  Proto = 1,
  World = 2,
};

// 单区块生命周期控制器：用标签 + 联合体持有 `ProtoChunk` 或 `WorldChunk`，
// 统一承担 `ProtoChunk -> WorldChunk` 转换、生成调度状态与失效广播。
class ChunkHolder {
public:
  enum class ExecState : std::uint8_t {
    Idle = 0,
    WaitingDeps = 1,
    Queued = 2,
    Running = 3,
  };

  struct GenTaskState {
    enum class InitState : std::uint8_t {
      Uninitialized = 0,
      Initializing = 1,
      Ready = 2,
    };

    struct WaiterRef {
      std::shared_ptr<ChunkHolder> holder;
      std::uint32_t armSeq = 0;
    };

    struct PinnedDep {
      std::shared_ptr<ChunkHolder> holder;
      std::shared_ptr<BaseChunk> chunk;
      std::uint8_t waitedStage = 0;
      std::int8_t dx = 0;
      std::int8_t dz = 0;
    };

    std::mutex mtx;
    std::uint8_t currentStage = 0;
    std::uint8_t initialPersistedStage = 0;
    std::uint8_t explicitTarget = 0;
    std::uint8_t dependentTarget = 0;
    std::uint8_t pendingStage = 0;
    std::uint8_t pendingWriteRadius = 0;
    std::uint8_t distBucket = 32;
    ExecState execState = ExecState::Idle;
    InitState initState = InitState::Uninitialized;
    bool fromLoaded = false;
    bool dependencyFailed = false;
    bool isActive = false;
    bool residencyPath = false;
    bool arming = false;
    std::uint32_t initWaiters = 0;
    std::uint32_t generationId = 0;
    std::uint32_t armSeq = 0;
    std::uint32_t satisfiedDeps = 0;
    std::uint64_t firstArmedNs = 0;
    std::atomic<std::uint32_t> pendingDeps{0};
    std::array<std::vector<WaiterRef>, worldgen::kChunkStatusCount>
        waitersByStage;
    std::array<std::uint16_t, worldgen::kChunkStatusCount> depTargetCounts{};
    std::vector<PinnedDep> stagePinnedDeps;

    ~GenTaskState() { clearPinnedDeps(); }

    void clearPinnedDeps() noexcept {
      for (const auto &dep : stagePinnedDeps) {
        if (dep.chunk)
          dep.chunk->releaseGenerationUser();
        if (dep.holder)
          dep.holder->generationRefCount.fetch_sub(1, std::memory_order_acq_rel);
      }
      stagePinnedDeps.clear();
    }

    void recomputeDependentTargetLocked() noexcept {
      dependentTarget = 0;
      for (std::size_t s = worldgen::kChunkStatusCount; s-- > 1;) {
        if (depTargetCounts[s] > 0) {
          dependentTarget = static_cast<std::uint8_t>(s);
          break;
        }
      }
    }

    std::uint8_t effectiveTarget() const noexcept {
      return std::max(explicitTarget, dependentTarget);
    }
  };

  struct SpatialWaiter {
    std::shared_ptr<ChunkHolder> holder;
    std::uint32_t genId = 0;
    std::uint8_t nextStage = 0;
    std::uint8_t distBucket = 32;
    bool residencyPath = false;
    bool isFinalize = false;
  };

  ChunkHolder(ChunkTicketPos pos, std::uint64_t key, std::uint8_t color,
              ChunkMap *owner = nullptr) noexcept;
  ~ChunkHolder();

  ChunkHolder(const ChunkHolder &) = delete;
  ChunkHolder &operator=(const ChunkHolder &) = delete;

  ChunkTicketPos pos() const noexcept { return pos_; }
  std::uint64_t key() const noexcept { return key_; }
  std::uint8_t color() const noexcept { return color_; }
  std::shared_mutex &chunkMutex() noexcept { return chunkMtx_; }

  HeldChunkKind heldKind() const noexcept {
    std::shared_lock lock(chunkMtx_);
    return kind_;
  }
  bool empty() const noexcept {
    std::shared_lock lock(chunkMtx_);
    return kind_ == HeldChunkKind::Empty;
  }

  bool evicted() const noexcept {
    return evicted_.load(std::memory_order_acquire);
  }
  void markEvicted() noexcept {
    evicted_.store(true, std::memory_order_release);
  }
  void clearEvicted() noexcept {
    evicted_.store(false, std::memory_order_release);
  }

  // 线程安全地获取当前持有的区块共享句柄。
  std::shared_ptr<BaseChunk> chunkShared() const;
  std::shared_ptr<worldgen::ProtoChunk> protoShared() const;
  std::shared_ptr<WorldChunk> worldShared() const;

  // 仅限主线程快路径：无锁返回当前持有的 WorldChunk 裸指针。
  WorldChunk *worldChunkRawMainThread() const noexcept {
    return kind_ == HeldChunkKind::World ? world_.get() : nullptr;
  }

  // 设置/替换持有的区块；旧区块会被置为 invalidated，若旧区块为 WorldChunk，
  // 会在释放前同步清理主线程快表与读缓存。
  void setProto(std::shared_ptr<worldgen::ProtoChunk> proto);
  void setWorld(std::shared_ptr<WorldChunk> world);
  void resetChunk();
  bool tryEvictProto();

  // 将当前持有的 `ProtoChunk` 原地晋升为 `WorldChunk`：
  // 直接移交/共享区段与群系、move 4 张终态高度图、复制/移交结构数据，
  // 将旧 `ProtoChunk` 置为 invalidated，并更新主线程 `worldIndex_`。
  std::shared_ptr<WorldChunk> promoteProtoToWorld(
      int minY, int maxY,
      std::vector<Pos> *outPostProcessing = nullptr,
      std::vector<terrain::SavedTick> *outBlockTicks = nullptr,
      std::vector<terrain::SavedTick> *outFluidTicks = nullptr,
      std::vector<terrain::MovingPistonEntity> *outMovingPistons = nullptr,
      std::vector<nbt::Tag> *outEntities = nullptr);

  // 供 `LightService` 直接从当前持有区块（ProtoChunk 或 WorldChunk）读取区段视图。
  std::optional<LightSectionView> readLightSection(int sectionY,
                                                   int minBlockSectionY,
                                                   int blockSectionCount) const;

  GenTaskState &ensureGenState();
  GenTaskState *genState() noexcept { return gen_.get(); }
  const GenTaskState *genState() const noexcept { return gen_.get(); }
  void releaseGenStateIfIdle();

  std::atomic<std::uint32_t> generationRefCount{0};

  // Protected by WorldGenScheduler::spatialMutex_ (except writeRegionLockedAtomic).
  bool writeRegionLocked = false;
  std::atomic<bool> writeRegionLockedAtomic{false};
  std::vector<SpatialWaiter> writeWaiters;

private:
  void releaseOutgoingLocked() noexcept;

  ChunkTicketPos pos_{};
  std::uint64_t key_ = 0;
  std::uint8_t color_ = 0;
  ChunkMap *owner_ = nullptr;
  std::atomic<bool> evicted_{false};

  mutable std::shared_mutex chunkMtx_;
  HeldChunkKind kind_ = HeldChunkKind::Empty;
  union {
    std::shared_ptr<worldgen::ProtoChunk> proto_;
    std::shared_ptr<WorldChunk> world_;
  };

  std::mutex genInitMtx_;
  std::unique_ptr<GenTaskState> gen_;
};

// 统一区块资源管理器（`World` 成员）：
// 负责所有已加载/生成中 `ChunkHolder` 的存储、主线程无锁 `worldIndex_`、缓存失效联动
// 以及内置的多线程世界生成调度器（`WorldGenScheduler`）。
class ChunkMap {
public:
  using HolderMap = phmap::parallel_flat_hash_map<
      std::uint64_t, std::shared_ptr<ChunkHolder>, ChunkKeyHash,
      std::equal_to<std::uint64_t>,
      std::allocator<std::pair<const std::uint64_t, std::shared_ptr<ChunkHolder>>>,
      6, std::shared_mutex>;
  using WorldIndexMap =
      phmap::flat_hash_map<std::uint64_t, WorldChunk *, ChunkKeyHash>;

  explicit ChunkMap(World *world = nullptr);
  ~ChunkMap();

  ChunkMap(const ChunkMap &) = delete;
  ChunkMap &operator=(const ChunkMap &) = delete;

  void rebindWorld(World *world) noexcept { world_ = world; }
  World *world() const noexcept { return world_; }

  // ---- 多线程 ChunkHolder 主表 ----
  std::shared_ptr<ChunkHolder> findHolder(ChunkTicketPos pos) const;
  std::shared_ptr<ChunkHolder> findHolder(std::uint64_t key) const;
  std::shared_ptr<ChunkHolder> getOrCreateHolder(ChunkTicketPos pos);
  std::shared_ptr<ChunkHolder> getOrCreateHolder(std::uint64_t key);
  bool eraseHolderIf(std::uint64_t key,
                     const std::function<bool(ChunkHolder &)> &pred);
  HolderMap &holders() noexcept { return holders_; }
  const HolderMap &holders() const noexcept { return holders_; }

  // ---- 主线程专用无锁 worldIndex_ ----
  WorldChunk *findWorldChunk(std::uint64_t key) const noexcept {
    const auto it = worldIndex_.find(key);
    return it == worldIndex_.end() ? nullptr : it->second;
  }
  WorldChunk *findWorldChunk(int x, int z) const noexcept {
    return findWorldChunk(packChunkKey(x, z));
  }
  const WorldIndexMap &worldIndex() const noexcept { return worldIndex_; }

  // 主线程将 WorldChunk 挂载进 ChunkHolder + worldIndex_；若已有 holder 则复用。
  std::shared_ptr<ChunkHolder> installWorldChunk(
      int x, int z, std::shared_ptr<WorldChunk> chunk);
  // 离线软卸载：保持 WorldChunk 在 ChunkHolder 中，但置 loaded=false 并从 worldIndex_ 与缓存移除。
  bool unloadWorldChunk(int x, int z, bool &wasLoaded);
  // 彻底淘汰：使旧区块失效、清理全部裸指针缓存、清空 ChunkHolder 并从主表移除。
  bool evictChunk(int x, int z);

  // 供 ChunkHolder 在释放/替换 WorldChunk 时同步失效主线程裸指针缓存。
  void invalidateWorldCaches(std::uint64_t key, WorldChunk *expected = nullptr);
  void registerWorldIndex(std::uint64_t key, WorldChunk *chunk);

  // ---- 世界生成调度器（合并为 ChunkMap 成员）----
  worldgen::WorldGenScheduler &enableWorldGen(
      worldgen::ChunkGenerator &generator, std::int64_t worldSeed,
      RawChunkStore *diskStore = nullptr,
      std::shared_ptr<worldgen::LightOpticalContext> lightContext = {},
      worldgen::GeneratedLightQueue *lightQueue = nullptr,
      worldgen::WorldGenSchedulerConfig config = {});
  void disableWorldGen();
  worldgen::WorldGenScheduler *scheduler() noexcept { return scheduler_.get(); }
  const worldgen::WorldGenScheduler *scheduler() const noexcept {
    return scheduler_.get();
  }
  bool worldGenEnabled() const noexcept { return scheduler_ != nullptr; }

  // ---- 光照工作线程统一只读入口 ----
  LightSectionView directReadSection(int chunkX, int chunkZ,
                                     int sectionY) const noexcept;
  static LightSectionView directReadSectionCallback(const void *context,
                                                    int chunkX, int chunkZ,
                                                    int sectionY) noexcept;

  std::size_t holderCount() const;
  std::size_t worldChunkCount() const noexcept { return worldIndex_.size(); }

private:
  World *world_ = nullptr;
  HolderMap holders_;
  WorldIndexMap worldIndex_;
  std::unique_ptr<worldgen::WorldGenScheduler> scheduler_;
};

// 跨 `ProtoChunk` / `WorldChunk` 的非虚状态查询辅助（替代虚基类 `ChunkAccess`）。
inline worldgen::ChunkStatusId
chunkPersistedStatus(const BaseChunk &chunk) noexcept {
  if (chunk.isWorldChunk())
    return worldgen::ChunkStatusId::Full;
  return static_cast<const worldgen::ProtoChunk &>(chunk).persistedStatus();
}

inline int chunkHeight(BaseChunk &chunk, HeightmapType type, int x, int z) {
  if (chunk.isWorldChunk())
    return static_cast<const WorldChunk &>(chunk).getHeight(type, x, z);
  return static_cast<worldgen::ProtoChunk &>(chunk).getHeight(type, x, z);
}

} // namespace redstone
