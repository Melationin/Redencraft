#include "redstone/engine.hpp"
#include "redstone/worldgen.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

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
        const std::string key = stateKey(chunk.getBlockState(minX + x, y, minZ + z));
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

std::string dumpColumnRuns(const ChunkAccess &chunk, int lx, int lz) {
  const int bx = (chunk.pos().x << 4) + lx;
  const int bz = (chunk.pos().z << 4) + lz;
  const int minY = chunk.minY();
  const int maxY = chunk.maxY();
  BlockState prev = Blocks::AIR.defaultState();
  int runStart = minY;
  std::vector<std::string> runs;
  for (int y = minY; y < maxY; ++y) {
    const BlockState cur = chunk.getBlockState(bx, y, bz);
    if (cur != prev) {
      if (y > runStart && !prev.isAir()) {
        runs.push_back(std::to_string(runStart) + ".." + std::to_string(y - 1) +
                       "=" + stateKey(prev));
      }
      prev = cur;
      runStart = y;
    }
  }
  if (!prev.isAir()) {
    runs.push_back(std::to_string(runStart) + ".." +
                   std::to_string(maxY - 1) + "=" + stateKey(prev));
  }
  std::string joined;
  for (std::size_t i = 0; i < runs.size(); ++i) {
    if (i > 0)
      joined += ';';
    joined += runs[i];
  }
  return joined;
}

std::map<std::string, int> countChunkBlockNames(const ChunkAccess &chunk) {
  std::map<std::string, int> counts;
  const int minX = chunk.pos().x << 4;
  const int minZ = chunk.pos().z << 4;
  const int minY = chunk.minY();
  const int maxY = chunk.maxY();
  for (int y = minY; y < maxY; ++y) {
    for (int x = 0; x < 16; ++x) {
      for (int z = 0; z < 16; ++z) {
        const BlockState s = chunk.getBlockState(minX + x, y, minZ + z);
        ++counts[s.name()];
      }
    }
  }
  return counts;
}

void assertChunkHash(const ChunkAccess &chunk, std::uint64_t expectedHash,
                     std::string_view label) {
  const std::uint64_t got = fnvHashChunk(chunk);
  if (got != expectedHash) {
    std::cerr << "Hash mismatch on " << label << ": got 0x" << std::hex
              << std::setw(16) << std::setfill('0') << got << " expected 0x"
              << std::setw(16) << std::setfill('0') << expectedHash << std::dec
              << "\n";
    assert(false && "Vanilla 26.1 3D chunk hash mismatch");
  }
}

void assertBlockHistogram(const ChunkAccess &chunk,
                          const std::map<std::string, int> &expected,
                          std::string_view label) {
  const auto got = countChunkBlockNames(chunk);
  if (got != expected) {
    std::cerr << "Block histogram mismatch on " << label << ":\n  got:      ";
    for (const auto &[k, v] : got)
      std::cerr << k << "=" << v << ",";
    std::cerr << "\n  expected: ";
    for (const auto &[k, v] : expected)
      std::cerr << k << "=" << v << ",";
    std::cerr << "\n";
    assert(false && "Vanilla 26.1 chunk block histogram mismatch");
  }
}

void assertColumn(ChunkAccess &chunk, const NoiseBasedChunkGenerator &gen,
                  std::int64_t seed, int lx, int lz, int expectedWs,
                  int expectedOf, int expectedBh, std::string_view expectedBiome,
                  std::string_view expectedRuns, std::string_view label) {
  const int bx = (chunk.pos().x << 4) + lx;
  const int bz = (chunk.pos().z << 4) + lz;
  const int ws = chunk.getHeight(HeightmapType::WorldSurfaceWG, lx, lz);
  const int of = chunk.getHeight(HeightmapType::OceanFloorWG, lx, lz);
  const int bh = gen.getBaseHeight(bx, bz, HeightmapType::WorldSurfaceWG, seed);
  const std::string_view biome =
      chunk.getNoiseBiome(bx >> 2, ws >> 2, bz >> 2);
  const std::string runs = dumpColumnRuns(chunk, lx, lz);

  if (ws != expectedWs || of != expectedOf || bh != expectedBh ||
      biome != expectedBiome || runs != expectedRuns) {
    std::cerr << "Mismatch on " << label << " (" << bx << "," << bz << "):\n"
              << "  got:      ws=" << ws << " of=" << of << " bh=" << bh
              << " biome=" << biome << " runs=" << runs << "\n"
              << "  expected: ws=" << expectedWs << " of=" << expectedOf
              << " bh=" << expectedBh << " biome=" << expectedBiome
              << " runs=" << expectedRuns << "\n";
    assert(false && "Vanilla 26.1 terrain column mismatch");
  }
}

void testDisabledAquiferInvariants() {
  auto picker = [](int, int, int) {
    return FluidStatus{63, Blocks::WATER.defaultState()};
  };
  auto aq = Aquifer::createDisabled(picker);
  assert(!aq->shouldScheduleFluidUpdate());
  assert(!aq->computeSubstance(DensityFunctionContext{0, 60, 0}, 0.5).has_value());
  assert(aq->computeSubstance(DensityFunctionContext{0, 62, 0}, -0.1) ==
         Blocks::WATER.defaultState());
  assert(aq->computeSubstance(DensityFunctionContext{0, 63, 0}, -0.1) ==
         Blocks::AIR.defaultState());
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: worldgen_g2_terrain_tests <worldgen_assets_dir>\n";
    return 2;
  }
  testDisabledAquiferInvariants();

  const std::filesystem::path assetsDir(argv[1]);
  const auto reg = WorldgenRegistries::loadFromDirectory(assetsDir);

  assert(reg.noiseSettings().size() == 7);
  assert(reg.biomeClimates().size() == 65);
  for (const auto &[id, settings] : reg.noiseSettings()) {
    assert(settings.surfaceRule != nullptr && "All noise settings have surface rules");
    (void)id;
  }

  constexpr std::int64_t kSeed1 = 123456789LL;
  constexpr std::int64_t kSeed2 = -987654321LL;

  // 1. Overworld: golden parity against server-26.1.jar bare Noise + Surface
  {
    const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
    assert(owSettings != nullptr);
    auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
        MultiNoiseBiomeSource::createOverworld());
    NoiseBasedChunkGenerator gen(reg, *owSettings, owBiomes);
    ChunkGenerationPipeline pipe(gen, kSeed1);

    // River + caves + deepslate/stone transition
    ChunkAccess &c00 = pipe.ensureStatus(ChunkTicketPos{0, 0}, ChunkStatusId::Surface);
    assert(c00.persistedStatus() == ChunkStatusId::Surface);
    assertColumn(c00, gen, kSeed1, 0, 0, 62, 54, 63, "minecraft:river",
                 "-64..-61=minecraft:bedrock;-60..-28=minecraft:deepslate;"
                 "-24..-16=minecraft:deepslate;-11..5=minecraft:deepslate;"
                 "6..49=minecraft:stone;50..54=minecraft:dirt;55..62=minecraft:water[level=0]",
                 "overworld(0,0)[0,0]");
    assertColumn(c00, gen, kSeed1, 7, 9, 62, 54, 63, "minecraft:river",
                 "-64..-60=minecraft:bedrock;-59..-26=minecraft:deepslate;"
                 "-12..1=minecraft:deepslate;2..49=minecraft:stone;"
                 "50..54=minecraft:dirt;55..62=minecraft:water[level=0]",
                 "overworld(0,0)[7,9]");
    assertColumn(c00, gen, kSeed1, 15, 15, 62, 53, 63, "minecraft:river",
                 "-64..-63=minecraft:bedrock;-62..-24=minecraft:deepslate;"
                 "-8..2=minecraft:deepslate;3..3=minecraft:stone;4..6=minecraft:deepslate;"
                 "7..48=minecraft:stone;49..53=minecraft:dirt;54..62=minecraft:water[level=0]",
                 "overworld(0,0)[15,15]");
    assertBlockHistogram(c00,
                         {{"minecraft:air", 68909},
                          {"minecraft:bedrock", 790},
                          {"minecraft:deepslate", 13667},
                          {"minecraft:dirt", 1288},
                          {"minecraft:grass_block", 4},
                          {"minecraft:stone", 11507},
                          {"minecraft:water", 2139}},
                         "overworld(0,0)");
    assertChunkHash(c00, 0x8ba53b098eafeb39ULL, "overworld(0,0)");

    // Forest
    ChunkAccess &c3m2 =
        pipe.ensureStatus(ChunkTicketPos{3, -2}, ChunkStatusId::Surface);
    assertColumn(c3m2, gen, kSeed1, 0, 0, 64, 64, 65, "minecraft:forest",
                 "-64..-62=minecraft:bedrock;-61..-25=minecraft:deepslate;"
                 "-17..2=minecraft:deepslate;3..59=minecraft:stone;"
                 "60..63=minecraft:dirt;64..64=minecraft:grass_block[snowy=false]",
                 "overworld(3,-2)[0,0]");
    assertColumn(c3m2, gen, kSeed1, 7, 9, 65, 65, 66, "minecraft:forest",
                 "-64..-61=minecraft:bedrock;-60..-29=minecraft:deepslate;"
                 "-15..5=minecraft:deepslate;6..59=minecraft:stone;"
                 "60..64=minecraft:dirt;65..65=minecraft:grass_block[snowy=false]",
                 "overworld(3,-2)[7,9]");
    assertColumn(c3m2, gen, kSeed1, 15, 15, 66, 66, 67, "minecraft:forest",
                 "-64..-64=minecraft:bedrock;-63..-62=minecraft:deepslate;"
                 "-61..-60=minecraft:bedrock;-59..-51=minecraft:deepslate;"
                 "-46..-27=minecraft:deepslate;-17..5=minecraft:deepslate;"
                 "6..6=minecraft:stone;7..7=minecraft:deepslate;"
                 "8..60=minecraft:stone;61..65=minecraft:dirt;66..66=minecraft:grass_block[snowy=false]",
                 "overworld(3,-2)[15,15]");
    assertBlockHistogram(c3m2,
                         {{"minecraft:air", 67821},
                          {"minecraft:bedrock", 774},
                          {"minecraft:deepslate", 13860},
                          {"minecraft:dirt", 1124},
                          {"minecraft:grass_block", 256},
                          {"minecraft:stone", 14469}},
                         "overworld(3,-2)");
    assertChunkHash(c3m2, 0x3aec8b4062bc3706ULL, "overworld(3,-2)");

    // Ore vein + lava aquifer (-6, 1)
    ChunkAccess &cOreLava =
        pipe.ensureStatus(ChunkTicketPos{-6, 1}, ChunkStatusId::Surface);
    assertBlockHistogram(cOreLava,
                         {{"minecraft:air", 62550},
                          {"minecraft:bedrock", 763},
                          {"minecraft:copper_ore", 36},
                          {"minecraft:deepslate", 16470},
                          {"minecraft:dirt", 512},
                          {"minecraft:granite", 331},
                          {"minecraft:grass_block", 256},
                          {"minecraft:lava", 6},
                          {"minecraft:raw_copper_block", 1},
                          {"minecraft:stone", 17379}},
                         "overworld(-6,1)");
    assertChunkHash(cOreLava, 0x5455e4b63a19fa9aULL, "overworld(-6,1)");

    // Eroded badlands (-154, -138) terracotta bands + red sand
    ChunkAccess &cBadlands =
        pipe.ensureStatus(ChunkTicketPos{-154, -138}, ChunkStatusId::Surface);
    assertColumn(cBadlands, gen, kSeed1, 8, 8, 58, 58, 59,
                 "minecraft:eroded_badlands",
                 "-64..-63=minecraft:bedrock;-62..-62=minecraft:deepslate;"
                 "-61..-61=minecraft:bedrock;-60..-18=minecraft:deepslate;"
                 "-3..3=minecraft:deepslate;4..5=minecraft:stone;"
                 "6..6=minecraft:deepslate;7..20=minecraft:stone;"
                 "28..54=minecraft:stone;55..55=minecraft:sandstone;"
                 "56..58=minecraft:sand",
                 "badlands(-154,-138)[8,8]");
    assertChunkHash(cBadlands, 0xe90b660058f37a78ULL, "overworld(-154,-138)");

    // Frozen ocean + iceberg with packed_ice
    ChunkAccess &cFrozen =
        pipe.ensureStatus(ChunkTicketPos{146, -122}, ChunkStatusId::Surface);
    assertChunkHash(cFrozen, 0x397d100aeb1acea3ULL, "overworld(146,-122)");

    ChunkAccess &cIceberg =
        pipe.ensureStatus(ChunkTicketPos{148, -122}, ChunkStatusId::Surface);
    assertChunkHash(cIceberg, 0xd462a89a8a94da37ULL, "overworld(148,-122)");

    // Second seed Overworld (-2, 5)
    ChunkGenerationPipeline pipe2(gen, kSeed2);
    ChunkAccess &cSeed2 =
        pipe2.ensureStatus(ChunkTicketPos{-2, 5}, ChunkStatusId::Surface);
    assertChunkHash(cSeed2, 0x7078a1e40a58c2cbULL, "overworld(-2,5) seed2");

    // Also test installing a generated chunk into a live World at Full status
    World world;
    assert(pipe.generateAndInstallIntoWorld(ChunkTicketPos{0, 0}, world));
    assert(world.isChunkLoaded(0, 0));
    assert(world.getBlock(Pos{0, 62, 0}).is(Blocks::WATER));
    assert(world.getBlock(Pos{0, 54, 0}).is(Blocks::SAND));
    assert(world.getBlock(Pos{0, -64, 0}).is(Blocks::BEDROCK));
  }

  // 2. Nether: golden parity against server-26.1.jar (Nether Wastes + Basalt Deltas)
  {
    const auto *neSettings = reg.findNoiseSettings("minecraft:nether");
    assert(neSettings != nullptr);
    auto neBiomes = std::make_shared<MultiNoiseBiomeSource>(
        MultiNoiseBiomeSource::createNether());
    NoiseBasedChunkGenerator gen(reg, *neSettings, neBiomes);
    ChunkGenerationPipeline pipe(gen, kSeed1);
    ChunkAccess &c00 = pipe.ensureStatus(ChunkTicketPos{0, 0}, ChunkStatusId::Surface);
    assertColumn(c00, gen, kSeed1, 4, 11, 127, 127, 128, "minecraft:nether_wastes",
                 "0..1=minecraft:bedrock;2..51=minecraft:netherrack;"
                 "63..65=minecraft:netherrack;109..123=minecraft:netherrack;"
                 "124..124=minecraft:bedrock;125..125=minecraft:netherrack;126..127=minecraft:bedrock",
                 "nether(0,0)[4,11]");
    assertChunkHash(c00, 0xe0141efdf508e598ULL, "nether(0,0)");

    ChunkAccess &cBasalt =
        pipe.ensureStatus(ChunkTicketPos{0, 8}, ChunkStatusId::Surface);
    assertChunkHash(cBasalt, 0x8ef271e761ce57fdULL, "nether(0,8) basalt_deltas");
  }

  // 3. End: golden parity against server-26.1.jar (central + outer island)
  {
    const auto *enSettings = reg.findNoiseSettings("minecraft:end");
    assert(enSettings != nullptr);
    auto enBiomes = std::make_shared<TheEndBiomeSource>();
    NoiseBasedChunkGenerator gen(reg, *enSettings, enBiomes);
    ChunkGenerationPipeline pipe(gen, kSeed1);
    ChunkAccess &c00 = pipe.ensureStatus(ChunkTicketPos{0, 0}, ChunkStatusId::Surface);
    assertColumn(c00, gen, kSeed1, 2, 3, 63, 63, 64, "minecraft:the_end",
                 "14..63=minecraft:end_stone", "end(0,0)[2,3]");
    assertChunkHash(c00, 0x2493d3596aa95c19ULL, "end(0,0)");

    ChunkAccess &cOuter =
        pipe.ensureStatus(ChunkTicketPos{70, 0}, ChunkStatusId::Surface);
    assertChunkHash(cOuter, 0xb9292e07be0c2325ULL, "end(70,0)");
  }

  std::cout << "worldgen_g2_terrain_tests passed\n";
  return 0;
}
