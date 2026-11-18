#include "redstone/engine.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace redstone;
using Clock = std::chrono::steady_clock;

namespace {

LightChunkInput emptyChunk() {
  auto chunk = std::make_shared<BaseChunk>();
  chunk->initialize({0, 0}, 0, 16);
  LightChunkInput input;
  input.chunkX = 0;
  input.chunkZ = 0;
  input.minBlockSectionY = 0;
  input.blockSectionCount = 1;
  input.chunkHandle = chunk;
  return input;
}

void change(LightService &service, int x, const BlockState &,
            const BlockState &newState) {
  service.reportBlockCell({x, 8, 8, newState.id()});
}

} // namespace

int main(int argc, char **argv) {
  try {
    int cycles = argc > 1 ? std::stoi(argv[1]) : 500;
    if (cycles <= 0)
      throw std::runtime_error("cycles must be positive");
    LightService service(0, 1, LightWorkerOptions{1000, 4096, false});
    if (!service.submitChunk(emptyChunk()) || !service.waitIdleForTesting())
      throw std::runtime_error("initial light load failed");
    service.drainPublished();

    const auto air = airState();
    const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
    double elapsed = 0.0;
    for (int cycle = 0; cycle < cycles; ++cycle) {
      const int x = cycle & 15;
      const auto start = Clock::now();
      change(service, x, air, lamp);
      if (!service.waitIdleForTesting())
        throw std::runtime_error("worker failed during insertion");
      service.drainPublished();
      change(service, x, lamp, air);
      if (!service.waitIdleForTesting())
        throw std::runtime_error("worker failed during removal");
      service.drainPublished();
      elapsed += std::chrono::duration<double, std::milli>(
                     Clock::now() - start)
                     .count();
    }
    const auto stats = service.stats();
    std::printf(
        "BLOCKLIGHT_BENCH cycles=%d changes=%llu light_queue_entries=%llu "
        "batches=%llu mean_pair_ms=%.4f error=%s\n",
        cycles,
        static_cast<unsigned long long>(stats.processedBlockChanges),
        static_cast<unsigned long long>(stats.lightQueueEntries),
        static_cast<unsigned long long>(stats.processedBatches),
        elapsed / cycles, service.workerError().empty() ? "none" : "present");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 2;
  }
}
