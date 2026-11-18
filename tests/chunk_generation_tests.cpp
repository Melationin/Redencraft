#include "redstone/engine.hpp"
#include "redstone/light/service.hpp"
#include "redstone/world_chunk_manager.hpp"
#include "redstone/worldgen.hpp"
#include "redstone/worldgen/chunk_generator.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace redstone;

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

// 纯内存区域存储：记录写入的完整 NBT，供测试用 terrain codec 解码核对。
struct MemoryStore final : RawChunkStore {
  StoreReadResult readChunk(const ChunkTicketPos &position) override {
    std::lock_guard lock(mutex);
    ++reads;
    const auto found = payloads.find(position);
    if (found == payloads.end())
      return {StoreReadStatus::Missing, {}, "generated store is empty"};
    return {StoreReadStatus::Found, found->second, {}};
  }

  StoreWriteResult writeChunk(const ChunkTicketPos &position,
                              std::span<const std::uint8_t> bytes,
                              int) override {
    std::lock_guard lock(mutex);
    ++writes;
    payloads[position].assign(bytes.begin(), bytes.end());
    return {true, false, {}};
  }

  bool hasPayload(const ChunkTicketPos &position) const {
    std::lock_guard lock(mutex);
    return payloads.contains(position);
  }

  std::optional<std::vector<std::uint8_t>>
  payload(const ChunkTicketPos &position) const {
    std::lock_guard lock(mutex);
    const auto found = payloads.find(position);
    if (found == payloads.end())
      return std::nullopt;
    return found->second;
  }

  int writeCount() const {
    std::lock_guard lock(mutex);
    return writes;
  }

  mutable std::mutex mutex;
  std::map<ChunkTicketPos, std::vector<std::uint8_t>> payloads;
  int reads = 0;
  int writes = 0;
};

// 确定性的最小生成器：地表以上放金块、记录一个计划刻，并按需放置水源后处理。
// 只用来证明“票等级 -> 生成 -> 安装 -> 可模拟 -> 落盘”的接线，不冒充 26.1 地形。
class TestGenerator final : public worldgen::ChunkGenerator {
public:
  explicit TestGenerator(bool withFluid = true) : withFluid_(withFluid) {}

  int minY() const noexcept override { return -64; }
  int height() const noexcept override { return 384; }

  std::atomic<int> delayMs{0};

  void fillFromNoise(worldgen::WorldGenRegion &, worldgen::ProtoChunk &chunk,
                     std::int64_t) override {
    if (delayMs.load(std::memory_order_relaxed) > 0) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(delayMs.load(std::memory_order_relaxed)));
    }
    const int bx = chunk.pos().x << 4;
    const int bz = chunk.pos().z << 4;
    const BlockState stone("minecraft:stone");
    const BlockState dirt("minecraft:dirt");
    const BlockState grass("minecraft:grass_block");
    const BlockState bedrock("minecraft:bedrock");
    for (int lx = 0; lx < 16; ++lx) {
      for (int lz = 0; lz < 16; ++lz) {
        chunk.setBlockState(bx + lx, -64, bz + lz, bedrock, 3);
        for (int y = -63; y <= 62; ++y)
          chunk.setBlockState(bx + lx, y, bz + lz, stone, 3);
        chunk.setBlockState(bx + lx, 63, bz + lz, dirt, 3);
        chunk.setBlockState(bx + lx, 64, bz + lz, grass, 3);
      }
    }
    if (withFluid_) {
      // 特征放置的水源：安装后必须由 postLoadProtoChunk 等价路径重新调度流体刻。
      chunk.setBlockState(bx + 2, 65, bz + 2,
                          BlockState("minecraft:water"), 3);
      chunk.markPosForPostprocessing(bx + 2, 65, bz + 2);
    }
  }

  void applyBiomeDecoration(worldgen::WorldGenRegion &,
                            worldgen::ProtoChunk &chunk,
                            std::int64_t) override {
    const int bx = chunk.pos().x << 4;
    const int bz = chunk.pos().z << 4;
    chunk.setBlockState(bx + 5, 70, bz + 5,
                        BlockState("minecraft:diamond_block"), 3);
    chunk.blockTicks().push_back(
        {"minecraft:stone", bx, 64, bz, 2, 0});
  }

private:
  bool withFluid_;
};

EntityChunkDataResult emptyEntityProbe(const ChunkTicketPos &) {
  return EntityChunkDataResult{EntityChunkDataResult::Status::Empty, {}};
}

template <class Predicate>
bool pumpUntil(WorldChunkManager &manager, Predicate predicate,
               int timeoutMs = 20000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  for (;;) {
    manager.pump();
    if (predicate())
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return predicate();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// pump() 不驱动自动保存预算；需要落盘时用真实服务端刻推进。
template <class Predicate>
bool tickUntil(WorldChunkManager &manager, Predicate predicate,
               int timeoutMs = 20000) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  for (;;) {
    manager.serverTick(true, true);
    if (predicate())
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return predicate();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::string decodedBlock(const terrain::DecodedChunk &chunk, int x, int y,
                         int z) {
  const int sectionY = y >> 4;
  for (const terrain::Section &section : chunk.sections) {
    if (section.y != sectionY || !section.blockStates)
      continue;
    const std::size_t cell = (static_cast<std::size_t>(y & 15) << 8) |
                             (static_cast<std::size_t>(z & 15) << 4) |
                             static_cast<std::size_t>(x & 15);
    const std::size_t index = section.blockStates->cell(cell);
    if (index >= section.blockStates->palette.size())
      return {};
    return section.blockStates->palette[index].name;
  }
  return {};
}

// 1) 加载票要求 FULL 且磁盘缺失 -> 生成 -> 普通 install -> 可模拟。
void testGeneratedChunkBecomesSimulatable() {
  World world;
  auto store = std::make_shared<MemoryStore>();
  auto generator = std::make_shared<TestGenerator>();

  WorldChunkManager manager(world, store, 2, 10, emptyEntityProbe, {},
                            generator);
  manager.setAutosaveLimitsForTest({64, 8, 32,
                                    std::chrono::milliseconds{5},
                                    std::chrono::milliseconds{20},
                                    std::chrono::milliseconds{20}});
  require(manager.chunkGenerationEnabled(), "generation must be enabled");
  manager.addPlayer(1, {0, 0});
  if (!pumpUntil(manager, [&] { return world.isChunkLoaded(0, 0); }, 8000)) {
    const ChunkGenerationStats gen = manager.generationStats();
    std::cerr << "DIAG requested=" << gen.requested
              << " completed=" << gen.completed << " failed=" << gen.failed
              << " buffer=" << gen.bufferRequests << "/"
              << gen.bufferCompleted
              << " cached=" << gen.cachedChunks << std::endl;
    const auto lifecycle = manager.lifecycleStatus({0, 0});
    if (lifecycle)
      std::cerr << "DIAG lifecycle state="
                << static_cast<int>(lifecycle->state)
                << " failure=" << static_cast<int>(lifecycle->failure)
                << " detail=" << lifecycle->detail << std::endl;
    const auto info = manager.managedStatus({0, 0});
    if (info)
      std::cerr << "DIAG managed error=" << info->error
                << " blocker=" << info->blocker << std::endl;
    require(false, "center chunk generated and installed");
  }
  // 加载生命周期跨 Proto -> World 必须保持不变。
  const auto centerLifecycle = manager.lifecycleStatus({0, 0});
  require(centerLifecycle && centerLifecycle->epoch != 0,
          "controller assigned a load lifecycle epoch");
  require(world.chunkLoadEpochForTest(0, 0) == centerLifecycle->epoch,
          "Proto -> World load epoch preserved");
  require(world.chunkLightEpochForTest(0, 0) == centerLifecycle->epoch,
          "light lifecycle equals the load lifecycle");
  require(world.getBlock({0, -63, 0}).name() == "minecraft:stone",
          "generated terrain visible in World");
  require(world.getBlock({5, 70, 5}).name() == "minecraft:diamond_block",
          "generated feature visible in World");
  require(world.getBlock({2, 65, 2}).name() == "minecraft:water",
          "generated fluid visible in World");
  const ChunkTickSnapshot generatedTicks = world.snapshotChunkTicks(0, 0);
  require(generatedTicks.blockTicks.size() == 1,
          "generated block tick installed with the chunk");
  require(generatedTicks.fluidTicks.size() == 1,
          "post-processing fluid tick rescheduled on install");
  require(world.fluidTickCount() >= 1,
          "fluid tick visible to the world queue");
  require(manager.tickReadiness({0, 0}).entityDataReady,
          "generated chunk reports empty entity data");
  require(pumpUntil(manager, [&] {
            for (int dx = -1; dx <= 1; ++dx) {
              for (int dz = -1; dz <= 1; ++dz) {
                if (!world.isChunkLoaded(dx, dz))
                  return false;
              }
            }
            return manager.tickReadiness({0, 0}).blockTicking;
          }),
          "generated 3x3 neighborhood is block ticking");
  const ChunkGenerationStats stats = manager.generationStats();
  require(stats.requested >= 1, "generation requested through miss");
  require(stats.completed >= 9, "resident generation completed");
  require(stats.failed == 0, "resident generation did not fail");

  // 受管写入被观察并进入脏版本（与磁盘载入区块相同的模拟语义）。
  require(manager.setBlock({1, 80, 1}, BlockState("minecraft:redstone_block")),
          "generated chunk accepts block edits");
  const auto status = manager.managedStatus({0, 0});
  require(status && status->lifecycle.state == ChunkResidencyState::Resident,
          "generated chunk stays resident");
  require(status->lifecycle.dirtyVersion >= 1,
          "generated chunk records the edit as dirty");

  const ChunkShutdownResult shutdown = manager.close();
  require(manager.generationStats().requested >= 1,
          "generation statistics survive shutdown");
  (void)shutdown;
}

// 2) 生成的区块由专用 I/O 线程写回完整 NBT；撤票驱逐后从磁盘重读，而不是重新生成。
void testGeneratedChunkPersistsAndReloads() {
  World world;
  auto store = std::make_shared<MemoryStore>();
  auto generator = std::make_shared<TestGenerator>(false);
  WorldChunkManager manager(world, store, 2, 4, emptyEntityProbe, {},
                            generator);
  manager.setAutosaveLimitsForTest({64, 8, 32,
                                    std::chrono::milliseconds{5},
                                    std::chrono::milliseconds{20},
                                    std::chrono::milliseconds{20}});
  manager.addPlayer(1, {0, 0});
  require(pumpUntil(manager, [&] { return world.isChunkLoaded(0, 0); }),
          "center chunk generated");
  // 等待整个 9x9 驻留环完成：未安装的坐标会在撤票时被丢弃，重进时重新生成，
  // 那样就无法用 requested 计数证明“重读磁盘而不是重新生成”。
  require(pumpUntil(manager, [&] {
            for (int dx = -4; dx <= 4; ++dx) {
              for (int dz = -4; dz <= 4; ++dz) {
                if (!world.isChunkLoaded(dx, dz))
                  return false;
              }
            }
            return true;
          }, 60000),
          "generated resident ring installed");
  require(manager.setBlock({3, 90, 3}, BlockState("minecraft:smooth_stone")),
          "edit generated chunk");
  require(tickUntil(manager, [&] { return store->hasPayload({0, 0}); }, 15000),
          "generated chunk persisted by the I/O worker");
  const auto saved = store->payload({0, 0});
  require(saved.has_value(), "saved payload present");
  const terrain::DecodedChunk decoded =
      terrain::decodeChunkNbt(std::span<const std::uint8_t>(*saved));
  require(decoded.status == terrain::DecodeStatus::Ok,
          "saved generated chunk decodes");
  require(decoded.statusName == "minecraft:full",
          "saved generated chunk keeps FULL status");
  require(decodedBlock(decoded, 5, 70, 5) == "minecraft:diamond_block",
          "generated feature persisted");
  require(decodedBlock(decoded, 3, 90, 3) == "minecraft:smooth_stone",
          "edit persisted through the generated template patch");
  require(decodedBlock(decoded, 0, 64, 0) == "minecraft:grass_block",
          "surface persisted");

  const std::uint64_t firstEpoch = world.chunkLoadEpochForTest(0, 0);
  require(firstEpoch != 0, "generated chunk carries a load epoch");
  const std::size_t generatedBefore = manager.generationStats().requested;
  manager.removePlayer(1);
  require(pumpUntil(manager, [&] { return manager.managedChunks().empty(); }),
          "all clean generated chunks evicted after demand removal");
  require(!world.isChunkLoaded(0, 0), "center chunk left the World");

  manager.addPlayer(1, {0, 0});
  require(pumpUntil(manager, [&] { return world.isChunkLoaded(0, 0); }),
          "chunk reloaded from disk");
  require(world.getBlock({3, 90, 3}).name() == "minecraft:smooth_stone",
          "persisted edit survives reload");
  require(manager.generationStats().requested == generatedBefore,
          "reload read disk instead of regenerating");
  const auto reloaded = manager.lifecycleStatus({0, 0});
  require(reloaded && reloaded->epoch != 0 && reloaded->epoch != firstEpoch,
          "reload starts a new load lifecycle");
  require(world.chunkLoadEpochForTest(0, 0) == reloaded->epoch,
          "reloaded World chunk keeps its new load epoch");
  (void)manager.close();
}

// 3) 缓冲等级（34..44）只推进生成缓存，不安装进 World。
void testBufferLevelsDoNotInstall() {
  World world;
  auto store = std::make_shared<MemoryStore>();
  auto generator = std::make_shared<TestGenerator>(false);
  WorldChunkManager manager(world, store, 2, 4, emptyEntityProbe, {},
                            generator);
  manager.addPlayer(1, {0, 0});
  require(pumpUntil(manager, [&] {
            return manager.generationStats().bufferCompleted > 0;
          }, 30000),
          "buffer generation completed");
  require(manager.generationStats().bufferRequests > 0,
          "buffer levels requested generation");
  ChunkTicketPos bufferPos{};
  bool found = false;
  for (int distance = 5; distance <= 10 && !found; ++distance) {
    const ChunkTicketPos candidate{distance, 0};
    const ChunkDemandSnapshot demand =
        manager.ticketSources().query(candidate);
    if (demand.loadingLevel > ticket_levels::FullChunkLevel &&
        demand.loadingLevel <= ticket_levels::MaxLevel) {
      bufferPos = candidate;
      found = true;
    }
  }
  require(found, "a buffer-level chunk exists beyond the resident ring");
  require(!world.isChunkLoaded(bufferPos.x, bufferPos.z),
          "buffer generation must not install a World chunk");
  require(pumpUntil(manager, [&] {
            return manager.generationStats().bufferCompleted >= 4;
          }, 30000),
          "buffer pipeline keeps progressing");
  require(!world.isChunkLoaded(bufferPos.x, bufferPos.z),
          "buffer chunk still absent after more generation");
  (void)manager.close();
}

// 4) 需求在生成完成前撤销：迟到结果被 epoch 拒绝，绝不安装。
void testStaleGenerationIsDiscarded() {
  World world;
  auto store = std::make_shared<MemoryStore>();
  auto generator = std::make_shared<TestGenerator>(false);
  generator->delayMs.store(50);
  WorldChunkManager manager(world, store, 2, 4, emptyEntityProbe, {},
                            generator);
  manager.addPlayer(1, {0, 0});
  require(pumpUntil(manager,
                    [&] { return manager.generationStats().requested > 0; }),
          "generation queued before demand removal");
  manager.removePlayer(1);
  require(pumpUntil(manager,
                    [&] { return manager.generationStats().completed > 0; }),
          "in-flight generation finished after demand removal");
  require(pumpUntil(manager, [&] { return manager.managedChunks().empty(); }),
          "rejected generated chunks never become managed");
  require(!world.isChunkLoaded(0, 0) && !world.isChunkLoaded(-2, -2),
          "rejected generated chunks never enter World");
  (void)manager.close();
}

// 5) 并行调度器的生成缓存按玩家中心裁剪，只保留 keep 半径内的半成品。
void testSchedulerCacheEviction() {
  auto generator = std::make_shared<TestGenerator>(false);
  worldgen::WorldGenSchedulerConfig config;
  config.threadCount = 2;
  worldgen::WorldGenScheduler scheduler(*generator, 1234, nullptr, nullptr,
                                        nullptr, nullptr, config);
  std::vector<worldgen::ChunkBatchRequest> batch;
  for (int dx = -1; dx <= 1; ++dx) {
    for (int dz = -1; dz <= 1; ++dz)
      batch.push_back({{dx, dz}, worldgen::ChunkStatusId::Biomes});
  }
  scheduler.ensureStatusBatch(batch, {0, 0});
  require(scheduler.heldChunkCount() >= 9,
          "biomes generation retains the dependency neighborhood");
  const ChunkTicketPos keep{0, 0};
  const std::size_t evicted =
      scheduler.evictIdleOutsideRadius(std::span<const ChunkTicketPos>(&keep, 1), 0);
  require(evicted > 0, "cache eviction removed distant half-chunks");
  require(scheduler.heldChunkCount() == 1,
          "only the keep center remains after eviction");
}

// 6) 真实 26.1 生成器接入 WorldChunkManager：生成、安装、模拟与专用 I/O 落盘。
// 6) 生成期光照：worker 在 InitializeLight/Light 阶段只投递纯值输入，主线程 pump 提交，
//    安装时接管同一份登记（不再整份重传几何）并对齐光照生命周期代次。
void testGenerationTimeLightSubmission() {
  World world;
  LightWorkerOptions lightOpts;
  lightOpts.batchWindowMs = 0;
  world.enableLighting(lightOpts);
  auto store = std::make_shared<MemoryStore>();
  auto generator = std::make_shared<TestGenerator>(false);
  WorldChunkManager manager(world, store, 2, 4, emptyEntityProbe, {}, generator);
  manager.addPlayer(1, {0, 0});
  require(pumpUntil(manager, [&] { return world.isChunkLoaded(0, 0); }, 20000),
          "generated chunk installed with lighting enabled");

  require(world.isChunkLoaded(0, 0), "generated chunk resident in World");
  const LightService::ChunkStatus status =
      world.lightService()->chunkStatus(0, 0);
  require(status.loaded,
          "light engine already knew the chunk while it was being generated");
  const std::uint64_t requiredGeneration =
      world.chunkRequiredLightGenerationForTest(0, 0);
  require(requiredGeneration != 0,
          "generation-time light request carried into the World chunk");
  require(status.submittedGeneration == requiredGeneration,
          "install adopted the generation-time submission instead of "
          "resubmitting the full geometry");
  require(status.lifecycleEpoch == world.chunkLightEpochForTest(0, 0),
          "install aligned the light lifecycle to the engine lifetime");
  require(world.generatedLightAdoptionCountForTest() >= 1,
          "install adopted the generation-time light registration");

  require(pumpUntil(
              manager,
              [&] {
                const LightService::ChunkStatus st =
                    world.lightService()->chunkStatus(0, 0);
                return st.complete && !st.pending;
              },
              20000),
          "generated chunk light converges without a second submission");
  world.drainLightPublications();
  require(world.lightService()->latestLayerChunk(LightLayer::Block, 0, 0) !=
              nullptr,
          "generated chunk publishes a block light layer");

  // 默认低内存：安装后已切换为直读 World 几何，运行时埋光源必须立刻被光照线程看到。
  require(manager.setBlock({0, 0, 0}, BlockState("minecraft:glowstone")),
          "generated chunk accepts a light source after adoption");
  require(pumpUntil(
              manager,
              [&] { return world.getBrightness({0, 0, 0}, 0) == 15; }, 20000),
          "direct light reads observe runtime edits after adoptDirectGeometry");
  require(manager.close().complete, "lighting generation world closes cleanly");
}

void testRealGeneratorInstall(const std::filesystem::path &assets) {
  const worldgen::WorldgenRegistries registries =
      worldgen::WorldgenRegistries::loadFromDirectory(assets);
  const worldgen::NoiseGeneratorSettings *settings =
      registries.findNoiseSettings("minecraft:overworld");
  require(settings != nullptr, "overworld noise settings loaded");
  auto generator = std::make_shared<worldgen::NoiseBasedChunkGenerator>(
      registries, *settings,
      std::make_shared<worldgen::MultiNoiseBiomeSource>(
          worldgen::MultiNoiseBiomeSource::createOverworld()));
  World world;
  auto store = std::make_shared<MemoryStore>();
  WorldChunkManager manager(world, store, 2, 4, emptyEntityProbe, {},
                            generator);
  manager.setGenerationThreadsForTest(4);
  manager.setAutosaveLimitsForTest({64, 8, 32,
                                    std::chrono::milliseconds{5},
                                    std::chrono::milliseconds{20},
                                    std::chrono::milliseconds{20}});
  manager.addPlayer(1, {0, 0});
  require(pumpUntil(manager, [&] { return world.isChunkLoaded(0, 0); },
                    120000),
          "real 26.1 generator installed the center chunk");
  require(world.getBlock({0, -64, 0}).name() != "minecraft:air",
          "real generated terrain has bedrock at minY");
  require(world.getBlock({0, 200, 0}).name() == "minecraft:air",
          "real generated terrain is air near build limit");
  const auto lifecycle = manager.lifecycleStatus({0, 0});
  require(lifecycle && world.chunkLoadEpochForTest(0, 0) == lifecycle->epoch,
          "real generated chunk keeps its load lifecycle");
  require(tickUntil(manager, [&] { return store->hasPayload({0, 0}); },
                    120000),
          "real generated chunk persisted through the I/O thread");
  const auto payload = store->payload({0, 0});
  require(payload.has_value(), "real generated payload present");
  terrain::DecodeOptions decodeOptions;
  // 生成地形包含已注册但尚未实现模拟的方块，解码器需按“已注册即可”读取。
  decodeOptions.allowUnimplementedBlocks = true;
  const terrain::DecodedChunk decoded =
      terrain::decodeChunkNbt(std::span<const std::uint8_t>(*payload),
                              decodeOptions);
  require(decoded.status == terrain::DecodeStatus::Ok,
          "real generated chunk decodes as valid Anvil data");
  require(decoded.statusName == "minecraft:full",
          "real generated chunk is FULL");
  require(!decoded.sections.empty(), "real generated chunk has sections");
  require(manager.close().complete, "real generated world closes cleanly");
}

} // namespace

int main(int argc, char **argv) {
  try {
    testGeneratedChunkBecomesSimulatable();
    testGeneratedChunkPersistsAndReloads();
    testBufferLevelsDoNotInstall();
    testStaleGenerationIsDiscarded();
    testSchedulerCacheEviction();
    testGenerationTimeLightSubmission();
    if (argc >= 2)
      testRealGeneratorInstall(argv[1]);
    std::cout << "chunk generation tests passed"
              << (argc >= 2 ? " (with real 26.1 generator)" : "")
              << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "chunk generation test failure: " << error.what() << '\n';
    return 1;
  }
}
