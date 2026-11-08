#include "redstone/fluid.hpp"
#include "redstone/engine.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
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

  // Lava covers the same three cases so the rule is not water-specific.
  const FluidState lavaSource = fluidStateOf(levelState(Blocks::LAVA, 0));
  const FluidState lavaFlow = fluidStateOf(levelState(Blocks::LAVA, 2));
  require(lavaSource.isSourceOfType(Fluids::lava()),
          "source lava is a source of lava");
  require(!lavaFlow.isSourceOfType(Fluids::flowingLava()),
          "flowing lava is not a source of flowing_lava");
  require(!lavaSource.isSourceOfType(Fluids::water()),
          "source lava is not a source of water");
  require(!FluidState().isSourceOfType(Fluids::water()),
          "empty is not a source of anything");

  // F-2 must dispatch with is(), never isSourceOfType(): a flowing state is not
  // a source, so isSourceOfType would skip every flowing fluid tick.
  require(waterFlow.is(Fluids::flowingWater()) &&
              !waterFlow.isSourceOfType(Fluids::flowingWater()),
          "identity and source-of differ for flowing water");

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
  require(Fluids::empty().getAmount(FluidState()) == 0, "empty amount is 0");

  // isSource() is only reachable through FluidState (Fluid.java:97 takes the
  // state), so the dispatch chain is exercised instead of the raw flag.
  require(!FluidState(Fluids::empty(), 0, false).isSource(),
          "empty fluid is not a source");
  require(FluidState(Fluids::water(), 8, false).isSource(), "water is a source");
  require(!FluidState(Fluids::flowingWater(), 8, false).isSource(),
          "flowing_water is not a source");
  require(FluidState(Fluids::lava(), 8, false).isSource(), "lava is a source");
  require(!FluidState(Fluids::flowingLava(), 8, false).isSource(),
          "flowing_lava is not a source");

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

// F-8: the five process-wide objects have one owner each, stable addresses, and
// remain reachable through both the public registry and the default FluidState
// fallback. The second thread fills its own thread-local decode cache, proving
// cache ownership is separate from the fluid singleton lifetime.
void singletonOwnershipAndLifetime() {
  const std::array<const Fluid *, 5> instances = {
      &Fluids::empty(), &Fluids::flowingWater(), &Fluids::water(),
      &Fluids::flowingLava(), &Fluids::lava()};
  const std::array<const char *, 5> names = {
      "minecraft:empty", "minecraft:flowing_water", "minecraft:water",
      "minecraft:flowing_lava", "minecraft:lava"};

  for (std::size_t i = 0; i < instances.size(); ++i) {
    require(instances[i]->name() == names[i],
            "the owned fluid registry keeps its name");
    require(findFluidByName(names[i]) == instances[i],
            "name lookup returns the owned singleton");
    for (std::size_t j = 0; j < i; ++j)
      require(instances[i] != instances[j],
              "fluid singleton identities are distinct");
  }

  require(&Fluids::empty() == &Fluids::empty(),
          "empty has one stable address");
  require(&Fluids::flowingWater() == &Fluids::flowingWater(),
          "flowing_water has one stable address");
  require(&Fluids::water() == &Fluids::water(),
          "water has one stable address");
  require(&Fluids::flowingLava() == &Fluids::flowingLava(),
          "flowing_lava has one stable address");
  require(&Fluids::lava() == &Fluids::lava(), "lava has one stable address");

  const FluidRef waterRef(Fluids::water());
  require(waterRef.fluid() == &Fluids::water() && !waterRef.empty(),
          "FluidRef keeps the registered observer identity");
  const FluidRef namedWater("minecraft:water");
  require(&*namedWater == &Fluids::water(),
          "FluidRef name resolution keeps the registered observer identity");

  // Fill this thread's state cache, then make the default fallback observable.
  (void)fluidStateOf(levelState(Blocks::WATER, 2));
  (void)fluidStateOf(levelState(Blocks::LAVA, 8));
  require(&FluidState().getType() == &Fluids::empty(),
          "default FluidState reaches the owned empty singleton");

  bool workerOk = false;
  std::string workerError;
  std::thread worker([&]() {
    try {
      const FluidState flowing = fluidStateOf(levelState(Blocks::WATER, 3));
      require(flowing.is(Fluids::flowingWater()),
              "worker cache resolves flowing_water");
      require(&flowing.getType() == &Fluids::flowingWater(),
              "worker cache uses the owned flowing_water singleton");
      require(&FluidState().getType() == &Fluids::empty(),
              "worker default reaches the owned empty singleton");
      require(findFluidByName("minecraft:lava") == &Fluids::lava(),
              "worker registry lookup is stable");
      workerOk = true;
    } catch (const std::exception &error) {
      workerError = error.what();
    }
  });
  worker.join();
  require(workerOk, workerError.empty()
                        ? std::string("worker failed without an error")
                        : workerError);
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

// BlockBehaviour.BlockStateBase caches its FluidState at state initialization.
// Exercise the id cache through schema states, a block registered after the
// static table, and imported extension states with an extra property.
void cachedFluidStateDecode() {
  for (int level = 0; level <= 15; ++level) {
    const BlockState water = levelState(Blocks::WATER, level);
    const BlockState lava = levelState(Blocks::LAVA, level);
    const FluidState waterFirst = fluidStateOf(water);
    const FluidState waterSecond = fluidStateOf(water);
    const FluidState lavaFirst = fluidStateOf(lava);
    const FluidState lavaSecond = fluidStateOf(lava);
    require(waterFirst == waterSecond, "water cache keeps the exact state");
    require(lavaFirst == lavaSecond, "lava cache keeps the exact state");
    require(waterFirst.is(Fluids::water()) ||
                waterFirst.is(Fluids::flowingWater()),
            "water cache keeps the water family");
    require(lavaFirst.is(Fluids::lava()) ||
                lavaFirst.is(Fluids::flowingLava()),
            "lava cache keeps the lava family");
  }

  // A block registered after startup is a fresh, higher state id. Its default
  // state is dry, and repeated lookup must not intern another state.
  const Block &late = blockByName("minecraft:future_cache_probe");
  const BlockState lateState = late.defaultState();
  const auto before = registeredStateCount();
  require(fluidStateOf(lateState).isEmpty(),
          "a post-registration dry block has the empty fluid");
  require(fluidStateOf(lateState).isEmpty(),
          "the post-registration dry result is cached");
  require(registeredStateCount() == before,
          "fluid lookup does not intern post-registration states");

  // An unknown imported property makes the whole state an extension. Baseline
  // decodeFluidState reads packed() through propertyValue, so an extension has
  // no LEVEL/WATERLOGGED field and uses the same defaults as before caching.
  const BlockState waterExtension(
      "minecraft:water", {{"level", "5"}, {"future_state", "kept"}});
  require(!waterExtension.isSchemaState(), "the water state is an extension");
  const FluidState extensionSource = fluidStateOf(waterExtension);
  require(extensionSource.is(Fluids::water()) && extensionSource.isSource(),
          "an extended water level keeps the baseline level-0 source fallback");
  require(fluidStateOf(waterExtension) == extensionSource,
          "the extended water fallback is stable on repeat");

  const BlockState fallingExtension(
      "minecraft:water", {{"level", "15"}, {"future_state", "kept"}});
  require(fluidStateOf(fallingExtension).is(Fluids::water()),
          "an extended level 15 also keeps the baseline source fallback");

  const BlockState lavaExtension(
      "minecraft:lava", {{"level", "2"}, {"future_state", "kept"}});
  const FluidState lava = fluidStateOf(lavaExtension);
  require(lava.is(Fluids::lava()) && lava.isSource(),
          "an extended lava level keeps the baseline source fallback");

  const BlockState malformedLevel(
      "minecraft:water", {{"level", "not-a-level"}, {"future_state", "kept"}});
  require(fluidStateOf(malformedLevel).is(Fluids::water()),
          "an invalid extended level still uses the packed-property fallback");

  const BlockState wetRailExtension(
      "minecraft:rail",
      {{"shape", "north_south"}, {"waterlogged", "true"},
       {"future_state", "kept"}});
  require(!wetRailExtension.isSchemaState(),
          "the waterlogged rail is an extension state");
  const FluidState wetRail = fluidStateOf(wetRailExtension);
  require(wetRail.isEmpty(),
          "an extended waterlogged property keeps the dry fallback");

  const BlockState unknownWet(
      "minecraft:future_unknown_wet_probe", {{"waterlogged", "true"}});
  require(!unknownWet.isSchemaState(), "the unknown block is an extension");
  require(fluidStateOf(unknownWet).isEmpty(),
          "an unknown block does not become a fluid from its property map");

  const BlockState schemaWet = Blocks::RAIL.defaultState().with(
      PropertyKind::Waterlogged, std::uint8_t{1});
  require(schemaWet.isSchemaState(), "the rail state uses its schema");
  require(fluidStateOf(schemaWet).is(Fluids::water()),
          "normal schema waterlogged hosts still report a water source");
  require(fluidStateOf(levelState(Blocks::WATER, 8)).isFalling(),
          "normal schema falling states are unchanged");
}

// The 32 water/lava states must match the 26.1 exported protocol table: the
// JSON-derived TSV lists every block state in registry-id order, so the rows at
// the water/lava block states pin both the ids and the property values.
void protocolStateTable(const std::filesystem::path &assets) {
  const auto path = assets / "block_states.tsv";
  std::ifstream in(path, std::ios::binary);
  require(in.good(), "open " + path.string());

  // Parse "id<TAB>block<TAB>key=value,...". Every field is kept: the point of
  // this test is to pin the exact exported (id, block, properties) rows, not
  // just to notice that some id exists.
  struct ProtocolRow {
    int id = 0;
    std::string block;
    std::string properties;
  };
  auto rows = std::vector<ProtocolRow>{};
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    const auto firstTab = line.find('\t');
    require(firstTab != std::string::npos, "TSV row has an id field: " + line);
    const auto secondTab = line.find('\t', firstTab + 1);
    ProtocolRow row;
    row.id = std::stoi(line.substr(0, firstTab));
    row.block = line.substr(firstTab + 1, secondTab == std::string::npos
                                               ? std::string::npos
                                               : secondTab - firstTab - 1);
    row.properties = secondTab == std::string::npos ? std::string{}
                                                    : line.substr(secondTab + 1);
    rows.push_back(std::move(row));
  }
  require(!rows.empty(), "the protocol table is not empty");

  // The exported table is ordered by protocol state id, so the row index must
  // equal the id. Asserting it also proves the file is the table we think.
  for (std::size_t i = 0; i < rows.size(); ++i)
    require(rows[i].id == static_cast<int>(i),
            "protocol row " + std::to_string(i) + " has id " +
                std::to_string(rows[i].id));

  // Select the 32 water/lava liquid states and require an exact match against
  // the properties this build produces, including that nothing else is present.
  const auto expectedProperties = [](int level) {
    return "level=" + std::to_string(level);
  };
  for (const auto &[blockName, block] :
       {std::pair<std::string, const Block *>{"minecraft:water", &Blocks::WATER},
        std::pair<std::string, const Block *>{"minecraft:lava", &Blocks::LAVA}}) {
    require(block->stateCount() == 16, blockName + " keeps 16 level states");

    auto matched = std::vector<const ProtocolRow *>{};
    for (const auto &row : rows)
      if (row.block == blockName)
        matched.push_back(&row);
    require(matched.size() == 16,
            blockName + " has exactly 16 rows in the protocol table");

    // Contiguity is asserted against the *table*, and the first id is recorded
    // so a swapped water/lava block is rejected by the level ordering below.
    const int firstId = matched.front()->id;
    for (int level = 0; level <= 15; ++level) {
      const ProtocolRow &row = *matched[static_cast<std::size_t>(level)];
      require(row.id == firstId + level,
              blockName + " level " + std::to_string(level) +
                  " is not the expected contiguous id " +
                  std::to_string(firstId + level));
      // Full property string, so an extra falling=true is rejected.
      require(row.properties == expectedProperties(level),
              blockName + " level " + std::to_string(level) +
                  " has properties '" + row.properties + "', expected '" +
                  expectedProperties(level) + "'");

      // The local state must exist and carry the same level.
      const BlockState state = levelState(*block, level);
      require(state.isSchemaState(),
              blockName + " level " + std::to_string(level) +
                  " stays inside the StateDefinition");
      require(state.get("level") == std::to_string(level),
              blockName + " level " + std::to_string(level) +
                  " round-trips its level property");
      require(!state.hasProperty("falling"),
              blockName + " has no falling block property");
    }
    require(block->defaultState().get("level") == "0",
            blockName + " default level is 0");
  }

  // Water and lava must not share ids, and water must precede lava.
  int waterFirst = -1, lavaFirst = -1;
  for (const auto &row : rows) {
    if (row.block == "minecraft:water" &&
        row.properties == expectedProperties(0))
      waterFirst = row.id;
    if (row.block == "minecraft:lava" && row.properties == expectedProperties(0))
      lavaFirst = row.id;
  }
  require(waterFirst >= 0 && lavaFirst >= 0, "both fluid blocks are exported");
  require(waterFirst + 16 == lavaFirst,
          "the water block states immediately precede the lava block states");
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc > 1, "fluid_tests needs the assets/protocol775 directory");
    const std::filesystem::path assets = argv[1];

    fluidRegistry();
    singletonOwnershipAndLifetime();
    identityTests();
    emptyFluidState();
    levels();
    queriesAreSideEffectFree();
    cachedFluidStateDecode();
    protocolStateTable(assets);
    std::cout << "fluid tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid test failure: " << error.what() << "\n";
    return 1;
  }
}
