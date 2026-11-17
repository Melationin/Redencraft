#include "redstone/worldgen/noise_chunk_generator.hpp"

#include "redstone/engine.hpp"
#include "redstone/worldgen/jigsaw.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace redstone::worldgen {
namespace {

struct ChunkStructureData {
  std::map<std::string, StructureStart, std::less<>> starts;
  std::map<std::string, std::vector<ChunkTicketPos>, std::less<>> references;
};

ChunkStructureData &ensureChunkStructureData(ChunkAccess &chunk) {
  if (auto cached = chunk.structureStartsOpaque()) {
    return *std::static_pointer_cast<ChunkStructureData>(cached);
  }
  auto data = std::make_shared<ChunkStructureData>();
  ChunkStructureData &ref = *data;
  chunk.setStructureStartsOpaque(std::move(data));
  return ref;
}

const ChunkStructureData *findChunkStructureData(const ChunkAccess &chunk) {
  auto cached = chunk.structureStartsOpaque();
  return cached ? std::static_pointer_cast<const ChunkStructureData>(cached).get()
                : nullptr;
}

int floorDivInt(int a, int b) noexcept {
  int d = a / b;
  int r = a % b;
  if (r != 0 && ((r ^ b) < 0))
    --d;
  return d;
}

} // namespace

NoiseBasedChunkGenerator::NoiseBasedChunkGenerator(
    const WorldgenRegistries &registries, NoiseGeneratorSettings settings,
    std::shared_ptr<const BiomeSource> biomeSource)
    : registries_(registries), settings_(std::move(settings)),
      biomeSource_(std::move(biomeSource)),
      globalFluidPicker_(createFluidPicker(settings_)) {
  if (!biomeSource_) {
    throw std::invalid_argument(
        "NoiseBasedChunkGenerator requires non-null BiomeSource");
  }
  featuresPerStep_ =
      buildFeaturesPerStep(registries_, biomeSource_->possibleBiomes());
}

NoiseBasedChunkGenerator::~NoiseBasedChunkGenerator() = default;

FluidPicker NoiseBasedChunkGenerator::createFluidPicker(
    const NoiseGeneratorSettings &settings) {
  BlockState lava = BlockState("minecraft:lava");
  FluidStatus lavaStatus{-54, lava};
  const int seaLevel = settings.seaLevel;
  FluidStatus seaStatus{seaLevel, settings.defaultFluidState};
  const int cutoff = std::min(-54, seaLevel);
  return [lavaStatus, seaStatus, cutoff](int, int blockY, int) -> FluidStatus {
    return blockY < cutoff ? lavaStatus : seaStatus;
  };
}

RandomState &
NoiseBasedChunkGenerator::randomStateForSeed(std::int64_t worldSeed) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = randomStates_.find(worldSeed);
  if (it != randomStates_.end())
    return *it->second;
  auto rs = std::make_unique<RandomState>(
      RandomState::create(settings_, registries_, worldSeed));
  RandomState &ref = *rs;
  randomStates_.emplace(worldSeed, std::move(rs));
  return ref;
}

ChunkGeneratorStructureState &
NoiseBasedChunkGenerator::structureStateForSeed(std::int64_t worldSeed) const {
  RandomState &rs = randomStateForSeed(worldSeed);
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = structureStates_.find(worldSeed);
  if (it != structureStates_.end())
    return *it->second;
  auto st = std::make_unique<ChunkGeneratorStructureState>(
      worldSeed, worldSeed, registries_, *biomeSource_, rs.sampler());
  ChunkGeneratorStructureState &ref = *st;
  structureStates_.emplace(worldSeed, std::move(st));
  return ref;
}

void NoiseBasedChunkGenerator::createStructures(ProtoChunk &chunk,
                                                std::int64_t worldSeed) {
  RandomState &rs = randomStateForSeed(worldSeed);
  ChunkGeneratorStructureState &structState = structureStateForSeed(worldSeed);
  ChunkStructureData &chunkData = ensureChunkStructureData(chunk);
  const ChunkTicketPos chunkPos = chunk.pos();

  for (const StructureSetDefinition *set : structState.possibleStructureSets()) {
    bool alreadyHasValid = false;
    for (const auto &entry : set->structures) {
      if (auto it = chunkData.starts.find(entry.structureId);
          it != chunkData.starts.end() && it->second.isValid()) {
        alreadyHasValid = true;
        break;
      }
    }
    if (alreadyHasValid)
      continue;

    if (!structState.isStructureChunk(*set, chunkPos.x, chunkPos.z))
      continue;

    auto tryGenerate = [&](const std::string &structId) -> bool {
      const StructureDefinition *def = registries_.findStructure(structId);
      if (!def)
        return false;
      auto startOpt = generateStructureStart(
          *def, *this, rs, structState, worldSeed, chunkPos, settings_.minY,
          settings_.height);
      if (startOpt && startOpt->isValid()) {
        chunkData.starts[structId] = std::move(*startOpt);
        return true;
      }
      return false;
    };

    if (set->structures.size() == 1) {
      (void)tryGenerate(set->structures.front().structureId);
    } else if (!set->structures.empty()) {
      std::vector<StructureSelectionEntry> options = set->structures;
      LegacyRandomSource rng(0);
      worldgen_random::setLargeFeatureSeed(rng, worldSeed, chunkPos.x,
                                           chunkPos.z);
      int totalWeight = 0;
      for (const auto &opt : options)
        totalWeight += opt.weight;
      while (!options.empty() && totalWeight > 0) {
        int roll = rng.nextInt(totalWeight);
        std::size_t idx = 0;
        for (; idx < options.size(); ++idx) {
          roll -= options[idx].weight;
          if (roll < 0)
            break;
        }
        if (idx >= options.size())
          break;
        const StructureSelectionEntry selected = options[idx];
        if (tryGenerate(selected.structureId))
          break;
        options.erase(options.begin() + static_cast<std::ptrdiff_t>(idx));
        totalWeight -= selected.weight;
      }
    }
  }
}

void NoiseBasedChunkGenerator::createReferences(WorldGenRegion &region,
                                                ProtoChunk &chunk) {
  const ChunkTicketPos center = chunk.pos();
  const int bx0 = center.x << 4;
  const int bz0 = center.z << 4;
  ChunkStructureData &targetData = ensureChunkStructureData(chunk);

  for (int sx = center.x - 8; sx <= center.x + 8; ++sx) {
    for (int sz = center.z - 8; sz <= center.z + 8; ++sz) {
      const ChunkAccess *srcChunk =
          region.getChunkIfAllowed(sx, sz, ChunkStatusId::StructureStarts);
      if (!srcChunk)
        continue;
      const ChunkStructureData *srcData = findChunkStructureData(*srcChunk);
      if (!srcData)
        continue;
      for (const auto &[structId, start] : srcData->starts) {
        if (!start.isValid())
          continue;
        const BoundingBox bb = start.calculateBoundingBox();
        if (bb.intersectsXZ(bx0, bz0, bx0 + 15, bz0 + 15)) {
          auto &refs = targetData.references[structId];
          const ChunkTicketPos srcPos{sx, sz};
          if (std::find(refs.begin(), refs.end(), srcPos) == refs.end()) {
            refs.push_back(srcPos);
          }
        }
      }
    }
  }
}

std::shared_ptr<NoiseChunk>
NoiseBasedChunkGenerator::getOrCreateNoiseChunk(ChunkAccess &chunk,
                                                RandomState &rs,
                                                const WorldGenRegion *region,
                                                bool requireTerrain) const {
  std::shared_ptr<NoiseChunk> cached;
  if (auto opaque = chunk.cachedNoiseChunkOpaque()) {
    cached = std::static_pointer_cast<NoiseChunk>(opaque);
    if (!requireTerrain || cached->isTerrainInitialized())
      return cached;
  }

  DensityNodePtr beardifierNode;
  if (requireTerrain && region != nullptr) {
    std::vector<const StructureStart *> starts;
    const ChunkTicketPos cpos = chunk.pos();
    for (int sx = cpos.x - 8; sx <= cpos.x + 8; ++sx) {
      for (int sz = cpos.z - 8; sz <= cpos.z + 8; ++sz) {
        const ChunkAccess *src =
            region->getChunkIfAllowed(sx, sz, ChunkStatusId::StructureStarts);
        if (!src)
          continue;
        const ChunkStructureData *sd = findChunkStructureData(*src);
        if (!sd)
          continue;
        for (const auto &[_, start] : sd->starts) {
          if (start.isValid())
            starts.push_back(&start);
        }
      }
    }
    if (!starts.empty()) {
      auto bf = Beardifier::forChunk(starts, cpos);
      if (!bf->isEmpty()) {
        beardifierNode = density_functions::custom(std::move(bf), -1e9, 1e9);
      }
    }
  }

  if (cached) {
    cached->ensureTerrainInitialized(rs, settings_, globalFluidPicker_,
                                     std::move(beardifierNode));
    return cached;
  }

  std::shared_ptr<NoiseChunk> created =
      requireTerrain
          ? NoiseChunk::forChunk(chunk, rs, settings_, globalFluidPicker_,
                                 std::move(beardifierNode))
          : NoiseChunk::forChunkBiomes(chunk, rs, settings_);
  chunk.setCachedNoiseChunkOpaque(created);
  return created;
}

void NoiseBasedChunkGenerator::createBiomes(WorldGenRegion &region,
                                            ProtoChunk &chunk,
                                            std::int64_t worldSeed) {
  (void)region;
  const int sectionCount = static_cast<int>(chunk.sections().size());
  if (const auto *fbs =
          dynamic_cast<const FixedBiomeSource *>(biomeSource_.get())) {
    const ClimateSampler emptySampler{};
    const std::string_view fixedBiome =
        fbs->getNoiseBiome(0, 0, 0, emptySampler);
    for (int secIdx = 0; secIdx < sectionCount; ++secIdx)
      chunk.section(secIdx).biomes().fill(fixedBiome);
    return;
  }

  RandomState &rs = randomStateForSeed(worldSeed);
  std::shared_ptr<NoiseChunk> nc =
      getOrCreateNoiseChunk(chunk, rs, nullptr, /*requireTerrain=*/false);
  ClimateSampler sampler =
      nc->cachedClimateSampler(rs.router(), settings_.spawnTarget);

  const int quartMinX = chunk.pos().x << 2;
  const int quartMinZ = chunk.pos().z << 2;
  const int minSectionY = chunk.minSectionY();
  const std::uint8_t yMask = sampler.yDependentMask;

  if (const auto *mnbs =
          dynamic_cast<const MultiNoiseBiomeSource *>(biomeSource_.get())) {
    for (int qx = 0; qx < 4; ++qx) {
      const int quartX = quartMinX + qx;
      const int blockX = quartX * 4;
      for (int qz = 0; qz < 4; ++qz) {
        const int quartZ = quartMinZ + qz;
        const int blockZ = quartZ * 4;
        const int minBlockY = (minSectionY << 4);

        const float baseT =
            ((yMask & (1u << 0u)) == 0 && sampler.temperature)
                ? static_cast<float>(
                      sampler.temperature->compute(blockX, minBlockY, blockZ))
                : 0.0f;
        const float baseH =
            ((yMask & (1u << 1u)) == 0 && sampler.humidity)
                ? static_cast<float>(
                      sampler.humidity->compute(blockX, minBlockY, blockZ))
                : 0.0f;
        const float baseC =
            ((yMask & (1u << 2u)) == 0 && sampler.continentalness)
                ? static_cast<float>(sampler.continentalness->compute(
                      blockX, minBlockY, blockZ))
                : 0.0f;
        const float baseE =
            ((yMask & (1u << 3u)) == 0 && sampler.erosion)
                ? static_cast<float>(
                      sampler.erosion->compute(blockX, minBlockY, blockZ))
                : 0.0f;
        const float baseD =
            ((yMask & (1u << 4u)) == 0 && sampler.depth)
                ? static_cast<float>(
                      sampler.depth->compute(blockX, minBlockY, blockZ))
                : 0.0f;
        const float baseW =
            ((yMask & (1u << 5u)) == 0 && sampler.weirdness)
                ? static_cast<float>(
                      sampler.weirdness->compute(blockX, minBlockY, blockZ))
                : 0.0f;

        const ClimateTargetPoint basePoint =
            ClimateTargetPoint::make(baseT, baseH, baseC, baseE, baseD, baseW);
        if (yMask == 0) {
          const std::string_view biomeId = mnbs->getNoiseBiome(basePoint);
          for (int secIdx = 0; secIdx < sectionCount; ++secIdx) {
            auto &biomes = chunk.section(secIdx).biomes();
            for (int qy = 0; qy < 4; ++qy)
              biomes.set(qx, qy, qz, biomeId);
          }
          continue;
        }

        for (int secIdx = 0; secIdx < sectionCount; ++secIdx) {
          auto &biomes = chunk.section(secIdx).biomes();
          const int quartMinY = (minSectionY + secIdx) << 2;
          for (int qy = 0; qy < 4; ++qy) {
            const int blockY = (quartMinY + qy) * 4;
            ClimateTargetPoint pt = basePoint;
            if ((yMask & (1u << 0u)) && sampler.temperature)
              pt.temperature = quantizeClimateCoord(static_cast<float>(
                  sampler.temperature->compute(blockX, blockY, blockZ)));
            if ((yMask & (1u << 1u)) && sampler.humidity)
              pt.humidity = quantizeClimateCoord(static_cast<float>(
                  sampler.humidity->compute(blockX, blockY, blockZ)));
            if ((yMask & (1u << 2u)) && sampler.continentalness)
              pt.continentalness = quantizeClimateCoord(static_cast<float>(
                  sampler.continentalness->compute(blockX, blockY, blockZ)));
            if ((yMask & (1u << 3u)) && sampler.erosion)
              pt.erosion = quantizeClimateCoord(static_cast<float>(
                  sampler.erosion->compute(blockX, blockY, blockZ)));
            if ((yMask & (1u << 4u)) && sampler.depth)
              pt.depth = quantizeClimateCoord(static_cast<float>(
                  sampler.depth->compute(blockX, blockY, blockZ)));
            if ((yMask & (1u << 5u)) && sampler.weirdness)
              pt.weirdness = quantizeClimateCoord(static_cast<float>(
                  sampler.weirdness->compute(blockX, blockY, blockZ)));
            biomes.set(qx, qy, qz, mnbs->getNoiseBiome(pt));
          }
        }
      }
    }
    return;
  }

  for (int qx = 0; qx < 4; ++qx) {
    for (int qz = 0; qz < 4; ++qz) {
      for (int secIdx = 0; secIdx < sectionCount; ++secIdx) {
        ProtoChunkSection &section = chunk.section(secIdx);
        const int quartMinY = (minSectionY + secIdx) << 2;
        for (int qy = 0; qy < 4; ++qy) {
          const std::string_view biomeId = biomeSource_->getNoiseBiome(
              quartMinX + qx, quartMinY + qy, quartMinZ + qz, sampler);
          section.biomes().set(qx, qy, qz, biomeId);
        }
      }
    }
  }
}

namespace {

template <int FixedW, int FixedH>
void fillFromNoiseCells(NoiseChunk &nc, ProtoChunk &chunk,
                        const BlockState &defaultBlock, int runtimeW,
                        int runtimeH) {
  const int cellW = FixedW > 0 ? FixedW : runtimeW;
  const int cellH = FixedH > 0 ? FixedH : runtimeH;
  const int cellCountXZ = nc.cellCountXZ();
  const int cellCountY = nc.cellCountY();
  const int cellMinY = nc.cellNoiseMinY();
  const int chunkStartBlockX = chunk.pos().x << 4;
  const int chunkStartBlockZ = chunk.pos().z << 4;

  Aquifer &aquifer = nc.aquifer();
  const bool scheduleFluid = aquifer.shouldScheduleFluidUpdate();
  const bool defaultIsAir = defaultBlock.isAir();
  const bool defaultBlocksMotion = blocksMotion(defaultBlock);
  const bool defaultIsFluid =
      scheduleFluid && !defaultIsAir && !fluidStateOf(defaultBlock).isEmpty();
  const bool sectionAligned =
      (FixedH == 4 || FixedH == 8) && ((chunk.minY() & 15) == 0);

  Heightmap &oceanFloor =
      chunk.getOrCreateHeightmapUnprimed(HeightmapType::OceanFloorWG);
  Heightmap &worldSurface =
      chunk.getOrCreateHeightmapUnprimed(HeightmapType::WorldSurfaceWG);

  const double invW =
      FixedW > 0 ? (1.0 / static_cast<double>(FixedW > 0 ? FixedW : 1))
                 : (1.0 / static_cast<double>(runtimeW));
  const double invH =
      FixedH > 0 ? (1.0 / static_cast<double>(FixedH > 0 ? FixedH : 1))
                 : (1.0 / static_cast<double>(runtimeH));

  constexpr int kMaxStackCellW = FixedW > 0 ? FixedW : 16;

  for (int cellXIndex = 0; cellXIndex < cellCountXZ; ++cellXIndex) {
    nc.advanceCellX(cellXIndex);
    for (int cellZIndex = 0; cellZIndex < cellCountXZ; ++cellZIndex) {
      int surfTop[kMaxStackCellW][kMaxStackCellW];
      int oceanTop[kMaxStackCellW][kMaxStackCellW];
      bool hasSurf[kMaxStackCellW][kMaxStackCellW]{};
      bool hasOcean[kMaxStackCellW][kMaxStackCellW]{};
      const bool useStackHm = (cellW <= kMaxStackCellW);

      int lastSectionIndex = static_cast<int>(chunk.sections().size()) - 1;
      ProtoChunkSection *section = &chunk.section(lastSectionIndex);
      auto blockGetter = [&chunk](int lx, int py, int lz) {
        return chunk.getBlockState(lx, py, lz);
      };

      const bool fastCell =
          !nc.hasPostDensityInterpolators() && nc.hasDirectFinalCellCache();
      const std::span<const double> cellVals = nc.directFinalCellValues();

      for (int cellYIndex = cellCountY - 1; cellYIndex >= 0; --cellYIndex) {
        nc.selectCellYZ(cellYIndex, cellZIndex);
        const int basePosY = (cellMinY + cellYIndex) * cellH;
        if constexpr (FixedH == 4 || FixedH == 8) {
          if (sectionAligned) {
            section = &chunk.section(chunk.sectionIndex(basePosY));
          }
        }
        std::size_t cellIdx = 0;
        for (int yInCell = cellH - 1; yInCell >= 0; --yInCell) {
          const int posY = basePosY + yInCell;
          const int localY = posY & 15;
          if (!sectionAligned) {
            const int sectionIndex = chunk.sectionIndex(posY);
            if (lastSectionIndex != sectionIndex) {
              lastSectionIndex = sectionIndex;
              section = &chunk.section(sectionIndex);
            }
          }
          if (!fastCell) {
            const double factorY = FixedH > 0
                                       ? static_cast<double>(yInCell) * invH
                                       : static_cast<double>(yInCell) /
                                             static_cast<double>(cellH);
            nc.updateForY(posY, factorY);
          }
          for (int xInCell = 0; xInCell < cellW; ++xInCell) {
            const int localX = (cellXIndex * cellW + xInCell) & 15;
            const int posX = chunkStartBlockX + cellXIndex * cellW + xInCell;
            if (!fastCell) {
              const double factorX = FixedW > 0
                                         ? static_cast<double>(xInCell) * invW
                                         : static_cast<double>(xInCell) /
                                               static_cast<double>(cellW);
              nc.updateForX(posX, factorX);
            }
            for (int zInCell = 0; zInCell < cellW; ++zInCell) {
              const int localZ = (cellZIndex * cellW + zInCell) & 15;
              const int posZ = chunkStartBlockZ + cellZIndex * cellW + zInCell;
              std::optional<BlockState> optState;
              if (fastCell) {
                nc.advanceVoxelCounter();
                optState = nc.computeSubstanceFast(posX, posY, posZ,
                                                   cellVals[cellIdx++]);
              } else {
                const double factorZ = FixedW > 0
                                           ? static_cast<double>(zInCell) * invW
                                           : static_cast<double>(zInCell) /
                                                 static_cast<double>(cellW);
                nc.updateForZ(posZ, factorZ);
                optState = nc.getInterpolatedState();
              }
              if (!optState.has_value()) {
                if (defaultIsAir)
                  continue;
                section->setBlockState(localX, localY, localZ, defaultBlock);
                if (useStackHm) {
                  if (!hasSurf[xInCell][zInCell]) {
                    hasSurf[xInCell][zInCell] = true;
                    surfTop[xInCell][zInCell] = posY + 1;
                  }
                  if (defaultBlocksMotion && !hasOcean[xInCell][zInCell]) {
                    hasOcean[xInCell][zInCell] = true;
                    oceanTop[xInCell][zInCell] = posY + 1;
                  }
                } else {
                  oceanFloor.update(localX, posY, localZ, defaultBlock,
                                    blockGetter);
                  worldSurface.update(localX, posY, localZ, defaultBlock,
                                      blockGetter);
                }
                if (defaultIsFluid) {
                  chunk.markPosForPostprocessing(posX, posY, posZ);
                }
              } else {
                const BlockState &state = *optState;
                if (state.isAir())
                  continue;
                section->setBlockState(localX, localY, localZ, state);
                if (useStackHm) {
                  if (!hasSurf[xInCell][zInCell]) {
                    hasSurf[xInCell][zInCell] = true;
                    surfTop[xInCell][zInCell] = posY + 1;
                  }
                  if (!hasOcean[xInCell][zInCell] && blocksMotion(state)) {
                    hasOcean[xInCell][zInCell] = true;
                    oceanTop[xInCell][zInCell] = posY + 1;
                  }
                } else {
                  oceanFloor.update(localX, posY, localZ, state, blockGetter);
                  worldSurface.update(localX, posY, localZ, state, blockGetter);
                }
                if (scheduleFluid && !fluidStateOf(state).isEmpty()) {
                  chunk.markPosForPostprocessing(posX, posY, posZ);
                }
              }
            }
          }
        }
      }

      if (useStackHm) {
        for (int xInCell = 0; xInCell < cellW; ++xInCell) {
          const int localX = (cellXIndex * cellW + xInCell) & 15;
          for (int zInCell = 0; zInCell < cellW; ++zInCell) {
            const int localZ = (cellZIndex * cellW + zInCell) & 15;
            if (hasSurf[xInCell][zInCell]) {
              worldSurface.raiseHeight(localX, localZ, surfTop[xInCell][zInCell]);
            }
            if (hasOcean[xInCell][zInCell]) {
              oceanFloor.raiseHeight(localX, localZ, oceanTop[xInCell][zInCell]);
            }
          }
        }
      }
    }
    nc.swapSlices();
  }
}

} // namespace

void NoiseBasedChunkGenerator::fillFromNoise(WorldGenRegion &region,
                                             ProtoChunk &chunk,
                                             std::int64_t worldSeed) {
  RandomState &rs = randomStateForSeed(worldSeed);
  std::shared_ptr<NoiseChunk> nc = getOrCreateNoiseChunk(chunk, rs, &region);

  nc->initializeForFirstCellX();
  const int cellWidth = nc->cellWidth();
  const int cellHeight = nc->cellHeight();
  const BlockState &defaultBlock = settings_.defaultBlockState;

  if (cellWidth == 4 && cellHeight == 8) {
    fillFromNoiseCells<4, 8>(*nc, chunk, defaultBlock, 4, 8);
  } else if (cellWidth == 8 && cellHeight == 4) {
    fillFromNoiseCells<8, 4>(*nc, chunk, defaultBlock, 8, 4);
  } else if (cellWidth == 4 && cellHeight == 4) {
    fillFromNoiseCells<4, 4>(*nc, chunk, defaultBlock, 4, 4);
  } else {
    fillFromNoiseCells<0, 0>(*nc, chunk, defaultBlock, cellWidth, cellHeight);
  }
  nc->stopInterpolation();
}

void NoiseBasedChunkGenerator::buildSurface(WorldGenRegion &region,
                                            ProtoChunk &chunk,
                                            std::int64_t worldSeed) {
  RandomState &rs = randomStateForSeed(worldSeed);
  std::shared_ptr<NoiseChunk> nc = getOrCreateNoiseChunk(chunk, rs, &region);
  BiomeManager biomeManager(BiomeManager::obfuscateSeed(worldSeed));
  BiomeManager::NoiseBiomeReader reader =
      [&region, &rs, this](int qx, int qy, int qz) -> std::string_view {
    if (auto *c =
            region.getChunkIfAllowed(qx >> 2, qz >> 2, ChunkStatusId::Biomes)) {
      return c->getNoiseBiome(qx, qy, qz);
    }
    return biomeSource_->getNoiseBiome(qx, qy, qz, rs.sampler());
  };
  rs.surfaceSystem(settings_).buildSurface(
      rs, biomeManager, reader, registries_, settings_.legacyRandomSource,
      settings_.minY, settings_.height, chunk, *nc, settings_.surfaceRule);
}

void NoiseBasedChunkGenerator::applyCarvers(WorldGenRegion &region,
                                            ProtoChunk &chunk,
                                            std::int64_t worldSeed) {
  RandomState &rs = randomStateForSeed(worldSeed);
  std::shared_ptr<NoiseChunk> nc = getOrCreateNoiseChunk(chunk, rs, &region);
  Aquifer &aquifer = nc->aquifer();
  BiomeManager correctBiomeManager(BiomeManager::obfuscateSeed(worldSeed));
  BiomeManager::NoiseBiomeReader sourceReader =
      [this, &rs](int qx, int qy, int qz) -> std::string_view {
    return biomeSource_->getNoiseBiome(qx, qy, qz, rs.sampler());
  };
  WorldGenerationContext genCtx{settings_.minY, settings_.height};
  CarvingMask &mask = chunk.getOrCreateCarvingMask();
  LegacyRandomSource carverRng(0);

  for (int dx = -8; dx <= 8; ++dx) {
    for (int dz = -8; dz <= 8; ++dz) {
      const ChunkTicketPos sourcePos{chunk.pos().x + dx, chunk.pos().z + dz};
      const std::string_view sourceBiome = biomeSource_->getNoiseBiome(
          sourcePos.x << 2, 0, sourcePos.z << 2, rs.sampler());
      const BiomeGenerationSettings *bgi =
          registries_.findBiomeGeneration(sourceBiome);
      if (!bgi)
        continue;
      for (std::size_t index = 0; index < bgi->carvers.size(); ++index) {
        const ConfiguredCarver *carver =
            registries_.findCarver(bgi->carvers[index]);
        if (!carver)
          continue;
        worldgen_random::setLargeFeatureSeed(
            carverRng, worldSeed + static_cast<std::int64_t>(index),
            sourcePos.x, sourcePos.z);
        if (carver->isStartChunk(carverRng)) {
          carver->carve(genCtx, settings_, rs, correctBiomeManager, sourceReader,
                        registries_, chunk, *nc, carverRng, aquifer, sourcePos,
                        mask);
        }
      }
    }
  }
}

void NoiseBasedChunkGenerator::applyBiomeDecoration(WorldGenRegion &region,
                                                    ProtoChunk &chunk,
                                                    std::int64_t worldSeed) {
  const ChunkTicketPos center = chunk.pos();
  const int originX = center.x << 4;
  const int originZ = center.z << 4;
  const BoundingBox writableArea{originX, settings_.minY + 1, originZ,
                                 originX + 15, settings_.minY + settings_.height - 1,
                                 originZ + 15};

  WorldgenRandom rng = WorldgenRandom::xoroshiro(0);
  const std::int64_t decorationSeed =
      worldgen_random::setDecorationSeed(rng, worldSeed, originX, originZ);

  // 对齐 `ChunkGenerator.applyBiomeDecoration`：收集中心及 8 邻域区块内的全群系集合（仅保留本维度 BiomeSource 可能生成的群系）。
  const auto allowedList = biomeSource_->possibleBiomes();
  std::set<std::string_view> allowedSet(allowedList.begin(), allowedList.end());
  std::set<std::string_view> presentBiomes;
  for (int dx = -1; dx <= 1; ++dx) {
    for (int dz = -1; dz <= 1; ++dz) {
      const ChunkAccess *nb = region.getChunkIfAllowed(
          center.x + dx, center.z + dz, ChunkStatusId::Biomes);
      if (!nb)
        continue;
      const int secCount = nb->sectionCount();
      const int minSecY = nb->minSectionY();
      const int qx0 = (center.x + dx) << 2;
      const int qz0 = (center.z + dz) << 2;
      for (int s = 0; s < secCount; ++s) {
        const int qy0 = (minSecY + s) << 2;
        for (int qx = 0; qx < 4; ++qx) {
          for (int qy = 0; qy < 4; ++qy) {
            for (int qz = 0; qz < 4; ++qz) {
              const std::string_view b =
                  nb->getNoiseBiome(qx0 + qx, qy0 + qy, qz0 + qz);
              if (allowedSet.count(b))
                presentBiomes.insert(b);
            }
          }
        }
      }
    }
  }

  const ChunkStructureData *refData = findChunkStructureData(chunk);
  const auto *templates = registries_.templateCache();
  const std::size_t genSteps =
      std::max(kDecorationStepCount, featuresPerStep_.size());

  for (std::size_t step = 0; step < genSteps; ++step) {
    int structFeatureIndex = 0;
    if (refData && templates) {
      for (const auto &[structId, def] : registries_.structures()) {
        if (static_cast<std::size_t>(def.step) != step)
          continue;
        worldgen_random::setFeatureSeed(rng, decorationSeed, structFeatureIndex,
                                        static_cast<int>(step));
        auto refIt = refData->references.find(structId);
        if (refIt != refData->references.end()) {
          for (const ChunkTicketPos &startChunkPos : refIt->second) {
            const ChunkAccess *startChunk = region.getChunkIfAllowed(
                startChunkPos.x, startChunkPos.z, ChunkStatusId::StructureStarts);
            if (!startChunk)
              continue;
            const ChunkStructureData *sd = findChunkStructureData(*startChunk);
            if (!sd)
              continue;
            auto startIt = sd->starts.find(structId);
            if (startIt == sd->starts.end() || !startIt->second.isValid())
              continue;
            for (const StructurePiece &piece : startIt->second.pieces) {
              if (!piece.templateId.empty() &&
                  templates->getOrLoad(piece.templateId) == nullptr) {
                if (const PlacedFeature *pf =
                        registries_.findPlacedFeature(piece.templateId);
                    pf && writableArea.isInside(piece.position.x,
                                                piece.position.y,
                                                piece.position.z)) {
                  pf->place(region, *this, worldSeed, rng, piece.position,
                            false);
                }
                continue;
              }
              placeStructurePieceBlocks(piece, *templates, registries_, region,
                                        writableArea, rng);
            }
          }
        }
        ++structFeatureIndex;
      }
    }

    if (step < featuresPerStep_.size()) {
      const StepFeatureData &stepData = featuresPerStep_[step];
      std::vector<int> stepIndices;
      for (const std::string_view biomeId : presentBiomes) {
        const auto *bgi = registries_.findBiomeGeneration(biomeId);
        if (!bgi || step >= bgi->features.size())
          continue;
        for (const std::string &fid : bgi->features[step]) {
          auto idxIt = stepData.indexById.find(fid);
          if (idxIt != stepData.indexById.end())
            stepIndices.push_back(idxIt->second);
        }
      }
      std::sort(stepIndices.begin(), stepIndices.end());
      stepIndices.erase(std::unique(stepIndices.begin(), stepIndices.end()),
                        stepIndices.end());

      const Pos featureOrigin{originX, settings_.minY, originZ};
      for (const int globalIdx : stepIndices) {
        const std::string &featId =
            stepData.featureIds[static_cast<std::size_t>(globalIdx)];
        const PlacedFeature *pf = registries_.findPlacedFeature(featId);
        if (!pf)
          continue;
        worldgen_random::setFeatureSeed(rng, decorationSeed, globalIdx,
                                        static_cast<int>(step));
        (void)pf->place(region, *this, worldSeed, rng, featureOrigin, true);
      }
    }
  }
}

int NoiseBasedChunkGenerator::getBaseHeight(int x, int z, HeightmapType type,
                                            std::int64_t worldSeed) const {
  RandomState &rs = randomStateForSeed(worldSeed);
  const int cellWidth = 4 * settings_.sizeHorizontal;
  const int cellHeight = 4 * settings_.sizeVertical;
  const int firstCellX = floorDivInt(x, cellWidth);
  const int firstCellZ = floorDivInt(z, cellWidth);
  const int startX = firstCellX * cellWidth;
  const int startZ = firstCellZ * cellWidth;
  const int minCellY = floorDivInt(settings_.minY, cellHeight);
  const int cellCountY = floorDivInt(settings_.height, cellHeight);

  NoiseChunk nc(1, rs, startX, startZ, settings_.minY, settings_.height,
                settings_.sizeHorizontal, settings_.sizeVertical, settings_,
                globalFluidPicker_, {}, true, true);
  nc.initializeForFirstCellX();
  nc.advanceCellX(0);

  const double factorX =
      static_cast<double>(x - startX) / static_cast<double>(cellWidth);
  const double factorZ =
      static_cast<double>(z - startZ) / static_cast<double>(cellWidth);
  const BlockState &defaultBlock = settings_.defaultBlockState;

  for (int cellYIndex = cellCountY - 1; cellYIndex >= 0; --cellYIndex) {
    nc.selectCellYZ(cellYIndex, 0);
    for (int yInCell = cellHeight - 1; yInCell >= 0; --yInCell) {
      const int posY = (minCellY + cellYIndex) * cellHeight + yInCell;
      const double factorY =
          static_cast<double>(yInCell) / static_cast<double>(cellHeight);
      nc.updateForY(posY, factorY);
      nc.updateForX(x, factorX);
      nc.updateForZ(z, factorZ);
      std::optional<BlockState> optState = nc.getInterpolatedState();
      const BlockState state = optState ? *optState : defaultBlock;
      if (heightmapMatches(type, state)) {
        nc.stopInterpolation();
        return posY + 1;
      }
    }
  }
  nc.stopInterpolation();
  return settings_.minY;
}

} // namespace redstone::worldgen
