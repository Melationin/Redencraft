#pragma once
// 对齐 Minecraft Java Edition 26.1 的世界生成数值提供器（`FloatProvider`、`IntProvider`、`HeightProvider`）
// 与 65536 项正余弦查找表（`Mth::sin` / `Mth::cos`）：
// 为 `Carvers`（洞穴/峡谷）与 `G-3`（特征放置修饰器、矿脉高度分布、结构起始高度）提供统一确定性采样。

#include "redstone/worldgen/random_source.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace picojson {
class value;
}

namespace redstone::worldgen {

// 对应 26.1 `VerticalAnchor.java`。
struct VerticalAnchor {
  enum class Kind : std::uint8_t {
    Absolute = 0,
    AboveBottom = 1,
    BelowTop = 2,
  };

  Kind kind = Kind::Absolute;
  int value = 0;

  static VerticalAnchor absolute(int y) noexcept {
    return {Kind::Absolute, y};
  }
  static VerticalAnchor aboveBottom(int offset) noexcept {
    return {Kind::AboveBottom, offset};
  }
  static VerticalAnchor belowTop(int offset) noexcept {
    return {Kind::BelowTop, offset};
  }

  int resolveY(int minGenY, int genDepth) const noexcept {
    switch (kind) {
    case Kind::Absolute:
      return value;
    case Kind::AboveBottom:
      return minGenY + value;
    case Kind::BelowTop:
      return genDepth - 1 + minGenY - value;
    }
    return value;
  }
};

struct WorldGenerationContext {
  int minGenY = -64;
  int genDepth = 384;
};

namespace worldgen_math {

float minecraftSin(double radians) noexcept;
float minecraftCos(double radians) noexcept;

int randomBetweenInclusive(RandomSource &rng, int minInclusive,
                           int maxInclusive);
float randomBetween(RandomSource &rng, float minInclusive,
                    float maxExclusive);

} // namespace worldgen_math

enum class FloatProviderKind : std::uint8_t {
  Constant,
  Uniform,
  Trapezoid,
  ClampedNormal,
};

struct FloatProvider {
  FloatProviderKind kind = FloatProviderKind::Constant;
  float min = 0.0f;
  float max = 0.0f;
  float plateau = 0.0f;
  float mean = 0.0f;
  float deviation = 0.0f;

  static FloatProvider constant(float v) noexcept {
    FloatProvider p;
    p.kind = FloatProviderKind::Constant;
    p.min = v;
    p.max = v;
    return p;
  }

  float sample(RandomSource &rng) const;
};

FloatProvider parseFloatProviderJson(const picojson::value &v,
                                     std::string_view ctx);

enum class IntProviderKind : std::uint8_t {
  Constant,
  Uniform,
  BiasedToBottom,
  Clamped,
  ClampedNormal,
  WeightedList,
  Trapezoid,
};

struct IntProvider;

struct WeightedIntEntry {
  std::vector<IntProvider> provider; // size 1 for leaf
  int weight = 1;
};

struct IntProvider {
  IntProviderKind kind = IntProviderKind::Constant;
  int minInclusive = 0;
  int maxInclusive = 0;
  int plateau = 0;
  float mean = 0.0f;
  float deviation = 0.0f;
  std::vector<IntProvider> child; // size 1 when kind == Clamped
  std::vector<WeightedIntEntry> weighted;

  static IntProvider constant(int v) noexcept {
    IntProvider p;
    p.kind = IntProviderKind::Constant;
    p.minInclusive = v;
    p.maxInclusive = v;
    return p;
  }

  int sample(RandomSource &rng) const;
};

IntProvider parseIntProviderJson(const picojson::value &v, std::string_view ctx);

enum class HeightProviderKind : std::uint8_t {
  Constant,
  Uniform,
  BiasedToBottom,
  VeryBiasedToBottom,
  Trapezoid,
  WeightedList,
};

struct HeightProvider;

struct WeightedHeightEntry {
  std::vector<HeightProvider> provider; // size 1
  int weight = 1;
};

struct HeightProvider {
  HeightProviderKind kind = HeightProviderKind::Constant;
  VerticalAnchor minInclusive = VerticalAnchor::absolute(0);
  VerticalAnchor maxInclusive = VerticalAnchor::absolute(0);
  int plateau = 0;
  int inner = 1;
  std::vector<WeightedHeightEntry> weighted;

  static HeightProvider uniform(VerticalAnchor minInc, VerticalAnchor maxInc) {
    HeightProvider p;
    p.kind = HeightProviderKind::Uniform;
    p.minInclusive = minInc;
    p.maxInclusive = maxInc;
    return p;
  }

  int sample(RandomSource &rng, const WorldGenerationContext &ctx) const;
};

HeightProvider parseHeightProviderJson(const picojson::value &v,
                                       std::string_view ctx);

VerticalAnchor parseVerticalAnchorJson(const picojson::value &v,
                                       std::string_view ctx);

} // namespace redstone::worldgen
