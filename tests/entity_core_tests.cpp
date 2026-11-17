#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include "redstone/entity/living_entity.hpp"
#include "redstone/entity/player.hpp"
#include "redstone/entity/vehicles.hpp"

#include <cassert>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace redstone;

class StepProbeEntity final : public Entity {
public:
  StepProbeEntity() : Entity("minecraft:player") {}
  float stepHeight = 0.6f;
  bool solidCollider = false;
  bool blockOnTick = false;

  float maxUpStep() const override { return stepHeight; }
  bool canBeCollidedWith() const override { return solidCollider; }
  EntityDimensions getDimensions(Pose p) const override {
    if (p == Pose::Crouching)
      return EntityDimensions::scalable(0.6f, 1.5f).withEyeHeight(1.27f);
    return Entity::getDimensions(p);
  }
  void tick(World &world) override { (void)world; }
};

void testEntityTypeRegistry() {
  const auto &all = entityRegistry();
  assert(all.size() == 157);
  std::set<int> protocolIds;
  for (const auto &entry : all) {
    assert(entry.protocolId >= 0 && entry.protocolId < 157);
    assert(protocolIds.insert(entry.protocolId).second);
    assert(entityTypeByProtocolId(entry.protocolId) == &entry);
    assert(entityType(entry.name) == &entry);
  }
  const auto *player = entityType("minecraft:player");
  assert(player && player->protocolId == 155);
  assert(player->eyeHeight > 1.5 && player->height > 1.7);
  const auto *tnt = entityType("minecraft:tnt");
  assert(tnt && tnt->simulated && tnt->protocolId == 132);
  const auto *boat = entityType("minecraft:oak_boat");
  assert(boat && boat->width > 1.3f && boat->height > 0.5f);
}

void testSynchedEntityDataAndPose() {
  StepProbeEntity probe;
  assert(probe.pose() == Pose::Standing);
  assert(std::abs(probe.dimensions().height - 1.8f) < 1e-4f);

  const std::uint64_t r0 = probe.entityData().revision();
  probe.setCustomName(std::string("Alice"));
  probe.setPose(Pose::Crouching);
  probe.refreshDimensions();
  assert(probe.dimensions().height < 1.6f);

  const auto dirtyS0 = probe.entityData().dirtySince(r0);
  assert(dirtyS0.size() == 2);
  const auto nonDefault = probe.entityData().getNonDefaultValues();
  assert(nonDefault.size() == 2);

  const auto packed = probe.entityData().packDirty();
  assert(packed.size() == 2);
  assert(probe.entityData().packDirty().empty());
  // dirtySince continues to serve independent sessions even after packDirty().
  assert(probe.entityData().dirtySince(r0).size() == 2);
}

void testStepUpSupportAndEntityCollisions() {
  World world;
  world.loadChunk(0, 0);
  world.loadChunk(1, 0);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      world.setBlock({x, 63, z}, defaultState("stone"), 0);
  // Half slab step at x = 6, z = 4, y = 64 (height 0.5).
  BlockState slab = defaultState("stone_slab").with("type", "bottom");
  world.setBlock({6, 64, 4}, slab, 0);

  auto walker = std::make_shared<StepProbeEntity>();
  walker->stepHeight = 0.6f;
  walker->setPos(5.2, 64.0, 4.5);
  world.addEntity(walker);

  walker->move(world, MoverType::Self, {0.0, -0.08, 0.0});
  assert(walker->onGround);
  walker->move(world, MoverType::Self, {1.0, -0.08, 0.0});
  assert(std::abs(walker->position.y - 64.5) < 1e-4);
  assert(walker->position.x > 5.9);
  assert(walker->onGround);
  assert(walker->mainSupportingBlockPos.has_value());
  assert(*walker->mainSupportingBlockPos == (Pos{6, 64, 4}));

  // Same setup with maxUpStep == 0 fails to climb the slab.
  auto nonStepper = std::make_shared<StepProbeEntity>();
  nonStepper->stepHeight = 0.0f;
  nonStepper->setPos(5.2, 64.0, 8.5);
  world.setBlock({6, 64, 8}, slab, 0);
  world.addEntity(nonStepper);
  nonStepper->move(world, MoverType::Self, {0.0, -0.08, 0.0});
  nonStepper->move(world, MoverType::Self, {1.0, 0.0, 0.0});
  assert(std::abs(nonStepper->position.y - 64.0) < 1e-4);
  assert(nonStepper->horizontalCollision);

  // Solid entity collision blocks World::move / getEntityCollisions.
  auto blocker = std::make_shared<StepProbeEntity>();
  blocker->solidCollider = true;
  blocker->setPos(10.0, 64.0, 4.5);
  world.addEntity(blocker);

  auto mover = std::make_shared<StepProbeEntity>();
  mover->stepHeight = 0.0f;
  mover->setPos(9.0, 64.0, 4.5);
  world.addEntity(mover);
  assert(!world.getEntityCollisions(mover.get(),
                                    mover->boundingBox().move(1.0, 0.0, 0.0))
              .empty());
  mover->move(world, MoverType::Self, {2.0, 0.0, 0.0});
  assert(mover->position.x < 9.5);
  assert(mover->horizontalCollision);
}

void testFluidInteraction() {
  World world;
  world.loadChunk(0, 0);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      world.setBlock({x, 63, z}, defaultState("stone"), 0);

  // Create a 3-block flowing water stream along +X.
  world.setBlock({4, 64, 4}, defaultState("water"), 0);
  world.setBlock({5, 64, 4}, defaultState("water").with("level", "1"), 0);
  world.setBlock({6, 64, 4}, defaultState("water").with("level", "2"), 0);

  auto swimmer = std::make_shared<StepProbeEntity>();
  swimmer->setPos(5.5, 64.0, 4.5);
  world.addEntity(swimmer);

  assert(swimmer->updateFluidInteraction(world));
  assert(swimmer->isInWater());
  assert(swimmer->fluidHeight(false) > 0.5);
  assert(swimmer->deltaMovement.x > 0.0);
  assert(!swimmer->isEyeInWater());

  // Submerged 2 blocks deep -> eyes in water.
  world.setBlock({8, 64, 8}, defaultState("water"), 0);
  world.setBlock({8, 65, 8}, defaultState("water"), 0);
  world.setBlock({8, 66, 8}, defaultState("water"), 0);
  swimmer->setPos(8.5, 64.0, 8.5);
  assert(swimmer->updateFluidInteraction(world));
  assert(swimmer->isEyeInWater());
}

void testSweptInsideBlockEffects() {
  World world;
  world.loadChunk(0, 0);
  world.loadChunk(1, 0);
  for (int x = 0; x < 24; ++x)
    for (int z = 0; z < 16; ++z)
      world.setBlock({x, 63, z}, defaultState("stone"), 0);

  // Place fire at x=4 and water at x=8; move from x=2.5 to x=10.5 in one step.
  world.setBlock({4, 64, 4}, defaultState("fire"), 0);
  world.setBlock({8, 64, 4}, defaultState("water"), 0);
  auto probe = std::make_shared<StepProbeEntity>();
  probe->setPos(2.5, 64.0, 4.5);
  world.addEntity(probe);

  // Fire ignite swept-only test: pass through x=4 without hitting x=8.
  probe->applyEffectsFromBlocks(world, {2.5, 64.0, 4.5}, {6.5, 64.0, 4.5});
  assert(probe->remainingFireTicks > 0);

  // Full sweep from x=2.5 to x=10.5 hits fire at 4, then water at 8 -> extinguished!
  probe->remainingFireTicks = 0;
  probe->applyEffectsFromBlocks(world, {2.5, 64.0, 4.5}, {10.5, 64.0, 4.5});
  assert(probe->remainingFireTicks <= 0);

  // Powder snow freezes and extinguishes; cobweb applies stuck slowdown.
  world.setBlock({12, 64, 4}, defaultState("powder_snow"), 0);
  probe->igniteForTicks(40);
  probe->applyEffectsFromBlocks(world, {10.5, 64.0, 4.5}, {13.5, 64.0, 4.5});
  assert(probe->remainingFireTicks <= 0);
  assert(probe->isInPowderSnow);
  assert(probe->ticksFrozen() > 0);
}

void testRidingAndVehicleTree() {
  World world;
  world.loadChunk(0, 0);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      world.setBlock({x, 63, z}, defaultState("stone"), 0);
  for (int x = 2; x <= 10; ++x)
    world.setBlock({x, 64, 4}, defaultState("rail").with("shape", "east_west"), 0);

  world.spawnMinecart({3.5, 64.0625, 4.5});
  auto cart = world.entities.back();
  auto rider = std::make_shared<StepProbeEntity>();
  rider->setPos(3.5, 64.0, 4.5);
  world.addEntity(rider);

  // Cannot ride oneself or form a cycle.
  assert(!rider->startRiding(*rider, true, false));
  assert(rider->startRiding(*cart, true, false));
  assert(!cart->startRiding(*rider, true, false));
  assert(rider->isPassenger());
  assert(cart->isVehicle());
  assert(rider->getVehicle().get() == cart.get());
  assert(&rider->getRootVehicle() == cart.get());

  // Moving the cart and ticking the world moves the passenger too.
  cart->setDeltaMovement(0.3, 0.0, 0.0);
  const double startX = cart->position.x;
  world.tick();
  assert(cart->position.x > startX);
  assert(std::abs(rider->position.x - cart->position.x) < 1e-6);
  assert(std::abs(rider->position.z - cart->position.z) < 1e-6);

  rider->stopRiding();
  assert(!rider->isPassenger());
  assert(!cart->isVehicle());
}

void testEntitiesInPreservesSectionInsertionOrder() {
  World world;
  world.loadChunk(0, 0);
  world.loadChunk(1, 0);
  auto e1 = std::make_shared<StepProbeEntity>();
  e1->setPos({2.5, 64.0, 2.5});
  auto e2 = std::make_shared<StepProbeEntity>();
  e2->setPos({3.0, 64.0, 2.5});
  world.addEntity(e1);
  world.addEntity(e2);
  assert(e1->id < e2->id);

  // Move e1 out of section (0,4,0) and back in so e1 sits at the tail of that
  // section's insertion list after e2.
  const Vec3 p0 = e1->position;
  const Vec3 otherSection{18.5, 64.0, 2.5};
  e1->setPos(otherSection);
  world.synchronizeEntityAfterMove(*e1, p0);
  e1->setPos(p0);
  world.synchronizeEntityAfterMove(*e1, otherSection);

  const auto found = world.entitiesIn({1.0, 63.0, 1.0, 5.0, 67.0, 5.0});
  assert(found.size() == 2);
  assert(found[0] == e2.get());
  assert(found[1] == e1.get());
}

void assertBucketInvariants(const EntitySectionBucket &bucket) {
  assert(bucket.entities.size() == bucket.boxes.size());
  for (std::size_t i = 0; i < bucket.entities.size(); ++i) {
    const Entity *e = bucket.entities[i];
    assert(e != nullptr);
    assert(e->entitySectionIndex == i);
    const Aabb expected = e->boundingBox();
    const Aabb &cached = bucket.boxes[i];
    assert(cached.minX == expected.minX && cached.minY == expected.minY &&
           cached.minZ == expected.minZ && cached.maxX == expected.maxX &&
           cached.maxY == expected.maxY && cached.maxZ == expected.maxZ);
    assert(bucket.active.test(i) == !e->isRemoved());
    assert(bucket.pushable.test(i) == e->isPushable());
    assert(bucket.hardCollider.test(i) == e->canBeCollidedWith());
    assert(bucket.minecart.test(i) == e->isMinecart());
    assert(bucket.item.test(i) == e->isItemEntity());
  }
}

void testEntitySectionBucketBoxOrderAndBitmaps() {
  World world;
  world.loadChunk(0, 0);
  // Fill one section with > 64 entities so BitPlane spills into overflowWords
  // and later shrinks back cleanly when entities are discarded.
  std::vector<std::shared_ptr<Entity>> created;
  for (int i = 0; i < 66; ++i) {
    auto item = std::make_shared<ItemEntity>(
        world, Vec3{2.0 + (i % 8) * 0.5, 65.0, 2.0 + (i / 8) * 0.5},
        ItemStack{"minecraft:stone", 1});
    world.addEntity(item);
    created.push_back(item);
  }
  auto walker = std::make_shared<StepProbeEntity>();
  walker->setPos(4.5, 65.0, 4.5);
  world.addEntity(walker);
  created.push_back(walker);

  auto cart = std::make_shared<Minecart>(world, Vec3{6.5, 65.0, 6.5});
  world.addEntity(cart);
  created.push_back(cart);

  auto stand = std::make_shared<ArmorStand>(world, Vec3{8.5, 65.0, 8.5});
  world.addEntity(stand);
  created.push_back(stand);

  auto boat =
      std::make_shared<Boat>(world, Vec3{10.5, 65.0, 10.5}, "minecraft:oak_boat",
                             "minecraft:oak_boat");
  world.addEntity(boat);
  created.push_back(boat);

  const std::uint64_t key = walker->entitySection;
  const EntitySectionBucket *bucket = world.findEntitySectionBucket(key);
  assert(bucket != nullptr);
  assert(bucket->size() == 70);
  assert(!bucket->active.overflowWords.empty());
  assertBucketInvariants(*bucket);

  // Same-section setPos() updates cached box in-place immediately.
  cart->setPos(7.5, 65.0, 6.5);
  assert(bucket->boxes[cart->entitySectionIndex].minX ==
         cart->boundingBox().minX);
  assert(!world.getEntityCollisions(
                  nullptr, {7.0, 64.8, 6.0, 8.0, 65.9, 7.0})
              .empty());
  assert(world.getEntityCollisions(
                 nullptr, {6.0, 64.8, 6.0, 7.0, 65.9, 7.0})
             .empty());

  // refreshDimensions() updates cached box height immediately in-place.
  walker->setPose(Pose::Crouching);
  walker->refreshDimensions();
  assert(std::abs(bucket->boxes[walker->entitySectionIndex].getYsize() - 1.5) <
         1e-5);
  assertBucketInvariants(*bucket);

  // Remove entities from index 10..15 to cross 70 -> 64 (overflow -> inline)
  // and verify order and indices stay compact.
  for (int i = 10; i < 16; ++i)
    created[static_cast<std::size_t>(i)]->discard();
  // Before tickEntities cleanup, active bitplane is already cleared!
  for (int i = 10; i < 16; ++i)
    assert(!bucket->active.test(created[static_cast<std::size_t>(i)]
                                    ->entitySectionIndex));
  world.tickEntities();
  bucket = world.findEntitySectionBucket(key);
  assert(bucket != nullptr);
  assert(bucket->size() == 64);
  assert(bucket->active.overflowWords.empty());
  assertBucketInvariants(*bucket);
}

class PushProbeLiving final : public LivingEntity {
public:
  PushProbeLiving() : LivingEntity("minecraft:zombie") {}
  using LivingEntity::pushEntities;
};

void testNonPushableFilteringAndSameTickDiscard() {
  World world;
  world.loadChunk(0, 0);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      world.setBlock({x, 63, z}, defaultState("stone"), 0);

  auto mobA = std::make_shared<PushProbeLiving>();
  mobA->setPos(4.5, 64.0, 4.5);
  auto mobB = std::make_shared<PushProbeLiving>();
  mobB->setPos(4.7, 64.0, 4.5);
  auto item = std::make_shared<ItemEntity>(world, Vec3{4.6, 64.0, 4.5},
                                           ItemStack{"minecraft:iron_ingot", 1});
  auto tnt = std::make_shared<PrimedTnt>(world, Vec3{4.6, 64.0, 4.5});
  auto stand = std::make_shared<ArmorStand>(world, Vec3{4.6, 64.0, 4.5});
  auto deadMob = std::make_shared<PushProbeLiving>();
  deadMob->setPos(4.6, 64.0, 4.5);
  auto player = std::make_shared<ServerPlayer>(world, Vec3{4.6, 64.0, 4.5});

  world.addEntity(mobA);
  world.addEntity(mobB);
  world.addEntity(item);
  world.addEntity(tnt);
  world.addEntity(stand);
  world.addEntity(deadMob);
  world.addEntity(player);

  deadMob->setHealth(0.0f);
  player->setSpectator(true);
  item->setDeltaMovement(0.0, 0.0, 0.0);
  tnt->setDeltaMovement(0.0, 0.0, 0.0);

  const EntitySectionBucket *bucket =
      world.findEntitySectionBucket(mobA->entitySection);
  assert(bucket != nullptr);
  // Only mobA and mobB are pushable among the 7 overlapping entities.
  assert(bucket->pushable.count == 2);
  assertBucketInvariants(*bucket);

  mobA->pushEntities(world);
  assert(item->deltaMovement.lengthSqr() == 0.0);
  assert(tnt->deltaMovement.lengthSqr() == 0.0);
  assert(stand->deltaMovement.lengthSqr() == 0.0);
  assert(deadMob->deltaMovement.lengthSqr() == 0.0);
  assert(player->deltaMovement.lengthSqr() == 0.0);
  assert(mobA->deltaMovement.lengthSqr() > 0.0);
  assert(mobB->deltaMovement.lengthSqr() > 0.0);

  // Toggling player out of spectator restores pushable bit immediately.
  player->setSpectator(false);
  assert(bucket->pushable.count == 3);
  assert(bucket->pushable.test(player->entitySectionIndex));

  // Mid-tick discard of a hard collider immediately hides it before cleanup.
  auto boat =
      std::make_shared<Boat>(world, Vec3{8.5, 65.0, 8.5}, "minecraft:oak_boat",
                             "minecraft:oak_boat");
  world.addEntity(boat);
  const Aabb boatQuery = boat->boundingBox().inflate(0.1, -0.01, 0.1);
  assert(!world.noCollision(nullptr, boatQuery));
  boat->discard();
  assert(world.getEntityCollisions(nullptr, boatQuery).empty());
  assert(world.noCollision(nullptr, boatQuery));

  // Verify Minecart bitmap filter + hasCustomEntityCollision vs normal entity.
  auto cart = std::make_shared<Minecart>(world, Vec3{11.5, 64.0625, 11.5});
  auto cartStand = std::make_shared<ArmorStand>(world, Vec3{11.6, 64.0, 11.5});
  world.addEntity(cart);
  world.addEntity(cartStand);
  assert(cart->deltaMovement.lengthSqr() == 0.0);
  cartStand->pushEntities(world);
  assert(cart->deltaMovement.lengthSqr() > 0.0);
}

} // namespace

int main() {
  testEntityTypeRegistry();
  testSynchedEntityDataAndPose();
  testStepUpSupportAndEntityCollisions();
  testFluidInteraction();
  testSweptInsideBlockEffects();
  testRidingAndVehicleTree();
  testEntitiesInPreservesSectionInsertionOrder();
  testEntitySectionBucketBoxOrderAndBitmaps();
  testNonPushableFilteringAndSameTickDiscard();
  return 0;
}
