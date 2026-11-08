#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace redstone;
using Clock = std::chrono::steady_clock;

namespace {

BlockState waterLevel(int level) {
  return Blocks::WATER.defaultState().with(
      PropertyKind::Level, static_cast<std::uint8_t>(level));
}

std::uint64_t stateFingerprint(const World &world) {
  std::uint64_t hash = 14695981039346656037ULL;
  const auto add = [&hash](std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      hash = (hash ^ (value & 0xFF)) * 1099511628211ULL;
      value >>= 8;
    }
  };
  for (int x = -16; x <= 15; ++x)
    for (int z = -16; z <= 15; ++z) {
      add(world.getBlock({x, 64, z}).id());
      add(world.getBlock({x, 63, z}).id());
    }
  return hash;
}

// A source is explicitly dispatched every tick on an open, stone-bottomed
// platform. Its horizontal spread therefore repeats the full water slope search
// (radius four, plus the radius-five hole probe) after the four initial writes.
int run(int ticks) {
  World world;
  for (int cx = -1; cx <= 1; ++cx)
    for (int cz = -1; cz <= 1; ++cz)
      world.loadChunk(cx, cz);
  for (int x = -16; x <= 15; ++x)
    for (int z = -16; z <= 15; ++z)
      world.rawSetBlock({x, 63, z}, Blocks::SMOOTH_STONE.defaultState());

  constexpr Pos source{0, 64, 0};
  world.rawSetBlock(source, waterLevel(0));
  std::vector<double> perTick;
  perTick.reserve(static_cast<std::size_t>(ticks));
  double total = 0.0;
  for (int tick = 0; tick < ticks; ++tick) {
    if (!world.scheduleFluidTick(source, Fluids::water(), 0) &&
        !world.hasFluidTick(source, Fluids::water()))
      throw std::runtime_error("source fluid tick did not schedule at tick " +
                               std::to_string(tick));
    const auto start = Clock::now();
    world.tick();
    const double ms = std::chrono::duration<double, std::milli>(
                          Clock::now() - start)
                          .count();
    total += ms;
    perTick.push_back(ms);
  }
  std::sort(perTick.begin(), perTick.end());
  std::printf(
      "FLUID_BENCH ticks=%d mean_ms=%.4f median_ms=%.4f p95_ms=%.4f "
      "max_ms=%.4f over50=%d dispatches=%llu writes=%llu pending=%zu "
      "fingerprint=%016llx\n",
      ticks, total / ticks, perTick[perTick.size() / 2],
      perTick[perTick.size() * 95 / 100], perTick.back(),
      static_cast<int>(std::count_if(perTick.begin(), perTick.end(),
                                     [](double value) { return value > 50.0; })),
      static_cast<unsigned long long>(world.fluidDispatchCount()),
      static_cast<unsigned long long>(world.fluidWriteCount()),
      world.fluidTickCount(),
      static_cast<unsigned long long>(stateFingerprint(world)));
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    int ticks = 1000;
    if (argc > 2)
      throw std::runtime_error(
          "usage: redstone_fluid_perf_benchmark [positive ticks]");
    if (argc == 2) {
      const std::string value(argv[1]);
      std::size_t end = 0;
      ticks = std::stoi(value, &end);
      if (end != value.size() || ticks <= 0)
        throw std::runtime_error("ticks must be a positive integer");
    }
    return run(ticks);
  } catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 2;
  }
}
