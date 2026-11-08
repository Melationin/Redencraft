#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"
#include "redstone/fluid_flow.hpp"
#include "redstone/piston.hpp"

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

// H-01/H-03: a water source fills a dry host through the container interface;
// flowing water and lava are refused, and a second fill does not rewrite the
// block or queue a second tick.
void containerFillRules() {
  World world;
  flatFloor(world);
  const Pos wall{0, 64, 0};
  const BlockState dry = Blocks::COBBLESTONE_WALL.defaultState();
  require(!waterloggedOf(dry), "the wall starts dry");
  world.rawSetBlock(wall, dry);

  // canPlaceLiquid: the exact WATER source only.
  require(canPlaceLiquid(dry, Fluids::water()), "a water source is accepted");
  require(!canPlaceLiquid(dry, Fluids::flowingWater()),
          "flowing water is refused");
  require(!canPlaceLiquid(dry, Fluids::lava()), "lava is refused");
  require(!canPlaceLiquid(dry, Fluids::flowingLava()),
          "flowing lava is refused");

  // Flowing water and lava are refused outright and leave the host alone.
  require(!placeLiquid(world, wall, dry,
                       FluidState(Fluids::flowingWater(), 7, false)),
          "placeLiquid refuses flowing water");
  require(!placeLiquid(world, wall, dry, FluidState(Fluids::lava(), 8, false)),
          "placeLiquid refuses lava");
  require(world.getBlock(wall) == dry, "a refused fill leaves the host as it was");
  require(world.fluidTickCount() == 0, "a refused fill queues nothing");

  // The source fills it: waterlogged under flags 3 plus its own water tick.
  require(placeLiquid(world, wall, dry, FluidState(Fluids::water(), 8, false)),
          "placeLiquid accepts a water source");
  require(waterloggedOf(world.getBlock(wall)), "the host is now waterlogged");
  require(world.getBlock(wall).is(Blocks::COBBLESTONE_WALL),
          "the host block survives the fill");
  require(fluidStateOf(world.getBlock(wall)).isSource(),
          "the filled host reports a water source");
  require(hasWaterTick(world, wall), "the fill queues a water tick");

  // Re-filling an already wet host is a no-op: no rewrite, no second entry.
  const auto before = world.fluidTickCount();
  require(!placeLiquid(world, wall, world.getBlock(wall),
                       FluidState(Fluids::water(), 8, false)),
          "a wet host reports that it was not filled again");
  require(world.fluidTickCount() == before, "no duplicate tick is queued");
}

// H-03: real propagation reaches a dry host. FlowingFluid.getSpread asks the
// container about the fluid it just computed FOR THE HOST CELL, so a plain
// side-flow candidate (flowing_water) is refused -- only a candidate that is
// the exact source may enter. That happens when the host is squeezed between
// two sources over a solid floor: getNewLiquid then answers a source
// (FlowingFluid.java:378-403 + SimpleWaterloggedBlock.java:20-22).
void spreadFillsHosts() {
  const auto fillFromTwoSources = [](const BlockState &host, const char *what) {
    World world;
    flatFloor(world);
    const Pos left{0, 64, 0};
    const Pos middle{1, 64, 0};
    const Pos right{2, 64, 0};
    world.rawSetBlock(left, waterLevel(0));
    world.rawSetBlock(right, waterLevel(0));
    world.rawSetBlock({1, 63, 0}, Blocks::SMOOTH_STONE.defaultState());
    world.rawSetBlock(middle, host);
    require(fluidNewLiquid(world, middle, world.getBlock(middle),
                           Fluids::water())
                .isSource(),
            std::string("two sources make the ") + what +
                " cell a source candidate");
    fluidSpread(world, left, world.getBlock(left),
                fluidStateOf(world.getBlock(left)));
    require(world.getBlock(middle).is(host.block()),
            std::string("water does not replace a ") + what);
    require(waterloggedOf(world.getBlock(middle)),
            std::string("the spread waterlogs the ") + what);
  };
  fillFromTwoSources(Blocks::COBBLESTONE_WALL.defaultState(), "wall");
  fillFromTwoSources(Blocks::DEAD_TUBE_CORAL_FAN.defaultState()
                         .with("waterlogged", "false"),
                     "coral fan");

  // One source only: the candidate is flowing_water and the host refuses it,
  // so the water goes around the block instead of into it.
  World single;
  flatFloor(single);
  single.rawSetBlock({0, 64, 0}, waterLevel(0));
  single.rawSetBlock({1, 64, 0}, Blocks::COBBLESTONE_WALL.defaultState());
  single.rawSetBlock({1, 63, 0}, Blocks::SMOOTH_STONE.defaultState());
  fluidSpread(single, {0, 64, 0}, single.getBlock({0, 64, 0}),
              fluidStateOf(single.getBlock({0, 64, 0})));
  require(!waterloggedOf(single.getBlock({1, 64, 0})),
          "a sideways flow candidate is not the exact source, so a dry "
          "host stays dry");
}

// A dry host beside flowing water must not be filled by it: the container's
// canPlaceLiquid refuses anything but the exact source, so the flowing
// neighbour is simply not a candidate that may enter the host.
void flowingWaterDoesNotFillHosts() {
  // canHoldSpecificFluid is internal; the public contract is canPlaceLiquid,
  // which is exactly what the container answers with.
  const BlockState dry = Blocks::COBBLESTONE_WALL.defaultState();
  require(!canPlaceLiquid(dry, Fluids::flowingWater()),
          "a host refuses the flowing variant");
  require(canPlaceLiquid(dry, Fluids::water()),
          "a host accepts the source variant");

  // And the real spread leaves the dry host dry when only flowing fluid is
  // beside it: canHoldSpecificFluid rejects the flowing candidate before
  // spreadTo ever runs.
  World world;
  flatFloor(world);
  const Pos wall{1, 64, 0};
  world.rawSetBlock({0, 64, 0}, waterLevel(3)); // flowing, amount 5
  world.rawSetBlock(wall, dry);
  fluidSpread(world, {0, 64, 0}, world.getBlock({0, 64, 0}),
              fluidStateOf(world.getBlock({0, 64, 0})));
  require(world.getBlock(wall) == dry,
          "flowing water leaves a dry host untouched");
  require(!waterloggedOf(world.getBlock(wall)),
          "flowing water does not waterlog the host");
}

// H-02: breaking a waterlogged host's support drains it through the destroy
// path, and the water source is what the cell keeps.
void hostDrainOnSupportLoss() {
  World world;
  flatFloor(world);
  const Pos fan{0, 64, 0};
  world.rawSetBlock({0, 63, 0}, Blocks::SMOOTH_STONE.defaultState());
  world.rawSetBlock(fan, Blocks::DEAD_TUBE_CORAL_FAN.defaultState());
  require(waterloggedOf(world.getBlock(fan)), "the fan starts waterlogged");
  world.setBlock({0, 63, 0}, airState());
  require(world.getBlock(fan).is(Blocks::WATER),
          "the drained cell keeps the water source");
  require(fluidStateOf(world.getBlock(fan)).isSource(),
          "the kept fluid is a source");

  // A dry host leaves plain air behind.
  World dry;
  flatFloor(dry);
  const Pos dryFan{0, 64, 0};
  dry.rawSetBlock({0, 63, 0}, Blocks::SMOOTH_STONE.defaultState());
  dry.rawSetBlock(dryFan, Blocks::DEAD_TUBE_CORAL_FAN.defaultState()
                             .with("waterlogged", "false"));
  dry.setBlock({0, 63, 0}, airState());
  require(dry.getBlock(dryFan).isAir(), "a dry host leaves air");
}

// H-02: a waterlogged rail that loses its support drains through
// Level.removeBlock rather than leaving air behind.
void railDrainKeepsWater() {
  World world;
  flatFloor(world);
  const Pos rail{0, 64, 0};
  world.rawSetBlock(rail, Blocks::RAIL.defaultState().with("waterlogged", "true"));
  require(waterloggedOf(world.getBlock(rail)), "the rail starts waterlogged");
  require(world.removeBlock(rail), "removeBlock writes the fluid state");
  require(world.getBlock(rail).is(Blocks::WATER),
          "the rail's water stays behind");
}

// UpdateShape on a waterlogged host schedules its own water tick, one per
// block class (rail / wall / coral fan), independent of LiquidBlock.
void hostUpdateShapeSchedulesWater() {
  const auto check = [](const BlockState &wet, const char *what) {
    World world;
    flatFloor(world);
    const Pos pos{0, 64, 0};
    world.rawSetBlock(pos, wet);
    require(world.fluidTickCount() == 0, "the queue starts empty");
    world.updateFromNeighbourShapes(pos, world.getBlock(pos));
    require(world.fluidTickCount() > 0,
            std::string("a waterlogged ") + what +
                " schedule its water tick from updateShape");
  };
  check(Blocks::RAIL.defaultState().with("waterlogged", "true"), "rail");
  check(Blocks::COBBLESTONE_WALL.defaultState().with("waterlogged", "true"),
        "wall");
  check(Blocks::DEAD_TUBE_CORAL_FAN.defaultState(), "coral fan");

  // A dry host schedules nothing.
  World dry;
  flatFloor(dry);
  const Pos pos{0, 64, 0};
  dry.rawSetBlock(pos, Blocks::COBBLESTONE_WALL.defaultState());
  dry.updateFromNeighbourShapes(pos, dry.getBlock(pos));
  require(dry.fluidTickCount() == 0, "a dry host schedules no water tick");
}

// Blocker 4: the queue identity comes from the CALLBACK state, not a re-read.
// A callback handed a flowing-water state must queue flowing_water even though
// the world at that position holds a source by the time it runs.
void callbackStateDecidesIdentity() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0)); // source, the state on disk

  // Hand the entry point a FLOWING state while the world keeps its source.
  const BlockState flowing = waterLevel(4);
  liquidNeighborChanged(world, pos, flowing);
  require(world.fluidTickCount() == 1, "one entry is queued");
  const auto ticks = world.fluidTicks();
  require(ticks.size() == 1, "the snapshot has one entry");
  require(ticks[0].second.fluid() == &Fluids::flowingWater(),
          "the entry carries the callback's flowing_water, not the world's "
          "water source");
  require(ticks[0].first.trigger - world.gameTime() ==
              Fluids::flowingWater().tickDelay(),
          "the delay is the callback's too");

  // The reverse: a source callback over a flowing cell queues water.
  World reverse;
  flatFloor(reverse);
  reverse.rawSetBlock(pos, waterLevel(4));
  liquidNeighborChanged(reverse, pos, waterLevel(0));
  require(reverse.fluidTicks().at(0).second.fluid() == &Fluids::water(),
          "the source identity comes from the callback as well");
}

// Blocker 4: a same-fluid level change still runs onPlace. flags 512 skips it.
void sameFluidLevelChangeRunsOnPlace() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0));
  require(world.fluidTickCount() == 0, "the queue starts empty");
  // Same block, different level: LevelChunk still calls onPlace.
  require(world.setBlock(pos, waterLevel(3)), "the level write lands");
  require(world.fluidTickCount() == 1,
          "re-levelling water onto water requests a tick");
  require(world.fluidTicks().at(0).second.fluid() == &Fluids::flowingWater(),
          "the entry follows the new level's fluid identity");

  // flags 512 suppresses onPlace entirely (LevelChunk.java:339).
  World suppressed;
  flatFloor(suppressed);
  suppressed.rawSetBlock(pos, waterLevel(0));
  require(suppressed.setBlock(pos, waterLevel(2), 512),
          "the flagged write lands");
  require(suppressed.fluidTickCount() == 0, "flags 512 skips onPlace");

  // A same-state write is a LevelChunk no-op and never reaches onPlace.
  World same;
  flatFloor(same);
  same.rawSetBlock(pos, waterLevel(0));
  require(!same.setBlock(pos, waterLevel(0)),
          "writing the identical state reports no change");
  require(same.fluidTickCount() == 0, "no tick for an unchanged state");
}

// P-01: a waterlogged host moved by a piston. The moved block arrives at the
// destination with its waterlogged property cleared when the moving entity
// completes (PistonMovingBlockEntity.java clears WATERLOGGED on a non-air
// final state), so the water does not travel with the block.
void pistonMovesWaterloggedHost() {
  World world;
  flatFloor(world);
  const Pos pistonPos{0, 64, 0};
  const Pos hostPos{1, 64, 0};
  const Pos dest{2, 64, 0};
  world.rawSetBlock(pistonPos,
                    Blocks::PISTON.defaultState().with("facing", "east"));
  world.rawSetBlock(hostPos,
                    Blocks::COBBLESTONE_WALL.defaultState().with(
                        "waterlogged", "true"));
  world.rawSetBlock({0, 64, -1}, Blocks::REDSTONE_BLOCK.defaultState());
  // Drive the base directly, exactly like the piston tests do: a raw write
  // does not run the neighbor pass that would power it.
  pistonNeighborChanged(world, pistonPos, world.getBlock(pistonPos));
  const auto startState = world.getBlock(hostPos);
  require(waterloggedOf(startState), "the host starts waterlogged");
  // Extension needs three ticks; the moving entity completes one tick later.
  for (int i = 0; i < 6; ++i)
    world.tick();
  const auto moved = world.getBlock(dest);
  require(moved.is(Blocks::COBBLESTONE_WALL),
          std::string("the waterlogged wall reaches the destination, got ") +
              std::string(moved.shortName()));
  require(!waterloggedOf(moved),
          "the moving piston clears waterlogged when it completes");
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
    containerFillRules();
    spreadFillsHosts();
    flowingWaterDoesNotFillHosts();
    hostDrainOnSupportLoss();
    railDrainKeepsWater();
    hostUpdateShapeSchedulesWater();
    callbackStateDecidesIdentity();
    sameFluidLevelChangeRunsOnPlace();
    pistonMovesWaterloggedHost();
    std::cout << "fluid block tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid block test failure: " << error.what() << "\n";
    return 1;
  }
}
