#include "redstone/internal.hpp"
#include "redstone/shapes.hpp"

#include <algorithm>
#include <optional>

namespace redstone {
namespace {

using redstone_detail::dropAndRemoveBlock;
using redstone_detail::isWireConnected;
using redstone_detail::kAllDirections;
using redstone_detail::kHorizontalOrder;
using redstone_detail::kWireNone;
using redstone_detail::kWireSide;
using redstone_detail::kWireUp;
using redstone_detail::wirePowerOf;
using redstone_detail::wireSideOf;
using redstone_detail::wireSideProperty;

bool isWireCross(const BlockState &state) {
  return isWireConnected(state, Direction::North) &&
         isWireConnected(state, Direction::South) &&
         isWireConnected(state, Direction::East) &&
         isWireConnected(state, Direction::West);
}

bool isWireDot(const BlockState &state) {
  return !isWireConnected(state, Direction::North) &&
         !isWireConnected(state, Direction::South) &&
         !isWireConnected(state, Direction::East) &&
         !isWireConnected(state, Direction::West);
}

BlockState wireCrossState() {
  return Blocks::REDSTONE_WIRE.defaultState()
      .with(PropertyKind::WallNorth, kWireSide)
      .with(PropertyKind::WallEast, kWireSide)
      .with(PropertyKind::WallSouth, kWireSide)
      .with(PropertyKind::WallWest, kWireSide);
}

bool wireShouldConnectTo(const World &world, const BlockState &state,
                         std::optional<Direction> direction) {
  if (state.is(Blocks::REDSTONE_WIRE))
    return true;
  if (state.is(Blocks::REPEATER)) {
    const Direction facing = directionOf(state);
    return direction && (facing == *direction || opposite(facing) == *direction);
  }
  if (state.is(Blocks::OBSERVER))
    return direction && directionOf(state, Direction::South) == *direction;
  return direction.has_value() && isRedstoneSignalSource(world, state);
}

bool wireCanSurviveOn(const BlockState &belowState) {
  return faceSturdy(belowState, Direction::Up, SupportType::Full) ||
         belowState.is(Blocks::HOPPER);
}

std::uint8_t wireGetConnectingSide(const World &world, Pos pos,
                                   Direction direction, bool canConnectUp) {
  const Pos relativePos = pos.relative(direction);
  const BlockState relativeState = world.getBlock(relativePos);
  if (canConnectUp) {
    const bool isPlaceableAbove =
        relativeState.block().isTrapdoor() || wireCanSurviveOn(relativeState);
    if (isPlaceableAbove &&
        wireShouldConnectTo(world,
                            world.getBlock(relativePos.relative(Direction::Up)),
                            std::nullopt)) {
      if (faceSturdy(relativeState, opposite(direction), SupportType::Full))
        return kWireUp;
      return kWireSide;
    }
  }
  const bool connectSide =
      wireShouldConnectTo(world, relativeState, direction) ||
      (!isRedstoneConductor(relativeState) &&
       wireShouldConnectTo(
           world, world.getBlock(relativePos.relative(Direction::Down)),
           std::nullopt));
  return connectSide ? kWireSide : kWireNone;
}

std::uint8_t wireGetConnectingSide(const World &world, Pos pos,
                                   Direction direction) {
  return wireGetConnectingSide(
      world, pos, direction,
      !isRedstoneConductor(world.getBlock(pos.relative(Direction::Up))));
}

BlockState wireGetMissingConnections(const World &world, BlockState state,
                                     Pos pos) {
  const bool canConnectUp =
      !isRedstoneConductor(world.getBlock(pos.relative(Direction::Up)));
  for (Direction direction : kHorizontalOrder) {
    if (!isWireConnected(state, direction)) {
      const auto side =
          wireGetConnectingSide(world, pos, direction, canConnectUp);
      state = state.with(wireSideProperty(direction), side);
    }
  }
  return state;
}

struct WireSignalGuard {
  const World &world;
  bool prev;
  explicit WireSignalGuard(const World &w)
      : world(w), prev(w.wireShouldSignal()) {
    w.setWireShouldSignal(false);
  }
  ~WireSignalGuard() { world.setWireShouldSignal(prev); }
};

int wireGetBlockSignal(const World &world, Pos pos) {
  WireSignalGuard guard(world);
  return world.getBestNeighborSignal(pos);
}

int wireGetSignalAt(const BlockState &state) {
  return state.is(Blocks::REDSTONE_WIRE) ? wirePowerOf(state) : 0;
}

int wireGetIncomingSignal(const World &world, Pos pos) {
  int wireSignal = 0;
  const BlockState aboveState = world.getBlock(pos.relative(Direction::Up));
  const bool aboveConductor = isRedstoneConductor(aboveState);
  for (Direction direction : kHorizontalOrder) {
    const Pos neighborPos = pos.relative(direction);
    const BlockState neighborState = world.getBlock(neighborPos);
    wireSignal = std::max(wireSignal, wireGetSignalAt(neighborState));
    const bool neighborConductor = isRedstoneConductor(neighborState);
    if (neighborConductor && !aboveConductor) {
      const Pos upPos = neighborPos.relative(Direction::Up);
      wireSignal =
          std::max(wireSignal, wireGetSignalAt(world.getBlock(upPos)));
    } else if (!neighborConductor) {
      const Pos downPos = neighborPos.relative(Direction::Down);
      wireSignal =
          std::max(wireSignal, wireGetSignalAt(world.getBlock(downPos)));
    }
  }
  return std::max(0, wireSignal - 1);
}

void wireUpdatePowerStrength(World &world, Pos pos, const BlockState &state) {
  const int blockSignal = wireGetBlockSignal(world, pos);
  const int targetStrength =
      blockSignal == 15
          ? 15
          : std::max(blockSignal, wireGetIncomingSignal(world, pos));
  if (wirePowerOf(state) != targetStrength) {
    if (world.getBlock(pos) == state) {
      world.setBlock(pos,
                     state.with(PropertyKind::Power,
                                static_cast<std::uint8_t>(targetStrength)),
                     2);
    }
    world.updateNeighborsAt(pos, Blocks::REDSTONE_WIRE);
    for (Direction d : kAllDirections)
      world.updateNeighborsAt(pos.relative(d), Blocks::REDSTONE_WIRE);
  }
}

void wireCheckCornerChangeAt(World &world, Pos pos) {
  if (world.getBlock(pos).is(Blocks::REDSTONE_WIRE)) {
    world.updateNeighborsAt(pos, Blocks::REDSTONE_WIRE);
    for (Direction d : kAllDirections)
      world.updateNeighborsAt(pos.relative(d), Blocks::REDSTONE_WIRE);
  }
}

void wireUpdateNeighborsOfNeighboringWires(World &world, Pos pos) {
  for (Direction d : kHorizontalOrder)
    wireCheckCornerChangeAt(world, pos.relative(d));
  for (Direction d : kHorizontalOrder) {
    const Pos target = pos.relative(d);
    if (isRedstoneConductor(world.getBlock(target)))
      wireCheckCornerChangeAt(world, target.relative(Direction::Up));
    else
      wireCheckCornerChangeAt(world, target.relative(Direction::Down));
  }
}

void wireUpdatesOnShapeChange(World &world, Pos pos, const BlockState &oldState,
                              const BlockState &newState) {
  for (Direction d : kHorizontalOrder) {
    const Pos rel = pos.relative(d);
    if (isWireConnected(oldState, d) != isWireConnected(newState, d) &&
        isRedstoneConductor(world.getBlock(rel))) {
      world.updateNeighborsAtExceptFromFacing(rel, Blocks::REDSTONE_WIRE,
                                              opposite(d));
    }
  }
}

} // namespace

BlockState redstoneWireConnectionState(const World &world, Pos pos,
                                       const BlockState &state) {
  const bool wasDot = isWireDot(state);
  BlockState out = wireGetMissingConnections(
      world,
      Blocks::REDSTONE_WIRE.defaultState().with(
          PropertyKind::Power, static_cast<std::uint8_t>(wirePowerOf(state))),
      pos);
  if (wasDot && isWireDot(out))
    return out;
  const bool north = isWireConnected(out, Direction::North);
  const bool south = isWireConnected(out, Direction::South);
  const bool east = isWireConnected(out, Direction::East);
  const bool west = isWireConnected(out, Direction::West);
  const bool northSouthEmpty = !north && !south;
  const bool eastWestEmpty = !east && !west;
  if (!west && northSouthEmpty)
    out = out.with(PropertyKind::WallWest, kWireSide);
  if (!east && northSouthEmpty)
    out = out.with(PropertyKind::WallEast, kWireSide);
  if (!north && eastWestEmpty)
    out = out.with(PropertyKind::WallNorth, kWireSide);
  if (!south && eastWestEmpty)
    out = out.with(PropertyKind::WallSouth, kWireSide);
  return out;
}

BlockState redstoneWirePlacementState(const World &world, Pos pos) {
  return redstoneWireConnectionState(world, pos, wireCrossState());
}

bool redstoneWireCanSurvive(World &world, Pos pos) {
  return wireCanSurviveOn(world.getBlock(pos.relative(Direction::Down)));
}

bool redstoneWireUse(World &world, Pos pos, const BlockState &state) {
  if (!state.is(Blocks::REDSTONE_WIRE))
    return false;
  if (isWireCross(state) || isWireDot(state)) {
    BlockState next =
        isWireCross(state) ? Blocks::REDSTONE_WIRE.defaultState()
                           : wireCrossState();
    next = next.with(PropertyKind::Power,
                     static_cast<std::uint8_t>(wirePowerOf(state)));
    next = redstoneWireConnectionState(world, pos, next);
    if (!(next == state)) {
      world.setBlock(pos, next, 3);
      wireUpdatesOnShapeChange(world, pos, state, next);
      return true;
    }
  }
  return false;
}

void redstoneUpdateIndirectNeighbourShapes(World &world, Pos pos,
                                           const BlockState &state, int flags,
                                           int limit) {
  if (!state.is(Blocks::REDSTONE_WIRE) || limit <= 0)
    return;
  for (Direction d : kHorizontalOrder) {
    if (wireSideOf(state, d) == kWireNone)
      continue;
    const Pos rel = pos.relative(d);
    if (world.getBlock(rel).is(Blocks::REDSTONE_WIRE))
      continue;
    const Pos downPos = rel.relative(Direction::Down);
    if (world.getBlock(downPos).is(Blocks::REDSTONE_WIRE)) {
      const Pos nPos = downPos.relative(opposite(d));
      world.neighborShapeChanged(downPos, opposite(d), nPos,
                                 world.getBlock(nPos), flags, limit);
    }
    const Pos upPos = rel.relative(Direction::Up);
    if (world.getBlock(upPos).is(Blocks::REDSTONE_WIRE)) {
      const Pos nPos = upPos.relative(opposite(d));
      world.neighborShapeChanged(upPos, opposite(d), nPos, world.getBlock(nPos),
                                 flags, limit);
    }
  }
}

void redstoneWireOnPlace(World &world, Pos pos, const BlockState &state,
                         const BlockState &oldState, bool) {
  if (!oldState.sameBlock(state)) {
    wireUpdatePowerStrength(world, pos, state);
    for (Direction d : {Direction::Up, Direction::Down})
      world.updateNeighborsAt(pos.relative(d), Blocks::REDSTONE_WIRE);
    wireUpdateNeighborsOfNeighboringWires(world, pos);
  }
}

void redstoneWireAffectRemoval(World &world, Pos pos, const BlockState &state,
                               bool movedByPiston) {
  if (!movedByPiston) {
    for (Direction d : kAllDirections)
      world.updateNeighborsAt(pos.relative(d), Blocks::REDSTONE_WIRE);
    wireUpdatePowerStrength(world, pos, state);
    wireUpdateNeighborsOfNeighboringWires(world, pos);
  }
}

void redstoneWireNeighborChanged(World &world, Pos pos, const BlockState &state,
                                 const BlockRef &, bool) {
  if (redstoneWireCanSurvive(world, pos))
    wireUpdatePowerStrength(world, pos, state);
  else
    dropAndRemoveBlock(world, pos, state);
}

BlockState redstoneWireUpdateShape(World &world, Pos pos,
                                   const BlockState &state, Direction direction,
                                   Pos neighborPos,
                                   const BlockState &neighbor) {
  if (direction == Direction::Down)
    return wireCanSurviveOn(neighbor) ? state : airState();
  if (direction == Direction::Up)
    return redstoneWireConnectionState(world, pos, state);
  (void)neighborPos;
  const std::uint8_t sideConnection =
      wireGetConnectingSide(world, pos, direction);
  const std::uint8_t currentSide = wireSideOf(state, direction);
  if ((sideConnection != kWireNone) == (currentSide != kWireNone) &&
      !isWireCross(state))
    return state.with(wireSideProperty(direction), sideConnection);
  return redstoneWireConnectionState(
      world, pos,
      wireCrossState()
          .with(PropertyKind::Power,
                static_cast<std::uint8_t>(wirePowerOf(state)))
          .with(wireSideProperty(direction), sideConnection));
}

} // namespace redstone
