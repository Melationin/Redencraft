#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
using namespace redstone;
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
    // 800 registered note block states: 25 notes x 16 instruments x 2 powered.
    const std::vector<BlockState> all =
        findBlockByName("minecraft:note_block")->states();
    assert(all.size() == 800);
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
  sectionReadCacheLifecycle();
  inlineSectionStorage();
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
  assert(coral.getBlock({0, 1, 0}).name() == "minecraft:air");
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
}
