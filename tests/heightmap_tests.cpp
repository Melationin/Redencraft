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
}
} // namespace

int main() {
  try {
    typesAndEncoding();
    predicates();
    updatesAndPrime();
    worldIntegration();
    std::cout << "heightmap tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "heightmap test failure: " << error.what() << '\n';
    return 1;
  }
}
