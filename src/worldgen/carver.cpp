#include "redstone/worldgen/carver.hpp"

#include "picojson.h"
#include "redstone/block_tags.hpp"
#include "redstone/engine.hpp"
#include "redstone/worldgen/aquifer.hpp"
#include "redstone/worldgen/biome_source.hpp"
#include "redstone/worldgen/noise_chunk.hpp"
#include "redstone/worldgen/noise_router.hpp"
#include "redstone/worldgen/proto_chunk.hpp"
#include "redstone/worldgen/surface_rules.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace redstone::worldgen {
namespace {

constexpr int kCarverRange = 4;
constexpr float kPiFloat = 3.1415927f;
constexpr float kHalfPiFloat = 1.5707964f;
constexpr float kTwoPiFloat = 6.2831855f;

std::string normId(std::string_view id) {
  if (id.find(':') != std::string_view::npos)
    return std::string(id);
  return "minecraft:" + std::string(id);
}

std::string stripHash(std::string_view s) {
  if (!s.empty() && s.front() == '#')
    s.remove_prefix(1);
  return normId(s);
}

const picojson::object &asObject(const picojson::value &v,
                                 std::string_view ctx) {
  if (!v.is<picojson::object>())
    throw std::runtime_error("Expected object in " + std::string(ctx));
  return v.get<picojson::object>();
}

const picojson::value &req(const picojson::object &obj, const char *key,
                           std::string_view ctx) {
  auto it = obj.find(key);
  if (it == obj.end())
    throw std::runtime_error("Missing '" + std::string(key) + "' in " +
                             std::string(ctx));
  return it->second;
}

double asNum(const picojson::value &v, std::string_view ctx) {
  if (!v.is<double>())
    throw std::runtime_error("Expected number in " + std::string(ctx));
  return v.get<double>();
}

std::string asStr(const picojson::value &v, std::string_view ctx) {
  if (!v.is<std::string>())
    throw std::runtime_error("Expected string in " + std::string(ctx));
  return v.get<std::string>();
}

int floorInt(double v) noexcept {
  const int i = static_cast<int>(v);
  return v < static_cast<double>(i) ? i - 1 : i;
}

bool canReplaceBlock(std::string_view replaceableTag, const BlockState &state) {
  const auto match = redstone::blockHasTag(state, replaceableTag);
  return match.value_or(false);
}

bool canReachChunk(ChunkTicketPos chunkPos, double x, double z, int currentStep,
                   int totalSteps, float thickness) noexcept {
  const double xMid = static_cast<double>(chunkPos.x * 16 + 8);
  const double zMid = static_cast<double>(chunkPos.z * 16 + 8);
  const double xd = x - xMid;
  const double zd = z - zMid;
  const double remaining = static_cast<double>(totalSteps - currentStep);
  const double rr = static_cast<double>(thickness + 2.0f + 16.0f);
  return xd * xd + zd * zd - remaining * remaining <= rr * rr;
}

std::optional<BlockState> getCarveState(const ConfiguredCarver &cfg,
                                        const WorldGenerationContext &genCtx,
                                        int x, int y, int z, Aquifer &aquifer) {
  if (y <= cfg.lavaLevel.resolveY(genCtx.minGenY, genCtx.genDepth)) {
    return Blocks::LAVA.defaultState();
  }
  return aquifer.computeSubstance(DensityFunctionContext{x, y, z, nullptr}, 0.0);
}

template <typename SkipFn>
bool carveEllipsoid(const ConfiguredCarver &cfg,
                    const WorldGenerationContext &genCtx,
                    const NoiseGeneratorSettings &settings, RandomState &rs,
                    const BiomeManager &biomeManager,
                    const BiomeManager::NoiseBiomeReader &noiseBiomeReader,
                    const WorldgenRegistries &registries, ChunkAccess &chunk,
                    NoiseChunk &noiseChunk, Aquifer &aquifer, double x,
                    double y, double z, double horizontalRadius,
                    double verticalRadius, CarvingMask &mask,
                    SkipFn &&shouldSkip) {
  const ChunkTicketPos chunkPos = chunk.pos();
  const double centerX = static_cast<double>(chunkPos.x * 16 + 8);
  const double centerZ = static_cast<double>(chunkPos.z * 16 + 8);
  const double maxDelta = 16.0 + horizontalRadius * 2.0;
  if (std::abs(x - centerX) > maxDelta || std::abs(z - centerZ) > maxDelta) {
    return false;
  }

  const int chunkMinX = chunkPos.x << 4;
  const int chunkMinZ = chunkPos.z << 4;
  const int minXIndex =
      std::max(floorInt(x - horizontalRadius) - chunkMinX - 1, 0);
  const int maxXIndex =
      std::min(floorInt(x + horizontalRadius) - chunkMinX, 15);
  const int minY =
      std::max(floorInt(y - verticalRadius) - 1, genCtx.minGenY + 1);
  // Upgraders not used on fresh chunks: protectedBlocksOnTop = 7.
  const int maxY = std::min(floorInt(y + verticalRadius) + 1,
                            genCtx.minGenY + genCtx.genDepth - 1 - 7);
  const int minZIndex =
      std::max(floorInt(z - horizontalRadius) - chunkMinZ - 1, 0);
  const int maxZIndex =
      std::min(floorInt(z + horizontalRadius) - chunkMinZ, 15);

  bool carved = false;
  const BlockState kCaveAir = BlockState("minecraft:cave_air");
  const BlockState kLava = Blocks::LAVA.defaultState();

  for (int xIndex = minXIndex; xIndex <= maxXIndex; ++xIndex) {
    const int worldX = chunkMinX + xIndex;
    const double xd =
        (static_cast<double>(worldX) + 0.5 - x) / horizontalRadius;
    for (int zIndex = minZIndex; zIndex <= maxZIndex; ++zIndex) {
      const int worldZ = chunkMinZ + zIndex;
      const double zd =
          (static_cast<double>(worldZ) + 0.5 - z) / horizontalRadius;
      if (xd * xd + zd * zd >= 1.0)
        continue;

      bool hasGrass = false;
      for (int worldY = maxY; worldY > minY; --worldY) {
        const double yd =
            (static_cast<double>(worldY) - 0.5 - y) / verticalRadius;
        if (shouldSkip(xd, yd, zd, worldY) ||
            mask.get(xIndex, worldY, zIndex)) {
          continue;
        }
        mask.set(xIndex, worldY, zIndex);

        const BlockState existing = chunk.getBlockState(worldX, worldY, worldZ);
        if (cfg.kind == CarverKind::NetherCave) {
          if (canReplaceBlock(cfg.replaceableTag, existing)) {
            const BlockState replacement =
                worldY <= genCtx.minGenY + 31 ? kLava : kCaveAir;
            chunk.setBlockState(worldX, worldY, worldZ, replacement, 3);
            carved = true;
          }
          continue;
        }

        if (existing.is(Blocks::GRASS_BLOCK) || existing.is(Blocks::MYCELIUM)) {
          hasGrass = true;
        }
        if (!canReplaceBlock(cfg.replaceableTag, existing))
          continue;
        const std::optional<BlockState> state =
            getCarveState(cfg, genCtx, worldX, worldY, worldZ, aquifer);
        if (!state.has_value())
          continue;

        chunk.setBlockState(worldX, worldY, worldZ, *state, 3);
        if (aquifer.shouldScheduleFluidUpdate() &&
            !fluidStateOf(*state).isEmpty()) {
          chunk.markPosForPostprocessing(worldX, worldY, worldZ);
        }

        if (hasGrass) {
          const int belowY = worldY - 1;
          if (chunk.getBlockState(worldX, belowY, worldZ).is(Blocks::DIRT)) {
            if (auto topMat = rs.surfaceSystem(settings).topMaterial(
                    rs, biomeManager, noiseBiomeReader, registries,
                    genCtx.minGenY, genCtx.genDepth, chunk, noiseChunk,
                    settings.surfaceRule, worldX, belowY, worldZ,
                    !fluidStateOf(*state).isEmpty())) {
              chunk.setBlockState(worldX, belowY, worldZ, *topMat, 3);
              if (!fluidStateOf(*topMat).isEmpty()) {
                chunk.markPosForPostprocessing(worldX, belowY, worldZ);
              }
            }
          }
        }
        carved = true;
      }
    }
  }
  return carved;
}

float caveThickness(const ConfiguredCarver &cfg, RandomSource &rng) {
  if (cfg.kind == CarverKind::NetherCave) {
    const float r1 = rng.nextFloat();
    const float r2 = rng.nextFloat();
    return (r1 * 2.0f + r2) * 2.0f;
  }
  const float r1 = rng.nextFloat();
  const float r2 = rng.nextFloat();
  float thickness = r1 * 2.0f + r2;
  if (rng.nextInt(10) == 0) {
    const float m1 = rng.nextFloat();
    const float m2 = rng.nextFloat();
    thickness *= m1 * m2 * 3.0f + 1.0f;
  }
  return thickness;
}

void createTunnel(const ConfiguredCarver &cfg,
                  const WorldGenerationContext &genCtx,
                  const NoiseGeneratorSettings &settings, RandomState &rs,
                  const BiomeManager &biomeManager,
                  const BiomeManager::NoiseBiomeReader &noiseBiomeReader,
                  const WorldgenRegistries &registries, std::int64_t tunnelSeed,
                  ChunkAccess &chunk, NoiseChunk &noiseChunk, Aquifer &aquifer,
                  double x, double y, double z, double hMult, double vMult,
                  float thickness, float horizontalRotation,
                  float verticalRotation, int step, int dist, double yScale,
                  CarvingMask &mask, double floorLevel) {
  LegacyRandomSource random(tunnelSeed);
  const int splitPoint = random.nextInt(dist / 2) + dist / 4;
  const bool steep = random.nextInt(6) == 0;
  float yRota = 0.0f;
  float xRota = 0.0f;
  auto skipChecker = [floorLevel](double xd, double yd, double zd, int) {
    return yd <= floorLevel || xd * xd + yd * yd + zd * zd >= 1.0;
  };

  for (int currentStep = step; currentStep < dist; ++currentStep) {
    const double horizontalRadius =
        1.5 + static_cast<double>(worldgen_math::minecraftSin(
                  static_cast<float>(kPiFloat * static_cast<float>(currentStep) /
                                     static_cast<float>(dist))) *
                                  thickness);
    const double verticalRadius = horizontalRadius * yScale;
    const float cosX = worldgen_math::minecraftCos(verticalRotation);
    x += static_cast<double>(worldgen_math::minecraftCos(horizontalRotation) *
                             cosX);
    y += static_cast<double>(worldgen_math::minecraftSin(verticalRotation));
    z += static_cast<double>(worldgen_math::minecraftSin(horizontalRotation) *
                             cosX);
    verticalRotation *= steep ? 0.92f : 0.7f;
    verticalRotation += xRota * 0.1f;
    horizontalRotation += yRota * 0.1f;
    xRota *= 0.9f;
    yRota *= 0.75f;
    {
      const float f1 = random.nextFloat();
      const float f2 = random.nextFloat();
      const float f3 = random.nextFloat();
      xRota += (f1 - f2) * f3 * 2.0f;
    }
    {
      const float f1 = random.nextFloat();
      const float f2 = random.nextFloat();
      const float f3 = random.nextFloat();
      yRota += (f1 - f2) * f3 * 4.0f;
    }

    if (currentStep == splitPoint && thickness > 1.0f) {
      const std::int64_t seedA = random.nextLong();
      const float thickA = random.nextFloat() * 0.5f + 0.5f;
      createTunnel(cfg, genCtx, settings, rs, biomeManager, noiseBiomeReader,
                   registries, seedA, chunk, noiseChunk, aquifer, x,
                   y, z, hMult, vMult, thickA,
                   horizontalRotation - kHalfPiFloat, verticalRotation / 3.0f,
                   currentStep, dist, 1.0, mask, floorLevel);
      const std::int64_t seedB = random.nextLong();
      const float thickB = random.nextFloat() * 0.5f + 0.5f;
      createTunnel(cfg, genCtx, settings, rs, biomeManager, noiseBiomeReader,
                   registries, seedB, chunk, noiseChunk, aquifer, x,
                   y, z, hMult, vMult, thickB,
                   horizontalRotation + kHalfPiFloat, verticalRotation / 3.0f,
                   currentStep, dist, 1.0, mask, floorLevel);
      return;
    }

    if (random.nextInt(4) != 0) {
      if (!canReachChunk(chunk.pos(), x, z, currentStep, dist, thickness)) {
        return;
      }
      carveEllipsoid(cfg, genCtx, settings, rs, biomeManager, noiseBiomeReader,
                     registries, chunk, noiseChunk, aquifer, x, y, z,
                     horizontalRadius * hMult, verticalRadius * vMult, mask,
                     skipChecker);
    }
  }
}

bool carveCanyon(const ConfiguredCarver &cfg,
                 const WorldGenerationContext &genCtx,
                 const NoiseGeneratorSettings &settings, RandomState &rs,
                 const BiomeManager &biomeManager,
                 const BiomeManager::NoiseBiomeReader &noiseBiomeReader,
                 const WorldgenRegistries &registries, ChunkAccess &chunk,
                 NoiseChunk &noiseChunk, RandomSource &rng, Aquifer &aquifer,
                 ChunkTicketPos sourceChunkPos, CarvingMask &mask) {
  const int maxDistance = (kCarverRange * 2 - 1) * 16;
  double x = static_cast<double>((sourceChunkPos.x << 4) + rng.nextInt(16));
  double y = static_cast<double>(cfg.y.sample(rng, genCtx));
  double z = static_cast<double>((sourceChunkPos.z << 4) + rng.nextInt(16));
  float horizontalRotation = rng.nextFloat() * kTwoPiFloat;
  float verticalRotation = cfg.verticalRotation.sample(rng);
  const double yScale = static_cast<double>(cfg.yScale.sample(rng));
  const float thickness = cfg.canyonShape.thickness.sample(rng);
  const int distance = static_cast<int>(
      static_cast<float>(maxDistance) * cfg.canyonShape.distanceFactor.sample(rng));
  const std::int64_t tunnelSeed = rng.nextLong();

  LegacyRandomSource tunnelRandom(tunnelSeed);
  std::vector<float> widthFactorPerHeight(static_cast<std::size_t>(genCtx.genDepth));
  float widthFactor = 1.0f;
  for (int yIndex = 0; yIndex < genCtx.genDepth; ++yIndex) {
    if (yIndex == 0 ||
        tunnelRandom.nextInt(cfg.canyonShape.widthSmoothness) == 0) {
      const float w1 = tunnelRandom.nextFloat();
      const float w2 = tunnelRandom.nextFloat();
      widthFactor = 1.0f + w1 * w2;
    }
    widthFactorPerHeight[static_cast<std::size_t>(yIndex)] =
        widthFactor * widthFactor;
  }

  float yRota = 0.0f;
  float xRota = 0.0f;
  auto skipChecker = [&](double xd, double yd, double zd, int worldY) {
    const int yIndex = worldY - genCtx.minGenY;
    const float w = widthFactorPerHeight[static_cast<std::size_t>(yIndex - 1)];
    return (xd * xd + zd * zd) * static_cast<double>(w) + yd * yd / 6.0 >= 1.0;
  };

  for (int currentStep = 0; currentStep < distance; ++currentStep) {
    double horizontalRadius =
        1.5 + static_cast<double>(worldgen_math::minecraftSin(
                  static_cast<float>(currentStep) * kPiFloat /
                  static_cast<float>(distance)) *
                                  thickness);
    double verticalRadius = horizontalRadius * yScale;
    horizontalRadius *=
        static_cast<double>(cfg.canyonShape.horizontalRadiusFactor.sample(tunnelRandom));
    const float verticalMultiplier =
        1.0f - std::abs(0.5f - static_cast<float>(currentStep) /
                                   static_cast<float>(distance)) *
                   2.0f;
    const float factor = cfg.canyonShape.verticalRadiusDefaultFactor +
                         cfg.canyonShape.verticalRadiusCenterFactor *
                             verticalMultiplier;
    verticalRadius =
        static_cast<double>(factor) * verticalRadius *
        static_cast<double>(worldgen_math::randomBetween(tunnelRandom, 0.75f, 1.0f));

    const float xc = worldgen_math::minecraftCos(verticalRotation);
    const float xs = worldgen_math::minecraftSin(verticalRotation);
    x += static_cast<double>(worldgen_math::minecraftCos(horizontalRotation) *
                             xc);
    y += static_cast<double>(xs);
    z += static_cast<double>(worldgen_math::minecraftSin(horizontalRotation) *
                             xc);
    verticalRotation *= 0.7f;
    verticalRotation += xRota * 0.05f;
    horizontalRotation += yRota * 0.05f;
    xRota *= 0.8f;
    yRota *= 0.5f;
    {
      const float f1 = tunnelRandom.nextFloat();
      const float f2 = tunnelRandom.nextFloat();
      const float f3 = tunnelRandom.nextFloat();
      xRota += (f1 - f2) * f3 * 2.0f;
    }
    {
      const float f1 = tunnelRandom.nextFloat();
      const float f2 = tunnelRandom.nextFloat();
      const float f3 = tunnelRandom.nextFloat();
      yRota += (f1 - f2) * f3 * 4.0f;
    }

    if (tunnelRandom.nextInt(4) != 0) {
      if (!canReachChunk(chunk.pos(), x, z, currentStep, distance, thickness)) {
        return true;
      }
      carveEllipsoid(cfg, genCtx, settings, rs, biomeManager, noiseBiomeReader,
                     registries, chunk, noiseChunk, aquifer, x, y, z,
                     horizontalRadius, verticalRadius, mask, skipChecker);
    }
  }
  return true;
}

} // namespace

bool ConfiguredCarver::isStartChunk(RandomSource &rng) const {
  return rng.nextFloat() <= probability;
}

bool ConfiguredCarver::carve(
    const WorldGenerationContext &genCtx, const NoiseGeneratorSettings &settings,
    RandomState &rs, const BiomeManager &biomeManager,
    const CarverNoiseBiomeReader &noiseBiomeReader,
    const WorldgenRegistries &registries, ChunkAccess &chunk,
    NoiseChunk &noiseChunk, RandomSource &rng, Aquifer &aquifer,
    ChunkTicketPos sourceChunkPos, CarvingMask &mask) const {
  if (kind == CarverKind::Canyon) {
    return carveCanyon(*this, genCtx, settings, rs, biomeManager,
                       noiseBiomeReader, registries, chunk, noiseChunk, rng,
                       aquifer, sourceChunkPos, mask);
  }

  const int maxDistance = (kCarverRange * 2 - 1) * 16;
  const int caveBound = (kind == CarverKind::NetherCave) ? 10 : 15;
  const int caveCount =
      rng.nextInt(rng.nextInt(rng.nextInt(caveBound) + 1) + 1);
  const double defaultYScale = (kind == CarverKind::NetherCave) ? 5.0 : 1.0;

  for (int cave = 0; cave < caveCount; ++cave) {
    const double x =
        static_cast<double>((sourceChunkPos.x << 4) + rng.nextInt(16));
    const double y = static_cast<double>(this->y.sample(rng, genCtx));
    const double z =
        static_cast<double>((sourceChunkPos.z << 4) + rng.nextInt(16));
    const double hMult =
        static_cast<double>(horizontalRadiusMultiplier.sample(rng));
    const double vMult =
        static_cast<double>(verticalRadiusMultiplier.sample(rng));
    const double floorLvl = static_cast<double>(floorLevel.sample(rng));
    auto skipChecker = [floorLvl](double xd, double yd, double zd, int) {
      return yd <= floorLvl || xd * xd + yd * yd + zd * zd >= 1.0;
    };

    int tunnels = 1;
    if (rng.nextInt(4) == 0) {
      const double roomYScale = static_cast<double>(yScale.sample(rng));
      const float thickness = 1.0f + rng.nextFloat() * 6.0f;
      const double hRad =
          1.5 + static_cast<double>(worldgen_math::minecraftSin(kHalfPiFloat) *
                                    thickness);
      const double vRad = hRad * roomYScale;
      carveEllipsoid(*this, genCtx, settings, rs, biomeManager, noiseBiomeReader,
                     registries, chunk, noiseChunk, aquifer, x + 1.0, y, z,
                     hRad, vRad, mask, skipChecker);
      tunnels += rng.nextInt(4);
    }

    for (int i = 0; i < tunnels; ++i) {
      const float hRot = rng.nextFloat() * kTwoPiFloat;
      const float vRot = (rng.nextFloat() - 0.5f) / 4.0f;
      const float thickness = caveThickness(*this, rng);
      const int distance = maxDistance - rng.nextInt(maxDistance / 4);
      const std::int64_t tseed = rng.nextLong();
      createTunnel(*this, genCtx, settings, rs, biomeManager, noiseBiomeReader,
                   registries, tseed, chunk, noiseChunk, aquifer, x, y,
                   z, hMult, vMult, thickness, hRot, vRot, 0, distance,
                   defaultYScale, mask, floorLvl);
    }
  }
  return true;
}

ConfiguredCarver parseConfiguredCarverJson(const picojson::value &v,
                                           std::string_view id) {
  const auto &obj = asObject(v, id);
  const std::string type = normId(asStr(req(obj, "type", id), id));
  const auto &cfg = asObject(req(obj, "config", id), id);

  ConfiguredCarver out;
  out.id = normId(id);
  if (type == "minecraft:cave") {
    out.kind = CarverKind::Cave;
  } else if (type == "minecraft:nether_cave") {
    out.kind = CarverKind::NetherCave;
  } else if (type == "minecraft:canyon") {
    out.kind = CarverKind::Canyon;
  } else {
    throw std::runtime_error("Unsupported carver type '" + type + "' in " +
                             std::string(id));
  }

  out.probability = static_cast<float>(asNum(req(cfg, "probability", id), id));
  out.y = parseHeightProviderJson(req(cfg, "y", id), id);
  out.yScale = parseFloatProviderJson(req(cfg, "yScale", id), id);
  out.lavaLevel = parseVerticalAnchorJson(req(cfg, "lava_level", id), id);
  out.replaceableTag = stripHash(asStr(req(cfg, "replaceable", id), id));

  if (out.kind == CarverKind::Canyon) {
    out.verticalRotation =
        parseFloatProviderJson(req(cfg, "vertical_rotation", id), id);
    const auto &shape = asObject(req(cfg, "shape", id), id);
    out.canyonShape.distanceFactor =
        parseFloatProviderJson(req(shape, "distance_factor", id), id);
    out.canyonShape.thickness =
        parseFloatProviderJson(req(shape, "thickness", id), id);
    out.canyonShape.widthSmoothness =
        static_cast<int>(asNum(req(shape, "width_smoothness", id), id));
    out.canyonShape.horizontalRadiusFactor =
        parseFloatProviderJson(req(shape, "horizontal_radius_factor", id), id);
    out.canyonShape.verticalRadiusDefaultFactor = static_cast<float>(
        asNum(req(shape, "vertical_radius_default_factor", id), id));
    out.canyonShape.verticalRadiusCenterFactor = static_cast<float>(
        asNum(req(shape, "vertical_radius_center_factor", id), id));
  } else {
    out.horizontalRadiusMultiplier = parseFloatProviderJson(
        req(cfg, "horizontal_radius_multiplier", id), id);
    out.verticalRadiusMultiplier =
        parseFloatProviderJson(req(cfg, "vertical_radius_multiplier", id), id);
    out.floorLevel = parseFloatProviderJson(req(cfg, "floor_level", id), id);
  }
  return out;
}

} // namespace redstone::worldgen
