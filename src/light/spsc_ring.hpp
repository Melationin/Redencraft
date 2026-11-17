#pragma once

// 单生产者单消费者（SPSC）无锁环形队列与引擎内部 FIFO 任务队列：用于主线程与光照工作线程间低开销无锁通信及单线程 BFS 传播排队。

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace redstone::light_internal {

// 固定容量的单生产者单消费者（SPSC）无锁环形缓冲区。
// 存在原因：用于无需动态扩容的轻量 SPSC 场景，以 `acquire`/`release` 内存序避免互斥锁开销。
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

// 可自动倍增扩容的 SPSC 环形缓冲区。
// 存在原因：热路径的方块更新、控制命令与快照发布都需要无锁投递且不能因突发流量丢包；当当前环满时像 `std::vector` 一样分配两倍大小的 2 的幂新环并串接在 `next` 指针后，稳态下在活动环内零堆分配复用，消费者彻底排空旧环后再释放之。
template <class T> class GrowableSpscRing {
  struct Ring {
    explicit Ring(std::size_t capacity)
        : slots(std::bit_ceil(std::max<std::size_t>(capacity, 2))),
          mask(slots.size() - 1) {}
    std::vector<T> slots;
    std::size_t mask;
    // 将生产者指针、消费者指针及其缓存的对方游标隔离到不同缓存行，避免伪共享。
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
        // 当前环形缓冲区已满时，按 2 倍指数扩容并链接新环。
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
    // 必须先发布逻辑计数再让槽位对消费者可见：一旦 `head` 可见，消费者即可消费并推进 `consumed_`，若后递增计数会导致短暂出现 `consumed > published`。
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
      // 前一次 `head` 读取可能与生产者填满最后几个槽位并挂接 `next` 的操作并发竞争；一旦观测到 `next` 非空，生产者已停止写入当前环，因此这次 `acquire` 读取得到的 `confirmedHead` 是最终值，在 `tail == confirmedHead` 之前绝不能提前释放当前环。
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
    // 先读取消费者计数 `consumed_`：由于 `published_` 总是先于 `head` 递增，所有已消费元素必已被计入 `published_`；若生产者正处于两个 store 之间，返回值在 `head` 可见前至多略高估（上界）。
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

// 单线程工作器内部使用的可扩容连续循环队列。
// 存在原因：替代 `std::deque`（分段块分配且指针跳转多），为光照传播的增亮/减暗/检查 BFS 队列提供连续内存访问和跨批次复用的零重置 `clear()`。
template <class T> class TaskQueue {
public:
  static constexpr std::size_t kMinCapacity = 1024;

  TaskQueue() : values_(kMinCapacity), mask_(values_.size() - 1) {}
  bool empty() const { return head_ == tail_; }
  std::size_t size() const noexcept { return tail_ - head_; }
  std::size_t capacity() const noexcept { return values_.size(); }
  std::size_t freeSlots() const noexcept { return values_.size() - size(); }
  std::size_t capacityBytes() const noexcept {
    return values_.capacity() * sizeof(T);
  }
  const T &front() const { return values_[head_ & mask_]; }
  void clear() {
    head_ = 0;
    tail_ = 0;
  }
  // 当空闲槽位占比过高（已用 <= 1/4 容量且超过最小保留容量）时按 2 的幂收缩底层缓冲，释放历史峰值内存。
  bool needsCompact(std::size_t minCapacity = kMinCapacity) const noexcept {
    const std::size_t floorCap =
        std::bit_ceil(std::max(kMinCapacity, minCapacity));
    const std::size_t cap = values_.size();
    return cap > floorCap && size() * 4 <= cap;
  }
  bool compact(std::size_t minCapacity = kMinCapacity) {
    const std::size_t floorCap =
        std::bit_ceil(std::max(kMinCapacity, minCapacity));
    const std::size_t cap = values_.size();
    const std::size_t count = size();
    if (cap <= floorCap || count * 4 > cap)
      return false;
    const std::size_t targetCap = std::max(
        floorCap, std::bit_ceil(std::max<std::size_t>(1, count * 2)));
    if (targetCap >= cap)
      return false;
    std::vector<T> shrunk(targetCap);
    for (std::size_t i = 0; i < count; ++i)
      shrunk[i] = std::move(values_[(head_ + i) & mask_]);
    values_.swap(shrunk);
    mask_ = values_.size() - 1;
    head_ = 0;
    tail_ = count;
    return true;
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
