#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"
#include "redstone/fluid_flow.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

// F-4: the differences between water and lava that the flow algorithm consults.
// L-01..L-03 cover water and the shared parameters, M-01..M-03 the lava-only
// rules (random spread delay and the reaction table).

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

// L-01: the registered parameters. These are the constants F-3 landed; this pins
// them so a later refactor cannot quietly change one family's numbers.
void parameters() {
  require(Fluids::water().dropOff() == 1, "water dropOff is 1");
  require(Fluids::water().slopeFindDistance() == 4, "water slope distance is 4");
  require(Fluids::water().tickDelay() == 5, "water tick delay is 5");

  require(Fluids::lava().dropOff() == 2, "lava dropOff is 2");
  require(Fluids::lava().slopeFindDistance() == 2, "lava slope distance is 2");
  require(Fluids::lava().tickDelay() == 30, "lava tick delay is 30");

  // The flowing variants answer the same: the parameters belong to the family.
  require(Fluids::flowingWater().dropOff() == 1, "flowing water shares dropOff");
  require(Fluids::flowingLava().tickDelay() == 30, "flowing lava shares delay");
}

// FluidTags.WATER / LAVA are a separate judgement from isSame: they group the
// source and the flowing variant, and nothing else.
void tags() {
  require(fluidHasTag(Fluids::water(), FluidTag::Water), "water is in the water tag");
  require(fluidHasTag(Fluids::flowingWater(), FluidTag::Water),
          "flowing water is in the water tag");
  require(fluidHasTag(Fluids::lava(), FluidTag::Lava), "lava is in the lava tag");
  require(fluidHasTag(Fluids::flowingLava(), FluidTag::Lava),
          "flowing lava is in the lava tag");

  require(!fluidHasTag(Fluids::water(), FluidTag::Lava),
          "water is not in the lava tag");
  require(!fluidHasTag(Fluids::lava(), FluidTag::Water),
          "lava is not in the water tag");
  require(!fluidHasTag(Fluids::empty(), FluidTag::Water),
          "empty is in no tag");
  require(!fluidHasTag(Fluids::empty(), FluidTag::Lava),
          "empty is in no tag");
}

// L-02: the source-conversion switches are per world and default to on for water
// and off for lava.
void sourceConversionSwitch() {
  World world;
  flatFloor(world);
  // Two sources around the gap, both standing on stone.
  world.rawSetBlock({0, 64, 0}, waterLevel(0));
  world.rawSetBlock({2, 64, 0}, waterLevel(0));
  const Pos gap{1, 64, 0};

  require(world.waterSourceConversion, "water source conversion defaults on");
  require(!world.lavaSourceConversion, "lava source conversion defaults off");
  require(fluidNewLiquid(world, gap, world.getBlock(gap), Fluids::water())
              .isSource(),
          "two sources convert the gap while the rule is on");

  world.waterSourceConversion = false;
  const FluidState off =
      fluidNewLiquid(world, gap, world.getBlock(gap), Fluids::water());
  require(!off.isSource(), "the gap stops converting with the rule off");
  require(!off.isEmpty(), "the gap still holds flowing water");

  // Lava stays off by default even with two neighbours, and the switch can turn
  // it on.
  World lava;
  flatFloor(lava);
  lava.rawSetBlock({0, 64, 0}, lavaLevel(0));
  lava.rawSetBlock({2, 64, 0}, lavaLevel(0));
  require(!fluidNewLiquid(lava, gap, lava.getBlock(gap), Fluids::lava())
               .isSource(),
          "lava does not convert by default");
  lava.lavaSourceConversion = true;
  require(fluidNewLiquid(lava, gap, lava.getBlock(gap), Fluids::lava())
              .isSource(),
          "lava converts once the rule is enabled");
}

// L-03: LavaFluid.canBeReplacedWith only yields to a WATER-tagged fluid once its
// surface is at least 4/9 high, and WaterFluid only yields downward. These go
// through fluidSpread so the whole replace-or-not path runs, not just the helper.
void replacementRules() {
  const auto waterAbove = [](int lavaLevelValue) {
    World world;
    flatFloor(world);
    const Pos above{0, 65, 0};
    const Pos target{0, 64, 0};
    world.rawSetBlock(above, waterLevel(0));
    world.rawSetBlock(target, lavaLevel(lavaLevelValue));
    fluidSpread(world, above, world.getBlock(above),
                fluidStateOf(world.getBlock(above)));
    return world.getBlock(target);
  };

  // lava[level=5] decodes to amount 3, so its height is 3/9 < 0.44444445.
  const BlockState low = waterAbove(5);
  require(low.is(Blocks::LAVA),
          "water above lava[5] leaves the lava alone, got " +
              std::string(low.shortName()));
  // lava[level=4] decodes to amount 4, so its height is 4/9 >= 0.44444445.
  const BlockState high = waterAbove(4);
  require(high.is(Blocks::WATER),
          "water above lava[4] replaces it with water, got " +
              std::string(high.shortName()));

  // Sideways is water's rule, which only allows replacement downward, so lava
  // spreading east into water must leave the water alone.
  World side;
  flatFloor(side);
  const Pos lavaPos{0, 64, 0};
  const Pos waterPos{1, 64, 0};
  side.rawSetBlock(lavaPos, lavaLevel(0));
  side.rawSetBlock(waterPos, waterLevel(0));
  fluidSpread(side, lavaPos, side.getBlock(lavaPos),
              fluidStateOf(side.getBlock(lavaPos)));
  require(side.getBlock(waterPos).is(Blocks::WATER),
          "lava cannot replace water sideways, got " +
              std::string(side.getBlock(waterPos).shortName()));

  // Downward lava still replaces water: that is LavaFluid.spreadTo's stone rule,
  // covered by lavaFallsOntoWater. Directly assert the predicate's water-tag
  // half: a downward target holding water is replaceable.
  World down;
  flatFloor(down);
  const Pos above{0, 65, 0};
  const Pos target{0, 64, 0};
  down.rawSetBlock(above, lavaLevel(0));
  down.rawSetBlock(target, waterLevel(0));
  fluidSpread(down, above, down.getBlock(above),
              fluidStateOf(down.getBlock(above)));
  require(down.getBlock(target).is(Blocks::STONE),
          "a downward water-tagged target is replaceable (turns to stone)");
}

// M-01: lava's spread delay is multiplied by 4 when the surface rises without
// falling and the random roll is non-zero.
void lavaRandomDelay() {
  World world;
  flatFloor(world);
  const Pos pos{0, 64, 0};

  // Low level rising to a higher one, neither falling.
  const FluidState low = fluidStateOf(lavaLevel(6)); // amount 2
  const FluidState high = fluidStateOf(lavaLevel(2)); // amount 6
  require(low.getAmount() == 2 && high.getAmount() == 6, "levels decode");

  auto sawBase = 0;
  auto sawQuadrupled = 0;
  // Draw from one stream rather than from per-seed first draws: the exact random
  // sequence is not a compatibility requirement, only that both outcomes occur.
  world.setSeed(1234);
  for (int i = 0; i < 256; ++i) {
    const int delay = fluidSpreadDelay(world, pos, Fluids::lava(), low, high);
    require(delay == 30 || delay == 120,
            "the lava delay is either the base 30 or 4x it, got " +
                std::to_string(delay));
    if (delay == 30)
      ++sawBase;
    if (delay == 120)
      ++sawQuadrupled;
  }
  require(sawBase > 0, "some rolls keep the base delay");
  require(sawQuadrupled > 0, "some rolls quadruple the delay");

  // Falling states and a shrinking surface always keep the base delay.
  world.setSeed(0);
  const FluidState falling = fluidStateOf(lavaLevel(8));
  require(falling.isFalling(), "level 8 is falling");
  for (int seed = 0; seed < 16; ++seed) {
    world.setSeed(seed);
    require(fluidSpreadDelay(world, pos, Fluids::lava(), low, falling) == 30,
            "a falling new state keeps the base delay");
    require(fluidSpreadDelay(world, pos, Fluids::lava(), high, low) == 30,
            "a shrinking surface keeps the base delay");
  }

  // Water never takes the lava branch.
  const FluidState waterLow = fluidStateOf(waterLevel(6));
  const FluidState waterHigh = fluidStateOf(waterLevel(2));
  for (int seed = 0; seed < 8; ++seed) {
    world.setSeed(seed);
    require(fluidSpreadDelay(world, pos, Fluids::water(), waterLow, waterHigh) ==
                5,
            "water ignores the lava delay rule");
  }
}

// M-02: lava touching water turns into obsidian when it is a source and into
// cobblestone when it is flowing.
void lavaMeetsWater() {
  World world;
  flatFloor(world);
  const Pos lavaSource{0, 64, 0};
  world.rawSetBlock(lavaSource, lavaLevel(0));
  world.rawSetBlock({1, 64, 0}, waterLevel(0));
  require(!liquidShouldSpreadLiquid(world, lavaSource,
                                    world.getBlock(lavaSource)),
          "a lava source next to water reports that it converted");
  require(world.getBlock(lavaSource).is(Blocks::OBSIDIAN),
          "the lava source becomes obsidian");

  const Pos lavaFlow{0, 64, 4};
  world.rawSetBlock(lavaFlow, lavaLevel(3));
  world.rawSetBlock({1, 64, 4}, waterLevel(0));
  require(!liquidShouldSpreadLiquid(world, lavaFlow, world.getBlock(lavaFlow)),
          "flowing lava next to water reports that it converted");
  require(world.getBlock(lavaFlow).is(Blocks::COBBLESTONE),
          "flowing lava becomes cobblestone");

  // Lava away from water keeps spreading.
  const Pos dryLava{0, 64, 8};
  world.rawSetBlock(dryLava, lavaLevel(0));
  require(liquidShouldSpreadLiquid(world, dryLava, world.getBlock(dryLava)),
          "lava with no water neighbour keeps spreading");
  require(world.getBlock(dryLava).is(Blocks::LAVA), "the lava stays lava");
}

// M-03: lava over soul soil next to blue ice turns into basalt.
void lavaMeetsBlueIce() {
  World world;
  flatFloor(world);
  const Pos lavaPos{0, 64, 0};
  world.rawSetBlock(lavaPos, lavaLevel(0));
  world.rawSetBlock({0, 63, 0}, Blocks::SOUL_SOIL.defaultState());
  world.rawSetBlock({1, 64, 0}, Blocks::BLUE_ICE.defaultState());
  require(!liquidShouldSpreadLiquid(world, lavaPos, world.getBlock(lavaPos)),
          "lava over soul soil beside blue ice reports that it converted");
  require(world.getBlock(lavaPos).is(Blocks::BASALT),
          "the lava becomes basalt");

  // Without the soul soil underneath the same layout stays lava.
  World plain;
  flatFloor(plain);
  plain.rawSetBlock(lavaPos, lavaLevel(0));
  plain.rawSetBlock({1, 64, 0}, Blocks::BLUE_ICE.defaultState());
  require(liquidShouldSpreadLiquid(plain, lavaPos, plain.getBlock(lavaPos)),
          "blue ice alone does not convert the lava");
  require(plain.getBlock(lavaPos).is(Blocks::LAVA), "the lava stays lava");
}

// The basalt product carries RotatedPillarBlock's AXIS, defaulting to y, and the
// resulting state must stay inside the registered schema (and therefore inside
// the exported protocol table).
void basaltAxis() {
  World world;
  flatFloor(world);
  const Pos lavaPos{0, 64, 0};
  world.rawSetBlock(lavaPos, lavaLevel(0));
  world.rawSetBlock({0, 63, 0}, Blocks::SOUL_SOIL.defaultState());
  world.rawSetBlock({1, 64, 0}, Blocks::BLUE_ICE.defaultState());
  require(!liquidShouldSpreadLiquid(world, lavaPos, world.getBlock(lavaPos)),
          "the lava converted");
  const BlockState basalt = world.getBlock(lavaPos);
  require(basalt.is(Blocks::BASALT), "the product is basalt");
  require(basalt.get("axis") == "y", "basalt is written with axis=y");
  require(basalt.isSchemaState(), "basalt axis=y is a registered state");
  require(Blocks::BASALT.stateCount() == 3,
          "basalt declares the three axis states the export lists");
  require(Blocks::BASALT.defaultState().get("axis") == "y",
          "basalt defaults to axis=y");
}

// M-03 ordering: LiquidBlock's direction list is {DOWN, SOUTH, NORTH, EAST,
// WEST} visited through getOpposite(), i.e. up, north, south, west, east. With a
// north blue ice and a south water the north one wins, and swapping the two
// sides must flip the product.
void lavaReactionOrder() {
  const Pos pos{0, 64, 0};

  // North blue ice, south water -> basalt (north is visited first).
  World northIce;
  flatFloor(northIce);
  northIce.rawSetBlock(pos, lavaLevel(0));
  northIce.rawSetBlock({0, 63, 0}, Blocks::SOUL_SOIL.defaultState());
  northIce.rawSetBlock({0, 64, -1}, Blocks::BLUE_ICE.defaultState());
  northIce.rawSetBlock({0, 64, 1}, waterLevel(0));
  require(!liquidShouldSpreadLiquid(northIce, pos, northIce.getBlock(pos)),
          "the north blue ice case converted");
  require(northIce.getBlock(pos).is(Blocks::BASALT),
          "north blue ice wins over the south water");

  // North water, south blue ice -> obsidian (north is visited first).
  World northWater;
  flatFloor(northWater);
  northWater.rawSetBlock(pos, lavaLevel(0));
  northWater.rawSetBlock({0, 63, 0}, Blocks::SOUL_SOIL.defaultState());
  northWater.rawSetBlock({0, 64, -1}, waterLevel(0));
  northWater.rawSetBlock({0, 64, 1}, Blocks::BLUE_ICE.defaultState());
  require(!liquidShouldSpreadLiquid(northWater, pos, northWater.getBlock(pos)),
          "the north water case converted");
  require(northWater.getBlock(pos).is(Blocks::OBSIDIAN),
          "north water wins over the south blue ice");
}

// LavaFluid.spreadTo: lava falling onto water turns the water into stone instead
// of writing lava, for both a source and a flowing water target.
void lavaFallsOntoWater() {
  for (int level : {0, 3}) {
    World world;
    flatFloor(world);
    const Pos above{0, 65, 0};
    const Pos target{0, 64, 0};
    world.rawSetBlock(above, lavaLevel(0));
    world.rawSetBlock(target, waterLevel(level));
    fluidSpread(world, above, world.getBlock(above),
                fluidStateOf(world.getBlock(above)));
    require(world.getBlock(target).is(Blocks::STONE),
            "lava falling onto water leaves stone, got " +
                std::string(world.getBlock(target).shortName()));
  }

  // Lava falling onto air still writes lava.
  World air;
  flatFloor(air);
  const Pos above{0, 65, 0};
  const Pos target{0, 64, 0};
  air.rawSetBlock(above, lavaLevel(0));
  fluidSpread(air, above, air.getBlock(above),
              fluidStateOf(air.getBlock(above)));
  require(air.getBlock(target).is(Blocks::LAVA),
          "lava falling onto air still places lava");
}

// The spread delay must come out of the real tick path, with both values reached.
void spreadDelayThroughTick() {
  auto sawBase = 0;
  auto sawQuadrupled = 0;
  for (int seed = 0; seed < 128; ++seed) {
    World world;
    flatFloor(world);
    const Pos pos{0, 64, 0};
    // Lava at amount 2 next to a source: the surface rises to amount 6.
    world.rawSetBlock(pos, lavaLevel(6));
    world.rawSetBlock({1, 64, 0}, lavaLevel(0));
    world.setSeed(seed);
    // Warm the stream: the first draw after a small seed is heavily biased in
    // its high bits, so advance twice before the tick's own draw.
    (void)world.random.nextInt(16);
    (void)world.random.nextInt(16);
    fluidTick(world, pos, world.getBlock(pos),
              fluidStateOf(world.getBlock(pos)));
    // Only the entry at this position is the re-scheduled one; the spread may
    // also have queued ticks for the neighbours it wrote (F-5 wiring).
    auto delay = -1;
    for (const auto &entry : world.fluidTicks())
      if (entry.first.pos == pos)
        delay = static_cast<int>(entry.first.trigger - world.gameTime());
    require(delay == 30 || delay == 120,
            "the tick schedules 30 or 120, got " + std::to_string(delay));
    if (delay == 30)
      ++sawBase;
    if (delay == 120)
      ++sawQuadrupled;
  }
  require(sawBase > 0, "the tick path reaches the base delay");
  require(sawQuadrupled > 0, "the tick path reaches the quadrupled delay");
}

// A branch that takes no random draw must leave the level RNG untouched.
void nonRandomDelayKeepsRng() {
  World withCalls;
  flatFloor(withCalls);
  withCalls.setSeed(99);
  World without;
  flatFloor(without);
  without.setSeed(99);

  const Pos pos{0, 64, 0};
  const FluidState low = fluidStateOf(lavaLevel(6));
  const FluidState high = fluidStateOf(lavaLevel(2));
  const FluidState falling = fluidStateOf(lavaLevel(8));
  for (int i = 0; i < 8; ++i) {
    require(fluidSpreadDelay(withCalls, pos, Fluids::lava(), low, falling) == 30,
            "a falling new state keeps the base delay");
    require(fluidSpreadDelay(withCalls, pos, Fluids::lava(), high, low) == 30,
            "a shrinking surface keeps the base delay");
    require(fluidSpreadDelay(withCalls, pos, Fluids::water(), low, high) == 5,
            "water ignores the lava rule");
  }
  require(withCalls.random.nextInt(100000) == without.random.nextInt(100000),
          "branches without a random draw leave the level RNG untouched");
}
} // namespace

int main() {
  try {
    parameters();
    tags();
    sourceConversionSwitch();
    replacementRules();
    lavaRandomDelay();
    lavaMeetsWater();
    lavaMeetsBlueIce();
    basaltAxis();
    lavaReactionOrder();
    lavaFallsOntoWater();
    spreadDelayThroughTick();
    nonRandomDelayKeepsRng();
    std::cout << "fluid parameter tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid parameter test failure: " << error.what() << "\n";
    return 1;
  }
}
