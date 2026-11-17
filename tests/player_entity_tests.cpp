#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include "redstone/piston.hpp"
#include "redstone/portal.hpp"
#include "redstone/redstone.hpp"
#include "redstone/server_worlds.hpp"
#include <cassert>
#include <cmath>
#include <vector>

using namespace redstone;

namespace {

void loadFlatArea(World &world, int minCx = -1, int maxCx = 2, int minCz = -1,
                  int maxCz = 2, int floorY = 63) {
  for (int cx = minCx; cx <= maxCx; ++cx) {
    for (int cz = minCz; cz <= maxCz; ++cz) {
      world.loadChunk(cx, cz);
      for (int lx = 0; lx < 16; ++lx) {
        for (int lz = 0; lz < 16; ++lz) {
          world.setBlock({cx * 16 + lx, floorY, cz * 16 + lz},
                         defaultState("stone"), 2);
        }
      }
    }
  }
}

void testPressurePlatesAndRedstoneOre() {
  World world;
  loadFlatArea(world);

  const Pos woodPlate{2, 64, 2};
  const Pos stonePlate{4, 64, 2};
  const Pos orePos{6, 63, 2};
  world.setBlock(woodPlate, defaultState("oak_pressure_plate"), 3);
  world.setBlock(stonePlate, defaultState("stone_pressure_plate"), 3);
  world.setBlock(orePos, defaultState("redstone_ore"), 3);

  auto player = std::make_shared<ServerPlayer>(world, Vec3{0.5, 64.0, 0.5}, "P");
  world.addEntity(player);

  // Move onto oak pressure plate.
  player->applyClientMovement(world, {2.5, 64.0, 2.5}, 0.0f, 0.0f, true);
  assert(world.getBlock(woodPlate).getView("powered") == "true");

  // Move onto stone pressure plate (must also trigger for LivingEntity/ServerPlayer).
  player->applyClientMovement(world, {4.5, 64.0, 2.5}, 0.0f, 0.0f, true);
  assert(world.getBlock(stonePlate).getView("powered") == "true");

  // Sneaking over redstone_ore must NOT light it.
  player->setShiftKeyDown(true);
  player->applyClientMovement(world, {6.5, 64.0, 2.5}, 0.0f, 0.0f, true);
  assert(world.getBlock(orePos).getView("lit") == "false");

  // Standing up and stepping on redstone_ore MUST light it.
  player->setShiftKeyDown(false);
  player->applyClientMovement(world, {6.5, 64.0, 2.5}, 0.0f, 0.0f, true);
  assert(world.getBlock(orePos).getView("lit") == "true");
}

void testTripwireHooksActivation() {
  World world;
  loadFlatArea(world);

  world.setBlock({1, 64, 5}, defaultState("stone"), 3);
  world.setBlock({5, 64, 5}, defaultState("stone"), 3);
  world.setBlock(
      {2, 64, 5},
      defaultState("tripwire_hook").with("facing", "east").with("attached", "true"),
      3);
  world.setBlock(
      {4, 64, 5},
      defaultState("tripwire_hook").with("facing", "west").with("attached", "true"),
      3);
  world.setBlock({3, 64, 5},
                 defaultState("tripwire")
                     .with("attached", "true")
                     .with("east", "true")
                     .with("west", "true"),
                 3);
  for (int i = 0; i < 4; ++i)
    world.tick();

  auto player = std::make_shared<ServerPlayer>(world, Vec3{0.5, 64.0, 0.5}, "P");
  world.addEntity(player);
  player->applyClientMovement(world, {3.5, 64.0, 5.5}, 0.0f, 0.0f, true);
  assert(world.getBlock({3, 64, 5}).getView("powered") == "true");
  assert(world.getBlock({2, 64, 5}).getView("powered") == "true");
  assert(world.getBlock({4, 64, 5}).getView("powered") == "true");

  // Move off tripwire and advance scheduled check (10 ticks).
  player->applyClientMovement(world, {0.5, 64.0, 0.5}, 0.0f, 0.0f, true);
  for (int i = 0; i < 12; ++i)
    world.tick();
  assert(world.getBlock({3, 64, 5}).getView("powered") == "false");
}

void testItemAndXpPickup() {
  World world;
  loadFlatArea(world);

  auto player = std::make_shared<ServerPlayer>(world, Vec3{2.5, 64.0, 2.5}, "P");
  int pickedAmount = 0;
  player->onItemPickup = [&](const ItemStack &s) {
    pickedAmount += s.count;
    return s.count;
  };
  world.addEntity(player);

  std::vector<std::tuple<int, int, int>> taken;
  world.entityTakenObserver = [&](int collectorId, int targetId, int amount) {
    taken.emplace_back(collectorId, targetId, amount);
  };

  auto item = std::make_shared<ItemEntity>(
      world, Vec3{2.6, 64.0, 2.5}, itemStack("minecraft:redstone", 12));
  item->pickupDelay = 0;
  world.addEntity(item);
  auto orb = std::make_shared<ExperienceOrb>(world, Vec3{2.5, 64.0, 2.6}, 7);
  world.addEntity(orb);

  world.tick();
  assert(pickedAmount == 12);
  assert(player->totalExperience == 7);
  assert(item->isRemoved());
  assert(orb->isRemoved());
  assert(taken.size() == 2);
}

void testPistonPushAndRidingDetectorRail() {
  World world;
  loadFlatArea(world);

  // Piston at (4,64,4) facing east with redstone block.
  world.setBlock({3, 64, 4}, defaultState("redstone_block"), 3);
  world.setBlock({4, 64, 4},
                 defaultState("piston").with("facing", "east"), 3);
  auto player = std::make_shared<ServerPlayer>(world, Vec3{5.5, 64.0, 4.5}, "P");
  world.addEntity(player);

  for (int i = 0; i < 6; ++i)
    world.tick();
  assert(player->position.x > 6.0);

  // Place detector rail + minecart, ride it, verify powered=true, then dismount.
  const Pos railPos{8, 64, 8};
  world.setBlock(railPos, defaultState("detector_rail"), 3);
  auto cart = std::make_shared<Minecart>(world, Vec3{8.5, 64.0625, 8.5});
  world.addEntity(cart);
  assert(player->startRiding(*cart));
  for (int i = 0; i < 12; ++i)
    world.tick();
  assert(world.getBlock(railPos).getView("powered") == "true");
  player->stopRiding();
  assert(!player->isPassenger());
}

void testBedSleepSkipsNight() {
  World world;
  loadFlatArea(world);
  world.setBlock({3, 64, 3},
                 defaultState("red_bed").with("part", "head"), 3);
  auto player = std::make_shared<ServerPlayer>(world, Vec3{3.5, 64.0, 3.5}, "P");
  world.addEntity(player);

  int skippedCount = 0;
  world.nightSkippedObserver = [&]() { ++skippedCount; };
  assert(player->sleepInBedAt(world, {3, 64, 3}));
  assert(player->isSleeping());
  for (int i = 0; i < ServerPlayer::kSleepDurationTicks; ++i)
    world.tick();
  assert(skippedCount == 1);
  assert(!player->isSleeping());
}

void testPortalDimensionTransfer() {
  ServerWorldSet set;
  auto owLevel = std::make_unique<DimensionLevel>();
  owLevel->stem = {"minecraft:overworld", "minecraft:overworld",
                   overworldDimensionType()};
  owLevel->world = std::make_unique<World>();
  owLevel->world->setDimension(owLevel->stem.type, owLevel->stem.id);
  World *overworld = owLevel->world.get();

  auto netherLevel = std::make_unique<DimensionLevel>();
  netherLevel->stem = {"minecraft:the_nether", "minecraft:the_nether",
                       netherDimensionType()};
  netherLevel->world = std::make_unique<World>(
      netherDimensionType(), 1000000, "minecraft:the_nether");
  World *nether = netherLevel->world.get();

  set.levels().push_back(std::move(owLevel));
  set.levels().push_back(std::move(netherLevel));

  loadFlatArea(*overworld, -1, 1, -1, 1, 63);
  loadFlatArea(*nether, -1, 1, -1, 1, 63);
  for (int y = 63; y <= 67; ++y) {
    overworld->setBlock({1, y, 2}, defaultState("obsidian"), 3);
    overworld->setBlock({4, y, 2}, defaultState("obsidian"), 3);
  }
  for (int x = 1; x <= 4; ++x) {
    overworld->setBlock({x, 63, 2}, defaultState("obsidian"), 3);
    overworld->setBlock({x, 67, 2}, defaultState("obsidian"), 3);
  }
  assert(overworld->setBlock({2, 64, 2}, Blocks::FIRE.defaultState(), 3));
  for (int y = 63; y <= 67; ++y) {
    nether->setBlock({-1, y, 0}, defaultState("obsidian"), 3);
    nether->setBlock({2, y, 0}, defaultState("obsidian"), 3);
  }
  for (int x = -1; x <= 2; ++x) {
    nether->setBlock({x, 63, 0}, defaultState("obsidian"), 3);
    nether->setBlock({x, 67, 0}, defaultState("obsidian"), 3);
  }
  assert(nether->setBlock({0, 64, 0}, Blocks::FIRE.defaultState(), 3));

  auto player = std::make_shared<ServerPlayer>(*overworld, Vec3{2.5, 64.0, 2.5},
                                               "PortalTester");
  player->creative = false;
  overworld->addEntity(player);
  player->applyClientMovement(*overworld, {2.5, 64.0, 2.5}, 0.0f, 0.0f, true);
  for (int i = 0; i < 85; ++i) {
    set.tick(true, true);
    bool inNether = false;
    for (const auto &e : nether->entities)
      if (e.get() == player.get())
        inNether = true;
    if (inNether)
      break;
  }
  bool foundInNether = false;
  bool foundInOverworld = false;
  for (const auto &e : nether->entities)
    if (e.get() == player.get())
      foundInNether = true;
  for (const auto &e : overworld->entities)
    if (e.get() == player.get())
      foundInOverworld = true;
  assert(foundInNether && !foundInOverworld);
}

} // namespace

int main() {
  testPressurePlatesAndRedstoneOre();
  testTripwireHooksActivation();
  testItemAndXpPickup();
  testPistonPushAndRidingDetectorRail();
  testBedSleepSkipsNight();
  testPortalDimensionTransfer();
  return 0;
}
