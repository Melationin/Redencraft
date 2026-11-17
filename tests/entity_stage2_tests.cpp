#include "redstone/containers.hpp"
#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include "redstone/nbt.hpp"
#include "redstone/redstone.hpp"
#include "redstone/world_chunk_manager.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace redstone;

class LivingProbeEntity final : public Entity {
public:
  LivingProbeEntity() : Entity("minecraft:armor_stand") {}
  bool isLiving() const override { return true; }
  bool hurt(World &, DamageKind kind, float amount) override {
    hurtCalls += 1;
    lastKind = kind;
    lastDamage = amount;
    return true;
  }
  void tick(World &) override {}

  int hurtCalls = 0;
  DamageKind lastKind = DamageKind::Generic;
  float lastDamage = 0.0f;
};

void buildFloor(World &world, int y = 63, int minX = -8, int maxX = 16,
                int minZ = -8, int maxZ = 16) {
  const auto stone = defaultState("stone");
  for (int x = minX; x <= maxX; ++x)
    for (int z = minZ; z <= maxZ; ++z)
      world.setBlock({x, y, z}, stone, 3);
}

void testFallingBlockConcreteAnvilAndBlockData() {
  World world;
  world.loadChunk(0, 0);
  buildFloor(world, 63);

  // Concrete powder falling into water solidifies into concrete.
  world.setBlock({2, 64, 2}, defaultState("water"), 3);
  auto *fallingConcrete = FallingBlockEntity::fall(
      world, {2, 68, 2}, defaultState("red_concrete_powder"));
  assert(fallingConcrete != nullptr);
  for (int i = 0; i < 40 && !fallingConcrete->isRemoved(); ++i)
    world.tick();
  assert(world.getBlock({2, 64, 2}).name() == "minecraft:red_concrete");

  // Anvil falling from height damages LivingProbeEntity, can chip/damage anvil.
  auto probe = std::make_shared<LivingProbeEntity>();
  probe->setPos(4.5, 64.0, 4.5);
  world.addEntity(probe);

  world.setSeed(77);
  auto *anvil =
      FallingBlockEntity::fall(world, {4, 72, 4}, defaultState("anvil"));
  assert(anvil != nullptr);
  for (int i = 0; i < 50 && !anvil->isRemoved(); ++i)
    world.tick();
  assert(probe->hurtCalls >= 1);
  assert(probe->lastKind == DamageKind::Anvil);
  assert(probe->lastDamage > 0.0f);
  const std::string landedName = world.getBlock({4, 64, 4}).name();
  assert(landedName == "minecraft:anvil" ||
         landedName == "minecraft:chipped_anvil" ||
         landedName == "minecraft:damaged_anvil" ||
         landedName == "minecraft:air");

  // FallingBlock blockData restores container contents on landing.
  nbt::Tag blockData;
  nbt::Tag items;
  nbt::Tag entry;
  nbt::Tag slotTag;
  slotTag.number = 0;
  entry.compound["Slot"] = slotTag;
  nbt::Tag idTag;
  idTag.text = "minecraft:redstone";
  entry.compound["id"] = idTag;
  nbt::Tag countTag;
  countTag.number = 12;
  entry.compound["count"] = countTag;
  items.list.push_back(entry);
  blockData.compound["Items"] = items;

  auto barrelEntity = std::make_shared<FallingBlockEntity>(
      world, Vec3{6.5, 65.0, 6.5}, defaultState("barrel"));
  barrelEntity->blockData = blockData;
  world.addEntity(barrelEntity);
  for (int i = 0; i < 20 && !barrelEntity->isRemoved(); ++i)
    world.tick();
  assert(world.getBlock({6, 64, 6}).is(Blocks::BARREL));
  auto container = world.containerEntity({6, 64, 6});
  assert(container != nullptr);
  assert(container->getItem(0).id == "minecraft:redstone");
  assert(container->getItem(0).count == 12);
}

void testMinecartVariantsAndHopperInteraction() {
  World world;
  world.loadChunk(0, 0);
  buildFloor(world, 63);

  // Rail line at y=64, x=0..6, z=2. Detector rail at x=2, powered comparator reading it.
  for (int x = 0; x <= 6; ++x)
    world.setBlock({x, 64, 2}, defaultState("rail"), 3);
  world.setBlock({2, 64, 2}, defaultState("detector_rail"), 3);

  auto cart = std::make_shared<MinecartChest>(world, Vec3{2.5, 64.1, 2.5});
  cart->setContainerItem(0, itemStack("minecraft:redstone", 64));
  cart->setContainerItem(1, itemStack("minecraft:redstone", 64));
  world.addEntity(cart);
  world.tick();

  const BlockState detState = world.getBlock({2, 64, 2});
  assert(propertyValue(detState, PropertyKind::Powered) == 1);
  assert(hasAnalogOutputSignal(detState));
  assert(comparatorAnalogOutput(world, {2, 64, 2}, detState, Direction::North) >=
         1);

  // Hopper ejecting into MinecartChest on rail at (4, 64, 2).
  world.setBlock({4, 65, 2}, defaultState("hopper").with("facing", "down"), 3);
  auto topHopper = std::dynamic_pointer_cast<HopperBlockEntity>(
      world.containerEntity({4, 65, 2}));
  assert(topHopper != nullptr);
  auto cartAt4 = std::make_shared<MinecartChest>(world, Vec3{4.5, 64.1, 2.5});
  world.addEntity(cartAt4);
  topHopper->setItem(0, itemStack("minecraft:iron_ingot", 2));
  topHopper->setChanged(world);
  for (int i = 0; i < 10; ++i)
    world.tick();
  assert(cartAt4->getContainerItem(0).id == "minecraft:iron_ingot");

  // MinecartHopper sucks items above and activator rail toggles enabled.
  auto hopperCart =
      std::make_shared<MinecartHopper>(world, Vec3{6.5, 64.1, 2.5});
  world.addEntity(hopperCart);
  world.spawnItemAt({6.5, 64.5, 2.5}, "minecraft:gold_ingot", 3, false);
  for (int i = 0; i < 20 && hopperCart->getContainerItem(0).empty(); ++i)
    world.tick();
  assert(hopperCart->getContainerItem(0).id == "minecraft:gold_ingot");
  assert(hopperCart->getContainerItem(0).count == 3);

  world.setBlock({6, 63, 2}, defaultState("redstone_block"), 3);
  world.setBlock({6, 64, 2}, defaultState("activator_rail"), 3);
  world.tick();
  assert(!hopperCart->enabled);

  // MinecartTNT primes on powered activator rail and explodes when falling onto ground.
  auto tntCart = std::make_shared<MinecartTNT>(world, Vec3{6.5, 64.1, 2.5});
  world.addEntity(tntCart);
  world.tick();
  assert(tntCart->isPrimed());
  assert(!tntCart->isRemoved());

  auto fallingTntCart =
      std::make_shared<MinecartTNT>(world, Vec3{12.5, 72.0, 12.5});
  world.addEntity(fallingTntCart);
  for (int i = 0; i < 40 && !fallingTntCart->isRemoved(); ++i)
    world.tick();
  assert(fallingTntCart->isRemoved());

  // MinecartFurnace pushes on fuel and consumes fuel each tick.
  auto furnaceCart =
      std::make_shared<MinecartFurnace>(world, Vec3{1.5, 64.1, 2.5});
  world.addEntity(furnaceCart);
  assert(furnaceCart->addFuel({0.5, 64.0, 2.5}, "minecraft:coal"));
  assert(furnaceCart->fuel == 3600);
  world.tick();
  assert(furnaceCart->fuel == 3599);
  assert(furnaceCart->deltaMovement.x > 0.0);

  // NewMinecartBehavior option runs without throwing and advances minecart along rail.
  world.minecartImprovements = true;
  auto newCart = std::make_shared<Minecart>(world, Vec3{1.5, 64.1, 2.5});
  newCart->deltaMovement = {0.3, 0.0, 0.0};
  world.addEntity(newCart);
  const double oldX = newCart->position.x;
  world.tick();
  assert(newCart->position.x > oldX);
}

void testProjectilesTargetWoodenButtonEnderPearlAndWindCharge() {
  World world;
  world.loadChunk(0, 0);
  world.loadChunk(2, 0);
  buildFloor(world, 63, -8, 48, -8, 16);

  // Arrow hitting Target block sets power > 0 and keeps wooden button pressed.
  world.setBlock({4, 65, 4}, defaultState("target"), 3);
  auto arrow = std::make_shared<Arrow>();
  arrow->setPos(2.5, 65.5, 4.5);
  arrow->deltaMovement = {1.0, 0.0, 0.0};
  world.addEntity(arrow);
  for (int i = 0; i < 5; ++i)
    world.tick();
  assert(propertyValue(world.getBlock({4, 65, 4}), PropertyKind::Power) > 0);
  assert(arrow->isInGround());

  // Wooden button stays pressed past 30 ticks while an arrow overlaps it.
  world.setBlock({6, 64, 6},
                 defaultState("oak_button").with("face", "floor"), 3);
  assert(buttonUse(world, {6, 64, 6}, world.getBlock({6, 64, 6})));
  auto pinArrow = std::make_shared<Arrow>();
  pinArrow->setPos(6.5, 64.2, 6.5);
  pinArrow->setInGround(true);
  world.addEntity(pinArrow);
  for (int i = 0; i < 40; ++i)
    world.tick();
  assert(poweredOf(world.getBlock({6, 64, 6})));
  pinArrow->discard();
  for (int i = 0; i < 35; ++i)
    world.tick();
  assert(!poweredOf(world.getBlock({6, 64, 6})));

  // ThrownEnderpearl invokes enderPearlTicketObserver and teleports owner on hit.
  int ticketCalls = 0;
  int lastTicketCx = -999;
  world.enderPearlTicketObserver = [&](int cx, int cz) -> std::int64_t {
    (void)cz;
    ++ticketCalls;
    lastTicketCx = cx;
    return 40;
  };
  auto owner = std::make_shared<LivingProbeEntity>();
  owner->setPos(0.5, 64.0, 8.5);
  world.addEntity(owner);

  world.setBlock({10, 65, 8}, defaultState("stone"), 3);
  auto pearl = std::make_shared<ThrownEnderpearl>();
  pearl->setOwner(owner.get());
  pearl->setPos(5.5, 65.5, 8.5);
  pearl->deltaMovement = {2.0, 0.0, 0.0};
  world.addEntity(pearl);
  for (int i = 0; i < 10 && !pearl->isRemoved(); ++i)
    world.tick();
  assert(ticketCalls >= 1);
  assert(lastTicketCx == 0);
  assert(pearl->isRemoved());
  assert(owner->position.x > 8.5);
  assert(owner->hurtCalls >= 1);
  assert(owner->lastKind == DamageKind::EnderPearl);

  // WindCharge toggles non-iron door/lever on impact.
  world.setBlock({12, 65, 2}, defaultState("stone"), 3);
  world.setBlock({11, 65, 2},
                 defaultState("lever").with("face", "wall").with("facing", "west"),
                 3);
  assert(!poweredOf(world.getBlock({11, 65, 2})));
  auto wind = std::make_shared<WindCharge>();
  wind->setPos(10.2, 65.5, 2.5);
  wind->deltaMovement = {1.0, 0.0, 0.0};
  world.addEntity(wind);
  for (int i = 0; i < 6 && !wind->isRemoved(); ++i)
    world.tick();
  assert(wind->isRemoved());
  assert(poweredOf(world.getBlock({11, 65, 2})));

  // Dispenser firing arrow spawns Arrow entity and consumes item.
  world.setBlock({0, 65, 0},
                 defaultState("dispenser").with("facing", "east"), 3);
  ensureContainerBlockEntity(world, {0, 65, 0}, world.getBlock({0, 65, 0}));
  auto disp = std::dynamic_pointer_cast<DispenserBlockEntity>(
      world.containerEntity({0, 65, 0}));
  assert(disp != nullptr);
  disp->setItem(0, itemStack("minecraft:arrow", 2));
  const std::size_t beforeCount = world.entities.size();
  world.scheduleTick({0, 65, 0}, world.getBlock({0, 65, 0}).block(), 1);
  world.tick();
  assert(world.entities.size() > beforeCount);
  assert(disp->getItem(0).count == 1);
}

void testBoatsItemFramesEndCrystalAndXpOrbs() {
  World world;
  world.loadChunk(0, 0);
  buildFloor(world, 63);

  // Boat floats in water pool.
  for (int x = 0; x <= 4; ++x)
    for (int z = 0; z <= 4; ++z)
      world.setBlock({x, 64, z}, defaultState("water"), 3);
  auto boat = std::make_shared<Boat>(world, Vec3{2.5, 64.2, 2.5});
  world.addEntity(boat);
  world.tick();
  assert(boat->status() == AbstractBoat::Status::InWater);

  // ItemFrame comparator signal behind a solid stone block.
  world.setBlock({8, 64, 4}, defaultState("stone"), 3);
  world.setBlock({7, 64, 4},
                 defaultState("comparator").with("facing", "east"), 3);
  auto frame =
      std::make_shared<ItemFrame>(Pos{9, 64, 4}, Direction::East, false);
  frame->setFramedItem(world, itemStack("minecraft:diamond", 1));
  world.addEntity(frame);
  assert(frame->getAnalogOutput() == 1);
  frame->rotate(world);
  assert(frame->getAnalogOutput() == 2);
  world.scheduleTick({7, 64, 4}, Blocks::COMPARATOR, 1);
  world.tick();
  world.tick();
  assert(world.comparatorOutput({7, 64, 4}) == 2);

  // EndCrystal explodes when hurt.
  auto crystal = std::make_shared<EndCrystal>(world, Vec3{12.5, 65.0, 12.5});
  world.addEntity(crystal);
  const std::uint64_t explosionsBefore = world.explosionStats.explosions;
  assert(crystal->hurt(world, DamageKind::Attack, 1.0f));
  assert(crystal->isRemoved());
  assert(world.explosionStats.explosions == explosionsBefore + 1);

  // ExperienceOrb merges nearby compatible orbs cleanly.
  auto orb1 = std::make_shared<ExperienceOrb>(world, Vec3{10.5, 64.0, 10.5}, 5);
  auto orb2 = std::make_shared<ExperienceOrb>(world, Vec3{10.6, 64.0, 10.5}, 5);
  orb1->deltaMovement = {};
  orb2->deltaMovement = {};
  world.addEntity(orb1);
  world.addEntity(orb2);
  for (int i = 0; i < 45; ++i)
    world.tick();
  assert(orb1->isRemoved() || orb2->isRemoved());
  if (!orb1->isRemoved())
    assert(orb1->count == 2);
  else
    assert(orb2->count == 2);
}

void testEnderPearlChunkTicketIntegration() {
  struct MinimalStore final : RawChunkStore {
    StoreReadResult readChunk(const ChunkTicketPos &position) override {
      nbt::RootTag root;
      root.name = "";
      root.value.type = 10;
      auto addTag = [](nbt::Tag &parent, std::string name, nbt::Tag val) {
        parent.compoundOrder.push_back(name);
        parent.compound.emplace(std::move(name), std::move(val));
      };
      auto numTag = [](std::uint8_t type, std::int64_t value) {
        nbt::Tag t;
        t.type = type;
        t.number = value;
        return t;
      };
      auto strTag = [](std::string s) {
        nbt::Tag t;
        t.type = 8;
        t.text = std::move(s);
        return t;
      };
      auto listTag = [](std::uint8_t elemType, std::vector<nbt::Tag> elems) {
        nbt::Tag t;
        t.type = 9;
        t.listElementType = elemType;
        t.list = std::move(elems);
        return t;
      };
      addTag(root.value, "DataVersion", numTag(3, 4786));
      addTag(root.value, "xPos", numTag(3, position.x));
      addTag(root.value, "zPos", numTag(3, position.z));
      addTag(root.value, "Status", strTag("minecraft:full"));
      addTag(root.value, "isLightOn", numTag(1, 1));
      nbt::Tag air;
      air.type = 10;
      addTag(air, "Name", strTag("minecraft:air"));
      nbt::Tag states;
      states.type = 10;
      addTag(states, "palette", listTag(10, {std::move(air)}));
      nbt::Tag section;
      section.type = 10;
      addTag(section, "Y", numTag(1, 4));
      addTag(section, "block_states", std::move(states));
      addTag(root.value, "sections", listTag(10, {std::move(section)}));

      StoreReadResult res;
      res.status = StoreReadStatus::Found;
      res.nbt = nbt::writeRoot(root);
      return res;
    }
    StoreWriteResult writeChunk(const ChunkTicketPos &,
                                std::span<const std::uint8_t>, int) override {
      StoreWriteResult res;
      res.wrote = true;
      return res;
    }
  };

  World world;
  auto store = std::make_shared<MinimalStore>();
  WorldChunkManager wcm(
      world, store, 3, 10,
      [](const ChunkTicketPos &) {
        return EntityChunkDataResult{EntityChunkDataResult::Status::Empty, {}};
      });
  wcm.addPlayer(1, ChunkTicketPos{0, 0});
  for (int i = 0; i < 40 && !world.entityTickPositionReady(Pos{2, 70, 2}); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    wcm.pump();
  }
  assert(world.entityTickPositionReady(Pos{2, 70, 2}));

  auto owner = std::make_shared<LivingProbeEntity>();
  owner->setPos(0.5, 64.0, 0.5);
  world.addEntity(owner);

  auto pearl = std::make_shared<ThrownEnderpearl>();
  pearl->setOwner(owner.get());
  pearl->setPos(2.5, 70.0, 2.5);
  pearl->deltaMovement = {0.1, 0.0, 0.0};
  world.addEntity(pearl);
  world.tick();

  // Removing the player leaves chunk (0, 0) entity-ticking via EnderPearl ticket.
  wcm.removePlayer(1);
  const auto snapshot = wcm.ticketSources().query(ChunkTicketPos{0, 0});
  assert(snapshot.entityTickingDemand);
  assert(snapshot.loadingLevel <= ticket_levels::EntityTickingLevel);
}
} // namespace

int main() {
  testFallingBlockConcreteAnvilAndBlockData();
  testMinecartVariantsAndHopperInteraction();
  testProjectilesTargetWoodenButtonEnderPearlAndWindCharge();
  testBoatsItemFramesEndCrystalAndXpOrbs();
  testEnderPearlChunkTicketIntegration();
  return 0;
}
