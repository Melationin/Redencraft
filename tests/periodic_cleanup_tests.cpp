#include "redstone/periodic_cleanup.hpp"

#include "redstone/engine.hpp"
#include "redstone/light.hpp"

#include "../src/light/spsc_ring.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace redstone;

namespace {

void testTaskQueueShrinkAndFifoOrder() {
  redstone::light_internal::TaskQueue<int> queue;
  assert(queue.capacity() == 1024);
  assert(!queue.needsCompact());
  assert(!queue.compact());

  // Push > 16384 items so capacity grows to 32768, after first creating wrap-around.
  for (int i = 0; i < 400; ++i)
    queue.push(i);
  for (int i = 0; i < 300; ++i)
    assert(queue.pop() == i);
  for (int i = 400; i < 20000; ++i)
    queue.push(i);
  assert(queue.capacity() >= 32768);
  assert(queue.size() == 19700);
  assert(!queue.needsCompact());
  assert(!queue.compact());

  // Drain down to <= 25% of capacity while retaining non-zero elements.
  int nextExpected = 300;
  while (queue.size() > 1500) {
    assert(queue.pop() == nextExpected++);
  }
  assert(queue.size() == 1500);
  assert(queue.needsCompact());
  const std::size_t beforeCap = queue.capacity();
  assert(queue.compact());
  const std::size_t afterCap = queue.capacity();
  assert(afterCap < beforeCap);
  assert(afterCap == 4096);
  assert(queue.size() == 1500);
  assert(!queue.needsCompact());

  // Verify exact FIFO preservation across compaction.
  while (!queue.empty()) {
    assert(queue.pop() == nextExpected++);
  }
  assert(nextExpected == 20000);

  // When empty and oversized (> 1024), compact shrinks all the way back to kMinCapacity.
  assert(queue.needsCompact());
  assert(queue.compact());
  assert(queue.capacity() == 1024);
  assert(!queue.needsCompact());
  assert(!queue.compact());
}

void testStaggeringAndSingleTaskPerTick() {
  PeriodicCleanupManager manager(PeriodicCleanupConfig{
      .defaultIntervalTicks = 100,
      .maxAllowedMsptMs = 40.0,
      .msptWindowTicks = 100,
  });

  std::vector<std::uint64_t> runTickA;
  std::vector<std::uint64_t> runTickB;
  std::vector<std::uint64_t> runTickC;
  std::uint64_t tickNow = 0;

  // Force identical initial delay = 10 to prove at most one task runs per tick even if due together.
  manager.registerTask(
      "a",
      [&](CleanupExecutionGate::Lease &) {
        runTickA.push_back(tickNow);
        return true;
      },
      100, 10);
  manager.registerTask(
      "b",
      [&](CleanupExecutionGate::Lease &) {
        runTickB.push_back(tickNow);
        return true;
      },
      100, 10);
  manager.registerTask(
      "c",
      [&](CleanupExecutionGate::Lease &) {
        runTickC.push_back(tickNow);
        return true;
      },
      100, 10);

  for (int t = 1; t <= 15; ++t) {
    tickNow = static_cast<std::uint64_t>(t);
    manager.advanceTick(5.0);
  }
  assert(runTickA.size() == 1 && runTickA[0] == 10);
  assert(runTickB.size() == 1 && runTickB[0] == 11);
  assert(runTickC.size() == 1 && runTickC[0] == 12);
  assert(manager.stats().executedCount == 3);
}

void testGatePreventsSimultaneousAsyncCleanup() {
  PeriodicCleanupManager manager(PeriodicCleanupConfig{
      .defaultIntervalTicks = 100,
      .maxAllowedMsptMs = 40.0,
      .msptWindowTicks = 100,
  });

  CleanupExecutionGate::Lease heldAsyncLease;
  int asyncStarted = 0;
  int syncRuns = 0;

  manager.registerTask(
      "async",
      [&](CleanupExecutionGate::Lease &lease) {
        ++asyncStarted;
        heldAsyncLease = std::move(lease);
        return true;
      },
      100, 5);
  manager.registerTask(
      "sync",
      [&](CleanupExecutionGate::Lease &) {
        ++syncRuns;
        return true;
      },
      100, 6);

  for (int t = 1; t <= 5; ++t)
    assert(manager.advanceTick(2.0) == (t == 5));
  assert(asyncStarted == 1);
  assert(heldAsyncLease);
  assert(manager.gate()->isBusy());

  // Cannot acquire another cleanup while async lease is held.
  assert(!manager.gate()->tryAcquire());
  assert(manager.gate()->concurrentRejectionCount() >= 1);

  // Tick 6, 7, 8 must defer sync task while async lease remains held.
  for (int t = 6; t <= 8; ++t)
    assert(!manager.advanceTick(2.0));
  assert(syncRuns == 0);
  assert(manager.stats().deferredBusyCount == 3);

  // Once async worker releases lease, next tick immediately runs sync cleanup.
  heldAsyncLease.release();
  assert(!manager.gate()->isBusy());
  assert(manager.advanceTick(2.0));
  assert(syncRuns == 1);
}

void testHighMsptDefersAndRecovers() {
  PeriodicCleanupManager manager(PeriodicCleanupConfig{
      .defaultIntervalTicks = 100,
      .maxAllowedMsptMs = 40.0,
      .msptWindowTicks = 20,
  });

  int runs = 0;
  manager.registerTask(
      "trim",
      [&](CleanupExecutionGate::Lease &) {
        ++runs;
        return true;
      },
      100, 10);

  // Single spike on tick 10 defers tick 10 despite low rolling average.
  for (int t = 1; t <= 9; ++t)
    assert(!manager.advanceTick(5.0));
  assert(!manager.advanceTick(45.0)); // tick 10 spikes -> deferred
  assert(runs == 0);
  assert(manager.stats().deferredHighMsptCount == 1);

  // Tick 11 normal MSPT recovers immediately (rolling avg ~ 9ms < 40ms).
  assert(manager.advanceTick(5.0));
  assert(runs == 1);

  // Sustained external high MSPT prevents next scheduled cycle (tick 111).
  manager.recordExternalTickTimeMs(55.0);
  for (int t = 12; t <= 115; ++t)
    (void)manager.advanceTick(5.0);
  assert(runs == 1);
  assert(manager.stats().deferredHighMsptCount >= 6);

  // Clear external spike with low samples until rolling average < 40 ms.
  for (int i = 0; i < 30; ++i)
    manager.recordExternalTickTimeMs(5.0);
  assert(manager.advanceTick(5.0));
  assert(runs == 2);
}

void testWorldAndLightServiceIntegration() {
  World world(-64, 320, 1000000);
  // Build an elevated perforated roof over 9x9 chunks to ensure sky light queues
  // exceed kMinCapacity (1024) during initial chunk lighting.
  for (int cx = -4; cx <= 4; ++cx) {
    for (int cz = -4; cz <= 4; ++cz) {
      world.loadChunk(cx, cz);
    }
  }
  const BlockState stone = Blocks::STONE.defaultState();
  for (int x = -60; x <= 60; ++x) {
    for (int z = -60; z <= 60; ++z) {
      if ((x + z) % 2 == 0)
        world.rawSetBlock({x, 200, z}, stone);
    }
  }
  world.primeHeightmaps();
  world.enableLighting();
  LightService *ls = world.lightService();
  assert(ls != nullptr);
  assert(ls->flushAndWait(std::chrono::seconds(30)));
  world.drainLightPublications();

  const std::size_t bytesAfterWarmup = ls->scratchTaskQueueBytesForTesting();
  assert(ls->needsTaskQueueCompact());
  assert(bytesAfterWarmup > 0);

  // Run 110 ticks under low MSPT: scheduled tick & light TaskQueue cleanups must trigger cleanly
  // without running simultaneously, and oversized queues shrink back to minimal capacity.
  for (int t = 0; t < 110; ++t)
    world.tick();

  assert(ls->flushAndWait(std::chrono::seconds(10)));
  const std::size_t bytesAfterCleanup = ls->scratchTaskQueueBytesForTesting();
  assert(!ls->needsTaskQueueCompact());
  assert(bytesAfterCleanup < bytesAfterWarmup);
  const auto st = world.cleanupManager()->stats();
  assert(st.totalTicks == 110);
  assert(st.executedCount >= 3);

  // Verify move-construction & destruction of World with cleanup manager & lighting work cleanly.
  World movedWorld(std::move(world));
  movedWorld.tick();
  assert(movedWorld.cleanupManager()->stats().totalTicks == 111);
}

void testWorldHighMsptSkipsScheduledTickCleanup() {
  World world(-64, 320, 1000000);
  world.loadChunk(0, 0);
  world.cleanupManager()->setMaxAllowedMsptMs(1.0);
  world.cleanupManager()->recordExternalTickTimeMs(50.0);

  for (int i = 0; i < 300; ++i) {
    world.scheduleTick(ScheduledTick{{i % 16, 0, (i / 16) % 16},
                                     Blocks::SMOOTH_STONE.name(), 2, 0, i});
  }
  for (int t = 0; t < 110; ++t)
    world.tick();

  const auto st = world.cleanupManager()->stats();
  assert(st.executedCount == 0);
  assert(st.deferredHighMsptCount > 0);
}

} // namespace

int main() {
  testTaskQueueShrinkAndFifoOrder();
  testStaggeringAndSingleTaskPerTick();
  testGatePreventsSimultaneousAsyncCleanup();
  testHighMsptDefersAndRecovers();
  testWorldAndLightServiceIntegration();
  testWorldHighMsptSkipsScheduledTickCleanup();
  std::cout << "periodic_cleanup tests passed\n";
  return 0;
}
