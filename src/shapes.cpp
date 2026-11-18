#include "redstone/shapes.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
namespace redstone {
#include "block_shapes.inc"
namespace {
constexpr std::size_t kRandomTickFlagCount = std::size(kStateBasicInfo);
consteval std::array<std::uint8_t, kRandomTickFlagCount> makeRandomTickFlags() {
  std::array<std::uint8_t, kRandomTickFlagCount> flags{};
  for (std::size_t i = 0; i < kRandomTickFlagCount; ++i)
    flags[i] = static_cast<std::uint8_t>(kStateBasicInfo[i].flags & 1u);
  return flags;
}
inline constexpr auto kRandomTickFlags = makeRandomTickFlags();
consteval std::array<std::uint8_t, kRandomTickFlagCount>
makeCollisionKindFlags() {
  std::array<std::uint8_t, kRandomTickFlagCount> flags{};
  for (std::size_t i = 0; i < kRandomTickFlagCount; ++i) {
    const ShapeSpan span = kBlockShapeSpans[kStateBasicInfo[i].collisionShape];
    if (span.count == 0u) {
      flags[i] = static_cast<std::uint8_t>(BlockCollisionKind::Empty);
      continue;
    }
    bool oversized = false;
    for (std::uint16_t j = 0; j < span.count; ++j) {
      const ShapeBox &b = kBlockShapeBoxes[span.offset + j];
      if (b.x0 < 0.0 || b.y0 < 0.0 || b.z0 < 0.0 || b.x1 > 16.0 ||
          b.y1 > 16.0 || b.z1 > 16.0) {
        oversized = true;
        break;
      }
    }
    if (oversized) {
      flags[i] = static_cast<std::uint8_t>(BlockCollisionKind::Oversized);
      continue;
    }
    if (span.count == 1u) {
      const ShapeBox &b = kBlockShapeBoxes[span.offset];
      if (b.x0 <= 0.0 && b.y0 <= 0.0 && b.z0 <= 0.0 && b.x1 >= 16.0 &&
          b.y1 >= 16.0 && b.z1 >= 16.0) {
        flags[i] = static_cast<std::uint8_t>(BlockCollisionKind::FullCube);
        continue;
      }
    }
    flags[i] = static_cast<std::uint8_t>(BlockCollisionKind::Partial);
  }
  return flags;
}
inline constexpr auto kCollisionKindFlags = makeCollisionKindFlags();
} // namespace
namespace detail {
const std::uint8_t *const gRandomTickStateFlags = kRandomTickFlags.data();
const std::uint32_t gRandomTickStateCount =
    static_cast<std::uint32_t>(kRandomTickFlags.size());
const std::uint8_t *const gCollisionKindFlags = kCollisionKindFlags.data();
const std::uint32_t gCollisionKindCount =
    static_cast<std::uint32_t>(kCollisionKindFlags.size());
} // namespace detail
bool stateIsRandomlyTicking(const BlockState &state) {
  return state.isSchemaState() && state.id() < std::size(kStateBasicInfo) &&
         (kStateBasicInfo[state.id()].flags & 1u) != 0;
}
bool stateBlocksMotion(const BlockState &state) {
  return state.isSchemaState() && state.id() < std::size(kStateBasicInfo) &&
         (kStateBasicInfo[state.id()].flags & 2u) != 0;
}
bool stateIsRedstoneConductor(const BlockState &state) {
  return state.isSchemaState() && state.id() < std::size(kStateBasicInfo) &&
         (kStateBasicInfo[state.id()].flags & 4u) != 0;
}
bool blocksMotion(const BlockState &state) {
  if (blockMetadata(state).forceSolid || state.block().isFenceGate())
    return true;
  // TODO: forceSolidOff has no registered user yet.
  const VoxelShape &shape = collisionShape(state);
  if (shape.empty())
    return false;
  double x0 = 16.0, y0 = 16.0, z0 = 16.0, x1 = 0.0, y1 = 0.0, z1 = 0.0;
  for (const ShapeBox &box : shape) {
    x0 = std::min(x0, box.x0);
    y0 = std::min(y0, box.y0);
    z0 = std::min(z0, box.z0);
    x1 = std::max(x1, box.x1);
    y1 = std::max(y1, box.y1);
    z1 = std::max(z1, box.z1);
  }
  const double xs = (x1 - x0) / 16.0;
  const double ys = (y1 - y0) / 16.0;
  const double zs = (z1 - z0) / 16.0;
  return (xs + ys + zs) / 3.0 >= 0.7291666666666666 || ys >= 1.0;
}
namespace {
bool wall(const BlockState &s) { return isWallBlock(s); }
ShapeBox orient(ShapeBox b, Direction d) {
  // Rotate north-facing shape around block center.
  switch (d) {
  case Direction::North:
    return b;
  case Direction::South:
    return {16 - b.x1, b.y0, 16 - b.z1, 16 - b.x0, b.y1, 16 - b.z0};
  case Direction::East:
    return {16 - b.z1, b.y0, b.x0, 16 - b.z0, b.y1, b.x1};
  case Direction::West:
    return {b.z0, b.y0, 16 - b.x1, b.z1, b.y1, 16 - b.x0};
  case Direction::Up:
    return {b.x0, 16 - b.z1, b.y0, b.x1, 16 - b.z0, b.y1};
  case Direction::Down:
    return {b.x0, b.z0, 16 - b.y1, b.x1, b.z1, 16 - b.y0};
  }
  return b;
}
struct Rect {
  double a, b, c, d;
};
std::vector<Rect> face(const VoxelShape &shape, Direction d) {
  std::vector<Rect> r;
  for (auto b : shape) {
    switch (d) {
    case Direction::Down:
      if (b.y0 <= 0 && b.y1 > 0)
        r.push_back({b.x0, b.z0, b.x1, b.z1});
      break;
    case Direction::Up:
      if (b.y0 < 16 && b.y1 >= 16)
        r.push_back({b.x0, b.z0, b.x1, b.z1});
      break;
    case Direction::North:
      if (b.z0 <= 0 && b.z1 > 0)
        r.push_back({b.x0, b.y0, b.x1, b.y1});
      break;
    case Direction::South:
      if (b.z0 < 16 && b.z1 >= 16)
        r.push_back({b.x0, b.y0, b.x1, b.y1});
      break;
    case Direction::West:
      if (b.x0 <= 0 && b.x1 > 0)
        r.push_back({b.z0, b.y0, b.z1, b.y1});
      break;
    case Direction::East:
      if (b.x0 < 16 && b.x1 >= 16)
        r.push_back({b.z0, b.y0, b.z1, b.y1});
      break;
    }
  }
  return r;
}
bool covers(const std::vector<Rect> &rs, Rect target) {
  if (rs.empty())
    return false;
  if (rs.size() == 1) {
    const auto &r = rs.front();
    return r.a <= target.a && r.b <= target.b && r.c >= target.c &&
           r.d >= target.d;
  }
  std::vector<double> xs{target.a, target.c}, ys{target.b, target.d};
  for (auto r : rs) {
    xs.push_back(std::clamp(r.a, target.a, target.c));
    xs.push_back(std::clamp(r.c, target.a, target.c));
    ys.push_back(std::clamp(r.b, target.b, target.d));
    ys.push_back(std::clamp(r.d, target.b, target.d));
  }
  std::sort(xs.begin(), xs.end());
  std::sort(ys.begin(), ys.end());
  for (size_t i = 1; i < xs.size(); ++i)
    for (size_t j = 1; j < ys.size(); ++j) {
      if (xs[i] == xs[i - 1] || ys[j] == ys[j - 1])
        continue;
      double x = (xs[i] + xs[i - 1]) / 2, y = (ys[j] + ys[j - 1]) / 2;
      bool hit = false;
      for (auto r : rs)
        if (x >= r.a && x <= r.c && y >= r.b && y <= r.d) {
          hit = true;
          break;
        }
      if (!hit)
        return false;
    }
  return true;
}
bool sturdyShape(const VoxelShape &shape, Direction d, SupportType type) {
  if (shape.empty())
    return false;
  if (type == SupportType::Full) {
    if (shape.size() != 1)
      return covers(face(shape, d), {0, 0, 16, 16});
    const auto &b = shape.front();
    switch (d) {
    case Direction::Down:
      return b.y0 <= 0 && b.y1 > 0 && b.x0 <= 0 && b.z0 <= 0 &&
             b.x1 >= 16 && b.z1 >= 16;
    case Direction::Up:
      return b.y0 < 16 && b.y1 >= 16 && b.x0 <= 0 && b.z0 <= 0 &&
             b.x1 >= 16 && b.z1 >= 16;
    case Direction::North:
      return b.z0 <= 0 && b.z1 > 0 && b.x0 <= 0 && b.y0 <= 0 &&
             b.x1 >= 16 && b.y1 >= 16;
    case Direction::South:
      return b.z0 < 16 && b.z1 >= 16 && b.x0 <= 0 && b.y0 <= 0 &&
             b.x1 >= 16 && b.y1 >= 16;
    case Direction::West:
      return b.x0 <= 0 && b.x1 > 0 && b.z0 <= 0 && b.y0 <= 0 &&
             b.z1 >= 16 && b.y1 >= 16;
    case Direction::East:
      return b.x0 < 16 && b.x1 >= 16 && b.z0 <= 0 && b.y0 <= 0 &&
             b.z1 >= 16 && b.y1 >= 16;
    }
    return false;
  }
  const auto rects = face(shape, d);
  if (type == SupportType::Center)
    return covers(rects, {7, 7, 9, 9});
  return covers(rects, {0, 0, 16, 2}) && covers(rects, {0, 14, 16, 16}) &&
         covers(rects, {0, 2, 2, 14}) && covers(rects, {14, 2, 16, 14});
}
const std::array<Direction, 4> dirs{Direction::North, Direction::East,
                                    Direction::South, Direction::West};
Rect wallTest(Direction d) {
  switch (d) {
  case Direction::North:
    return {7, 0, 9, 9};
  case Direction::South:
    return {7, 7, 9, 16};
  case Direction::West:
    return {0, 7, 9, 9};
  default:
    return {7, 7, 16, 9};
  }
}
} // namespace

VoxelShape spanToShape(std::uint16_t spanIndex) {
  const ShapeSpan span = kBlockShapeSpans[spanIndex];
  VoxelShape out;
  out.reserve(span.count);
  for (std::uint16_t i = 0; i < span.count; ++i)
    out.push_back(kBlockShapeBoxes[span.offset + i]);
  return out;
}

// 碰撞形状按 ShapeSpan 去重：所有 schema 状态共享同一组不可变 VoxelShape，
// 首次访问时一次性建表，之后只读，避免多线程 lazy 构造争用全局容器。
namespace {
std::vector<VoxelShape> buildCollisionSpanShapes() {
  std::vector<VoxelShape> shapes(std::size(kBlockShapeSpans));
  for (std::size_t i = 0; i < shapes.size(); ++i)
    shapes[i] = spanToShape(static_cast<std::uint16_t>(i));
  return shapes;
}
const std::vector<VoxelShape> &collisionSpanShapes() {
  static const std::vector<VoxelShape> shapes = buildCollisionSpanShapes();
  return shapes;
}
const VoxelShape &fullCubeShape() {
  static const VoxelShape shape{ShapeBox{0.0, 0.0, 0.0, 16.0, 16.0, 16.0}};
  return shape;
}
} // namespace

VoxelShape computeStateShape(const BlockState &s) {
  if (s.isSchemaState() && s.id() < std::size(kStateBasicInfo))
    return spanToShape(kStateBasicInfo[s.id()].collisionShape);
  return {{0, 0, 0, 16, 16, 16}};
}
VoxelShape outlineShape(const BlockState &s) {
  if (s.isSchemaState() && s.id() < std::size(kStateBasicInfo))
    return spanToShape(kStateBasicInfo[s.id()].outlineShape);
  return collisionShape(s);
}
const VoxelShape &collisionShape(const BlockState &s) {
  const auto id = static_cast<std::size_t>(s.id());
  if (s.isSchemaState() && id < std::size(kStateBasicInfo)) {
    const std::uint16_t span = kStateBasicInfo[id].collisionShape;
    const auto &shapes = collisionSpanShapes();
    if (span < shapes.size())
      return shapes[span];
  }
  return fullCubeShape();
}
VoxelShape collisionShape(World &w, Pos p, const BlockState &s) {
  if (!s.is(Blocks::MOVING_PISTON))
    return collisionShape(s);
  auto it = w.pistonEntities.find(p);
  if (it == w.pistonEntities.end())
    return {};
  auto e = it->second;
  VoxelShape result;
  if (w.pistonNoClip && e->progress < 1.0f &&
      e->movementDirection() == *w.pistonNoClip) {
    // PistonMovingBlockEntity.getCollisionShape's NOCLIP branch: a piston base
    // being retracted still exposes its extended head shape, while moved blocks
    // expose nothing so the pushed entity can travel through the moving space.
    if (!e->extending && e->source &&
        (e->movedState.is(Blocks::PISTON) ||
         e->movedState.is(Blocks::STICKY_PISTON)))
      result = collisionShape(e->movedState.with("extended", "true"));
    return result;
  }
  if (!e->extending && e->source &&
      (e->movedState.is(Blocks::PISTON) || e->movedState.is(Blocks::STICKY_PISTON)))
    result = collisionShape(e->movedState.with("extended", "true"));
  auto moved =
      e->source
          ? defaultState("piston_head")
                .with("facing", directionName(e->direction))
                .with("short", (e->extending != (1.0f - e->progress < 0.25f))
                                   ? "true"
                                   : "false")
          : e->movedState;
  const auto &parts = collisionShape(moved);
  result.reserve(result.size() + parts.size());
  double offset = (e->extending ? e->progress - 1.0 : 1.0 - e->progress) * 16;
  auto next = p.relative(e->direction);
  double dx = (next.x - p.x) * offset, dy = (next.y - p.y) * offset,
         dz = (next.z - p.z) * offset;
  for (auto b : parts)
    result.push_back(
        {b.x0 + dx, b.y0 + dy, b.z0 + dz, b.x1 + dx, b.y1 + dy, b.z1 + dz});
  return result;
}
bool faceSturdy(const BlockState &s, Direction d, SupportType type) {
  return sturdyShape(collisionShape(s), d, type);
}
bool faceSturdy(World &w, Pos p, const BlockState &s, Direction d,
                SupportType type) {
  if (!s.is(Blocks::MOVING_PISTON))
    return faceSturdy(s, d, type);
  return sturdyShape(collisionShape(w, p, s), d, type);
}
bool coralCanSurvive(World &w, Pos p, const BlockState &state) {
  const auto facing = isDeadCoralWallFan(state)
                          ? directionFromString(state.getView("facing", "north"))
                          : Direction::Up;
  const auto support = p.relative(opposite(facing));
  return faceSturdy(w, support, w.getBlock(support), facing);
}
namespace {
bool isHorizontalDir(Direction d) {
  return d == Direction::North || d == Direction::South ||
         d == Direction::East || d == Direction::West;
}

int horizontalAxis(Direction d) {
  return (d == Direction::East || d == Direction::West) ? 0
         : (d == Direction::North || d == Direction::South) ? 2
                                                            : 1;
}

Direction counterClockWise(Direction d) {
  switch (d) {
  case Direction::North:
    return Direction::West;
  case Direction::West:
    return Direction::South;
  case Direction::South:
    return Direction::East;
  case Direction::East:
    return Direction::North;
  default:
    return d;
  }
}

BlockState computeStairShape(World &w, Pos p, const BlockState &s,
                             Direction *overrideDir,
                             const BlockState *overrideState) {
  const auto neighborAt = [&](Direction d) -> BlockState {
    if (overrideDir && overrideState && d == *overrideDir)
      return *overrideState;
    return w.getBlock(p.relative(d));
  };
  const Direction facing = directionFromString(s.getView("facing", "north"));
  const auto half = s.getView("half", "bottom");
  const auto canTakeShape = [&](Direction neighbourDir) {
    const BlockState ns = neighborAt(neighbourDir);
    return !ns.block().isStairs() ||
           directionFromString(ns.getView("facing", "north")) != facing ||
           ns.getView("half", "bottom") != half;
  };
  const BlockState behindState = neighborAt(facing);
  if (behindState.block().isStairs() &&
      half == behindState.getView("half", "bottom")) {
    const Direction behindFacing =
        directionFromString(behindState.getView("facing", "north"));
    if (horizontalAxis(behindFacing) != horizontalAxis(facing) &&
        canTakeShape(opposite(behindFacing))) {
      return s.with("shape", behindFacing == counterClockWise(facing)
                                 ? "outer_left"
                                 : "outer_right");
    }
  }
  const BlockState frontState = neighborAt(opposite(facing));
  if (frontState.block().isStairs() &&
      half == frontState.getView("half", "bottom")) {
    const Direction frontFacing =
        directionFromString(frontState.getView("facing", "north"));
    if (horizontalAxis(frontFacing) != horizontalAxis(facing) &&
        canTakeShape(frontFacing)) {
      return s.with("shape", frontFacing == counterClockWise(facing)
                                 ? "inner_left"
                                 : "inner_right");
    }
  }
  return s.with("shape", "straight");
}
} // namespace

bool isIronBarsBlock(const BlockState &s) {
  const Block *b = &s.block();
  return b == &Blocks::IRON_BARS || b == &Blocks::GLASS_PANE ||
         b == &Blocks::WHITE_STAINED_GLASS_PANE ||
         b == &Blocks::ORANGE_STAINED_GLASS_PANE ||
         b == &Blocks::MAGENTA_STAINED_GLASS_PANE ||
         b == &Blocks::LIGHT_BLUE_STAINED_GLASS_PANE ||
         b == &Blocks::YELLOW_STAINED_GLASS_PANE ||
         b == &Blocks::LIME_STAINED_GLASS_PANE ||
         b == &Blocks::PINK_STAINED_GLASS_PANE ||
         b == &Blocks::GRAY_STAINED_GLASS_PANE ||
         b == &Blocks::LIGHT_GRAY_STAINED_GLASS_PANE ||
         b == &Blocks::CYAN_STAINED_GLASS_PANE ||
         b == &Blocks::PURPLE_STAINED_GLASS_PANE ||
         b == &Blocks::BLUE_STAINED_GLASS_PANE ||
         b == &Blocks::BROWN_STAINED_GLASS_PANE ||
         b == &Blocks::GREEN_STAINED_GLASS_PANE ||
         b == &Blocks::RED_STAINED_GLASS_PANE ||
         b == &Blocks::BLACK_STAINED_GLASS_PANE ||
         b == &Blocks::COPPER_BARS || b == &Blocks::EXPOSED_COPPER_BARS ||
         b == &Blocks::WEATHERED_COPPER_BARS ||
         b == &Blocks::OXIDIZED_COPPER_BARS ||
         b == &Blocks::WAXED_COPPER_BARS ||
         b == &Blocks::WAXED_EXPOSED_COPPER_BARS ||
         b == &Blocks::WAXED_WEATHERED_COPPER_BARS ||
         b == &Blocks::WAXED_OXIDIZED_COPPER_BARS;
}

bool isExceptionForConnection(const BlockState &s) {
  return s.block().isLeaves() || s.is(Blocks::BARRIER) ||
         s.is(Blocks::CARVED_PUMPKIN) || s.is(Blocks::JACK_O_LANTERN) ||
         s.is(Blocks::MELON) || s.is(Blocks::PUMPKIN) ||
         s.block().isShulkerBox();
}

bool fenceGateConnectsToDirection(const BlockState &s, Direction d) {
  if (!isFenceGate(s) || !isHorizontalDir(d))
    return false;
  const auto gateFacing = directionFromString(s.getView("facing", "north"));
  return (gateFacing == Direction::North || gateFacing == Direction::South) !=
         (d == Direction::North || d == Direction::South);
}

BlockState wallUpdateShape(World &w, Pos p, const BlockState &s,
                           Direction changed) {
  return wallUpdateShape(w, p, s, changed, w.getBlock(p.relative(changed)));
}
BlockState wallUpdateShape(World &w, Pos p, const BlockState &s,
                           Direction changed,
                           const BlockState &capturedNeighbor) {
  if (changed == Direction::Down)
    return s;
  bool connections[4];
  for (int i = 0; i < 4; ++i) {
    auto d = dirs[i];
    auto n = changed == d ? capturedNeighbor : w.getBlock(p.relative(d));
    const bool connects =
        wall(n) || fenceGateConnectsToDirection(n, d) || isIronBarsBlock(n) ||
        (!isExceptionForConnection(n) &&
         faceSturdy(w, p.relative(d), n, opposite(d)));
    connections[i] =
        changed == d ? connects : s.getView(directionName(d), "none") != "none";
  }
  auto above = changed == Direction::Up ? capturedNeighbor
                                        : w.getBlock(p.relative(Direction::Up));
  VoxelShape movingCoverage;
  const auto &aboveShape = above.is(Blocks::MOVING_PISTON)
                               ? (movingCoverage = collisionShape(
                                      w, p.relative(Direction::Up), above),
                                  movingCoverage)
                               : collisionShape(above);
  auto coverage = face(aboveShape, Direction::Down);
  BlockState out = s;
  for (int i = 0; i < 4; ++i)
    out = out.with(directionName(dirs[i]),
                   connections[i]
                       ? (covers(coverage, wallTest(dirs[i])) ? "tall" : "low")
                       : "none");
  bool n = connections[0], e = connections[1], south = connections[2],
       west = connections[3];
  bool corner = (!n && !e && !south && !west) || (n != south) || (e != west);
  bool high = (out.getView("north") == "tall" && out.getView("south") == "tall") ||
              (out.getView("east") == "tall" && out.getView("west") == "tall");
  bool post = (wall(above) && above.getView("up", "true") == "true") || corner ||
              (!high && covers(coverage, {7, 7, 9, 9}));
  return out.with("up", post ? "true" : "false");
}

BlockState fenceUpdateShape(World &w, Pos p, const BlockState &s,
                            Direction changed) {
  return fenceUpdateShape(w, p, s, changed, w.getBlock(p.relative(changed)));
}

BlockState fenceUpdateShape(World &w, Pos p, const BlockState &s,
                            Direction changed,
                            const BlockState &capturedNeighbor) {
  if (!isHorizontalDir(changed))
    return s;
  const bool sameFence =
      w.hasBlockTag(capturedNeighbor, "minecraft:fences").value_or(false) &&
      w.hasBlockTag(capturedNeighbor, "minecraft:wooden_fences").value_or(false) ==
          w.hasBlockTag(s, "minecraft:wooden_fences").value_or(false);
  const bool gate =
      fenceGateConnectsToDirection(capturedNeighbor, opposite(changed));
  const bool solid =
      !isExceptionForConnection(capturedNeighbor) &&
      faceSturdy(w, p.relative(changed), capturedNeighbor, opposite(changed));
  return s.with(directionName(changed),
                (sameFence || gate || solid) ? "true" : "false");
}

BlockState ironBarsUpdateShape(World &w, Pos p, const BlockState &s,
                               Direction changed) {
  return ironBarsUpdateShape(w, p, s, changed, w.getBlock(p.relative(changed)));
}

BlockState ironBarsUpdateShape(World &w, Pos p, const BlockState &s,
                               Direction changed,
                               const BlockState &capturedNeighbor) {
  if (!isHorizontalDir(changed))
    return s;
  const bool wallAttach =
      w.hasBlockTag(capturedNeighbor, "minecraft:walls").value_or(wall(capturedNeighbor));
  const bool attach =
      isIronBarsBlock(capturedNeighbor) || wallAttach ||
      (!isExceptionForConnection(capturedNeighbor) &&
       faceSturdy(w, p.relative(changed), capturedNeighbor, opposite(changed)));
  return s.with(directionName(changed), attach ? "true" : "false");
}

BlockState stairUpdateShape(World &w, Pos p, const BlockState &s) {
  return computeStairShape(w, p, s, nullptr, nullptr);
}

BlockState stairUpdateShape(World &w, Pos p, const BlockState &s,
                            Direction changed,
                            const BlockState &capturedNeighbor) {
  if (!isHorizontalDir(changed))
    return s;
  return computeStairShape(w, p, s, &changed, &capturedNeighbor);
}
} // namespace redstone
