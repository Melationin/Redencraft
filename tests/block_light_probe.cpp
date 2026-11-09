#include "redstone/engine.hpp"
#include "redstone/light.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace redstone;

namespace {

const std::vector<Pos> kCells = [] {
  std::vector<Pos> cells;
  for (int x = -15; x <= 15; ++x)
    cells.push_back({x, 200, 0});
  for (int y = 198; y <= 202; ++y)
    cells.push_back({0, y, 0});
  for (int z = -1; z <= 1; ++z)
    cells.push_back({5, 200, z});
  cells.push_back({4, -64, 4});
  cells.push_back({4, -65, 4});
  cells.push_back({4, -66, 4});
  cells.push_back({4, 319, 4});
  cells.push_back({4, 320, 4});
  cells.push_back({4, 321, 4});
  return cells;
}();

void place(World &world, Pos position, const BlockState &state) {
  if (world.getBlock(position) == state)
    return;
  if (!world.setBlock(position, state, 3))
    throw std::runtime_error(
        "fixture block write failed at " + std::to_string(position.x) + "," +
        std::to_string(position.y) + "," + std::to_string(position.z) +
        " for " + state.name());
}

void rawPlace(World &world, Pos position, const BlockState &state) {
  world.rawSetBlock(position, state);
  if (world.getBlock(position) != state)
    throw std::runtime_error(
        "raw fixture block write failed at " + std::to_string(position.x) +
        "," + std::to_string(position.y) + "," + std::to_string(position.z) +
        " for " + state.name() + " got " +
        world.getBlock(position).name());
}

std::vector<int> sample(World &world) {
  auto *service = world.lightService();
  if (!service || !service->waitIdleForTesting())
    throw std::runtime_error("light worker did not become idle");
  service->drainPublished();
  std::vector<int> values;
  values.reserve(kCells.size());
  for (const Pos &cell : kCells) {
    auto snapshot = service->latestChunk(cell.x >> 4, cell.z >> 4);
    if (!snapshot)
      throw std::runtime_error("light chunk was not published");
    const auto *section = snapshot->section(cell.y >> 4);
    values.push_back(section ? section->light(LightLayer::Block, cell.x & 15,
                                              cell.y & 15, cell.z & 15)
                             : -1);
  }
  return values;
}

void printStage(int stage, const std::vector<int> &values) {
  std::ostringstream output;
  output << "BLOCKLIGHT_JSON {\"stage\":" << stage << ",\"block_light\":[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) output << ',';
    output << values[i];
  }
  output << "]}\n";
  std::cout << output.str();
}

} // namespace

int main() {
  try {
    World world;
    world.loadChunk(-1, -1);
    world.loadChunk(-1, 0);
    world.loadChunk(0, -1);
    world.loadChunk(0, 0);

    const auto air = airState();
    for (int x = -15; x <= 15; ++x)
      for (int y = 198; y <= 202; ++y)
        for (int z = -1; z <= 1; ++z)
          rawPlace(world, {x, y, z}, air);
    for (int y = 199; y <= 201; ++y)
      for (int z = -1; z <= 1; ++z)
        rawPlace(world, {5, y, z}, Blocks::STONE.defaultState());
    rawPlace(world, {8, 200, 0}, Blocks::GLASS.defaultState());
    rawPlace(world, {-5, 200, 0},
             Blocks::PISTON.defaultState().with("facing", "north").with(
                 "extended", "true"));
    rawPlace(world, {0, 200, 0},
             Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));

    world.enableLighting(LightWorkerOptions{1000, 4096, false});
    printStage(1, sample(world));

    place(world, {5, 200, 0}, air);
    place(world, {-5, 200, 0}, Blocks::PISTON.defaultState());
    place(world, {-10, 200, 0},
          Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
    printStage(2, sample(world));

    place(world, {0, 200, 0}, air);
    place(world, {-10, 200, 0}, air);
    place(world, {8, 200, 0}, air);
    printStage(3, sample(world));

    const auto lamp =
        Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
    place(world, {4, -64, 4}, lamp);
    place(world, {4, 319, 4}, lamp);
    printStage(4, sample(world));

    place(world, {4, -64, 4}, air);
    place(world, {4, 319, 4}, air);
    printStage(5, sample(world));
    world.disableLighting();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "block light probe failure: " << error.what() << '\n';
    return 1;
  }
}
