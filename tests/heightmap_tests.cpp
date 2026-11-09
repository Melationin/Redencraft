#include "redstone/engine.hpp"
#include "redstone/heightmap.hpp"
#include "redstone/storage.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using namespace redstone;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

void typesAndEncoding() {
  require(heightmapSerializationKey(HeightmapType::WorldSurfaceWG) ==
              "WORLD_SURFACE_WG",
          "WORLD_SURFACE_WG key");
  require(heightmapSerializationKey(HeightmapType::WorldSurface) ==
              "WORLD_SURFACE",
          "WORLD_SURFACE key");
  require(heightmapSerializationKey(HeightmapType::OceanFloorWG) ==
              "OCEAN_FLOOR_WG",
          "OCEAN_FLOOR_WG key");
  require(heightmapSerializationKey(HeightmapType::OceanFloor) == "OCEAN_FLOOR",
          "OCEAN_FLOOR key");
  require(heightmapSerializationKey(HeightmapType::MotionBlocking) ==
              "MOTION_BLOCKING",
          "MOTION_BLOCKING key");
  require(heightmapSerializationKey(HeightmapType::MotionBlockingNoLeaves) ==
              "MOTION_BLOCKING_NO_LEAVES",
          "MOTION_BLOCKING_NO_LEAVES key");
  require(heightmapUsage(HeightmapType::WorldSurfaceWG) ==
              HeightmapUsage::Worldgen &&
              heightmapUsage(HeightmapType::OceanFloor) ==
                  HeightmapUsage::LiveWorld &&
              heightmapUsage(HeightmapType::WorldSurface) ==
                  HeightmapUsage::Client,
          "heightmap usages follow 26.1");
  require(!heightmapSendToClient(HeightmapType::WorldSurfaceWG) &&
              heightmapSendToClient(HeightmapType::WorldSurface) &&
              !heightmapKeepAfterWorldgen(HeightmapType::OceanFloorWG) &&
              heightmapKeepAfterWorldgen(HeightmapType::MotionBlocking),
          "heightmap client/worldgen flags agree with usage");
  require(!heightmapIsLive(HeightmapType::WorldSurfaceWG) &&
              heightmapIsLive(HeightmapType::OceanFloor) &&
              heightmapIsLive(HeightmapType::WorldSurface) &&
              heightmapIsLive(HeightmapType::MotionBlocking) &&
              heightmapIsLive(HeightmapType::MotionBlockingNoLeaves),
          "final chunks carry the four live maps");

  Heightmap map(HeightmapType::WorldSurface, -64, 384);
  require(map.bits() == 9, "height 384 uses nine bits");
  require(map.rawData().size() == 37,
          "SimpleBitStorage stores seven 9-bit values per long");
  require(map.firstAvailable(0, 0) == -64 && map.highestTaken(0, 0) == -65,
          "an empty heightmap has no taken block");
}

void predicates() {
  const auto air = Blocks::AIR.defaultState();
  const auto stone = Blocks::STONE.defaultState();
  const auto water = Blocks::WATER.defaultState();
  const auto carpet = Blocks::WHITE_CARPET.defaultState();
  Heightmap surface(HeightmapType::WorldSurface, -64, 384);
  Heightmap floor(HeightmapType::OceanFloor, -64, 384);
  Heightmap motion(HeightmapType::MotionBlocking, -64, 384);
  Heightmap noLeaves(HeightmapType::MotionBlockingNoLeaves, -64, 384);
  require(!surface.matches(air) && surface.matches(stone) &&
              surface.matches(water),
          "WORLD_SURFACE is NOT_AIR");
  require(!floor.matches(air) && floor.matches(stone) &&
              !floor.matches(water) && !floor.matches(carpet),
          "OCEAN_FLOOR is material motion blocking");
  require(!motion.matches(air) && motion.matches(water) &&
              motion.matches(stone),
          "MOTION_BLOCKING includes fluids");
  require(!noLeaves.matches(air) && noLeaves.matches(water),
          "MOTION_BLOCKING_NO_LEAVES still includes fluids");
}

void updatesAndPrime() {
  std::map<Pos, BlockState> blocks;
  const auto get = [&](int x, int y, int z) {
    const auto found = blocks.find({x, y, z});
    return found == blocks.end() ? airState() : found->second;
  };

  blocks[{0, -5, 0}] = Blocks::STONE.defaultState();
  blocks[{0, -4, 0}] = Blocks::WATER.defaultState();
  ChunkHeightmaps chunk(-64, 384);
  chunk.prime(320, get);
  require(chunk.get(HeightmapType::WorldSurface).height(0, 0) == -4,
          "prime stores the highest non-air block");
  require(chunk.get(HeightmapType::OceanFloor).height(0, 0) == -5,
          "prime ignores fluid for OCEAN_FLOOR");
  require(chunk.get(HeightmapType::MotionBlocking).height(0, 0) == -4,
          "prime includes fluid for MOTION_BLOCKING");
  require(chunk.get(HeightmapType::MotionBlockingNoLeaves).height(0, 0) == -4,
          "prime includes fluid for NO_LEAVES");
  Heightmap copied(HeightmapType::WorldSurface, -64, 384);
  require(copied.setRawData(chunk.get(HeightmapType::WorldSurface).rawData(),
                            320, get) &&
              copied.height(0, 0) == -4,
          "matching raw heightmap data restores the packed columns");
  Heightmap repaired(HeightmapType::OceanFloor, -64, 384);
  require(!repaired.setRawData(std::vector<std::uint64_t>{0}, 320, get) &&
              repaired.height(0, 0) == -5,
          "invalid raw data is discarded and the heightmap is primed");

  blocks[{0, 20, 0}] = Blocks::GLASS.defaultState();
  chunk.update(0, 20, 0, blocks[{0, 20, 0}], get);
  require(chunk.get(HeightmapType::WorldSurface).height(0, 0) == 20 &&
              chunk.get(HeightmapType::OceanFloor).height(0, 0) == 20,
          "placing above raises the relevant maps");
  blocks.erase({0, 20, 0});
  chunk.update(0, 20, 0, airState(), get);
  require(chunk.get(HeightmapType::WorldSurface).height(0, 0) == -4 &&
              chunk.get(HeightmapType::OceanFloor).height(0, 0) == -5,
          "removing the top scans down independently per predicate");
  blocks.erase({0, -4, 0});
  chunk.update(0, -4, 0, airState(), get);
  require(chunk.get(HeightmapType::MotionBlocking).height(0, 0) == -5,
          "removing the top fluid falls back to the solid below");
  blocks.erase({0, -5, 0});
  chunk.update(0, -5, 0, airState(), get);
  require(chunk.get(HeightmapType::WorldSurface).height(0, 0) == -65,
          "removing the final block returns the pre-minY sentinel");

  std::map<Pos, BlockState> layeredBlocks;
  const auto layeredGet = [&](int x, int y, int z) {
    const auto found = layeredBlocks.find({x, y, z});
    return found == layeredBlocks.end() ? airState() : found->second;
  };
  layeredBlocks[{2, 50, 2}] = Blocks::STONE.defaultState();
  layeredBlocks[{2, 64, 2}] = Blocks::WATER.defaultState();
  ChunkHeightmaps layered(-64, 384);
  layered.prime(100, layeredGet);
  require(layered.get(HeightmapType::OceanFloor).height(2, 2) == 50 &&
              layered.get(HeightmapType::WorldSurface).height(2, 2) == 64,
          "fixture has a low ocean floor and higher water surface");
  layeredBlocks[{2, 60, 2}] = Blocks::STONE.defaultState();
  require(layered.update(2, 60, 2, Blocks::STONE.defaultState(), layeredGet),
          "shared shortcut must not hide a lower map's update");
  require(layered.get(HeightmapType::OceanFloor).height(2, 2) == 60 &&
              layered.get(HeightmapType::WorldSurface).height(2, 2) == 64 &&
              layered.get(HeightmapType::MotionBlocking).height(2, 2) == 64,
          "y=60 raises only the lower OCEAN_FLOOR table");
  layeredBlocks.erase({2, 60, 2});
  require(layered.update(2, 60, 2, airState(), layeredGet),
          "removing the sub-surface stone rescans the affected table");
  require(layered.get(HeightmapType::OceanFloor).height(2, 2) == 50 &&
              layered.get(HeightmapType::WorldSurface).height(2, 2) == 64,
          "reverse rescan returns OCEAN_FLOOR to the seabed only");

  std::map<Pos, BlockState> reprimeBlocks;
  const auto reprimeGet = [&](int x, int y, int z) {
    const auto found = reprimeBlocks.find({x, y, z});
    return found == reprimeBlocks.end() ? airState() : found->second;
  };
  reprimeBlocks[{4, 10, 4}] = Blocks::STONE.defaultState();
  ChunkHeightmaps repeated(-64, 384);
  repeated.prime(100, reprimeGet);
  require(repeated.get(HeightmapType::WorldSurface).height(4, 4) == 10,
          "first prime records the test column");
  reprimeBlocks.erase({4, 10, 4});
  repeated.prime(100, reprimeGet);
  require(repeated.get(HeightmapType::WorldSurface).height(4, 4) == -65,
          "re-prime clears a formerly occupied column to minY-1");

  Heightmap lower(HeightmapType::WorldSurface, -64, 384);
  blocks[{3, -10, 4}] = Blocks::STONE.defaultState();
  require(lower.update(3, -10, 4, Blocks::STONE.defaultState(), get),
          "first placement updates a column");
  blocks[{3, -9, 4}] = Blocks::STONE.defaultState();
  lower.update(3, -9, 4, Blocks::STONE.defaultState(), get);
  blocks[{3, -8, 4}] = Blocks::STONE.defaultState();
  lower.update(3, -8, 4, Blocks::STONE.defaultState(), get);
  const int before = lower.height(3, 4);
  blocks[{3, -20, 4}] = Blocks::STONE.defaultState();
  require(!lower.update(3, -20, 4, Blocks::STONE.defaultState(), get) &&
              lower.height(3, 4) == before,
          "a write at least two blocks below firstAvailable is ignored");
}

void sectionedDownwardRescan() {
  World world;
  world.loadChunk(-1, -1);
  world.heightmapScanStats.enabled = true;

  require(world.setBlock({-1, -5, -1}, Blocks::STONE.defaultState()) &&
              world.setBlock({-1, -4, -1}, Blocks::WATER.defaultState()),
          "same-section rescan fixture builds its source column");
  require(world.setBlock({-1, -4, -1}, airState()),
          "remove the same-section top source");
  require(world.height(HeightmapType::WorldSurface, -1, -1) == -5 &&
              world.height(HeightmapType::OceanFloor, -1, -1) == -5 &&
              world.height(HeightmapType::MotionBlocking, -1, -1) == -5 &&
              world.height(HeightmapType::MotionBlockingNoLeaves, -1, -1) == -5,
          "same-section rescan finds the stone one block below");

  require(world.setBlock({-2, -17, -2}, Blocks::STONE.defaultState()) &&
              world.setBlock({-2, -16, -2}, Blocks::STONE.defaultState()),
          "adjacent-section fixture builds a two-block column");
  world.heightmapScanStats = {};
  world.heightmapScanStats.enabled = true;
  require(world.setBlock({-2, -16, -2}, airState()),
          "remove the upper block at a section boundary");
  require(world.height(HeightmapType::WorldSurface, -2, -2) == -17 &&
              world.height(HeightmapType::OceanFloor, -2, -2) == -17,
          "downward scan crosses the adjacent section boundary");
  require(world.heightmapScanStats.scans == 1 &&
              world.heightmapScanStats.visitedSections == 1 &&
              world.heightmapScanStats.skippedSections == 0 &&
              world.heightmapScanStats.readCells == 1,
          "adjacent-section scan reads only the target block once");

  World longGap;
  longGap.loadChunk(0, 0);
  longGap.heightmapScanStats.enabled = true;
  require(longGap.setBlock({1, -63, 1}, Blocks::STONE.defaultState()) &&
              longGap.setBlock({0, 300, 0}, Blocks::STONE.defaultState()) &&
              longGap.setBlock({1, 300, 1}, Blocks::WATER.defaultState()),
          "long-gap fixture creates a nonempty upper section and bottom hit");
  longGap.heightmapScanStats = {};
  longGap.heightmapScanStats.enabled = true;
  require(longGap.setBlock({1, 300, 1}, airState()),
          "remove water above a long empty gap");
  require(longGap.height(HeightmapType::WorldSurface, 1, 1) == -63 &&
              longGap.height(HeightmapType::OceanFloor, 1, 1) == -63 &&
              longGap.height(HeightmapType::MotionBlocking, 1, 1) == -63 &&
              longGap.height(HeightmapType::MotionBlockingNoLeaves, 1, 1) == -63,
          "one scan resolves all four maps through empty sections");
  const std::string longStats =
      " scans=" + std::to_string(longGap.heightmapScanStats.scans) +
      " visited=" + std::to_string(longGap.heightmapScanStats.visitedSections) +
      " skipped=" + std::to_string(longGap.heightmapScanStats.skippedSections) +
      " reads=" + std::to_string(longGap.heightmapScanStats.readCells);
  std::cout << "HEIGHTMAP_SCAN" << longStats << '\n';
  require(longGap.heightmapScanStats.scans == 1 &&
              longGap.heightmapScanStats.visitedSections == 23 &&
              longGap.heightmapScanStats.skippedSections == 21 &&
              longGap.heightmapScanStats.readCells == 27,
          "long-gap scan visits each section once, skips 21, and reads 27 cells:" +
              longStats);

  longGap.heightmapScanStats = {};
  longGap.heightmapScanStats.enabled = true;
  require(longGap.setBlock({1, -63, 1}, airState()),
          "remove the final bottom match");
  require(longGap.height(HeightmapType::WorldSurface, 1, 1) == -65 &&
              longGap.height(HeightmapType::OceanFloor, 1, 1) == -65 &&
              longGap.height(HeightmapType::MotionBlocking, 1, 1) == -65 &&
              longGap.height(HeightmapType::MotionBlockingNoLeaves, 1, 1) == -65,
          "no bottom hit returns every map to the minY sentinel");
  require(longGap.heightmapScanStats.scans == 1 &&
              longGap.heightmapScanStats.visitedSections == 1 &&
              longGap.heightmapScanStats.skippedSections == 1 &&
              longGap.heightmapScanStats.readCells == 0,
          "empty bottom section is skipped without a cell read");

  World ocean;
  ocean.loadChunk(0, 0);
  require(ocean.setBlock({0, 50, 0}, Blocks::STONE.defaultState()) &&
              ocean.setBlock({0, 64, 0}, Blocks::WATER.defaultState()),
          "ocean fixture places seabed and water surface");
  ocean.heightmapScanStats = {};
  ocean.heightmapScanStats.enabled = true;
  require(ocean.setBlock({0, 60, 0}, Blocks::STONE.defaultState()),
          "place submerged stone below the water surface");
  require(ocean.height(HeightmapType::OceanFloor, 0, 0) == 60 &&
              ocean.height(HeightmapType::WorldSurface, 0, 0) == 64 &&
              ocean.height(HeightmapType::MotionBlocking, 0, 0) == 64 &&
              ocean.height(HeightmapType::MotionBlockingNoLeaves, 0, 0) == 64 &&
              ocean.heightmapScanStats.scans == 0,
          "submerged placement raises only OCEAN_FLOOR without a rescan");
  require(ocean.setBlock({0, 60, 0}, airState()),
          "remove submerged stone");
  require(ocean.height(HeightmapType::OceanFloor, 0, 0) == 50 &&
              ocean.height(HeightmapType::WorldSurface, 0, 0) == 64,
          "submerged removal rescans OCEAN_FLOOR back to the seabed only");

  World unaligned(-63, 312);
  unaligned.loadChunk(-1, -1);
  bool u1 = unaligned.setBlock({-1, -48, -1}, Blocks::STONE.defaultState());
  bool u2 = unaligned.setBlock({-1, -47, -1}, Blocks::STONE.defaultState());
  require(u1 && u2,
          "unaligned fixture builds a column across its section layout");
  bool u3 = unaligned.setBlock({-1, -47, -1}, airState());
  require(u3 && unaligned.height(HeightmapType::WorldSurface, -1, -1) == -48,
          "unaligned minY keeps the existing per-cell result");
}

void worldIntegration() {
  World world;
  world.loadChunk(0, 0);
  world.primeHeightmaps();
  require(world.height(HeightmapType::WorldSurface, 0, 0) == -65,
          "new empty chunk has an empty WORLD_SURFACE");
  require(world.setBlock({0, -5, 0}, Blocks::STONE.defaultState()),
          "place stone for live heightmap");
  require(world.height(HeightmapType::WorldSurface, 0, 0) == -5,
          "World.setBlock updates WORLD_SURFACE");
  require(world.setBlock({0, -4, 0}, Blocks::WATER.defaultState()),
          "place fluid outside a carved column");
  require(world.height(HeightmapType::WorldSurface, 0, 0) == -4 &&
              world.height(HeightmapType::OceanFloor, 0, 0) == -5 &&
              world.height(HeightmapType::MotionBlocking, 0, 0) == -4,
          "live maps preserve their independent predicates");
  require(world.heightmap(0, 0, HeightmapType::WorldSurfaceWG) == nullptr,
          "final chunks do not create WORLD_SURFACE_WG");

  LoadedWorld loaded;
  loaded.chunks.push_back({1, -1});
  loaded.blocks[{17, -3, -1}] = Blocks::STONE.defaultState();
  loaded.blocks[{17, -2, -1}] = Blocks::WATER.defaultState();
  World imported;
  importWorld(loaded, imported);
  require(imported.height(HeightmapType::WorldSurface, 17, -1) == -2 &&
              imported.height(HeightmapType::OceanFloor, 17, -1) == -3,
          "importWorld primes raw block data after loading chunks");

  imported.rawSetBlock({17, -3, -1}, airState());
  imported.rawSetBlock({17, -2, -1}, airState());
  LoadedWorld emptied;
  emptied.chunks.push_back({1, -1});
  importWorld(emptied, imported);
  require(imported.height(HeightmapType::WorldSurface, 17, -1) == -65 &&
              imported.height(HeightmapType::OceanFloor, 17, -1) == -65,
          "a second import clears formerly occupied heightmap columns");

  World shifted(-63, 312);
  shifted.loadChunk(0, 0);
  require(shifted.setBlock({0, -5, 0}, Blocks::STONE.defaultState()) &&
              shifted.height(HeightmapType::WorldSurface, 0, 0) == -5,
          "heightmaps retain the existing unaligned-minY simulation contract");
  bool rejectedShiftedLight = false;
  try {
    shifted.enableLighting();
  } catch (const std::invalid_argument &) {
    rejectedShiftedLight = true;
  }
  require(rejectedShiftedLight,
          "only the section-addressed light entry rejects unaligned minY");
  require(shifted.getBlock({0, -5, 0}).is(Blocks::STONE),
          "rejected lighting opt-in leaves the world usable");
}
} // namespace

int main() {
  try {
    typesAndEncoding();
    predicates();
    updatesAndPrime();
    sectionedDownwardRescan();
    worldIntegration();
    std::cout << "heightmap tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "heightmap test failure: " << error.what() << '\n';
    return 1;
  }
}
