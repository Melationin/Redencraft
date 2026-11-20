#include "redstone/worldgen/noise_chunk.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <stdexcept>

namespace redstone::worldgen {
namespace {

int floorDiv(int a, int b) noexcept {
  const int q = a / b;
  const int r = a % b;
  return (r != 0 && ((a ^ b) < 0)) ? q - 1 : q;
}

int floorMod(int a, int b) noexcept {
  const int r = a % b;
  return (r < 0) ? r + b : r;
}

std::int64_t packColumn(int x, int z) noexcept {
  return (static_cast<std::int64_t>(x) & 0xFFFFFFFFLL) |
         ((static_cast<std::int64_t>(z) & 0xFFFFFFFFLL) << 32);
}

} // namespace

struct NoiseChunk::Cache2DState final : CustomDensityFunction {
  explicit Cache2DState(DensityNodePtr fn)
      : function(std::move(fn)),
        yIndependent(!density_functions::dependsOnY(function.get())) {}

  double compute(const DensityFunctionContext &ctx) override {
    const std::int64_t key = packColumn(ctx.blockX, ctx.blockZ);
    if (hasLast && lastPos2D == key)
      return lastValue;
    hasLast = true;
    lastPos2D = key;
    lastValue = function->compute(ctx);
    return lastValue;
  }

  void fillArray(double *output, std::size_t count,
                 DensityContextProvider &provider) override {
    function->fillArray(output, count, provider);
  }

  DensityNodePtr wrapped() const override { return function; }
  bool isYIndependent() const noexcept override { return yIndependent; }

  DensityNodePtr function;
  bool yIndependent = false;
  bool hasLast = false;
  std::int64_t lastPos2D = 0;
  double lastValue = 0.0;
};

struct NoiseChunk::FlatCacheState final : CustomDensityFunction {
  FlatCacheState(DensityNodePtr fn, NoiseChunk &owner, bool fill)
      : function(std::move(fn)), owner_(owner), sizeXZ(owner.noiseSizeXZ_ + 1),
        values(static_cast<std::size_t>(sizeXZ * sizeXZ), 0.0) {
    if (fill) {
      for (int x = 0; x <= owner_.noiseSizeXZ_; ++x) {
        const int quartX = owner_.firstNoiseX_ + x;
        const int blockX = quartX * 4;
        for (int z = 0; z <= owner_.noiseSizeXZ_; ++z) {
          const int quartZ = owner_.firstNoiseZ_ + z;
          const int blockZ = quartZ * 4;
          values[static_cast<std::size_t>(x + z * sizeXZ)] = function->compute(
              DensityFunctionContext{blockX, 0, blockZ, nullptr});
        }
      }
    }
  }

  double compute(const DensityFunctionContext &ctx) override {
    const int quartX = ctx.blockX >> 2;
    const int quartZ = ctx.blockZ >> 2;
    const int x = quartX - owner_.firstNoiseX_;
    const int z = quartZ - owner_.firstNoiseZ_;
    if (x >= 0 && z >= 0 && x < sizeXZ && z < sizeXZ)
      return values[static_cast<std::size_t>(x + z * sizeXZ)];
    return function->compute(ctx);
  }

  void fillArray(double *output, std::size_t count,
                 DensityContextProvider &provider) override {
    provider.fillAllDirectly(output, count, *ownerNode);
  }

  DensityNodePtr wrapped() const override { return function; }
  bool isYIndependent() const noexcept override { return true; }

  DensityNodePtr function;
  NoiseChunk &owner_;
  int sizeXZ = 1;
  std::vector<double> values;
  const DensityNode *ownerNode = nullptr;
};

struct NoiseChunk::CacheAllInCellState final : CustomDensityFunction {
  CacheAllInCellState(DensityNodePtr fn, NoiseChunk &owner)
      : noiseFiller(std::move(fn)), owner_(owner),
        values(static_cast<std::size_t>(owner.cellWidth_ * owner.cellWidth_ *
                                        owner.cellHeight_),
               0.0) {}

  double compute(const DensityFunctionContext &ctx) override {
    if (ctx.activeToken != &owner_) [[unlikely]]
      return noiseFiller->compute(ctx);
    assert(owner_.interpolating_);
    const int x = owner_.inCellX_;
    const int y = owner_.inCellY_;
    const int z = owner_.inCellZ_;
    if (static_cast<unsigned>(x) < static_cast<unsigned>(owner_.cellWidth_) &&
        static_cast<unsigned>(y) < static_cast<unsigned>(owner_.cellHeight_) &&
        static_cast<unsigned>(z) < static_cast<unsigned>(owner_.cellWidth_))
        [[likely]] {
      const std::size_t idx = static_cast<std::size_t>(
          ((owner_.cellHeight_ - 1 - y) * owner_.cellWidth_ + x) *
              owner_.cellWidth_ +
          z);
      return values[idx];
    }
    return noiseFiller->compute(ctx);
  }

  void fillArray(double *output, std::size_t count,
                 DensityContextProvider &provider) override {
    provider.fillAllDirectly(output, count, *ownerNode);
  }

  DensityNodePtr wrapped() const override { return noiseFiller; }

  DensityNodePtr noiseFiller;
  NoiseChunk &owner_;
  std::vector<double> values;
  const DensityNode *ownerNode = nullptr;
};

struct NoiseChunk::CacheOnceState final : CustomDensityFunction {
  CacheOnceState(DensityNodePtr fn, NoiseChunk &owner)
      : function(std::move(fn)), owner_(owner) {}

  double compute(const DensityFunctionContext &ctx) override {
    if (ctx.activeToken != &owner_)
      return function->compute(ctx);
    if (hasArray && lastArrayCounter == owner_.arrayInterpolationCounter_)
      return lastArray[owner_.arrayIndex_];
    if (hasValue && lastCounter == owner_.interpolationCounter_)
      return lastValue;
    hasValue = true;
    lastCounter = owner_.interpolationCounter_;
    lastValue = function->compute(ctx);
    return lastValue;
  }

  void fillArray(double *output, std::size_t count,
                 DensityContextProvider &provider) override {
    if (hasArray && lastArrayCounter == owner_.arrayInterpolationCounter_ &&
        lastArray.size() == count) {
      std::copy_n(lastArray.data(), count, output);
      return;
    }
    function->fillArray(output, count, provider);
    lastArray.assign(output, output + count);
    hasArray = true;
    lastArrayCounter = owner_.arrayInterpolationCounter_;
  }

  DensityNodePtr wrapped() const override { return function; }
  bool isYIndependent() const noexcept override { return true; }

  DensityNodePtr function;
  NoiseChunk &owner_;
  bool hasValue = false;
  bool hasArray = false;
  std::uint64_t lastCounter = 0;
  std::uint64_t lastArrayCounter = 0;
  double lastValue = 0.0;
  std::vector<double> lastArray;
};

struct NoiseChunk::NoiseInterpolatorState final : CustomDensityFunction {
  NoiseInterpolatorState(DensityNodePtr fn, NoiseChunk &owner)
      : noiseFiller(std::move(fn)), owner_(owner),
        strideY(static_cast<std::size_t>(owner.cellCountY_ + 1)) {
    const std::size_t total =
        static_cast<std::size_t>(owner_.cellCountXZ_ + 1) * strideY;
    slice0.assign(total, 0.0);
    slice1.assign(total, 0.0);
  }

  void selectCellYZ(int cellYIndex, int cellZIndex) noexcept {
    const std::size_t off00 =
        static_cast<std::size_t>(cellZIndex) * strideY +
        static_cast<std::size_t>(cellYIndex);
    const std::size_t off10 = off00 + strideY;
    noise000 = slice0[off00];
    noise010 = slice0[off00 + 1];
    noise001 = slice0[off10];
    noise011 = slice0[off10 + 1];
    noise100 = slice1[off00];
    noise110 = slice1[off00 + 1];
    noise101 = slice1[off10];
    noise111 = slice1[off10 + 1];
  }

  void updateForY(double factorY) noexcept {
    valueXZ00 = worldgen_math::lerp(factorY, noise000, noise010);
    valueXZ10 = worldgen_math::lerp(factorY, noise100, noise110);
    valueXZ01 = worldgen_math::lerp(factorY, noise001, noise011);
    valueXZ11 = worldgen_math::lerp(factorY, noise101, noise111);
  }

  void updateForX(double factorX) noexcept {
    valueZ0 = worldgen_math::lerp(factorX, valueXZ00, valueXZ10);
    valueZ1 = worldgen_math::lerp(factorX, valueXZ01, valueXZ11);
  }

  void updateForZ(double factorZ) noexcept {
    value = worldgen_math::lerp(factorZ, valueZ0, valueZ1);
  }

  void swapSlices() noexcept { slice0.swap(slice1); }

  double compute(const DensityFunctionContext &ctx) override {
    if (ctx.activeToken != &owner_) [[unlikely]]
      return noiseFiller->compute(ctx);
    assert(owner_.interpolating_);
    if (!owner_.fillingCell_) [[likely]]
      return value;
    return worldgen_math::lerp3(
        static_cast<double>(owner_.inCellX_) /
            static_cast<double>(owner_.cellWidth_),
        static_cast<double>(owner_.inCellY_) /
            static_cast<double>(owner_.cellHeight_),
        static_cast<double>(owner_.inCellZ_) /
            static_cast<double>(owner_.cellWidth_),
        noise000, noise100, noise010, noise110, noise001, noise101, noise011,
        noise111);
  }

  void fillArray(double *output, std::size_t count,
                 DensityContextProvider &provider) override {
    if (owner_.fillingCell_) {
      const int cw = owner_.cellWidth_;
      const int ch = owner_.cellHeight_;
      if (provider.noiseChunkToken() == &owner_ &&
          count == static_cast<std::size_t>(cw * cw * ch) && cw <= 16) {
        const double invW = 1.0 / static_cast<double>(cw);
        const double invH = 1.0 / static_cast<double>(ch);
        double y00[16], y01[16], y10[16], y11[16], fz[16];
        for (int x = 0; x < cw; ++x) {
          const double fx = static_cast<double>(x) * invW;
          fz[x] = fx;
          y00[x] = worldgen_math::lerp(fx, noise000, noise100);
          y01[x] = worldgen_math::lerp(fx, noise010, noise110);
          y10[x] = worldgen_math::lerp(fx, noise001, noise101);
          y11[x] = worldgen_math::lerp(fx, noise011, noise111);
        }
        std::size_t idx = 0;
        for (int y = ch - 1; y >= 0; --y) {
          const double fy = static_cast<double>(y) * invH;
          for (int x = 0; x < cw; ++x) {
            const double z0 = worldgen_math::lerp(fy, y00[x], y01[x]);
            const double z1 = worldgen_math::lerp(fy, y10[x], y11[x]);
            for (int z = 0; z < cw; ++z) {
              output[idx++] = worldgen_math::lerp(fz[z], z0, z1);
            }
          }
        }
        return;
      }
      provider.fillAllDirectly(output, count, *ownerNode);
    } else {
      noiseFiller->fillArray(output, count, provider);
    }
  }

  DensityNodePtr wrapped() const override { return noiseFiller; }

  DensityNodePtr noiseFiller;
  NoiseChunk &owner_;
  std::size_t strideY = 1;
  const DensityNode *ownerNode = nullptr;
  std::vector<double> slice0;
  std::vector<double> slice1;
  double noise000 = 0.0, noise001 = 0.0, noise100 = 0.0, noise101 = 0.0;
  double noise010 = 0.0, noise011 = 0.0, noise110 = 0.0, noise111 = 0.0;
  double valueXZ00 = 0.0, valueXZ10 = 0.0, valueXZ01 = 0.0, valueXZ11 = 0.0;
  double valueZ0 = 0.0, valueZ1 = 0.0, value = 0.0;
};

DensityFunctionContext NoiseChunk::SliceContextProvider::forIndex(int cellYIndex) {
  owner_.cellStartBlockY_ =
      (cellYIndex + owner_.cellNoiseMinY_) * owner_.cellHeight_;
  ++owner_.interpolationCounter_;
  owner_.inCellY_ = 0;
  owner_.arrayIndex_ = static_cast<std::size_t>(cellYIndex);
  return owner_.currentContext();
}

void NoiseChunk::SliceContextProvider::fillAllDirectly(double *output,
                                                       std::size_t count,
                                                       const DensityNode &node) {
  for (std::size_t i = 0; i < count; ++i) {
    output[i] = node.compute(forIndex(static_cast<int>(i)));
  }
}

std::unique_ptr<NoiseChunk>
NoiseChunk::forChunk(const BaseChunk &chunk, RandomState &randomState,
                     const NoiseGeneratorSettings &settings,
                     FluidPicker globalFluidPicker, DensityNodePtr beardifier) {
  const int newMinY = std::max(settings.minY, chunk.minY());
  const int rawTop = std::min(settings.minY + settings.height, chunk.maxY());
  const int clampedHeight = std::max(0, rawTop - newMinY);
  const int cw = settings.sizeHorizontal * 4;
  const int cellCountXZ = 16 / cw;
  return std::make_unique<NoiseChunk>(
      cellCountXZ, randomState, chunk.pos().x * 16, chunk.pos().z * 16,
      newMinY, clampedHeight, settings.sizeHorizontal, settings.sizeVertical,
      settings, std::move(globalFluidPicker), std::move(beardifier), true,
      false);
}

std::unique_ptr<NoiseChunk>
NoiseChunk::forChunkBiomes(const BaseChunk &chunk, RandomState &randomState,
                           const NoiseGeneratorSettings &settings) {
  const int newMinY = std::max(settings.minY, chunk.minY());
  const int rawTop = std::min(settings.minY + settings.height, chunk.maxY());
  const int clampedHeight = std::max(0, rawTop - newMinY);
  const int cw = settings.sizeHorizontal * 4;
  const int cellCountXZ = 16 / cw;
  return std::make_unique<NoiseChunk>(
      cellCountXZ, randomState, chunk.pos().x * 16, chunk.pos().z * 16,
      newMinY, clampedHeight, settings.sizeHorizontal, settings.sizeVertical,
      settings, FluidPicker{}, DensityNodePtr{}, false, false);
}

NoiseChunk::NoiseChunk(int cellCountXZ, RandomState &randomState,
                       int firstBlockX, int firstBlockZ, int minY, int height,
                       int sizeHorizontal, int sizeVertical,
                       const NoiseGeneratorSettings &settings,
                       FluidPicker globalFluidPicker, DensityNodePtr beardifier,
                       bool initTerrain, bool isColumn)
    : cellCountXZ_(cellCountXZ), cellWidth_(sizeHorizontal * 4),
      cellHeight_(sizeVertical * 4), minY_(minY), height_(height),
      isColumn_(isColumn), sliceFillingContextProvider_(*this),
      beardifier_(beardifier ? std::move(beardifier)
                             : density_functions::beardifier()) {
  wrapped_.reserve(512);
  interpolators_.reserve(16);
  cellCaches_.reserve(4);
  if (cellWidth_ == 4) {
    cellWidthShift_ = 2;
    cellWidthMask_ = 3;
  } else if (cellWidth_ == 8) {
    cellWidthShift_ = 3;
    cellWidthMask_ = 7;
  }
  cellCountY_ = floorDiv(height, cellHeight_);
  cellNoiseMinY_ = floorDiv(minY, cellHeight_);
  firstCellX_ = floorDiv(firstBlockX, cellWidth_);
  firstCellZ_ = floorDiv(firstBlockZ, cellWidth_);
  firstNoiseX_ = firstBlockX >> 2;
  firstNoiseZ_ = firstBlockZ >> 2;
  noiseSizeXZ_ = (cellCountXZ_ * cellWidth_) >> 2;

  if (initTerrain) {
    ensureTerrainInitialized(randomState, settings,
                             std::move(globalFluidPicker),
                             std::move(beardifier_));
  }
}

void NoiseChunk::ensureTerrainInitialized(RandomState &randomState,
                                          const NoiseGeneratorSettings &settings,
                                          FluidPicker globalFluidPicker,
                                          DensityNodePtr beardifier) {
  if (terrainInitialized_)
    return;
  terrainInitialized_ = true;
  if (beardifier)
    beardifier_ = std::move(beardifier);
  if (!beardifier_)
    beardifier_ = density_functions::beardifier();

  const NoiseRouter &router = randomState.router();
  const int firstBlockX = firstCellX_ * cellWidth_;
  const int firstBlockZ = firstCellZ_ * cellWidth_;

  // 先包装核心地形通道与 preliminarySurfaceLevel
  DensityNodePtr wrappedFinal = wrap(router.finalDensity);
  preliminarySurfaceLevel_ = wrap(router.preliminarySurfaceLevel);
  DensityNodePtr baseFinal = density_functions::marker(
      MarkerKind::CacheAllInCell,
      density_functions::twoArg(TwoArgOp::Add, wrappedFinal, beardifier_));
  fullNoiseDensity_ = wrap(baseFinal);
  finalCellCache_ = cellCaches_.empty() ? nullptr : cellCaches_.back().get();

  // 记录 finalDensity 自身使用的 interpolator 数量；之后若有含水层/矿脉新增插值器才需在体素循环中更新
  postDensityInterpStart_ = interpolators_.size();
  // 矿脉通道的 Interpolated 插值器只在 OreVeinifier 真正读取时推进；含水层/其它
  // postDensity 插值器仍逐体素 eager 更新。
  eagerPostInterpEnd_ = interpolators_.size();
  veinInterpStart_ = eagerPostInterpEnd_;

  NoiseRouter wrappedAquiferRouter{
      .barrierNoise = wrap(router.barrierNoise),
      .fluidLevelFloodednessNoise = wrap(router.fluidLevelFloodednessNoise),
      .fluidLevelSpreadNoise = wrap(router.fluidLevelSpreadNoise),
      .lavaNoise = wrap(router.lavaNoise),
      .temperature = {},
      .vegetation = {},
      .continents = {},
      .erosion = wrap(router.erosion),
      .depth = wrap(router.depth),
      .ridges = {},
      .preliminarySurfaceLevel = preliminarySurfaceLevel_,
      .finalDensity = wrappedFinal,
      .veinToggle = {},
      .veinRidged = {},
      .veinGap = {},
  };

  if (!settings.aquifersEnabled) {
    aquifer_ = Aquifer::createDisabled(std::move(globalFluidPicker));
  } else {
    const ChunkTicketPos cpos{firstBlockX >> 4, firstBlockZ >> 4};
    aquifer_ = Aquifer::create(*this, cpos, wrappedAquiferRouter,
                               randomState.aquiferRandom(), minY_, height_,
                               std::move(globalFluidPicker));
  }

  if (!isColumn_ && settings.oreVeinsEnabled) {
    DensityNodePtr wrappedVeinToggle = wrap(router.veinToggle);
    DensityNodePtr wrappedVeinRidged = wrap(router.veinRidged);
    DensityNodePtr wrappedVeinGap = wrap(router.veinGap);
    oreVeinifier_ = createOreVeinifier(
        wrappedVeinToggle, wrappedVeinRidged, wrappedVeinGap,
        randomState.oreRandom());
  }

  veinInterpEnd_ = interpolators_.size();
  hasPostDensityInterpolators_ = (eagerPostInterpEnd_ > postDensityInterpStart_);
}

DensityNodePtr NoiseChunk::wrap(const DensityNodePtr &function) {
  return wrapRec(function);
}

DensityNodePtr NoiseChunk::wrapRec(const DensityNodePtr &node) {
  if (!node)
    return nullptr;
  // DAG 无环：递归前不会重复访问同一节点，因此只需一个跨 wrap() 调用的 memo。
  if (auto it = wrapped_.find(node.get()); it != wrapped_.end())
    return it->second;

  auto rec = [&](const DensityNodePtr &child) { return wrapRec(child); };

  DensityNodePtr result;
  if (node->kind == DensityNodeKind::Marker) {
    const DensityNodePtr inner = rec(node->arg1);
    switch (node->markerKind) {
    case MarkerKind::Interpolated: {
      auto state = std::make_shared<NoiseInterpolatorState>(inner, *this);
      interpolators_.push_back(state);
      result = density_functions::custom(state, inner->minValue, inner->maxValue);
      state->ownerNode = result.get();
      break;
    }
    case MarkerKind::FlatCache: {
      auto state = std::make_shared<FlatCacheState>(inner, *this, true);
      result = density_functions::custom(state, inner->minValue, inner->maxValue);
      state->ownerNode = result.get();
      break;
    }
    case MarkerKind::Cache2D: {
      auto state = std::make_shared<Cache2DState>(inner);
      result = density_functions::custom(state, inner->minValue, inner->maxValue);
      break;
    }
    case MarkerKind::CacheOnce: {
      auto state = std::make_shared<CacheOnceState>(inner, *this);
      result = density_functions::custom(state, inner->minValue, inner->maxValue);
      break;
    }
    case MarkerKind::CacheAllInCell: {
      auto state = std::make_shared<CacheAllInCellState>(inner, *this);
      cellCaches_.push_back(state);
      result = density_functions::custom(state, inner->minValue, inner->maxValue);
      state->ownerNode = result.get();
      break;
    }
    }
  } else {
    switch (node->kind) {
    case DensityNodeKind::Constant:
    case DensityNodeKind::YClampedGradient:
    case DensityNodeKind::Noise:
    case DensityNodeKind::ShiftA:
    case DensityNodeKind::ShiftB:
    case DensityNodeKind::Shift:
    case DensityNodeKind::OldBlendedNoise:
    case DensityNodeKind::EndIslands:
    case DensityNodeKind::BlendAlpha:
    case DensityNodeKind::BlendOffset:
    case DensityNodeKind::Custom:
      result = node;
      break;
    case DensityNodeKind::Beardifier:
      result = beardifier_;
      break;
    case DensityNodeKind::Mapped: {
      DensityNodePtr c1 = rec(node->arg1);
      result = (c1.get() == node->arg1.get())
                   ? node
                   : density_functions::mapped(node->mappedOp, std::move(c1));
      break;
    }
    case DensityNodeKind::Clamp: {
      DensityNodePtr c1 = rec(node->arg1);
      result = (c1.get() == node->arg1.get())
                   ? node
                   : density_functions::clamp(std::move(c1), node->param1,
                                              node->param2);
      break;
    }
    case DensityNodeKind::TwoArg: {
      DensityNodePtr c1 = rec(node->arg1);
      DensityNodePtr c2 = rec(node->arg2);
      result = (c1.get() == node->arg1.get() && c2.get() == node->arg2.get())
                   ? node
                   : density_functions::twoArg(node->twoArgOp, std::move(c1),
                                               std::move(c2));
      break;
    }
    case DensityNodeKind::RangeChoice: {
      DensityNodePtr c1 = rec(node->arg1);
      DensityNodePtr c2 = rec(node->arg2);
      DensityNodePtr c3 = rec(node->arg3);
      result = (c1.get() == node->arg1.get() && c2.get() == node->arg2.get() &&
                c3.get() == node->arg3.get())
                   ? node
                   : density_functions::rangeChoice(
                         std::move(c1), node->param1, node->param2,
                         std::move(c2), std::move(c3));
      break;
    }
    case DensityNodeKind::ShiftedNoise: {
      DensityNodePtr c1 = rec(node->arg1);
      DensityNodePtr c2 = rec(node->arg2);
      DensityNodePtr c3 = rec(node->arg3);
      result = (c1.get() == node->arg1.get() && c2.get() == node->arg2.get() &&
                c3.get() == node->arg3.get())
                   ? node
                   : density_functions::shiftedNoise(
                         std::move(c1), std::move(c2), std::move(c3),
                         node->xzScale, node->yScale, node->noiseHolder.noiseId,
                         node->noiseHolder.noise);
      break;
    }
    case DensityNodeKind::WeirdScaledSampler: {
      DensityNodePtr c1 = rec(node->arg1);
      result = (c1.get() == node->arg1.get())
                   ? node
                   : density_functions::weirdScaledSampler(
                         std::move(c1), node->noiseHolder.noiseId,
                         node->rarityMapper, node->noiseHolder.noise);
      break;
    }
    case DensityNodeKind::Spline: {
      CubicSplinePtr sp = node->spline->mapAll(rec);
      result = (sp.get() == node->spline.get())
                   ? node
                   : density_functions::spline(std::move(sp));
      break;
    }
    case DensityNodeKind::FindTopSurface: {
      DensityNodePtr c1 = rec(node->arg1);
      DensityNodePtr c2 = rec(node->arg2);
      result = (c1.get() == node->arg1.get() && c2.get() == node->arg2.get())
                   ? node
                   : density_functions::findTopSurface(
                         std::move(c1), std::move(c2), node->intParam1,
                         node->intParam2);
      break;
    }
    case DensityNodeKind::BlendDensity: {
      DensityNodePtr c1 = rec(node->arg1);
      result = (c1.get() == node->arg1.get())
                   ? node
                   : density_functions::blendDensity(std::move(c1));
      break;
    }
    case DensityNodeKind::Marker:
      break;
    }
  }

  wrapped_[node.get()] = result;
  return result;
}

ClimateSampler NoiseChunk::cachedClimateSampler(
    const NoiseRouter &router,
    std::vector<ClimateParameterPoint> spawnTarget) {
  ClimateSampler s{
      .temperature = wrap(router.temperature),
      .humidity = wrap(router.vegetation),
      .continentalness = wrap(router.continents),
      .erosion = wrap(router.erosion),
      .depth = wrap(router.depth),
      .weirdness = wrap(router.ridges),
      .spawnTarget = std::move(spawnTarget),
  };
  s.computeYDependentMask();
  return s;
}

int NoiseChunk::preliminarySurfaceLevel(int sampleX, int sampleZ) {
  const int quantizedX = (sampleX >> 2) << 2;
  const int quantizedZ = (sampleZ >> 2) << 2;
  const std::int64_t key = packColumn(quantizedX, quantizedZ);
  const auto &pred = preliminarySurfaceLevelCache_.find(key);
  if (pred != preliminarySurfaceLevelCache_.end()) {
    return pred->second;
  }
  const int val = static_cast<int>(std::floor(
      preliminarySurfaceLevel_
          ? preliminarySurfaceLevel_->compute(
                DensityFunctionContext{quantizedX, 0, quantizedZ, nullptr})
          : 0.0));
  preliminarySurfaceLevelCache_[key] = val;
  return val;
}

int NoiseChunk::maxPreliminarySurfaceLevel(int minBlockX, int minBlockZ,
                                           int maxBlockX, int maxBlockZ) {
  int maxY = std::numeric_limits<int>::min();
  for (int blockZ = minBlockZ; blockZ <= maxBlockZ; blockZ += 4) {
    for (int blockX = minBlockX; blockX <= maxBlockX; blockX += 4) {
      maxY = std::max(maxY, preliminarySurfaceLevel(blockX, blockZ));
    }
  }
  return maxY;
}

DensityFunctionContext NoiseChunk::forIndex(int cellIndex) {
  int zInCell = 0;
  int xInCell = 0;
  int yInCell = 0;
  if (cellWidthShift_ >= 0) {
    zInCell = cellIndex & cellWidthMask_;
    const int xyIndex = cellIndex >> cellWidthShift_;
    xInCell = xyIndex & cellWidthMask_;
    yInCell = cellHeight_ - 1 - (xyIndex >> cellWidthShift_);
  } else {
    zInCell = floorMod(cellIndex, cellWidth_);
    const int xyIndex = floorDiv(cellIndex, cellWidth_);
    xInCell = floorMod(xyIndex, cellWidth_);
    yInCell = cellHeight_ - 1 - floorDiv(xyIndex, cellWidth_);
  }
  inCellX_ = xInCell;
  inCellY_ = yInCell;
  inCellZ_ = zInCell;
  arrayIndex_ = static_cast<std::size_t>(cellIndex);
  return currentContext();
}

void NoiseChunk::fillAllDirectly(double *output, std::size_t,
                                 const DensityNode &node) {
  arrayIndex_ = 0;
  for (int yInCell = cellHeight_ - 1; yInCell >= 0; --yInCell) {
    inCellY_ = yInCell;
    for (int xInCell = 0; xInCell < cellWidth_; ++xInCell) {
      inCellX_ = xInCell;
      for (int zInCell = 0; zInCell < cellWidth_; ++zInCell) {
        inCellZ_ = zInCell;
        output[arrayIndex_++] = node.compute(currentContext());
      }
    }
  }
}

void NoiseChunk::fillSlice(bool slice0, int cellX) {
  cellStartBlockX_ = cellX * cellWidth_;
  inCellX_ = 0;
  const std::size_t sliceLen = static_cast<std::size_t>(cellCountY_ + 1);
  for (int cellZIndex = 0; cellZIndex <= cellCountXZ_; ++cellZIndex) {
    const int cellZ = firstCellZ_ + cellZIndex;
    cellStartBlockZ_ = cellZ * cellWidth_;
    inCellZ_ = 0;
    ++arrayInterpolationCounter_;
    for (const auto &interp : interpolators_) {
      double *dst =
          (slice0 ? interp->slice0 : interp->slice1).data() +
          static_cast<std::size_t>(cellZIndex) * interp->strideY;
      interp->noiseFiller->fillArray(dst, sliceLen, sliceFillingContextProvider_);
    }
  }
  ++arrayInterpolationCounter_;
}

void NoiseChunk::initializeForFirstCellX() {
  if (interpolating_)
    throw std::runtime_error("Starting NoiseChunk interpolation twice");
  interpolating_ = true;
  interpolationCounter_ = 0;
  fillSlice(true, firstCellX_);
}

void NoiseChunk::advanceCellX(int cellXIndex) {
  fillSlice(false, firstCellX_ + cellXIndex + 1);
  cellStartBlockX_ = (firstCellX_ + cellXIndex) * cellWidth_;
}

void NoiseChunk::selectCellYZ(int cellYIndex, int cellZIndex) {
  for (const auto &interp : interpolators_)
    interp->selectCellYZ(cellYIndex, cellZIndex);

  fillingCell_ = true;
  cellStartBlockY_ = (cellYIndex + cellNoiseMinY_) * cellHeight_;
  cellStartBlockZ_ = (firstCellZ_ + cellZIndex) * cellWidth_;
  ++arrayInterpolationCounter_;

  for (const auto &cellCache : cellCaches_) {
    cellCache->noiseFiller->fillArray(cellCache->values.data(),
                                      cellCache->values.size(), *this);
  }

  ++arrayInterpolationCounter_;
  fillingCell_ = false;
}

void NoiseChunk::updateForY(int posY, double factorY) {
  inCellY_ = posY - cellStartBlockY_;
  for (std::size_t i = postDensityInterpStart_; i < interpolators_.size(); ++i)
    interpolators_[i]->updateForY(factorY);
}

void NoiseChunk::updateForX(int posX, double factorX) {
  inCellX_ = posX - cellStartBlockX_;
  for (std::size_t i = postDensityInterpStart_; i < interpolators_.size(); ++i)
    interpolators_[i]->updateForX(factorX);
}

void NoiseChunk::updateForZ(int posZ, double factorZ) {
  inCellZ_ = posZ - cellStartBlockZ_;
  ++interpolationCounter_;
  for (std::size_t i = postDensityInterpStart_; i < interpolators_.size(); ++i)
    interpolators_[i]->updateForZ(factorZ);
}

void NoiseChunk::swapSlices() {
  for (const auto &interp : interpolators_)
    interp->swapSlices();
}

void NoiseChunk::stopInterpolation() {
  if (!interpolating_)
    throw std::runtime_error("Stopping NoiseChunk interpolation twice");
  interpolating_ = false;
}

std::span<const double> NoiseChunk::directFinalCellValues() const noexcept {
  if (!finalCellCache_)
    return {};
  return finalCellCache_->values;
}

void NoiseChunk::prepareVeinInterpolatorsFor(int x, int y, int z) {
  if (veinInterpStart_ >= veinInterpEnd_)
    return;
  // 与 OreVeinifier 的首个早退条件一致：区间外不会读取矿脉插值器。
  if (y < -60 || y > 50 || (y > -8 && y < 0))
    return;
  inCellX_ = x - cellStartBlockX_;
  inCellY_ = y - cellStartBlockY_;
  inCellZ_ = z - cellStartBlockZ_;
  const double factorY =
      static_cast<double>(inCellY_) / static_cast<double>(cellHeight_);
  const double factorX =
      static_cast<double>(inCellX_) / static_cast<double>(cellWidth_);
  const double factorZ =
      static_cast<double>(inCellZ_) / static_cast<double>(cellWidth_);
  for (std::size_t i = veinInterpStart_; i < veinInterpEnd_; ++i) {
    interpolators_[i]->updateForY(factorY);
    interpolators_[i]->updateForX(factorX);
    interpolators_[i]->updateForZ(factorZ);
  }
}

std::optional<BlockState> NoiseChunk::computeSubstanceFast(int x, int y, int z,
                                                           double density) {
  prepareVeinInterpolatorsFor(x, y, z);
  const DensityFunctionContext ctx{x, y, z, this};
  if (auto state = aquifer_->computeSubstance(ctx, density); state.has_value()) {
    return state;
  }
  return oreVeinifier_ ? oreVeinifier_(ctx) : std::nullopt;
}

std::optional<BlockState> NoiseChunk::getInterpolatedState() {
  const DensityFunctionContext ctx = currentContext();
  if (auto state =
          aquifer_->computeSubstance(ctx, fullNoiseDensity_->compute(ctx));
      state.has_value()) {
    return state;
  }
  return oreVeinifier_ ? oreVeinifier_(ctx) : std::nullopt;
}

double NoiseChunk::getInterpolatedDensity() {
  return fullNoiseDensity_ ? fullNoiseDensity_->compute(currentContext()) : 0.0;
}

} // namespace redstone::worldgen
