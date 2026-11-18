#pragma once

// 光照引擎内部公共基础结构：提供高性能扁平哈希容器、坐标与区块键工具、批处理上下文、可写光照层封装及双引擎共享的只读宿主上下文。

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "redstone/light/service.hpp"
#include "redstone/light/snapshot.hpp"
#include "redstone/light/types.hpp"

namespace redstone {
const BlockLightProperties &unsupportedLightProperties() noexcept;
}

namespace redstone::light_internal {

// 直读低内存模式下记录在工作线程中的区块元数据（无需保存几何单元，仅保留坐标与生命周期代次）。
struct DirectChunkMeta {
  int chunkX = 0;
  int chunkZ = 0;
  std::uint64_t lifecycleEpoch = 0;
};

// 线程 CPU 亲和性设置结果，记录实际生效的掩码与平台错误码。
struct ThreadAffinityResult {
  std::uint64_t mask = 0;
  std::uint32_t error = 0;
};

// 返回当前线程累计消耗的 CPU 时间（毫秒），用于统计工作线程开销。
double currentThreadCpuMs();
#if defined(_WIN32) || defined(__linux__)
ThreadAffinityResult applyThreadAffinity(std::thread::native_handle_type handle,
                                         std::uintptr_t mask);
#endif

// 将 `(chunkX, chunkZ)` 打包为 64 位无符号区块键，便于哈希查找与排序发布。
inline std::uint64_t chunkKey(int x, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
         static_cast<std::uint32_t>(z);
}

// 计算区块坐标在 64 槽（8x8）直接映射 L1 缓存中的槽位索引。
inline std::size_t chunkL1Slot(int chunkX, int chunkZ) {
  return static_cast<std::size_t>(((chunkX & 7) << 3) | (chunkZ & 7));
}

inline std::size_t chunkL1Slot(std::uint64_t key) {
  const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
  const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
  return chunkL1Slot(chunkX, chunkZ);
}

// SplitMix64 混合哈希器，避免连续整数区块键在开放寻址表中发生聚簇退化。
struct UInt64Hash {
  std::size_t operator()(std::uint64_t x) const noexcept {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return static_cast<std::size_t>(x);
  }
};

template <class Key> struct FlatSetSlot {
  Key key{};
  static const Key &slotKey(const FlatSetSlot &s) { return s.key; }
  static Key &valueRef(FlatSetSlot &s) { return s.key; }
  static const Key &constValueRef(const FlatSetSlot &s) { return s.key; }
};

template <class Key, class Value> struct FlatMapSlot {
  Key first{};
  Value second{};
  static const Key &slotKey(const FlatMapSlot &s) { return s.first; }
  static FlatMapSlot &valueRef(FlatMapSlot &s) { return s; }
  static const FlatMapSlot &constValueRef(const FlatMapSlot &s) { return s; }
};

// 紧凑型开放寻址哈希表，采用类似 SwissTable 的 1 字节 H2 标签。
// 存在原因：替代 `std::unordered_map`/`std::unordered_set`，将探测局限在连续字节缓存行中并彻底消除每个节点一次堆分配与指针追踪，是热路径（区块/区块段/去重集合）降低延迟的关键容器；同时通过 `rehashEpoch_` 让外部 L1 指针缓存能 O(1) 感知重哈希。
template <class Slot, class Key, class Hash> class FlatTable {
  static constexpr std::uint8_t kEmpty = 0;
  static constexpr std::uint8_t kTombstone = 1;
  static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

  // 取哈希高 7 位并置位最高位（`>= 0x80`）作为占用槽位的 H2 指纹标签，从而与 `kEmpty(0)`/`kTombstone(1)` 区分。
  static std::uint8_t tagOf(std::size_t hash) {
    return static_cast<std::uint8_t>(0x80u | ((hash >> 57) & 0x7Fu));
  }

  template <bool IsConst> class Iter {
  public:
    using TablePtr = std::conditional_t<IsConst, const FlatTable *, FlatTable *>;
    using value_type =
        std::conditional_t<IsConst,
                           const decltype(Slot::constValueRef(
                               std::declval<const Slot &>())),
                           decltype(Slot::valueRef(std::declval<Slot &>()))>;
    using difference_type = std::ptrdiff_t;
    using pointer = std::remove_reference_t<value_type> *;
    using reference = value_type &;
    using iterator_category = std::forward_iterator_tag;

    Iter() = default;
    Iter(TablePtr table, std::size_t idx) : table_(table), idx_(idx) {
      skipEmpty();
    }
    template <bool OtherConst, class = std::enable_if_t<IsConst && !OtherConst>>
    Iter(const Iter<OtherConst> &other)
        : table_(other.table_), idx_(other.idx_) {}

    reference operator*() const {
      if constexpr (IsConst)
        return Slot::constValueRef(table_->slots_[idx_]);
      else
        return Slot::valueRef(table_->slots_[idx_]);
    }
    pointer operator->() const { return &(**this); }
    Iter &operator++() {
      ++idx_;
      skipEmpty();
      return *this;
    }
    Iter operator++(int) {
      Iter copy = *this;
      ++(*this);
      return copy;
    }
    bool operator==(const Iter &other) const {
      return table_ == other.table_ && idx_ == other.idx_;
    }

  private:
    friend class FlatTable;
    void skipEmpty() {
      if (!table_)
        return;
      const std::size_t limit = table_->states_.size();
      while (idx_ < limit && table_->states_[idx_] < 0x80u)
        ++idx_;
    }
    TablePtr table_ = nullptr;
    std::size_t idx_ = 0;
  };

public:
  using iterator = Iter<false>;
  using const_iterator = Iter<true>;

  bool empty() const { return size_ == 0; }
  std::size_t size() const { return size_; }
  std::uint64_t rehashEpoch() const { return rehashEpoch_; }

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, states_.size()); }
  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, states_.size()); }

  void reserve(std::size_t minElements) {
    const std::size_t needed =
        std::bit_ceil(std::max<std::size_t>((minElements * 10) / 7 + 2, 16));
    if (needed > states_.size())
      rehash(needed);
  }

  void clear() {
    if (size_ > 0) {
      if constexpr (!std::is_trivially_destructible_v<Slot>) {
        for (std::size_t i = 0; i < states_.size(); ++i) {
          if (states_[i] >= 0x80u)
            slots_[i] = Slot{};
        }
      }
    }
    if (!states_.empty())
      std::memset(states_.data(), kEmpty, states_.size());
    size_ = 0;
    tombstones_ = 0;
  }

  iterator find(const Key &key) {
    const std::size_t idx = findIndex(key);
    return idx == kNpos ? end() : iterator(this, idx);
  }
  const_iterator find(const Key &key) const {
    const std::size_t idx = findIndex(key);
    return idx == kNpos ? end() : const_iterator(this, idx);
  }
  bool contains(const Key &key) const { return findIndex(key) != kNpos; }

  std::size_t erase(const Key &key) {
    const std::size_t idx = findIndex(key);
    if (idx == kNpos)
      return 0;
    eraseIndex(idx);
    return 1;
  }
  iterator erase(iterator pos) {
    if (pos == end())
      return end();
    const std::size_t idx = pos.idx_;
    eraseIndex(idx);
    return iterator(this, idx + 1);
  }

protected:
  std::pair<std::size_t, bool> prepareInsert(const Key &key) {
    if (states_.empty())
      rehash(16);
    else if ((size_ + tombstones_ + 1) * 10 >= states_.size() * 7)
      rehash(size_ * 2 < states_.size() ? states_.size() : states_.size() * 2);

    const std::size_t mask = states_.size() - 1;
    const std::size_t h = Hash{}(key);
    const std::uint8_t tag = tagOf(h);
    std::size_t firstTombstone = kNpos;
    for (std::size_t idx = h & mask;; idx = (idx + 1) & mask) {
      const std::uint8_t st = states_[idx];
      if (st == kEmpty) {
        const std::size_t target =
            firstTombstone != kNpos ? firstTombstone : idx;
        if (states_[target] == kTombstone)
          --tombstones_;
        states_[target] = tag;
        ++size_;
        return {target, true};
      }
      if (st == kTombstone) {
        if (firstTombstone == kNpos)
          firstTombstone = idx;
      } else if (st == tag && Slot::slotKey(slots_[idx]) == key) {
        return {idx, false};
      }
    }
  }

  Slot &slotAt(std::size_t idx) { return slots_[idx]; }
  const Slot &slotAt(std::size_t idx) const { return slots_[idx]; }
  std::size_t findIndex(const Key &key) const {
    if (size_ == 0)
      return kNpos;
    const std::size_t mask = states_.size() - 1;
    const std::size_t h = Hash{}(key);
    const std::uint8_t tag = tagOf(h);
    for (std::size_t idx = h & mask;; idx = (idx + 1) & mask) {
      const std::uint8_t st = states_[idx];
      if (st == kEmpty)
        return kNpos;
      if (st == tag && Slot::slotKey(slots_[idx]) == key)
        return idx;
    }
  }

private:
  void eraseIndex(std::size_t idx) {
    states_[idx] = kTombstone;
    if constexpr (!std::is_trivially_destructible_v<Slot>)
      slots_[idx] = Slot{};
    --size_;
    ++tombstones_;
  }

  void rehash(std::size_t newCapacity) {
    std::vector<std::uint8_t> newStates(newCapacity, kEmpty);
    std::vector<Slot> newSlots(newCapacity);
    const std::size_t mask = newCapacity - 1;
    for (std::size_t i = 0; i < states_.size(); ++i) {
      if (states_[i] < 0x80u)
        continue;
      const std::size_t h = Hash{}(Slot::slotKey(slots_[i]));
      const std::uint8_t tag = tagOf(h);
      std::size_t dst = h & mask;
      while (newStates[dst] != kEmpty)
        dst = (dst + 1) & mask;
      newStates[dst] = tag;
      newSlots[dst] = std::move(slots_[i]);
    }
    states_ = std::move(newStates);
    slots_ = std::move(newSlots);
    tombstones_ = 0;
    ++rehashEpoch_;
  }

  std::vector<std::uint8_t> states_;
  std::vector<Slot> slots_;
  std::size_t size_ = 0;
  std::size_t tombstones_ = 0;
  std::uint64_t rehashEpoch_ = 1;
};

template <class Key, class Hash = std::hash<Key>>
class FlatSet : public FlatTable<FlatSetSlot<Key>, Key, Hash> {
  using Base = FlatTable<FlatSetSlot<Key>, Key, Hash>;

public:
  using iterator = typename Base::iterator;
  using const_iterator = typename Base::const_iterator;

  std::pair<iterator, bool> insert(const Key &key) {
    auto [idx, inserted] = Base::prepareInsert(key);
    if (inserted)
      Base::slotAt(idx).key = key;
    return {iterator(this, idx), inserted};
  }
};

template <class Key, class Value, class Hash = std::hash<Key>>
class FlatMap : public FlatTable<FlatMapSlot<Key, Value>, Key, Hash> {
  using Base = FlatTable<FlatMapSlot<Key, Value>, Key, Hash>;
  using Slot = FlatMapSlot<Key, Value>;

public:
  using iterator = typename Base::iterator;
  using const_iterator = typename Base::const_iterator;

  template <class... Args>
  std::pair<iterator, bool> try_emplace(const Key &key, Args &&...args) {
    auto [idx, inserted] = Base::prepareInsert(key);
    if (inserted) {
      Slot &slot = Base::slotAt(idx);
      slot.first = key;
      slot.second = Value(std::forward<Args>(args)...);
    }
    return {iterator(this, idx), inserted};
  }

  std::pair<iterator, bool> emplace(const Key &key, Value value) {
    auto [idx, inserted] = Base::prepareInsert(key);
    if (inserted) {
      Slot &slot = Base::slotAt(idx);
      slot.first = key;
      slot.second = std::move(value);
    }
    return {iterator(this, idx), inserted};
  }

  Value &operator[](const Key &key) {
    auto [idx, inserted] = Base::prepareInsert(key);
    Slot &slot = Base::slotAt(idx);
    if (inserted)
      slot.first = key;
    return slot.second;
  }

  const Value &at(const Key &key) const {
    const auto it = this->find(key);
    if (it == this->end())
      throw std::out_of_range("FlatMap::at missing key");
    return it->second;
  }

  Value &at(const Key &key) {
    const auto it = this->find(key);
    if (it == this->end())
      throw std::out_of_range("FlatMap::at missing key");
    return it->second;
  }
};

inline bool validLightValue(std::uint8_t value) { return value <= 15; }

// 判断方块在原版光照传播中是否可视为“无遮挡形状”（空气、非遮挡方块或未启用基于形状的面遮挡）。
inline bool emptyShape(const BlockLightProperties &properties) {
  return !properties.supported || !properties.canOcclude ||
         !properties.useShapeForLightOcclusion;
}

// 计算光进入该方块后的衰减量：受支持方块至少衰减 1 级，未受支持方块保守视为完全阻挡（16）。
inline int lightOpacity(const BlockLightProperties &properties) {
  return properties.supported ? std::max(1, int(properties.dampening)) : 16;
}

// 返回空气/虚空等无阻挡方块的默认光学属性常量。
const BlockLightProperties &emptyLightProperties();
bool hasLightFaceRect(const BlockLightProperties &properties, int direction);
bool faceShapesOcclude(const BlockLightProperties &from,
                       const BlockLightProperties &to, int direction);

// 世界空间方块坐标。
struct LightPos {
  int x = 0;
  int y = 0;
  int z = 0;
  bool operator==(const LightPos &) const = default;
};

struct LightPosHash {
  std::size_t operator()(const LightPos &position) const noexcept {
    std::uint64_t value = static_cast<std::uint32_t>(position.x);
    value = value * 0x9E3779B185EBCA87ull ^
            static_cast<std::uint32_t>(position.y);
    value = value * 0xC2B2AE3D27D4EB4Full ^
            static_cast<std::uint32_t>(position.z);
    return static_cast<std::size_t>(value ^ (value >> 32));
  }
};

// 三维区块段坐标 `(sectionX, sectionY, sectionZ)`。
struct SectionPos3 {
  int x = 0;
  int y = 0;
  int z = 0;
  bool operator==(const SectionPos3 &) const = default;
};

struct SectionPos3Hash {
  std::size_t operator()(const SectionPos3 &position) const noexcept {
    std::uint64_t value = static_cast<std::uint32_t>(position.x);
    value = value * 0x9E3779B185EBCA87ull ^
            static_cast<std::uint32_t>(position.y);
    value = value * 0xC2B2AE3D27D4EB4Full ^
            static_cast<std::uint32_t>(position.z);
    return static_cast<std::size_t>(value ^ (value >> 32));
  }
};

// 与 `LightDirection` 一一对应的 6 个正交邻居偏移表（下、上、北、南、西、东）。
inline constexpr std::array<LightPos, 6> kDirectionOffsets{{
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

inline LightPos offset(LightPos position, int direction) {
  const LightPos delta = kDirectionOffsets[static_cast<std::size_t>(direction)];
  return {position.x + delta.x, position.y + delta.y, position.z + delta.z};
}

// 单个区块段在“未计算”阶段收到的入向光照。
// 存在原因：原版会把邻居推来的光直接写进已分配的光照段；我们的“未计算”区块还没有
// 光照壳，写不进去。跨区块的光只可能从 4 个水平侧面进入（同区块内部的段间传播在
// 同一个批次里完成），所以每段只需保存 4 x 16 x 16 格、4 bit/格 = 512 字节。
// 用 max 合并，天然去重，也不需要容量上限。
struct DeferredSectionInbox {
  static constexpr int kFaceCount = 4;
  static constexpr int kFaceNibbles = 256;
  static constexpr int kFaceBytes = kFaceNibbles / 2;   // 128
  // 面顺序对应“进入方向的反向”：(north,south,west,east) 边缘 = 方向 3,2,5,4。
  std::array<std::array<std::uint8_t, kFaceBytes>, kFaceCount> faces{};

  std::uint8_t get(int face, int u, int v) const {
    const int index = (v << 4) | u;
    const std::uint8_t packed = faces[face][index >> 1];
    return static_cast<std::uint8_t>((packed >> ((index & 1) * 4)) & 15u);
  }
  void raise(int face, int u, int v, std::uint8_t level) {
    const int index = (v << 4) | u;
    std::uint8_t &packed = faces[face][index >> 1];
    const int shift = (index & 1) * 4;
    if (level <= static_cast<std::uint8_t>((packed >> shift) & 15u))
      return;
    packed = static_cast<std::uint8_t>((packed & ~(0x0Fu << shift)) |
                                       ((level & 15u) << shift));
  }
  bool empty() const {
    for (const auto &face : faces)
      for (std::uint8_t packed : face)
        if (packed != 0)
          return false;
    return true;
  }
};
// 单区块的收件箱：只保存有入向光的段（通常 1~3 个）。
using DeferredChunkInbox = std::vector<std::pair<int, DeferredSectionInbox>>;

// 按区块段首元素 `sectionY` 计算连续段数组中的下标，越界时返回 `values.size()`。
template <class T>
std::size_t vectorIndex(const std::vector<T> &values, int sectionY) {
  if (values.empty())
    return values.size();
  const int minY = values.front()->sectionY;
  const int index = sectionY - minY;
  return index >= 0 && static_cast<std::size_t>(index) < values.size()
             ? static_cast<std::size_t>(index)
             : values.size();
}

// 传入无锁环形队列 `inputQueue` 的单格几何变更事件，附带单调序号用于与控制队列定序合并。
struct GeometryInput {
  std::uint64_t sequence = 0;
  LightCellUpdate update;
};
static_assert(sizeof(GeometryInput) == 32);

// 在合批与合并阶段保留的“单格最新方块状态”记录，仅保留序号更高的写覆盖。
struct OwnedCell {
  std::uint64_t sequence = 0;
  BlockState::Id state = 0;
};

// 归属于同一区块的待处理合批控制与方块变更集合。
// 存在原因：将同一合批窗口内对同一区块的加载、卸载与多次离散方块修改折叠到一条记录中，避免重复构建/传播同一区块。
struct PendingChunk {
  std::uint64_t key = 0;
  std::uint64_t generation = 0;
  std::uint64_t inputSequence = 0;
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
  std::unique_ptr<LightChunkInput> load;
  bool unload = false;
  // 首次计算请求（`LightService::requestChunkCompute`）：几何已注册、空壳已存在，
  // 本批对其实施一次完整播种（seedChunk + seedIncoming）并置为需要发布。
  bool compute = false;
  // 低内存模式：把“自有几何镜像”切换成“直读 World 几何”。切换时 World 区块的
  // 方块内容与镜像完全一致，因此不改光照值、不推进代次、不触发重算，只释放镜像。
  bool adoptDirectGeometry = false;
  int chunkX = 0;
  int chunkZ = 0;
  std::unordered_map<std::uint32_t, OwnedCell> ownedCells;
};

// 写时复制（COW）辅助：若快照已被赋予非 0 发布版本号（可能已入队或被主线程持有），则克隆一份新头部供当前批次修改并清零 `revision`。
inline void mutableLayerSnapshot(std::shared_ptr<LightChunkSnapshot> &slot) {
  if (slot && slot->revision != 0) {
    slot = std::make_shared<LightChunkSnapshot>(*slot);
    slot->revision = 0;
  }
}

// 传播过程中单个区块的可变工作副本。
// 存在原因：在区块发生修改前与已发布快照共享 `shared_ptr`；一旦发生修改，通过 `sectionForEdit` 按段执行写时复制，从而只克隆真正变脏的段头。
struct WorkingChunk {
  std::uint64_t key = 0;
  std::uint64_t lifecycleEpoch = 0;
  int chunkX = 0;
  int chunkZ = 0;
  std::shared_ptr<LightChunkSnapshot> snapshot;
  std::vector<int> dirtySections;
  bool dirty = false;
  bool propagated = false;
  bool inPlace = false;

  // 按需写时复制指定 `sectionY` 的快照头部，返回可安全写入的段指针。
  LightSectionSnapshot *sectionForEdit(int sectionY);
};

// 工作线程持有的单区块完整光学几何镜像（仅标准模式使用；直读模式使用 `DirectChunkMeta`）。
struct GeometryChunk {
  int chunkX = 0;
  int chunkZ = 0;
  std::uint64_t lifecycleEpoch = 0;
  std::vector<GeometrySection> sections;
};

// 一轮合批经 `prepareBatch` 折叠并差分后的输入摘要，供方块光与天空光两个传播阶段共用。
struct PreparedBatch {
  FlatSet<std::uint64_t, UInt64Hash> touched;
  FlatSet<std::uint64_t, UInt64Hash> dirty;
  FlatSet<std::uint64_t, UInt64Hash> removed;
  FlatSet<std::uint64_t, UInt64Hash> replaced;
  FlatSet<std::uint64_t, UInt64Hash> loaded;
  FlatSet<std::uint64_t, UInt64Hash> persistedLoaded;
  // 只注册了几何、停在“未计算”的区块（`Completion::Deferred`）。
  FlatSet<std::uint64_t, UInt64Hash> registered;
  // 本批首次计算的区块（“未计算” → “完整”）。
  FlatSet<std::uint64_t, UInt64Hash> computed;
  FlatSet<std::uint64_t, UInt64Hash> affected;
  std::vector<LightPos> changedPositions;
  std::uint64_t maxGeneration = 0;
};

// 单光照层单批次执行期间的所有工作区块与已移除区块集合。
struct LayerWorkspace {
  FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> chunks;
  FlatSet<std::uint64_t, UInt64Hash> removed;
};

// 单光照层执行结束后的输出（包含工作区及本轮是否完全收敛无缺元数据）。
struct LayerOutcome {
  LayerWorkspace layers;
  bool complete = false;
};

// 追踪网络协议可见的“已收敛基线版本”及跨未收敛批次累积的脏段列表。
// 存在原因：若某次传播因邻居缺失/未支持方块而未收敛（`protocolReady == false`），增量脏段不能推进协议基准版本号，而必须暂存到 `pendingDirty` 中，直到下次完全收敛时一并合并发布。
struct ProtocolBaseMeta {
  bool hasBase = false;
  std::uint64_t epoch = 0;
  std::uint64_t revision = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  std::uint64_t pendingEpoch = 0;
  std::vector<int> pendingDirty;
};

// 工作线程经 `resultQueue` 投递给主线程的单层发布项。
struct LayerResult {
  LightLayer layer = LightLayer::Block;
  std::uint64_t key = 0;
  std::shared_ptr<LightChunkSnapshot> snapshot;
};

// 增亮 BFS 任务，记录目标坐标、源亮度、允许传播方向掩码及是否来自空段/自发光（直射天空光）。
struct IncreaseTask {
  LightPos position;
  std::uint8_t level = 0;
  std::uint8_t directions = 0;
  bool fromEmpty = false;
  bool fromEmission = false;
};

// 减暗 BFS 任务，记录坐标、原传播亮度及允许扩散的邻居方向掩码。
struct DecreaseTask {
  LightPos position;
  std::uint8_t level = 0;
  std::uint8_t directions = 0;
};

// 传播器内部使用的可懒扩容单个区块段光照值容器（包装 `LightLayerRef`）。
// 存在原因：在只读或整段均一填充（如 `fill(15)`）时保持零页分配的 `Uniform` 形态；仅当局部单元被修改为异质值时才通过 `ensureMixed()` 懒分配/展开为 2048 字节半字节页，并通过代理 `CellRef` 提供数组式下标读写。
class LightValues {
public:
  class CellRef {
  public:
    CellRef(LightValues &owner, std::size_t index)
        : owner_(owner), index_(index) {}
    operator std::uint8_t() const { return owner_.value(index_); }
    CellRef &operator=(std::uint8_t value) {
      owner_.write(index_, value);
      return *this;
    }

  private:
    LightValues &owner_;
    std::size_t index_;
  };

  LightValues();
  LightValues(const LightValues &) = default;
  LightValues &operator=(const LightValues &) = default;
  LightValues(LightValues &&) noexcept = default;
  LightValues &operator=(LightValues &&) noexcept = default;

  static LightValues fromLayer(const LightLayerRef &layer);

  CellRef operator[](std::size_t index) { return {*this, index}; }
  std::uint8_t operator[](std::size_t index) const { return value(index); }
  void fill(std::uint8_t value);
  // 用从上方继承来的 256 列底面光照一次性平铺填充整个 16x16x16 混合区块段，避免逐格位操作开销。
  void fillInheritedMixed(const std::array<std::uint8_t, 256> &cols);
  // 检测当前层是否全段相同（若是混合页则扫描确认是否已退化为均一值，以便提交时折叠回 `Uniform` 省内存）。
  std::optional<std::uint8_t> uniformValue() const;
  const LightLayerRef &ref() const { return layer_; }
  const LightLayerSnapshot::PackedPagePtr &shared() const {
    return layer_.shared();
  }

private:
  std::uint8_t value(std::size_t index) const {
    if (layer_.isUniform())
      return layer_.uniformValue();
    const auto &page = layer_.shared();
    if (!page)
      return 0;
    const std::uint8_t packed =
        page->bytes[index >> 1].load(std::memory_order_relaxed);
    return static_cast<std::uint8_t>((packed >> ((index & 1) * 4)) & 15);
  }
  void write(std::size_t index, std::uint8_t value) {
    if (!layer_.isMixed())
      ensureMixed();
    layer_.writeCell(index, value);
  }
  void ensureMixed();

  LightLayerRef layer_;
};

// 工作线程自持的只读上下文宿主，暴露给独立的方块光与天空光传播器使用（除队列计数遥测与本地 L1 几何缓存外均为只读）。
// 存在原因：统一屏蔽“标准 `GeometryChunk` 镜像”与“直读 `DirectWorldGeometrySource` 视图”两种底层数据源差异，并用 64 槽 L1 缓存加速高频坐标的光学属性查询。
struct LightEngineHost {
  LightEngineHost(
      int minSection, int sectionCount, const LightOpticalTable &table,
      const FlatMap<std::uint64_t, GeometryChunk, UInt64Hash> &geo,
      const FlatMap<std::uint64_t, DirectChunkMeta, UInt64Hash> &directMeta,
      const LightWorkerOptions::DirectWorldGeometrySource &directSource,
      const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                    UInt64Hash> &readyBlock,
      const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                    UInt64Hash> &readySky,
      std::uint64_t &queueEntries, std::uint64_t &unloadedSkips)
      : minBlockSectionY(minSection), blockSectionCount(sectionCount),
        opticalTable(table), geometry(geo), directChunks(directMeta),
        directWorld(directSource), lastReadyBlock(readyBlock),
        lastReadySky(readySky), lightQueueEntries(queueEntries),
        unloadedNeighborSkips(unloadedSkips) {}

  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  const LightOpticalTable &opticalTable;
  const FlatMap<std::uint64_t, GeometryChunk, UInt64Hash> &geometry;
  const FlatMap<std::uint64_t, DirectChunkMeta, UInt64Hash> &directChunks;
  LightWorkerOptions::DirectWorldGeometrySource directWorld{};
  const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>, UInt64Hash>
      &lastReadyBlock;
  const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>, UInt64Hash>
      &lastReadySky;
  std::uint64_t &lightQueueEntries;
  std::uint64_t &unloadedNeighborSkips;
  // 每个光照层各一个“未计算区块收件箱”。
  FlatMap<std::uint64_t, DeferredChunkInbox, UInt64Hash> *deferredBlock = nullptr;
  FlatMap<std::uint64_t, DeferredChunkInbox, UInt64Hash> *deferredSky = nullptr;

  // 目标区块已注册几何但还没算过（“未计算”）时，把 \`level\` 以 max 方式记进它对应
  // 段的那一侧 16x16 收件箱并返回 true；返回 false 表示目标真的不存在（世界边界外
  // 的区块）或已经有光照壳，调用方按“跳过”处理。
  bool deferIncoming(bool sky, LightPos to, int direction, std::uint8_t level) const {
    auto *inbox = sky ? deferredSky : deferredBlock;
    if (inbox == nullptr)
      return false;
    const std::uint64_t key = chunkKey(to.x >> 4, to.z >> 4);
    if (!geometry.contains(key))
      return false;
    const auto &ready = sky ? lastReadySky : lastReadyBlock;
    if (ready.find(key) != ready.end())
      return false;
    DeferredChunkInbox &chunkInbox = (*inbox)[key];
    const int sectionY = to.y >> 4;
    DeferredSectionInbox *section = nullptr;
    for (auto &entry : chunkInbox) {
      if (entry.first == sectionY) {
        section = &entry.second;
        break;
      }
    }
    if (section == nullptr) {
      chunkInbox.emplace_back(sectionY, DeferredSectionInbox{});
      section = &chunkInbox.back().second;
    }
    const int face = (direction ^ 1) - 2;
    const int u = (direction == 2 || direction == 3) ? (to.x & 15) : (to.z & 15);
    section->raise(face, u, to.y & 15, level);
    return true;
  }

  const LightOpticalClass &opticalClass(std::uint16_t id) const {
    return opticalTable[static_cast<std::uint8_t>(id)];
  }
  const BlockLightProperties &propertiesForState(BlockState::Id state) const {
    return opticalClass(opticalTable.classForState(state)).properties;
  }
  struct GeoCacheEntry {
    std::uint64_t key = 0;
    const GeometryChunk *chunk = nullptr;
    bool valid = false;
  };
  struct DirectSectionCacheEntry {
    std::uint64_t key = 0;
    int sectionY = 0;
    bool present = false;
    bool valid = false;
    LightSectionView view{};
  };
  const DirectSectionCacheEntry *
  lookupDirectSectionCache(std::uint64_t key, int sectionY) const;
  const GeometryChunk *geometryChunk(std::uint64_t key) const;
  bool hasChunkGeometry(std::uint64_t key) const;
  const GeometrySection *geometrySection(std::uint64_t key, int sectionY) const;
  std::optional<LightSectionView> directSectionView(std::uint64_t key,
                                                    int sectionY) const;
  std::optional<bool> geometrySectionNonEmpty(std::uint64_t key,
                                              int sectionY) const;
  // 查询世界坐标处的方块光学属性；若遇到未受支持方块，将 `incomplete` 置为 true 以阻止本轮标记为 `protocolReady`。
  const BlockLightProperties &blockPropertiesAt(int x, int y, int z,
                                                bool &incomplete) const;
  const BlockLightProperties *sectionPropertiesAt(std::uint64_t key, int x,
                                                   int y, int z) const;
  const std::shared_ptr<LightChunkSnapshot> &storedLayerSnapshot(
      LightLayer layer, std::uint64_t key) const;

private:
  mutable std::array<GeoCacheEntry, 64> geoCache_{};
  mutable std::array<DirectSectionCacheEntry, 64> directSectionCache_{};
};

} // namespace redstone::light_internal
