#pragma once
// 实体子系统内部辅助工具：封装流体判断、UUID 生成、爆炸射线步长、AABB/体素碰撞、
// 支撑判定及区块区段空间索引等共用实现细节，避免在多个实体实现文件间重复代码。
#include "redstone/entity.hpp"
#include "redstone/containers.hpp"
#include "redstone/fluid_flow.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace redstone {
namespace entity_internal {
// 判断流体状态是否为非空的水，供实体浮力、熄火及水下状态逻辑复用。
inline bool isWaterFluidState(const FluidState &fs) {
  return !fs.isEmpty() && fs.getType().isSame(Fluids::water());
}

// 生成符合 RFC 4122 v4 规范且非全零的 128 位实体 UUID（拆分为 4 个 int32_t），
// 以匹配原版 NBT 中的 UUID 整数数组格式并保证实体全局标识唯一。
inline std::array<std::int32_t, 4> generateEntityUuid() {
  static thread_local std::mt19937_64 generator([] {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device()};
    return std::mt19937_64(seed);
  }());
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  do {
    high = generator();
    low = generator();
    high = (high & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    low = (low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
  } while (high == 0 && low == 0);
  return {static_cast<std::int32_t>(high >> 32),
          static_cast<std::int32_t>(high),
          static_cast<std::int32_t>(low >> 32),
          static_cast<std::int32_t>(low)};
}

constexpr double EPS = 1.0e-7;
// 预计算原版爆炸射线遍历的 1352 个固定步进向量（16x16x16 立方体外壳各格点归一化后乘以 0.3），
// 存在原因：避免每次爆炸重复做 1352 次开方归一化，同时锁定原版射线顺序。
inline const std::array<Vec3, 1352> &explosionSteps() {
  static const auto steps = [] {
    std::array<Vec3, 1352> result;
    std::size_t index = 0;
    // 保持原版外壳格点遍历顺序与精确的归一化计算次序，确保爆炸破坏范围与原版一致。
    for (int x = 0; x < 16; ++x)
      for (int y = 0; y < 16; ++y)
        for (int z = 0; z < 16; ++z) {
          if (x != 0 && x != 15 && y != 0 && y != 15 && z != 0 && z != 15)
            continue;
          double dx = x / 15.0 * 2.0 - 1.0;
          double dy = y / 15.0 * 2.0 - 1.0;
          double dz = z / 15.0 * 2.0 - 1.0;
          const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
          dx /= length;
          dy /= length;
          dz /= length;
          result[index++] = {dx * 0.3, dy * 0.3, dz * 0.3};
        }
    return result;
  }();
  return steps;
}
// 对应原版 Mth.floor：通过截断与负小数修正实现向下取整，避免在爆炸射线高频循环中
// 调用 libm 的 floor（坐标始终远小于 int 溢出范围）。
inline int floorToInt(double value) {
  const int truncated = static_cast<int>(value);
  return truncated - (value < static_cast<double>(truncated) ? 1 : 0);
}
constexpr double COLLISION_EPS = 1.0e-5;
// 为缺少命名空间前缀的标识符补全 "minecraft:"，统一查表口径。
inline std::string canonical(std::string n) {
  if (n.find(':') == std::string::npos)
    n = "minecraft:" + n;
  return n;
}
inline bool air(const BlockState &s) {
  return s.isAir();
}
// 获取方块在指定位置的有效碰撞体素形状；MOVING_PISTON 的碰撞箱依赖方块实体进度，
// 无法只查静态表，因此借助调用方提供的 scratch 缓冲按需生成，避免普通方块堆分配。
inline const VoxelShape &shapeAt(World &world, Pos pos, const BlockState &state,
                          VoxelShape &scratch) {
  if (!state.is(Blocks::MOVING_PISTON))
    return collisionShape(state);
  scratch = collisionShape(world, pos, state);
  return scratch;
}
// 判断指定位置方块的碰撞形状是否为完整 16x16x16 立方体。
inline bool fullCube(World &world, Pos pos) {
  const auto state = world.getBlock(pos);
  if (state.isAir())
    return false;
  VoxelShape scratch;
  const auto &shape = shapeAt(world, pos, state, scratch);
  if (shape.empty())
    return false;
  for (auto &box : shape)
    if (!(box.x0 <= 0 && box.y0 <= 0 && box.z0 <= 0 && box.x1 >= 16 &&
          box.y1 >= 16 && box.z1 >= 16))
      return false;
  return true;
}
// 按轴索引（0=X、1=Y、2=Z）访问 ShapeBox/Aabb 边界，使三轴碰撞解算共用同一套逻辑。
inline double boxMin(const ShapeBox &b, int axis) {
  return axis == 0 ? b.x0 : axis == 1 ? b.y0 : b.z0;
}
inline double boxMax(const ShapeBox &b, int axis) {
  return axis == 0 ? b.x1 : axis == 1 ? b.y1 : b.z1;
}
inline double aabbMin(const Aabb &b, int axis) {
  return axis == 0 ? b.minX : axis == 1 ? b.minY : b.minZ;
}
inline double aabbMax(const Aabb &b, int axis) {
  return axis == 0 ? b.maxX : axis == 1 ? b.maxY : b.maxZ;
}
// 判断实体 Aabb 与方块 ShapeBox 在除当前移动轴外的其余两轴上是否存在有效重叠（带 EPS 容差）。
inline bool overlapsOtherAxes(const Aabb &box, const ShapeBox &shape, int axis) {
  for (int other = 0; other < 3; ++other) {
    if (other == axis)
      continue;
    if (!(aabbMax(box, other) > boxMin(shape, other) + EPS &&
          aabbMin(box, other) < boxMax(shape, other) - EPS))
      return false;
  }
  return true;
}
// 沿单一轴向计算实体 Aabb 移动 distance 时受碰撞箱集合阻挡后的实际可移动距离，
// 用于对齐原版分轴扫掠碰撞（先 Y 后 X/Z）而非同时三轴裁剪。
inline double collideAxis(int axis, const Aabb &box,
                   const std::vector<ShapeBox> &colliders, double distance) {
  for (const auto &part : colliders) {
    if (std::abs(distance) < EPS)
      return 0;
    if (!overlapsOtherAxes(box, part, axis))
      continue;
    if (distance > 0) {
      if (boxMin(part, axis) < aabbMax(box, axis) - EPS)
        continue;
      distance = std::min(distance, boxMin(part, axis) - aabbMax(box, axis));
    } else {
      if (boxMax(part, axis) > aabbMin(box, axis) + EPS)
        continue;
      distance = std::max(distance, boxMax(part, axis) - aabbMin(box, axis));
    }
  }
  return distance;
}
// 使用 slab 平板求交法判定线段 [from, to] 是否命中方块碰撞箱 ShapeBox。
inline bool rayBox(Vec3 from, Vec3 to, const ShapeBox &box) {
  double tmin = 0, tmax = 1;
  const double lo[3] = {box.x0, box.y0, box.z0};
  const double hi[3] = {box.x1, box.y1, box.z1};
  const double start[3] = {from.x, from.y, from.z};
  const double delta[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(delta[axis]) < EPS) {
      if (start[axis] < lo[axis] || start[axis] > hi[axis])
        return false;
      continue;
    }
    double a = (lo[axis] - start[axis]) / delta[axis];
    double b = (hi[axis] - start[axis]) / delta[axis];
    if (a > b)
      std::swap(a, b);
    tmin = std::max(tmin, a);
    tmax = std::min(tmax, b);
    if (tmin > tmax)
      return false;
  }
  return tmax >= tmin;
}
inline double clamp(double value, double min, double max) {
  return std::max(min, std::min(max, value));
}
// 对齐原版 Mth.wrapDegrees：将角度折叠到 [-180, 180) 区间，防止实体旋转角度越界或插值跳变。
inline float wrapDegrees(double value) {
  double result = std::fmod(value + 180.0, 360.0);
  if (result < 0)
    result += 360.0;
  return static_cast<float>(result - 180.0);
}
// 判断方块是否可作为完整刚性支撑面（排除空气、流体、铁轨以及非满面碰撞箱）。
inline bool isFullSupport(World &world, Pos pos) {
  const auto state = world.getBlock(pos);
  if (state.isAir() || isLiquidBlock(state) || isRailBlock(state))
    return false;
  VoxelShape scratch;
  const auto &shape = shapeAt(world, pos, state, scratch);
  for (auto &box : shape)
    if (box.x0 <= 0 && box.y0 <= 0 && box.z0 <= 0 && box.x1 >= 16 &&
        box.y1 >= 16 && box.z1 >= 16)
      return true;
  return false;
}
// 对应原版 Block.isFaceSturdy(..., SupportType.RIGID)：排除铁轨、流体以及顶面不完整的
// 方块（例如墙的顶面仅为 4..12 的立柱，因此铁轨无法放置其上）。
inline bool canSupportRail(World &world, Pos pos) {
  return isFullSupport(world, pos);
}
// 检查依赖附着条件的方块（如地毯、失活珊瑚扇、活塞头、铁轨）当前是否满足存活条件。
inline bool canSurvive(World &world, Pos pos, const BlockState &state) {
  if (isWoolCarpet(state))
    return !air(world.getBlock(pos.relative(Direction::Down)));
  if (isDeadCoralFan(state))
    return coralCanSurvive(world, pos, state);
  if (state.is(Blocks::PISTON_HEAD)) {
    auto d = directionOf(state);
    auto base = world.getBlock(pos.relative(opposite(d)));
    return base.is(Blocks::PISTON) || base.is(Blocks::STICKY_PISTON) ||
           (base.is(Blocks::MOVING_PISTON) &&
            facingOf(base) == facingOf(state));
  }
  if (isRailBlock(state))
    return railCanSurvive(world, pos, state);
  return true;
}
inline bool isFree(const BlockState &state) {
  return air(state) || isReplaceable(state);
}
// 复用的线程局部碰撞箱查询缓冲：对齐原版 Level.noCollision，避免每次调用反复分配内存。
inline std::vector<ShapeBox> &scratchBoxes() {
  thread_local std::vector<ShapeBox> boxes;
  return boxes;
}
// 对应原版 SectionPos.blockToSectionCoord：将世界坐标转换到 16 格区段坐标。
inline int blockToSection(double coordinate) {
  return static_cast<int>(std::floor(coordinate)) >> 4;
}
// 对应原版 SectionPos.asLong(x, y, z)：高 26 位存 x、中间 26 位存 z、低 12 位存 y，
// 将三维区段坐标压缩为 64 位键值，并提供对应的有符号坐标解包函数。
inline std::uint64_t sectionPosKey(int x, int y, int z) {
  return (static_cast<std::uint64_t>(x) & 0x3FFFFFFull) << 38 |
         (static_cast<std::uint64_t>(z) & 0x3FFFFFFull) << 12 |
         (static_cast<std::uint64_t>(y) & 0xFFFull);
}
inline int sectionKeyX(std::uint64_t key) {
  return (static_cast<int>(key >> 38) & 0x3FFFFFF) << 6 >> 6;
}
inline int sectionKeyY(std::uint64_t key) {
  return (static_cast<int>(key) & 0xFFF) << 20 >> 20;
}
inline int sectionKeyZ(std::uint64_t key) {
  return (static_cast<int>(key >> 12) & 0x3FFFFFF) << 6 >> 6;
}
inline std::uint64_t entitySectionKey(const Vec3 &position) {
  return sectionPosKey(blockToSection(position.x),
                       blockToSection(position.y),
                       blockToSection(position.z));
}
// 对应原版 EntitySectionStorage.forEachAccessibleNonEmptySection：水平搜索范围外扩 2 格、
// 向下外扩 4 格，确保包围盒跨越区段边界的实体（对应 CHONKY_ENTITY_SEARCH_GRACE = 2）
// 仍能被遍历到；支持返回 bool 的提前终止回调。
template <class F>
void forEachEntitySection(
    const std::map<std::uint64_t, std::unique_ptr<EntitySectionBucket>> &sections,
    const Aabb &box, F &&visit) {
  if (sections.empty())
    return;
  const int xMin = blockToSection(box.minX - 2.0);
  const int yMin = blockToSection(box.minY - 4.0);
  const int zMin = blockToSection(box.minZ - 2.0);
  const int xMax = blockToSection(box.maxX + 2.0);
  const int yMax = blockToSection(box.maxY);
  const int zMax = blockToSection(box.maxZ + 2.0);
  for (int x = xMin; x <= xMax; ++x) {
    const auto lowest = sectionPosKey(x, 0, 0);
    // 不能使用右开区间上界键，否则当 x == -1 时高位会溢出回绕。
    const auto highest = sectionPosKey(x, -1, -1);
    for (auto it = sections.lower_bound(lowest);
         it != sections.end() && it->first <= highest; ++it) {
      if (!it->second)
        continue;
      const int y = sectionKeyY(it->first);
      const int z = sectionKeyZ(it->first);
      if (y < yMin || y > yMax || z < zMin || z > zMax)
        continue;
      if constexpr (std::is_same_v<std::invoke_result_t<
                                       F &, const EntitySectionBucket &>,
                                   bool>) {
        if (!visit(*it->second))
          return;
      } else {
        visit(*it->second);
      }
    }
  }
}
// 解析实体所在方块的流体状态；若方块为含水宿主（waterlogged）且自身未携带流体属性，
// 则补全为满格水源（等级 8），确保水物理与浮力逻辑生效。
inline FluidState resolveEntityFluidState(const BlockState &state) {
  FluidState fs = fluidStateOf(state);
  if (fs.isEmpty() && isWaterloggedHost(state))
    fs = FluidState(Fluids::water(), 8, false);
  return fs;
}
} // namespace entity_internal
using namespace entity_internal;
} // namespace redstone
