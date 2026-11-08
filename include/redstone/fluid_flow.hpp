#pragma once
#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"

// net/minecraft/world/level/material/FlowingFluid.java
//
// Flow simulation. Everything here reads and writes the world through the same
// block-level API the rest of the core uses, so the fluid tick stays a normal
// scheduled tick and never bypasses the update machinery.
namespace redstone {

// FlowingFluid.getHeight: a same-family fluid directly above raises the surface
// to a full block, otherwise the fluid's own height (amount / 9).
float fluidHeight(World &world, Pos pos, const FluidState &state);

// Vec3 of FlowingFluid.getFlow with y always 0. `state` is the fluid at pos.
Vec3 fluidFlow(World &world, Pos pos, const FluidState &state);

// FlowingFluid.tick: recompute a non-source fluid's level, then spread.
void fluidTick(World &world, Pos pos, const BlockState &blockState,
               const FluidState &state);

// FlowingFluid.spread, exposed for the tests that drive one step at a time.
void fluidSpread(World &world, Pos pos, const BlockState &blockState,
                 const FluidState &state);

// FlowingFluid.getNewLiquid: the state this position should hold, given its
// horizontal neighbours and the block above.
FluidState fluidNewLiquid(World &world, Pos pos, const BlockState &blockState,
                          const Fluid &self);

// LiquidBlock.onPlace / neighborChanged: run shouldSpreadLiquid and, if the
// liquid survived, request a fluid tick. The two are separate entry points
// because vanilla calls them from different places. Both queue the fluid of
// the state the callback was handed, exactly like LiquidBlock.java:148-151
// and 194-198, which read the captured BlockState instead of the world.
void liquidOnPlace(World &world, Pos pos, const BlockState &state);
void liquidNeighborChanged(World &world, Pos pos, const BlockState &state);

// LiquidBlock.updateShape: request a fluid tick only when this block or the
// changed neighbour is a source.
void liquidUpdateShape(World &world, Pos pos, const BlockState &state,
                       const BlockState &neighbor);

// SimpleWaterloggedBlock: a waterlogged host reports a water source.
bool isWaterloggedHost(const BlockState &state);

// LiquidBlockContainer.canPlaceLiquid: only an exact water source may fill a
// waterlogged host (SimpleWaterloggedBlock.java:20-22 tests type == Fluids.WATER,
// so flowing_water and both lava variants are rejected).
bool canPlaceLiquid(const BlockState &state, const Fluid &type);

// LiquidBlockContainer.placeLiquid: a dry host turns waterlogged under flags 3
// and schedules its own water tick. Returns false for a wet host or a fluid
// that is not the exact water source, leaving the block untouched
// (SimpleWaterloggedBlock.java:24-36).
bool placeLiquid(World &world, Pos pos, const BlockState &state,
                 const FluidState &fluidState);

// FlowingFluid.getSpreadDelay: the tick delay to re-schedule with, including
// LavaFluid's x4 when the surface rises without falling.
int fluidSpreadDelay(World &world, Pos pos, const Fluid &self,
                     const FluidState &oldState, const FluidState &newState);

// LiquidBlock.shouldSpreadLiquid: lava meeting water (or soul soil + blue ice)
// converts in place and returns false, telling the caller not to spread.
bool liquidShouldSpreadLiquid(World &world, Pos pos, const BlockState &state);

// Shapes.mergedFaceOccludes for a pair of already-resolved shapes. The registry
// has no two blocks whose partial shapes complement each other (its only
// partial shapes are the fence gate's bar band and the wall post's column, and
// neither pair seals a face), so the merge path itself is exercised with
// synthetic shapes -- exactly the half-slab pair the vanilla probe uses. This
// is a read-only query: it never writes a block and never schedules a tick.
bool fluidShapesSealSharedFace(const VoxelShape &source,
                               const VoxelShape &target, Direction direction);

// FlowingFluid.canPassThroughWall: whether fluid may cross from sourcePos to
// targetPos through the face in `direction`.
bool fluidCanPassThroughWall(World &world, Direction direction, Pos sourcePos,
                             const BlockState &sourceState, Pos targetPos,
                             const BlockState &targetState);

} // namespace redstone
