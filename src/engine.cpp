#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include "redstone/piston.hpp"
#include "redstone/rail.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace redstone {
namespace {
// Java long arithmetic wraps modulo 2^64, including subTickCount++.
std::int64_t nextSubTick(std::int64_t value) {
  auto bits = std::bit_cast<std::uint64_t>(value);
  return std::bit_cast<std::int64_t>(bits + std::uint64_t{1});
}
constexpr std::array<Direction, 6> neighborOrder{
    Direction::West, Direction::East,  Direction::Down,
    Direction::Up,   Direction::North, Direction::South};
constexpr std::array<Direction, 6> shapeOrder{
    Direction::West,  Direction::East, Direction::North,
    Direction::South, Direction::Down, Direction::Up};
std::string canonical(std::string n) { return canonicalName(std::move(n)); }
bool fittingBase(const BlockState &head, const BlockState &base) {
  return (head.get("type", "normal") == "sticky" ? base.is(Blocks::STICKY_PISTON)
                                                 : base.is(Blocks::PISTON)) &&
         base.get("extended", "false") == "true" &&
         base.get("facing", "north") == head.get("facing", "north");
}
bool piston(const BlockState &s) {
  return s.is(Blocks::PISTON) || s.is(Blocks::STICKY_PISTON);
}
} // namespace
Direction opposite(Direction d) {
  switch (d) {
  case Direction::Down:
    return Direction::Up;
  case Direction::Up:
    return Direction::Down;
  case Direction::North:
    return Direction::South;
  case Direction::South:
    return Direction::North;
  case Direction::West:
    return Direction::East;
  case Direction::East:
    return Direction::West;
  }
  return Direction::Down;
}
Direction directionFromString(const std::string &s) {
  if (s == "up")
    return Direction::Up;
  if (s == "north")
    return Direction::North;
  if (s == "south")
    return Direction::South;
  if (s == "west")
    return Direction::West;
  if (s == "east")
    return Direction::East;
  return Direction::Down;
}
std::string directionName(Direction d) {
  switch (d) {
  case Direction::Down:
    return "down";
  case Direction::Up:
    return "up";
  case Direction::North:
    return "north";
  case Direction::South:
    return "south";
  case Direction::West:
    return "west";
  case Direction::East:
    return "east";
  }
  return "down";
}
Pos Pos::relative(Direction d) const {
  Pos p = *this;
  switch (d) {
  case Direction::Down:
    --p.y;
    break;
  case Direction::Up:
    ++p.y;
    break;
  case Direction::North:
    --p.z;
    break;
  case Direction::South:
    ++p.z;
    break;
  case Direction::West:
    --p.x;
    break;
  case Direction::East:
    ++p.x;
    break;
  }
  return p;
}
std::uint32_t Random::next(int bits) {
  state_ = (state_ * 0x5deece66dULL + 0xbULL) & ((1ULL << 48) - 1);
  return static_cast<std::uint32_t>(state_ >> (48 - bits));
}
int Random::nextInt() { return static_cast<std::int32_t>(next(32)); }
int Random::nextInt(int bound) {
  if (bound <= 0)
    throw std::invalid_argument("bound must be positive");
  if ((bound & -bound) == bound)
    return static_cast<int>((static_cast<std::uint64_t>(bound) * next(31)) >>
                            31);
  int bits, value;
  do {
    bits = next(31);
    value = bits % bound;
  } while (bits - value + (bound - 1) < 0);
  return value;
}
std::int64_t Random::nextLong() {
  std::int64_t high = static_cast<std::int32_t>(next(32));
  std::int64_t low = static_cast<std::int32_t>(next(32));
  return (high << 32) + low;
}
float Random::nextFloat() { return next(24) / static_cast<float>(1 << 24); }
double Random::nextDouble() {
  return ((static_cast<std::uint64_t>(next(26)) << 27) + next(27)) /
         static_cast<double>(1ULL << 53);
}
Vec3 Vec3::operator+(const Vec3 &o) const {
  return {x + o.x, y + o.y, z + o.z};
}
Vec3 Vec3::operator-(const Vec3 &o) const {
  return {x - o.x, y - o.y, z - o.z};
}
Vec3 Vec3::operator*(double s) const { return {x * s, y * s, z * s}; }
Vec3 &Vec3::operator+=(const Vec3 &o) {
  x += o.x;
  y += o.y;
  z += o.z;
  return *this;
}
double Vec3::lengthSqr() const { return x * x + y * y + z * z; }
double Vec3::length() const { return std::sqrt(lengthSqr()); }
double Vec3::horizontalDistanceSqr() const { return x * x + z * z; }
double Vec3::horizontalDistance() const { return std::sqrt(horizontalDistanceSqr()); }
Vec3 Vec3::normalize() const {
  double length = this->length();
  return length < 1.0e-4 ? Vec3{} : *this * (1.0 / length);
}
Vec3 Vec3::multiply(double mx, double my, double mz) const {
  return {x * mx, y * my, z * mz};
}
Vec3 Vec3::with(double nx, double ny, double nz) const {
  return {nx, ny, nz};
}
bool Vec3::isFinite() const {
  return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
}
Aabb Aabb::ofSize(Vec3 center, double width, double height) {
  double half = width / 2.0;
  return {center.x - half, center.y, center.z - half,
          center.x + half, center.y + height, center.z + half};
}
Aabb Aabb::move(double x, double y, double z) const {
  return {minX + x, minY + y, minZ + z, maxX + x, maxY + y, maxZ + z};
}
Aabb Aabb::expandTowards(const Vec3 &v) const {
  double x0 = std::min(minX, minX + v.x), y0 = std::min(minY, minY + v.y),
         z0 = std::min(minZ, minZ + v.z);
  double x1 = std::max(maxX, maxX + v.x), y1 = std::max(maxY, maxY + v.y),
         z1 = std::max(maxZ, maxZ + v.z);
  return {x0, y0, z0, x1, y1, z1};
}
Aabb Aabb::inflate(double x, double y, double z) const {
  return {minX - x, minY - y, minZ - z, maxX + x, maxY + y, maxZ + z};
}
Aabb Aabb::inflate(double amount) const {
  return inflate(amount, amount, amount);
}
Aabb Aabb::deflate(double amount) const {
  return inflate(-amount, -amount, -amount);
}
bool Aabb::intersects(const Aabb &o) const {
  return minX < o.maxX && maxX > o.minX && minY < o.maxY && maxY > o.minY &&
         minZ < o.maxZ && maxZ > o.minZ;
}
const std::vector<EntityType> &entityRegistry() {
  static const std::vector<EntityType> entries = {
      // EntityType.clientTrackingRange/updateInterval of the vanilla types.
      {"minecraft:item", 0.25f, 0.25f, 0.2125f, false, true, 20},
      {"minecraft:tnt", 0.98f, 0.98f, 0.15f, true, true, 10},
      {"minecraft:falling_block", 0.98f, 0.98f, 0.833f, false, true, 20},
      {"minecraft:minecart", 0.98f, 0.7f, 0.595f, false, true, 3}};
  return entries;
}
const EntityType *entityType(const std::string &name) {
  const auto n = canonical(name);
  const auto &registry = entityRegistry();
  auto found = std::find_if(registry.begin(), registry.end(),
                            [&](const auto &entry) { return entry.name == n; });
  return found == registry.end() ? nullptr : &*found;
}
const std::vector<ItemType> &itemRegistry() {
  static const std::vector<ItemType> entries = [] {
    std::vector<ItemType> result;
    auto add = [&](const std::string &name, int max = 64) {
      result.push_back({canonical(name), max});
    };
    add("minecraft:dirt");
    add("minecraft:grass_block");
    add("minecraft:bedrock");
    add("minecraft:slime_block");
    add("minecraft:honey_block");
    add("minecraft:smooth_stone");
    add("minecraft:white_terracotta");
    for (const auto *color : {"tube", "brain", "bubble", "fire", "horn"})
      add(std::string("minecraft:dead_") + color + "_coral_fan");
    add("minecraft:cobblestone_wall");
    add("minecraft:stone_brick_wall");
    add("minecraft:sand");
    add("minecraft:redstone_lamp");
    add("minecraft:obsidian");
    add("minecraft:glass");
    add("minecraft:piston");
    add("minecraft:sticky_piston");
    add("minecraft:observer");
    add("minecraft:note_block");
    add("minecraft:redstone_block");
    for (const auto *color :
         {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink",
          "gray", "light_gray", "cyan", "purple", "blue", "brown", "green",
          "red", "black"})
      add(std::string("minecraft:") + color + "_carpet");
    add("minecraft:tnt");
    add("minecraft:rail");
    add("minecraft:powered_rail");
    add("minecraft:detector_rail");
    add("minecraft:activator_rail");
    add("minecraft:minecart", 1);
    return result;
  }();
  return entries;
}
const ItemType *itemType(const std::string &name) {
  const auto n = canonical(name);
  const auto &registry = itemRegistry();
  auto found = std::find_if(registry.begin(), registry.end(),
                            [&](const auto &entry) { return entry.name == n; });
  return found == registry.end() ? nullptr : &*found;
}
World::World(int minY, int maxY, int maxNeighborUpdates)
    : minY_(minY), maxY_(maxY), maxUpdates_(maxNeighborUpdates) {}
World::~World() = default;
World::World(World &&) noexcept = default;
World &World::operator=(World &&) noexcept = default;
// ---------------------------------------------------------------------------
// ScheduledTick.UNIQUE_TICK_HASH hashes the block identity and the position.
// The interned name pointer is our Block reference, so the dedupe key is three
// ints plus a pointer: no string, no ownership, no allocation.
// ---------------------------------------------------------------------------
namespace {
struct TickKey {
  Pos pos;
  const Block *block = nullptr;
};
std::size_t tickKeyHash(const TickKey &key) {
  auto hash = static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.pos.x)) *
              0x9E3779B97F4A7C15ull;
  hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.pos.y)) *
          0xC2B2AE3D27D4EB4Full;
  hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.pos.z)) *
          0x165667B19E3779F9ull;
  hash ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key.block)) *
          0x9E3779B185EBCA87ull;
  hash ^= hash >> 29;
  hash *= 0x9E3779B97F4A7C15ull;
  hash ^= hash >> 32;
  return static_cast<std::size_t>(hash);
}
bool tickKeyEquals(const TickKey &a, const TickKey &b) {
  return a.pos == b.pos && a.block == b.block;
}
// Open-addressed set of (position, block) keys with tombstone deletion and a
// capacity-preserving clear, replacing a node-allocating std::unordered_set.
class TickKeySet {
public:
  std::size_t size() const { return count_; }
  bool contains(const TickKey &key) const { return slotOf(key) != kNone; }
  bool insert(const TickKey &key) {
    if (slots_.empty())
      rehash(64);
    if (slotOf(key) != kNone)
      return false;
    if ((used_ + 1) * 3 >= slots_.size() * 2)
      rehash(roundUp(std::max<std::size_t>(64, (count_ + 1) * 4)));
    std::size_t index = tickKeyHash(key) & mask_;
    std::size_t empty = kNone;
    for (;;) {
      if (state_[index] == kEmpty) {
        const auto target = empty == kNone ? index : empty;
        slots_[target] = key;
        state_[target] = kUsed;
        ++count_;
        if (empty == kNone)
          ++used_;
        return true;
      }
      if (state_[index] == kDeleted && empty == kNone)
        empty = index;
      index = (index + 1) & mask_;
    }
  }
  void erase(const TickKey &key) {
    const auto index = slotOf(key);
    if (index == kNone)
      return;
    state_[index] = kDeleted;
    --count_;
  }
  void clear() {
    std::fill(state_.begin(), state_.end(), static_cast<std::uint8_t>(kEmpty));
    count_ = used_ = 0;
  }

private:
  static constexpr std::uint8_t kEmpty = 0, kUsed = 1, kDeleted = 2;
  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  static std::size_t roundUp(std::size_t value) {
    std::size_t power = 64;
    while (power < value)
      power <<= 1;
    return power;
  }
  std::size_t slotOf(const TickKey &key) const {
    if (slots_.empty())
      return kNone;
    std::size_t index = tickKeyHash(key) & mask_;
    for (;;) {
      if (state_[index] == kEmpty)
        return kNone;
      if (state_[index] == kUsed && tickKeyEquals(slots_[index], key))
        return index;
      index = (index + 1) & mask_;
    }
  }
  void rehash(std::size_t capacity) {
    std::vector<TickKey> oldSlots = std::move(slots_);
    std::vector<std::uint8_t> oldState = std::move(state_);
    slots_.assign(capacity, TickKey{});
    state_.assign(capacity, static_cast<std::uint8_t>(kEmpty));
    mask_ = capacity - 1;
    count_ = used_ = 0;
    for (std::size_t i = 0; i < oldState.size(); ++i)
      if (oldState[i] == kUsed)
        insert(oldSlots[i]);
  }
  std::vector<TickKey> slots_;
  std::vector<std::uint8_t> state_;
  std::size_t mask_ = 0;
  std::size_t count_ = 0, used_ = 0;
};

std::size_t chunkKeyHash(std::uint64_t key) {
  key ^= key >> 33;
  key *= 0xff51afd7ed558ccdull;
  key ^= key >> 33;
  key *= 0xc4ceb9fe1a85ec53ull;
  key ^= key >> 33;
  return static_cast<std::size_t>(key);
}
// Packed-long open-addressed map, the equivalent of fastutil's
// Long2ObjectOpenHashMap / Long2LongOpenHashMap used by LevelTicks.
template <class V> class FlatChunkMap {
public:
  std::size_t size() const { return count_; }
  V *find(std::uint64_t key) { return const_cast<V *>(std::as_const(*this).find(key)); }
  const V *find(std::uint64_t key) const {
    if (keys_.empty())
      return nullptr;
    std::size_t index = chunkKeyHash(key) & mask_;
    for (;;) {
      if (state_[index] == kEmpty)
        return nullptr;
      if (state_[index] == kUsed && keys_[index] == key)
        return &values_[index];
      index = (index + 1) & mask_;
    }
  }
  void set(std::uint64_t key, const V &value) {
    if (keys_.empty())
      rehash(64);
    (void)assign(key, value, true);
  }
  bool erase(std::uint64_t key) {
    if (keys_.empty())
      return false;
    std::size_t index = chunkKeyHash(key) & mask_;
    for (;;) {
      if (state_[index] == kEmpty)
        return false;
      if (state_[index] == kUsed && keys_[index] == key) {
        state_[index] = kDeleted;
        --count_;
        return true;
      }
      index = (index + 1) & mask_;
    }
  }
  template <class F> void forEach(F &&visitor) const {
    for (std::size_t i = 0; i < keys_.size(); ++i)
      if (state_[i] == kUsed)
        visitor(keys_[i], values_[i]);
  }
  void clear() {
    std::fill(state_.begin(), state_.end(), static_cast<std::uint8_t>(kEmpty));
    count_ = used_ = 0;
  }

private:
  static constexpr std::uint8_t kEmpty = 0, kUsed = 1, kDeleted = 2;
  static std::size_t roundUp(std::size_t value) {
    std::size_t power = 64;
    while (power < value)
      power <<= 1;
    return power;
  }
  bool assign(std::uint64_t key, const V &value, bool allowGrow) {
    if ((used_ + 1) * 3 >= keys_.size() * 2 && allowGrow) {
      rehash(roundUp(std::max<std::size_t>(64, (count_ + 1) * 4)));
      return assign(key, value, false);
    }
    std::size_t index = chunkKeyHash(key) & mask_;
    std::size_t empty = static_cast<std::size_t>(-1);
    for (;;) {
      if (state_[index] == kEmpty) {
        const auto target = empty == static_cast<std::size_t>(-1) ? index : empty;
        keys_[target] = key;
        values_[target] = value;
        state_[target] = kUsed;
        ++count_;
        if (empty == static_cast<std::size_t>(-1))
          ++used_;
        return true;
      }
      if (state_[index] == kDeleted && empty == static_cast<std::size_t>(-1))
        empty = index;
      if (state_[index] == kUsed && keys_[index] == key) {
        values_[index] = value;
        return false;
      }
      index = (index + 1) & mask_;
    }
  }
  void rehash(std::size_t capacity) {
    std::vector<std::uint64_t> oldKeys = std::move(keys_);
    std::vector<V> oldValues = std::move(values_);
    std::vector<std::uint8_t> oldState = std::move(state_);
    keys_.assign(capacity, 0);
    values_.assign(capacity, V{});
    state_.assign(capacity, static_cast<std::uint8_t>(kEmpty));
    mask_ = capacity - 1;
    count_ = used_ = 0;
    for (std::size_t i = 0; i < oldState.size(); ++i)
      if (oldState[i] == kUsed)
        assign(oldKeys[i], oldValues[i], false);
  }
  std::vector<std::uint64_t> keys_;
  std::vector<V> values_;
  std::vector<std::uint8_t> state_;
  std::size_t mask_ = 0;
  std::size_t count_ = 0, used_ = 0;
};
} // namespace

// ---------------------------------------------------------------------------
// LevelChunkTicks<ScheduledTick>: per-chunk DRAIN_ORDER heap plus the
// (position, block identity) dedupe set of ScheduledTick.UNIQUE_TICK_HASH.
// ---------------------------------------------------------------------------
struct World::ChunkTicks {
  std::vector<ScheduledTick> heap; // min-heap under ScheduledTickDrainOrder
  TickKeySet ticksPerPosition;

  const ScheduledTick *peek() const { return heap.empty() ? nullptr : &heap.front(); }
  std::size_t count() const { return heap.size(); }
  bool has(Pos pos, const BlockRef &block) const {
    return ticksPerPosition.contains(TickKey{pos, block.block()});
  }
  void push(const ScheduledTick &tick) {
    heap.push_back(tick);
    std::push_heap(heap.begin(), heap.end(), ScheduledTickDrainOrder{});
  }
  bool schedule(const ScheduledTick &tick) {
    if (!ticksPerPosition.insert(TickKey{tick.pos, tick.block.block()}))
      return false;
    push(tick);
    return true;
  }
  void scheduleUnchecked(const ScheduledTick &tick) {
    // LevelChunkTicks.scheduleUnchecked: saved lists may repeat an entry; the
    // duplicates stay in the queue while the set records one of them.
    ticksPerPosition.insert(TickKey{tick.pos, tick.block.block()});
    push(tick);
  }
  ScheduledTick poll() {
    std::pop_heap(heap.begin(), heap.end(), ScheduledTickDrainOrder{});
    ScheduledTick tick = std::move(heap.back());
    heap.pop_back();
    ticksPerPosition.erase(TickKey{tick.pos, tick.block.block()});
    return tick;
  }
};

// ---------------------------------------------------------------------------
// LevelTicks<ScheduledTick>: chunk containers, nextTickForContainer index and
// the containersToTick / toRunThisTick drain state of
// LevelTicks.collectTicks/drainContainers/runCollectedTicks.
// ---------------------------------------------------------------------------
struct World::TickQueue {
  struct Entry {
    std::uint64_t key = 0;
    ChunkTicks *container = nullptr;
  };
  struct EntryOrder {
    bool operator()(const Entry &a, const Entry &b) const {
      const ScheduledTick *left = a.container->peek();
      const ScheduledTick *right = b.container->peek();
      if (!left || !right)
        return left == nullptr && right != nullptr;
      return ScheduledTickIntraOrder{}(*left, *right);
    }
  };
  // Containers live in a deque so their addresses stay stable; the maps only
  // store the packed chunk key.
  std::deque<ChunkTicks> storage;
  FlatChunkMap<ChunkTicks *> containers;
  FlatChunkMap<std::int64_t> nextTickForContainer;
  std::vector<Entry> containersToTick; // min-heap under EntryOrder
  std::deque<ScheduledTick> toRunThisTick;
  std::vector<std::uint64_t> dueScratch;

  // LevelTicks.addContainer: one empty container per ticking chunk.
  void addContainer(std::uint64_t key) {
    if (containers.find(key))
      return;
    storage.emplace_back();
    containers.set(key, &storage.back());
  }
  void updateContainerScheduling(std::uint64_t key, std::int64_t trigger) {
    nextTickForContainer.set(key, trigger);
  }
  // LevelChunkTicks.onTickAdded: only a new queue head changes the index.
  void onTickAdded(std::uint64_t key, ChunkTicks &container,
                   const ScheduledTick &tick) {
    const ScheduledTick *head = container.peek();
    if (head && *head == tick)
      updateContainerScheduling(key, head->trigger);
  }
  // LevelTicks.schedule: drop ticks whose chunk has no container.
  bool schedule(std::uint64_t key, const ScheduledTick &tick) {
    auto *container = containers.find(key);
    if (!container)
      return false; // "Trying to schedule tick in not loaded position"
    if (!(*container)->schedule(tick))
      return false;
    onTickAdded(key, **container, tick);
    return true;
  }
  bool restore(std::uint64_t key, const ScheduledTick &tick) {
    auto *container = containers.find(key);
    if (!container)
      return false;
    (*container)->scheduleUnchecked(tick);
    onTickAdded(key, **container, tick);
    return true;
  }
  std::size_t count() const {
    std::size_t total = 0;
    containers.forEach([&](std::uint64_t, ChunkTicks *container) {
      total += container->count();
    });
    return total;
  }
  bool canScheduleMoreTicks(int maxTicks) const {
    return static_cast<int>(toRunThisTick.size()) < maxTicks;
  }
  void pushContainer(const Entry &entry) {
    containersToTick.push_back(entry);
    std::push_heap(containersToTick.begin(), containersToTick.end(),
                   EntryOrder{});
  }
  Entry popContainer() {
    std::pop_heap(containersToTick.begin(), containersToTick.end(),
                  EntryOrder{});
    Entry entry = containersToTick.back();
    containersToTick.pop_back();
    return entry;
  }
  void tick(World &world, std::int64_t gameTime, int maxTicks) {
    collectTicks(world, gameTime, maxTicks);
    while (!toRunThisTick.empty()) {
      ScheduledTick tick = std::move(toRunThisTick.front());
      toRunThisTick.pop_front();
      world.runScheduled(tick);
    }
    cleanupAfterTick();
  }
  void collectTicks(World &world, std::int64_t gameTime, int maxTicks) {
    sortContainersToTick(world, gameTime);
    drainContainers(world, gameTime, maxTicks);
    rescheduleLeftoverContainers();
  }
  // LevelTicks.sortContainersToTick: only chunks whose head is due are visited.
  void sortContainersToTick(World &world, std::int64_t gameTime) {
    dueScratch.clear();
    nextTickForContainer.forEach([&](std::uint64_t key, std::int64_t next) {
      if (next <= gameTime)
        dueScratch.push_back(key);
    });
    for (auto key : dueScratch) {
      auto *container = containers.find(key);
      if (!container) {
        nextTickForContainer.erase(key);
        continue;
      }
      const ScheduledTick *head = (*container)->peek();
      if (!head) {
        nextTickForContainer.erase(key);
        continue;
      }
      if (head->trigger > gameTime) {
        updateContainerScheduling(key, head->trigger);
        continue;
      }
      // LevelTicks tickCheck: chunks that are not ticking keep their container
      // registered (pending ticks resume when the chunk ticks again).
      if (!world.isChunkLoaded(chunkIndex(head->pos.x), chunkIndex(head->pos.z)))
        continue;
      nextTickForContainer.erase(key);
      pushContainer(Entry{key, *container});
    }
  }
  void drainContainers(World &world, std::int64_t gameTime, int maxTicks) {
    while (canScheduleMoreTicks(maxTicks) && !containersToTick.empty()) {
      const Entry entry = popContainer();
      ChunkTicks &container = *entry.container;
      toRunThisTick.push_back(container.poll());
      drainFromCurrentContainer(world, container, gameTime, maxTicks);
      const ScheduledTick *next = container.peek();
      if (next) {
        if (next->trigger <= gameTime && canScheduleMoreTicks(maxTicks))
          pushContainer(entry);
        else
          updateContainerScheduling(entry.key, next->trigger);
      }
    }
  }
  void drainFromCurrentContainer(World &world, ChunkTicks &current,
                                 std::int64_t gameTime, int maxTicks) {
    (void)world;
    if (!canScheduleMoreTicks(maxTicks))
      return;
    const ScheduledTick *nextFromNextContainer =
        containersToTick.empty() ? nullptr : containersToTick.front().container->peek();
    while (canScheduleMoreTicks(maxTicks)) {
      const ScheduledTick *next = current.peek();
      // ScheduledTick.INTRA_TICK_DRAIN_ORDER.compare(next, nextOfNext) > 0
      if (!next || next->trigger > gameTime ||
          (nextFromNextContainer &&
           ScheduledTickIntraOrder{}(*next, *nextFromNextContainer)))
        break;
      toRunThisTick.push_back(current.poll());
    }
  }
  void rescheduleLeftoverContainers() {
    // LevelTicks.rescheduleLeftoverContainers iterates the queue in place.
    for (const Entry &entry : containersToTick) {
      const ScheduledTick *head = entry.container->peek();
      if (head)
        updateContainerScheduling(entry.key, head->trigger);
    }
  }
  void cleanupAfterTick() {
    toRunThisTick.clear();
    containersToTick.clear();
  }
};

// ---------------------------------------------------------------------------
// ServerLevel.blockEvents is an insertion-ordered hash set; unloaded chunks are
// deferred through blockEventsToReschedule and appended afterwards.
// ---------------------------------------------------------------------------
struct World::BlockEventQueue {
  std::deque<BlockEvent> order;
  std::unordered_set<BlockEvent, BlockEventHash> index;
  bool add(const BlockEvent &event) {
    if (!index.insert(event).second)
      return false;
    order.push_back(event);
    return true;
  }
  bool removeFirst(BlockEvent &event) {
    if (order.empty())
      return false;
    event = std::move(order.front());
    order.pop_front();
    index.erase(event);
    return true;
  }
  bool empty() const { return order.empty(); }
  std::size_t size() const { return order.size(); }
  std::vector<BlockEvent> snapshot() const {
    return {order.begin(), order.end()};
  }
};

bool ScheduledTickDrainOrder::operator()(const ScheduledTick &a,
                                         const ScheduledTick &b) const {
  if (a.trigger != b.trigger)
    return a.trigger > b.trigger;
  if (a.priority != b.priority)
    return a.priority > b.priority;
  return a.order > b.order;
}
bool ScheduledTickIntraOrder::operator()(const ScheduledTick &a,
                                         const ScheduledTick &b) const {
  if (a.priority != b.priority)
    return a.priority > b.priority;
  return a.order > b.order;
}
std::size_t BlockEventHash::operator()(const BlockEvent &event) const {
  const auto posHash =
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(event.pos.x)) *
       0x9E3779B97F4A7C15ull) ^
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(event.pos.y)) *
       0xC2B2AE3D27D4EB4Full) ^
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(event.pos.z)) *
       0x165667B19E3779F9ull);
  std::size_t hash =
      static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(event.block.block()));
  hash ^= static_cast<std::size_t>(posHash ^ (posHash >> 32));
  hash ^= static_cast<std::size_t>(event.type) * 0x9E3779B1u;
  hash ^= static_cast<std::size_t>(event.data) * 0x85EBCA6Bu;
  return hash;
}

void World::setSeed(std::uint64_t value) {
  seed = value;
  random.setSeed(static_cast<std::int64_t>(value));
}
void World::setPistonEntity(Pos pos,
                            std::shared_ptr<PistonMovingBlockEntity> entity) {
  pistonEntities[pos] = entity;
  auto it = pistonTickerSlots.find(pos);
  if (it != pistonTickerSlots.end()) {
    it->second->entity = entity;
  } else {
    auto slot = std::make_shared<PistonTickerSlot>();
    slot->pos = pos;
    slot->entity = entity;
    pistonTickerSlots[pos] = slot;
    pistonEntityOrder.push_back(slot);
  }
}
void World::removePistonEntity(Pos pos) {
  pistonEntities.erase(pos);
  auto it = pistonTickerSlots.find(pos);
  if (it != pistonTickerSlots.end()) {
    it->second->entity.reset();
    pistonTickerSlots.erase(it);
  }
}
const World::Chunk *World::chunkAtCached(int x, int z) const {
  const auto key = chunkKey(x, z);
  ChunkCacheSlot &slot = chunkCache_[(key * 0x9E3779B97F4A7C15ull) >> 58];
  if (slot.key == key)
    return slot.chunk && slot.chunk->loaded ? slot.chunk : nullptr;
  auto it = chunks_.find(key);
  slot.key = key;
  slot.chunk = it == chunks_.end() ? nullptr : &it->second;
  return slot.chunk && slot.chunk->loaded ? slot.chunk : nullptr;
}
World::Chunk *World::chunkAt(int x, int z) {
  return const_cast<Chunk *>(chunkAtCached(x, z));
}
const World::Chunk *World::chunkAt(int x, int z) const {
  return chunkAtCached(x, z);
}
World::Section *World::sectionAt(Pos p, bool create) {
  Chunk *c = chunkAt(chunkIndex(p.x), chunkIndex(p.z));
  if (!c)
    return nullptr;
  const auto index = static_cast<std::size_t>(sectionIndex(p.y));
  if (index >= c->sections.size())
    return nullptr;
  auto &slot = c->sections[index];
  if (!slot && create)
    slot = std::make_unique<Section>();
  return slot.get();
}
BlockState::Id World::Section::get(int index) const {
  // PalettedContainer.get: palette entry of the stored index (bits == 0 means
  // the section holds a single value and needs no storage at all).
  if (bits == 0)
    return single;
  const auto entry = bits == 8 ? indices8[index] : indices16[index];
  return palette[entry];
}
BlockState::Id World::Section::set(int index, BlockState::Id state) {
  // PalettedContainer.getAndSet + LevelChunkSection.setBlockState counters.
  const auto previous = get(index);
  if (previous == state)
    return previous;
  if (bits == 0) {
    palette.assign(2, 0);
    palette[0] = single;
    palette[1] = state;
    bits = 8;
    indices8.assign(4096, 0);
    indices8[index] = 1;
  } else if (bits == 8) {
    auto found = std::find(palette.begin(), palette.end(), state);
    if (found == palette.end()) {
      if (palette.size() < 256) {
        palette.push_back(state);
        found = palette.end() - 1;
      } else {
        // HashMapPalette.grow: widen every stored index to 16 bits.
        indices16.assign(4096, 0);
        for (int i = 0; i < 4096; ++i)
          indices16[i] = indices8[i];
        indices8.clear();
        palette.push_back(state);
        bits = 16;
        indices16[index] = static_cast<std::uint16_t>(palette.size() - 1);
        if (BlockState::fromId(previous).isAir() &&
            !BlockState::fromId(state).isAir())
          ++nonEmpty;
        else if (!BlockState::fromId(previous).isAir() &&
                 BlockState::fromId(state).isAir())
          --nonEmpty;
        return previous;
      }
    }
    indices8[index] = static_cast<std::uint8_t>(found - palette.begin());
  } else {
    auto found = std::find(palette.begin(), palette.end(), state);
    if (found == palette.end()) {
      palette.push_back(state);
      found = palette.end() - 1;
    }
    indices16[index] = static_cast<std::uint16_t>(found - palette.begin());
  }
  const bool previousAir = BlockState::fromId(previous).isAir();
  const bool stateAir = BlockState::fromId(state).isAir();
  if (previousAir && !stateAir)
    ++nonEmpty;
  else if (!previousAir && stateAir)
    --nonEmpty;
  return previous;
}
void World::loadChunk(int x, int z) {
  const auto key = chunkKey(x, z);
  auto &c = chunks_[key];
  c.loaded = true;
  // Publish the fresh node so a cached miss cannot mask the load.
  ChunkCacheSlot &slot = chunkCache_[(key * 0x9E3779B97F4A7C15ull) >> 58];
  slot.key = key;
  slot.chunk = &c;
  if (c.sections.size() != static_cast<std::size_t>(sectionCount()))
    c.sections.resize(static_cast<std::size_t>(sectionCount()));
  // LevelTicks.addContainer: every ticking chunk owns exactly one tick
  // container. Reloading a chunk keeps the pending ticks it already had.
  tickQueue_->addContainer(chunkKey(x, z));
}
void World::unloadChunk(int x, int z) {
  auto it = chunks_.find(chunkKey(x, z));
  if (it != chunks_.end())
    it->second.loaded = false;
}
bool World::isChunkLoaded(int x, int z) const { return chunkAt(x, z) != nullptr; }
bool World::isLoaded(Pos p) const {
  // Level.getBlockState: bounds check, then the chunk cache (hash) lookup.
  return p.y >= minY_ && p.y < maxY_ &&
         chunkAt(chunkIndex(p.x), chunkIndex(p.z)) != nullptr;
}
BlockState World::getBlock(Pos p) const {
  if (p.y < minY_ || p.y >= maxY_)
    return airState();
  const Chunk *c = chunkAt(chunkIndex(p.x), chunkIndex(p.z));
  if (!c)
    return airState();
  const auto index = static_cast<std::size_t>(sectionIndex(p.y));
  if (index >= c->sections.size())
    return airState();
  const Section *s = c->sections[index].get();
  if (!s || s->hasOnlyAir())
    return airState();
  const int cell = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  return BlockState::fromId(s->get(cell));
}
void World::rawSetBlock(Pos p, BlockState state) {
  if (p.y < minY_ || p.y >= maxY_)
    return;
  Section *s = sectionAt(p, false);
  if (!s) {
    if (state.isAir())
      return;
    s = sectionAt(p, true);
    if (!s)
      return;
  }
  const int index = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  s->set(index, state.id());
}
std::map<Pos, BlockState> World::allBlocks() const {
  std::map<Pos, BlockState> result;
  for (const auto &[key, c] : chunks_) {
    if (!c.loaded)
      continue;
    const int chunkX = static_cast<int>(key >> 32);
    const int chunkZ = static_cast<int>(key & 0xffffffffu);
    for (std::size_t si = 0; si < c.sections.size(); ++si) {
      const Section *s = c.sections[si].get();
      if (!s || s->hasOnlyAir())
        continue;
      for (int i = 0; i < 4096; ++i) {
        const auto id = s->get(i);
        BlockState state = BlockState::fromId(id);
        if (state.isAir())
          continue;
        result.emplace(Pos{chunkX * 16 + (i & 15),
                           minY_ + static_cast<int>(si) * 16 +
                               ((i >> 8) & 15),
                           chunkZ * 16 + ((i >> 4) & 15)},
                       state);
      }
    }
  }
  return result;
}
bool World::setBlock(Pos p, BlockState state, int flags, int limit) {
  if (!isLoaded(p))
    return false;
  auto old = getBlock(p);
  if (old == state)
    return false;
  rawSetBlock(p, state);
  // TODO: reproduce chunk heightmaps, lighting, dirty/client notifications,
  // POI, and generic block-entity pre-removal/creation for supported entity
  // blocks.
  if (!old.sameBlock(state) && old.is(Blocks::MOVING_PISTON)) {
    if (!(flags & 256))
      pistonPreRemoveSideEffects(*this, p);
    removePistonEntity(p);
  }
  if (!old.sameBlock(state) && (flags & (1 | 64))) {
    affectNeighborsAfterRemoval(p, old, (flags & 64) != 0);
    if (isRailBlock(old))
      railAffectNeighborsAfterRemoval(*this, p, old, (flags & 64) != 0);
  }
  if (!getBlock(p).sameBlock(state))
    return false;
  if (!(flags & 512)) {
    // FallingBlock.onPlace is a type-change callback, including piston moves.
    if (state.is(Blocks::SAND) && !old.sameBlock(state))
      scheduleTick(p, state.block(), 2);
    if (state.is(Blocks::OBSERVER) && !old.sameBlock(state) &&
        state.get("powered", "false") == "true" &&
        !hasScheduledTick(p, state.block())) {
      setBlock(p, state.with("powered", "false"), 18);
      observerNotify(p, state.with("powered", "false"));
    }
    if (state.is(Blocks::TNT) && !old.sameBlock(state) &&
        hasNeighborSignal(p) && primeTntBlock(p)) {
      // primeTntBlock performs TntBlock.prime; onPlace then removes the block.
    }
    if (piston(state))
      pistonOnPlace(*this, p, state, old);
    if (isRailBlock(state))
      railOnPlace(*this, p, state, old, (flags & 64) != 0);
  }
  if (getBlock(p) == state) {
    if ((flags & 2) && blockUpdateObserver)
      blockUpdateObserver(p, state, flags);
    if (flags & 1)
      updateNeighborsAt(p, old.block());
    if (!(flags & 16) && limit > 0) {
      updateIndirectNeighbourShapes(p, old, flags & ~33, limit - 1);
      updateNeighbourShapes(p, state, flags & ~33, limit - 1);
      updateIndirectNeighbourShapes(p, state, flags & ~33, limit - 1);
    }
  }
  return true;
}
std::optional<BlockState> World::coralPlacementState(Pos pos,
                                                     const BlockState &selected,
                                                     Direction clickedFace) {
  if (!isDeadCoralFan(selected) || clickedFace == Direction::Down)
    return std::nullopt;
  auto name = canonical(selected.name());
  const auto wall = name.find("_coral_wall_fan");
  if (wall != std::string::npos)
    name.replace(wall, 15, "_coral_fan");
  if (clickedFace != Direction::Up) {
    auto floor = name.find("_coral_fan");
    name.replace(floor, 10, "_coral_wall_fan");
  }
  auto state = defaultState(name).with("waterlogged", "false");
  if (clickedFace != Direction::Up)
    state = state.with("facing", directionName(clickedFace));
  return coralCanSurvive(*this, pos, state) ? std::optional<BlockState>(state)
                                            : std::nullopt;
}
BlockState World::updateShapeState(Pos p, const BlockState &state,
                                   Direction direction, Pos neighborPos,
                                   const BlockState &neighbor) {
  // FallingBlock.updateShape schedules on every direction, even if the sand
  // currently has support. Scheduled-tick deduplication preserves the first due time.
  if (state.is(Blocks::SAND))
    scheduleTick(p, state.block(), 2);
  if (isWoolCarpet(state)) {
    auto below = getBlock(p.relative(Direction::Down));
    if (below.isAir())
      return airState();
  }
  if (isDeadCoralFan(state)) {
    const auto supportDirection =
        isDeadCoralWallFan(state)
            ? opposite(directionFromString(state.get("facing", "north")))
            : Direction::Down;
    if (direction == supportDirection && !coralCanSurvive(*this, p, state))
      return airState();
  }
  if (state.is(Blocks::GRASS_BLOCK) && direction == Direction::Up) {
    const bool snowy = neighbor.is(Blocks::SNOW) ||
                       neighbor.is(Blocks::SNOW_BLOCK) ||
                       neighbor.is(Blocks::POWDER_SNOW);
    return state.with("snowy", snowy ? "true" : "false");
  }
  // Water simulation is outside the current scope; waterlogged values persist.
  if (isWallBlock(state) && direction != Direction::Down)
    return wallUpdateShape(*this, p, state, direction, neighbor);
  if (state.is(Blocks::PISTON_HEAD) &&
      opposite(direction) ==
          directionFromString(state.get("facing", "north"))) {
    const auto base = getBlock(neighborPos);
    if (!fittingBase(state, base) &&
        !(base.is(Blocks::MOVING_PISTON) &&
          base.get("facing", "north") == state.get("facing", "north")))
      return airState();
  }
  if (state.is(Blocks::OBSERVER) &&
      directionFromString(state.get("facing", "south")) == direction &&
      state.get("powered", "false") == "false" &&
      !hasScheduledTick(p, state.block()))
    scheduleTick(p, state.block(), 2);
  if (state.is(Blocks::NOTE_BLOCK) &&
      (direction == Direction::Up || direction == Direction::Down)) {
    const auto below = getBlock(p.relative(Direction::Down));
    std::string instrument = "harp";
    if (below.is(Blocks::GLASS))
      instrument = "hat";
    else if (below.is(Blocks::SAND))
      instrument = "snare";
    else if (below.is(Blocks::SMOOTH_STONE) || isWallBlock(below) ||
             below.is(Blocks::WHITE_TERRACOTTA) || below.is(Blocks::BEDROCK) ||
             below.is(Blocks::OBSIDIAN) || below.is(Blocks::OBSERVER) ||
             isDeadCoralFan(below))
      instrument = "basedrum";
    else if (below.is(Blocks::NOTE_BLOCK))
      instrument = "bass";
    // TODO: unknown instrument overrides and skull-backed sounds need registry
    // data.
    return state.with("instrument", instrument);
  }
  // TODO: unsupported blocks retain their stored state; their updateShape is
  // unknown.
  return state;
}
BlockState World::updateFromNeighbourShapes(Pos p, const BlockState &input) {
  auto state = input;
  for (auto d : shapeOrder) {
    auto neighborPos = p.relative(d);
    state = updateShapeState(p, state, d, neighborPos, getBlock(neighborPos));
  }
  return state;
}
void World::updateOrDestroy(Pos p, const BlockState &oldState,
                            const BlockState &newState, int flags, int limit) {
  if (oldState == newState)
    return;
  if (newState.isAir()) {
    // Level.destroyBlock resolves the actual current state, which can differ
    // from the earlier shape result after nested piston callbacks.
    auto current = getBlock(p);
    if (current.isAir())
      return;
    if (!(flags & 32)) {
      for (const auto &drop : blockDrops(current))
        spawnItem(p, drop.id, drop.count);
    }
    // TODO: game events and experience drops.
    setBlock(p, airState(), 3, limit);
  } else
    setBlock(p, newState, flags & ~32, limit);
}
void World::neighborShapeChanged(Pos p, Direction d, Pos neighbor,
                                 const BlockState &state, int flags,
                                 int limit) {
  shapeUpdate(p, d, neighbor, state, flags, limit);
}
void World::updateNeighbourShapes(Pos p, const BlockState &state, int flags,
                                  int limit) {
  for (auto d : shapeOrder)
    shapeUpdate(p.relative(d), opposite(d), p, state, flags, limit);
}
void World::updateIndirectNeighbourShapes(
    Pos, const BlockState &, int,
    int) { /* No supported block currently overrides this hook. */ }
void World::affectNeighborsAfterRemoval(Pos p, const BlockState &state, bool) {
  if (state.is(Blocks::PISTON_HEAD)) {
    auto q =
        p.relative(opposite(directionFromString(state.get("facing", "north"))));
    if (fittingBase(state, getBlock(q))) {
      for (const auto &drop : blockDrops(getBlock(q)))
        spawnItem(q, drop.id, drop.count);
      setBlock(q, airState(), 3);
    }
  }
  if (state.is(Blocks::OBSERVER) && state.get("powered", "false") == "true" &&
      hasScheduledTick(p, state.block()))
    observerNotify(p, state.with("powered", "false"));
}
void World::neighborChanged(Pos p, const BlockRef &source) {
  Update u{};
  u.kind = Update::Kind::Simple;
  u.pos = p;
  u.source = source;
  enqueue(std::move(u));
}
void World::neighborChanged(const BlockState &state, Pos p,
                            const BlockRef &source, bool movedByPiston) {
  Update u{};
  u.kind = Update::Kind::Full;
  u.pos = p;
  u.source = source;
  u.capturedState = state;
  u.movedByPiston = movedByPiston;
  enqueue(std::move(u));
}
void World::updateNeighborsAt(Pos p, const BlockRef &source,
                              std::optional<Direction> skip) {
  Update u{};
  u.kind = Update::Kind::Multi;
  u.pos = p;
  u.source = source;
  u.skip = skip;
  enqueue(std::move(u));
}
void World::shapeUpdate(Pos p, Direction d, Pos neighborPos,
                        const BlockState &neighbor, int flags, int limit) {
  Update u{};
  u.kind = Update::Kind::Shape;
  u.pos = p;
  u.direction = d;
  u.neighborPos = neighborPos;
  u.neighbor = neighbor;
  u.flags = flags;
  u.limit = limit;
  enqueue(std::move(u));
}
void World::enqueue(Update u) {
  bool running = updateCount_ > 0;
  bool tooMany = maxUpdates_ >= 0 && updateCount_ >= maxUpdates_;
  ++updateCount_;
  if (!tooMany) {
    if (running)
      added_.push_back(std::move(u));
    else
      stack_.push_back(std::move(u));
  } else if (updateCount_ - 1 == maxUpdates_)
    log.push_back({"neighbor_update_limit", u.pos, ""});
  if (!running)
    runUpdates();
}
void World::runUpdates() {
  try {
    while (!stack_.empty() || !added_.empty()) {
      for (auto it = added_.rbegin(); it != added_.rend(); ++it)
        stack_.push_back(std::move(*it));
      added_.clear();
      while (added_.empty()) {
        bool more = runNext(stack_.back());
        if (!more) {
          stack_.pop_back();
          break;
        }
      }
    }
  } catch (...) {
    stack_.clear();
    added_.clear();
    updateCount_ = 0;
    throw;
  }
  stack_.clear();
  added_.clear();
  updateCount_ = 0;
}
bool World::runNext(Update &u) {
  if (u.kind == Update::Kind::Simple) {
    executeNeighbor(u.pos, getBlock(u.pos), u.source, false);
    return false;
  }
  if (u.kind == Update::Kind::Full) {
    executeNeighbor(u.pos, u.capturedState, u.source, u.movedByPiston);
    return false;
  }
  if (u.kind == Update::Kind::Multi) {
    while (u.index < 6 && u.skip && neighborOrder[u.index] == *u.skip)
      ++u.index;
    if (u.index >= 6)
      return false;
    auto p = u.pos.relative(neighborOrder[u.index++]);
    auto src = u.source;
    while (u.index < 6 && u.skip && neighborOrder[u.index] == *u.skip)
      ++u.index;
    bool more = u.index < 6;
    executeNeighbor(p, getBlock(p), src, false);
    return more;
  }
  auto state = getBlock(u.pos);
  if (traceUpdates)
    log.push_back({"shape", u.pos, directionName(u.direction)});
  auto next =
      updateShapeState(u.pos, state, u.direction, u.neighborPos, u.neighbor);
  updateOrDestroy(u.pos, state, next, u.flags, u.limit);
  return false;
}
void World::executeNeighbor(Pos p, const BlockState &state,
                            const BlockRef &source, bool movedByPiston) {
  // TODO: orientation and movedByPiston for unsupported blocks.
  (void)movedByPiston;
  if (traceUpdates)
    log.push_back({"neighbor", p, source.str()});
  if (piston(state))
    pistonNeighborChanged(*this, p, state);
  else if (state.is(Blocks::PISTON_HEAD)) {
    auto d = directionFromString(state.get("facing", "north"));
    auto basePos = p.relative(opposite(d));
    auto base = getBlock(basePos);
    if (fittingBase(state, base) ||
        (base.is(Blocks::MOVING_PISTON) &&
         base.get("facing", "north") == state.get("facing", "north")))
      neighborChanged(basePos, source.str());
  } else if (state.is(Blocks::TNT) && hasNeighborSignal(p) && tntExplodes) {
    primeTntBlock(p);
  } else if (isRailBlock(state)) {
    railNeighborChanged(*this, p, state, source, movedByPiston);
  } else if (state.is(Blocks::REDSTONE_LAMP)) {
    const bool lit = state.get("lit", "false") == "true";
    if (lit != hasNeighborSignal(p)) {
      if (lit)
        scheduleTick(p, state.block(), 4);
      else
        setBlock(p, state.with("lit", "true"), 2);
    }
  } else if (state.is(Blocks::NOTE_BLOCK)) {
    bool signal = hasNeighborSignal(p),
         powered = state.get("powered", "false") == "true";
    if (signal != powered) {
      if (signal && getBlock(p.relative(Direction::Up)).isAir())
        blockEvent(p, state.block(), 0, 0);
      setBlock(p, state.with("powered", signal ? "true" : "false"), 3);
    }
  }
}
int World::getSignal(Pos p, Direction d) const {
  auto s = getBlock(p);
  if (s.is(Blocks::REDSTONE_BLOCK))
    return 15;
  if (s.is(Blocks::OBSERVER) && s.get("powered", "false") == "true" &&
      directionFromString(s.get("facing", "south")) == d)
    return 15;
  if (isRailBlock(s) && railSignal(s) > 0)
    return railSignal(s);
  // TODO: generic block registries and redstone wire direct signals.
  if (isRedstoneConductor(s)) {
    for (auto n : neighborOrder) {
      auto q = p.relative(n);
      auto x = getBlock(q);
      if (x.is(Blocks::OBSERVER) && x.get("powered", "false") == "true" &&
          directionFromString(x.get("facing", "south")) == n)
        return 15;
      // DetectorRailBlock.getDirectSignal returns 15 only toward UP. With the
      // SignalGetter convention (direction from the conductor to the source),
      // this reaches the block below a powered detector rail.
      if (n == Direction::Up && x.is(Blocks::DETECTOR_RAIL) &&
          x.get("powered", "false") == "true")
        return 15;
    }
  }
  return 0;
}
bool World::hasNeighborSignal(Pos p) const {
  for (auto d : neighborOrder)
    if (getSignal(p.relative(d), d) > 0)
      return true;
  return false;
}


void World::scheduleTick(Pos p, const BlockRef &block, int delay,
                         int priority) {
  // LevelAccessor.createTick: trigger = gameTime + delay, subTickOrder from the
  // monotonic counter. Callers pass the Block object, so no name lookup runs.
  // when the caller already passes a canonical name.
  if (!isLoaded(p))
    return;
  scheduleTick(ScheduledTick{p, block, time_ + delay, priority,
                             nextSubTickCount()});
}
bool World::scheduleTick(const ScheduledTick &tick) {
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  return tickQueue_->schedule(key, tick);
}
bool World::restoreScheduledTick(const ScheduledTick &tick) {
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  return tickQueue_->restore(key, tick);
}
bool World::hasScheduledTick(Pos p, const BlockRef &block) const {
  const auto key = chunkKey(chunkIndex(p.x), chunkIndex(p.z));
  auto *container = tickQueue_->containers.find(key);
  return container && (*container)->has(p, block);
}
std::size_t World::scheduledTickCount() const { return tickQueue_->count(); }
std::vector<ScheduledTick> World::scheduledTicks() const {
  // Deterministic snapshot: chunk order, then each chunk's own DRAIN_ORDER.
  std::vector<std::uint64_t> keys;
  keys.reserve(tickQueue_->containers.size());
  tickQueue_->containers.forEach(
      [&](std::uint64_t key, ChunkTicks *) { keys.push_back(key); });
  std::sort(keys.begin(), keys.end());
  std::vector<ScheduledTick> ticks;
  for (auto key : keys) {
    auto ordered = (*tickQueue_->containers.find(key))->heap;
    std::sort(ordered.begin(), ordered.end(),
              [](const ScheduledTick &x, const ScheduledTick &y) {
                return std::tie(x.trigger, x.priority, x.order) <
                       std::tie(y.trigger, y.priority, y.order);
              });
    ticks.insert(ticks.end(), ordered.begin(), ordered.end());
  }
  return ticks;
}
std::int64_t World::nextSubTickCount() {
  // Level.nextSubTickCount: subTickCount++ with Java long wraparound.
  const auto value = subTickCount_;
  subTickCount_ = nextSubTick(subTickCount_);
  return value;
}
void World::blockEvent(Pos p, const BlockRef &block, int type, int data) {
  // BlockEventData carries the Block identity; the interned name is our Block.
  blockEventQueue_->add({p, block, type, data});
}
std::size_t World::blockEventCount() const { return blockEventQueue_->size(); }
std::vector<BlockEvent> World::pendingBlockEvents() const {
  return blockEventQueue_->snapshot();
}
void World::observerNotify(Pos p, const BlockState &s) {
  auto d = directionFromString(s.get("facing", "south"));
  auto q = p.relative(opposite(d));
  neighborChanged(q, s.block());
  updateNeighborsAt(q, s.block(), d);
}
void World::runScheduled(const ScheduledTick &t) {
  if (traceUpdates)
    log.push_back({"scheduled_tick", t.pos, t.block.str()});
  auto s = getBlock(t.pos);
  if (t.block.block() != s.blockOrNull())
    return;
  if (s.is(Blocks::OBSERVER)) {
    bool on = s.get("powered", "false") == "true";
    setBlock(t.pos, s.with("powered", on ? "false" : "true"), 2);
    if (!on)
      scheduleTick(t.pos, s.block(), 2);
    observerNotify(t.pos, s);
  } else if (isRailBlock(s)) {
    railScheduledTick(*this, t.pos, s);
  } else if (s.is(Blocks::REDSTONE_LAMP)) {
    if (s.get("lit", "false") == "true" && !hasNeighborSignal(t.pos))
      setBlock(t.pos, s.with("lit", "false"), 2);
  } else if (s.is(Blocks::SAND)) {
    // TODO: FIRE tags and replaceable blocks outside the implemented registry.
    if (t.pos.y >= minY_ &&
        isReplaceable(getBlock(t.pos.relative(Direction::Down))))
      FallingBlockEntity::fall(*this, t.pos, s);
  }
}
// TODO: note particles, sound, game events, skull sounds and event broadcasts.
void World::runEvents() {
  // ServerLevel.runBlockEvents: drain the insertion-ordered set, defer events
  // for chunks that are not ticking, then re-add the deferred ones.
  blockEventsToReschedule_.clear();
  while (!blockEventQueue_->empty()) {
    BlockEvent e;
    blockEventQueue_->removeFirst(e);
    if (!isLoaded(e.pos)) {
      blockEventsToReschedule_.push_back(std::move(e));
      continue;
    }
    if (traceUpdates)
      log.push_back({"block_event", e.pos, e.block.str()});
    const auto s = getBlock(e.pos);
    if (e.block.block() != s.blockOrNull())
      continue;
    bool accepted = false;
    if (piston(s))
      accepted = pistonBlockEvent(*this, e.pos, s, e.type, e.data);
    else if (s.is(Blocks::NOTE_BLOCK)) {
      log.push_back({"note", e.pos,
                     s.get("instrument", "harp") + ":" + s.get("note", "0")});
      accepted = true;
    }
    if (accepted && blockEventObserver)
      blockEventObserver(e);
  }
  for (auto &e : blockEventsToReschedule_)
    blockEventQueue_->add(e);
}
// TODO: random ticks, fluid ticks, weather, and world systems are absent.
void World::tick() {
  ++time_;
  handlingTick_ = true;
  // ServerLevel.tick: blockTicks.tick(gameTime, 65536, this::tickBlock).
  // LevelTicks collects the due ticks first, then runs them; ticks scheduled
  // during execution stay pending for the next tick.
  tickQueue_->tick(*this, time_, 65536);
  // ServerChunkCache.tick broadcasts ChunkHolder's dirty block positions
  // after scheduled ticks and before block events. Event-induced piston
  // source changes stay pending until the following chunk broadcast phase.
  if (blockUpdatesBroadcastObserver)
    blockUpdatesBroadcastObserver();
  runEvents();
  handlingTick_ = false;
  // ServerLevel.tick: entities, then block entities.
  tickEntities();
  if (traceUpdates)
    log.push_back({"block_entities", {}, ""});
  pistonTickBlockEntities(*this);
  if (blockEntityTicker)
    blockEntityTicker(*this);
}
} // namespace redstone
