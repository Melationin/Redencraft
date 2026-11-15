#include "redstone/engine.hpp"
#include "redstone/block_tags.hpp"
#include "redstone/containers.hpp"
#include "redstone/entity.hpp"
#include "redstone/fluid_flow.hpp"
#include "redstone/light.hpp"
#include "redstone/piston.hpp"
#include "redstone/portal.hpp"
#include "redstone/rail.hpp"
#include "redstone/random_tick.hpp"
#include "redstone/redstone.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace redstone {
BlockState rotatePillar(BlockState state, Rotation rotation) {
  if (rotation != Rotation::Clockwise90 &&
      rotation != Rotation::Counterclockwise90)
    return state;
  const auto axis = state.getView("axis");
  if (axis == "x") return state.with("axis", "z");
  if (axis == "z") return state.with("axis", "x");
  return state;
}
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
  return (pistonTypeOf(head) == PistonType::Sticky ? base.is(Blocks::STICKY_PISTON)
                                                   : base.is(Blocks::PISTON)) &&
         extendedOf(base) && facingOf(base) == facingOf(head);
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
Direction directionFromString(std::string_view s) {
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
    add("minecraft:oak_fence_gate");
    add("minecraft:birch_fence_gate");
    add("minecraft:moss_block");
    add("minecraft:stone_bricks");
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
    for (const auto *axe : {"wooden_axe", "copper_axe", "stone_axe", "golden_axe",
                            "iron_axe", "diamond_axe", "netherite_axe"})
      add(axe, 1);
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
// ---------------------------------------------------------------------------
// ScheduledTick.UNIQUE_TICK_HASH hashes the block identity and the position.
// The interned name pointer is our Block reference, so the dedupe key is three
// ints plus a pointer: no string, no ownership, no allocation.
// ---------------------------------------------------------------------------
namespace {
// ScheduledTick.UNIQUE_TICK_HASH hashes the position and the ticked object's
// identity. The target type is the queue's template parameter: Block for block
// ticks and Fluid for fluid ticks, both interned singletons.
constexpr std::uint64_t packTickPos(Pos pos) noexcept {
  return ((static_cast<std::uint64_t>(pos.x) & 0x3FFFFFFull) << 38) |
         ((static_cast<std::uint64_t>(pos.z) & 0x3FFFFFFull) << 12) |
         (static_cast<std::uint64_t>(pos.y) & 0xFFFull);
}
constexpr Pos unpackTickPos(std::uint64_t packed) noexcept {
  return Pos{
      static_cast<int>(static_cast<std::int64_t>(packed) >> 38),
      static_cast<int>(static_cast<std::int64_t>(packed << 52) >> 52),
      static_cast<int>(static_cast<std::int64_t>(packed << 26) >> 38),
  };
}
template <class Target> struct TickKey {
  std::uint64_t packedPos = 0;
  const Target *target = nullptr;
};
static_assert(sizeof(TickKey<Block>) == 16);
template <class Target>
std::size_t tickKeyHash(const TickKey<Target> &key) {
  auto hash = key.packedPos * 0x9E3779B97F4A7C15ull;
  hash ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key.target)) *
          0x9E3779B185EBCA87ull;
  hash ^= hash >> 29;
  hash *= 0x9E3779B97F4A7C15ull;
  hash ^= hash >> 32;
  return static_cast<std::size_t>(hash);
}
template <class Target>
bool tickKeyEquals(const TickKey<Target> &a, const TickKey<Target> &b) {
  return a.packedPos == b.packedPos && a.target == b.target;
}
// Open-addressed set of (packedPos, target) keys with tombstone deletion,
// capacity-preserving clear, and periodic/empty shrink support.
template <class Target> class TickKeySet {
public:
  std::size_t size() const { return count_; }
  std::size_t capacity() const { return slots_.size(); }
  bool contains(const TickKey<Target> &key) const { return slotOf(key) != kNone; }
  bool insert(const TickKey<Target> &key) {
    if (slots_.empty())
      rehash(16);
    if (slotOf(key) != kNone)
      return false;
    if ((used_ + 1) * 3 >= slots_.size() * 2)
      rehash(roundUp(std::max<std::size_t>(16, (count_ + 1) * 2)));
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
  void erase(const TickKey<Target> &key) {
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
  void release() {
    std::vector<TickKey<Target>>{}.swap(slots_);
    std::vector<std::uint8_t>{}.swap(state_);
    mask_ = count_ = used_ = 0;
  }
  void compactIfNeeded() {
    if (count_ == 0) {
      if (!slots_.empty())
        release();
      return;
    }
    const std::size_t cap = slots_.size();
    const std::size_t tombstones = used_ - count_;
    if ((cap > 32 && count_ * 6 <= cap) || (cap > 32 && tombstones * 2 >= cap))
      rehash(roundUp(std::max<std::size_t>(16, (count_ + 1) * 2)));
  }

private:
  static constexpr std::uint8_t kEmpty = 0, kUsed = 1, kDeleted = 2;
  static constexpr std::size_t kNone = static_cast<std::size_t>(-1);
  static std::size_t roundUp(std::size_t value) {
    std::size_t power = 16;
    while (power < value)
      power <<= 1;
    return power;
  }
  std::size_t slotOf(const TickKey<Target> &key) const {
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
    std::vector<TickKey<Target>> oldSlots = std::move(slots_);
    std::vector<std::uint8_t> oldState = std::move(state_);
    slots_.assign(capacity, TickKey<Target>{});
    state_.assign(capacity, static_cast<std::uint8_t>(kEmpty));
    mask_ = capacity - 1;
    count_ = used_ = 0;
    for (std::size_t i = 0; i < oldState.size(); ++i)
      if (oldState[i] == kUsed)
        insert(oldSlots[i]);
  }
  std::vector<TickKey<Target>> slots_;
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
// (position, target identity) dedupe set of ScheduledTick.UNIQUE_TICK_HASH.
// Target is Block for the block queue and Fluid for the fluid queue, so the
// ticked object stays a typed pointer instead of an erased one.
// ---------------------------------------------------------------------------
template <class Target> struct QueuedTick {
  std::uint64_t packedPos = 0;
  const Target *target = nullptr;
  std::int64_t trigger = 0;
  std::int32_t priority = 0;
  std::int32_t order = 0;

  static QueuedTick fromScheduled(const ScheduledTick &tick,
                                  const Target *t) noexcept {
    return QueuedTick{packTickPos(tick.pos), t, tick.trigger,
                      static_cast<std::int32_t>(tick.priority),
                      static_cast<std::int32_t>(tick.order)};
  }
  ScheduledTick toBlockScheduledTick() const {
    return ScheduledTick{unpackTickPos(packedPos),
                         target ? BlockRef(*target) : BlockRef{}, trigger,
                         priority, static_cast<std::int64_t>(order)};
  }
  ScheduledTick toFluidScheduledTick() const {
    return ScheduledTick{unpackTickPos(packedPos), BlockRef{}, trigger,
                         priority, static_cast<std::int64_t>(order)};
  }
};
static_assert(sizeof(QueuedTick<Block>) == 32);
static_assert(sizeof(QueuedTick<Fluid>) == 32);
template <class Target> struct QueuedDrainOrder {
  bool operator()(const QueuedTick<Target> &a, const QueuedTick<Target> &b) const {
    if (a.trigger != b.trigger)
      return a.trigger > b.trigger;
    if (a.priority != b.priority)
      return a.priority > b.priority;
    return a.order > b.order;
  }
};
template <class Target> struct QueuedIntraOrder {
  bool operator()(const QueuedTick<Target> &a, const QueuedTick<Target> &b) const {
    if (a.priority != b.priority)
      return a.priority > b.priority;
    return a.order > b.order;
  }
};

template <class Target> struct ChunkTicksOf {
  std::vector<QueuedTick<Target>> heap; // min-heap under QueuedDrainOrder
  TickKeySet<Target> ticksPerPosition;

  const QueuedTick<Target> *peek() const {
    return heap.empty() ? nullptr : &heap.front();
  }
  std::size_t count() const { return heap.size(); }
  bool has(Pos pos, const Target *target) const {
    return ticksPerPosition.contains(TickKey<Target>{packTickPos(pos), target});
  }
  void releaseStorage() {
    std::vector<QueuedTick<Target>>{}.swap(heap);
    ticksPerPosition.release();
  }
  void compact() {
    if (heap.empty()) {
      if (heap.capacity() > 0 || ticksPerPosition.capacity() > 0)
        releaseStorage();
      return;
    }
    if (heap.capacity() > 32 && heap.size() * 4 <= heap.capacity()) {
      std::vector<QueuedTick<Target>> shrunk;
      shrunk.reserve(std::max<std::size_t>(16, heap.size() * 2));
      shrunk.assign(heap.begin(), heap.end());
      heap.swap(shrunk);
    }
    ticksPerPosition.compactIfNeeded();
  }
  QueuedTick<Target> push(const ScheduledTick &tick, const Target *target) {
    const auto entry = QueuedTick<Target>::fromScheduled(tick, target);
    heap.push_back(entry);
    std::push_heap(heap.begin(), heap.end(), QueuedDrainOrder<Target>{});
    return entry;
  }
  bool schedule(const ScheduledTick &tick, const Target *target,
                QueuedTick<Target> *addedOut = nullptr) {
    const auto packedPos = packTickPos(tick.pos);
    if (!ticksPerPosition.insert(TickKey<Target>{packedPos, target}))
      return false;
    const auto entry = push(tick, target);
    if (addedOut)
      *addedOut = entry;
    return true;
  }
  QueuedTick<Target> scheduleUnchecked(const ScheduledTick &tick,
                                       const Target *target) {
    // LevelChunkTicks.scheduleUnchecked: saved lists may repeat an entry; the
    // duplicates stay in the queue while the set records one of them.
    ticksPerPosition.insert(TickKey<Target>{packTickPos(tick.pos), target});
    return push(tick, target);
  }
  QueuedTick<Target> poll() {
    std::pop_heap(heap.begin(), heap.end(), QueuedDrainOrder<Target>{});
    QueuedTick<Target> entry = heap.back();
    heap.pop_back();
    ticksPerPosition.erase(TickKey<Target>{entry.packedPos, entry.target});
    if (heap.empty() &&
        (heap.capacity() > 64 || ticksPerPosition.capacity() > 64))
      releaseStorage();
    return entry;
  }
};
// ---------------------------------------------------------------------------
// LevelTicks<ScheduledTick>: chunk containers, nextTickForContainer index and
// the containersToTick / toRunThisTick drain state of
// LevelTicks.collectTicks/drainContainers/runCollectedTicks. ServerLevel keeps
// one instance for Block and one for Fluid (`blockTicks` / `fluidTicks`), each
// with its own 65536 budget, so this is templated on the ticked object type.
// ---------------------------------------------------------------------------
template <class Target> struct TickQueueOf {
  using ChunkTicks = ChunkTicksOf<Target>;
  struct Entry {
    std::uint64_t key = 0;
    ChunkTicks *container = nullptr;
  };
  struct EntryOrder {
    bool operator()(const Entry &a, const Entry &b) const {
      const QueuedTick<Target> *left = a.container->peek();
      const QueuedTick<Target> *right = b.container->peek();
      if (!left || !right)
        return left == nullptr && right != nullptr;
      return QueuedIntraOrder<Target>{}(*left, *right);
    }
  };
  // Each container has a stable heap address while mapped. Removal clears
  // transient references and then erases this owning pointer immediately.
  std::deque<std::unique_ptr<ChunkTicks>> storage;
  FlatChunkMap<ChunkTicks *> containers;
  FlatChunkMap<std::int64_t> nextTickForContainer;
  std::vector<Entry> containersToTick; // min-heap under EntryOrder
  std::deque<QueuedTick<Target>> toRunThisTick;
  mutable TickKeySet<Target> toRunThisTickSet;
  mutable bool toRunSetBuilt = false;
  std::vector<std::uint64_t> dueScratch;
  std::uint64_t ticksSinceCompact = 0;

  // LevelTicks.addContainer: one empty container per ticking chunk.
  void addContainer(std::uint64_t key) {
    if (containers.find(key))
      return;
    storage.emplace_back(std::make_unique<ChunkTicks>());
    containers.set(key, storage.back().get());
  }
  // removeContainer clears all transient references and erases the owning
  // unique_ptr, so the container storage is released immediately.
  void removeContainer(std::uint64_t key) {
    auto existing = containers.find(key);
    if (!existing)
      return;
    ChunkTicks *removing = *existing;
    nextTickForContainer.erase(key);
    containersToTick.erase(
        std::remove_if(containersToTick.begin(), containersToTick.end(),
                       [key](const Entry &entry) { return entry.key == key; }),
        containersToTick.end());
    std::make_heap(containersToTick.begin(), containersToTick.end(),
                   EntryOrder{});
    std::erase_if(toRunThisTick, [key](const QueuedTick<Target> &entry) {
      const Pos pos = unpackTickPos(entry.packedPos);
      const std::uint64_t entryKey =
          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pos.x >> 4))
           << 32) |
          static_cast<std::uint32_t>(pos.z >> 4);
      return entryKey == key;
    });
    containers.erase(key);
    auto storageIt =
        std::find_if(storage.begin(), storage.end(),
                     [removing](const auto &candidate) {
                       return candidate.get() == removing;
                     });
    if (storageIt != storage.end())
      storage.erase(storageIt);
  }
  void updateContainerScheduling(std::uint64_t key, std::int64_t trigger) {
    nextTickForContainer.set(key, trigger);
  }
  // LevelChunkTicks.onTickAdded: only a new queue head changes the index.
  void onTickAdded(std::uint64_t key, ChunkTicks &container,
                   const QueuedTick<Target> &tick) {
    const QueuedTick<Target> *head = container.peek();
    if (head && head->packedPos == tick.packedPos &&
        head->target == tick.target && head->trigger == tick.trigger &&
        head->priority == tick.priority && head->order == tick.order)
      updateContainerScheduling(key, head->trigger);
  }
  // LevelTicks.schedule: drop ticks whose chunk has no container.
  bool schedule(std::uint64_t key, const ScheduledTick &tick,
                const Target *target) {
    auto *container = containers.find(key);
    if (!container)
      return false; // "Trying to schedule tick in not loaded position"
    QueuedTick<Target> entry{};
    if (!(*container)->schedule(tick, target, &entry))
      return false;
    onTickAdded(key, **container, entry);
    return true;
  }
  bool restore(std::uint64_t key, const ScheduledTick &tick,
               const Target *target) {
    auto *container = containers.find(key);
    if (!container)
      return false;
    const QueuedTick<Target> entry = (*container)->scheduleUnchecked(tick, target);
    onTickAdded(key, **container, entry);
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
  // `run` is ServerLevel.tickBlock / tickFluid: it re-reads the block or fluid
  // state at the position and only dispatches when the identity still matches.
  template <class Run>
  void tick(World &world, std::int64_t gameTime, int maxTicks, Run &&run) {
    collectTicks(world, gameTime, maxTicks);
    while (!toRunThisTick.empty()) {
      QueuedTick<Target> entry = std::move(toRunThisTick.front());
      toRunThisTick.pop_front();
      if (toRunSetBuilt) {
        const TickKey<Target> key{entry.packedPos, entry.target};
        const bool duplicateRemains = std::any_of(
            toRunThisTick.begin(), toRunThisTick.end(),
            [&](const QueuedTick<Target> &other) {
              return other.packedPos == key.packedPos &&
                     other.target == key.target;
            });
        if (!duplicateRemains)
          toRunThisTickSet.erase(key);
      }
      run(unpackTickPos(entry.packedPos), entry.target);
    }
    cleanupAfterTick();
  }
  bool willTickThisTick(Pos pos, const Target *target) const {
    const auto packedPos = packTickPos(pos);
    if (!toRunSetBuilt && !toRunThisTick.empty()) {
      for (const auto &entry : toRunThisTick)
        toRunThisTickSet.insert(TickKey<Target>{entry.packedPos, entry.target});
      toRunSetBuilt = true;
    }
    return toRunSetBuilt &&
           toRunThisTickSet.contains(TickKey<Target>{packedPos, target});
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
      const QueuedTick<Target> *head = (*container)->peek();
      if (!head) {
        nextTickForContainer.erase(key);
        continue;
      }
      if (head->trigger > gameTime) {
        updateContainerScheduling(key, head->trigger);
        continue;
      }
      const Pos headPos = unpackTickPos(head->packedPos);
      // LevelTicks tickCheck: chunks that are not ticking keep their container
      // registered (pending ticks resume when the chunk ticks again).
      if (!world.isChunkLoaded(headPos.x >> 4, headPos.z >> 4))
        continue;
      if (world.blockTickPositionReady &&
          !world.blockTickPositionReady(headPos)) {
        // Keep the due tick at the head. It becomes runnable when manager
        // readiness changes; no queue rebuild or per-world scan is needed.
        updateContainerScheduling(key, head->trigger);
        continue;
      }
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
      const QueuedTick<Target> *next = container.peek();
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
    const QueuedTick<Target> *nextFromNextContainer =
        containersToTick.empty() ? nullptr : containersToTick.front().container->peek();
    while (canScheduleMoreTicks(maxTicks)) {
      const QueuedTick<Target> *next = current.peek();
      // ScheduledTick.INTRA_TICK_DRAIN_ORDER.compare(next, nextOfNext) > 0
      if (!next || next->trigger > gameTime ||
          (nextFromNextContainer &&
           QueuedIntraOrder<Target>{}(*next, *nextFromNextContainer)))
        break;
      toRunThisTick.push_back(current.poll());
    }
  }
  void rescheduleLeftoverContainers() {
    // LevelTicks.rescheduleLeftoverContainers iterates the queue in place.
    for (const Entry &entry : containersToTick) {
      const QueuedTick<Target> *head = entry.container->peek();
      if (head)
        updateContainerScheduling(entry.key, head->trigger);
    }
  }
  void compactContainers() {
    containers.forEach(
        [](std::uint64_t, ChunkTicks *container) { container->compact(); });
    if (toRunThisTickSet.capacity() > 1024)
      toRunThisTickSet.release();
  }
  void cleanupAfterTick() {
    toRunThisTick.clear();
    containersToTick.clear();
    toRunThisTickSet.clear();
    toRunSetBuilt = false;
    if (++ticksSinceCompact >= 100) {
      ticksSinceCompact = 0;
      compactContainers();
    }
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

World::World(int minY, int maxY, int maxNeighborUpdates)
    : minY_(minY), maxY_(maxY), maxUpdates_(maxNeighborUpdates),
      tickQueue_(std::make_unique<BlockTickQueue>()),
      fluidTickQueue_(std::make_unique<FluidTickQueue>()),
      poiStorage_(std::make_unique<PoiStorage>(minY, maxY)),
      blockEventQueue_(std::make_unique<BlockEventQueue>()) {
  // Chunk section storage is a fixed inline array; reject a taller dimension
  // instead of silently truncating sections or indexing out of bounds.
  if (sectionCount() > kSectionCapacity)
    throw std::invalid_argument(
        "World: vertical range needs more than kSectionCapacity sections");
  dimension_.minY = minY;
  dimension_.height = maxY - minY;
  dimension_.logicalHeight = dimension_.height;
}

World::World(DimensionTypeSpec dimension, int maxNeighborUpdates,
             std::string_view dimensionId)
    : World(dimension.minY, dimension.maxY(), maxNeighborUpdates) {
  validateEngineDimensionType(dimension, kSectionCapacity);
  setDimension(std::move(dimension), dimensionId);
}

void World::setDimension(DimensionTypeSpec dimension,
                         std::string_view dimensionId) {
  validateEngineDimensionType(dimension, kSectionCapacity);
  if (dimension.minY != minY_ || dimension.maxY() != maxY_) {
    throw std::invalid_argument(
        "World::setDimension: vertical range does not match world bounds");
  }
  const std::string resolvedId = dimensionId.empty()
                                     ? (dimension.id.empty()
                                            ? std::string("minecraft:overworld")
                                            : normalizeDimensionId(dimension.id))
                                     : normalizeDimensionId(dimensionId);
  dimension_ = std::move(dimension);
  if (dimension_.id.empty())
    dimension_.id = resolvedId;
  dimensionId_ = resolvedId;
  waterEvaporates = dimension_.waterEvaporates;
  fastLava = dimension_.fastLava;
}
World::~World() { disableLighting(); }
World::World(World &&) noexcept = default;
World &World::operator=(World &&) noexcept = default;

BlockTagRegistry::ReloadResult World::reloadBlockTags(const std::vector<std::filesystem::path> &packs) {
  auto next = std::make_shared<BlockTagRegistry>();
  auto result = next->reload(packs);
  if (result.applied) {
    blockTags_ = std::move(next);
    ++blockTagRevision_;
  }
  return result;
}
std::optional<bool> World::hasBlockTag(const BlockState &state, std::string_view tag) const {
  return blockTags_ ? blockTags_->contains(tag, state.nameView()) : blockHasTag(state, tag);
}

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
  randValue_ = static_cast<std::uint32_t>(
      (value ^ (value >> 32)) * 0x9E3779B9u + 1013904223u);
}
void World::setPistonEntity(Pos pos,
                            std::shared_ptr<PistonMovingBlockEntity> entity) {
  // LevelChunk.setBlockEntity: replacing an entry marks the previous one
  // removed, so a stale owner can never be ticked again.
  auto &current = pistonEntities[pos];
  if (!current) {
    const auto chunk = chunkKey(pos.x >> 4, pos.z >> 4);
    pistonPositionsByChunk_[chunk].push_back(pos);
  }
  if (current && current != entity)
    current->live = false;
  current = std::move(entity);
  // LevelChunk.updateBlockEntityTicker: rebind the ticker that already owns
  // this position (same list slot, same order) instead of appending a new one.
  if (auto it = pistonTickerSlots.find(pos); it != pistonTickerSlots.end()) {
    it->second->entity = current;
    if (pistonEntityMutationObserver)
      pistonEntityMutationObserver(pos);
    return;
  }
  auto slot = std::make_unique<PistonTickerSlot>();
  slot->pos = pos;
  slot->entity = current;
  PistonTickerSlot *raw = slot.get();
  pistonTickerSlots[pos] = raw;
  // Level.addBlockEntityTicker: a ticker created while the pass runs waits in
  // the pending list and first ticks on the following pass.
  (tickingPistonBlockEntities ? pendingPistonTickers : pistonTickers)
      .push_back(std::move(slot));
  if (pistonEntityMutationObserver)
    pistonEntityMutationObserver(pos);
}
void World::removePistonEntity(Pos pos, bool notify) {
  // LevelChunk.removeBlockEntity: the position table drops the entity at once.
  if (auto it = pistonEntities.find(pos); it != pistonEntities.end()) {
    if (it->second)
      it->second->live = false;
    pistonEntities.erase(pos);
    const auto key = chunkKey(pos.x >> 4, pos.z >> 4);
    auto positions = pistonPositionsByChunk_.find(key);
    if (positions != pistonPositionsByChunk_.end()) {
      std::erase(positions->second, pos);
      if (positions->second.empty())
        pistonPositionsByChunk_.erase(positions);
    }
  }
  // removeBlockEntityTicker: rebind the ticker to nothing. The list slot stays
  // until sweepPistonTickers(), and it keeps owning the entity until then, so
  // a pass that already referenced it cannot see it freed.
  if (auto it = pistonTickerSlots.find(pos); it != pistonTickerSlots.end()) {
    it->second->removed = true;
    pistonTickerSlots.erase(pos);
  }
  if (notify && pistonEntityMutationObserver)
    pistonEntityMutationObserver(pos);
}
void World::mergePendingPistonTickers() {
  if (pendingPistonTickers.empty())
    return;
  pistonTickers.reserve(pistonTickers.size() + pendingPistonTickers.size());
  for (auto &slot : pendingPistonTickers)
    pistonTickers.push_back(std::move(slot));
  pendingPistonTickers.clear();
}
void World::sweepPistonTickers() {
  // Level.tickBlockEntities' iterator.remove(), as one compaction: surviving
  // tickers keep their relative registration order.
  std::size_t kept = 0;
  for (std::size_t i = 0; i < pistonTickers.size(); ++i) {
    if (pistonTickers[i]->removed)
      continue;
    if (kept != i)
      pistonTickers[kept] = std::move(pistonTickers[i]);
    ++kept;
  }
  pistonTickers.resize(kept);
}

void World::setContainerEntity(Pos pos,
                               std::shared_ptr<ContainerBlockEntity> entity,
                               bool registerTicker) {
  auto &current = containerEntities[pos];
  if (!current) {
    const auto chunk = chunkKey(pos.x >> 4, pos.z >> 4);
    containerPositionsByChunk_[chunk].push_back(pos);
  }
  if (current && current != entity) {
    current->removed = true;
    current->wakeWatchers(*this);
  }
  current = std::move(entity);
  if (registerTicker && current) {
    registerContainerTicker(pos, current);
  } else if (auto it = containerTickerSlots.find(pos);
             it != containerTickerSlots.end()) {
    it->second->removed = true;
    containerTickerSlots.erase(pos);
  }
}

void World::registerContainerTicker(
    Pos pos, std::shared_ptr<ContainerBlockEntity> entity) {
  if (!entity)
    return;
  if (auto it = containerTickerSlots.find(pos); it != containerTickerSlots.end()) {
    it->second->entity = std::move(entity);
    it->second->sleepMode = ContainerSleepMode::Active;
    it->second->removed = false;
    return;
  }
  auto slot = std::make_unique<ContainerTickerSlot>();
  slot->pos = pos;
  slot->entity = std::move(entity);
  ContainerTickerSlot *raw = slot.get();
  containerTickerSlots[pos] = raw;
  (tickingContainerBlockEntities ? pendingContainerTickers : containerTickers)
      .push_back(std::move(slot));
}

void World::removeContainerEntity(Pos pos, bool dropContents, bool notify) {
  if (auto it = containerEntities.find(pos); it != containerEntities.end()) {
    auto existing = it->second;
    if (existing) {
      if (dropContents)
        dropContainerContents(*this, pos, *existing);
      existing->removed = true;
      existing->wakeWatchers(*this);
    }
    containerEntities.erase(pos);
    const auto key = chunkKey(pos.x >> 4, pos.z >> 4);
    auto positions = containerPositionsByChunk_.find(key);
    if (positions != containerPositionsByChunk_.end()) {
      std::erase(positions->second, pos);
      if (positions->second.empty())
        containerPositionsByChunk_.erase(key);
    }
  }
  if (auto it = containerTickerSlots.find(pos); it != containerTickerSlots.end()) {
    it->second->removed = true;
    containerTickerSlots.erase(pos);
  }
  if (notify) {
    updateNeighborsAt(pos, Blocks::TRAPPED_CHEST);
    updateNeighborsAt(pos.relative(Direction::Down), Blocks::TRAPPED_CHEST);
  }
}

std::shared_ptr<ContainerBlockEntity> World::containerEntity(Pos pos) const {
  auto it = containerEntities.find(pos);
  return it == containerEntities.end() ? nullptr : it->second;
}

void World::mergePendingContainerTickers() {
  if (pendingContainerTickers.empty())
    return;
  containerTickers.reserve(containerTickers.size() +
                           pendingContainerTickers.size());
  for (auto &slot : pendingContainerTickers)
    containerTickers.push_back(std::move(slot));
  pendingContainerTickers.clear();
}

void World::sweepContainerTickers() {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < containerTickers.size(); ++i) {
    if (containerTickers[i]->removed)
      continue;
    if (kept != i)
      containerTickers[kept] = std::move(containerTickers[i]);
    ++kept;
  }
  containerTickers.resize(kept);
}

void World::watchHopperBlock(Pos pos,
                             const std::shared_ptr<HopperBlockEntity> &hopper) {
  if (!hopper)
    return;
  auto &list = hopperBlockWatchersByPos_[pos];
  for (const auto &w : list) {
    if (w.lock().get() == hopper.get())
      return;
  }
  list.push_back(hopper);
}

void World::watchHopperItemSection(
    std::uint64_t sectionKey,
    const std::shared_ptr<HopperBlockEntity> &hopper) {
  if (!hopper)
    return;
  auto &list = hopperItemWatchersBySection_[sectionKey];
  for (const auto &w : list) {
    if (w.lock().get() == hopper.get())
      return;
  }
  list.push_back(hopper);
}

void World::wakeHopperBlockWatchers(Pos pos) {
  auto it = hopperBlockWatchersByPos_.find(pos);
  if (it == hopperBlockWatchersByPos_.end())
    return;
  auto watchers = std::move(it->second);
  hopperBlockWatchersByPos_.erase(pos);
  for (const auto &weak : watchers) {
    if (auto h = weak.lock()) {
      if (!h->removed)
        h->wake(*this);
    }
  }
}

void World::wakeHopperItemWatchers(Vec3 pos) {
  const int sx = static_cast<int>(std::floor(pos.x)) >> 4;
  const int sy = static_cast<int>(std::floor(pos.y)) >> 4;
  const int sz = static_cast<int>(std::floor(pos.z)) >> 4;
  const std::uint64_t key = (static_cast<std::uint64_t>(sx) & 0x3FFFFFFull) << 38 |
                            (static_cast<std::uint64_t>(sz) & 0x3FFFFFFull) << 12 |
                            (static_cast<std::uint64_t>(sy) & 0xFFFull);
  auto it = hopperItemWatchersBySection_.find(key);
  if (it == hopperItemWatchersBySection_.end())
    return;
  auto watchers = std::move(it->second);
  hopperItemWatchersBySection_.erase(key);
  for (const auto &weak : watchers) {
    if (auto h = weak.lock()) {
      if (!h->removed)
        h->wake(*this);
    }
  }
}
const World::Chunk *World::chunkAtCached(int x, int z) const {
  const auto key = chunkKey(x, z);
  ChunkCacheSlot &slot = blockReadCache_.chunks[(key * 0x9E3779B97F4A7C15ull) >> 58];
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
BlockState::Id World::Section::get(int index) const {
  // PalettedContainer.get: palette entry of the stored index (bits == 0 means
  // the section holds a single value and needs no storage at all).
  const SectionStorage *base = storage.get();
  if (!base)
    return single;
  const auto cell = static_cast<std::size_t>(index);
  return visitSectionStorage(base, [cell](const auto *block) {
    return block->palette[block->indexAt(cell)];
  });
}
bool World::Section::tryGetRandomlyTicking(int index,
                                          BlockState::Id &outId) const {
  const SectionStorage *base = storage.get();
  if (!base) {
    if (!stateIsRandomlyTickingId(single))
      return false;
    outId = single;
    return true;
  }
  const auto cell = static_cast<std::size_t>(index);
  return visitSectionStorage(base, [cell, &outId](const auto *block) {
    const auto idx = block->indexAt(cell);
    if (!block->paletteTicking[idx])
      return false;
    outId = block->palette[idx];
    return true;
  });
}
void World::Section::initSingleRandomTick(BlockState::Id state) {
  const bool ticking = stateIsRandomlyTickingId(state);
  tickingBlockCount = ticking ? static_cast<std::uint16_t>(kLightVolume) : 0;
  if (ticking) {
    randomTickByMinisection.fill(
        static_cast<std::uint8_t>(kRandomTickMinisectionSize));
    randomTickByMinisection.back() =
        static_cast<std::uint8_t>(kRandomTickLastMinisectionSize);
  } else {
    randomTickByMinisection.fill(0);
  }
}
void World::Chunk::refreshRandomTickMask(int count) {
  std::uint32_t mask = 0;
  for (int i = 0; i < count; ++i) {
    if (sections[static_cast<std::size_t>(i)].isRandomlyTicking())
      mask |= (std::uint32_t{1} << i);
  }
  randomTickingSectionMask = mask;
}
void World::Chunk::refreshPersistedLightIndex(int minSectionY,
                                              int numSections) {
  persistedLightSectionIndex.fill(-1);
  if (!persistedLight || !persistedLight->isLightOn)
    return;
  for (std::size_t i = 0; i < persistedLight->sections.size(); ++i) {
    const int rel = persistedLight->sections[i].sectionY - minSectionY;
    if (rel >= 0 && rel < numSections &&
        persistedLightSectionIndex[static_cast<std::size_t>(rel)] < 0) {
      persistedLightSectionIndex[static_cast<std::size_t>(rel)] =
          static_cast<std::int16_t>(i);
    }
  }
}
namespace {
std::uint8_t nextSectionBitsForWidth(std::uint8_t bits) noexcept {
  if (bits < 7)
    return static_cast<std::uint8_t>(bits + 1);
  if (bits == 7)
    return 8;
  return 16;
}
void updateSectionCounters(
    std::uint16_t &nonEmpty, std::uint16_t &tickingBlockCount,
    std::array<std::uint8_t, World::kRandomTickMinisectionCount> &minisections,
    int index, BlockState::Id previous, BlockState::Id state) {
  const bool previousAir = BlockState::fromId(previous).isAir();
  const bool stateAir = BlockState::fromId(state).isAir();
  std::uint16_t nextNonEmpty = nonEmpty;
  if (previousAir && !stateAir)
    ++nextNonEmpty;
  else if (!previousAir && stateAir)
    --nextNonEmpty;
  if (nextNonEmpty != nonEmpty)
    std::atomic_ref<std::uint16_t>(nonEmpty).store(nextNonEmpty,
                                                   std::memory_order_relaxed);
  const bool prevTicking = stateIsRandomlyTickingId(previous);
  const bool nextTicking = stateIsRandomlyTickingId(state);
  if (prevTicking != nextTicking) {
    const std::size_t mini =
        static_cast<std::size_t>(index) / World::kRandomTickMinisectionSize;
    if (nextTicking) {
      ++tickingBlockCount;
      ++minisections[mini];
    } else {
      --tickingBlockCount;
      --minisections[mini];
    }
  }
}
} // namespace
BlockState::Id World::Section::set(int index, BlockState::Id state,
                                   bool compact,
                                   std::shared_mutex *resizeMutex) {
  // PalettedContainer.getAndSet + LevelChunkSection.setBlockState counters.
  const auto previous = get(index);
  if (previous == state)
    return previous;
  const auto cell = static_cast<std::size_t>(index);
  SectionStorage *base = storage.get();
  if (!base) {
    // A default block is value-initialised, so every cell already reads
    // palette[0] == single and only the written cell needs an index.
    auto next = makeSectionStorageForBits(compact ? 1 : 8);
    visitSectionStorage(next.get(), [&](auto *block) {
      block->palette[0] = single;
      block->paletteTicking[0] = stateIsRandomlyTickingId(single) ? 1 : 0;
      block->palette[1] = state;
      block->paletteTicking[1] = stateIsRandomlyTickingId(state) ? 1 : 0;
      block->paletteSize = 2;
      block->setIndexAt(cell, 1);
    });
    if (resizeMutex) {
      std::unique_lock lock(*resizeMutex);
      storage = std::move(next);
    } else {
      storage = std::move(next);
    }
  } else {
    visitSectionStorage(base, [&](auto *block) {
      using BlockT = std::remove_pointer_t<decltype(block)>;
      const auto end = block->palette.begin() + block->paletteSize;
      auto found = std::find(block->palette.begin(), end, state);
      std::uint16_t targetIdx = 0;
      if (found == end) {
        if (block->paletteSize < BlockT::kPaletteCapacity) {
          targetIdx = block->paletteSize;
          // Store palette slot BEFORE publishing paletteSize & cell index so a
          // lockless reader observing targetIdx sees an initialized state id.
          std::atomic_ref<BlockState::Id>(block->palette[targetIdx])
              .store(state, std::memory_order_release);
          block->paletteTicking[targetIdx] =
              stateIsRandomlyTickingId(state) ? 1 : 0;
          std::atomic_ref<std::uint16_t>(block->paletteSize)
              .store(static_cast<std::uint16_t>(targetIdx + 1),
                     std::memory_order_release);
        } else {
          if constexpr (std::is_same_v<BlockT, SectionStorage16>) {
            // Unreachable: kLightVolume cells cannot hold more than
            // kLightVolume distinct states. Fail loudly instead of overflowing.
            throw std::logic_error("section palette overflow");
          } else {
            // HashMapPalette.grow: widen stored indices outside lock, then take
            // resizeMutex only to swap storage.
            auto nextStorage =
                makeSectionStorageForBits(nextSectionBitsForWidth(block->bits));
            visitSectionStorage(nextStorage.get(), [&](auto *wider) {
              std::copy(block->palette.begin(), end, wider->palette.begin());
              std::copy(block->paletteTicking.begin(),
                        block->paletteTicking.begin() + block->paletteSize,
                        wider->paletteTicking.begin());
              wider->palette[block->paletteSize] = state;
              wider->paletteTicking[block->paletteSize] =
                  stateIsRandomlyTickingId(state) ? 1 : 0;
              wider->paletteSize =
                  static_cast<std::uint16_t>(block->paletteSize + 1);
              for (std::size_t i = 0; i < kLightVolume; ++i)
                wider->setIndexAt(i, block->indexAt(i));
              wider->setIndexAt(cell, block->paletteSize);
            });
            if (resizeMutex) {
              std::unique_lock lock(*resizeMutex);
              storage = std::move(nextStorage);
            } else {
              storage = std::move(nextStorage);
            }
            return;
          }
        }
      } else {
        targetIdx = static_cast<std::uint16_t>(found - block->palette.begin());
      }
      if (resizeMutex)
        block->atomicSetIndexAt(cell, targetIdx);
      else
        block->setIndexAt(cell, targetIdx);
    });
  }
  updateSectionCounters(nonEmpty, tickingBlockCount, randomTickByMinisection,
                        index, previous, state);
  return previous;
}
bool World::installManagedChunk(
    int x, int z, Chunk &&chunk,
    const std::vector<ScheduledTick> &blockTicks,
    const std::vector<std::pair<ScheduledTick, FluidRef>> &fluidTicks,
    std::span<const std::shared_ptr<PistonMovingBlockEntity>> pistons,
    std::span<const std::shared_ptr<Entity>> itemEntities,
    std::span<const std::shared_ptr<Entity>> tntEntities) {
  const std::uint64_t key = chunkKey(x, z);
  if (chunks_.contains(key))
    return false;
  chunk.loaded = false;
  chunk.randomTickIndex = Chunk::kInvalidRandomTickIndex;
  decltype(chunks_)::iterator insertedIt;
  {
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    insertedIt = chunks_.emplace(key, std::move(chunk)).first;
  }
  insertedIt->second.refreshPersistedLightIndex(minY_ >> 4, sectionCount());
  try {
    tickQueue_->addContainer(key);
    fluidTickQueue_->addContainer(key);
    for (const ScheduledTick &tick : blockTicks) {
      if (!tickQueue_->restore(key, tick, tick.block.block()))
        throw std::logic_error("failed to restore block tick");
    }
    for (const auto &[tick, fluid] : fluidTicks) {
      if (!fluidTickQueue_->restore(key, tick, fluid.fluid()))
        throw std::logic_error("failed to restore fluid tick");
    }
    // Block entities and tickers are installed before loadChunk publishes the
    // optical snapshot and lifecycle edge.
    for (const auto &piston : pistons) {
      if (!piston)
        throw std::logic_error("null moving piston entity");
      setPistonEntity(piston->pos, piston);
    }
    for (const auto &entity : itemEntities) {
      if (!entity)
        throw std::logic_error("null persisted item entity");
      if (!entityUuids_.insert(entity->uuid).second)
        throw std::logic_error("duplicate entity UUID during install");
      entity->id = allocateEntityId();
      entity->removalObserved_ = false;
      entities.push_back(entity);
      entitySectionAdd(*entity);
    }
    for (const auto &entity : tntEntities) {
      if (!entity)
        throw std::logic_error("null persisted tnt entity");
      if (!entityUuids_.insert(entity->uuid).second)
        throw std::logic_error("duplicate entity UUID during install");
      entity->id = allocateEntityId();
      entity->removalObserved_ = false;
      entities.push_back(entity);
      entitySectionAdd(*entity);
    }
    loadChunk(x, z);
    reconcilePoiFromChunk(x, z, insertedIt->second);
  } catch (...) {
    std::vector<std::shared_ptr<Entity>> insertedEntities;
    insertedEntities.reserve(itemEntities.size() + tntEntities.size());
    insertedEntities.insert(insertedEntities.end(), itemEntities.begin(),
                            itemEntities.end());
    insertedEntities.insert(insertedEntities.end(), tntEntities.begin(),
                            tntEntities.end());
    for (const auto &entity : insertedEntities) {
      if (!entity ||
          std::find(entities.begin(), entities.end(), entity) == entities.end())
        continue;
      entityUuids_.erase(entity->uuid);
      entitySectionRemove(*entity);
      std::erase(entities, entity);
    }
    for (const auto &piston : pistons)
      if (piston)
        removePistonEntity(piston->pos, false);
    sweepPistonTickers();
    tickQueue_->removeContainer(key);
    fluidTickQueue_->removeContainer(key);
    removeRandomTickingChunk(insertedIt->second);
    {
      std::unique_lock<std::shared_mutex> resizeLock;
      if (std::shared_mutex *mu = directResizeMutex())
        resizeLock = std::unique_lock<std::shared_mutex>(*mu);
      chunks_.erase(insertedIt);
    }
    blockReadCache_.reset();
    throw;
  }
  return true;
}

bool World::managedChunkTransientUnloadState(
    int x, int z, std::string &reason) const {
  if (handlingTick_) {
    reason = "chunk lifecycle cannot evict during the World tick";
    return true;
  }
  const std::uint64_t key = chunkKey(x, z);
  for (const BlockEvent &event : blockEventQueue_->order) {
    if (chunkKey(event.pos.x >> 4, event.pos.z >> 4) == key) {
      reason = "chunk has a transient pending block event";
      return true;
    }
  }
  for (const BlockEvent &event : blockEventsToReschedule_) {
    if (chunkKey(event.pos.x >> 4, event.pos.z >> 4) == key) {
      reason = "chunk has a transient deferred block event";
      return true;
    }
  }
  return false;
}

bool World::managedChunkPersistenceState(int x, int z,
                                         std::string &reason) const {
  const std::uint64_t key = chunkKey(x, z);
  if (entityCountsByChunk_.contains(key)) {
    for (const Entity *entity : entitiesInSectionRange(x, x, z, z)) {
      if (!dynamic_cast<const ItemEntity *>(entity) &&
          !dynamic_cast<const PrimedTnt *>(entity) &&
          !dynamic_cast<const FallingBlockEntity *>(entity) &&
          !dynamic_cast<const Minecart *>(entity)) {
        reason =
            "chunk contains an entity type outside supported persistence";
        return true;
      }
    }
  }
  if (containerPositionsByChunk_.contains(key)) {
    reason = "chunk contains container block entities outside supported "
             "managed persistence";
    return true;
  }
  return false;
}

bool World::removeManagedChunk(int x, int z, std::string &error,
                               bool allowTransientDrop) {
  if (managedChunkPersistenceState(x, z, error) ||
      (!allowTransientDrop &&
       managedChunkTransientUnloadState(x, z, error)))
    return false;
  const std::uint64_t key = chunkKey(x, z);
  auto found = chunks_.find(key);
  if (found == chunks_.end()) {
    error = "managed chunk is not resident in World";
    return false;
  }
  // Invalidate every direct chunk/section pointer before node destruction.
  blockReadCache_.reset();
  unloadChunk(x, z);
  tickQueue_->removeContainer(key);
  fluidTickQueue_->removeContainer(key);
  std::erase_if(blockEventQueue_->order, [key](const BlockEvent &event) {
    return chunkKey(event.pos.x >> 4, event.pos.z >> 4) == key;
  });
  blockEventQueue_->index.clear();
  for (const BlockEvent &event : blockEventQueue_->order)
    blockEventQueue_->index.insert(event);
  std::erase_if(blockEventsToReschedule_, [key](const BlockEvent &event) {
    return chunkKey(event.pos.x >> 4, event.pos.z >> 4) == key;
  });
  std::vector<Pos> pistonPositions;
  if (auto indexed = pistonPositionsByChunk_.find(key);
      indexed != pistonPositionsByChunk_.end())
    pistonPositions = indexed->second;
  for (const Pos &pos : pistonPositions)
    removePistonEntity(pos, false);
  if (!pistonPositions.empty())
    sweepPistonTickers();
  std::vector<Pos> containerPositions;
  if (auto indexed = containerPositionsByChunk_.find(key);
      indexed != containerPositionsByChunk_.end())
    containerPositions = indexed->second;
  for (const Pos &pos : containerPositions)
    removeContainerEntity(pos, false, false);
  if (!containerPositions.empty())
    sweepContainerTickers();
  const std::vector<Entity *> chunkEntities =
      entitiesInSectionRange(x, x, z, z);
  for (Entity *entity : chunkEntities) {
    entityUuids_.erase(entity->uuid);
    entitySectionRemove(*entity);
    std::erase_if(entities, [entity](const std::shared_ptr<Entity> &owned) {
      return owned.get() == entity;
    });
  }
  removeRandomTickingChunk(found->second);
  if (poiStorage_)
    poiStorage_->removeChunk(x, z);
  {
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    chunks_.erase(found);
  }
  blockReadCache_.reset();
  return true;
}

void World::addRandomTickingChunk(Chunk &chunk, std::uint64_t key) {
  if (chunk.randomTickIndex != Chunk::kInvalidRandomTickIndex)
    return;
  chunk.randomTickIndex = static_cast<std::uint32_t>(randomTickingChunks_.size());
  const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
  const int chunkZ =
      static_cast<int>(static_cast<std::int32_t>(key & 0xffffffffu));
  randomTickingChunks_.push_back(TickingChunkEntry{key, &chunk, chunkX, chunkZ});
  randomTickingDirty_ = true;
}

void World::removeRandomTickingChunk(Chunk &chunk) {
  const std::uint32_t idx = chunk.randomTickIndex;
  if (idx == Chunk::kInvalidRandomTickIndex)
    return;
  const std::size_t lastIdx = randomTickingChunks_.size() - 1;
  if (idx != lastIdx) {
    TickingChunkEntry moved = randomTickingChunks_[lastIdx];
    randomTickingChunks_[idx] = moved;
    moved.chunk->randomTickIndex = idx;
    randomTickingDirty_ = true;
  }
  randomTickingChunks_.pop_back();
  chunk.randomTickIndex = Chunk::kInvalidRandomTickIndex;
}

void World::updateChunkRandomTickMask(Chunk &chunk, std::uint64_t key,
                                      int sectionIdx, bool sectionTicking) {
  if (sectionIdx < 0 || sectionIdx >= kSectionCapacity)
    return;
  const std::uint32_t bit = std::uint32_t{1} << sectionIdx;
  const std::uint32_t prevMask = chunk.randomTickingSectionMask;
  const std::uint32_t nextMask =
      sectionTicking ? (prevMask | bit) : (prevMask & ~bit);
  if (nextMask == prevMask)
    return;
  chunk.randomTickingSectionMask = nextMask;
  if (!chunk.loaded)
    return;
  if (prevMask == 0 && nextMask != 0)
    addRandomTickingChunk(chunk, key);
  else if (prevMask != 0 && nextMask == 0)
    removeRandomTickingChunk(chunk);
}

void World::loadChunk(int x, int z) {
  blockReadCache_.sectionIndex = -1;
  const auto key = chunkKey(x, z);
  auto it = chunks_.find(key);
  if (it == chunks_.end()) {
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    it = chunks_.emplace(key, Chunk{}).first;
  }
  auto &c = it->second;
  const bool wasLoaded = c.loaded;
  c.loaded = true;
  if (!wasLoaded)
    ++c.lightLifecycleEpoch;
  if (c.randomTickingSectionMask != 0)
    addRandomTickingChunk(c, key);
  if (!c.heightmaps)
    c.heightmaps = std::make_unique<ChunkHeightmaps>(minY_, sectionCount() * 16);
  // Publish the fresh node so a cached miss cannot mask the load.
  ChunkCacheSlot &slot = blockReadCache_.chunks[(key * 0x9E3779B97F4A7C15ull) >> 58];
  slot.key = key;
  slot.chunk = &c;
  // LevelTicks.addContainer: every ticking chunk owns exactly one tick
  // container in each queue. Reloading a chunk keeps the pending ticks it
  // already had.
  tickQueue_->addContainer(chunkKey(x, z));
  fluidTickQueue_->addContainer(chunkKey(x, z));
  if (lightService_) {
    flushPendingLightCells();
    if (auto input = makeLightChunkInput(x, z)) {
      const std::uint64_t seq = lightService_->submitChunk(std::move(*input));
      if (seq > 0)
        c.requiredLightGeneration = std::max(c.requiredLightGeneration, seq);
      c.persistedLight.reset();
    }
    if (!wasLoaded && lightChunkLifecycleObserver)
      lightChunkLifecycleObserver(x, z, true);
  }
}
void World::unloadChunk(int x, int z) {
  blockReadCache_.sectionIndex = -1;
  const auto key = chunkKey(x, z);
  auto it = chunks_.find(key);
  const bool wasLoaded = it != chunks_.end() && it->second.loaded;
  if (wasLoaded) {
    it->second.loaded = false;
    removeRandomTickingChunk(it->second);
  }
  if (lightService_) {
    flushPendingLightCells();
    // The lifecycle tombstone invalidates both full snapshots and queued owned
    // cell records from this loaded lifetime.
    lightService_->unloadChunk(x, z);
    if (wasLoaded && lightChunkLifecycleObserver)
      lightChunkLifecycleObserver(x, z, false);
  }
}

LightSectionView World::sectionToLightView(const Section &data,
                                           bool atomicHeader) noexcept {
  LightSectionView view;
  const std::uint16_t nonEmpty =
      atomicHeader
          ? std::atomic_ref<std::uint16_t>(
                const_cast<std::uint16_t &>(data.nonEmpty))
                .load(std::memory_order_relaxed)
          : data.nonEmpty;
  view.empty = nonEmpty == 0;
  const SectionStorage *base = data.storage.get();
  if (!base) {
    view.bits = 0;
    view.single = data.single;
    return view;
  }
  const std::uint16_t paletteSize =
      atomicHeader
          ? std::atomic_ref<std::uint16_t>(
                const_cast<std::uint16_t &>(base->paletteSize))
                .load(std::memory_order_acquire)
          : base->paletteSize;
  view.bits = base->bits;
  if (atomicHeader)
    view.storageOwner = data.storage;
  visitSectionStorage(base, [&](const auto *block) {
    view.palette =
        std::span<const BlockState::Id>(block->palette.data(), paletteSize);
    if constexpr (std::is_same_v<std::remove_cv_t<
                                     std::remove_pointer_t<decltype(block)>>,
                                 SectionStorage8>) {
      view.cells8 = std::span<const std::uint8_t>(block->indices.data(),
                                                  block->indices.size());
    } else if constexpr (std::is_same_v<
                             std::remove_cv_t<
                                 std::remove_pointer_t<decltype(block)>>,
                             SectionStorage16>) {
      view.cells16 = std::span<const std::uint16_t>(block->indices.data(),
                                                    block->indices.size());
    } else {
      view.packedWords = std::span<const std::uint64_t>(block->words.data(),
                                                        block->words.size());
    }
  });
  return view;
}

LightSectionView World::directReadSection(const void *context, int chunkX,
                                          int chunkZ, int sectionY) noexcept {
  const auto *self = static_cast<const World *>(context);
  const int idx = sectionY - (self->minY_ >> 4);
  if (idx < 0 || idx >= self->sectionCount())
    return LightSectionView{};
  std::shared_lock<std::shared_mutex> lock(*self->lightDirectMutex_);
  const auto it = self->chunks_.find(chunkKey(chunkX, chunkZ));
  if (it == self->chunks_.end())
    return LightSectionView{};
  return sectionToLightView(
      it->second.sections[static_cast<std::size_t>(idx)], true);
}

std::unique_ptr<LightChunkInput> World::makeLightChunkInput(int x, int z) const {
  const Chunk *chunk = chunkAt(x, z);
  if (!chunk || !lightService_)
    return nullptr;

  auto input = std::make_unique<LightChunkInput>();
  input->chunkX = x;
  input->chunkZ = z;
  input->lifecycleEpoch = chunk->lightLifecycleEpoch;
  input->minBlockSectionY = minY_ >> 4;
  input->blockSectionCount = sectionCount();
  input->persistedLight = chunk->persistedLight;
  if (lowMemoryMode_) {
    input->directGeometry = true;
    return input;
  }
  const auto optical = lightService_->opticalTable();
  if (!optical)
    throw std::logic_error("light service has no optical table");
  input->geometrySections.reserve(static_cast<std::size_t>(sectionCount()));
  for (int section = 0; section < sectionCount(); ++section) {
    const Section &data = chunk->sections[section];
    input->geometrySections.push_back(
        makeLightGeometrySection(*optical, sectionToLightView(data, false)));
  }
  return input;
}

void World::drainLightPublications() {
  if (lightService_)
    lightService_->drainPublished();
}

int World::getBrightness(Pos pos, int skyDarken) const {
  if (!lightService_ || !isLoaded(pos))
    return 0;
  int block = 0;
  int sky = 0;
  if (const auto view = lightService_->latestLayerChunk(
          LightLayer::Block, chunkIndex(pos.x), chunkIndex(pos.z)))
    block = view->lightValue(LightLayer::Block, pos.x, pos.y, pos.z);
  if (const auto view = lightService_->latestLayerChunk(
          LightLayer::Sky, chunkIndex(pos.x), chunkIndex(pos.z)))
    sky = view->lightValue(LightLayer::Sky, pos.x, pos.y, pos.z);
  sky = std::max(0, sky - std::max(0, skyDarken));
  return std::max(block, sky);
}

void World::setLowMemoryMode(bool enabled) {
  if (lightService_)
    throw std::logic_error(
        "cannot change low-memory mode while lighting is active");
  lowMemoryMode_ = enabled;
}

void World::enableLighting(LightWorkerOptions options) {
  if (lightService_)
    return;
  // Heightmaps and the existing simulation permit an unaligned minY. The light
  // handoff alone requires section alignment because its keys use y >> 4.
  if ((minY_ & 15) != 0)
    throw std::invalid_argument(
        "lighting requires minY aligned to a 16-block section");
  options.hasSkyLight = options.hasSkyLight && dimension_.hasSkyLight;
  if (lowMemoryMode_ && !options.directWorld.context) {
    options.directWorld.context = this;
    options.directWorld.resizeMutex = lightDirectMutex_.get();
    options.directWorld.readSection = &World::directReadSection;
  }
  lightService_ = std::make_unique<LightService>(
      minY_ >> 4, sectionCount(), options);
  lightOptical_ = lightService_->opticalTable().get();
  lightService_->setPublishCallback(
      [this](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        if (managedLightPublishedObserver && snapshot)
          managedLightPublishedObserver(snapshot->chunkX, snapshot->chunkZ);
        if (lightPublicationObserver)
          lightPublicationObserver(std::move(snapshot));
      });
  refreshLightSnapshots();
}

void World::flushPendingLightCells() {
  if (!lightService_ || pendingLightCells_.empty())
    return;
  const std::uint64_t seq =
      lightService_->reportBlockCells(pendingLightCells_);
  for (std::uint64_t key : pendingLightChunkKeys_) {
    if (auto it = chunks_.find(key); it != chunks_.end()) {
      it->second.hasPendingLightCell = false;
      if (seq > 0)
        it->second.requiredLightGeneration =
            std::max(it->second.requiredLightGeneration, seq);
    }
  }
  pendingLightChunkKeys_.clear();
  pendingLightCells_.clear();
}

void World::setChunkPersistedLight(int x, int z, PersistedChunkLight persisted) {
  if (Chunk *c = chunkAt(x, z)) {
    c->persistedLight = std::move(persisted);
    c->refreshPersistedLightIndex(minY_ >> 4, sectionCount());
  }
}

void World::refreshLightSnapshots() {
  if (!lightService_)
    return;
  flushPendingLightCells();
  std::vector<std::uint64_t> keys;
  keys.reserve(chunks_.size());
  for (const auto &[key, chunk] : chunks_)
    if (chunk.loaded)
      keys.push_back(key);
  std::sort(keys.begin(), keys.end());
  for (std::uint64_t key : keys) {
    const int chunkX = static_cast<int>(key >> 32);
    const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
    if (auto input = makeLightChunkInput(chunkX, chunkZ)) {
      const std::uint64_t seq = lightService_->submitChunk(std::move(*input));
      if (Chunk *c = chunkAt(chunkX, chunkZ)) {
        if (seq > 0)
          c->requiredLightGeneration =
              std::max(c->requiredLightGeneration, seq);
        c->persistedLight.reset();
      }
    }
  }
}

void World::disableLighting() {
  for (std::uint64_t key : pendingLightChunkKeys_) {
    if (auto it = chunks_.find(key); it != chunks_.end())
      it->second.hasPendingLightCell = false;
  }
  pendingLightChunkKeys_.clear();
  pendingLightCells_.clear();
  lightOptical_ = nullptr;
  lightService_.reset();
}
bool World::isChunkLoaded(int x, int z) const { return chunkAt(x, z) != nullptr; }
bool World::isLoaded(Pos p) const {
  // Level.getBlockState: bounds check, then the chunk cache (hash) lookup.
  return p.y >= minY_ && p.y < maxY_ &&
         chunkAt(chunkIndex(p.x), chunkIndex(p.z)) != nullptr;
}
const Heightmap *World::heightmap(int chunkX, int chunkZ,
                                  HeightmapType type) const {
  const Chunk *chunk = chunkAt(chunkX, chunkZ);
  return chunk && chunk->heightmaps ? chunk->heightmaps->find(type) : nullptr;
}
int World::height(HeightmapType type, int blockX, int blockZ) const {
  const Heightmap *map = heightmap(chunkIndex(blockX), chunkIndex(blockZ), type);
  return map ? map->height(blockX & 15, blockZ & 15) : minY_ - 1;
}
void World::primeHeightmaps() {
  for (auto &[key, chunk] : chunks_) {
    if (!chunk.loaded)
      continue;
    const int chunkX = static_cast<int>(key >> 32);
    const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
    if (!chunk.heightmaps)
      chunk.heightmaps = std::make_unique<ChunkHeightmaps>(
          minY_, sectionCount() * 16);
    int highestSectionPosition = minY_;
    for (int section = sectionCount(); section-- > 0;) {
      if (!chunk.sections[section].hasOnlyAir()) {
        highestSectionPosition = (minY_ >> 4) * 16 + section * 16 + 16;
        break;
      }
    }
    chunk.heightmaps->prime(
        highestSectionPosition, [this, chunkX, chunkZ](int x, int y, int z) {
          return getBlock({chunkX * 16 + x, y, chunkZ * 16 + z});
        });
  }
}
const World::Section *World::loadedSectionAt(Pos p) const {
  if (p.y < minY_ || p.y >= maxY_)
    return nullptr;
  const int x = chunkIndex(p.x), z = chunkIndex(p.z);
  const auto key = chunkKey(x, z);
  const int index = sectionIndex(p.y);
  auto &cache = blockReadCache_;
  if (cache.sectionIndex == index && cache.sectionKey == key)
    return cache.section;
  const Chunk *c = chunkAt(x, z);
  if (!c || index >= sectionCount())
    return nullptr;
  const Section *s = &c->sections[index];
  cache.sectionKey = key;
  cache.sectionIndex = index;
  cache.section = s;
  return s;
}
std::optional<BlockState> World::getBlockIfLoaded(Pos p) const {
  if (p.y < minY_ || p.y >= maxY_)
    return isChunkLoaded(chunkIndex(p.x), chunkIndex(p.z))
               ? std::optional<BlockState>(airState())
               : std::nullopt;
  const Section *s = loadedSectionAt(p);
  if (!s)
    return std::nullopt;
  if (s->hasOnlyAir())
    return airState();
  const int cell = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  return BlockState::fromId(s->get(cell));
}
BlockState World::getBlock(Pos p) const {
  const Section *s = loadedSectionAt(p);
  if (!s || s->hasOnlyAir())
    return airState();
  const int cell = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  return BlockState::fromId(s->get(cell));
}
ChunkSnapshot World::snapshotChunk(int chunkX, int chunkZ) const {
  ChunkSnapshot snapshot;
  snapshot.minY = minY_;
  snapshot.maxY = maxY_;
  snapshot.sections.resize(static_cast<std::size_t>(sectionCount()));
  if (const Chunk *chunk = chunkAt(chunkX, chunkZ)) {
    for (std::size_t i = 0;
         i < snapshot.sections.size() && i < chunk->sections.size(); ++i) {
      // An untouched section is a default Section (single air, no storage), so
      // it copies over the snapshot's default entry without any branch.
      const Section &section = chunk->sections[i];
      SectionSnapshot &out = snapshot.sections[i];
      out.nonEmpty = section.nonEmpty;
      out.bits = section.bits();
      out.single = section.single;
      // The snapshot stays an owning copy: the network encoder runs off the
      // tick thread, so it must not read the live container.
      if (const SectionStorage *base = section.storage.get()) {
        visitSectionStorage(base, [&](const auto *block) {
          out.palette.assign(block->palette.begin(),
                             block->palette.begin() + block->paletteSize);
          if (block->bits <= 8) {
            out.bits = 8;
            out.indices8.resize(static_cast<std::size_t>(kLightVolume));
            for (std::size_t k = 0; k < kLightVolume; ++k)
              out.indices8[k] = static_cast<std::uint8_t>(block->indexAt(k));
          } else {
            out.bits = 16;
            out.indices16.resize(static_cast<std::size_t>(kLightVolume));
            for (std::size_t k = 0; k < kLightVolume; ++k)
              out.indices16[k] = block->indexAt(k);
          }
        });
      }
    }
  }
  snapshot.pistons = snapshotChunkPistons(chunkX, chunkZ);
  return snapshot;
}
std::vector<PistonSnapshot>
World::snapshotChunkPistons(int chunkX, int chunkZ) const {
  std::vector<PistonSnapshot> result;
  const auto indexed = pistonPositionsByChunk_.find(chunkKey(chunkX, chunkZ));
  if (indexed == pistonPositionsByChunk_.end())
    return result;
  result.reserve(indexed->second.size());
  for (const Pos &pos : indexed->second) {
    const auto found = pistonEntities.find(pos);
    if (found == pistonEntities.end() || !found->second)
      continue;
    const auto &entity = *found->second;
    result.push_back(PistonSnapshot{pos, entity.movedState, entity.direction,
                                    entity.progressO, entity.extending,
                                    entity.source});
  }
  std::sort(result.begin(), result.end(),
            [](const PistonSnapshot &a, const PistonSnapshot &b) {
              return a.pos < b.pos;
            });
  return result;
}
void World::reconcilePoiFromChunk(int cx, int cz, const Chunk &chunk) {
  if (!poiStorage_)
    return;
  const int count = sectionCount();
  std::vector<std::pair<std::uint16_t, PoiTypeId>> found;
  for (int si = 0; si < count; ++si) {
    const Section &sec = chunk.sections[static_cast<std::size_t>(si)];
    if (!sec.storage) {
      const PoiTypeId singlePoi = fastPoiTypeForState(sec.single);
      if (singlePoi == PoiTypeId::None) {
        poiStorage_->markSectionValidEmpty(cx, cz, si);
        continue;
      }
      found.resize(static_cast<std::size_t>(kLightVolume));
      for (int i = 0; i < kLightVolume; ++i)
        found[static_cast<std::size_t>(i)] = {
            static_cast<std::uint16_t>(i), singlePoi};
      poiStorage_->reconcileSectionFromScan(cx, cz, si, found, false);
      continue;
    }

    found.clear();
    bool hasAnyPoi = false;
    visitSectionStorage(sec.storage.get(), [&](const auto *typed) {
      using BlockT = std::remove_pointer_t<decltype(typed)>;
      std::array<PoiTypeId, BlockT::kPaletteCapacity> palettePoi{};
      const std::size_t paletteCount =
          std::min<std::size_t>(typed->paletteSize, BlockT::kPaletteCapacity);
      for (std::size_t p = 0; p < paletteCount; ++p) {
        const PoiTypeId poi = fastPoiTypeForState(typed->palette[p]);
        palettePoi[p] = poi;
        if (poi != PoiTypeId::None)
          hasAnyPoi = true;
      }
      if (!hasAnyPoi)
        return;
      for (int i = 0; i < kLightVolume; ++i) {
        const PoiTypeId poi =
            palettePoi[typed->indexAt(static_cast<std::size_t>(i))];
        if (poi != PoiTypeId::None)
          found.emplace_back(static_cast<std::uint16_t>(i), poi);
      }
    });
    if (!hasAnyPoi) {
      poiStorage_->markSectionValidEmpty(cx, cz, si);
    } else {
      poiStorage_->reconcileSectionFromScan(cx, cz, si, found, false);
    }
  }
}

void World::rawSetBlock(Pos p, BlockState state) {
  if (p.y < minY_ || p.y >= maxY_)
    return;
  const int cx = chunkIndex(p.x);
  const int cz = chunkIndex(p.z);
  Chunk *c = chunkAt(cx, cz);
  if (!c)
    return;
  // Writing the value a fresh section already holds (air, state id 0) is a
  // no-op in Section::set, so an untouched section never materialises storage.
  const int si = sectionIndex(p.y);
  Section &section = c->sections[si];
  const int index = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  const BlockState::Id previous =
      section.set(index, state.id(), lowMemoryMode_, directResizeMutex());
  if (previous != state.id()) {
    updateChunkRandomTickMask(*c, chunkKey(cx, cz), si,
                              section.isRandomlyTicking());
    c->persistedLight.reset();
    if (poiStorage_ &&
        (fastPoiTypeForState(previous) != PoiTypeId::None ||
         fastPoiTypeForState(state.id()) != PoiTypeId::None)) {
      poiStorage_->onBlockStateChange(PoiPos{p.x, p.y, p.z}, previous,
                                      state.id());
    }
    if (managedBlockMutationObserver) {
      const BlockState old = BlockState::fromId(previous);
      managedBlockMutationObserver(p, old, state);
    }
    if (lightService_ &&
        lightOptical_->classForState(previous) !=
            lightOptical_->classForState(state.id())) {
      const LightCellUpdate update{p.x, p.y, p.z, state.id()};
      const std::uint64_t seq = lightService_->reportBlockCell(update);
      if (seq > 0)
        c->requiredLightGeneration = std::max(c->requiredLightGeneration, seq);
    }
  }
}
void World::importSectionBlocks(
    int cx, int cz, int sectionY, std::span<const BlockState::Id> palette,
    std::span<const std::uint16_t, kLightVolume> indices) {
  if (palette.empty() || palette.size() > static_cast<std::size_t>(kLightVolume))
    return;
  Chunk *c = chunkAt(cx, cz);
  if (!c)
    return;
  const int secBaseY = sectionY * 16;
  if ((minY_ & 15) != 0 || secBaseY < minY_ || secBaseY + 15 >= maxY_) {
    for (int i = 0; i < kLightVolume; ++i) {
      const BlockState::Id sid =
          palette.size() == 1 ? palette[0] : palette[indices[i]];
      const BlockState s = BlockState::fromId(sid);
      if (s.isAir())
        continue;
      const int y = secBaseY + (i >> 8);
      if (y >= minY_ && y < maxY_)
        rawSetBlock({cx * 16 + (i & 15), y, cz * 16 + ((i >> 4) & 15)}, s);
    }
    return;
  }
  const int si = sectionIndex(secBaseY);
  Section &sec = c->sections[si];
  blockReadCache_.sectionIndex = -1;
  if (palette.size() == 1) {
    const BlockState::Id sid = palette[0];
    if (BlockState::fromId(sid).isAir()) {
      if (poiStorage_)
        poiStorage_->markSectionValidEmpty(cx, cz, si);
      return;
    }
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    sec.storage.reset();
    sec.single = sid;
    sec.nonEmpty = static_cast<std::uint16_t>(kLightVolume);
    sec.initSingleRandomTick(sid);
    if (poiStorage_) {
      const PoiTypeId singlePoi = fastPoiTypeForState(sid);
      if (singlePoi == PoiTypeId::None) {
        poiStorage_->markSectionValidEmpty(cx, cz, si);
      } else {
        std::vector<std::pair<std::uint16_t, PoiTypeId>> found(kLightVolume);
        for (int i = 0; i < kLightVolume; ++i)
          found[static_cast<std::size_t>(i)] = {
              static_cast<std::uint16_t>(i), singlePoi};
        poiStorage_->reconcileSectionFromScan(cx, cz, si, found, false);
      }
    }
  } else {
    const std::uint8_t targetBits =
        palette.size() > 256
            ? 16
            : (lowMemoryMode_ ? minBitsForPaletteSize(palette.size()) : 8);
    auto block = makeSectionStorageForBits(targetBits);
    std::uint16_t nonEmpty = 0;
    std::uint16_t tickingCount = 0;
    sec.randomTickByMinisection.fill(0);
    std::vector<std::pair<std::uint16_t, PoiTypeId>> poiFound;
    bool hasAnyPoi = false;
    visitSectionStorage(block.get(), [&](auto *typed) {
      using BlockT = std::remove_pointer_t<decltype(typed)>;
      const std::size_t paletteCount =
          std::min(palette.size(), BlockT::kPaletteCapacity);
      typed->paletteSize = static_cast<std::uint16_t>(paletteCount);
      std::array<bool, kLightVolume> isNonAir{};
      std::array<PoiTypeId, kLightVolume> palettePoi{};
      for (std::size_t p = 0; p < paletteCount; ++p) {
        const BlockState::Id sid = palette[p];
        typed->palette[p] = sid;
        typed->paletteTicking[p] = stateIsRandomlyTickingId(sid) ? 1 : 0;
        isNonAir[p] = !BlockState::fromId(sid).isAir();
        const PoiTypeId poi = fastPoiTypeForState(sid);
        palettePoi[p] = poi;
        if (poi != PoiTypeId::None)
          hasAnyPoi = true;
      }
      for (int i = 0; i < kLightVolume; ++i) {
        const std::uint16_t pi = indices[static_cast<std::size_t>(i)];
        typed->setIndexAt(static_cast<std::size_t>(i), pi);
        if (isNonAir[pi])
          ++nonEmpty;
        if (typed->paletteTicking[pi]) {
          ++tickingCount;
          ++sec.randomTickByMinisection[static_cast<std::size_t>(i) /
                                        kRandomTickMinisectionSize];
        }
        if (hasAnyPoi && palettePoi[pi] != PoiTypeId::None)
          poiFound.emplace_back(static_cast<std::uint16_t>(i), palettePoi[pi]);
      }
    });
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    sec.nonEmpty = nonEmpty;
    sec.tickingBlockCount = tickingCount;
    if (nonEmpty == 0) {
      sec.storage.reset();
      sec.single = 0;
    } else {
      sec.storage = std::move(block);
    }
    if (poiStorage_) {
      if (!hasAnyPoi) {
        poiStorage_->markSectionValidEmpty(cx, cz, si);
      } else {
        poiStorage_->reconcileSectionFromScan(cx, cz, si, poiFound, false);
      }
    }
  }
  updateChunkRandomTickMask(*c, chunkKey(cx, cz), si, sec.isRandomlyTicking());
  c->persistedLight.reset();
}
std::map<Pos, BlockState> World::allBlocks() const {
  std::map<Pos, BlockState> result;
  for (const auto &[key, c] : chunks_) {
    if (!c.loaded)
      continue;
    const int chunkX = static_cast<int>(key >> 32);
    const int chunkZ = static_cast<int>(key & 0xffffffffu);
    for (int si = 0; si < sectionCount(); ++si) {
      const Section &s = c.sections[si];
      if (s.hasOnlyAir())
        continue;
      for (int i = 0; i < 4096; ++i) {
        const auto id = s.get(i);
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
  if (p.y < minY_ || p.y >= maxY_)
    return false;
  const int targetCx = chunkIndex(p.x);
  const int targetCz = chunkIndex(p.z);
  Chunk *changedChunk = chunkAt(targetCx, targetCz);
  if (!changedChunk)
    return false;
  const int targetSi = sectionIndex(p.y);
  Section &targetSection = changedChunk->sections[targetSi];
  const int localIndex =
      ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  const BlockState old = targetSection.hasOnlyAir()
                             ? airState()
                             : BlockState::fromId(targetSection.get(localIndex));
  if (old == state)
    return false;
  targetSection.set(localIndex, state.id(), lowMemoryMode_,
                    directResizeMutex());
  updateChunkRandomTickMask(*changedChunk, chunkKey(targetCx, targetCz),
                            targetSi, targetSection.isRandomlyTicking());
  if (poiStorage_ &&
      (fastPoiTypeForState(old.id()) != PoiTypeId::None ||
       fastPoiTypeForState(state.id()) != PoiTypeId::None)) {
    poiStorage_->onBlockStateChange(PoiPos{p.x, p.y, p.z}, old.id(), state.id());
  }
  if (managedBlockMutationObserver)
    managedBlockMutationObserver(p, old, state);
  if (!changedChunk->heightmaps)
    changedChunk->heightmaps = std::make_unique<ChunkHeightmaps>(
        minY_, sectionCount() * 16);
  // The regular 16-aligned layout keeps section grouping and local indices in
  // a fixed relationship. Walk it once for all four maps: empty sections are
  // skipped by their header and non-empty sections expose only the target
  // column through Section::get. The unaligned contract keeps the existing
  // per-cell path because its y&15 local index does not match section bases.
  auto &heightmaps = *changedChunk->heightmaps;
  if ((minY_ & 15) != 0) {
    const int chunkX = chunkIndex(p.x), chunkZ = chunkIndex(p.z);
    heightmaps.update(p.x & 15, p.y, p.z & 15, state,
                      [this, chunkX, chunkZ](int x, int y, int z) {
                        return getBlock(
                            {chunkX * 16 + x, y, chunkZ * 16 + z});
                      });
  } else {
    heightmaps.updateFromColumn(
        p.x & 15, p.y, p.z & 15, state,
        [this, changedChunk](int localX, int startY, int localZ,
                            const std::array<bool, 4> &needed) {
          std::array<int, 4> firstAvailable;
          firstAvailable.fill(minY_);
          std::array<bool, 4> pending = needed;
          int remaining = 0;
          for (bool value : pending)
            remaining += value ? 1 : 0;
          if (remaining == 0 || startY < minY_)
            return firstAvailable;

          HeightmapScanStats *stats =
              heightmapScanStats.enabled ? &heightmapScanStats : nullptr;
          if (stats)
            ++stats->scans;
          const int column = ((localZ & 15) << 4) | (localX & 15);
          for (int sectionY = sectionIndex(startY);
               sectionY >= 0 && startY >= minY_ && remaining != 0;
               --sectionY) {
            if (stats)
              ++stats->visitedSections;
            const Section &section = changedChunk->sections[sectionY];
            const int sectionBase = minY_ + sectionY * 16;
            if (section.hasOnlyAir()) {
              if (stats)
                ++stats->skippedSections;
              startY = sectionBase - 1;
              continue;
            }
            for (; startY >= sectionBase && startY >= minY_ && remaining != 0;
                 --startY) {
              if (stats)
                ++stats->readCells;
              const BlockState below = BlockState::fromId(
                  section.get(((startY & 15) << 8) | column));
              const auto matches = ChunkHeightmaps::matchesFinalMaps(below);
              for (std::size_t i = 0; i < pending.size(); ++i) {
                if (pending[i] && matches[i]) {
                  firstAvailable[i] = startY + 1;
                  pending[i] = false;
                  --remaining;
                }
              }
            }
          }
          return firstAvailable;
        });
  }
  if (lightService_ &&
      lightOptical_->classForState(old.id()) !=
          lightOptical_->classForState(state.id())) {
    changedChunk->persistedLight.reset();
    const LightCellUpdate update{p.x, p.y, p.z, state.id()};
    if (inWorldTick_) {
      pendingLightCells_.push_back(update);
      if (!changedChunk->hasPendingLightCell) {
        changedChunk->hasPendingLightCell = true;
        pendingLightChunkKeys_.push_back(
            chunkKey(chunkIndex(p.x), chunkIndex(p.z)));
      }
    } else {
      const std::uint64_t seq = lightService_->reportBlockCell(update);
      if (seq > 0)
        changedChunk->requiredLightGeneration =
            std::max(changedChunk->requiredLightGeneration, seq);
    }
  }
  // TODO: reproduce dirty/client notifications, POI, and generic block-entity
  // pre-removal/creation for supported entity blocks.
  // The opt-in L-1 light handoff is below; propagation remains TODO(L-2a/L-2b).
  if (!old.sameBlock(state) && old.is(Blocks::MOVING_PISTON)) {
    if (!(flags & 256))
      pistonPreRemoveSideEffects(*this, p);
    removePistonEntity(p);
  }
  if (!old.sameBlock(state) && containerKindForState(old).has_value())
    preRemoveContainerSideEffects(*this, p, old, !(flags & 256));
  if (old.is(Blocks::COMPARATOR) && !state.is(Blocks::COMPARATOR))
    comparatorSignals_.erase(p);
  if (!old.sameBlock(state) && (flags & (1 | 64))) {
    // High-frequency fast path for rails; per-block hooks live on Block.
    if (isRailBlock(old))
      railAffectNeighborsAfterRemoval(*this, p, old, (flags & 64) != 0);
    else
      old.block().affectNeighborsAfterRemoval(*this, p, old, (flags & 64) != 0);
  }
  if (!getBlock(p).sameBlock(state))
    return false;
  if (insideFluidTick_)
    ++fluidWrites_;
  if (!(flags & 512)) {
    // LevelChunk.java:339-341: every write that survives the type check runs
    // onPlace, whatever the old block was. LiquidBlock.onPlace has no
    // same-type early return, so re-levelling water onto water (level 0 -> 1)
    // requests a fresh tick too; the sink's (pos, fluid) dedupe then decides.
    // High-frequency fast paths (air/fluid/piston/rail/falling); remaining blocks
    // dispatch via Block::onPlace function pointer.
    if (state.isAir()) {
    } else if (isLiquidBlock(state)) {
      liquidOnPlace(*this, p, state);
    } else if (piston(state)) {
      pistonOnPlace(*this, p, state, old);
    } else if (isRailBlock(state)) {
      railOnPlace(*this, p, state, old, (flags & 64) != 0);
    } else {
      if (state.block().isFalling() && !old.sameBlock(state))
        scheduleTick(p, state.block(), 2);
      state.block().onPlace(*this, p, state, old, (flags & 64) != 0);
    }
  } else if (containerKindForState(state).has_value()) {
    ensureContainerBlockEntity(*this, p, state);
  }
  if (old != state)
    wakeHopperBlockWatchers(p);
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
  // BaseCoralPlantTypeBlock.getStateForPlacement (which BaseCoralWallFanBlock
  // inherits) reads the fluid of the cell being filled and waterlogs the fan for
  // FluidTags.WATER && isFull() -- BaseCoralPlantTypeBlock.java:57-62. That is a
  // TAG test, not the exact-source test the rails and the walls use: falling
  // water[level=8] decodes to the flowing identity with amount 8 and still
  // satisfies both halves (FluidState.java:57-58 only checks amount == 8).
  const FluidState replacedFluid = fluidStateOf(getBlock(pos));
  const bool wet =
      fluidHasTag(replacedFluid.getType(), FluidTag::Water) && replacedFluid.isFull();
  auto state =
      defaultState(name).with("waterlogged", wet ? "true" : "false");
  if (clickedFace != Direction::Up)
    state = state.with("facing", directionName(clickedFace));
  return coralCanSurvive(*this, pos, state) ? std::optional<BlockState>(state)
                                            : std::nullopt;
}
BlockState World::updateShapeState(Pos p, const BlockState &state,
                                   Direction direction, Pos neighborPos,
                                   const BlockState &neighbor) {
  // High-frequency fast paths: air, fluids, falling tick, waterlogged fluid
  // tick; remaining per-block shape logic dispatches via Block::updateShape.
  if (state.isAir())
    return state;
  // FallingBlock.updateShape schedules on every direction, even if the block
  // currently has support. Scheduled-tick deduplication preserves the first due time.
  if (state.block().isFalling())
    scheduleTick(p, state.block(), 2);
  // LiquidBlock.updateShape: only a source on either side requests a fluid tick.
  if (isLiquidBlock(state)) {
    liquidUpdateShape(*this, p, state, neighbor);
    return state;
  }
  // SimpleWaterloggedBlock.updateShape schedules a water tick when waterlogged.
  if (state.block().isWaterloggedHost() && waterloggedOf(state))
    scheduleFluidTick(p, FluidRef(Fluids::water()),
                      Fluids::water().tickDelay());
  return state.block().updateShape(*this, p, state, direction, neighborPos,
                                   neighbor);
}
bool World::removeBlock(Pos p, bool movedByPiston) {
  // Level.java:276-280 reads the fluid BEFORE the write; for a waterlogged
  // host that is its own water source (SimpleWaterloggedBlock.getFluidState),
  // and for a liquid block it is the fluid the block already is.
  const FluidState fluidState = fluidStateOf(getBlock(p));
  return setBlock(p, fluidState.getType().createLegacyBlock(fluidState),
                  3 | (movedByPiston ? 64 : 0));
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
    // Level.destroyBlock writes the position's fluid legacy block, not air
    // (Level.java:288,298): a waterlogged host that fails its shape update
    // leaves its water behind, and a broken liquid leaves the same fluid.
    const FluidState fluidState = fluidStateOf(current);
    setBlock(p, fluidState.getType().createLegacyBlock(fluidState), 3, limit);
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
void World::updateIndirectNeighbourShapes(Pos p, const BlockState &state,
                                         int flags, int limit) {
  redstoneUpdateIndirectNeighbourShapes(*this, p, state, flags, limit);
}
void World::affectNeighborsAfterRemoval(Pos p, const BlockState &state,
                                        bool movedByPiston) {
  state.block().affectNeighborsAfterRemoval(*this, p, state, movedByPiston);
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
  if (traceUpdates)
    log.push_back({"neighbor", p, source.str()});
  // High-frequency fast paths (air/piston/liquid/rail); remaining blocks
  // dispatch via Block::neighborChanged function pointer.
  if (state.isAir())
    return;
  if (piston(state))
    pistonNeighborChanged(*this, p, state);
  else if (isLiquidBlock(state))
    liquidNeighborChanged(*this, p, state);
  else if (isRailBlock(state))
    railNeighborChanged(*this, p, state, source, movedByPiston);
  else
    state.block().neighborChanged(*this, p, state, source, movedByPiston);
}
int World::getDirectSignal(Pos p, Direction d) const {
  return redstoneBlockDirectSignal(*this, p, getBlock(p), d);
}
int World::getDirectSignalTo(Pos p) const {
  int signal = 0;
  signal = std::max(signal, getDirectSignal(p.relative(Direction::Down),
                                            Direction::Down));
  if (signal >= 15)
    return signal;
  signal =
      std::max(signal, getDirectSignal(p.relative(Direction::Up), Direction::Up));
  if (signal >= 15)
    return signal;
  signal = std::max(
      signal, getDirectSignal(p.relative(Direction::North), Direction::North));
  if (signal >= 15)
    return signal;
  signal = std::max(
      signal, getDirectSignal(p.relative(Direction::South), Direction::South));
  if (signal >= 15)
    return signal;
  signal = std::max(
      signal, getDirectSignal(p.relative(Direction::West), Direction::West));
  if (signal >= 15)
    return signal;
  return std::max(
      signal, getDirectSignal(p.relative(Direction::East), Direction::East));
}
int World::getControlInputSignal(Pos p, Direction d, bool diodesOnly) const {
  const auto state = getBlock(p);
  if (diodesOnly)
    return isDiodeBlock(state) ? getDirectSignal(p, d) : 0;
  if (state.is(Blocks::REDSTONE_BLOCK))
    return 15;
  if (state.is(Blocks::REDSTONE_WIRE))
    return static_cast<int>(propertyValue(state, PropertyKind::Power));
  return isRedstoneSignalSource(*this, state)
             ? redstoneBlockDirectSignal(*this, p, state, d)
             : 0;
}
int World::getSignal(Pos p, Direction d) const {
  const auto s = getBlock(p);
  const int signal = redstoneBlockSignal(*this, p, s, d);
  return isRedstoneConductor(s) ? std::max(signal, getDirectSignalTo(p)) : signal;
}
int World::getBestNeighborSignal(Pos p) const {
  int best = 0;
  for (auto d : neighborOrder) {
    const int signal = getSignal(p.relative(d), d);
    if (signal >= 15)
      return 15;
    if (signal > best)
      best = signal;
  }
  return best;
}
bool World::hasSignal(Pos p, Direction d) const { return getSignal(p, d) > 0; }
bool World::hasNeighborSignal(Pos p) const {
  for (auto d : neighborOrder)
    if (getSignal(p.relative(d), d) > 0)
      return true;
  return false;
}
int World::comparatorOutput(Pos pos) const {
  auto it = comparatorSignals_.find(pos);
  return it == comparatorSignals_.end() ? 0 : it->second;
}
void World::setComparatorOutput(Pos pos, int value) {
  if (value == 0)
    comparatorSignals_.erase(pos);
  else
    comparatorSignals_[pos] = value;
}

void World::updateNeighbourForOutputSignal(Pos pos,
                                           const BlockRef &changedBlock) {
  for (Direction d :
       {Direction::North, Direction::East, Direction::South, Direction::West}) {
    Pos rp = pos.relative(d);
    if (!isLoaded(rp))
      continue;
    BlockState rs = getBlock(rp);
    if (rs.is(Blocks::COMPARATOR)) {
      neighborChanged(rs, rp, changedBlock, false);
    } else if (isRedstoneConductor(rs)) {
      rp = rp.relative(d);
      if (isLoaded(rp)) {
        rs = getBlock(rp);
        if (rs.is(Blocks::COMPARATOR))
          neighborChanged(rs, rp, changedBlock, false);
      }
    }
  }
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
  const bool changed = tickQueue_->schedule(key, tick, tick.block.block());
  if (changed && tickMutationObserver)
    tickMutationObserver(tick.pos);
  return changed;
}
bool World::restoreScheduledTick(const ScheduledTick &tick) {
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  const bool changed = tickQueue_->restore(key, tick, tick.block.block());
  if (changed && tickMutationObserver)
    tickMutationObserver(tick.pos);
  return changed;
}
bool World::hasScheduledTick(Pos p, const BlockRef &block) const {
  const auto key = chunkKey(chunkIndex(p.x), chunkIndex(p.z));
  auto *container = tickQueue_->containers.find(key);
  return container && (*container)->has(p, block.block());
}
bool World::willTickThisTick(Pos p, const BlockRef &block) const {
  return tickQueue_->willTickThisTick(p, block.block());
}
std::size_t World::scheduledTickCount() const { return tickQueue_->count(); }
std::vector<ScheduledTick> World::scheduledTicks() const {
  // Deterministic snapshot: chunk order, then each chunk's own DRAIN_ORDER.
  std::vector<std::uint64_t> keys;
  keys.reserve(tickQueue_->containers.size());
  tickQueue_->containers.forEach(
      [&](std::uint64_t key, BlockTickQueue::ChunkTicks *) { keys.push_back(key); });
  std::sort(keys.begin(), keys.end());
  std::vector<ScheduledTick> ticks;
  for (auto key : keys) {
    auto ordered = (*tickQueue_->containers.find(key))->heap;
    std::sort(ordered.begin(), ordered.end(),
              [](const QueuedTick<Block> &x, const QueuedTick<Block> &y) {
                return std::tie(x.trigger, x.priority, x.order) <
                       std::tie(y.trigger, y.priority, y.order);
              });
    for (const auto &entry : ordered)
      ticks.push_back(entry.toBlockScheduledTick());
  }
  return ticks;
}
ChunkTickSnapshot World::snapshotChunkTicks(int chunkX,
                                                 int chunkZ) const {
  ChunkTickSnapshot result;
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  if (const auto *container = tickQueue_->containers.find(key)) {
    result.blockTicks.reserve((*container)->heap.size());
    for (const auto &entry : (*container)->heap)
      result.blockTicks.push_back(entry.toBlockScheduledTick());
    std::sort(result.blockTicks.begin(), result.blockTicks.end(),
              [](const ScheduledTick &a, const ScheduledTick &b) {
                return std::tie(a.trigger, a.priority, a.order) <
                       std::tie(b.trigger, b.priority, b.order);
              });
  }
  if (const auto *container = fluidTickQueue_->containers.find(key)) {
    result.fluidTicks.reserve((*container)->heap.size());
    for (const auto &entry : (*container)->heap)
      result.fluidTicks.emplace_back(entry.toFluidScheduledTick(),
                                     FluidRef(*entry.target));
    std::sort(result.fluidTicks.begin(), result.fluidTicks.end(),
              [](const auto &a, const auto &b) {
                return std::tie(a.first.trigger, a.first.priority,
                                a.first.order) <
                       std::tie(b.first.trigger, b.first.priority,
                                b.first.order);
              });
  }
  return result;
}

std::size_t World::chunkTickCount(int chunkX, int chunkZ) const {
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  std::size_t result = 0;
  if (const auto *container = tickQueue_->containers.find(key))
    result += (*container)->count();
  if (const auto *container = fluidTickQueue_->containers.find(key))
    result += (*container)->count();
  return result;
}

ChunkTickSnapshot World::takeChunkTicks(int chunkX, int chunkZ) {
  ChunkTickSnapshot result = snapshotChunkTicks(chunkX, chunkZ);
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  if (auto *container = tickQueue_->containers.find(key)) {
    (*container)->releaseStorage();
    tickQueue_->nextTickForContainer.erase(key);
  }
  if (auto *container = fluidTickQueue_->containers.find(key)) {
    (*container)->releaseStorage();
    fluidTickQueue_->nextTickForContainer.erase(key);
  }
  if (tickMutationObserver)
    tickMutationObserver({chunkX * 16, minY_, chunkZ * 16});
  return result;
}

// ---------------------------------------------------------------------------
// ServerLevel.fluidTicks: same LevelTicks structure, keyed on the Fluid
// identity instead of the Block identity.
// ---------------------------------------------------------------------------
bool World::scheduleFluidTick(Pos p, const FluidRef &fluid, int delay,
                              int priority) {
  // LevelAccessor.createTick: trigger = gameTime + delay and the same monotonic
  // subTickOrder counter the block queue uses (Level.nextSubTickCount).
  if (!isLoaded(p) || fluid.empty())
    return false;
  return scheduleFluidTick(ScheduledTick{p, BlockRef{}, time_ + delay, priority,
                                         nextSubTickCount()},
                           fluid);
}
bool World::scheduleFluidTick(const ScheduledTick &tick, const FluidRef &fluid) {
  // A default-constructed FluidRef has no identity; queueing it would leave a
  // null target for the snapshot and the dispatch to dereference. Note that
  // Fluids::empty() is a real identity, so only the null handle is rejected.
  if (fluid.empty())
    return false;
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  const bool changed = fluidTickQueue_->schedule(key, tick, fluid.fluid());
  if (changed && tickMutationObserver)
    tickMutationObserver(tick.pos);
  return changed;
}
bool World::restoreFluidTick(const ScheduledTick &tick, const FluidRef &fluid) {
  if (fluid.empty())
    return false;
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  const bool changed = fluidTickQueue_->restore(key, tick, fluid.fluid());
  if (changed && tickMutationObserver)
    tickMutationObserver(tick.pos);
  return changed;
}
bool World::hasFluidTick(Pos p, const FluidRef &fluid) const {
  const auto key = chunkKey(chunkIndex(p.x), chunkIndex(p.z));
  auto *container = fluidTickQueue_->containers.find(key);
  return container && (*container)->has(p, fluid.fluid());
}
std::size_t World::fluidTickCount() const { return fluidTickQueue_->count(); }
std::vector<std::pair<ScheduledTick, FluidRef>> World::fluidTicks() const {
  std::vector<std::uint64_t> keys;
  keys.reserve(fluidTickQueue_->containers.size());
  fluidTickQueue_->containers.forEach(
      [&](std::uint64_t key, FluidTickQueue::ChunkTicks *) { keys.push_back(key); });
  std::sort(keys.begin(), keys.end());
  std::vector<std::pair<ScheduledTick, FluidRef>> ticks;
  for (auto key : keys) {
    auto ordered = (*fluidTickQueue_->containers.find(key))->heap;
    std::sort(ordered.begin(), ordered.end(),
              [](const QueuedTick<Fluid> &x, const QueuedTick<Fluid> &y) {
                return std::tie(x.trigger, x.priority, x.order) <
                       std::tie(y.trigger, y.priority, y.order);
              });
    for (const auto &entry : ordered)
      ticks.emplace_back(entry.toFluidScheduledTick(), FluidRef(*entry.target));
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
  auto d = directionOf(s, Direction::South);
  auto q = p.relative(opposite(d));
  neighborChanged(q, s.block());
  updateNeighborsAt(q, s.block(), d);
}
// ServerLevel.tickBlock: re-read the position and only dispatch when the block
// identity still matches. A stale tick silently does nothing.
void World::tickBlockAt(Pos pos, const Block *expected) {
  if (tickMutationObserver)
    tickMutationObserver(pos);
  if (traceUpdates)
    log.push_back({"scheduled_tick", pos,
                   expected ? expected->name() : std::string{}});
  const auto s = getBlock(pos);
  if (s.blockOrNull() != expected)
    return;
  if (s.isAir())
    return;
  if (isRailBlock(s))
    railScheduledTick(*this, pos, s);
  else
    s.block().tick(*this, pos, s);
}
// ServerLevel.tickFluid: `fluidState.is(type)` is the exact identity test, not
// the "is a source" test, so a flowing tick must not run source logic and vice
// versa. Flow behaviour itself is task F-3; until then a due tick only records
// itself so the queue can be exercised.
void World::tickFluidAt(Pos pos, const Fluid *expected) {
  if (tickMutationObserver)
    tickMutationObserver(pos);
  ++fluidDispatches_;
  if (!expected)
    return;
  const auto state = getBlock(pos);
  const FluidState fluidState = fluidStateOf(state);
  // An empty fluid has an empty tick (Fluid.java:61-62). A tick registered for
  // minecraft:empty -- which the queue accepts, since it is a real identity --
  // must be consumed without touching the block, otherwise the recompute would
  // see "no neighbours" and write air over whatever is there.
  //
  // Both inert cases are still traced: an entry that reached dispatch and did
  // nothing must be distinguishable from an entry that never existed, and the
  // trace is the only place that distinction survives.
  if (expected->isEmpty() || fluidState.isEmpty()) {
    if (traceUpdates)
      log.push_back({fluidState.is(*expected) ? "fluid_tick"
                                             : "fluid_tick_skipped",
                     pos, expected->name()});
    return;
  }
  // The queue dispatched an entry; only the identity check decides whether the
  // fluid's tick logic runs, so trace both outcomes distinctly.
  if (traceUpdates)
    log.push_back({fluidState.is(*expected) ? "fluid_tick" : "fluid_tick_skipped",
                   pos, expected->name()});
  if (!fluidState.is(*expected))
    return;
  // FlowingFluid.tick: recompute a non-source level, then spread.
  insideFluidTick_ = true;
  try {
    fluidTick(*this, pos, state, fluidState);
  } catch (...) {
    insideFluidTick_ = false;
    throw;
  }
  insideFluidTick_ = false;
}
// TODO: note particles, sound, game events, skull sounds and event broadcasts.
void World::runEvents() {
  // ServerLevel.runBlockEvents: drain the insertion-ordered set, defer events
  // for chunks that are not ticking, then re-add the deferred ones.
  blockEventsToReschedule_.clear();
  while (!blockEventQueue_->empty()) {
    BlockEvent e;
    blockEventQueue_->removeFirst(e);
    if (!isLoaded(e.pos) ||
        (blockTickPositionReady && !blockTickPositionReady(e.pos))) {
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
    else
      accepted = s.block().triggerEvent(*this, e.pos, s, e.type, e.data);
    if (accepted && blockEventObserver)
      blockEventObserver(e);
  }
  for (auto &e : blockEventsToReschedule_)
    blockEventQueue_->add(e);
}
namespace {
int readNibbleLight(const std::optional<std::array<std::uint8_t, 2048>> &layer,
                    Pos pos) {
  if (!layer)
    return 0;
  const std::size_t idx = lightIndex(pos.x, pos.y, pos.z);
  const std::uint8_t byteVal = (*layer)[idx >> 1];
  return static_cast<int>((idx & 1u) ? (byteVal >> 4) : (byteVal & 0x0fu));
}

void dispatchRandomTickCell(World &world, RandomTickAccess &access, Pos pos,
                            const BlockState &state) {
  if (state.isRandomlyTicking().value_or(true)) {
    ++world.randomTickStats.blocksTicked;
    const RandomTickResult res = state.randomTick(access, pos, world.random);
    if (res == RandomTickResult::Applied)
      ++world.randomTickStats.ticksApplied;
    else {
      ++world.randomTickStats.ticksUnsupported;
      if (world.unsupportedRandomTickObserver)
        world.unsupportedRandomTickObserver(pos, state, false);
    }
  }
  const FluidState fluid = fluidStateOf(state);
  if (fluid.isRandomlyTicking()) {
    ++world.randomTickStats.fluidsTicked;
    const RandomTickResult res = fluid.randomTick(access, pos, world.random);
    if (res == RandomTickResult::Applied)
      ++world.randomTickStats.ticksApplied;
    else {
      ++world.randomTickStats.ticksUnsupported;
      if (world.unsupportedRandomTickObserver)
        world.unsupportedRandomTickObserver(pos, state, true);
    }
  }
}

[[noreturn]] [[gnu::cold]] void throwRandomTickError(const char *msg) {
  throw std::logic_error(msg);
}

inline void dispatchRandomTickIndex(World &world, RandomTickAccess &access,
                                    int sectionBlockX, int sectionBaseY,
                                    int sectionBlockZ, int blockIndex,
                                    BlockState::Id id) {
  const Pos targetPos{sectionBlockX + (blockIndex & 15),
                      sectionBaseY + ((blockIndex >> 8) & 15),
                      sectionBlockZ + ((blockIndex >> 4) & 15)};
  dispatchRandomTickCell(world, access, targetPos, BlockState::fromId(id));
}

class WorldRandomTickAccess final : public RandomTickAccess {
public:
  explicit WorldRandomTickAccess(World &world) : world_(world) {}

  BlockState getBlock(Pos pos) const override { return world_.getBlock(pos); }

  std::optional<bool> isFullFluid(Pos pos) const override {
    const auto state = world_.getBlockIfLoaded(pos);
    if (!state)
      return std::nullopt;
    return fluidStateOf(*state).isFull();
  }

  std::optional<bool> isWaterFluid(Pos pos) const override {
    const auto state = world_.getBlockIfLoaded(pos);
    if (!state)
      return std::nullopt;
    return fluidHasTag(fluidStateOf(*state).getType(), FluidTag::Water);
  }

  std::optional<int> lightBlockIntoUp(BlockState from,
                                      Pos above) const override {
    const auto to = world_.getBlockIfLoaded(above);
    if (!to)
      return std::nullopt;
    return lightBlockIntoUpFromStates(from, *to);
  }

  std::optional<int> maxLocalRawBrightness(Pos pos) const override {
    return world_.rawBrightness(pos);
  }

  std::optional<bool> isSnowySetting(Pos above) const override {
    const auto state = world_.getBlockIfLoaded(above);
    if (!state)
      return std::nullopt;
    return world_.hasBlockTag(*state, "minecraft:snow");
  }

  std::optional<bool> canSpreadGrassSurvive(BlockState state,
                                            Pos above) const override {
    const auto aboveState = world_.getBlockIfLoaded(above);
    if (!aboveState)
      return std::nullopt;
    return canStayAliveFromAboveState(state, *aboveState);
  }

  SpreadTargetResult canSpreadGrassTarget(BlockState spreadDefault,
                                          Pos targetAbove) const override {
    const auto aboveState = world_.getBlockIfLoaded(targetAbove);
    if (!aboveState)
      return SpreadTargetResult::Unsupported;
    const auto survives = canStayAliveFromAboveState(spreadDefault, *aboveState);
    if (!survives)
      return SpreadTargetResult::Unsupported;
    if (!*survives)
      return SpreadTargetResult::CannotSpread;
    if (fluidHasTag(fluidStateOf(*aboveState).getType(), FluidTag::Water))
      return SpreadTargetResult::CannotSpread;
    const auto snowy = world_.hasBlockTag(*aboveState, "minecraft:snow");
    if (!snowy)
      return SpreadTargetResult::Unsupported;
    return *snowy ? SpreadTargetResult::SpreadSnowy
                  : SpreadTargetResult::SpreadClear;
  }

  void setBlockAndUpdate(Pos pos, BlockState state) override {
    world_.setBlock(pos, state, 3, 512);
  }

  void setBlock(Pos pos, BlockState state, int flags) override {
    world_.setBlock(pos, state, flags, 512);
  }

  std::optional<bool> growSaplingTree(Pos, BlockState, Random &) override {
    // TODO: TreeGrower / world generation is not implemented yet.
    return std::nullopt;
  }

  std::optional<bool> canMushroomSurvive(Pos pos,
                                         BlockState) const override {
    const Pos belowPos = pos.relative(Direction::Down);
    const auto belowState = world_.getBlockIfLoaded(belowPos);
    if (!belowState)
      return std::nullopt;
    const auto overrides = world_.hasBlockTag(
        *belowState, "minecraft:overrides_mushroom_light_requirement");
    if (overrides && *overrides)
      return true;
    const auto brightness = world_.rawBrightness(pos);
    if (!brightness)
      return std::nullopt;
    return *brightness < 13 && isSolidRender(*belowState);
  }

  int minY() const override { return world_.minY(); }
  int maxBlockY() const override { return world_.maxY() - 1; }
  bool isLoaded(Pos pos) const override { return world_.isLoaded(pos); }
  bool hasChunkAt(Pos pos) const override { return inLoadedColumn(pos); }
  std::optional<int> blockBrightness(Pos pos) const override {
    return world_.blockBrightness(pos);
  }
  std::optional<bool> hasBlockTag(BlockState state,
                                  std::string_view tag) const override {
    return world_.hasBlockTag(state, tag);
  }
  bool isFaceSturdy(Pos pos, BlockState state, Direction dir) const override {
    return faceSturdy(world_, pos, state, dir);
  }
  bool isFaceFullBlockSupportOrCollision(Pos pos, BlockState state,
                                         Direction dir) const override {
    // Support and collision shapes are both checked via faceSturdy for full
    // face attachment; leaves are solid collision cubes in registered shapes.
    return faceSturdy(world_, pos, state, dir);
  }
  bool hasCollisionIntersecting(Pos pos, double x0, double y0, double z0,
                                double x1, double y1,
                                double z1) const override {
    const BlockState state = world_.getBlock(pos);
    const auto intersects = [&](const VoxelShape &boxes) {
      for (const ShapeBox &b : boxes) {
        if (b.x1 > x0 && b.x0 < x1 && b.y1 > y0 && b.y0 < y1 && b.z1 > z0 &&
            b.z0 < z1)
          return true;
      }
      return false;
    };
    return state.is(Blocks::MOVING_PISTON)
               ? intersects(collisionShape(world_, pos, state))
               : intersects(collisionShape(state));
  }
  void removeBlock(Pos pos, bool movedByPiston) override {
    world_.setBlock(pos, Blocks::AIR.defaultState(),
                    movedByPiston ? 67 : 3, 512);
  }
  void neighborChanged(Pos pos, const BlockRef &source) override {
    world_.neighborChanged(pos, source);
  }
  void scheduleTick(Pos pos, const BlockRef &block, int delay) override {
    world_.scheduleTick(pos, block, delay);
  }
  bool hasScheduledTick(Pos pos, const BlockRef &block) const override {
    return world_.hasScheduledTick(pos, block);
  }
  bool canSpreadFireAround(Pos pos) const override {
    return world_.anyPlayerCloseEnoughForFireSpread(pos);
  }
  bool spreadVinesEnabled() const override { return world_.spreadVines; }
  bool waterEvaporates() const override { return world_.waterEvaporates; }
  std::optional<bool> isRainingAt(Pos) const override {
    // TODO: weather and biome precipitation are not implemented yet.
    return std::nullopt;
  }

private:
  static std::optional<int> lightBlockIntoUpFromStates(BlockState from,
                                                       BlockState to) {
    const BlockLightProperties &fromProps = blockLightProperties(from);
    const BlockLightProperties &toProps = blockLightProperties(to);
    if (!fromProps.supported || !toProps.supported)
      return std::nullopt;
    if (lightFaceShapesOcclude(fromProps, toProps, LightDirection::Up))
      return 15;
    return static_cast<int>(toProps.dampening);
  }

  static std::optional<bool> canStayAliveFromAboveState(BlockState state,
                                                        BlockState aboveState) {
    if (aboveState.is(Blocks::SNOW)) {
      if (!aboveState.hasProperty("layers"))
        return std::nullopt;
      if (aboveState.getView("layers") == "1")
        return true;
    }
    if (fluidStateOf(aboveState).isFull())
      return false;
    const auto blocked = lightBlockIntoUpFromStates(state, aboveState);
    if (!blocked)
      return std::nullopt;
    return *blocked < 15;
  }

  bool inLoadedColumn(Pos pos) const {
    return world_.isChunkLoaded(pos.x >> 4, pos.z >> 4);
  }

  World &world_;
};
} // namespace

bool World::anyPlayerCloseEnoughForFireSpread(Pos pos) const {
  if (fireSpreadRadiusAroundPlayer == -1)
    return true;
  if (fireSpreadRadiusAroundPlayer <= 0)
    return false;
  const double maxDistSq = static_cast<double>(fireSpreadRadiusAroundPlayer) *
                           static_cast<double>(fireSpreadRadiusAroundPlayer);
  const double tx = static_cast<double>(pos.x) + 0.5;
  const double ty = static_cast<double>(pos.y) + 0.5;
  const double tz = static_cast<double>(pos.z) + 0.5;
  for (const Vec3 &playerPos : fireSpreadPlayerPositions) {
    const double dx = playerPos.x - tx;
    const double dy = playerPos.y - ty;
    const double dz = playerPos.z - tz;
    if (dx * dx + dy * dy + dz * dz < maxDistSq)
      return true;
  }
  return false;
}

template <class ChunkPtr>
static std::optional<int> readWorldBrightness(const LightService *lightService,
                                              ChunkPtr chunk, Pos pos, int cx,
                                              int cz, int minY, int maxY,
                                              bool blockOnly) {
  if (!chunk)
    return std::nullopt;
  if (lightService) {
    const auto blockView = lightService->latestLayerChunk(LightLayer::Block, cx, cz);
    if (blockView) {
      if (blockOnly) {
        if (pos.y >= maxY || pos.y < minY)
          return 0;
        return blockView->lightValue(LightLayer::Block, pos.x, pos.y, pos.z);
      }
      const auto skyView = lightService->latestLayerChunk(LightLayer::Sky, cx, cz);
      if (skyView) {
        if (pos.y >= maxY)
          return skyView->skyLightEnabled ? 15 : 0;
        if (pos.y < minY)
          return 0;
        const int block = blockView->lightValue(LightLayer::Block, pos.x, pos.y, pos.z);
        const int sky = skyView->lightValue(LightLayer::Sky, pos.x, pos.y, pos.z);
        return std::max(block, sky);
      }
    }
  }
  if (chunk->persistedLight && chunk->persistedLight->isLightOn) {
    if (pos.y >= maxY)
      return blockOnly ? 0 : 15;
    if (pos.y < minY)
      return 0;
    const int syRel = (pos.y >> 4) - (minY >> 4);
    if (syRel < 0 ||
        syRel >= static_cast<int>(chunk->persistedLightSectionIndex.size()))
      return 0;
    const int secIdx =
        chunk->persistedLightSectionIndex[static_cast<std::size_t>(syRel)];
    if (secIdx < 0)
      return 0;
    const PersistedLightSection &sec =
        chunk->persistedLight->sections[static_cast<std::size_t>(secIdx)];
    const int block = readNibbleLight(sec.blockLight, pos);
    if (blockOnly)
      return block;
    return std::max(block, readNibbleLight(sec.skyLight, pos));
  }
  return std::nullopt;
}

std::optional<int> World::rawBrightness(Pos pos) const {
  const int cx = chunkIndex(pos.x);
  const int cz = chunkIndex(pos.z);
  return readWorldBrightness(lightService_.get(), chunkAt(cx, cz), pos, cx, cz,
                             minY_, maxY_, false);
}

std::optional<int> World::blockBrightness(Pos pos) const {
  const int cx = chunkIndex(pos.x);
  const int cz = chunkIndex(pos.z);
  return readWorldBrightness(lightService_.get(), chunkAt(cx, cz), pos, cx, cz,
                             minY_, maxY_, true);
}

std::uint16_t World::sectionTickingBlockCountForTest(int chunkX, int sectionY,
                                                    int chunkZ) const {
  const Chunk *c = chunkAt(chunkX, chunkZ);
  if (!c)
    return 0;
  const int idx = sectionY - (minY_ >> 4);
  if (idx < 0 || idx >= sectionCount())
    return 0;
  return c->sections[static_cast<std::size_t>(idx)].tickingBlockCount;
}

std::uint8_t World::sectionStorageBitsForTest(int chunkX, int sectionY,
                                              int chunkZ) const {
  const Chunk *c = chunkAt(chunkX, chunkZ);
  if (!c)
    return 0;
  const int idx = sectionY - (minY_ >> 4);
  if (idx < 0 || idx >= sectionCount())
    return 0;
  return c->sections[static_cast<std::size_t>(idx)].bits();
}

std::array<std::uint8_t, World::kRandomTickMinisectionCount>
World::sectionRandomTickMinisectionsForTest(int chunkX, int sectionY,
                                            int chunkZ) const {
  std::array<std::uint8_t, kRandomTickMinisectionCount> out{};
  const Chunk *c = chunkAt(chunkX, chunkZ);
  if (!c)
    return out;
  const int idx = sectionY - (minY_ >> 4);
  if (idx < 0 || idx >= sectionCount())
    return out;
  return c->sections[static_cast<std::size_t>(idx)].randomTickByMinisection;
}

int World::nextRandomBlockIndex() {
  // Level.getBlockRandomPos LCG packed directly into YZX cell index [0..4095].
  randValue_ = randValue_ * 3u + 1013904223u;
  const std::uint32_t r = randValue_ >> 2;
  return static_cast<int>((r & 15u) | ((r >> 8) & 0xf00u) |
                          ((r >> 4) & 0xf0u));
}

[[gnu::noinline]] void World::tickSparseRandomTickHit(
    int chunkX, int sectionBaseY, int chunkZ, Section &section, int nth,
    RandomTickAccess &access) {
  int mini = 0;
  for (; mini < kRandomTickMinisectionCount; ++mini) {
    const int countInMini =
        section.randomTickByMinisection[static_cast<std::size_t>(mini)];
    if (nth >= countInMini)
      nth -= countInMini;
    else
      break;
  }
  if (mini >= kRandomTickMinisectionCount)
    throwRandomTickError(
        "random-tick minisection count mismatch during sparse lookup");
  const int sectionBlockX = chunkX * 16;
  const int sectionBlockZ = chunkZ * 16;
  const int start = mini * kRandomTickMinisectionSize;
  const SectionStorage *base = section.storage.get();
  if (!base) {
    if (!stateIsRandomlyTickingId(section.single))
      throwRandomTickError(
          "single-value section marked ticking without ticking state");
    const int blockIndex = start + nth;
    if (blockIndex >= kLightVolume)
      throwRandomTickError("random-tick single-value index out of bounds");
    dispatchRandomTickIndex(*this, access, sectionBlockX, sectionBaseY,
                            sectionBlockZ, blockIndex, section.single);
    return;
  }
  const int end = std::min(start + kRandomTickMinisectionSize, kLightVolume);
  const bool found = visitSectionStorage(base, [&](const auto *block) -> bool {
    for (int blockIndex = start; blockIndex < end; ++blockIndex) {
      const auto idx = block->indexAt(static_cast<std::size_t>(blockIndex));
      if (block->paletteTicking[idx] && nth-- == 0) {
        dispatchRandomTickIndex(*this, access, sectionBlockX, sectionBaseY,
                                sectionBlockZ, blockIndex, block->palette[idx]);
        return true;
      }
    }
    return false;
  });
  if (!found)
    throwRandomTickError(
        "failed to find nth randomly ticking block inside section");
}

[[gnu::noinline]] void World::tickDenseRandomTickSection(
    int chunkX, int sectionBaseY, int chunkZ, Section &section, int tickSpeed,
    RandomTickAccess &access) {
  ++randomTickStats.sectionsDensePath;
  randomTickStats.positionsProbed += static_cast<std::uint64_t>(tickSpeed);
  const int sectionBlockX = chunkX * 16;
  const int sectionBlockZ = chunkZ * 16;
  for (int p = 0; p < tickSpeed; ++p) {
    const int blockIndex = nextRandomBlockIndex();
    BlockState::Id id = 0;
    if (section.tryGetRandomlyTicking(blockIndex, id)) {
      dispatchRandomTickIndex(*this, access, sectionBlockX, sectionBaseY,
                              sectionBlockZ, blockIndex, id);
    }
  }
}

void World::tickRandomChunkSection(int chunkX, int sectionBaseY, int chunkZ,
                                   Section &section, int tickSpeed,
                                   RandomTickAccess &access) {
  std::uint16_t tickingCount = section.tickingBlockCount;
  if (tickingCount == 0)
    return;
  ++randomTickStats.sectionsVisited;

  // Lithium ServerLevelMixin: when ticking blocks <= 384, draw random index r;
  // only if r < tickingCount (N/4096 chance) scan from minisection offset.
  if (tickingCount <= kRandomTickSparseThreshold) {
    ++randomTickStats.sectionsSparsePath;
    randomTickStats.positionsProbed += static_cast<std::uint64_t>(tickSpeed);
    for (int p = 0; p < tickSpeed; ++p) {
      const int r = nextRandomBlockIndex();
      if (r < tickingCount) {
        tickSparseRandomTickHit(chunkX, sectionBaseY, chunkZ, section, r,
                                access);
        tickingCount = section.tickingBlockCount;
      }
    }
    return;
  }

  tickDenseRandomTickSection(chunkX, sectionBaseY, chunkZ, section, tickSpeed,
                             access);
}

void World::tickChunks() {
  // TODO: weather/precipitation/ice-and-snow and remaining chunk-tick systems
  // are absent (requires biomes, climate, and weather state).
  if (randomTickSpeed <= 0 || randomTickingChunks_.empty())
    return;
  if (randomTickingDirty_) {
    std::sort(randomTickingChunks_.begin(), randomTickingChunks_.end(),
              [](const TickingChunkEntry &a, const TickingChunkEntry &b) {
                return a.key < b.key;
              });
    for (std::size_t i = 0; i < randomTickingChunks_.size(); ++i) {
      randomTickingChunks_[i].chunk->randomTickIndex =
          static_cast<std::uint32_t>(i);
    }
    randomTickingDirty_ = false;
  }
  randomTickChunkScratch_ = randomTickingChunks_;
  WorldRandomTickAccess access(*this);
  const int maxSections = sectionCount();
  for (const TickingChunkEntry &entry : randomTickChunkScratch_) {
    Chunk *chunk = entry.chunk;
    if (!chunk->loaded)
      continue;
    std::uint32_t mask = chunk->randomTickingSectionMask;
    if (mask == 0)
      continue;
    if (entityTickPositionReady &&
        !entityTickPositionReady(Pos{entry.x * 16, minY_, entry.z * 16}))
      continue;
    ++randomTickStats.chunksVisited;
    while (mask != 0) {
      const int si = std::countr_zero(mask);
      mask &= (mask - 1);
      if (si >= maxSections)
        break;
      Section &section = chunk->sections[static_cast<std::size_t>(si)];
      if (!section.isRandomlyTicking())
        continue;
      tickRandomChunkSection(entry.x, minY_ + si * 16, entry.z, section,
                             randomTickSpeed, access);
    }
  }
}

// TODO: weather, biomes, and the remaining world systems are absent.
void World::tick() {
  struct TickLightGuard {
    World &world;
    ~TickLightGuard() {
      world.inWorldTick_ = false;
      world.flushPendingLightCells();
    }
  };
  inWorldTick_ = true;
  TickLightGuard tickLightGuard{*this};
  ++time_;
  handlingTick_ = true;
  // ServerLevel.tick: blockTicks.tick(gameTime, 65536, this::tickBlock).
  // LevelTicks collects the due ticks first, then runs them; ticks scheduled
  // during execution stay pending for the next tick.
  tickQueue_->tick(*this, time_, 65536,
                   [this](Pos pos, const Block *block) {
                     tickBlockAt(pos, block);
                   });
  // ServerLevel.tick: fluidTicks.tick(gameTime, 65536, this::tickFluid) runs
  // right after blockTicks and before ChunkSource.tick, with its own budget.
  fluidTickQueue_->tick(*this, time_, 65536,
                        [this](Pos pos, const Fluid *fluid) {
                          tickFluidAt(pos, fluid);
                        });
  // ServerLevel.tick -> ServerChunkCache.tick -> ServerChunkCache.tickChunks:
  // random block/fluid ticks run after scheduled ticks and before broadcasting
  // dirty block updates.
  const auto rtStart = std::chrono::steady_clock::now();
  tickChunks();
  const auto rtEnd = std::chrono::steady_clock::now();
  randomTickStats.lastTickNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(rtEnd - rtStart)
          .count());
  // ServerChunkCache.tick broadcasts ChunkHolder's dirty block positions
  // after scheduled ticks and before block events. Event-induced piston
  // source changes stay pending until the following chunk broadcast phase.
  if (lightService_)
    lightService_->drainPublished();
  if (blockUpdatesBroadcastObserver)
    blockUpdatesBroadcastObserver();
  // ServerChunkCache.tick: ChunkMap.tick/ServerEntity.sendChanges follows
  // chunk broadcasts and precedes block events and this tick's entity motion.
  if (entityTrackingBroadcastObserver)
    entityTrackingBroadcastObserver();
  runEvents();
  handlingTick_ = false;
  // ServerLevel.tick: entities, then block entities.
  tickEntities();
  if (traceUpdates)
    log.push_back({"block_entities", {}, ""});
  pistonTickBlockEntities(*this);
  tickContainerBlockEntities(*this);
  if (blockEntityTicker)
    blockEntityTicker(*this);
  // Observe removals caused by block entities without altering the core's
  // existing cleanup timing. Network trackers must not wait one more tick.
  broadcastEntityRemovals();
}

namespace detail {
struct WorldBehaviours {
  // ---- onPlace ----
  static void observerOnPlace(World &world, Pos p, const BlockState &state,
                              const BlockState &oldState, bool) {
    if (!oldState.sameBlock(state) && poweredOf(state) &&
        !world.hasScheduledTick(p, state.block())) {
      const BlockState next =
          state.with(PropertyKind::Powered, std::uint8_t{0});
      world.setBlock(p, next, 18);
      world.observerNotify(p, next);
    }
  }

  static void tntOnPlace(World &world, Pos p, const BlockState &state,
                         const BlockState &oldState, bool) {
    if (!oldState.sameBlock(state) && world.hasNeighborSignal(p) &&
        world.primeTntBlock(p)) {
      // primeTntBlock performs TntBlock.prime; onPlace then removes the block.
    }
  }

  // ---- affectNeighborsAfterRemoval ----
  static void pistonHeadAffectRemoval(World &world, Pos p,
                                      const BlockState &state, bool) {
    const Pos q = p.relative(opposite(directionOf(state)));
    if (fittingBase(state, world.getBlock(q))) {
      for (const auto &drop : blockDrops(world.getBlock(q)))
        world.spawnItem(q, drop.id, drop.count);
      world.setBlock(q, airState(), 3);
    }
  }

  static void observerAffectRemoval(World &world, Pos p,
                                    const BlockState &state, bool) {
    if (poweredOf(state) && world.hasScheduledTick(p, state.block()))
      world.observerNotify(p,
                           state.with(PropertyKind::Powered, std::uint8_t{0}));
  }

  // ---- neighborChanged ----
  static void pistonHeadNeighborChanged(World &world, Pos p,
                                        const BlockState &state,
                                        const BlockRef &source, bool) {
    const Direction d = directionOf(state);
    const Pos basePos = p.relative(opposite(d));
    const BlockState base = world.getBlock(basePos);
    if (fittingBase(state, base) ||
        (base.is(Blocks::MOVING_PISTON) && facingOf(base) == facingOf(state)))
      world.neighborChanged(basePos, source.str());
  }

  static void tntNeighborChanged(World &world, Pos p, const BlockState &,
                                 const BlockRef &, bool) {
    if (world.hasNeighborSignal(p) && world.tntExplodes)
      world.primeTntBlock(p);
  }

  static void redstoneLampNeighborChanged(World &world, Pos p,
                                          const BlockState &state,
                                          const BlockRef &, bool) {
    const bool lit = litOf(state);
    if (lit != world.hasNeighborSignal(p)) {
      if (lit)
        world.scheduleTick(p, state.block(), 4);
      else
        world.setBlock(p, state.with(PropertyKind::Lit, std::uint8_t{1}), 2);
    }
  }

  static void fenceGateNeighborChanged(World &world, Pos p,
                                       const BlockState &state,
                                       const BlockRef &, bool) {
    const bool signal = world.hasNeighborSignal(p);
    const bool powered = poweredOf(state);
    if (signal != powered) {
      world.setBlock(p,
                     state
                         .with(PropertyKind::Powered,
                               signal ? std::uint8_t{1} : std::uint8_t{0})
                         .with(PropertyKind::Open,
                               signal ? std::uint8_t{1} : std::uint8_t{0}),
                     2);
      if (openOf(state) != signal) {
        (void)world.random.nextFloat();
        // TODO: fence gate sound and BLOCK_OPEN/BLOCK_CLOSE game events.
      }
    }
  }

  static void noteBlockNeighborChanged(World &world, Pos p,
                                       const BlockState &state,
                                       const BlockRef &, bool) {
    const bool signal = world.hasNeighborSignal(p);
    const bool powered = poweredOf(state);
    if (signal != powered) {
      if (signal && world.getBlock(p.relative(Direction::Up)).isAir())
        world.blockEvent(p, state.block(), 0, 0);
      world.setBlock(
          p,
          state.with(PropertyKind::Powered,
                     signal ? std::uint8_t{1} : std::uint8_t{0}),
          3);
    }
  }

  // ---- updateShape ----
  static BlockState seagrassUpdateShape(World &world, Pos p,
                                        const BlockState &state, Direction, Pos,
                                        const BlockState &) {
    const Pos below = p.relative(Direction::Down);
    const BlockState support = world.getBlock(below);
    const bool survives =
        faceSturdy(world, below, support, Direction::Up) &&
        !world.hasBlockTag(support, "minecraft:cannot_support_seagrass")
             .value_or(false);
    if (!survives)
      return airState();
    world.scheduleFluidTick(p, FluidRef(Fluids::water()),
                            Fluids::water().tickDelay());
    return state;
  }

  static BlockState tallSeagrassUpdateShape(World &world, Pos p,
                                            const BlockState &state,
                                            Direction direction, Pos,
                                            const BlockState &neighbor) {
    const bool lower = state.getView("half") == "lower";
    if (((lower && direction == Direction::Up) ||
         (!lower && direction == Direction::Down)) &&
        (!neighbor.is(Blocks::TALL_SEAGRASS) ||
         neighbor.getView("half") == state.getView("half")))
      return airState();
    const Pos below = p.relative(Direction::Down);
    const BlockState support = world.getBlock(below);
    const bool survives =
        lower
            ? faceSturdy(world, below, support, Direction::Up) &&
                  !world.hasBlockTag(support, "minecraft:cannot_support_seagrass")
                       .value_or(false)
            : support.is(Blocks::TALL_SEAGRASS) &&
                  support.getView("half") == "lower";
    return survives ? state : airState();
  }

  static BlockState checkSupportTag(World &world, Pos p,
                                    const BlockState &state,
                                    std::string_view tag) {
    const BlockState support = world.getBlock(p.relative(Direction::Down));
    return world.hasBlockTag(support, tag).value_or(false) ? state : airState();
  }

  static BlockState dryVegetationUpdateShape(World &world, Pos p,
                                             const BlockState &state,
                                             Direction, Pos,
                                             const BlockState &) {
    return checkSupportTag(world, p, state,
                           "minecraft:supports_dry_vegetation");
  }

  static BlockState witherRoseUpdateShape(World &world, Pos p,
                                          const BlockState &state, Direction,
                                          Pos, const BlockState &) {
    return checkSupportTag(world, p, state, "minecraft:supports_wither_rose");
  }

  static BlockState vegetationUpdateShape(World &world, Pos p,
                                          const BlockState &state, Direction,
                                          Pos, const BlockState &) {
    return checkSupportTag(world, p, state, "minecraft:supports_vegetation");
  }

  static BlockState carpetUpdateShape(World &world, Pos p,
                                      const BlockState &state, Direction, Pos,
                                      const BlockState &) {
    return world.getBlock(p.relative(Direction::Down)).isAir() ? airState()
                                                               : state;
  }

  static BlockState coralFanUpdateShape(World &world, Pos p,
                                        const BlockState &state,
                                        Direction direction, Pos,
                                        const BlockState &) {
    const Direction supportDirection = isDeadCoralWallFan(state)
                                           ? opposite(directionOf(state))
                                           : Direction::Down;
    if (direction == supportDirection && !coralCanSurvive(world, p, state))
      return airState();
    return state;
  }

  static BlockState snowyUpdateShape(World &world, Pos,
                                     const BlockState &state,
                                     Direction direction, Pos,
                                     const BlockState &neighbor) {
    if (direction == Direction::Up) {
      const bool snowy =
          world.hasBlockTag(neighbor, "minecraft:snow").value_or(false);
      return state.with(PropertyKind::Snowy,
                        snowy ? std::uint8_t{1} : std::uint8_t{0});
    }
    return state;
  }

  static BlockState fenceGateUpdateShape(World &world, Pos p,
                                         const BlockState &state,
                                         Direction direction, Pos,
                                         const BlockState &neighbor) {
    const Direction facing = directionOf(state);
    const bool facingZ =
        facing == Direction::North || facing == Direction::South;
    const bool directionX =
        direction == Direction::East || direction == Direction::West;
    const bool directionZ =
        direction == Direction::North || direction == Direction::South;
    if ((facingZ && directionX) || (!facingZ && directionZ)) {
      const bool inWall =
          isWallBlock(neighbor) ||
          isWallBlock(world.getBlock(p.relative(opposite(direction))));
      return state.with("in_wall", inWall ? "true" : "false");
    }
    return state;
  }

  static BlockState wallBlockUpdateShape(World &world, Pos p,
                                         const BlockState &state,
                                         Direction direction, Pos,
                                         const BlockState &neighbor) {
    if (direction != Direction::Down)
      return wallUpdateShape(world, p, state, direction, neighbor);
    return state;
  }

  static BlockState stairBlockUpdateShape(World &world, Pos p,
                                          const BlockState &state,
                                          Direction direction, Pos,
                                          const BlockState &neighbor) {
    return stairUpdateShape(world, p, state, direction, neighbor);
  }

  static BlockState fenceBlockUpdateShape(World &world, Pos p,
                                          const BlockState &state,
                                          Direction direction, Pos,
                                          const BlockState &neighbor) {
    return fenceUpdateShape(world, p, state, direction, neighbor);
  }

  static BlockState ironBarsBlockUpdateShape(World &world, Pos p,
                                             const BlockState &state,
                                             Direction direction, Pos,
                                             const BlockState &neighbor) {
    return ironBarsUpdateShape(world, p, state, direction, neighbor);
  }

  static BlockState pistonHeadUpdateShape(World &world, Pos,
                                          const BlockState &state,
                                          Direction direction, Pos neighborPos,
                                          const BlockState &) {
    if (opposite(direction) == directionOf(state)) {
      const BlockState base = world.getBlock(neighborPos);
      if (!fittingBase(state, base) &&
          !(base.is(Blocks::MOVING_PISTON) && facingOf(base) == facingOf(state)))
        return airState();
    }
    return state;
  }

  static BlockState observerUpdateShape(World &world, Pos p,
                                        const BlockState &state,
                                        Direction direction, Pos,
                                        const BlockState &) {
    if (directionOf(state, Direction::South) == direction &&
        !poweredOf(state) && !world.hasScheduledTick(p, state.block()))
      world.scheduleTick(p, state.block(), 2);
    return state;
  }

  static bool fireCanSurvive(World &world, Pos p) {
    const Pos below = p.relative(Direction::Down);
    const BlockState belowState = world.getBlock(below);
    if (faceSturdy(world, below, belowState, Direction::Up, SupportType::Full))
      return true;
    for (Direction d : {Direction::Down, Direction::Up, Direction::North,
                        Direction::South, Direction::West, Direction::East}) {
      if (world.getBlock(p.relative(d)).block().metadata().ignitedByLava)
        return true;
    }
    return false;
  }

  static bool soulFireCanSurvive(World &world, Pos p) {
    const BlockState belowState = world.getBlock(p.relative(Direction::Down));
    return world.hasBlockTag(belowState, "minecraft:soul_fire_base_blocks")
        .value_or(false);
  }

  static void fireOnPlace(World &world, Pos p, const BlockState &state,
                          const BlockState &oldState, bool) {
    if (oldState.sameBlock(state))
      return;
    if (isInPortalDimension(world)) {
      if (auto shape =
              PortalShape::findEmptyPortalShape(world, p, PortalAxis::X);
          shape.has_value()) {
        shape->createPortalBlocks(world);
        return;
      }
    }
    const bool survives = state.is(Blocks::SOUL_FIRE)
                              ? soulFireCanSurvive(world, p)
                              : fireCanSurvive(world, p);
    if (!survives)
      world.removeBlock(p, false);
  }

  static BlockState fireUpdateShape(World &world, Pos p,
                                    const BlockState &state, Direction, Pos,
                                    const BlockState &) {
    return fireCanSurvive(world, p) ? state : airState();
  }

  static BlockState soulFireUpdateShape(World &world, Pos p,
                                        const BlockState &state, Direction, Pos,
                                        const BlockState &) {
    return soulFireCanSurvive(world, p) ? state : airState();
  }

  static BlockState netherPortalUpdateShape(World &world, Pos p,
                                            const BlockState &state,
                                            Direction direction, Pos,
                                            const BlockState &neighbor) {
    const BlockAxis updateAxis = axisOfDirection(direction);
    const PortalAxis portalAxis = portalAxisOfState(state);
    const BlockAxis portalBlockAxis =
        portalAxis == PortalAxis::X ? BlockAxis::X : BlockAxis::Z;
    const bool wrongAxis =
        portalBlockAxis != updateAxis && isHorizontalDirection(direction);
    if (!wrongAxis && !neighbor.is(Blocks::NETHER_PORTAL) &&
        !PortalShape::findAnyShape(world, p, portalAxis).isComplete()) {
      return airState();
    }
    return state;
  }

  static BlockState noteBlockUpdateShape(World &world, Pos p,
                                         const BlockState &state,
                                         Direction direction, Pos,
                                         const BlockState &) {
    if (direction == Direction::Up || direction == Direction::Down) {
      const Instrument aboveInst =
          world.getBlock(p.relative(Direction::Up))
              .block()
              .metadata()
              .instrument;
      Instrument chosen = Instrument::Harp;
      if (static_cast<std::uint8_t>(aboveInst) >=
          static_cast<std::uint8_t>(Instrument::Zombie)) {
        chosen = aboveInst;
      } else {
        const Instrument belowInst =
            world.getBlock(p.relative(Direction::Down))
                .block()
                .metadata()
                .instrument;
        chosen = static_cast<std::uint8_t>(belowInst) >=
                         static_cast<std::uint8_t>(Instrument::Zombie)
                     ? Instrument::Harp
                     : belowInst;
      }
      return state.with(PropertyKind::Instrument,
                        static_cast<std::uint8_t>(chosen));
    }
    return state;
  }

  // ---- scheduled tick ----
  static void observerTick(World &world, Pos pos, const BlockState &s) {
    const bool on = poweredOf(s);
    world.setBlock(
        pos, s.with(PropertyKind::Powered, on ? std::uint8_t{0} : std::uint8_t{1}),
        2);
    if (!on)
      world.scheduleTick(pos, s.block(), 2);
    world.observerNotify(pos, s);
  }

  static void redstoneLampTick(World &world, Pos pos, const BlockState &s) {
    if (litOf(s) && !world.hasNeighborSignal(pos))
      world.setBlock(pos, s.with("lit", "false"), 2);
  }

  static void fallingBlockTick(World &world, Pos pos, const BlockState &s) {
    // TODO: FIRE tags and replaceable blocks outside the implemented registry.
    if (pos.y >= world.minY_ &&
        isReplaceable(world.getBlock(pos.relative(Direction::Down))))
      FallingBlockEntity::fall(world, pos, s);
  }

  // ---- block event ----
  static bool noteBlockTriggerEvent(World &world, Pos pos, const BlockState &s,
                                    int, int) {
    world.log.push_back(
        {"note", pos, s.get("instrument", "harp") + ":" + s.get("note", "0")});
    return true;
  }
};

void bindBlockBehaviours(Block &block) {
  auto &b = block.behaviours_;
  if (&block == &Blocks::OBSERVER) {
    b.onPlace = &WorldBehaviours::observerOnPlace;
    b.affectNeighborsAfterRemoval = &WorldBehaviours::observerAffectRemoval;
    b.updateShape = &WorldBehaviours::observerUpdateShape;
    b.tick = &WorldBehaviours::observerTick;
  } else if (&block == &Blocks::TNT) {
    b.onPlace = &WorldBehaviours::tntOnPlace;
    b.neighborChanged = &WorldBehaviours::tntNeighborChanged;
  } else if (&block == &Blocks::PISTON_HEAD) {
    b.affectNeighborsAfterRemoval = &WorldBehaviours::pistonHeadAffectRemoval;
    b.neighborChanged = &WorldBehaviours::pistonHeadNeighborChanged;
    b.updateShape = &WorldBehaviours::pistonHeadUpdateShape;
  } else if (&block == &Blocks::REDSTONE_WIRE) {
    b.onPlace = &redstoneWireOnPlace;
    b.affectNeighborsAfterRemoval = &redstoneWireAffectRemoval;
    b.neighborChanged = &redstoneWireNeighborChanged;
    b.updateShape = &redstoneWireUpdateShape;
  } else if (&block == &Blocks::REDSTONE_TORCH) {
    b.onPlace = &redstoneTorchOnPlace;
    b.affectNeighborsAfterRemoval = &redstoneTorchAffectRemoval;
    b.neighborChanged = &redstoneTorchNeighborChanged;
    b.updateShape = &redstoneTorchUpdateShape;
    b.tick = &redstoneTorchTick;
  } else if (&block == &Blocks::REDSTONE_WALL_TORCH) {
    b.onPlace = &redstoneTorchOnPlace;
    b.affectNeighborsAfterRemoval = &redstoneTorchAffectRemoval;
    b.neighborChanged = &redstoneTorchNeighborChanged;
    b.updateShape = &redstoneWallTorchUpdateShape;
    b.tick = &redstoneTorchTick;
  } else if (&block == &Blocks::REPEATER) {
    b.onPlace = &repeaterOnPlace;
    b.affectNeighborsAfterRemoval = &repeaterAffectRemoval;
    b.neighborChanged = &repeaterNeighborChanged;
    b.updateShape = &repeaterUpdateShape;
    b.tick = &repeaterTick;
  } else if (&block == &Blocks::COMPARATOR) {
    b.onPlace = &comparatorOnPlace;
    b.affectNeighborsAfterRemoval = &comparatorAffectRemoval;
    b.neighborChanged = &comparatorNeighborChanged;
    b.tick = &comparatorTick;
  } else if (&block == &Blocks::LEVER) {
    b.affectNeighborsAfterRemoval = &leverAffectRemoval;
    b.updateShape = &leverUpdateShape;
  } else if (block.isButton()) {
    b.affectNeighborsAfterRemoval = &buttonAffectRemoval;
    b.updateShape = &buttonUpdateShape;
    b.tick = &buttonTick;
  } else if (&block == &Blocks::TARGET) {
    b.onPlace = &targetOnPlace;
    b.tick = &targetTick;
  } else if (block.isPressurePlate()) {
    b.affectNeighborsAfterRemoval = &pressurePlateAffectRemoval;
    b.updateShape = &pressurePlateUpdateShape;
    b.tick = &pressurePlateTick;
  } else if (block.isDoor()) {
    b.neighborChanged = &doorNeighborChanged;
    b.updateShape = &doorUpdateShape;
  } else if (block.isTrapdoor()) {
    b.neighborChanged = &trapdoorNeighborChanged;
  } else if (block.isCopperBulb()) {
    b.onPlace = &copperBulbOnPlace;
    b.neighborChanged = &copperBulbNeighborChanged;
  } else if (&block == &Blocks::REDSTONE_LAMP) {
    b.neighborChanged = &WorldBehaviours::redstoneLampNeighborChanged;
    b.tick = &WorldBehaviours::redstoneLampTick;
  } else if (block.isChest()) {
    b.onPlace = &chestOnPlace;
    b.affectNeighborsAfterRemoval = &chestAffectRemoval;
    b.updateShape = &chestUpdateShape;
  } else if (&block == &Blocks::BARREL) {
    b.onPlace = &barrelOnPlace;
    b.affectNeighborsAfterRemoval = &barrelAffectRemoval;
    b.tick = &barrelTick;
  } else if (&block == &Blocks::DISPENSER) {
    b.onPlace = &dispenserOnPlace;
    b.affectNeighborsAfterRemoval = &dispenserAffectRemoval;
    b.neighborChanged = &dispenserNeighborChanged;
    b.tick = &dispenserTick;
  } else if (&block == &Blocks::DROPPER) {
    b.onPlace = &dispenserOnPlace;
    b.affectNeighborsAfterRemoval = &dispenserAffectRemoval;
    b.neighborChanged = &dispenserNeighborChanged;
    b.tick = &dropperTick;
  } else if (&block == &Blocks::HOPPER) {
    b.onPlace = &hopperOnPlace;
    b.affectNeighborsAfterRemoval = &hopperAffectRemoval;
    b.neighborChanged = &hopperNeighborChanged;
  } else if (&block == &Blocks::COMPOSTER) {
    b.onPlace = &composterOnPlace;
    b.tick = &composterTick;
  } else if (&block == &Blocks::NOTE_BLOCK) {
    b.neighborChanged = &WorldBehaviours::noteBlockNeighborChanged;
    b.updateShape = &WorldBehaviours::noteBlockUpdateShape;
    b.triggerEvent = &WorldBehaviours::noteBlockTriggerEvent;
  } else if (&block == &Blocks::FIRE) {
    b.onPlace = &WorldBehaviours::fireOnPlace;
    b.updateShape = &WorldBehaviours::fireUpdateShape;
    // TODO: FireBlock flammable tick/spread & burn-out remain unimplemented.
  } else if (&block == &Blocks::SOUL_FIRE) {
    b.onPlace = &WorldBehaviours::fireOnPlace;
    b.updateShape = &WorldBehaviours::soulFireUpdateShape;
  } else if (&block == &Blocks::NETHER_PORTAL) {
    b.updateShape = &WorldBehaviours::netherPortalUpdateShape;
    // TODO: NetherPortalBlock::randomTick ZombifiedPiglin spawning remains
    // unimplemented (no mob entity implementation yet).
  } else if (block.isFenceGate()) {
    b.neighborChanged = &WorldBehaviours::fenceGateNeighborChanged;
    b.updateShape = &WorldBehaviours::fenceGateUpdateShape;
  } else if (block.isWall()) {
    b.updateShape = &WorldBehaviours::wallBlockUpdateShape;
  } else if (block.isStairs()) {
    b.updateShape = &WorldBehaviours::stairBlockUpdateShape;
  } else if (block.isFence()) {
    b.updateShape = &WorldBehaviours::fenceBlockUpdateShape;
  } else if (isIronBarsBlock(block.defaultState())) {
    b.updateShape = &WorldBehaviours::ironBarsBlockUpdateShape;
  } else if (block.isCarpet()) {
    b.updateShape = &WorldBehaviours::carpetUpdateShape;
  } else if (block.isSnowy()) {
    b.updateShape = &WorldBehaviours::snowyUpdateShape;
  } else if ((block.isCoralFan() || block.isCoralWallFan()) &&
             block.supported()) {
    b.updateShape = &WorldBehaviours::coralFanUpdateShape;
  } else if (&block == &Blocks::SEAGRASS) {
    b.updateShape = &WorldBehaviours::seagrassUpdateShape;
  } else if (&block == &Blocks::TALL_SEAGRASS) {
    b.updateShape = &WorldBehaviours::tallSeagrassUpdateShape;
  } else if (&block == &Blocks::DEAD_BUSH ||
             &block == &Blocks::SHORT_DRY_GRASS ||
             &block == &Blocks::TALL_DRY_GRASS) {
    b.updateShape = &WorldBehaviours::dryVegetationUpdateShape;
  } else if (&block == &Blocks::WITHER_ROSE) {
    b.updateShape = &WorldBehaviours::witherRoseUpdateShape;
  } else if (&block == &Blocks::SHORT_GRASS || &block == &Blocks::FERN ||
             &block == &Blocks::BUSH ||
             isRegisteredSmallFlower(block.defaultState())) {
    b.updateShape = &WorldBehaviours::vegetationUpdateShape;
  }
  if (block.isFalling())
    b.tick = &WorldBehaviours::fallingBlockTick;
}
} // namespace detail

void Block::onPlace(World &world, Pos pos, const BlockState &state,
                    const BlockState &oldState, bool movedByPiston) const {
  if (behaviours_.onPlace)
    behaviours_.onPlace(world, pos, state, oldState, movedByPiston);
}

void Block::affectNeighborsAfterRemoval(World &world, Pos pos,
                                        const BlockState &state,
                                        bool movedByPiston) const {
  if (behaviours_.affectNeighborsAfterRemoval)
    behaviours_.affectNeighborsAfterRemoval(world, pos, state, movedByPiston);
}

void Block::neighborChanged(World &world, Pos pos, const BlockState &state,
                            const BlockRef &source, bool movedByPiston) const {
  if (behaviours_.neighborChanged)
    behaviours_.neighborChanged(world, pos, state, source, movedByPiston);
}

BlockState Block::updateShape(World &world, Pos pos, const BlockState &state,
                              Direction direction, Pos neighborPos,
                              const BlockState &neighbor) const {
  if (behaviours_.updateShape)
    return behaviours_.updateShape(world, pos, state, direction, neighborPos,
                                   neighbor);
  return state;
}

void Block::tick(World &world, Pos pos, const BlockState &state) const {
  if (behaviours_.tick)
    behaviours_.tick(world, pos, state);
}

bool Block::triggerEvent(World &world, Pos pos, const BlockState &state,
                         int type, int data) const {
  return behaviours_.triggerEvent &&
         behaviours_.triggerEvent(world, pos, state, type, data);
}
} // namespace redstone
