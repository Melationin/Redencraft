#include "redstone/entity.hpp"
#include "redstone/piston.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <tuple>
using namespace redstone;
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
bool close(double a, double b) { return std::abs(a - b) < 1.0e-6; }
void setup(World &w) {
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      w.loadChunk(x, z);
}
BlockState block(const char *n) { return defaultState(n); }
BlockState piston(bool sticky = false) {
  return block(sticky ? "sticky_piston" : "piston").with("facing", "east");
}
bool is(World &w, Pos p, const char *n) {
  return w.getBlock(p).name() == std::string("minecraft:") + n;
}
void put(World &w, Pos p, const BlockState &s) { w.rawSetBlock(p, s); }
void run(World &w, int n) {
  while (n--)
    w.tick();
}
void carpet() {
  World w;
  setup(w);
  put(w, {1, 0, 0}, block("red_carpet"));
  PistonStructureResolver push(w, {0, 0, 0}, Direction::East, true);
  require(push.resolve() && push.toPush().size() == 1 &&
              push.toDestroy().empty(),
          "wool carpet has normal push reaction");
  PistonStructureResolver pull(w, {-1, 0, 0}, Direction::East, false);
  require(pull.resolve() && pull.toPush().size() == 1 &&
              pull.toDestroy().empty(),
          "sticky piston can pull carpet");
}
void carpetDuplicator() {
  World w;
  setup(w);
  // Coordinates and tick counts match validation/vanilla26_1/run_validation.py.
  put(w, {0, 100, 0}, piston(true));
  put(w, {1, 100, 0}, block("slime_block"));
  put(w, {1, 101, 0}, block("slime_block"));
  w.setBlock({-1, 100, 0}, block("redstone_block"));
  run(w, 3);
  require(is(w, {1, 100, 0}, "piston_head") && is(w, {2, 101, 0}, "slime_block"),
          "duper extended setup");
  w.setBlock({1, 101, 0}, block("white_carpet"));
  w.setBlock({-1, 100, 0}, block("air"));
  run(w, 5);
  require(is(w, {0, 101, 0}, "white_carpet"), "duper moves original carpet");
  require(is(w,{1,100,0},"slime_block")&&is(w,{1,101,0},"slime_block"),"duper slime positions match vanilla server");
  int drops = 0;
  for (const auto &entry : w.log)
    if (entry.kind == "entity_spawn" &&
        entry.detail == "minecraft:item:minecraft:white_carpet")
      ++drops;
  require(drops == 1, "duper also emits one carpet item");
  int entities = 0;
  ItemEntity *carpetItem = nullptr;
  for (const auto &entity : w.entities)
    if (auto *item = dynamic_cast<ItemEntity *>(entity.get());
        item && item->item.id == "minecraft:white_carpet" &&
        item->item.count == 1) {
      ++entities;
      carpetItem = item;
    }
  require(entities == 1, "duper carpet drop is a real item entity");
  // The real 26.1 probe recorded in validation/vanilla26_1 shows the same
  // post-hit sequence (-1, -0.98, -0.9604, ... on X). The absolute tick index of
  // the hit depends on when the server processes the queued retraction event,
  // so only the direction/launch is asserted here; slimePistonBounce() covers
  // the exact one-tick launch and the first -1.0 velocity.
  require(carpetItem && carpetItem->deltaMovement.x < -0.8 &&
              carpetItem->position.x < 0.0,
          "duper carpet item is launched westward by the retracting slime");
}
void slimePistonBounce() {
  World w;
  setup(w);
  Pos p{2, 0, 0};
  put(w, p, block("moving_piston").with("facing", "east"));
  auto entity =
      std::make_shared<PistonMovingBlockEntity>(PistonMovingBlockEntity{
          p, block("slime_block"), Direction::East, false, false});
  w.setPistonEntity(p, entity);
  auto item = std::make_shared<ItemEntity>(
      w, Vec3{2.5, 0.5, 0.5}, itemStack("minecraft:white_carpet", 1),
      Vec3{0, 0, 0});
  w.addEntity(item);
  pistonTickBlockEntities(w);
  require(close(item->deltaMovement.x, -1.0),
          "retracting slime block launches an item to the west");
  require(item->position.x < 2.5 && item->position.x > 1.9,
          "item is pushed by the moving slime block");
  w.tick();
  require(item->position.x < 1.99,
          "item keeps the slime velocity on the next entity tick");
}
void movingPistonStageOrder() {
  World w;
  setup(w);
  Pos p{2, 0, 0};
  put(w, p, block("moving_piston").with("facing", "east"));
  auto entity =
      std::make_shared<PistonMovingBlockEntity>(PistonMovingBlockEntity{
          p, block("slime_block"), Direction::East, false, false});
  w.setPistonEntity(p, entity);
  auto item = std::make_shared<ItemEntity>(
      w, Vec3{2.5, 0.5, 0.5}, itemStack("minecraft:white_carpet", 1),
      Vec3{0, 0, 0});
  w.addEntity(item);
  // If the block entity ran before the entity phase, the item would already
  // have velocity -1 when its own tick executed and would end near x=0.99.
  // The correct order updates the item's gravity first, then pushes it once.
  w.tick();
  require(close(item->deltaMovement.x, -1.0),
          "slime launch velocity is assigned in the block-entity stage");
  require(item->position.x > 2.3 && item->position.x < 2.4,
          "entity tick precedes the moving-piston block-entity tick");
  require(item->position.y < 0.5,
          "item gravity ticked before the moving-piston push");
}
void extension() {
  World w;
  setup(w);
  put(w, {0, 0, 0}, piston());
  put(w, {1, 0, 0}, block("smooth_stone"));
  put(w, {0, 0, -1}, block("redstone_block"));
  pistonNeighborChanged(w, {0, 0, 0}, w.getBlock({0, 0, 0}));
  run(w, 1);
  require(is(w, {1, 0, 0}, "moving_piston") &&
              is(w, {2, 0, 0}, "moving_piston"),
          "extension creates moving blocks");
  run(w, 2);
  require(is(w, {1, 0, 0}, "piston_head") && is(w, {2, 0, 0}, "smooth_stone"),
          "extension finishes after three ticks");
  w.setBlock({0, 0, -1}, block("air"));
  run(w, 3);
  require(is(w, {0, 0, 0}, "piston") &&
              w.getBlock({0, 0, 0}).get("extended") == "false",
          "retraction restores base");
  require(is(w, {1, 0, 0}, "air") && is(w, {2, 0, 0}, "smooth_stone"),
          "ordinary piston leaves pushed block");
}
void pulse() {
  World w;
  setup(w);
  put(w, {0, 0, 0}, piston(true));
  put(w, {1, 0, 0}, block("smooth_stone"));
  put(w, {0, 0, -1}, block("redstone_block"));
  pistonNeighborChanged(w, {0, 0, 0}, w.getBlock({0, 0, 0}));
  run(w, 1);
  w.setBlock({0, 0, -1}, block("air"));
  auto pendingEvents = w.pendingBlockEvents();
  require(!pendingEvents.empty() && pendingEvents.back().type == 2,
          "short pulse chooses drop event");
  run(w, 4);
  require(is(w, {1, 0, 0}, "air") && is(w, {2, 0, 0}, "smooth_stone"),
          "short pulse drops pushed block");
}
void limits() {
  for (int count : {12, 13}) {
    World w;
    setup(w);
    for (int i = 1; i <= count; ++i)
      put(w, {i, 0, 0}, block("smooth_stone"));
    PistonStructureResolver r(w, {0, 0, 0}, Direction::East, true);
    require(r.resolve() == (count == 12), "12/13 push limit");
  }
}
void adhesion() {
  World w;
  setup(w);
  put(w, {1, 0, 0}, block("slime_block"));
  put(w, {1, 1, 0}, block("smooth_stone"));
  put(w, {1, -1, 0}, block("honey_block"));
  PistonStructureResolver r(w, {0, 0, 0}, Direction::East, true);
  require(r.resolve(), "adhesion resolves");
  bool stone = false, honey = false;
  for (auto p : r.toPush()) {
    stone |= p == Pos{1, 1, 0};
    honey |= p == Pos{1, -1, 0};
  }
  require(stone && !honey, "slime sticks to stone but not honey");
}
void quasi() {
  World w;
  setup(w);
  put(w, {0, 0, 0}, piston());
  put(w, {0, 2, 0}, block("redstone_block"));
  pistonNeighborChanged(w, {0, 0, 0}, w.getBlock({0, 0, 0}));
  auto pendingEvents = w.pendingBlockEvents();
  require(!pendingEvents.empty() && pendingEvents[0].type == 0,
          "quasi-connectivity powers piston");
}
void head() {
  World w;
  setup(w);
  auto base = piston().with("extended", "true");
  auto h = block("piston_head").with("facing", "east");
  put(w, {0, 0, 0}, base);
  put(w, {1, 0, 0}, h);
  w.setBlock({0, 0, 0}, block("air"), 3);
  require(is(w, {1, 0, 0}, "air"), "orphaned head removed");
  World v;
  setup(v);
  put(v, {0, 0, 0}, base);
  put(v, {1, 0, 0}, h);
  v.setBlock({1, 0, 0}, block("air"), 3);
  require(is(v, {0, 0, 0}, "air"), "head removal removes base");
}
void registrationOrder() {
  World w;
  w.traceUpdates = true;
  setup(w);
  Pos first{8, 0, 0}, second{2, 0, 0};
  for (Pos p : {first, second}) {
    put(w, p, block("moving_piston"));
    auto entity =
        std::make_shared<PistonMovingBlockEntity>(PistonMovingBlockEntity{
            p, block("smooth_stone"), Direction::East, true, false});
    entity->progress = entity->progressO = 1;
    w.setPistonEntity(p, entity);
  }
  w.log.clear();
  pistonTickBlockEntities(w);
  std::vector<Pos> notifications;
  for (const auto &entry : w.log)
    if (entry.kind == "neighbor" && entry.detail == "minecraft:smooth_stone" &&
        (entry.pos == first || entry.pos == second))
      notifications.push_back(entry.pos);
  require(notifications.size() == 2 && notifications[0] == first &&
              notifications[1] == second,
          "moving entities finish in registration order");
}
void unloadedEntity() {
  World w;
  setup(w);
  Pos p{2, 0, 0};
  put(w, p, block("moving_piston"));
  auto entity =
      std::make_shared<PistonMovingBlockEntity>(PistonMovingBlockEntity{
          p, block("smooth_stone"), Direction::East, true, false});
  w.setPistonEntity(p, entity);
  w.unloadChunk(0, 0);
  pistonTickBlockEntities(w);
  require(entity->progress == 0, "unloaded moving block remains frozen");
  w.loadChunk(0, 0);
  pistonTickBlockEntities(w);
  require(entity->progress == 0.5f, "reloaded moving block resumes");
}
void heightBoundaries() {
  World w(0, 8);
  setup(w);
  for (auto [dir, edge, interior] :
       {std::tuple{Direction::Up, Pos{0, 7, 0}, Pos{0, 6, 0}},
        std::tuple{Direction::Down, Pos{0, 0, 0}, Pos{0, 1, 0}}}) {
    require(pistonIsPushable(w, edge, block("air"), dir, false, dir),
            "boundary air is pushable");
    require(!pistonIsPushable(w, edge, block("smooth_stone"), dir, false, dir),
            "occupied boundary cannot be pushed outward");
    put(w, interior, block("smooth_stone"));
    auto origin = interior.relative(opposite(dir));
    PistonStructureResolver resolver(w, origin, dir, true);
    require(resolver.resolve(), "resolver can move block into boundary air");
  }
}
void moveNotificationOrder() {
  World w;
  setup(w);
  w.traceUpdates = true;
  auto base = piston();
  put(w, {0, 0, 0}, base);
  put(w, {0, 0, -1}, block("redstone_block"));
  put(w, {1, 0, 0}, block("smooth_stone"));
  put(w, {2, 0, 0}, block("smooth_stone"));
  put(w, {3, 0, 0}, block("dead_tube_coral_fan"));
  require(pistonBlockEvent(w, {0, 0, 0}, base, 0,
                           static_cast<int>(Direction::East)),
          "extension event succeeds");
  std::vector<std::string> phases;
  for (const auto &e : w.log) {
    if (e.kind != "neighbor" || e.pos.z != -1 || e.pos.y != 0)
      continue;
    if (e.pos.x == 3 && e.detail == "minecraft:dead_tube_coral_fan")
      phases.push_back("destroy");
    if (e.pos.x == 2 && e.detail == "minecraft:smooth_stone")
      phases.push_back("far");
    if (e.pos.x == 1 && e.detail == "minecraft:smooth_stone")
      phases.push_back("near");
    if (e.pos.x == 1 &&
        (e.detail == "piston_head" || e.detail == "minecraft:piston_head"))
      phases.push_back("head");
  }
  require(phases == std::vector<std::string>{"destroy", "far", "near", "head"},
          "destroy, pushed sources, then piston head notifications");
}
void completionStages() {
  World w;
  setup(w);
  w.traceUpdates = true;
  Pos p{4, 0, 0};
  put(w, p, block("moving_piston"));
  auto entity =
      std::make_shared<PistonMovingBlockEntity>(PistonMovingBlockEntity{
          p, block("smooth_stone"), Direction::East, true, false});
  w.setPistonEntity(p, entity);
  w.setGameTime(7);
  pistonTickBlockEntities(w);
  require(entity->lastTicked == 7 && entity->progressO == 0 &&
              entity->progress == 0.5f,
          "first tick records prior progress and tick time");
  w.setGameTime(8);
  pistonTickBlockEntities(w);
  require(entity->lastTicked == 8 && entity->progressO == 0.5f &&
              entity->progress == 1,
          "second tick reaches progress one without finalization");
  require(w.pistonEntities.contains(p), "entity persists through progress one");
  w.log.clear();
  w.setGameTime(9);
  pistonTickBlockEntities(w);
  require(entity->lastTicked == 9 && entity->progressO == 1 &&
              !w.pistonEntities.contains(p),
          "third tick removes completed entity");
  std::vector<std::string> stages;
  for (const auto &entry : w.log)
    if (entry.pos == p && entry.kind.starts_with("piston_"))
      stages.push_back(entry.kind);
  require(stages == std::vector<std::string>{"piston_entity_remove",
                                             "piston_shape_resolve",
                                             "piston_set_block",
                                             "piston_self_notify"},
          "completion lifecycle order");
  require(is(w, p, "smooth_stone"),
          "completed state installed before self notification");
}
void tickerRebindingAndPending() {
  World w;
  setup(w);
  Pos first{8, 0, 0}, rebound{2, 0, 0}, removed{3, 0, 0}, added{4, 0, 0};
  auto make = [&](Pos p) {
    put(w, p, block("moving_piston"));
    return std::make_shared<PistonMovingBlockEntity>(PistonMovingBlockEntity{
        p, block("smooth_stone"), Direction::East, true, false});
  };
  auto completing = make(first), stale = make(rebound),
       staleRemoved = make(removed);
  auto replacement = make(rebound), afterRemoval = make(removed),
       newlyAdded = make(added);
  completing->progress = completing->progressO = 1;
  w.setPistonEntity(first, completing);
  w.setPistonEntity(rebound, stale);
  w.setPistonEntity(removed, staleRemoved);
  w.pistonCompletionObserver = [&](World &world, Pos p) {
    if (p != first)
      return;
    world.setPistonEntity(rebound, replacement);
    world.removePistonEntity(removed);
    world.setPistonEntity(removed, afterRemoval);
    world.setPistonEntity(added, newlyAdded);
  };
  pistonTickBlockEntities(w);
  require(stale->progress == 0 && replacement->progress == 0.5f,
          "live wrapper rebind ticks replacement in existing position");
  require(staleRemoved->progress == 0 && afterRemoval->progress == 0 &&
              newlyAdded->progress == 0,
          "removed ticker and new tickers wait until next pass");
  w.pistonCompletionObserver = {};
  pistonTickBlockEntities(w);
  require(afterRemoval->progress == 0.5f && newlyAdded->progress == 0.5f,
          "pending tickers advance on next pass");
}
void importedFractionalProgressSweep() {
  World w;
  setup(w);
  Pos p{4, 0, 0};
  put(w, p, block("moving_piston"));
  auto moving = std::make_shared<PistonMovingBlockEntity>(
      PistonMovingBlockEntity{p, block("smooth_stone"), Direction::East,
                              true, false});
  moving->progress = 0.75f;
  w.setPistonEntity(p, moving);
  auto item = std::make_shared<ItemEntity>(
      w, Vec3{5.0, 0.25, 0.5}, itemStack("white_carpet"), Vec3{});
  w.addEntity(item);
  pistonTickBlockEntities(w);
  require(std::abs(item->position.x - 5.385) < 1.0e-6,
          "fractional imported progress sweeps a full half block before clamp");
  require(moving->progressO == 0.75f && moving->progress == 1.0f &&
              w.pistonEntities.contains(p),
          "fractional progress is clamped only after pushing and finalizes later");
}
int main() {
  try {
    carpet();
    carpetDuplicator();
    slimePistonBounce();
    movingPistonStageOrder();
    extension();
    pulse();
    limits();
    adhesion();
    quasi();
    head();
    registrationOrder();
    unloadedEntity();
    heightBoundaries();
    moveNotificationOrder();
    completionStages();
    tickerRebindingAndPending();
    importedFractionalProgressSweep();
    std::cout << "piston tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "piston test failed: " << e.what() << '\n';
    return 1;
  }
}
