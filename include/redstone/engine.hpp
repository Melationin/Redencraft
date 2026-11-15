#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <deque>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "redstone/block.hpp"
#include "redstone/block_tags.hpp"
#include "redstone/entity_codec.hpp"
#include "redstone/fluid.hpp"
#include "redstone/flat_hash_map.hpp"
#include "redstone/heightmap.hpp"
#include "redstone/light.hpp"

namespace redstone {
enum class Direction { Down, Up, North, South, West, East };
Direction opposite(Direction d);
enum class Rotation { None, Clockwise90, Clockwise180, Counterclockwise90 };
// RotatedPillarBlock.rotatePillar; state must have an axis property.
BlockState rotatePillar(BlockState state, Rotation rotation);
Direction directionFromString(std::string_view s);
std::string directionName(Direction d);
// Facing and Direction share the Down..East order, so the property field can be
// reinterpreted instead of re-parsed from its name.
inline Direction directionOf(const BlockState &state,
                             Direction fallback = Direction::North) {
  return static_cast<Direction>(
      facingOf(state, static_cast<Facing>(fallback)));
}
// Namespaced resource location with the default "minecraft:" namespace.
std::string canonicalName(std::string name);
struct Pos {
  int x = 0, y = 0, z = 0;
  auto operator<=>(const Pos &) const = default;
  Pos relative(Direction d) const;
};
// Same mixing as the scheduled-tick key: the low bits select the bucket, and
// the multiply-xor chain keeps neighbouring block positions apart.
struct PosHash {
  std::uint64_t operator()(const Pos &p) const noexcept {
    std::uint64_t hash =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) *
        0x9E3779B97F4A7C15ull;
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) *
            0xC2B2AE3D27D4EB4Full;
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) *
            0x165667B19E3779F9ull;
    return hash ^ (hash >> 32);
  }
};
// Position keyed flat table: LevelChunk.blockEntities / tickersInLevel are
// Object2ObjectOpenHashMap in vanilla, not ordered maps.
template <class T> using PosMap = FlatHashMap<Pos, T, PosHash>;
// Java java.util.Random-compatible stream. Vanilla's ServerLevel.random is a
// legacy RandomSource; this follows the same generator for source structure,
// but exact random sequences are not a compatibility requirement (see
// docs/entities.md).
class Random {
public:
  explicit Random(std::int64_t seed = 0) { setSeed(seed); }
  void setSeed(std::int64_t seed) {
    state_ = (static_cast<std::uint64_t>(seed) ^ 0x5deece66dULL) &
             ((1ULL << 48) - 1);
  }
  int nextInt();
  int nextInt(int bound);
  std::int64_t nextLong();
  float nextFloat();
  double nextDouble();
  double nextDouble(double min, double max) {
    return min + nextDouble() * (max - min);
  }

private:
  std::uint32_t next(int bits);
  std::uint64_t state_ = 0;
};
struct Vec3 {
  double x = 0, y = 0, z = 0;
  Vec3 operator+(const Vec3 &) const;
  Vec3 operator-(const Vec3 &) const;
  Vec3 operator*(double) const;
  Vec3 &operator+=(const Vec3 &);
  double lengthSqr() const;
  double length() const;
  double horizontalDistanceSqr() const;
  double horizontalDistance() const;
  Vec3 normalize() const;
  Vec3 multiply(double x, double y, double z) const;
  Vec3 with(double x, double y, double z) const;
  bool isFinite() const;
};
struct ShapeBox {
  double x0, y0, z0, x1, y1, z1;
};
using VoxelShape = std::vector<ShapeBox>;
struct Aabb {
  double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
  static Aabb ofSize(Vec3 center, double width, double height);
  Aabb move(double x, double y, double z) const;
  Aabb expandTowards(const Vec3 &) const;
  Aabb inflate(double x, double y, double z) const;
  Aabb inflate(double amount) const;
  Aabb deflate(double amount) const;
  bool intersects(const Aabb &) const;
};
struct EntityType {
  std::string name;
  float width = 0.6f;
  float height = 1.8f;
  float eyeHeight = 1.53f;
  bool fireImmune = false;
  bool simulated = false;
  // EntityType.clientTrackingRange/updateInterval: how often ServerEntity
  // syncs a tracked entity's position and rotation.
  int updateInterval = 3;
};
const std::vector<EntityType> &entityRegistry();
const EntityType *entityType(const std::string &name);
struct ItemType {
  std::string name;
  int maxStackSize = 64;
};
const std::vector<ItemType> &itemRegistry();
const ItemType *itemType(const std::string &name);
struct ScheduledTick {
  Pos pos;
  BlockRef block;
  std::int64_t trigger = 0;
  int priority = 0;
  std::int64_t order = 0;
  bool operator==(const ScheduledTick &) const = default;
};
struct ChunkTickSnapshot {
  std::vector<ScheduledTick> blockTicks;
  std::vector<std::pair<ScheduledTick, FluidRef>> fluidTicks;
};
// ScheduledTick.DRAIN_ORDER / ScheduledTick.INTRA_TICK_DRAIN_ORDER: both are
// min-heap comparators (std::priority_queue puts the "greatest" last).
struct ScheduledTickDrainOrder {
  bool operator()(const ScheduledTick &a, const ScheduledTick &b) const;
};
struct ScheduledTickIntraOrder {
  bool operator()(const ScheduledTick &a, const ScheduledTick &b) const;
};
struct BlockEvent {
  Pos pos;
  BlockRef block;
  int type = 0, data = 0;
  bool operator==(const BlockEvent &) const = default;
};
struct BlockEventHash {
  std::size_t operator()(const BlockEvent &event) const;
};
struct LogEntry {
  std::string kind;
  Pos pos;
  std::string detail;
};
struct PistonMovingBlockEntity;
// LevelChunk.BoundTickingBlockEntity: the ticker owns its block entity and
// `removed` is the only liveness test the pass performs. Holding the entity
// strongly (vanilla does too) keeps every reference the pass takes valid even
// when a removal lands mid-pass; the sweep drops the slot afterwards.
struct PistonTickerSlot {
  Pos pos;
  std::shared_ptr<PistonMovingBlockEntity> entity;
  bool removed = false;
};
struct ContainerBlockEntity;
struct HopperBlockEntity;
enum class ContainerSleepMode : std::uint8_t {
  Active = 0,
  SleepIndefinite = 1,
  SleepUntil = 2,
};
// Lithium-style SleepingBlockEntity / SleepUntilTimeBlockEntityTickInvoker slot:
// preserves registration order in World::containerTickers while allowing idle
// or cooled containers (hoppers) to skip ticking at zero cost until woken by
// an inventory mutation, neighbor/block change, or nearby ItemEntity event.
struct ContainerTickerSlot {
  Pos pos;
  std::shared_ptr<ContainerBlockEntity> entity;
  ContainerSleepMode sleepMode = ContainerSleepMode::Active;
  std::int64_t wakeGameTime = 0;
  bool removed = false;
};
class Entity;
class WorldChunkManager;
// Chunk data exported for the network encoder: palette + index copies and the
// moving piston block entities, so wire encoding can run off the tick thread.
struct SectionSnapshot {
  std::uint16_t nonEmpty = 0;
  std::uint8_t bits = 0;
  BlockState::Id single = 0;
  std::vector<BlockState::Id> palette;
  std::vector<std::uint8_t> indices8;
  std::vector<std::uint16_t> indices16;
};
struct PistonSnapshot {
  Pos pos;
  BlockState movedState;
  Direction direction = Direction::Down;
  float progressO = 0.0f;
  bool extending = false;
  bool source = false;
};
struct ChunkSnapshot {
  int minY = 0, maxY = 0;
  std::vector<SectionSnapshot> sections;
  std::vector<PistonSnapshot> pistons;
};
// LevelTicks<ScheduledTick> holds one container per ticking chunk. ServerLevel
// keeps two instances, keyed on Block and on Fluid, so the queue is a template
// over the ticked object type. Both are completed in engine.cpp: the header only
// needs the names for World's unique_ptr members.
template <class Target> struct ChunkTicksOf;
template <class Target> struct TickQueueOf;
using BlockTickQueue = TickQueueOf<Block>;
using FluidTickQueue = TickQueueOf<Fluid>;

namespace detail {
struct WorldBehaviours;
}
class World {
public:
  explicit World(int minY = -64, int maxY = 320,
                 int maxNeighborUpdates = 1000000);
  ~World();
  World(World &&) noexcept;
  World &operator=(World &&) noexcept;
  World(const World &) = delete;
  World &operator=(const World &) = delete;
  void loadChunk(int x, int z);
  void unloadChunk(int x, int z);
  bool isChunkLoaded(int x, int z) const;
  bool isLoaded(Pos pos) const;
  // Explicit ordered pack list, lowest priority first. Reload replaces this
  // world's snapshot only after the new definitions have been built.
  BlockTagRegistry::ReloadResult reloadBlockTags(const std::vector<std::filesystem::path> &packs);
  std::optional<bool> hasBlockTag(const BlockState &state, std::string_view tag) const;
  const BlockTagRegistry *customBlockTags() const { return blockTags_.get(); }
  std::uint64_t blockTagRevision() const { return blockTagRevision_; }
  // Opt-in L-1 handoff. Production servers currently leave this disabled; the
  // profile still has neither propagation nor a protocol-ready light snapshot.
  void enableLighting(LightWorkerOptions options = {});
  void disableLighting();
  void setLowMemoryMode(bool enabled);
  bool lowMemoryMode() const noexcept { return lowMemoryMode_; }
  bool lightingEnabled() const { return lightService_ != nullptr; }
  LightService *lightService() const { return lightService_.get(); }
  // Main-thread result handoff. It is also valid to call while the world tick
  // is frozen, allowing completed light work to reach sessions immediately.
  void drainLightPublications();
  // Query-time vanilla composition. Block and sky remain separate in storage;
  // this reads the two main-thread layer caches and returns
  // max(block, sky - skyDarken), without creating a combined section.
  int getBrightness(Pos pos, int skyDarken = 0) const;
  BlockState getBlock(Pos pos) const;
  std::optional<BlockState> getBlockIfLoaded(Pos pos) const;
  // Heightmap.Types final maps. The four live maps are primed on import or
  // explicitly after raw construction; normal setBlock updates them in the
  // LevelChunk.setBlockState order.
  const Heightmap *heightmap(int chunkX, int chunkZ, HeightmapType type) const;
  int height(HeightmapType type, int blockX, int blockZ) const;
  void primeHeightmaps();
  // Replaces any queued pre-import snapshots after raw writes with the current
  // authoritative chunk state. No-op when lighting is disabled.
  void refreshLightSnapshots();
  void setChunkPersistedLight(int x, int z, PersistedChunkLight persisted);
  // Copy of one chunk's paletted sections (mutable state is never exported).
  ChunkSnapshot snapshotChunk(int chunkX, int chunkZ) const;
  bool setBlock(Pos pos, BlockState state, int flags = 3, int limit = 512);
  // Level.removeBlock: write back the position's CURRENT fluid legacy block
  // instead of air, so a waterlogged host leaves its water behind
  // (Level.java:276-280). `movedByPiston` sets flag 64 exactly like vanilla.
  bool removeBlock(Pos pos, bool movedByPiston = false);
  void rawSetBlock(Pos pos, BlockState state);
  // Bulk-imports one decoded 16x16x16 section directly into a loaded chunk.
  void importSectionBlocks(int cx, int cz, int sectionY,
                           std::span<const BlockState::Id> palette,
                           std::span<const std::uint16_t, kLightVolume> indices);
  std::map<Pos, BlockState> allBlocks() const;
  int minY() const { return minY_; }
  int maxY() const { return maxY_; }
  // LevelChunkSection capacity: 24 * 16 = 384 blocks of vertical range, which
  // covers the default -64..320 dimension. Chunk section storage is a fixed
  // inline array of this size, so the constructor rejects taller worlds.
  static constexpr int kSectionCapacity = 24;
  // Lithium random-tick minisection layout: 4096 cells divided into 17 slices
  // of <= 248 cells so per-slice counts fit in uint8_t without overflow.
  static constexpr int kRandomTickMinisectionSize = 248;
  static constexpr int kRandomTickMinisectionCount =
      (kLightVolume + kRandomTickMinisectionSize - 1) /
      kRandomTickMinisectionSize;
  static constexpr int kRandomTickLastMinisectionSize =
      kLightVolume -
      kRandomTickMinisectionSize * (kRandomTickMinisectionCount - 1);
  // ServerLevelMixin.MAX_COUNT_FOR_BLOCK_SEARCH_AFTER_RANDOM_CHANCE = 4096 * 0.09375.
  static constexpr int kRandomTickSparseThreshold = 384;
  static_assert(kSectionCapacity <= 32,
                "randomTickingSectionMask requires <= 32 sections");
  std::optional<BlockState> coralPlacementState(Pos pos,
                                                const BlockState &selected,
                                                Direction clickedFace);
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
  // LevelTicks.schedule: insert an already created tick (restored saves, test
  // fixtures). Deduplicated by (pos, block) like LevelChunkTicks.schedule.
  bool scheduleTick(const ScheduledTick &tick);
  // LevelChunkTicks.scheduleUnchecked: keeps duplicates found in saved lists.
  bool restoreScheduledTick(const ScheduledTick &tick);
  bool hasScheduledTick(Pos pos, const BlockRef &block) const;
  bool willTickThisTick(Pos pos, const BlockRef &block) const;
  std::size_t scheduledTickCount() const;
  // Deterministic snapshot in drain order, for logs, tools and tests.
  std::vector<ScheduledTick> scheduledTicks() const;
  std::vector<PistonSnapshot> snapshotChunkPistons(int chunkX,
                                                    int chunkZ) const;
  // Per-chunk access for online persistence. These visit only the target
  // chunk's containers; they do not enumerate the dimension.
  ChunkTickSnapshot snapshotChunkTicks(int chunkX, int chunkZ) const;
  ChunkTickSnapshot takeChunkTicks(int chunkX, int chunkZ);
  std::size_t chunkTickCount(int chunkX, int chunkZ) const;
  // ServerLevel.fluidTicks: a separate LevelTicks container per fluid identity.
  // Scheduling is deduplicated by (pos, fluid) exactly like block ticks, and the
  // two queues never share container state.
  bool scheduleFluidTick(Pos pos, const FluidRef &fluid, int delay,
                         int priority = 0);
  bool scheduleFluidTick(const ScheduledTick &tick, const FluidRef &fluid);
  // LevelChunkTicks.scheduleUnchecked for restored fluid lists.
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
  // Lightweight work counters for benchmarks. Dispatches include stale/empty
  // entries that were still handed to tickFluidAt; writes are successful
  // setBlock calls made from inside a dispatched fluid tick.
  std::uint64_t fluidDispatchCount() const { return fluidDispatches_; }
  std::uint64_t fluidWriteCount() const { return fluidWrites_; }
  std::vector<LogEntry> log;
  std::function<void(World &)> blockEntityTicker;
  // Observational network hooks at Level.sendBlockUpdated / doBlockEvent.
  // Consumers must not mutate the simulation from these callbacks.
  std::function<void(Pos, const BlockState &, int)> blockUpdateObserver;
  // FallingBlockEntity sends a distinct immediate correction to its trackers
  // after successful landing, before discarding the entity.
  std::function<void(int, Pos, const BlockState &)> fallingBlockLandObserver;
  std::function<void(const BlockEvent &)> blockEventObserver;
  // Runs on the main thread after a complete light-worker batch is published
  // and before ChunkHolder.broadcastChanges drains ordinary block updates.
  std::function<void(std::shared_ptr<const LightChunkSnapshot>)>
      lightPublicationObserver;
  // Explicit World load/unload, separate from a Session's tracking-view edge.
  // Runs on the main thread after the light service accepted the lifecycle.
  std::function<void(int, int, bool)> lightChunkLifecycleObserver;
  std::function<void()> blockUpdatesBroadcastObserver;
  std::function<void()> entityTrackingBroadcastObserver;
  std::function<void(const Entity &)> entityAddedObserver;
  std::function<void(int)> entityRemovedObserver;
  std::function<void(Vec3, float, int)> explosionObserver;
  std::function<void(Pos, BlockState, bool isFluid)> unsupportedRandomTickObserver;
  // Opt-in WorldChunkManager hook. Every successful state-changing write,
  // including nested setBlock calls, reports the previous and final state.
  std::function<void(Pos, const BlockState &, const BlockState &)>
      managedBlockMutationObserver;
  // Tick schedule/restore/execute/removal reports the affected block position.
  std::function<void(Pos)> tickMutationObserver;
  // Moving-piston progress/creation/removal invalidates the owning chunk's
  // persisted block-entity view.
  std::function<void(Pos)> pistonEntityMutationObserver;
  // Runtime entity tick/add/remove observer. Old and current positions let the
  // manager dirty only the source and destination chunks; the persistent UUID
  // identifies a cross-chunk handoff independently of the runtime entity id.
  std::function<void(Vec3, Vec3, const std::array<std::int32_t, 4> &, bool)>
      managedEntityMutationObserver;
  // Updates the entity-section index immediately after a world mechanism moves
  // an entity outside its own tick. The manager receives the original indexed
  // position, so section and persistence cannot lag until the next entity tick.
  void synchronizeEntityAfterMove(Entity &entity, Vec3 previousPosition);
  // Fixture-only: simulates an explicit bucket move between ticks and sends
  // the production boundary observer with the same stable persistent UUID.
  void moveEntityForManagedTest(const Entity &entity, Vec3 to);
  // Empty hooks preserve legacy offline behavior. WorldChunkManager installs
  // cached block/fluid and entity position readiness predicates.
  std::function<bool(Pos)> blockTickPositionReady;
  std::function<bool(Pos)> entityTickPositionReady;
  // Diagnostic counters for the direct section-column scan. They are disabled
  // on the production path; tests enable them only for the long-gap fixture.
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
  // LevelChunk.blockEntities: the position table is authoritative for queries
  // (getBlockEntity) and never consulted while ticking.
  PosMap<std::shared_ptr<PistonMovingBlockEntity>> pistonEntities;
  // LevelChunk.tickersInLevel: only used to rebind a position that already owns
  // a ticker, so a same-slot replacement keeps its list slot and order.
  PosMap<PistonTickerSlot *> pistonTickerSlots;
  // Level.blockEntityTickers / pendingBlockEntityTickers: one flat list walked
  // in registration order. Tickers created while the pass runs are appended to
  // the pending list and merged at the start of the next pass; removed tickers
  // are skipped by the pass and dropped by sweepPistonTickers().
  std::vector<std::unique_ptr<PistonTickerSlot>> pistonTickers;
  std::vector<std::unique_ptr<PistonTickerSlot>> pendingPistonTickers;
  bool tickingPistonBlockEntities = false;
  std::size_t pistonTickerCount() const { return pistonTickers.size(); }
  std::size_t pendingPistonTickerCount() const {
    return pendingPistonTickers.size();
  }
  void mergePendingPistonTickers();
  void sweepPistonTickers();

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
  // Entity phase. Java EntityTickList iterates its active insertion-ordered
  // map; entities added during the pass are copied to the next active map and
  // therefore tick on the following tick. tickEntities() uses a snapshot of
  // this vector and skips any entry marked removed.
  std::shared_ptr<Entity> addEntity(std::shared_ptr<Entity> entity);
  // Server-side protocol players share this monotonic namespace with World
  // entities. IDs remain unique after entity removal and connection churn.
  int allocateEntityId();
  void removeEntity(const std::shared_ptr<Entity> &entity);
  // EntitySectionStorage.getEntities: only sections overlapping the box are
  // visited; results keep the previous spawn-order (entity id) ordering.
  std::vector<Entity *> entitiesIn(const Aabb &box) const;
  // Entities in the section columns [minX,maxX] x [minZ,maxZ] at any height,
  // sorted by entity id (spawn order). ChunkMap tracking only visits the
  // chunks a player can see instead of the whole world entity list.
  std::vector<Entity *> entitiesInSectionRange(int minX, int maxX, int minZ,
                                               int maxZ) const;
  struct ItemEntitySnapshotResult {
    std::vector<entity_codec::EntityRecord> records;
    bool hasUnsupported = false;
    std::string detail;
  };
  ItemEntitySnapshotResult snapshotChunkItemEntities(int chunkX,
                                                      int chunkZ) const;
  // Test-only view of the ordered compact entity sequence in one chunk.
  std::vector<std::array<std::int32_t, 4>>
  chunkEntityOrderForTest(int chunkX, int chunkZ) const;
  bool hasEntityUuid(const std::array<std::int32_t, 4> &uuid) const;
  std::vector<std::shared_ptr<Entity>> entities;
  int nextEntityId = 1;
  Random random;
  std::uint64_t seed = 0;
  void setSeed(std::uint64_t value);
  std::vector<VoxelShape> blockCollisions(const Aabb &box);
  // Same query as blockCollisions, but fills world-space boxes directly so the
  // per-cell VoxelShape vectors (BlockCollisions' iterator) are never built.
  void collectCollisionBoxes(const Aabb &box, std::vector<ShapeBox> &out);
  bool noCollision(const Aabb &box);
  bool clip(Vec3 from, Vec3 to);
  void tickEntities();
  // Spawn helpers install a real entity and keep an observable spawn log entry.
  void spawnItem(Pos pos, const std::string &item, int count = 1,
                 bool defaultPickupDelay = true);
  void spawnItemAt(Vec3 pos, const std::string &item, int count = 1,
                   bool defaultPickupDelay = true);
  void spawnPrimedTnt(Vec3 pos, int fuse = 80);
  void spawnFallingBlock(Vec3 pos, const BlockState &state);
  void spawnMinecart(Vec3 pos);
  // TntBlock.prime followed by Level.removeBlock(pos, false).
  bool primeTntBlock(Pos pos);
  // Java ServerExplosion with TNT's power, damage calculator, and block
  // interaction. source may be null; only the direct damage source identity is
  // needed for knockback filtering.
  void explode(Entity *source, Vec3 center, float radius, bool fire);
  // ServerExplosion.calculateExplodedPositions work counters, accumulated for
  // the lifetime of the world. The simulation never reads them: they exist so a
  // harness can report how many probes and list entries an explosion really
  // costs, and how much of the list the final sort/unique has to discard.
  struct ExplosionStats {
    std::uint64_t explosions = 0;
    // Positions that missed the direct-mapped block cache, i.e. the ones that
    // actually read a block state ("probe").
    std::uint64_t probes = 0;
    // Entries appended to the destruction list before deduplication: every step
    // that enters a new block position and still has power left.
    std::uint64_t candidates = 0;
    // Entries surviving sort + unique, i.e. what interactWithBlocks visits.
    std::uint64_t destroyed = 0;
  };
  ExplosionStats explosionStats;
  // BlockGetter.traverseBlocks work counters (same rationale): how often a clip
  // runs and how many cells it probes.
  std::uint64_t clipCalls = 0;
  std::uint64_t clipCells = 0;
  // PistonMovingBlockEntity's ThreadLocal NOCLIP guard. While set, the moving
  // piston(s) whose movement direction matches ignore their moved-state
  // collision shape for pushed entities.
  std::optional<Direction> pistonNoClip;
  // Optional instrumentation hook, invoked after a moving entity is removed.
  std::function<void(World &, Pos)> pistonCompletionObserver;
  bool tntExplodes = true;
  // GameRules.WATER_SOURCE_CONVERSION / LAVA_SOURCE_CONVERSION. 26.1 defaults to
  // water on and lava off; FlowingFluid.canConvertToSource reads them.
  bool waterSourceConversion = true;
  bool lavaSourceConversion = false;
  bool spreadVines = true;
  int fireSpreadRadiusAroundPlayer = 128;
  bool waterEvaporates = false;
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
  // Local raw light max(block, sky) and LightLayer::BLOCK from published
  // LightService snapshots or persisted chunk light.
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
    // Update flags carry the source Block identity, not an owned name.
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
  // LevelChunkSection: a paletted container of block states. An empty section
  // is a single air value (PalettedContainer bits=0) and needs no storage.
  //
  // Both paletted representations live behind one pointer, so a section owns at
  // most one heap block and "which representation is live" is carried by that
  // block instead of by two nullable members that must always disagree. The
  // capacities are the widest the representation can address: 8-bit indices
  // reach exactly 256 palette entries, and a 4096-cell section cannot hold more
  // than 4096 distinct states, so the 16-bit palette can never overflow. Fixed
  // capacity means an index write and a palette append never reallocate and
  // never move the arrays, so a block's address is stable for its whole life.
  struct SectionStorage {
    std::uint16_t paletteSize = 0;
    std::uint8_t bits = 0; // 1..8 or 16; fixed when the block is constructed
  };
  template <std::uint8_t Bits>
  struct SectionStoragePacked final : SectionStorage {
    static_assert(Bits >= 1 && Bits <= 7);
    static constexpr std::size_t kPerWord = std::size_t{64} / Bits;
    static constexpr std::size_t kWords =
        (kLightVolume + kPerWord - 1) / kPerWord;
    static constexpr std::size_t kPaletteCapacity = std::size_t{1} << Bits;
    static constexpr std::uint64_t kMask = (std::uint64_t{1} << Bits) - 1u;

    SectionStoragePacked() { bits = Bits; }
    std::array<std::uint64_t, kWords> words{};
    std::array<BlockState::Id, kPaletteCapacity> palette{};
    std::array<std::uint8_t, kPaletteCapacity> paletteTicking{};

    std::uint16_t indexAt(std::size_t cell) const noexcept {
      const std::size_t word = cell / kPerWord;
      const std::size_t shift = (cell % kPerWord) * Bits;
      return static_cast<std::uint16_t>((words[word] >> shift) & kMask);
    }
    void setIndexAt(std::size_t cell, std::uint16_t value) noexcept {
      const std::size_t word = cell / kPerWord;
      const std::size_t shift = (cell % kPerWord) * Bits;
      words[word] = (words[word] & ~(kMask << shift)) |
                    ((static_cast<std::uint64_t>(value) & kMask) << shift);
    }
    void atomicSetIndexAt(std::size_t cell, std::uint16_t value) noexcept {
      const std::size_t word = cell / kPerWord;
      const std::size_t shift = (cell % kPerWord) * Bits;
      const std::uint64_t next =
          (words[word] & ~(kMask << shift)) |
          ((static_cast<std::uint64_t>(value) & kMask) << shift);
      std::atomic_ref<std::uint64_t>(words[word])
          .store(next, std::memory_order_release);
    }
  };
  struct SectionStorage8 final : SectionStorage {
    static constexpr std::size_t kPaletteCapacity = 256;
    SectionStorage8() { bits = 8; }
    std::array<std::uint8_t, kLightVolume> indices{}; // value-initialised
    std::array<BlockState::Id, 256> palette{};
    std::array<std::uint8_t, 256> paletteTicking{};

    std::uint16_t indexAt(std::size_t cell) const noexcept {
      return indices[cell];
    }
    void setIndexAt(std::size_t cell, std::uint16_t value) noexcept {
      indices[cell] = static_cast<std::uint8_t>(value);
    }
    void atomicSetIndexAt(std::size_t cell, std::uint16_t value) noexcept {
      std::atomic_ref<std::uint8_t>(indices[cell])
          .store(static_cast<std::uint8_t>(value), std::memory_order_release);
    }
  };
  struct SectionStorage16 final : SectionStorage {
    static constexpr std::size_t kPaletteCapacity = kLightVolume;
    SectionStorage16() { bits = 16; }
    std::array<std::uint16_t, kLightVolume> indices{};
    std::array<BlockState::Id, kLightVolume> palette{};
    std::array<std::uint8_t, kLightVolume> paletteTicking{};

    std::uint16_t indexAt(std::size_t cell) const noexcept {
      return indices[cell];
    }
    void setIndexAt(std::size_t cell, std::uint16_t value) noexcept {
      indices[cell] = value;
    }
    void atomicSetIndexAt(std::size_t cell, std::uint16_t value) noexcept {
      std::atomic_ref<std::uint16_t>(indices[cell])
          .store(value, std::memory_order_release);
    }
  };
  static std::uint8_t minBitsForPaletteSize(std::size_t paletteSize) noexcept {
    if (paletteSize <= 2)
      return 1;
    if (paletteSize <= 4)
      return 2;
    if (paletteSize <= 8)
      return 3;
    if (paletteSize <= 16)
      return 4;
    if (paletteSize <= 32)
      return 5;
    if (paletteSize <= 64)
      return 6;
    if (paletteSize <= 128)
      return 7;
    if (paletteSize <= 256)
      return 8;
    return 16;
  }
  static std::shared_ptr<SectionStorage>
  makeSectionStorageForBits(std::uint8_t bits) {
    switch (bits) {
    case 1:
      return std::make_shared<SectionStoragePacked<1>>();
    case 2:
      return std::make_shared<SectionStoragePacked<2>>();
    case 3:
      return std::make_shared<SectionStoragePacked<3>>();
    case 4:
      return std::make_shared<SectionStoragePacked<4>>();
    case 5:
      return std::make_shared<SectionStoragePacked<5>>();
    case 6:
      return std::make_shared<SectionStoragePacked<6>>();
    case 7:
      return std::make_shared<SectionStoragePacked<7>>();
    case 8:
      return std::make_shared<SectionStorage8>();
    case 16:
      return std::make_shared<SectionStorage16>();
    default:
      return nullptr;
    }
  }
  template <typename StoragePtr, typename Fn>
  static decltype(auto) visitSectionStorage(StoragePtr *base, Fn &&fn) {
    if constexpr (std::is_const_v<StoragePtr>) {
      switch (base->bits) {
      case 1:
        return fn(static_cast<const SectionStoragePacked<1> *>(base));
      case 2:
        return fn(static_cast<const SectionStoragePacked<2> *>(base));
      case 3:
        return fn(static_cast<const SectionStoragePacked<3> *>(base));
      case 4:
        return fn(static_cast<const SectionStoragePacked<4> *>(base));
      case 5:
        return fn(static_cast<const SectionStoragePacked<5> *>(base));
      case 6:
        return fn(static_cast<const SectionStoragePacked<6> *>(base));
      case 7:
        return fn(static_cast<const SectionStoragePacked<7> *>(base));
      case 8:
        return fn(static_cast<const SectionStorage8 *>(base));
      case 16:
        return fn(static_cast<const SectionStorage16 *>(base));
      }
      return fn(static_cast<const SectionStorage16 *>(base));
    } else {
      switch (base->bits) {
      case 1:
        return fn(static_cast<SectionStoragePacked<1> *>(base));
      case 2:
        return fn(static_cast<SectionStoragePacked<2> *>(base));
      case 3:
        return fn(static_cast<SectionStoragePacked<3> *>(base));
      case 4:
        return fn(static_cast<SectionStoragePacked<4> *>(base));
      case 5:
        return fn(static_cast<SectionStoragePacked<5> *>(base));
      case 6:
        return fn(static_cast<SectionStoragePacked<6> *>(base));
      case 7:
        return fn(static_cast<SectionStoragePacked<7> *>(base));
      case 8:
        return fn(static_cast<SectionStorage8 *>(base));
      case 16:
        return fn(static_cast<SectionStorage16 *>(base));
      }
      return fn(static_cast<SectionStorage16 *>(base));
    }
  }
  struct Section {
    std::uint16_t nonEmpty = 0;
    std::uint16_t tickingBlockCount = 0;
    BlockState::Id single = 0;
    std::array<std::uint8_t, kRandomTickMinisectionCount>
        randomTickByMinisection{};
    std::shared_ptr<SectionStorage> storage; // null = single value
    Section() = default;
    // Copying would alias the container, so the value is move-only. Nothing
    // copies a Chunk either (it owns a unique_ptr), so no call site needs it.
    Section(const Section &) = delete;
    Section &operator=(const Section &) = delete;
    Section(Section &&) noexcept = default;
    Section &operator=(Section &&) noexcept = default;
    std::uint8_t bits() const { return storage ? storage->bits : 0; }
    // Null unless the live representation is the requested one.
    const SectionStorage8 *as8() const {
      return storage && storage->bits == 8
                 ? static_cast<const SectionStorage8 *>(storage.get())
                 : nullptr;
    }
    const SectionStorage16 *as16() const {
      return storage && storage->bits == 16
                 ? static_cast<const SectionStorage16 *>(storage.get())
                 : nullptr;
    }
    bool hasOnlyAir() const { return nonEmpty == 0; }
    bool isRandomlyTicking() const { return tickingBlockCount > 0; }
    void initSingleRandomTick(BlockState::Id state);
    BlockState::Id get(int index) const;
    bool tryGetRandomlyTicking(int index, BlockState::Id &outId) const;
    BlockState::Id set(int index, BlockState::Id state, bool compact = false,
                       std::shared_mutex *resizeMutex = nullptr);
  };
  struct Chunk {
    static constexpr std::uint32_t kInvalidRandomTickIndex =
        std::numeric_limits<std::uint32_t>::max();
    bool loaded = true;
    bool hasPendingLightCell = false;
    std::uint32_t randomTickingSectionMask = 0;
    std::uint32_t randomTickIndex = kInvalidRandomTickIndex;
    // LightService generation token for this loaded lifetime. A reload at the
    // same address advances it, so queued old-lifetime cells are discarded.
    std::uint64_t lightLifecycleEpoch = 1;
    // Latest LightService sequence produced by a main-thread optical block
    // write on this chunk. Persistence checks require published snapshot
    // generation >= requiredLightGeneration before marking isLightOn=1.
    std::uint64_t requiredLightGeneration = 0;
    std::optional<PersistedChunkLight> persistedLight;
    std::array<std::int16_t, kSectionCapacity> persistedLightSectionIndex{};
    // The sections live inline: one pointer step from the chunk to the section
    // header, no per-section allocation, and every header of a chunk is
    // contiguous. A default Section is a single air value with no storage, so
    // "never written" needs no separate presence flag; Section::set materialises
    // the palette on the first real write.
    std::array<Section, kSectionCapacity> sections;
    std::unique_ptr<ChunkHeightmaps> heightmaps;
    void refreshRandomTickMask(int count);
    void refreshPersistedLightIndex(int minSectionY, int numSections);
  };
  static int chunkIndex(int v) {
    // C++20 arithmetic right shift rounds negative coordinates down.
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
  // Explosions and tick containers revisit the same handful of chunks; map
  // nodes are stable, so a direct-mapped cache of the last 64 chunk lookups
  // removes the unordered_map probe (and its bucket modulo) from getBlock.
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
    // World moves must not leave either world's read cache pointing into the
    // other's storage. Both caches are cheap to repopulate after the move.
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
  // Loaded chunks whose randomTickingSectionMask != 0, kept lazily sorted by
  // key for deterministic chunk iteration and O(1) swap-and-pop updates.
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
  // EntitySectionStorage: 16^3 sections keyed by SectionPos.asLong (vanilla
  // entity section storage); queries visit only the sections a box overlaps.
  std::map<std::uint64_t, std::vector<Entity *>> entitySections_;
  // Managed-save blocker indexes. They turn the former per-candidate full
  // scans into local lookups and are maintained by the same insert/remove
  // paths as their authoritative tables.
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
  bool handlingTick_ = false;
  bool insideFluidTick_ = false;
  std::uint64_t fluidDispatches_ = 0, fluidWrites_ = 0;
  std::int64_t time_ = 0;
  // Level.nextSubTickCount: monotonic long counter, never persisted.
  std::int64_t subTickCount_ = 0;
  std::vector<Update> stack_, added_;
  // LevelChunkTicks<ScheduledTick>: per-chunk priority queue (DRAIN_ORDER) plus
  // a hash set of (pos, target) for ScheduledTick.UNIQUE_TICK_HASH dedupe.
  // LevelTicks<ScheduledTick>: chunk containers, nextTickForContainer index and
  // the containersToTick/toRunThisTick drain state. ServerLevel keeps one pair
  // for Block and one for Fluid, so both are templates; they are completed in
  // engine.cpp so the containers stay out of the header.
  std::unique_ptr<BlockTickQueue> tickQueue_;
  // ServerLevel.fluidTicks: a second LevelTicks with its own 65536 budget,
  // collected and drained after blockTicks and before the chunk stage.
  std::unique_ptr<FluidTickQueue> fluidTickQueue_;
  std::unique_ptr<LightService> lightService_;
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

  // ServerLevel.blockEvents (ObjectLinkedOpenHashSet) + reschedule list.
  struct BlockEventQueue;
  std::unique_ptr<BlockEventQueue> blockEventQueue_;
  std::vector<BlockEvent> blockEventsToReschedule_;
  std::int64_t nextSubTickCount();

  // ServerExplosion.calculateExplodedPositions memoises per-block probes: the
  // ray shell revisits the same positions many times. The table lives across
  // explosions and a generation stamp replaces the old per-call zero fill.
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
  // Scratch list of exploded positions; capacity is kept between explosions.
  std::vector<Pos> explosionTargets_;
};
} // namespace redstone
