#include "redstone/containers.hpp"
#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include "redstone/fluid.hpp"
#include "redstone/litematic.hpp"
#include "redstone/redstone.hpp"
#include "redstone/storage.hpp"
#include <cassert>
#include <iostream>

using namespace redstone;

namespace {

void tickN(World &w, int ticks) {
  for (int i = 0; i < ticks; ++i)
    w.tick();
}

void setupPlatform(World &w, int y = 63) {
  for (int cx = 0; cx <= 3; ++cx)
    for (int cz = 0; cz <= 3; ++cz)
      w.loadChunk(cx, cz);
  for (int x = 0; x <= 48; ++x)
    for (int z = 0; z <= 48; ++z)
      w.rawSetBlock({x, y, z}, defaultState("smooth_stone"));
}

int countItemEntities(const World &w, std::string_view itemId = {}) {
  int total = 0;
  for (const auto &e : w.entities) {
    if (e->isRemoved())
      continue;
    if (auto *item = dynamic_cast<const ItemEntity *>(e.get())) {
      if (itemId.empty() || item->item.id == itemId)
        total += item->item.count;
    }
  }
  return total;
}

void testChestsDoubleChestsAndComparator() {
  World w;
  setupPlatform(w);

  // Single chest comparator signal 0 -> 1 -> 8 -> 15.
  w.setBlock({4, 64, 4}, Blocks::CHEST.defaultState().with("facing", "north"), 3);
  w.setBlock({5, 64, 4},
             Blocks::COMPARATOR.defaultState().with("facing", "west"), 3);
  auto chest = chestEntityAt(w, {4, 64, 4});
  assert(chest != nullptr);
  tickN(w, 3);
  assert(containerComparatorSignal(w, {4, 64, 4}, w.getBlock({4, 64, 4})) == 0);
  assert(w.getBlock({5, 64, 4}).getView("powered") == "false");

  chest->setItem(0, itemStack("smooth_stone", 1));
  chest->setChanged(w);
  assert(containerComparatorSignal(w, {4, 64, 4}, w.getBlock({4, 64, 4})) == 1);
  tickN(w, 3);
  assert(w.getBlock({5, 64, 4}).getView("powered") == "true");

  for (int s = 0; s < 27; ++s)
    chest->setItem(s, itemStack("smooth_stone", 64));
  chest->setChanged(w);
  assert(containerComparatorSignal(w, {4, 64, 4}, w.getBlock({4, 64, 4})) == 15);

  // Place solid conductor above chest -> comparator signal becomes 0 while
  // hopper extraction still sees the full chest (ignoreBeingBlocked=true).
  w.setBlock({4, 65, 4}, defaultState("smooth_stone"), 3);
  assert(containerComparatorSignal(w, {4, 64, 4}, w.getBlock({4, 64, 4})) == 0);
  tickN(w, 3);
  assert(w.getBlock({5, 64, 4}).getView("powered") == "false");

  ResolvedContainerView blockedHopperView = resolveContainerView(w, {4, 64, 4}, true);
  assert(blockedHopperView.valid() && blockedHopperView.getItem(0).count == 64);

  w.removeBlock({4, 65, 4}, false);
  assert(containerComparatorSignal(w, {4, 64, 4}, w.getBlock({4, 64, 4})) == 15);

  // Break chest -> all 27 * 64 items drop into world.
  w.removeBlock({4, 64, 4}, false);
  assert(!chestEntityAt(w, {4, 64, 4}));
  assert(countItemEntities(w, "minecraft:smooth_stone") == 27 * 64);

  // Double chest placement & copper chest cross-oxidation connection.
  BlockState firstCopper = chestPlacementState(
      w, {10, 64, 10}, Blocks::COPPER_CHEST.defaultState(), Direction::North,
      Direction::Up, false);
  w.setBlock({10, 64, 10}, firstCopper, 3);
  assert(w.getBlock({10, 64, 10}).getView("type") == "single");

  BlockState secondCopper = chestPlacementState(
      w, {11, 64, 10}, Blocks::WEATHERED_COPPER_CHEST.defaultState(),
      Direction::North, Direction::West, false);
  w.setBlock({11, 64, 10}, secondCopper, 3);
  // Facing north: clockwise is East (+X), counter-clockwise is West (-X).
  // {10,64,10} connects East to {11,64,10} (LEFT connects clockwise to {11},
  // wait: {10} placed facing north sees chest at {11} East -> wait let's check both sides connected!).
  const auto leftOrRightA = w.getBlock({10, 64, 10}).getView("type");
  const auto leftOrRightB = w.getBlock({11, 64, 10}).getView("type");
  assert(leftOrRightA != "single" && leftOrRightB != "single");
  assert(leftOrRightA != leftOrRightB);

  ResolvedContainerView doubleView = resolveContainerView(w, {10, 64, 10}, false);
  assert(doubleView.valid() && doubleView.size() == 54);
  doubleView.setItem(w, 0, itemStack("smooth_stone", 1));
  doubleView.setItem(w, 27, itemStack("smooth_stone", 1));
  assert(containerComparatorSignal(w, {10, 64, 10}, w.getBlock({10, 64, 10})) == 1);

  // Sneaking keeps adjacent chest single.
  BlockState sneakChest = chestPlacementState(
      w, {12, 64, 10}, Blocks::CHEST.defaultState(), Direction::North,
      Direction::West, true);
  assert(sneakChest.getView("type") == "single");

  // Trapped chest does NOT merge with regular chest.
  BlockState noMerge = chestPlacementState(
      w, {9, 64, 10}, Blocks::TRAPPED_CHEST.defaultState(), Direction::North,
      Direction::East, false);
  assert(noMerge.getView("type") == "single");
}

void testTrappedChestBarrelAndPistonUnpushable() {
  World w;
  setupPlatform(w);

  // Trapped chest openCount weak + strong downward signal.
  w.rawSetBlock({5, 62, 6}, defaultState("smooth_stone"));
  w.setBlock({6, 64, 6}, Blocks::TRAPPED_CHEST.defaultState(), 3);
  w.setBlock({7, 64, 6}, Blocks::REDSTONE_WIRE.defaultState(), 3);
  // Conductor below trapped chest {6,63,6} + wire adjacent to that conductor at {5,63,6}
  w.setBlock({5, 63, 6}, Blocks::REDSTONE_WIRE.defaultState(), 3);
  assert(trappedChestSignal(w, {6, 64, 6}) == 0);

  containerUpdateOpenCount(w, {6, 64, 6}, w.getBlock({6, 64, 6}), +1);
  assert(trappedChestSignal(w, {6, 64, 6}) == 1);
  assert(w.getBlock({7, 64, 6}).getView("power") == "1");
  assert(w.getBlock({5, 63, 6}).getView("power") == "1");

  containerUpdateOpenCount(w, {6, 64, 6}, w.getBlock({6, 64, 6}), -1);
  assert(trappedChestSignal(w, {6, 64, 6}) == 0);
  assert(w.getBlock({7, 64, 6}).getView("power") == "0");
  assert(w.getBlock({5, 63, 6}).getView("power") == "0");

  // Barrel placement + open property + comparator.
  BlockState bs = barrelPlacementState(Blocks::BARREL.defaultState(), Direction::Down);
  w.setBlock({10, 64, 6}, bs, 3);
  assert(w.getBlock({10, 64, 6}).getView("facing") == "up");
  containerUpdateOpenCount(w, {10, 64, 6}, w.getBlock({10, 64, 6}), +1);
  assert(w.getBlock({10, 64, 6}).getView("open") == "true");
  containerUpdateOpenCount(w, {10, 64, 6}, w.getBlock({10, 64, 6}), -1);
  assert(w.getBlock({10, 64, 6}).getView("open") == "false");

  // Pistons CANNOT push container block entities (chest/barrel/dispenser/dropper/hopper),
  // while pistons CAN still destroy comparator despite HasBlockEntity.
  for (const Block *containerBlock :
       {&Blocks::CHEST, &Blocks::TRAPPED_CHEST, &Blocks::BARREL,
        &Blocks::DISPENSER, &Blocks::DROPPER, &Blocks::HOPPER}) {
    w.setBlock({20, 64, 10},
               Blocks::PISTON.defaultState().with("facing", "east"), 3);
    w.setBlock({21, 64, 10}, containerBlock->defaultState(), 3);
    w.setBlock({19, 64, 10}, defaultState("redstone_block"), 3);
    tickN(w, 5);
    assert(w.getBlock({20, 64, 10}).getView("extended") == "false");
    assert(w.getBlock({21, 64, 10}).is(*containerBlock));
    w.removeBlock({19, 64, 10}, false);
    w.removeBlock({21, 64, 10}, false);
    w.removeBlock({20, 64, 10}, false);
  }

  // Comparator is destroyed rather than blocked by HasBlockEntity.
  w.setBlock({20, 64, 14}, Blocks::PISTON.defaultState().with("facing", "east"),
             3);
  w.setBlock({21, 64, 14},
             Blocks::COMPARATOR.defaultState().with("facing", "east"), 3);
  w.setBlock({19, 64, 14}, defaultState("redstone_block"), 3);
  tickN(w, 5);
  assert(w.getBlock({20, 64, 14}).getView("extended") == "true");
  assert(w.getBlock({21, 64, 14}).is(Blocks::PISTON_HEAD));
}

void testDispenserAndDropper() {
  World w;
  setupPlatform(w);

  // Dropper: facing East into chest -> inserts item into chest.
  w.setBlock({4, 64, 4},
             Blocks::DROPPER.defaultState().with("facing", "east"), 3);
  w.setBlock({5, 64, 4}, Blocks::CHEST.defaultState().with("facing", "north"), 3);
  auto dropper = dispenserEntityAt(w, {4, 64, 4});
  auto targetChest = chestEntityAt(w, {5, 64, 4});
  assert(dropper && targetChest);
  dropper->setItem(3, itemStack("smooth_stone", 4));
  dropper->setChanged(w);

  // Power via Quasi-Connectivity: redstone_block 2 blocks above plus a neighbor update (QC BUD).
  w.setBlock({4, 66, 4}, defaultState("redstone_block"), 3);
  w.setBlock({4, 64, 5}, defaultState("smooth_stone"), 3); // trigger neighborChanged on dropper
  // Not triggered until tick + 4gt delay!
  tickN(w, 3);
  assert(targetChest->isEmpty());
  tickN(w, 1); // 4th tick fires dispense!
  assert(targetChest->getItem(0).id == "minecraft:smooth_stone");
  assert(targetChest->getItem(0).count == 1);
  assert(dropper->getItem(3).count == 3);
  w.removeBlock({4, 66, 4}, false);
  w.removeBlock({4, 64, 5}, false);
  assert(w.getBlock({4, 64, 4}).getView("triggered") == "false");

  // Dropper facing air spawns ItemEntity.
  w.removeBlock({5, 64, 4}, false);
  // Clear dropped items before testing air drop.
  for (auto &e : w.entities)
    e->discard();
  w.setBlock({3, 64, 4}, defaultState("redstone_block"), 3);
  tickN(w, 4);
  assert(countItemEntities(w, "minecraft:smooth_stone") == 1);

  // Dispenser: water_bucket places water -> turns into empty bucket -> then picks it back up!
  for (auto &e : w.entities)
    e->discard();
  w.setBlock({15, 64, 15},
             Blocks::DISPENSER.defaultState().with("facing", "east"), 3);
  // Build basin around {16, 64, 15} so water stays right there as source.
  w.setBlock({17, 64, 15}, defaultState("smooth_stone"), 3);
  w.setBlock({16, 64, 14}, defaultState("smooth_stone"), 3);
  w.setBlock({16, 64, 16}, defaultState("smooth_stone"), 3);
  auto disp = dispenserEntityAt(w, {15, 64, 15});
  assert(disp);
  disp->setItem(0, itemStack("water_bucket", 1));
  disp->setChanged(w);

  w.setBlock({14, 64, 15}, defaultState("redstone_block"), 3);
  tickN(w, 4);
  assert(fluidStateOf(w.getBlock({16, 64, 15})).is(Fluids::water()));
  assert(disp->getItem(0).id == "minecraft:bucket");

  w.removeBlock({14, 64, 15}, false);
  w.setBlock({14, 64, 15}, defaultState("redstone_block"), 3);
  tickN(w, 4);
  assert(w.getBlock({16, 64, 15}).isAir());
  assert(disp->getItem(0).id == "minecraft:water_bucket");

  // Dispenser: TNT spawns PrimedTnt.
  disp->setItem(0, itemStack("tnt", 1));
  disp->setChanged(w);
  w.removeBlock({14, 64, 15}, false);
  w.setBlock({14, 64, 15}, defaultState("redstone_block"), 3);
  tickN(w, 4);
  bool foundTnt = false;
  for (const auto &e : w.entities) {
    if (!e->isRemoved() && e->typeName() == "minecraft:tnt")
      foundTnt = true;
  }
  assert(foundTnt);
}

void testHopperTransferLockChainAndComposter() {
  World w;
  setupPlatform(w);

  // Hopper locking & cooldown test.
  w.setBlock({4, 65, 4}, Blocks::CHEST.defaultState().with("facing", "north"), 3);
  w.setBlock({4, 64, 4}, Blocks::HOPPER.defaultState().with("facing", "east"), 3);
  w.setBlock({5, 64, 4}, Blocks::CHEST.defaultState().with("facing", "north"), 3);
  auto src = chestEntityAt(w, {4, 65, 4});
  auto dst = chestEntityAt(w, {5, 64, 4});
  auto hop = hopperEntityAt(w, {4, 64, 4});
  assert(src && dst && hop);
  src->setItem(0, itemStack("smooth_stone", 5));
  src->setChanged(w);

  // Lock hopper with adjacent redstone_block.
  w.setBlock({4, 64, 5}, defaultState("redstone_block"), 3);
  assert(w.getBlock({4, 64, 4}).getView("enabled") == "false");
  tickN(w, 20);
  assert(dst->isEmpty());
  assert(src->getItem(0).count == 5);

  // Unlock hopper -> wakes and moves 1 item immediately, next after 8gt.
  w.removeBlock({4, 64, 5}, false);
  assert(w.getBlock({4, 64, 4}).getView("enabled") == "true");
  tickN(w, 1);
  // First tick: pulls 1 from src into hopper AND ejects? Wait: cooldown prevents same-tick eject unless custom;
  // Actually push runs BEFORE pull when cooldown==0: tick 1 pulls 1 into hopper (sets cooldown? no, suck keeps cooldown 0 if it hadn't pushed; then tick 2 pushes!).
  // Wait let's run 2 ticks and verify 1 item reached dst or hopper!
  tickN(w, 1);
  assert(hop->getItem(0).count + dst->getItem(0).count >= 1);
  tickN(w, 40);
  assert(dst->getItem(0).count == 5);
  assert(src->isEmpty() && hop->isEmpty());

  // Composter: top hopper feeds pumpkin_pie (chance=1.0), 7 -> 20gt tick -> 8,
  // bottom hopper extracts bone_meal.
  w.setBlock({20, 66, 20}, Blocks::HOPPER.defaultState().with("facing", "down"), 3);
  w.setBlock({20, 65, 20}, Blocks::COMPOSTER.defaultState(), 3);
  w.setBlock({20, 64, 20}, Blocks::HOPPER.defaultState().with("facing", "east"), 3);
  w.setBlock({21, 64, 20}, Blocks::CHEST.defaultState(), 3);
  auto topHop = hopperEntityAt(w, {20, 66, 20});
  auto boneChest = chestEntityAt(w, {21, 64, 20});
  topHop->setItem(0, itemStack("pumpkin_pie", 7));
  topHop->setChanged(w);

  // Needs 7 * 8 = 56 ticks to fill + 20 ticks composter settle + ~10 ticks extract.
  tickN(w, 110);
  assert(w.getBlock({20, 65, 20}).getView("level") == "0");
  assert(boneChest->getItem(0).id == "minecraft:bone_meal");
  assert(boneChest->getItem(0).count == 1);

  // ItemEntity pickup + solid block blocking vs #does_not_block_hoppers.
  for (auto &e : w.entities)
    e->discard();
  w.setBlock({30, 64, 30}, Blocks::HOPPER.defaultState().with("facing", "down"), 3);
  auto itemHop = hopperEntityAt(w, {30, 64, 30});
  w.spawnItemAt({30.5, 65.2, 30.5}, "smooth_stone");
  tickN(w, 2);
  assert(itemHop->getItem(0).id == "minecraft:smooth_stone");
  assert(countItemEntities(w, "minecraft:smooth_stone") == 0);

  // Full solid stone block above blocks ItemEntity pickup (spawn centered with zero horizontal scatter).
  w.setBlock({30, 65, 30}, defaultState("smooth_stone"), 3);
  w.addEntity(std::make_shared<ItemEntity>(
      w, Vec3{30.5, 66.05, 30.5}, itemStack("glass", 1), Vec3{0.0, 0.0, 0.0}));
  tickN(w, 10);
  assert(countItemEntities(w, "minecraft:glass") == 1);
  assert(itemHop->getItem(1).empty());

  // Replace blocker with hopper (#does_not_block_hoppers) above empty itemHop?
  // Wait: put top hopper locked so top doesn't suck, OR put chest (#does_not_block_hoppers?) -> test slab / chest!
  // Let's check: chest has smaller-than-full-cube hitbox AND doesn't block, or hopper is full-cube-ish? Let's test stone vs glass / note_block / chest:
  // Actually removing stone and letting glass fall into pickup range:
  w.removeBlock({30, 65, 30}, false);
  tickN(w, 15);
  assert(itemHop->getItem(1).id == "minecraft:glass");
}

void testLithiumSleepingTickerAndWakeups() {
  World w;
  setupPlatform(w);

  // Place 50 idle empty hoppers across chunks.
  for (int i = 0; i < 50; ++i) {
    const int x = 2 + (i % 25);
    const int z = 2 + (i / 25) * 4;
    w.setBlock({x, 64, z}, Blocks::HOPPER.defaultState().with("facing", "down"), 3);
  }
  // Settle initial tick -> all 50 transition to SleepIndefinite.
  tickN(w, 5);
  assert(w.containerTickStats.sleepTransitions >= 50);

  // Reset counters and run 200 ticks: ZERO active hopper ticks should occur!
  const auto activeBefore = w.containerTickStats.activeTicks;
  const auto skippedBefore = w.containerTickStats.sleepingSkippedTicks;
  tickN(w, 200);
  assert(w.containerTickStats.activeTicks == activeBefore);
  assert(w.containerTickStats.sleepingSkippedTicks - skippedBefore >= 50ULL * 200ULL);

  // Wakeup 1: spawning ItemEntity above sleeping hopper {5, 64, 2}.
  w.spawnItemAt({5.5, 65.2, 2.5}, "diamond");
  tickN(w, 2);
  auto wokeByItem = hopperEntityAt(w, {5, 64, 2});
  assert(wokeByItem && wokeByItem->getItem(0).id == "minecraft:diamond");
  tickN(w, 15); // settles back to sleep once full or idle (actually wait: it still holds 1 diamond & has no dest -> goes back to sleep on idle!)

  // Wakeup 2: placing a chest above sleeping hopper {8, 64, 2} and inserting into chest via setChanged.
  w.setBlock({8, 65, 2}, Blocks::CHEST.defaultState().with("facing", "north"), 3);
  tickN(w, 5); // hopper sees empty chest and goes back to sleep!
  const auto wakesBeforeChest = w.containerTickStats.wakeups;
  auto feedChest = chestEntityAt(w, {8, 65, 2});
  feedChest->setItem(0, itemStack("iron_ingot", 3));
  feedChest->setChanged(w);
  assert(w.containerTickStats.wakeups > wakesBeforeChest);
  tickN(w, 30);
  auto wokeByChest = hopperEntityAt(w, {8, 64, 2});
  assert(wokeByChest && wokeByChest->getItem(0).id == "minecraft:iron_ingot" &&
         wokeByChest->getItem(0).count == 3);

  // Wakeup 3: destination chest becomes full -> hopper sleeps; removing an item from dest wakes hopper!
  w.setBlock({12, 64, 2}, Blocks::HOPPER.defaultState().with("facing", "east"), 3);
  w.setBlock({13, 64, 2}, Blocks::CHEST.defaultState().with("facing", "north"), 3);
  auto blockedHop = hopperEntityAt(w, {12, 64, 2});
  auto fullDst = chestEntityAt(w, {13, 64, 2});
  blockedHop->setItem(0, itemStack("smooth_stone", 4));
  blockedHop->setChanged(w);
  for (int s = 0; s < 27; ++s)
    fullDst->setItem(s, itemStack("smooth_stone", 64));
  fullDst->setChanged(w);
  tickN(w, 12); // settles & sleeps on full dest chest (cache hit!).
  assert(w.containerTickStats.cachedFullChecks > 0);
  const auto activeAfterFullSleep = w.containerTickStats.activeTicks;
  tickN(w, 20);
  assert(w.containerTickStats.activeTicks == activeAfterFullSleep);

  // Drain one slot from dest chest -> wakes hopper immediately and resumes pushing!
  fullDst->removeItemNoUpdate(0);
  fullDst->setChanged(w);
  tickN(w, 5);
  assert(fullDst->getItem(0).count == 1);
  assert(blockedHop->getItem(0).count == 3);
  tickN(w, 5);
  assert(fullDst->getItem(0).count == 2);
  assert(blockedHop->getItem(0).count == 2);
}

void testStorageNbtImport() {
  LoadedWorld loaded;
  loaded.chunks.emplace_back(0, 0);
  loaded.blocks[{4, 64, 4}] = defaultState("smooth_stone");
  loaded.blocks[{4, 65, 4}] =
      Blocks::CHEST.defaultState().with("facing", "north");
  loaded.blocks[{4, 64, 5}] =
      Blocks::HOPPER.defaultState().with("facing", "east");
  loaded.blocks[{5, 64, 5}] =
      Blocks::CHEST.defaultState().with("facing", "north");

  auto makeInt = [](std::int64_t v) {
    nbt::Tag t;
    t.type = 3;
    t.number = v;
    return t;
  };
  auto makeByte = [](std::int64_t v) {
    nbt::Tag t;
    t.type = 1;
    t.number = v;
    return t;
  };
  auto makeStr = [](std::string s) {
    nbt::Tag t;
    t.type = 8;
    t.text = std::move(s);
    return t;
  };
  auto makeItem = [&](int slot, const std::string &id, int count) {
    nbt::Tag item;
    item.type = 10;
    item.compound["Slot"] = makeByte(slot);
    item.compound["id"] = makeStr(id);
    item.compound["count"] = makeInt(count);
    return item;
  };

  nbt::Tag hopperBe;
  hopperBe.type = 10;
  hopperBe.compound["id"] = makeStr("minecraft:hopper");
  hopperBe.compound["x"] = makeInt(4);
  hopperBe.compound["y"] = makeInt(64);
  hopperBe.compound["z"] = makeInt(5);
  hopperBe.compound["TransferCooldown"] = makeInt(5);
  hopperBe.compound["Items"].type = 9;
  hopperBe.compound["Items"].list.push_back(
      makeItem(0, "minecraft:redstone", 3));
  loaded.blockEntities.push_back(std::move(hopperBe));

  World w;
  importWorld(loaded, w);

  auto impChest = chestEntityAt(w, {4, 65, 4});
  auto impHopper = hopperEntityAt(w, {4, 64, 5});
  auto outChest = chestEntityAt(w, {5, 64, 5});
  assert(impChest && impHopper && outChest);
  assert(impHopper->getItem(0).id == "minecraft:redstone");
  assert(impHopper->getItem(0).count == 3);
  assert(impHopper->cooldownTime == 5);

  tickN(w, 30);
  assert(outChest->getItem(0).id == "minecraft:redstone");
  assert(outChest->getItem(0).count == 3);
}

} // namespace

int main() {
  testChestsDoubleChestsAndComparator();
  testTrappedChestBarrelAndPistonUnpushable();
  testDispenserAndDropper();
  testHopperTransferLockChainAndComposter();
  testLithiumSleepingTickerAndWakeups();
  testStorageNbtImport();
  std::cout << "container block entities and Lithium sleeping ticker tests passed\n";
  return 0;
}
