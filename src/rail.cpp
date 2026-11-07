#include "redstone/rail.hpp"
#include "redstone/entity.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace redstone {
namespace {
std::string canonical(std::string n) {
  if (n.find(':') == std::string::npos)
    n = "minecraft:" + n;
  return n;
}
bool air(const BlockState &s) {
  return s.isAir();
}
bool straightRail(const BlockState &s) {
  return !s.is(Blocks::RAIL);
}
bool sameRailType(const BlockState &a, const BlockState &b) {
  return a.sameBlock(b);
}
bool fullSupport(World &world, Pos pos) {
  auto state = world.getBlock(pos);
  if (!world.isLoaded(pos) || air(state) || isLiquidBlock(state) ||
      isRailBlock(state))
    return false;
  VoxelShape movingShape;
  const auto &shape = state.is(Blocks::MOVING_PISTON)
                          ? (movingShape = collisionShape(world, pos, state))
                          : collisionShape(state);
  if (shape.empty())
    return false;
  if (shape.size() == 1) {
    const auto &part = shape.front();
    return part.y0 < 16 && part.y1 >= 16 && part.x0 <= 0 &&
           part.x1 >= 16 && part.z0 <= 0 && part.z1 >= 16;
  }
  // SupportType.RIGID checks the top face's outer 2-pixel ring, not the
  // volume of the block. The full top of an upward piston head and of a
  // downward-facing extended piston base can support rails.
  std::vector<double> xs{0, 2, 14, 16}, zs{0, 2, 14, 16};
  for (const auto &part : shape) {
    if (part.y0 < 16 && part.y1 >= 16) {
      xs.push_back(std::clamp(part.x0, 0.0, 16.0));
      xs.push_back(std::clamp(part.x1, 0.0, 16.0));
      zs.push_back(std::clamp(part.z0, 0.0, 16.0));
      zs.push_back(std::clamp(part.z1, 0.0, 16.0));
    }
  }
  std::sort(xs.begin(), xs.end());
  std::sort(zs.begin(), zs.end());
  for (std::size_t i = 1; i < xs.size(); ++i)
    for (std::size_t j = 1; j < zs.size(); ++j) {
      if (xs[i - 1] == xs[i] || zs[j - 1] == zs[j])
        continue;
      double x = (xs[i - 1] + xs[i]) * 0.5;
      double z = (zs[j - 1] + zs[j]) * 0.5;
      if (x > 2 && x < 14 && z > 2 && z < 14)
        continue;
      if (std::none_of(shape.begin(), shape.end(), [&](const auto &part) {
            return part.y0 < 16 && part.y1 >= 16 && part.x0 <= x &&
                   part.x1 >= x && part.z0 <= z && part.z1 >= z;
          }))
        return false;
    }
  return true;
}
bool signalSource(const BlockState &state) {
  return state.is(Blocks::DETECTOR_RAIL) || state.is(Blocks::OBSERVER) ||
         state.is(Blocks::REDSTONE_BLOCK);
}
bool ascending(RailShape shape) { return railShapeIsSlope(shape); }

class RailState {
public:
  RailState(World &world, Pos pos, BlockState state)
      : world_(world), pos_(pos), state_(std::move(state)),
        straight_(straightRail(state_)) {
    updateConnections(railShapeOf(state_));
  }
  const std::vector<Pos> &connections() const { return connections_; }
  int countPotentialConnections() const {
    int count = 0;
    for (auto d : {Direction::North, Direction::South, Direction::West,
                   Direction::East})
      if (hasRail(pos_.relative(d)))
        ++count;
    return count;
  }
  BlockState place(bool hasSignal, bool first, RailShape defaultShape) {
    Pos north = pos_.relative(Direction::North);
    Pos south = pos_.relative(Direction::South);
    Pos west = pos_.relative(Direction::West);
    Pos east = pos_.relative(Direction::East);
    bool n = hasNeighborRail(north), s = hasNeighborRail(south),
         w = hasNeighborRail(west), e = hasNeighborRail(east);
    std::optional<RailShape> shape;
    bool northOrSouth = n || s, westOrEast = w || e;
    if (northOrSouth && !westOrEast)
      shape = RailShape::NorthSouth;
    if (westOrEast && !northOrSouth)
      shape = RailShape::EastWest;
    bool southAndEast = s && e, southAndWest = s && w,
         northAndEast = n && e, northAndWest = n && w;
    if (!straight_) {
      if (southAndEast && !n && !w)
        shape = RailShape::SouthEast;
      else if (southAndWest && !n && !e)
        shape = RailShape::SouthWest;
      else if (northAndWest && !s && !e)
        shape = RailShape::NorthWest;
      else if (northAndEast && !s && !w)
        shape = RailShape::NorthEast;
    }
    if (!shape) {
      if (northOrSouth && westOrEast)
        shape = defaultShape;
      else if (northOrSouth)
        shape = RailShape::NorthSouth;
      else if (westOrEast)
        shape = RailShape::EastWest;
      if (!straight_) {
        if (hasSignal) {
          if (southAndEast)
            shape = RailShape::SouthEast;
          if (southAndWest)
            shape = RailShape::SouthWest;
          if (northAndEast)
            shape = RailShape::NorthEast;
          if (northAndWest)
            shape = RailShape::NorthWest;
        } else {
          if (northAndWest)
            shape = RailShape::NorthWest;
          if (northAndEast)
            shape = RailShape::NorthEast;
          if (southAndWest)
            shape = RailShape::SouthWest;
          if (southAndEast)
            shape = RailShape::SouthEast;
        }
      }
    }
    if (!shape)
      shape = defaultShape;
    if (*shape == RailShape::NorthSouth) {
      if (isRailBlock(world_.getBlock(north.relative(Direction::Up))))
        shape = RailShape::AscendingNorth;
      if (isRailBlock(world_.getBlock(south.relative(Direction::Up))))
        shape = RailShape::AscendingSouth;
    }
    if (*shape == RailShape::EastWest) {
      if (isRailBlock(world_.getBlock(east.relative(Direction::Up))))
        shape = RailShape::AscendingEast;
      if (isRailBlock(world_.getBlock(west.relative(Direction::Up))))
        shape = RailShape::AscendingWest;
    }
    updateConnections(*shape);
    state_ = state_.with(PropertyKind::Shape, static_cast<std::uint8_t>(*shape));
    if (first || world_.getBlock(pos_) != state_) {
      world_.setBlock(pos_, state_, 3);
      for (auto connection : connections_) {
        auto neighbor = getRail(connection);
        if (!neighbor)
          continue;
        neighbor->removeSoftConnections();
        if (neighbor->canConnectTo(*this))
          neighbor->connectTo(*this);
      }
    }
    return state_;
  }

private:
  bool hasRail(Pos pos) const {
    return isRailBlock(world_.getBlock(pos)) ||
           isRailBlock(world_.getBlock(pos.relative(Direction::Up))) ||
           isRailBlock(world_.getBlock(pos.relative(Direction::Down)));
  }
  std::optional<RailState> getRail(Pos pos) const {
    auto state = world_.getBlock(pos);
    if (isRailBlock(state))
      return RailState(world_, pos, state);
    auto above = pos.relative(Direction::Up);
    state = world_.getBlock(above);
    if (isRailBlock(state))
      return RailState(world_, above, state);
    auto below = pos.relative(Direction::Down);
    state = world_.getBlock(below);
    if (isRailBlock(state))
      return RailState(world_, below, state);
    return std::nullopt;
  }
  bool hasConnection(Pos pos) const {
    for (auto connection : connections_)
      if (connection.x == pos.x && connection.z == pos.z)
        return true;
    return false;
  }
  bool connectsTo(const RailState &rail) const {
    return hasConnection(rail.pos_);
  }
  bool canConnectTo(const RailState &rail) const {
    return connectsTo(rail) || connections_.size() != 2;
  }
  void connectTo(RailState &rail) {
    connections_.push_back(rail.pos_);
    Pos north = pos_.relative(Direction::North);
    Pos south = pos_.relative(Direction::South);
    Pos west = pos_.relative(Direction::West);
    Pos east = pos_.relative(Direction::East);
    bool n = hasConnection(north), s = hasConnection(south),
         w = hasConnection(west), e = hasConnection(east);
    RailShape shape = RailShape::NorthSouth;
    if (n || s)
      shape = RailShape::NorthSouth;
    if (w || e)
      shape = RailShape::EastWest;
    if (!straight_) {
      if (s && e && !n && !w)
        shape = RailShape::SouthEast;
      if (s && w && !n && !e)
        shape = RailShape::SouthWest;
      if (n && w && !s && !e)
        shape = RailShape::NorthWest;
      if (n && e && !s && !w)
        shape = RailShape::NorthEast;
    }
    if (shape == RailShape::NorthSouth) {
      if (isRailBlock(world_.getBlock(north.relative(Direction::Up))))
        shape = RailShape::AscendingNorth;
      if (isRailBlock(world_.getBlock(south.relative(Direction::Up))))
        shape = RailShape::AscendingSouth;
    }
    if (shape == RailShape::EastWest) {
      if (isRailBlock(world_.getBlock(east.relative(Direction::Up))))
        shape = RailShape::AscendingEast;
      if (isRailBlock(world_.getBlock(west.relative(Direction::Up))))
        shape = RailShape::AscendingWest;
    }
    state_ = state_.with(PropertyKind::Shape, static_cast<std::uint8_t>(shape));
    world_.setBlock(pos_, state_, 3);
  }
  bool hasNeighborRail(Pos pos) {
    auto rail = getRail(pos);
    if (!rail)
      return false;
    rail->removeSoftConnections();
    return rail->canConnectTo(*this);
  }
  void removeSoftConnections() {
    for (size_t i = 0; i < connections_.size();) {
      auto rail = getRail(connections_[i]);
      if (rail && rail->connectsTo(*this))
        connections_[i++] = rail->pos_;
      else
        connections_.erase(connections_.begin() + i);
    }
  }
  void updateConnections(RailShape shape) {
    connections_.clear();
    switch (shape) {
    case RailShape::NorthSouth:
      connections_.push_back(pos_.relative(Direction::North));
      connections_.push_back(pos_.relative(Direction::South));
      break;
    case RailShape::EastWest:
      connections_.push_back(pos_.relative(Direction::West));
      connections_.push_back(pos_.relative(Direction::East));
      break;
    case RailShape::AscendingEast:
      connections_.push_back(pos_.relative(Direction::West));
      connections_.push_back(
          pos_.relative(Direction::East).relative(Direction::Up));
      break;
    case RailShape::AscendingWest:
      connections_.push_back(
          pos_.relative(Direction::West).relative(Direction::Up));
      connections_.push_back(pos_.relative(Direction::East));
      break;
    case RailShape::AscendingNorth:
      connections_.push_back(
          pos_.relative(Direction::North).relative(Direction::Up));
      connections_.push_back(pos_.relative(Direction::South));
      break;
    case RailShape::AscendingSouth:
      connections_.push_back(pos_.relative(Direction::North));
      connections_.push_back(
          pos_.relative(Direction::South).relative(Direction::Up));
      break;
    case RailShape::SouthEast:
      connections_.push_back(pos_.relative(Direction::East));
      connections_.push_back(pos_.relative(Direction::South));
      break;
    case RailShape::SouthWest:
      connections_.push_back(pos_.relative(Direction::West));
      connections_.push_back(pos_.relative(Direction::South));
      break;
    case RailShape::NorthWest:
      connections_.push_back(pos_.relative(Direction::West));
      connections_.push_back(pos_.relative(Direction::North));
      break;
    case RailShape::NorthEast:
      connections_.push_back(pos_.relative(Direction::East));
      connections_.push_back(pos_.relative(Direction::North));
      break;
    }
  }

  World &world_;
  Pos pos_;
  BlockState state_;
  bool straight_;
  std::vector<Pos> connections_;
};

bool shouldBeRemoved(World &world, Pos pos, RailShape shape) {
  if (!fullSupport(world, pos.relative(Direction::Down)))
    return true;
  switch (shape) {
  case RailShape::AscendingEast:
    return !fullSupport(world, pos.relative(Direction::East));
  case RailShape::AscendingWest:
    return !fullSupport(world, pos.relative(Direction::West));
  case RailShape::AscendingNorth:
    return !fullSupport(world, pos.relative(Direction::North));
  case RailShape::AscendingSouth:
    return !fullSupport(world, pos.relative(Direction::South));
  default:
    return false;
  }
}
BlockState updateDir(World &world, Pos pos, const BlockState &state,
                     bool first) {
  RailShape current = railShapeOf(state);
  return RailState(world, pos, state)
      .place(world.hasNeighborSignal(pos), first, current);
}
bool findPoweredRailSignal(World &world, Pos pos, const BlockState &state,
                           bool forward, int searchDepth) {
  if (searchDepth >= 8)
    return false;
  int x = pos.x, y = pos.y, z = pos.z;
  bool checkBelow = true;
  RailShape shape = railShapeOf(state);
  switch (shape) {
  case RailShape::NorthSouth:
    z += forward ? 1 : -1;
    break;
  case RailShape::EastWest:
    x += forward ? -1 : 1;
    break;
  case RailShape::AscendingEast:
    if (forward)
      --x;
    else {
      ++x;
      ++y;
      checkBelow = false;
    }
    shape = RailShape::EastWest;
    break;
  case RailShape::AscendingWest:
    if (forward) {
      --x;
      ++y;
      checkBelow = false;
    } else
      ++x;
    shape = RailShape::EastWest;
    break;
  case RailShape::AscendingNorth:
    if (forward)
      ++z;
    else {
      --z;
      ++y;
      checkBelow = false;
    }
    shape = RailShape::NorthSouth;
    break;
  case RailShape::AscendingSouth:
    if (forward) {
      ++z;
      ++y;
      checkBelow = false;
    } else
      --z;
    shape = RailShape::NorthSouth;
    break;
  default:
    break;
  }
  auto sameRailWithPower = [&](Pos check) {
    auto checkState = world.getBlock(check);
    if (!sameRailType(checkState, state))
      return false;
    RailShape myShape = railShapeOf(checkState);
    if (shape == RailShape::EastWest &&
        (myShape == RailShape::NorthSouth ||
         myShape == RailShape::AscendingNorth ||
         myShape == RailShape::AscendingSouth))
      return false;
    if (shape == RailShape::NorthSouth &&
        (myShape == RailShape::EastWest ||
         myShape == RailShape::AscendingEast ||
         myShape == RailShape::AscendingWest))
      return false;
    if (!poweredOf(checkState))
      return false;
    return world.hasNeighborSignal(check) ||
           findPoweredRailSignal(world, check, checkState, forward,
                                 searchDepth + 1);
  };
  return sameRailWithPower({x, y, z}) ||
         (checkBelow && sameRailWithPower({x, y - 1, z}));
}
// TODO: comparator/output-signal updates (updateNeighbourForOutputSignal) and
// analog output from command-block/container minecarts. Only the digital 15
// signal and its 20-tick pressed recheck are modeled.
void detectorCheckPressed(World &world, Pos pos, const BlockState &state) {
  if (!railCanSurvive(world, pos, state))
    return;
  bool wasPressed = poweredOf(state);
  Aabb search{pos.x + 0.2, double(pos.y), pos.z + 0.2, pos.x + 0.8,
              pos.y + 0.8, pos.z + 0.8};
  bool shouldBePressed = false;
  for (Entity *entity : world.entitiesIn(search))
    if (dynamic_cast<Minecart *>(entity)) {
      shouldBePressed = true;
      break;
    }
  if (shouldBePressed != wasPressed) {
    auto next = state.with(PropertyKind::Powered, shouldBePressed ? std::uint8_t{1} : std::uint8_t{0});
    world.setBlock(pos, next, 3);
    RailState rail(world, pos, next);
    for (auto connection : rail.connections()) {
      auto connectionState = world.getBlock(connection);
      world.neighborChanged(connectionState, connection, connectionState.name(),
                            false);
    }
    world.updateNeighborsAt(pos, state.name());
    world.updateNeighborsAt(pos.relative(Direction::Down), state.name());
  }
  if (shouldBePressed)
    world.scheduleTick(pos, state.name(), 20);
}
void poweredUpdateState(World &world, Pos pos, const BlockState &state) {
  bool powered = poweredOf(state);
  bool shouldPower = world.hasNeighborSignal(pos) ||
                     findPoweredRailSignal(world, pos, state, true, 0) ||
                     findPoweredRailSignal(world, pos, state, false, 0);
  if (shouldPower != powered) {
    world.setBlock(pos, state.with(PropertyKind::Powered, shouldPower ? std::uint8_t{1} : std::uint8_t{0}),
                   3);
    world.updateNeighborsAt(pos.relative(Direction::Down), state.name());
    if (ascending(railShapeOf(state)))
      world.updateNeighborsAt(pos.relative(Direction::Up), state.name());
  }
}
} // namespace

RailShape railShapeFromName(std::string_view name) {
  if (name == "east_west")
    return RailShape::EastWest;
  if (name == "ascending_east")
    return RailShape::AscendingEast;
  if (name == "ascending_west")
    return RailShape::AscendingWest;
  if (name == "ascending_north")
    return RailShape::AscendingNorth;
  if (name == "ascending_south")
    return RailShape::AscendingSouth;
  if (name == "south_east")
    return RailShape::SouthEast;
  if (name == "south_west")
    return RailShape::SouthWest;
  if (name == "north_west")
    return RailShape::NorthWest;
  if (name == "north_east")
    return RailShape::NorthEast;
  return RailShape::NorthSouth;
}
std::string railShapeName(RailShape shape) {
  switch (shape) {
  case RailShape::EastWest:
    return "east_west";
  case RailShape::AscendingEast:
    return "ascending_east";
  case RailShape::AscendingWest:
    return "ascending_west";
  case RailShape::AscendingNorth:
    return "ascending_north";
  case RailShape::AscendingSouth:
    return "ascending_south";
  case RailShape::SouthEast:
    return "south_east";
  case RailShape::SouthWest:
    return "south_west";
  case RailShape::NorthWest:
    return "north_west";
  case RailShape::NorthEast:
    return "north_east";
  case RailShape::NorthSouth:
  default:
    return "north_south";
  }
}
bool railShapeIsSlope(RailShape shape) {
  return shape == RailShape::AscendingEast ||
         shape == RailShape::AscendingWest ||
         shape == RailShape::AscendingNorth ||
         shape == RailShape::AscendingSouth;
}
RailShape railShapeOf(const BlockState &state) {
  return static_cast<RailShape>(propertyValue(state, PropertyKind::Shape, static_cast<std::uint32_t>(RailShape::NorthSouth)));
}
bool railIsStraight(const BlockState &state) {
  return !state.is(Blocks::RAIL);
}
std::pair<Pos, Pos> railExits(RailShape shape) {
  switch (shape) {
  case RailShape::NorthSouth:
    return {{0, 0, -1}, {0, 0, 1}};
  case RailShape::EastWest:
    return {{-1, 0, 0}, {1, 0, 0}};
  case RailShape::AscendingEast:
    return {{-1, -1, 0}, {1, 0, 0}};
  case RailShape::AscendingWest:
    return {{-1, 0, 0}, {1, -1, 0}};
  case RailShape::AscendingNorth:
    return {{0, 0, -1}, {0, -1, 1}};
  case RailShape::AscendingSouth:
    return {{0, -1, -1}, {0, 0, 1}};
  case RailShape::SouthEast:
    return {{0, 0, 1}, {1, 0, 0}};
  case RailShape::SouthWest:
    return {{0, 0, 1}, {-1, 0, 0}};
  case RailShape::NorthWest:
    return {{-1, 0, 0}, {0, 0, -1}};
  case RailShape::NorthEast:
    return {{1, 0, 0}, {0, 0, -1}};
  }
  return {{0, 0, -1}, {0, 0, 1}};
}
bool railCanSurvive(World &world, Pos pos, const BlockState &) {
  return fullSupport(world, pos.relative(Direction::Down));
}
// TODO: waterlogged rails should schedule a water tick through
// ScheduledTickAccess; fluid ticks are excluded from this project, so the
// property is preserved without draining/flow.
void railOnPlace(World &world, Pos pos, const BlockState &state,
                 const BlockState &oldState, bool movedByPiston) {
  if (sameRailType(oldState, state))
    return;
  auto updated = updateDir(world, pos, state, true);
  if (railIsStraight(updated)) {
    world.neighborChanged(updated, pos, updated.name(), movedByPiston);
    if (updated.is(Blocks::DETECTOR_RAIL))
      detectorCheckPressed(world, pos, updated);
  }
}
void railNeighborChanged(World &world, Pos pos, const BlockState &state,
                         const BlockRef &sourceBlock, bool movedByPiston) {
  if (!sameRailType(world.getBlock(pos), state))
    return;
  auto shape = railShapeOf(state);
  if (shouldBeRemoved(world, pos, shape)) {
    for (const auto &drop : blockDrops(state))
      world.spawnItem(pos, drop.id, drop.count);
    // Rail's existing state was destroyed without a piston move unless the
    // callback says otherwise; 64 suppresses removal callbacks as in
    // Level.removeBlock(pos, movedByPiston).
    world.setBlock(pos, airState(), movedByPiston ? 67 : 3);
    return;
  }
  if (state.is(Blocks::RAIL)) {
    auto source = BlockState(sourceBlock.str());
    if (signalSource(source) &&
        RailState(world, pos, state).countPotentialConnections() == 3)
      updateDir(world, pos, state, false);
  } else if (!state.is(Blocks::DETECTOR_RAIL)) {
    poweredUpdateState(world, pos, state);
  }
  (void)sourceBlock;
}
void railAffectNeighborsAfterRemoval(World &world, Pos pos,
                                     const BlockState &state,
                                     bool movedByPiston) {
  if (movedByPiston)
    return;
  if (ascending(railShapeOf(state)))
    world.updateNeighborsAt(pos.relative(Direction::Up), state.name());
  if (railIsStraight(state)) {
    world.updateNeighborsAt(pos, state.name());
    world.updateNeighborsAt(pos.relative(Direction::Down), state.name());
  }
}
void railScheduledTick(World &world, Pos pos, const BlockState &state) {
  if (state.is(Blocks::DETECTOR_RAIL))
    detectorCheckPressed(world, pos, state);
}
int railSignal(const BlockState &state) {
  return state.is(Blocks::DETECTOR_RAIL) &&
                 poweredOf(state)
             ? 15
             : 0;
}
BlockState railPlacementState(World &world, Pos pos,
                              Direction horizontalFacing) {
  auto state = defaultState("rail");
  bool eastWest = horizontalFacing == Direction::East ||
                  horizontalFacing == Direction::West;
  state = state.with(PropertyKind::Shape, static_cast<std::uint8_t>(eastWest ? RailShape::EastWest : RailShape::NorthSouth));
  (void)world;
  (void)pos;
  return state;
}
void railEntityInside(World &world, Pos pos, const BlockState &state,
                      Entity &entity) {
  if (state.is(Blocks::DETECTOR_RAIL) &&
      !poweredOf(state) &&
      dynamic_cast<Minecart *>(&entity))
    detectorCheckPressed(world, pos, state);
}
} // namespace redstone
