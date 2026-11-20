#pragma once
// 模拟引擎内部共享头文件：定义分区块计划刻队列（TickQueueOf / ChunkTicksOf）、开放寻址哈希容器、方块事件队列与原版更新顺序常量，仅由 src/engine/*.cpp 包含。
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
namespace engine_internal {
// Java 的 long 算术按 2^64 模回绕（包括 subTickCount++）；通过无符号加法 + bit_cast 复现该语义并规避 C++ 有符号溢出未定义行为。
inline std::int64_t nextSubTick(std::int64_t value) {
  auto bits = std::bit_cast<std::uint64_t>(value);
  return std::bit_cast<std::int64_t>(bits + std::uint64_t{1});
}
// 严格复刻原版 NeighborUpdater.UPDATE_ORDER（西、东、下、上、北、南）与 BlockBehaviour.UPDATE_SHAPE_ORDER（西、东、北、南、下、上）。
constexpr std::array<Direction, 6> neighborOrder{
    Direction::West, Direction::East,  Direction::Down,
    Direction::Up,   Direction::North, Direction::South};
constexpr std::array<Direction, 6> shapeOrder{
    Direction::West,  Direction::East, Direction::North,
    Direction::South, Direction::Down, Direction::Up};
inline std::string canonical(std::string n) { return canonicalName(std::move(n)); }
// 校验活塞头是否匹配相邻的已伸出活塞底座（类型与朝向均需一致），供活塞破坏/移动回调使用。
inline bool fittingBase(const BlockState &head, const BlockState &base) {
  return (pistonTypeOf(head) == PistonType::Sticky ? base.is(Blocks::STICKY_PISTON)
                                                   : base.is(Blocks::PISTON)) &&
         extendedOf(base) && facingOf(base) == facingOf(head);
}
inline bool piston(const BlockState &s) {
  return s.is(Blocks::PISTON) || s.is(Blocks::STICKY_PISTON);
}

// 对应 ScheduledTick.UNIQUE_TICK_HASH：对坐标与被调度对象的身份标识求哈希。
// Target 为队列的模板参数：方块刻为 Block，流体刻为 Fluid，二者均为进程内驻留单例指针。
// 存在原因：将三维坐标压缩打包为原版 BlockPos.asLong() 的 64 位位域（X:26位, Z:26位, Y:12位），使 TickKey 保持 16 字节紧凑布局并快速判重。
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
inline std::size_t tickKeyHash(const TickKey<Target> &key) {
  auto hash = key.packedPos * 0x9E3779B97F4A7C15ull;
  hash ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(key.target)) *
          0x9E3779B185EBCA87ull;
  hash ^= hash >> 29;
  hash *= 0x9E3779B97F4A7C15ull;
  hash ^= hash >> 32;
  return static_cast<std::size_t>(hash);
}
template <class Target>
inline bool tickKeyEquals(const TickKey<Target> &a, const TickKey<Target> &b) {
  return a.packedPos == b.packedPos && a.target == b.target;
}
// 基于开放寻址与墓碑删除（tombstone）的 (packedPos, target) 去重集合，支持保留容量的 clear 与周期性/空表缩容。
// 存在原因：高频计划刻调度与弹出过程中会反复插入和删除大量条目，开放寻址扁平数组可避免 std::unordered_set 的逐节点堆分配开销。
template <class Target> class TickKeySet {
  struct Hasher {
    std::size_t operator()(const TickKey<Target> &key) const noexcept {
      return tickKeyHash(key);
    }
  };
  struct Eq {
    bool operator()(const TickKey<Target> &a,
                    const TickKey<Target> &b) const noexcept {
      return tickKeyEquals(a, b);
    }
  };
  using Set = phmap::flat_hash_set<TickKey<Target>, Hasher, Eq>;

public:
  std::size_t size() const { return set_.size(); }
  std::size_t capacity() const { return set_.bucket_count(); }
  bool contains(const TickKey<Target> &key) const {
    return set_.contains(key);
  }
  bool insert(const TickKey<Target> &key) { return set_.insert(key).second; }
  void erase(const TickKey<Target> &key) { set_.erase(key); }
  void clear() { set_.clear(); }
  void release() { Set{}.swap(set_); }
  void compactIfNeeded() {
    if (set_.empty()) {
      if (set_.bucket_count() > 0)
        release();
      return;
    }
    const std::size_t cap = set_.bucket_count();
    if (cap > 32 && set_.size() * 6 <= cap)
      set_.rehash(0);
  }

private:
  Set set_;
};

template <class V> class FlatChunkMap {
  using Map = phmap::flat_hash_map<std::uint64_t, V, ChunkKeyHash>;

public:
  std::size_t size() const { return map_.size(); }
  V *find(std::uint64_t key) {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  const V *find(std::uint64_t key) const {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
  }
  void set(std::uint64_t key, const V &value) {
    map_.insert_or_assign(key, value);
  }
  bool erase(std::uint64_t key) { return map_.erase(key) > 0; }
  template <class F> void forEach(F &&visitor) const {
    for (const auto &[k, v] : map_)
      visitor(k, v);
  }
  void clear() { map_.clear(); }

private:
  Map map_;
};
} // namespace engine_internal
using namespace engine_internal;

// ---------------------------------------------------------------------------
// 对应 LevelChunkTicks<ScheduledTick>：单区块按 DRAIN_ORDER 排序的最小堆，
// 以及按 ScheduledTick.UNIQUE_TICK_HASH 构成的（位置，目标对象身份）去重集合。
// Target 在方块队列中为 Block、在流体队列中为 Fluid，使被调度对象保持强类型指针而非类型擦除。
// 存在原因：每条 QueuedTick 压缩为 32 字节并连续存放于堆数组中，从而支撑大规模红石电路的密集调度。
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

// 按触发刻、优先级与子刻顺序组织单个区块内的计划刻最小堆，并维护同位置同目标的去重集合。
template <class Target> struct ChunkTicksOf {
  std::vector<QueuedTick<Target>> heap; // 按 QueuedDrainOrder 排列的最小堆
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
  // 在计划刻峰值回落或彻底排空后收缩底层缓冲区，避免单区块长期滞留历史峰值内存。
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
    // 对应 LevelChunkTicks.scheduleUnchecked：从存档恢复的列表允许包含重复条目；
    // 重复项会继续保留在队列中，而去重集合仅登记其中一份（保证后续新调度仍能被正确判重）。
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
// 对应 LevelTicks<ScheduledTick>：管理各区块容器、nextTickForContainer 索引，以及
// LevelTicks.collectTicks / drainContainers / runCollectedTicks 的本刻排空状态。
// ServerLevel 为 Block 和 Fluid 各持有一个实例（blockTicks / fluidTicks），各自拥有独立的 65536 上限预算，
// 因此按被调度对象类型模板化，使两者复用完全一致的多路归并调度算法。
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
  // 每个容器在映射期间保持稳定的堆内存地址（由 deque<unique_ptr> 持有所有权）。
  // 移除容器时会先清理所有瞬态引用，随后立即擦除该独占所有权指针。
  std::deque<std::unique_ptr<ChunkTicks>> storage;
  FlatChunkMap<ChunkTicks *> containers;
  FlatChunkMap<std::int64_t> nextTickForContainer;
  std::vector<Entry> containersToTick; // 按 EntryOrder 排列的最小堆
  std::deque<QueuedTick<Target>> toRunThisTick;
  mutable TickKeySet<Target> toRunThisTickSet;
  mutable bool toRunSetBuilt = false;
  std::vector<std::uint64_t> dueScratch;

  // 对应 LevelTicks.addContainer：为每个进入可刻状态的区块注册一个空容器。
  void addContainer(std::uint64_t key) {
    if (containers.find(key))
      return;
    storage.emplace_back(std::make_unique<ChunkTicks>());
    containers.set(key, storage.back().get());
  }
  // 清理所有引用该区块容器的瞬态索引（nextTickForContainer、containersToTick、toRunThisTick）
  // 并立即擦除独占 unique_ptr，从而在区块卸载时立刻释放容器存储。
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
  // 对应 LevelChunkTicks.onTickAdded：仅当新加入的计划刻成为区块堆顶时，才需要更新该区块的下次触发索引。
  void onTickAdded(std::uint64_t key, ChunkTicks &container,
                   const QueuedTick<Target> &tick) {
    const QueuedTick<Target> *head = container.peek();
    if (head && head->packedPos == tick.packedPos &&
        head->target == tick.target && head->trigger == tick.trigger &&
        head->priority == tick.priority && head->order == tick.order)
      updateContainerScheduling(key, head->trigger);
  }
  // 对应 LevelTicks.schedule：目标位置所属区块没有容器时直接丢弃。
  bool schedule(std::uint64_t key, const ScheduledTick &tick,
                const Target *target) {
    auto *container = containers.find(key);
    if (!container)
      return false; // 对应原版 "Trying to schedule tick in not loaded position"
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
  // run 回调对应 ServerLevel.tickBlock / tickFluid：执行前会重新读取目标位置的方块或流体状态，仅当身份仍匹配时才触发。
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
  // 按需懒构建本刻待执行集合，避免绝大多数不调用 willTickThisTick 的游戏刻付出额外哈希构建开销。
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
  // 对应 LevelTicks.sortContainersToTick：仅筛选堆顶时间已到期的区块容器，避免扫描全图区块。
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
      // 对应 LevelTicks tickCheck：未处于可刻状态的区块仍保留其容器注册（待区块恢复 ticking 时继续执行待处理刻）。
      if (!world.isChunkLoaded(headPos.x >> 4, headPos.z >> 4))
        continue;
      if (world.blockTickPositionReady &&
          !world.blockTickPositionReady(headPos)) {
        // 将到期刻继续留在堆顶；当区块管理器的就绪状态变为可刻后即可直接执行，无需重建队列或扫描整个世界。
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
      // 对应 ScheduledTick.INTRA_TICK_DRAIN_ORDER.compare(next, nextOfNext) > 0：
      // 若当前区块的下一条优先级低于其他待处理区块的堆顶，则停止连续排空并重新参与跨区块堆排序。
      if (!next || next->trigger > gameTime ||
          (nextFromNextContainer &&
           QueuedIntraOrder<Target>{}(*next, *nextFromNextContainer)))
        break;
      toRunThisTick.push_back(current.poll());
    }
  }
  void rescheduleLeftoverContainers() {
    // 对应 LevelTicks.rescheduleLeftoverContainers：原地遍历本刻未排空的容器，写回它们新的堆顶触发时间。
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
  }
};

// ---------------------------------------------------------------------------
// 对应 ServerLevel.blockEvents（按插入顺序维护的去重哈希集合，用于活塞动作、音符盒等方块事件）；
// 位于未加载区块的事件会经由 blockEventsToReschedule 暂存，并在本轮结束后重新追加到队列。
// ---------------------------------------------------------------------------
struct World::BlockEventQueue {
  std::deque<BlockEvent> order;
  phmap::flat_hash_set<BlockEvent, BlockEventHash> index;
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

} // namespace redstone
