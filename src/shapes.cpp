#include "redstone/shapes.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <string>
namespace redstone {
bool blocksMotion(const BlockState &state) {
  // BlockBehaviour.calculateSolid: forceSolidOn wins, then the collision
  // shape's bounding box decides. COBWEB and BAMBOO_SAPLING are not in the
  // registered subset.
  if (blockMetadata(state).forceSolid)
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
bool sturdyShape(const VoxelShape &shape, Direction d) {
  if (shape.empty())
    return false;
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
// BlockStateBase exposes immutable per-state voxel shapes; cache them by the
// interned state id so a shape is built at most once.
std::deque<VoxelShape> &shapeCache() {
  static std::deque<VoxelShape> cache;
  return cache;
}
std::vector<bool> &shapeReady() {
  static std::vector<bool> ready;
  return ready;
}
} // namespace
VoxelShape computeStateShape(const BlockState &s) {
  if (isFenceGate(s)) {
    if (s.getView("open", "false") == "true") return {};
    const auto d = directionFromString(s.getView("facing", "north"));
    return {orient({0, 0, 6, 16, 24, 10}, d)};
  }
  if (s.isAir() ||
      isDeadCoralFan(s) || s.is(Blocks::MOVING_PISTON) || isRailBlock(s) ||
      isLiquidBlock(s))
    return {};
  if (isWoolCarpet(s))
    return {{0, 0, 0, 16, 1, 16}};
  if (s.is(Blocks::HONEY_BLOCK))
    return {{1, 0, 1, 15, 15, 15}};
  if (wall(s)) {
    VoxelShape out;
    if (s.getView("up", "true") == "true")
      out.push_back({4, 0, 4, 12, 24, 12});
    for (auto d : dirs)
      if (s.getView(directionName(d), "none") != "none")
        out.push_back(orient({5, 0, 0, 11, 24, 11}, d));
    return out;
  }
  if (s.is(Blocks::PISTON) || s.is(Blocks::STICKY_PISTON)) {
    if (s.getView("extended", "false") == "true")
      return {orient({0, 0, 4, 16, 16, 16},
                     directionFromString(s.getView("facing", "north")))};
    return {{0, 0, 0, 16, 16, 16}};
  }
  if (s.is(Blocks::PISTON_HEAD)) {
    auto d = directionFromString(s.getView("facing", "north"));
    return {orient({0, 0, 0, 16, 16, 4}, d),
            orient({6, 6, 4, 10, 10,
                    s.getView("short", "false") == "true" ? 16.0 : 20.0},
                   d)};
  }
  // TODO: unknown blocks require their registered collision shapes.
  return {{0, 0, 0, 16, 16, 16}};
}
VoxelShape outlineShape(const BlockState &s) {
  if (!isFenceGate(s))
    return collisionShape(s);
  const auto d = directionFromString(s.getView("facing", "north"));
  const double bottom = s.getView("in_wall", "false") == "true" ? 13 : 0;
  return {orient({0, bottom, 6, 16, 16, 10}, d)};
}
const VoxelShape &collisionShape(const BlockState &s) {
  const auto id = static_cast<std::size_t>(s.id());
  auto &cache = shapeCache();
  auto &ready = shapeReady();
  if (id >= cache.size()) {
    cache.resize(id + 1);
    ready.resize(id + 1, false);
  }
  if (!ready[id]) {
    cache[id] = computeStateShape(s);
    ready[id] = true;
  }
  return cache[id];
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
bool faceSturdy(const BlockState &s, Direction d) {
  return sturdyShape(collisionShape(s), d);
}
bool faceSturdy(World &w, Pos p, const BlockState &s, Direction d) {
  if (!s.is(Blocks::MOVING_PISTON))
    return faceSturdy(s, d);
  return sturdyShape(collisionShape(w, p, s), d);
}
bool coralCanSurvive(World &w, Pos p, const BlockState &state) {
  const auto facing = isDeadCoralWallFan(state)
                          ? directionFromString(state.getView("facing", "north"))
                          : Direction::Up;
  const auto support = p.relative(opposite(facing));
  return faceSturdy(w, support, w.getBlock(support), facing);
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
    const auto gateFacing = directionFromString(n.getView("facing", "north"));
    const bool gateConnects = isFenceGate(n) &&
        ((gateFacing == Direction::North || gateFacing == Direction::South) !=
         (d == Direction::North || d == Direction::South));
    connections[i] =
        changed == d ? (wall(n) || gateConnects || faceSturdy(w, p.relative(d), n, opposite(d)))
                     : s.getView(directionName(d), "none") != "none";
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
} // namespace redstone
