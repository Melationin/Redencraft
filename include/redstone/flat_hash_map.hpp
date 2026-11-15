#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace redstone {

// Open-addressed hash map (linear probing, power-of-two capacity) for the block
// entity tables. Vanilla keeps those in fastutil's Object2ObjectOpenHashMap, so
// a lookup is one probe into a contiguous slot array instead of a red-black
// tree walk plus a node allocation per entry.
//
// Erase leaves a tombstone; growth reclaims them. There is no per-entry
// allocation, no rebalancing and no ordering. Iteration order is therefore
// unspecified: callers that expose an ordered list (chunk snapshots) sort after
// collecting.
template <class Key, class T, class Hash> class FlatHashMap {
public:
  using value_type = std::pair<Key, T>;

private:
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

  bool contains(const Key &key) const {
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

  // Inserts a default constructed value when the key is absent. Like vanilla's
  // Object2ObjectOpenHashMap.put this never rehashes an existing entry.
  T &operator[](const Key &key) {
    const std::uint64_t hash = hashKey(key);
    const std::size_t index = findIndex(key, hash);
    if (index < slots_.size())
      return slots_[index].entry.second;
    return insertSlot(key, hash).entry.second;
  }

  // Returns true when the key was present.
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
  std::uint64_t hashKey(const Key &key) const {
    return static_cast<std::uint64_t>(hasher_(key));
  }

  // Returns slots_.size() when the key is absent. Deleted slots do not
  // terminate a probe chain, so an erase can never hide a later entry.
  std::size_t findIndex(const Key &key, std::uint64_t hash) const {
    if (slots_.empty())
      return 0;
    const std::size_t mask = slots_.size() - 1;
    std::size_t index = static_cast<std::size_t>(hash) & mask;
    for (;;) {
      const Slot &slot = slots_[index];
      if (slot.state == State::Empty)
        return slots_.size();
      if (slot.state == State::Used && slot.hash == hash &&
          slot.entry.first == key)
        return index;
      index = (index + 1) & mask;
    }
  }

  Slot &insertSlot(const Key &key, std::uint64_t hash) {
    // Keep at most 75% occupancy (used + tombstones) so a probe always finds an
    // empty slot, and grow to ~50% load to keep the chains short.
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
};

} // namespace redstone
