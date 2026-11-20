#pragma once
// 协议发送队列：单生产者单消费者（SPSC）无锁有界环形队列，附带字节与包数背压计量，
// 用于世界主线程向网络写线程异步移交待编码或已帧化的数据包。
#include "redstone/net/wire.hpp"
#include <atomic>
#include <memory>

namespace redstone::net {
// 单生产者（世界线程）与单消费者（网络写线程）间的无锁发送队列。
// 采用两阶段消费（tryPop 取出批次并在写线程完成编码后调用 complete）：
// 这样延迟编码工作（如区块打包）在其占用的快照内存被释放前会持续计入队列积压配额，
// 防止慢客户端导致主线程无限堆积内存。
template <std::size_t Capacity = 4096> class OutboundQueue {
  static_assert(Capacity && (Capacity & (Capacity - 1)) == 0);

public:
  struct Batch {
    std::vector<OutPacket> packets;
    std::size_t charge = 0;
  };
  bool tryPush(std::vector<OutPacket> &packets,
               bool enforceLimits = true) noexcept {
    if (packets.empty())
      return true;
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == Capacity)
      return false;
    std::size_t charge = 0;
    if (enforceLimits) {
      for (const auto &packet : packets) {
        const auto bytes = outPacketCharge(packet);
        if (bytes > maxQueuedOutputBytes || charge > maxQueuedOutputBytes - bytes)
          return false;
        charge += bytes;
      }
      if (packets.size() > maxQueuedOutputPackets ||
          bytes_.load(std::memory_order_acquire) >
              maxQueuedOutputBytes - charge ||
          packets_.load(std::memory_order_acquire) >
              maxQueuedOutputPackets - packets.size())
        return false;
    }
    bytes_.fetch_add(charge, std::memory_order_relaxed);
    packets_.fetch_add(packets.size(), std::memory_order_relaxed);
    auto &slot = slots_[tail & (Capacity - 1)];
    slot.charge = charge;
    slot.packets = std::move(packets);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }
  bool tryPop(Batch &batch) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire))
      return false;
    batch = std::move(slots_[head & (Capacity - 1)]);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }
  void complete(Batch &batch) noexcept {
    bytes_.fetch_sub(batch.charge, std::memory_order_release);
    packets_.fetch_sub(batch.packets.size(), std::memory_order_release);
    batch.packets.clear();
    batch.charge = 0;
  }
  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }
  std::size_t queuedBytes() const noexcept {
    return bytes_.load(std::memory_order_acquire);
  }

private:
  std::unique_ptr<Batch[]> slots_ = std::make_unique<Batch[]>(Capacity);
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
  std::atomic<std::size_t> bytes_{0}, packets_{0};
};
} // namespace redstone::net
