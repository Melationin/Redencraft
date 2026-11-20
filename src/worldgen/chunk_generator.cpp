#include "redstone/worldgen/chunk_generator.hpp"

#include "redstone/chunk_map.hpp"
#include "redstone/engine.hpp"
#include "redstone/light/chunk_input.hpp"
#include "redstone/light/initial_chunk_lighter.hpp"
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

WorldGenRegion::WorldGenRegion(
    const ChunkStep &step, ProtoChunk &center,
    std::vector<BaseChunk *> squareCache, int radius, std::int64_t worldSeed,
    std::vector<std::shared_ptr<BaseChunk>> pinnedChunks)
    : step_(step), center_(center), cache_(std::move(squareCache)),
      pinned_(std::move(pinnedChunks)), radius_(radius),
      stride_(radius * 2 + 1), worldSeed_(worldSeed) {
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

BaseChunk *WorldGenRegion::getChunkIfAllowed(int chunkX, int chunkZ,
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
  BaseChunk *chunk = cache_[idx];
  if (!chunk)
    return nullptr;
  if (static_cast<int>(chunkPersistedStatus(*chunk)) <
      static_cast<int>(minStatus))
    return nullptr;
  return chunk;
}

BaseChunk &WorldGenRegion::getChunk(int chunkX, int chunkZ,
                                    ChunkStatusId minStatus) const {
  BaseChunk *chunk = getChunkIfAllowed(chunkX, chunkZ, minStatus);
  if (!chunk) {
    const int dx = chunkX - center_.pos().x;
    const int dz = chunkZ - center_.pos().z;
    const int dist = std::max(std::abs(dx), std::abs(dz));
    std::string detail = "Requested chunk (" + std::to_string(chunkX) + "," +
                         std::to_string(chunkZ) + ") minStatus=" +
                         std::to_string(static_cast<int>(minStatus)) +
                         " center=(" + std::to_string(center_.pos().x) + "," +
                         std::to_string(center_.pos().z) + ") stage=" +
                         std::to_string(static_cast<int>(step_.targetStatus())) +
                         " dist=" + std::to_string(dist) +
                         " depSize=" +
                         std::to_string(step_.directDependencies().size());
    if (std::abs(dx) <= radius_ && std::abs(dz) <= radius_) {
      const std::size_t idx =
          static_cast<std::size_t>(dz + radius_) * stride_ +
          static_cast<std::size_t>(dx + radius_);
      BaseChunk *raw = cache_[idx];
      detail += raw ? (" actualStatus=" +
                       std::to_string(static_cast<int>(chunkPersistedStatus(*raw))))
                    : " cache=null";
    }
    throw std::out_of_range(detail);
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
  BaseChunk &chunk = getChunk(x >> 4, z >> 4, ChunkStatusId::Empty);
  if (chunk.isWorldChunk() && chunk.resizeMutex != nullptr) {
    std::shared_lock<std::shared_mutex> lock(*chunk.resizeMutex);
    return chunk.getBlockState(x, y, z);
  }
  return chunk.getBlockState(x, y, z);
}

FluidState WorldGenRegion::getFluidState(int x, int y, int z) const {
  BaseChunk &chunk = getChunk(x >> 4, z >> 4, ChunkStatusId::Empty);
  if (chunk.isWorldChunk() && chunk.resizeMutex != nullptr) {
    std::shared_lock<std::shared_mutex> lock(*chunk.resizeMutex);
    return chunk.getFluidState(x, y, z);
  }
  return chunk.getFluidState(x, y, z);
}

bool WorldGenRegion::setBlockState(int x, int y, int z, const BlockState &state,
                                   int flags) {
  if (!ensureCanWrite(x, y, z))
    return false;
  BaseChunk &chunk = getChunk(x >> 4, z >> 4, ChunkStatusId::Empty);
  if (!chunk.isProtoChunk())
    return false;
  auto &proto = static_cast<ProtoChunk &>(chunk);
  proto.setBlockState(x, y, z, state, flags);
  if ((flags & 16) == 0 && needsPostProcess(state))
    proto.markPosForPostprocessing(x, y, z);
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
  return chunkHeight(getChunk(x >> 4, z >> 4, ChunkStatusId::Empty), type, x,
                     z);
}

ChunkGenerationPipeline::ChunkGenerationPipeline(
    ChunkGenerator &generator, std::int64_t worldSeed, RawChunkStore *diskStore,
    World *residentWorld, std::shared_ptr<LightOpticalContext> lightContext,
    GeneratedLightQueue *lightQueue, bool asyncWorldGenLight)
    : generator_(generator), worldSeed_(worldSeed), diskStore_(diskStore),
      residentWorld_(residentWorld), lightContext_(std::move(lightContext)),
      lightQueue_(lightQueue), asyncWorldGenLight_(asyncWorldGenLight) {}

BaseChunk *ChunkGenerationPipeline::findChunk(ChunkTicketPos pos) const {
  auto it = chunks_.find(pos);
  return it == chunks_.end() ? nullptr : it->second.get();
}

std::shared_ptr<ProtoChunk>
ChunkGenerationPipeline::takeProtoChunk(ChunkTicketPos pos) {
  auto it = chunks_.find(pos);
  if (it == chunks_.end())
    return nullptr;
  if (!it->second || !it->second->isProtoChunk())
    return nullptr;
  auto proto = std::static_pointer_cast<ProtoChunk>(it->second);
  it->second.reset();
  chunks_.erase(it);
  startedLoadedFromStore_.erase(pos);
  return proto;
}

BaseChunk &ChunkGenerationPipeline::getOrLoadChunk(ChunkTicketPos pos) {
  if (auto it = chunks_.find(pos); it != chunks_.end())
    return *it->second;

  if (residentWorld_ != nullptr &&
      residentWorld_->isChunkLoaded(pos.x, pos.z)) {
    if (auto holder = residentWorld_->chunkMap().findHolder(pos)) {
      if (auto shared = holder->worldShared()) {
        BaseChunk &ref = *shared;
        chunks_.emplace(pos, std::move(shared));
        startedLoadedFromStore_[pos] = true;
        return ref;
      }
    }
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
      BaseChunk &ref = *ptr;
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
  BaseChunk &ref = *proto;
  chunks_.emplace(pos, std::move(proto));
  startedLoadedFromStore_[pos] = false;
  ++stats_.chunksCreated;
  return ref;
}

BaseChunk &ChunkGenerationPipeline::ensureStatus(ChunkTicketPos pos,
                                                 ChunkStatusId targetStatus) {
  BaseChunk &chunk = getOrLoadChunk(pos);
  if (static_cast<int>(chunkPersistedStatus(chunk)) >=
      static_cast<int>(targetStatus)) {
    return chunk;
  }

  const bool fromLoaded = startedLoadedFromStore_[pos];
  const ChunkPyramid &pyramid =
      fromLoaded &&
              static_cast<int>(chunkPersistedStatus(chunk)) >=
                  static_cast<int>(ChunkStatusId::Spawn)
          ? ChunkPyramid::loadingPyramid()
          : ChunkPyramid::generationPyramid();

  while (static_cast<int>(chunkPersistedStatus(chunk)) <
         static_cast<int>(targetStatus)) {
    const ChunkStatusId nextStatus = static_cast<ChunkStatusId>(
        static_cast<int>(chunkPersistedStatus(chunk)) + 1);
    const ChunkStep &step = pyramid.getStepTo(nextStatus);
    advanceOneStep(pos, chunk, step, fromLoaded);
  }
  return chunk;
}

void ChunkGenerationPipeline::advanceOneStep(ChunkTicketPos pos,
                                             BaseChunk &chunk,
                                             const ChunkStep &step,
                                             bool) {
  if (!chunk.isProtoChunk())
    return;
  auto &proto = static_cast<ProtoChunk &>(chunk);
  const int radius = step.directDependencies().getRadius();
  const int stride = radius * 2 + 1;
  std::vector<BaseChunk *> squareCache(static_cast<std::size_t>(stride) *
                                           stride,
                                       nullptr);
  for (int dz = -radius; dz <= radius; ++dz) {
    for (int dx = -radius; dx <= radius; ++dx) {
      const int dist = std::max(std::abs(dx), std::abs(dz));
      BaseChunk *entry = nullptr;
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

  WorldGenRegion region(step, proto, std::move(squareCache), radius,
                        worldSeed_);
  std::shared_ptr<const BaseChunk> handle;
  if (auto it = chunks_.find(pos); it != chunks_.end())
    handle = it->second;
  executeChunkGenerationStep(generator_, region, proto, step.targetStatus(),
                             worldSeed_, lightContext_.get(), lightQueue_,
                             std::move(handle), asyncWorldGenLight_);
  ++stats_.stepsExecuted;
}

void executeChunkGenerationStep(ChunkGenerator &generator, WorldGenRegion &region,
                                ProtoChunk &proto, ChunkStatusId targetStatus,
                                std::int64_t worldSeed,
                                LightOpticalContext *lightContext,
                                GeneratedLightQueue *lightQueue,
                                std::shared_ptr<const BaseChunk> chunkHandle,
                                bool asyncWorldGenLight) {
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
    proto.setCachedNoiseChunkRaw(
        BaseChunk::OpaqueUniquePtr(nullptr, +[](void *) {}));
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
    if (lightContext != nullptr) {
      if (const auto optical = lightContext->get()) {
        if (asyncWorldGenLight) {
          if (lightQueue != nullptr && !proto.deferredLightSubmitted()) {
            GeneratedLightWork work;
            work.position = pos;
            work.compute = false;
            work.input = buildLightChunkInput(
                proto,
                proto.isLightCorrect()
                    ? std::optional<PersistedChunkLight>(
                          extractPersistedLight(proto))
                    : std::nullopt,
                /*deferCompute=*/!proto.isLightCorrect(),
                std::move(chunkHandle));
            lightQueue->push(std::move(work));
            proto.setDeferredLightSubmitted(true);
          }
        } else if (!proto.isLightCorrect()) {
          auto summary = computeProtoChunkLightSummary(
              proto, *optical, lightContext->skyLightEnabled());
          if (summary && summary->mutationSeq == proto.blockMutationSeq()) {
            proto.setLightSummary(std::move(summary));
          }
        }
      }
    }
    break;
  case ChunkStatusId::Light:
    if (lightContext != nullptr) {
      if (const auto optical = lightContext->get()) {
        if (asyncWorldGenLight) {
          if (proto.isLightCorrect()) {
            if (lightQueue != nullptr && !proto.deferredLightSubmitted()) {
              GeneratedLightWork work;
              work.position = pos;
              work.compute = false;
              work.input = buildLightChunkInput(
                  proto,
                  std::optional<PersistedChunkLight>(
                      extractPersistedLight(proto)),
                  /*deferCompute=*/false, std::move(chunkHandle));
              lightQueue->push(std::move(work));
              proto.setDeferredLightSubmitted(true);
            }
          } else {
            proto.setLightCorrect(false);
            if (lightQueue != nullptr) {
              if (!proto.deferredLightSubmitted()) {
                GeneratedLightWork initWork;
                initWork.position = pos;
                initWork.compute = false;
                initWork.input = buildLightChunkInput(
                    proto, std::nullopt, /*deferCompute=*/true,
                    std::move(chunkHandle));
                lightQueue->push(std::move(initWork));
                proto.setDeferredLightSubmitted(true);
              }
              GeneratedLightWork computeWork;
              computeWork.position = pos;
              computeWork.compute = true;
              lightQueue->push(std::move(computeWork));
            }
          }
          break;
        }
        const bool skyEnabled = lightContext->skyLightEnabled();
        std::shared_ptr<const PrecomputedChunkLight> precomputed =
            proto.precomputedLight();
        if (!proto.isLightCorrect()) {
          std::array<const BaseChunk *, 9> chunks3x3{};
          for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
              chunks3x3[static_cast<std::size_t>((dz + 1) * 3 + (dx + 1))] =
                  region.getChunkIfAllowed(pos.x + dx, pos.z + dz,
                                           ChunkStatusId::InitializeLight);
            }
          }
          auto computed =
              computeInitialChunkLight(chunks3x3, *optical, skyEnabled);
          proto.setPrecomputedLight(computed);
          proto.setLightCorrect(computed->isLightOn);
          precomputed = std::move(computed);
        }
        if (lightQueue != nullptr) {
          GeneratedLightWork work;
          work.position = pos;
          work.compute = false;
          if (precomputed && precomputed->isLightOn) {
            work.input = buildLightChunkInput(proto, std::nullopt,
                                              /*deferCompute=*/false,
                                              std::move(chunkHandle),
                                              std::move(precomputed));
          } else {
            work.input = buildLightChunkInput(
                proto,
                proto.isLightCorrect()
                    ? std::optional<PersistedChunkLight>(
                          extractPersistedLight(proto))
                    : std::nullopt,
                /*deferCompute=*/!proto.isLightCorrect(),
                std::move(chunkHandle));
          }
          lightQueue->push(std::move(work));
        }
      } else {
        proto.setLightCorrect(false);
      }
    } else {
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

BaseChunk *ChunkGenerationPipeline::ensureForLoadingLevel(ChunkTicketPos pos,
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
  return world.installGeneratedProtoChunk(std::move(protoPtr));
}

} // namespace redstone::worldgen
