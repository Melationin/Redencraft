#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include "redstone/shapes.hpp"
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

// Step through the real scheduling path instead: schedule a fluid tick and let
// World::tick collect and dispatch it. Use `follow` to queue the ticks that the
// previous round created, which is what F-5 will do from setBlock.
void stepScheduled(World &world, int rounds) {
  const auto follow = [&world] {
    for (int x = -16; x <= 15; ++x)
      for (int z = -16; z <= 15; ++z)
        for (int y = 60; y <= 67; ++y) {
          const Pos pos{x, y, z};
          const BlockState state = world.getBlock(pos);
          if (!isLiquidBlock(state))
            continue;
          const FluidState fluid = fluidStateOf(state);
          if (!world.hasFluidTick(pos, FluidRef(fluid.getType())))
            world.scheduleFluidTick(pos, FluidRef(fluid.getType()), 0);
        }
  };
  for (int round = 0; round < rounds; ++round) {
    follow();
    world.tick();
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

// 3. Waterfall: a source over a drop forms a falling column that lands on the
// platform below and spreads out along it.
void waterfall() {
  World world;
  // A platform at y=62 with open air above, so the column lands on it and has
  // room to spread sideways at y=63.
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      world.loadChunk(x, z);
  for (int x = -16; x <= 15; ++x)
    for (int z = -16; z <= 15; ++z)
      world.rawSetBlock({x, 62, z}, Blocks::SMOOTH_STONE.defaultState());

  const Pos source{0, 64, 0};
  world.rawSetBlock(source, waterLevel(0));

  // One spread puts the falling column into the cell directly below.
  fluidSpread(world, source, world.getBlock(source), fluidStateOf(world.getBlock(source)));
  require(levelOf(world, {0, 63, 0}) == 8,
          "the block below the source is level 8");
  const FluidState below = fluidStateOf(world.getBlock({0, 63, 0}));
  require(below.isFalling(), "the block below is a falling fluid");
  require(below.getAmount() == 8, "the falling fluid is full");

  // Continue through the real scheduling path: the column lands on the platform
  // and spreads horizontally along y=63.
  stepScheduled(world, 8);
  require(isLiquidBlock(world.getBlock({1, 63, 0})),
          "the landed water spreads east along the platform");
  require(isLiquidBlock(world.getBlock({0, 63, 1})),
          "the landed water spreads south along the platform");
  require(world.getBlock({0, 62, 0}).is(Blocks::SMOOTH_STONE),
          "the platform is not replaced by the fluid");
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
  const auto blockTicks = world.scheduledTickCount();
  const auto fluidTicks = world.fluidTickCount();
  for (int i = 0; i < 32; ++i) {
    const FluidState state = fluidStateOf(world.getBlock(pos));
    (void)fluidFlow(world, pos, state);
    (void)fluidHeight(world, pos, state);
    (void)fluidNewLiquid(world, pos, world.getBlock(pos), Fluids::water());
  }
  require(world.getBlock(pos) == before, "queries do not rewrite the block");
  require(world.scheduledTickCount() == blockTicks,
          "queries schedule no block ticks");
  require(world.fluidTickCount() == fluidTicks,
          "queries schedule no fluid ticks");
}

// An explicit tick for minecraft:empty must be consumed without touching the
// block. EmptyFluid.tick is a no-op and its height is the constant 0.
void emptyFluidTickIsInert() {
  World world;
  flatFloor(world);
  const Pos stonePos{0, 64, 0};
  world.rawSetBlock(stonePos, Blocks::SMOOTH_STONE.defaultState());
  const Pos airPos{1, 64, 0};

  // Height of an empty state next to other empty states stays zero.
  require(fluidHeight(world, airPos, fluidStateOf(world.getBlock(airPos))) == 0.0f,
          "an empty fluid's height is zero, even with empty above");

  require(world.scheduleFluidTick(stonePos, FluidRef(Fluids::empty()), 0),
          "an empty-fluid tick can be queued");
  world.tick();
  require(world.fluidTickCount() == 0, "the empty tick was consumed");
  require(world.getBlock(stonePos).is(Blocks::SMOOTH_STONE),
          "an empty-fluid tick does not delete the stone");

  // The same for a water tick pointing at a position that holds no water.
  require(world.scheduleFluidTick(airPos, FluidRef(Fluids::water()), 0),
          "a water tick against air is queued");
  world.tick();
  require(world.getBlock(airPos).is(Blocks::AIR),
          "a stale water tick does not turn air into water");
}

// 3b. Glass is a solid support for source conversion even though it is not a
// redstone conductor; a 1/16 carpet is not solid, so it does not block sideways
// flow the way a stone wall would.
void soliditySemantics() {
  World world;
  flatFloor(world);
  // Two sources around a gap whose support is glass (solid, non-conductor).
  world.rawSetBlock({0, 64, 3}, waterLevel(0));
  world.rawSetBlock({2, 64, 3}, waterLevel(0));
  world.rawSetBlock({1, 63, 3}, Blocks::GLASS.defaultState());
  const Pos gap{1, 64, 3};
  const FluidState converted =
      fluidNewLiquid(world, gap, world.getBlock(gap), Fluids::water());
  require(converted.isSource(),
          "glass under the gap lets two sources convert it");

  // A carpet is only 1/16 tall, so it is not solid: water flowing from a
  // neighbour must replace it, and replacing it must drop its loot first
  // (WaterFluid.beforeDestroyingBlock -> Block.dropResources).
  World carpet;
  flatFloor(carpet);
  const Pos carpetSource{0, 64, 8};
  const Pos carpetCell{1, 64, 8};
  carpet.rawSetBlock(carpetSource, waterLevel(0));
  carpet.rawSetBlock(carpetCell, Blocks::WHITE_CARPET.defaultState());
  fluidSpread(carpet, carpetSource, carpet.getBlock(carpetSource),
              fluidStateOf(carpet.getBlock(carpetSource)));
  require(carpet.getBlock(carpetCell).is(Blocks::WATER),
          std::string("a 1/16 carpet does not block the fluid and is replaced by it, got ") +
              std::string(carpet.getBlock(carpetCell).shortName()));

  // The carpet dropped its item before the fluid was written.
  bool dropped = false;
  for (const auto &entity : carpet.entities) {
    if (!entity || entity->isRemoved())
      continue;
    if (auto *item = dynamic_cast<ItemEntity *>(entity.get()))
      if (item->item.id == "minecraft:white_carpet")
        dropped = true;
  }
  require(dropped, "the replaced carpet drops its item before the fluid lands");
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
    emptyFluidTickIsInert();
    soliditySemantics();
    tickReschedulesNonSource();
    std::cout << "fluid flow tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid flow test failure: " << error.what() << "\n";
    return 1;
  }
}
