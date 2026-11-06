#include "redstone/piston.hpp"
#include <algorithm>
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
Direction facing(const BlockState &s) {
  auto f = s.get("facing", "north");
  if (f == "down")
    return Direction::Down;
  if (f == "up")
    return Direction::Up;
  if (f == "south")
    return Direction::South;
  if (f == "west")
    return Direction::West;
  if (f == "east")
    return Direction::East;
  return Direction::North;
}
std::string dirName(Direction d) {
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
bool named(const BlockState &s, const char *n) {
  return s.name == n || s.name == std::string("minecraft:") + n;
}
bool air(const BlockState &s) {
  return s.name.empty() || named(s, "air") || named(s, "cave_air") ||
         named(s, "void_air");
}
bool sticky(const BlockState &s) {
  return named(s, "slime_block") || named(s, "honey_block");
}
bool sticks(const BlockState &a, const BlockState &b) {
  return !((named(a, "honey_block") && named(b, "slime_block")) ||
           (named(a, "slime_block") && named(b, "honey_block"))) &&
         (sticky(a) || sticky(b));
}
bool base(const BlockState &s) {
  return named(s, "piston") || named(s, "sticky_piston");
}
BlockState state(std::string n) { return defaultState(n); }
BlockState moving(Direction d, bool stickyType = false) {
  return state("moving_piston")
      .with("facing", dirName(d))
      .with("type", stickyType ? "sticky" : "normal");
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
  if (named(w.getBlock(e->pos), "moving_piston")) {
    auto s = e->source ? state("air")
                       : w.updateFromNeighbourShapes(e->pos, e->movedState);
    w.setBlock(e->pos, s, 3);
    w.neighborChanged(e->pos, s.name);
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
  if (named(s, "moving_piston") || named(s, "piston_head") ||
      named(s, "obsidian") || named(s, "crying_obsidian") ||
      named(s, "respawn_anchor") || named(s, "reinforced_deepslate") ||
      named(s, "bedrock"))
    return false;
  if (base(s))
    return s.get("extended", "false") != "true";
  auto metadata = blockMetadata(s);
  auto reaction = s.get("push_reaction", metadata.pushReaction);
  if (metadata.hardness == -1 || s.get("hardness") == "-1" ||
      s.get("has_block_entity") == "true" || reaction == "block")
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
  push_.clear();
  destroy_.clear();
  auto s = world_.getBlock(start_);
  if (!pistonIsPushable(world_, start_, s, movement_, false, facing_)) {
    if (extending_ &&
        (s.get("push_reaction", blockMetadata(s).pushReaction) == "destroy")) {
      destroy_.push_back(start_);
      return true;
    }
    return false;
  }
  if (!addBlockLine(start_, movement_))
    return false;
  for (size_t i = 0; i < push_.size(); ++i)
    if (sticky(world_.getBlock(push_[i])) && !addBranchingBlocks(push_[i]))
      return false;
  return true;
}
bool PistonStructureResolver::addBlockLine(Pos start, Direction connection) {
  auto s = world_.getBlock(start);
  if (air(s) ||
      !pistonIsPushable(world_, start, s, movement_, false, connection) ||
      eq(start, piston_) || std::any_of(push_.begin(), push_.end(), [&](Pos p) {
        return eq(p, start);
      }))
    return true;
  int count = 1;
  if (count + push_.size() > 12)
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
    if (++count + push_.size() > 12)
      return false;
  }
  int added = 0;
  for (int i = count - 1; i >= 0; --i) {
    push_.push_back(relative(start, opposite(movement_), i));
    ++added;
  }
  for (int i = 1;; ++i) {
    auto p = relative(start, movement_, i);
    auto it = std::find_if(push_.begin(), push_.end(),
                           [&](Pos q) { return eq(p, q); });
    if (it != push_.end()) {
      auto index = static_cast<size_t>(it - push_.begin());
      std::rotate(push_.begin() + index, push_.end() - added, push_.end());
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
    if ((s.get("push_reaction", blockMetadata(s).pushReaction) == "destroy")) {
      destroy_.push_back(p);
      return true;
    }
    if (push_.size() >= 12)
      return false;
    push_.push_back(p);
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
  if (!extending && named(w.getBlock(arm), "piston_head"))
    w.setBlock(arm, state("air"), 276);
  PistonStructureResolver resolver(w, p, f, extending);
  if (!resolver.resolve())
    return false;
  auto push = resolver.toPush(), destroy = resolver.toDestroy();
  std::vector<std::pair<Pos, BlockState>> removed;
  std::vector<BlockState> original;
  for (auto q : push) {
    auto s = w.getBlock(q);
    original.push_back(s);
    removed.emplace_back(q, s);
  }
  auto movement = extending ? f : opposite(f);
  std::vector<BlockState> updates;
  for (auto it = destroy.rbegin(); it != destroy.rend(); ++it) {
    auto s = w.getBlock(*it);
    updates.push_back(s);
    // Coral fans require Silk Touch, which a piston does not provide.
    // TODO: evaluate registered loot tables for additional destroyable blocks.
    if (!isDeadCoralFan(s))
      w.log.push_back({"entity_spawn", *it, "minecraft:item:" + s.name});
    // TODO: destruction game event.
    w.setBlock(*it, state("air"), 18);
  }
  for (int i = static_cast<int>(push.size()) - 1; i >= 0; --i) {
    auto s = w.getBlock(push[i]);
    auto dest = push[i].relative(movement);
    std::erase_if(removed, [&](auto &pair) { return eq(pair.first, dest); });
    w.setBlock(dest, moving(f), 324);
    putEntity(w, dest, original[i], f, extending, false);
    updates.push_back(s);
  }
  if (extending) {
    auto head = state("piston_head")
                    .with("facing", dirName(f))
                    .with("type", stickyBase ? "sticky" : "normal");
    std::erase_if(removed, [&](auto &pair) { return eq(pair.first, arm); });
    w.setBlock(arm, moving(f, stickyBase), 324);
    putEntity(w, arm, head, f, true, true);
  }
  for (auto &pair : removed)
    w.setBlock(pair.first, state("air"), 82);
  for (auto &pair : removed) {
    w.updateIndirectNeighbourShapes(pair.first, pair.second, 2, 512);
    w.updateNeighbourShapes(pair.first, state("air"), 2, 512);
    w.updateIndirectNeighbourShapes(pair.first, state("air"), 2, 512);
  }
  size_t index = 0;
  for (auto it = destroy.rbegin(); it != destroy.rend(); ++it) {
    auto old = updates[index++];
    w.affectNeighborsAfterRemoval(*it, old, false);
    w.updateIndirectNeighbourShapes(*it, old, 2, 512);
    w.updateNeighborsAt(*it, old.name);
  }
  for (auto it = push.rbegin(); it != push.rend(); ++it)
    w.updateNeighborsAt(*it, updates[index++].name);
  if (extending)
    w.updateNeighborsAt(arm, "piston_head");
  return true;
}
void check(World &w, Pos p, const BlockState &s) {
  auto f = facing(s);
  bool power = powered(w, p, f);
  bool extended = s.get("extended", "false") == "true";
  if (power && !extended) {
    PistonStructureResolver resolver(w, p, f, true);
    if (resolver.resolve())
      w.blockEvent(p, s.name, 0, static_cast<int>(f));
  } else if (!power && extended) {
    auto e = entityAt(w, relative(p, f, 2));
    int event = 1;
    if (named(w.getBlock(relative(p, f, 2)), "moving_piston") && e &&
        e->direction == f && e->extending &&
        (e->progressO < 0.5f || w.gameTime() == e->lastTicked ||
         w.isHandlingTick()))
      event = 2;
    w.blockEvent(p, s.name, event, static_cast<int>(f));
  }
}
} // namespace
void pistonNeighborChanged(World &w, Pos p, const BlockState &s) {
  if (base(s))
    check(w, p, s);
}
void pistonOnPlace(World &w, Pos p, const BlockState &s,
                   const BlockState &old) {
  if (base(s) && old.name != s.name && !entityAt(w, p))
    check(w, p, s);
}
bool pistonBlockEvent(World &w, Pos p, const BlockState &s, int type,
                      int data) {
  auto f = facing(s);
  bool power = powered(w, p, f);
  auto extended = s.with("extended", "true");
  if (power && (type == 1 || type == 2)) {
    w.setBlock(p, extended, 2);
    return false;
  }
  if (!power && type == 0)
    return false;
  bool isSticky = named(s, "sticky_piston");
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
              state(isSticky ? "sticky_piston" : "piston")
                  .with("facing", dirName(saved))
                  .with("extended", "false"),
              f, false, true);
    w.updateNeighborsAt(p, m.name);
    w.updateNeighbourShapes(p, m, 2, 512);
    if (isSticky) {
      auto two = relative(p, f, 2);
      auto moved = w.getBlock(two);
      auto e = entityAt(w, two);
      bool piece = named(moved, "moving_piston") && e && e->direction == f &&
                   e->extending;
      if (piece)
        finalTick(w, e);
      else if (type != 1 || air(moved) ||
               !pistonIsPushable(w, two, moved, opposite(f), false, f) ||
               (moved.get("push_reaction", blockMetadata(moved).pushReaction) !=
                    "normal" &&
                !base(moved)))
        w.setBlock(arm, state("air"), 3);
      else
        moveBlocks(w, p, f, false, true);
    } else
      w.setBlock(arm, state("air"), 3);
  }
  // TODO: piston sounds and block activation/deactivation game events.
  return true;
}
void pistonTickBlockEntities(World &w) {
  auto snapshot = w.pistonEntityOrder;
  for (auto &entry : snapshot) {
    auto e = entry->entity.lock();
    if (!e || entityAt(w, e->pos) != e || !w.isLoaded(e->pos))
      continue;
    e->lastTicked = w.gameTime();
    e->progressO = e->progress;
    if (e->progressO >= 1) {
      eraseEntity(w, e->pos);
      if (w.traceUpdates)
        w.log.push_back({"piston_entity_remove", e->pos, ""});
      if (w.pistonCompletionObserver)
        w.pistonCompletionObserver(w, e->pos);
      if (named(w.getBlock(e->pos), "moving_piston")) {
        if (w.traceUpdates)
          w.log.push_back({"piston_shape_resolve", e->pos, ""});
        auto s = w.updateFromNeighbourShapes(e->pos, e->movedState);
        if (air(s)) {
          if (w.traceUpdates)
            w.log.push_back({"piston_set_block", e->pos, "340"});
          w.setBlock(e->pos, e->movedState, 340);
          w.updateOrDestroy(e->pos, e->movedState, s, 3, 512);
        } else {
          if (s.get("waterlogged") == "true")
            s = s.with("waterlogged", "false");
          if (w.traceUpdates)
            w.log.push_back({"piston_set_block", e->pos, "67"});
          w.setBlock(e->pos, s, 67);
          if (w.traceUpdates)
            w.log.push_back({"piston_self_notify", e->pos, s.name});
          w.neighborChanged(e->pos, s.name);
        }
      }
    } else {
      float next = std::min(1.0f, e->progress + 0.5f);
      w.log.push_back(
          {"todo", e->pos,
           "piston entity collision, slime bounce, honey carry, and "
           "piston-base displacement"}); // TODO: implement entity collision
                                         // paths.
      e->progress = next;
    }
  }
  std::erase_if(w.pistonEntityOrder, [&](const auto &entry) {
    auto e = entry->entity.lock();
    return !e || entityAt(w, e->pos) != e;
  });
}
void pistonPreRemoveSideEffects(World &w, Pos p) {
  finalTick(w, entityAt(w, p));
}
void pistonForgetWorld(World &w) {
  w.pistonEntities.clear();
  w.pistonTickerSlots.clear();
  w.pistonEntityOrder.clear();
}
} // namespace redstone
