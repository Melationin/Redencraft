#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace redstone;
// Mirror of validation/vanilla26_1/fluid_validation.py.
static constexpr int X0 = -2, X1 = 6, Y = 80, Z = 0;
static std::string stateAt(const BlockState &s) {
  if (s.isAir()) return "air";
  if (s.is(Blocks::STONE)) return "stone";
  if (s.is(Blocks::OBSIDIAN)) return "obsidian";
  if (s.is(Blocks::COBBLESTONE)) return "cobblestone";
  if (s.is(Blocks::WATER)) return "water" + s.get("level", "0");
  if (s.is(Blocks::LAVA)) return "lava" + s.get("level", "0");
  return "other:" + std::string(s.shortName());
}

// M-01: water next to lava. The shell is a sealed 3x1 channel at y=80:
//   z=-1 water, z=0 lava, z=1 air; stone floor/ceiling and side walls.
// T0 is recorded after the water source exists; the lava source is written at
// T0, so the first observed tick is the conversion tick.
static int mixingCase(int ticks, bool flowing) {
  const char *caseName = flowing ? "m01-flowing" : "m01-source";
  World w;
  for (int cx = -1; cx <= 0; ++cx)
    for (int cz = -1; cz <= 0; ++cz) w.loadChunk(cx, cz);
  const Pos water{0, 80, -1}, lava{0, 80, 0}, air{0, 80, 1};
  for (int z = -2; z <= 2; ++z) {
    w.rawSetBlock({0, 79, z}, Blocks::STONE.defaultState());
    w.rawSetBlock({0, 81, z}, Blocks::STONE.defaultState());
    w.rawSetBlock({-1, 80, z}, Blocks::STONE.defaultState());
    w.rawSetBlock({1, 80, z}, Blocks::STONE.defaultState());
  }
  for (int z = -1; z <= 1; ++z)
    w.rawSetBlock({0, 80, z}, Blocks::AIR.defaultState());
  w.rawSetBlock({0, 80, -2}, Blocks::STONE.defaultState());
  w.rawSetBlock({0, 80, 2}, Blocks::STONE.defaultState());
  w.setBlock(water, Blocks::WATER.defaultState().with("level", "0"));
  const Pos cells[] = {{0, 80, -1}, {0, 80, 0}, {0, 80, 1}, {-1, 80, 0},
                       {1, 80, 0},   {0, 79, 0}, {0, 81, 0}};
  auto dump = [&](const char *tag) {
    for (int i = 0; i < 7; ++i)
      std::printf("{'case': '%s', 't': '%s', 'cell': %d, 'state': '%s'}\n",
                  caseName, tag, i, stateAt(w.getBlock(cells[i])).c_str());
  };
  dump("T0");
  w.setBlock(lava, Blocks::LAVA.defaultState().with(
                       "level", flowing ? "2" : "0"));
  for (int t = 1; t <= ticks; ++t) {
    w.tick();
    char tag[16];
    std::snprintf(tag, sizeof(tag), "T%d", t);
    dump(tag);
  }
  return 0;
}
int main(int argc, char **argv) {
  if (argc > 1 && (std::strcmp(argv[1], "m01-source") == 0 ||
                   std::strcmp(argv[1], "m01-flowing") == 0))
    return mixingCase(argc > 2 ? std::atoi(argv[2]) : 10,
                      std::strcmp(argv[1], "m01-flowing") == 0);
  const int ticks = argc > 1 ? std::atoi(argv[1]) : 12;
  World w;
  for (int cx = -1; cx <= 0; ++cx)
    for (int cz = -1; cz <= 0; ++cz) w.loadChunk(cx, cz);
  // Layout: identical fill order to the python script.
  for (int x = X0; x <= X1; ++x) {
    w.rawSetBlock({x, Y - 1, Z}, Blocks::STONE.defaultState());
    w.rawSetBlock({x, Y + 1, Z}, Blocks::STONE.defaultState());
    w.rawSetBlock({x, Y, Z - 1}, Blocks::STONE.defaultState());
    w.rawSetBlock({x, Y, Z + 1}, Blocks::STONE.defaultState());
    w.rawSetBlock({x, Y, Z}, Blocks::AIR.defaultState());
  }
  w.rawSetBlock({X0, Y, Z}, Blocks::STONE.defaultState());
  w.rawSetBlock({X1, Y, Z}, Blocks::STONE.defaultState());
  auto dump = [&](const char *tag) {
    for (int i = 0; i <= X1 - X0; ++i)
      std::printf("{'t': '%s', 'cell': %d, 'state': '%s'}\n", tag, i,
                  stateAt(w.getBlock({X0 + i, Y, Z})).c_str());
  };
  dump("T0");
  w.setBlock({0, Y, Z}, Blocks::WATER.defaultState().with("level", "0"));
  std::vector<double> perTick;
  perTick.reserve(static_cast<std::size_t>(ticks));
  auto startAll = std::chrono::steady_clock::now();
  for (int t = 1; t <= ticks; ++t) {
    const auto start = std::chrono::steady_clock::now();
    w.tick();
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    perTick.push_back(ms);
    char tag[16];
    std::snprintf(tag, sizeof(tag), "T%d", t);
    dump(tag);
  }
  const double total = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - startAll)
                           .count();
  std::sort(perTick.begin(), perTick.end());
  std::printf("BENCH ticks=%d mean_ms=%.4f median_ms=%.4f p95_ms=%.4f "
              "max_ms=%.4f over50=%d wall_ms=%.3f pending_fluid=%zu "
              "fluid_dispatches=%llu fluid_writes=%llu\n",
              ticks, total / ticks, perTick[perTick.size() / 2],
              perTick[perTick.size() * 95 / 100], perTick.back(),
              static_cast<int>(std::count_if(perTick.begin(), perTick.end(),
                                             [](double v) { return v > 50; })),
              total, w.fluidTickCount(),
              (unsigned long long)w.fluidDispatchCount(),
              (unsigned long long)w.fluidWriteCount());
  return 0;
}