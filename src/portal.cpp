#include "redstone/portal.hpp"

#include "redstone/poi.hpp"
#include "redstone/server_worlds.hpp"
#include "redstone/shapes.hpp"
#include "redstone/tickets.hpp"
#include "redstone/world_chunk_manager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace redstone {

namespace {

constexpr int kMinPortalYOffsetFromMax = 1;

constexpr Direction negativeDir(BlockAxis axis) noexcept {
  switch (axis) {
  case BlockAxis::X:
    return Direction::West;
  case BlockAxis::Y:
    return Direction::Down;
  case BlockAxis::Z:
    return Direction::North;
  }
  return Direction::Down;
}

constexpr Direction positiveHorizontal(PortalAxis axis) noexcept {
  return axis == PortalAxis::X ? Direction::East : Direction::South;
}

constexpr Direction clockWise(Direction d) noexcept {
  switch (d) {
  case Direction::North:
    return Direction::East;
  case Direction::East:
    return Direction::South;
  case Direction::South:
    return Direction::West;
  case Direction::West:
    return Direction::North;
  default:
    return d;
  }
}

constexpr int stepX(Direction d) noexcept {
  return d == Direction::East ? 1 : (d == Direction::West ? -1 : 0);
}

constexpr int stepY(Direction d) noexcept {
  return d == Direction::Up ? 1 : (d == Direction::Down ? -1 : 0);
}

constexpr int stepZ(Direction d) noexcept {
  return d == Direction::South ? 1 : (d == Direction::North ? -1 : 0);
}

constexpr Pos movePos(Pos p, Direction d, int steps = 1) noexcept {
  return {p.x + stepX(d) * steps, p.y + stepY(d) * steps, p.z + stepZ(d) * steps};
}

constexpr Pos offsetPos(Pos p, BlockAxis axis, int delta) noexcept {
  switch (axis) {
  case BlockAxis::X:
    return {p.x + delta, p.y, p.z};
  case BlockAxis::Y:
    return {p.x, p.y + delta, p.z};
  case BlockAxis::Z:
    return {p.x, p.y, p.z + delta};
  }
  return p;
}

int getLimit(const std::function<bool(Pos)> &test, Pos center,
             Direction direction, int limit) {
  int count = 0;
  Pos cur = center;
  while (count < limit) {
    cur = cur.relative(direction);
    if (!test(cur))
      break;
    ++count;
  }
  return count;
}

struct IntBounds {
  int min = 0;
  int max = 0;
};

std::pair<IntBounds, int>
getMaxRectangleLocation(const std::vector<int> &columns) {
  int maxStart = 0;
  int maxEnd = 0;
  int maxHeight = 0;
  std::vector<int> stack;
  stack.reserve(columns.size() + 1);
  stack.push_back(0);

  for (int column = 1; column <= static_cast<int>(columns.size()); ++column) {
    const int height =
        column == static_cast<int>(columns.size()) ? 0 : columns[column];
    while (!stack.empty()) {
      const int topIdx = stack.back();
      const int stackHeight = columns[topIdx];
      if (height >= stackHeight) {
        stack.push_back(column);
        break;
      }
      stack.pop_back();
      const int start = stack.empty() ? 0 : stack.back() + 1;
      if (stackHeight * (column - start) > maxHeight * (maxEnd - maxStart)) {
        maxEnd = column;
        maxStart = start;
        maxHeight = stackHeight;
      }
    }
    if (stack.empty())
      stack.push_back(column);
  }
  return {{maxStart, maxEnd - 1}, maxHeight};
}

double coordForAxis(const Vec3 &v, PortalAxis axis) noexcept {
  return axis == PortalAxis::X ? v.x : v.z;
}

int blockCoordForAxis(const Pos &p, PortalAxis axis) noexcept {
  return axis == PortalAxis::X ? p.x : p.z;
}

double inverseLerp(double value, double start, double end) noexcept {
  const double delta = end - start;
  return delta == 0.0 ? 0.0 : (value - start) / delta;
}

// Subtracts box `hole` from every AABB in `boxes` (VoxelShape BooleanOp.ONLY_FIRST).
void subtractBox(std::vector<Aabb> &boxes, const Aabb &hole) {
  std::vector<Aabb> next;
  next.reserve(boxes.size());
  for (const Aabb &b : boxes) {
    if (hole.maxX <= b.minX || hole.minX >= b.maxX || hole.maxY <= b.minY ||
        hole.minY >= b.maxY || hole.maxZ <= b.minZ || hole.minZ >= b.maxZ) {
      next.push_back(b);
      continue;
    }
    const double ix0 = std::max(b.minX, hole.minX);
    const double ix1 = std::min(b.maxX, hole.maxX);
    const double iy0 = std::max(b.minY, hole.minY);
    const double iy1 = std::min(b.maxY, hole.maxY);
    const double iz0 = std::max(b.minZ, hole.minZ);
    const double iz1 = std::min(b.maxZ, hole.maxZ);

    if (b.minX < ix0)
      next.push_back({b.minX, b.minY, b.minZ, ix0, b.maxY, b.maxZ});
    if (ix1 < b.maxX)
      next.push_back({ix1, b.minY, b.minZ, b.maxX, b.maxY, b.maxZ});
    if (b.minY < iy0)
      next.push_back({ix0, b.minY, b.minZ, ix1, iy0, b.maxZ});
    if (iy1 < b.maxY)
      next.push_back({ix0, iy1, b.minZ, ix1, b.maxY, b.maxZ});
    if (b.minZ < iz0)
      next.push_back({ix0, iy0, b.minZ, ix1, iy1, iz0});
    if (iz1 < b.maxZ)
      next.push_back({ix0, iy0, iz1, ix1, iy1, b.maxZ});
  }
  boxes.swap(next);
}

int getDistanceUntilEdgeAboveFrame(const World &world, Pos pos,
                                   Direction direction) {
  for (int width = 0; width <= PortalShape::kMaxWidth; ++width) {
    const Pos cur = movePos(pos, direction, width);
    const BlockState state = world.getBlock(cur);
    if (!PortalShape::isEmptyBlock(world, state)) {
      if (PortalShape::isFrameBlock(state))
        return width;
      break;
    }
    const Pos below = movePos(cur, Direction::Down, 1);
    if (!PortalShape::isFrameBlock(world.getBlock(below)))
      break;
  }
  return 0;
}

std::optional<Pos> calculateBottomLeft(const World &world, Direction rightDir,
                                       Pos pos) {
  const int minY = std::max(world.minY(), pos.y - PortalShape::kMaxHeight);
  while (pos.y > minY &&
         PortalShape::isEmptyBlock(world, world.getBlock(movePos(pos, Direction::Down)))) {
    --pos.y;
  }
  const Direction leftDir = opposite(rightDir);
  const int edge = getDistanceUntilEdgeAboveFrame(world, pos, leftDir) - 1;
  return edge < 0 ? std::nullopt : std::optional<Pos>(movePos(pos, leftDir, edge));
}

int calculateWidth(const World &world, Pos bottomLeft, Direction rightDir) {
  const int width = getDistanceUntilEdgeAboveFrame(world, bottomLeft, rightDir);
  return (width >= PortalShape::kMinWidth && width <= PortalShape::kMaxWidth)
             ? width
             : 0;
}

bool hasTopFrame(const World &world, Pos bottomLeft, Direction rightDir,
                 int width, int height) {
  for (int i = 0; i < width; ++i) {
    const Pos framePos =
        movePos(movePos(bottomLeft, Direction::Up, height), rightDir, i);
    if (!PortalShape::isFrameBlock(world.getBlock(framePos)))
      return false;
  }
  return true;
}

int getDistanceUntilTop(const World &world, Pos bottomLeft, Direction rightDir,
                        int width, int &portalBlockCount) {
  for (int height = 0; height < PortalShape::kMaxHeight; ++height) {
    const Pos leftFrame =
        movePos(movePos(bottomLeft, Direction::Up, height), rightDir, -1);
    if (!PortalShape::isFrameBlock(world.getBlock(leftFrame)))
      return height;

    const Pos rightFrame =
        movePos(movePos(bottomLeft, Direction::Up, height), rightDir, width);
    if (!PortalShape::isFrameBlock(world.getBlock(rightFrame)))
      return height;

    for (int i = 0; i < width; ++i) {
      const Pos interior =
          movePos(movePos(bottomLeft, Direction::Up, height), rightDir, i);
      const BlockState state = world.getBlock(interior);
      if (!PortalShape::isEmptyBlock(world, state))
        return height;
      if (state.is(Blocks::NETHER_PORTAL))
        ++portalBlockCount;
    }
  }
  return PortalShape::kMaxHeight;
}

int calculateHeight(const World &world, Pos bottomLeft, Direction rightDir,
                    int width, int &portalBlockCount) {
  const int height =
      getDistanceUntilTop(world, bottomLeft, rightDir, width, portalBlockCount);
  if (height >= PortalShape::kMinHeight && height <= PortalShape::kMaxHeight &&
      hasTopFrame(world, bottomLeft, rightDir, width, height)) {
    return height;
  }
  return 0;
}

template <class Fn>
void spiralAroundColumns(Pos center, int radius, Direction firstDir,
                         Direction secondDir, Fn &&fn) {
  const std::array<Direction, 4> dirs{
      firstDir, secondDir, opposite(firstDir), opposite(secondDir)};
  Pos cursor = movePos(center, secondDir, 1);
  const int legs = 4 * radius;
  int leg = -1;
  int legSize = 0;
  int legIndex = 0;
  while (true) {
    cursor = movePos(cursor, dirs[(leg + 4) & 3], 1);
    if (legIndex >= legSize) {
      if (leg >= legs)
        break;
      ++leg;
      legIndex = 0;
      legSize = leg / 2 + 1;
    }
    ++legIndex;
    fn(cursor);
  }
}

} // namespace

FoundRectangle getLargestRectangleAround(Pos center, BlockAxis axis1,
                                         int limit1, BlockAxis axis2,
                                         int limit2,
                                         const std::function<bool(Pos)> &test) {
  const Direction neg1 = negativeDir(axis1);
  const Direction pos1 = opposite(neg1);
  const Direction neg2 = negativeDir(axis2);
  const Direction pos2 = opposite(neg2);

  const int negDelta1 = getLimit(test, center, neg1, limit1);
  const int posDelta1 = getLimit(test, center, pos1, limit1);
  const int centerIdx1 = negDelta1;
  std::vector<IntBounds> bounds(static_cast<std::size_t>(negDelta1 + 1 + posDelta1));
  bounds[centerIdx1] = {
      getLimit(test, center, neg2, limit2),
      getLimit(test, center, pos2, limit2),
  };
  const int centerIdx2 = bounds[centerIdx1].min;

  for (int i = 1; i <= negDelta1; ++i) {
    const IntBounds &last = bounds[centerIdx1 - (i - 1)];
    const Pos base = movePos(center, neg1, i);
    bounds[centerIdx1 - i] = {
        getLimit(test, base, neg2, last.min),
        getLimit(test, base, pos2, last.max),
    };
  }
  for (int i = 1; i <= posDelta1; ++i) {
    const IntBounds &last = bounds[centerIdx1 + i - 1];
    const Pos base = movePos(center, pos1, i);
    bounds[centerIdx1 + i] = {
        getLimit(test, base, neg2, last.min),
        getLimit(test, base, pos2, last.max),
    };
  }

  int minAxis1 = 0;
  int minAxis2 = 0;
  int sizeAxis1 = 0;
  int sizeAxis2 = 0;
  std::vector<int> columns(bounds.size(), 0);

  for (int i2 = centerIdx2; i2 >= 0; --i2) {
    for (std::size_t i1 = 0; i1 < bounds.size(); ++i1) {
      const int min2 = centerIdx2 - bounds[i1].min;
      const int max2 = centerIdx2 + bounds[i1].max;
      columns[i1] = (i2 >= min2 && i2 <= max2) ? (max2 + 1 - i2) : 0;
    }
    const auto [bounds1, newSize2] = getMaxRectangleLocation(columns);
    const int newSize1 = 1 + bounds1.max - bounds1.min;
    if (newSize1 * newSize2 > sizeAxis1 * sizeAxis2) {
      minAxis1 = bounds1.min;
      minAxis2 = i2;
      sizeAxis1 = newSize1;
      sizeAxis2 = newSize2;
    }
  }

  Pos minCorner = offsetPos(center, axis1, minAxis1 - centerIdx1);
  minCorner = offsetPos(minCorner, axis2, minAxis2 - centerIdx2);
  return {minCorner, sizeAxis1, sizeAxis2};
}

bool PortalShape::isEmptyBlock(const World &world, const BlockState &state) {
  if (state.isAir() || state.is(Blocks::FIRE) || state.is(Blocks::SOUL_FIRE) ||
      state.is(Blocks::NETHER_PORTAL)) {
    return true;
  }
  if (const auto tagged = world.hasBlockTag(state, "minecraft:fire");
      tagged.has_value() && *tagged) {
    return true;
  }
  return false;
}

bool PortalShape::isFrameBlock(const BlockState &state) {
  return state.is(Blocks::OBSIDIAN);
}

std::optional<PortalShape>
PortalShape::findEmptyPortalShape(const World &world, Pos pos,
                                  PortalAxis preferredAxis) {
  return findPortalShape(
      world, pos,
      [](const PortalShape &s) {
        return s.isValid() && s.numPortalBlocks() == 0;
      },
      preferredAxis);
}

std::optional<PortalShape>
PortalShape::findPortalShape(const World &world, Pos pos,
                             const std::function<bool(const PortalShape &)> &isValidFn,
                             PortalAxis preferredAxis) {
  const PortalShape first = findAnyShape(world, pos, preferredAxis);
  if (isValidFn && isValidFn(first))
    return first;
  const PortalShape second = findAnyShape(world, pos, otherPortalAxis(preferredAxis));
  if (isValidFn && isValidFn(second))
    return second;
  return std::nullopt;
}

PortalShape PortalShape::findAnyShape(const World &world, Pos pos,
                                      PortalAxis axis) {
  const Direction rightDir =
      axis == PortalAxis::X ? Direction::West : Direction::South;
  const std::optional<Pos> bottomLeft = calculateBottomLeft(world, rightDir, pos);
  if (!bottomLeft.has_value())
    return PortalShape(axis, 0, rightDir, pos, 0, 0);
  const int width = calculateWidth(world, *bottomLeft, rightDir);
  if (width == 0)
    return PortalShape(axis, 0, rightDir, *bottomLeft, 0, 0);
  int portalBlockCount = 0;
  const int height =
      calculateHeight(world, *bottomLeft, rightDir, width, portalBlockCount);
  return PortalShape(axis, portalBlockCount, rightDir, *bottomLeft, width, height);
}

void PortalShape::createPortalBlocks(World &world) const {
  if (!isValid())
    return;
  const BlockState portalState = Blocks::NETHER_PORTAL.defaultState().with(
      "axis", axis_ == PortalAxis::X ? "x" : "z");
  for (int h = 0; h < height_; ++h) {
    for (int w = 0; w < width_; ++w) {
      const Pos p = movePos(movePos(bottomLeft_, Direction::Up, h), rightDir_, w);
      world.setBlock(p, portalState, 18);
    }
  }
}

Vec3 PortalShape::getRelativePosition(const FoundRectangle &rect,
                                      PortalAxis axis, Vec3 position,
                                      double entityWidth,
                                      double entityHeight) {
  const double width = static_cast<double>(rect.axis1Size) - entityWidth;
  const double height = static_cast<double>(rect.axis2Size) - entityHeight;
  double relativeRight = 0.5;
  if (width > 0.0) {
    const double bottomStart =
        static_cast<double>(blockCoordForAxis(rect.minCorner, axis)) +
        entityWidth / 2.0;
    relativeRight = std::clamp(
        inverseLerp(coordForAxis(position, axis) - bottomStart, 0.0, width), 0.0,
        1.0);
  }
  double relativeUp = 0.0;
  if (height > 0.0) {
    relativeUp = std::clamp(
        inverseLerp(position.y - static_cast<double>(rect.minCorner.y), 0.0,
                    height),
        0.0, 1.0);
  }
  const PortalAxis forwardAxis = otherPortalAxis(axis);
  const double relativeForward =
      coordForAxis(position, forwardAxis) -
      (static_cast<double>(blockCoordForAxis(rect.minCorner, forwardAxis)) +
       0.5);
  return {relativeRight, relativeUp, relativeForward};
}

Vec3 PortalShape::findCollisionFreePosition(Vec3 bottomCenter, World &world,
                                            double entityWidth,
                                            double entityHeight) {
  if (entityWidth > 4.0 || entityHeight > 4.0)
    return bottomCenter;

  const double halfW = entityWidth / 2.0;
  const double halfH = entityHeight / 2.0;
  const Vec3 center{bottomCenter.x, bottomCenter.y + halfH, bottomCenter.z};
  const Aabb allowedAabb =
      Aabb{center.x - halfW, center.y, center.z - halfW,
           center.x + halfW, center.y + 1.0, center.z + halfW}
          .inflate(1.0e-6);
  std::vector<Aabb> freeBoxes{allowedAabb};

  const Aabb searchArea =
      allowedAabb.inflate(entityWidth, entityHeight, entityWidth);
  std::vector<ShapeBox> colliders;
  world.collectCollisionBoxes(searchArea, colliders);
  for (const ShapeBox &box : colliders) {
    subtractBox(freeBoxes, {box.x0 - halfW, box.y0 - halfH, box.z0 - halfW,
                            box.x1 + halfW, box.y1 + halfH, box.z1 + halfW});
    if (freeBoxes.empty())
      return bottomCenter;
  }

  double bestDistSq = std::numeric_limits<double>::infinity();
  Vec3 bestCenter = center;
  bool found = false;
  for (const Aabb &b : freeBoxes) {
    const double cx = std::clamp(center.x, b.minX, b.maxX);
    const double cy = std::clamp(center.y, b.minY, b.maxY);
    const double cz = std::clamp(center.z, b.minZ, b.maxZ);
    const double dx = cx - center.x;
    const double dy = cy - center.y;
    const double dz = cz - center.z;
    const double distSq = dx * dx + dy * dy + dz * dz;
    if (!found || distSq < bestDistSq) {
      bestDistSq = distSq;
      bestCenter = {cx, cy, cz};
      found = true;
    }
  }
  if (!found)
    return bottomCenter;
  return {bestCenter.x, bestCenter.y - halfH, bestCenter.z};
}

bool WorldBorderBounds::isWithinBounds(Pos pos) const noexcept {
  return static_cast<double>(pos.x) >= minX &&
         static_cast<double>(pos.x) < maxX &&
         static_cast<double>(pos.z) >= minZ &&
         static_cast<double>(pos.z) < maxZ;
}

bool WorldBorderBounds::isWithinBounds(const Aabb &box) const noexcept {
  return box.maxX > minX && box.minX < maxX && box.maxZ > minZ &&
         box.minZ < maxZ;
}

Pos WorldBorderBounds::clampToBounds(double x, double y, double z) const noexcept {
  const double cx = std::clamp(x, minX, maxX - 1.0e-5);
  const double cz = std::clamp(z, minZ, maxZ - 1.0e-5);
  return {static_cast<int>(std::floor(cx)), static_cast<int>(std::floor(y)),
          static_cast<int>(std::floor(cz))};
}

Pos WorldBorderBounds::clampToBounds(Pos pos) const noexcept {
  return clampToBounds(static_cast<double>(pos.x), static_cast<double>(pos.y),
                       static_cast<double>(pos.z));
}

bool PortalForcer::isChunkColumnLoaded(int x, int z) const noexcept {
  return world_.isChunkLoaded(x >> 4, z >> 4);
}

bool PortalForcer::canPortalReplaceBlock(Pos pos) const {
  if (pos.y < world_.minY() || pos.y >= world_.maxY())
    return false;
  if (!isChunkColumnLoaded(pos.x, pos.z))
    return false;
  const BlockState state = world_.getBlock(pos);
  return state.block().metadata().replaceable && fluidStateOf(state).isEmpty();
}

bool PortalForcer::canHostFrame(Pos origin, Direction direction,
                                int offset) const {
  const Direction cw = clockWise(direction);
  for (int width = -1; width < 3; ++width) {
    for (int height = -1; height < 4; ++height) {
      const Pos p{
          origin.x + stepX(direction) * width + stepX(cw) * offset,
          origin.y + height,
          origin.z + stepZ(direction) * width + stepZ(cw) * offset,
      };
      if (p.y < world_.minY() || p.y >= world_.maxY() ||
          !isChunkColumnLoaded(p.x, p.z)) {
        return false;
      }
      if (height < 0 && !blocksMotion(world_.getBlock(p)))
        return false;
      if (height >= 0 && !canPortalReplaceBlock(p))
        return false;
    }
  }
  return true;
}

int PortalForcer::surfaceMotionBlockingY(int x, int z) const {
  if (!isChunkColumnLoaded(x, z))
    return world_.minY();
  const Heightmap *map =
      world_.heightmap(x >> 4, z >> 4, HeightmapType::MotionBlocking);
  if (map)
    return map->firstAvailable(x & 15, z & 15);
  for (int y = world_.maxY() - 1; y >= world_.minY(); --y) {
    const BlockState state = world_.getBlock({x, y, z});
    if (blocksMotion(state) || !fluidStateOf(state).isEmpty())
      return y + 1;
  }
  return world_.minY();
}

std::optional<Pos> PortalForcer::findClosestPortalPosition(
    Pos approximateExitPos, bool toNether,
    const WorldBorderBounds &border) const {
  const int radius = toNether ? kNetherPortalRadius : kOverworldPortalRadius;
  const PoiPos center{approximateExitPos.x, approximateExitPos.y,
                      approximateExitPos.z};
  const std::optional<PoiPos> match = world_.pois().findClosestPortal(
      center, radius, [&](const PoiMatch &poiMatch) {
        const Pos p{poiMatch.pos.x, poiMatch.pos.y, poiMatch.pos.z};
        if (!border.isWithinBounds(p))
          return false;
        if (!isChunkColumnLoaded(p.x, p.z))
          return false;
        return world_.getBlock(p).hasProperty("axis");
      });
  if (!match.has_value())
    return std::nullopt;
  return Pos{match->x, match->y, match->z};
}

std::optional<FoundRectangle> PortalForcer::createPortal(
    Pos origin, PortalAxis portalAxis, const WorldBorderBounds &border) {
  // TODO: world generation (PLAN G-1..G-4) is not implemented yet. When the
  // destination area around `origin` is ungenerated / unloaded, refuse to
  // carve or construct an exit portal and return std::nullopt instead.
  if (!isChunkColumnLoaded(origin.x, origin.z))
    return std::nullopt;

  const Direction direction = positiveHorizontal(portalAxis);
  double closestFullDistSqr = -1.0;
  Pos closestFullPos{};
  double closestPartialDistSqr = -1.0;
  Pos closestPartialPos{};

  const int maxPlaceableY = std::min(
      world_.maxY(),
      world_.minY() + world_.dimension().logicalHeight - kMinPortalYOffsetFromMax);

  auto distSqrToOrigin = [&](Pos p) -> double {
    const double dx = static_cast<double>(p.x - origin.x);
    const double dy = static_cast<double>(p.y - origin.y);
    const double dz = static_cast<double>(p.z - origin.z);
    return dx * dx + dy * dy + dz * dz;
  };

  spiralAroundColumns(
      origin, kNetherPortalRadius, Direction::East, Direction::South,
      [&](Pos column) {
        if (!isChunkColumnLoaded(column.x, column.z))
          return;
        const int height =
            std::min(maxPlaceableY, surfaceMotionBlockingY(column.x, column.z));
        const Pos forwardColumn = movePos(column, direction, 1);
        if (!border.isWithinBounds(column) ||
            !border.isWithinBounds(forwardColumn)) {
          return;
        }
        for (int y = height; y >= world_.minY(); --y) {
          Pos candidate{column.x, y, column.z};
          if (!canPortalReplaceBlock(candidate))
            continue;
          const int firstEmptyY = y;
          while (y > world_.minY() &&
                 canPortalReplaceBlock({column.x, y - 1, column.z})) {
            --y;
          }
          if (y + 4 > maxPlaceableY)
            continue;
          const int deltaY = firstEmptyY - y;
          if (deltaY > 0 && deltaY < 3)
            continue;
          candidate.y = y;
          if (!canHostFrame(candidate, direction, 0))
            continue;
          const double distance = distSqrToOrigin(candidate);
          if (canHostFrame(candidate, direction, -1) &&
              canHostFrame(candidate, direction, 1) &&
              (closestFullDistSqr == -1.0 || closestFullDistSqr > distance)) {
            closestFullDistSqr = distance;
            closestFullPos = candidate;
          }
          if (closestFullDistSqr == -1.0 &&
              (closestPartialDistSqr == -1.0 ||
               closestPartialDistSqr > distance)) {
            closestPartialDistSqr = distance;
            closestPartialPos = candidate;
          }
        }
      });

  if (closestFullDistSqr == -1.0 && closestPartialDistSqr != -1.0) {
    closestFullPos = closestPartialPos;
    closestFullDistSqr = closestPartialDistSqr;
  }

  bool needsFallbackPlatform = false;
  if (closestFullDistSqr == -1.0) {
    const int minStartY = std::max(world_.minY() + 1, 70);
    const int maxStartY = maxPlaceableY - 9;
    if (maxStartY < minStartY)
      return std::nullopt;

    closestFullPos = border.clampToBounds({
        origin.x - stepX(direction),
        std::clamp(origin.y, minStartY, maxStartY),
        origin.z - stepZ(direction),
    });
    needsFallbackPlatform = true;
  }

  // Verify the complete frame / fallback footprint lies within loaded chunks so
  // we never silently drop portal or obsidian blocks across an ungenerated edge.
  const Direction cw = clockWise(direction);
  if (needsFallbackPlatform) {
    for (int box = -1; box < 2; ++box) {
      for (int width = 0; width < 2; ++width) {
        for (int h = -1; h < 3; ++h) {
          const Pos target{
              closestFullPos.x + width * stepX(direction) + box * stepX(cw),
              closestFullPos.y + h,
              closestFullPos.z + width * stepZ(direction) + box * stepZ(cw),
          };
          if (target.y < world_.minY() || target.y >= world_.maxY() ||
              !isChunkColumnLoaded(target.x, target.z)) {
            // TODO: ungenerated/unloaded chunk in fallback footprint; refusing
            // partial portal/platform without world generation.
            return std::nullopt;
          }
        }
      }
    }
  }
  for (int width = -1; width < 3; ++width) {
    for (int h = -1; h < 4; ++h) {
      const Pos target{
          closestFullPos.x + width * stepX(direction),
          closestFullPos.y + h,
          closestFullPos.z + width * stepZ(direction),
      };
      if (target.y < world_.minY() || target.y >= world_.maxY() ||
          !isChunkColumnLoaded(target.x, target.z)) {
        return std::nullopt;
      }
    }
  }

  if (needsFallbackPlatform) {
    for (int box = -1; box < 2; ++box) {
      for (int width = 0; width < 2; ++width) {
        for (int h = -1; h < 3; ++h) {
          const Pos target{
              closestFullPos.x + width * stepX(direction) + box * stepX(cw),
              closestFullPos.y + h,
              closestFullPos.z + width * stepZ(direction) + box * stepZ(cw),
          };
          const BlockState fill =
              h < 0 ? Blocks::OBSIDIAN.defaultState() : Blocks::AIR.defaultState();
          world_.setBlock(target, fill, 3);
        }
      }
    }
  }

  const BlockState obsidian = Blocks::OBSIDIAN.defaultState();
  for (int width = -1; width < 3; ++width) {
    for (int h = -1; h < 4; ++h) {
      if (width == -1 || width == 2 || h == -1 || h == 3) {
        const Pos target{
            closestFullPos.x + width * stepX(direction),
            closestFullPos.y + h,
            closestFullPos.z + width * stepZ(direction),
        };
        world_.setBlock(target, obsidian, 3);
      }
    }
  }

  const BlockState portalState = Blocks::NETHER_PORTAL.defaultState().with(
      "axis", portalAxis == PortalAxis::X ? "x" : "z");
  for (int width = 0; width < 2; ++width) {
    for (int h = 0; h < 3; ++h) {
      const Pos target{
          closestFullPos.x + width * stepX(direction),
          closestFullPos.y + h,
          closestFullPos.z + width * stepZ(direction),
      };
      world_.setBlock(target, portalState, 18);
    }
  }

  return FoundRectangle{closestFullPos, 2, 3};
}

bool isInPortalDimension(const World &world) noexcept {
  return world.dimensionId() == "minecraft:overworld" ||
         world.dimensionId() == "minecraft:the_nether";
}

std::optional<PortalTransitionResult> computeNetherPortalTransition(
    World &sourceWorld, const Entity &entity, Pos portalEntryPos,
    const std::function<World *(std::string_view)> &findWorld,
    const WorldBorderBounds &targetBorder) {
  if (!isInPortalDimension(sourceWorld) || !findWorld) {
    // TODO: End portals / End gateways / custom dimension portal destinations
    // remain unimplemented.
    return std::nullopt;
  }

  const std::string targetDimensionId =
      sourceWorld.dimensionId() == "minecraft:the_nether"
          ? "minecraft:overworld"
          : "minecraft:the_nether";
  World *targetWorld = findWorld(targetDimensionId);
  if (!targetWorld) {
    // TODO: target dimension is missing/unloaded; do not create a portal or
    // teleport the entity.
    return std::nullopt;
  }

  const bool toNether = targetDimensionId == "minecraft:the_nether";
  const double scale =
      teleportationScale(sourceWorld.dimension(), targetWorld->dimension());
  const Pos approximateExitPos = targetBorder.clampToBounds(
      entity.position.x * scale, entity.position.y, entity.position.z * scale);

  PortalForcer forcer(*targetWorld);
  std::optional<Pos> existingPortalPos =
      forcer.findClosestPortalPosition(approximateExitPos, toNether, targetBorder);

  FoundRectangle exitPortal{};
  bool createdPortal = false;
  Pos exitAnchorPos{};
  if (existingPortalPos.has_value()) {
    exitAnchorPos = *existingPortalPos;
    const BlockState exitState = targetWorld->getBlock(exitAnchorPos);
    exitPortal = getLargestRectangleAround(
        exitAnchorPos,
        portalAxisOfState(exitState) == PortalAxis::X ? BlockAxis::X : BlockAxis::Z,
        PortalShape::kMaxWidth, BlockAxis::Y, PortalShape::kMaxHeight,
        [&](Pos p) { return targetWorld->getBlock(p) == exitState; });
  } else {
    const BlockState entryState = sourceWorld.getBlock(portalEntryPos);
    const PortalAxis sourceAxis = entryState.hasProperty("axis")
                                      ? portalAxisOfState(entryState)
                                      : PortalAxis::X;
    const std::optional<FoundRectangle> created =
        forcer.createPortal(approximateExitPos, sourceAxis, targetBorder);
    if (!created.has_value()) {
      // TODO: destination portal area is ungenerated/unloaded (or out of
      // border); keep entity in source dimension without creating a portal.
      return std::nullopt;
    }
    exitPortal = *created;
    exitAnchorPos = exitPortal.minCorner;
    createdPortal = true;
  }

  const BlockState entryState = sourceWorld.getBlock(portalEntryPos);
  PortalAxis sourceAxis = PortalAxis::X;
  Vec3 relOffset{0.5, 0.0, 0.0};
  const double entityWidth = entity.type().width;
  const double entityHeight = entity.type().height;
  if (entryState.hasProperty("axis")) {
    sourceAxis = portalAxisOfState(entryState);
    const FoundRectangle sourceRect = getLargestRectangleAround(
        portalEntryPos, sourceAxis == PortalAxis::X ? BlockAxis::X : BlockAxis::Z,
        PortalShape::kMaxWidth, BlockAxis::Y, PortalShape::kMaxHeight,
        [&](Pos p) { return sourceWorld.getBlock(p) == entryState; });
    relOffset = PortalShape::getRelativePosition(
        sourceRect, sourceAxis, entity.position, entityWidth, entityHeight);
  }

  const BlockState targetAnchorState = targetWorld->getBlock(exitPortal.minCorner);
  const PortalAxis targetAxis = targetAnchorState.hasProperty("axis")
                                    ? portalAxisOfState(targetAnchorState)
                                    : PortalAxis::X;
  const float yawDelta = sourceAxis == targetAxis ? 0.0f : 90.0f;
  const double offsetRight =
      entityWidth / 2.0 +
      (static_cast<double>(exitPortal.axis1Size) - entityWidth) * relOffset.x;
  const double offsetUp =
      (static_cast<double>(exitPortal.axis2Size) - entityHeight) * relOffset.y;
  const double offsetForward = 0.5 + relOffset.z;
  const bool xAligned = targetAxis == PortalAxis::X;
  const Vec3 targetPos{
      static_cast<double>(exitPortal.minCorner.x) +
          (xAligned ? offsetRight : offsetForward),
      static_cast<double>(exitPortal.minCorner.y) + offsetUp,
      static_cast<double>(exitPortal.minCorner.z) +
          (xAligned ? offsetForward : offsetRight),
  };
  const Vec3 freePos = PortalShape::findCollisionFreePosition(
      targetPos, *targetWorld, entityWidth, entityHeight);

  return PortalTransitionResult{
      targetDimensionId,
      freePos,
      Vec3{0.0, 0.0, 0.0},
      yawDelta,
      exitAnchorPos,
      createdPortal,
  };
}

bool teleportEntityThroughPortal(
    World &sourceWorld, const std::shared_ptr<Entity> &entity,
    Pos portalEntryPos,
    const std::function<World *(std::string_view)> &findWorld,
    const std::function<WorldChunkManager *(std::string_view)> &findChunkManager,
    const WorldBorderBounds &targetBorder) {
  if (!entity || entity->isRemoved())
    return false;
  const std::optional<PortalTransitionResult> transition =
      computeNetherPortalTransition(sourceWorld, *entity, portalEntryPos,
                                    findWorld, targetBorder);
  if (!transition.has_value())
    return false;
  World *targetWorld = findWorld(transition->targetDimensionId);
  if (!targetWorld)
    return false;

  if (!sourceWorld.detachEntityForDimensionTransfer(entity))
    return false;

  entity->setPos(transition->position);
  entity->deltaMovement = transition->deltaMovement;
  entity->yaw = std::fmod(entity->yaw + transition->yawDelta, 360.0f);
  entity->pitch = 0.0f;
  entity->setOldPosAndRot();
  entity->portalCooldown = entity->dimensionChangingDelay();
  entity->clearPortalProcess();

  targetWorld->addEntity(entity);

  if (findChunkManager) {
    if (WorldChunkManager *chunkManager =
            findChunkManager(transition->targetDimensionId)) {
      const ChunkTicketPos ticketPos{
          static_cast<int>(std::floor(transition->position.x)) >> 4,
          static_cast<int>(std::floor(transition->position.z)) >> 4,
      };
      chunkManager->addPortalTicket(ticketPos, PortalForcer::kTicketRadius);
    }
  }
  return true;
}

std::size_t processPendingPortalTransfers(ServerWorldSet &worlds,
                                          const WorldBorderBounds &targetBorder) {
  std::size_t transferred = 0;
  auto findWorld = [&worlds](std::string_view id) -> World * {
    auto *level = worlds.find(id);
    return level && level->world ? level->world.get() : nullptr;
  };
  auto findChunkManager = [&worlds](std::string_view id) -> WorldChunkManager * {
    auto *level = worlds.find(id);
    return level ? level->chunkManager.get() : nullptr;
  };
  for (const auto &level : worlds.levels()) {
    if (!level || !level->world)
      continue;
    auto pending = level->world->takePendingPortalTransfers();
    for (const auto &req : pending) {
      if (teleportEntityThroughPortal(*level->world, req.entity,
                                      req.portalEntryPos, findWorld,
                                      findChunkManager, targetBorder)) {
        ++transferred;
      }
    }
  }
  return transferred;
}

} // namespace redstone
