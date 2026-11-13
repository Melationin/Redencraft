#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace redstone::light_internal {

template <class T> class SpscRing {
public:
  explicit SpscRing(std::size_t capacity)
      : slots_(std::max<std::size_t>(capacity, 2)) {}

  bool push(T value) {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= slots_.size())
      return false;
    slots_[static_cast<std::size_t>(head % slots_.size())] =
        std::move(value);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  const T *front() const {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto head = head_.load(std::memory_order_acquire);
    if (tail == head)
      return nullptr;
    return &slots_[static_cast<std::size_t>(tail % slots_.size())];
  }

  bool pop(T &value) {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto head = head_.load(std::memory_order_acquire);
    if (tail == head)
      return false;
    value = std::move(slots_[static_cast<std::size_t>(tail % slots_.size())]);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool empty() const {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }
  bool full() const {
    return head_.load(std::memory_order_relaxed) -
               tail_.load(std::memory_order_acquire) >=
           slots_.size();
  }
  std::size_t size() const {
    return static_cast<std::size_t>(
        head_.load(std::memory_order_acquire) -
        tail_.load(std::memory_order_acquire));
  }

private:
  std::vector<T> slots_;
  std::atomic<std::uint64_t> head_{0};
  std::atomic<std::uint64_t> tail_{0};
};

// SPSC circular ring buffer that automatically doubles its power-of-2
// capacity (like std::vector) when full. Steady-state traffic wraps within the
// active ring without further heap allocation, while the consumer drains any
// older ring before releasing it.
template <class T> class GrowableSpscRing {
  struct Ring {
    explicit Ring(std::size_t capacity)
        : slots(std::bit_ceil(std::max<std::size_t>(capacity, 2))),
          mask(slots.size() - 1) {}
    std::vector<T> slots;
    std::size_t mask;
    alignas(64) std::atomic<std::size_t> head{0};
    std::size_t cachedTail = 0;
    std::atomic<Ring *> next{nullptr};
    alignas(64) std::atomic<std::size_t> tail{0};
    std::size_t cachedHead = 0;
  };

public:
  explicit GrowableSpscRing(std::size_t capacity)
      : read_(new Ring(capacity)), write_(read_) {}
  ~GrowableSpscRing() {
    Ring *ring = read_;
    while (ring) {
      Ring *next = ring->next.load(std::memory_order_acquire);
      delete ring;
      ring = next;
    }
  }
  GrowableSpscRing(const GrowableSpscRing &) = delete;
  GrowableSpscRing &operator=(const GrowableSpscRing &) = delete;

  bool push(T value) {
    Ring *ring = write_;
    const auto head = ring->head.load(std::memory_order_relaxed);
    if (head - ring->cachedTail >= ring->slots.size()) {
      ring->cachedTail = ring->tail.load(std::memory_order_acquire);
      if (head - ring->cachedTail >= ring->slots.size()) {
        // Exponential 2x capacity growth when the current circular ring is
        // full.
        Ring *next = new Ring(ring->slots.size() * 2);
        next->slots[0] = std::move(value);
        next->head.store(1, std::memory_order_relaxed);
        write_ = next;
        published_.store(published_.load(std::memory_order_relaxed) + 1,
                         std::memory_order_release);
        ring->next.store(next, std::memory_order_release);
        return true;
      }
    }
    ring->slots[head & ring->mask] = std::move(value);
    // Publish the logical count before making the slot visible. A consumer
    // that acquires head may already advance its tail, so incrementing the
    // count afterwards could let consumed temporarily exceed published.
    published_.store(published_.load(std::memory_order_relaxed) + 1,
                     std::memory_order_release);
    ring->head.store(head + 1, std::memory_order_release);
    return true;
  }

  const T *front() {
    for (;;) {
      Ring *ring = read_;
      const auto tail = ring->tail.load(std::memory_order_relaxed);
      if (tail != ring->cachedHead)
        return &ring->slots[tail & ring->mask];
      ring->cachedHead = ring->head.load(std::memory_order_acquire);
      if (tail != ring->cachedHead)
        return &ring->slots[tail & ring->mask];
      Ring *next = ring->next.load(std::memory_order_acquire);
      if (!next)
        return nullptr;
      // The prior head load may have raced with the producer filling the last
      // slots before linking next. Once next is visible the producer has
      // stopped writing this ring, so this acquire load is final. Never free a
      // ring until that confirmed head equals our consumer tail.
      const auto confirmedHead = ring->head.load(std::memory_order_acquire);
      if (tail != confirmedHead) {
        ring->cachedHead = confirmedHead;
        return &ring->slots[tail & ring->mask];
      }
      read_ = next;
      delete ring;
    }
  }

  bool pop(T &value) {
    if (!front())
      return false;
    Ring *ring = read_;
    const auto tail = ring->tail.load(std::memory_order_relaxed);
    const std::size_t idx = tail & ring->mask;
    value = std::move(ring->slots[idx]);
    ring->slots[idx] = T{};
    ring->tail.store(tail + 1, std::memory_order_release);
    consumed_.store(consumed_.load(std::memory_order_relaxed) + 1,
                    std::memory_order_release);
    return true;
  }

  bool empty() const { return size() == 0; }
  std::size_t size() const {
    // Read the consumer first. published is incremented before head, so every
    // consumed item has already been counted. The producer can still be
    // between those stores, making this an upper bound until head is visible.
    const auto consumed = consumed_.load(std::memory_order_acquire);
    const auto published = published_.load(std::memory_order_acquire);
    return published >= consumed
               ? static_cast<std::size_t>(published - consumed)
               : 0;
  }

private:
  alignas(64) Ring *read_ = nullptr;
  std::atomic<std::uint64_t> consumed_{0};
  alignas(64) Ring *write_ = nullptr;
  std::atomic<std::uint64_t> published_{0};
};

template <class T> class TaskQueue {
public:
  TaskQueue() : values_(1024), mask_(values_.size() - 1) {}
  bool empty() const { return head_ == tail_; }
  const T &front() const { return values_[head_ & mask_]; }
  void clear() {
    head_ = 0;
    tail_ = 0;
  }
  void push(T value) {
    if (tail_ - head_ == values_.size()) {
      const std::size_t newCap = values_.size() * 2;
      if (newCap == 0)
        return;
      std::vector<T> grown(newCap);
      const std::size_t count = tail_ - head_;
      for (std::size_t i = 0; i < count; ++i)
        grown[i] = std::move(values_[(head_ + i) & mask_]);
      values_ = std::move(grown);
      mask_ = values_.size() - 1;
      head_ = 0;
      tail_ = count;
    }
    values_[tail_++ & mask_] = std::move(value);
  }
  T pop() { return std::move(values_[head_++ & mask_]); }

private:
  std::vector<T> values_;
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
  std::size_t mask_ = 0;
};

} // namespace redstone::light_internal
