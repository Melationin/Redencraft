#include "redstone/entity.hpp"
#include "redstone/piston.hpp"
#include "redstone/shapes.hpp"
#include "redstone/storage.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace redstone;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
bool close(double a, double b, double eps = 1.0e-6) {
  return std::abs(a - b) <= eps;
}
World floorWorld() {
  World world;
  for (int x = -2; x <= 2; ++x)
    for (int z = -2; z <= 2; ++z)
      world.loadChunk(x, z);
  for (int x = -2; x <= 2; ++x)
    for (int z = -2; z <= 2; ++z)
      world.rawSetBlock({x, 63, z}, defaultState("smooth_stone"));
  world.setSeed(12345);
  return world;
}
void run(World &world, int ticks) {
  for (int i = 0; i < ticks; ++i)
    world.tick();
}
ItemEntity *itemAt(World &world, std::size_t index = 0) {
  std::size_t seen = 0;
  for (auto &entity : world.entities)
    if (auto *item = dynamic_cast<ItemEntity *>(entity.get())) {
      if (seen++ == index)
        return item;
    }
  return nullptr;
}
PrimedTnt *tntAt(World &world, std::size_t index = 0) {
  std::size_t seen = 0;
  for (auto &entity : world.entities)
    if (auto *tnt = dynamic_cast<PrimedTnt *>(entity.get())) {
      if (seen++ == index)
        return tnt;
    }
  return nullptr;
}
Minecart *minecartAt(World &world, std::size_t index = 0) {
  std::size_t seen = 0;
  for (auto &entity : world.entities)
    if (auto *cart = dynamic_cast<Minecart *>(entity.get())) {
      if (seen++ == index)
        return cart;
    }
  return nullptr;
}
void itemPhysics() {
  auto world = floorWorld();
  auto item = std::make_shared<ItemEntity>(
      world, Vec3{0.5, 66.0, 0.5}, itemStack("minecraft:white_carpet", 1),
      Vec3{0, 0, 0});
  world.addEntity(item);
  item->pickupDelay = 0;
  run(world, 30);
  require(!item->isRemoved(), "item survives fall");
  require(close(item->position.y, 64.0), "item rests on the full block top");
  require(item->onGround, "item reports onGround");
  require(item->age == 30, "item age advances once per tick");
  // The stationary-on-ground branch in ItemEntity.tick skips move(), so the
  // gravity applied before that branch remains in the velocity until the next
  // 4-tick movement branch. Position must still stay on the floor.
  require(item->position.y >= 64.0 - 1.0e-9,
          "stationary item does not sink through the floor");
  // Carpet is 1/16 high and supplies a supporting collision shape.
  World carpet;
  carpet.loadChunk(0, 0);
  carpet.rawSetBlock({0, 63, 0}, defaultState("smooth_stone"));
  carpet.rawSetBlock({0, 64, 0}, defaultState("white_carpet"));
  auto onCarpet = std::make_shared<ItemEntity>(
      carpet, Vec3{0.5, 66.0, 0.5}, itemStack("minecraft:red_carpet", 1),
      Vec3{0, 0, 0});
  carpet.addEntity(onCarpet);
  for (int i = 0; i < 30; ++i)
    carpet.tick();
  require(close(onCarpet->position.y, 64.0 + 1.0 / 16.0),
          "item rests on carpet height");
}
void itemMerging() {
  auto world = floorWorld();
  auto a = std::make_shared<ItemEntity>(world, Vec3{0.5, 64.0, 0.5},
                                        itemStack("minecraft:smooth_stone", 1),
                                        Vec3{0, 0, 0});
  auto b = std::make_shared<ItemEntity>(world, Vec3{0.7, 64.0, 0.5},
                                        itemStack("minecraft:smooth_stone", 1),
                                        Vec3{0, 0, 0});
  a->pickupDelay = b->pickupDelay = 0;
  world.addEntity(a);
  world.addEntity(b);
  run(world, 40);
  auto *merged = itemAt(world);
  require(merged && merged->item.count == 2 && world.entities.size() == 1,
          "stackable item entities merge");
  // A stack at max size does not merge further.
  World full = floorWorld();
  auto c = std::make_shared<ItemEntity>(full, Vec3{0.5, 64.0, 0.5},
                                        itemStack("minecraft:smooth_stone", 64),
                                        Vec3{0, 0, 0});
  full.addEntity(c);
  full.entitiesIn(c->boundingBox());
  require(c->item.count == 64 && c->item.maxStackSize() == 64,
          "full stack remains full");
}
void itemRegistryAndDrops() {
  require(itemType("minecraft:white_carpet") &&
              itemType("minecraft:white_carpet")->maxStackSize == 64,
          "block item registered with max stack 64");
  require(itemType("minecraft:minecart") &&
              itemType("minecraft:minecart")->maxStackSize == 1,
          "minecart item max stack is one");
  require(itemType("minecraft:water") == nullptr, "fluid has no item");
  auto grass = blockDrops(defaultState("grass_block"));
  require(grass.size() == 1 && grass[0].id == "minecraft:dirt",
          "grass block drops dirt with an empty tool");
  require(blockDrops(defaultState("glass")).empty(),
          "glass needs Silk Touch and drops nothing");
  require(blockDrops(defaultState("piston_head")).empty(),
          "piston head has no item drop");
  auto rail = blockDrops(defaultState("rail"));
  require(rail.size() == 1 && rail[0].id == "minecraft:rail",
          "rail drops itself");
  require(blockDrops(defaultState("tnt").with("unstable", "true")).empty(),
          "unstable TNT does not drop");
  auto wireDrop = blockDrops(defaultState("redstone_wire"));
  require(wireDrop.size() == 1 && wireDrop[0].id == "minecraft:redstone",
          "redstone_wire drops minecraft:redstone");
  auto wallTorchDrop = blockDrops(defaultState("redstone_wall_torch"));
  require(wallTorchDrop.size() == 1 &&
              wallTorchDrop[0].id == "minecraft:redstone_torch",
          "redstone_wall_torch drops minecraft:redstone_torch");
  require(blockItem(defaultState("wall_torch")) == "minecraft:torch",
          "wall_torch maps to torch item");
  require(blockItem(defaultState("water_cauldron")) == "minecraft:cauldron",
          "water_cauldron maps to cauldron item");
  require(blockItem(defaultState("fire")).empty(),
          "fire has no item representation");
}
void itemFluids() {
  World water;
  water.loadChunk(0, 0);
  water.rawSetBlock({0, 62, 0}, defaultState("water"));
  auto wet = std::make_shared<ItemEntity>(
      water, Vec3{0.5, 62.5, 0.5}, itemStack("minecraft:white_carpet", 1),
      Vec3{0, 0, 0});
  water.addEntity(wet);
  water.tick();
  require(wet->isInWater(water), "item detects water");
  World lava;
  lava.loadChunk(0, 0);
  lava.rawSetBlock({0, 61, 0}, defaultState("smooth_stone"));
  lava.rawSetBlock({0, 62, 0}, defaultState("lava"));
  auto hot = std::make_shared<ItemEntity>(
      lava, Vec3{0.5, 62.1, 0.5}, itemStack("minecraft:white_carpet", 1),
      Vec3{0, 0, 0});
  lava.addEntity(hot);
  lava.tick();
  lava.tick();
  require(hot->isRemoved() || hot->health < 5, "lava damages item entities");
}
void horizontalCollision() {
  auto world = floorWorld();
  world.rawSetBlock({1, 64, 0}, defaultState("smooth_stone"));
  auto item = std::make_shared<ItemEntity>(
      world, Vec3{0.5, 64.0, 0.5}, itemStack("minecraft:white_carpet", 1),
      Vec3{1.0, 0, 0});
  world.addEntity(item);
  world.tick();
  require(item->position.x <= 0.875 + 1.0e-9,
          "item AABB stops at the wall face");
  require(item->horizontalCollision, "horizontal collision is reported");
  world.rawSetBlock({1, 64, 0}, defaultState("air"));
  const auto before = item->position.x;
  item->move(world, Vec3{1.0, 0, 0});
  require(close(item->position.x, before + 1.0) && !item->horizontalCollision,
          "subsequent movement clears collision boxes from the previous move");
}
void collisionShapesOutsideTheirBlock() {
  World world;
  world.loadChunk(-1, 0);
  world.loadChunk(0, 0);
  world.rawSetBlock({1, 64, 0}, defaultState("cobblestone_wall"));
  auto item = std::make_shared<ItemEntity>(world, Vec3{0.5, 65.125, 0.5},
                                           itemStack("white_carpet"), Vec3{});
  item->move(world, {1, 0, 0});
  require(close(item->position.x, 1.125) && item->horizontalCollision,
          "wall's upper half collides above its owning block cell");
  require(!world.noCollision({1.4, 65.1, 0.4, 1.6, 65.3, 0.6}),
          "wall's upper half also participates in overlap queries");

  world.rawSetBlock({1, 64, 0},
                    defaultState("moving_piston").with("facing", "east"));
  auto moving = std::make_shared<PistonMovingBlockEntity>(
      PistonMovingBlockEntity{{1, 64, 0},
                              defaultState("smooth_stone"),
                              Direction::East,
                              true,
                              false});
  moving->progress = 0.5f;
  world.setPistonEntity({1, 64, 0}, moving);
  item->setPos(0, 64.2, 0.5);
  item->move(world, {0.5, 0, 0});
  require(close(item->position.x, 0.375) && item->horizontalCollision,
          "moving block collides in the cell before its owning position");
  require(!world.noCollision({0.6, 64.2, 0.4, 0.8, 64.4, 0.6}),
          "moving block's offset shape participates in overlap queries");
}
void itemMovementSupport() {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({0, 63, 0}, defaultState("slime_block"));
  world.rawSetBlock({0, 64, 0}, defaultState("white_carpet"));
  auto item = std::make_shared<ItemEntity>(world, Vec3{0.5, 64.0625, 0.5},
                                           itemStack("white_carpet"),
                                           Vec3{0.1, -0.04, 0});
  world.addEntity(item);
  world.tick();
  require(item->onGround && close(item->position.y, 64.0625),
          "carpet supplies the collision surface");
  // getOnPosLegacy(.2) also sees the slime: its landing bounce is applied
  // before stepOn, and that resulting Y velocity sets the stepOn slowdown.
  require(close(item->deltaMovement.x,
                0.1 * (0.4 + 0.08 * 0.8 * 0.2) * 0.8f * 0.98f),
          "item on carpet uses underlying slime friction after stepOn");

  world.rawSetBlock({0, 63, 0}, defaultState("honey_block"));
  item->setPos(0.5, 64.0625, 0.5);
  item->setDeltaMovement(0.1, 0, 0);
  item->move(world, {0.1, 0, 0});
  require(close(item->deltaMovement.x, 0.1 * 0.4f),
          "item movement also uses underlying honey speed factor");
}
void escapeThroughEmptyCollision() {
  World world;
  world.loadChunk(0, 0);
  for (auto direction : {Direction::North, Direction::South, Direction::West,
                         Direction::East, Direction::Up})
    world.rawSetBlock(Pos{1, 64, 1}.relative(direction),
                      defaultState("smooth_stone"));
  world.rawSetBlock({1, 64, 1}, defaultState("smooth_stone"));
  world.rawSetBlock({2, 64, 1}, defaultState("rail"));
  ItemEntity item(world, {1.5, 64.0, 1.5}, itemStack("white_carpet"), {});
  item.moveTowardsClosestSpace(world, 1.5, 64.125, 1.5);
  require(item.deltaMovement.x > 0 && close(item.deltaMovement.y, 0) &&
              close(item.deltaMovement.z, 0),
          "empty rail collision is a valid exit from an embedded block");
}
void slimeBounce() {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({0, 63, 0}, defaultState("slime_block"));
  auto item = std::make_shared<ItemEntity>(
      world, Vec3{0.5, 67.0, 0.5}, itemStack("minecraft:white_carpet", 1),
      Vec3{0, 0, 0});
  world.addEntity(item);
  bool bounced = false;
  for (int i = 0; i < 30; ++i) {
    world.tick();
    bounced |= item->deltaMovement.y > 0.01;
  }
  require(bounced, "simple entities bounce on slime blocks");
  require(item->position.y >= 64.0 - 1.0e-6,
          "slime bounce does not sink through the block");
}
void itemLifetime() {
  auto world = floorWorld();
  auto item = std::make_shared<ItemEntity>(world, Vec3{0.5, 64.0, 0.5},
                                           itemStack("minecraft:tnt", 1),
                                           Vec3{0, 0, 0});
  item->age = 5999;
  world.addEntity(item);
  run(world, 1);
  require(item->isRemoved(), "item despawns at age 6000");
}
void fractionalItemDamage() {
  World world;
  ItemEntity item(world, {0.5, 64.0, 0.5}, itemStack("white_carpet"), {});
  require(item.hurt(world, DamageKind::Explosion, 0.25f) && item.health == 4,
          "fractional item damage truncates the resulting health");
  require(item.hurt(world, DamageKind::Explosion, 3.25f) && item.isRemoved(),
          "fractional damage can reduce the resulting health to zero");
}
void tntBlockPriming() {
  auto world = floorWorld();
  world.rawSetBlock({-1, 64, 0}, defaultState("redstone_block"));
  world.setBlock({0, 64, 0}, defaultState("tnt"));
  require(isAirState(world.getBlock({0, 64, 0})),
          "powered TNT block is removed");
  require(tntAt(world) != nullptr, "primed TNT entity exists");
  require(tntAt(world)->fuse == 80, "default TNT fuse is 80");
  require(world.log.size() >= 1, "priming is observable");
}
void tntDisabledRule() {
  auto world = floorWorld();
  world.tntExplodes = false;
  world.rawSetBlock({-1, 64, 0}, defaultState("redstone_block"));
  world.setBlock({0, 64, 0}, defaultState("tnt"));
  require(world.getBlock({0, 64, 0}).name() == "minecraft:tnt",
          "disabled TNT rule keeps the block");
  require(tntAt(world) == nullptr, "disabled TNT rule spawns no entity");
}
void tntExplosion() {
  auto world = floorWorld();
  world.rawSetBlock({1, 64, 0}, defaultState("obsidian"));
  auto tnt = std::make_shared<PrimedTnt>(world, Vec3{0.5, 64.0, 0.5});
  tnt->fuse = 1;
  world.addEntity(tnt);
  run(world, 1);
  require(tnt->isRemoved(), "exploded TNT entity is removed");
  require(world.getBlock({1, 64, 0}).name() == "minecraft:obsidian",
          "obsidian survives a power-4 explosion");
  require(isAirState(world.getBlock({0, 63, 0})),
          "floor below the explosion is destroyed");
  bool dropped = false;
  for (auto &entity : world.entities)
    if (auto *item = dynamic_cast<ItemEntity *>(entity.get()))
      dropped |= item->item.id == "minecraft:smooth_stone";
  require(dropped, "destroyed blocks produce real item entities");
  require(!world.log.empty(), "explosion is observable");
}
void protocolPlayerEntityIdsShareOneMonotonicNamespace() {
  World world;
  world.loadChunk(0, 0);
  const int playerA = world.allocateEntityId();
  auto first = std::make_shared<ItemEntity>(
      world, Vec3{0.5, 64.0, 0.5}, itemStack("minecraft:stone", 1));
  world.addEntity(first);
  const int playerB = world.allocateEntityId();
  auto second = std::make_shared<ItemEntity>(
      world, Vec3{0.5, 64.0, 0.5}, itemStack("minecraft:stone", 1));
  world.addEntity(second);
  require(playerA != first->id && playerB != first->id &&
              playerB != second->id && first->id != second->id &&
              playerA != playerB,
          "protocol players and World entities have disjoint live IDs");
  world.removeEntity(first);
  const int laterPlayer = world.allocateEntityId();
  require(laterPlayer != playerA && laterPlayer != playerB &&
              laterPlayer != first->id && laterPlayer != second->id,
          "entity and connection churn cannot reuse an allocated ID");
}

void denseExplosion() {
  // Low-resistance solid in every direction, so the ray shell covers ~1,400
  // distinct cells instead of the handful an air burst touches. Every other
  // explosion test detonates in open air; this one also pins down how far a
  // power-4 blast reaches into a solid mass.
  World world;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      world.loadChunk(x, z);
  for (int x = -8; x <= 8; ++x)
    for (int y = 56; y <= 72; ++y)
      for (int z = -8; z <= 8; ++z)
        world.rawSetBlock({x, y, z}, defaultState("sand"));
  world.setSeed(4242);
  world.explode(nullptr, {0.5, 64.5, 0.5}, 4.0f, false);
  int removed = 0;
  for (int x = -8; x <= 8; ++x)
    for (int y = 56; y <= 72; ++y)
      for (int z = -8; z <= 8; ++z)
        removed += isAirState(world.getBlock({x, y, z})) ? 1 : 0;
  require(isAirState(world.getBlock({0, 64, 0})),
          "an explosion inside sand destroys the cell it starts in");
  // 180 cells on this seed; the band tolerates float drift without hiding a
  // blast that stops short of its cell radius or leaks past it.
  require(removed > 150 && removed < 220,
          "an explosion inside sand destroys the expected ball of blocks");
}
void explosionRayRandomConsumption() {
  World world;
  world.setSeed(123);
  Random expected(123);
  // Even zero-strength explosions consume one random float for every shell
  // ray. Check both the first table initialization and a subsequent reuse.
  for (int pass = 0; pass < 2; ++pass) {
    world.explode(nullptr, {-0.5, 64, -0.5}, 0.0f, false);
    for (int i = 0; i < 1352; ++i)
      expected.nextFloat();
    require(world.random.nextInt() == expected.nextInt(),
            "precomputed explosion rays preserve random stream consumption");
    require(world.entities.empty() && world.allBlocks().empty(),
            "zero-strength rays do not create or destroy blocks/entities");
  }
}
void tntChain() {
  auto world = floorWorld();
  for (int x = -1; x <= 1; ++x)
    world.rawSetBlock({x, 64, 0}, defaultState("tnt"));
  auto tnt = std::make_shared<PrimedTnt>(world, Vec3{0.5, 64.0, 0.5});
  tnt->fuse = 1;
  world.addEntity(tnt);
  run(world, 1);
  require(tntAt(world) != nullptr,
          "explosion primes neighbouring TNT blocks as entities");
  require(isAirState(world.getBlock({1, 64, 0})),
          "neighbouring TNT block is consumed");
}
void itemExplosionDamage() {
  auto world = floorWorld();
  auto item = std::make_shared<ItemEntity>(
      world, Vec3{1.0, 64.0, 0.5}, itemStack("minecraft:white_carpet", 1),
      Vec3{0, 0, 0});
  world.addEntity(item);
  auto tnt = std::make_shared<PrimedTnt>(world, Vec3{0.5, 64.0, 0.5});
  tnt->fuse = 1;
  world.addEntity(tnt);
  run(world, 1);
  require(item->isRemoved() || item->health < 5,
          "explosion damages item entities");
}
void minecartExplosion() {
  auto world = floorWorld();
  auto cart = std::make_shared<Minecart>(world, Vec3{1.5, 64.0, 0.5});
  world.addEntity(cart);
  auto tnt = std::make_shared<PrimedTnt>(world, Vec3{0.5, 64.0, 0.5});
  tnt->fuse = 1;
  world.addEntity(tnt);
  run(world, 1);
  require(cart->isRemoved(), "explosion destroys a minecart above 40 damage");
  bool dropped = false;
  for (auto &entity : world.entities)
    if (auto *item = dynamic_cast<ItemEntity *>(entity.get()))
      dropped |= item->item.id == "minecraft:minecart";
  require(dropped, "destroyed minecart leaves a minecart item entity");
}
void fallingBlock() {
  World rngWorld(-64, 320);
  const auto rngBefore = [&] {
    auto probe = rngWorld.random;
    return probe.nextLong();
  }();
  auto restored = std::make_shared<FallingBlockEntity>(
      rngWorld, Vec3{0.5, 20.0, 0.5}, defaultState("sand"));
  restored->uuid = {12, 34, 56, 78};
  rngWorld.addEntity(restored);
  const auto rngAfter = [&] {
    auto probe = rngWorld.random;
    return probe.nextLong();
  }();
  require(rngAfter == rngBefore,
          "falling block install does not consume World random");

  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({0, 60, 0}, defaultState("smooth_stone"));
  auto *entity =
      FallingBlockEntity::fall(world, {0, 64, 0}, defaultState("smooth_stone"));
  require(entity != nullptr, "falling block entity created");
  int landingPackets = 0;
  world.fallingBlockLandObserver = [&](int id, Pos pos, const BlockState &state) {
    require(id == entity->id, "landing correction identifies the tracked entity");
    require(!entity->isRemoved(), "landing correction precedes entity discard");
    require(pos == Pos{0, 61, 0} && state == world.getBlock(pos),
            "landing correction observes the current final world state");
    ++landingPackets;
  };
  for (int i = 0; i < 30; ++i)
    world.tick();
  require(world.getBlock({0, 61, 0}).name() == "minecraft:smooth_stone",
          "falling block lands at the first free position");
  require(world.entities.empty(), "landed falling block entity is removed");
  require(landingPackets == 1, "successful landing sends one immediate correction");
}
void sandPistonCompletion() {
  World world;
  world.loadChunk(0, 0);
  Pos destination{4, 65, 4};
  world.rawSetBlock({4, 60, 4}, defaultState("smooth_stone"));
  world.rawSetBlock(destination, defaultState("moving_piston").with("facing", "east"));
  auto moving = std::make_shared<PistonMovingBlockEntity>();
  moving->pos = destination;
  moving->movedState = defaultState("sand");
  moving->direction = Direction::East;
  moving->source = false;
  moving->extending = true;
  moving->progress = 1;
  world.setPistonEntity(destination, moving);
  world.tick();
  require(world.getBlock(destination).name() == "minecraft:sand",
          "completed moving piston installs sand");
  require(world.hasScheduledTick(destination, "sand"),
          "piston completion shape query and onPlace schedule sand gravity");
  world.tick();
  require(world.entities.empty(), "sand waits two ticks after piston completion");
  world.tick();
  require(world.getBlock(destination).name() == "minecraft:air" &&
              world.entities.size() == 1,
          "sand starts falling in scheduled phase after piston completion");

  World obstructed;
  obstructed.loadChunk(0, 0);
  obstructed.rawSetBlock({4, 63, 4}, defaultState("smooth_stone"));
  obstructed.rawSetBlock({4, 64, 4}, defaultState("white_carpet"));
  FallingBlockEntity::fall(obstructed, {4, 67, 4}, defaultState("sand"));
  int corrections = 0;
  obstructed.fallingBlockLandObserver = [&](int, Pos, const BlockState &) { ++corrections; };
  for (int i = 0; i < 30; ++i) obstructed.tick();
  bool droppedSand = false;
  for (auto &entry : obstructed.entities) {
    require(!dynamic_cast<FallingBlockEntity *>(entry.get()),
            "sand landing in a nonreplaceable carpet cell breaks");
    if (auto *item = dynamic_cast<ItemEntity *>(entry.get()))
      droppedSand |= item->item.id == "minecraft:sand" && item->item.count == 1;
  }
  require(droppedSand && corrections == 0,
          "failed sand landing drops an item and sends no placement correction");
}
void minecartFall() {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({0, 60, 0}, defaultState("smooth_stone"));
  auto cart = std::make_shared<Minecart>(world, Vec3{0.5, 66.0, 0.5});
  world.addEntity(cart);
  for (int i = 0; i < 30; ++i)
    world.tick();
  require(close(cart->position.y, 61.0, 1.0e-4),
          "off-track minecart rests on the floor");
  require(cart->onGround, "minecart reports onGround");
}
void railMovement() {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({0, 63, 0}, defaultState("smooth_stone"));
  world.rawSetBlock({0, 64, 0},
                    defaultState("rail").with("shape", "north_south"));
  auto cart = std::make_shared<Minecart>(world, Vec3{0.5, 64.0625, 0.5});
  cart->deltaMovement = {0, 0, 0.2};
  world.addEntity(cart);
  world.tick();
  require(cart->onRails, "minecart detects rail below");
  require(cart->position.z > 0.5, "minecart moves along a north-south rail");
  require(close(cart->position.y, 64.0625, 1.0e-4),
          "minecart stays at rail height");
}
void poweredRail() {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({0, 63, 0}, defaultState("smooth_stone"));
  world.rawSetBlock({0, 64, 0}, defaultState("powered_rail")
                                    .with("shape", "north_south")
                                    .with("powered", "true"));
  // A redstone conductor at the south end launches an idle cart north.
  world.rawSetBlock({0, 64, 1}, defaultState("smooth_stone"));
  auto cart = std::make_shared<Minecart>(world, Vec3{0.5, 64.0625, 0.5});
  world.addEntity(cart);
  world.tick();
  require(cart->deltaMovement.z < 0,
          "powered rail with a southern conductor accelerates north");
}
void poweredRailChain() {
  World world;
  for (int z = -1; z <= 1; ++z) {
    world.loadChunk(0, z);
    world.rawSetBlock({0, 63, z}, defaultState("smooth_stone"));
  }
  world.rawSetBlock({0, 64, -1}, defaultState("redstone_block"));
  world.rawSetBlock({0, 64, 0}, defaultState("powered_rail")
                                    .with("shape", "north_south")
                                    .with("powered", "true"));
  world.rawSetBlock({0, 64, 1}, defaultState("powered_rail")
                                    .with("shape", "north_south")
                                    .with("powered", "false"));
  world.neighborChanged({0, 64, 1}, "minecraft:redstone_block");
  require(world.getBlock({0, 64, 1}).get("powered") == "true",
          "powered rail chain propagates power up to eight blocks");
}
void ascendingRail() {
  World world;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z) {
      world.loadChunk(x, z);
      world.rawSetBlock({x, 63, z}, defaultState("smooth_stone"));
    }
  world.rawSetBlock({1, 64, 0}, defaultState("smooth_stone"));
  world.rawSetBlock({0, 64, 0},
                    defaultState("rail").with("shape", "ascending_east"));
  auto cart = std::make_shared<Minecart>(world, Vec3{0.5, 64.0625, 0.5});
  cart->deltaMovement = {0.2, 0, 0};
  world.addEntity(cart);
  world.tick();
  require(cart->position.x > 0.5, "minecart moves east on an ascending rail");
  require(cart->position.y > 64.5,
          "ascending-east rail lifts the minecart by one block");
}
void railBlockMetadata() {
  auto rail = defaultState("powered_rail");
  require(isRailBlock(rail), "powered rail is recognized");
  require(railShapeOf(rail) == RailShape::NorthSouth,
          "default rail shape is north-south");
  require(blockMetadata(rail).pushReaction == "normal" &&
              !blockMetadata(rail).conductor,
          "rail metadata defaults");
  require(collisionShape(rail).empty(), "rails have no collision shape");
}
void detectorRail() {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({0, 63, 0}, defaultState("smooth_stone"));
  world.rawSetBlock({0, 64, 0},
                    defaultState("detector_rail").with("shape", "north_south"));
  auto cart = std::make_shared<Minecart>(world, Vec3{0.5, 64.0625, 0.5});
  world.addEntity(cart);
  world.tick();
  require(world.getBlock({0, 64, 0}).get("powered") == "true",
          "detector rail powers when a minecart occupies it");
  require(world.getSignal({0, 64, 0}, Direction::Up) == 15,
          "powered detector rail emits 15");
  require(world.getSignal({0, 63, 0}, Direction::Down) == 15,
          "powered detector rail strongly powers the block below it");
  require(world.hasScheduledTick({0, 64, 0}, "minecraft:detector_rail"),
          "detector rail schedules its pressed recheck");
  world.removeEntity(world.entities.front());
  run(world, 20);
  require(world.getBlock({0, 64, 0}).get("powered") == "false",
          "detector rail releases when the minecart leaves");
}
void railSupport() {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({0, 63, 0}, defaultState("smooth_stone"));
  world.rawSetBlock({0, 64, 0},
                    defaultState("rail").with("shape", "north_south"));
  world.setBlock({0, 63, 0}, defaultState("air"));
  require(isAirState(world.getBlock({0, 64, 0})),
          "unsupported rail is removed");
  ItemEntity *drop = itemAt(world);
  require(drop && drop->item.id == "minecraft:rail",
          "unsupported rail drops its item entity");
}
void railCurve() {
  World world;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z) {
      world.loadChunk(x, z);
      world.rawSetBlock({x, 63, z}, defaultState("smooth_stone"));
    }
  // A corner made from independently placed rails must curve rather than stay
  // straight: the west/east and north/south groups each have one connection.
  world.setBlock({0, 64, 0}, defaultState("rail"));
  world.setBlock({1, 64, 0}, defaultState("rail"));
  world.setBlock({0, 64, 1}, defaultState("rail"));
  require(world.getBlock({0, 64, 0}).get("shape") != "north_south" ||
              world.getBlock({0, 64, 1}).get("shape") != "north_south" ||
              world.getBlock({1, 64, 0}).get("shape") != "north_south",
          "rail placement reorients at least one connected rail");
}
void railPistonSupport() {
  World world;
  world.loadChunk(0, 0);
  auto rail = defaultState("rail");
  world.rawSetBlock(
      {0, 63, 0},
      defaultState("piston").with("facing", "down").with("extended", "true"));
  require(railCanSurvive(world, {0, 64, 0}, rail),
          "downward extended piston base supports a rail on its full top");
  world.rawSetBlock({0, 63, 0},
                    defaultState("piston_head").with("facing", "up"));
  require(railCanSurvive(world, {0, 64, 0}, rail),
          "upward piston head's full top supports a rail");
  for (auto state :
       {defaultState("piston").with("facing", "up").with("extended", "true"),
        defaultState("cobblestone_wall"), defaultState("honey_block"),
        defaultState("white_carpet")}) {
    world.rawSetBlock({0, 63, 0}, state);
    require(!railCanSurvive(world, {0, 64, 0}, rail),
            "rail support needs the top outer ring at block height");
  }
}
void railJunctionPowerSwitch() {
  auto world = floorWorld();
  for (Pos pos : {Pos{0, 64, 0}, Pos{0, 64, -1}, Pos{0, 64, 1}})
    world.rawSetBlock(pos, defaultState("rail"));
  world.rawSetBlock({1, 64, 0},
                    defaultState("rail").with("shape", "east_west"));
  world.neighborChanged({0, 64, 0}, "minecraft:redstone_block");
  require(world.getBlock({0, 64, 0}).get("shape") == "south_east",
          "unpowered T junction prefers the south-east turn");
  world.setBlock({0, 63, 0}, defaultState("redstone_block"));
  world.neighborChanged({0, 64, 0}, "minecraft:redstone_block");
  require(world.getBlock({0, 64, 0}).get("shape") == "north_east",
          "power switches the T junction to the north-east turn");
  world.setBlock({0, 63, 0}, defaultState("smooth_stone"));
  // RailBlock only re-evaluates a junction for a signal-source notification;
  // Level uses the removed block as the source when power is taken away.
  world.neighborChanged({0, 64, 0}, "minecraft:redstone_block");
  require(world.getBlock({0, 64, 0}).get("shape") == "south_east",
          "removing power switches the T junction back");
}
class ProbeEntity : public Entity {
public:
  ProbeEntity(World &, std::vector<int> &order, int value,
              std::shared_ptr<Entity> spawn = nullptr)
      : Entity("minecraft:item"), order_(order), value_(value),
        spawn_(std::move(spawn)) {
    setPos(0.5, 64.0, 0.5);
  }
  void tick(World &world) override {
    order_.push_back(value_);
    if (spawn_) {
      auto next = spawn_;
      spawn_.reset();
      world.addEntity(next);
    }
  }

private:
  std::vector<int> &order_;
  int value_;
  std::shared_ptr<Entity> spawn_;
};
class RemovingProbe : public Entity {
public:
  RemovingProbe(std::vector<int> &order, std::shared_ptr<Entity> target)
      : Entity("minecraft:item"), order_(order), target_(std::move(target)) {
    setPos(0.5, 64.0, 0.5);
  }
  void tick(World &world) override {
    order_.push_back(10);
    world.removeEntity(target_);
    discard();
  }
private:
  std::vector<int> &order_;
  std::shared_ptr<Entity> target_;
};
void entityTickOrder() {
  auto world = floorWorld();
  std::vector<int> order;
  world.addEntity(std::make_shared<ProbeEntity>(world, order, 1));
  world.addEntity(std::make_shared<ProbeEntity>(world, order, 2));
  world.tick();
  require((order == std::vector<int>{1, 2}),
          "entities tick in registration order");
  order.clear();
  auto late = std::make_shared<ProbeEntity>(world, order, 3);
  world.addEntity(std::make_shared<ProbeEntity>(world, order, 4, late));
  world.tick();
  require((order == std::vector<int>{1, 2, 4}),
          "entity added during the pass waits for the next pass");
  order.clear();
  world.tick();
  require(std::find(order.begin(), order.end(), 3) != order.end(),
          "deferred entity ticks on the following pass");

  auto removals = floorWorld();
  order.clear();
  auto skipped = std::make_shared<ProbeEntity>(removals, order, 11);
  auto first = std::make_shared<ProbeEntity>(removals, order, 9);
  auto last = std::make_shared<ProbeEntity>(removals, order, 12);
  removals.addEntity(first);
  removals.addEntity(std::make_shared<RemovingProbe>(order, skipped));
  removals.addEntity(skipped);
  removals.addEntity(last);
  removals.tick();
  require((order == std::vector<int>{9, 10, 12}),
          "discarded entity is skipped during the current snapshot");
  require(removals.entities.size() == 2 && removals.entities[0] == first &&
              removals.entities[1] == last,
          "entity cleanup retains survivors in registration order");
}
void persistentUuidGeneratorIsRestartIndependent() {
  World first;
  World second;
  first.setSeed(2614786);
  second.setSeed(2614786);
  first.loadChunk(0, 0);
  second.loadChunk(0, 0);
  auto firstEntity = std::make_shared<ItemEntity>(
      first, Vec3{.5, 64.0, .5}, itemStack("minecraft:stone", 3), Vec3{});
  auto secondEntity = std::make_shared<ItemEntity>(
      second, Vec3{.5, 64.0, .5}, itemStack("minecraft:stone", 3), Vec3{});
  first.addEntity(firstEntity);
  second.addEntity(secondEntity);
  require(firstEntity->id == secondEntity->id,
          "restart fixture reuses runtime id 1");
  require(firstEntity->uuid != secondEntity->uuid &&
              firstEntity->uuid != std::array<std::int32_t, 4>{} &&
              secondEntity->uuid != std::array<std::int32_t, 4>{},
          "same seed/runtime id instances do not reuse persistent UUIDs");
  auto thirdEntity = std::make_shared<ItemEntity>(
      first, Vec3{1.5, 64.0, 1.5}, itemStack("minecraft:stone", 1), Vec3{});
  first.addEntity(thirdEntity);
  require(thirdEntity->uuid != firstEntity->uuid,
          "one process allocates distinct persistent UUIDs");
  std::cout << "persistent_uuid same_seed_same_runtime_id=distinct\n";
}

void entityHeightBoundaries() {
  World world;
  world.loadChunk(0, 0);
  auto upper = std::make_shared<ItemEntity>(world, Vec3{.5, 320.5, .5},
                                            itemStack("white_carpet"), Vec3{});
  auto lower = std::make_shared<ItemEntity>(world, Vec3{.5, -64.5, .5},
                                            itemStack("white_carpet"), Vec3{});
  auto voidItem = std::make_shared<ItemEntity>(
      world, Vec3{.5, -129, .5}, itemStack("white_carpet"), Vec3{});
  auto unloaded = std::make_shared<ItemEntity>(
      world, Vec3{16.5, -64.5, .5}, itemStack("white_carpet"), Vec3{});
  world.addEntity(upper);
  world.addEntity(lower);
  world.addEntity(voidItem);
  world.addEntity(unloaded);
  world.tick();
  require(upper->tickCount == 1 && upper->position.y < 320.5,
          "items above build height still fall in an entity-ticking chunk");
  require(lower->tickCount == 1 && lower->position.y < -64.5,
          "items below build height must not freeze before reaching the void");
  require(voidItem->isRemoved(), "void removal executes below minY minus 64");
  require(unloaded->tickCount == 0, "unloaded X/Z chunk still pauses entities");
  run(world, 120);
  require(lower->isRemoved(),
          "a falling item eventually crosses the void threshold");
  // PrimedTnt.tick intentionally doesn't call Entity.baseTick in vanilla:
  // TNT below the void threshold keeps counting down instead of void-killing.
  world.tntExplodes = false;
  auto tnt = std::make_shared<PrimedTnt>(world, Vec3{.5, -129, .5});
  tnt->fuse = 2;
  world.addEntity(tnt);
  world.tick();
  require(!tnt->isRemoved() && tnt->fuse == 1,
          "TNT preserves its separate void behavior and continues its fuse");
  world.tick();
  require(tnt->isRemoved(), "TNT still expires at zero fuse");
  auto falling = std::make_shared<FallingBlockEntity>(
      world, Vec3{.5, 320, .5}, defaultState("smooth_stone"));
  falling->time = 100;
  falling->deltaMovement.y = .04;
  world.addEntity(falling);
  world.tick();
  require(falling->isRemoved(), "falling block timeout uses inclusive vanilla "
                                "maxY (319), not exclusive C++ maxY (320)");
}
void storageEntityImport() {
  LoadedWorld loaded;
  loaded.chunks = {{0, 0}};
  nbt::Tag pos;
  pos.type = 9;
  for (double value : {0.5, 64.0, 0.5}) {
    nbt::Tag number;
    number.type = 6;
    number.real = value;
    pos.list.push_back(number);
  }
  nbt::Tag entity;
  entity.type = 10;
  nbt::Tag id;
  id.type = 8;
  id.text = "minecraft:item";
  entity.compound["id"] = id;
  entity.compound["Pos"] = pos;
  nbt::Tag item;
  item.type = 10;
  nbt::Tag itemId;
  itemId.type = 8;
  itemId.text = "minecraft:white_carpet";
  item.compound["id"] = itemId;
  nbt::Tag count;
  count.type = 3;
  count.number = 3;
  item.compound["count"] = count;
  entity.compound["Item"] = item;
  loaded.entities.push_back(entity);
  World world;
  importWorld(loaded, world);
  auto *itemEntity = itemAt(world);
  require(itemEntity && itemEntity->item.count == 3 &&
              itemEntity->item.id == "minecraft:white_carpet",
          "item entity NBT is imported");
}

// Slab test used by the reference scan: does the segment touch this world-space
// box? Independent of World::clip's traversal, only of its per-box answer.
bool rayHits(double x0, double y0, double z0, double x1, double y1, double z1,
             Vec3 from, Vec3 to) {
  double tmin = 0, tmax = 1;
  const double lo[3] = {x0, y0, z0};
  const double hi[3] = {x1, y1, z1};
  const double start[3] = {from.x, from.y, from.z};
  const double delta[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(delta[axis]) < 1.0e-7) {
      if (start[axis] < lo[axis] || start[axis] > hi[axis])
        return false;
      continue;
    }
    double a = (lo[axis] - start[axis]) / delta[axis];
    double b = (hi[axis] - start[axis]) / delta[axis];
    if (a > b)
      std::swap(a, b);
    tmin = std::max(tmin, a);
    tmax = std::min(tmax, b);
    if (tmin > tmax)
      return false;
  }
  return true;
}

// Every block in the segment's bounding box, no traversal shortcut: the answer
// World::clip must keep producing after switching to BlockGetter.traverseBlocks'
// DDA.
bool scanClip(World &world, Vec3 from, Vec3 to) {
  const int x0 = static_cast<int>(std::floor(std::min(from.x, to.x)));
  const int y0 = static_cast<int>(std::floor(std::min(from.y, to.y)));
  const int z0 = static_cast<int>(std::floor(std::min(from.z, to.z)));
  const int x1 = static_cast<int>(std::floor(std::max(from.x, to.x)));
  const int y1 = static_cast<int>(std::floor(std::max(from.y, to.y)));
  const int z1 = static_cast<int>(std::floor(std::max(from.z, to.z)));
  for (int x = x0; x <= x1; ++x)
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z) {
        const Pos pos{x, y, z};
        const auto state = world.getBlock(pos);
        if (state.isAir())
          continue;
        VoxelShape moving;
        const VoxelShape &shape =
            state.is(Blocks::MOVING_PISTON)
                ? (moving = collisionShape(world, pos, state), moving)
                : collisionShape(state);
        for (const auto &b : shape)
          if (rayHits(b.x0 / 16.0 + x, b.y0 / 16.0 + y, b.z0 / 16.0 + z,
                      b.x1 / 16.0 + x, b.y1 / 16.0 + y, b.z1 / 16.0 + z,
                      from, to))
            return true;
      }
  return false;
}

void clipTraversal() {
  World world;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      world.loadChunk(x, z);
  for (int x = -12; x <= 12; ++x)
    for (int z = -12; z <= 12; ++z)
      world.rawSetBlock({x, 62, z}, defaultState("smooth_stone"));
  // Full wall at x=4, a 0.0625-high carpet, a partial snow layer and a glass
  // cube, so the ray meets full, thin and airborne-only cells.
  for (int y = 63; y <= 66; ++y)
    for (int z = -6; z <= 6; ++z)
      world.rawSetBlock({4, y, z}, defaultState("smooth_stone"));
  world.rawSetBlock({-3, 63, 2}, defaultState("white_carpet"));
  world.rawSetBlock({-6, 63, -4}, defaultState("snow"));
  world.rawSetBlock({0, 65, 0}, defaultState("glass"));

  // Axis-aligned cases with a known answer.
  require(world.clip({2.5, 64.5, 0.5}, {6.5, 64.5, 0.5}),
          "clip crosses a one-block wall");
  require(!world.clip({2.5, 64.5, 8.5}, {6.5, 64.5, 8.5}),
          "clip misses the wall outside its extent");
  // BlockGetter.traverseBlocks pushes both endpoints outward by 1e-7, so a
  // segment that ends exactly on the wall face still enters the wall cell (the
  // exhaustive scan answers the same way for the same endpoint).
  require(world.clip({2.5, 64.5, 0.5}, {4.0, 64.5, 0.5}),
          "clip visits the cell of a boundary endpoint");
  require(!world.clip({2.5, 64.5, 0.5}, {3.9999, 64.5, 0.5}),
          "clip does not reach the wall before the face");
  require(world.clip({-5.5, 63.03125, 2.5}, {-1.5, 63.03125, 2.5}),
          "clip hits a 1/16-high carpet");
  require(!world.clip({-5.5, 63.5, 2.5}, {-1.5, 63.5, 2.5}),
          "clip passes over a 1/16-high carpet");
  require(world.clip({0.2, 65.2, 0.2}, {0.8, 65.8, 0.8}),
          "clip probes the block that contains the start point");
  require(!world.clip({-0.5, 70.5, -0.5}, {6.5, 74.5, 6.5}),
          "clip through open air reports no hit");

  // Random short segments must agree with the exhaustive bounding-box scan.
  std::uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto nextDouble = [&state](double lo, double hi) {
    state = state * 6364136223846793005ull + 1442695040888963407ull;
    const double unit = static_cast<double>((state >> 11) & ((1ull << 53) - 1)) /
                        static_cast<double>(1ull << 53);
    return lo + (hi - lo) * unit;
  };
  int hits = 0;
  for (int i = 0; i < 20000; ++i) {
    const Vec3 from{nextDouble(-10.5, 10.5), nextDouble(62.0, 68.0),
                    nextDouble(-10.5, 10.5)};
    const Vec3 to{from.x + nextDouble(-5.5, 5.5), from.y + nextDouble(-3.5, 3.5),
                  from.z + nextDouble(-5.5, 5.5)};
    const bool expected = scanClip(world, from, to);
    const bool actual = world.clip(from, to);
    hits += actual ? 1 : 0;
    require(actual == expected,
            "clip agrees with the exhaustive segment scan");
  }
  require(hits > 2000 && hits < 18000,
          "clip fixture produces a mix of hits and misses");
}
} // namespace

int main() {
  try {
    itemPhysics();
    horizontalCollision();
    collisionShapesOutsideTheirBlock();
    itemMovementSupport();
    escapeThroughEmptyCollision();
    itemMerging();
    itemRegistryAndDrops();
    itemFluids();
    slimeBounce();
    itemLifetime();
    fractionalItemDamage();
    tntBlockPriming();
    tntDisabledRule();
    tntExplosion();
    denseExplosion();
    protocolPlayerEntityIdsShareOneMonotonicNamespace();
    explosionRayRandomConsumption();
    tntChain();
    itemExplosionDamage();
    minecartExplosion();
    fallingBlock();
    sandPistonCompletion();
    minecartFall();
    railMovement();
    poweredRail();
    poweredRailChain();
    ascendingRail();
    railBlockMetadata();
    detectorRail();
    railSupport();
    railCurve();
    railPistonSupport();
    railJunctionPowerSwitch();
    entityTickOrder();
    persistentUuidGeneratorIsRestartIndependent();
    entityHeightBoundaries();
    storageEntityImport();
    clipTraversal();
    std::cout << "entity tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "entity test failed: " << e.what() << '\n';
    return 1;
  }
}
