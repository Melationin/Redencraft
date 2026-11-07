#include "redstone/fluid.hpp"
#include "redstone/engine.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

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

std::string describe(const BlockState &state) {
  return state.shortName().empty() ? std::string("?") : std::string(state.shortName());
}

BlockState levelState(const Block &block, int level) {
  return block.defaultState().with(PropertyKind::Level,
                                   static_cast<std::uint8_t>(level));
}

// Every (fluid, level) pair from LiquidBlock.stateCache: level 0 is the source,
// 1..7 are flowing with amount 8-level, 8..15 all clamp to the falling full
// state while the block keeps its own level property.
struct Expectation {
  const Fluid *type;
  int amount;
  bool source;
  bool falling;
};

Expectation waterExpectation(int level) {
  if (level == 0)
    return {&Fluids::water(), 8, true, false};
  if (level < 8)
    return {&Fluids::flowingWater(), 8 - level, false, false};
  return {&Fluids::flowingWater(), 8, false, true};
}

Expectation lavaExpectation(int level) {
  if (level == 0)
    return {&Fluids::lava(), 8, true, false};
  if (level < 8)
    return {&Fluids::flowingLava(), 8 - level, false, false};
  return {&Fluids::flowingLava(), 8, false, true};
}

void checkLevels(const Block &block, bool water, const char *label) {
  for (int level = 0; level <= 15; ++level) {
    const BlockState state = levelState(block, level);
    // The block state must keep the imported level; getFluidState clamps only
    // its own cache index.
    require(state.get("level") == std::to_string(level),
            std::string(label) + " state keeps level " + std::to_string(level));

    const FluidState fluid = fluidStateOf(state);
    const Expectation expected =
        water ? waterExpectation(level) : lavaExpectation(level);

    require(fluid.getType().is(*expected.type),
            std::string(label) + " level " + std::to_string(level) +
                " type is " + std::string(expected.type->shortName()));
    require(fluid.getAmount() == expected.amount,
            std::string(label) + " level " + std::to_string(level) + " amount");
    require(fluid.isSource() == expected.source,
            std::string(label) + " level " + std::to_string(level) + " isSource");
    require(fluid.isFalling() == expected.falling,
            std::string(label) + " level " + std::to_string(level) + " falling");
    require(fluid.isEmpty() == false,
            std::string(label) + " level " + std::to_string(level) + " not empty");
    require(fluid.isFull() == (expected.amount == 8),
            std::string(label) + " level " + std::to_string(level) + " isFull");
    require(fluid.getOwnHeight() == static_cast<float>(expected.amount) / 9.0f,
            std::string(label) + " level " + std::to_string(level) + " ownHeight");

    // Family test: both variants of a fluid agree with each other and disagree
    // with the other family.
    const Fluid &otherFamily = water ? static_cast<const Fluid &>(Fluids::lava())
                                     : static_cast<const Fluid &>(Fluids::water());
    require(fluid.getType().isSame(*expected.type),
            std::string(label) + " level " + std::to_string(level) + " same family");
    require(!fluid.getType().isSame(otherFamily),
            std::string(label) + " level " + std::to_string(level) + " other family");

    // createLegacyBlock writes the legacy level back: a source writes 0 and a
    // falling fluid writes 8, so imported levels above the 8-entry cache
    // normalise instead of round-tripping exactly.
    const BlockState legacy = fluid.getType().createLegacyBlock(fluid);
    require(legacy.is(block),
            std::string(label) + " level " + std::to_string(level) + " legacy block");
    const int expectedLegacy = level == 0 ? 0 : (level <= 8 ? level : 8);
    require(legacy.get("level") == std::to_string(expectedLegacy),
            std::string(label) + " level " + std::to_string(level) +
                " legacy level " + legacy.get("level") + " != " +
                std::to_string(expectedLegacy));
  }
}

void fluidRegistry() {
  require(Fluids::empty().isEmpty(), "empty fluid is empty");
  require(!Fluids::empty().isSource(), "empty fluid is not a source");
  require(Fluids::empty().getAmount(FluidState()) == 0, "empty amount is 0");

  require(Fluids::water().isSource(), "water is a source");
  require(!Fluids::flowingWater().isSource(), "flowing_water is not a source");
  require(Fluids::lava().isSource(), "lava is a source");
  require(!Fluids::flowingLava().isSource(), "flowing_lava is not a source");

  require(Fluids::water().isSame(Fluids::flowingWater()),
          "water family matches flowing_water");
  require(Fluids::flowingWater().isSame(Fluids::water()),
          "flowing_water family matches water");
  require(!Fluids::water().isSame(Fluids::lava()), "water does not match lava");
  require(!Fluids::lava().isSame(Fluids::flowingWater()),
          "lava does not match flowing_water");

  require(Fluids::empty().bucketItem().empty(), "empty fluid has no bucket");
  require(Fluids::water().bucketItem() == "minecraft:water_bucket",
          "water bucket item");
  require(Fluids::lava().bucketItem() == "minecraft:lava_bucket",
          "lava bucket item");

  require(findFluidByName("minecraft:water") == &Fluids::water(),
          "findFluidByName water");
  require(findFluidByName("water") == &Fluids::water(),
          "findFluidByName accepts a bare name");
  require(findFluidByName("minecraft:flowing_lava") == &Fluids::flowingLava(),
          "findFluidByName flowing_lava");
  require(findFluidByName("minecraft:stone") == nullptr,
          "findFluidByName misses an unknown name");
}

void emptyFluidState() {
  const FluidState none;
  require(none.isEmpty(), "default fluid state is empty");
  require(none.getAmount() == 0, "default fluid state amount is 0");
  require(none.getOwnHeight() == 0.0f, "empty own height is 0");
  require(!none.isSource(), "empty is not a source");
  require(!none.isFull(), "empty is not full");
  require(none == FluidState(), "default fluid states compare equal");

  // Non-fluid blocks report the empty fluid, including air.
  for (const char *name : {"minecraft:air", "minecraft:stone",
                           "minecraft:smooth_stone", "minecraft:glass"}) {
    const FluidState fluid = fluidStateOf(defaultState(name));
    require(fluid.isEmpty(), std::string(name) + " has no fluid");
    require(fluid.getAmount() == 0, std::string(name) + " fluid amount is 0");
  }
}

void levels() {
  checkLevels(Blocks::WATER, true, "water");
  checkLevels(Blocks::LAVA, false, "lava");

  // Levels above the 8-entry cache must decode identically without rewriting
  // the block property.
  for (int level = 8; level <= 15; ++level) {
    const BlockState water = levelState(Blocks::WATER, level);
    require(water.get("level") == std::to_string(level),
            "imported water keeps level " + std::to_string(level));
    const FluidState fluid = fluidStateOf(water);
    require(fluid.isFalling() && fluid.getAmount() == 8,
            "imported water level " + std::to_string(level) + " is falling full");
    require(fluid.getLegacyLevel() == 8,
            "legacy level normalises to 8 for imported level " +
                std::to_string(level));
  }
}

void queriesAreSideEffectFree() {
  World world;
  world.loadChunk(0, 0);
  const BlockState water = levelState(Blocks::WATER, 3);
  world.rawSetBlock({0, 64, 0}, water);
  world.rawSetBlock({1, 64, 0}, levelState(Blocks::LAVA, 0));

  const auto ticksBefore = world.scheduledTickCount();
  const auto waterBefore = world.getBlock({0, 64, 0});
  const auto lavaBefore = world.getBlock({1, 64, 0});

  for (int i = 0; i < 64; ++i) {
    (void)fluidStateOf(world.getBlock({0, 64, 0}));
    (void)fluidStateOf(world.getBlock({1, 64, 0}));
    (void)fluidStateOf(world.getBlock({2, 64, 0}));
  }

  require(world.scheduledTickCount() == ticksBefore,
          "fluid queries schedule no ticks");
  require(world.getBlock({0, 64, 0}) == waterBefore,
          "fluid queries do not rewrite the water block");
  require(world.getBlock({1, 64, 0}) == lavaBefore,
          "fluid queries do not rewrite the lava block");
  require(world.getBlock({2, 64, 0}).is(Blocks::AIR),
          "fluid queries do not create blocks");
}

// The 32 water/lava states must stay inside the 26.1 exported protocol table.
void protocolStateTable() {
  require(Blocks::WATER.stateCount() == 16, "water keeps 16 level states");
  require(Blocks::LAVA.stateCount() == 16, "lava keeps 16 level states");
  require(Blocks::WATER.defaultState().get("level") == "0",
          "water default level is 0");
  require(Blocks::LAVA.defaultState().get("level") == "0",
          "lava default level is 0");
  for (const Block *block : {&Blocks::WATER, &Blocks::LAVA}) {
    require(!block->defaultState().hasProperty("falling"),
            std::string(block->shortName()) + " has no falling block property");
    for (int level = 0; level <= 15; ++level) {
      const BlockState state = levelState(*block, level);
      require(state.isSchemaState(),
              std::string(block->shortName()) + " level " + std::to_string(level) +
                  " stays inside the StateDefinition");
    }
  }
}
} // namespace

int main() {
  try {
    fluidRegistry();
    emptyFluidState();
    levels();
    queriesAreSideEffectFree();
    protocolStateTable();
    std::cout << "fluid tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid test failure: " << error.what() << "\n";
    return 1;
  }
}
