#include "redstone/engine.hpp"
#include "redstone/worldgen.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

namespace {

using namespace redstone;
using namespace redstone::worldgen;

std::uint64_t hashChunkBlocksAndBiomes(const ChunkAccess &chunk) {
  std::uint64_t h = 14695981039346656037ULL;
  auto mix = [&](std::uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
  };
  const ChunkTicketPos cpos = chunk.pos();
  const int bx0 = cpos.x << 4;
  const int bz0 = cpos.z << 4;
  for (int y = chunk.minY(); y < chunk.maxY(); ++y) {
    for (int lz = 0; lz < 16; ++lz) {
      for (int lx = 0; lx < 16; ++lx) {
        mix(chunk.getBlockState(bx0 + lx, y, bz0 + lz).id());
      }
    }
  }
  const int qx0 = cpos.x << 2;
  const int qz0 = cpos.z << 2;
  const int qMinY = chunk.minY() >> 2;
  const int qMaxY = chunk.maxY() >> 2;
  for (int qy = qMinY; qy < qMaxY; ++qy) {
    for (int qz = 0; qz < 4; ++qz) {
      for (int qx = 0; qx < 4; ++qx) {
        const std::string_view b = chunk.getNoiseBiome(qx0 + qx, qy, qz0 + qz);
        for (unsigned char ch : b)
          mix(ch);
        mix(0xff);
      }
    }
  }
  return h;
}

void testParityWithSingleThreadCarvers(const WorldgenRegistries &reg) {
  constexpr std::int64_t kSeed = 123456789LL;
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  assert(owSettings != nullptr);
  auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createOverworld());

  NoiseBasedChunkGenerator refGen(reg, *owSettings, owBiomes);
  ChunkGenerationPipeline refPipe(refGen, kSeed);

  NoiseBasedChunkGenerator mtGen(reg, *owSettings, owBiomes);
  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 8;
  WorldGenScheduler sched(mtGen, kSeed, nullptr, nullptr, nullptr, nullptr,
                          cfg);

  const std::vector<ChunkTicketPos> coords = {
      {0, 0}, {1, 0}, {0, 1}, {1, 1}, {3, -2}, {-6, 1},
  };
  std::vector<ChunkBatchRequest> batch;
  for (ChunkTicketPos p : coords) {
    batch.push_back(ChunkBatchRequest{p, ChunkStatusId::Carvers});
  }
  sched.ensureStatusBatch(batch, {0, 0});

  for (ChunkTicketPos p : coords) {
    ChunkAccess &refChunk = refPipe.ensureStatus(p, ChunkStatusId::Carvers);
    ChunkAccess *mtChunk = sched.findChunk(p);
    assert(mtChunk != nullptr);
    assert(mtChunk->persistedStatus() == ChunkStatusId::Carvers);
    // NoiseChunk must have been stripped after Carvers(6).
    assert(mtChunk->cachedNoiseChunkOpaque() == nullptr);
    assert(hashChunkBlocksAndBiomes(*mtChunk) ==
           hashChunkBlocksAndBiomes(refChunk));
  }
}

void testClusteredBatchFullAndEviction(const WorldgenRegistries &reg) {
  constexpr std::int64_t kSeed = 849201745213LL;
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  assert(owSettings != nullptr);
  auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createOverworld());
  NoiseBasedChunkGenerator gen(reg, *owSettings, owBiomes);

  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 8;
  WorldGenScheduler sched(gen, kSeed, nullptr, nullptr, nullptr, nullptr, cfg);

  // Simulate fast player movement submitting a clustered 3x3 grid of FULL targets,
  // first upgrading center to Biomes, then pushing full 3x3 to FULL incrementally.
  std::vector<ChunkBatchRequest> wave1 = {
      {{200, 200}, ChunkStatusId::Biomes},
      {{201, 200}, ChunkStatusId::Carvers},
  };
  sched.ensureStatusBatch(wave1, {200, 200});

  std::vector<ChunkBatchRequest> wave2;
  std::vector<ChunkTicketPos> allTargets;
  for (int dz = 0; dz < 3; ++dz) {
    for (int dx = 0; dx < 3; ++dx) {
      ChunkTicketPos p{200 + dx, 200 + dz};
      wave2.push_back(ChunkBatchRequest{p, ChunkStatusId::Full});
      allTargets.push_back(p);
    }
  }
  sched.ensureStatusBatch(wave2, {201, 201});

  for (ChunkTicketPos p : allTargets) {
    ChunkAccess *c = sched.findChunk(p);
    assert(c != nullptr);
    assert(c->persistedStatus() == ChunkStatusId::Full);
    assert(c->cachedNoiseChunkOpaque() == nullptr);
    auto *proto = dynamic_cast<ProtoChunk *>(c);
    assert(proto != nullptr);
    assert(proto->carvingMask() == nullptr);
    assert(c->getHeight(HeightmapType::WorldSurface, 8, 8) > c->minY());
  }

  // Install one chunk into live World cleanly.
  assert(sched.stats().inlineSteps > 0);
  World world;
  assert(sched.generateAndInstallIntoWorld({201, 201}, world));
  assert(world.isChunkLoaded(201, 201));

  // Evict peripheral holders outside radius 0 around empty keep list -> clears all remaining.
  const std::size_t evicted = sched.evictIdleOutsideRadius({}, 0);
  assert(evicted > 100);
}

void testClusteredLightService(const WorldgenRegistries &reg) {
  constexpr std::int64_t kSeed = -987654321LL;
  const auto *endSettings = reg.findNoiseSettings("minecraft:end");
  assert(endSettings != nullptr);
  auto endBiomes = std::make_shared<TheEndBiomeSource>();
  NoiseBasedChunkGenerator gen(reg, *endSettings, endBiomes, redstone::endDimensionType().minY, redstone::endDimensionType().height);

  World world(0, 256);
  LightWorkerOptions lopts;
  lopts.batchWindowMs = 0;
  world.enableLighting(lopts);

  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 4;
  auto lightContext = std::make_shared<LightOpticalContext>();
  lightContext->set(world.lightService()->opticalTable());
  GeneratedLightQueue lightQueue;
  WorldGenScheduler sched(gen, kSeed, nullptr, &world, lightContext, &lightQueue,
                          cfg);

  std::vector<ChunkBatchRequest> batch = {
      {{0, 0}, ChunkStatusId::Full},
      {{1, 0}, ChunkStatusId::Full},
      {{0, 1}, ChunkStatusId::Full},
      {{1, 1}, ChunkStatusId::Full},
  };
  sched.ensureStatusBatch(batch, {0, 0});
  // 生成 worker 只投递纯值光照输入；LightService 的提交契约要求在主线程执行。
  assert(lightQueue.size() > 0);
  for (GeneratedLightWork &work : lightQueue.drain()) {
    if (work.compute) {
      assert(world.requestGeneratedChunkLightCompute(work.position.x,
                                                     work.position.z) != 0);
    } else {
      assert(world.submitGeneratedChunkLight(std::move(work.input)) != 0);
    }
  }
  for (const auto &req : batch) {
    ChunkAccess *c = sched.findChunk(req.pos);
    assert(c != nullptr);
    assert(c->persistedStatus() == ChunkStatusId::Full);
    // Light 阶段不再等待：状态到 Full 时光照可能还在引擎里异步计算。
    assert(!c->isLightCorrect());
  }
  // 需要光照数据的消费方自己等：引擎算完后这些区块才算“完整”。
  assert(world.lightService() != nullptr);
  world.lightService()->flushAndWait();
  for (const auto &req : batch) {
    assert(world.lightService()->isChunkSettled(req.pos.x, req.pos.z));
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "usage: worldgen_scheduler_tests <worldgen_assets_dir>\n";
    return 2;
  }
  const std::filesystem::path assetsDir(argv[1]);
  const WorldgenRegistries reg = WorldgenRegistries::loadFromDirectory(assetsDir);

  testParityWithSingleThreadCarvers(reg);
  testClusteredBatchFullAndEviction(reg);
  testClusteredLightService(reg);

  std::cout << "worldgen_scheduler_tests passed\n";
  return 0;
}
