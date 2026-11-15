#pragma once

#include "redstone/entity.hpp"
#include "redstone/redstone.hpp"

#include <array>
#include <cstdint>

namespace redstone::redstone_detail {

inline constexpr std::uint8_t kWireNone = 0;
inline constexpr std::uint8_t kWireSide = 5;
inline constexpr std::uint8_t kWireUp = 6;

inline constexpr std::array<Direction, 4> kHorizontalOrder{
    Direction::North, Direction::East, Direction::South, Direction::West};
inline constexpr std::array<Direction, 6> kAllDirections{
    Direction::Down,  Direction::Up,   Direction::North,
    Direction::South, Direction::West, Direction::East};

inline bool isHorizontal(Direction d) {
  return d == Direction::North || d == Direction::South ||
         d == Direction::East || d == Direction::West;
}

inline Direction clockWise(Direction d) {
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

inline Direction counterClockWise(Direction d) {
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

inline int directionAxis(Direction d) {
  if (d == Direction::Up || d == Direction::Down)
    return 1;
  if (d == Direction::East || d == Direction::West)
    return 0;
  return 2;
}

inline PropertyKind wireSideProperty(Direction d) {
  switch (d) {
  case Direction::North:
    return PropertyKind::WallNorth;
  case Direction::East:
    return PropertyKind::WallEast;
  case Direction::South:
    return PropertyKind::WallSouth;
  case Direction::West:
    return PropertyKind::WallWest;
  default:
    return PropertyKind::WallNorth;
  }
}

inline std::uint8_t wireSideOf(const BlockState &state, Direction d) {
  return static_cast<std::uint8_t>(propertyValue(state, wireSideProperty(d)));
}

inline bool isWireConnected(const BlockState &state, Direction d) {
  return wireSideOf(state, d) != kWireNone;
}

inline int wirePowerOf(const BlockState &state) {
  return static_cast<int>(propertyValue(state, PropertyKind::Power));
}

inline Direction faceAttachedConnectedDirection(const BlockState &state) {
  const auto face = propertyValue(state, PropertyKind::Face, 1);
  if (face == 0) // floor
    return Direction::Up;
  if (face == 2) // ceiling
    return Direction::Down;
  return directionOf(state); // wall
}

inline int pressurePlateSignalOf(const BlockState &state) {
  if (state.is(Blocks::LIGHT_WEIGHTED_PRESSURE_PLATE) ||
      state.is(Blocks::HEAVY_WEIGHTED_PRESSURE_PLATE))
    return static_cast<int>(propertyValue(state, PropertyKind::Power));
  return poweredOf(state) ? 15 : 0;
}

inline void dropAndRemoveBlock(World &world, Pos pos, const BlockState &state) {
  for (const auto &drop : blockDrops(state))
    world.spawnItem(pos, drop.id, drop.count);
  world.removeBlock(pos, false);
}

} // namespace redstone::redstone_detail
