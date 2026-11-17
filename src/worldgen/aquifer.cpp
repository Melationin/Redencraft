#include "redstone/worldgen/aquifer.hpp"

#include "redstone/block.hpp"
#include "redstone/worldgen/noise_chunk.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace redstone::worldgen {
namespace {

int floorDivInt(int a, int b) noexcept {
  const int q = a / b;
  const int r = a % b;
  return (r != 0 && ((a ^ b) < 0)) ? q - 1 : q;
}

int quantizeInt(double value, int step) noexcept {
  return static_cast<int>(std::floor(value / static_cast<double>(step))) * step;
}

double inverseLerp(double value, double minVal, double maxVal) noexcept {
  return (value - minVal) / (maxVal - minVal);
}

double mapValue(double value, double fromMin, double fromMax, double toMin,
                double toMax) noexcept {
  return worldgen_math::lerp(inverseLerp(value, fromMin, fromMax), toMin, toMax);
}

double clampedMapValue(double value, double fromMin, double fromMax,
                       double toMin, double toMax) noexcept {
  return worldgen_math::clampedLerp(inverseLerp(value, fromMin, fromMax), toMin,
                                    toMax);
}

bool isDeepDarkRegion(const DensityNodePtr &erosion,
                      const DensityNodePtr &depth,
                      const DensityFunctionContext &ctx) {
  const double e = erosion ? erosion->compute(ctx) : 0.0;
  const double d = depth ? depth->compute(ctx) : 0.0;
  return e < static_cast<double>(-0.225f) && d > static_cast<double>(0.9f);
}

class DisabledAquifer final : public Aquifer {
public:
  explicit DisabledAquifer(FluidPicker fluidPicker)
      : fluidPicker_(std::move(fluidPicker)) {}

  std::optional<BlockState> computeSubstance(const DensityFunctionContext &ctx,
                                             double density) override {
    if (density > 0.0)
      return std::nullopt;
    return fluidPicker_(ctx.blockX, ctx.blockY, ctx.blockZ).at(ctx.blockY);
  }

  bool shouldScheduleFluidUpdate() const noexcept override { return false; }

private:
  FluidPicker fluidPicker_;
};

class NoiseBasedAquifer final : public Aquifer {
public:
  static constexpr int kXRange = 10;
  static constexpr int kYRange = 9;
  static constexpr int kZRange = 10;
  static constexpr int kSampleOffsetX = -5;
  static constexpr int kSampleOffsetY = 1;
  static constexpr int kSampleOffsetZ = -5;

  static double similarity(int distanceSqr1, int distanceSqr2) noexcept {
    return 1.0 - static_cast<double>(distanceSqr2 - distanceSqr1) / 25.0;
  }

  static inline const double kFlowingUpdateSimilarity =
      similarity(10 * 10, 12 * 12);

  static constexpr std::array<std::array<int, 2>, 13>
      kSurfaceSamplingOffsetsInChunks{{
          {0, 0},
          {-2, -1},
          {-1, -1},
          {0, -1},
          {1, -1},
          {-3, 0},
          {-2, 0},
          {-1, 0},
          {1, 0},
          {-2, 1},
          {-1, 1},
          {0, 1},
          {1, 1},
      }};

  NoiseBasedAquifer(NoiseChunk &noiseChunk, ChunkTicketPos pos,
                    const NoiseRouter &router,
                    const PositionalRandomFactory &positionalRandomFactory,
                    int minBlockY, int yBlockSize,
                    FluidPicker globalFluidPicker)
      : noiseChunk_(noiseChunk), barrierNoise_(router.barrierNoise),
        fluidLevelFloodednessNoise_(router.fluidLevelFloodednessNoise),
        fluidLevelSpreadNoise_(router.fluidLevelSpreadNoise),
        lavaNoise_(router.lavaNoise), erosion_(router.erosion),
        depth_(router.depth),
        positionalRandomFactory_(positionalRandomFactory),
        globalFluidPicker_(std::move(globalFluidPicker)),
        waterState_(Blocks::WATER.defaultState()),
        lavaState_(Blocks::LAVA.defaultState()),
        airState_(Blocks::AIR.defaultState()) {
    const int minBlockX = pos.x * 16;
    const int maxBlockX = minBlockX + 15;
    const int minBlockZ = pos.z * 16;
    const int maxBlockZ = minBlockZ + 15;

    minGridX_ = gridX(minBlockX + kSampleOffsetX);
    const int maxGridX = gridX(maxBlockX + kSampleOffsetX) + 1;
    gridSizeX_ = maxGridX - minGridX_ + 1;

    minGridY_ = gridY(minBlockY + kSampleOffsetY) - 1;
    const int maxGridY = gridY(minBlockY + yBlockSize + kSampleOffsetY) + 1;
    const int gridSizeY = maxGridY - minGridY_ + 1;

    minGridZ_ = gridZ(minBlockZ + kSampleOffsetZ);
    const int maxGridZ = gridZ(maxBlockZ + kSampleOffsetZ) + 1;
    gridSizeZ_ = maxGridZ - minGridZ_ + 1;

    const int totalGridSize = gridSizeX_ * gridSizeY * gridSizeZ_;
    aquiferCache_.resize(static_cast<std::size_t>(totalGridSize));
    aquiferLocationCache_.resize(static_cast<std::size_t>(totalGridSize));

    const int maxAdjustedSurfaceLevel = adjustSurfaceLevel(
        noiseChunk_.maxPreliminarySurfaceLevel(
            fromGridX(minGridX_, 0), fromGridZ(minGridZ_, 0),
            fromGridX(maxGridX, 9), fromGridZ(maxGridZ, 9)));
    const int skipSamplingAboveGridY = gridY(maxAdjustedSurfaceLevel + 12) + 1;
    skipSamplingAboveY_ = fromGridY(skipSamplingAboveGridY, 11) - 1;
  }

  std::optional<BlockState> computeSubstance(const DensityFunctionContext &ctx,
                                             double density) override {
    if (density > 0.0) {
      shouldScheduleFluidUpdate_ = false;
      return std::nullopt;
    }

    const int posX = ctx.blockX;
    const int posY = ctx.blockY;
    const int posZ = ctx.blockZ;
    const FluidStatus globalFluid = globalFluidPicker_(posX, posY, posZ);
    if (posY > skipSamplingAboveY_) {
      shouldScheduleFluidUpdate_ = false;
      return globalFluid.at(posY);
    }
    if (globalFluid.at(posY).is(Blocks::LAVA)) {
      shouldScheduleFluidUpdate_ = false;
      return lavaState_;
    }

    const int xAnchor = gridX(posX + kSampleOffsetX);
    const int yAnchor = gridY(posY + kSampleOffsetY);
    const int zAnchor = gridZ(posZ + kSampleOffsetZ);
    int distanceSqr1 = std::numeric_limits<int>::max();
    int distanceSqr2 = std::numeric_limits<int>::max();
    int distanceSqr3 = std::numeric_limits<int>::max();
    int distanceSqr4 = std::numeric_limits<int>::max();
    int closestIndex1 = 0;
    int closestIndex2 = 0;
    int closestIndex3 = 0;
    int closestIndex4 = 0;

    for (int x1 = 0; x1 <= 1; ++x1) {
      for (int y1 = -1; y1 <= 1; ++y1) {
        for (int z1 = 0; z1 <= 1; ++z1) {
          const int spacedGridX = xAnchor + x1;
          const int spacedGridY = yAnchor + y1;
          const int spacedGridZ = zAnchor + z1;
          const int index = getIndex(spacedGridX, spacedGridY, spacedGridZ);
          auto &locSlot = aquiferLocationCache_[static_cast<std::size_t>(index)];
          Pos location;
          if (locSlot.has_value()) {
            location = *locSlot;
          } else {
            AnyRandomSource rng = positionalRandomFactory_.at(
                spacedGridX, spacedGridY, spacedGridZ);
            location = Pos{fromGridX(spacedGridX, rng.nextInt(kXRange)),
                           fromGridY(spacedGridY, rng.nextInt(kYRange)),
                           fromGridZ(spacedGridZ, rng.nextInt(kZRange))};
            locSlot = location;
          }

          const int dx = location.x - posX;
          const int dy = location.y - posY;
          const int dz = location.z - posZ;
          const int newDistance = dx * dx + dy * dy + dz * dz;
          if (distanceSqr1 >= newDistance) {
            closestIndex4 = closestIndex3;
            closestIndex3 = closestIndex2;
            closestIndex2 = closestIndex1;
            closestIndex1 = index;
            distanceSqr4 = distanceSqr3;
            distanceSqr3 = distanceSqr2;
            distanceSqr2 = distanceSqr1;
            distanceSqr1 = newDistance;
          } else if (distanceSqr2 >= newDistance) {
            closestIndex4 = closestIndex3;
            closestIndex3 = closestIndex2;
            closestIndex2 = index;
            distanceSqr4 = distanceSqr3;
            distanceSqr3 = distanceSqr2;
            distanceSqr2 = newDistance;
          } else if (distanceSqr3 >= newDistance) {
            closestIndex4 = closestIndex3;
            closestIndex3 = index;
            distanceSqr4 = distanceSqr3;
            distanceSqr3 = newDistance;
          } else if (distanceSqr4 >= newDistance) {
            closestIndex4 = index;
            distanceSqr4 = newDistance;
          }
        }
      }
    }

    const FluidStatus closestStatus1 = getAquiferStatus(closestIndex1);
    const double similarity12 = similarity(distanceSqr1, distanceSqr2);
    const BlockState fluidState = closestStatus1.at(posY);
    if (similarity12 <= 0.0) {
      if (similarity12 >= kFlowingUpdateSimilarity) {
        const FluidStatus closestStatus2 = getAquiferStatus(closestIndex2);
        shouldScheduleFluidUpdate_ = !(closestStatus1 == closestStatus2);
      } else {
        shouldScheduleFluidUpdate_ = false;
      }
      return fluidState;
    }
    if (fluidState.is(Blocks::WATER) &&
        globalFluidPicker_(posX, posY - 1, posZ).at(posY - 1).is(Blocks::LAVA)) {
      shouldScheduleFluidUpdate_ = true;
      return fluidState;
    }

    double barrierNoiseValue = std::numeric_limits<double>::quiet_NaN();
    const FluidStatus closestStatus2 = getAquiferStatus(closestIndex2);
    const double barrier12 =
        similarity12 *
        calculatePressure(ctx, barrierNoiseValue, closestStatus1, closestStatus2);
    if (density + barrier12 > 0.0) {
      shouldScheduleFluidUpdate_ = false;
      return std::nullopt;
    }

    const FluidStatus closestStatus3 = getAquiferStatus(closestIndex3);
    const double similarity13 = similarity(distanceSqr1, distanceSqr3);
    if (similarity13 > 0.0) {
      const double barrier13 =
          similarity12 * similarity13 *
          calculatePressure(ctx, barrierNoiseValue, closestStatus1,
                            closestStatus3);
      if (density + barrier13 > 0.0) {
        shouldScheduleFluidUpdate_ = false;
        return std::nullopt;
      }
    }

    const double similarity23 = similarity(distanceSqr2, distanceSqr3);
    if (similarity23 > 0.0) {
      const double barrier23 =
          similarity12 * similarity23 *
          calculatePressure(ctx, barrierNoiseValue, closestStatus2,
                            closestStatus3);
      if (density + barrier23 > 0.0) {
        shouldScheduleFluidUpdate_ = false;
        return std::nullopt;
      }
    }

    const bool mayFlow12 = !(closestStatus1 == closestStatus2);
    const bool mayFlow23 = similarity23 >= kFlowingUpdateSimilarity &&
                           !(closestStatus2 == closestStatus3);
    const bool mayFlow13 = similarity13 >= kFlowingUpdateSimilarity &&
                           !(closestStatus1 == closestStatus3);
    if (!mayFlow12 && !mayFlow23 && !mayFlow13) {
      shouldScheduleFluidUpdate_ =
          similarity13 >= kFlowingUpdateSimilarity &&
          similarity(distanceSqr1, distanceSqr4) >= kFlowingUpdateSimilarity &&
          !(closestStatus1 == getAquiferStatus(closestIndex4));
    } else {
      shouldScheduleFluidUpdate_ = true;
    }

    return fluidState;
  }

  bool shouldScheduleFluidUpdate() const noexcept override {
    return shouldScheduleFluidUpdate_;
  }

private:
  int getIndex(int gridX, int gridY, int gridZ) const noexcept {
    const int x = gridX - minGridX_;
    const int y = gridY - minGridY_;
    const int z = gridZ - minGridZ_;
    return (y * gridSizeZ_ + z) * gridSizeX_ + x;
  }

  static int gridX(int blockCoord) noexcept { return blockCoord >> 4; }
  static int fromGridX(int gridCoord, int blockOffset) noexcept {
    return (gridCoord << 4) + blockOffset;
  }
  static int gridY(int blockCoord) noexcept { return floorDivInt(blockCoord, 12); }
  static int fromGridY(int gridCoord, int blockOffset) noexcept {
    return gridCoord * 12 + blockOffset;
  }
  static int gridZ(int blockCoord) noexcept { return blockCoord >> 4; }
  static int fromGridZ(int gridCoord, int blockOffset) noexcept {
    return (gridCoord << 4) + blockOffset;
  }
  static int adjustSurfaceLevel(int preliminarySurfaceLevel) noexcept {
    return preliminarySurfaceLevel + 8;
  }

  double calculatePressure(const DensityFunctionContext &ctx,
                           double &barrierNoiseValue,
                           const FluidStatus &status1,
                           const FluidStatus &status2) const {
    const int posY = ctx.blockY;
    const BlockState type1 = status1.at(posY);
    const BlockState type2 = status2.at(posY);
    if ((type1.is(Blocks::LAVA) && type2.is(Blocks::WATER)) ||
        (type1.is(Blocks::WATER) && type2.is(Blocks::LAVA))) {
      return 2.0;
    }
    const int fluidYDiff = std::abs(status1.fluidLevel - status2.fluidLevel);
    if (fluidYDiff == 0)
      return 0.0;

    const double averageFluidY =
        0.5 * static_cast<double>(status1.fluidLevel + status2.fluidLevel);
    const double howFarAbove = static_cast<double>(posY) + 0.5 - averageFluidY;
    const double baseValue = static_cast<double>(fluidYDiff) / 2.0;
    const double distanceFromEdge = baseValue - std::abs(howFarAbove);
    double gradient;
    if (howFarAbove > 0.0) {
      const double centerPoint = 0.0 + distanceFromEdge;
      gradient = centerPoint > 0.0 ? centerPoint / 1.5 : centerPoint / 2.5;
    } else {
      const double centerPoint = 3.0 + distanceFromEdge;
      gradient = centerPoint > 0.0 ? centerPoint / 3.0 : centerPoint / 10.0;
    }

    double noiseValue = 0.0;
    if (!(gradient < -2.0) && !(gradient > 2.0)) {
      if (std::isnan(barrierNoiseValue)) {
        barrierNoiseValue = barrierNoise_ ? barrierNoise_->compute(ctx) : 0.0;
      }
      noiseValue = barrierNoiseValue;
    }
    return 2.0 * (noiseValue + gradient);
  }

  FluidStatus getAquiferStatus(int index) {
    auto &cached = aquiferCache_[static_cast<std::size_t>(index)];
    if (cached.has_value())
      return *cached;
    const Pos loc = *aquiferLocationCache_[static_cast<std::size_t>(index)];
    FluidStatus status = computeFluid(loc.x, loc.y, loc.z);
    cached = status;
    return status;
  }

  FluidStatus computeFluid(int x, int y, int z) {
    const FluidStatus globalFluid = globalFluidPicker_(x, y, z);
    int lowestPreliminarySurface = std::numeric_limits<int>::max();
    const int topOfAquiferCell = y + 12;
    const int bottomOfAquiferCell = y - 12;
    bool surfaceAtCenterIsUnderGlobalFluidLevel = false;

    for (const auto &offset : kSurfaceSamplingOffsetsInChunks) {
      const int sampleX = x + (offset[0] << 4);
      const int sampleZ = z + (offset[1] << 4);
      const int preliminarySurface =
          noiseChunk_.preliminarySurfaceLevel(sampleX, sampleZ);
      const int adjustedSurface = adjustSurfaceLevel(preliminarySurface);
      const bool start = (offset[0] == 0 && offset[1] == 0);
      if (start && bottomOfAquiferCell > adjustedSurface)
        return globalFluid;

      const bool topPokesAbove = topOfAquiferCell > adjustedSurface;
      if (topPokesAbove || start) {
        const FluidStatus globalAtSurface =
            globalFluidPicker_(sampleX, adjustedSurface, sampleZ);
        if (!globalAtSurface.at(adjustedSurface).isAir()) {
          if (start)
            surfaceAtCenterIsUnderGlobalFluidLevel = true;
          if (topPokesAbove)
            return globalAtSurface;
        }
      }
      lowestPreliminarySurface =
          std::min(lowestPreliminarySurface, preliminarySurface);
    }

    const int fluidSurfaceLevel =
        computeSurfaceLevel(x, y, z, globalFluid, lowestPreliminarySurface,
                            surfaceAtCenterIsUnderGlobalFluidLevel);
    return FluidStatus{fluidSurfaceLevel,
                       computeFluidType(x, y, z, globalFluid, fluidSurfaceLevel)};
  }

  int computeSurfaceLevel(int x, int y, int z, const FluidStatus &globalFluid,
                          int lowestPreliminarySurface,
                          bool surfaceAtCenterIsUnderGlobalFluidLevel) {
    const DensityFunctionContext ctx{x, y, z, nullptr};
    double partiallyFloodedness;
    double fullyFloodedness;
    if (isDeepDarkRegion(erosion_, depth_, ctx)) {
      partiallyFloodedness = -1.0;
      fullyFloodedness = -1.0;
    } else {
      const int distanceBelowSurface = lowestPreliminarySurface + 8 - y;
      const double floodednessFactor =
          surfaceAtCenterIsUnderGlobalFluidLevel
              ? clampedMapValue(static_cast<double>(distanceBelowSurface), 0.0,
                                64.0, 1.0, 0.0)
              : 0.0;
      const double floodednessNoiseValue = std::clamp(
          fluidLevelFloodednessNoise_ ? fluidLevelFloodednessNoise_->compute(ctx)
                                      : 0.0,
          -1.0, 1.0);
      const double fullyFloodedThreshold =
          mapValue(floodednessFactor, 1.0, 0.0, -0.3, 0.8);
      const double partiallyFloodedThreshold =
          mapValue(floodednessFactor, 1.0, 0.0, -0.8, 0.4);
      partiallyFloodedness = floodednessNoiseValue - partiallyFloodedThreshold;
      fullyFloodedness = floodednessNoiseValue - fullyFloodedThreshold;
    }

    if (fullyFloodedness > 0.0)
      return globalFluid.fluidLevel;
    if (partiallyFloodedness > 0.0)
      return computeRandomizedFluidSurfaceLevel(x, y, z,
                                                lowestPreliminarySurface);
    return kWayBelowMinY;
  }

  int computeRandomizedFluidSurfaceLevel(int x, int y, int z,
                                         int lowestPreliminarySurface) {
    const int fluidLevelCellX = floorDivInt(x, 16);
    const int fluidLevelCellY = floorDivInt(y, 40);
    const int fluidLevelCellZ = floorDivInt(z, 16);
    const int fluidCellMiddleY = fluidLevelCellY * 40 + 20;
    const double spread =
        (fluidLevelSpreadNoise_
             ? fluidLevelSpreadNoise_->compute(DensityFunctionContext{
                   fluidLevelCellX, fluidLevelCellY, fluidLevelCellZ, nullptr})
             : 0.0) *
        10.0;
    const int spreadQuantized = quantizeInt(spread, 3);
    return std::min(lowestPreliminarySurface, fluidCellMiddleY + spreadQuantized);
  }

  BlockState computeFluidType(int x, int y, int z,
                              const FluidStatus &globalFluid,
                              int fluidSurfaceLevel) {
    BlockState fluidType = globalFluid.fluidType;
    if (fluidSurfaceLevel <= -10 && fluidSurfaceLevel != kWayBelowMinY &&
        !globalFluid.fluidType.is(Blocks::LAVA)) {
      const int fluidTypeCellX = floorDivInt(x, 64);
      const int fluidTypeCellY = floorDivInt(y, 40);
      const int fluidTypeCellZ = floorDivInt(z, 64);
      const double lavaNoiseValue =
          lavaNoise_ ? lavaNoise_->compute(DensityFunctionContext{
                           fluidTypeCellX, fluidTypeCellY, fluidTypeCellZ,
                           nullptr})
                     : 0.0;
      if (std::abs(lavaNoiseValue) > 0.3)
        fluidType = lavaState_;
    }
    return fluidType;
  }

  NoiseChunk &noiseChunk_;
  DensityNodePtr barrierNoise_;
  DensityNodePtr fluidLevelFloodednessNoise_;
  DensityNodePtr fluidLevelSpreadNoise_;
  DensityNodePtr lavaNoise_;
  DensityNodePtr erosion_;
  DensityNodePtr depth_;
  const PositionalRandomFactory &positionalRandomFactory_;
  FluidPicker globalFluidPicker_;
  BlockState waterState_;
  BlockState lavaState_;
  BlockState airState_;
  bool shouldScheduleFluidUpdate_ = false;
  int skipSamplingAboveY_ = 0;
  int minGridX_ = 0;
  int minGridY_ = 0;
  int minGridZ_ = 0;
  int gridSizeX_ = 0;
  int gridSizeZ_ = 0;
  std::vector<std::optional<FluidStatus>> aquiferCache_;
  std::vector<std::optional<Pos>> aquiferLocationCache_;
};

} // namespace

BlockState FluidStatus::at(int blockY) const noexcept {
  return blockY < fluidLevel ? fluidType : Blocks::AIR.defaultState();
}

std::unique_ptr<Aquifer>
Aquifer::create(NoiseChunk &noiseChunk, ChunkTicketPos chunkPos,
                const NoiseRouter &router,
                const PositionalRandomFactory &aquiferRandom, int minBlockY,
                int yBlockSize, FluidPicker globalFluidPicker) {
  return std::make_unique<NoiseBasedAquifer>(
      noiseChunk, chunkPos, router, aquiferRandom, minBlockY, yBlockSize,
      std::move(globalFluidPicker));
}

std::unique_ptr<Aquifer> Aquifer::createDisabled(FluidPicker globalFluidPicker) {
  return std::make_unique<DisabledAquifer>(std::move(globalFluidPicker));
}

BlockStateFiller createOreVeinifier(const DensityNodePtr &veinToggle,
                                    const DensityNodePtr &veinRidged,
                                    const DensityNodePtr &veinGap,
                                    const PositionalRandomFactory &oreRandom) {
  struct VeinInfo {
    BlockState ore;
    BlockState rawOreBlock;
    BlockState filler;
    int minY;
    int maxY;
  };
  const VeinInfo copper{
      BlockState("minecraft:copper_ore"),
      BlockState("minecraft:raw_copper_block"),
      BlockState("minecraft:granite"),
      0,
      50,
  };
  const VeinInfo iron{
      BlockState("minecraft:deepslate_iron_ore"),
      BlockState("minecraft:raw_iron_block"),
      BlockState("minecraft:tuff"),
      -60,
      -8,
  };

  return [veinToggle, veinRidged, veinGap, &oreRandom, copper,
          iron](const DensityFunctionContext &ctx) -> std::optional<BlockState> {
    const int posY = ctx.blockY;
    // Copper veins exist in [0, 50] and iron veins in [-60, -8]; skip veinToggle outside bounds.
    if (posY < -60 || posY > 50 || (posY > -8 && posY < 0))
      return std::nullopt;
    const double veininessValue = veinToggle ? veinToggle->compute(ctx) : 0.0;
    const VeinInfo &vein = veininessValue > 0.0 ? copper : iron;
    const double veininessRidged = std::abs(veininessValue);
    const int distanceFromTop = vein.maxY - posY;
    const int distanceFromBottom = posY - vein.minY;
    if (distanceFromBottom < 0 || distanceFromTop < 0)
      return std::nullopt;

    const int distanceFromEdge = std::min(distanceFromTop, distanceFromBottom);
    const double edgeRoundoff = clampedMapValue(
        static_cast<double>(distanceFromEdge), 0.0, 20.0, -0.2, 0.0);
    if (veininessRidged + edgeRoundoff < static_cast<double>(0.4f))
      return std::nullopt;

    AnyRandomSource rng = oreRandom.at(ctx.blockX, posY, ctx.blockZ);
    if (rng.nextFloat() > 0.7f)
      return std::nullopt;
    if ((veinRidged ? veinRidged->compute(ctx) : 0.0) >= 0.0)
      return std::nullopt;

    const double richness = clampedMapValue(
        veininessRidged, static_cast<double>(0.4f), static_cast<double>(0.6f),
        static_cast<double>(0.1f), static_cast<double>(0.3f));
    if (rng.nextFloat() < richness &&
        (veinGap ? veinGap->compute(ctx) : 0.0) > static_cast<double>(-0.3f)) {
      return rng.nextFloat() < 0.02f ? vein.rawOreBlock : vein.ore;
    }
    return vein.filler;
  };
}

} // namespace redstone::worldgen
