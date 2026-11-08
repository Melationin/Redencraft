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

// FlowingFluid.getSpreadDelay: the tick delay to re-schedule with, including
// LavaFluid's x4 when the surface rises without falling.
int fluidSpreadDelay(World &world, Pos pos, const Fluid &self,
                     const FluidState &oldState, const FluidState &newState);

// LiquidBlock.shouldSpreadLiquid: lava meeting water (or soul soil + blue ice)
// converts in place and returns false, telling the caller not to spread.
bool liquidShouldSpreadLiquid(World &world, Pos pos, const BlockState &state);

// FlowingFluid.canPassThroughWall: whether fluid may cross from sourcePos to
// targetPos through the face in `direction`.
bool fluidCanPassThroughWall(World &world, Direction direction, Pos sourcePos,
                             const BlockState &sourceState, Pos targetPos,
                             const BlockState &targetState);

} // namespace redstone
