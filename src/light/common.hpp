#pragma once

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

#include "redstone/light/snapshot.hpp"
#include "redstone/light/types.hpp"

namespace redstone {
const BlockLightProperties &unsupportedLightProperties() noexcept;
}

namespace redstone::light_internal {

struct ThreadAffinityResult {
  std::uint64_t mask = 0;
  std::uint32_t error = 0;
};

double currentThreadCpuMs();
#if defined(_WIN32) || defined(__linux__)
ThreadAffinityResult applyThreadAffinity(std::thread::native_handle_type handle,
                                         std::uintptr_t mask);
#endif

inline std::uint64_t chunkKey(int x, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
         static_cast<std::uint32_t>(z);
}

inline std::size_t chunkL1Slot(int chunkX, int chunkZ) {
  return static_cast<std::size_t>(((chunkX & 7) << 3) | (chunkZ & 7));
}

inline std::size_t chunkL1Slot(std::uint64_t key) {
  const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
  const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
  return chunkL1Slot(chunkX, chunkZ);
}

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

// Compact open-addressing hash table with Swiss-style 1-byte H2 tags.
// Keeps probes in contiguous byte cache lines and eliminates heap nodes.
template <class Slot, class Key, class Hash> class FlatTable {
  static constexpr std::uint8_t kEmpty = 0;
  static constexpr std::uint8_t kTombstone = 1;
  static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

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

inline bool emptyShape(const BlockLightProperties &properties) {
  return !properties.supported || !properties.canOcclude ||
         !properties.useShapeForLightOcclusion;
}

inline int lightOpacity(const BlockLightProperties &properties) {
  return properties.supported ? std::max(1, int(properties.dampening)) : 16;
}

const BlockLightProperties &emptyLightProperties();
bool hasLightFaceRect(const BlockLightProperties &properties, int direction);
bool faceShapesOcclude(const BlockLightProperties &from,
                       const BlockLightProperties &to, int direction);

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

inline constexpr std::array<LightPos, 6> kDirectionOffsets{{
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

inline LightPos offset(LightPos position, int direction) {
  const LightPos delta = kDirectionOffsets[static_cast<std::size_t>(direction)];
  return {position.x + delta.x, position.y + delta.y, position.z + delta.z};
}

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

struct GeometryInput {
  std::uint64_t sequence = 0;
  LightCellUpdate update;
};
static_assert(sizeof(GeometryInput) == 32);

struct OwnedCell {
  std::uint64_t sequence = 0;
  BlockState::Id state = 0;
};

struct PendingChunk {
  std::uint64_t key = 0;
  std::uint64_t generation = 0;
  std::uint64_t inputSequence = 0;
  std::uint64_t lifecycleEpoch = 0;
  std::uint64_t revision = 0;
  std::unique_ptr<LightChunkInput> load;
  bool unload = false;
  std::unordered_map<std::uint32_t, OwnedCell> ownedCells;
};

inline void mutableLayerSnapshot(std::shared_ptr<LightChunkSnapshot> &slot) {
  if (slot && slot->revision != 0) {
    slot = std::make_shared<LightChunkSnapshot>(*slot);
    slot->revision = 0;
  }
}

struct WorkingChunk {
  std::uint64_t key = 0;
  std::uint64_t lifecycleEpoch = 0;
  int chunkX = 0;
  int chunkZ = 0;
  std::shared_ptr<LightChunkSnapshot> snapshot;
  std::vector<int> dirtySections;
  bool dirty = false;
  bool inPlace = false;

  LightSectionSnapshot *sectionForEdit(int sectionY);
};

struct GeometryChunk {
  int chunkX = 0;
  int chunkZ = 0;
  std::uint64_t lifecycleEpoch = 0;
  std::vector<GeometrySection> sections;
};

struct PreparedBatch {
  FlatSet<std::uint64_t, UInt64Hash> touched;
  FlatSet<std::uint64_t, UInt64Hash> dirty;
  FlatSet<std::uint64_t, UInt64Hash> removed;
  FlatSet<std::uint64_t, UInt64Hash> replaced;
  FlatSet<std::uint64_t, UInt64Hash> loaded;
  FlatSet<std::uint64_t, UInt64Hash> persistedLoaded;
  FlatSet<std::uint64_t, UInt64Hash> affected;
  std::vector<LightPos> changedPositions;
  std::uint64_t maxGeneration = 0;
};

struct LayerWorkspace {
  FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> chunks;
  FlatSet<std::uint64_t, UInt64Hash> removed;
};

struct LayerOutcome {
  LayerWorkspace layers;
  bool complete = false;
};

struct ProtocolBaseMeta {
  bool hasBase = false;
  std::uint64_t epoch = 0;
  std::uint64_t revision = 0;
  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  std::uint64_t pendingEpoch = 0;
  std::vector<int> pendingDirty;
};

struct LayerResult {
  LightLayer layer = LightLayer::Block;
  std::uint64_t key = 0;
  std::shared_ptr<LightChunkSnapshot> snapshot;
};

struct IncreaseTask {
  LightPos position;
  std::uint8_t level = 0;
  std::uint8_t directions = 0;
  bool fromEmpty = false;
  bool fromEmission = false;
};

struct DecreaseTask {
  LightPos position;
  std::uint8_t level = 0;
  std::uint8_t directions = 0;
};

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
  void fillInheritedMixed(const std::array<std::uint8_t, 256> &cols);
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

// Worker-owned context exposed read-only (except queue entry telemetry and
// its local L1 geometry cache) to independent block and sky propagators.
struct LightEngineHost {
  LightEngineHost(
      int minSection, int sectionCount, const LightOpticalTable &table,
      const FlatMap<std::uint64_t, GeometryChunk, UInt64Hash> &geo,
      const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                    UInt64Hash> &readyBlock,
      const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>,
                    UInt64Hash> &readySky,
      std::uint64_t &queueEntries, std::uint64_t &unloadedSkips)
      : minBlockSectionY(minSection), blockSectionCount(sectionCount),
        opticalTable(table), geometry(geo), lastReadyBlock(readyBlock),
        lastReadySky(readySky), lightQueueEntries(queueEntries),
        unloadedNeighborSkips(unloadedSkips) {}

  int minBlockSectionY = 0;
  int blockSectionCount = 0;
  const LightOpticalTable &opticalTable;
  const FlatMap<std::uint64_t, GeometryChunk, UInt64Hash> &geometry;
  const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>, UInt64Hash>
      &lastReadyBlock;
  const FlatMap<std::uint64_t, std::shared_ptr<LightChunkSnapshot>, UInt64Hash>
      &lastReadySky;
  std::uint64_t &lightQueueEntries;
  std::uint64_t &unloadedNeighborSkips;

  const LightOpticalClass &opticalClass(std::uint16_t id) const {
    return opticalTable[static_cast<std::uint8_t>(id)];
  }
  const GeometryChunk *geometryChunk(std::uint64_t key) const {
    const std::size_t slot = chunkL1Slot(key);
    GeoCacheEntry &entry = geoCache_[slot];
    if (entry.valid && entry.key == key)
      return entry.chunk;
    const auto found = geometry.find(key);
    entry.key = key;
    entry.chunk = found == geometry.end() ? nullptr : &found->second;
    entry.valid = true;
    return entry.chunk;
  }
  const GeometrySection *geometrySection(std::uint64_t key, int sectionY) const;
  const BlockLightProperties &blockPropertiesAt(int x, int y, int z,
                                                bool &incomplete) const;
  const std::shared_ptr<LightChunkSnapshot> &storedLayerSnapshot(
      LightLayer layer, std::uint64_t key) const;

private:
  struct GeoCacheEntry {
    std::uint64_t key = 0;
    const GeometryChunk *chunk = nullptr;
    bool valid = false;
  };
  mutable std::array<GeoCacheEntry, 64> geoCache_{};
};

} // namespace redstone::light_internal
