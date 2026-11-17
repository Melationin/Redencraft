#pragma once
// 下界传送门（Nether Portal）框架识别、最大矩形搜索、传送门构建及跨维度传送解析接口。
// 存在原因：将对齐原版 26.1 的 BlockUtil、PortalShape 与 PortalForcer 行为从方块交互和实体刻中
// 解耦，供打火石点燃、实体碰撞跨维及传送门生成/复用测试共同调用。

#include "redstone/engine.hpp"
#include "redstone/entity.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace redstone {

class ServerWorldSet;
class WorldChunkManager;

// 下界传送门平面仅沿 X 或 Z 水平轴向展开；通用矩形查找则支持三维任意两轴（BlockAxis）。
enum class PortalAxis : std::uint8_t { X, Z };
enum class BlockAxis : std::uint8_t { X, Y, Z };

// 从方块状态属性读取传送门朝向轴（默认 X），用于匹配框架方向与实体碰撞体对齐。
inline PortalAxis portalAxisOfState(const BlockState &state) noexcept {
  return state.getView("axis") == "z" ? PortalAxis::Z : PortalAxis::X;
}

// 将六向 Direction 映射为其所属三维坐标轴，供通用矩形扩展与传送门方向计算使用。
constexpr BlockAxis axisOfDirection(Direction dir) noexcept {
  switch (dir) {
  case Direction::West:
  case Direction::East:
    return BlockAxis::X;
  case Direction::Down:
  case Direction::Up:
    return BlockAxis::Y;
  case Direction::North:
  case Direction::South:
    return BlockAxis::Z;
  }
  return BlockAxis::Y;
}

constexpr bool isHorizontalDirection(Direction dir) noexcept {
  return dir != Direction::Down && dir != Direction::Up;
}

constexpr PortalAxis otherPortalAxis(PortalAxis axis) noexcept {
  return axis == PortalAxis::X ? PortalAxis::Z : PortalAxis::X;
}

// 对齐原版 net.minecraft.util.BlockUtil.FoundRectangle，表示二维平面上找到的矩形区域。
struct FoundRectangle {
  Pos minCorner{};
  int axis1Size = 0;
  int axis2Size = 0;

  bool operator==(const FoundRectangle &) const = default;
};

// 从 center 出发沿两轴扩展并用单调栈求最大连续合法矩形（对齐原版 BlockUtil.getLargestRectangleAround），
// 用于定位不规则黑曜石框内最大可用传送门或实体进入的完整传送门区域。
FoundRectangle getLargestRectangleAround(
    Pos center, BlockAxis axis1, int limit1, BlockAxis axis2, int limit2,
    const std::function<bool(Pos)> &test);

// 对齐原版 net.minecraft.world.level.portal.PortalShape。
// 存在原因：封装黑曜石外框校验（宽 2..21、高 3..21）、传送门方块批量生成，以及基于矩形相对坐标的
// 跨维安全落点计算，确保点燃与传送位置语义与原版一致。
class PortalShape {
public:
  static constexpr int kMinWidth = 2;
  static constexpr int kMaxWidth = 21;
  static constexpr int kMinHeight = 3;
  static constexpr int kMaxHeight = 21;

  static bool isEmptyBlock(const World &world, const BlockState &state);
  static bool isFrameBlock(const BlockState &state);

  // 搜索尚未激活的空框架；preferredAxis 优先匹配调用方期望朝向，找不到有效框架时返回空。
  static std::optional<PortalShape> findEmptyPortalShape(
      const World &world, Pos pos, PortalAxis preferredAxis);
  static std::optional<PortalShape> findPortalShape(
      const World &world, Pos pos,
      const std::function<bool(const PortalShape &)> &isValidFn,
      PortalAxis preferredAxis);
  // 按指定轴向测量框架（即使无效或未完全填充也返回测量结果），用于诊断或增量检测。
  static PortalShape findAnyShape(const World &world, Pos pos, PortalAxis axis);

  bool isValid() const noexcept {
    return width_ >= kMinWidth && width_ <= kMaxWidth &&
           height_ >= kMinHeight && height_ <= kMaxHeight;
  }
  bool isComplete() const noexcept {
    return isValid() && numPortalBlocks_ == width_ * height_;
  }

  PortalAxis axis() const noexcept { return axis_; }
  Direction rightDir() const noexcept { return rightDir_; }
  Pos bottomLeft() const noexcept { return bottomLeft_; }
  int width() const noexcept { return width_; }
  int height() const noexcept { return height_; }
  int numPortalBlocks() const noexcept { return numPortalBlocks_; }

  // 在已验证的框架内部填充对应轴向的 nether_portal 方块。
  void createPortalBlocks(World &world) const;

  // 计算实体在传送门矩形内的归一化相对偏移，以便在目标传送门按比例还原站位。
  static Vec3 getRelativePosition(const FoundRectangle &rect, PortalAxis axis,
                                  Vec3 position, double entityWidth,
                                  double entityHeight);
  // 在目标传送门附近搜索无碰撞阻塞的安全立足点，避免实体传送后卡入方块。
  static Vec3 findCollisionFreePosition(Vec3 bottomCenter, World &world,
                                        double entityWidth,
                                        double entityHeight);

private:
  PortalShape(PortalAxis axis, int numPortalBlocks, Direction rightDir,
              Pos bottomLeft, int width, int height) noexcept
      : axis_(axis), rightDir_(rightDir), numPortalBlocks_(numPortalBlocks),
        bottomLeft_(bottomLeft), width_(width), height_(height) {}

  PortalAxis axis_ = PortalAxis::X;
  Direction rightDir_ = Direction::West;
  int numPortalBlocks_ = 0;
  Pos bottomLeft_{};
  int width_ = 0;
  int height_ = 0;
};

// 世界边界约束，防止跨维传送门搜索或新建越出可活动世界边界之外。
struct WorldBorderBounds {
  double minX = -29999984.0;
  double maxX = 29999984.0;
  double minZ = -29999984.0;
  double maxZ = 29999984.0;

  bool isWithinBounds(Pos pos) const noexcept;
  bool isWithinBounds(const Aabb &box) const noexcept;
  Pos clampToBounds(double x, double y, double z) const noexcept;
  Pos clampToBounds(Pos pos) const noexcept;
};

// 对齐原版 net.minecraft.world.level.portal.PortalForcer，负责在目标维度查找或新建出口传送门。
// 注意：与原版 ServerLevel 不同，redstone-cpp 尚无世界生成。
// 每当目标维度缺失或目标区域未生成/未加载时，PortalForcer 直接返回 std::nullopt，
// 不会生成区块、放置方块或传送实体。
class PortalForcer {
public:
  static constexpr int kTicketRadius = 3;
  static constexpr int kNetherPortalRadius = 16;
  static constexpr int kOverworldPortalRadius = 128;

  explicit PortalForcer(World &world) noexcept : world_(world) {}

  // 基于 POI 索引在目标缩放坐标附近搜索最近的现有下界传送门。
  std::optional<Pos> findClosestPortalPosition(
      Pos approximateExitPos, bool toNether,
      const WorldBorderBounds &border = {}) const;

  // 在目标位置附近按原版螺旋柱逻辑寻找可承托框架的地形并建造完整下界传送门。
  std::optional<FoundRectangle> createPortal(
      Pos origin, PortalAxis portalAxis,
      const WorldBorderBounds &border = {});

private:
  bool isChunkColumnLoaded(int x, int z) const noexcept;
  bool canPortalReplaceBlock(Pos pos) const;
  bool canHostFrame(Pos origin, Direction direction, int offset) const;
  int surfaceMotionBlockingY(int x, int z) const;

  World &world_;
};

// 跨维下界传送门迁移的纯计算结果，将落点计算与实际的跨世界实体迁移解耦。
struct PortalTransitionResult {
  std::string targetDimensionId;
  Vec3 position{};
  Vec3 deltaMovement{};
  float yawDelta = 0.0f;
  Pos exitPortalPos{};
  bool createdPortal = false;
};

// 判断世界是否为支持下界传送门往返的维度（目前为主世界或下界）。
bool isInPortalDimension(const World &world) noexcept;

// 解析实体经由下界传送门跨维度的目标状态。
// 以下情况返回 std::nullopt：
// - sourceWorld 不是主世界或下界（// TODO: 支持自定义维度与末地维度）
// - 目标维度缺失或未注册
// - 目标传送门区域未生成或未加载（此时不会新建传送门，也不会传送实体）
std::optional<PortalTransitionResult> computeNetherPortalTransition(
    World &sourceWorld, const Entity &entity, Pos portalEntryPos,
    const std::function<World *(std::string_view)> &findWorld,
    const WorldBorderBounds &targetBorder = {});

// 执行实体向目标维度的跨维传送；若提供了 targetChunkManager，会在出口处放置
// TicketType::Portal（半径 3）的加载票。
// 当目标世界或出口传送门区域不可用/未生成时返回 false，并保持源世界中的实体原封不动。
bool teleportEntityThroughPortal(
    World &sourceWorld, const std::shared_ptr<Entity> &entity,
    Pos portalEntryPos,
    const std::function<World *(std::string_view)> &findWorld,
    const std::function<WorldChunkManager *(std::string_view)>
        &findChunkManager = {},
    const WorldBorderBounds &targetBorder = {});

// 批量处理所有世界里待执行的跨维传送请求，避免在实体 tick 遍历期间直接修改跨世界实体列表。
std::size_t processPendingPortalTransfers(
    ServerWorldSet &worlds, const WorldBorderBounds &targetBorder = {});

} // namespace redstone
