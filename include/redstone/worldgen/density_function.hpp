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

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

struct DensityNode;

// 单次采样内的共享子图求值 memo。Java 的 Climate.Sampler 会剥离所有 Marker，
// 因此同一次 sample 中 continents/erosion/ridges 等共享子树会被重复求值；
// 这里按节点指针记忆（值完全相同，只消除重复计算）。
struct DensityEvalCache {
  static constexpr std::size_t kCapacity = 32;
  struct Slot {
    const DensityNode *node = nullptr;
    double value = 0.0;
  };
  std::array<Slot, kCapacity> slots{};

  const double *find(const DensityNode *node) const noexcept {
    std::size_t index =
        (reinterpret_cast<std::uintptr_t>(node) >> 4) & (kCapacity - 1);
    for (std::size_t probe = 0; probe < kCapacity; ++probe) {
      const Slot &slot = slots[index];
      if (slot.node == node)
        return &slot.value;
      if (slot.node == nullptr)
        return nullptr;
      index = (index + 1) & (kCapacity - 1);
    }
    return nullptr;
  }

  void insert(const DensityNode *node, double value) noexcept {
    std::size_t index =
        (reinterpret_cast<std::uintptr_t>(node) >> 4) & (kCapacity - 1);
    for (std::size_t probe = 0; probe < kCapacity; ++probe) {
      Slot &slot = slots[index];
      if (slot.node == node) {
        slot.value = value;
        return;
      }
      if (slot.node == nullptr) {
        slot.node = node;
        slot.value = value;
        return;
      }
      index = (index + 1) & (kCapacity - 1);
    }
  }
};

struct DensityFunctionContext {
  int blockX = 0;
  int blockY = 0;
  int blockZ = 0;
  const void *activeToken = nullptr;
  DensityEvalCache *evalCache = nullptr;
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
  double computeUncached(const DensityFunctionContext &ctx) const;
  void fillArray(double *output, std::size_t count,
                 DensityContextProvider &provider) const;
};

// 扁平密度函数求值器：把共享 DAG 按后序编译为连续指令表（操作数用索引而非 shared_ptr），
// 求值时顺序写 slots 数组，消除递归、指针跳转与每节点缓存查找。参考 cubiomes 的
// SplineStack 做法（预分配连续节点），但这里直接面向 26.1 DensityFunction 图。
// 仅编译纯函数子图；遇到 Custom/FindTopSurface 这类需要节点身份或额外上下文的节点时返回 nullptr。
class FlatDensityFunction {
public:
  static std::shared_ptr<const FlatDensityFunction>
  compile(std::span<const DensityNodePtr> roots);

  std::size_t opCount() const noexcept { return ops_.size(); }

  // 按 compile 传入的 roots 顺序写出结果；null root 写 0。
  void evaluate(const DensityFunctionContext &ctx, double *out) const;

private:
  friend struct FlatDensityCompiler;

  enum class OpKind : std::uint8_t {
    Constant,
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
    WeirdScaled,
    OldBlended,
    Spline,
    EndIslands,
    BlendAlpha,
    BlendOffset,
    Beardifier,
    PassThrough,
  };

  struct Op {
    OpKind kind = OpKind::Constant;
    std::uint8_t aux = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    double p1 = 0.0;
    double p2 = 0.0;
    int i1 = 0;
    int i2 = 0;
    const void *ptr = nullptr;
    double s1 = 1.0;
    double s2 = 1.0;
  };

  struct FlatSpline {
    bool isConstant = true;
    float constant = 0.0f;
    std::uint32_t coordOp = 0;
    std::vector<float> locations;
    std::vector<float> derivatives;
    std::vector<std::uint32_t> values;
  };

  float evaluateSpline(std::uint32_t index, const double *slots) const;

  std::vector<Op> ops_;
  std::vector<FlatSpline> splines_;
  std::vector<std::uint32_t> roots_;
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
