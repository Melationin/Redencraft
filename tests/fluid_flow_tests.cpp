#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"
#include "redstone/fluid_flow.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

// F-3: FlowingFluid spread. These drive the algorithm directly through
// fluidSpread / fluidTick with explicit scheduled ticks, so they do not depend on
// the block-side wiring that task F-5 adds.

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
int levelOf(const World &world, Pos pos) {
  return std::stoi(world.getBlock(pos).get("level", "-1"));
}

// A stone floor at y=63 and air above, over a 3x3 chunk area.
void flatFloor(World &world) {
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      world.loadChunk(x, z);
  for (int x = -16; x <= 15; ++x)
    for (int z = -16; z <= 15; ++z)
      world.rawSetBlock({x, 63, z}, Blocks::SMOOTH_STONE.defaultState());
}

// Step the fluid simulation by ticking every fluid block once per round. The
// block-side wiring that schedules fluid ticks on setBlock is task F-5, so F-3
// drives the algorithm directly: snapshot the fluid positions, then tick each.
void stepFluids(World &world, int rounds) {
  for (int round = 0; round < rounds; ++round) {
    std::vector<Pos> fluids;
    for (int x = -16; x <= 15; ++x)
      for (int z = -16; z <= 15; ++z)
        for (int y = 60; y <= 67; ++y) {
          const Pos pos{x, y, z};
          if (isLiquidBlock(world.getBlock(pos)))
            fluids.push_back(pos);
        }
    for (const Pos pos : fluids) {
      const BlockState state = world.getBlock(pos);
      if (!isLiquidBlock(state))
        continue;
      fluidTick(world, pos, state, fluidStateOf(state));
    }
  }
}

// 1. Horizontal spread: a source on the floor spreads 8-level in each of the
// four directions and stops after the seventh step.
void horizontalSpread() {
  World world;
  flatFloor(world);
  const Pos source{0, 64, 0};
  world.rawSetBlock(source, waterLevel(0));

  // Tick the source once; it should spread to the four neighbours at level 1.
  fluidSpread(world, source, world.getBlock(source), fluidStateOf(world.getBlock(source)));
  for (const Pos n : {Pos{1, 64, 0}, Pos{-1, 64, 0}, Pos{0, 64, 1}, Pos{0, 64, -1}})
    require(levelOf(world, n) == 1,
            "the four neighbours get level 1, got " +
                std::to_string(levelOf(world, n)));

  // Run enough ticks for the front to reach the seventh ring and stop.
  stepFluids(world, 8);
  require(levelOf(world, {7, 64, 0}) == 7, "the seventh ring is level 7");
  require(world.getBlock({8, 64, 0}).is(Blocks::AIR),
          "spread stops before the eighth ring");
}

// 2. Drain: removing the source lets the flowing water fall back to air.
void drainAfterSourceRemoved() {
  World world;
  flatFloor(world);
  const Pos source{0, 64, 0};
  world.rawSetBlock(source, waterLevel(0));
  fluidSpread(world, source, world.getBlock(source), fluidStateOf(world.getBlock(source)));
  require(levelOf(world, {1, 64, 0}) == 1, "the neighbour starts at level 1");

  world.rawSetBlock(source, Blocks::AIR.defaultState());
  stepFluids(world, 12);
  require(world.getBlock({1, 64, 0}).is(Blocks::AIR),
          "the flowing water drains to air");
}

// 3. Waterfall: a source with a hole below produces a falling full column.
void waterfall() {
  World world;
  flatFloor(world);
  // A one-block hole directly under the source position.
  world.rawSetBlock({0, 63, 0}, Blocks::AIR.defaultState());
  world.rawSetBlock({0, 62, 0}, Blocks::AIR.defaultState());
  const Pos source{0, 64, 0};
  world.rawSetBlock(source, waterLevel(0));

  fluidSpread(world, source, world.getBlock(source), fluidStateOf(world.getBlock(source)));
  require(levelOf(world, {0, 63, 0}) == 8,
          "the falling block below is level 8");
  const FluidState below = fluidStateOf(world.getBlock({0, 63, 0}));
  require(below.isFalling(), "the block below is a falling fluid");
  require(below.getAmount() == 8, "the falling fluid is full");
}

// 4. Source conversion: two adjacent sources around an empty cell make it a
// source for water; lava does not convert.
void sourceConversion() {
  World world;
  flatFloor(world);
  // Two sources at (0,64,0) and (2,64,0); the cell between is empty and sits on
  // stone, which is what vanilla requires.
  world.rawSetBlock({0, 64, 0}, waterLevel(0));
  world.rawSetBlock({2, 64, 0}, waterLevel(0));
  const Pos middle{1, 64, 0};
  const FluidState state = fluidNewLiquid(world, middle, world.getBlock(middle),
                                          Fluids::water());
  require(state.isSource(), "two neighbouring sources convert the middle to a source");
  require(state.is(Fluids::water()),
          "the converted state is the source fluid, not flowing_water");

  // Lava has canConvertToSource() == false.
  world.rawSetBlock({0, 64, 8}, lavaLevel(0));
  world.rawSetBlock({2, 64, 8}, lavaLevel(0));
  const Pos lavaMiddle{1, 64, 8};
  const FluidState lava = fluidNewLiquid(world, lavaMiddle,
                                         world.getBlock(lavaMiddle), Fluids::lava());
  require(!lava.isSource(), "lava does not convert to a source");
}

// 5. Slope preference: a hole on one side wins over flat neighbours, so only
// that direction receives fluid.
void slopePreference() {
  World world;
  flatFloor(world);
  // A hole two blocks east of the source: (3,63,0) is open.
  world.rawSetBlock({3, 63, 0}, Blocks::AIR.defaultState());
  const Pos source{0, 64, 0};
  world.rawSetBlock(source, waterLevel(0));

  fluidSpread(world, source, world.getBlock(source), fluidStateOf(world.getBlock(source)));
  require(levelOf(world, {1, 64, 0}) == 1, "the path toward the hole gets fluid");
  require(world.getBlock({0, 64, 1}).is(Blocks::AIR),
          "a flat neighbour away from the hole gets nothing");
  require(world.getBlock({-1, 64, 0}).is(Blocks::AIR),
          "the opposite flat neighbour gets nothing");
}

// 6. Level recomputation: a flowing fluid takes the highest neighbour minus the
// drop-off, and a same-family fluid above forces the falling full state.
void levelRecomputation() {
  World world;
  flatFloor(world);
  // Neighbour at level 2 to the east, nothing else.
  world.rawSetBlock({1, 64, 0}, waterLevel(6)); // amount 2
  const Pos target{0, 64, 0};
  const FluidState fromNeighbor =
      fluidNewLiquid(world, target, world.getBlock(target), Fluids::water());
  require(fromNeighbor.getAmount() == 1,
          "the level is highest neighbour minus dropOff (2 - 1 = 1)");

  // A same-family fluid above forces flowing(8, falling).
  world.rawSetBlock({0, 65, 0}, waterLevel(0));
  const FluidState fromAbove =
      fluidNewLiquid(world, target, world.getBlock(target), Fluids::water());
  require(fromAbove.isFalling() && fromAbove.getAmount() == 8,
          "a fluid above forces the falling full state");

  // With no same-family neighbour at all the position empties.
  world.rawSetBlock({0, 65, 0}, Blocks::AIR.defaultState());
  world.rawSetBlock({1, 64, 0}, Blocks::AIR.defaultState());
  const FluidState empty =
      fluidNewLiquid(world, target, world.getBlock(target), Fluids::water());
  require(empty.isEmpty(), "no neighbours means the fluid drains");
}

// 7. getHeight: a same-family fluid above makes the surface a full block.
void height() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(4)); // amount 4
  const FluidState state = fluidStateOf(world.getBlock(pos));
  require(fluidHeight(world, pos, state) == state.getOwnHeight(),
          "no fluid above means the own height");
  require(fluidHeight(world, pos, state) == 4.0f / 9.0f,
          "own height is amount / 9");

  world.rawSetBlock({0, 65, 0}, waterLevel(0));
  require(fluidHeight(world, pos, state) == 1.0f,
          "a same-family fluid above raises the height to 1.0");
}

// 8. getFlow: a higher neighbour pushes the flow toward the lower side; a
// falling fluid against a blocking face gains a downward component.
void flowVector() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(2)); // amount 6
  // A higher neighbour to the west pushes flow east (+x).
  world.rawSetBlock({-1, 64, 0}, waterLevel(0)); // source, own height 8/9
  const FluidState state = fluidStateOf(world.getBlock(pos));
  const Vec3 flow = fluidFlow(world, pos, state);
  require(flow.x > 0.0, "the flow points away from the higher neighbour");
  require(std::abs(flow.y) < 1e-9, "a non-falling fluid has no vertical flow");

  // Falling fluid with a solid face: the flow gains -6 on y before normalising.
  world.rawSetBlock(pos, waterLevel(8)); // falling full
  world.rawSetBlock({1, 64, 0}, Blocks::SMOOTH_STONE.defaultState());
  const FluidState falling = fluidStateOf(world.getBlock(pos));
  require(falling.isFalling(), "level 8 decodes as falling");
  const Vec3 down = fluidFlow(world, pos, falling);
  require(down.y < 0.0, "a falling fluid with a blocked face flows downward");
}

// 9. Queries are side-effect free.
void queriesDoNotMutate() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(3));
  world.rawSetBlock({-1, 64, 0}, waterLevel(0));
  const BlockState before = world.getBlock(pos);
  const auto ticks = world.scheduledTickCount();
  for (int i = 0; i < 32; ++i) {
    const FluidState state = fluidStateOf(world.getBlock(pos));
    (void)fluidFlow(world, pos, state);
    (void)fluidHeight(world, pos, state);
    (void)fluidNewLiquid(world, pos, world.getBlock(pos), Fluids::water());
  }
  require(world.getBlock(pos) == before, "queries do not rewrite the block");
  require(world.scheduledTickCount() == ticks, "queries do not schedule ticks");
}

// fluidTick on a non-source rewrites the level and re-schedules itself.
void tickReschedulesNonSource() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(6)); // amount 2, no source nearby
  const BlockState before = world.getBlock(pos);
  const auto beforeTicks = world.fluidTickCount();
  fluidTick(world, pos, before, fluidStateOf(before));
  require(world.getBlock(pos).is(Blocks::AIR),
          "an unreplenished flowing fluid turns to air");
  require(world.fluidTickCount() == beforeTicks,
          "draining to air does not schedule another tick");

  // A fluid that only changes level re-schedules itself.
  World again;
  flatFloor(again);
  const Pos p{0, 64, 0};
  again.rawSetBlock(p, waterLevel(6));
  again.rawSetBlock({1, 64, 0}, waterLevel(0)); // a source raises the level
  const BlockState b2 = again.getBlock(p);
  const auto t2 = again.fluidTickCount();
  fluidTick(again, p, b2, fluidStateOf(b2));
  require(!again.getBlock(p).is(Blocks::AIR),
          "a replenished fluid keeps its block");
  require(again.fluidTickCount() == t2 + 1,
          "a level change schedules the next fluid tick");
}
} // namespace

int main() {
  try {
    horizontalSpread();
    drainAfterSourceRemoved();
    waterfall();
    sourceConversion();
    slopePreference();
    levelRecomputation();
    height();
    flowVector();
    queriesDoNotMutate();
    tickReschedulesNonSource();
    std::cout << "fluid flow tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid flow test failure: " << error.what() << "\n";
    return 1;
  }
}
