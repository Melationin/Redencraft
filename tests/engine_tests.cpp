#include "redstone/engine.hpp"
#include "redstone/block_tags.hpp"
#include "redstone/entity.hpp"
#include "redstone/random_tick.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
using namespace redstone;
namespace {
struct GrassTickFixture : RandomTickAccess {
  std::map<Pos, BlockState> blocks;
  std::vector<std::pair<Pos, BlockState>> writes;
  std::optional<bool> full = false, water = false, snowy = false;
  std::optional<bool> mushroomSurvival;
  std::optional<int> brightness = 9, blocked = 0;
  BlockState getBlock(Pos pos) const override {
    auto it = blocks.find(pos);
    return it == blocks.end() ? airState() : it->second;
  }
  std::optional<bool> isFullFluid(Pos) const override { return full; }
  std::optional<bool> isWaterFluid(Pos) const override { return water; }
  std::optional<int> lightBlockIntoUp(BlockState, Pos) const override { return blocked; }
  std::optional<int> maxLocalRawBrightness(Pos) const override { return brightness; }
  std::optional<bool> isSnowySetting(Pos) const override { return snowy; }
  void setBlockAndUpdate(Pos pos, BlockState state) override {
    writes.emplace_back(pos, state);
    blocks[pos] = state;
  }
  void setBlock(Pos pos, BlockState state, int flags) override {
    assert(flags == 260 || flags == 2);
    setBlockAndUpdate(pos, state);
  }
  std::optional<bool> growSaplingTree(Pos, BlockState, Random &) override {
    return std::nullopt;
  }
  std::optional<bool> canMushroomSurvive(Pos, BlockState) const override {
    return mushroomSurvival;
  }
};
void mushroomRandomTick() {
  std::int64_t seed = 0;
  while (Random(seed).nextInt(25) != 0) ++seed;
  for (const Block *mushroom : {&Blocks::BROWN_MUSHROOM, &Blocks::RED_MUSHROOM}) {
    const auto state = mushroom->defaultState();
    assert(state.isRandomlyTicking() == true);
    GrassTickFixture fixture;
    fixture.blocks[{0, 0, 0}] = state;
    fixture.mushroomSurvival = true;
    Random actual(seed), expected(seed);
    assert(expected.nextInt(25) == 0);
    const auto offset = [&](Pos base) {
      const int x = expected.nextInt(3) - 1;
      const int y1 = expected.nextInt(2);
      const int y2 = expected.nextInt(2);
      const int z = expected.nextInt(3) - 1;
      return Pos{base.x + x, base.y + y1 - y2, base.z + z};
    };
    Pos base{0, 0, 0};
    auto candidate = offset(base);
    for (int i = 0; i < 4; ++i) {
      if (candidate != Pos{0, 0, 0}) base = candidate;
      candidate = offset(base);
    }
    assert(state.randomTick(fixture, {0, 0, 0}, actual) == RandomTickResult::Applied);
    assert(fixture.writes.size() == (candidate == Pos{0, 0, 0} ? 0u : 1u));
    if (!fixture.writes.empty()) assert(fixture.writes[0].first == candidate);
    assert(actual.nextInt() == expected.nextInt());
    GrassTickFixture crowded;
    for (int x = 0; x < 5; ++x) crowded.blocks[{x, 0, 0}] = state;
    Random crowdedRandom(seed);
    assert(state.randomTick(crowded, {0, 0, 0}, crowdedRandom) == RandomTickResult::Applied);
    assert(crowded.writes.empty());
    GrassTickFixture unavailable;
    unavailable.blocks[{0, 0, 0}] = state;
    Random unavailableRandom(seed);
    assert(state.randomTick(unavailable, {0, 0, 0}, unavailableRandom) == RandomTickResult::Unsupported);
  }
}
void grassRandomTick() {
  const auto grass = Blocks::GRASS_BLOCK.defaultState();
  assert(Blocks::AIR.defaultState().isRandomlyTicking() == false);
  assert(Blocks::GLASS.defaultState().isRandomlyTicking() == false);
  assert(Blocks::COBWEB.defaultState().isRandomlyTicking() == false);
  assert(collisionShape(Blocks::COBWEB.defaultState()).empty());
  assert(blocksMotion(Blocks::COBWEB.defaultState()));
  assert(!Blocks::COBWEB.supported());
  for (const auto &[block, sound] : std::initializer_list<std::pair<const Block *, const char *>>{
       {&Blocks::GOLD_BLOCK, "bell"}, {&Blocks::IRON_BLOCK, "iron_xylophone"},
       {&Blocks::BRICKS, "basedrum"}, {&Blocks::BOOKSHELF, "bass"}}) {
    assert(!block->supported() && block->stateCount() == 1);
    assert(block->defaultState().isRandomlyTicking() == false);
    World w;
    w.loadChunk(0, 0);
    w.rawSetBlock({0, 0, 0}, block->defaultState());
    assert(w.updateFromNeighbourShapes({0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == sound);
  }
  for (const Block *flower : {&Blocks::DANDELION, &Blocks::GOLDEN_DANDELION,
       &Blocks::TORCHFLOWER, &Blocks::POPPY, &Blocks::BLUE_ORCHID, &Blocks::ALLIUM,
       &Blocks::AZURE_BLUET, &Blocks::RED_TULIP, &Blocks::ORANGE_TULIP,
       &Blocks::WHITE_TULIP, &Blocks::PINK_TULIP, &Blocks::OXEYE_DAISY,
       &Blocks::CORNFLOWER, &Blocks::WITHER_ROSE, &Blocks::LILY_OF_THE_VALLEY}) {
    assert(!flower->supported() && flower->stateCount() == 1);
    assert(collisionShape(flower->defaultState()).empty());
    assert(!isReplaceable(flower->defaultState()));
    assert(flower->defaultState().isRandomlyTicking() == false);
  }
  for (const Block *wool : {&Blocks::WHITE_WOOL, &Blocks::ORANGE_WOOL,
       &Blocks::MAGENTA_WOOL, &Blocks::LIGHT_BLUE_WOOL, &Blocks::YELLOW_WOOL,
       &Blocks::LIME_WOOL, &Blocks::PINK_WOOL, &Blocks::GRAY_WOOL,
       &Blocks::LIGHT_GRAY_WOOL, &Blocks::CYAN_WOOL, &Blocks::PURPLE_WOOL,
       &Blocks::BLUE_WOOL, &Blocks::BROWN_WOOL, &Blocks::GREEN_WOOL,
       &Blocks::RED_WOOL, &Blocks::BLACK_WOOL}) {
    assert(wool->supported() && wool->stateCount() == 1);
    assert(isWoolBlock(wool->defaultState()));
    assert(wool->defaultState().isRandomlyTicking() == false);
    World w;
    w.loadChunk(0, 0);
    w.rawSetBlock({0, 0, 0}, wool->defaultState());
    assert(w.updateFromNeighbourShapes({0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == "guitar");
  }
  for (const Block *plant : {&Blocks::SHORT_GRASS, &Blocks::FERN,
                             &Blocks::DEAD_BUSH, &Blocks::BUSH,
                             &Blocks::SHORT_DRY_GRASS, &Blocks::TALL_DRY_GRASS}) {
    assert(collisionShape(plant->defaultState()).empty());
    assert(plant->defaultState().isRandomlyTicking() == false);
    assert(isReplaceable(plant->defaultState()));
    assert(!plant->supported());
  }
  const auto dispenser = Blocks::DISPENSER.defaultState();
  assert(Blocks::DISPENSER.supported() && Blocks::DISPENSER.stateCount() == 12);
  assert(dispenser.getView("facing") == "north" && dispenser.getView("triggered") == "false");
  assert(dispenser.with("facing", "up").with("triggered", "true").getView("triggered") == "true");
  assert(dispenser.isRandomlyTicking() == false);
  for (const Block *bed : {
           &Blocks::WHITE_BED,
           &Blocks::ORANGE_BED,
           &Blocks::MAGENTA_BED,
           &Blocks::LIGHT_BLUE_BED,
           &Blocks::YELLOW_BED,
           &Blocks::LIME_BED,
           &Blocks::PINK_BED,
           &Blocks::GRAY_BED,
           &Blocks::LIGHT_GRAY_BED,
           &Blocks::CYAN_BED,
           &Blocks::PURPLE_BED,
           &Blocks::BLUE_BED,
           &Blocks::BROWN_BED,
           &Blocks::GREEN_BED,
           &Blocks::RED_BED,
           &Blocks::BLACK_BED}) {
    assert(!bed->supported() && bed->stateCount() == 16);
    const auto state = bed->defaultState();
    assert(state.getView("part") == "foot" && state.getView("facing") == "north");
    assert(state.getView("occupied") == "false");
    assert(state.with("part", "head").with("occupied", "true").getView("part") == "head");
  }
  for (const Block *block : {&Blocks::SANDSTONE, &Blocks::CHISELED_SANDSTONE,
                              &Blocks::CUT_SANDSTONE}) {
    assert(block->stateCount() == 1 && block->metadata().hardness == 0.8f);
    assert(block->metadata().explosionResistance == 0.8f);
    assert(block->defaultState().isRandomlyTicking() == false);
    assert(lightPropertiesOf(block->defaultState()).supported);
  }
  assert(Blocks::STONE.defaultState().isRandomlyTicking() == false);
  assert(Blocks::COBBLESTONE.defaultState().isRandomlyTicking() == false);
  assert(Blocks::BEDROCK.defaultState().isRandomlyTicking() == false);
  assert(Blocks::SAND.defaultState().isRandomlyTicking() == false);
  for (const auto state : Blocks::MANGROVE_ROOTS.states())
    assert(state.isRandomlyTicking() == false);
  assert(Blocks::RED_SAND.defaultState().isRandomlyTicking() == false);
  assert(Blocks::GRAVEL.defaultState().isRandomlyTicking() == false);
  for (const Block *block : {&Blocks::LAPIS_ORE, &Blocks::DEEPSLATE_LAPIS_ORE,
                              &Blocks::LAPIS_BLOCK}) {
    assert(block->stateCount() == 1 && block->defaultState().isRandomlyTicking() == false);
    assert(block->metadata().explosionResistance == 3.0f);
    assert(lightPropertiesOf(block->defaultState()).supported);
  }
  for (const Block *ore : {&Blocks::GOLD_ORE, &Blocks::DEEPSLATE_GOLD_ORE,
                           &Blocks::IRON_ORE, &Blocks::DEEPSLATE_IRON_ORE,
                           &Blocks::COAL_ORE, &Blocks::DEEPSLATE_COAL_ORE,
                           &Blocks::NETHER_GOLD_ORE}) {
    assert(ore->stateCount() == 1 && ore->defaultState().isRandomlyTicking() == false);
    assert(ore->metadata().explosionResistance == 3.0f);
    assert(blocksMotion(ore->defaultState()) && lightPropertiesOf(ore->defaultState()).supported);
  }
  for (const auto state : Blocks::WATER.states())
    assert(state.isRandomlyTicking() == false);
  for (const auto state : Blocks::LAVA.states())
    assert(state.isRandomlyTicking() == true);
  assert(Blocks::PODZOL.defaultState().isRandomlyTicking() == false);
  const auto oakLeaves = Blocks::OAK_LEAVES.defaultState();
  assert(Blocks::OAK_LEAVES.stateCount() == 28);
  assert(oakLeaves.getView("distance") == "7" && oakLeaves.getView("persistent") == "false");
  assert(oakLeaves.getView("waterlogged") == "false" && oakLeaves.isRandomlyTicking() == true);
  assert(oakLeaves.with("persistent", "true").isRandomlyTicking() == false);
  assert(oakLeaves.with("distance", "6").isRandomlyTicking() == false);
  for (const Block *block : {
           &Blocks::SPRUCE_LEAVES,
           &Blocks::BIRCH_LEAVES,
           &Blocks::JUNGLE_LEAVES,
           &Blocks::ACACIA_LEAVES,
           &Blocks::CHERRY_LEAVES,
           &Blocks::DARK_OAK_LEAVES,
           &Blocks::PALE_OAK_LEAVES,
           &Blocks::MANGROVE_LEAVES,
           &Blocks::AZALEA_LEAVES,
           &Blocks::FLOWERING_AZALEA_LEAVES}) {
    const auto state = block->defaultState();
    assert(block->stateCount() == 28 && state.getView("distance") == "7");
    assert(state.isRandomlyTicking() == true);
    assert(state.with("persistent", "true").isRandomlyTicking() == false);
    assert(state.with("distance", "6").isRandomlyTicking() == false);
  }
  assert(Blocks::OAK_SAPLING.defaultState().isRandomlyTicking() == true);
  assert(grass.isRandomlyTicking() == true);
  assert(Blocks::DIRT.defaultState().isRandomlyTicking() == false);
  GrassTickFixture fixture;
  Random rng(17);
  const Pos origin{0, 10, 0};
  assert(oakLeaves.randomTick(fixture, origin, rng) == RandomTickResult::Unsupported);
  assert(Blocks::LAVA.defaultState().randomTick(fixture, origin, rng) ==
         RandomTickResult::Applied);
  fixture.full = true;
  assert(grass.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.size() == 1 && fixture.writes[0].first == origin);
  assert(fixture.writes[0].second.is(Blocks::DIRT));
  fixture.writes.clear();
  fixture.full = std::nullopt;
  assert(grass.randomTick(fixture, origin, rng) == RandomTickResult::Unsupported);
  assert(fixture.writes.empty());
  fixture.full = false;
  fixture.blocked = 15;
  assert(grass.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.size() == 1 && fixture.writes[0].second.is(Blocks::DIRT));
  fixture.writes.clear();
  fixture.blocked = 0;
  fixture.brightness = 8;
  assert(grass.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.empty());
  fixture.brightness = 9;
  fixture.snowy = true;
  fixture.blocks.clear();
  Random expected(123);
  std::vector<Pos> targets;
  for (int i = 0; i < 4; ++i) {
    const int dx = expected.nextInt(3) - 1;
    const int dy = expected.nextInt(5) - 3;
    const int dz = expected.nextInt(3) - 1;
    targets.push_back({origin.x + dx, origin.y + dy, origin.z + dz});
    fixture.blocks[targets.back()] = Blocks::DIRT.defaultState();
  }
  fixture.water = true;
  rng.setSeed(123);
  assert(grass.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.empty());
  fixture.water = false;
  rng.setSeed(123);
  assert(grass.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  std::vector<Pos> uniqueTargets;
  for (const auto target : targets)
    if (std::find(uniqueTargets.begin(), uniqueTargets.end(), target) == uniqueTargets.end())
      uniqueTargets.push_back(target);
  assert(fixture.writes.size() == uniqueTargets.size());
  for (std::size_t i = 0; i < uniqueTargets.size(); ++i) {
    assert(fixture.writes[i].first == uniqueTargets[i]);
    assert(fixture.writes[i].second.is(Blocks::GRASS_BLOCK));
    assert(fixture.writes[i].second.getView("snowy") == "true");
  }
  assert(rng.nextInt() == expected.nextInt());
  const auto sapling = Blocks::OAK_SAPLING.defaultState();
  assert(sapling.getView("stage") == "0");
  assert(collisionShape(sapling).empty());
  const auto outline = outlineShape(sapling);
  assert(outline.size() == 1 && outline[0].x0 == 2 && outline[0].y1 == 12 && outline[0].x1 == 14);
  fixture.blocks.clear();
  fixture.writes.clear();
  fixture.brightness = 8;
  rng.setSeed(0);
  assert(sapling.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.empty());
  fixture.brightness = 9;
  int saplingSeed = 0;
  while (Random(saplingSeed).nextInt(7) != 0) ++saplingSeed;
  rng.setSeed(saplingSeed);
  assert(sapling.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.size() == 1 && fixture.writes[0].first == origin);
  assert(fixture.writes[0].second.getView("stage") == "1");
  for (const Block *block : {&Blocks::CHERRY_SAPLING, &Blocks::DARK_OAK_SAPLING,
                              &Blocks::PALE_OAK_SAPLING}) {
    fixture.writes.clear();
    const auto state = block->defaultState();
    assert(state.isRandomlyTicking() == true && state.getView("stage") == "0");
    assert(collisionShape(state).empty());
    rng.setSeed(saplingSeed);
    assert(state.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
    assert(fixture.writes.size() == 1 && fixture.writes[0].second.is(*block));
    assert(fixture.writes[0].second.getView("stage") == "1");
  }
  fixture.writes.clear();
  const auto jungleSapling = Blocks::JUNGLE_SAPLING.defaultState();
  assert(jungleSapling.isRandomlyTicking() == true);
  assert(jungleSapling.getView("stage") == "0" && collisionShape(jungleSapling).empty());
  rng.setSeed(saplingSeed);
  assert(jungleSapling.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.size() == 1 && fixture.writes[0].second.is(Blocks::JUNGLE_SAPLING));
  assert(fixture.writes[0].second.getView("stage") == "1");
  fixture.writes.clear();
  const auto acaciaSapling = Blocks::ACACIA_SAPLING.defaultState();
  assert(acaciaSapling.isRandomlyTicking() == true);
  assert(acaciaSapling.getView("stage") == "0" && collisionShape(acaciaSapling).empty());
  rng.setSeed(saplingSeed);
  assert(acaciaSapling.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.size() == 1 && fixture.writes[0].second.is(Blocks::ACACIA_SAPLING));
  assert(fixture.writes[0].second.getView("stage") == "1");
  fixture.writes.clear();
  const auto birchSapling = Blocks::BIRCH_SAPLING.defaultState();
  assert(birchSapling.isRandomlyTicking() == true);
  assert(birchSapling.getView("stage") == "0" && collisionShape(birchSapling).empty());
  rng.setSeed(saplingSeed);
  assert(birchSapling.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.size() == 1 && fixture.writes[0].second.is(Blocks::BIRCH_SAPLING));
  assert(fixture.writes[0].second.getView("stage") == "1");
  fixture.writes.clear();
  rng.setSeed(saplingSeed);
  assert(sapling.with("stage", "1").randomTick(fixture, origin, rng) ==
         RandomTickResult::Unsupported);
  assert(fixture.writes.empty());
  const auto spruceSapling = Blocks::SPRUCE_SAPLING.defaultState();
  assert(spruceSapling.isRandomlyTicking() == true);
  assert(spruceSapling.getView("stage") == "0" && collisionShape(spruceSapling).empty());
  rng.setSeed(saplingSeed);
  assert(spruceSapling.randomTick(fixture, origin, rng) == RandomTickResult::Applied);
  assert(fixture.writes.size() == 1 && fixture.writes[0].second.is(Blocks::SPRUCE_SAPLING));
  assert(fixture.writes[0].second.getView("stage") == "1");
  assert(Blocks::OAK_PLANKS.defaultState().isRandomlyTicking() == false);
  for (const auto state : Blocks::OAK_LOG.states())
    assert(state.isRandomlyTicking() == false);
  for (const auto state : Blocks::STRIPPED_OAK_LOG.states())
    assert(state.isRandomlyTicking() == false);
  for (const Block *wood : {&Blocks::SPRUCE_PLANKS, &Blocks::SPRUCE_LOG,
                            &Blocks::STRIPPED_SPRUCE_LOG, &Blocks::BIRCH_PLANKS,
                            &Blocks::BIRCH_LOG, &Blocks::STRIPPED_BIRCH_LOG,
                            &Blocks::JUNGLE_PLANKS, &Blocks::JUNGLE_LOG,
                            &Blocks::STRIPPED_JUNGLE_LOG, &Blocks::ACACIA_LOG,
                            &Blocks::CHERRY_LOG, &Blocks::DARK_OAK_LOG,
                            &Blocks::PALE_OAK_LOG, &Blocks::MANGROVE_LOG,
                            &Blocks::ACACIA_PLANKS,
                            &Blocks::CHERRY_PLANKS, &Blocks::DARK_OAK_PLANKS,
                            &Blocks::PALE_OAK_WOOD, &Blocks::PALE_OAK_PLANKS,
                            &Blocks::MANGROVE_PLANKS, &Blocks::BAMBOO_PLANKS,
                            &Blocks::BAMBOO_MOSAIC})
    for (const auto state : wood->states())
      assert(state.isRandomlyTicking() == false);
}
} // namespace
void sectionReadCacheLifecycle() {
  World w;
  const Pos p{-17, -1, -1};
  const auto stone = Blocks::SMOOTH_STONE.defaultState();
  const auto glass = Blocks::GLASS.defaultState();
  assert(w.getBlock(p).isAir()); // Missing chunk must not mask a later load.
  w.loadChunk(-2, -1);
  assert(w.getBlock(p).isAir()); // Cache the absent section.
  w.rawSetBlock(p, stone);
  assert(w.getBlock(p) == stone); // Section creation invalidates that miss.
  w.rawSetBlock(p, glass);
  assert(w.getBlock(p) == glass); // Cached pointer reads current palette data.
  w.rawSetBlock(p, airState());
  assert(w.getBlock(p).isAir());
  w.rawSetBlock(p, stone);
  w.unloadChunk(-2, -1);
  assert(w.getBlock(p).isAir());
  w.loadChunk(-2, -1);
  assert(w.getBlock(p) == stone);
  const Pos above{p.x, 16, p.z};
  w.rawSetBlock(above, glass);
  assert(w.getBlock(above) == glass && w.getBlock(p) == stone);
  assert(w.getBlock({p.x, w.minY() - 1, p.z}).isAir());
  assert(w.getBlock({p.x, w.maxY(), p.z}).isAir());
  World moved(std::move(w));
  assert(moved.getBlock(p) == stone && w.getBlock(p).isAir());
  World assigned;
  assigned.loadChunk(-2, -1);
  assigned.rawSetBlock(p, glass);
  assert(assigned.getBlock(p) == glass);
  assigned = std::move(moved);
  assert(assigned.getBlock(p) == stone && moved.getBlock(p).isAir());
  for (int x : {std::numeric_limits<int>::min(), -17, -16, -1, 0, 15, 16,
                std::numeric_limits<int>::max()}) {
    assigned.loadChunk(x >> 4, 0);
    assigned.rawSetBlock({x, 0, 0}, glass);
    assert(assigned.getBlock({x, 0, 0}) == glass);
  }
}
void stateViewsAndTransitions() {
  const std::string key = "_instrument_";
  const std::string value = "_iron_xylophone_";
  const auto keyView = std::string_view(key).substr(1, key.size() - 2);
  const auto valueView = std::string_view(value).substr(1, value.size() - 2);
  const auto note = Blocks::NOTE_BLOCK.defaultState().with(keyView, valueView);
  assert(Blocks::NOTE_BLOCK.stateCount() == 1350);
  assert(Blocks::NOTE_BLOCK.defaultState().with("instrument", "trumpet_weathered")
             .getView("instrument") == "trumpet_weathered");
  assert(Blocks::NOTE_BLOCK.defaultState().with("instrument", "custom_head")
             .getView("instrument") == "custom_head");
  assert(note.getView(keyView) == "iron_xylophone");
  assert(note.with(keyView, valueView) == note);
  assert(note.getView("missing", "fallback") == "fallback");
  // Out-of-schema values are no longer registered: a state change outside the
  // block's own StateDefinition is a hard error, like vanilla's setValue.
  bool threw = false;
  try {
    (void)note.with(keyView, "an_imported_value_longer_than_small_string_storage");
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  threw = false;
  try {
    (void)note.with("outside_schema", "true");
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  for (const auto &registration : blockRegistry()) {
    const auto &block = blockByName(registration.name);
    for (const auto state : block.states())
      for (const auto &property : block.stateDefinition().properties())
        for (const auto &nextValue : property.values()) {
          auto expected = state.properties();
          expected[property.name()] = nextValue;
          std::uint32_t variant = 0;
          assert(block.stateDefinition().variantOf(expected, variant));
          const auto next = state.with(property.name(), std::string_view(nextValue));
          assert(next.id() == block.firstState() + variant);
          assert(next.getView(property.name()) == nextValue);
        }
  }
}
void addedBlockTicks() {
  World lamp;
  lamp.loadChunk(0, 0);
  Pos p{4, 65, 4}, power{5, 65, 4};
  lamp.setBlock(p, defaultState("redstone_lamp"));
  lamp.setBlock(power, defaultState("redstone_block"));
  assert(lamp.getBlock(p).get("lit") == "true");
  assert(!lamp.hasScheduledTick(p, "redstone_lamp"));
  lamp.setBlock(power, defaultState("air"));
  assert(lamp.hasScheduledTick(p, "redstone_lamp"));
  for (int i = 0; i < 3; ++i) lamp.tick();
  assert(lamp.getBlock(p).get("lit") == "true");
  // The callback rechecks power rather than unconditionally toggling LIT.
  lamp.setBlock(power, defaultState("redstone_block"));
  lamp.tick();
  assert(lamp.getBlock(p).get("lit") == "true");
  assert(!lamp.hasScheduledTick(p, "redstone_lamp"));
  lamp.setBlock(power, defaultState("air"));
  for (int i = 0; i < 3; ++i) lamp.tick();
  assert(lamp.getBlock(p).get("lit") == "true");
  lamp.tick();
  assert(lamp.getBlock(p).get("lit") == "false");
  assert(blockMetadata(defaultState("redstone_lamp")).conductor);
  assert(blockMetadata(defaultState("stone_brick_wall")).explosionResistance == 6);
  assert(blockMetadata(defaultState("stone_brick_wall")).hardness == 1.5f);

  World flicker;
  flicker.loadChunk(0, 0);
  flicker.rawSetBlock(p, defaultState("redstone_lamp").with("lit", "true"));
  flicker.neighborChanged(p, "redstone_block"); // First off edge is due at 4.
  flicker.tick();
  flicker.setBlock(power, defaultState("redstone_block"));
  flicker.tick();
  flicker.setBlock(power, defaultState("air")); // Does not restart the 4-tick delay.
  assert(flicker.scheduledTickCount() == 1 &&
         flicker.scheduledTicks()[0].trigger == 4);
  flicker.tick();
  assert(flicker.getBlock(p).get("lit") == "true");
  flicker.tick();
  assert(flicker.getBlock(p).get("lit") == "false");

  World sand;
  sand.loadChunk(0, 0);
  sand.rawSetBlock({4, 60, 4}, defaultState("smooth_stone"));
  sand.setBlock(p, defaultState("sand"));
  assert(sand.hasScheduledTick(p, "sand"));
  sand.tick();
  assert(sand.getBlock(p).name() == "minecraft:sand" && sand.entities.empty());
  sand.tick();
  assert(sand.getBlock(p).name() == "minecraft:air");
  assert(sand.entities.size() == 1);
  auto *falling = dynamic_cast<FallingBlockEntity *>(sand.entities[0].get());
  assert(falling && falling->blockState.name() == "minecraft:sand");
  assert(falling->time == 1); // Scheduled spawn precedes this tick's entity phase.
  for (int i = 0; i < 30; ++i) sand.tick();
  assert(sand.getBlock({4, 61, 4}).name() == "minecraft:sand");
  assert(sand.entities.empty());

  World redSand;
  redSand.loadChunk(0, 0);
  redSand.rawSetBlock({4, 60, 4}, Blocks::STONE.defaultState());
  redSand.setBlock(p, Blocks::RED_SAND.defaultState());
  assert(redSand.hasScheduledTick(p, "red_sand"));
  redSand.tick(); redSand.tick();
  assert(redSand.getBlock(p).isAir() && redSand.entities.size() == 1);
  auto *redFalling = dynamic_cast<FallingBlockEntity *>(redSand.entities[0].get());
  assert(redFalling && redFalling->blockState.is(Blocks::RED_SAND));
  for (int i = 0; i < 30; ++i) redSand.tick();
  assert(redSand.getBlock({4, 61, 4}).is(Blocks::RED_SAND));

  World gravel;
  gravel.loadChunk(0, 0);
  gravel.rawSetBlock({4, 60, 4}, Blocks::STONE.defaultState());
  gravel.setBlock(p, Blocks::GRAVEL.defaultState());
  assert(gravel.hasScheduledTick(p, "gravel"));
  gravel.tick(); gravel.tick();
  assert(gravel.getBlock(p).isAir() && gravel.entities.size() == 1);
  auto *gravelFalling = dynamic_cast<FallingBlockEntity *>(gravel.entities[0].get());
  assert(gravelFalling && gravelFalling->blockState.is(Blocks::GRAVEL));
  for (int i = 0; i < 30; ++i) gravel.tick();
  assert(gravel.getBlock({4, 61, 4}).is(Blocks::GRAVEL));

  World supported;
  supported.loadChunk(0, 0);
  supported.rawSetBlock(p, defaultState("sand"));
  supported.rawSetBlock(p.relative(Direction::Down), defaultState("glass"));
  // Lateral shape updates schedule even while supported; neighborChanged alone
  // does not substitute for FallingBlock.updateShape.
  supported.neighborChanged(p, "smooth_stone");
  assert(!supported.hasScheduledTick(p, "sand"));
  supported.setBlock(p.relative(Direction::East), defaultState("smooth_stone"), 2);
  assert(supported.hasScheduledTick(p, "sand"));
  supported.tick(); supported.tick();
  assert(supported.entities.empty() && supported.getBlock(p).name() == "minecraft:sand");
  supported.setBlock(p.relative(Direction::Down), defaultState("air"), 2);
  supported.tick();
  assert(supported.entities.empty());
  supported.tick();
  assert(supported.entities.size() == 1);

  World suppressed;
  suppressed.loadChunk(0, 0);
  suppressed.setBlock(p, defaultState("sand"), 2 | 16 | 512);
  assert(!suppressed.hasScheduledTick(p, "sand"));
  suppressed.scheduleTick(p, "sand", 2);
  suppressed.setBlock(p, defaultState("smooth_stone"), 2 | 16);
  suppressed.tick(); suppressed.tick();
  assert(suppressed.entities.empty()); // Stale callbacks check current block type.
}
void inlineSectionStorage() {
  // Chunk sections are stored inline. A section that was never written keeps
  // bits == 0 and no palette, so writing air (the value it already holds) must
  // not materialise any storage.
  {
    World w;
    w.loadChunk(0, 0);
    w.rawSetBlock({0, 64, 0}, airState());
    const auto chunk = w.snapshotChunk(0, 0);
    const int section = (64 - w.minY()) / 16;
    assert(chunk.sections[section].bits == 0);
    assert(chunk.sections[section].nonEmpty == 0);
    assert(chunk.sections[section].palette.empty());
    assert(chunk.sections[section].indices8.empty());
    assert(w.getBlock({0, 64, 0}).isAir());
  }
  // One real write materialises exactly one section; its neighbours stay empty.
  {
    World w;
    w.loadChunk(0, 0);
    const auto stone = Blocks::SMOOTH_STONE.defaultState();
    w.rawSetBlock({3, 70, 5}, stone);
    const auto chunk = w.snapshotChunk(0, 0);
    const int section = (70 - w.minY()) / 16;
    assert(chunk.sections[section].bits == 8);
    assert(chunk.sections[section].nonEmpty == 1);
    assert(w.getBlock({3, 70, 5}) == stone);
    for (std::size_t i = 0; i < chunk.sections.size(); ++i)
      if (static_cast<int>(i) != section)
        assert(chunk.sections[i].bits == 0 && chunk.sections[i].nonEmpty == 0);
  }
  // Past 256 distinct states the palette widens to 16 bit indices; every cell
  // must still resolve to the state it was written with.
  {
    World w;
    w.loadChunk(0, 0);
    // 1350 registered note block states: 25 notes x 27 instruments x 2 powered.
    const std::vector<BlockState> all =
        findBlockByName("minecraft:note_block")->states();
    assert(all.size() == 1350);
    const std::vector<BlockState> states(all.begin(), all.begin() + 300);
    for (int n = 0; n < 300; ++n) {
      const int x = n & 15, z = (n >> 4) & 15, y = 64 + n / 256;
      w.rawSetBlock({x, y, z}, states[n]);
    }
    for (int n = 0; n < 300; ++n) {
      const int x = n & 15, z = (n >> 4) & 15, y = 64 + n / 256;
      assert(w.getBlock({x, y, z}) == states[n]);
    }
    const auto chunk = w.snapshotChunk(0, 0);
    const int section = (64 - w.minY()) / 16;
    assert(chunk.sections[section].bits == 16);
    assert(chunk.sections[section].nonEmpty == 300);
    assert(chunk.sections[section].palette.size() == 301); // air + 300 states
    assert(chunk.sections[section].indices16.size() == 4096);
    assert(chunk.sections[section].indices8.empty());
  }
  // The fixed inline array covers 24 sections; taller worlds are rejected
  // instead of indexing past the array.
  {
    World edge(0, 24 * 16);
    edge.loadChunk(0, 0);
    const auto stone = Blocks::SMOOTH_STONE.defaultState();
    edge.rawSetBlock({0, edge.maxY() - 1, 0}, stone);
    assert(edge.getBlock({0, edge.maxY() - 1, 0}) == stone);
    bool threw = false;
    try {
      World tooTall(0, 24 * 16 + 1);
    } catch (const std::invalid_argument &) {
      threw = true;
    }
    assert(threw);
  }
}
void lowMemorySectionCompaction() {
  World w;
  assert(w.lowMemoryMode());  // 默认低内存
  w.setLowMemoryMode(true);
  w.loadChunk(0, 0);
  const int secY = (64 - w.minY()) / 16 + (w.minY() >> 4);
  assert(w.sectionStorageBitsForTest(0, secY, 0) == 0);
  const std::vector<BlockState> all =
      findBlockByName("minecraft:note_block")->states();
  assert(all.size() >= 300);

  auto expectedBitsForCount = [](int uniqueNonAir) -> std::uint8_t {
    const int totalPalette = 1 + uniqueNonAir;
    if (totalPalette <= 2)
      return 1;
    if (totalPalette <= 4)
      return 2;
    if (totalPalette <= 8)
      return 3;
    if (totalPalette <= 16)
      return 4;
    if (totalPalette <= 32)
      return 5;
    if (totalPalette <= 64)
      return 6;
    if (totalPalette <= 128)
      return 7;
    if (totalPalette <= 256)
      return 8;
    return 16;
  };
  for (int n = 0; n < 300; ++n) {
    const int x = n & 15, z = (n >> 4) & 15, y = 64 + n / 256;
    w.rawSetBlock({x, y, z}, all[n]);
    assert(w.sectionStorageBitsForTest(0, secY, 0) ==
           expectedBitsForCount(n + 1));
  }
  for (int n = 0; n < 300; ++n) {
    const int x = n & 15, z = (n >> 4) & 15, y = 64 + n / 256;
    assert(w.getBlock({x, y, z}) == all[n]);
  }

  // importSectionBlocks chooses compact bit width and snapshotChunk expands
  // back to 8-bit indices for wire serialization.
  w.loadChunk(1, 0);
  std::array<BlockState::Id, 5> pal5{
      airState().id(), all[0].id(), all[1].id(), all[2].id(), all[3].id()};
  std::array<std::uint16_t, kLightVolume> idx5{};
  for (int i = 0; i < kLightVolume; ++i)
    idx5[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(i % 5);
  w.importSectionBlocks(1, 0, secY, pal5, idx5);
  assert(w.sectionStorageBitsForTest(1, secY, 0) == 3);
  const auto snap = w.snapshotChunk(1, 0);
  const int secIdx = (64 - w.minY()) / 16;
  assert(snap.sections[secIdx].bits == 8);
  assert(snap.sections[secIdx].indices8.size() == kLightVolume);
  for (int i = 0; i < 64; ++i) {
    const auto pIdx = snap.sections[secIdx].indices8[i];
    assert(snap.sections[secIdx].palette[pIdx] == pal5[i % 5]);
  }

  // Sparse random ticking works across bit-packed storage.
  w.loadChunk(2, 0);
  w.setSeed(77);
  w.randomTickSpeed = 400;
  w.rawSetBlock({32, 64, 0}, Blocks::GRASS_BLOCK.defaultState());
  w.rawSetBlock({32, 65, 0}, Blocks::WATER.defaultState());
  assert(w.sectionStorageBitsForTest(2, secY, 0) == 2);
  bool decayed = false;
  for (int t = 0; t < 200; ++t) {
    w.tick();
    if (w.getBlock({32, 64, 0}).is(Blocks::DIRT)) {
      decayed = true;
      break;
    }
  }
  assert(decayed);
}
// The canonical bit fields must agree with the string registry for every
// registered state, and a transition through the enum must land on exactly the
// same state id as the same transition through the property/value names.
void enumPropertyAccessors() {
  std::size_t transitions = 0;
  for (const auto &entry : blockRegistry()) {
    const Block *block = findBlockByName(entry.name);
    assert(block != nullptr);
    const StateDefinition &definition = block->stateDefinition();
    for (const BlockState &state : block->states()) {
      assert(state.isSchemaState());
      assert(definition.packedOf(state.variant()) == state.packed());
      for (const Property &property : definition.properties()) {
        const std::uint32_t canonical = propertyValue(state, property.kind());
        const std::uint8_t local = property.localIndex(canonical);
        assert(local != Property::kAbsent);
        assert(property.values()[local] == state.getView(property.name()));
        for (std::uint32_t v = 0; v < property.values().size(); ++v) {
          const std::uint8_t target = property.canonicalValue(v);
          assert(state.with(property.kind(), target) ==
                 state.with(property.name(), property.values()[v]));
          ++transitions;
        }
      }
    }
  }
  assert(transitions > 1000); // the registry really was enumerated
  // A property the block does not declare is rejected, not silently applied.
  const BlockState air = airState();
  assert(facingOf(air) == Facing::North);
  bool threw = false;
  try {
    (void)air.with(PropertyKind::Facing, 0);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  threw = false;
  try {
    (void)air.with("facing", "north");
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  // Values outside the block's own list are rejected too (a gate has four
  // facings, not six).
  const BlockState gate = defaultState("oak_fence_gate");
  assert(facingOf(gate) == Facing::North);
  threw = false;
  try {
    (void)gate.with(PropertyKind::Facing, static_cast<std::uint8_t>(Facing::Down));
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  assert(threw);
  assert(gate.with(PropertyKind::Facing, static_cast<std::uint8_t>(Facing::East)) !=
         gate);
  // Enum accessors agree with the string accessors on a real state.
  const BlockState observer =
      defaultState("observer").with(PropertyKind::Powered, std::uint8_t{1});
  assert(poweredOf(observer) && observer.getView("powered") == "true");
  assert(directionOf(observer, Direction::South) == Direction::South);
}
int main() {
  assert(blockHasTag(Blocks::SNOW, "minecraft:snow") == true);
  assert(blockHasTag(Blocks::SNOW_BLOCK, "minecraft:snow") == true);
  assert(blockHasTag(Blocks::DIRT, "minecraft:snow") == false);
  assert(blockHasTag(Blocks::DIRT, "minecraft:supports_vegetation") == true);
  assert(blockHasTag(Blocks::STONE, "minecraft:supports_vegetation") == false);
  assert(blockHasTag(Blocks::OAK_LOG, "minecraft:logs") == true);
  assert(blockHasTag(Blocks::OAK_LOG, "minecraft:does_not_exist") == std::nullopt);
  grassRandomTick();
  mushroomRandomTick();
  assert(Blocks::AIR.defaultState().isAir());
  assert(!blocksMotion(Blocks::AIR.defaultState()));
  assert(Blocks::AIR.metadata().pushReaction == "normal");
  assert(Blocks::BEDROCK.metadata().pushReaction == "normal");
  assert(Blocks::BEDROCK.metadata().hardness == -1.0f);
  assert(Blocks::BEDROCK.metadata().explosionResistance == 3600000.0f);
  assert(Blocks::PODZOL.stateCount() == 2);
  assert(Blocks::PODZOL.defaultState().getView("snowy") == "false");
  assert(Blocks::MANGROVE_ROOTS.stateCount() == 2);
  assert(Blocks::MANGROVE_ROOTS.defaultState().getView("waterlogged") == "false");
  assert(Blocks::MUDDY_MANGROVE_ROOTS.stateCount() == 3);
  assert(Blocks::MUDDY_MANGROVE_ROOTS.metadata().hardness == 0.7f);
  assert(Blocks::MUDDY_MANGROVE_ROOTS.defaultState().getView("axis") == "y");
  assert(Blocks::BAMBOO_BLOCK.stateCount() == 3);
  assert(Blocks::BAMBOO_BLOCK.defaultState().getView("axis") == "y");
  for (const Block *block : {&Blocks::STRIPPED_ACACIA_LOG, &Blocks::STRIPPED_CHERRY_LOG,
                              &Blocks::STRIPPED_DARK_OAK_LOG, &Blocks::STRIPPED_PALE_OAK_LOG,
                              &Blocks::STRIPPED_MANGROVE_LOG, &Blocks::STRIPPED_BAMBOO_BLOCK}) {
    assert(block->stateCount() == 3 && block->defaultState().getView("axis") == "y");
    for (const auto state : block->states()) {
      assert(state.isRandomlyTicking() == false && blocksMotion(state));
      assert(lightPropertiesOf(state).supported);
    }
  }
  const auto wetRoots = Blocks::MANGROVE_ROOTS.defaultState().with("waterlogged", "true");
  assert(fluidStateOf(wetRoots).is(Fluids::water()));
  assert(wetRoots.block().isLiquidContainer());
  World roots;
  roots.loadChunk(0, 0);
  roots.rawSetBlock({1, 5, 1}, wetRoots);
  roots.updateFromNeighbourShapes({1, 5, 1}, wetRoots);
  assert(!roots.fluidTicks().empty());
  World podzol;
  podzol.loadChunk(0, 0);
  const Pos podzolPos{1, 5, 1};
  podzol.rawSetBlock(podzolPos, Blocks::PODZOL.defaultState());
  podzol.setBlock(podzolPos.relative(Direction::Up), Blocks::SNOW_BLOCK.defaultState());
  assert(podzol.getBlock(podzolPos).getView("snowy") == "true");
  podzol.setBlock(podzolPos.relative(Direction::Up), airState());
  assert(podzol.getBlock(podzolPos).getView("snowy") == "false");
  World stoneNote;
  stoneNote.loadChunk(0, 0);
  stoneNote.rawSetBlock({0, 0, 0}, Blocks::STONE.defaultState());
  assert(stoneNote.updateFromNeighbourShapes({0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
             .getView("instrument") == "basedrum");
  for (const Block *ore : {&Blocks::GOLD_ORE, &Blocks::DEEPSLATE_GOLD_ORE,
                           &Blocks::IRON_ORE, &Blocks::DEEPSLATE_IRON_ORE,
                           &Blocks::COAL_ORE, &Blocks::DEEPSLATE_COAL_ORE,
                           &Blocks::NETHER_GOLD_ORE}) {
    stoneNote.rawSetBlock({0, 0, 0}, ore->defaultState());
    assert(stoneNote.updateFromNeighbourShapes({0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == "basedrum");
  }
  stoneNote.rawSetBlock({0, 0, 0}, Blocks::COBBLESTONE.defaultState());
  assert(stoneNote.updateFromNeighbourShapes({0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
             .getView("instrument") == "basedrum");
  for (const Block *ore : {&Blocks::LAPIS_ORE, &Blocks::DEEPSLATE_LAPIS_ORE}) {
    stoneNote.rawSetBlock({0, 0, 0}, ore->defaultState());
    assert(stoneNote.updateFromNeighbourShapes({0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == "basedrum");
  }
  for (const Block *block : {&Blocks::SANDSTONE, &Blocks::CHISELED_SANDSTONE,
                              &Blocks::CUT_SANDSTONE}) {
    stoneNote.rawSetBlock({0, 0, 0}, block->defaultState());
    assert(stoneNote.updateFromNeighbourShapes({0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == "basedrum");
  }
  stoneNote.rawSetBlock({0, 0, 0}, Blocks::LAPIS_BLOCK.defaultState());
  assert(stoneNote.updateFromNeighbourShapes({0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
             .getView("instrument") == "harp");
  assert(Blocks::OAK_PLANKS.stateCount() == 1);
  assert(Blocks::OAK_PLANKS.metadata().hardness == 2.0f);
  assert(Blocks::OAK_PLANKS.metadata().explosionResistance == 3.0f);
  assert(Blocks::OAK_LOG.stateCount() == 3);
  assert(Blocks::OAK_LOG.defaultState().getView("axis") == "y");
  assert(Blocks::OAK_LOG.metadata().explosionResistance == 2.0f);
  assert(Blocks::STRIPPED_OAK_LOG.stateCount() == 3);
  assert(Blocks::STRIPPED_OAK_LOG.defaultState().getView("axis") == "y");
  assert(Blocks::SPRUCE_PLANKS.stateCount() == 1);
  assert(Blocks::SPRUCE_PLANKS.metadata().explosionResistance == 3.0f);
  assert(Blocks::BIRCH_PLANKS.stateCount() == 1);
  assert(Blocks::BIRCH_PLANKS.metadata().explosionResistance == 3.0f);
  assert(Blocks::JUNGLE_PLANKS.stateCount() == 1);
  assert(Blocks::JUNGLE_PLANKS.metadata().explosionResistance == 3.0f);
  assert(Blocks::ACACIA_PLANKS.stateCount() == 1);
  assert(Blocks::ACACIA_PLANKS.metadata().hardness == 2.0f);
  assert(Blocks::ACACIA_PLANKS.metadata().explosionResistance == 3.0f);
  for (const Block *planks : {&Blocks::CHERRY_PLANKS, &Blocks::DARK_OAK_PLANKS,
                              &Blocks::PALE_OAK_PLANKS, &Blocks::MANGROVE_PLANKS,
                              &Blocks::BAMBOO_PLANKS, &Blocks::BAMBOO_MOSAIC}) {
    assert(planks->stateCount() == 1 && planks->metadata().hardness == 2.0f);
    assert(planks->metadata().explosionResistance == 3.0f);
    assert(lightPropertiesOf(planks->defaultState()).supported);
  }
  for (const Block *log : {&Blocks::SPRUCE_LOG, &Blocks::STRIPPED_SPRUCE_LOG,
                           &Blocks::BIRCH_LOG, &Blocks::STRIPPED_BIRCH_LOG,
                           &Blocks::JUNGLE_LOG, &Blocks::STRIPPED_JUNGLE_LOG,
                           &Blocks::ACACIA_LOG, &Blocks::CHERRY_LOG, &Blocks::DARK_OAK_LOG,
                           &Blocks::PALE_OAK_LOG, &Blocks::MANGROVE_LOG,
                           &Blocks::PALE_OAK_WOOD, &Blocks::OAK_WOOD, &Blocks::SPRUCE_WOOD,
                           &Blocks::BIRCH_WOOD, &Blocks::JUNGLE_WOOD, &Blocks::ACACIA_WOOD,
                           &Blocks::CHERRY_WOOD, &Blocks::DARK_OAK_WOOD, &Blocks::MANGROVE_WOOD,
                           &Blocks::STRIPPED_OAK_WOOD,
                           &Blocks::STRIPPED_SPRUCE_WOOD,
                           &Blocks::STRIPPED_BIRCH_WOOD,
                           &Blocks::STRIPPED_JUNGLE_WOOD,
                           &Blocks::STRIPPED_ACACIA_WOOD,
                           &Blocks::STRIPPED_CHERRY_WOOD,
                           &Blocks::STRIPPED_DARK_OAK_WOOD,
                           &Blocks::STRIPPED_PALE_OAK_WOOD,
                           &Blocks::STRIPPED_MANGROVE_WOOD}) {
    assert(log->stateCount() == 3);
    assert(log->defaultState().getView("axis") == "y");
    assert(log->metadata().hardness == 2.0f && log->metadata().explosionResistance == 2.0f);
    for (const auto *axis : {"x", "y", "z"})
      assert(lightPropertiesOf(log->defaultState().with("axis", axis)).supported);
  }
  for (const auto axis : {"x", "y", "z"}) {
    const auto log = Blocks::OAK_LOG.defaultState().with("axis", axis);
    assert(log.getView("axis") == axis && blocksMotion(log));
    assert(lightPropertiesOf(log).supported);
  }
  const auto logX = Blocks::OAK_LOG.defaultState().with("axis", "x");
  assert(rotatePillar(logX, Rotation::Clockwise90).getView("axis") == "z");
  assert(rotatePillar(logX, Rotation::Counterclockwise90).getView("axis") == "z");
  assert(rotatePillar(logX, Rotation::Clockwise180) == logX);
  assert(rotatePillar(Blocks::OAK_LOG.defaultState(), Rotation::Clockwise90)
             .getView("axis") == "y");
  World woodInstrument;
  woodInstrument.loadChunk(0, 0);
  for (const Block *wood : {&Blocks::OAK_PLANKS, &Blocks::OAK_LOG,
                            &Blocks::STRIPPED_OAK_LOG, &Blocks::SPRUCE_PLANKS,
                            &Blocks::SPRUCE_LOG, &Blocks::STRIPPED_SPRUCE_LOG,
                            &Blocks::BIRCH_PLANKS, &Blocks::BIRCH_LOG,
                            &Blocks::STRIPPED_BIRCH_LOG, &Blocks::JUNGLE_PLANKS,
                            &Blocks::JUNGLE_LOG, &Blocks::STRIPPED_JUNGLE_LOG, &Blocks::ACACIA_LOG,
                            &Blocks::CHERRY_LOG, &Blocks::DARK_OAK_LOG, &Blocks::PALE_OAK_LOG,
                            &Blocks::MANGROVE_LOG,
                            &Blocks::ACACIA_PLANKS, &Blocks::CHERRY_PLANKS,
                            &Blocks::DARK_OAK_PLANKS, &Blocks::PALE_OAK_WOOD,
                            &Blocks::OAK_WOOD, &Blocks::SPRUCE_WOOD,
                            &Blocks::BIRCH_WOOD, &Blocks::JUNGLE_WOOD, &Blocks::ACACIA_WOOD,
                            &Blocks::CHERRY_WOOD, &Blocks::DARK_OAK_WOOD, &Blocks::MANGROVE_WOOD,
                            &Blocks::PALE_OAK_PLANKS, &Blocks::MANGROVE_PLANKS,
                            &Blocks::BAMBOO_PLANKS, &Blocks::BAMBOO_MOSAIC}) {
    woodInstrument.rawSetBlock({0, 0, 0}, wood->defaultState());
    assert(woodInstrument.updateFromNeighbourShapes(
               {0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == "bass");
  }
  for (const Block *block : {&Blocks::GRANITE, &Blocks::POLISHED_GRANITE,
                             &Blocks::DIORITE, &Blocks::POLISHED_DIORITE,
                             &Blocks::ANDESITE, &Blocks::POLISHED_ANDESITE,
                             &Blocks::DIRT, &Blocks::COARSE_DIRT}) {
    assert(findBlockByName(block->name()) == block);
    assert(block->supported() && block->stateCount() == 1);
    assert(block->defaultState().properties().empty());
    assert(blocksMotion(block->defaultState()));
    assert(isRedstoneConductor(block->defaultState()));
    assert(lightPropertiesOf(block->defaultState()).supported);
    assert(blockMetadata(block->defaultState()).pushReaction == "normal");
  }
  for (const Block *block : {&Blocks::GRANITE, &Blocks::POLISHED_GRANITE,
                             &Blocks::DIORITE, &Blocks::POLISHED_DIORITE,
                             &Blocks::ANDESITE, &Blocks::POLISHED_ANDESITE}) {
    assert(block->metadata().hardness == 1.5f);
    assert(block->metadata().explosionResistance == 6.0f);
  }
  World stoneInstrument;
  stoneInstrument.loadChunk(0, 0);
  for (const Block *block : {&Blocks::GRANITE, &Blocks::POLISHED_GRANITE,
                             &Blocks::DIORITE, &Blocks::POLISHED_DIORITE,
                             &Blocks::ANDESITE, &Blocks::POLISHED_ANDESITE}) {
    stoneInstrument.rawSetBlock({0, 0, 0}, block->defaultState());
    assert(stoneInstrument.updateFromNeighbourShapes(
               {0, 1, 0}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == "basedrum");
  }
  sectionReadCacheLifecycle();
  inlineSectionStorage();
  lowMemorySectionCompaction();
  enumPropertyAccessors();
  stateViewsAndTransitions();
  addedBlockTicks();
  World w;
  w.loadChunk(-1, -1);
  w.loadChunk(0, 0);
  w.loadChunk(1, 0);
  w.rawSetBlock({-1, -1, -1}, defaultState("bedrock"));
  assert(w.getBlock({-1, -1, -1}).name() == "minecraft:bedrock");
  assert(w.allBlocks().contains(Pos{-1, -1, -1}));
  auto observer = defaultState("observer").with("facing", "east");
  w.rawSetBlock({1, 0, 0}, observer);
  w.setBlock({2, 0, 0}, defaultState("smooth_stone"), 2);
  assert(w.hasScheduledTick({1, 0, 0}, "observer"));
  w.tick();
  assert(w.getBlock({1, 0, 0}).get("powered") == "false");
  w.tick();
  assert(w.getBlock({1, 0, 0}).get("powered") == "true");
  w.tick();
  w.tick();
  assert(w.getBlock({1, 0, 0}).get("powered") == "false");
  World ticks;
  ticks.traceUpdates = true;
  ticks.loadChunk(0, 0);
  ticks.loadChunk(1, 0);
  ticks.scheduleTick({0, 0, 0}, "air", 0, 3);
  ticks.scheduleTick({1, 0, 0}, "air", 0, -3);
  ticks.scheduleTick({16, 0, 0}, "air", 0, 0);
  ticks.tick();
  std::vector<Pos> order;
  for (const auto &e : ticks.log)
    if (e.kind == "scheduled_tick")
      order.push_back(e.pos);
  assert((order == std::vector<Pos>{{1, 0, 0}, {16, 0, 0}, {0, 0, 0}}));
  World coral;
  coral.loadChunk(0, 0);
  coral.rawSetBlock({0, 0, 0}, defaultState("smooth_stone"));
  coral.rawSetBlock({0, 1, 0}, defaultState("dead_tube_coral_fan"));
  coral.setBlock({0, 0, 0}, defaultState("air"));
  // BaseCoralPlantTypeBlock.updateShape answers AIR and Level.destroyBlock then
  // writes the position's fluid legacy block, not air (Level.java:288,298).
  // The fan defaults to waterlogged, so the cell keeps a water source.
  assert(coral.getBlock({0, 1, 0}).name() == "minecraft:water");
  assert(fluidStateOf(coral.getBlock({0, 1, 0})).isSource());
  World dryCoral;
  dryCoral.loadChunk(0, 0);
  dryCoral.rawSetBlock({0, 0, 0}, defaultState("smooth_stone"));
  dryCoral.rawSetBlock({0, 1, 0},
                       defaultState("dead_tube_coral_fan")
                           .with("waterlogged", "false"));
  dryCoral.setBlock({0, 0, 0}, defaultState("air"));
  assert(dryCoral.getBlock({0, 1, 0}).isAir());
  World events;
  events.loadChunk(0, 0);
  events.rawSetBlock({0, 0, 0}, defaultState("note_block"));
  bool saw = false;
  events.blockEntityTicker = [&](World &v) {
    assert(!v.isHandlingTick());
    for (auto &e : v.log)
      if (e.kind == "note")
        saw = true;
  };
  events.blockEvent({0, 0, 0}, "note_block", 0, 0);
  events.tick();
  assert(saw);
  assert(blockMetadata(defaultState("slime_block")).conductor);
  assert(!blockMetadata(defaultState("honey_block")).conductor);
  assert(!blockMetadata(defaultState("observer")).conductor);
  assert(!blockMetadata(BlockState("example:unknown")).supported);
  World tracing;
  tracing.loadChunk(0, 0);
  tracing.setBlock({0, 0, 0}, defaultState("smooth_stone"));
  assert(tracing.log.empty());
  tracing.traceUpdates = true;
  tracing.setBlock({1, 0, 0}, defaultState("smooth_stone"));
  assert(!tracing.log.empty());
  World grass;
  grass.loadChunk(0, 0);
  grass.rawSetBlock({0, 0, 0}, defaultState("grass_block"));
  grass.setBlock({0, 1, 0}, BlockState("minecraft:snow"), 2);
  assert(grass.getBlock({0, 0, 0}).get("snowy") == "true");
  World limited;
  limited.loadChunk(0, 0);
  limited.rawSetBlock({1, 0, 0},
                      defaultState("observer").with("facing", "east"));
  limited.setBlock({2, 0, 0}, defaultState("smooth_stone"), 2, 0);
  assert(!limited.hasScheduledTick({1, 0, 0}, "observer"));
  limited.setBlock({2, 0, 0}, defaultState("glass"), 2, 1);
  assert(limited.hasScheduledTick({1, 0, 0}, "observer"));
  World full;
  full.loadChunk(0, 0);
  full.rawSetBlock({0, 0, 0}, defaultState("grass_block"));
  full.rawSetBlock({1, 0, 0}, defaultState("redstone_block"));
  full.neighborChanged({0, 0, 0}, "redstone_block");
  assert(full.getBlock({0, 0, 0}).name() == "minecraft:grass_block");
  full.neighborChanged(defaultState("note_block"), {0, 0, 0}, "redstone_block",
                       true);
  assert(full.getBlock({0, 0, 0}).name() == "minecraft:note_block");
  assert(full.getBlock({0, 0, 0}).get("powered") == "true");
  World derived;
  derived.loadChunk(0, 0);
  derived.rawSetBlock({0, -1, 0}, defaultState("white_terracotta"));
  assert(
      derived.updateFromNeighbourShapes({0, 0, 0}, defaultState("note_block"))
          .get("instrument") == "basedrum");
  derived.updateFromNeighbourShapes(
      {0, 0, 0}, defaultState("observer").with("facing", "east"));
  assert(derived.hasScheduledTick({0, 0, 0}, "observer"));
  for (const auto *color :
       {"white", "orange", "magenta", "light_blue", "yellow", "lime", "pink",
        "gray", "light_gray", "cyan", "purple", "blue", "brown", "green", "red",
        "black"}) {
    const auto name = std::string(color) + "_carpet";
    auto carpet = defaultState(name);
    assert(isSupportedBlock(name) && isWoolCarpet(carpet));
    assert(carpet.properties().empty());
    assert(blockMetadata(carpet).pushReaction == "normal");
    assert(!blockMetadata(carpet).conductor);
    World carpetWorld;
    carpetWorld.loadChunk(0, 0);
    carpetWorld.rawSetBlock({0, 0, 0}, defaultState("dead_tube_coral_fan"));
    carpetWorld.rawSetBlock({0, 1, 0}, carpet);
    carpetWorld.neighborShapeChanged({0, 1, 0}, Direction::East, {1, 1, 0},
                                     defaultState("air"), 2, 512);
    assert(carpetWorld.getBlock({0, 1, 0}) == carpet);
    carpetWorld.setBlock({0, 0, 0}, defaultState("air"), 2);
    assert(carpetWorld.getBlock({0, 1, 0}).name() == "minecraft:air");
    bool dropped = false;
    for (const auto &entry : carpetWorld.log)
      dropped |= entry.kind == "entity_spawn" &&
                 entry.detail == "minecraft:item:minecraft:" + name;
    assert(dropped);
  }
  World suppressed;
  suppressed.loadChunk(0, 0);
  auto white = defaultState("white_carpet");
  suppressed.rawSetBlock({0, 1, 0}, white);
  suppressed.neighborShapeChanged({0, 1, 0}, Direction::Up, {0, 2, 0},
                                  defaultState("air"), 32, 512);
  assert(suppressed.getBlock({0, 1, 0}).name() == "minecraft:air");
  for (const auto &entry : suppressed.log)
    assert(entry.kind != "entity_spawn");
  for (const auto *type : {"tube", "brain", "bubble", "fire", "horn"}) {
    auto floor = defaultState(std::string("dead_") + type + "_coral_fan");
    for (auto face : {Direction::North, Direction::South, Direction::West,
                      Direction::East}) {
      World world;
      world.loadChunk(0, 0);
      Pos pos{2, 2, 2};
      Pos support = pos.relative(opposite(face));
      world.rawSetBlock(support, defaultState("smooth_stone"));
      auto placed = world.coralPlacementState(pos, floor, face);
      assert(placed && isDeadCoralWallFan(*placed));
      assert(blockMetadata(*placed).pushReaction == "normal");
      assert(placed->get("facing") == directionName(face));
      assert(placed->get("waterlogged") == "false");
      world.rawSetBlock(pos, *placed);
      world.setBlock(support, defaultState("air"));
      assert(world.getBlock(pos).name() == "minecraft:air");
    }
    World world;
    world.loadChunk(0, 0);
    world.rawSetBlock({2, 1, 2}, defaultState("glass"));
    auto placed = world.coralPlacementState({2, 2, 2}, floor, Direction::Up);
    assert(placed && placed->name() == floor.name() &&
           placed->get("waterlogged") == "false");
    assert(!world.coralPlacementState({2, 2, 2}, floor, Direction::Down));
  }
  World stale;
  stale.loadChunk(0, 0);
  stale.rawSetBlock({0, 0, 0}, defaultState("red_carpet"));
  stale.updateOrDestroy({0, 0, 0}, defaultState("white_carpet"),
                        defaultState("air"), 0, 512);
  assert(stale.getBlock({0, 0, 0}).name() == "minecraft:air");
  assert(stale.log.size() == 1 &&
         stale.log[0].detail == "minecraft:item:minecraft:red_carpet");
  // The multi-update pauses after west; nested removal work drains before east.
  World nested;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      nested.loadChunk(x, z);
  nested.rawSetBlock({-1, 0, 0}, defaultState("tnt"));
  nested.rawSetBlock({-2, 0, 0}, defaultState("redstone_block"));
  nested.traceUpdates = true;
  nested.updateNeighborsAt({0, 0, 0}, "minecraft:smooth_stone");
  using Row = std::tuple<std::string, Pos, std::string>;
  std::vector<Row> actual;
  for (const auto &e : nested.log)
    actual.emplace_back(e.kind, e.pos, e.detail);
  std::vector<Row> expected = {
      {"neighbor", {-1, 0, 0}, "minecraft:smooth_stone"},
      {"entity_spawn", {-1, 0, 0}, "minecraft:tnt"},
      {"neighbor", {-2, 0, 0}, "minecraft:tnt"},
      {"neighbor", {0, 0, 0}, "minecraft:tnt"},
      {"neighbor", {-1, -1, 0}, "minecraft:tnt"},
      {"neighbor", {-1, 1, 0}, "minecraft:tnt"},
      {"neighbor", {-1, 0, -1}, "minecraft:tnt"},
      {"neighbor", {-1, 0, 1}, "minecraft:tnt"},
      {"shape", {-2, 0, 0}, "east"},
      {"shape", {0, 0, 0}, "west"},
      {"shape", {-1, 0, -1}, "south"},
      {"shape", {-1, 0, 1}, "north"},
      {"shape", {-1, -1, 0}, "up"},
      {"shape", {-1, 1, 0}, "down"},
      {"neighbor", {1, 0, 0}, "minecraft:smooth_stone"},
      {"neighbor", {0, -1, 0}, "minecraft:smooth_stone"},
      {"neighbor", {0, 1, 0}, "minecraft:smooth_stone"},
      {"neighbor", {0, 0, -1}, "minecraft:smooth_stone"},
      {"neighbor", {0, 0, 1}, "minecraft:smooth_stone"}};
  assert(actual == expected);
  World observerTrace;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      observerTrace.loadChunk(x, z);
  observerTrace.rawSetBlock({0, 0, 0},
                            defaultState("observer").with("facing", "east"));
  observerTrace.scheduleTick({0, 0, 0}, "observer", 1);
  observerTrace.rawSetBlock({3, 0, 0}, defaultState("note_block"));
  observerTrace.blockEvent({3, 0, 0}, "note_block", 0, 0);
  observerTrace.traceUpdates = true;
  observerTrace.tick();
  actual.clear();
  for (const auto &e : observerTrace.log)
    actual.emplace_back(e.kind, e.pos, e.detail);
  expected = {{"scheduled_tick", {0, 0, 0}, "minecraft:observer"},
              {"shape", {-1, 0, 0}, "east"},
              {"shape", {1, 0, 0}, "west"},
              {"shape", {0, 0, -1}, "south"},
              {"shape", {0, 0, 1}, "north"},
              {"shape", {0, -1, 0}, "up"},
              {"shape", {0, 1, 0}, "down"},
              {"neighbor", {-1, 0, 0}, "minecraft:observer"},
              {"neighbor", {-2, 0, 0}, "minecraft:observer"},
              {"neighbor", {-1, -1, 0}, "minecraft:observer"},
              {"neighbor", {-1, 1, 0}, "minecraft:observer"},
              {"neighbor", {-1, 0, -1}, "minecraft:observer"},
              {"neighbor", {-1, 0, 1}, "minecraft:observer"},
              {"block_event", {3, 0, 0}, "minecraft:note_block"},
              {"note", {3, 0, 0}, "harp:0"},
              {"block_entities", {}, ""}};
  assert(actual == expected);
  World capturedShape;
  capturedShape.loadChunk(0, 0);
  Pos wallPos{2, 2, 2};
  capturedShape.rawSetBlock(wallPos, defaultState("cobblestone_wall"));
  // The world currently contains air, but the queued shape update carries
  // stone.
  capturedShape.neighborShapeChanged(wallPos, Direction::East, {3, 2, 2},
                                     defaultState("smooth_stone"), 18, 512);
  assert(capturedShape.getBlock(wallPos).get("east") == "low");
  // A captured full cube above makes the existing arm tall even if world is
  // air.
  capturedShape.neighborShapeChanged(wallPos, Direction::Up, {2, 3, 2},
                                     defaultState("smooth_stone"), 18, 512);
  assert(capturedShape.getBlock(wallPos).get("east") == "tall");
  // Chain limit counts queued updates, not each direction in a multi-update.
  World bounded(-64, 320, 1);
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      bounded.loadChunk(x, z);
  bounded.rawSetBlock({-1, 0, 0}, defaultState("tnt"));
  bounded.rawSetBlock({-2, 0, 0}, defaultState("redstone_block"));
  bounded.traceUpdates = true;
  bounded.updateNeighborsAt({0, 0, 0}, "minecraft:smooth_stone");
  std::vector<Pos> boundedNeighbors;
  int skipped = 0;
  for (const auto &entry : bounded.log) {
    if (entry.kind == "neighbor")
      boundedNeighbors.push_back(entry.pos);
    if (entry.kind == "neighbor_update_limit")
      ++skipped;
  }
  assert((boundedNeighbors == std::vector<Pos>{{-1, 0, 0},
                                               {1, 0, 0},
                                               {0, -1, 0},
                                               {0, 1, 0},
                                               {0, 0, -1},
                                               {0, 0, 1}}));
  assert(skipped == 1);
  // Per-chunk trigger time selects each chunk head; only those heads compete
  // globally by intra-tick priority, not global trigger time.
  World backlog;
  backlog.loadChunk(0, 0);
  backlog.loadChunk(1, 0);
  backlog.traceUpdates = true;
  backlog.setGameTime(10);
  backlog.scheduleTick({0, 0, 0}, "air", -5, 2);
  backlog.scheduleTick({1, 0, 0}, "air", -4, -3);
  backlog.scheduleTick({16, 0, 0}, "air", -2, 0);
  backlog.tick();
  std::vector<Pos> drained;
  for (const auto &entry : backlog.log)
    if (entry.kind == "scheduled_tick")
      drained.push_back(entry.pos);
  assert((drained == std::vector<Pos>{{16, 0, 0}, {0, 0, 0}, {1, 0, 0}}));
  World observerPiston;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      observerPiston.loadChunk(x, z);
  observerPiston.rawSetBlock({0, 0, 0},
                             defaultState("observer").with("facing", "east"));
  observerPiston.rawSetBlock({-1, 0, 0},
                             defaultState("piston").with("facing", "north"));
  observerPiston.scheduleTick({0, 0, 0}, "observer", 1);
  observerPiston.traceUpdates = true;
  observerPiston.tick();
  actual.clear();
  for (const auto &e : observerPiston.log)
    actual.emplace_back(e.kind, e.pos, e.detail);
  assert(actual.size() > 16);
  assert((actual[0] == Row{"scheduled_tick", {0, 0, 0}, "minecraft:observer"}));
  assert((actual[7] == Row{"neighbor", {-1, 0, 0}, "minecraft:observer"}));
  assert((actual[13] == Row{"block_event", {-1, 0, 0}, "minecraft:piston"}));
  assert(std::get<0>(actual[14]) == "shape");
  auto phase = std::find_if(actual.begin(), actual.end(), [](const Row &row) {
    return std::get<0>(row) == "block_entities";
  });
  assert(phase != actual.end() && phase > actual.begin() + 14);
  assert(observerPiston.getBlock({-1, 0, 0}).get("extended") == "true");
  World signedOrder;
  signedOrder.loadChunk(-1, 0);
  signedOrder.loadChunk(1, 0);
  signedOrder.traceUpdates = true;
  signedOrder.setGameTime(9);
  // LevelTicks.schedule with explicit subTickOrders, as saved-tick unpack does.
  signedOrder.scheduleTick(
      ScheduledTick{{16, 82, 5}, "minecraft:smooth_stone", 10, 0, 0});
  signedOrder.scheduleTick(
      ScheduledTick{{-1, 82, 5}, "minecraft:smooth_stone", 10, 0, -2});
  signedOrder.tick();
  std::vector<Pos> signedExecution;
  for (const auto &entry : signedOrder.log)
    if (entry.kind == "scheduled_tick")
      signedExecution.push_back(entry.pos);
  assert((signedExecution == std::vector<Pos>{{-1, 82, 5}, {16, 82, 5}}));
  World gates;
  gates.loadChunk(0, 0);
  const Pos gatePos{8, 65, 8};
  auto gate = defaultState("oak_fence_gate");
  assert(gate.get("facing") == "north" && gate.get("open") == "false" &&
         gate.get("powered") == "false" && gate.get("in_wall") == "false");
  assert(blockMetadata(gate).hardness == 2.0f &&
         blockMetadata(gate).explosionResistance == 3.0f &&
         blockMetadata(gate).pushReaction == "normal");
  gates.rawSetBlock(gatePos, gate);
  gates.setBlock({9, 65, 8}, defaultState("stone_brick_wall"));
  assert(gates.getBlock(gatePos).get("in_wall") == "true");
  gates.setBlock({9, 65, 8}, airState());
  assert(gates.getBlock(gatePos).get("in_wall") == "false");
  gates.setBlock({8, 65, 7}, defaultState("redstone_block"));
  assert(gates.getBlock(gatePos).get("powered") == "true" &&
         gates.getBlock(gatePos).get("open") == "true");
  gates.setBlock(gatePos, gates.getBlock(gatePos).with("open", "false"), 10);
  gates.neighborChanged(gatePos, "redstone_block");
  assert(gates.getBlock(gatePos).get("open") == "false");
  gates.setBlock({8, 65, 7}, airState());
  assert(gates.getBlock(gatePos).get("powered") == "false" &&
         gates.getBlock(gatePos).get("open") == "false");

  // All 1,168 vanilla blocks & 29,873 states basic invariants.
  assert(blockRegistry().size() == 610);
  assert(registeredStateCount() >= 29873);
  assert(Blocks::AIR.index() == 0 && Blocks::AIR.firstState() == 0);
  assert(Blocks::FIREFLY_BUSH.index() == 1167);
  assert(Blocks::FIREFLY_BUSH.firstState() + Blocks::FIREFLY_BUSH.stateCount() == 29873);
  assert(blockRegistry().front().name == "minecraft:air");
  for (const Block *b : {&Blocks::FIRE, &Blocks::REDSTONE_WIRE, &Blocks::OAK_STAIRS,
                         &Blocks::STONE_SLAB, &Blocks::SNOW,
                         &Blocks::CHISELED_BOOKSHELF, &Blocks::COPPER_BULB}) {
    for (const BlockState &s : b->states()) {
      assert(s.isSchemaState());
      assert(s.isRandomlyTicking().has_value());
    }
  }
  assert(collisionShape(Blocks::SNOW.defaultState().with("layers", "1")).empty());
  assert(!blocksMotion(Blocks::SNOW.defaultState().with("layers", "1")));
  assert(collisionShape(Blocks::SNOW.defaultState().with("layers", "2")).front().y1 == 2.0);
  assert(collisionShape(Blocks::SOUL_SAND.defaultState()).front().y1 == 14.0);
  assert(collisionShape(Blocks::OAK_SLAB.defaultState().with("type", "bottom")).front().y1 == 8.0);
  assert(collisionShape(Blocks::OAK_SLAB.defaultState().with("type", "top")).front().y0 == 8.0);
  assert(collisionShape(Blocks::OAK_SLAB.defaultState().with("type", "top")).front().y1 == 16.0);
  assert(collisionShape(Blocks::OAK_SLAB.defaultState().with("type", "double")).front().y0 == 0.0);
  assert(collisionShape(Blocks::OAK_SLAB.defaultState().with("type", "double")).front().y1 == 16.0);
  assert(collisionShape(Blocks::COBWEB.defaultState()).empty());
  assert(blocksMotion(Blocks::COBWEB.defaultState()));

  // Generalized basic block behaviors.
  {
    World bw;
    bw.loadChunk(0, 0);
    // WallBlock + waterlogged
    bw.rawSetBlock({4, 65, 4}, Blocks::BRICK_WALL.defaultState().with("waterlogged", "true"));
    bw.setBlock({5, 65, 4}, Blocks::STONE.defaultState());
    assert(bw.getBlock({4, 65, 4}).getView("east") == "low");
    assert(!bw.fluidTicks().empty());
    // FenceGateBlock in_wall
    bw.setBlock({3, 65, 4}, Blocks::SPRUCE_FENCE_GATE.defaultState());
    assert(bw.getBlock({3, 65, 4}).getView("in_wall") == "true");
    // CarpetBlock survival
    bw.rawSetBlock({8, 64, 8}, Blocks::STONE.defaultState());
    bw.rawSetBlock({8, 65, 8}, Blocks::MOSS_CARPET.defaultState());
    bw.setBlock({8, 64, 8}, Blocks::AIR.defaultState());
    assert(bw.getBlock({8, 65, 8}).is(Blocks::AIR));
    // SnowyBlock
    bw.rawSetBlock({10, 64, 10}, Blocks::MYCELIUM.defaultState());
    bw.setBlock({10, 65, 10}, Blocks::SNOW.defaultState());
    assert(bw.getBlock({10, 64, 10}).getView("snowy") == "true");
    bw.setBlock({10, 65, 10}, Blocks::AIR.defaultState());
    assert(bw.getBlock({10, 64, 10}).getView("snowy") == "false");
    // FallingBlock
    bw.setBlock({12, 65, 12}, Blocks::WHITE_CONCRETE_POWDER.defaultState());
    assert(bw.hasScheduledTick({12, 65, 12}, Blocks::WHITE_CONCRETE_POWDER));
    // SimpleWaterloggedBlock
    bw.rawSetBlock({14, 65, 14}, Blocks::OAK_SLAB.defaultState().with("waterlogged", "true"));
    const auto slabUpdated = bw.updateFromNeighbourShapes({14, 65, 14}, bw.getBlock({14, 65, 14}));
    assert(slabUpdated.getView("waterlogged") == "true");
    // NoteBlock instrument lookup
    for (const auto &[below, inst] : std::initializer_list<std::pair<const Block *, const char *>>{
             {&Blocks::SOUL_SAND, "cow_bell"},
             {&Blocks::BONE_BLOCK, "xylophone"},
             {&Blocks::EMERALD_BLOCK, "bit"},
             {&Blocks::PUMPKIN, "didgeridoo"},
             {&Blocks::HAY_BLOCK, "banjo"},
             {&Blocks::CLAY, "flute"},
             {&Blocks::PACKED_ICE, "chime"},
             {&Blocks::GLOWSTONE, "pling"},
             {&Blocks::COPPER_BLOCK, "trumpet"}}) {
      bw.rawSetBlock({2, 64, 2}, below->defaultState());
      assert(bw.updateFromNeighbourShapes({2, 65, 2}, Blocks::NOTE_BLOCK.defaultState())
                 .getView("instrument") == inst);
    }
    bw.rawSetBlock({2, 64, 2}, Blocks::AIR.defaultState());
    bw.rawSetBlock({2, 66, 2}, Blocks::ZOMBIE_HEAD.defaultState());
    assert(bw.updateFromNeighbourShapes({2, 65, 2}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == "zombie");
    bw.rawSetBlock({2, 66, 2}, Blocks::SKELETON_SKULL.defaultState());
    assert(bw.updateFromNeighbourShapes({2, 65, 2}, Blocks::NOTE_BLOCK.defaultState())
               .getView("instrument") == "skeleton");

    // BlockBehaviours per-block function pointers bound at registration
    assert(Blocks::OBSERVER.behaviours().onPlace != nullptr &&
           Blocks::OBSERVER.behaviours().affectNeighborsAfterRemoval != nullptr &&
           Blocks::OBSERVER.behaviours().updateShape != nullptr &&
           Blocks::OBSERVER.behaviours().tick != nullptr);
    assert(Blocks::REDSTONE_LAMP.behaviours().neighborChanged != nullptr &&
           Blocks::REDSTONE_LAMP.behaviours().tick != nullptr);
    assert(Blocks::NOTE_BLOCK.behaviours().neighborChanged != nullptr &&
           Blocks::NOTE_BLOCK.behaviours().updateShape != nullptr &&
           Blocks::NOTE_BLOCK.behaviours().triggerEvent != nullptr);
    assert(Blocks::BRICK_WALL.behaviours().updateShape != nullptr);
    assert(Blocks::GRASS_BLOCK.behaviours().randomTick != nullptr &&
           Blocks::GRASS_BLOCK.behaviours().updateShape != nullptr);
    assert(Blocks::WHITE_CONCRETE_POWDER.behaviours().tick != nullptr);
    assert(Blocks::STONE.behaviours().neighborChanged == nullptr &&
           Blocks::STONE.behaviours().tick == nullptr &&
           Blocks::STONE.behaviours().randomTick == nullptr);

    // Generalized FallingBlock scheduled tick execution through Block::tick
    bw.tick();
    bw.tick();
    assert(bw.getBlock({12, 65, 12}).is(Blocks::AIR));
  }

  // World-level random ticks: Lithium minisection counting, sparse vs dense,
  // 8->16 palette widening, load/unload, entityTickPositionReady gating,
  // and WorldRandomTickAccess (lighting, fluid, snow tag, LavaFluid dispatch).
  {
    World rw;
    rw.setSeed(42);
    rw.loadChunk(0, 0);

    // 1. Empty chunk & randomTickSpeed == 0 are O(1) skipped.
    rw.rawSetBlock({1, 64, 1}, Blocks::STONE.defaultState());
    rw.randomTickSpeed = 100;
    rw.tick();
    assert(rw.randomTickStats.chunksVisited == 0);
    assert(rw.randomTickStats.sectionsVisited == 0);

    // 2. Minisection boundaries: 0, 247 (mini 0), 248 (mini 1), 4095 (mini 16).
    const auto indexPos = [](int idx) {
      return Pos{idx & 15, (idx >> 8) & 15, (idx >> 4) & 15};
    };
    rw.setBlock(indexPos(0), Blocks::GRASS_BLOCK.defaultState());
    rw.setBlock(indexPos(247), Blocks::GRASS_BLOCK.defaultState());
    rw.setBlock(indexPos(248), Blocks::GRASS_BLOCK.defaultState());
    rw.setBlock(indexPos(4095), Blocks::GRASS_BLOCK.defaultState());
    assert(rw.sectionTickingBlockCountForTest(0, 0, 0) == 4);
    const auto minis0 = rw.sectionRandomTickMinisectionsForTest(0, 0, 0);
    assert(minis0[0] == 2 && minis0[1] == 1 && minis0[16] == 1);

    rw.setBlock(indexPos(247), Blocks::STONE.defaultState());
    assert(rw.sectionTickingBlockCountForTest(0, 0, 0) == 3);
    assert(rw.sectionRandomTickMinisectionsForTest(0, 0, 0)[0] == 1);

    // 3. randomTickSpeed == 0 skips even when ticking blocks exist.
    rw.randomTickStats.reset();
    rw.randomTickSpeed = 0;
    rw.tick();
    assert(rw.randomTickStats.chunksVisited == 0);

    // 4. Unload removes chunk from randomTickingChunks_; reload restores it.
    rw.unloadChunk(0, 0);
    rw.randomTickSpeed = 10;
    rw.tick();
    assert(rw.randomTickStats.chunksVisited == 0);
    rw.loadChunk(0, 0);

    // 5. entityTickPositionReady gates chunk random ticking.
    bool allowEntityTick = false;
    rw.entityTickPositionReady = [&](Pos) { return allowEntityTick; };
    rw.tick();
    assert(rw.randomTickStats.chunksVisited == 0);
    allowEntityTick = true;
    rw.tick();
    assert(rw.randomTickStats.chunksVisited == 1);
    assert(rw.randomTickStats.sectionsSparsePath == 1);
    rw.entityTickPositionReady = {};

    // 6. 8->16 bit palette widening preserves ticking counts & sparse lookup.
    std::vector<BlockState> wideStates;
    for (const Block *b : {&Blocks::OAK_STAIRS, &Blocks::BIRCH_STAIRS,
                           &Blocks::SPRUCE_STAIRS, &Blocks::JUNGLE_STAIRS}) {
      for (const auto s : b->states())
        wideStates.push_back(s);
    }
    assert(wideStates.size() > 260);
    int placedWide = 0;
    for (int idx = 1; idx < 270 && placedWide < 260; ++idx) {
      if (idx == 248)
        continue;
      rw.rawSetBlock(indexPos(idx), wideStates[static_cast<std::size_t>(placedWide++)]);
    }
    assert(rw.sectionTickingBlockCountForTest(0, 0, 0) == 3);
    const auto minisWide = rw.sectionRandomTickMinisectionsForTest(0, 0, 0);
    assert(minisWide[0] == 1 && minisWide[1] == 1 && minisWide[16] == 1);
  }

  {
    // 7. WorldRandomTickAccess + LightService + Water decay + Grass snowy spread.
    World gw;
    gw.setSeed(77);
    gw.loadChunk(0, 0);
    gw.rawSetBlock({4, 64, 4}, Blocks::GRASS_BLOCK.defaultState());
    gw.rawSetBlock({4, 65, 4}, Blocks::WATER.defaultState());
    gw.randomTickSpeed = 400;
    for (int t = 0; t < 200 && gw.getBlock({4, 64, 4}).is(Blocks::GRASS_BLOCK); ++t)
      gw.tick();
    assert(gw.getBlock({4, 64, 4}).is(Blocks::DIRT));
    assert(gw.sectionTickingBlockCountForTest(0, 4, 0) == 0);

    // Grass spread with live LightService daytime sky light & #minecraft:snow.
    gw.enableLighting();
    gw.rawSetBlock({8, 64, 8}, Blocks::GRASS_BLOCK.defaultState());
    gw.rawSetBlock({7, 64, 8}, Blocks::DIRT.defaultState());
    gw.rawSetBlock({7, 65, 8}, Blocks::SNOW.defaultState()); // layers=1
    gw.rawSetBlock({9, 64, 8}, Blocks::DIRT.defaultState());
    gw.refreshLightSnapshots();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!gw.rawBrightness({8, 65, 8}).has_value() &&
           std::chrono::steady_clock::now() < deadline) {
      gw.drainLightPublications();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto bright = gw.rawBrightness({8, 65, 8});
    assert(bright.has_value() && *bright >= 9);

    gw.randomTickStats.reset();
    gw.randomTickSpeed = 500;
    for (int t = 0;
         t < 200 && (!gw.getBlock({7, 64, 8}).is(Blocks::GRASS_BLOCK) ||
                     !gw.getBlock({9, 64, 8}).is(Blocks::GRASS_BLOCK));
         ++t) {
      gw.tick();
    }
    assert(gw.getBlock({7, 64, 8}).is(Blocks::GRASS_BLOCK));
    assert(gw.getBlock({7, 64, 8}).getView("snowy") == "true");
    assert(gw.getBlock({9, 64, 8}).is(Blocks::GRASS_BLOCK));
    assert(gw.getBlock({9, 64, 8}).getView("snowy") == "false");
    assert(gw.randomTickStats.sectionsSparsePath > 0);
    assert(gw.randomTickStats.ticksApplied > 0);
  }

  {
    // 8. Dense path (> 384 ticking blocks in section) and LavaFluid dispatch.
    World dw;
    dw.setSeed(99);
    dw.loadChunk(0, 0);
    int placed = 0;
    for (int y = 64; y <= 65 && placed < 450; ++y) {
      for (int z = 0; z < 16 && placed < 450; ++z) {
        for (int x = 0; x < 16 && placed < 450; ++x) {
          dw.rawSetBlock({x, y, z}, Blocks::GRASS_BLOCK.defaultState());
          ++placed;
        }
      }
    }
    assert(dw.sectionTickingBlockCountForTest(0, 4, 0) == 450);
    dw.randomTickSpeed = 3;
    dw.tick();
    assert(dw.randomTickStats.sectionsDensePath == 1);
    assert(dw.randomTickStats.sectionsSparsePath == 0);
    assert(dw.randomTickStats.positionsProbed == 3);

    // Dense chunk unloaded + new Lava section triggers both block & fluid ticks.
    dw.unloadChunk(0, 0);
    World lw;
    lw.setSeed(11);
    lw.loadChunk(0, 0);
    lw.rawSetBlock({1, 64, 1}, Blocks::LAVA.defaultState());
    lw.randomTickSpeed = 4000;
    lw.tick();
    assert(lw.randomTickStats.blocksTicked > 0);
    assert(lw.randomTickStats.fluidsTicked == lw.randomTickStats.blocksTicked);
    assert(lw.randomTickStats.ticksApplied ==
           lw.randomTickStats.blocksTicked * 2);
    assert(lw.randomTickStats.ticksUnsupported == 0);

    // LavaFluid fire spread activates when player is within spread radius.
    {
      World fw;
      fw.setSeed(7);
      fw.loadChunk(0, 0);
      fw.rawSetBlock({8, 64, 8}, Blocks::LAVA.defaultState());
      fw.rawSetBlock({8, 65, 8}, Blocks::OAK_PLANKS.defaultState());
      fw.rawSetBlock({9, 64, 8}, Blocks::OAK_PLANKS.defaultState());
      fw.enableLighting();
      fw.lightService()->waitIdleForTesting();
      fw.lightService()->drainPublished();
      fw.fireSpreadRadiusAroundPlayer = 128; // no players -> no fire
      fw.randomTickSpeed = 4096;
      fw.tick();
      assert(!fw.getBlock({8, 66, 8}).is(Blocks::FIRE));
      fw.fireSpreadPlayerPositions.push_back({8.0, 64.0, 8.0});
      for (int i = 0; i < 8; ++i)
        fw.tick();
      bool litFire = fw.getBlock({8, 66, 8}).is(Blocks::FIRE) ||
                     fw.getBlock({9, 65, 8}).is(Blocks::FIRE);
      assert(litFire);
    }

    // SnowLayer & Ice randomTick (LightLayer.BLOCK) + Redstone Ore + Dried Ghast + Mushroom solidRender.
    {
      World sw;
      sw.setSeed(21);
      sw.loadChunk(0, 0);
      sw.rawSetBlock({4, 63, 4}, Blocks::STONE.defaultState());
      sw.rawSetBlock({4, 64, 4}, Blocks::SNOW.defaultState());
      sw.rawSetBlock({6, 64, 6}, Blocks::ICE.defaultState());
      sw.rawSetBlock({6, 64, 7}, Blocks::GLOWSTONE.defaultState()); // emission 15 -> adjacent 14 > 11 - 1
      sw.rawSetBlock({10, 64, 10}, Blocks::REDSTONE_ORE.defaultState().with("lit", "true"));
      sw.rawSetBlock({12, 64, 12}, Blocks::DRIED_GHAST.defaultState().with("waterlogged", "true"));
      sw.enableLighting();
      sw.lightService()->waitIdleForTesting();
      sw.lightService()->drainPublished();
      sw.randomTickSpeed = 4096;
      for (int i = 0; i < 4; ++i)
        sw.tick();
      assert(sw.getBlock({4, 64, 4}).is(Blocks::SNOW));
      assert(sw.getBlock({6, 64, 6}).is(Blocks::WATER));
      assert(sw.getBlock({10, 64, 10}).is(Blocks::REDSTONE_ORE) &&
             sw.getBlock({10, 64, 10}).getView("lit") == "false");
      assert(sw.hasScheduledTick({12, 64, 12}, "minecraft:dried_ghast"));

      // MushroomBlock mayPlaceOn requires solidRender (STONE yes, OAK_SLAB bottom no).
      World mw;
      mw.setSeed(5);
      mw.loadChunk(0, 0);
      mw.rawSetBlock({8, 64, 8}, Blocks::BROWN_MUSHROOM.defaultState());
      for (int x = 2; x <= 14; ++x)
        for (int z = 2; z <= 14; ++z) {
          mw.rawSetBlock({x, 66, z}, Blocks::STONE.defaultState());
          mw.rawSetBlock({x, 63, z}, Blocks::OAK_SLAB.defaultState());
        }
      mw.rawSetBlock({8, 63, 8}, Blocks::STONE.defaultState());
      mw.rawSetBlock({9, 63, 8}, Blocks::STONE.defaultState());
      mw.enableLighting();
      mw.lightService()->waitIdleForTesting();
      mw.lightService()->drainPublished();
      mw.randomTickSpeed = 4096;
      bool grewOnStone = false;
      for (int i = 0; i < 600 && !grewOnStone; ++i) {
        mw.tick();
        if (mw.getBlock({9, 64, 8}).is(Blocks::BROWN_MUSHROOM))
          grewOnStone = true;
        for (int x = 3; x <= 13; ++x)
          for (int z = 3; z <= 13; ++z)
            if (!(x == 8 && z == 8) && !(x == 9 && z == 8))
              assert(!mw.getBlock({x, 64, z}).is(Blocks::BROWN_MUSHROOM));
      }
      assert(grewOnStone);
    }

    // Vine, Bamboo, BuddingAmethyst, CaveVines, Kelp, Copper, Crops, PointedDripstone.
    {
      World pw;
      pw.setSeed(33);
      pw.loadChunk(0, 0);
      // Vine attached on stone column facing south, spreads down / faces.
      pw.rawSetBlock({2, 66, 2}, Blocks::STONE.defaultState());
      pw.rawSetBlock({2, 66, 3}, Blocks::VINE.defaultState().with("north", "true"));
      // Bamboo stalk with light.
      pw.rawSetBlock({5, 63, 5}, Blocks::DIRT.defaultState());
      pw.rawSetBlock({5, 64, 5}, Blocks::BAMBOO_SAPLING.defaultState());
      // Budding amethyst.
      pw.rawSetBlock({9, 64, 9}, Blocks::BUDDING_AMETHYST.defaultState());
      // Kelp in water column.
      pw.rawSetBlock({12, 63, 3}, Blocks::SAND.defaultState());
      pw.rawSetBlock({12, 64, 3}, Blocks::KELP.defaultState());
      pw.rawSetBlock({12, 65, 3}, Blocks::WATER.defaultState());
      pw.rawSetBlock({12, 66, 3}, Blocks::WATER.defaultState());
      // Cave vines in air column.
      pw.rawSetBlock({14, 70, 3}, Blocks::STONE.defaultState());
      pw.rawSetBlock({14, 69, 3}, Blocks::CAVE_VINES.defaultState());
      // Pointed dripstone under water source + dripstone_block -> grows stalactite & fills cauldron.
      pw.rawSetBlock({3, 72, 12}, Blocks::WATER.defaultState());
      pw.rawSetBlock({3, 71, 12}, Blocks::DRIPSTONE_BLOCK.defaultState());
      pw.rawSetBlock({3, 70, 12}, Blocks::POINTED_DRIPSTONE.defaultState()
                                         .with("vertical_direction", "down")
                                         .with("thickness", "tip"));
      pw.rawSetBlock({3, 66, 12}, Blocks::CAULDRON.defaultState());
      // Farmland + wheat + torchflower + pitcher crop + pumpkin stem.
      pw.rawSetBlock({8, 63, 12}, Blocks::WATER.defaultState());
      pw.rawSetBlock({9, 63, 12}, Blocks::FARMLAND.defaultState().with("moisture", "0"));
      pw.rawSetBlock({9, 64, 12}, Blocks::WHEAT.defaultState().with("age", "0"));
      pw.rawSetBlock({10, 63, 12}, Blocks::FARMLAND.defaultState().with("moisture", "7"));
      pw.rawSetBlock({10, 64, 12}, Blocks::TORCHFLOWER_CROP.defaultState().with("age", "1"));
      pw.rawSetBlock({11, 63, 12}, Blocks::FARMLAND.defaultState().with("moisture", "7"));
      pw.rawSetBlock({11, 64, 12}, Blocks::PITCHER_CROP.defaultState().with("age", "2").with("half", "lower"));
      pw.rawSetBlock({13, 63, 12}, Blocks::FARMLAND.defaultState().with("moisture", "7"));
      pw.rawSetBlock({13, 64, 12}, Blocks::PUMPKIN_STEM.defaultState().with("age", "7"));
      pw.rawSetBlock({12, 63, 12}, Blocks::DIRT.defaultState());
      pw.rawSetBlock({14, 63, 12}, Blocks::DIRT.defaultState());
      pw.rawSetBlock({13, 63, 11}, Blocks::DIRT.defaultState());
      pw.rawSetBlock({13, 63, 13}, Blocks::DIRT.defaultState());
      // Weathering copper slab preserves properties.
      pw.rawSetBlock({1, 64, 14}, Blocks::CUT_COPPER_SLAB.defaultState().with("type", "top"));

      pw.enableLighting();
      pw.lightService()->waitIdleForTesting();
      pw.lightService()->drainPublished();
      pw.randomTickSpeed = 4096;
      for (int i = 0; i < 300; ++i)
        pw.tick();

      assert(pw.getBlock({2, 65, 3}).is(Blocks::VINE));
      assert(pw.getBlock({5, 65, 5}).is(Blocks::BAMBOO));
      bool crystalGrew = false;
      for (const Pos d : {Pos{0, 1, 0}, Pos{0, -1, 0}, Pos{1, 0, 0}, Pos{-1, 0, 0},
                          Pos{0, 0, 1}, Pos{0, 0, -1}}) {
        const BlockState cs = pw.getBlock({9 + d.x, 64 + d.y, 9 + d.z});
        if (cs.is(Blocks::SMALL_AMETHYST_BUD) ||
            cs.is(Blocks::MEDIUM_AMETHYST_BUD) ||
            cs.is(Blocks::LARGE_AMETHYST_BUD) ||
            cs.is(Blocks::AMETHYST_CLUSTER)) {
          crystalGrew = true;
        }
      }
      assert(crystalGrew);
      assert(pw.getBlock({12, 65, 3}).is(Blocks::KELP));
      assert(pw.getBlock({14, 68, 3}).is(Blocks::CAVE_VINES));
      assert(pw.getBlock({9, 63, 12}).getView("moisture") == "7");
      assert(pw.getBlock({10, 64, 12}).is(Blocks::TORCHFLOWER));
      assert(pw.getBlock({11, 65, 12}).is(Blocks::PITCHER_CROP) &&
             pw.getBlock({11, 65, 12}).getView("half") == "upper");
      assert(pw.getBlock({13, 64, 12}).is(Blocks::ATTACHED_PUMPKIN_STEM));
      const BlockState copperSlab = pw.getBlock({1, 64, 14});
      assert((copperSlab.is(Blocks::EXPOSED_CUT_COPPER_SLAB) ||
              copperSlab.is(Blocks::WEATHERED_CUT_COPPER_SLAB) ||
              copperSlab.is(Blocks::OXIDIZED_CUT_COPPER_SLAB)) &&
             copperSlab.getView("type") == "top");

      // Audited supported() upgrades vs partial blocks intentionally kept false.
      for (const Block *b : {&Blocks::MYCELIUM, &Blocks::BUDDING_AMETHYST,
                             &Blocks::COPPER_BLOCK, &Blocks::EXPOSED_COPPER,
                             &Blocks::WEATHERED_COPPER, &Blocks::OXIDIZED_COPPER,
                             &Blocks::CUT_COPPER, &Blocks::EXPOSED_CUT_COPPER,
                             &Blocks::WEATHERED_CUT_COPPER, &Blocks::OXIDIZED_CUT_COPPER,
                             &Blocks::CHISELED_COPPER, &Blocks::EXPOSED_CHISELED_COPPER,
                             &Blocks::WEATHERED_CHISELED_COPPER, &Blocks::OXIDIZED_CHISELED_COPPER,
                             &Blocks::CUT_COPPER_SLAB, &Blocks::OAK_SLAB,
                             &Blocks::OAK_STAIRS, &Blocks::OAK_FENCE,
                             &Blocks::IRON_BARS, &Blocks::WHITE_GLAZED_TERRACOTTA,
                             &Blocks::QUARTZ_PILLAR, &Blocks::IRON_CHAIN,
                             &Blocks::COPPER_GRATE, &Blocks::MOSS_CARPET}) {
        assert(b->supported());
      }
      for (const Block *b : {&Blocks::SNOW, &Blocks::ICE,
                             &Blocks::OAK_LEAVES, &Blocks::OAK_SAPLING,
                             &Blocks::KELP, &Blocks::KELP_PLANT, &Blocks::FARMLAND}) {
        assert(!b->supported());
      }
    }
  }
  // Compact packed scheduled ticks: coordinate round-trip, deduplication, and
  // drain/periodic compaction preserve exact order and remaining ticks.
  {
    const int extremeChunkX = -1875000;
    const int extremeChunkZ = 1875000;
    const Pos negExtreme{extremeChunkX * 16, -2048, extremeChunkZ * 16 + 15};
    const Pos posExtreme{extremeChunkX * 16 + 15, 2047, extremeChunkZ * 16};
    {
      World twLow(-2048, -1664, 1024);
      twLow.loadChunk(extremeChunkX, extremeChunkZ);
      twLow.scheduleTick(
          ScheduledTick{negExtreme, Blocks::SMOOTH_STONE.name(), 5, -2, -10});
      assert(twLow.hasScheduledTick(negExtreme, Blocks::SMOOTH_STONE.name()));
      const auto snap = twLow.snapshotChunkTicks(extremeChunkX, extremeChunkZ);
      assert(snap.blockTicks.size() == 1 &&
             snap.blockTicks[0].pos == negExtreme &&
             snap.blockTicks[0].priority == -2 &&
             snap.blockTicks[0].order == -10);
    }
    World tw(1664, 2048, 1024);
    tw.loadChunk(extremeChunkX, extremeChunkZ);
    tw.loadChunk(0, 0);
    tw.scheduleTick(
        ScheduledTick{posExtreme, Blocks::SMOOTH_STONE.name(), 5, 1, -5});
    assert(tw.hasScheduledTick(posExtreme, Blocks::SMOOTH_STONE.name()));
    const auto snap = tw.snapshotChunkTicks(extremeChunkX, extremeChunkZ);
    assert(snap.blockTicks.size() == 1 &&
           snap.blockTicks[0].pos == posExtreme &&
           snap.blockTicks[0].priority == 1 && snap.blockTicks[0].order == -5);

    // Burst 400 ticks due at t=2 plus 3 future ticks due at t=150 across 120 ticks.
    for (int i = 0; i < 400; ++i) {
      const Pos p{i % 16, 1664 + (i / 16) % 16, (i / 256) % 16};
      tw.scheduleTick(ScheduledTick{p, Blocks::SMOOTH_STONE.name(), 2, 0, i});
    }
    tw.scheduleTick(ScheduledTick{{0, 1700, 0}, Blocks::SMOOTH_STONE.name(), 150, 1, 20});
    tw.scheduleTick(ScheduledTick{{1, 1700, 0}, Blocks::SMOOTH_STONE.name(), 150, -1, 30});
    tw.scheduleTick(ScheduledTick{{2, 1700, 0}, Blocks::SMOOTH_STONE.name(), 150, -1, 10});
    assert(tw.scheduledTickCount() == 404);
    for (int i = 0; i < 120; ++i)
      tw.tick();
    assert(tw.scheduledTickCount() == 3);
    const auto remaining = tw.scheduledTicks();
    assert(remaining.size() == 3);
    assert(remaining[0].pos == (Pos{2, 1700, 0}));
    assert(remaining[1].pos == (Pos{1, 1700, 0}));
    assert(remaining[2].pos == (Pos{0, 1700, 0}));
    const auto taken = tw.takeChunkTicks(0, 0);
    assert(taken.blockTicks.size() == 3 && tw.scheduledTickCount() == 0);
  }
}
