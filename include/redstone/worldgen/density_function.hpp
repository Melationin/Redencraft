#pragma once
// 对齐 Minecraft Java Edition 26.1 的 `DensityFunction` AST 节点与扁平计算引擎：
// 支持 26.1 数据包中的全部密度算子（常量、单目映射、`Clamp`、`Add`/`Mul`/`Min`/`Max`、
// `YClampedGradient`、`RangeChoice`、`Noise`/`ShiftedNoise`/`ShiftA/B`/`Shift`、
// `WeirdScaledSampler`、`OldBlendedNoise`、`Spline`、`EndIslands`、`FindTopSurface`、
// `Blend*`/`Beardifier` 与 `Marker` 缓存/插值包装器）。
// 采用不可变共享节点 `DensityNodePtr` 保留 DAG 共享结构（避免深层噪声图反复拷贝展开），
// 供 `NoiseRouter` / `RandomState` 快速完成噪声注入及 `G-2/G-3` 噪声区块插值替换。

#include "redstone/worldgen/cubic_spline.hpp"
#include "redstone/worldgen/noise.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace redstone::worldgen {

enum class DensityNodeKind : std::uint8_t {
  Constant = 0,
  Mapped,
  Clamp,
  TwoArg,
  YClampedGradient,
  RangeChoice,
  Noise,
  ShiftedNoise,
  ShiftA,
  ShiftB,
  Shift,
  WeirdScaledSampler,
  OldBlendedNoise,
  Spline,
  EndIslands,
  FindTopSurface,
  BlendAlpha,
  BlendOffset,
  BlendDensity,
  Beardifier,
  Marker,
  Custom,
};

struct DensityFunctionContext {
  int blockX = 0;
  int blockY = 0;
  int blockZ = 0;
  const void *activeToken = nullptr;
};

struct DensityContextProvider {
  virtual ~DensityContextProvider() = default;
  virtual DensityFunctionContext forIndex(int index) = 0;
  virtual void fillAllDirectly(double *output, std::size_t count,
                               const DensityNode &node) = 0;
  virtual const void *noiseChunkToken() const noexcept { return nullptr; }
};

struct CustomDensityFunction {
  virtual ~CustomDensityFunction() = default;
  virtual double compute(const DensityFunctionContext &ctx) = 0;
  virtual void fillArray(double *output, std::size_t count,
                         DensityContextProvider &provider) = 0;
  virtual DensityNodePtr wrapped() const { return nullptr; }
  virtual bool isYIndependent() const noexcept { return false; }
};

enum class MappedOp : std::uint8_t {
  Abs = 0,
  Square = 1,
  Cube = 2,
  HalfNegative = 3,
  QuarterNegative = 4,
  Invert = 5,
  Squeeze = 6,
};

enum class TwoArgOp : std::uint8_t {
  Add = 0,
  Mul = 1,
  Min = 2,
  Max = 3,
};

enum class RarityMapperKind : std::uint8_t {
  Type1 = 0, // 对应 3D 意大利面洞穴 `getSpaghettiRarity3D`（最大倍率 2.0）
  Type2 = 1, // 对应 2D 意大利面洞穴 `getSpaghettiRarity2D`（最大倍率 3.0）
};

enum class MarkerKind : std::uint8_t {
  Interpolated = 0,
  FlatCache = 1,
  Cache2D = 2,
  CacheOnce = 3,
  CacheAllInCell = 4,
};

// 绑定到噪声参数键与（经 `RandomState` 注入后的）共享 `NormalNoise` 实例。
struct NoiseHolder {
  std::string noiseId;
  std::shared_ptr<const NormalNoise> noise;

  double getValue(double x, double y, double z) const noexcept {
    return noise ? noise->getValue(x, y, z) : 0.0;
  }
  double maxValue() const noexcept {
    return noise ? noise->maxValue() : 2.0;
  }
};

struct DensityNode {
  DensityNodeKind kind = DensityNodeKind::Constant;
  double minValue = 0.0;
  double maxValue = 0.0;

  // Constant
  double constantValue = 0.0;

  // Mapped / Clamp / TwoArg / RangeChoice / WeirdScaledSampler / BlendDensity / Marker
  DensityNodePtr arg1;
  DensityNodePtr arg2;
  DensityNodePtr arg3;
  MappedOp mappedOp = MappedOp::Abs;
  TwoArgOp twoArgOp = TwoArgOp::Add;
  double param1 = 0.0; // Clamp min / RangeChoice minInclusive / OldBlended xzScale / MulOrAdd value
  double param2 = 0.0; // Clamp max / RangeChoice maxExclusive / OldBlended yScale

  // YClampedGradient / FindTopSurface
  int intParam1 = 0; // fromY / lowerBound
  int intParam2 = 0; // toY / cellHeight

  // Noise / ShiftedNoise / ShiftA / ShiftB / Shift / WeirdScaledSampler
  NoiseHolder noiseHolder;
  double xzScale = 1.0;
  double yScale = 1.0;
  RarityMapperKind rarityMapper = RarityMapperKind::Type1;

  // OldBlendedNoise
  double xzFactor = 80.0;
  double yFactor = 160.0;
  double smearScaleMultiplier = 8.0;
  std::shared_ptr<const BlendedNoise> blendedNoise;

  // Spline
  CubicSplinePtr spline;

  // EndIslands
  std::int64_t endSeed = 0;
  std::shared_ptr<const SimplexNoise> endIslandNoise;

  // Marker
  MarkerKind markerKind = MarkerKind::CacheOnce;

  // Custom (`NoiseChunk` stateful cache / interpolator / blender nodes)
  std::shared_ptr<CustomDensityFunction> custom;

  double compute(int blockX, int blockY, int blockZ) const;
  double compute(const DensityFunctionContext &ctx) const;
  void fillArray(double *output, std::size_t count,
                 DensityContextProvider &provider) const;
};

namespace density_functions {

DensityNodePtr constant(double value);
DensityNodePtr mapped(MappedOp op, DensityNodePtr input);
DensityNodePtr clamp(DensityNodePtr input, double minVal, double maxVal);
DensityNodePtr twoArg(TwoArgOp op, DensityNodePtr a, DensityNodePtr b);
DensityNodePtr yClampedGradient(int fromY, int toY, double fromVal,
                                double toVal);
DensityNodePtr rangeChoice(DensityNodePtr input, double minInclusive,
                           double maxExclusive, DensityNodePtr whenInRange,
                           DensityNodePtr whenOutOfRange);
DensityNodePtr noise(std::string noiseId, double xzScale, double yScale,
                     std::shared_ptr<const NormalNoise> resolved = {});
DensityNodePtr shiftedNoise(DensityNodePtr shiftX, DensityNodePtr shiftY,
                            DensityNodePtr shiftZ, double xzScale,
                            double yScale, std::string noiseId,
                            std::shared_ptr<const NormalNoise> resolved = {});
DensityNodePtr shiftA(std::string noiseId,
                      std::shared_ptr<const NormalNoise> resolved = {});
DensityNodePtr shiftB(std::string noiseId,
                      std::shared_ptr<const NormalNoise> resolved = {});
DensityNodePtr shift(std::string noiseId,
                     std::shared_ptr<const NormalNoise> resolved = {});
DensityNodePtr
weirdScaledSampler(DensityNodePtr input, std::string noiseId,
                   RarityMapperKind mapper,
                   std::shared_ptr<const NormalNoise> resolved = {});
DensityNodePtr oldBlendedNoise(double xzScale, double yScale, double xzFactor,
                               double yFactor, double smearScaleMultiplier,
                               std::shared_ptr<const BlendedNoise> resolved = {});
DensityNodePtr spline(CubicSplinePtr sp);
DensityNodePtr endIslands(std::int64_t seed = 0);
DensityNodePtr findTopSurface(DensityNodePtr density, DensityNodePtr upperBound,
                              int lowerBound, int cellHeight);
DensityNodePtr blendAlpha();
DensityNodePtr blendOffset();
DensityNodePtr blendDensity(DensityNodePtr input);
DensityNodePtr beardifier();
DensityNodePtr marker(MarkerKind kind, DensityNodePtr wrapped);
DensityNodePtr custom(std::shared_ptr<CustomDensityFunction> fn,
                      double minVal, double maxVal);

// 保持子图共享语义的深度访问重写器（用于 `RandomState` 噪声实例绑定与平坦化）。
using NodeTransform =
    std::function<DensityNodePtr(const DensityNodePtr &)>;
DensityNodePtr mapAll(const DensityNodePtr &root,
                      const NodeTransform &nodeVisitor,
                      const std::function<NoiseHolder(const NoiseHolder &)>
                          &noiseVisitor = {});

// 判断密度子图是否可能随 Y 变化；可用于 `ClimateSampler` 剔除 Y 无关通道的逐层重算。
bool dependsOnY(const DensityNode *node) noexcept;

} // namespace density_functions

} // namespace redstone::worldgen
