#include "redstone/shapes.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
namespace redstone {
namespace {
bool named(const BlockState &s, const char *name) {
  return s.name == name || s.name == std::string("minecraft:") + name;
}
bool wall(const BlockState &s) { return named(s, "cobblestone_wall"); }
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
VoxelShape collisionShape(const BlockState &s) {
  if (named(s, "air") || named(s, "cave_air") || named(s, "void_air") ||
      isDeadCoralFan(s) || named(s, "moving_piston"))
    return {};
  if (isWoolCarpet(s))
    return {{0, 0, 0, 16, 1, 16}};
  if (named(s, "honey_block"))
    return {{1, 0, 1, 15, 15, 15}};
  if (wall(s)) {
    VoxelShape out;
    if (s.get("up", "true") == "true")
      out.push_back({4, 0, 4, 12, 24, 12});
    for (auto d : dirs)
      if (s.get(directionName(d), "none") != "none")
        out.push_back(orient({5, 0, 0, 11, 24, 11}, d));
    return out;
  }
  if (named(s, "piston") || named(s, "sticky_piston")) {
    if (s.get("extended", "false") == "true")
      return {orient({0, 0, 4, 16, 16, 16},
                     directionFromString(s.get("facing", "north")))};
    return {{0, 0, 0, 16, 16, 16}};
  }
  if (named(s, "piston_head")) {
    auto d = directionFromString(s.get("facing", "north"));
    return {orient({0, 0, 0, 16, 16, 4}, d),
            orient({6, 6, 4, 10, 10,
                    s.get("short", "false") == "true" ? 16.0 : 20.0},
                   d)};
  }
  // TODO: unknown blocks require their registered collision shapes.
  return {{0, 0, 0, 16, 16, 16}};
}
VoxelShape collisionShape(World &w, Pos p, const BlockState &s) {
  if (!named(s, "moving_piston"))
    return collisionShape(s);
  auto it = w.pistonEntities.find(p);
  if (it == w.pistonEntities.end())
    return {};
  auto e = it->second;
  VoxelShape result;
  if (!e->extending && e->source &&
      (named(e->movedState, "piston") || named(e->movedState, "sticky_piston")))
    result = collisionShape(e->movedState.with("extended", "true"));
  auto moved =
      e->source
          ? defaultState("piston_head")
                .with("facing", directionName(e->direction))
                .with("short", (e->extending != (1.0f - e->progress < 0.25f))
                                   ? "true"
                                   : "false")
          : e->movedState;
  auto parts = collisionShape(moved);
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
  return covers(face(collisionShape(s), d), {0, 0, 16, 16});
}
bool faceSturdy(World &w, Pos p, const BlockState &s, Direction d) {
  return covers(face(collisionShape(w, p, s), d), {0, 0, 16, 16});
}
bool coralCanSurvive(World &w, Pos p, const BlockState &state) {
  const auto facing = isDeadCoralWallFan(state)
                          ? directionFromString(state.get("facing", "north"))
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
    connections[i] =
        changed == d ? (wall(n) || faceSturdy(w, p.relative(d), n, opposite(d)))
                     : s.get(directionName(d), "none") != "none";
  }
  auto above = changed == Direction::Up ? capturedNeighbor
                                        : w.getBlock(p.relative(Direction::Up));
  auto coverage = face(collisionShape(w, p.relative(Direction::Up), above),
                       Direction::Down);
  BlockState out = s;
  for (int i = 0; i < 4; ++i)
    out = out.with(directionName(dirs[i]),
                   connections[i]
                       ? (covers(coverage, wallTest(dirs[i])) ? "tall" : "low")
                       : "none");
  bool n = connections[0], e = connections[1], south = connections[2],
       west = connections[3];
  bool corner = (!n && !e && !south && !west) || (n != south) || (e != west);
  bool high = (out.get("north") == "tall" && out.get("south") == "tall") ||
              (out.get("east") == "tall" && out.get("west") == "tall");
  bool post = (wall(above) && above.get("up", "true") == "true") || corner ||
              (!high && covers(coverage, {7, 7, 9, 9}));
  return out.with("up", post ? "true" : "false");
}
} // namespace redstone
