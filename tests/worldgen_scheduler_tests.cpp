#include "redstone/engine.hpp"
#include "redstone/light/chunk_input.hpp"
#include "redstone/terrain_codec.hpp"
#include "redstone/worldgen.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

using namespace redstone;
using namespace redstone::worldgen;

std::uint64_t hashChunkBlocksAndBiomes(const BaseChunk &chunk) {
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
    BaseChunk &refChunk = refPipe.ensureStatus(p, ChunkStatusId::Carvers);
    BaseChunk *mtChunk = sched.findChunk(p);
    assert(mtChunk != nullptr);
    assert(chunkPersistedStatus(*mtChunk) == ChunkStatusId::Carvers);
    // NoiseChunk must have been stripped after Carvers(6).
    assert(mtChunk->isProtoChunk());
    assert(static_cast<ProtoChunk *>(mtChunk)->cachedNoiseChunkRaw() == nullptr);
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
    BaseChunk *c = sched.findChunk(p);
    assert(c != nullptr);
    assert(chunkPersistedStatus(*c) == ChunkStatusId::Full);
    assert(c->isProtoChunk());
    auto *proto = static_cast<ProtoChunk *>(c);
    assert(proto->cachedNoiseChunkRaw() == nullptr);
    assert(proto->carvingMask() == nullptr);
    assert(chunkHeight(*c, HeightmapType::WorldSurface, 8, 8) > c->minY());
  }

  // Install one chunk into live World cleanly.
  assert(sched.stats().inlineSteps > 0);
  World world;
  assert(sched.generateAndInstallIntoWorld({201, 201}, world));
  assert(world.isChunkLoaded(201, 201));

  // Evict peripheral holders outside radius 0 around empty keep list -> clears all remaining.
  const std::size_t evicted = sched.evictIdleOutsideRadius({}, 0);
  assert(evicted >= 20 && sched.heldChunkCount() == 0);
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
  cfg.asyncWorldGenLight = false;
  auto lightContext = std::make_shared<LightOpticalContext>();
  lightContext->set(world.lightService()->opticalTable(),
                    world.lightService()->skyLightEnabled());
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
    BaseChunk *c = sched.findChunk(req.pos);
    assert(c != nullptr);
    assert(chunkPersistedStatus(*c) == ChunkStatusId::Full);
    // 并行 3x3 初始光照在 Light 阶段直接完成，Full 区块已携带预计算光照。
    assert(c->isLightCorrect());
  }
  // 需要光照数据的消费方自己等：零拷贝采纳与发布收敛后这些区块才算“完整”。
  assert(world.lightService() != nullptr);
  world.lightService()->flushAndWait();
  assert(world.lightService()->stats().lightQueueEntries == 0);
  for (const auto &req : batch) {
    assert(world.lightService()->isChunkSettled(req.pos.x, req.pos.z));
  }
}

void testParallelInitialLightParityWithLightService(
    const WorldgenRegistries &reg) {
  auto runParityCase = [&](const DimensionTypeSpec &dim, const char *noiseId,
                           std::shared_ptr<BiomeSource> biomes,
                           std::int64_t seed) {
    const auto *settings = reg.findNoiseSettings(noiseId);
    assert(settings != nullptr);
    NoiseBasedChunkGenerator gen(reg, *settings, std::move(biomes), dim.minY,
                                 dim.height);

    World world(dim.minY, dim.minY + dim.height);
    LightWorkerOptions lopts;
    lopts.batchWindowMs = 0;
    lopts.hasSkyLight = dim.hasSkyLight;
    world.enableLighting(lopts);

    WorldGenSchedulerConfig cfg;
    cfg.threadCount = 4;
    cfg.asyncWorldGenLight = false;
    auto lightContext = std::make_shared<LightOpticalContext>();
    lightContext->set(world.lightService()->opticalTable(),
                      world.lightService()->skyLightEnabled());
    GeneratedLightQueue lightQueue;
    WorldGenScheduler sched(gen, seed, nullptr, &world, lightContext,
                            &lightQueue, cfg);

    std::vector<ChunkBatchRequest> batch;
    batch.reserve(25);
    for (int cz = -2; cz <= 2; ++cz) {
      for (int cx = -2; cx <= 2; ++cx) {
        batch.push_back({{cx, cz}, ChunkStatusId::Full});
      }
    }
    sched.ensureStatusBatch(batch, {0, 0});
    for (GeneratedLightWork &work : lightQueue.drain()) {
      if (work.compute) {
        assert(world.requestGeneratedChunkLightCompute(work.position.x,
                                                       work.position.z) != 0);
      } else {
        assert(world.submitGeneratedChunkLight(std::move(work.input)) != 0);
      }
    }
    world.lightService()->flushAndWait();
    world.drainLightPublications();
    assert(world.lightService()->stats().lightQueueEntries == 0);

    const int minSectionY = dim.minY >> 4;
    const int sectionCount = dim.height >> 4;
    LightWorkerOptions refOpts;
    refOpts.batchWindowMs = 0;
    refOpts.hasSkyLight = dim.hasSkyLight;
    LightService refService(minSectionY, sectionCount, refOpts);

    for (const auto &req : batch) {
      std::shared_ptr<BaseChunk> c = sched.findChunkShared(req.pos);
      assert(c != nullptr);
      assert(c->isLightCorrect());
      assert(world.lightService()->isChunkSettled(req.pos.x, req.pos.z));
      const auto *proto = static_cast<const ProtoChunk *>(c.get());
      assert(proto->precomputedLight() != nullptr);
      assert(proto->precomputedLight()->isLightOn);
      if (!dim.hasSkyLight) {
        assert(proto->precomputedLight()->skyLayers.empty());
        assert(world.lightService()->latestLayerChunk(LightLayer::Sky,
                                                      req.pos.x,
                                                      req.pos.z) == nullptr);
      }
      LightChunkInput refInput =
          buildLightChunkInput(*c, std::nullopt, true, c);
      assert(refInput.precomputedLight == nullptr);
      assert(refService.submitChunk(std::move(refInput)) != 0);
    }
    for (const auto &req : batch) {
      assert(refService.requestChunkCompute(req.pos.x, req.pos.z) != 0);
    }
    refService.flushAndWait();
    refService.drainPublished();

    const int yBegin = (minSectionY - 1) * 16;
    const int yEnd = (minSectionY + sectionCount + 1) * 16;
    for (int cz = -1; cz <= 1; ++cz) {
      for (int cx = -1; cx <= 1; ++cx) {
        const auto checkLayer = [&](LightLayer layer) {
          const auto parSnap =
              world.lightService()->latestLayerChunk(layer, cx, cz);
          const auto refSnap = refService.latestLayerChunk(layer, cx, cz);
          assert(parSnap != nullptr);
          assert(refSnap != nullptr);
          const auto layerIdx = static_cast<std::size_t>(layer);
          for (int secY = minSectionY - 1; secY <= minSectionY + sectionCount;
               ++secY) {
            const LightSectionSnapshot *parSec = parSnap->section(secY);
            const LightSectionSnapshot *refSec = refSnap->section(secY);
            assert(parSec != nullptr && refSec != nullptr);
            if (parSec->layers[layerIdx].storage() !=
                    refSec->layers[layerIdx].storage() ||
                parSec->layers[layerIdx].uniformValue() !=
                    refSec->layers[layerIdx].uniformValue()) {
              std::cerr << "Storage mismatch dim=" << noiseId << " layer="
                        << static_cast<int>(layer) << " chunk=(" << cx << ","
                        << cz << ") secY=" << secY << "\n";
              assert(false);
            }
          }
          const int bx0 = cx * 16;
          const int bz0 = cz * 16;
          for (int y = yBegin; y < yEnd; ++y) {
            for (int lz = 0; lz < 16; ++lz) {
              for (int lx = 0; lx < 16; ++lx) {
                const int pv =
                    parSnap->lightValue(layer, bx0 + lx, y, bz0 + lz);
                const int rv =
                    refSnap->lightValue(layer, bx0 + lx, y, bz0 + lz);
                if (pv != rv) {
                  std::cerr << "Voxel mismatch dim=" << noiseId << " layer="
                            << static_cast<int>(layer) << " pos=(" << (bx0 + lx)
                            << "," << y << "," << (bz0 + lz) << ") par=" << pv
                            << " ref=" << rv << "\n";
                  assert(false);
                }
              }
            }
          }
        };
        checkLayer(LightLayer::Block);
        if (dim.hasSkyLight)
          checkLayer(LightLayer::Sky);
      }
    }
  };

  runParityCase(overworldDimensionType(), "minecraft:overworld",
                std::make_shared<MultiNoiseBiomeSource>(
                    MultiNoiseBiomeSource::createOverworld()),
                123456LL);
  runParityCase(netherDimensionType(), "minecraft:nether",
                std::make_shared<MultiNoiseBiomeSource>(
                    MultiNoiseBiomeSource::createNether()),
                654321LL);
}

void testGenerationUsersProtoRetentionAndStructures(
    const WorldgenRegistries &reg) {
  constexpr std::int64_t kSeed = 123456LL;
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  assert(owSettings != nullptr);
  auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createOverworld());

  NoiseBasedChunkGenerator refGen(reg, *owSettings, owBiomes);
  ChunkGenerationPipeline refPipe(refGen, kSeed);
  const auto &refFeatures = refPipe.ensureStatus({0, 0}, ChunkStatusId::Features);
  const std::uint64_t refFeaturesHash = hashChunkBlocksAndBiomes(refFeatures);

  struct MemoryRawStore final : RawChunkStore {
    StoreReadResult readChunk(const ChunkTicketPos &position) override {
      std::lock_guard lock(mutex);
      ++readCalls;
      auto it = map.find(position);
      if (it == map.end())
        return {StoreReadStatus::Missing, {}, "missing"};
      return {StoreReadStatus::Found, it->second, {}};
    }
    StoreWriteResult writeChunk(const ChunkTicketPos &position,
                                std::span<const std::uint8_t> bytes,
                                int) override {
      std::lock_guard lock(mutex);
      ++writeCalls;
      map[position].assign(bytes.begin(), bytes.end());
      return {true, false, {}};
    }
    std::mutex mutex;
    int readCalls = 0;
    int writeCalls = 0;
    std::map<ChunkTicketPos, std::vector<std::uint8_t>> map;
  };

  auto rawStore = std::make_shared<MemoryRawStore>();
  auto cachedStore = std::make_shared<SerializedChunkStore>(rawStore, 128);

  NoiseBasedChunkGenerator mtGen(reg, *owSettings, owBiomes);
  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 4;
  cfg.asyncWorldGenLight = false;
  World world;
  WorldGenScheduler sched(mtGen, kSeed, cachedStore.get(), &world, nullptr,
                          nullptr, cfg);
  ChunkMap &chunkMap = world.chunkMap();

  // Single-chunk Features parity (before neighbor Features execution order diverges in Light/Full).
  BaseChunk &mtChunk = sched.ensureStatus({0, 0}, ChunkStatusId::Features);
  assert(mtChunk.generationUserCount() == 0);
  assert(refFeaturesHash == hashChunkBlocksAndBiomes(mtChunk));
  assert(chunkPersistedStatus(sched.ensureStatus({0, 0}, ChunkStatusId::Full)) ==
         ChunkStatusId::Full);
  assert(mtChunk.generationUserCount() == 0);

  // Unfinished target ProtoChunk (currentStage < effectiveTarget) cannot be evicted.
  auto unfinishedHolder = chunkMap.getOrCreateHolder(ChunkTicketPos{50, 50});
  unfinishedHolder->setProto(std::make_shared<ProtoChunk>(
      ChunkTicketPos{50, 50}, mtGen.chunkMinY(), mtGen.chunkHeight()));
  {
    auto &ugen = unfinishedHolder->ensureGenState();
    std::lock_guard lock(ugen.mtx);
    ugen.currentStage = static_cast<std::uint8_t>(ChunkStatusId::Carvers);
    ugen.explicitTarget = static_cast<std::uint8_t>(ChunkStatusId::Full);
  }
  // generationUserCount() > 0 also prevents eviction.
  auto pinnedHolder = chunkMap.findHolder({0, 0});
  assert(pinnedHolder != nullptr);
  mtChunk.addGenerationUser();
  assert(mtChunk.generationUserCount() == 1);
  assert(sched.evictIdleOutsideRadius({}, 0) >= 20);
  assert(chunkMap.findHolder({0, 0}) != nullptr && !pinnedHolder->evicted());
  assert(chunkMap.findHolder({50, 50}) != nullptr &&
         !unfinishedHolder->evicted());
  mtChunk.releaseGenerationUser();
  assert(mtChunk.generationUserCount() == 0);
  chunkMap.eraseHolderIf(unfinishedHolder->key(), [&](ChunkHolder &h) {
    return &h == unfinishedHolder.get();
  });

  // Also verify WorldChunk inherits BaseChunk generationUsers_ tracking cleanly.
  assert(sched.generateAndInstallIntoWorld({0, 0}, world));
  auto worldHolder = chunkMap.findHolder({0, 0});
  assert(worldHolder != nullptr &&
         worldHolder->heldKind() == HeldChunkKind::World);
  auto worldChunk = worldHolder->chunkShared();
  assert(worldChunk != nullptr && worldChunk->generationUserCount() == 0);
  worldChunk->addGenerationUser();
  assert(worldChunk->generationUserCount() == 1);
  worldChunk->releaseGenerationUser();
  assert(worldChunk->generationUserCount() == 0);

  // Partial ProtoChunk with generationUserCount == 0 evicts to SerializedChunkStore
  // and reloads cleanly via SerializedChunkStore memory queue on next generation.
  BaseChunk &partialChunk =
      sched.ensureStatus({10, 10}, ChunkStatusId::Carvers);
  assert(partialChunk.generationUserCount() == 0);
  const std::size_t evictedPartial = sched.evictIdleOutsideRadius({}, 0);
  assert(evictedPartial >= 1);
  cachedStore->flushPending();
  {
    std::lock_guard lock(rawStore->mutex);
    assert(rawStore->writeCalls >= 1);
  }
  const int readsBeforeReload = [&] {
    std::lock_guard lock(rawStore->mutex);
    return rawStore->readCalls;
  }();
  BaseChunk &resumedChunk = sched.ensureStatus({10, 10}, ChunkStatusId::Full);
  assert(chunkPersistedStatus(resumedChunk) == ChunkStatusId::Full);
  // Resuming {10, 10} itself hits SerializedChunkStore's in-memory cache rather than disk.
  const StoreReadResult cachedCheck = cachedStore->readChunk({10, 10});
  assert(cachedCheck.status == StoreReadStatus::Found);
  {
    std::lock_guard lock(rawStore->mutex);
    assert(rawStore->readCalls >= readsBeforeReload);
  }
}

class SpatialProbeGenerator final : public ChunkGenerator {
public:
  int minY() const noexcept override { return -64; }
  int height() const noexcept override { return 384; }

  std::atomic<int> spatialViolations{0};
  std::atomic<int> maxConcurrent{0};
  std::atomic<int> featuresCalls{0};
  std::atomic<bool> crossColorConcurrency{false};

  void fillFromNoise(WorldGenRegion &, ProtoChunk &chunk,
                     std::int64_t) override {
    const int bx = chunk.pos().x << 4;
    const int bz = chunk.pos().z << 4;
    const BlockState bedrock("minecraft:bedrock");
    const BlockState stone("minecraft:stone");
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        chunk.setBlockState(bx + lx, -64, bz + lz, bedrock, 3);
        chunk.setBlockState(bx + lx, 0, bz + lz, stone, 3);
      }
    }
  }

  void applyBiomeDecoration(WorldGenRegion &region, ProtoChunk &chunk,
                            std::int64_t) override {
    const ChunkTicketPos pos = chunk.pos();
    auto color9 = [](ChunkTicketPos p) {
      return ((p.x % 3 + 3) % 3) * 3 + ((p.z % 3 + 3) % 3);
    };
    const int myColor = color9(pos);
    {
      std::lock_guard lock(activeMutex_);
      for (const ChunkTicketPos &other : active_) {
        const int dist =
            std::max(std::abs(pos.x - other.x), std::abs(pos.z - other.z));
        if (dist <= 2) {
          spatialViolations.fetch_add(1, std::memory_order_relaxed);
        } else if (color9(other) != myColor) {
          crossColorConcurrency.store(true, std::memory_order_relaxed);
        }
      }
      active_.push_back(pos);
      const int cur = static_cast<int>(active_.size());
      int prevMax = maxConcurrent.load(std::memory_order_relaxed);
      while (cur > prevMax &&
             !maxConcurrent.compare_exchange_weak(prevMax, cur)) {
      }
    }

    // Exercise 3x3 write neighborhood safely while holding spatial reservation.
    const int bx = pos.x << 4;
    const int bz = pos.z << 4;
    const BlockState gold("minecraft:gold_block");
    region.setBlockState(bx + 1, 1, bz + 1, gold, 3);
    region.setBlockState(bx - 1, 1, bz - 1, gold, 3);
    region.setBlockState(bx + 16, 1, bz + 16, gold, 3);

    std::this_thread::sleep_for(std::chrono::milliseconds(15));

    {
      std::lock_guard lock(activeMutex_);
      auto it = std::find(active_.begin(), active_.end(), pos);
      if (it != active_.end())
        active_.erase(it);
    }
    featuresCalls.fetch_add(1, std::memory_order_relaxed);
  }

private:
  std::mutex activeMutex_;
  std::vector<ChunkTicketPos> active_;
};

void testSpatial3x3MutualExclusionAndMultiColorParallelism() {
  SpatialProbeGenerator gen;
  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 8;
  WorldGenScheduler sched(gen, 112233LL, nullptr, nullptr, nullptr, nullptr,
                          cfg);

  std::vector<ChunkBatchRequest> batch;
  batch.reserve(64);
  for (int x = 0; x < 8; ++x) {
    for (int z = 0; z < 8; ++z) {
      batch.push_back(ChunkBatchRequest{{x, z}, ChunkStatusId::Features});
    }
  }
  sched.ensureStatusBatch(batch, {3, 3});

  assert(gen.featuresCalls.load() == 64);
  assert(gen.spatialViolations.load() == 0);
  assert(gen.maxConcurrent.load() >= 2);
  assert(gen.crossColorConcurrency.load());
}

class InlineProbeGenerator final : public ChunkGenerator {
public:
  int minY() const noexcept override { return -64; }
  int height() const noexcept override { return 384; }

  void fillFromNoise(WorldGenRegion &, ProtoChunk &chunk,
                     std::int64_t) override {
    const int bx = chunk.pos().x << 4;
    const int bz = chunk.pos().z << 4;
    const BlockState bedrock("minecraft:bedrock");
    const BlockState stone("minecraft:stone");
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        chunk.setBlockState(bx + lx, -64, bz + lz, bedrock, 3);
        chunk.setBlockState(bx + lx, 64, bz + lz, stone, 3);
      }
    }
    std::lock_guard lock(mtx_);
    fillThreads_[chunk.pos()] = std::this_thread::get_id();
  }

  void buildSurface(WorldGenRegion &, ProtoChunk &chunk,
                    std::int64_t) override {
    std::lock_guard lock(mtx_);
    surfaceThreads_[chunk.pos()] = std::this_thread::get_id();
  }

  void applyCarvers(WorldGenRegion &, ProtoChunk &chunk,
                    std::int64_t) override {
    std::lock_guard lock(mtx_);
    carverThreads_[chunk.pos()] = std::this_thread::get_id();
  }

  void verifyNoiseSurfaceCarversSameThread() const {
    std::lock_guard lock(mtx_);
    assert(!carverThreads_.empty());
    for (const auto &[pos, carverTid] : carverThreads_) {
      const auto fillIt = fillThreads_.find(pos);
      const auto surfIt = surfaceThreads_.find(pos);
      assert(fillIt != fillThreads_.end());
      assert(surfIt != surfaceThreads_.end());
      assert(fillIt->second == surfIt->second);
      assert(surfIt->second == carverTid);
    }
  }

private:
  mutable std::mutex mtx_;
  std::map<ChunkTicketPos, std::thread::id> fillThreads_;
  std::map<ChunkTicketPos, std::thread::id> surfaceThreads_;
  std::map<ChunkTicketPos, std::thread::id> carverThreads_;
};

void testFourDispatchInlineChaining(const WorldgenRegistries &reg) {
  {
    InlineProbeGenerator probeGen;
    WorldGenSchedulerConfig cfg;
    cfg.threadCount = 4;
    cfg.asyncWorldGenLight = false;
    WorldGenScheduler sched(probeGen, 445566LL, nullptr, nullptr, nullptr,
                            nullptr, cfg);
    std::vector<ChunkBatchRequest> batch;
    for (int x = 0; x < 6; ++x) {
      for (int z = 0; z < 6; ++z) {
        batch.push_back(ChunkBatchRequest{{x, z}, ChunkStatusId::Full});
      }
    }
    sched.ensureStatusBatch(batch, {2, 2});
    probeGen.verifyNoiseSurfaceCarversSameThread();

    const WorldGenSchedulerStats st = sched.stats();
    assert(st.chunksCreated > 0);
    assert(st.inlineSteps > 0);
    // V2 runs StructureStarts, Biomes, Noise->Surface->Carvers, Features->InitLight,
    // and Light->Spawn->Full in ~4 parallel dispatches (+1 colored) per Full chunk,
    // and only 1..3 for halo chunks, keeping parallelDispatches well below 6 per chunk.
    assert(st.parallelDispatches < 6 * st.chunksCreated);
    assert(st.parallelDispatches < 4 * st.chunksCreated);
  }

  // Also verify with real NoiseBasedChunkGenerator that Full produces valid terrain & heightmaps.
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  assert(owSettings != nullptr);
  auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createOverworld());
  NoiseBasedChunkGenerator realGen(reg, *owSettings, owBiomes);
  realGen.setConcentricRingsEnabled(false);

  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 8;
  WorldGenScheduler sched(realGen, 778899LL, nullptr, nullptr, nullptr, nullptr,
                          cfg);
  std::vector<ChunkBatchRequest> batch;
  for (int x = 0; x < 4; ++x) {
    for (int z = 0; z < 4; ++z) {
      batch.push_back(ChunkBatchRequest{{x, z}, ChunkStatusId::Full});
    }
  }
  sched.ensureStatusBatch(batch, {1, 1});

  for (const ChunkBatchRequest &req : batch) {
    BaseChunk *c = sched.findChunk(req.pos);
    assert(c != nullptr);
    assert(chunkPersistedStatus(*c) == ChunkStatusId::Full);
    assert(chunkHeight(*c, HeightmapType::WorldSurface, 8, 8) > c->minY());
    assert(chunkHeight(*c, HeightmapType::MotionBlocking, 8, 8) > c->minY());
    assert(chunkHeight(*c, HeightmapType::OceanFloor, 8, 8) > c->minY());
    const int bx = req.pos.x << 4;
    const int bz = req.pos.z << 4;
    assert(c->getBlockState(bx, c->minY(), bz).name() == "minecraft:bedrock");
  }
  const WorldGenSchedulerStats st = sched.stats();
  assert(st.inlineSteps > 0);
  assert(st.parallelDispatches < 6 * st.chunksCreated);
}

class StreamingDelayGenerator final : public ChunkGenerator {
public:
  int minY() const noexcept override { return -64; }
  int height() const noexcept override { return 384; }

  std::atomic<int> delayMs{8};

  void fillFromNoise(WorldGenRegion &, ProtoChunk &chunk,
                     std::int64_t) override {
    const int d = delayMs.load(std::memory_order_relaxed);
    if (d > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(d));
    const int bx = chunk.pos().x << 4;
    const int bz = chunk.pos().z << 4;
    const BlockState bedrock("minecraft:bedrock");
    const BlockState stone("minecraft:stone");
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        chunk.setBlockState(bx + lx, -64, bz + lz, bedrock, 3);
        chunk.setBlockState(bx + lx, 64, bz + lz, stone, 3);
      }
    }
  }
};

void testStreamingLowLatencyAndCancellation() {
  // Part A: Incremental low-latency completions before the full batch finishes.
  {
    StreamingDelayGenerator gen;
    gen.delayMs.store(10);
    WorldGenSchedulerConfig cfg;
    cfg.threadCount = 4;
    cfg.asyncWorldGenLight = false;
    WorldGenScheduler sched(gen, 4242LL, nullptr, nullptr, nullptr, nullptr,
                            cfg);
    const std::array<ChunkTicketPos, 1> center{{{0, 0}}};
    sched.updatePriorityCenters(center, 12);

    std::uint64_t epoch = 1;
    for (int x = -2; x <= 2; ++x) {
      for (int z = -2; z <= 2; ++z) {
        sched.submitDemand(DemandRequest{
            ChunkTicketPos{x, z}, ChunkStatusId::Full, true, true, epoch++, 0});
      }
    }

    std::size_t completedCount = 0;
    bool sawIncremental = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (completedCount < 25) {
      auto drained = sched.drainCompletedResidency();
      for (const CompletedResidency &item : drained) {
        assert(item.ok);
        assert(item.proto != nullptr);
        ++completedCount;
      }
      if (completedCount > 0 && completedCount < 25)
        sawIncremental = true;
      if (completedCount >= 25)
        break;
      assert(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(sawIncremental);

    const StreamingDemandStats sStats = sched.streamingStats();
    assert(sStats.requested == 25);
    assert(sStats.completed == 25);
    assert(sStats.failed == 0);
    assert(sStats.cancelled == 0);

    const WorldGenSchedulerStats wStats = sched.stats();
    assert(wStats.completedTargets >= 25);
    assert(wStats.latencyPercentileMs(0.50) > 0.0);
    assert(wStats.latencyPercentileMs(0.95) >= wStats.latencyPercentileMs(0.50));
    sched.shutdownStreaming();
  }

  // Part B: Cancellation of queued/in-flight demands & exact stats reconciliation.
  {
    StreamingDelayGenerator gen;
    gen.delayMs.store(25);
    WorldGenSchedulerConfig cfg;
    cfg.threadCount = 2;
    WorldGenScheduler sched(gen, 4243LL, nullptr, nullptr, nullptr, nullptr,
                            cfg);
    const std::array<ChunkTicketPos, 1> center{{{102, 102}}};
    sched.updatePriorityCenters(center, 14);

    std::uint64_t epoch = 1;
    std::vector<ChunkTicketPos> toCancel;
    for (int x = 100; x <= 105; ++x) {
      for (int z = 100; z <= 105; ++z) {
        const ChunkTicketPos p{x, z};
        sched.submitDemand(
            DemandRequest{p, ChunkStatusId::Full, true, true, epoch++, 0});
        if (z >= 102)
          toCancel.push_back(p);
      }
    }

    // Cancel 24 of the 36 demands immediately while work is queued/running.
    for (const ChunkTicketPos &p : toCancel) {
      sched.cancelDemand(p, true);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    StreamingDemandStats sStats{};
    for (;;) {
      (void)sched.drainCompletedResidency();
      sStats = sched.streamingStats();
      if (sStats.requested == 36 &&
          sStats.requested ==
              sStats.completed + sStats.failed + sStats.cancelled) {
        break;
      }
      assert(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(sStats.requested == 36);
    assert(sStats.cancelled > 0);
    assert(sStats.failed == 0);
    assert(sStats.requested ==
           sStats.completed + sStats.failed + sStats.cancelled);
    assert(sched.stats().completedTargets > 0);
    assert(sched.stats().latencyPercentileMs(0.50) > 0.0);
    sched.shutdownStreaming();
  }
}

void testParallelNbtEncodeSafety(const WorldgenRegistries &reg) {
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  assert(owSettings != nullptr);
  auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createOverworld());
  NoiseBasedChunkGenerator gen(reg, *owSettings, owBiomes);
  gen.setConcentricRingsEnabled(false);

  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 8;
  WorldGenScheduler sched(gen, 99887766LL, nullptr, nullptr, nullptr, nullptr,
                          cfg);
  const std::array<ChunkTicketPos, 1> center{{{0, 0}}};
  sched.updatePriorityCenters(center, 16);

  std::uint64_t epoch = 1;
  std::set<ChunkTicketPos> expected;
  for (int x = -2; x <= 3; ++x) {
    for (int z = -2; z <= 3; ++z) {
      const ChunkTicketPos p{x, z};
      expected.insert(p);
      sched.submitDemand(
          DemandRequest{p, ChunkStatusId::Full, true, true, epoch++, 0});
    }
  }

  std::set<ChunkTicketPos> received;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(60);
  while (received.size() < expected.size()) {
    auto drained = sched.drainCompletedResidency();
    for (CompletedResidency &item : drained) {
      assert(item.ok);
      assert(item.proto != nullptr);
      assert(item.root.value.type != 0);
      const auto *statusTag = item.root.value.get("Status");
      assert(statusTag != nullptr && statusTag->text == "minecraft:full");
      const auto *xTag = item.root.value.get("xPos");
      const auto *zTag = item.root.value.get("zPos");
      assert(xTag != nullptr && xTag->number == item.pos.x);
      assert(zTag != nullptr && zTag->number == item.pos.z);

      std::string err;
      assert(terrain::validateChunkRootForSave(item.root, err, true));

      const std::vector<std::uint8_t> encoded = nbt::writeRoot(item.root);
      terrain::DecodeOptions decodeOpts;
      decodeOpts.allowUnimplementedBlocks = true;
      const terrain::DecodedChunk decoded =
          terrain::decodeChunkNbt(encoded, decodeOpts);
      assert(decoded.status == terrain::DecodeStatus::Ok);
      assert(decoded.statusName == "minecraft:full");
      assert(decoded.chunkX == item.pos.x && decoded.chunkZ == item.pos.z);

      assert(received.insert(item.pos).second);
    }
    if (received.size() >= expected.size())
      break;
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  assert(received == expected);
  const StreamingDemandStats sStats = sched.streamingStats();
  assert(sStats.requested == expected.size());
  assert(sStats.completed == expected.size());
  assert(sStats.failed == 0);
  assert(sStats.cancelled == 0);
  sched.shutdownStreaming();
}

class GatedNoiseGenerator final : public ChunkGenerator {
public:
  GatedNoiseGenerator(ChunkGenerator &inner, ChunkTicketPos gatedPos)
      : inner_(inner), gatedPos_(gatedPos) {}

  int minY() const noexcept override { return inner_.minY(); }
  int height() const noexcept override { return inner_.height(); }
  int chunkMinY() const noexcept override { return inner_.chunkMinY(); }
  int chunkHeight() const noexcept override { return inner_.chunkHeight(); }

  void createStructures(ProtoChunk &chunk, std::int64_t worldSeed) override {
    inner_.createStructures(chunk, worldSeed);
  }
  void createReferences(WorldGenRegion &region, ProtoChunk &chunk) override {
    inner_.createReferences(region, chunk);
  }
  void createBiomes(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t worldSeed) override {
    inner_.createBiomes(region, chunk, worldSeed);
  }
  void fillFromNoise(WorldGenRegion &region, ProtoChunk &chunk,
                     std::int64_t worldSeed) override {
    const ChunkTicketPos pos = chunk.pos();
    if (pos == gatedPos_) {
      const int activeNow =
          activeTargetFill_.fetch_add(1, std::memory_order_acq_rel) + 1;
      int prevMax = maxTargetFillConcurrency_.load(std::memory_order_relaxed);
      while (activeNow > prevMax &&
             !maxTargetFillConcurrency_.compare_exchange_weak(prevMax,
                                                              activeNow)) {
      }
      struct ActiveGuard {
        std::atomic<int> &counter;
        ~ActiveGuard() { counter.fetch_sub(1, std::memory_order_acq_rel); }
      } guard{activeTargetFill_};

      if (!enteredOnce_.exchange(true, std::memory_order_acq_rel)) {
        {
          std::lock_guard<std::mutex> lock(gateMtx_);
          entered_ = true;
        }
        gateCv_.notify_all();
        std::unique_lock<std::mutex> lock(gateMtx_);
        gateCv_.wait(lock, [&]() { return gateReleased_; });
      }
      inner_.fillFromNoise(region, chunk, worldSeed);
      return;
    }
    inner_.fillFromNoise(region, chunk, worldSeed);
  }
  void buildSurface(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t worldSeed) override {
    inner_.buildSurface(region, chunk, worldSeed);
  }
  void applyCarvers(WorldGenRegion &region, ProtoChunk &chunk,
                    std::int64_t worldSeed) override {
    inner_.applyCarvers(region, chunk, worldSeed);
  }
  void applyBiomeDecoration(WorldGenRegion &region, ProtoChunk &chunk,
                            std::int64_t worldSeed) override {
    inner_.applyBiomeDecoration(region, chunk, worldSeed);
  }
  void spawnOriginalMobs(WorldGenRegion &region, ProtoChunk &chunk,
                         std::int64_t worldSeed) override {
    inner_.spawnOriginalMobs(region, chunk, worldSeed);
  }

  void waitUntilEntered() {
    std::unique_lock<std::mutex> lock(gateMtx_);
    const bool ok = gateCv_.wait_for(lock, std::chrono::seconds(15),
                                     [&]() { return entered_; });
    assert(ok);
  }

  void releaseGate() {
    {
      std::lock_guard<std::mutex> lock(gateMtx_);
      gateReleased_ = true;
    }
    gateCv_.notify_all();
  }

  int maxTargetFillConcurrency() const noexcept {
    return maxTargetFillConcurrency_.load(std::memory_order_acquire);
  }

private:
  ChunkGenerator &inner_;
  ChunkTicketPos gatedPos_;
  std::atomic<int> activeTargetFill_{0};
  std::atomic<int> maxTargetFillConcurrency_{0};
  std::atomic<bool> enteredOnce_{false};
  std::mutex gateMtx_;
  std::condition_variable gateCv_;
  bool entered_ = false;
  bool gateReleased_ = false;
};

void testCancelAndRearmWhileRunningDoesNotRace(const WorldgenRegistries &reg) {
  const auto *owSettings = reg.findNoiseSettings("minecraft:overworld");
  assert(owSettings != nullptr);
  auto owBiomes = std::make_shared<MultiNoiseBiomeSource>(
      MultiNoiseBiomeSource::createOverworld());

  // Part A: Deterministic gated cancel + re-submit while ExecState::Running.
  {
    NoiseBasedChunkGenerator baseGen(reg, *owSettings, owBiomes);
    baseGen.setConcentricRingsEnabled(false);
    GatedNoiseGenerator gatedGen(baseGen, ChunkTicketPos{0, 0});

    WorldGenSchedulerConfig cfg;
    cfg.threadCount = 4;
    cfg.asyncWorldGenLight = false;
    WorldGenScheduler sched(gatedGen, 123456LL, nullptr, nullptr, nullptr,
                            nullptr, cfg);
    const std::array<ChunkTicketPos, 1> center{{{0, 0}}};
    sched.updatePriorityCenters(center, 12);

    sched.submitDemand(
        DemandRequest{{0, 0}, ChunkStatusId::Full, true, true, 1, 0});
    gatedGen.waitUntilEntered();

    auto holder = sched.chunkMap().findHolder({0, 0});
    assert(holder != nullptr);
    auto *genState = holder->genState();
    assert(genState != nullptr);
    {
      std::lock_guard<std::mutex> lock(genState->mtx);
      assert(genState->execState == ChunkHolder::ExecState::Running);
    }

    // Cancel epoch 1 while worker is blocked inside fillFromNoise for (0, 0).
    sched.cancelDemand({0, 0}, true);
    const auto cancelDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(genState->mtx);
        if (genState->explicitTarget == 0) {
          assert(genState->execState == ChunkHolder::ExecState::Running);
          break;
        }
      }
      assert(std::chrono::steady_clock::now() < cancelDeadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Immediately re-submit epoch 2 while the first worker is still Running.
    sched.submitDemand(
        DemandRequest{{0, 0}, ChunkStatusId::Full, true, true, 2, 0});
    const auto rearmDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
      {
        std::lock_guard<std::mutex> lock(genState->mtx);
        if (genState->explicitTarget ==
            static_cast<std::uint8_t>(ChunkStatusId::Full)) {
          assert(genState->execState == ChunkHolder::ExecState::Running);
          break;
        }
      }
      assert(std::chrono::steady_clock::now() < rearmDeadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Ensure no second worker entered fillFromNoise concurrently for (0, 0).
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(gatedGen.maxTargetFillConcurrency() == 1);

    gatedGen.releaseGate();

    bool gotEpoch2 = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!gotEpoch2) {
      for (CompletedResidency &item : sched.drainCompletedResidency()) {
        assert(item.ok);
        assert(item.pos == ChunkTicketPos(0, 0));
        assert(item.epoch == 2);
        assert(item.proto != nullptr);
        assert(chunkPersistedStatus(*item.proto) == ChunkStatusId::Full);
        gotEpoch2 = true;
      }
      if (gotEpoch2)
        break;
      assert(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    assert(gatedGen.maxTargetFillConcurrency() == 1);
    assert(sched.lastError().empty());
    const StreamingDemandStats sStats = sched.streamingStats();
    assert(sStats.requested == 2);
    assert(sStats.completed == 1);
    assert(sStats.failed == 0);
    assert(sStats.cancelled == 1);
    assert(sStats.requested ==
           sStats.completed + sStats.failed + sStats.cancelled);
    sched.shutdownStreaming();
  }

  // Part B: Rapid flight cancel + re-submit stress across x = 0 -> -40.
  {
    NoiseBasedChunkGenerator realGen(reg, *owSettings, owBiomes);
    realGen.setConcentricRingsEnabled(false);

    WorldGenSchedulerConfig cfg;
    cfg.threadCount = 8;
    WorldGenScheduler sched(realGen, 123456LL, nullptr, nullptr, nullptr,
                            nullptr, cfg);

    constexpr int kRadius = 6;
    std::uint64_t nextEpoch = 1;
    std::map<ChunkTicketPos, std::uint64_t> activeView;
    std::set<ChunkTicketPos> completedFinal;

    auto stepFlightTo = [&](int centerX, bool thrashFrontier) {
      const std::array<ChunkTicketPos, 1> centers{{{centerX, 0}}};
      sched.updatePriorityCenters(centers, kRadius + 4);

      std::set<ChunkTicketPos> desired;
      for (int dx = -kRadius; dx <= kRadius; ++dx) {
        for (int dz = -kRadius; dz <= kRadius; ++dz) {
          desired.insert(ChunkTicketPos{centerX + dx, dz});
        }
      }

      for (auto it = activeView.begin(); it != activeView.end();) {
        if (!desired.contains(it->first)) {
          sched.cancelDemand(it->first, true, it->second);
          completedFinal.erase(it->first);
          it = activeView.erase(it);
        } else {
          ++it;
        }
      }

      for (const ChunkTicketPos &pos : desired) {
        if (!activeView.contains(pos)) {
          const std::uint64_t ep = nextEpoch++;
          activeView[pos] = ep;
          sched.submitDemand(
              DemandRequest{pos, ChunkStatusId::Full, true, true, ep, 0});
        } else if (thrashFrontier && pos.z >= -2 && pos.z <= 2) {
          // Repeatedly cancel and re-submit in-flight chunks along the corridor
          // (including crossing (-32, 0)) to exercise Running cancel+rearm.
          const std::uint64_t oldEp = activeView[pos];
          sched.cancelDemand(pos, true, oldEp);
          completedFinal.erase(pos);
          const std::uint64_t newEp = nextEpoch++;
          activeView[pos] = newEp;
          sched.submitDemand(
              DemandRequest{pos, ChunkStatusId::Full, true, true, newEp, 0});
        }
      }

      for (CompletedResidency &item : sched.drainCompletedResidency()) {
        assert(item.ok);
        auto it = activeView.find(item.pos);
        if (it != activeView.end() && it->second == item.epoch) {
          assert(item.proto != nullptr);
          assert(chunkPersistedStatus(*item.proto) == ChunkStatusId::Full);
          completedFinal.insert(item.pos);
        }
      }
    };

    for (int cx = 0; cx >= -40; --cx) {
      stepFlightTo(cx, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    // Settle at final view centered at (-40, 0) without further cancellations.
    std::set<ChunkTicketPos> finalDesired;
    for (int dx = -kRadius; dx <= kRadius; ++dx) {
      for (int dz = -kRadius; dz <= kRadius; ++dz) {
        finalDesired.insert(ChunkTicketPos{-40 + dx, dz});
      }
    }
    // Recompute completedFinal strictly for finalDesired from activeView + drain.
    std::set<ChunkTicketPos> verifiedFinal;
    for (const ChunkTicketPos &pos : finalDesired) {
      if (completedFinal.contains(pos))
        verifiedFinal.insert(pos);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (verifiedFinal.size() < finalDesired.size()) {
      for (CompletedResidency &item : sched.drainCompletedResidency()) {
        assert(item.ok);
        auto it = activeView.find(item.pos);
        if (it != activeView.end() && it->second == item.epoch &&
            finalDesired.contains(item.pos)) {
          assert(item.proto != nullptr);
          assert(chunkPersistedStatus(*item.proto) == ChunkStatusId::Full);
          verifiedFinal.insert(item.pos);
        }
      }
      if (verifiedFinal.size() >= finalDesired.size())
        break;
      assert(std::chrono::steady_clock::now() < deadline);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    assert(verifiedFinal == finalDesired);
    assert(sched.lastError().empty());
    const StreamingDemandStats sStats = sched.streamingStats();
    assert(sStats.failed == 0);
    assert(sStats.cancelled > 0);
    assert(sStats.requested ==
           sStats.completed + sStats.failed + sStats.cancelled);
    sched.shutdownStreaming();
  }
}

class ChurnTestGenerator final : public ChunkGenerator {
public:
  int minY() const noexcept override { return -64; }
  int height() const noexcept override { return 384; }

  void fillFromNoise(WorldGenRegion &, ProtoChunk &chunk,
                     std::int64_t) override {
    const int bx = chunk.pos().x << 4;
    const int bz = chunk.pos().z << 4;
    const BlockState bedrock("minecraft:bedrock");
    const BlockState stone("minecraft:stone");
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        chunk.setBlockState(bx + lx, -64, bz + lz, bedrock, 3);
        chunk.setBlockState(bx + lx, 64, bz + lz, stone, 3);
      }
    }
  }

  void applyBiomeDecoration(WorldGenRegion &region, ProtoChunk &chunk,
                            std::int64_t) override {
    const int bx = chunk.pos().x << 4;
    const int bz = chunk.pos().z << 4;
    const BlockState gold("minecraft:gold_block");
    region.setBlockState(bx + 1, 65, bz + 1, gold, 3);
  }
};

void testArmHoldersCancelRearmNoDeadlock() {
  ChurnTestGenerator gen;
  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 8;
  WorldGenScheduler sched(gen, 55667788LL, nullptr, nullptr, nullptr, nullptr,
                          cfg);

  std::set<ChunkTicketPos> touched;
  std::uint64_t nextEpoch = 1;
  constexpr int kChurnCycles = 350;

  for (int cycle = 0; cycle < kChurnCycles; ++cycle) {
    const int cx = (cycle % 7) - 3;
    const int cz = ((cycle / 7) % 7) - 3;
    const std::array<ChunkTicketPos, 1> centers{{{cx, cz}}};
    sched.updatePriorityCenters(centers, 16);

    // Submit an 11x11 overlapping grid mixing residency Full and buffer demands.
    for (int dx = -5; dx <= 5; ++dx) {
      for (int dz = -5; dz <= 5; ++dz) {
        const ChunkTicketPos pos{cx + dx, cz + dz};
        touched.insert(pos);
        const bool isResidency = ((dx + dz + cycle) & 1) == 0;
        const ChunkStatusId target =
            isResidency ? ChunkStatusId::Full
                        : (((cycle + dx) & 1) == 0 ? ChunkStatusId::Features
                                                   : ChunkStatusId::Carvers);
        sched.submitDemand(DemandRequest{pos, target, isResidency, true,
                                         isResidency ? nextEpoch++ : 0, 0});
      }
    }

    // Yield briefly so workers enter armHoldersIterative while cancellations fire.
    if ((cycle & 3) == 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(150));
    } else {
      std::this_thread::yield();
    }

    // Cancel a shifting subset of the 11x11 grid and immediately re-submit half of them.
    for (int dx = -5; dx <= 5; ++dx) {
      for (int dz = -5; dz <= 5; ++dz) {
        if (((dx * 3 + dz * 5 + cycle) & 3) == 0)
          continue;
        const ChunkTicketPos pos{cx + dx, cz + dz};
        sched.cancelDemand(pos, true);
        if (((dx + dz + cycle) & 3) == 1) {
          sched.submitDemand(DemandRequest{pos, ChunkStatusId::Full, true, true,
                                           nextEpoch++, 0});
        }
      }
    }

    for (const CompletedResidency &item : sched.drainCompletedResidency()) {
      assert(item.ok);
    }
  }

  // Cancel all touched coordinates and wait for full drain.
  for (const ChunkTicketPos &pos : touched) {
    sched.cancelDemand(pos, true);
  }

  const auto drainDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  StreamingDemandStats churnStats{};
  for (;;) {
    for (const CompletedResidency &item : sched.drainCompletedResidency()) {
      assert(item.ok);
    }
    churnStats = sched.streamingStats();
    bool anyActiveOrWaiting = false;
    sched.chunkMap().holders().for_each(
        [&](const ChunkMap::HolderMap::value_type &entry) {
          if (!entry.second)
            return;
          auto *g = entry.second->genState();
          if (!g)
            return;
          std::lock_guard<std::mutex> lock(g->mtx);
          if (g->execState != ChunkHolder::ExecState::Idle || g->isActive ||
              g->arming ||
              g->pendingDeps.load(std::memory_order_acquire) != 0) {
            anyActiveOrWaiting = true;
          }
        });
    if (churnStats.requested ==
            churnStats.completed + churnStats.failed + churnStats.cancelled &&
        !anyActiveOrWaiting) {
      break;
    }
    assert(std::chrono::steady_clock::now() < drainDeadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  assert(churnStats.failed == 0);
  assert(churnStats.cancelled > 0);
  assert(sched.lastError().empty());
  sched.chunkMap().holders().for_each(
      [](const ChunkMap::HolderMap::value_type &entry) {
        if (!entry.second)
          return;
        auto *g = entry.second->genState();
        if (!g)
          return;
        std::lock_guard<std::mutex> lock(g->mtx);
        assert(g->execState != ChunkHolder::ExecState::WaitingDeps);
        assert(!g->isActive);
        assert(!g->arming);
        assert(g->pendingDeps.load(std::memory_order_acquire) == 0);
      });

  // Submit a fresh 15x15 (225 chunks) residency batch + 50 far-away chunks (275 total).
  const std::array<ChunkTicketPos, 1> freshCenter{{{107, 107}}};
  sched.updatePriorityCenters(freshCenter, 24);
  std::map<ChunkTicketPos, std::uint64_t> freshExpected;
  for (int x = 100; x <= 114; ++x) {
    for (int z = 100; z <= 114; ++z) {
      const ChunkTicketPos pos{x, z};
      const std::uint64_t ep = nextEpoch++;
      freshExpected.emplace(pos, ep);
      sched.submitDemand(
          DemandRequest{pos, ChunkStatusId::Full, true, true, ep, 0});
    }
  }
  for (int i = 0; i < 50; ++i) {
    const ChunkTicketPos pos{500 + i * 20, 500};
    const std::uint64_t ep = nextEpoch++;
    freshExpected.emplace(pos, ep);
    sched.submitDemand(
        DemandRequest{pos, ChunkStatusId::Full, true, true, ep, 0});
  }
  assert(freshExpected.size() == 275);

  std::set<ChunkTicketPos> freshDone;
  const auto freshDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (freshDone.size() < freshExpected.size()) {
    for (CompletedResidency &item : sched.drainCompletedResidency()) {
      assert(item.ok);
      assert(item.proto != nullptr);
      assert(chunkPersistedStatus(*item.proto) == ChunkStatusId::Full);
      auto it = freshExpected.find(item.pos);
      if (it != freshExpected.end() && it->second == item.epoch) {
        assert(freshDone.insert(item.pos).second);
      }
    }
    if (freshDone.size() >= freshExpected.size())
      break;
    assert(std::chrono::steady_clock::now() < freshDeadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  assert(freshDone.size() == 275);
  assert(sched.lastError().empty());
  const StreamingDemandStats finalStats = sched.streamingStats();
  assert(finalStats.failed == 0);
  assert(finalStats.completed >= 275);
  assert(finalStats.requested ==
         finalStats.completed + finalStats.failed + finalStats.cancelled);
  sched.shutdownStreaming();
}

void testAsyncWorldGenLightNonBlocking(const WorldgenRegistries &reg) {
  constexpr std::int64_t kSeed = -987654321LL;
  const auto *endSettings = reg.findNoiseSettings("minecraft:end");
  assert(endSettings != nullptr);
  auto endBiomes = std::make_shared<TheEndBiomeSource>();
  NoiseBasedChunkGenerator gen(reg, *endSettings, endBiomes,
                               redstone::endDimensionType().minY,
                               redstone::endDimensionType().height);

  World world(0, 256);
  LightWorkerOptions lopts;
  lopts.batchWindowMs = 0;
  world.enableLighting(lopts);

  WorldGenSchedulerConfig cfg;
  cfg.threadCount = 4;
  assert(cfg.asyncWorldGenLight);
  auto lightContext = std::make_shared<LightOpticalContext>();
  lightContext->set(world.lightService()->opticalTable(),
                    world.lightService()->skyLightEnabled());
  GeneratedLightQueue lightQueue;
  WorldGenScheduler sched(gen, kSeed, nullptr, &world, lightContext,
                          &lightQueue, cfg);

  std::vector<ChunkBatchRequest> batch = {
      {{0, 0}, ChunkStatusId::Full},
      {{1, 0}, ChunkStatusId::Full},
      {{0, 1}, ChunkStatusId::Full},
      {{1, 1}, ChunkStatusId::Full},
  };
  sched.ensureStatusBatch(batch, {0, 0});
  assert(lightQueue.size() > 0);
  bool sawSubmit = false;
  bool sawCompute = false;
  for (GeneratedLightWork &work : lightQueue.drain()) {
    if (work.compute) {
      sawCompute = true;
      assert(world.requestGeneratedChunkLightCompute(work.position.x,
                                                     work.position.z) != 0);
    } else {
      sawSubmit = true;
      assert(work.input.completion == LightChunkInput::Completion::Deferred);
      assert(world.submitGeneratedChunkLight(std::move(work.input)) != 0);
    }
  }
  assert(sawSubmit && sawCompute);
  for (const auto &req : batch) {
    BaseChunk *c = sched.findChunk(req.pos);
    assert(c != nullptr);
    assert(chunkPersistedStatus(*c) == ChunkStatusId::Full);
    // 非阻塞模式：worldgen worker 不计算 3x3 初始光照，直接推进到 Full。
    assert(!c->isLightCorrect());
  }
  world.lightService()->flushAndWait();
  assert(world.lightService()->stats().lightQueueEntries > 0);
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
  testAsyncWorldGenLightNonBlocking(reg);
  testParallelInitialLightParityWithLightService(reg);
  testGenerationUsersProtoRetentionAndStructures(reg);
  testSpatial3x3MutualExclusionAndMultiColorParallelism();
  testFourDispatchInlineChaining(reg);
  testStreamingLowLatencyAndCancellation();
  testParallelNbtEncodeSafety(reg);
  testCancelAndRearmWhileRunningDoesNotRace(reg);
  testArmHoldersCancelRearmNoDeadlock();

  std::cout << "worldgen_scheduler_tests passed\n";
  return 0;
}
