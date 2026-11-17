#include "redstone/engine.hpp"
#ifdef REDSTONE_HAVE_PROTOCOL
#include "redstone/net/wire.hpp"
#endif
#include "redstone/worldgen.hpp"
#include "redstone/world_chunk_manager.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace redstone::worldgen;

void require(bool cond, const std::string &msg) {
  if (!cond)
    throw std::runtime_error(msg);
}

struct QuartExpect {
  std::int64_t seed;
  int qx;
  int qy;
  int qz;
  const char *biome;
};

struct BlockExpect {
  std::int64_t seed;
  int bx;
  int by;
  int bz;
  const char *biome;
};

// Captured directly from official server-26.1.jar via WorldgenBiomeProbe
constexpr std::array<QuartExpect, 30> kOverworldGolden = {{
    {123456789LL, 0, 16, 0, "minecraft:river"},
    {123456789LL, 1, 16, 2, "minecraft:river"},
    {123456789LL, 2, 16, 3, "minecraft:river"},
    {123456789LL, 3, 16, 3, "minecraft:river"},
    {123456789LL, 0, -16, 0, "minecraft:river"},
    {123456789LL, -5, 16, 8, "minecraft:river"},
    {123456789LL, -5, -12, 8, "minecraft:river"},
    {123456789LL, 300, 16, -300, "minecraft:deep_ocean"},
    {123456789LL, 300, -4, -300, "minecraft:deep_ocean"},
    {123456789LL, 512, 30, -384, "minecraft:cold_ocean"},
    {123456789LL, 512, -14, -384, "minecraft:cold_ocean"},
    {123456789LL, -250, 20, 410, "minecraft:deep_cold_ocean"},
    {123456789LL, -250, 5, 410, "minecraft:deep_cold_ocean"},
    {123456789LL, 1000, 16, 1000, "minecraft:lush_caves"},
    {123456789LL, 1000, -10, 1000, "minecraft:pale_garden"},

    {-987654321LL, 0, 16, 0, "minecraft:lukewarm_ocean"},
    {-987654321LL, 1, 16, 2, "minecraft:lukewarm_ocean"},
    {-987654321LL, 2, 16, 3, "minecraft:lukewarm_ocean"},
    {-987654321LL, 3, 16, 3, "minecraft:lukewarm_ocean"},
    {-987654321LL, 0, -16, 0, "minecraft:lukewarm_ocean"},
    {-987654321LL, -5, 16, 8, "minecraft:lukewarm_ocean"},
    {-987654321LL, -5, -12, 8, "minecraft:lukewarm_ocean"},
    {-987654321LL, 300, 16, -300, "minecraft:forest"},
    {-987654321LL, 300, -4, -300, "minecraft:forest"},
    {-987654321LL, 512, 30, -384, "minecraft:taiga"},
    {-987654321LL, 512, -14, -384, "minecraft:taiga"},
    {-987654321LL, -250, 20, 410, "minecraft:savanna"},
    {-987654321LL, -250, 5, 410, "minecraft:savanna"},
    {-987654321LL, 1000, 16, 1000, "minecraft:sparse_jungle"},
    {-987654321LL, 1000, -10, 1000, "minecraft:sparse_jungle"},
}};

constexpr std::array<BlockExpect, 8> kBiomeManagerGolden = {{
    {123456789LL, 0, 64, 0, "minecraft:river"},
    {123456789LL, -17, 70, 33, "minecraft:river"},
    {123456789LL, 1201, 65, -1203, "minecraft:deep_ocean"},
    {123456789LL, 2050, 100, -1539, "minecraft:cold_ocean"},

    {-987654321LL, 0, 64, 0, "minecraft:lukewarm_ocean"},
    {-987654321LL, -17, 70, 33, "minecraft:lukewarm_ocean"},
    {-987654321LL, 1201, 65, -1203, "minecraft:forest"},
    {-987654321LL, 2050, 100, -1539, "minecraft:taiga"},
}};

constexpr std::array<QuartExpect, 10> kNetherGolden = {{
    {123456789LL, 0, 16, 0, "minecraft:nether_wastes"},
    {123456789LL, -5, 8, 9, "minecraft:nether_wastes"},
    {123456789LL, 300, 16, -300, "minecraft:nether_wastes"},
    {123456789LL, 512, 24, -384, "minecraft:basalt_deltas"},
    {123456789LL, -420, 12, 777, "minecraft:basalt_deltas"},

    {-987654321LL, 0, 16, 0, "minecraft:nether_wastes"},
    {-987654321LL, -5, 8, 9, "minecraft:warped_forest"},
    {-987654321LL, 300, 16, -300, "minecraft:crimson_forest"},
    {-987654321LL, 512, 24, -384, "minecraft:basalt_deltas"},
    {-987654321LL, -420, 12, 777, "minecraft:basalt_deltas"},
}};

constexpr std::array<QuartExpect, 14> kEndGolden = {{
    {123456789LL, 0, 16, 0, "minecraft:the_end"},
    {123456789LL, 256, 16, 0, "minecraft:the_end"},
    {123456789LL, 260, 16, 0, "minecraft:small_end_islands"},
    {123456789LL, -260, 16, 4, "minecraft:small_end_islands"},
    {123456789LL, 300, 16, -300, "minecraft:end_highlands"},
    {123456789LL, 512, 30, -384, "minecraft:small_end_islands"},
    {123456789LL, 800, 16, 600, "minecraft:end_midlands"},

    {-987654321LL, 0, 16, 0, "minecraft:the_end"},
    {-987654321LL, 256, 16, 0, "minecraft:the_end"},
    {-987654321LL, 260, 16, 0, "minecraft:end_barrens"},
    {-987654321LL, -260, 16, 4, "minecraft:small_end_islands"},
    {-987654321LL, 300, 16, -300, "minecraft:end_midlands"},
    {-987654321LL, 512, 30, -384, "minecraft:small_end_islands"},
    {-987654321LL, 800, 16, 600, "minecraft:end_midlands"},
}};

#ifdef REDSTONE_HAVE_PROTOCOL
std::vector<int> readWirePaletted(redstone::net::Reader &r, int count,
                                  int globalBits) {
  const int bits = r.u8();
  if (bits == 0)
    return std::vector<int>(count, r.varInt());
  std::vector<int> palette;
  int effBits = bits;
  if (bits <= (count == 64 ? 3 : 8)) {
    const int n = r.varInt();
    palette.resize(n);
    for (int i = 0; i < n; ++i)
      palette[i] = r.varInt();
  } else {
    effBits = globalBits;
  }
  const int perLong = 64 / effBits;
  const int words = (count + perLong - 1) / perLong;
  std::vector<std::uint64_t> longs(words);
  for (int i = 0; i < words; ++i)
    longs[i] = std::bit_cast<std::uint64_t>(r.i64());
  std::vector<int> out(count);
  const std::uint64_t mask = (std::uint64_t{1} << effBits) - 1;
  for (int i = 0; i < count; ++i) {
    const int idx = static_cast<int>(
        (longs[i / perLong] >> ((i % perLong) * effBits)) & mask);
    out[i] = palette.empty() ? idx : palette[idx];
  }
  return out;
}
#endif

void testSeedObfuscation() {
  require(BiomeManager::obfuscateSeed(123456789LL) == -5458710421630621949LL,
          "obfuscateSeed(123456789)");
  require(BiomeManager::obfuscateSeed(0LL) == 8794265229978523055LL,
          "obfuscateSeed(0)");
  require(BiomeManager::obfuscateSeed(-987654321LL) == 739882241652834894LL,
          "obfuscateSeed(-987654321)");
}

void testPresetsAndVanillaProbes(const std::filesystem::path &worldgenDir) {
  const auto owEntries = OverworldBiomeBuilder::buildEntries();
  require(owEntries.size() == 7593,
          "OverworldBiomeBuilder entry count == 7593, got " +
              std::to_string(owEntries.size()));

  const MultiNoiseBiomeSource owBiomes =
      MultiNoiseBiomeSource::createFromList(owEntries);
  const MultiNoiseBiomeSource neBiomes = MultiNoiseBiomeSource::createNether();
  const TheEndBiomeSource endBiomes;
  const FixedBiomeSource fixedBiomes("minecraft:cherry_grove");
  require(fixedBiomes.possibleBiomes().size() == 1 &&
              fixedBiomes.getNoiseBiome(10, 5, -10, ClimateSampler{}) ==
                  "minecraft:cherry_grove",
          "FixedBiomeSource");

  const WorldgenRegistries reg = WorldgenRegistries::loadFromDirectory(worldgenDir);
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  const auto *neSettings = reg.findNoiseSettings("minecraft:nether");
  const auto *endSettings = reg.findNoiseSettings("minecraft:end");
  require(owSettings && neSettings && endSettings, "missing noise_settings");
  require(owSettings->spawnTarget.size() == 2,
          "overworld.json spawn_target parsed");

  for (const std::int64_t seed : {123456789LL, -987654321LL}) {
    const RandomState owRs = RandomState::create(*owSettings, reg, seed);
    const redstone::Pos spawn = owRs.sampler().findSpawnPosition();
    if (seed == 123456789LL) {
      require(spawn == (redstone::Pos{67, 0, -37}),
              "spawn seed 123456789 mismatch");
    } else {
      require(spawn == (redstone::Pos{-38, 0, -294}),
              "spawn seed -987654321 mismatch");
    }

    // Check every Overworld golden point + verify RTree matches brute force!
    for (const auto &g : kOverworldGolden) {
      if (g.seed != seed)
        continue;
      const ClimateTargetPoint tp = owRs.sampler().sample(g.qx, g.qy, g.qz);
      const std::string &rtreeBiome = owBiomes.getNoiseBiome(tp);
      const std::string &bruteBiome = owBiomes.parameters().findValueBruteForce(tp);
      require(rtreeBiome == g.biome,
              "Overworld RTree mismatch at seed=" + std::to_string(seed) +
                  " got " + rtreeBiome + " expected " + g.biome);
      require(bruteBiome == rtreeBiome,
              "Overworld RTree vs BruteForce mismatch");
    }

    // Check BiomeManager zoom jittering against Java
    const BiomeManager bm(BiomeManager::obfuscateSeed(seed));
    const BiomeManager::NoiseBiomeReader owReader =
        [&](int qx, int qy, int qz) -> std::string_view {
      return owBiomes.getNoiseBiome(qx, qy, qz, owRs.sampler());
    };
    for (const auto &b : kBiomeManagerGolden) {
      if (b.seed != seed)
        continue;
      const std::string_view got = bm.getBiome(b.bx, b.by, b.bz, owReader);
      require(got == b.biome,
              "BiomeManager mismatch got " + std::string(got) + " expected " +
                  b.biome);
    }

    // Check Nether
    const RandomState neRs = RandomState::create(*neSettings, reg, seed);
    for (const auto &g : kNetherGolden) {
      if (g.seed != seed)
        continue;
      const std::string_view got =
          neBiomes.getNoiseBiome(g.qx, g.qy, g.qz, neRs.sampler());
      require(got == g.biome,
              "Nether mismatch got " + std::string(got) + " expected " + g.biome);
    }

    // Check End
    const RandomState endRs = RandomState::create(*endSettings, reg, seed);
    for (const auto &g : kEndGolden) {
      if (g.seed != seed)
        continue;
      const std::string_view got =
          endBiomes.getNoiseBiome(g.qx, g.qy, g.qz, endRs.sampler());
      require(got == g.biome,
              "End mismatch got " + std::string(got) + " expected " + g.biome);
    }
  }

  // Dense RTree vs BruteForce check across grid
  const RandomState owRs = RandomState::create(*owSettings, reg, 123456789LL);
  for (int qx = -20; qx <= 20; qx += 5) {
    for (int qy = -16; qy <= 48; qy += 16) {
      for (int qz = -20; qz <= 20; qz += 5) {
        const ClimateTargetPoint tp = owRs.sampler().sample(qx, qy, qz);
        const std::string &rt = owBiomes.getNoiseBiome(tp);
        const std::string &bf = owBiomes.parameters().findValueBruteForce(tp);
        if (rt != bf) {
          // If there is an exact tie in squared fitness, both have equal fitness.
          // Check fitness equality across entries in case RTree encountered tie:
          std::int64_t rtFit = INT64_MAX;
          std::int64_t bfFit = INT64_MAX;
          for (const auto &[pt, val] : owBiomes.parameters().entries()) {
            const std::int64_t f = pt.fitness(tp);
            if (val == rt)
              rtFit = std::min(rtFit, f);
            if (val == bf)
              bfFit = std::min(bfFit, f);
          }
          require(rtFit == bfFit, "RTree sub-optimal fitness compared to brute force");
        }
      }
    }
  }
}

void testProtoChunkWorldAndWireRoundtrip(
    const std::filesystem::path &worldgenDir,
    const std::filesystem::path &protocolDir) {
  const WorldgenRegistries reg = WorldgenRegistries::loadFromDirectory(worldgenDir);
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  require(owSettings != nullptr, "overworld settings missing");
  const RandomState rs = RandomState::create(*owSettings, reg, 123456789LL);
  const MultiNoiseBiomeSource owBiomes = MultiNoiseBiomeSource::createOverworld();

  // Pick chunk (250, 250) where y changes from pale_garden (deep) to lush_caves (upper),
  // AND inject a 12-biome section in section 0 & 2-biome section in section 1 to verify
  // full ChunkGenerationPipeline -> World::Section -> Wire Protocol 775 encoding!
  class TestBiomeGenerator final : public ChunkGenerator {
  public:
    TestBiomeGenerator(const MultiNoiseBiomeSource &src,
                       const ClimateSampler &sampler)
        : src_(src), sampler_(sampler) {}
    void createBiomes(WorldGenRegion &, ProtoChunk &chunk,
                      std::int64_t) override {
      fillBiomesFromNoise(chunk, src_, sampler_);
      if (chunk.pos().x == 250 && chunk.pos().z == 250) {
        static constexpr std::array<const char *, 12> kTestBiomes = {
            "minecraft:plains",       "minecraft:desert",
            "minecraft:forest",       "minecraft:taiga",
            "minecraft:swamp",        "minecraft:river",
            "minecraft:ocean",        "minecraft:savanna",
            "minecraft:badlands",     "minecraft:jungle",
            "minecraft:meadow",       "minecraft:cherry_grove",
        };
        for (int i = 0; i < 64; ++i) {
          const int qx = 250 * 4 + (i & 3);
          const int qz = 250 * 4 + ((i >> 2) & 3);
          const int qy = (i >> 4) & 3;
          chunk.setNoiseBiome(qx, qy, qz, kTestBiomes[i % kTestBiomes.size()]);
        }
        for (int i = 0; i < 64; ++i) {
          const int qx = 250 * 4 + (i & 3);
          const int qz = 250 * 4 + ((i >> 2) & 3);
          const int qy = 4 + ((i >> 4) & 3);
          chunk.setNoiseBiome(
              qx, qy, qz, (i & 1) ? "minecraft:desert" : "minecraft:badlands");
        }
      }
    }

  private:
    const MultiNoiseBiomeSource &src_;
    const ClimateSampler &sampler_;
  };

  TestBiomeGenerator gen(owBiomes, rs.sampler());
  redstone::World world(-64, 320);
  ChunkGenerationPipeline pipeline(gen, 123456789LL, nullptr, &world);
  require(pipeline.generateAndInstallIntoWorld({250, 250}, world),
          "generateAndInstallIntoWorld with biomes");
  require(world.getNoiseBiome(250 * 4, 0, 250 * 4) == "minecraft:plains",
          "world.getNoiseBiome direct check");
  require(world.getNoiseBiome(250 * 4 + 1, 0, 250 * 4) == "minecraft:desert",
          "world.getNoiseBiome direct check 2");
  require(world.getNoiseBiome(250 * 4, 16, 250 * 4) == "minecraft:lush_caves",
          "world.getNoiseBiome from noise fill");
  require(world.getNoiseBiome(250 * 4, -10, 250 * 4) == "minecraft:pale_garden",
          "world.getNoiseBiome deep pale_garden");

#ifdef REDSTONE_HAVE_PROTOCOL
  const redstone::net::Registry netReg(protocolDir);
  const redstone::net::Bytes pkt = redstone::net::encodeChunk(
      world.snapshotChunk(250, 250), 250, 250, netReg);

  const std::array<const char *, 12> kTestBiomes = {
      "minecraft:plains",       "minecraft:desert",      "minecraft:forest",
      "minecraft:taiga",        "minecraft:swamp",       "minecraft:river",
      "minecraft:ocean",        "minecraft:savanna",     "minecraft:badlands",
      "minecraft:jungle",       "minecraft:meadow",      "minecraft:cherry_grove",
  };

  redstone::net::Reader r(pkt);
  require(r.i32() == 250 && r.i32() == 250, "wire chunk coords");
  const int hmCount = r.varInt();
  for (int h = 0; h < hmCount; ++h) {
    (void)r.varInt();
    const int n = r.varInt();
    for (int j = 0; j < n; ++j)
      (void)r.i64();
  }
  const auto secBytes = r.raw(r.varInt());
  redstone::net::Reader secReader(secBytes);
  const int globalBlockBits = netReg.stateBits();
  const int globalBiomeBits = netReg.biomeBits();
  for (int secIdx = 0; secIdx < 24; ++secIdx) {
    const int secY = -4 + secIdx;
    (void)secReader.i16();
    (void)secReader.i16();
    (void)readWirePaletted(secReader, 4096, globalBlockBits);
    const std::vector<int> wireBiomes =
        readWirePaletted(secReader, 64, globalBiomeBits);
    if (secY == 0) {
      for (int c = 0; c < 64; ++c) {
        const int expectedId = netReg.biomeId(kTestBiomes[c % kTestBiomes.size()]);
        require(wireBiomes[c] == expectedId,
                "wire sec 0 direct global biome mismatch");
      }
    } else if (secY == 1) {
      for (int c = 0; c < 64; ++c) {
        const int expectedId = netReg.biomeId(
            (c & 1) ? "minecraft:desert" : "minecraft:badlands");
        require(wireBiomes[c] == expectedId,
                "wire sec 1 1-bit biome mismatch");
      }
    } else if (secY == 4) {
      // quartY = 16 -> local qy = 0 at (0,0) -> lush_caves
      require(wireBiomes[0] == netReg.biomeId("minecraft:lush_caves"),
              "wire sec 4 lush_caves mismatch");
    }
  }
  require(secReader.empty(), "wire sections consumed cleanly");
#else
  (void)protocolDir;
#endif
}

} // namespace

int main(int argc, char **argv) {
  try {
    const std::filesystem::path worldgenDir =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("assets/worldgen26_1");
    const std::filesystem::path protocolDir =
        argc > 2 ? std::filesystem::path(argv[2])
                 : std::filesystem::path("assets/protocol775");
    testSeedObfuscation();
    testPresetsAndVanillaProbes(worldgenDir);
    testProtoChunkWorldAndWireRoundtrip(worldgenDir, protocolDir);
    std::cout << "worldgen_g2_biome tests passed\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "worldgen_g2_biome test failed: " << ex.what() << "\n";
    return 1;
  }
}
