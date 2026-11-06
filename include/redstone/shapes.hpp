#pragma once
#include "redstone/engine.hpp"
#include <vector>
namespace redstone {
struct ShapeBox {
  double x0, y0, z0, x1, y1, z1;
};
using VoxelShape = std::vector<ShapeBox>;
VoxelShape collisionShape(const BlockState &);
VoxelShape collisionShape(World &, Pos, const BlockState &);
bool faceSturdy(const BlockState &, Direction);
bool faceSturdy(World &, Pos, const BlockState &, Direction);
bool coralCanSurvive(World &, Pos, const BlockState &);
BlockState wallUpdateShape(World &, Pos, const BlockState &, Direction);
BlockState wallUpdateShape(World &, Pos, const BlockState &, Direction,
                           const BlockState &capturedNeighbor);
} // namespace redstone
