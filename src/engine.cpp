#include "redstone/engine.hpp"
#include "redstone/piston.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <bit>
#include <tuple>
#include <utility>

namespace redstone {
namespace {
// Java long arithmetic wraps modulo 2^64, including subTickCount++.
std::int64_t nextSubTick(std::int64_t value) {
  auto bits = std::bit_cast<std::uint64_t>(value);
  return std::bit_cast<std::int64_t>(bits + std::uint64_t{1});
}
constexpr std::array<Direction, 6> neighborOrder{
    Direction::West, Direction::East,  Direction::Down,
    Direction::Up,   Direction::North, Direction::South};
constexpr std::array<Direction, 6> shapeOrder{
    Direction::West,  Direction::East, Direction::North,
    Direction::South, Direction::Down, Direction::Up};
std::string canonical(std::string n) {
  if (n.find(':') == std::string::npos)
    n = "minecraft:" + n;
  return n;
}
bool named(const BlockState &s, const char *n) {
  return canonical(s.name) == std::string("minecraft:") + n;
}
bool fittingBase(const BlockState &head, const BlockState &base) {
  return (head.get("type", "normal") == "sticky" ? named(base, "sticky_piston")
                                                 : named(base, "piston")) &&
         base.get("extended", "false") == "true" &&
         base.get("facing", "north") == head.get("facing", "north");
}
bool piston(const BlockState &s) {
  return named(s, "piston") || named(s, "sticky_piston");
}
int chunk(int v) { return v >= 0 ? v / 16 : -1 - ((-1 - v) / 16); }
} // namespace
Direction opposite(Direction d) {
  switch (d) {
  case Direction::Down:
    return Direction::Up;
  case Direction::Up:
    return Direction::Down;
  case Direction::North:
    return Direction::South;
  case Direction::South:
    return Direction::North;
  case Direction::West:
    return Direction::East;
  case Direction::East:
    return Direction::West;
  }
  return Direction::Down;
}
Direction directionFromString(const std::string &s) {
  if (s == "up")
    return Direction::Up;
  if (s == "north")
    return Direction::North;
  if (s == "south")
    return Direction::South;
  if (s == "west")
    return Direction::West;
  if (s == "east")
    return Direction::East;
  return Direction::Down;
}
std::string directionName(Direction d) {
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
  case Direction::East:
    return "east";
  }
  return "down";
}
Pos Pos::relative(Direction d) const {
  Pos p = *this;
  switch (d) {
  case Direction::Down:
    --p.y;
    break;
  case Direction::Up:
    ++p.y;
    break;
  case Direction::North:
    --p.z;
    break;
  case Direction::South:
    ++p.z;
    break;
  case Direction::West:
    --p.x;
    break;
  case Direction::East:
    ++p.x;
    break;
  }
  return p;
}
std::string BlockState::get(const std::string &key,
                            const std::string &fallback) const {
  auto it = properties.find(key);
  return it == properties.end() ? fallback : it->second;
}
BlockState BlockState::with(const std::string &key,
                            const std::string &value) const {
  auto copy = *this;
  copy.properties[key] = value;
  return copy;
}
const std::vector<BlockRegistration> &blockRegistry() {
  static const std::vector<BlockRegistration> entries = {
      {"minecraft:air", {0, false, "destroy", true}},
      {"minecraft:grass_block", {0.6f, true, "normal", true}},
      {"minecraft:bedrock", {-1, true, "block", true}},
      {"minecraft:slime_block", {0, true, "normal", true}},
      {"minecraft:honey_block", {0, false, "normal", true}},
      {"minecraft:smooth_stone", {2, true, "normal", true}},
      {"minecraft:white_terracotta", {1.25f, true, "normal", true}},
      {"minecraft:dead_tube_coral_wall_fan", {0, false, "normal", true}},
      {"minecraft:dead_brain_coral_wall_fan", {0, false, "normal", true}},
      {"minecraft:dead_bubble_coral_wall_fan", {0, false, "normal", true}},
      {"minecraft:dead_fire_coral_wall_fan", {0, false, "normal", true}},
      {"minecraft:dead_horn_coral_wall_fan", {0, false, "normal", true}},
      {"minecraft:dead_tube_coral_fan", {0, false, "normal", true}},
      {"minecraft:dead_brain_coral_fan", {0, false, "normal", true}},
      {"minecraft:dead_bubble_coral_fan", {0, false, "normal", true}},
      {"minecraft:dead_fire_coral_fan", {0, false, "normal", true}},
      {"minecraft:dead_horn_coral_fan", {0, false, "normal", true}},
      {"minecraft:cobblestone_wall", {2, false, "normal", true}},
      {"minecraft:obsidian", {50, true, "block", true}},
      {"minecraft:glass", {0.3f, false, "normal", true}},
      {"minecraft:piston", {1.5f, true, "normal", true}},
      {"minecraft:sticky_piston", {1.5f, true, "normal", true}},
      {"minecraft:piston_head", {1.5f, false, "block", true}},
      {"minecraft:moving_piston", {-1, false, "block", true}},
      {"minecraft:observer", {3, false, "normal", true}},
      {"minecraft:note_block", {0.8f, true, "normal", true}},
      {"minecraft:redstone_block", {5, false, "normal", true}},
      {"minecraft:white_carpet", {0.1f, false, "normal", true}},
      {"minecraft:orange_carpet", {0.1f, false, "normal", true}},
      {"minecraft:magenta_carpet", {0.1f, false, "normal", true}},
      {"minecraft:light_blue_carpet", {0.1f, false, "normal", true}},
      {"minecraft:yellow_carpet", {0.1f, false, "normal", true}},
      {"minecraft:lime_carpet", {0.1f, false, "normal", true}},
      {"minecraft:pink_carpet", {0.1f, false, "normal", true}},
      {"minecraft:gray_carpet", {0.1f, false, "normal", true}},
      {"minecraft:light_gray_carpet", {0.1f, false, "normal", true}},
      {"minecraft:cyan_carpet", {0.1f, false, "normal", true}},
      {"minecraft:purple_carpet", {0.1f, false, "normal", true}},
      {"minecraft:blue_carpet", {0.1f, false, "normal", true}},
      {"minecraft:brown_carpet", {0.1f, false, "normal", true}},
      {"minecraft:green_carpet", {0.1f, false, "normal", true}},
      {"minecraft:red_carpet", {0.1f, false, "normal", true}},
      {"minecraft:black_carpet", {0.1f, false, "normal", true}},
      {"minecraft:tnt", {0, false, "normal", true}}};
  return entries;
}
bool isSupportedBlock(const std::string &name) {
  const auto n = canonical(name);
  const auto &registry = blockRegistry();
  return std::any_of(registry.begin(), registry.end(),
                     [&](const auto &entry) { return entry.name == n; });
}
bool isWoolCarpet(const BlockState &state) {
  const auto name = canonical(state.name);
  return name.ends_with("_carpet") && isSupportedBlock(name);
}
bool isDeadCoralWallFan(const BlockState &state) {
  return isSupportedBlock(state.name) &&
         canonical(state.name).ends_with("_coral_wall_fan");
}
bool isDeadCoralFan(const BlockState &state) {
  return isDeadCoralWallFan(state) ||
         (isSupportedBlock(state.name) &&
          canonical(state.name).ends_with("_coral_fan"));
}
const std::vector<EntityType> &entityRegistry() {
  static const std::vector<EntityType> entries = {
      {"minecraft:item", false},
      {"minecraft:tnt", false},
      {"minecraft:falling_block", false}};
  return entries;
}
BlockMetadata blockMetadata(const BlockState &s) {
  const auto n = canonical(s.name);
  const auto &registry = blockRegistry();
  auto found = std::find_if(registry.begin(), registry.end(),
                            [&](const auto &entry) { return entry.name == n; });
  if (found != registry.end()) {
    auto metadata = found->metadata;
    if (piston(s) && s.get("extended", "false") == "true")
      metadata.conductor = false;
    return metadata;
  }
  // Unknown states remain serializable, but no behavioral metadata is inferred.
  return {0, false, "unsupported", false};
}
BlockState defaultState(const std::string &name) {
  BlockState s(canonical(name));
  if (piston(s))
    s.properties = {{"facing", "north"}, {"extended", "false"}};
  else if (named(s, "observer"))
    s.properties = {{"facing", "south"}, {"powered", "false"}};
  else if (named(s, "piston_head"))
    s.properties = {
        {"facing", "north"}, {"short", "false"}, {"type", "normal"}};
  else if (named(s, "moving_piston"))
    s.properties = {{"facing", "north"}, {"type", "normal"}};
  else if (named(s, "note_block"))
    s.properties = {
        {"instrument", "harp"}, {"note", "0"}, {"powered", "false"}};
  else if (named(s, "tnt"))
    s.properties = {{"unstable", "false"}};
  else if (named(s, "grass_block"))
    s.properties = {{"snowy", "false"}};
  else if (named(s, "cobblestone_wall"))
    s.properties = {{"east", "none"},         {"north", "none"},
                    {"south", "none"},        {"up", "true"},
                    {"waterlogged", "false"}, {"west", "none"}};
  else if (isDeadCoralWallFan(s))
    s.properties = {{"facing", "north"}, {"waterlogged", "true"}};
  else if (isDeadCoralFan(s))
    s.properties = {{"waterlogged", "true"}};
  return s;
}
World::World(int minY, int maxY, int maxNeighborUpdates)
    : minY_(minY), maxY_(maxY), maxUpdates_(maxNeighborUpdates) {}
void World::setPistonEntity(Pos pos,
                            std::shared_ptr<PistonMovingBlockEntity> entity) {
  pistonEntities[pos] = entity;
  auto it = pistonTickerSlots.find(pos);
  if (it != pistonTickerSlots.end()) {
    it->second->entity = entity;
  } else {
    auto slot = std::make_shared<PistonTickerSlot>();
    slot->pos = pos;
    slot->entity = entity;
    pistonTickerSlots[pos] = slot;
    pistonEntityOrder.push_back(slot);
  }
}
void World::removePistonEntity(Pos pos) {
  pistonEntities.erase(pos);
  auto it = pistonTickerSlots.find(pos);
  if (it != pistonTickerSlots.end()) {
    it->second->entity.reset();
    pistonTickerSlots.erase(it);
  }
}
void World::loadChunk(int x, int z) {
  chunks_.try_emplace(std::make_pair(x, z)).first->second.loaded = true;
}
void World::unloadChunk(int x, int z) {
  auto it = chunks_.find({x, z});
  if (it != chunks_.end())
    it->second.loaded = false;
}
bool World::isChunkLoaded(int x, int z) const {
  auto it = chunks_.find({x, z});
  return it != chunks_.end() && it->second.loaded;
}
bool World::isLoaded(Pos p) const {
  return p.y >= minY_ && p.y < maxY_ && isChunkLoaded(chunk(p.x), chunk(p.z));
}
BlockState World::getBlock(Pos p) const {
  if (!isLoaded(p))
    return defaultState("air");
  const auto &c = chunks_.at({chunk(p.x), chunk(p.z)});
  auto si = c.sections.find(chunk(p.y));
  if (si == c.sections.end())
    return defaultState("air");
  int idx = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  auto it = si->second.states.find(idx);
  return it == si->second.states.end() ? defaultState("air") : it->second;
}
void World::rawSetBlock(Pos p, BlockState state) {
  if (!isLoaded(p))
    return;
  auto &c = chunks_.at({chunk(p.x), chunk(p.z)});
  auto sy = chunk(p.y);
  int idx = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  if (named(state, "air")) {
    auto it = c.sections.find(sy);
    if (it != c.sections.end()) {
      it->second.states.erase(idx);
      if (it->second.states.empty())
        c.sections.erase(it);
    }
  } else {
    state.name = canonical(state.name);
    c.sections[sy].states[idx] = std::move(state);
  }
}
std::map<Pos, BlockState> World::allBlocks() const {
  std::map<Pos, BlockState> result;
  for (const auto &[cp, c] : chunks_)
    for (const auto &[sy, sec] : c.sections)
      for (const auto &[i, s] : sec.states)
        result.emplace(Pos{cp.first * 16 + (i & 15), sy * 16 + ((i >> 8) & 15),
                           cp.second * 16 + ((i >> 4) & 15)},
                       s);
  return result;
}
bool World::setBlock(Pos p, BlockState state, int flags, int limit) {
  if (!isLoaded(p))
    return false;
  state.name = canonical(state.name);
  auto old = getBlock(p);
  if (old == state)
    return false;
  rawSetBlock(p, state);
  // TODO: reproduce chunk heightmaps, lighting, dirty/client notifications,
  // POI, and generic block-entity pre-removal/creation for supported entity
  // blocks.
  if (old.name != state.name && named(old, "moving_piston")) {
    if (!(flags & 256))
      pistonPreRemoveSideEffects(*this, p);
    removePistonEntity(p);
  }
  if (old.name != state.name && (flags & (1 | 64)))
    affectNeighborsAfterRemoval(p, old, (flags & 64) != 0);
  if (getBlock(p).name != state.name)
    return false;
  if (!(flags & 512)) {
    if (named(state, "observer") && old.name != state.name &&
        state.get("powered", "false") == "true" &&
        !hasScheduledTick(p, state.name)) {
      setBlock(p, state.with("powered", "false"), 18);
      observerNotify(p, state.with("powered", "false"));
    }
    if (named(state, "tnt") && old.name != state.name && hasNeighborSignal(p) &&
        tntExplodes) {
      log.push_back({"entity_spawn", p, "minecraft:tnt"});
      setBlock(p, defaultState("air"), 3);
    }
    if (piston(state))
      pistonOnPlace(*this, p, state, old);
  }
  if (getBlock(p) == state) {
    if (flags & 1)
      updateNeighborsAt(p, old.name);
    if (!(flags & 16) && limit > 0) {
      updateIndirectNeighbourShapes(p, old, flags & ~33, limit - 1);
      updateNeighbourShapes(p, state, flags & ~33, limit - 1);
      updateIndirectNeighbourShapes(p, state, flags & ~33, limit - 1);
    }
  }
  return true;
}
std::optional<BlockState> World::coralPlacementState(Pos pos,
                                                     const BlockState &selected,
                                                     Direction clickedFace) {
  if (!isDeadCoralFan(selected) || clickedFace == Direction::Down)
    return std::nullopt;
  auto name = canonical(selected.name);
  const auto wall = name.find("_coral_wall_fan");
  if (wall != std::string::npos)
    name.replace(wall, 15, "_coral_fan");
  if (clickedFace != Direction::Up) {
    auto floor = name.find("_coral_fan");
    name.replace(floor, 10, "_coral_wall_fan");
  }
  auto state = defaultState(name).with("waterlogged", "false");
  if (clickedFace != Direction::Up)
    state = state.with("facing", directionName(clickedFace));
  return coralCanSurvive(*this, pos, state) ? std::optional<BlockState>(state)
                                            : std::nullopt;
}
BlockState World::updateShapeState(Pos p, const BlockState &state,
                                   Direction direction, Pos neighborPos,
                                   const BlockState &neighbor) {
  if (isWoolCarpet(state)) {
    auto below = getBlock(p.relative(Direction::Down));
    if (named(below, "air") || named(below, "cave_air") ||
        named(below, "void_air"))
      return defaultState("air");
  }
  if (isDeadCoralFan(state)) {
    const auto supportDirection =
        isDeadCoralWallFan(state)
            ? opposite(directionFromString(state.get("facing", "north")))
            : Direction::Down;
    if (direction == supportDirection && !coralCanSurvive(*this, p, state))
      return defaultState("air");
  }
  if (named(state, "grass_block") && direction == Direction::Up) {
    const bool snowy = named(neighbor, "snow") ||
                       named(neighbor, "snow_block") ||
                       named(neighbor, "powder_snow");
    return state.with("snowy", snowy ? "true" : "false");
  }
  // Water simulation is outside the current scope; waterlogged values persist.
  if (named(state, "cobblestone_wall") && direction != Direction::Down)
    return wallUpdateShape(*this, p, state, direction, neighbor);
  if (named(state, "piston_head") &&
      opposite(direction) ==
          directionFromString(state.get("facing", "north"))) {
    const auto base = getBlock(neighborPos);
    if (!fittingBase(state, base) &&
        !(named(base, "moving_piston") &&
          base.get("facing", "north") == state.get("facing", "north")))
      return defaultState("air");
  }
  if (named(state, "observer") &&
      directionFromString(state.get("facing", "south")) == direction &&
      state.get("powered", "false") == "false" &&
      !hasScheduledTick(p, state.name))
    scheduleTick(p, state.name, 2);
  if (named(state, "note_block") &&
      (direction == Direction::Up || direction == Direction::Down)) {
    const auto below = getBlock(p.relative(Direction::Down));
    std::string instrument = "harp";
    if (named(below, "glass"))
      instrument = "hat";
    else if (named(below, "smooth_stone") || named(below, "cobblestone_wall") ||
             named(below, "white_terracotta") || named(below, "bedrock") ||
             named(below, "obsidian") || named(below, "observer") ||
             isDeadCoralFan(below))
      instrument = "basedrum";
    else if (named(below, "note_block"))
      instrument = "bass";
    // TODO: unknown instrument overrides and skull-backed sounds need registry
    // data.
    return state.with("instrument", instrument);
  }
  // TODO: unsupported blocks retain their stored state; their updateShape is
  // unknown.
  return state;
}
BlockState World::updateFromNeighbourShapes(Pos p, const BlockState &input) {
  auto state = input;
  for (auto d : shapeOrder) {
    auto neighborPos = p.relative(d);
    state = updateShapeState(p, state, d, neighborPos, getBlock(neighborPos));
  }
  return state;
}
void World::updateOrDestroy(Pos p, const BlockState &oldState,
                            const BlockState &newState, int flags, int limit) {
  if (oldState == newState)
    return;
  if (named(newState, "air")) {
    // Level.destroyBlock resolves the actual current state, which can differ
    // from the earlier shape result after nested piston callbacks.
    auto current = getBlock(p);
    if (named(current, "air") || named(current, "cave_air") ||
        named(current, "void_air"))
      return;
    if (!(flags & 32) && isWoolCarpet(current))
      log.push_back({"entity_spawn", p, "minecraft:item:" + current.name});
    // TODO: general loot-table evaluation and game events.
    setBlock(p, defaultState("air"), 3, limit);
  } else
    setBlock(p, newState, flags & ~32, limit);
}
void World::neighborShapeChanged(Pos p, Direction d, Pos neighbor,
                                 const BlockState &state, int flags,
                                 int limit) {
  shapeUpdate(p, d, neighbor, state, flags, limit);
}
void World::updateNeighbourShapes(Pos p, const BlockState &state, int flags,
                                  int limit) {
  for (auto d : shapeOrder)
    shapeUpdate(p.relative(d), opposite(d), p, state, flags, limit);
}
void World::updateIndirectNeighbourShapes(
    Pos, const BlockState &, int,
    int) { /* No supported block currently overrides this hook. */ }
void World::affectNeighborsAfterRemoval(Pos p, const BlockState &state, bool) {
  if (named(state, "piston_head")) {
    auto q =
        p.relative(opposite(directionFromString(state.get("facing", "north"))));
    if (fittingBase(state, getBlock(q))) {
      log.push_back({"entity_spawn", q, "minecraft:item:" + getBlock(q).name});
      setBlock(q, defaultState("air"), 3);
    }
  }
  if (named(state, "observer") && state.get("powered", "false") == "true" &&
      hasScheduledTick(p, state.name))
    observerNotify(p, state.with("powered", "false"));
}
void World::neighborChanged(Pos p, const std::string &source) {
  Update u{};
  u.kind = Update::Kind::Simple;
  u.pos = p;
  u.source = source;
  enqueue(std::move(u));
}
void World::neighborChanged(const BlockState &state, Pos p,
                            const std::string &source, bool movedByPiston) {
  Update u{};
  u.kind = Update::Kind::Full;
  u.pos = p;
  u.source = source;
  u.capturedState = state;
  u.movedByPiston = movedByPiston;
  enqueue(std::move(u));
}
void World::updateNeighborsAt(Pos p, const std::string &source,
                              std::optional<Direction> skip) {
  Update u{};
  u.kind = Update::Kind::Multi;
  u.pos = p;
  u.source = source;
  u.skip = skip;
  enqueue(std::move(u));
}
void World::shapeUpdate(Pos p, Direction d, Pos neighborPos,
                        const BlockState &neighbor, int flags, int limit) {
  Update u{};
  u.kind = Update::Kind::Shape;
  u.pos = p;
  u.direction = d;
  u.neighborPos = neighborPos;
  u.neighbor = neighbor;
  u.flags = flags;
  u.limit = limit;
  enqueue(std::move(u));
}
void World::enqueue(Update u) {
  bool running = updateCount_ > 0;
  bool tooMany = maxUpdates_ >= 0 && updateCount_ >= maxUpdates_;
  ++updateCount_;
  if (!tooMany) {
    if (running)
      added_.push_back(std::move(u));
    else
      stack_.push_back(std::move(u));
  } else if (updateCount_ - 1 == maxUpdates_)
    log.push_back({"neighbor_update_limit", u.pos, ""});
  if (!running)
    runUpdates();
}
void World::runUpdates() {
  try {
    while (!stack_.empty() || !added_.empty()) {
      for (auto it = added_.rbegin(); it != added_.rend(); ++it)
        stack_.push_back(std::move(*it));
      added_.clear();
      while (added_.empty()) {
        bool more = runNext(stack_.back());
        if (!more) {
          stack_.pop_back();
          break;
        }
      }
    }
  } catch (...) {
    stack_.clear();
    added_.clear();
    updateCount_ = 0;
    throw;
  }
  stack_.clear();
  added_.clear();
  updateCount_ = 0;
}
bool World::runNext(Update &u) {
  if (u.kind == Update::Kind::Simple) {
    executeNeighbor(u.pos, getBlock(u.pos), u.source, false);
    return false;
  }
  if (u.kind == Update::Kind::Full) {
    executeNeighbor(u.pos, u.capturedState, u.source, u.movedByPiston);
    return false;
  }
  if (u.kind == Update::Kind::Multi) {
    while (u.index < 6 && u.skip && neighborOrder[u.index] == *u.skip)
      ++u.index;
    if (u.index >= 6)
      return false;
    auto p = u.pos.relative(neighborOrder[u.index++]);
    auto src = u.source;
    while (u.index < 6 && u.skip && neighborOrder[u.index] == *u.skip)
      ++u.index;
    bool more = u.index < 6;
    executeNeighbor(p, getBlock(p), src, false);
    return more;
  }
  auto state = getBlock(u.pos);
  if (traceUpdates)
    log.push_back({"shape", u.pos, directionName(u.direction)});
  auto next =
      updateShapeState(u.pos, state, u.direction, u.neighborPos, u.neighbor);
  updateOrDestroy(u.pos, state, next, u.flags, u.limit);
  return false;
}
void World::executeNeighbor(Pos p, const BlockState &state,
                            const std::string &source, bool movedByPiston) {
  // TODO: orientation and movedByPiston for unsupported blocks.
  (void)movedByPiston;
  if (traceUpdates)
    log.push_back({"neighbor", p, source});
  if (piston(state))
    pistonNeighborChanged(*this, p, state);
  else if (named(state, "piston_head")) {
    auto d = directionFromString(state.get("facing", "north"));
    auto basePos = p.relative(opposite(d));
    auto base = getBlock(basePos);
    if (fittingBase(state, base) ||
        (named(base, "moving_piston") &&
         base.get("facing", "north") == state.get("facing", "north")))
      neighborChanged(basePos, source);
  } else if (named(state, "tnt") && hasNeighborSignal(p) && tntExplodes) {
    log.push_back({"entity_spawn", p, "minecraft:tnt"});
    setBlock(p, defaultState("air"), 3);
  } else if (named(state, "note_block")) {
    bool signal = hasNeighborSignal(p),
         powered = state.get("powered", "false") == "true";
    if (signal != powered) {
      if (signal && named(getBlock(p.relative(Direction::Up)), "air"))
        blockEvent(p, state.name, 0, 0);
      setBlock(p, state.with("powered", signal ? "true" : "false"), 3);
    }
  }
}
int World::getSignal(Pos p, Direction d) const {
  auto s = getBlock(p);
  if (named(s, "redstone_block"))
    return 15;
  if (named(s, "observer") && s.get("powered", "false") == "true" &&
      directionFromString(s.get("facing", "south")) == d)
    return 15;
  // TODO: generic block registries and redstone wire direct signals.
  if (blockMetadata(s).conductor) {
    for (auto n : neighborOrder) {
      auto q = p.relative(n);
      auto x = getBlock(q);
      if (named(x, "observer") && x.get("powered", "false") == "true" &&
          directionFromString(x.get("facing", "south")) == n)
        return 15;
    }
  }
  return 0;
}
bool World::hasNeighborSignal(Pos p) const {
  for (auto d : neighborOrder)
    if (getSignal(p.relative(d), d) > 0)
      return true;
  return false;
}
void World::scheduleTick(Pos p, const std::string &block, int delay,
                         int priority) {
  if (!isLoaded(p) || hasScheduledTick(p, canonical(block)))
    return;
  // Loaded ticks may have been restored directly by the storage adapter.
  for (const auto &tick : scheduledTicks)
    if (tick.order >= order_)
      order_ = nextSubTick(tick.order);
  const auto order = order_;
  order_ = nextSubTick(order_);
  scheduledTicks.push_back(
      {p, canonical(block), time_ + delay, priority, order});
}
bool World::hasScheduledTick(Pos p, const std::string &block) const {
  return std::any_of(
      scheduledTicks.begin(), scheduledTicks.end(), [&](const auto &t) {
        return t.pos == p && canonical(t.block) == canonical(block);
      });
}
void World::blockEvent(Pos p, const std::string &block, int type, int data) {
  BlockEvent e{p, canonical(block), type, data};
  if (std::find(blockEvents.begin(), blockEvents.end(), e) == blockEvents.end())
    blockEvents.push_back(e);
}
void World::observerNotify(Pos p, const BlockState &s) {
  auto d = directionFromString(s.get("facing", "south"));
  auto q = p.relative(opposite(d));
  neighborChanged(q, s.name);
  updateNeighborsAt(q, s.name, d);
}
void World::runScheduled(const ScheduledTick &t) {
  if (traceUpdates)
    log.push_back({"scheduled_tick", t.pos, t.block});
  auto s = getBlock(t.pos);
  if (s.name != t.block)
    return;
  if (named(s, "observer")) {
    bool on = s.get("powered", "false") == "true";
    setBlock(t.pos, s.with("powered", on ? "false" : "true"), 2);
    if (!on)
      scheduleTick(t.pos, s.name, 2);
    observerNotify(t.pos, s);
  }
}
// TODO: note particles, sound, game events, skull sounds and event broadcasts.
void World::runEvents() {
  std::vector<BlockEvent> deferred;
  while (!blockEvents.empty()) {
    auto e = blockEvents.front();
    blockEvents.erase(blockEvents.begin());
    if (!isLoaded(e.pos)) {
      deferred.push_back(e);
      continue;
    }
    if (traceUpdates)
      log.push_back({"block_event", e.pos, e.block});
    auto s = getBlock(e.pos);
    if (s.name != e.block)
      continue;
    if (piston(s))
      pistonBlockEvent(*this, e.pos, s, e.type, e.data);
    else if (named(s, "note_block"))
      log.push_back({"note", e.pos,
                     s.get("instrument", "harp") + ":" + s.get("note", "0")});
  }
  for (auto &e : deferred)
    if (std::find(blockEvents.begin(), blockEvents.end(), e) ==
        blockEvents.end())
      blockEvents.push_back(e);
}
// TODO: random ticks, fluid ticks, entity processing and world systems are
// absent.
void World::tick() {
  ++time_;
  for (const auto &tick : scheduledTicks)
    if (tick.order >= order_)
      order_ = nextSubTick(tick.order);
  handlingTick_ = true;
  std::vector<ScheduledTick> due;
  // Each chunk exposes its earliest tick by DRAIN_ORDER. Eligible chunk heads
  // compete by INTRA_TICK_DRAIN_ORDER, as in LevelTicks.drainContainers.
  while (due.size() < 65536) {
    std::map<std::pair<int, int>, std::size_t> heads;
    for (std::size_t i = 0; i < scheduledTicks.size(); ++i) {
      const auto &t = scheduledTicks[i];
      if (!isLoaded(t.pos))
        continue;
      auto key = std::make_pair(chunk(t.pos.x), chunk(t.pos.z));
      auto [it, inserted] = heads.emplace(key, i);
      if (!inserted) {
        const auto &old = scheduledTicks[it->second];
        if (std::tie(t.trigger, t.priority, t.order) <
            std::tie(old.trigger, old.priority, old.order))
          it->second = i;
      }
    }
    std::optional<std::size_t> best;
    for (const auto &[key, i] : heads) {
      const auto &t = scheduledTicks[i];
      if (t.trigger > time_)
        continue;
      if (!best || std::tie(t.priority, t.order) <
                       std::tie(scheduledTicks[*best].priority,
                                scheduledTicks[*best].order))
        best = i;
    }
    if (!best)
      break;
    due.push_back(scheduledTicks[*best]);
    scheduledTicks.erase(scheduledTicks.begin() + *best);
  }
  for (auto &t : due)
    runScheduled(t);
  runEvents();
  handlingTick_ = false;
  if (traceUpdates)
    log.push_back({"block_entities", {}, ""});
  pistonTickBlockEntities(*this);
  if (blockEntityTicker)
    blockEntityTicker(*this);
}
} // namespace redstone
