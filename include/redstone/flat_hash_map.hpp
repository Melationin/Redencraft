#pragma once
// 开放寻址扁平哈希表实现：为区块方块实体表等热点查找提供连续数组探测，避免红黑树与逐节点分配。
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace redstone {

struct TransparentStringHash {
  using is_transparent = void;
  std::uint64_t operator()(std::string_view sv) const noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : sv) {
      h ^= c;
      h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
  }
};

// 开放寻址哈希表（线性探测，容量始终为 2 的幂），用于方块实体表等高频访问映射。
// 原版将其存放在 fastutil 的 Object2ObjectOpenHashMap 中，因此一次查找只需在连续槽位数组上
// 进行一次探测，而无需红黑树遍历及每个条目的独立堆节点分配。
//
// 删除操作会留下墓碑标记（Deleted），并在扩容重哈希时统一回收；无逐项分配、无再平衡、无顺序约束。
// 因此遍历顺序是未指定的：需要对外输出有序列表的调用方（如区块快照）应在收集后自行排序。
template <class Key, class T, class Hash, class KeyEqual = std::equal_to<Key>>
class FlatHashMap {
public:
  using key_type = Key;
  using mapped_type = T;
  using value_type = std::pair<Key, T>;

private:
  template <class K>
  static constexpr bool kTransparentLookup =
      requires { typename Hash::is_transparent; } &&
      requires { typename KeyEqual::is_transparent; } &&
      requires(const Hash &h, const KeyEqual &eq, const Key &a, const K &b) {
        h(b);
        eq(a, b);
      };
  enum class State : std::uint8_t { Empty, Used, Deleted };
  struct Slot {
    std::uint64_t hash = 0;
    value_type entry{};
    State state = State::Empty;
  };

  template <bool Const> class IteratorImpl {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = std::pair<Key, T>;
    using difference_type = std::ptrdiff_t;
    using reference =
        std::conditional_t<Const, const value_type &, value_type &>;
    using pointer = std::conditional_t<Const, const value_type *, value_type *>;
    using map_type =
        std::conditional_t<Const, const FlatHashMap *, FlatHashMap *>;

    IteratorImpl() = default;
    IteratorImpl(map_type map, std::size_t index) : map_(map), index_(index) {
      skipEmpty();
    }
    reference operator*() const { return map_->slots_[index_].entry; }
    pointer operator->() const { return &map_->slots_[index_].entry; }
    IteratorImpl &operator++() {
      ++index_;
      skipEmpty();
      return *this;
    }
    IteratorImpl operator++(int) {
      IteratorImpl copy = *this;
      ++*this;
      return copy;
    }
    bool operator==(const IteratorImpl &) const = default;

  private:
    void skipEmpty() {
      while (index_ < map_->slots_.size() &&
             map_->slots_[index_].state != State::Used)
        ++index_;
    }
    map_type map_ = nullptr;
    std::size_t index_ = 0;
  };

public:
  using iterator = IteratorImpl<false>;
  using const_iterator = IteratorImpl<true>;

  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  iterator begin() { return iterator(this, 0); }
  iterator end() { return iterator(this, slots_.size()); }
  const_iterator begin() const { return const_iterator(this, 0); }
  const_iterator end() const { return const_iterator(this, slots_.size()); }

  iterator find(const Key &key) {
    const std::size_t index = findIndex(key, hashKey(key));
    return index < slots_.size() ? iterator(this, index) : end();
  }
  const_iterator find(const Key &key) const {
    const std::size_t index = findIndex(key, hashKey(key));
    return index < slots_.size() ? const_iterator(this, index) : end();
  }
  template <class K>
    requires kTransparentLookup<K>
  iterator find(const K &key) {
    const std::size_t index = findIndex(key, hashKey(key));
    return index < slots_.size() ? iterator(this, index) : end();
  }
  template <class K>
    requires kTransparentLookup<K>
  const_iterator find(const K &key) const {
    const std::size_t index = findIndex(key, hashKey(key));
    return index < slots_.size() ? const_iterator(this, index) : end();
  }

  bool contains(const Key &key) const {
    return findIndex(key, hashKey(key)) < slots_.size();
  }
  template <class K>
    requires kTransparentLookup<K>
  bool contains(const K &key) const {
    return findIndex(key, hashKey(key)) < slots_.size();
  }

  T &at(const Key &key) {
    const std::size_t index = findIndex(key, hashKey(key));
    if (index >= slots_.size())
      throw std::out_of_range("FlatHashMap::at: missing key");
    return slots_[index].entry.second;
  }
  const T &at(const Key &key) const {
    const std::size_t index = findIndex(key, hashKey(key));
    if (index >= slots_.size())
      throw std::out_of_range("FlatHashMap::at: missing key");
    return slots_[index].entry.second;
  }
  template <class K>
    requires kTransparentLookup<K>
  T &at(const K &key) {
    const std::size_t index = findIndex(key, hashKey(key));
    if (index >= slots_.size())
      throw std::out_of_range("FlatHashMap::at: missing key");
    return slots_[index].entry.second;
  }
  template <class K>
    requires kTransparentLookup<K>
  const T &at(const K &key) const {
    const std::size_t index = findIndex(key, hashKey(key));
    if (index >= slots_.size())
      throw std::out_of_range("FlatHashMap::at: missing key");
    return slots_[index].entry.second;
  }

  void reserve(std::size_t expectedEntries) {
    if (expectedEntries == 0)
      return;
    std::size_t capacity = 8;
    while (capacity * 3 < expectedEntries * 4)
      capacity <<= 1;
    if (capacity > slots_.size())
      rehash(capacity);
  }

  // 当键不存在时插入默认构造的值；与原版 Object2ObjectOpenHashMap.put 一样，
  // 对已存在条目绝不触发重哈希。
  T &operator[](const Key &key) {
    const std::uint64_t hash = hashKey(key);
    const std::size_t index = findIndex(key, hash);
    if (index < slots_.size())
      return slots_[index].entry.second;
    return insertSlot(key, hash).entry.second;
  }

  // 删除指定键；仅当键原本存在时返回 true。
  bool erase(const Key &key) {
    const std::size_t index = findIndex(key, hashKey(key));
    if (index >= slots_.size())
      return false;
    Slot &slot = slots_[index];
    slot.state = State::Deleted;
    slot.entry.second = T{};
    --size_;
    ++tombstones_;
    return true;
  }

  void clear() {
    slots_.clear();
    size_ = 0;
    tombstones_ = 0;
  }

private:
  template <class K> std::uint64_t hashKey(const K &key) const {
    return static_cast<std::uint64_t>(hasher_(key));
  }

  // 若键不存在则返回 slots_.size()。已删除槽位（Deleted）不会截断探测链，
  // 从而保证删除操作绝不会遮蔽探测链后方的有效条目。
  template <class K>
  std::size_t findIndex(const K &key, std::uint64_t hash) const {
    if (slots_.empty())
      return 0;
    const std::size_t mask = slots_.size() - 1;
    std::size_t index = static_cast<std::size_t>(hash) & mask;
    for (;;) {
      const Slot &slot = slots_[index];
      if (slot.state == State::Empty)
        return slots_.size();
      if (slot.state == State::Used && slot.hash == hash &&
          equal_(slot.entry.first, key))
        return index;
      index = (index + 1) & mask;
    }
  }

  Slot &insertSlot(const Key &key, std::uint64_t hash) {
    // 将总占用率（有效项 + 墓碑）限制在 75% 以内以确保探测总能命中空槽，
    // 并在重哈希时将有效负载控制到约 50% 以保持探测链较短。
    if (slots_.empty()) {
      rehash(8);
    } else if ((size_ + tombstones_ + 1) * 4 > slots_.size() * 3) {
      std::size_t capacity = 8;
      while (capacity < (size_ + 1) * 2)
        capacity <<= 1;
      rehash(capacity);
    }
    const std::size_t mask = slots_.size() - 1;
    std::size_t index = static_cast<std::size_t>(hash) & mask;
    std::size_t firstDeleted = slots_.size();
    for (;;) {
      Slot &slot = slots_[index];
      if (slot.state == State::Empty) {
        const std::size_t target =
            firstDeleted < slots_.size() ? firstDeleted : index;
        Slot &dest = slots_[target];
        if (dest.state == State::Deleted)
          --tombstones_;
        dest.hash = hash;
        dest.entry.first = key;
        dest.entry.second = T{};
        dest.state = State::Used;
        ++size_;
        return dest;
      }
      if (slot.state == State::Deleted && firstDeleted == slots_.size())
        firstDeleted = index;
      index = (index + 1) & mask;
    }
  }

  void rehash(std::size_t capacity) {
    if (capacity < 8)
      capacity = 8;
    std::size_t rounded = 8;
    while (rounded < capacity)
      rounded <<= 1;
    std::vector<Slot> previous = std::move(slots_);
    slots_ = std::vector<Slot>(rounded);
    size_ = 0;
    tombstones_ = 0;
    const std::size_t mask = slots_.size() - 1;
    for (Slot &slot : previous) {
      if (slot.state != State::Used)
        continue;
      std::size_t index = static_cast<std::size_t>(slot.hash) & mask;
      while (slots_[index].state == State::Used)
        index = (index + 1) & mask;
      slots_[index] = std::move(slot);
      ++size_;
    }
  }

  std::vector<Slot> slots_;
  std::size_t size_ = 0;
  std::size_t tombstones_ = 0;
  Hash hasher_{};
  KeyEqual equal_{};
};

} // namespace redstone
