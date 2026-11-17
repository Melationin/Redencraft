#pragma once
// 对齐 Minecraft Java Edition 26.1 的噪声原语层：
// 包含单八度三维 Perlin 噪声（`ImprovedNoise`）、2D/3D 单形噪声（`SimplexNoise`）、
// 多八度分形叠加噪声（`PerlinNoise`）、双 Perlin 差值噪声（`NormalNoise`）与
// 高低频混合 3D 地形噪声（`BlendedNoise`）。
// 全部系数（`wrap(33554432)`、`INPUT_FACTOR = 1.0181268882175227`、`684.412` 缩放、
// 以及八度倒排下标）均严格逐字对齐原版 `net.minecraft.world.level.levelgen.synth.*`。

#include "redstone/worldgen/random_source.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace redstone::worldgen {

namespace worldgen_math {

inline int floorDivInt(double v) noexcept {
  const int i = static_cast<int>(v);
  return v < static_cast<double>(i) ? i - 1 : i;
}

inline std::int64_t lfloor(double v) noexcept {
  return static_cast<std::int64_t>(std::floor(v));
}

inline constexpr double smoothstep(double x) noexcept {
  return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

inline constexpr double smoothstepDerivative(double x) noexcept {
  return 30.0 * x * x * (x - 1.0) * (x - 1.0);
}

inline constexpr double lerp(double alpha, double p0, double p1) noexcept {
  return p0 + alpha * (p1 - p0);
}

inline constexpr double lerp2(double a1, double a2, double x00, double x10,
                              double x01, double x11) noexcept {
  return lerp(a2, lerp(a1, x00, x10), lerp(a1, x01, x11));
}

inline constexpr double lerp3(double a1, double a2, double a3, double x000,
                              double x100, double x010, double x110, double x001,
                              double x101, double x011, double x111) noexcept {
  return lerp(a3, lerp2(a1, a2, x000, x100, x010, x110),
              lerp2(a1, a2, x001, x101, x011, x111));
}

inline constexpr double clampedLerp(double factor, double minVal,
                                    double maxVal) noexcept {
  if (factor < 0.0)
    return minVal;
  return factor > 1.0 ? maxVal : lerp(factor, minVal, maxVal);
}

} // namespace worldgen_math

// 2D/3D 单形噪声（对齐 `SimplexNoise.java`；同时为 `ImprovedNoise` 提供 16 向梯度向量表）。
class SimplexNoise {
public:
  static constexpr std::array<std::array<int, 3>, 16> kGradient{{
      {1, 1, 0},
      {-1, 1, 0},
      {1, -1, 0},
      {-1, -1, 0},
      {1, 0, 1},
      {-1, 0, 1},
      {1, 0, -1},
      {-1, 0, -1},
      {0, 1, 1},
      {0, -1, 1},
      {0, 1, -1},
      {0, -1, -1},
      {1, 1, 0},
      {0, -1, 1},
      {-1, 1, 0},
      {0, -1, -1},
  }};

  explicit SimplexNoise(RandomSource &random);

  double getValue(double xin, double yin) const noexcept;
  double getValue(double xin, double yin, double zin) const noexcept;

  static constexpr double dot(const std::array<int, 3> &g, double x, double y,
                            double z) noexcept {
    return g[0] * x + g[1] * y + g[2] * z;
  }

  double xo() const noexcept { return xo_; }
  double yo() const noexcept { return yo_; }
  double zo() const noexcept { return zo_; }

private:
  int perm(int x) const noexcept { return p_[x & 0xFF]; }
  double cornerNoise3D(int index, double x, double y, double z,
                       double base) const noexcept;

  std::array<int, 512> p_{};
  double xo_ = 0.0;
  double yo_ = 0.0;
  double zo_ = 0.0;
};

// 多八度 2D 单形噪声（对齐 `PerlinSimplexNoise.java`，用于群系温度冻结扰动与积雪高度调制）。
class PerlinSimplexNoise {
public:
  PerlinSimplexNoise(RandomSource &random, std::span<const int> octaves);

  double getValue(double x, double y, bool useNoiseStart) const noexcept;

private:
  std::vector<std::optional<SimplexNoise>> noiseLevels_;
  double highestFreqValueFactor_ = 0.0;
  double highestFreqInputFactor_ = 0.0;
};

// 单八度经典 Perlin 梯度噪声（对齐 `ImprovedNoise.java`）。
// 支持 `yScale` / `yFudge` 垂直阶梯截断采样（为老式 3D 地形混合与云团偏移所必需）与解析梯度导数。
class ImprovedNoise {
public:
  explicit ImprovedNoise(RandomSource &random);

  double noise(double x, double y, double z) const noexcept {
    return noise(x, y, z, 0.0, 0.0);
  }
  double noise(double x, double y, double z, double yScale,
               double yFudge) const noexcept;
  double noiseWithDerivative(double x, double y, double z,
                             std::array<double, 3> &derivativeOut) const noexcept;

  double xo() const noexcept { return xo_; }
  double yo() const noexcept { return yo_; }
  double zo() const noexcept { return zo_; }
  const std::array<std::uint8_t, 256> &permTable() const noexcept { return p_; }

private:
  int perm(int x) const noexcept {
    return static_cast<int>(p_[static_cast<std::size_t>(x & 0xFF)]);
  }
  double sampleAndLerp(int x, int y, int z, double xr, double yr, double zr,
                       double yrOriginal) const noexcept;
  double sampleWithDerivative(int x, int y, int z, double xr, double yr,
                              double zr,
                              std::array<double, 3> &derivativeOut) const noexcept;

  std::array<std::uint8_t, 256> p_{};
  double xo_ = 0.0;
  double yo_ = 0.0;
  double zo_ = 0.0;
};

// 多八度分形噪声（对齐 `PerlinNoise.java`）。
// 区分现代基于 `PositionalRandomFactory.fromHashOf("octave_i")` 的初始化，以及仅用于
// `BlendedNoise` / 旧下界群系的顺序消费 `skipOctave(262)` 兼容初始化路径。
class PerlinNoise {
public:
  static PerlinNoise create(RandomSource &random, int firstOctave,
                            std::vector<double> amplitudes);
  static PerlinNoise create(RandomSource &random, std::span<const int> octaves);
  static PerlinNoise createLegacyForBlendedNoise(RandomSource &random,
                                                 std::span<const int> octaves);
  static PerlinNoise
  createLegacyForNetherBiome(RandomSource &random, int firstOctave,
                             std::vector<double> amplitudes);

  double getValue(double x, double y, double z) const noexcept {
    return getValue(x, y, z, 0.0, 0.0);
  }
  double getValue(double x, double y, double z, double yScale,
                  double yFudge) const noexcept;
  double maxValue() const noexcept { return maxValue_; }
  double maxBrokenValue(double yScale) const noexcept {
    return edgeValue(yScale + 2.0);
  }

  // 与原版一致的倒排八度读取（下标 0 返回最高频八度）。
  const ImprovedNoise *getOctaveNoise(int i) const noexcept;
  int firstOctave() const noexcept { return firstOctave_; }
  const std::vector<double> &amplitudes() const noexcept { return amplitudes_; }

  static double wrap(double x) noexcept {
    return x -
           static_cast<double>(worldgen_math::lfloor(x / 3.3554432E7 + 0.5)) *
               3.3554432E7;
  }

private:
  PerlinNoise(RandomSource &random, int firstOctave,
              std::vector<double> amplitudes, bool useNewInitialization);

  double edgeValue(double noiseValue) const noexcept;

  std::vector<std::optional<ImprovedNoise>> noiseLevels_;
  int firstOctave_ = 0;
  std::vector<double> amplitudes_;
  double lowestFreqValueFactor_ = 0.0;
  double lowestFreqInputFactor_ = 0.0;
  double maxValue_ = 0.0;
};

struct NoiseParameters {
  int firstOctave = 0;
  std::vector<double> amplitudes;

  friend bool operator==(const NoiseParameters &,
                         const NoiseParameters &) = default;
};

// 26.1 标准双 Perlin 差值噪声（对齐 `NormalNoise.java`）。
// 将第二份同参 Perlin 的输入坐标乘以无理近似倍数 `1.0181268882175227`，抑制轴向网格对齐伪影，
// 并按有效八度跨度归一化使输出范围保持稳定。
class NormalNoise {
public:
  static constexpr double kInputFactor = 1.0181268882175227;

  static NormalNoise create(RandomSource &random,
                            const NoiseParameters &parameters);
  static NormalNoise createLegacyNetherBiome(RandomSource &random,
                                             const NoiseParameters &parameters);

  double getValue(double x, double y, double z) const noexcept;
  double maxValue() const noexcept { return maxValue_; }
  const NoiseParameters &parameters() const noexcept { return parameters_; }

private:
  NormalNoise(RandomSource &random, const NoiseParameters &parameters,
              bool useNewInitialization);

  static double expectedDeviation(int octaveSpan) noexcept {
    return 0.1 * (1.0 + 1.0 / (static_cast<double>(octaveSpan) + 1.0));
  }

  double valueFactor_ = 0.0;
  PerlinNoise first_;
  PerlinNoise second_;
  double maxValue_ = 0.0;
  NoiseParameters parameters_;
};

// 老式高低频混合 3D 地形噪声（对齐 `BlendedNoise.java`，用于旧山脉/下界/末地基本密度）。
class BlendedNoise {
public:
  static BlendedNoise createUnseeded(double xzScale, double yScale,
                                     double xzFactor, double yFactor,
                                     double smearScaleMultiplier);
  BlendedNoise(RandomSource &random, double xzScale, double yScale,
               double xzFactor, double yFactor, double smearScaleMultiplier);

  BlendedNoise withNewRandom(RandomSource &terrainRandom) const;
  double compute(int blockX, int blockY, int blockZ) const noexcept;

  double minValue() const noexcept { return -maxValue_; }
  double maxValue() const noexcept { return maxValue_; }
  double xzScale() const noexcept { return xzScale_; }
  double yScale() const noexcept { return yScale_; }
  double xzFactor() const noexcept { return xzFactor_; }
  double yFactor() const noexcept { return yFactor_; }
  double smearScaleMultiplier() const noexcept { return smearScaleMultiplier_; }

private:
  PerlinNoise minLimitNoise_;
  PerlinNoise maxLimitNoise_;
  PerlinNoise mainNoise_;
  double xzMultiplier_ = 0.0;
  double yMultiplier_ = 0.0;
  double xzFactor_ = 0.0;
  double yFactor_ = 0.0;
  double smearScaleMultiplier_ = 0.0;
  double maxValue_ = 0.0;
  double xzScale_ = 0.0;
  double yScale_ = 0.0;
};

} // namespace redstone::worldgen
