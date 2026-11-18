#include "redstone/engine.hpp"
#include "redstone/worldgen.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace redstone;
using namespace redstone::worldgen;

std::string stateKey(const BlockState &s) {
  std::string out = s.name();
  if (s.is(Blocks::WATER) || s.is(Blocks::LAVA)) {
    out += "[level=" + s.get("level", "0") + "]";
  } else if (s.hasProperty("snowy")) {
    out += "[snowy=" + s.get("snowy", "false") + "]";
  }
  return out;
}

std::uint64_t fnvHashChunk(const ChunkAccess &chunk) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  const int minX = chunk.pos().x << 4;
  const int minZ = chunk.pos().z << 4;
  const int minY = chunk.minY();
  const int maxY = chunk.maxY();
  for (int y = minY; y < maxY; ++y) {
    for (int x = 0; x < 16; ++x) {
      for (int z = 0; z < 16; ++z) {
        const std::string key =
            stateKey(chunk.getBlockState(minX + x, y, minZ + z));
        for (unsigned char c : key) {
          h ^= c;
          h *= 0x100000001b3ULL;
        }
        h ^= 0xffULL;
        h *= 0x100000001b3ULL;
      }
    }
  }
  return h;
}

void assertCarvedHash(ChunkAccess &chunk, std::uint64_t expected,
                      std::string_view label) {
  const std::uint64_t got = fnvHashChunk(chunk);
  if (got != expected) {
    std::cerr << "Carver hash mismatch on " << label << ": got 0x" << std::hex
              << std::setw(16) << std::setfill('0') << got << " expected 0x"
              << std::setw(16) << std::setfill('0') << expected << std::dec
              << "\n";
    assert(false && "Vanilla 26.1 carver chunk hash mismatch");
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: worldgen_carver_tests <worldgen_assets_dir>\n";
    return 2;
  }
  const std::filesystem::path assetsDir(argv[1]);
  const auto reg = WorldgenRegistries::loadFromDirectory(assetsDir);

  assert(reg.carvers().size() == 4);
  assert(reg.findCarver("minecraft:cave") != nullptr);
  assert(reg.findCarver("minecraft:cave_extra_underground") != nullptr);
  assert(reg.findCarver("minecraft:canyon") != nullptr);
  assert(reg.findCarver("minecraft:nether_cave") != nullptr);
  assert(reg.findBiomeGeneration("minecraft:plains")->carvers.size() == 3);
  assert(reg.findBiomeGeneration("minecraft:nether_wastes")->carvers.size() ==
         1);

  constexpr std::int64_t kSeed = 123456789LL;

  // 1. Overworld caves + canyons (including aquifer fluid preservation & grass promotion)
  {
    const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
    assert(owSettings != nullptr);
    auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
        MultiNoiseBiomeSource::createOverworld());
    NoiseBasedChunkGenerator gen(reg, *owSettings, owBiomes);
    ChunkGenerationPipeline pipe(gen, kSeed);

    const std::pair<ChunkTicketPos, std::uint64_t> cases[] = {
        {{0, 0}, 0x8ba53b098eafeb39ULL},
        {{1, 0}, 0x4ab0b0b531c9a4dcULL},
        {{-5, 4}, 0xe9c497ee9ac910beULL},
        {{4, 6}, 0x41c0de54e1f1d4e4ULL},
        {{-6, 2}, 0xcccca712de2b7269ULL},
        // Intersects trial_chambers (-17,5) + village_plains (-17,10) Beardifier;
        // verified bit-exact against server-26.1.jar with structures enabled.
        {{-15, 5}, 0x3d26862e72a9d57aULL},
    };
    for (const auto &[pos, expectedHash] : cases) {
      ChunkAccess &c = pipe.ensureStatus(pos, ChunkStatusId::Carvers);
      assert(c.persistedStatus() == ChunkStatusId::Carvers);
      assertCarvedHash(
          c, expectedHash,
          "overworld(" + std::to_string(pos.x) + "," + std::to_string(pos.z) +
              ")");
    }
  }

  // 2. Nether caves (cave_air + y<=31 lava)
  {
    const auto *neSettings = reg.findNoiseSettings("minecraft:nether");
    assert(neSettings != nullptr);
    auto neBiomes = std::make_shared<MultiNoiseBiomeSource>(
        MultiNoiseBiomeSource::createNether());
    NoiseBasedChunkGenerator gen(reg, *neSettings, neBiomes, redstone::netherDimensionType().minY, redstone::netherDimensionType().height);
    ChunkGenerationPipeline pipe(gen, kSeed);
    ChunkAccess &c =
        pipe.ensureStatus(ChunkTicketPos{-1, 0}, ChunkStatusId::Carvers);
    assert(c.persistedStatus() == ChunkStatusId::Carvers);
    assertCarvedHash(c, 0x1eb32a77db78b70bULL, "nether(-1,0)");
  }

  std::cout << "worldgen_carver_tests passed\n";
  return 0;
}
