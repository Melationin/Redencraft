#pragma once
// 对齐 Minecraft Java Edition 26.1 的分片三次 Hermite 样条（`CubicSpline.java`）。
// 主要用于计算主世界地形的 `offset`、`factor` 与 `jaggedness` 等非线性海拔调节曲线。
// 之所以严格以 32-bit `float` 执行区间二分与 Hermite 插值并独立计算精确极值上/下界
// （含过冲项 `0.25f * minLerp2`），是因为 `DensityFunction::minValue/maxValue` 会直接参与
// `Min`/`Max` 节点的短路剪枝与后续地形网格高度预估。

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace redstone::worldgen {

struct DensityNode;
using DensityNodePtr = std::shared_ptr<const DensityNode>;
struct DensityFunctionContext;

class CubicSpline;
using CubicSplinePtr = std::shared_ptr<const CubicSpline>;

// 一维或多维嵌套三次 Hermite 样条节点（常量或按某一密度函数坐标分段的多点插值曲线）。
class CubicSpline : public std::enable_shared_from_this<const CubicSpline> {
public:
  struct Point {
    float location = 0.0f;
    CubicSplinePtr value;
    float derivative = 0.0f;
  };

  static CubicSplinePtr constant(float value);
  static CubicSplinePtr multipoint(DensityNodePtr coordinate,
                                   std::vector<float> locations,
                                   std::vector<CubicSplinePtr> values,
                                   std::vector<float> derivatives);

  bool isConstant() const noexcept { return isConstant_; }
  float constantValue() const noexcept { return constantValue_; }
  const DensityNodePtr &coordinate() const noexcept { return coordinate_; }
  const std::vector<float> &locations() const noexcept { return locations_; }
  const std::vector<CubicSplinePtr> &values() const noexcept { return values_; }
  const std::vector<float> &derivatives() const noexcept {
    return derivatives_;
  }
  float minValue() const noexcept { return minValue_; }
  float maxValue() const noexcept { return maxValue_; }

  float apply(int blockX, int blockY, int blockZ) const;
  float apply(const DensityFunctionContext &ctx) const;

  // 按给定坐标变换访问器（例如 `RandomState` 噪声注入）深拷贝样条树并重新推导值域。
  CubicSplinePtr mapAll(
      const std::function<DensityNodePtr(const DensityNodePtr &)> &visitor) const;

private:
  CubicSpline() = default;

  bool isConstant_ = true;
  float constantValue_ = 0.0f;
  DensityNodePtr coordinate_;
  std::vector<float> locations_;
  std::vector<CubicSplinePtr> values_;
  std::vector<float> derivatives_;
  float minValue_ = 0.0f;
  float maxValue_ = 0.0f;
};

} // namespace redstone::worldgen
