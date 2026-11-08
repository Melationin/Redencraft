#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"
#include <cstdio>
#include <string>
using namespace redstone;
// Mirror of validation/vanilla26_1/fluid_validation.py.
static constexpr int X0 = -2, X1 = 6, Y = 80, Z = 0;
static std::string stateAt(const BlockState &s) {
  if (s.isAir()) return "air";
  if (s.is(Blocks::STONE)) return "stone";
  if (s.is(Blocks::WATER)) return "water" + s.get("level", "0");
  return "other:" + std::string(s.shortName());
}
int main(int argc, char **argv) {
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
  for (int t = 1; t <= ticks; ++t) {
    w.tick();
    char tag[16];
    std::snprintf(tag, sizeof(tag), "T%d", t);
    dump(tag);
  }
  return 0;
}