#pragma once
// 对齐 Minecraft Java Edition 26.1 的 6 维气候参数空间与 R-Tree 最近邻索引（`Climate.java`）。
// 用途：把 `ClimateSampler` 输出的温度、湿度、大陆性、侵蚀度、深度、怪异度 6 条连续密度值
// 量化为整数定点坐标（×10000），并在数千条生物群系 7D 超矩形参数（含偏移量 `offset`）中
// 以 6 叉包围盒 R-Tree 与线程局部剪枝快速检索唯一最近邻生物群系，同时提供出生点启发式搜索。

#include "redstone/engine_types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace redstone::worldgen {

struct ClimateSampler;

// 对应 26.1 `Biome.ClimateSettings` 与高度温度衰减 / 积雪判定逻辑（`Biome.java:108-187, 363-415`）。
enum class BiomeTemperatureModifier : std::uint8_t {
  None = 0,
  Frozen = 1,
};

struct BiomeClimateProperties {
  bool hasPrecipitation = true;
  float temperature = 0.5f;
  BiomeTemperatureModifier temperatureModifier = BiomeTemperatureModifier::None;
  float downfall = 0.5f;

  float getHeightAdjustedTemperature(int blockX, int blockY, int blockZ,
                                     int seaLevel) const noexcept;
  bool warmEnoughToRain(int blockX, int blockY, int blockZ,
                        int seaLevel) const noexcept;
  bool coldEnoughToSnow(int blockX, int blockY, int blockZ,
                        int seaLevel) const noexcept;
  bool shouldMeltFrozenOceanIcebergSlightly(int blockX, int blockY, int blockZ,
                                            int seaLevel) const noexcept;
};

inline constexpr float kClimateQuantizationFactor = 10000.0f;

// 与 Java `(long)(coord * 10000.0F)` 一致，截断小数部分保留 4 位定点精度。
inline std::int64_t quantizeClimateCoord(float coord) noexcept {
  return static_cast<std::int64_t>(coord * kClimateQuantizationFactor);
}

inline float unquantizeClimateCoord(std::int64_t coord) noexcept {
  return static_cast<float>(coord) / kClimateQuantizationFactor;
}

// 一维量化闭区间 `[min, max]`（等价 `Climate.Parameter`）。
struct ClimateParameter {
  std::int64_t min = 0;
  std::int64_t max = 0;

  static ClimateParameter point(float value) { return span(value, value); }
  static ClimateParameter span(float minVal, float maxVal);
  static ClimateParameter span(const ClimateParameter &minParam,
                               const ClimateParameter &maxParam);

  std::int64_t distance(std::int64_t target) const noexcept {
    const std::int64_t above = target - max;
    const std::int64_t below = min - target;
    return above > 0 ? above : (below > 0 ? below : 0);
  }

  std::int64_t distance(const ClimateParameter &other) const noexcept {
    const std::int64_t above = other.min - max;
    const std::int64_t below = min - other.max;
    return above > 0 ? above : (below > 0 ? below : 0);
  }

  ClimateParameter
  spanWith(const std::optional<ClimateParameter> &other) const noexcept {
    if (!other)
      return *this;
    return ClimateParameter{min < other->min ? min : other->min,
                            max > other->max ? max : other->max};
  }
};

// 已量化的 6 维采样点（等价 `Climate.TargetPoint`）。
struct ClimateTargetPoint {
  std::int64_t temperature = 0;
  std::int64_t humidity = 0;
  std::int64_t continentalness = 0;
  std::int64_t erosion = 0;
  std::int64_t depth = 0;
  std::int64_t weirdness = 0;

  static ClimateTargetPoint make(float temperature, float humidity,
                                 float continentalness, float erosion,
                                 float depth, float weirdness) noexcept {
    return ClimateTargetPoint{
        .temperature = quantizeClimateCoord(temperature),
        .humidity = quantizeClimateCoord(humidity),
        .continentalness = quantizeClimateCoord(continentalness),
        .erosion = quantizeClimateCoord(erosion),
        .depth = quantizeClimateCoord(depth),
        .weirdness = quantizeClimateCoord(weirdness),
    };
  }

  std::array<std::int64_t, 7> toParameterArray() const noexcept {
    return {temperature, humidity, continentalness, erosion, depth, weirdness,
            0};
  }
};

// 生物群系在 6 维气候空间 + `offset` 第 7 轴上的区间规则（等价 `Climate.ParameterPoint`）。
struct ClimateParameterPoint {
  ClimateParameter temperature;
  ClimateParameter humidity;
  ClimateParameter continentalness;
  ClimateParameter erosion;
  ClimateParameter depth;
  ClimateParameter weirdness;
  std::int64_t offset = 0;

  static ClimateParameterPoint
  make(float temperature, float humidity, float continentalness, float erosion,
       float depth, float weirdness, float offset) {
    return ClimateParameterPoint{
        .temperature = ClimateParameter::point(temperature),
        .humidity = ClimateParameter::point(humidity),
        .continentalness = ClimateParameter::point(continentalness),
        .erosion = ClimateParameter::point(erosion),
        .depth = ClimateParameter::point(depth),
        .weirdness = ClimateParameter::point(weirdness),
        .offset = quantizeClimateCoord(offset),
    };
  }

  static ClimateParameterPoint
  make(const ClimateParameter &temperature, const ClimateParameter &humidity,
       const ClimateParameter &continentalness, const ClimateParameter &erosion,
       const ClimateParameter &depth, const ClimateParameter &weirdness,
       float offset) {
    return ClimateParameterPoint{
        .temperature = temperature,
        .humidity = humidity,
        .continentalness = continentalness,
        .erosion = erosion,
        .depth = depth,
        .weirdness = weirdness,
        .offset = quantizeClimateCoord(offset),
    };
  }

  std::int64_t fitness(const ClimateTargetPoint &target) const noexcept;
  std::array<ClimateParameter, 7> parameterSpace() const noexcept;
};

namespace detail {
class RawClimateRTree {
public:
  struct FlatNode {
    std::int64_t min[7]{};
    std::int64_t max[7]{};
    std::uint32_t firstChildOrValue = 0;
    std::uint8_t childCount = 0;
  };

  RawClimateRTree();
  ~RawClimateRTree();
  RawClimateRTree(const RawClimateRTree &) = delete;
  RawClimateRTree &operator=(const RawClimateRTree &) = delete;
  RawClimateRTree(RawClimateRTree &&) noexcept;
  RawClimateRTree &operator=(RawClimateRTree &&) noexcept;

  static RawClimateRTree
  build(const std::vector<ClimateParameterPoint> &points);

  std::size_t search(const ClimateTargetPoint &target) const noexcept;
  bool empty() const noexcept { return nodes_.empty(); }

private:
  std::uint64_t treeId_ = 0;
  std::vector<FlatNode> nodes_;
};
} // namespace detail

// 将泛型条目集合（如生物群系 ID 字符串或整型键）编入 7 维 6 叉 R-Tree 并提供最近邻查询。
// 线程安全性：`search` 在 `thread_local` 映射中保存 `lastResult`（以单调 `treeId` 为键），
// 多线程区块生成并发读取同一只读 `ClimateParameterList` 时无锁且互不干扰。
template <class T> class ClimateParameterList {
public:
  using Entry = std::pair<ClimateParameterPoint, T>;

  ClimateParameterList() = default;
  explicit ClimateParameterList(std::vector<Entry> entries)
      : entries_(std::move(entries)) {
    if (entries_.empty())
      throw std::invalid_argument(
          "ClimateParameterList requires at least one entry");
    std::vector<ClimateParameterPoint> points;
    points.reserve(entries_.size());
    values_.reserve(entries_.size());
    for (const auto &entry : entries_) {
      points.push_back(entry.first);
      values_.push_back(entry.second);
    }
    tree_ = detail::RawClimateRTree::build(points);
  }

  const std::vector<Entry> &entries() const noexcept { return entries_; }
  bool empty() const noexcept { return entries_.empty(); }
  std::size_t size() const noexcept { return entries_.size(); }

  const T &findValue(const ClimateTargetPoint &target) const {
    return values_[tree_.search(target)];
  }

  // O(N) 暴力扫描对照接口，用于测试校验 R-Tree 剪枝与 Java 完全等价。
  const T &findValueBruteForce(const ClimateTargetPoint &target) const {
    std::size_t bestIdx = 0;
    std::int64_t bestFitness = entries_[0].first.fitness(target);
    for (std::size_t i = 1; i < entries_.size(); ++i) {
      const std::int64_t f = entries_[i].first.fitness(target);
      if (f < bestFitness) {
        bestFitness = f;
        bestIdx = i;
      }
    }
    return values_[bestIdx];
  }

private:
  std::vector<Entry> entries_;
  std::vector<T> values_;
  detail::RawClimateRTree tree_;
};

// 对齐 `Climate.SpawnFinder`：绕世界原点径向搜索最适合 `spawnTarget` 的世界出生点水平坐标。
class ClimateSpawnFinder {
public:
  static Pos findSpawnPosition(
      const std::vector<ClimateParameterPoint> &targetClimates,
      const ClimateSampler &sampler);
};

} // namespace redstone::worldgen
