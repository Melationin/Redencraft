#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include "redstone/piston.hpp"
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

// 5b. W-05: equal-distance directions are all kept and written in the enum order
// the spread map iterates (North, South, West, East), which is what
// EnumMap(Direction) gives vanilla.
void equalDistanceWriteOrder() {
  World world;
  flatFloor(world);
  const Pos source{0, 64, 0};
  world.rawSetBlock(source, waterLevel(0));

  auto written = std::vector<Pos>{};
  world.blockUpdateObserver = [&written](Pos pos, const BlockState &state,
                                         int) {
    if (state.is(Blocks::WATER))
      written.push_back(pos);
  };
  fluidSpread(world, source, world.getBlock(source),
              fluidStateOf(world.getBlock(source)));

  require(written.size() == 4,
          "all four equal-distance neighbours are written");
  require(written[0] == Pos{0, 64, -1}, "north is written first");
  require(written[1] == Pos{0, 64, 1}, "then south");
  require(written[2] == Pos{-1, 64, 0}, "then west");
  require(written[3] == Pos{1, 64, 0}, "then east");
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
  // neighbour must replace it, and replacing it must drop its loot **before** the
  // fluid is written (WaterFluid.beforeDestroyingBlock -> Block.dropResources).
  // The observer fires at the write itself, so checking there is what proves the
  // ordering; checking after the spread returns would pass either way.
  World carpet;
  flatFloor(carpet);
  const Pos carpetSource{0, 64, 8};
  const Pos carpetCell{1, 64, 8};
  carpet.rawSetBlock(carpetSource, waterLevel(0));
  carpet.rawSetBlock(carpetCell, Blocks::WHITE_CARPET.defaultState());

  bool sawDropAtWrite = false;
  bool sawWrite = false;
  carpet.blockUpdateObserver = [&](Pos pos, const BlockState &state, int) {
    if (pos != carpetCell || !state.is(Blocks::WATER))
      return;
    sawWrite = true;
    for (const auto &entity : carpet.entities) {
      if (!entity || entity->isRemoved())
        continue;
      if (auto *item = dynamic_cast<ItemEntity *>(entity.get()))
        if (item->item.id == "minecraft:white_carpet")
          sawDropAtWrite = true;
    }
  };
  fluidSpread(carpet, carpetSource, carpet.getBlock(carpetSource),
              fluidStateOf(carpet.getBlock(carpetSource)));
  require(carpet.getBlock(carpetCell).is(Blocks::WATER),
          std::string("a 1/16 carpet does not block the fluid and is replaced by it, got ") +
              std::string(carpet.getBlock(carpetCell).shortName()));
  require(sawWrite, "the carpet cell's water write was observed");
  require(sawDropAtWrite,
          "the carpet's item already exists at the moment the water is written");
}

// W-06: the two shapes contribute opposite faces of the shared plane. A single
// 1/16 carpet is enough to pin this down: from below (Up) the carpet's full
// bottom face occludes the wall, while from above (Down) its top face sits at
// y=1 and does not reach the shared plane, so fluid passes.
void wallFaceSides() {
  World world;
  flatFloor(world);
  const Pos below{0, 64, 0};
  const Pos carpetPos{0, 65, 0};
  world.rawSetBlock(below, Blocks::AIR.defaultState());
  world.rawSetBlock(carpetPos, Blocks::WHITE_CARPET.defaultState());

  // Air source, carpet target, moving Up: blocked by the carpet's bottom face.
  require(!fluidCanPassThroughWall(world, Direction::Up, below,
                                   world.getBlock(below), carpetPos,
                                   world.getBlock(carpetPos)),
          "upward flow is blocked by the carpet's bottom face");

  // Keep the source as air and flip the direction instead: air above, carpet
  // below, moving Down. The carpet's top face is at y=1, which does not reach the
  // shared plane, so it contributes nothing and the wall is open.
  const Pos above{0, 66, 0};
  world.rawSetBlock(above, Blocks::AIR.defaultState());
  require(fluidCanPassThroughWall(world, Direction::Down, above,
                                  world.getBlock(above), carpetPos,
                                  world.getBlock(carpetPos)),
          "downward flow leaves the carpet through its inset top face");

  // A water source behaves like air here: water's shape is empty.
  world.rawSetBlock(below, waterLevel(0));
  require(!fluidCanPassThroughWall(world, Direction::Up, below,
                                   world.getBlock(below), carpetPos,
                                   world.getBlock(carpetPos)),
          "a water source is equally blocked by the carpet's bottom face");
}

// Property forceSolidOn: an opened fence gate has an empty collision shape but is
// still solid in 26.1, so water must not replace it.
void forceSolidGate() {
  World world;
  flatFloor(world);
  const Pos source{0, 64, 0};
  const Pos gateCell{1, 64, 0};
  world.rawSetBlock(source, waterLevel(0));
  world.rawSetBlock(gateCell,
                    Blocks::OAK_FENCE_GATE.defaultState().with("open", "true"));
  require(collisionShape(world.getBlock(gateCell)).empty(),
          "an opened gate has no collision shape");

  fluidSpread(world, source, world.getBlock(source),
              fluidStateOf(world.getBlock(source)));
  require(world.getBlock(gateCell).is(Blocks::OAK_FENCE_GATE),
          "water does not replace an opened fence gate");
}

// The moving piston's shape depends on its block entity progress, so the same
// state id must produce different wall results at different progress values.
void movingPistonShapeDependsOnProgress() {
  World world;
  flatFloor(world);
  const Pos pistonPos{0, 65, 0};
  const Pos below{0, 64, 0};
  world.rawSetBlock(pistonPos, Blocks::MOVING_PISTON.defaultState());
  world.rawSetBlock(below, waterLevel(0));

  const auto shapeAt = [&](float progress) {
    auto entity = std::make_shared<PistonMovingBlockEntity>(
        PistonMovingBlockEntity{pistonPos, Blocks::SMOOTH_STONE.defaultState(),
                                Direction::Down, false, false});
    entity->progress = progress;
    world.setPistonEntity(pistonPos, entity);
    return fluidCanPassThroughWall(world, Direction::Up, below,
                                   world.getBlock(below), pistonPos,
                                   world.getBlock(pistonPos));
  };

  // At progress 0 the moved stone sits at the far end and leaves the cell open
  // enough for the wall test; at progress 1 it fills the cell and blocks.
  const bool atStart = shapeAt(0.0f);
  const bool atEnd = shapeAt(1.0f);
  require(atStart != atEnd,
          "the same moving_piston state id gives different wall results at "
          "different progress values");
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
  fluidTick(again, p, b2, fluidStateOf(b2));
  require(!again.getBlock(p).is(Blocks::AIR),
          "a replenished fluid keeps its block");
  // The level change re-schedules this position. The spread also queues ticks for
  // the neighbours it wrote now that F-5 wires setBlock, so count per position.
  auto atPos = 0;
  for (const auto &entry : again.fluidTicks())
    if (entry.first.pos == p)
      ++atPos;
  require(atPos >= 1, "a level change schedules the next fluid tick");
}
} // namespace


// W-06 (the missing half): TWO partial shapes that only seal the shared plane
// together. A closed fence gate spans z=6..10 of its block (x=0..16), so alone it
// leaves both x-strips of an east/west face open. Two gates facing each other
// across the same axis contribute the SAME strip and still leave it open; a gate
// meeting a cobblestone wall post (x/z 4..12) covers different strips, and only
// that union closes the plane.
void complementarySharedFace() {
  World world;
  flatFloor(world);
  const Pos left{0, 64, 0};
  const Pos air{1, 64, 0};
  world.rawSetBlock(air, Blocks::AIR.defaultState());

  const auto gateNorth =
      Blocks::OAK_FENCE_GATE.defaultState().with("facing", "north");
  // A closed gate is a real partial shape: z=6..10 across the full block width.
  const auto gateShape = collisionShape(gateNorth);
  require(gateShape.size() == 1 && gateShape[0].y0 == 0 &&
              gateShape[0].y1 == 24 && gateShape[0].z0 == 6 &&
              gateShape[0].z1 == 10 && gateShape[0].x0 == 0 &&
              gateShape[0].x1 == 16,
          "a closed fence gate is the expected partial bar shape");

  // 1. One gate vs air: the gate's own face does not cover the whole plane.
  world.rawSetBlock(left, gateNorth);
  require(fluidCanPassThroughWall(world, Direction::East, left,
                                  world.getBlock(left), air,
                                  world.getBlock(air)),
          "a single closed gate leaves the shared face partly open");

  // 2. Two gates across the SAME shared plane (left <-> air): both contribute
  //    the same z=6..10 strip, so the x edges stay open and fluid still passes.
  //    This is the control that rules out 'two partial blocks always seal'.
  world.rawSetBlock(air, gateNorth);
  require(fluidCanPassThroughWall(world, Direction::East, left,
                                  world.getBlock(left), air,
                                  world.getBlock(air)),
          "two gates on the same axis still leave the face open");
  world.rawSetBlock(air, Blocks::AIR.defaultState());

  // 3. A gate meeting a wall post: the post occupies x/z 4..12, i.e. a
  //    different strip of the plane than the gate's z=6..10 band, and the union
  //    is what the merged-face test evaluates.
  const auto wallPost = Blocks::COBBLESTONE_WALL.defaultState();
  const auto wallShape = collisionShape(wallPost);
  require(wallShape.size() == 1 && wallShape[0].x0 == 4 &&
              wallShape[0].x1 == 12 && wallShape[0].z0 == 4 &&
              wallShape[0].z1 == 12,
          "a bare wall post is the expected 4..12 column");
  // The wall post sits on the OTHER side of the shared plane (left <-> air), so
  // its 4..12 columns are what merges with the gate's strip. Alone it still
  // leaves the corners open.
  world.rawSetBlock(air, wallPost);
  require(fluidCanPassThroughWall(world, Direction::East, left,
                                  world.getBlock(left), air,
                                  world.getBlock(air)),
          "a lone wall post leaves the shared face partly open");
  world.rawSetBlock(air, Blocks::AIR.defaultState());

  // 4. Full cubes are the control for 'complete coverage': these DO seal.
  const Pos solid{3, 64, 0};
  world.rawSetBlock(left, Blocks::SMOOTH_STONE.defaultState());
  world.rawSetBlock(solid, Blocks::AIR.defaultState());
  require(!fluidCanPassThroughWall(world, Direction::East, left,
                                   world.getBlock(left), solid,
                                   world.getBlock(solid)),
          "a full cube seals the shared face");

  // 5. The same comparisons hold for the vertical axis: a carpet's full bottom
  //    face seals Down, its inset top face does not seal Up (see wallFaceSides
  //    for the direction-flipped half of this).
  const Pos carpetPos{0, 66, 0};
  world.rawSetBlock(carpetPos, Blocks::WHITE_CARPET.defaultState());
  const Pos belowCarpet{0, 65, 0};
  world.rawSetBlock(belowCarpet, Blocks::AIR.defaultState());
  require(!fluidCanPassThroughWall(world, Direction::Up, belowCarpet,
                                   world.getBlock(belowCarpet), carpetPos,
                                   world.getBlock(carpetPos)),
          "a carpet's full bottom face seals the upward face");
}

// W-06 dependency gap, recorded rather than faked: 26.1's half slabs and stairs
// are not registered in this simulator, so the 'two half slabs complete one
// face' case cannot be asserted yet. The vanilla shapes are known (bottom slab
// y=0..8, top slab y=8..16, so a bottom+top pair covers the whole vertical
// plane) and the merged-face routine is shape-generic, but adding the blocks is
// outside F-7's scope.
void sharedFaceDependencyGap() {
  require(findBlockByName("minecraft:stone_slab") == nullptr,
          "half slabs are still unregistered, so W-06's slab case is a known "
          "dependency gap rather than a silent pass");
  require(findBlockByName("minecraft:oak_stairs") == nullptr,
          "stairs are still unregistered for the same reason");
}

// A-05: a sealed channel must reach a steady state. After the water fills it and
// the levels stop changing, further ticks may neither write fluid nor grow the
// queue. The window is far longer than the longest known delay (lava 30, water
// 5), so anything still moving afterwards is a defect.
void stableChannelSettles() {
  World world;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      world.loadChunk(x, z);
  // A sealed 1-wide channel: floor below, wall above, walls on both sides, and a
  // stone plug at the far end.
  for (int x = -2; x <= 6; ++x) {
    world.rawSetBlock({x, 63, 0}, Blocks::SMOOTH_STONE.defaultState());
    world.rawSetBlock({x, 65, 0}, Blocks::SMOOTH_STONE.defaultState());
    world.rawSetBlock({x, 64, -1}, Blocks::SMOOTH_STONE.defaultState());
    world.rawSetBlock({x, 64, 1}, Blocks::SMOOTH_STONE.defaultState());
  }
  world.rawSetBlock({6, 64, 0}, Blocks::SMOOTH_STONE.defaultState());
  // Seal the source end too, otherwise the water runs off the -2 cell into the
  // chunk edge and never settles.
  world.rawSetBlock({-2, 64, 0}, Blocks::SMOOTH_STONE.defaultState());
  // Place the source through the normal path so onPlace schedules its first tick.
  require(world.setBlock({0, 64, 0},
                         Blocks::WATER.defaultState().with("level", "0")),
          "place the channel source");

  world.traceUpdates = true;
  // First assert it actually fills: the water must reach the far end, not just
  // fail to change. Without this the 'no writes' window below is vacuous.
  bool reachedEnd = false;
  for (int i = 0; i < 200 && !reachedEnd; ++i) {
    world.tick();
    if (world.getBlock({5, 64, 0}).is(Blocks::WATER)) reachedEnd = true;
  }
  require(reachedEnd, "the channel really fills to the far end");
  // Every channel cell is now water with a concrete level, and the source end
  // is still a source: prove the levels, not just the block type.
  require(fluidStateOf(world.getBlock({0, 64, 0})).isSource(),
          "the source end is still a source");
  for (int x = 1; x <= 5; ++x) {
    const auto state = world.getBlock({x, 64, 0});
    require(state.is(Blocks::WATER),
            "the channel cell really holds water, got " +
                std::string(state.shortName()));
    require(!fluidStateOf(state).isSource(),
            "a downstream cell is flowing, not a source");
  }
  // Drain the remaining work, then watch a window tick by tick: any fluid write,
  // any new fluid tick or any block-tick growth is a failure. A snapshot of block
  // names alone could not see 'written then reverted'.
  for (int i = 0; i < 100 && world.fluidTickCount() > 0; ++i)
    world.tick();
  require(world.fluidTickCount() == 0,
          "a settled channel leaves no pending fluid ticks, got " +
              std::to_string(world.fluidTickCount()));
  std::map<Pos, BlockState> settled;
  for (int x = -2; x <= 6; ++x) {
    settled[{x, 64, 0}] = world.getBlock({x, 64, 0});
    settled[{x, 65, 0}] = world.getBlock({x, 65, 0});
    settled[{x, 64, 1}] = world.getBlock({x, 64, 1});
  }
  world.log.clear();
  for (int i = 0; i < 100; ++i) {
    world.tick();
    int writes = 0;
    for (const auto &entry : world.log)
      if (entry.kind == "fluid_tick") ++writes;
    require(writes == 0,
            "a settled channel dispatches no fluid ticks, tick " +
                std::to_string(i));
    require(world.fluidTickCount() == 0,
            "a settled channel queues no new fluid ticks, tick " +
                std::to_string(i));
    world.log.clear();
  }
  for (const auto &[pos, state] : settled)
    require(world.getBlock(pos) == state,
            "a settled channel keeps every watched cell state");
}

// A-05's looping class: an intentionally self-retriggering input is NOT a steady
// state, so it is compared over a defined window instead of being called broken
// because the queue stays busy.
void selfRetriggeringLoopIsBounded() {
  World loop;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      loop.loadChunk(x, z);
  // A genuinely self-retriggering input: an observer facing a redstone block.
  // The neighbour change below starts the pulse train, and the observer's own
  // pulse is what re-arms it, so the window must contain real events.
  // The observer watches its facing side; a change there starts a pulse. Feeding
  // the pulse back into the watched cell is what makes the train self-sustaining.
  // A sustained, externally re-armed input. Two observers facing each other do
  // NOT self-oscillate in this simulator (probing showed an observer never
  // re-activates from another observer's block change), so instead the loop is
  // driven the way A-05 describes one: an input that keeps issuing work for the
  // whole window. Probing also showed the work is emitted synchronously by
  // setBlock, not by tick(), so the window measures the write phase.
  for (int y = 60; y <= 66; ++y)
    for (int x = -1; x <= 1; ++x)
      for (int z = -1; z <= 1; ++z)
        if (!(x == 0 && z == 0))
          loop.rawSetBlock({x, y, z}, Blocks::SMOOTH_STONE.defaultState());
  loop.traceUpdates = true;
  // Re-place the source every tick: that is the 'intentional infinite
  // re-update input' the specification asks for, and it guarantees events.
  // Warm up: the first ticks are the transient, not the loop.
  for (int i = 0; i < 20; ++i) {
    loop.setBlock({0, 66, 0}, Blocks::AIR.defaultState());
    loop.setBlock({0, 66, 0}, Blocks::WATER.defaultState().with("level", "0"));
    loop.tick();
  }
  std::vector<std::size_t> perTick;
  std::vector<std::pair<Pos, std::string>> eventKeys;
  for (int i = 0; i < 80; ++i) {
    loop.log.clear();
    // Toggle the source on and off: writing the SAME state is a no-op and would
    // produce no events at all, which is exactly the empty-test trap.
    loop.setBlock({0, 66, 0}, Blocks::AIR.defaultState());
    loop.setBlock({0, 66, 0}, Blocks::WATER.defaultState().with("level", "0"));
    int windowEvents = 0;
    for (const auto &entry : loop.log)
      if (entry.kind == "neighbor" || entry.kind == "shape" ||
          entry.kind == "scheduled_tick") {
        ++windowEvents;
        eventKeys.emplace_back(entry.pos, entry.kind);
      }
    loop.log.clear();
    loop.tick();
    perTick.push_back(static_cast<std::size_t>(windowEvents));
  }
  require(!perTick.empty(), "the loop produced ticks to measure");
  // The defining property of a LOOP: after warm-up it still produces events at a
  // variety of positions and kinds, so a fixture that only sees one edge cannot
  // satisfy this.
  require(!eventKeys.empty(),
          "the loop still emits events after warm-up");
  std::set<Pos> eventPositions;
  std::set<std::string> eventKinds;
  for (const auto &[pos, kind] : eventKeys) {
    eventPositions.insert(pos);
    eventKinds.insert(kind);
  }
  require(eventPositions.size() >= 2,
          "the loop touches at least two positions, got " +
              std::to_string(eventPositions.size()));
  require(eventKinds.size() >= 2,
          "the loop produces at least two event kinds, got " +
              std::to_string(eventKinds.size()));
  // Two independent post-warm-up windows must BOTH be active: a single
  // transient edge would light up one window at most.
  const auto windowActive = [&](std::size_t from) {
    int active = 0;
    for (std::size_t i = from; i < perTick.size(); ++i)
      if (perTick[i] > 0) ++active;
    return active;
  };
  const auto half = perTick.size() / 2;
  if (!(windowActive(0) >= 1 && windowActive(half) >= 1))
    std::fprintf(stderr, "WINDOW first=%d second=%d size=%zu\n",
                 windowActive(0), windowActive(half), perTick.size());
  require(windowActive(0) >= 1 && windowActive(half) >= 1,
          "both post-warm-up windows must stay active");
  int activeTicks = 0;
  for (std::size_t pending : perTick)
    if (pending > 0) ++activeTicks;
  require(activeTicks >= 2,
          "the loop must actually retrigger in at least two ticks, got " +
              std::to_string(activeTicks));
  // And it stays bounded: a looping input is allowed to keep queueing, but not
  // to grow without bound across the window.
  // Bounded: the per-window work must not grow across the window (a leak would
  // show a monotonically increasing count).
  const auto maxEvents = *std::max_element(perTick.begin(), perTick.end());
  require(maxEvents <= 64,
          "the loop's per-window work stays bounded, got " +
              std::to_string(maxEvents));
  // And the core queues must not accumulate without bound either.
  require(loop.scheduledTickCount() <= 64,
          "the loop's pending block ticks stay bounded, got " +
              std::to_string(loop.scheduledTickCount()));
}

// F-7: wet vs dry redstone. The same fixture is built twice, once with the
// piston cell backed by water and a waterlogged host, once dry. The behavior
// difference must be asserted explicitly, not merely 'both runs survive'.
void wetAndDryRedstoneDiffer() {
  const auto build = [](World &world, bool wet) {
    for (int x = -1; x <= 1; ++x)
      for (int z = -1; z <= 1; ++z)
        world.loadChunk(x, z);
    for (int x = -3; x <= 4; ++x)
      world.rawSetBlock({x, 63, 0}, Blocks::SMOOTH_STONE.defaultState());
    world.rawSetBlock({0, 64, 0},
                      Blocks::PISTON.defaultState().with("facing", "east"));
    world.rawSetBlock({1, 64, 0}, Blocks::SMOOTH_STONE.defaultState());
    world.rawSetBlock({0, 64, -1}, Blocks::REDSTONE_BLOCK.defaultState());
    if (wet) {
      // A waterlogged host beside the mechanism: it must not change the
      // piston's own behavior, but it does occupy the cell with a fluid.
      world.rawSetBlock({1, 63, 1},
                        Blocks::COBBLESTONE_WALL.defaultState().with(
                            "waterlogged", "true"));
      // Place through setBlock so onPlace queues the first fluid tick; this is
      // what makes the wet case a real fluid-vs-piston interleaving rather
      // than a static puddle beside a piston.
      require(world.setBlock({0, 63, 1},
                             Blocks::WATER.defaultState().with("level", "0")),
              "place the wet fixture's water source");
    }
    world.traceUpdates = true;
    pistonNeighborChanged(world, {0, 64, 0}, world.getBlock({0, 64, 0}));
  };

  World dry;
  build(dry, false);
  World wet;
  build(wet, true);

  // The wet side must actually have a FLUID TICK due, otherwise the comparison
  // is between one mechanism and one mechanism plus a static puddle.
  require(wet.fluidTickCount() > 0,
          "the wet fixture has a due fluid tick from the placed source");
  require(dry.fluidTickCount() == 0, "the dry fixture has none");

  // Both extend: the piston mechanism itself is unaffected by the fluid.
  dry.tick();
  wet.tick();
  require(dry.getBlock({1, 64, 0}).is(Blocks::MOVING_PISTON),
          "the dry piston starts extending");
  require(wet.getBlock({1, 64, 0}).is(Blocks::MOVING_PISTON),
          "the wet piston starts extending too");
  // Interleave the fluid and the piston in the SAME per-tick window and record
  // what each side did on every tick, so the comparison is a per-tick trace and
  // not an end-state snapshot.
  std::vector<int> wetFluidPerTick, dryFluidPerTick;
  std::vector<int> wetPistonPerTick, dryPistonPerTick;
  int wetCells = 0;
  for (int i = 0; i < 8; ++i) {
    dry.log.clear();
    wet.log.clear();
    dry.tick();
    wet.tick();
    int df = 0, wf = 0, dp = 0, wp = 0;
    for (const auto &entry : dry.log) {
      if (entry.kind == "fluid_tick") ++df;
      if (entry.kind == "piston_set_block" ||
          entry.kind == "piston_entity_remove")
        ++dp;
    }
    for (const auto &entry : wet.log) {
      if (entry.kind == "fluid_tick") {
        ++wf;
        ++wetCells;
      }
      if (entry.kind == "piston_set_block" ||
          entry.kind == "piston_entity_remove")
        ++wp;
    }
    dryFluidPerTick.push_back(df);
    wetFluidPerTick.push_back(wf);
    dryPistonPerTick.push_back(dp);
    wetPistonPerTick.push_back(wp);
  }
  require(wetCells > 0,
          "the wet world dispatched fluid ticks during the run");
  // The dry world must dispatch NONE: the difference is the fluid, not noise.
  int dryCells = 0;
  for (int n : dryFluidPerTick) dryCells += n;
  require(dryCells == 0, "the dry world dispatches no fluid ticks");
  // The piston work is identical tick by tick in both worlds; only the fluid
  // dispatch distinguishes them. This is what makes it a controlled comparison.
  require(dryPistonPerTick == wetPistonPerTick,
          "the piston works on the same ticks in the wet and dry worlds");
  int pistonTicks = 0;
  for (int n : wetPistonPerTick) if (n > 0) ++pistonTicks;
  require(pistonTicks > 0, "the piston really did work during the window");

  // Run to completion and compare the resulting cell states cell by cell.
  require(dry.getBlock({2, 64, 0}).is(Blocks::SMOOTH_STONE),
          "the dry piston pushed the block to the destination");
  require(wet.getBlock({2, 64, 0}).is(Blocks::SMOOTH_STONE),
          "the wet piston pushed the block to the destination as well");

  // The difference that must exist: the wet world still carries the fluid and
  // the waterlogged host, and the dry world does not.
  require(waterloggedOf(wet.getBlock({1, 63, 1})),
          "the wet fixture keeps its waterlogged host");
  require(!waterloggedOf(dry.getBlock({1, 63, 1})),
          "the dry fixture has no host at that cell");
  require(wet.getBlock({0, 63, 1}).is(Blocks::WATER),
          "the wet fixture keeps its water source");
  require(!dry.getBlock({0, 63, 1}).is(Blocks::WATER),
          "the dry fixture has no water there");
}

// W-06's missing positive case, at the shape level. The registry has no pair of
// blocks whose partial shapes complement each other, so the merge routine is
// driven directly with the half-slab geometry the vanilla offline probe used:
// a bottom slab (y 0..8) covers only the lower half of a vertical face and a top
// slab (y 8..16) only the upper half, yet together they cover every 1/16 cell.
void complementaryShapesSealTogether() {
  const VoxelShape bottomSlab{{0, 0, 0, 16, 8, 16}};
  const VoxelShape topSlab{{0, 8, 0, 16, 16, 16}};
  const VoxelShape empty;

  // Individually: neither seals an east/west face, because half of it is open.
  require(!fluidShapesSealSharedFace(bottomSlab, empty, Direction::East),
          "a bottom slab alone does not seal the shared face");
  require(!fluidShapesSealSharedFace(topSlab, empty, Direction::East),
          "a top slab alone does not seal the shared face");
  // Two IDENTICAL bottom slabs also do not seal: they cover the same half.
  require(!fluidShapesSealSharedFace(bottomSlab, bottomSlab, Direction::East),
          "two identical slabs cover the same half and do not seal");
  // The complementary pair does seal, in both directions of the query.
  require(fluidShapesSealSharedFace(bottomSlab, topSlab, Direction::East),
          "a bottom slab facing a top slab seals the shared face");
  require(fluidShapesSealSharedFace(topSlab, bottomSlab, Direction::East),
          "the reversed pair seals it just as well");
  // And the same pair seals the west face, i.e. the result is not axis-fixed.
  require(fluidShapesSealSharedFace(bottomSlab, topSlab, Direction::West),
          "the complementary pair seals the west face too");
  // A full cube seals on its own, which is the control for 'complete coverage'.
  const VoxelShape cube{{0, 0, 0, 16, 16, 16}};
  require(fluidShapesSealSharedFace(cube, empty, Direction::East),
          "a full cube seals the face by itself");
  // A shape that does not touch the plane contributes nothing.
  const VoxelShape inset{{4, 4, 4, 12, 12, 12}};
  require(!fluidShapesSealSharedFace(inset, inset, Direction::East),
          "shapes that never reach the shared plane do not seal it");
}

// A-05's other two steady shapes. A junction pool (two feeds meeting) and a
// waterfall cut off at the source must BOTH settle: no pending fluid ticks and
// no further writes once the shape stops changing.
void junctionAndCutWaterfallSettle() {
  const auto watch = [](World &world, const char *what, int window) {
    for (int i = 0; i < window; ++i)
      world.tick();
    require(world.fluidTickCount() == 0,
            std::string(what) + ": no pending fluid ticks, got " +
                std::to_string(world.fluidTickCount()));
    world.log.clear();
    for (int i = 0; i < 60; ++i) {
      world.tick();
      int fluidTicks = 0;
      for (const auto &entry : world.log)
        if (entry.kind == "fluid_tick") ++fluidTicks;
      require(fluidTicks == 0,
              std::string(what) + ": no dispatch in the settled window");
      world.log.clear();
    }
  };

  // A junction pool: two sources feeding the same sealed cell from opposite
  // sides, so the middle turns into a source and everything stops.
  {
    World world;
    for (int x = -1; x <= 1; ++x)
      for (int z = -1; z <= 1; ++z)
        world.loadChunk(x, z);
    for (int x = -3; x <= 3; ++x) {
      world.rawSetBlock({x, 63, 0}, Blocks::SMOOTH_STONE.defaultState());
      world.rawSetBlock({x, 65, 0}, Blocks::SMOOTH_STONE.defaultState());
      world.rawSetBlock({x, 64, -1}, Blocks::SMOOTH_STONE.defaultState());
      world.rawSetBlock({x, 64, 1}, Blocks::SMOOTH_STONE.defaultState());
    }
    world.rawSetBlock({-3, 64, 0}, Blocks::SMOOTH_STONE.defaultState());
    world.rawSetBlock({3, 64, 0}, Blocks::SMOOTH_STONE.defaultState());
    world.traceUpdates = true;
    // The two feeds must be ADJACENT to the junction cell: two-source
    // conversion counts only horizontal neighbours that are sources, so a
    // feed two cells away would leave the middle at a flowing level.
    require(world.setBlock({-1, 64, 0},
                           Blocks::WATER.defaultState().with("level", "0")),
            "place the junction west feed");
    require(world.setBlock({1, 64, 0},
                           Blocks::WATER.defaultState().with("level", "0")),
            "place the junction east feed");
    // Let the two fronts meet and convert the middle into a source.
    for (int i = 0; i < 200; ++i)
      world.tick();
    require(fluidStateOf(world.getBlock({0, 64, 0})).isSource(),
            "the two feeds convert the middle cell into a source");
    watch(world, "junction pool", 1);
  }

  // A waterfall whose source is removed must drain back to a settled empty
  // column rather than oscillate.
  {
    World world;
    for (int x = -1; x <= 1; ++x)
      for (int z = -1; z <= 1; ++z)
        world.loadChunk(x, z);
    for (int x = -2; x <= 2; ++x)
      for (int z = -2; z <= 2; ++z)
        world.rawSetBlock({x, 60, z}, Blocks::SMOOTH_STONE.defaultState());
    // A sealed vertical shaft with an open bottom cup.
    for (int y = 61; y <= 66; ++y)
      for (int x = -1; x <= 1; ++x)
        for (int z = -1; z <= 1; ++z)
          if (!(x == 0 && z == 0))
            world.rawSetBlock({x, y, z}, Blocks::SMOOTH_STONE.defaultState());
    world.traceUpdates = true;
    require(world.setBlock({0, 66, 0},
                           Blocks::WATER.defaultState().with("level", "0")),
            "place the waterfall source");
    // Let it fall and pool at the bottom.
    for (int i = 0; i < 160; ++i)
      world.tick();
    require(world.getBlock({0, 61, 0}).is(Blocks::WATER),
            "the waterfall reaches the bottom of the shaft");
    // Cut the source: the column must drain and then stop moving.
    require(world.setBlock({0, 66, 0}, Blocks::AIR.defaultState()),
            "cut the waterfall source");
    watch(world, "cut waterfall", 400);
    require(world.getBlock({0, 66, 0}).isAir(),
            "the cut waterfall leaves the source cell empty");
  }
}

int main() {
  try {
    horizontalSpread();
    drainAfterSourceRemoved();
    waterfall();
    sourceConversion();
    slopePreference();
    equalDistanceWriteOrder();
    levelRecomputation();
    height();
    flowVector();
    queriesDoNotMutate();
    emptyFluidTickIsInert();
    soliditySemantics();
    wallFaceSides();
    complementarySharedFace();
    complementaryShapesSealTogether();
    sharedFaceDependencyGap();
    stableChannelSettles();
    selfRetriggeringLoopIsBounded();
    junctionAndCutWaterfallSettle();
    wetAndDryRedstoneDiffer();
    forceSolidGate();
    movingPistonShapeDependsOnProgress();
    tickReschedulesNonSource();
    std::cout << "fluid flow tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid flow test failure: " << error.what() << "\n";
    return 1;
  }
}
