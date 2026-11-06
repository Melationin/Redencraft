#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <deque>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "redstone/block.hpp"

namespace redstone {
enum class Direction { Down, Up, North, South, West, East };
Direction opposite(Direction d);
Direction directionFromString(const std::string &s);
std::string directionName(Direction d);
// Namespaced resource location with the default "minecraft:" namespace.
std::string canonicalName(std::string name);
struct Pos {
  int x = 0, y = 0, z = 0;
  auto operator<=>(const Pos &) const = default;
  Pos relative(Direction d) const;
};
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
struct PistonTickerSlot {
  Pos pos;
  std::weak_ptr<PistonMovingBlockEntity> entity;
};
class Entity;
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
  BlockState getBlock(Pos pos) const;
  bool setBlock(Pos pos, BlockState state, int flags = 3, int limit = 512);
  void rawSetBlock(Pos pos, BlockState state);
  std::map<Pos, BlockState> allBlocks() const;
  int minY() const { return minY_; }
  int maxY() const { return maxY_; }
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
  void scheduleTick(Pos pos, const BlockRef &block, int delay,
                    int priority = 0);
  // LevelTicks.schedule: insert an already created tick (restored saves, test
  // fixtures). Deduplicated by (pos, block) like LevelChunkTicks.schedule.
  bool scheduleTick(const ScheduledTick &tick);
  // LevelChunkTicks.scheduleUnchecked: keeps duplicates found in saved lists.
  bool restoreScheduledTick(const ScheduledTick &tick);
  bool hasScheduledTick(Pos pos, const BlockRef &block) const;
  std::size_t scheduledTickCount() const;
  // Deterministic snapshot in drain order, for logs, tools and tests.
  std::vector<ScheduledTick> scheduledTicks() const;
  void blockEvent(Pos pos, const BlockRef &block, int type, int data);
  std::size_t blockEventCount() const;
  std::vector<BlockEvent> pendingBlockEvents() const;
  int getSignal(Pos pos, Direction direction) const;
  bool hasNeighborSignal(Pos pos) const;
  void tick();
  std::int64_t gameTime() const { return time_; }
  void setGameTime(std::int64_t value) { time_ = value; }
  bool isHandlingTick() const { return handlingTick_; }
  std::vector<LogEntry> log;
  std::function<void(World &)> blockEntityTicker;
  // Observational network hooks at Level.sendBlockUpdated / doBlockEvent.
  // Consumers must not mutate the simulation from these callbacks.
  std::function<void(Pos, const BlockState &, int)> blockUpdateObserver;
  // FallingBlockEntity sends a distinct immediate correction to its trackers
  // after successful landing, before discarding the entity.
  std::function<void(int, Pos, const BlockState &)> fallingBlockLandObserver;
  std::function<void(const BlockEvent &)> blockEventObserver;
  std::function<void()> blockUpdatesBroadcastObserver;

  void setPistonEntity(Pos pos,
                       std::shared_ptr<PistonMovingBlockEntity> entity);
  void removePistonEntity(Pos pos);
  std::map<Pos, std::shared_ptr<PistonMovingBlockEntity>> pistonEntities;
  std::map<Pos, std::shared_ptr<PistonTickerSlot>> pistonTickerSlots;
  std::vector<std::shared_ptr<PistonTickerSlot>> pistonEntityOrder;
  // Entity phase. Java EntityTickList iterates its active insertion-ordered
  // map; entities added during the pass are copied to the next active map and
  // therefore tick on the following tick. tickEntities() uses a snapshot of
  // this vector and skips any entry marked removed.
  std::shared_ptr<Entity> addEntity(std::shared_ptr<Entity> entity);
  void removeEntity(const std::shared_ptr<Entity> &entity);
  // EntitySectionStorage.getEntities: only sections overlapping the box are
  // visited; results keep the previous spawn-order (entity id) ordering.
  std::vector<Entity *> entitiesIn(const Aabb &box) const;
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
  // PistonMovingBlockEntity's ThreadLocal NOCLIP guard. While set, the moving
  // piston(s) whose movement direction matches ignore their moved-state
  // collision shape for pushed entities.
  std::optional<Direction> pistonNoClip;
  // Optional instrumentation hook, invoked after a moving entity is removed.
  std::function<void(World &, Pos)> pistonCompletionObserver;
  bool tntExplodes = true;
  bool traceUpdates = false;

private:
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
  void runScheduled(const ScheduledTick &tick);
  void runEvents();
  // LevelChunkSection: a paletted container of block states. An empty section
  // is a single air value (PalettedContainer bits=0) and needs no storage.
  struct Section {
    std::uint16_t nonEmpty = 0;
    std::uint8_t bits = 0; // 0 = single value, 8 or 16 bits of palette index
    BlockState::Id single = 0;
    std::vector<BlockState::Id> palette;
    std::vector<std::uint8_t> indices8;
    std::vector<std::uint16_t> indices16;
    bool hasOnlyAir() const { return nonEmpty == 0; }
    BlockState::Id get(int index) const;
    BlockState::Id set(int index, BlockState::Id state);
  };
  struct Chunk {
    bool loaded = true;
    std::vector<std::unique_ptr<Section>> sections;
  };
  static int chunkIndex(int v) {
    return v >= 0 ? v / 16 : -1 - ((-1 - v) / 16);
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
  mutable std::array<ChunkCacheSlot, 64> chunkCache_{};
  Section *sectionAt(Pos p, bool create);
  std::unordered_map<std::uint64_t, Chunk> chunks_;
  // EntitySectionStorage: 16^3 sections keyed by SectionPos.asLong (vanilla
  // entity section storage); queries visit only the sections a box overlaps.
  std::map<std::uint64_t, std::vector<Entity *>> entitySections_;
  void entitySectionAdd(Entity &entity);
  void entitySectionRemove(Entity &entity);
  void entitySectionUpdate(Entity &entity);
  int minY_, maxY_, maxUpdates_, updateCount_ = 0;
  bool handlingTick_ = false;
  std::int64_t time_ = 0;
  // Level.nextSubTickCount: monotonic long counter, never persisted.
  std::int64_t subTickCount_ = 0;
  std::vector<Update> stack_, added_;
  // LevelChunkTicks<ScheduledTick>: per-chunk priority queue (DRAIN_ORDER) plus
  // a hash set of (pos, block) for ScheduledTick.UNIQUE_TICK_HASH dedupe.
  struct ChunkTicks;
  // LevelTicks<ScheduledTick>: chunk containers, nextTickForContainer index and
  // the containersToTick/toRunThisTick drain state.
  struct TickQueue;
  std::unique_ptr<TickQueue> tickQueue_ = std::make_unique<TickQueue>();
  // ServerLevel.blockEvents (ObjectLinkedOpenHashSet) + reschedule list.
  struct BlockEventQueue;
  std::unique_ptr<BlockEventQueue> blockEventQueue_ =
      std::make_unique<BlockEventQueue>();
  std::vector<BlockEvent> blockEventsToReschedule_;
  std::int64_t nextSubTickCount();
};
} // namespace redstone
