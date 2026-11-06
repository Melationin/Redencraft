#pragma once
#include "redstone/engine.hpp"
#include <string>

namespace redstone {
// net.minecraft.world.level.block.state.properties.RailShape.
enum class RailShape {
  NorthSouth,
  EastWest,
  AscendingEast,
  AscendingWest,
  AscendingNorth,
  AscendingSouth,
  SouthEast,
  SouthWest,
  NorthWest,
  NorthEast
};
RailShape railShapeFromName(const std::string &name);
std::string railShapeName(RailShape shape);
bool railShapeIsSlope(RailShape shape);
RailShape railShapeOf(const BlockState &state);
bool railIsStraight(const BlockState &state);

// BaseRailBlock/PoweredRailBlock/DetectorRailBlock/RailBlock callbacks.
void railOnPlace(World &world, Pos pos, const BlockState &state,
                 const BlockState &oldState, bool movedByPiston);
void railNeighborChanged(World &world, Pos pos, const BlockState &state,
                         const BlockRef &sourceBlock,
                         bool movedByPiston = false);
void railAffectNeighborsAfterRemoval(World &world, Pos pos,
                                     const BlockState &state,
                                     bool movedByPiston);
void railScheduledTick(World &world, Pos pos, const BlockState &state);
int railSignal(const BlockState &state);
bool railCanSurvive(World &world, Pos pos, const BlockState &state);
BlockState railPlacementState(World &world, Pos pos, Direction horizontalFacing);
std::pair<Pos, Pos> railExits(RailShape shape);
void railEntityInside(World &world, Pos pos, const BlockState &state,
                      Entity &entity);
} // namespace redstone
