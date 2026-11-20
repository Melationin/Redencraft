#pragma once
// 世界生成器核心抽象（`ChunkGenerator`）、受限区块生成视图（`WorldGenRegion`）以及
// 基于 26.1 `ChunkPyramid` 的 12 阶段按需生成管线（`ChunkGenerationPipeline`）。
// 之所以单独抽象 `WorldGenRegion` 而不是直接暴露 `World*`，是因为原版多线程区块生成要求：
// 每个阶段只能读写当前步骤 `directDependencies` / `blockStateWriteRadius` 规定半径内的邻域区块，
// 防止越界写入引发数据竞争或依赖死锁，同时保持与 26.1 `ChunkStatusTasks` 完全一致的阶段推进契约。

#include "redstone/chunk_lifecycle.hpp"
#include "redstone/light/chunk_input.hpp"
#include "redstone/worldgen/chunk_status.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace redstone {
class LightService;
class World;
} // namespace redstone

namespace redstone::worldgen {

// 生成期光照工作项（对齐 26.1 `InitializeLight` 注册几何 → `Light` 请求首次计算的两段式交接）。
// 存在原因：`LightService::submitChunk` / `requestChunkCompute` 只在主线程调用，而生成本身跑在
// 并行 worker 上。worker 因此只构造纯值的 `LightChunkInput` 投递到队列，由主线程 pump 真正提交，
// 既不破坏生成并行度，也不把光照引擎的线程契约扩散到生成代码里。
struct GeneratedLightWork {
  ChunkTicketPos position;
  // false = InitializeLight 阶段注册几何（停在“未计算”）；true = Light 阶段请求首次计算。
  bool compute = false;
  LightChunkInput input;
};

// 生成 worker → 主线程的单向投递队列：互斥保护、不阻塞、不参与生成依赖图。
class GeneratedLightQueue {
public:
  void push(GeneratedLightWork work) {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.push_back(std::move(work));
  }
  std::vector<GeneratedLightWork> drain() {
    std::vector<GeneratedLightWork> out;
    std::lock_guard<std::mutex> lock(mutex_);
    out.reserve(items_.size());
    for (GeneratedLightWork &item : items_)
      out.push_back(std::move(item));
    items_.clear();
    return out;
  }
  std::size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return items_.size();
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    items_.clear();
  }

private:
  mutable std::mutex mutex_;
  std::deque<GeneratedLightWork> items_;
};

// 生成 worker 与主线程共享的光学查找表句柄。光照服务可能在生成线程启动后才启用
// （`World::enableLighting`），因此用互斥保护而不是在调度器构造时固化一份指针。
class LightOpticalContext {
public:
  std::shared_ptr<const LightOpticalTable> get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return optical_;
  }
  bool skyLightEnabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return skyLightEnabled_;
  }
  void set(std::shared_ptr<const LightOpticalTable> optical,
           bool skyLightEnabled = true) {
    std::lock_guard<std::mutex> lock(mutex_);
    optical_ = std::move(optical);
    skyLightEnabled_ = skyLightEnabled;
  }

private:
  mutable std::mutex mutex_;
  std::shared_ptr<const LightOpticalTable> optical_;
  bool skyLightEnabled_ = true;
};

// 对齐原版 26.1 `WorldGenRegion.java`：向单个生成步骤暴露中心区块及有限邻域切片。
// 所有跨区块读写均受 `ChunkStep::directDependencies()` 与 `ChunkStep::blockStateWriteRadius()` 门控。
class WorldGenRegion {
public:
  WorldGenRegion(const ChunkStep &step, ProtoChunk &center,
                 std::vector<BaseChunk *> squareCache, int radius,
                 std::int64_t worldSeed,
                 std::vector<std::shared_ptr<BaseChunk>> pinnedChunks = {});

  ChunkTicketPos centerPos() const noexcept { return center_.pos(); }
  const ChunkStep &generatingStep() const noexcept { return step_; }
  std::int64_t seed() const noexcept { return worldSeed_; }
  int minY() const noexcept { return center_.minY(); }
  int height() const noexcept { return center_.height(); }
  int maxY() const noexcept { return center_.maxY(); }

  bool hasChunk(int chunkX, int chunkZ) const noexcept;
  BaseChunk *getChunkIfAllowed(int chunkX, int chunkZ,
                               ChunkStatusId minStatus = ChunkStatusId::Empty) const;
  BaseChunk &getChunk(int chunkX, int chunkZ,
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
  ProtoChunk &center_;
  std::vector<BaseChunk *> cache_;
  std::vector<std::shared_ptr<BaseChunk>> pinned_;
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
  // 维度区块高度（LevelHeightAccessor）与生成深度（getGenDepth）分离：
  // 下界/末地生成深度 128，但区块仍是 16 段（0..255），128 以上为空气。
  virtual int chunkMinY() const noexcept { return minY(); }
  virtual int chunkHeight() const noexcept { return height(); }

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
                          std::shared_ptr<LightOpticalContext> lightContext = {},
                          GeneratedLightQueue *lightQueue = nullptr,
                          bool asyncWorldGenLight = true);

  // 确保目标区块达到 `targetStatus`（若已在更高阶段则直接返回）。
  // 使用 `LOADING_PYRAMID` 当区块已在磁盘处于较后阶段时可跳过前面已固化的邻域递归。
  BaseChunk &ensureStatus(ChunkTicketPos pos, ChunkStatusId targetStatus);

  // 将目标区块推进到当前加载票等级所要求的状态（等级 > 44 则不触发生成并返回 nullptr）。
  BaseChunk *ensureForLoadingLevel(ChunkTicketPos pos, int loadingLevel);

  BaseChunk *findChunk(ChunkTicketPos pos) const;
  std::shared_ptr<ProtoChunk> takeProtoChunk(ChunkTicketPos pos);

  // 生成至 `Full` 并通过共享安装路径写入运行态 `World`。
  bool generateAndInstallIntoWorld(ChunkTicketPos pos, World &world);


  const Stats &stats() const noexcept { return stats_; }

private:
  BaseChunk &getOrLoadChunk(ChunkTicketPos pos);
  void advanceOneStep(ChunkTicketPos pos, BaseChunk &chunk,
                      const ChunkStep &step, bool fromLoadedProto);

  ChunkGenerator &generator_;
  std::int64_t worldSeed_ = 0;
  RawChunkStore *diskStore_ = nullptr;
  World *residentWorld_ = nullptr;
  std::shared_ptr<LightOpticalContext> lightContext_;
  GeneratedLightQueue *lightQueue_ = nullptr;
  bool asyncWorldGenLight_ = true;
  std::map<ChunkTicketPos, std::shared_ptr<BaseChunk>> chunks_;
  std::map<ChunkTicketPos, bool> startedLoadedFromStore_;
  Stats stats_;
};

// 执行单个 `ChunkStatusId` 的生成步骤并在完成时原子推进 `proto.persistedStatus()`。
// `lightContext` / `lightQueue` 非空时，`InitializeLight` 与 `Light` 阶段只构造并投递
// 光照工作（真正的 `LightService` 提交由主线程 pump 完成）；两者为空时生成不触碰光照。
void executeChunkGenerationStep(ChunkGenerator &generator, WorldGenRegion &region,
                                ProtoChunk &proto, ChunkStatusId targetStatus,
                                std::int64_t worldSeed,
                                LightOpticalContext *lightContext,
                                GeneratedLightQueue *lightQueue,
                                std::shared_ptr<const BaseChunk> chunkHandle = {},
                                bool asyncWorldGenLight = true);

// 将到达 `Full` 的 `ProtoChunk` 零拷贝/紧凑写入运行态 `World` 并调度后处理流体刻。
// 从光照引擎的最新快照刷新 ProtoChunk 的光照层（晋升/存档前调用）。
// 光照的权威数据在光照引擎里可变地更新，ProtoChunk 里的层只是存档缓存。
void refreshProtoChunkLight(ProtoChunk &proto, LightService &lightService);

bool installProtoChunkIntoWorld(std::shared_ptr<ProtoChunk> proto,
                                World &world);

} // namespace redstone::worldgen
