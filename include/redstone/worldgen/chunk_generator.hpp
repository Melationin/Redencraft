#pragma once
// 世界生成器核心抽象（`ChunkGenerator`）、受限区块生成视图（`WorldGenRegion`）以及
// 基于 26.1 `ChunkPyramid` 的 12 阶段按需生成管线（`ChunkGenerationPipeline`）。
// 之所以单独抽象 `WorldGenRegion` 而不是直接暴露 `World*`，是因为原版多线程区块生成要求：
// 每个阶段只能读写当前步骤 `directDependencies` / `blockStateWriteRadius` 规定半径内的邻域区块，
// 防止越界写入引发数据竞争或依赖死锁，同时保持与 26.1 `ChunkStatusTasks` 完全一致的阶段推进契约。

#include "redstone/chunk_lifecycle.hpp"
#include "redstone/worldgen/chunk_status.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace redstone {
class LightService;
class World;
} // namespace redstone

namespace redstone::worldgen {

// 对齐原版 26.1 `WorldGenRegion.java`：向单个生成步骤暴露中心区块及有限邻域切片。
// 所有跨区块读写均受 `ChunkStep::directDependencies()` 与 `ChunkStep::blockStateWriteRadius()` 门控。
class WorldGenRegion {
public:
  WorldGenRegion(const ChunkStep &step, ChunkAccess &center,
                 std::vector<ChunkAccess *> squareCache, int radius,
                 std::int64_t worldSeed);

  ChunkTicketPos centerPos() const noexcept { return center_.pos(); }
  const ChunkStep &generatingStep() const noexcept { return step_; }
  std::int64_t seed() const noexcept { return worldSeed_; }
  int minY() const noexcept { return center_.minY(); }
  int height() const noexcept { return center_.height(); }
  int maxY() const noexcept { return center_.maxY(); }

  bool hasChunk(int chunkX, int chunkZ) const noexcept;
  ChunkAccess *getChunkIfAllowed(int chunkX, int chunkZ,
                                 ChunkStatusId minStatus = ChunkStatusId::Empty) const;
  ChunkAccess &getChunk(int chunkX, int chunkZ,
                        ChunkStatusId minStatus = ChunkStatusId::Empty) const;

  bool ensureCanWrite(int x, int y, int z) const noexcept;
  BlockState getBlockState(int x, int y, int z) const;
  FluidState getFluidState(int x, int y, int z) const;
  bool setBlockState(int x, int y, int z, const BlockState &state,
                     int flags = 3);
  std::string_view getNoiseBiome(int quartX, int quartY, int quartZ) const;
  int getHeight(HeightmapType type, int x, int z) const;

private:
  const ChunkStep &step_;
  ChunkAccess &center_;
  std::vector<ChunkAccess *> cache_;
  int radius_ = 0;
  int stride_ = 1;
  std::int64_t worldSeed_ = 0;
};

// 世界生成器接口（对齐 26.1 `ChunkGenerator`）：按 `ChunkStatus` 阶段拆分地形、群系、雕刻、结构与装饰入口。
// 后续阶段（G-1 ~ G-6）在此接口上实现噪声地形生成器、超平坦生成器等具体策略。
class ChunkGenerator {
public:
  virtual ~ChunkGenerator() = default;

  virtual int minY() const noexcept { return -64; }
  virtual int height() const noexcept { return 384; }

  virtual void createStructures(ProtoChunk &chunk, std::int64_t worldSeed) {
    (void)chunk;
    (void)worldSeed;
  }
  virtual void createReferences(WorldGenRegion &region, ProtoChunk &chunk) {
    (void)region;
    (void)chunk;
  }
  virtual void createBiomes(WorldGenRegion &region, ProtoChunk &chunk,
                            std::int64_t worldSeed) {
    (void)region;
    (void)chunk;
    (void)worldSeed;
  }
  virtual void fillFromNoise(WorldGenRegion &region, ProtoChunk &chunk,
                             std::int64_t worldSeed) {
    (void)region;
    (void)chunk;
    (void)worldSeed;
  }
  virtual void buildSurface(WorldGenRegion &region, ProtoChunk &chunk,
                            std::int64_t worldSeed) {
    (void)region;
    (void)chunk;
    (void)worldSeed;
  }
  virtual void applyCarvers(WorldGenRegion &region, ProtoChunk &chunk,
                            std::int64_t worldSeed) {
    (void)region;
    (void)chunk;
    (void)worldSeed;
  }
  virtual void applyBiomeDecoration(WorldGenRegion &region, ProtoChunk &chunk,
                                    std::int64_t worldSeed) {
    (void)region;
    (void)chunk;
    (void)worldSeed;
  }
  virtual void spawnOriginalMobs(WorldGenRegion &region, ProtoChunk &chunk,
                                 std::int64_t worldSeed) {
    (void)region;
    (void)chunk;
    (void)worldSeed;
  }
};

// 阶段 G-0 驱动器：严格按 `ChunkPyramid::generationPyramid()` 递归满足邻域依赖，
// 支持从磁盘恢复半成品 `ProtoChunk` 续跑、推进到任意指定 `ChunkStatusId`（包括外围 34..44 等级），
// 并在到达 `Full` 时通过 `promoteProtoChunkToLoaded` 晋升安装进 `World`。
class ChunkGenerationPipeline {
public:
  struct Stats {
    std::size_t chunksCreated = 0;
    std::size_t chunksLoadedFromStore = 0;
    std::size_t stepsExecuted = 0;
  };

  ChunkGenerationPipeline(ChunkGenerator &generator, std::int64_t worldSeed,
                          RawChunkStore *diskStore = nullptr,
                          World *residentWorld = nullptr,
                          LightService *lightService = nullptr);

  // 确保目标区块达到 `targetStatus`（若已在更高阶段则直接返回）。
  // 使用 `LOADING_PYRAMID` 当区块已在磁盘处于较后阶段时可跳过前面已固化的邻域递归。
  ChunkAccess &ensureStatus(ChunkTicketPos pos, ChunkStatusId targetStatus);

  // 将目标区块推进到当前加载票等级所要求的状态（等级 > 44 则不触发生成并返回 nullptr）。
  ChunkAccess *ensureForLoadingLevel(ChunkTicketPos pos, int loadingLevel);

  ChunkAccess *findChunk(ChunkTicketPos pos) const;
  std::optional<ProtoChunk> takeProtoChunk(ChunkTicketPos pos);

  // 生成至 `Full`，并将完整区块数据、后处理流体/形状刻与初始高度图安装到指定 `World`。
  bool generateAndInstallIntoWorld(ChunkTicketPos pos, World &world);

  const Stats &stats() const noexcept { return stats_; }

private:
  LightService *activeLightService() const noexcept;
  ChunkAccess &getOrLoadChunk(ChunkTicketPos pos);
  void advanceOneStep(ChunkTicketPos pos, ChunkAccess &chunk,
                      const ChunkStep &step, bool fromLoadedProto);

  ChunkGenerator &generator_;
  std::int64_t worldSeed_ = 0;
  RawChunkStore *diskStore_ = nullptr;
  World *residentWorld_ = nullptr;
  LightService *lightService_ = nullptr;
  std::map<ChunkTicketPos, std::unique_ptr<ChunkAccess>> chunks_;
  std::map<ChunkTicketPos, bool> startedLoadedFromStore_;
  Stats stats_;
};

// 执行单个 `ChunkStatusId` 的生成步骤并在完成时原子推进 `proto.persistedStatus()`。
void executeChunkGenerationStep(ChunkGenerator &generator, WorldGenRegion &region,
                                ProtoChunk &proto, ChunkStatusId targetStatus,
                                std::int64_t worldSeed,
                                LightService *lightService);

// 将到达 `Full` 的 `ProtoChunk` 零拷贝/紧凑写入运行态 `World` 并调度后处理流体刻。
bool installProtoChunkIntoWorld(ProtoChunk &&proto, World &world);

} // namespace redstone::worldgen
