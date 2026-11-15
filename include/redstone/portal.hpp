#pragma once

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

enum class PortalAxis : std::uint8_t { X, Z };
enum class BlockAxis : std::uint8_t { X, Y, Z };

inline PortalAxis portalAxisOfState(const BlockState &state) noexcept {
  return state.getView("axis") == "z" ? PortalAxis::Z : PortalAxis::X;
}

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

// Vanilla net.minecraft.util.BlockUtil.FoundRectangle.
struct FoundRectangle {
  Pos minCorner{};
  int axis1Size = 0;
  int axis2Size = 0;

  bool operator==(const FoundRectangle &) const = default;
};

FoundRectangle getLargestRectangleAround(
    Pos center, BlockAxis axis1, int limit1, BlockAxis axis2, int limit2,
    const std::function<bool(Pos)> &test);

// Vanilla net.minecraft.world.level.portal.PortalShape.
class PortalShape {
public:
  static constexpr int kMinWidth = 2;
  static constexpr int kMaxWidth = 21;
  static constexpr int kMinHeight = 3;
  static constexpr int kMaxHeight = 21;

  static bool isEmptyBlock(const World &world, const BlockState &state);
  static bool isFrameBlock(const BlockState &state);

  static std::optional<PortalShape> findEmptyPortalShape(
      const World &world, Pos pos, PortalAxis preferredAxis);
  static std::optional<PortalShape> findPortalShape(
      const World &world, Pos pos,
      const std::function<bool(const PortalShape &)> &isValidFn,
      PortalAxis preferredAxis);
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

  void createPortalBlocks(World &world) const;

  static Vec3 getRelativePosition(const FoundRectangle &rect, PortalAxis axis,
                                  Vec3 position, double entityWidth,
                                  double entityHeight);
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

// Vanilla net.minecraft.world.level.portal.PortalForcer.
// Note: unlike vanilla ServerLevel, redstone-cpp has no world generation yet.
// Whenever the target dimension is missing or the target region is ungenerated /
// unloaded, PortalForcer returns std::nullopt without generating chunks, placing
// blocks, or teleporting entities.
class PortalForcer {
public:
  static constexpr int kTicketRadius = 3;
  static constexpr int kNetherPortalRadius = 16;
  static constexpr int kOverworldPortalRadius = 128;

  explicit PortalForcer(World &world) noexcept : world_(world) {}

  std::optional<Pos> findClosestPortalPosition(
      Pos approximateExitPos, bool toNether,
      const WorldBorderBounds &border = {}) const;

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

struct PortalTransitionResult {
  std::string targetDimensionId;
  Vec3 position{};
  Vec3 deltaMovement{};
  float yawDelta = 0.0f;
  Pos exitPortalPos{};
  bool createdPortal = false;
};

bool isInPortalDimension(const World &world) noexcept;

// Resolves cross-dimension Nether Portal transition for an entity.
// Returns std::nullopt when:
// - sourceWorld is not Overworld or Nether (custom/End dimensions: TODO)
// - target dimension is missing/unregistered
// - destination portal area is ungenerated/unloaded (does NOT create portal or teleport)
std::optional<PortalTransitionResult> computeNetherPortalTransition(
    World &sourceWorld, const Entity &entity, Pos portalEntryPos,
    const std::function<World *(std::string_view)> &findWorld,
    const WorldBorderBounds &targetBorder = {});

// Executes cross-dimension teleport for an entity into targetWorld, placing a
// TicketType::Portal radius-3 ticket if targetChunkManager is provided.
// Returns false and leaves entity untouched in sourceWorld when target world or
// exit portal area is unavailable / ungenerated.
bool teleportEntityThroughPortal(
    World &sourceWorld, const std::shared_ptr<Entity> &entity,
    Pos portalEntryPos,
    const std::function<World *(std::string_view)> &findWorld,
    const std::function<WorldChunkManager *(std::string_view)>
        &findChunkManager = {},
    const WorldBorderBounds &targetBorder = {});

std::size_t processPendingPortalTransfers(
    ServerWorldSet &worlds, const WorldBorderBounds &targetBorder = {});

} // namespace redstone
