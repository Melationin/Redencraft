#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"
#include "redstone/fluid_flow.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

// F-5: the block-side wiring. LiquidBlock reaches the fluid tick from three
// separate places (onPlace, neighborChanged, updateShape) and the waterlogged
// hosts need their own fluid state and solidity.

using namespace redstone;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

BlockState waterLevel(int level) {
  return Blocks::WATER.defaultState().with(
      PropertyKind::Level, static_cast<std::uint8_t>(level));
}
BlockState lavaLevel(int level) {
  return Blocks::LAVA.defaultState().with(
      PropertyKind::Level, static_cast<std::uint8_t>(level));
}
void flatFloor(World &world) {
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      world.loadChunk(x, z);
  for (int x = -16; x <= 15; ++x)
    for (int z = -16; z <= 15; ++z)
      world.rawSetBlock({x, 63, z}, Blocks::SMOOTH_STONE.defaultState());
}
// Any pending fluid tick at the position, whatever exact fluid type was queued.
// Vanilla schedules with `state.getFluidState().getType()`, and for a flowing
// block that is fluid_water's FLOWING_WATER entry, not the WATER source entry:
// Fluids registers five fluids and `water` is only the Source variant.
bool hasWaterTick(const World &world, Pos pos) {
  for (const auto &entry : world.fluidTicks())
    if (entry.first.pos == pos)
      return true;
  return false;
}

// H-01: placing a liquid block requests its first fluid tick (onPlace).
void placementRequestsTick() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  require(world.fluidTickCount() == 0, "the fluid queue starts empty");
  require(world.setBlock(pos, waterLevel(0)), "place water");
  require(hasWaterTick(world, pos),
          "placing a liquid requests a fluid tick");
  require(world.fluidTickCount() == 1, "exactly one tick is queued");
}

// H-02: a neighbour change requests a tick through its own path, even when the
// block itself was not replaced.
void neighborChangeRequestsTick() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0));
  require(world.fluidTickCount() == 0, "rawSetBlock does not request a tick");

  // A neighbour update on the water block runs LiquidBlock.neighborChanged.
  world.neighborChanged(world.getBlock(pos), pos, BlockRef(Blocks::SMOOTH_STONE));
  require(hasWaterTick(world, pos), "a neighbour change requests a fluid tick");
}

// H-03: updateShape requests a tick only when this block or the changed
// neighbour is a source.
void updateShapeNeedsSource() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  const Pos side{1, 64, 0};

  // Flowing water next to a plain neighbour: no request.
  world.rawSetBlock(pos, waterLevel(4));
  world.rawSetBlock(side, Blocks::SMOOTH_STONE.defaultState());
  world.updateFromNeighbourShapes(pos, world.getBlock(pos));
  require(!hasWaterTick(world, pos),
          "flowing water with a non-source neighbour requests nothing");

  // The same position with a source neighbour: requested.
  World withSource;
  flatFloor(withSource);
  withSource.rawSetBlock(pos, waterLevel(4));
  withSource.rawSetBlock(side, waterLevel(0));
  withSource.updateFromNeighbourShapes(pos, withSource.getBlock(pos));
  require(hasWaterTick(withSource, pos),
          std::string("a source neighbour makes updateShape request a tick; count=") +
              std::to_string(withSource.fluidTickCount()) + " side=" +
              std::string(withSource.getBlock(side).shortName()) +
              " sideIsSource=" +
              std::to_string(fluidStateOf(withSource.getBlock(side)).isSource()));
}

// Waterlogged hosts report a water source through getFluidState.
void waterloggedHostFluid() {
  World world;
  flatFloor(world);
  const Pos rail{0, 64, 0};
  world.rawSetBlock(rail,
                    Blocks::RAIL.defaultState().with("waterlogged", "true"));
  const FluidState fluid = fluidStateOf(world.getBlock(rail));
  require(!fluid.isEmpty(), "a waterlogged rail reports a fluid");
  require(fluid.is(Fluids::water()), "the reported fluid is the water source");
  require(fluid.isSource(), "the reported fluid is a source");
  require(isWaterloggedHost(world.getBlock(rail)),
          "the rail is a waterlogged host");

  const Pos dryRail{2, 64, 0};
  world.rawSetBlock(dryRail, Blocks::RAIL.defaultState());
  require(fluidStateOf(world.getBlock(dryRail)).isEmpty(),
          "a dry rail reports no fluid");
}

// forceSolidOn: the registered walls and dead coral fans count as solid, so they
// can support a two-source conversion and water does not replace them.
void forceSolidWallsAndFans() {
  World world;
  flatFloor(world);
  // Two sources around a gap whose support is a wall.
  const Pos gap{1, 64, 3};
  world.rawSetBlock({0, 64, 3}, waterLevel(0));
  world.rawSetBlock({2, 64, 3}, waterLevel(0));
  world.rawSetBlock({1, 63, 3}, Blocks::COBBLESTONE_WALL.defaultState());
  require(fluidNewLiquid(world, gap, world.getBlock(gap), Fluids::water())
              .isSource(),
          "a wall supports the two-source conversion");

  // Water next to a wall does not replace it.
  const Pos source{0, 64, 8};
  const Pos wall{1, 64, 8};
  world.rawSetBlock(source, waterLevel(0));
  world.rawSetBlock(wall, Blocks::COBBLESTONE_WALL.defaultState());
  fluidSpread(world, source, world.getBlock(source),
              fluidStateOf(world.getBlock(source)));
  require(world.getBlock(wall).is(Blocks::COBBLESTONE_WALL),
          "water does not replace a wall");

  // A dead coral fan is solid for the same reason.
  const Pos fan{5, 64, 8};
  world.rawSetBlock(fan, Blocks::DEAD_TUBE_CORAL_FAN.defaultState());
  require(blockMetadata(world.getBlock(fan)).forceSolid,
          "a dead coral fan is marked forceSolid");
}

// A dry world must not gain scheduled ticks from the new request paths.
void dryWorldStaysQuiet() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  world.setBlock(pos, Blocks::SMOOTH_STONE.defaultState());
  world.neighborChanged(world.getBlock(pos), pos, BlockRef(Blocks::SAND));
  world.updateFromNeighbourShapes(pos, world.getBlock(pos));
  require(world.fluidTickCount() == 0,
          "non-liquid blocks request no fluid ticks");
}
} // namespace

int main() {
  try {
    placementRequestsTick();
    neighborChangeRequestsTick();
    updateShapeNeedsSource();
    waterloggedHostFluid();
    forceSolidWallsAndFans();
    dryWorldStaysQuiet();
    std::cout << "fluid block tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid block test failure: " << error.what() << "\n";
    return 1;
  }
}
