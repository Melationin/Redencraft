#pragma once
#include "redstone/engine.hpp"
#include <vector>
namespace redstone {
// Per-state collision shapes are immutable, so they are cached: the returned
// reference stays valid for the life of the process (like BlockStateBase).
const VoxelShape &collisionShape(const BlockState &);
VoxelShape outlineShape(const BlockState &);
VoxelShape collisionShape(World &, Pos, const BlockState &);
bool faceSturdy(const BlockState &, Direction);
bool faceSturdy(World &, Pos, const BlockState &, Direction);
bool coralCanSurvive(World &, Pos, const BlockState &);
BlockState wallUpdateShape(World &, Pos, const BlockState &, Direction);
BlockState wallUpdateShape(World &, Pos, const BlockState &, Direction,
                           const BlockState &capturedNeighbor);
} // namespace redstone
