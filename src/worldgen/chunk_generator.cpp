#include "redstone/worldgen/chunk_generator.hpp"

#include "redstone/engine.hpp"
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

GeometrySection makeGeometryFromProtoSection(const ProtoChunkSection &sec,
                                            const LightOpticalTable &optical) {
  GeometrySection g;
  const std::uint8_t airClass = optical.classForState(0);
  if (sec.hasOnlyAir()) {
    g.empty = true;
    g.uniform = true;
    g.uniformClass = airClass;
    return g;
  }
  auto cells = std::make_unique<GeometrySection::Cells>();
  std::uint16_t nonAir = 0;
  std::uint16_t unknown = 0;
  const std::uint8_t firstClass = optical.classForState(sec.blockIdAt(0));
  bool allSame = true;
  for (std::size_t i = 0; i < kLightVolume; ++i) {
    const std::uint8_t cls = optical.classForState(sec.blockIdAt(i));
    (*cells)[i] = cls;
    if (cls != firstClass)
      allSame = false;
    if (!optical[cls].air)
      ++nonAir;
    if (cls == 0)
      ++unknown;
  }
  g.empty = (nonAir == 0);
  g.nonAir = nonAir;
  g.unknown = unknown;
  if (allSame) {
    g.uniform = true;
    g.uniformClass = firstClass;
  } else {
    g.uniform = false;
    g.cells = std::move(cells);
  }
  return g;
}

PersistedChunkLight extractPersistedLight(const ProtoChunk &proto) {
  PersistedChunkLight persisted;
  persisted.isLightOn = proto.isLightCorrect();
  persisted.sections.reserve(proto.sections().size());
  for (const ProtoChunkSection &sec : proto.sections()) {
    if (!sec.blockLight().has_value() && !sec.skyLight().has_value())
      continue;
    PersistedLightSection ps;
    ps.sectionY = sec.sectionY();
    ps.blockLight = sec.blockLight().copy();
    ps.skyLight = sec.skyLight().copy();
    persisted.sections.push_back(std::move(ps));
  }
  return persisted;
}

std::uint64_t submitProtoChunkToLight(const ProtoChunk &proto,
                                      LightService &ls) {
  const auto optical = ls.opticalTable();
  if (!optical)
    return 0;
  LightChunkInput input;
  input.chunkX = proto.pos().x;
  input.chunkZ = proto.pos().z;
  input.minBlockSectionY = proto.minSectionY();
  input.blockSectionCount = proto.sectionCount();
  if (proto.isLightCorrect())
    input.persistedLight = extractPersistedLight(proto);
  input.geometrySections.reserve(proto.sections().size());
  for (const ProtoChunkSection &sec : proto.sections()) {
    input.geometrySections.push_back(
        makeGeometryFromProtoSection(sec, *optical));
  }
  return ls.submitChunk(std::move(input));
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

ChunkGenerationPipeline::ChunkGenerationPipeline(ChunkGenerator &generator,
                                                 std::int64_t worldSeed,
                                                 RawChunkStore *diskStore,
                                                 World *residentWorld,
                                                 LightService *lightService)
    : generator_(generator), worldSeed_(worldSeed), diskStore_(diskStore),
      residentWorld_(residentWorld), lightService_(lightService) {}

LightService *ChunkGenerationPipeline::activeLightService() const noexcept {
  if (lightService_ != nullptr)
    return lightService_;
  return residentWorld_ != nullptr ? residentWorld_->lightService() : nullptr;
}

ChunkAccess *ChunkGenerationPipeline::findChunk(ChunkTicketPos pos) const {
  auto it = chunks_.find(pos);
  return it == chunks_.end() ? nullptr : it->second.get();
}

std::optional<ProtoChunk>
ChunkGenerationPipeline::takeProtoChunk(ChunkTicketPos pos) {
  auto it = chunks_.find(pos);
  if (it == chunks_.end())
    return std::nullopt;
  auto *proto = dynamic_cast<ProtoChunk *>(it->second.get());
  if (!proto)
    return std::nullopt;
  ProtoChunk result = std::move(*proto);
  chunks_.erase(it);
  startedLoadedFromStore_.erase(pos);
  return result;
}

ChunkAccess &ChunkGenerationPipeline::getOrLoadChunk(ChunkTicketPos pos) {
  if (auto it = chunks_.find(pos); it != chunks_.end())
    return *it->second;

  if (residentWorld_ != nullptr &&
      residentWorld_->isChunkLoaded(pos.x, pos.z)) {
    auto imposter = std::make_unique<ImposterProtoChunk>(*residentWorld_, pos,
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
      auto ptr = std::make_unique<ProtoChunk>(std::move(*loaded));
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

  auto proto = std::make_unique<ProtoChunk>(pos, generator_.chunkMinY(),
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
    executeChunkGenerationStep(generator_, region, *proto, step.targetStatus(),
                               worldSeed_, activeLightService());
  }
  ++stats_.stepsExecuted;
}

void executeChunkGenerationStep(ChunkGenerator &generator, WorldGenRegion &region,
                                ProtoChunk &proto, ChunkStatusId targetStatus,
                                std::int64_t worldSeed,
                                LightService *lightService) {
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
    if (lightService != nullptr) {
      proto.setLightSubmissionGeneration(
          submitProtoChunkToLight(proto, *lightService));
    }
    break;
  case ChunkStatusId::Light:
    if (lightService != nullptr) {
      auto waitAndDrain = [&]() {
        const std::uint64_t generation = proto.lightSubmissionGeneration();
        const bool perChunkWait =
            std::getenv("REDSTONE_LIGHT_PER_CHUNK") != nullptr;
        if (generation != 0 && perChunkWait)
          (void)lightService->waitForChunk(
              pos.x, pos.z, generation, std::chrono::milliseconds{30000});
        else
          lightService->flushAndWait();
        lightService->drainPublished();
      };
      waitAndDrain();
      bool settled = lightService->isChunkSettled(pos.x, pos.z);
      // 兜底重投：快照不完整时再投一次本区块，让跨区块边界重新传播。
      for (int attempt = 0; !settled && attempt < 2; ++attempt) {
        proto.setLightSubmissionGeneration(
            submitProtoChunkToLight(proto, *lightService));
        waitAndDrain();
        settled = lightService->isChunkSettled(pos.x, pos.z);
      }
      // 光照数据只保留在光照引擎里（可变、可被邻居继续更新），ProtoChunk 不再
      // 拷贝副本；写存档/晋升前用 refreshProtoChunkLight() 现取一次。
      proto.setLightCorrect(settled);
    } else {
      // TODO(G-6): 未挂载 LightService 时无法执行真实光照传播，
      // 保持 lightCorrect=false 而不伪造已完成光照快照。
      proto.setLightCorrect(false);
    }
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
  auto protoOpt = takeProtoChunk(pos);
  if (!protoOpt)
    return world.isChunkLoaded(pos.x, pos.z);
  return installProtoChunkIntoWorld(std::move(*protoOpt), world);
}

void refreshProtoChunkLight(ProtoChunk &proto, LightService &lightService) {
  if (!proto.isLightCorrect())
    return;
  lightService.drainPublished();
  const auto blockSnap = lightService.latestLayerChunk(
      LightLayer::Block, proto.pos().x, proto.pos().z);
  const auto skySnap = lightService.latestLayerChunk(
      LightLayer::Sky, proto.pos().x, proto.pos().z);
  for (int s = 0; s < proto.sectionCount(); ++s) {
    ProtoChunkSection &sec = proto.section(s);
    const int secY = sec.sectionY();
    copySnapshotLayerToOptional(blockSnap ? blockSnap->section(secY) : nullptr,
                                LightLayer::Block, sec.blockLight());
    copySnapshotLayerToOptional(skySnap ? skySnap->section(secY) : nullptr,
                                LightLayer::Sky, sec.skyLight());
  }
}

bool installProtoChunkIntoWorld(ProtoChunk &&protoInput, World &world) {
  ProtoChunk proto = std::move(protoInput);
  const ChunkTicketPos pos = proto.pos();
  // 备份待后处理坐标（用于晋升为 LevelChunk 后立即调度流体/形状刻）。
  auto postProcessCopy = proto.postProcessing();
  const int minSec = proto.minSectionY();

  world.loadChunk(pos.x, pos.z);
  for (const ProtoChunkSection &sec : proto.sections()) {
    const auto &secBiomes = sec.biomes();
    const bool uniformPlains =
        secBiomes.palette().size() <= 1 &&
        (secBiomes.palette().empty() ||
         secBiomes.palette().front() == "minecraft:plains");
    if (!uniformPlains) {
      auto sb = std::make_shared<SectionBiomes>();
      sb->palette = secBiomes.palette();
      sb->indices = secBiomes.indices();
      world.setSectionBiomes(pos.x, sec.sectionY(), pos.z, std::move(sb));
    }
    if (sec.hasOnlyAir())
      continue;
    std::vector<BlockState::Id> palette;
    std::array<std::uint16_t, 4096> indices{};
    std::map<BlockState::Id, std::uint16_t> mapIdx;
    for (int ly = 0; ly < 16; ++ly) {
      for (int lz = 0; lz < 16; ++lz) {
        for (int lx = 0; lx < 16; ++lx) {
          const BlockState st = sec.getBlockState(lx, ly, lz);
          auto it = mapIdx.find(st.id());
          if (it == mapIdx.end()) {
            const std::uint16_t nextIdx =
                static_cast<std::uint16_t>(palette.size());
            palette.push_back(st.id());
            it = mapIdx.emplace(st.id(), nextIdx).first;
          }
          const std::size_t cell =
              static_cast<std::size_t>((ly << 8) | (lz << 4) | lx);
          indices[cell] = it->second;
        }
      }
    }
    world.importSectionBlocks(pos.x, pos.z, sec.sectionY(), palette, indices);
  }
  // 只重建刚导入的这一个区块：整世界重扫会让 N 个区块的导入变成 O(N^2)。
  world.primeHeightmaps(pos.x, pos.z);
  if (proto.isLightCorrect()) {
    if (LightService *light = world.lightService())
      refreshProtoChunkLight(proto, *light);
    world.setChunkPersistedLight(pos.x, pos.z, extractPersistedLight(proto));
    // 只重投当前区块：refreshLightSnapshots() 会重投所有已加载区块，
    // 逐区块晋升时是 O(N^2) 次提交（实测 50x50 会因此 OOM）。
    world.reloadChunkLight(pos.x, pos.z);
  }

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
