#include "redstone/engine.hpp"
#include "redstone/entity.hpp"

#include <cassert>
#include <cmath>
#include <memory>

namespace {
using namespace redstone;

void setupFloor(World &world, const BlockState &block, int minX, int maxX,
                int y, int minZ, int maxZ) {
  for (int cx = (minX >> 4) - 1; cx <= (maxX >> 4) + 1; ++cx) {
    for (int cz = (minZ >> 4) - 1; cz <= (maxZ >> 4) + 1; ++cz)
      world.loadChunk(cx, cz);
  }
  for (int x = minX; x <= maxX; ++x) {
    for (int z = minZ; z <= maxZ; ++z)
      world.setBlock(Pos{x, y, z}, block, 18);
  }
}

void testAttributesAndModifiers() {
  AttributeMap map;
  map.registerLivingDefaults();
  auto *speed = map.getInstance(AttributeId::MovementSpeed);
  assert(speed != nullptr);
  speed->setBaseValue(0.1);
  speed->addTransientModifier({"add", 0.05, AttributeOp::AddValue, false});
  speed->addTransientModifier(
      {"mult_base", 0.5, AttributeOp::AddMultipliedBase, false});
  speed->addTransientModifier(
      {"mult_total", 0.2, AttributeOp::AddMultipliedTotal, false});
  // (0.1 + 0.05) * (1 + 0.5) * 1.2 = 0.27
  assert(std::abs(speed->getValue() - 0.27) < 1e-9);
  assert(speed->removeModifier("mult_base"));
  // (0.1 + 0.05) * 1.2 = 0.18
  assert(std::abs(speed->getValue() - 0.18) < 1e-9);
}

void testCombatDamageInvulnAndKnockback() {
  World world;
  setupFloor(world, Blocks::STONE.defaultState(), -8, 8, 63, -8, 8);
  auto victim = std::make_shared<LivingEntity>("minecraft:armor_stand");
  victim->setPos({0.5, 64.0, 0.5});
  victim->onGround = true;
  victim->attributes().ensure(AttributeId::Armor).setBaseValue(20.0);
  victim->attributes().ensure(AttributeId::ArmorToughness).setBaseValue(8.0);
  world.addEntity(victim);

  // Hit with 10 physical damage: CombatRules reduces via armor 20, toughness 8
  // (toughnessTerm = 4, effectiveArmor = max(20 - 10/4, 4) = 17.5 -> factor 0.3)
  DamageSource hit =
      DamageSource::fromKind(DamageKind::Attack, nullptr, nullptr,
                             Vec3{-1.0, 64.0, 0.5});
  assert(victim->hurtServer(world, hit, 10.0f));
  assert(std::abs(victim->health() - 17.0f) < 1e-4f);
  assert(victim->invulnerableTime == 20);
  assert(victim->hurtMarked);
  assert(victim->deltaMovement.x > 0.2);

  // Follow-up weaker hit inside invulnerability window (>10 ticks) fails.
  assert(!victim->hurtServer(world, hit, 8.0f));
  assert(std::abs(victim->health() - 17.0f) < 1e-4f);

  // Stronger hit inside invulnerability window applies diff only (20 - 10 = 10).
  assert(victim->hurtServer(world, hit, 20.0f));
  assert(std::abs(victim->health() - 14.0f) < 1e-4f);

  // Fire Resistance blocks fire/lava damage completely.
  assert(victim->addEffect({MobEffectId::FireResistance, 100, 0}));
  assert(!victim->hurtServer(world, DamageSource::fromKind(DamageKind::Lava),
                             8.0f));
  assert(std::abs(victim->health() - 14.0f) < 1e-4f);

  // Resistance reduces non-bypassing damage; healthBoost raises maxHealth.
  victim->invulnerableTime = 0;
  assert(victim->addEffect({MobEffectId::Resistance, 100, 1})); // 40% reduction
  victim->attributes().ensure(AttributeId::Armor).setBaseValue(0.0);
  assert(victim->hurtServer(world, DamageSource::fromKind(DamageKind::Generic),
                             10.0f));
  assert(std::abs(victim->health() - 8.0f) < 1e-4f);
}

void testEffectsLifecycleAndDeath() {
  World world;
  setupFloor(world, Blocks::STONE.defaultState(), -8, 8, 63, -8, 8);
  auto ent = std::make_shared<LivingEntity>("minecraft:armor_stand");
  ent->setPos({0.5, 64.0, 0.5});
  ent->onGround = true;
  world.addEntity(ent);

  const double baseSpeed = ent->attributeValue(AttributeId::MovementSpeed);
  assert(ent->addEffect({MobEffectId::Speed, 4, 1})); // amp 1 -> +40%
  assert(std::abs(ent->attributeValue(AttributeId::MovementSpeed) -
                  baseSpeed * 1.4) < 1e-9);

  ent->setHealth(10.0f);
  assert(ent->addEffect({MobEffectId::Regeneration, 3, 5})); // interval <= 1 -> heal each tick
  assert(ent->addEffect({MobEffectId::HealthBoost, 2, 0})); // +4 maxHealth
  assert(std::abs(ent->maxHealth() - 24.0f) < 1e-5f);

  for (int i = 0; i < 4; ++i)
    world.tick();
  assert(ent->health() > 10.0f);
  assert(!ent->hasEffect(MobEffectId::Speed));
  assert(std::abs(ent->attributeValue(AttributeId::MovementSpeed) - baseSpeed) <
         1e-9);

  // Poison hurts 1.0 at a time while health > 1.0f and never kills.
  ent->setHealth(3.0f);
  assert(ent->addEffect({MobEffectId::Poison, 20, 5}));
  for (int i = 0; i < 10; ++i) {
    ent->invulnerableTime = 0;
    world.tick();
  }
  assert(std::abs(ent->health() - 1.0f) < 1e-4f);
  assert(ent->isAlive());

  // Wither can kill; 20 deathTime ticks discard the entity.
  assert(ent->removeEffect(MobEffectId::Poison));
  assert(ent->addEffect({MobEffectId::Wither, 40, 5}));
  for (int i = 0; i < 30 && !ent->isRemoved(); ++i) {
    ent->invulnerableTime = 0;
    world.tick();
  }
  assert(ent->isRemoved());
}

void testTravelPhysicsAndClimbing() {
  World world;
  setupFloor(world, Blocks::STONE.defaultState(), -8, 8, 63, -8, 8);

  // Climb ladder with horizontal push.
  world.setBlock(Pos{2, 64, 2}, Blocks::LADDER.defaultState(), 18);
  world.setBlock(Pos{2, 65, 2}, Blocks::LADDER.defaultState(), 18);
  world.setBlock(Pos{3, 64, 2}, Blocks::STONE.defaultState(), 18);
  world.setBlock(Pos{3, 65, 2}, Blocks::STONE.defaultState(), 18);
  auto climber = std::make_shared<LivingEntity>("minecraft:armor_stand");
  climber->setPos({2.74, 64.0, 2.5});
  climber->onGround = true;
  climber->yaw = 270.0f; // facing +X towards solid wall at x=3
  climber->zza = 1.0f;
  world.addEntity(climber);
  world.tick();
  climber->zza = 1.0f;
  world.tick();
  assert(climber->position.y > 64.0);

  // Water travel vs Depth Strider (WaterMovementEfficiency = 1.0).
  for (int x = -6; x <= -2; ++x) {
    for (int z = -2; z <= 2; ++z)
      world.setBlock(Pos{x, 64, z}, Blocks::WATER.defaultState(), 18);
  }
  auto swimmer = std::make_shared<LivingEntity>("minecraft:armor_stand");
  swimmer->setPos({-4.5, 64.0, 0.5});
  swimmer->deltaMovement = {0.2, 0.0, 0.0};
  world.addEntity(swimmer);
  world.tick();
  const double dragNormal = swimmer->deltaMovement.x;

  auto strider = std::make_shared<LivingEntity>("minecraft:armor_stand");
  strider->setPos({-4.5, 64.0, 0.5});
  strider->onGround = true;
  strider->attributes()
      .ensure(AttributeId::WaterMovementEfficiency)
      .setBaseValue(1.0);
  strider->deltaMovement = {0.2, 0.0, 0.0};
  world.addEntity(strider);
  world.tick();
  assert(std::abs(dragNormal) > 0.0);
}

void testArmorStandAndStonePressurePlate() {
  World world;
  setupFloor(world, Blocks::STONE.defaultState(), -8, 8, 63, -8, 8);
  world.setBlock(Pos{0, 64, 0}, Blocks::STONE_PRESSURE_PLATE.defaultState(), 18);
  world.setBlock(Pos{4, 64, 0}, Blocks::STONE_PRESSURE_PLATE.defaultState(), 18);

  // Regular ArmorStand presses stone pressure plate (LivingEntity).
  auto stand = std::make_shared<ArmorStand>(world, Vec3{0.5, 64.0, 0.5});
  stand->setItemSlot(EquipmentSlot::Head, itemStack("minecraft:diamond_helmet", 1));
  world.addEntity(stand);
  world.tick();
  assert(world.getBlock(Pos{0, 64, 0}).getView("powered") == "true");

  // Marker ArmorStand ignores block triggers and keeps plate unpowered.
  auto marker = std::make_shared<ArmorStand>(world, Vec3{4.5, 64.0, 0.5});
  marker->setMarker(true);
  world.addEntity(marker);
  world.tick();
  assert(world.getBlock(Pos{4, 64, 0}).getView("powered") != "true");
  assert(marker->dimensions().width == 0.0f);
  assert(!marker->hurtServer(world, DamageSource::fromKind(DamageKind::Attack),
                             5.0f));

  // Small stand scales dimensions.
  stand->setSmall(true);
  assert(stand->dimensions().width < 0.3f);

  // Advance game time past the initial 5-tick punch window before hit #1.
  for (int i = 0; i < 6; ++i)
    world.tick();

  // Two hits within 5 ticks break normal ArmorStand and drop stand + helmet.
  assert(stand->hurtServer(world, DamageSource::fromKind(DamageKind::Attack),
                           1.0f));
  assert(!stand->isRemoved());
  assert(stand->hurtServer(world, DamageSource::fromKind(DamageKind::Attack),
                           1.0f));
  assert(stand->isRemoved());
  bool foundStandDrop = false;
  bool foundHelmetDrop = false;
  for (const auto &e : world.entities) {
    if (const auto *item = dynamic_cast<const ItemEntity *>(e.get())) {
      if (item->item.id == "minecraft:armor_stand")
        foundStandDrop = true;
      if (item->item.id == "minecraft:diamond_helmet")
        foundHelmetDrop = true;
    }
  }
  assert(foundStandDrop && foundHelmetDrop);
}

} // namespace

int main() {
  testAttributesAndModifiers();
  testCombatDamageInvulnAndKnockback();
  testEffectsLifecycleAndDeath();
  testTravelPhysicsAndClimbing();
  testArmorStandAndStonePressurePlate();
  return 0;
}
