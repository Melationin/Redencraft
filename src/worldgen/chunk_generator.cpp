#include "redstone/worldgen/chunk_generator.hpp"

#include "redstone/engine.hpp"
#include "redstone/light/chunk_input.hpp"
#include "redstone/light/service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace redstone::worldgen {
namespace {

bool needsPostProcess(const BlockState &state) {
  return !fluidStateOf(state).isEmpty();
}

PersistedChunkLight extractPersistedLight(const ProtoChunk &proto) {
  PersistedChunkLight persisted;
  persisted.isLightOn = proto.isLightCorrect();
  persisted.sections.reserve(proto.sections().size());
  for (const ProtoChunkSection &sec : proto.sections()) {
    if (!sec.blockLight.has_value() && !sec.skyLight.has_value())
      continue;
    PersistedLightSection ps;
    ps.sectionY = sec.sectionY();
    ps.blockLight = sec.blockLight.copy();
    ps.skyLight = sec.skyLight.copy();
    persisted.sections.push_back(std::move(ps));
  }
  return persisted;
}

void copySnapshotLayerToOptional(const LightSectionSnapshot *secSnap,
                                 LightLayer layer, LightLayerStorage &out) {
  if (!secSnap) {
    out.reset();
    return;
  }
  const LightLayerRef &ref = secSnap->layers[static_cast<std::size_t>(layer)];
  if (ref.isMissing()) {
    out.reset();
    return;
  }
  if (ref.isUniform()) {
    const std::uint8_t v = static_cast<std::uint8_t>(ref.uniformValue() & 15u);
    out.emplace().fill(static_cast<std::uint8_t>(v | (v << 4)));
    return;
  }
  ref.readPacked(out.emplace());
}

} // namespace

WorldGenRegion::WorldGenRegion(const ChunkStep &step, ChunkAccess &center,
                               std::vector<ChunkAccess *> squareCache,
                               int radius, std::int64_t worldSeed)
    : step_(step), center_(center), cache_(std::move(squareCache)),
      radius_(radius), stride_(radius * 2 + 1), worldSeed_(worldSeed) {
  const std::size_t expected = static_cast<std::size_t>(stride_) * stride_;
  if (cache_.size() != expected) {
    throw std::invalid_argument(
        "WorldGenRegion cache size does not match radius");
  }
}

bool WorldGenRegion::hasChunk(int chunkX, int chunkZ) const noexcept {
  const int dx = std::abs(chunkX - center_.pos().x);
  const int dz = std::abs(chunkZ - center_.pos().z);
  const int dist = std::max(dx, dz);
  return static_cast<std::size_t>(dist) < step_.directDependencies().size();
}

ChunkAccess *WorldGenRegion::getChunkIfAllowed(int chunkX, int chunkZ,
                                               ChunkStatusId minStatus) const {
  const int dx = chunkX - center_.pos().x;
  const int dz = chunkZ - center_.pos().z;
  const int dist = std::max(std::abs(dx), std::abs(dz));
  if (static_cast<std::size_t>(dist) >= step_.directDependencies().size() &&
      dist != 0) {
    return nullptr;
  }
  if (dist == 0) {
    if (static_cast<int>(minStatus) > static_cast<int>(step_.targetStatus()))
      return nullptr;
    return &center_;
  }
  const ChunkStatusId maxAllowed = step_.directDependencies().get(dist);
  if (static_cast<int>(minStatus) > static_cast<int>(maxAllowed))
    return nullptr;
  if (std::abs(dx) > radius_ || std::abs(dz) > radius_)
    return nullptr;
  const std::size_t idx =
      static_cast<std::size_t>(dz + radius_) * stride_ +
      static_cast<std::size_t>(dx + radius_);
  ChunkAccess *chunk = cache_[idx];
  if (!chunk)
    return nullptr;
  if (static_cast<int>(chunk->persistedStatus()) < static_cast<int>(minStatus))
    return nullptr;
  return chunk;
}

ChunkAccess &WorldGenRegion::getChunk(int chunkX, int chunkZ,
                                      ChunkStatusId minStatus) const {
  ChunkAccess *chunk = getChunkIfAllowed(chunkX, chunkZ, minStatus);
  if (!chunk) {
    throw std::out_of_range(
        "Requested chunk unavailable or outside direct dependency bounds");
  }
  return *chunk;
}

bool WorldGenRegion::ensureCanWrite(int x, int y, int z) const noexcept {
  if (center_.isOutsideBuildHeight(y))
    return false;
  const int writeRadius = step_.blockStateWriteRadius();
  if (writeRadius < 0)
    return false;
  const int cx = x >> 4;
  const int cz = z >> 4;
  const int dx = std::abs(cx - center_.pos().x);
  const int dz = std::abs(cz - center_.pos().z);
  return dx <= writeRadius && dz <= writeRadius;
}

BlockState WorldGenRegion::getBlockState(int x, int y, int z) const {
  return getChunk(x >> 4, z >> 4, ChunkStatusId::Empty).getBlockState(x, y, z);
}

FluidState WorldGenRegion::getFluidState(int x, int y, int z) const {
  return getChunk(x >> 4, z >> 4, ChunkStatusId::Empty).getFluidState(x, y, z);
}

bool WorldGenRegion::setBlockState(int x, int y, int z, const BlockState &state,
                                   int flags) {
  if (!ensureCanWrite(x, y, z))
    return false;
  ChunkAccess &chunk = getChunk(x >> 4, z >> 4, ChunkStatusId::Empty);
  chunk.setBlockState(x, y, z, state, flags);
  if ((flags & 16) == 0 && needsPostProcess(state))
    chunk.markPosForPostprocessing(x, y, z);
  return true;
}

std::string_view WorldGenRegion::getNoiseBiome(int quartX, int quartY,
                                               int quartZ) const {
  const int cx = quartX >> 2;
  const int cz = quartZ >> 2;
  return getChunk(cx, cz, ChunkStatusId::Biomes)
      .getNoiseBiome(quartX, quartY, quartZ);
}

int WorldGenRegion::getHeight(HeightmapType type, int x, int z) const {
  return getChunk(x >> 4, z >> 4, ChunkStatusId::Empty).getHeight(type, x, z);
}

ChunkGenerationPipeline::ChunkGenerationPipeline(
    ChunkGenerator &generator, std::int64_t worldSeed, RawChunkStore *diskStore,
    World *residentWorld, std::shared_ptr<LightOpticalContext> lightContext,
    GeneratedLightQueue *lightQueue)
    : generator_(generator), worldSeed_(worldSeed), diskStore_(diskStore),
      residentWorld_(residentWorld), lightContext_(std::move(lightContext)),
      lightQueue_(lightQueue) {}

ChunkAccess *ChunkGenerationPipeline::findChunk(ChunkTicketPos pos) const {
  auto it = chunks_.find(pos);
  return it == chunks_.end() ? nullptr : it->second.get();
}

std::shared_ptr<ProtoChunk>
ChunkGenerationPipeline::takeProtoChunk(ChunkTicketPos pos) {
  auto it = chunks_.find(pos);
  if (it == chunks_.end())
    return nullptr;
  auto proto = std::dynamic_pointer_cast<ProtoChunk>(it->second);
  if (!proto)
    return nullptr;
  it->second.reset();
  chunks_.erase(it);
  startedLoadedFromStore_.erase(pos);
  return proto;
}

ChunkAccess &ChunkGenerationPipeline::getOrLoadChunk(ChunkTicketPos pos) {
  if (auto it = chunks_.find(pos); it != chunks_.end())
    return *it->second;

  if (residentWorld_ != nullptr &&
      residentWorld_->isChunkLoaded(pos.x, pos.z)) {
    auto imposter = std::make_shared<ImposterProtoChunk>(*residentWorld_, pos,
                                                         false);
    ChunkAccess &ref = *imposter;
    chunks_.emplace(pos, std::move(imposter));
    startedLoadedFromStore_[pos] = true;
    return ref;
  }

  if (diskStore_ != nullptr) {
    const StoreReadResult raw = diskStore_->readChunk(pos);
    if (raw.status == StoreReadStatus::Found) {
      std::string err;
      auto loaded = decodeProtoChunkNbt(raw.nbt, generator_.minY(),
                                        generator_.height(), err);
      if (!loaded) {
        throw std::runtime_error("failed to decode persisted chunk (" +
                                 std::to_string(pos.x) + "," +
                                 std::to_string(pos.z) + "): " + err);
      }
      auto ptr = std::make_shared<ProtoChunk>(std::move(*loaded));
      ChunkAccess &ref = *ptr;
      chunks_.emplace(pos, std::move(ptr));
      startedLoadedFromStore_[pos] = true;
      ++stats_.chunksLoadedFromStore;
      return ref;
    }
    if (raw.status != StoreReadStatus::Missing) {
      throw std::runtime_error("disk chunk read error: " + raw.detail);
    }
  }

  auto proto = std::make_shared<ProtoChunk>(pos, generator_.chunkMinY(),
                                            generator_.chunkHeight());
  ChunkAccess &ref = *proto;
  chunks_.emplace(pos, std::move(proto));
  startedLoadedFromStore_[pos] = false;
  ++stats_.chunksCreated;
  return ref;
}

ChunkAccess &ChunkGenerationPipeline::ensureStatus(ChunkTicketPos pos,
                                                   ChunkStatusId targetStatus) {
  ChunkAccess &chunk = getOrLoadChunk(pos);
  if (static_cast<int>(chunk.persistedStatus()) >=
      static_cast<int>(targetStatus)) {
    return chunk;
  }

  const bool fromLoaded = startedLoadedFromStore_[pos];
  const ChunkPyramid &pyramid =
      fromLoaded &&
              static_cast<int>(chunk.persistedStatus()) >=
                  static_cast<int>(ChunkStatusId::Spawn)
          ? ChunkPyramid::loadingPyramid()
          : ChunkPyramid::generationPyramid();

  while (static_cast<int>(chunk.persistedStatus()) <
         static_cast<int>(targetStatus)) {
    const ChunkStatusId nextStatus = static_cast<ChunkStatusId>(
        static_cast<int>(chunk.persistedStatus()) + 1);
    const ChunkStep &step = pyramid.getStepTo(nextStatus);
    advanceOneStep(pos, chunk, step, fromLoaded);
  }
  return chunk;
}

void ChunkGenerationPipeline::advanceOneStep(ChunkTicketPos pos,
                                             ChunkAccess &chunk,
                                             const ChunkStep &step,
                                             bool) {
  const int radius = step.directDependencies().getRadius();
  const int stride = radius * 2 + 1;
  std::vector<ChunkAccess *> squareCache(static_cast<std::size_t>(stride) *
                                             stride,
                                         nullptr);
  for (int dz = -radius; dz <= radius; ++dz) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int dist = std::max(std::abs(dx), std::abs(dz));
      ChunkAccess *entry = nullptr;
      if (dx == 0 && dz == 0) {
        entry = &chunk;
      } else if (static_cast<std::size_t>(dist) <
                 step.directDependencies().size()) {
        const ChunkStatusId depStatus = step.directDependencies().get(dist);
        entry = &ensureStatus({pos.x + dx, pos.z + dz}, depStatus);
      }
      const std::size_t idx =
          static_cast<std::size_t>(dz + radius) * stride +
          static_cast<std::size_t>(dx + radius);
      squareCache[idx] = entry;
    }
  }

  if (auto *proto = dynamic_cast<ProtoChunk *>(&chunk)) {
    WorldGenRegion region(step, chunk, std::move(squareCache), radius,
                          worldSeed_);
    std::shared_ptr<const BaseChunk> handle;
    if (auto it = chunks_.find(pos); it != chunks_.end())
      handle = std::static_pointer_cast<const BaseChunk>(it->second);
    executeChunkGenerationStep(generator_, region, *proto, step.targetStatus(),
                               worldSeed_, lightContext_.get(), lightQueue_,
                               std::move(handle));
  }
  ++stats_.stepsExecuted;
}

void executeChunkGenerationStep(ChunkGenerator &generator, WorldGenRegion &region,
                                ProtoChunk &proto, ChunkStatusId targetStatus,
                                std::int64_t worldSeed,
                                LightOpticalContext *lightContext,
                                GeneratedLightQueue *lightQueue,
                                std::shared_ptr<const BaseChunk> chunkHandle) {
  const ChunkTicketPos pos = proto.pos();
  switch (targetStatus) {
  case ChunkStatusId::Empty:
    break;
  case ChunkStatusId::StructureStarts:
    generator.createStructures(proto, worldSeed);
    break;
  case ChunkStatusId::StructureReferences:
    generator.createReferences(region, proto);
    break;
  case ChunkStatusId::Biomes:
    generator.createBiomes(region, proto, worldSeed);
    break;
  case ChunkStatusId::Noise:
    generator.fillFromNoise(region, proto, worldSeed);
    break;
  case ChunkStatusId::Surface:
    generator.buildSurface(region, proto, worldSeed);
    proto.flushDirtyHeightmaps();
    break;
  case ChunkStatusId::Carvers:
    generator.applyCarvers(region, proto, worldSeed);
    proto.setCachedNoiseChunkOpaque(nullptr);
    proto.flushDirtyHeightmaps();
    break;
  case ChunkStatusId::Features: {
    const auto &finalTypes =
        ChunkStatus::byId(ChunkStatusId::Features).heightmapsAfter();
    proto.primeHeightmaps(finalTypes);
    generator.applyBiomeDecoration(region, proto, worldSeed);
    proto.flushDirtyHeightmaps();
    proto.clearCarvingMask();
    break;
  }
  case ChunkStatusId::InitializeLight:
    proto.primeHeightmaps(ChunkStatus::initializeLight().heightmapsAfter());
    proto.flushDirtyHeightmaps();
    if (lightQueue != nullptr && lightContext != nullptr) {
      if (const auto optical = lightContext->get()) {
        // 生成 worker 只构造纯值输入；真正的 submitChunk 由主线程 pump 完成。
        GeneratedLightWork work;
        work.position = pos;
        // 有句柄就零拷贝直读（worker 读这个 ProtoChunk 的区段），否则退回值快照。
        work.input = buildLightChunkInput(
            proto,
            proto.isLightCorrect()
                ? std::optional<PersistedChunkLight>(extractPersistedLight(proto))
                : std::nullopt,
            /*deferCompute=*/!proto.isLightCorrect(), std::move(chunkHandle));
        lightQueue->push(std::move(work));
      }
    }
    break;
  case ChunkStatusId::Light:
    // 不等待：只登记首次计算请求，区块带着“未计算”直接进入 Spawn/Full。
    // 光照由引擎异步发布，需要数据的消费方（存档、协议、查询）自己按
    // isChunkSettled()/latestLayerChunk() 取，绝不在生成流水线里做全局屏障。
    if (lightQueue != nullptr) {
      GeneratedLightWork work;
      work.position = pos;
      work.compute = true;
      lightQueue->push(std::move(work));
    }
    proto.setLightCorrect(false);
    break;
  case ChunkStatusId::Spawn:
    generator.spawnOriginalMobs(region, proto, worldSeed);
    break;
  case ChunkStatusId::Full:
    proto.primeHeightmaps(ChunkStatus::full().heightmapsAfter());
    proto.flushDirtyHeightmaps();
    break;
  }
  proto.setPersistedStatus(targetStatus);
}

ChunkAccess *ChunkGenerationPipeline::ensureForLoadingLevel(ChunkTicketPos pos,
                                                            int loadingLevel) {
  const auto target = generationStatusForLevel(loadingLevel);
  if (!target)
    return nullptr;
  return &ensureStatus(pos, *target);
}

bool ChunkGenerationPipeline::generateAndInstallIntoWorld(ChunkTicketPos pos,
                                                          World &world) {
  ensureStatus(pos, ChunkStatusId::Full);
  std::shared_ptr<ProtoChunk> proto = takeProtoChunk(pos);
  if (!proto)
    return world.isChunkLoaded(pos.x, pos.z);
  return installProtoChunkIntoWorld(std::move(proto), world);
}


void refreshProtoChunkLight(ProtoChunk &proto, LightService &lightService) {
  lightService.drainPublished();
  const auto blockSnap = lightService.latestLayerChunk(
      LightLayer::Block, proto.pos().x, proto.pos().z);
  const auto skySnap = lightService.latestLayerChunk(
      LightLayer::Sky, proto.pos().x, proto.pos().z);
  // 引擎还没算完这个区块：保留 ProtoChunk 里已有的（可能是存档读入的）光照，
  // 绝不能用一个空快照把它抹掉。
  if (!blockSnap && !skySnap)
    return;
  for (int s = 0; s < proto.sectionCount(); ++s) {
    ProtoChunkSection &sec = proto.section(s);
    const int secY = sec.sectionY();
    if (blockSnap)
      copySnapshotLayerToOptional(blockSnap->section(secY), LightLayer::Block,
                                  sec.blockLight);
    if (skySnap)
      copySnapshotLayerToOptional(skySnap->section(secY), LightLayer::Sky,
                                  sec.skyLight);
  }
}

bool installProtoChunkIntoWorld(std::shared_ptr<ProtoChunk> protoPtr,
                              World &world) {
  ProtoChunk &proto = *protoPtr;
  const ChunkTicketPos pos = proto.pos();
  // 备份待后处理坐标（用于晋升为 LevelChunk 后立即调度流体/形状刻）。
  auto postProcessCopy = proto.postProcessing();
  const int minSec = proto.minSectionY();

  // 光照快照要在搬运区段之前取：`BlockSection` 会被整体 move 进 World。
  std::optional<PersistedChunkLight> persistedLight;
  if (proto.isLightCorrect()) {
    if (LightService *light = world.lightService())
      refreshProtoChunkLight(proto, *light);
    persistedLight = extractPersistedLight(proto);
  }

  // 不在此时登记光照：方块还没导入，提交进去的会是“全空气”几何。
  world.loadChunk(pos.x, pos.z, /*registerLight=*/false);
  // `ProtoChunk::sections()` 掩盖了基类同名成员，显式走 BaseChunk 访问存储数组。
  auto &protoBase = static_cast<BaseChunk &>(proto);
  for (int secIndex = 0; secIndex < proto.sectionCount(); ++secIndex) {
    BlockSection &sec = protoBase.sections[static_cast<std::size_t>(secIndex)];
    const int sectionY = sec.sectionY();
    auto secBiomes = proto.biomes(secIndex, proto.defaultBiome());
    const bool uniformPlains =
        secBiomes.palette().size() <= 1 &&
        (secBiomes.palette().empty() ||
         secBiomes.palette().front() == "minecraft:plains");
    if (!uniformPlains) {
      auto sb = std::make_shared<SectionBiomes>();
      sb->palette = secBiomes.palette();
      sb->indices = secBiomes.indices();
      world.setSectionBiomes(pos.x, sectionY, pos.z, std::move(sb));
    }
    // 零拷贝：位宽/调色板/计数器/随机刻分桶原样移交（共享底层存储），不再按
    // 调色板重建；原型本身保持可用，直到光照引擎在其安装后切换到 World 直读。
    if (!world.adoptSectionStorage(pos.x, pos.z, sectionY, sec.share()))
      throw std::runtime_error("generated section cannot be adopted by World");
  }
  // 只重建刚导入的这一个区块：整世界重扫会让 N 个区块的导入变成 O(N^2)。
  world.primeHeightmaps(pos.x, pos.z);
  if (persistedLight)
    world.setChunkPersistedLight(pos.x, pos.z, std::move(*persistedLight));
  // 生成期已经登记过（句柄或几何）：直接接管，不重算；只有引擎里没有该区块时
  // （例如未挂载 LightService 的调用方）才重新提交。
  if (!world.adoptChunkLightDirectGeometry(pos.x, pos.z))
    world.reloadChunkLight(pos.x, pos.z);

  // 执行原版 postLoadProtoChunk 对应的 postProcessing（例如唤醒特征放置的水源/岩浆源）。
  for (std::size_t s = 0; s < postProcessCopy.size(); ++s) {
    const int secY = minSec + static_cast<int>(s);
    for (std::int16_t packedOffset : postProcessCopy[s]) {
      const Pos p =
          ProtoChunk::unpackOffsetCoordinates(packedOffset, pos, secY);
      const FluidState fs = fluidStateOf(world.getBlock(p));
      if (!fs.isEmpty())
        world.scheduleFluidTick(p, fs.getType(), 1);
    }
  }
  return true;
}

} // namespace redstone::worldgen
