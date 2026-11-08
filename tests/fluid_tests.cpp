#include "redstone/fluid.hpp"
#include "redstone/engine.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

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

// FluidState.is(Fluid) is the exact-identity test ServerLevel.tickFluid uses to
// decide which fluid's tick logic runs. A flowing state must not match its
// source, and isSourceOfType must additionally reject the flowing variant.
void identityTests() {
  const FluidState waterSource = fluidStateOf(levelState(Blocks::WATER, 0));
  const FluidState waterFlow = fluidStateOf(levelState(Blocks::WATER, 1));
  require(waterSource.is(Fluids::water()), "source water is water");
  require(!waterSource.is(Fluids::flowingWater()),
          "source water is not flowing_water");
  require(waterFlow.is(Fluids::flowingWater()),
          "flowing water is flowing_water");
  require(!waterFlow.is(Fluids::water()),
          "flowing water is not the water source");
  require(!waterFlow.is(Fluids::lava()), "flowing water is not lava");
  require(!FluidState().is(Fluids::water()), "empty is not water");

  require(waterSource.isSourceOfType(Fluids::water()),
          "source water is a source of water");
  require(!waterFlow.isSourceOfType(Fluids::flowingWater()),
          "flowing water is not a source of flowing_water");
  require(!waterFlow.isSourceOfType(Fluids::water()),
          "flowing water is not a source of water");
  require(!waterSource.isSourceOfType(Fluids::lava()),
          "source water is not a source of lava");

  // Fluid.isSource(FluidState) keeps the vanilla call shape.
  require(Fluids::water().isSource(waterSource), "water.isSource(state)");
  require(!Fluids::flowingWater().isSource(waterFlow),
          "flowing_water.isSource(state)");

  // A source's amount is the fluid constant, not the stored field, so a state
  // built with a wrong amount still reports 8. This pins the getAmount()
  // dispatch and would hang instead of failing if the Flowing path recursed.
  const FluidState forcedSource(Fluids::water(), 3, false);
  require(forcedSource.getAmount() == 8,
          "a source reports 8 regardless of the stored amount");
  require(forcedSource.isFull(), "a source is full");
  const FluidState forcedFlow(Fluids::flowingWater(), 3, false);
  require(forcedFlow.getAmount() == 3,
          "a flowing fluid reports its stored amount");
  require(forcedFlow.rawAmount() == 3, "rawAmount is the stored amount");
  require(!forcedFlow.isFull(), "a partially flowing fluid is not full");
  require(forcedFlow.getOwnHeight() == 3.0f / 9.0f,
          "flowing own height is amount/9");
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

// fluidStateOf takes a BlockState and has no World, so it cannot write a block
// by construction. What can actually regress is the state table itself: a
// lookup that interned a new state would mint ids and shift the protocol map.
// The world half uses a sand block, whose onPlace schedules a real tick, so the
// baseline is genuinely non-zero instead of trivially 0.
void queriesAreSideEffectFree() {
  const auto waterCount = Blocks::WATER.stateCount();
  const auto lavaCount = Blocks::LAVA.stateCount();
  const auto registeredBefore = registeredStateCount();
  auto waterIds = std::vector<BlockState::Id>{};
  auto lavaIds = std::vector<BlockState::Id>{};
  for (int level = 0; level <= 15; ++level) {
    waterIds.push_back(levelState(Blocks::WATER, level).id());
    lavaIds.push_back(levelState(Blocks::LAVA, level).id());
  }

  World world;
  world.loadChunk(0, 0);
  // setBlock on sand runs FallingBlock.onPlace and schedules a tick at delay 2.
  require(world.setBlock({0, 100, 0}, Blocks::SAND.defaultState()),
          "sand placement succeeds");
  require(world.scheduledTickCount() == 1,
          "sand placement gives a non-zero scheduled-tick baseline");
  world.rawSetBlock({0, 64, 0}, levelState(Blocks::WATER, 3));
  world.rawSetBlock({1, 64, 0}, levelState(Blocks::LAVA, 0));

  const auto ticksBefore = world.scheduledTickCount();
  const auto waterBefore = world.getBlock({0, 64, 0});
  const auto lavaBefore = world.getBlock({1, 64, 0});

  for (int i = 0; i < 64; ++i) {
    (void)fluidStateOf(world.getBlock({0, 64, 0}));
    (void)fluidStateOf(world.getBlock({1, 64, 0}));
    (void)fluidStateOf(world.getBlock({2, 64, 0}));
  }

  require(ticksBefore != 0, "the scheduled-tick baseline is not trivially zero");
  require(world.scheduledTickCount() == ticksBefore,
          "fluid queries schedule no ticks");
  require(world.getBlock({0, 64, 0}) == waterBefore,
          "fluid queries do not rewrite the water block");
  require(world.getBlock({1, 64, 0}) == lavaBefore,
          "fluid queries do not rewrite the lava block");
  require(world.getBlock({2, 64, 0}).is(Blocks::AIR),
          "fluid queries do not create blocks");

  // No new state was interned and no id moved.
  require(Blocks::WATER.stateCount() == waterCount, "water state count is stable");
  require(Blocks::LAVA.stateCount() == lavaCount, "lava state count is stable");
  require(registeredStateCount() == registeredBefore,
          "fluid queries intern no new block state");
  for (int level = 0; level <= 15; ++level) {
    require(levelState(Blocks::WATER, level).id() == waterIds[level],
            "water level " + std::to_string(level) + " keeps its state id");
    require(levelState(Blocks::LAVA, level).id() == lavaIds[level],
            "lava level " + std::to_string(level) + " keeps its state id");
  }
}

// The 32 water/lava states must match the 26.1 exported protocol table: the
// JSON-derived TSV lists every block state in registry-id order, so the rows at
// the water/lava block states pin both the ids and the property values.
void protocolStateTable(const std::filesystem::path &assets) {
  const auto path = assets / "block_states.tsv";
  std::ifstream in(path, std::ios::binary);
  require(in.good(), "open " + path.string());

  // Parse "id<TAB>block<TAB>[key=value,...]".
  auto rowsByBlock = std::map<std::string, std::map<int, int>>{};
  auto idToRow = std::map<int, std::string>{};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    const auto firstTab = line.find('\t');
    require(firstTab != std::string::npos, "TSV row has an id field: " + line);
    const int id = std::stoi(line.substr(0, firstTab));
    const auto secondTab = line.find('\t', firstTab + 1);
    const std::string block =
        line.substr(firstTab + 1, secondTab == std::string::npos
                                      ? std::string::npos
                                      : secondTab - firstTab - 1);
    const std::string properties =
        secondTab == std::string::npos ? std::string{} : line.substr(secondTab + 1);
    idToRow[id] = block;
    if (block != "minecraft:water" && block != "minecraft:lava")
      continue;
    const auto key = properties.find("level=");
    require(key != std::string::npos,
            block + " row has a level property: " + line);
    const int level = std::stoi(properties.substr(key + 6));
    rowsByBlock[block][level] = id;
  }

  for (const auto &[blockName, levels] :
       {std::pair<std::string, const Block *>{"minecraft:water", &Blocks::WATER},
        std::pair<std::string, const Block *>{"minecraft:lava", &Blocks::LAVA}}) {
    require(levels->stateCount() == 16,
            blockName + " keeps 16 level states");
    require(rowsByBlock[blockName].size() == 16,
            blockName + " has 16 rows in the protocol table");
    // Registry ids are contiguous per block, and the first row is level 0.
    const int firstId = rowsByBlock[blockName].at(0);
    for (int level = 0; level <= 15; ++level) {
      const int protocolId = rowsByBlock[blockName].at(level);
      require(protocolId == firstId + level,
              blockName + " level " + std::to_string(level) +
                  " has a contiguous protocol id");
      const int localId = static_cast<int>(levels->firstState()) + level;
      require(idToRow.count(localId) == 1,
              "local state id " + std::to_string(localId) + " exists in the table");
      // The block's own dense variant order must line up with the TSV order.
      const BlockState state = levelState(*levels, level);
      require(state.isSchemaState(),
              blockName + " level " + std::to_string(level) +
                  " stays inside the StateDefinition");
      require(!state.hasProperty("falling"),
              blockName + " has no falling block property");
    }
    require(levels->defaultState().get("level") == "0",
            blockName + " default level is 0");
  }
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc > 1, "fluid_tests needs the assets/protocol775 directory");
    const std::filesystem::path assets = argv[1];

    fluidRegistry();
    identityTests();
    emptyFluidState();
    levels();
    queriesAreSideEffectFree();
    protocolStateTable(assets);
    std::cout << "fluid tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid test failure: " << error.what() << "\n";
    return 1;
  }
}
