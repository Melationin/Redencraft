#include "redstone/net/outbox.hpp"
#include "redstone/net/tick_timing.hpp"
#include <cassert>
#include <limits>
#include <thread>
using namespace redstone::net;
static std::vector<OutPacket> packet(int value) {
  std::vector<OutPacket> result;
  result.push_back(OutPacket{{static_cast<std::uint8_t>(value)}, value, {}});
  return result;
}
int main() {
  const TickTimingWindow::Clock::time_point epoch{};
  TickTimingWindow timing(epoch);
  using namespace std::chrono_literals;
  assert(!timing.record(epoch + 100ms, 10));
  assert(!timing.record(epoch + 600ms, 30));
  auto report = timing.record(epoch + 1s, 50);
  assert(report && report->ticks == 3 && report->milliseconds == 30);
  assert(!timing.record(epoch + 1100ms, 70));
  report = timing.record(epoch + 2s, 90);
  assert(report && report->ticks == 2 && report->milliseconds == 80);
  report = timing.record(epoch + 4500ms, 120);
  assert(report && report->ticks == 1 && report->milliseconds == 120);
  assert(!timing.record(epoch + 4501ms, 130)); // No catch-up packet burst.
  OutboundQueue<2> slow, healthy;
  auto a = packet(1), b = packet(2), c = packet(3);
  assert(slow.tryPush(a) && a.empty());
  assert(slow.tryPush(b));
  assert(!slow.tryPush(c) && c.size() == 1);
  assert(healthy.tryPush(
      c)); // The full connection cannot consume another's capacity.
  OutboundQueue<2>::Batch batch;
  assert(healthy.tryPop(batch) && batch.packets[0].packetId == 3);
  healthy.complete(batch);
  assert(healthy.queuedBytes() == 0);
  for (int value : {1, 2}) {
    assert(slow.tryPop(batch) && batch.packets[0].packetId == value);
    slow.complete(batch);
  }
  assert(slow.empty() && slow.queuedBytes() == 0);
  bool encoded = false;
  OutPacket deferred;
  deferred.deferred = [&] {
    encoded = true;
    return Bytes{9};
  };
  deferred.retainedBytes = maxQueuedOutputBytes / 2;
  std::vector<OutPacket> first{deferred}, second{deferred};
  assert(slow.tryPush(first) && !encoded);
  assert(slow.tryPop(batch));
  assert(!slow.tryPush(
      second)); // Charge includes the batch already being encoded.
  slow.complete(batch);
  assert(slow.tryPush(second) && !encoded);
  assert(slow.tryPop(batch));
  assert(batch.packets[0].deferred() == Bytes{9} && encoded);
  slow.complete(batch);
  auto oversized = packet(0);
  oversized[0].retainedBytes = std::numeric_limits<std::size_t>::max();
  assert(!slow.tryPush(oversized));
  std::vector<OutPacket> many(maxQueuedOutputPackets + 1);
  assert(!slow.tryPush(many));
  OutboundQueue<16> concurrent;
  constexpr int count = 20000;
  std::thread consumer([&] {
    OutboundQueue<16>::Batch item;
    for (int expected = 0; expected < count;) {
      if (!concurrent.tryPop(item)) {
        std::this_thread::yield();
        continue;
      }
      assert(item.packets.size() == 1 &&
             item.packets[0].packetId == expected++);
      concurrent.complete(item);
    }
  });
  for (int value = 0; value < count; ++value) {
    auto item = packet(value);
    while (!concurrent.tryPush(item))
      std::this_thread::yield();
  }
  consumer.join();
  assert(concurrent.empty() && concurrent.queuedBytes() == 0);
}
