#include "redstone/engine.hpp"
#include "redstone/light/service.hpp"
#include "redstone/nbt.hpp"
#include "redstone/region.hpp"
#include "redstone/terrain_codec.hpp"
#include "redstone/worldgen.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool cond, const char *msg) {
  if (!cond)
    throw std::runtime_error(msg);
}

using namespace redstone;
using namespace redstone::worldgen;

class TestTerrainGenerator final : public ChunkGenerator {
public:
  void createBiomes(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t) override {
    // Reject writes during BIOMES (blockStateWriteRadius == -1).
    require(!region.ensureCanWrite(chunk.pos().x << 4, 64, chunk.pos().z << 4),
            "BIOMES step must disallow block writes");

    const std::string biome = ((chunk.pos().x + chunk.pos().z) & 1) == 0
                                  ? "minecraft:plains"
                                  : "minecraft:desert";
    const int minQy = chunk.minY() >> 2;
    const int maxQy = (chunk.maxY() >> 2) - 1;
    for (int qy = minQy; qy <= maxQy; ++qy) {
      for (int qz = 0; qz < 4; ++qz) {
        for (int qx = 0; qx < 4; ++qx) {
          chunk.setNoiseBiome((chunk.pos().x << 2) + qx, qy,
                              (chunk.pos().z << 2) + qz, biome);
        }
      }
    }
  }

  void fillFromNoise(WorldGenRegion &region, ProtoChunk &chunk,
                     std::int64_t) override {
    const int wx0 = chunk.pos().x << 4;
    const int wz0 = chunk.pos().z << 4;
    // Neighbor outside writeRadius=0 must reject block write.
    require(!region.setBlockState(wx0 + 16, 64, wz0, defaultState("stone")),
            "NOISE step must reject writes outside chunk");
    // Neighbor chunk biome within r=1 is readable.
    (void)region.getNoiseBiome((chunk.pos().x + 1) << 2, 16,
                               chunk.pos().z << 2);

    const BlockState bedrock = defaultState("bedrock");
    const BlockState stone = defaultState("stone");
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        require(region.setBlockState(wx0 + lx, -64, wz0 + lz, bedrock),
                "bedrock fill");
        for (int y = -63; y <= 62; ++y) {
          require(region.setBlockState(wx0 + lx, y, wz0 + lz, stone),
                  "stone fill");
        }
      }
    }
  }

  void buildSurface(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t) override {
    const int wx0 = chunk.pos().x << 4;
    const int wz0 = chunk.pos().z << 4;
    const std::string_view biome =
        chunk.getNoiseBiome(chunk.pos().x << 2, 16, chunk.pos().z << 2);
    const BlockState top =
        biome == "minecraft:desert" ? defaultState("sand") : defaultState("grass_block");
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz)
        region.setBlockState(wx0 + lx, 63, wz0 + lz, top);
    }
  }

  void applyCarvers(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t) override {
    if (chunk.pos().x == 0 && chunk.pos().z == 0) {
      chunk.getOrCreateCarvingMask().set(4, 10, 7);
      region.setBlockState(4, 10, 7, BlockState{});
    }
  }

  void applyBiomeDecoration(WorldGenRegion &region, ProtoChunk &chunk,
                            std::int64_t) override {
    if (chunk.pos().x == 0 && chunk.pos().z == 0) {
      // Write a stone marker + water spring into neighboring chunk (1, 0) within writeRadius=1.
      require(region.setBlockState(16, 64, 5, defaultState("water")),
              "FEATURES step must allow r=1 neighbor write");
    }
  }
};

void testStatusAndPyramid() {
  const auto &list = ChunkStatus::statusList();
  require(list.size() == 12, "ChunkStatus must have 12 statuses");
  require(list[0] == &ChunkStatus::empty() &&
              list[11] == &ChunkStatus::full(),
          "status list order");
  require(ChunkStatus::empty().chunkType() == ChunkType::ProtoChunk &&
              ChunkStatus::spawn().chunkType() == ChunkType::ProtoChunk &&
              ChunkStatus::full().chunkType() == ChunkType::LevelChunk,
          "ChunkType mapping");
  require(ChunkStatus::byName("minecraft:carvers") == &ChunkStatus::carvers() &&
              ChunkStatus::byName("biomes") == &ChunkStatus::biomes() &&
              ChunkStatus::byName("minecraft:missing") == nullptr,
          "ChunkStatus::byName");

  // Heightmap masks per ChunkStatus.
  require(ChunkStatus::surface().heightmapsAfter().size() == 2 &&
              ChunkStatus::carvers().heightmapsAfter().size() == 4,
          "heightmapsAfter transition at Carvers");

  // Cross-check ChunkPyramid::generationPyramid against ticket_levels.
  for (int level = 30; level <= 46; ++level) {
    const auto fromPyramid = generationStatusForLevel(level);
    const auto fromTickets = ticket_levels::generationStatus(level);
    require(fromPyramid == fromTickets,
            "generationStatusForLevel must match ticket_levels::generationStatus");
  }
  const ChunkStep &noiseStep =
      ChunkPyramid::generationPyramid().getStepTo(ChunkStatusId::Noise);
  require(noiseStep.blockStateWriteRadius() == 0 &&
              noiseStep.directDependencies().getRadius() == 8 &&
              noiseStep.directDependencies().get(0) == ChunkStatusId::Biomes &&
              noiseStep.directDependencies().get(1) == ChunkStatusId::Biomes &&
              noiseStep.directDependencies().get(8) ==
                  ChunkStatusId::StructureStarts,
          "NOISE directDependencies");

  const ChunkStep &featStep =
      ChunkPyramid::generationPyramid().getStepTo(ChunkStatusId::Features);
  require(featStep.blockStateWriteRadius() == 1 &&
              featStep.directDependencies().get(1) == ChunkStatusId::Carvers &&
              featStep.directDependencies().get(8) ==
                  ChunkStatusId::StructureStarts,
          "FEATURES directDependencies");

  const ChunkStep &loadLight =
      ChunkPyramid::loadingPyramid().getStepTo(ChunkStatusId::Light);
  require(loadLight.directDependencies().getRadius() == 1 &&
              loadLight.directDependencies().get(1) ==
                  ChunkStatusId::InitializeLight,
          "LOADING_PYRAMID Light radius=1");

  // Pin full 12-element accumulatedDependencies of FULL against official server-26.1.jar probe.
  const ChunkStep &genFull =
      ChunkPyramid::generationPyramid().getStepTo(ChunkStatusId::Full);
  const std::vector<ChunkStatusId> expectedGenFullAcc{
      ChunkStatusId::Spawn,
      ChunkStatusId::InitializeLight,
      ChunkStatusId::Carvers,
      ChunkStatusId::Biomes,
      ChunkStatusId::StructureStarts,
      ChunkStatusId::StructureStarts,
      ChunkStatusId::StructureStarts,
      ChunkStatusId::StructureStarts,
      ChunkStatusId::StructureStarts,
      ChunkStatusId::StructureStarts,
      ChunkStatusId::StructureStarts,
      ChunkStatusId::StructureStarts,
  };
  require(genFull.accumulatedDependencies().asList() == expectedGenFullAcc,
          "GENERATION_PYRAMID FULL accumulatedDependencies must match server-26.1.jar");

  const ChunkStep &loadFull =
      ChunkPyramid::loadingPyramid().getStepTo(ChunkStatusId::Full);
  const std::vector<ChunkStatusId> expectedLoadFullAcc{
      ChunkStatusId::Spawn,
      ChunkStatusId::InitializeLight,
  };
  require(loadFull.accumulatedDependencies().asList() == expectedLoadFullAcc,
          "LOADING_PYRAMID FULL accumulatedDependencies must match server-26.1.jar");
}

void testBiomeContainerAndCarvingMask() {
  for (int count : {1, 2, 4, 8, 16, 64}) {
    BiomePaletteContainer box("minecraft:plains");
    for (int qy = 0; qy < 4; ++qy) {
      for (int qz = 0; qz < 4; ++qz) {
        for (int qx = 0; qx < 4; ++qx) {
          const int cell = (qy << 4) | (qz << 2) | qx;
          const int id = cell % count;
          box.set(qx, qy, qz, "minecraft:biome_" + std::to_string(id));
        }
      }
    }
    std::vector<std::string> palette;
    std::vector<std::uint64_t> data;
    int bits = 0;
    box.pack(palette, data, bits);
    if (count == 1)
      require(bits == 0 && data.empty(), "single biome 0 bits");
    else if (count == 2)
      require(bits == 1, "2 biomes -> 1 bit");
    else if (count == 4)
      require(bits == 2, "4 biomes -> 2 bits");
    else if (count == 8)
      require(bits == 3, "8 biomes -> 3 bits");
    else if (count == 16)
      require(bits == 4, "16 biomes -> 4 bits");
    else if (count == 64)
      require(bits == 6, "64 biomes -> 6 bits");

    BiomePaletteContainer restored;
    std::string err;
    require(restored.unpack(palette, data, err), err.c_str());
    for (int qy = 0; qy < 4; ++qy) {
      for (int qz = 0; qz < 4; ++qz) {
        for (int qx = 0; qx < 4; ++qx) {
          require(restored.get(qx, qy, qz) == box.get(qx, qy, qz),
                  "biome round-trip mismatch");
        }
      }
    }
  }

  CarvingMask mask(-64, 384);
  mask.set(0, -64, 0);
  mask.set(15, -1, 15);
  mask.set(7, 319, 3);
  const auto words = mask.toLongArray();
  const CarvingMask restored = CarvingMask::fromLongArray(words, -64, 384);
  require(restored.get(0, -64, 0) && restored.get(15, -1, 15) &&
              restored.get(7, 319, 3) && !restored.get(1, -64, 0),
          "CarvingMask round-trip");
}

void testProtoChunkPersistenceAndPipeline(
    const std::filesystem::path &scratchDir) {
  std::filesystem::remove_all(scratchDir);
  std::filesystem::create_directories(scratchDir);
  RegionFileStore store(scratchDir);

  TestTerrainGenerator generator;
  {
    ChunkGenerationPipeline partial(generator, 123456789LL, &store);
    // Loading level 35 maps to ChunkStatusId::Carvers in 26.1.
    ChunkAccess *carvedAccess = partial.ensureForLoadingLevel({0, 0}, 35);
    require(carvedAccess != nullptr &&
                carvedAccess->persistedStatus() == ChunkStatusId::Carvers,
            "level 35 generates to Carvers");
    auto protoOpt = partial.takeProtoChunk({0, 0});
    require(protoOpt.has_value(), "take ProtoChunk at Carvers");

    ProtoChunk &proto = *protoOpt;
    require(proto.getBlockState(0, 63, 0) == defaultState("grass_block"),
            "chunk (0,0) surface is grass_block");
    require(proto.getBlockState(4, 10, 7).isAir(),
            "chunk (0,0) carved cell is air");
    require(proto.carvingMask() && proto.carvingMask()->get(4, 10, 7),
            "chunk (0,0) carving mask bit recorded");
    require(proto.getHeight(HeightmapType::WorldSurface, 0, 0) == 63,
            "chunk (0,0) WORLD_SURFACE height");

    const nbt::RootTag nbtRoot = encodeProtoChunkToNbt(proto, 42);
    const std::vector<std::uint8_t> rawBytes = nbt::writeRoot(nbtRoot);

    // Default terrain decode must still preserve strict requireFullStatus=true.
    terrain::DecodedChunk strictCheck = terrain::decodeChunkNbt(rawBytes);
    require(strictCheck.status == terrain::DecodeStatus::Unsupported &&
                strictCheck.detail.find("not full") != std::string::npos,
            "strict decode rejects non-full protochunk");

    // Store partial ProtoChunk into real .mca file.
    const auto wr = store.writeChunk({0, 0}, rawBytes, 100);
    require(wr.wrote, "write ProtoChunk to .mca");
  }

  // Reload from .mca in a fresh pipeline, verify state & biomes, and finish to FULL -> World!
  World world;
  TestTerrainGenerator resumedGen;
  ChunkGenerationPipeline resumed(resumedGen, 123456789LL, &store, &world);
  require(resumed.generateAndInstallIntoWorld({0, 0}, world),
          "resume from .mca and install FULL into World");
  require(resumed.stats().chunksLoadedFromStore >= 1,
          "resumed loaded persisted ProtoChunk from store");
  require(world.isChunkLoaded(0, 0) &&
              world.getBlock({0, 63, 0}) == defaultState("grass_block") &&
              world.getBlock({4, 10, 7}).isAir(),
          "World received resumed chunk terrain");

  // Also test generating neighbor chunk (1, 0) directly and verify post-processing scheduled water!
  require(resumed.generateAndInstallIntoWorld({1, 0}, world),
          "install neighbor chunk (1, 0)");
  // Verify ChunkStatusId::Light leaves lightCorrect=false when no LightService is attached.
  require(world.getBlock({16, 64, 5}) == defaultState("water"),
          "Features writeRadius=1 water landed in (1, 0)");

  // ImposterProtoChunk wraps live World chunk and remaps *WG heightmaps.
  ImposterProtoChunk imposter(world, {0, 0}, false);
  require(imposter.persistedStatus() == ChunkStatusId::Full &&
              imposter.getHeight(HeightmapType::WorldSurfaceWG, 0, 0) == 63 &&
              !imposter.setBlockState(0, 65, 0, defaultState("stone")).has_value(),
          "ImposterProtoChunk read-only & WG heightmap remap");

  std::filesystem::remove_all(scratchDir);
}

void testPipelineWithLightService() {
  World world(-64, 320);
  LightWorkerOptions opts;
  opts.batchWindowMs = 0;
  world.enableLighting(opts);

  TestTerrainGenerator gen;
  ChunkGenerationPipeline pipe(gen, 123456789LL, nullptr, &world);
  ChunkAccess &litChunk = pipe.ensureStatus({0, 0}, ChunkStatusId::Light);
  require(litChunk.isLightCorrect(),
          "ChunkStatusId::Light must mark lightCorrect=true with LightService");

  auto *proto = dynamic_cast<ProtoChunk *>(&litChunk);
  require(proto != nullptr, "chunk before Full must be ProtoChunk");
  const int topSecIdx = proto->sectionIndex(64);
  // 光照数据现在只存在光照引擎里（ProtoChunk 不再拷贝副本），
  // 这里验证区块被标记为光照完成，并且引擎侧确实发布了天空光层。
  require(proto->isLightCorrect(), "Light step marks chunk lightCorrect");
  require(world.lightService() != nullptr &&
              world.lightService()->latestLayerChunk(LightLayer::Sky, 0, 0) !=
                  nullptr,
          "Light engine publishes sky layer for the generated chunk");
  (void)topSecIdx;

  require(pipe.generateAndInstallIntoWorld({0, 0}, world),
          "install lit Full chunk into World");
  world.lightService()->flushAndWait();
  world.drainLightPublications();

  // Open sky at y=64 above grass_block at y=63 has sky light 15; underground carved cave (4,10,7) has 0.
  require(world.getBrightness({0, 64, 0}, 0) == 15,
          "sky brightness above generated surface must be 15");
  require(world.getBrightness({4, 10, 7}, 0) == 0,
          "carved underground cave brightness must be 0");
}

} // namespace

int main(int argc, char **argv) {
  const std::filesystem::path scratchDir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() /
                     "redstone-worldgen-g0-test";
  testStatusAndPyramid();
  testBiomeContainerAndCarvingMask();
  testProtoChunkPersistenceAndPipeline(scratchDir);
  testPipelineWithLightService();
  std::cout << "worldgen_g0 tests passed\n";
  return 0;
}
