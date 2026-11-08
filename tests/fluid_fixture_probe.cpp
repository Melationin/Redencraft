#include "redstone/engine.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace redstone;

namespace {
struct Cell {
  int id;
  Pos pos;
};

std::string stateAt(const BlockState &state) {
  if (state.isAir())
    return "air";
  if (state.is(Blocks::STONE))
    return "stone";
  if (state.is(Blocks::SOUL_SOIL))
    return "soul_soil";
  if (state.is(Blocks::BLUE_ICE))
    return "blue_ice";
  if (state.is(Blocks::OBSIDIAN))
    return "obsidian";
  if (state.is(Blocks::BASALT))
    return "basalt[axis=" + state.get("axis", "y") + "]";
  if (state.is(Blocks::WATER))
    return "water[level=" + state.get("level", "0") + "]";
  if (state.is(Blocks::LAVA))
    return "lava[level=" + state.get("level", "0") + "]";
  if (state.is(Blocks::RAIL))
    return "rail[shape=" + state.get("shape", "north_south") +
           ",waterlogged=" + state.get("waterlogged", "false") + "]";
  throw std::runtime_error("unexpected fixture state: " +
                           std::string(state.name()) +
                           " id=" + std::to_string(state.id()));
}

BlockState water() {
  return Blocks::WATER.defaultState().with(PropertyKind::Level,
                                            std::uint8_t{0});
}

BlockState lava() {
  return Blocks::LAVA.defaultState().with(PropertyKind::Level,
                                           std::uint8_t{0});
}

void dump(World &world, std::string_view fixture, std::string_view tick,
          const std::vector<Cell> &cells) {
  for (const Cell &cell : cells) {
    const std::string value = stateAt(world.getBlock(cell.pos));
    std::printf(
        "{\"fixture\":\"%.*s\",\"t\":\"%.*s\",\"cell\":%d,"
        "\"state\":\"%s\"}\n",
        static_cast<int>(fixture.size()), fixture.data(),
        static_cast<int>(tick.size()), tick.data(), cell.id, value.c_str());
  }
}

void dumpAndTick(World &world, std::string_view fixture,
                 const std::vector<Cell> &cells, int ticks) {
  dump(world, fixture, "T0", cells);
  for (int tick = 1; tick <= ticks; ++tick) {
    world.tick();
    const std::string tag = "T" + std::to_string(tick);
    dump(world, fixture, tag, cells);
  }
}

void fillBox(World &world, Pos low, Pos high, const BlockState &state) {
  for (int x = low.x; x <= high.x; ++x)
    for (int y = low.y; y <= high.y; ++y)
      for (int z = low.z; z <= high.z; ++z)
        world.rawSetBlock({x, y, z}, state);
}

void w02() {
  World world;
  for (int cx = -1; cx <= 4; ++cx)
    for (int cz = -1; cz <= 1; ++cz)
      world.loadChunk(cx, cz);
  fillBox(world, {-1, 79, -1}, {2, 84, 1}, Blocks::STONE.defaultState());
  fillBox(world, {0, 80, 0}, {0, 83, 0}, Blocks::AIR.defaultState());
  world.rawSetBlock({1, 80, 0}, Blocks::AIR.defaultState());
  const std::vector<Cell> cells = {
      {0, {0, 83, 0}},  {1, {0, 82, 0}},  {2, {0, 81, 0}},
      {3, {0, 80, 0}},  {4, {1, 80, 0}},  {5, {0, 79, 0}},
      {6, {1, 79, 0}},  {7, {-1, 80, 0}}, {8, {0, 80, -1}},
      {9, {0, 80, 1}},  {10, {2, 80, 0}}, {11, {0, 84, 0}},
  };
  dump(world, "w02", "T0", cells);
  world.setBlock({0, 83, 0}, water());
  for (int tick = 1; tick <= 20; ++tick) {
    world.tick();
    const std::string tag = "T" + std::to_string(tick);
    dump(world, "w02", tag, cells);
  }
}

struct M03Case {
  const char *name;
  int x;
  const Block *below;
  const char *west;
  const char *east;
};

void m03() {
  World world;
  for (int cx = -1; cx <= 4; ++cx)
    for (int cz = -1; cz <= 1; ++cz)
      world.loadChunk(cx, cz);
  const M03Case cases[] = {
      {"basalt-blue", 10, &Blocks::SOUL_SOIL, "blue", nullptr},
      {"missing-soul", 20, &Blocks::STONE, "blue", nullptr},
      {"missing-blue", 30, &Blocks::SOUL_SOIL, nullptr, nullptr},
      {"blue-below", 40, &Blocks::BLUE_ICE, nullptr, nullptr},
      {"priority-blue", 50, &Blocks::SOUL_SOIL, "blue", "water"},
      {"priority-water", 60, &Blocks::SOUL_SOIL, "water", "blue"},
  };
  std::vector<Cell> cells;
  for (const M03Case &test : cases) {
    fillBox(world, {test.x - 1, 79, -2}, {test.x + 1, 81, 2},
            Blocks::STONE.defaultState());
    world.rawSetBlock({test.x, 80, 0}, Blocks::AIR.defaultState());
    world.rawSetBlock({test.x, 79, 0}, test.below->defaultState());
    const auto side = [&](int x, const char *kind) {
      if (kind == nullptr)
        return;
      world.rawSetBlock({x, 80, 0}, Blocks::AIR.defaultState());
      if (std::string_view(kind) == "blue")
        world.rawSetBlock({x, 80, 0}, Blocks::BLUE_ICE.defaultState());
      else
        world.rawSetBlock({x, 80, 0}, water());
    };
    side(test.x - 1, test.west);
    side(test.x + 1, test.east);
    const int base = static_cast<int>(cells.size());
    cells.push_back({base, {test.x, 80, 0}});
    cells.push_back({base + 1, {test.x, 79, 0}});
    cells.push_back({base + 2, {test.x - 1, 80, 0}});
    cells.push_back({base + 3, {test.x + 1, 80, 0}});
  }
  for (const M03Case &test : cases)
    world.setBlock({test.x, 80, 0}, lava());
  dumpAndTick(world, "m03", cells, 1);
}

void h01() {
  World world;
  for (int cx = -1; cx <= 4; ++cx)
    for (int cz = -1; cz <= 1; ++cz)
      world.loadChunk(cx, cz);
  fillBox(world, {-2, 119, -1}, {2, 121, 1}, Blocks::STONE.defaultState());
  fillBox(world, {-1, 120, 0}, {1, 120, 0}, Blocks::AIR.defaultState());
  fillBox(world, {9, 119, -1}, {12, 121, 1}, Blocks::STONE.defaultState());
  fillBox(world, {9, 120, 0}, {10, 120, 0}, Blocks::AIR.defaultState());
  world.setBlock({0, 120, 0}, Blocks::RAIL.defaultState());
  world.setBlock({10, 120, 0}, Blocks::RAIL.defaultState());
  const std::vector<Cell> cells = {
      {0, {0, 120, 0}},  {1, {-1, 120, 0}}, {2, {1, 120, 0}},
      {3, {0, 119, 0}},  {4, {0, 121, 0}},  {5, {10, 120, 0}},
      {6, {9, 120, 0}},  {7, {10, 119, 0}}, {8, {10, 121, 0}},
  };
  world.setBlock({-1, 120, 0}, water());
  world.setBlock({1, 120, 0}, water());
  world.setBlock({9, 120, 0}, water());
  dumpAndTick(world, "h01", cells, 10);
}
} // namespace

int main() {
  try {
    w02();
    m03();
    h01();
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "fluid_fixture_probe: %s\n", error.what());
    return 1;
  }
}
