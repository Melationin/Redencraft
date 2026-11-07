#include "redstone/piston.hpp"
#include "redstone/entity.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace redstone {
namespace {
constexpr Direction directions[] = {Direction::Down,  Direction::Up,
                                    Direction::North, Direction::South,
                                    Direction::West,  Direction::East};
int axis(Direction d) {
  return d == Direction::Down || d == Direction::Up       ? 0
         : d == Direction::North || d == Direction::South ? 1
                                                          : 2;
}
bool eq(Pos a, Pos b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
Pos relative(Pos p, Direction d, int n) {
  while (n-- > 0)
    p = p.relative(d);
  return p;
}
Direction facing(const BlockState &s) { return directionOf(s); }

std::string_view dirName(Direction d) {
  switch (d) {
  case Direction::Down:
    return "down";
  case Direction::Up:
    return "up";
  case Direction::North:
    return "north";
  case Direction::South:
    return "south";
  case Direction::West:
    return "west";
  default:
    return "east";
  }
}
bool air(const BlockState &s) { return stateIsAir(s); }
bool sticky(const BlockState &s) {
  return s.is(Blocks::SLIME_BLOCK) || s.is(Blocks::HONEY_BLOCK);
}
bool sticks(const BlockState &a, const BlockState &b) {
  return !((a.is(Blocks::HONEY_BLOCK) && b.is(Blocks::SLIME_BLOCK)) ||
           (a.is(Blocks::SLIME_BLOCK) && b.is(Blocks::HONEY_BLOCK))) &&
         (sticky(a) || sticky(b));
}
bool base(const BlockState &s) {
  return s.is(Blocks::PISTON) || s.is(Blocks::STICKY_PISTON);
}
BlockState moving(Direction d, bool stickyType = false) {
  return Blocks::MOVING_PISTON.defaultState()
      .with(PropertyKind::Facing, static_cast<std::uint8_t>(static_cast<Facing>(d)))
      .with(PropertyKind::Type, stickyType ? static_cast<std::uint8_t>(PistonType::Sticky)
                                   : static_cast<std::uint8_t>(PistonType::Normal));
}
using Entity = std::shared_ptr<PistonMovingBlockEntity>;
Entity entityAt(World &w, Pos p) {
  auto it = w.pistonEntities.find(p);
  return it == w.pistonEntities.end() ? nullptr : it->second;
}
void eraseEntity(World &w, Pos p) { w.removePistonEntity(p); }
void putEntity(World &w, Pos p, BlockState s, Direction d, bool extending,
               bool source) {
  w.setPistonEntity(
      p, std::make_shared<PistonMovingBlockEntity>(
             PistonMovingBlockEntity{p, std::move(s), d, extending, source}));
}

void finalTick(World &w, const Entity &e) {
  if (!e || e->progressO >= 1)
    return;
  e->progress = e->progressO = 1;
  eraseEntity(w, e->pos);
  if (w.getBlock(e->pos).is(Blocks::MOVING_PISTON)) {
    auto s = e->source ? airState()
                       : w.updateFromNeighbourShapes(e->pos, e->movedState);
    w.setBlock(e->pos, s, 3);
    w.neighborChanged(e->pos, s.name());
  }
}
bool powered(World &w, Pos p, Direction f) {
  for (auto d : directions)
    if (d != f && w.getSignal(p.relative(d), d) > 0)
      return true;
  if (w.getSignal(p, Direction::Down) > 0)
    return true;
  auto above = p.relative(Direction::Up);
  for (auto d : directions)
    if (d != Direction::Down && w.getSignal(above.relative(d), d) > 0)
      return true;
  return false;
}
} // namespace
bool pistonIsPushable(World &w, Pos p, const BlockState &s, Direction d,
                      bool allowDestroy, Direction connection) {
  // TODO: configurable world borders and full block entity metadata.
  if (p.x < -29999984 || p.x >= 29999984 || p.z < -29999984 || p.z >= 29999984)
    return false;
  if (p.y < w.minY() || p.y >= w.maxY())
    return false;
  if (air(s))
    return true;
  if ((d == Direction::Down && p.y == w.minY()) ||
      (d == Direction::Up && p.y == w.maxY() - 1))
    return false;
  if (s.is(Blocks::MOVING_PISTON) || s.is(Blocks::PISTON_HEAD) ||
      s.is(Blocks::OBSIDIAN) || s.is(Blocks::CRYING_OBSIDIAN) ||
      s.is(Blocks::RESPAWN_ANCHOR) || s.is(Blocks::REINFORCED_DEEPSLATE) ||
      s.is(Blocks::BEDROCK))
    return false;
  if (base(s))
    return !extendedOf(s);
  const auto &metadata = blockMetadata(s);
  auto reaction = s.getView("push_reaction", metadata.pushReaction);
  if (metadata.hardness == -1 || s.getView("hardness") == "-1" ||
      s.getView("has_block_entity") == "true" || reaction == "block")
    return false;
  if (reaction == "destroy")
    return allowDestroy;
  if (reaction == "push_only")
    return d == connection;
  return true;
}
PistonStructureResolver::PistonStructureResolver(World &w, Pos p, Direction f,
                                                 bool e)
    : world_(w), piston_(p), start_(relative(p, f, e ? 1 : 2)), facing_(f),
      movement_(e ? f : opposite(f)), extending_(e) {}
bool PistonStructureResolver::resolve() {
  pushCount_ = 0;
  destroy_.clear();
  auto s = world_.getBlock(start_);
  if (!pistonIsPushable(world_, start_, s, movement_, false, facing_)) {
    if (extending_ &&
        (s.getView("push_reaction", blockMetadata(s).pushReaction) == "destroy")) {
      destroy_.push_back(start_);
      return true;
    }
    return false;
  }
  if (!addBlockLine(start_, movement_))
    return false;
  for (size_t i = 0; i < pushCount_; ++i)
    if (sticky(world_.getBlock(push_[i])) && !addBranchingBlocks(push_[i]))
      return false;
  return true;
}
bool PistonStructureResolver::addBlockLine(Pos start, Direction connection) {
  auto s = world_.getBlock(start);
  if (air(s) ||
      !pistonIsPushable(world_, start, s, movement_, false, connection) ||
      eq(start, piston_) ||
      std::any_of(push_.begin(), push_.begin() + pushCount_,
                  [&](Pos p) { return eq(p, start); }))
    return true;
  int count = 1;
  if (count + pushCount_ > 12)
    return false;
  while (sticky(s)) {
    auto p = relative(start, opposite(movement_), count);
    auto previous = s;
    s = world_.getBlock(p);
    if (air(s) || !sticks(previous, s) ||
        !pistonIsPushable(world_, p, s, movement_, false,
                          opposite(movement_)) ||
        eq(p, piston_))
      break;
    if (++count + pushCount_ > 12)
      return false;
  }
  int added = 0;
  for (int i = count - 1; i >= 0; --i) {
    push_[pushCount_++] = relative(start, opposite(movement_), i);
    ++added;
  }
  for (int i = 1;; ++i) {
    auto p = relative(start, movement_, i);
    auto it = std::find_if(push_.begin(), push_.begin() + pushCount_,
                           [&](Pos q) { return eq(p, q); });
    if (it != push_.begin() + pushCount_) {
      auto index = static_cast<size_t>(it - push_.begin());
      std::rotate(push_.begin() + index, push_.begin() + pushCount_ - added,
                  push_.begin() + pushCount_);
      for (size_t j = 0; j <= index + added; ++j)
        if (sticky(world_.getBlock(push_[j])) && !addBranchingBlocks(push_[j]))
          return false;
      return true;
    }
    s = world_.getBlock(p);
    if (air(s))
      return true;
    if (!pistonIsPushable(world_, p, s, movement_, true, movement_) ||
        eq(p, piston_))
      return false;
    if ((s.getView("push_reaction", blockMetadata(s).pushReaction) == "destroy")) {
      destroy_.push_back(p);
      return true;
    }
    if (pushCount_ >= 12)
      return false;
    push_[pushCount_++] = p;
    ++added;
  }
}
bool PistonStructureResolver::addBranchingBlocks(Pos p) {
  auto s = world_.getBlock(p);
  for (auto d : directions)
    if (axis(d) != axis(movement_)) {
      auto q = p.relative(d);
      if (sticks(world_.getBlock(q), s) && !addBlockLine(q, d))
        return false;
    }
  return true;
}
namespace {
bool moveBlocks(World &w, Pos p, Direction f, bool extending, bool stickyBase) {
  auto arm = p.relative(f);
  if (!extending && w.getBlock(arm).is(Blocks::PISTON_HEAD))
    w.setBlock(arm, airState(), 276);
  PistonStructureResolver resolver(w, p, f, extending);
  if (!resolver.resolve())
    return false;
  const auto push = resolver.toPush();
  const auto &destroy = resolver.toDestroy();
  // Only the pushed list is bounded by PUSH_LIMIT. Destruction may include
  // multiple sticky branches, so keep that snapshot dynamically sized.
  std::array<std::pair<Pos, BlockState>, 12> removedStorage;
  std::span removed(removedStorage.data(), push.size());
  std::array<BlockState, 12> original;
  for (std::size_t i = 0; i < push.size(); ++i) {
    const auto q = push[i];
    auto s = w.getBlock(q);
    original[i] = s;
    removed[i] = {q, s};
  }
  auto movement = extending ? f : opposite(f);
  std::array<BlockState, 12> pushedUpdates;
  std::vector<BlockState> destroyedUpdates;
  destroyedUpdates.reserve(destroy.size());
  const auto forgetRemoved = [&](Pos dest) {
    const auto end = std::remove_if(
        removed.begin(), removed.end(),
        [&](const auto &pair) { return eq(pair.first, dest); });
    removed = removed.first(static_cast<std::size_t>(end - removed.begin()));
  };
  for (auto it = destroy.rbegin(); it != destroy.rend(); ++it) {
    auto s = w.getBlock(*it);
    destroyedUpdates.push_back(s);
    // Empty-tool loot evaluation: coral fans/glass require Silk Touch, while
    // normal blocks pop a real item entity before the destruction callback.
    for (const auto &drop : blockDrops(s))
      w.spawnItem(*it, drop.id, drop.count);
    // TODO: destruction game event.
    w.setBlock(*it, airState(), 18);
  }
  for (int i = static_cast<int>(push.size()) - 1; i >= 0; --i) {
    auto s = w.getBlock(push[i]);
    auto dest = push[i].relative(movement);
    forgetRemoved(dest);
    w.setBlock(dest, moving(f), 324);
    putEntity(w, dest, original[i], f, extending, false);
    pushedUpdates[i] = s;
  }
  if (extending) {
    auto head = Blocks::PISTON_HEAD.defaultState()
                    .with(PropertyKind::Facing, static_cast<std::uint8_t>(static_cast<Facing>(f)))
                    .with(PropertyKind::Type, stickyBase ? static_cast<std::uint8_t>(PistonType::Sticky)
                                              : static_cast<std::uint8_t>(PistonType::Normal));
    forgetRemoved(arm);
    w.setBlock(arm, moving(f, stickyBase), 324);
    putEntity(w, arm, head, f, true, true);
  }
  for (auto &pair : removed)
    w.setBlock(pair.first, airState(), 82);
  for (auto &pair : removed) {
    w.updateIndirectNeighbourShapes(pair.first, pair.second, 2, 512);
    w.updateNeighbourShapes(pair.first, airState(), 2, 512);
    w.updateIndirectNeighbourShapes(pair.first, airState(), 2, 512);
  }
  size_t index = 0;
  for (auto it = destroy.rbegin(); it != destroy.rend(); ++it) {
    auto old = destroyedUpdates[index++];
    w.affectNeighborsAfterRemoval(*it, old, false);
    w.updateIndirectNeighbourShapes(*it, old, 2, 512);
    w.updateNeighborsAt(*it, old.name());
  }
  for (std::size_t i = push.size(); i-- > 0;)
    w.updateNeighborsAt(push[i], pushedUpdates[i].block());
  if (extending)
    w.updateNeighborsAt(arm, "piston_head");
  return true;
}
void check(World &w, Pos p, const BlockState &s) {
  auto f = facing(s);
  bool power = powered(w, p, f);
  bool extended = extendedOf(s);
  if (power && !extended) {
    PistonStructureResolver resolver(w, p, f, true);
    if (resolver.resolve())
      w.blockEvent(p, s.name(), 0, static_cast<int>(f));
  } else if (!power && extended) {
    auto e = entityAt(w, relative(p, f, 2));
    int event = 1;
    if (w.getBlock(relative(p, f, 2)).is(Blocks::MOVING_PISTON) && e &&
        e->direction == f && e->extending &&
        (e->progressO < 0.5f || w.gameTime() == e->lastTicked ||
         w.isHandlingTick()))
      event = 2;
    w.blockEvent(p, s.name(), event, static_cast<int>(f));
  }
}
} // namespace
void pistonNeighborChanged(World &w, Pos p, const BlockState &s) {
  if (base(s))
    check(w, p, s);
}
void pistonOnPlace(World &w, Pos p, const BlockState &s,
                   const BlockState &old) {
  if (base(s) && !old.sameBlock(s) && !entityAt(w, p))
    check(w, p, s);
}
bool pistonBlockEvent(World &w, Pos p, const BlockState &s, int type,
                      int data) {
  auto f = facing(s);
  bool power = powered(w, p, f);
  auto extended = s.with(PropertyKind::Extended, std::uint8_t{1});
  if (power && (type == 1 || type == 2)) {
    w.setBlock(p, extended, 2);
    return false;
  }
  if (!power && type == 0)
    return false;
  bool isSticky = s.is(Blocks::STICKY_PISTON);
  if (type == 0) {
    if (!moveBlocks(w, p, f, true, isSticky))
      return false;
    w.setBlock(p, extended, 67);
  } else if (type == 1 || type == 2) {
    auto arm = p.relative(f);
    finalTick(w, entityAt(w, arm));
    auto m = moving(f, isSticky);
    w.setBlock(p, m, 276);
    Direction saved = directions[(data & 7) % 6];
    putEntity(w, p,
              (isSticky ? Blocks::STICKY_PISTON : Blocks::PISTON)
                  .defaultState()
                  .with(PropertyKind::Facing, static_cast<std::uint8_t>(static_cast<Facing>(saved)))
                  .with(PropertyKind::Extended, std::uint8_t{0}),
              f, false, true);
    w.updateNeighborsAt(p, m.name());
    w.updateNeighbourShapes(p, m, 2, 512);
    if (isSticky) {
      auto two = relative(p, f, 2);
      auto moved = w.getBlock(two);
      auto e = entityAt(w, two);
      bool piece = moved.is(Blocks::MOVING_PISTON) && e && e->direction == f &&
                   e->extending;
      if (piece)
        finalTick(w, e);
      else if (type != 1 || air(moved) ||
               !pistonIsPushable(w, two, moved, opposite(f), false, f) ||
               (moved.getView("push_reaction", blockMetadata(moved).pushReaction) !=
                    "normal" &&
                !base(moved)))
        w.setBlock(arm, airState(), 3);
      else
        moveBlocks(w, p, f, false, true);
    } else
      w.setBlock(arm, airState(), 3);
  }
  // TODO: piston sounds and block activation/deactivation game events.
  return true;
}
namespace {
Aabb boundsOf(const VoxelShape &shape) {
  Aabb bounds{16, 16, 16, 0, 0, 0};
  for (const auto &box : shape) {
    bounds.minX = std::min(bounds.minX, box.x0 / 16.0);
    bounds.minY = std::min(bounds.minY, box.y0 / 16.0);
    bounds.minZ = std::min(bounds.minZ, box.z0 / 16.0);
    bounds.maxX = std::max(bounds.maxX, box.x1 / 16.0);
    bounds.maxY = std::max(bounds.maxY, box.y1 / 16.0);
    bounds.maxZ = std::max(bounds.maxZ, box.z1 / 16.0);
  }
  return bounds;
}
Aabb toAabb(const ShapeBox &box) {
  return {box.x0 / 16.0, box.y0 / 16.0, box.z0 / 16.0,
          box.x1 / 16.0, box.y1 / 16.0, box.z1 / 16.0};
}
Aabb unionBox(const Aabb &a, const Aabb &b) {
  return {std::min(a.minX, b.minX), std::min(a.minY, b.minY),
          std::min(a.minZ, b.minZ), std::max(a.maxX, b.maxX),
          std::max(a.maxY, b.maxY), std::max(a.maxZ, b.maxZ)};
}
// PistonMath.getMovementArea.
Aabb movementArea(const Aabb &aabb, Direction direction, double amount) {
  double delta = amount * (direction == Direction::Down ||
                                   direction == Direction::West ||
                                   direction == Direction::North
                               ? -1.0
                               : 1.0);
  double min = std::min(delta, 0.0), max = std::max(delta, 0.0);
  switch (direction) {
  case Direction::West:
    return {aabb.minX + min, aabb.minY, aabb.minZ, aabb.minX + max, aabb.maxY,
            aabb.maxZ};
  case Direction::East:
    return {aabb.maxX + min, aabb.minY, aabb.minZ, aabb.maxX + max, aabb.maxY,
            aabb.maxZ};
  case Direction::Down:
    return {aabb.minX, aabb.minY + min, aabb.minZ, aabb.maxX, aabb.minY + max,
            aabb.maxZ};
  case Direction::North:
    return {aabb.minX, aabb.minY, aabb.minZ + min, aabb.maxX, aabb.maxY,
            aabb.minZ + max};
  case Direction::South:
    return {aabb.minX, aabb.minY, aabb.maxZ + min, aabb.maxX, aabb.maxY,
            aabb.maxZ + max};
  case Direction::Up:
  default:
    return {aabb.minX, aabb.maxY + min, aabb.minZ, aabb.maxX, aabb.maxY + max,
            aabb.maxZ};
  }
}
Aabb byPositionAndProgress(Pos pos, const Aabb &aabb,
                           const PistonMovingBlockEntity &entity) {
  double progress = entity.extendedProgress();
  return aabb.move(pos.x + progress * (entity.direction == Direction::East
                                           ? 1.0
                                           : entity.direction == Direction::West
                                                 ? -1.0
                                                 : 0.0),
                   pos.y + progress * (entity.direction == Direction::Up
                                           ? 1.0
                                           : entity.direction == Direction::Down
                                                 ? -1.0
                                                 : 0.0),
                   pos.z + progress * (entity.direction == Direction::South
                                           ? 1.0
                                           : entity.direction == Direction::North
                                                 ? -1.0
                                                 : 0.0));
}
BlockState collisionRelatedState(const PistonMovingBlockEntity &entity) {
  if (!entity.extending && entity.source &&
      (entity.movedState.is(Blocks::PISTON) ||
       entity.movedState.is(Blocks::STICKY_PISTON))) {
    auto head = Blocks::PISTON_HEAD.defaultState();
    head = head.with(
        PropertyKind::Facing,
        static_cast<std::uint8_t>(facingOf(
            entity.movedState, static_cast<Facing>(entity.direction))));
    head = head.with(PropertyKind::Short,
                     entity.progress > 0.25f ? std::uint8_t{1} : std::uint8_t{0});
    head = head.with(PropertyKind::Type,
                     entity.movedState.is(Blocks::STICKY_PISTON)
                         ? static_cast<std::uint8_t>(PistonType::Sticky)
                         : static_cast<std::uint8_t>(PistonType::Normal));
    return head;
  }
  return entity.movedState;
}
double getMovement(const Aabb &moving, Direction movement,
                   const Aabb &entity) {
  switch (movement) {
  case Direction::East:
    return moving.maxX - entity.minX;
  case Direction::West:
    return entity.maxX - moving.minX;
  case Direction::Down:
    return entity.maxY - moving.minY;
  case Direction::South:
    return moving.maxZ - entity.minZ;
  case Direction::North:
    return entity.maxZ - moving.minZ;
  case Direction::Up:
  default:
    return moving.maxY - entity.minY;
  }
}
void moveEntityByPiston(World &world, Direction pistonDirection, redstone::Entity &entity,
                        double delta, Direction movement) {
  world.pistonNoClip = pistonDirection;
  entity.move(world,
              Vec3{delta * (movement == Direction::East
                                ? 1.0
                                : movement == Direction::West ? -1.0 : 0.0),
                   delta * (movement == Direction::Up
                                ? 1.0
                                : movement == Direction::Down ? -1.0 : 0.0),
                   delta * (movement == Direction::South
                                ? 1.0
                                : movement == Direction::North ? -1.0 : 0.0)},
              true);
  entity.applyEffectsFromBlocks(world);
  world.pistonNoClip.reset();
}
void fixEntityWithinPistonBase(World &world, Pos pos, redstone::Entity &entity,
                               Direction movement, double deltaProgress) {
  Aabb entityBox = entity.boundingBox();
  Aabb pistonBox{double(pos.x), double(pos.y), double(pos.z), pos.x + 1.0,
                 pos.y + 1.0, pos.z + 1.0};
  if (!entityBox.intersects(pistonBox))
    return;
  Direction opposite = redstone::opposite(movement);
  double delta = getMovement(pistonBox, opposite, entityBox) + 0.01;
  Aabb intersection{std::max(entityBox.minX, pistonBox.minX),
                    std::max(entityBox.minY, pistonBox.minY),
                    std::max(entityBox.minZ, pistonBox.minZ),
                    std::min(entityBox.maxX, pistonBox.maxX),
                    std::min(entityBox.maxY, pistonBox.maxY),
                    std::min(entityBox.maxZ, pistonBox.maxZ)};
  double deltaIntersected = getMovement(pistonBox, opposite, intersection) + 0.01;
  if (std::abs(delta - deltaIntersected) < 0.01) {
    delta = std::min(delta, deltaProgress) + 0.01;
    moveEntityByPiston(world, movement, entity, delta, opposite);
  }
}
void moveCollidedEntities(World &world, Pos pos, float newProgress,
                          const PistonMovingBlockEntity &self) {
  Direction movement = self.movementDirection();
  double deltaProgress = newProgress - self.progress;
  const VoxelShape &shape = collisionShape(collisionRelatedState(self));
  if (shape.empty())
    return;
  Aabb aabb = byPositionAndProgress(pos, boundsOf(shape), self);
  auto entities = world.entitiesIn(
      unionBox(movementArea(aabb, movement, deltaProgress), aabb));
  if (entities.empty())
    return;
  bool causeBounce = self.movedState.is(Blocks::SLIME_BLOCK);
  for (redstone::Entity *entityPtr : entities) {
    redstone::Entity &entity = *entityPtr;
    if (causeBounce) {
      Vec3 movementVec = entity.deltaMovement;
      if (movement == Direction::East || movement == Direction::West)
        movementVec.x = movement == Direction::East ? 1.0 : -1.0;
      else if (movement == Direction::Up || movement == Direction::Down)
        movementVec.y = movement == Direction::Up ? 1.0 : -1.0;
      else
        movementVec.z = movement == Direction::South ? 1.0 : -1.0;
      entity.setDeltaMovement(movementVec);
    }
    double delta = 0;
    for (const auto &part : shape) {
      Aabb moving = movementArea(byPositionAndProgress(pos, toAabb(part), self),
                                 movement, deltaProgress);
      if (!moving.intersects(entity.boundingBox()))
        continue;
      delta = std::max(delta, getMovement(moving, movement,
                                          entity.boundingBox()));
      if (delta >= deltaProgress)
        break;
    }
    if (delta <= 0)
      continue;
    delta = std::min(delta, deltaProgress) + 0.01;
    moveEntityByPiston(world, movement, entity, delta, movement);
    if (!self.extending && self.source)
      fixEntityWithinPistonBase(world, pos, entity, movement, deltaProgress);
  }
}
void moveStuckEntities(World &world, Pos pos, float newProgress,
                       const PistonMovingBlockEntity &self) {
  if (!self.movedState.is(Blocks::HONEY_BLOCK))
    return;
  Direction movement = self.movementDirection();
  if (movement != Direction::East && movement != Direction::West &&
      movement != Direction::North && movement != Direction::South)
    return;
  double stickyTop =
      boundsOf(collisionShape(self.movedState)).maxY;
  Aabb aabb = byPositionAndProgress(
      pos, {0, stickyTop, 0, 1, 1.500001, 1}, self);
  double deltaProgress = newProgress - self.progress;
  for (redstone::Entity *entityPtr : world.entitiesIn(aabb)) {
    redstone::Entity &entity = *entityPtr;
    if (!entity.onGround)
      continue;
    if (!(entity.position.x >= aabb.minX && entity.position.x <= aabb.maxX &&
          entity.position.z >= aabb.minZ && entity.position.z <= aabb.maxZ))
      continue;
    moveEntityByPiston(world, movement, entity, deltaProgress, movement);
  }
}
} // namespace

void pistonTickBlockEntities(World &w) {
  // Level.tickBlockEntities: fold the deferred tickers in, walk the flat list
  // once, then sweep the entries marked removed. Tickers registered during the
  // walk land in the pending list and first run on the next pass, so the list
  // being walked is never resized underneath us and registration order holds.
  w.mergePendingPistonTickers();
  struct PassGuard {
    bool &flag;
    explicit PassGuard(bool &value) : flag(value) { flag = true; }
    ~PassGuard() { flag = false; }
  } guard{w.tickingPistonBlockEntities};
  const std::size_t count = w.pistonTickers.size();
  for (std::size_t i = 0; i < count; ++i) {
    PistonTickerSlot &slot = *w.pistonTickers[i];
    if (slot.removed || !slot.entity)
      continue;
    // The slot owns the entity until the sweep, so a removal caused by this
    // very tick (finalTick, completion, explosions) cannot invalidate it. No
    // position lookup is needed: the ticker is bound to its entity.
    PistonMovingBlockEntity &e = *slot.entity;
    if (!e.live || !w.isLoaded(e.pos))
      continue;
    e.lastTicked = w.gameTime();
    e.progressO = e.progress;
    if (e.progressO >= 1) {
      eraseEntity(w, e.pos);
      if (w.traceUpdates)
        w.log.push_back({"piston_entity_remove", e.pos, ""});
      if (w.pistonCompletionObserver)
        w.pistonCompletionObserver(w, e.pos);
      if (w.getBlock(e.pos).is(Blocks::MOVING_PISTON)) {
        if (w.traceUpdates)
          w.log.push_back({"piston_shape_resolve", e.pos, ""});
        auto s = w.updateFromNeighbourShapes(e.pos, e.movedState);
        if (air(s)) {
          if (w.traceUpdates)
            w.log.push_back({"piston_set_block", e.pos, "340"});
          w.setBlock(e.pos, e.movedState, 340);
          w.updateOrDestroy(e.pos, e.movedState, s, 3, 512);
        } else {
          if (waterloggedOf(s))
            s = s.with(PropertyKind::Waterlogged, std::uint8_t{0});
          if (w.traceUpdates)
            w.log.push_back({"piston_set_block", e.pos, "67"});
          w.setBlock(e.pos, s, 67);
          if (w.traceUpdates)
            w.log.push_back({"piston_self_notify", e.pos, s.name()});
          w.neighborChanged(e.pos, s.name());
        }
      }
    } else {
      // Vanilla sweeps the full half-block step before clamping the stored
      // progress. Nonstandard progress loaded from NBT can overshoot 1 here.
      float next = e.progress + 0.5f;
      moveCollidedEntities(w, e.pos, next, e);
      moveStuckEntities(w, e.pos, next, e);
      e.progress = std::min(1.0f, next);
    }
  }
  w.sweepPistonTickers();
}
void pistonPreRemoveSideEffects(World &w, Pos p) {
  finalTick(w, entityAt(w, p));
}
void pistonForgetWorld(World &w) {
  w.pistonEntities.clear();
  w.pistonTickerSlots.clear();
  w.pistonTickers.clear();
  w.pendingPistonTickers.clear();
  w.tickingPistonBlockEntities = false;
}
} // namespace redstone
