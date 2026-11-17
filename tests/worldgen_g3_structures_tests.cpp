#include "redstone/engine.hpp"
#include "redstone/worldgen.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace redstone::worldgen;

std::string stateHashKey(const redstone::BlockState &s) {
  std::string key = s.name();
  if (s.is(redstone::Blocks::WATER) || s.is(redstone::Blocks::LAVA)) {
    key += "[level=" + s.get("level", "0") + "]";
  }
  return key;
}

std::uint64_t fnvHashChunk(const ChunkAccess &chunk, int minY, int height) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  const int baseX = chunk.pos().x << 4;
  const int baseZ = chunk.pos().z << 4;
  for (int y = minY; y < minY + height; ++y) {
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        const std::string key =
            stateHashKey(chunk.getBlockState(baseX + lx, y, baseZ + lz));
        for (unsigned char b : key) {
          h ^= static_cast<std::uint64_t>(b);
          h *= 0x100000001b3ULL;
        }
        h ^= 0xffULL;
        h *= 0x100000001b3ULL;
      }
    }
  }
  return h;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: worldgen_g3_structures_tests <assets/worldgen26_1>\n";
    return 2;
  }
  const std::filesystem::path worldgenDir(argv[1]);
  const WorldgenRegistries registries =
      WorldgenRegistries::loadFromDirectory(worldgenDir);

  assert(registries.structureSets().size() == 20);
  assert(registries.structures().size() == 34);
  assert(registries.processorLists().size() == 40);
  assert(registries.templatePools().size() == 188);

  const auto *owSettings = registries.findNoiseSettings("minecraft:overworld");
  assert(owSettings != nullptr);
  auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createOverworld());
  NoiseBasedChunkGenerator owGen(registries, *owSettings, owBiomes);

  constexpr std::int64_t kSeed = 123456789LL;
  auto &structState = owGen.structureStateForSeed(kSeed);

  // 1. Stronghold ConcentricRings parity against server-26.1.jar
  const auto *strongholdSet = registries.findStructureSet("minecraft:strongholds");
  assert(strongholdSet != nullptr);
  const auto *rings = structState.getRingPositionsFor(*strongholdSet);
  assert(rings != nullptr && rings->size() == 128);
  const std::vector<redstone::ChunkTicketPos> expectedStrongholds = {
      {-70, -100}, {154, -7},  {-57, 102}, {174, 236},  {-127, 269},
      {-299, 26},  {-204, -291}, {139, -301}, {347, -38}, {513, 119},
  };
  for (std::size_t i = 0; i < expectedStrongholds.size(); ++i) {
    assert((*rings)[i] == expectedStrongholds[i]);
  }

  // 2. RandomSpread + ExclusionZone + LegacyType2 parity against server-26.1.jar
  const auto *villageSet = registries.findStructureSet("minecraft:villages");
  const auto *outpostSet =
      registries.findStructureSet("minecraft:pillager_outposts");
  const auto *treasureSet =
      registries.findStructureSet("minecraft:buried_treasures");
  assert(villageSet && outpostSet && treasureSet);

  std::vector<redstone::ChunkTicketPos> villageChunks;
  std::vector<redstone::ChunkTicketPos> outpostChunks;
  for (int cx = -24; cx <= 36; ++cx) {
    for (int cz = -24; cz <= 36; ++cz) {
      if (villageChunks.size() < 6 &&
          structState.isStructureChunk(*villageSet, cx, cz)) {
        villageChunks.push_back({cx, cz});
      }
      if (outpostChunks.size() < 6 &&
          structState.isStructureChunk(*outpostSet, cx, cz)) {
        outpostChunks.push_back({cx, cz});
      }
    }
  }
  assert((villageChunks == std::vector<redstone::ChunkTicketPos>{
             {-22, -22}, {-17, 10}, {21, 23}, {23, -13}}));
  assert((outpostChunks == std::vector<redstone::ChunkTicketPos>{{8, -11}}));

  std::vector<redstone::ChunkTicketPos> treasureChunks;
  for (int cx = 0; cx <= 40 && treasureChunks.size() < 6; ++cx) {
    for (int cz = 0; cz <= 40 && treasureChunks.size() < 6; ++cz) {
      if (structState.isStructureChunk(*treasureSet, cx, cz)) {
        treasureChunks.push_back({cx, cz});
      }
    }
  }
  assert((treasureChunks == std::vector<redstone::ChunkTicketPos>{
             {4, 0}, {5, 0}, {6, 38}, {7, 10}, {7, 25}, {9, 20}}));

  // 3. Full Jigsaw placement + Beardifier parity at (-51, -62) against server-26.1.jar
  const auto *plainsVillage =
      registries.findStructure("minecraft:village_plains");
  assert(plainsVillage != nullptr);
  auto &rs = owGen.randomStateForSeed(kSeed);
  const redstone::ChunkTicketPos vPos{-51, -62};
  auto start = generateStructureStart(*plainsVillage, owGen, rs, structState,
                                      kSeed, vPos, owGen.minY(), owGen.height());
  assert(start.has_value() && start->isValid());
  const BoundingBox totalBB = start->calculateBoundingBox();
  std::cerr << "C++ VILLAGE_START pieces=" << start->pieces.size()
            << " bb=(" << totalBB.minX << "," << totalBB.minY << ","
            << totalBB.minZ << ".." << totalBB.maxX << "," << totalBB.maxY
            << "," << totalBB.maxZ << ")\n";
  for (std::size_t i = 0; i < std::min<std::size_t>(6, start->pieces.size());
       ++i) {
    const auto &pc = start->pieces[i];
    std::cerr << "  piece[" << i << "] rot=" << static_cast<int>(pc.rotation)
              << " bb=(" << pc.boundingBox.minX << "," << pc.boundingBox.minY
              << "," << pc.boundingBox.minZ << ".." << pc.boundingBox.maxX << ","
              << pc.boundingBox.maxY << "," << pc.boundingBox.maxZ
              << ") junctions=" << pc.junctions.size() << "\n";
  }

  assert(start->pieces.size() == 98);
  assert(totalBB.minX == -892 && totalBB.minY == 58 && totalBB.minZ == -1084 &&
         totalBB.maxX == -773 && totalBB.maxY == 139 && totalBB.maxZ == -915);
  assert(start->pieces[0].rotation == redstone::Rotation::Clockwise180);
  assert(start->pieces[0].boundingBox.minX == -824 &&
         start->pieces[0].boundingBox.minY == 93 &&
         start->pieces[0].boundingBox.minZ == -1000 &&
         start->pieces[0].boundingBox.maxX == -816 &&
         start->pieces[0].boundingBox.maxY == 96 &&
         start->pieces[0].boundingBox.maxZ == -992);
  assert(start->pieces[0].junctions.size() == 9);

  // Check Beardifier single point density
  auto bf = Beardifier::forChunk({&(*start)}, vPos);
  const double bVal = bf->compute(DensityFunctionContext{
      (vPos.x << 4) + 4, start->pieces[0].boundingBox.minY, (vPos.z << 4) + 4});
  std::cout << "C++ BEARDIFIER_CENTER_DENSITY=" << bVal << "\n";
  assert(std::abs(bVal - (-0.331913)) < 1e-4);

  // Check Noise chunk hash via ChunkGenerationPipeline WITH StructureStarts / Beardifier
  ChunkGenerationPipeline pipeline(owGen, kSeed);
  ChunkAccess &generated = pipeline.ensureStatus(vPos, ChunkStatusId::Noise);
  const std::uint64_t hash =
      fnvHashChunk(generated, owGen.minY(), owGen.height());
  std::cout << "C++ VILLAGE_NOISE_HASH=0x" << std::hex << hash << std::dec
            << "\n";
  assert(hash == 0xc2529270cf16dafeULL);

  std::cout << "worldgen_g3_structures_tests passed!\n";
  return 0;
}
