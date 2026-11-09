#include "redstone/engine.hpp"
#include "redstone/light.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace redstone;

namespace {

const std::vector<Pos> kCells = {
    {8, 200, 8},   {0, 200, 8},   {15, 200, 8}, {16, 200, 8},
    {8, 200, 0},   {8, 200, 15},  {8, 200, 16}, {8, 202, 8},
    {8, 203, 8},   {8, 320, 8},   {0, -62, 0},  {0, -61, 0},
    {0, -65, 0},   {0, -60, 0},   {0, 100, 0},
};

void rawPlace(World &world, Pos position, const BlockState &state) {
  world.rawSetBlock(position, state);
}

void place(World &world, Pos position, const BlockState &state) {
  if (world.getBlock(position) == state)
    return;
  if (!world.setBlock(position, state, 3))
    throw std::runtime_error("sky fixture block write failed");
}

void fillRaw(World &world, Pos first, Pos last, const BlockState &state) {
  for (int x = first.x; x <= last.x; ++x)
    for (int y = first.y; y <= last.y; ++y)
      for (int z = first.z; z <= last.z; ++z)
        rawPlace(world, {x, y, z}, state);
}

void fill(World &world, Pos first, Pos last, const BlockState &state) {
  for (int x = first.x; x <= last.x; ++x)
    for (int y = first.y; y <= last.y; ++y)
      for (int z = first.z; z <= last.z; ++z)
        place(world, {x, y, z}, state);
}

std::vector<int> sample(World &world) {
  auto *service = world.lightService();
  if (!service || !service->waitIdleForTesting())
    throw std::runtime_error("sky-light worker did not become idle");
  service->drainPublished();
  std::vector<int> values;
  values.reserve(kCells.size());
  for (const Pos &cell : kCells) {
    auto snapshot = service->latestChunk(cell.x >> 4, cell.z >> 4);
    if (!snapshot)
      throw std::runtime_error("sky-light chunk was not published");
    const auto *section = snapshot->section(cell.y >> 4);
    values.push_back(section ? section->light(LightLayer::Sky, cell.x & 15,
                                               cell.y & 15, cell.z & 15)
                             : -1);
  }
  return values;
}

void printStage(int stage, const std::vector<int> &values) {
  std::ostringstream output;
  output << "SKYLIGHT_JSON {\"stage\":" << stage << ",\"sky_light\":[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i)
      output << ',';
    output << values[i];
  }
  output << "]}\n";
  std::cout << output.str();
}

} // namespace

int main() {
  try {
    World world;
    for (int x = -1; x <= 1; ++x)
      for (int z = -1; z <= 1; ++z)
        world.loadChunk(x, z);

    const auto air = airState();
    const auto stone = Blocks::STONE.defaultState();
    fillRaw(world, {-16, 198, -2}, {16, 203, 16}, air);
    fillRaw(world, {-2, -62, -2}, {2, 100, 2}, air);
    world.enableLighting();
    printStage(1, sample(world));

    fill(world, {0, 202, 0}, {15, 202, 15}, stone);
    printStage(2, sample(world));
    place(world, {8, 202, 8}, air);
    printStage(3, sample(world));
    place(world, {8, 202, 8}, stone);
    printStage(4, sample(world));

    fill(world, {-1, -60, -1}, {1, -60, 1}, stone);
    printStage(5, sample(world));
    fill(world, {-1, -60, -1}, {1, -60, 1}, air);
    printStage(6, sample(world));
    world.disableLighting();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "sky light probe failure: " << error.what() << '\n';
    return 1;
  }
}
