#include "redstone/entity.hpp"
#include "redstone/litematic.hpp"
#include <cassert>
#include <iostream>

using namespace redstone;
using nbt::Tag;
static Tag integer(int value) {
  Tag t;
  t.type = 3;
  t.number = value;
  return t;
}
static Tag longTag(int64_t value) {
  Tag t;
  t.type = 4;
  t.number = value;
  return t;
}
static Tag string(const std::string &value) {
  Tag t;
  t.type = 8;
  t.text = value;
  return t;
}
static Tag compound(std::map<std::string, Tag> values) {
  Tag t;
  t.type = 10;
  t.compound = std::move(values);
  return t;
}
static Tag list(std::vector<Tag> values) {
  Tag t;
  t.type = 9;
  t.list = std::move(values);
  return t;
}
static Tag vector(int x, int y, int z) {
  return compound({{"x", integer(x)}, {"y", integer(y)}, {"z", integer(z)}});
}
static Tag real(double value) {
  Tag t;
  t.type = 6;
  t.real = value;
  return t;
}
static Tag fixture() {
  Tag words;
  words.type = 12;
  words.longs.resize(2);
  for (unsigned i = 0; i < 31; ++i) {
    const auto id = std::uint64_t(i == 21 ? 3 : i % 5);
    const auto bit = i * 3u, word = bit / 64, shift = bit % 64;
    words.longs[word] =
        std::int64_t(std::uint64_t(words.longs[word]) | (id << shift));
    if (shift + 3 > 64)
      words.longs[word + 1] = std::int64_t(
          std::uint64_t(words.longs[word + 1]) | (id >> (64 - shift)));
  }
  Tag uuid;
  uuid.type = 11;
  uuid.ints = {0, 0, 0, 1};
  auto cart = compound({{"id", string("minecraft:minecart")},
                        {"UUID", uuid},
                        {"Pos", list({real(-3.5), real(0), real(.5)})}});
  auto palette =
      list({compound({{"Name", string("minecraft:air")}}),
            compound({{"Name", string("minecraft:anvil")}}),
            compound({{"Name", string("minecraft:smooth_stone")}}),
            compound({{"Name", string("minecraft:sand")}}),
            compound({{"Name", string("minecraft:birch_wall_sign")}})});
  auto region = compound(
      {{"Position", vector(10, 0, 0)},
       {"Size", vector(-31, 1, 1)},
       {"BlockStatePalette", palette},
       {"BlockStates", words},
       {"Entities", list({cart, cart})},
       {"TileEntities", list({compound({{"id", string("minecraft:sign")}})})},
       {"PendingBlockTicks",
        list({compound({{"x", integer(1)},
                        {"y", integer(0)},
                        {"z", integer(0)},
                        {"Block", string("minecraft:sand")},
                        {"Time", integer(2)},
                        {"Priority", integer(-1)}})})},
       // PendingFluidTicks stores the registry ID under Fluid, not Block.
       {"PendingFluidTicks",
        list({compound({{"x", integer(2)},
                        {"y", integer(0)},
                        {"z", integer(0)},
                        {"Fluid", string("minecraft:flowing_water")},
                        {"Time", integer(5)},
                        {"Priority", integer(-2)},
                        {"SubTick", longTag(7)}}),
               compound({{"x", integer(3)},
                         {"y", integer(0)},
                         {"z", integer(0)},
                         {"Fluid", string("minecraft:water")},
                         {"Time", integer(-4)},
                         {"Priority", integer(1)},
                         {"SubTick", longTag(3)}})})}});
  return compound({{"Version", integer(6)},
                   {"SubVersion", integer(1)},
                   {"MinecraftDataVersion", integer(20060210)},
                   {"Regions", compound({{"one", region}})}});
}
template <class F> static void rejects(F action) {
  bool rejected = false;
  try {
    action();
  } catch (const nbt::Error &) {
    rejected = true;
  }
  assert(rejected);
}
int main(int argc, char **argv) {
  auto doc = fixture();
  auto loaded = loadLitematic(doc);
  assert(loaded.regionCount == 1 && loaded.replacedAnvils == 5 &&
         loaded.skippedSigns == 6);
  assert(loaded.skippedSignBlockEntities == 1 &&
         loaded.regeneratedEntityUuids == 2);
  assert(loaded.min == (Pos{-20, 64, 0}) &&
         loaded.maxExclusive == (Pos{11, 65, 1}));
  assert(loaded.world.blocks.size() == 18 && loaded.world.chunks.size() == 3);
  assert(loaded.world.blocks.at({1, 64, 0}).name() ==
         "minecraft:sand"); // index 21 has set bits in both longs.
  assert(loaded.world.entities[0].get("Pos")->list[0].real ==
         6.5); // relative to start, not min.
  assert(loaded.world.entities[0].get("UUID")->ints !=
         loaded.world.entities[1].get("UUID")->ints);
  assert(loaded.world.ticks[0].pos == (Pos{-19, 64, 0}));
  // A-04: the fluid ticks keep the projection offset, their exact identity and
  // the schematic's own SubTick ordering key.
  assert(loaded.world.ticks.size() == 3);
  assert(loaded.world.ticks[1].fluid && loaded.world.ticks[2].fluid);
  assert(loaded.world.ticks[1].pos == (Pos{-18, 64, 0}));
  assert(loaded.world.ticks[1].type == "minecraft:flowing_water");
  assert(loaded.world.ticks[1].delay == 5 &&
         loaded.world.ticks[1].priority == -2);
  assert(loaded.world.ticks[1].subTick == 7);
  // Negative relative delay, positive priority and a second identity.
  assert(loaded.world.ticks[2].pos == (Pos{-17, 64, 0}));
  assert(loaded.world.ticks[2].type == "minecraft:water");
  assert(loaded.world.ticks[2].delay == -4 &&
         loaded.world.ticks[2].priority == 1);
  assert(loaded.world.ticks[2].subTick == 3);
  // The block tick has no SubTick in the fixture, so it uses the vanilla
  // per-chunk -N..-1 sequence instead of a schematic key.
  assert(!loaded.world.ticks[0].subTick.has_value());
  World world;
  importWorld(loaded.world, world);
  assert(world.entities.size() == 2 && world.scheduledTickCount() == 1);
  assert(world.fluidTickCount() == 2);
  assert(world.hasFluidTick({-18, 64, 0}, FluidRef(Fluids::flowingWater())));
  // Exact identity again: the source entry must not match a flowing one.
  assert(!world.hasFluidTick({-18, 64, 0}, FluidRef(Fluids::water())));
  assert(world.hasFluidTick({-17, 64, 0}, FluidRef(Fluids::water())));
  // SubTick is the ordering key, distinct from the vanilla -N..-1 rule.
  {
    std::map<Pos, std::int64_t> orders;
    for (const auto &entry : world.fluidTicks())
      orders[entry.first.pos] = entry.first.order;
    assert(orders.at({-18, 64, 0}) == 7);
    assert(orders.at({-17, 64, 0}) == 3);
  }
  {
    // Hold delay and priority equal so only SubTick can reverse list order.
    auto tied = doc;
    auto &ticks = tied.compound.at("Regions").compound.at("one")
                      .compound.at("PendingFluidTicks").list;
    for (auto &tick : ticks) {
      tick.compound.at("Fluid") = string("minecraft:water");
      tick.compound.at("Time") = integer(1);
      tick.compound.at("Priority") = integer(0);
    }
    World ordered;
    importWorld(loadLitematic(tied).world, ordered);
    const Pos firstInFile{-18, 64, 0};
    const Pos secondInFile{-17, 64, 0};
    for (const auto pos : {firstInFile, secondInFile}) {
      ordered.rawSetBlock(pos, Blocks::WATER.defaultState());
      for (auto direction : {Direction::Down, Direction::North, Direction::South,
                             Direction::West, Direction::East}) {
        const auto neighbor = pos.relative(direction);
        if (neighbor != firstInFile && neighbor != secondInFile)
          ordered.rawSetBlock(neighbor, Blocks::STONE.defaultState());
      }
    }
    ordered.traceUpdates = true;
    ordered.tick();
    std::vector<Pos> dispatched;
    for (const auto &entry : ordered.log)
      if (entry.kind == "fluid_tick")
        dispatched.push_back(entry.pos);
    assert((dispatched == std::vector<Pos>{secondInFile, firstInFile}));
    assert(ordered.fluidTickCount() == 0);
  }
  // Snapshot import preserves unsupported/suspended physics until a saved tick
  // or a real world update reaches it; no onPlace callbacks are invented.
  assert(!world.hasScheduledTick({-17, 64, 0}, "minecraft:sand"));
  auto modern = doc;
  modern.compound["Version"] = integer(7);
  modern.compound["MinecraftDataVersion"] = integer(4435);
  const auto modernLoaded = loadLitematic(modern);
  assert(modernLoaded.world.blocks == loaded.world.blocks);
  assert(modernLoaded.min == loaded.min &&
         modernLoaded.maxExclusive == loaded.maxExclusive);
  assert(modernLoaded.world.entities.size() == 2 &&
         modernLoaded.world.ticks[0].pos == loaded.world.ticks[0].pos);
  modern.compound["Version"] = integer(8);
  rejects([&] { loadLitematic(modern); });
  modern = doc;
  modern.compound["SubVersion"] = integer(2);
  rejects([&] { loadLitematic(modern); });
  // Thousands of disjoint one-block subregions, as used by the v7 projection.
  auto large = doc;
  large.compound["Version"] = integer(7);
  auto &regions = large.compound["Regions"].compound;
  regions.clear();
  Tag singleWord;
  singleWord.type = 12;
  singleWord.longs = {0};
  for (int i = 0; i < 4262; ++i)
    regions.emplace(std::to_string(i), compound({
        {"Position", vector(i % 100, 0, i / 100)},
        {"Size", vector(1, 1, 1)},
        {"BlockStatePalette", list({compound({
            {"Name", string("minecraft:smooth_stone")}})})},
        {"BlockStates", singleWord}}));
  const auto many = loadLitematic(large);
  assert(many.regionCount == 4262 && many.world.blocks.size() == 4262);
  assert(many.world.blocks.at({61, 64, 42}).name() == "minecraft:smooth_stone");
  // A-04: an unknown fluid name in PendingFluidTicks is a data error by
  // default and is only skipped when substitution is explicitly allowed --
  // and then it is REPORTED, never dropped silently.
  {
    auto unknownFluid = doc;
    auto &ticks = unknownFluid.compound["Regions"]
                       .compound["one"]
                       .compound["PendingFluidTicks"]
                       .list;
    ticks[0].compound["Fluid"].text = "minecraft:not_a_fluid";
    rejects([&] { loadLitematic(unknownFluid); });
    LitematicOptions lenient;
    lenient.substituteUnsupported = true;
    const auto skipped = loadLitematic(unknownFluid, lenient);
    // The known tick and the block tick survive; the unknown one is counted.
    assert(skipped.world.ticks.size() == 2);
    assert(skipped.skippedUnsupportedFluidTicks.size() == 1);
    assert(skipped.skippedUnsupportedFluidTicks.at(
               "minecraft:not_a_fluid") == 1);
  }
  // An empty fluid name is structurally invalid and is rejected even leniently.
  {
    auto emptyName = doc;
    emptyName.compound["Regions"]
        .compound["one"]
        .compound["PendingFluidTicks"]
        .list[0]
        .compound["Fluid"].text = "";
    rejects([&] { loadLitematic(emptyName); });
    LitematicOptions lenient;
    lenient.substituteUnsupported = true;
    rejects([&] { loadLitematic(emptyName, lenient); });
  }
  // A explicit minecraft:empty is a real identity and imports as one.
  {
    auto explicitEmpty = doc;
    explicitEmpty.compound["Regions"]
        .compound["one"]
        .compound["PendingFluidTicks"]
        .list[0]
        .compound["Fluid"].text = "minecraft:empty";
    const auto imported = loadLitematic(explicitEmpty);
    assert(imported.world.ticks[1].type == "minecraft:empty");
    World emptyWorld;
    importWorld(imported.world, emptyWorld);
    assert(emptyWorld.hasFluidTick({-18, 64, 0}, FluidRef(Fluids::empty())));
  }
  // A record missing the "Fluid" key entirely is rejected, not defaulted.
  {
    auto missing = doc;
    missing.compound["Regions"]
        .compound["one"]
        .compound["PendingFluidTicks"]
        .list[0]
        .compound.erase("Fluid");
    rejects([&] { loadLitematic(missing); });
  }
  auto malformed = doc;
  malformed.compound["Regions"]
      .compound["one"]
      .compound["BlockStates"]
      .longs.pop_back();
  rejects([&] { loadLitematic(malformed); });
  malformed = doc;
  malformed.compound["Regions"]
      .compound["one"]
      .compound["BlockStates"]
      .longs[0] |= 7;
  rejects([&] { loadLitematic(malformed); });
  malformed = doc;
  malformed.compound["Regions"].compound["one"].compound["Size"] =
      vector(0, 1, 1);
  rejects([&] { loadLitematic(malformed); });
  malformed = doc;
  malformed.compound["Regions"].compound["copy"] =
      malformed.compound["Regions"].compound["one"];
  rejects([&] { loadLitematic(malformed); });
  malformed = doc;
  malformed.compound["Regions"]
      .compound["one"]
      .compound["BlockStatePalette"]
      .list[2]
      .compound["Name"] = string("minecraft:diamond_block");
  rejects([&] { loadLitematic(malformed); });
  LitematicOptions options;
  options.offset.y = 320;
  rejects([&] { loadLitematic(doc, options); });
  options.offset.y = 64;
  options.validateState = [](const BlockState &s) {
    if (s.properties().contains("invalid"))
      throw nbt::Error("invalid property");
  };
  malformed = doc;
  malformed.compound["Regions"]
      .compound["one"]
      .compound["BlockStatePalette"]
      .list[2]
      .compound["Properties"] = compound({{"invalid", string("yes")}});
  rejects([&] { loadLitematic(malformed, options); });
  if (argc == 2) {
    auto actual = loadLitematic(std::filesystem::path(argv[1]));
    assert(actual.regionCount == 410 && actual.world.blocks.size() == 70111);
    assert(actual.replacedAnvils == 28764 && actual.skippedSigns == 1);
    assert(actual.world.entities.size() == 133 &&
           actual.world.chunks.size() == 2500);
    World actualWorld;
    importWorld(actual.world, actualWorld);
    assert(actualWorld.entities.size() == 133);
    std::cout << "Actual schematic: 70111 blocks, 133 entities, 2500 chunks\n";
  }
  std::cout << "litematic snapshot, packed state, coordinate and identity "
               "tests passed\n";
}
