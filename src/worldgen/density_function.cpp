#include "redstone/worldgen/density_function.hpp"
#include "redstone/flat_hash_map.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace redstone::worldgen {
namespace {

float linearExtendFloat(float input, const std::vector<float> &locations,
                        float value, const std::vector<float> &derivatives,
                        std::size_t index) noexcept {
  const float deriv = derivatives[index];
  return deriv == 0.0f ? value : value + deriv * (input - locations[index]);
}

int findSplineInterval(const std::vector<float> &locations,
                       float input) noexcept {
  // Exact match to Mth.binarySearch(0, len, i -> input < locations[i]) - 1
  int from = 0;
  int len = static_cast<int>(locations.size());
  while (len > 0) {
    const int half = len / 2;
    const int mid = from + half;
    if (input < locations[static_cast<std::size_t>(mid)]) {
      len = half;
    } else {
      from = mid + 1;
      len -= half + 1;
    }
  }
  return from - 1;
}

double applyMappedTransform(MappedOp op, double input) noexcept {
  switch (op) {
  case MappedOp::Abs:
    return std::abs(input);
  case MappedOp::Square:
    return input * input;
  case MappedOp::Cube:
    return input * input * input;
  case MappedOp::HalfNegative:
    return input > 0.0 ? input : input * 0.5;
  case MappedOp::QuarterNegative:
    return input > 0.0 ? input : input * 0.25;
  case MappedOp::Invert:
    return 1.0 / input;
  case MappedOp::Squeeze: {
    const double c = std::clamp(input, -1.0, 1.0);
    return c / 2.0 - c * c * c / 24.0;
  }
  }
  return input;
}

// 无缓存上下文（NoiseChunk 路径）直接走 computeUncached，避免每条边都经过
// compute 的缓存检查与一次额外函数调用；有缓存时保持原有语义。
inline double computeChild(const DensityNodePtr &node,
                           const DensityFunctionContext &ctx) noexcept {
  return ctx.evalCache ? node->compute(ctx) : node->computeUncached(ctx);
}

double mapRarity(RarityMapperKind kind, double input) noexcept {
  if (kind == RarityMapperKind::Type1) {
    // QuantizedSpaghettiRarity.getSpaghettiRarity3D
    if (input < -0.5)
      return 0.75;
    if (input < 0.0)
      return 1.0;
    return input < 0.5 ? 1.5 : 2.0;
  }
  // QuantizedSpaghettiRarity.getSphaghettiRarity2D
  if (input < -0.75)
    return 0.5;
  if (input < -0.5)
    return 0.75;
  if (input < 0.5)
    return 1.0;
  return input < 0.75 ? 2.0 : 3.0;
}

double maxRarityOf(RarityMapperKind kind) noexcept {
  return kind == RarityMapperKind::Type1 ? 2.0 : 3.0;
}

float endIslandHeightValue(const SimplexNoise &islandNoise, int sectionX,
                           int sectionZ) noexcept {
  const int chunkX = sectionX / 2;
  const int chunkZ = sectionZ / 2;
  const int subX = sectionX % 2;
  const int subZ = sectionZ % 2;
  const double distSq = static_cast<double>(sectionX * sectionX +
                                            sectionZ * sectionZ);
  float doffs = 100.0f - static_cast<float>(std::sqrt(distSq)) * 8.0f;
  doffs = std::clamp(doffs, -100.0f, 80.0f);

  for (int xo = -12; xo <= 12; ++xo) {
    for (int zo = -12; zo <= 12; ++zo) {
      const std::int64_t totalChunkX = static_cast<std::int64_t>(chunkX) + xo;
      const std::int64_t totalChunkZ = static_cast<std::int64_t>(chunkZ) + zo;
      if (totalChunkX * totalChunkX + totalChunkZ * totalChunkZ > 4096LL &&
          islandNoise.getValue(static_cast<double>(totalChunkX),
                               static_cast<double>(totalChunkZ)) < -0.9f) {
        const float islandSize =
            std::fmod(std::abs(static_cast<float>(totalChunkX)) * 3439.0f +
                          std::abs(static_cast<float>(totalChunkZ)) * 147.0f,
                      13.0f) +
            9.0f;
        const float xd = static_cast<float>(subX - xo * 2);
        const float zd = static_cast<float>(subZ - zo * 2);
        const float d2 = xd * xd + zd * zd;
        float newDoffs =
            100.0f - static_cast<float>(std::sqrt(static_cast<double>(d2))) *
                         islandSize;
        newDoffs = std::clamp(newDoffs, -100.0f, 80.0f);
        doffs = std::max(doffs, newDoffs);
      }
    }
  }
  return doffs;
}

std::shared_ptr<const SimplexNoise> buildEndIslandNoise(std::int64_t seed) {
  LegacyRandomSource random(seed);
  random.consumeCount(17292);
  return std::make_shared<SimplexNoise>(random);
}

} // namespace

CubicSplinePtr CubicSpline::constant(float value) {
  auto sp = std::shared_ptr<CubicSpline>(new CubicSpline());
  sp->isConstant_ = true;
  sp->constantValue_ = value;
  sp->minValue_ = value;
  sp->maxValue_ = value;
  return sp;
}

CubicSplinePtr CubicSpline::multipoint(DensityNodePtr coordinate,
                                       std::vector<float> locations,
                                       std::vector<CubicSplinePtr> values,
                                       std::vector<float> derivatives) {
  if (locations.empty() || locations.size() != values.size() ||
      locations.size() != derivatives.size()) {
    throw std::invalid_argument("Invalid CubicSpline::multipoint arrays");
  }
  auto sp = std::shared_ptr<CubicSpline>(new CubicSpline());
  sp->isConstant_ = false;
  sp->coordinate_ = std::move(coordinate);
  sp->locations_ = std::move(locations);
  sp->values_ = std::move(values);
  sp->derivatives_ = std::move(derivatives);

  const std::size_t lastIdx = sp->locations_.size() - 1;
  float minV = std::numeric_limits<float>::infinity();
  float maxV = -std::numeric_limits<float>::infinity();
  const float minInput = static_cast<float>(sp->coordinate_->minValue);
  const float maxInput = static_cast<float>(sp->coordinate_->maxValue);

  if (minInput < sp->locations_[0]) {
    const float e1 = linearExtendFloat(minInput, sp->locations_,
                                       sp->values_[0]->minValue(),
                                       sp->derivatives_, 0);
    const float e2 = linearExtendFloat(minInput, sp->locations_,
                                       sp->values_[0]->maxValue(),
                                       sp->derivatives_, 0);
    minV = std::min(minV, std::min(e1, e2));
    maxV = std::max(maxV, std::max(e1, e2));
  }
  if (maxInput > sp->locations_[lastIdx]) {
    const float e1 = linearExtendFloat(maxInput, sp->locations_,
                                       sp->values_[lastIdx]->minValue(),
                                       sp->derivatives_, lastIdx);
    const float e2 = linearExtendFloat(maxInput, sp->locations_,
                                       sp->values_[lastIdx]->maxValue(),
                                       sp->derivatives_, lastIdx);
    minV = std::min(minV, std::min(e1, e2));
    maxV = std::max(maxV, std::max(e1, e2));
  }
  for (const auto &v : sp->values_) {
    minV = std::min(minV, v->minValue());
    maxV = std::max(maxV, v->maxValue());
  }
  for (std::size_t i = 0; i < lastIdx; ++i) {
    const float xDiff = sp->locations_[i + 1] - sp->locations_[i];
    const float d1 = sp->derivatives_[i];
    const float d2 = sp->derivatives_[i + 1];
    if (d1 != 0.0f || d2 != 0.0f) {
      const float min1 = sp->values_[i]->minValue();
      const float max1 = sp->values_[i]->maxValue();
      const float min2 = sp->values_[i + 1]->minValue();
      const float max2 = sp->values_[i + 1]->maxValue();
      const float p1 = d1 * xDiff;
      const float p2 = d2 * xDiff;
      const float minLerp1 = std::min(min1, min2);
      const float maxLerp1 = std::max(max1, max2);
      const float minA = p1 - max2 + min1;
      const float maxA = p1 - min2 + max1;
      const float minB = -p2 + min2 - max1;
      const float maxB = -p2 + max2 - min1;
      minV = std::min(minV, minLerp1 + 0.25f * std::min(minA, minB));
      maxV = std::max(maxV, maxLerp1 + 0.25f * std::max(maxA, maxB));
    }
  }
  sp->minValue_ = minV;
  sp->maxValue_ = maxV;
  return sp;
}

float CubicSpline::apply(int blockX, int blockY, int blockZ) const {
  return apply(DensityFunctionContext{blockX, blockY, blockZ, nullptr});
}

float CubicSpline::apply(const DensityFunctionContext &ctx) const {
  if (isConstant_)
    return constantValue_;
  const float input = static_cast<float>(computeChild(coordinate_, ctx));
  const int start = findSplineInterval(locations_, input);
  const int lastIdx = static_cast<int>(locations_.size()) - 1;
  if (start < 0) {
    return linearExtendFloat(input, locations_, values_[0]->apply(ctx),
                             derivatives_, 0);
  }
  if (start == lastIdx) {
    const auto idx = static_cast<std::size_t>(lastIdx);
    return linearExtendFloat(input, locations_, values_[idx]->apply(ctx),
                             derivatives_, idx);
  }
  const auto s = static_cast<std::size_t>(start);
  const float x1 = locations_[s];
  const float x2 = locations_[s + 1];
  const float t = (input - x1) / (x2 - x1);
  const float d1 = derivatives_[s];
  const float d2 = derivatives_[s + 1];
  const float y1 = values_[s]->apply(ctx);
  const float y2 = values_[s + 1]->apply(ctx);
  const float a = d1 * (x2 - x1) - (y2 - y1);
  const float b = -d2 * (x2 - x1) + (y2 - y1);
  const float lerpY = y1 + t * (y2 - y1);
  const float lerpAB = a + t * (b - a);
  return lerpY + t * (1.0f - t) * lerpAB;
}

CubicSplinePtr CubicSpline::mapAll(
    const std::function<DensityNodePtr(const DensityNodePtr &)> &visitor) const {
  if (isConstant_)
    return shared_from_this();
  DensityNodePtr mappedCoord = visitor(coordinate_);
  bool changed = (mappedCoord.get() != coordinate_.get());
  std::vector<CubicSplinePtr> mappedValues;
  mappedValues.reserve(values_.size());
  for (const auto &v : values_) {
    CubicSplinePtr mv = v->mapAll(visitor);
    if (mv.get() != v.get())
      changed = true;
    mappedValues.push_back(std::move(mv));
  }
  if (!changed)
    return shared_from_this();
  return multipoint(std::move(mappedCoord), locations_, std::move(mappedValues),
                    derivatives_);
}

double DensityNode::compute(int blockX, int blockY, int blockZ) const {
  return compute(DensityFunctionContext{blockX, blockY, blockZ, nullptr});
}

namespace {

bool isCacheableKind(DensityNodeKind kind) noexcept {
  switch (kind) {
  case DensityNodeKind::Noise:
  case DensityNodeKind::ShiftedNoise:
  case DensityNodeKind::ShiftA:
  case DensityNodeKind::ShiftB:
  case DensityNodeKind::Shift:
  case DensityNodeKind::WeirdScaledSampler:
  case DensityNodeKind::OldBlendedNoise:
  case DensityNodeKind::Spline:
  case DensityNodeKind::EndIslands:
    return true;
  default:
    return false;
  }
}

} // namespace

double DensityNode::compute(const DensityFunctionContext &ctx) const {
  if (!ctx.evalCache || !isCacheableKind(kind))
    return computeUncached(ctx);
  if (const double *cached = ctx.evalCache->find(this))
    return *cached;
  const double value = computeUncached(ctx);
  ctx.evalCache->insert(this, value);
  return value;
}

double DensityNode::computeUncached(const DensityFunctionContext &ctx) const {
  const int blockX = ctx.blockX;
  const int blockY = ctx.blockY;
  const int blockZ = ctx.blockZ;
  switch (kind) {
  case DensityNodeKind::Constant:
    return constantValue;
  case DensityNodeKind::Mapped:
    return applyMappedTransform(mappedOp, computeChild(arg1, ctx));
  case DensityNodeKind::Clamp:
    return std::clamp(computeChild(arg1, ctx), param1, param2);
  case DensityNodeKind::TwoArg: {
    const double v1 = computeChild(arg1, ctx);
    switch (twoArgOp) {
    case TwoArgOp::Add:
      return v1 + computeChild(arg2, ctx);
    case TwoArgOp::Mul:
      return v1 == 0.0 ? 0.0 : v1 * computeChild(arg2, ctx);
    case TwoArgOp::Min:
      return v1 < arg2->minValue ? v1 : std::min(v1, computeChild(arg2, ctx));
    case TwoArgOp::Max:
      return v1 > arg2->maxValue ? v1 : std::max(v1, computeChild(arg2, ctx));
    }
    return 0.0;
  }
  case DensityNodeKind::YClampedGradient: {
    const double factor =
        (static_cast<double>(blockY) - static_cast<double>(intParam1)) /
        (static_cast<double>(intParam2) - static_cast<double>(intParam1));
    return worldgen_math::clampedLerp(factor, param1, param2);
  }
  case DensityNodeKind::RangeChoice: {
    const double v = computeChild(arg1, ctx);
    return (v >= param1 && v < param2) ? computeChild(arg2, ctx)
                                       : computeChild(arg3, ctx);
  }
  case DensityNodeKind::Noise:
    return noiseHolder.getValue(static_cast<double>(blockX) * xzScale,
                                static_cast<double>(blockY) * yScale,
                                static_cast<double>(blockZ) * xzScale);
  case DensityNodeKind::ShiftedNoise: {
    const double sx = static_cast<double>(blockX) * xzScale + computeChild(arg1, ctx);
    const double sy = static_cast<double>(blockY) * yScale + computeChild(arg2, ctx);
    const double sz = static_cast<double>(blockZ) * xzScale + computeChild(arg3, ctx);
    return noiseHolder.getValue(sx, sy, sz);
  }
  case DensityNodeKind::ShiftA:
    return noiseHolder.getValue(static_cast<double>(blockX) * 0.25, 0.0,
                                static_cast<double>(blockZ) * 0.25) *
           4.0;
  case DensityNodeKind::ShiftB:
    return noiseHolder.getValue(static_cast<double>(blockZ) * 0.25,
                                static_cast<double>(blockX) * 0.25, 0.0) *
           4.0;
  case DensityNodeKind::Shift:
    return noiseHolder.getValue(static_cast<double>(blockX) * 0.25,
                                static_cast<double>(blockY) * 0.25,
                                static_cast<double>(blockZ) * 0.25) *
           4.0;
  case DensityNodeKind::WeirdScaledSampler: {
    const double input = computeChild(arg1, ctx);
    const double rarity = mapRarity(rarityMapper, input);
    return rarity * std::abs(noiseHolder.getValue(
                        static_cast<double>(blockX) / rarity,
                        static_cast<double>(blockY) / rarity,
                        static_cast<double>(blockZ) / rarity));
  }
  case DensityNodeKind::OldBlendedNoise:
    return blendedNoise ? blendedNoise->compute(blockX, blockY, blockZ) : 0.0;
  case DensityNodeKind::Spline:
    return static_cast<double>(spline->apply(ctx));
  case DensityNodeKind::EndIslands: {
    if (!endIslandNoise)
      return 0.0;
    return (static_cast<double>(
                endIslandHeightValue(*endIslandNoise, blockX / 8, blockZ / 8)) -
            8.0) /
           128.0;
  }
  case DensityNodeKind::FindTopSurface: {
    const int step = intParam2;
    const int lower = intParam1;
    const int topY =
        worldgen_math::floorDivInt(computeChild(arg2, ctx) /
                                   static_cast<double>(step)) *
        step;
    if (topY <= lower)
      return static_cast<double>(lower);
    for (int y = topY; y >= lower; y -= step) {
      if (arg1->computeUncached(DensityFunctionContext{blockX, y, blockZ, nullptr}) > 0.0)
        return static_cast<double>(y);
    }
    return static_cast<double>(lower);
  }
  case DensityNodeKind::BlendAlpha:
    return 1.0;
  case DensityNodeKind::BlendOffset:
  case DensityNodeKind::Beardifier:
    return 0.0;
  case DensityNodeKind::BlendDensity:
  case DensityNodeKind::Marker:
    return computeChild(arg1, ctx);
  case DensityNodeKind::Custom:
    return custom ? custom->compute(ctx) : 0.0;
  }
  return 0.0;
}

void DensityNode::fillArray(double *output, std::size_t count,
                            DensityContextProvider &provider) const {
  switch (kind) {
  case DensityNodeKind::Constant:
    std::fill_n(output, count, constantValue);
    return;
  case DensityNodeKind::Mapped:
    arg1->fillArray(output, count, provider);
    for (std::size_t i = 0; i < count; ++i)
      output[i] = applyMappedTransform(mappedOp, output[i]);
    return;
  case DensityNodeKind::Clamp:
    arg1->fillArray(output, count, provider);
    for (std::size_t i = 0; i < count; ++i)
      output[i] = std::clamp(output[i], param1, param2);
    return;
  case DensityNodeKind::TwoArg: {
    arg1->fillArray(output, count, provider);
    switch (twoArgOp) {
    case TwoArgOp::Add: {
      constexpr std::size_t kStackSize = 256;
      double stackBuf[kStackSize];
      std::vector<double> heapBuf;
      double *v2 = stackBuf;
      if (count > kStackSize) {
        heapBuf.resize(count);
        v2 = heapBuf.data();
      }
      arg2->fillArray(v2, count, provider);
      for (std::size_t i = 0; i < count; ++i)
        output[i] += v2[i];
      return;
    }
    case TwoArgOp::Mul: {
      bool anyZero = false;
      for (std::size_t i = 0; i < count; ++i) {
        if (output[i] == 0.0) {
          anyZero = true;
          break;
        }
      }
      if (!anyZero) {
        constexpr std::size_t kStackSize = 256;
        double stackBuf[kStackSize];
        std::vector<double> heapBuf;
        double *v2 = stackBuf;
        if (count > kStackSize) {
          heapBuf.resize(count);
          v2 = heapBuf.data();
        }
        arg2->fillArray(v2, count, provider);
        for (std::size_t i = 0; i < count; ++i)
          output[i] *= v2[i];
      } else {
        for (std::size_t i = 0; i < count; ++i) {
          const double v = output[i];
          output[i] =
              v == 0.0 ? 0.0
                       : v * arg2->compute(provider.forIndex(static_cast<int>(i)));
        }
      }
      return;
    }
    case TwoArgOp::Min: {
      const double minVal = arg2->minValue;
      bool allBelow = true;
      for (std::size_t i = 0; i < count; ++i) {
        if (output[i] >= minVal) {
          allBelow = false;
          break;
        }
      }
      if (allBelow)
        return;
      for (std::size_t i = 0; i < count; ++i) {
        const double v = output[i];
        output[i] = v < minVal ? v
                               : std::min(v, arg2->compute(provider.forIndex(
                                                 static_cast<int>(i))));
      }
      return;
    }
    case TwoArgOp::Max: {
      const double maxVal = arg2->maxValue;
      bool allAbove = true;
      for (std::size_t i = 0; i < count; ++i) {
        if (output[i] <= maxVal) {
          allAbove = false;
          break;
        }
      }
      if (allAbove)
        return;
      for (std::size_t i = 0; i < count; ++i) {
        const double v = output[i];
        output[i] = v > maxVal ? v
                               : std::max(v, arg2->compute(provider.forIndex(
                                                 static_cast<int>(i))));
      }
      return;
    }
    }
    return;
  }
  case DensityNodeKind::RangeChoice: {
    arg1->fillArray(output, count, provider);
    bool allIn = true;
    bool allOut = true;
    for (std::size_t i = 0; i < count; ++i) {
      const bool inRange = (output[i] >= param1 && output[i] < param2);
      allIn &= inRange;
      allOut &= !inRange;
    }
    if (allIn) {
      arg2->fillArray(output, count, provider);
      return;
    }
    if (allOut) {
      arg3->fillArray(output, count, provider);
      return;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const double v = output[i];
      const DensityFunctionContext c = provider.forIndex(static_cast<int>(i));
      output[i] = (v >= param1 && v < param2) ? arg2->compute(c)
                                              : arg3->compute(c);
    }
    return;
  }
  case DensityNodeKind::BlendAlpha:
    std::fill_n(output, count, 1.0);
    return;
  case DensityNodeKind::BlendOffset:
  case DensityNodeKind::Beardifier:
    std::fill_n(output, count, 0.0);
    return;
  case DensityNodeKind::BlendDensity:
  case DensityNodeKind::Marker:
    arg1->fillArray(output, count, provider);
    return;
  case DensityNodeKind::WeirdScaledSampler: {
    arg1->fillArray(output, count, provider);
    for (std::size_t i = 0; i < count; ++i) {
      const DensityFunctionContext c = provider.forIndex(static_cast<int>(i));
      const double rarity = mapRarity(rarityMapper, output[i]);
      output[i] = rarity * std::abs(noiseHolder.getValue(
                               static_cast<double>(c.blockX) / rarity,
                               static_cast<double>(c.blockY) / rarity,
                               static_cast<double>(c.blockZ) / rarity));
    }
    return;
  }
  case DensityNodeKind::Custom:
    if (custom)
      custom->fillArray(output, count, provider);
    else
      std::fill_n(output, count, 0.0);
    return;
  case DensityNodeKind::YClampedGradient:
  case DensityNodeKind::Noise:
  case DensityNodeKind::ShiftedNoise:
  case DensityNodeKind::ShiftA:
  case DensityNodeKind::ShiftB:
  case DensityNodeKind::Shift:
  case DensityNodeKind::OldBlendedNoise:
  case DensityNodeKind::Spline:
  case DensityNodeKind::EndIslands:
  case DensityNodeKind::FindTopSurface:
    provider.fillAllDirectly(output, count, *this);
    return;
  }
}

namespace {

constexpr std::uint32_t kFlatInvalid = 0xFFFFFFFFu;

} // namespace

// 编译器需要访问 FlatDensityFunction 的私有指令表。
struct FlatDensityCompiler {
  FlatDensityFunction &fn;
  std::unordered_map<const DensityNode *, std::uint32_t> memo;
  std::unordered_map<const CubicSpline *, std::uint32_t> splineMemo;
  bool ok = true;

  template <class F>
  std::uint32_t push(const DensityNode *node, F &&fill) {
    const std::uint32_t index = static_cast<std::uint32_t>(fn.ops_.size());
    fn.ops_.emplace_back();
    fill(fn.ops_.back());
    memo.emplace(node, index);
    return index;
  }

  std::uint32_t visit(const DensityNodePtr &node) {
    if (!node)
      return kFlatInvalid;
    if (auto it = memo.find(node.get()); it != memo.end())
      return it->second;
    if (!ok)
      return kFlatInvalid;

    switch (node->kind) {
    case DensityNodeKind::Constant:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::Constant;
        op.p1 = node->constantValue;
      });
    case DensityNodeKind::Mapped: {
      const std::uint32_t a = visit(node->arg1);
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::Mapped;
        op.aux = static_cast<std::uint8_t>(node->mappedOp);
        op.a = a;
      });
    }
    case DensityNodeKind::Clamp: {
      const std::uint32_t a = visit(node->arg1);
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::Clamp;
        op.a = a;
        op.p1 = node->param1;
        op.p2 = node->param2;
      });
    }
    case DensityNodeKind::TwoArg: {
      const std::uint32_t a = visit(node->arg1);
      const std::uint32_t b = visit(node->arg2);
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::TwoArg;
        op.aux = static_cast<std::uint8_t>(node->twoArgOp);
        op.a = a;
        op.b = b;
        op.p1 = node->twoArgOp == TwoArgOp::Min
                    ? node->arg2->minValue
                    : (node->twoArgOp == TwoArgOp::Max ? node->arg2->maxValue
                                                       : 0.0);
      });
    }
    case DensityNodeKind::YClampedGradient:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::YClampedGradient;
        op.i1 = node->intParam1;
        op.i2 = node->intParam2;
        op.p1 = node->param1;
        op.p2 = node->param2;
      });
    case DensityNodeKind::RangeChoice: {
      const std::uint32_t a = visit(node->arg1);
      const std::uint32_t b = visit(node->arg2);
      const std::uint32_t c = visit(node->arg3);
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::RangeChoice;
        op.a = a;
        op.b = b;
        op.c = c;
        op.p1 = node->param1;
        op.p2 = node->param2;
      });
    }
    case DensityNodeKind::Noise:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::Noise;
        op.ptr = node->noiseHolder.noise.get();
        op.s1 = node->xzScale;
        op.s2 = node->yScale;
      });
    case DensityNodeKind::ShiftedNoise: {
      const std::uint32_t a = visit(node->arg1);
      const std::uint32_t b = visit(node->arg2);
      const std::uint32_t c = visit(node->arg3);
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::ShiftedNoise;
        op.a = a;
        op.b = b;
        op.c = c;
        op.ptr = node->noiseHolder.noise.get();
        op.s1 = node->xzScale;
        op.s2 = node->yScale;
      });
    }
    case DensityNodeKind::ShiftA:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::ShiftA;
        op.ptr = node->noiseHolder.noise.get();
      });
    case DensityNodeKind::ShiftB:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::ShiftB;
        op.ptr = node->noiseHolder.noise.get();
      });
    case DensityNodeKind::Shift:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::Shift;
        op.ptr = node->noiseHolder.noise.get();
      });
    case DensityNodeKind::WeirdScaledSampler: {
      const std::uint32_t a = visit(node->arg1);
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::WeirdScaled;
        op.a = a;
        op.aux = static_cast<std::uint8_t>(node->rarityMapper);
        op.ptr = node->noiseHolder.noise.get();
      });
    }
    case DensityNodeKind::OldBlendedNoise:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::OldBlended;
        op.ptr = node->blendedNoise.get();
      });
    case DensityNodeKind::Spline: {
      const std::uint32_t spline = compileSpline(node->spline.get());
      if (!ok)
        return kFlatInvalid;
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::Spline;
        op.a = spline;
      });
    }
    case DensityNodeKind::EndIslands:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::EndIslands;
        op.ptr = node->endIslandNoise.get();
      });
    case DensityNodeKind::BlendAlpha:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::BlendAlpha;
      });
    case DensityNodeKind::BlendOffset:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::BlendOffset;
      });
    case DensityNodeKind::Beardifier:
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::Beardifier;
      });
    case DensityNodeKind::BlendDensity:
    case DensityNodeKind::Marker: {
      const std::uint32_t a = visit(node->arg1);
      return push(node.get(), [&](FlatDensityFunction::Op &op) {
        op.kind = FlatDensityFunction::OpKind::PassThrough;
        op.a = a;
      });
    }
    case DensityNodeKind::FindTopSurface:
    case DensityNodeKind::Custom:
      ok = false;
      return kFlatInvalid;
    }
    ok = false;
    return kFlatInvalid;
  }

  std::uint32_t compileSpline(const CubicSpline *spline) {
    if (!spline) {
      ok = false;
      return kFlatInvalid;
    }
    if (auto it = splineMemo.find(spline); it != splineMemo.end())
      return it->second;
    const std::uint32_t index =
        static_cast<std::uint32_t>(fn.splines_.size());
    fn.splines_.emplace_back();
    splineMemo.emplace(spline, index);
    if (spline->isConstant()) {
      fn.splines_[index].isConstant = true;
      fn.splines_[index].constant = spline->constantValue();
      return index;
    }
    fn.splines_[index].isConstant = false;
    const std::uint32_t coordOp = visit(spline->coordinate());
    if (!ok)
      return index;
    fn.splines_[index].coordOp = coordOp;
    fn.splines_[index].locations = spline->locations();
    fn.splines_[index].derivatives = spline->derivatives();
    std::vector<std::uint32_t> values;
    values.reserve(spline->values().size());
    for (const auto &child : spline->values())
      values.push_back(compileSpline(child.get()));
    fn.splines_[index].values = std::move(values);
    return index;
  }
};

std::shared_ptr<const FlatDensityFunction>
FlatDensityFunction::compile(std::span<const DensityNodePtr> roots) {
  auto fn = std::make_shared<FlatDensityFunction>();
  FlatDensityCompiler compiler{*fn, {}, {}, true};
  fn->roots_.reserve(roots.size());
  for (const DensityNodePtr &root : roots) {
    if (!root) {
      fn->roots_.push_back(kFlatInvalid);
      continue;
    }
    const std::uint32_t index = compiler.visit(root);
    if (!compiler.ok)
      return nullptr;
    fn->roots_.push_back(index);
  }
  return fn;
}

float FlatDensityFunction::evaluateSpline(std::uint32_t index,
                                          const double *slots) const {
  const FlatSpline &spline = splines_[index];
  if (spline.isConstant)
    return spline.constant;
  const float input = static_cast<float>(slots[spline.coordOp]);
  const int start = findSplineInterval(spline.locations, input);
  const int lastIdx = static_cast<int>(spline.locations.size()) - 1;
  if (start < 0) {
    return linearExtendFloat(input, spline.locations,
                             evaluateSpline(spline.values[0], slots),
                             spline.derivatives, 0);
  }
  if (start == lastIdx) {
    const auto idx = static_cast<std::size_t>(lastIdx);
    return linearExtendFloat(input, spline.locations,
                             evaluateSpline(spline.values[idx], slots),
                             spline.derivatives, idx);
  }
  const auto s = static_cast<std::size_t>(start);
  const float x1 = spline.locations[s];
  const float x2 = spline.locations[s + 1];
  const float t = (input - x1) / (x2 - x1);
  const float d1 = spline.derivatives[s];
  const float d2 = spline.derivatives[s + 1];
  const float y1 = evaluateSpline(spline.values[s], slots);
  const float y2 = evaluateSpline(spline.values[s + 1], slots);
  const float a = d1 * (x2 - x1) - (y2 - y1);
  const float b = -d2 * (x2 - x1) + (y2 - y1);
  const float lerpY = y1 + t * (y2 - y1);
  const float lerpAB = a + t * (b - a);
  return lerpY + t * (1.0f - t) * lerpAB;
}

void FlatDensityFunction::evaluate(const DensityFunctionContext &ctx,
                                   double *out) const {
  thread_local std::vector<double> slots;
  if (slots.size() < ops_.size())
    slots.resize(ops_.size());
  const int blockX = ctx.blockX;
  const int blockY = ctx.blockY;
  const int blockZ = ctx.blockZ;
  for (std::size_t i = 0; i < ops_.size(); ++i) {
    const Op &op = ops_[i];
    switch (op.kind) {
    case OpKind::Constant:
      slots[i] = op.p1;
      break;
    case OpKind::Mapped:
      slots[i] = applyMappedTransform(static_cast<MappedOp>(op.aux),
                                      slots[op.a]);
      break;
    case OpKind::Clamp:
      slots[i] = std::clamp(slots[op.a], op.p1, op.p2);
      break;
    case OpKind::TwoArg: {
      const double v1 = slots[op.a];
      switch (static_cast<TwoArgOp>(op.aux)) {
      case TwoArgOp::Add:
        slots[i] = v1 + slots[op.b];
        break;
      case TwoArgOp::Mul:
        slots[i] = v1 == 0.0 ? 0.0 : v1 * slots[op.b];
        break;
      case TwoArgOp::Min:
        slots[i] = v1 < op.p1 ? v1 : std::min(v1, slots[op.b]);
        break;
      case TwoArgOp::Max:
        slots[i] = v1 > op.p1 ? v1 : std::max(v1, slots[op.b]);
        break;
      }
      break;
    }
    case OpKind::YClampedGradient: {
      const double factor =
          (static_cast<double>(blockY) - static_cast<double>(op.i1)) /
          (static_cast<double>(op.i2) - static_cast<double>(op.i1));
      slots[i] = worldgen_math::clampedLerp(factor, op.p1, op.p2);
      break;
    }
    case OpKind::RangeChoice: {
      const double v = slots[op.a];
      slots[i] = (v >= op.p1 && v < op.p2) ? slots[op.b] : slots[op.c];
      break;
    }
    case OpKind::Noise: {
      const auto *noise = static_cast<const NormalNoise *>(op.ptr);
      slots[i] = noise ? noise->getValue(static_cast<double>(blockX) * op.s1,
                                         static_cast<double>(blockY) * op.s2,
                                         static_cast<double>(blockZ) * op.s1)
                       : 0.0;
      break;
    }
    case OpKind::ShiftedNoise: {
      const auto *noise = static_cast<const NormalNoise *>(op.ptr);
      const double sx = static_cast<double>(blockX) * op.s1 + slots[op.a];
      const double sy = static_cast<double>(blockY) * op.s2 + slots[op.b];
      const double sz = static_cast<double>(blockZ) * op.s1 + slots[op.c];
      slots[i] = noise ? noise->getValue(sx, sy, sz) : 0.0;
      break;
    }
    case OpKind::ShiftA: {
      const auto *noise = static_cast<const NormalNoise *>(op.ptr);
      slots[i] =
          noise ? noise->getValue(static_cast<double>(blockX) * 0.25, 0.0,
                                  static_cast<double>(blockZ) * 0.25) *
                      4.0
                : 0.0;
      break;
    }
    case OpKind::ShiftB: {
      const auto *noise = static_cast<const NormalNoise *>(op.ptr);
      slots[i] =
          noise ? noise->getValue(static_cast<double>(blockZ) * 0.25,
                                  static_cast<double>(blockX) * 0.25, 0.0) *
                      4.0
                : 0.0;
      break;
    }
    case OpKind::Shift: {
      const auto *noise = static_cast<const NormalNoise *>(op.ptr);
      slots[i] =
          noise ? noise->getValue(static_cast<double>(blockX) * 0.25,
                                  static_cast<double>(blockY) * 0.25,
                                  static_cast<double>(blockZ) * 0.25) *
                      4.0
                : 0.0;
      break;
    }
    case OpKind::WeirdScaled: {
      const double input = slots[op.a];
      const double rarity =
          mapRarity(static_cast<RarityMapperKind>(op.aux), input);
      const auto *noise = static_cast<const NormalNoise *>(op.ptr);
      slots[i] =
          noise ? rarity * std::abs(noise->getValue(
                                      static_cast<double>(blockX) / rarity,
                                      static_cast<double>(blockY) / rarity,
                                      static_cast<double>(blockZ) / rarity))
                : 0.0;
      break;
    }
    case OpKind::OldBlended: {
      const auto *noise = static_cast<const BlendedNoise *>(op.ptr);
      slots[i] = noise ? noise->compute(blockX, blockY, blockZ) : 0.0;
      break;
    }
    case OpKind::Spline:
      slots[i] = static_cast<double>(evaluateSpline(op.a, slots.data()));
      break;
    case OpKind::EndIslands: {
      const auto *noise = static_cast<const SimplexNoise *>(op.ptr);
      slots[i] = noise ? (static_cast<double>(endIslandHeightValue(
                              *noise, blockX / 8, blockZ / 8)) -
                         8.0) /
                             128.0
                       : 0.0;
      break;
    }
    case OpKind::BlendAlpha:
      slots[i] = 1.0;
      break;
    case OpKind::BlendOffset:
    case OpKind::Beardifier:
      slots[i] = 0.0;
      break;
    case OpKind::PassThrough:
      slots[i] = slots[op.a];
      break;
    }
  }
  for (std::size_t r = 0; r < roots_.size(); ++r) {
    out[r] = roots_[r] == kFlatInvalid ? 0.0 : slots[roots_[r]];
  }
}

namespace density_functions {

DensityNodePtr constant(double value) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::Constant;
  n->constantValue = value;
  n->minValue = value;
  n->maxValue = value;
  return n;
}

DensityNodePtr mapped(MappedOp op, DensityNodePtr input) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::Mapped;
  n->mappedOp = op;
  n->arg1 = std::move(input);
  const double lo = n->arg1->minValue;
  const double hi = n->arg1->maxValue;
  const double minImg = applyMappedTransform(op, lo);
  const double maxImg = applyMappedTransform(op, hi);
  if (op == MappedOp::Invert) {
    if (lo < 0.0 && hi > 0.0) {
      n->minValue = -std::numeric_limits<double>::infinity();
      n->maxValue = std::numeric_limits<double>::infinity();
    } else {
      n->minValue = maxImg;
      n->maxValue = minImg;
    }
  } else if (op == MappedOp::Abs || op == MappedOp::Square) {
    n->minValue = std::max(0.0, lo);
    n->maxValue = std::max(minImg, maxImg);
  } else {
    n->minValue = minImg;
    n->maxValue = maxImg;
  }
  return n;
}

DensityNodePtr clamp(DensityNodePtr input, double minVal, double maxVal) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::Clamp;
  n->arg1 = std::move(input);
  n->param1 = minVal;
  n->param2 = maxVal;
  n->minValue = minVal;
  n->maxValue = maxVal;
  return n;
}

DensityNodePtr twoArg(TwoArgOp op, DensityNodePtr a, DensityNodePtr b) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::TwoArg;
  n->twoArgOp = op;
  const double min1 = a->minValue;
  const double min2 = b->minValue;
  const double max1 = a->maxValue;
  const double max2 = b->maxValue;
  switch (op) {
  case TwoArgOp::Add:
    n->minValue = min1 + min2;
    n->maxValue = max1 + max2;
    break;
  case TwoArgOp::Mul:
    n->minValue = (min1 > 0.0 && min2 > 0.0)
                      ? min1 * min2
                      : ((max1 < 0.0 && max2 < 0.0)
                             ? max1 * max2
                             : std::min(min1 * max2, max1 * min2));
    n->maxValue = (min1 > 0.0 && min2 > 0.0)
                      ? max1 * max2
                      : ((max1 < 0.0 && max2 < 0.0)
                             ? min1 * min2
                             : std::max(min1 * min2, max1 * max2));
    break;
  case TwoArgOp::Min:
    n->minValue = std::min(min1, min2);
    n->maxValue = std::min(max1, max2);
    break;
  case TwoArgOp::Max:
    n->minValue = std::max(min1, min2);
    n->maxValue = std::max(max1, max2);
    break;
  }
  n->arg1 = std::move(a);
  n->arg2 = std::move(b);
  return n;
}

DensityNodePtr yClampedGradient(int fromY, int toY, double fromVal,
                                double toVal) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::YClampedGradient;
  n->intParam1 = fromY;
  n->intParam2 = toY;
  n->param1 = fromVal;
  n->param2 = toVal;
  n->minValue = std::min(fromVal, toVal);
  n->maxValue = std::max(fromVal, toVal);
  return n;
}

DensityNodePtr rangeChoice(DensityNodePtr input, double minInclusive,
                           double maxExclusive, DensityNodePtr whenInRange,
                           DensityNodePtr whenOutOfRange) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::RangeChoice;
  n->param1 = minInclusive;
  n->param2 = maxExclusive;
  n->minValue = std::min(whenInRange->minValue, whenOutOfRange->minValue);
  n->maxValue = std::max(whenInRange->maxValue, whenOutOfRange->maxValue);
  n->arg1 = std::move(input);
  n->arg2 = std::move(whenInRange);
  n->arg3 = std::move(whenOutOfRange);
  return n;
}

DensityNodePtr noise(std::string noiseId, double xzScale, double yScale,
                     std::shared_ptr<const NormalNoise> resolved) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::Noise;
  n->noiseHolder = NoiseHolder{std::move(noiseId), std::move(resolved)};
  n->xzScale = xzScale;
  n->yScale = yScale;
  n->maxValue = n->noiseHolder.maxValue();
  n->minValue = -n->maxValue;
  return n;
}

DensityNodePtr shiftedNoise(DensityNodePtr shiftX, DensityNodePtr shiftY,
                            DensityNodePtr shiftZ, double xzScale,
                            double yScale, std::string noiseId,
                            std::shared_ptr<const NormalNoise> resolved) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::ShiftedNoise;
  n->arg1 = std::move(shiftX);
  n->arg2 = std::move(shiftY);
  n->arg3 = std::move(shiftZ);
  n->xzScale = xzScale;
  n->yScale = yScale;
  n->noiseHolder = NoiseHolder{std::move(noiseId), std::move(resolved)};
  n->maxValue = n->noiseHolder.maxValue();
  n->minValue = -n->maxValue;
  return n;
}

DensityNodePtr shiftA(std::string noiseId,
                      std::shared_ptr<const NormalNoise> resolved) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::ShiftA;
  n->noiseHolder = NoiseHolder{std::move(noiseId), std::move(resolved)};
  n->maxValue = n->noiseHolder.maxValue() * 4.0;
  n->minValue = -n->maxValue;
  return n;
}

DensityNodePtr shiftB(std::string noiseId,
                      std::shared_ptr<const NormalNoise> resolved) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::ShiftB;
  n->noiseHolder = NoiseHolder{std::move(noiseId), std::move(resolved)};
  n->maxValue = n->noiseHolder.maxValue() * 4.0;
  n->minValue = -n->maxValue;
  return n;
}

DensityNodePtr shift(std::string noiseId,
                     std::shared_ptr<const NormalNoise> resolved) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::Shift;
  n->noiseHolder = NoiseHolder{std::move(noiseId), std::move(resolved)};
  n->maxValue = n->noiseHolder.maxValue() * 4.0;
  n->minValue = -n->maxValue;
  return n;
}

DensityNodePtr
weirdScaledSampler(DensityNodePtr input, std::string noiseId,
                   RarityMapperKind mapper,
                   std::shared_ptr<const NormalNoise> resolved) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::WeirdScaledSampler;
  n->arg1 = std::move(input);
  n->rarityMapper = mapper;
  n->noiseHolder = NoiseHolder{std::move(noiseId), std::move(resolved)};
  n->minValue = 0.0;
  n->maxValue = maxRarityOf(mapper) * n->noiseHolder.maxValue();
  return n;
}

DensityNodePtr oldBlendedNoise(double xzScale, double yScale, double xzFactor,
                               double yFactor, double smearScaleMultiplier,
                               std::shared_ptr<const BlendedNoise> resolved) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::OldBlendedNoise;
  n->xzScale = xzScale;
  n->yScale = yScale;
  n->xzFactor = xzFactor;
  n->yFactor = yFactor;
  n->smearScaleMultiplier = smearScaleMultiplier;
  if (!resolved) {
    resolved = std::make_shared<BlendedNoise>(BlendedNoise::createUnseeded(
        xzScale, yScale, xzFactor, yFactor, smearScaleMultiplier));
  }
  n->blendedNoise = std::move(resolved);
  n->maxValue = n->blendedNoise->maxValue();
  n->minValue = -n->maxValue;
  return n;
}

DensityNodePtr spline(CubicSplinePtr sp) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::Spline;
  n->minValue = static_cast<double>(sp->minValue());
  n->maxValue = static_cast<double>(sp->maxValue());
  n->spline = std::move(sp);
  return n;
}

DensityNodePtr endIslands(std::int64_t seed) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::EndIslands;
  n->endSeed = seed;
  n->endIslandNoise = buildEndIslandNoise(seed);
  n->minValue = -0.84375;
  n->maxValue = 0.5625;
  return n;
}

DensityNodePtr findTopSurface(DensityNodePtr density, DensityNodePtr upperBound,
                              int lowerBound, int cellHeight) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::FindTopSurface;
  n->intParam1 = lowerBound;
  n->intParam2 = cellHeight;
  n->minValue = static_cast<double>(lowerBound);
  n->maxValue = std::max(static_cast<double>(lowerBound), upperBound->maxValue);
  n->arg1 = std::move(density);
  n->arg2 = std::move(upperBound);
  return n;
}

DensityNodePtr blendAlpha() {
  static const auto kNode = []() {
    auto n = std::make_shared<DensityNode>();
    n->kind = DensityNodeKind::BlendAlpha;
    n->minValue = 1.0;
    n->maxValue = 1.0;
    return n;
  }();
  return kNode;
}

DensityNodePtr blendOffset() {
  static const auto kNode = []() {
    auto n = std::make_shared<DensityNode>();
    n->kind = DensityNodeKind::BlendOffset;
    n->minValue = 0.0;
    n->maxValue = 0.0;
    return n;
  }();
  return kNode;
}

DensityNodePtr blendDensity(DensityNodePtr input) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::BlendDensity;
  n->arg1 = std::move(input);
  n->minValue = -std::numeric_limits<double>::infinity();
  n->maxValue = std::numeric_limits<double>::infinity();
  return n;
}

DensityNodePtr beardifier() {
  static const auto kNode = []() {
    auto n = std::make_shared<DensityNode>();
    n->kind = DensityNodeKind::Beardifier;
    n->minValue = 0.0;
    n->maxValue = 0.0;
    return n;
  }();
  return kNode;
}

DensityNodePtr marker(MarkerKind kind, DensityNodePtr wrapped) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::Marker;
  n->markerKind = kind;
  n->minValue = wrapped->minValue;
  n->maxValue = wrapped->maxValue;
  n->arg1 = std::move(wrapped);
  return n;
}

DensityNodePtr custom(std::shared_ptr<CustomDensityFunction> fn,
                      double minVal, double maxVal) {
  auto n = std::make_shared<DensityNode>();
  n->kind = DensityNodeKind::Custom;
  n->custom = std::move(fn);
  n->minValue = minVal;
  n->maxValue = maxVal;
  return n;
}

namespace {

struct DensityPtrHash {
  std::uint64_t operator()(const DensityNode *p) const noexcept {
    std::uint64_t x =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
  }
};

using DensityMemo = FlatHashMap<const DensityNode *, DensityNodePtr, DensityPtrHash>;

DensityNodePtr mapAllImpl(
    const DensityNodePtr &root, const NodeTransform &nodeVisitor,
    const std::function<NoiseHolder(const NoiseHolder &)> &noiseVisitor,
    DensityMemo &memo) {
  if (!root)
    return nullptr;
  if (auto it = memo.find(root.get()); it != memo.end())
    return it->second;

  auto recurse = [&](const DensityNodePtr &child) {
    return mapAllImpl(child, nodeVisitor, noiseVisitor, memo);
  };
  auto visitN = [&](const NoiseHolder &nh) {
    return noiseVisitor ? noiseVisitor(nh) : nh;
  };
  auto sameNoise = [](const NoiseHolder &a, const NoiseHolder &b) {
    return a.noise.get() == b.noise.get() && a.noiseId == b.noiseId;
  };

  DensityNodePtr rebuilt;
  switch (root->kind) {
  case DensityNodeKind::Constant:
  case DensityNodeKind::YClampedGradient:
  case DensityNodeKind::OldBlendedNoise:
  case DensityNodeKind::EndIslands:
  case DensityNodeKind::BlendAlpha:
  case DensityNodeKind::BlendOffset:
  case DensityNodeKind::Beardifier:
  case DensityNodeKind::Custom:
    rebuilt = root;
    break;
  case DensityNodeKind::Mapped: {
    DensityNodePtr c1 = recurse(root->arg1);
    rebuilt = (c1.get() == root->arg1.get()) ? root
                                             : mapped(root->mappedOp, std::move(c1));
    break;
  }
  case DensityNodeKind::Clamp: {
    DensityNodePtr c1 = recurse(root->arg1);
    rebuilt = (c1.get() == root->arg1.get())
                  ? root
                  : clamp(std::move(c1), root->param1, root->param2);
    break;
  }
  case DensityNodeKind::TwoArg: {
    DensityNodePtr c1 = recurse(root->arg1);
    DensityNodePtr c2 = recurse(root->arg2);
    rebuilt = (c1.get() == root->arg1.get() && c2.get() == root->arg2.get())
                  ? root
                  : twoArg(root->twoArgOp, std::move(c1), std::move(c2));
    break;
  }
  case DensityNodeKind::RangeChoice: {
    DensityNodePtr c1 = recurse(root->arg1);
    DensityNodePtr c2 = recurse(root->arg2);
    DensityNodePtr c3 = recurse(root->arg3);
    rebuilt = (c1.get() == root->arg1.get() && c2.get() == root->arg2.get() &&
               c3.get() == root->arg3.get())
                  ? root
                  : rangeChoice(std::move(c1), root->param1, root->param2,
                                std::move(c2), std::move(c3));
    break;
  }
  case DensityNodeKind::Noise: {
    const NoiseHolder nh = visitN(root->noiseHolder);
    rebuilt = sameNoise(nh, root->noiseHolder)
                  ? root
                  : noise(nh.noiseId, root->xzScale, root->yScale, nh.noise);
    break;
  }
  case DensityNodeKind::ShiftedNoise: {
    const NoiseHolder nh = visitN(root->noiseHolder);
    DensityNodePtr c1 = recurse(root->arg1);
    DensityNodePtr c2 = recurse(root->arg2);
    DensityNodePtr c3 = recurse(root->arg3);
    rebuilt = (sameNoise(nh, root->noiseHolder) &&
               c1.get() == root->arg1.get() && c2.get() == root->arg2.get() &&
               c3.get() == root->arg3.get())
                  ? root
                  : shiftedNoise(std::move(c1), std::move(c2), std::move(c3),
                                 root->xzScale, root->yScale, nh.noiseId,
                                 nh.noise);
    break;
  }
  case DensityNodeKind::ShiftA: {
    const NoiseHolder nh = visitN(root->noiseHolder);
    rebuilt = sameNoise(nh, root->noiseHolder) ? root : shiftA(nh.noiseId, nh.noise);
    break;
  }
  case DensityNodeKind::ShiftB: {
    const NoiseHolder nh = visitN(root->noiseHolder);
    rebuilt = sameNoise(nh, root->noiseHolder) ? root : shiftB(nh.noiseId, nh.noise);
    break;
  }
  case DensityNodeKind::Shift: {
    const NoiseHolder nh = visitN(root->noiseHolder);
    rebuilt = sameNoise(nh, root->noiseHolder) ? root : shift(nh.noiseId, nh.noise);
    break;
  }
  case DensityNodeKind::WeirdScaledSampler: {
    const NoiseHolder nh = visitN(root->noiseHolder);
    DensityNodePtr c1 = recurse(root->arg1);
    rebuilt =
        (sameNoise(nh, root->noiseHolder) && c1.get() == root->arg1.get())
            ? root
            : weirdScaledSampler(std::move(c1), nh.noiseId, root->rarityMapper,
                                 nh.noise);
    break;
  }
  case DensityNodeKind::Spline: {
    CubicSplinePtr sp = root->spline->mapAll(recurse);
    rebuilt = (sp.get() == root->spline.get()) ? root : spline(std::move(sp));
    break;
  }
  case DensityNodeKind::FindTopSurface: {
    DensityNodePtr c1 = recurse(root->arg1);
    DensityNodePtr c2 = recurse(root->arg2);
    rebuilt = (c1.get() == root->arg1.get() && c2.get() == root->arg2.get())
                  ? root
                  : findTopSurface(std::move(c1), std::move(c2),
                                   root->intParam1, root->intParam2);
    break;
  }
  case DensityNodeKind::BlendDensity: {
    DensityNodePtr c1 = recurse(root->arg1);
    rebuilt = (c1.get() == root->arg1.get()) ? root : blendDensity(std::move(c1));
    break;
  }
  case DensityNodeKind::Marker: {
    DensityNodePtr c1 = recurse(root->arg1);
    rebuilt = (c1.get() == root->arg1.get())
                  ? root
                  : marker(root->markerKind, std::move(c1));
    break;
  }
  }

  DensityNodePtr visited = nodeVisitor ? nodeVisitor(rebuilt) : rebuilt;
  memo[root.get()] = visited;
  return visited;
}

bool splineDependsOnY(const CubicSpline *sp,
                      FlatHashMap<const DensityNode *, bool, DensityPtrHash> &memo);

bool dependsOnYImpl(const DensityNode *node,
                    FlatHashMap<const DensityNode *, bool, DensityPtrHash> &memo) {
  if (!node)
    return false;
  if (auto it = memo.find(node); it != memo.end())
    return it->second;
  bool res = false;
  switch (node->kind) {
  case DensityNodeKind::Constant:
  case DensityNodeKind::ShiftA:
  case DensityNodeKind::ShiftB:
  case DensityNodeKind::Shift:
  case DensityNodeKind::BlendAlpha:
  case DensityNodeKind::BlendOffset:
  case DensityNodeKind::Beardifier:
    res = false;
    break;
  case DensityNodeKind::YClampedGradient:
  case DensityNodeKind::OldBlendedNoise:
  case DensityNodeKind::EndIslands:
  case DensityNodeKind::FindTopSurface:
  case DensityNodeKind::WeirdScaledSampler:
    res = true;
    break;
  case DensityNodeKind::Noise:
    res = (node->yScale != 0.0);
    break;
  case DensityNodeKind::ShiftedNoise:
    res = (node->yScale != 0.0) || dependsOnYImpl(node->arg1.get(), memo) ||
          dependsOnYImpl(node->arg2.get(), memo) ||
          dependsOnYImpl(node->arg3.get(), memo);
    break;
  case DensityNodeKind::Mapped:
  case DensityNodeKind::Clamp:
  case DensityNodeKind::BlendDensity:
    res = dependsOnYImpl(node->arg1.get(), memo);
    break;
  case DensityNodeKind::Marker:
    res = (node->markerKind != MarkerKind::FlatCache) &&
          dependsOnYImpl(node->arg1.get(), memo);
    break;
  case DensityNodeKind::TwoArg:
    res = dependsOnYImpl(node->arg1.get(), memo) ||
          dependsOnYImpl(node->arg2.get(), memo);
    break;
  case DensityNodeKind::RangeChoice:
    res = dependsOnYImpl(node->arg1.get(), memo) ||
          dependsOnYImpl(node->arg2.get(), memo) ||
          dependsOnYImpl(node->arg3.get(), memo);
    break;
  case DensityNodeKind::Spline:
    res = node->spline && splineDependsOnY(node->spline.get(), memo);
    break;
  case DensityNodeKind::Custom:
    res = node->custom && !node->custom->isYIndependent();
    break;
  }
  memo[node] = res;
  return res;
}

bool splineDependsOnY(const CubicSpline *sp,
                      FlatHashMap<const DensityNode *, bool, DensityPtrHash> &memo) {
  if (!sp || sp->isConstant())
    return false;
  if (dependsOnYImpl(sp->coordinate().get(), memo))
    return true;
  for (const auto &v : sp->values()) {
    if (splineDependsOnY(v.get(), memo))
      return true;
  }
  return false;
}

} // namespace

DensityNodePtr mapAll(
    const DensityNodePtr &root, const NodeTransform &nodeVisitor,
    const std::function<NoiseHolder(const NoiseHolder &)> &noiseVisitor) {
  DensityMemo memo;
  return mapAllImpl(root, nodeVisitor, noiseVisitor, memo);
}

bool dependsOnY(const DensityNode *node) noexcept {
  if (!node)
    return false;
  FlatHashMap<const DensityNode *, bool, DensityPtrHash> memo;
  return dependsOnYImpl(node, memo);
}

} // namespace density_functions

} // namespace redstone::worldgen
