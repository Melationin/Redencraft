#pragma once
#include "redstone/engine.hpp"
#include <vector>
namespace redstone {
enum class SupportType { Full, Center, Rigid };
// Per-state collision shapes are immutable, so they are cached: the returned
// reference stays valid for the life of the process (like BlockStateBase).
const VoxelShape &collisionShape(const BlockState &);
VoxelShape outlineShape(const BlockState &);
VoxelShape collisionShape(World &, Pos, const BlockState &);
bool faceSturdy(const BlockState &, Direction,
                SupportType = SupportType::Full);
bool faceSturdy(World &, Pos, const BlockState &, Direction,
                SupportType = SupportType::Full);
bool coralCanSurvive(World &, Pos, const BlockState &);
bool isIronBarsBlock(const BlockState &);
bool isExceptionForConnection(const BlockState &);
bool fenceGateConnectsToDirection(const BlockState &, Direction);
BlockState wallUpdateShape(World &, Pos, const BlockState &, Direction);
BlockState wallUpdateShape(World &, Pos, const BlockState &, Direction,
                           const BlockState &capturedNeighbor);
BlockState fenceUpdateShape(World &, Pos, const BlockState &, Direction);
BlockState fenceUpdateShape(World &, Pos, const BlockState &, Direction,
                            const BlockState &capturedNeighbor);
BlockState ironBarsUpdateShape(World &, Pos, const BlockState &, Direction);
BlockState ironBarsUpdateShape(World &, Pos, const BlockState &, Direction,
                               const BlockState &capturedNeighbor);
BlockState stairUpdateShape(World &, Pos, const BlockState &);
BlockState stairUpdateShape(World &, Pos, const BlockState &, Direction,
                            const BlockState &capturedNeighbor);
} // namespace redstone
