#include "redstone/entity.hpp"

namespace redstone {

ArmorStand::ArmorStand(World &world, Vec3 pos)
    : LivingEntity("minecraft:armor_stand") {
  (void)world;
  entityData_.define(kDataClientFlagsId, EntityDataSerializer::Byte,
                     std::int8_t{0});
  entityData_.define(kDataHeadPoseId, EntityDataSerializer::Rotations,
                     headPose_);
  entityData_.define(kDataBodyPoseId, EntityDataSerializer::Rotations,
                     bodyPose_);
  entityData_.define(kDataLeftArmPoseId, EntityDataSerializer::Rotations,
                     leftArmPose_);
  entityData_.define(kDataRightArmPoseId, EntityDataSerializer::Rotations,
                     rightArmPose_);
  entityData_.define(kDataLeftLegPoseId, EntityDataSerializer::Rotations,
                     leftLegPose_);
  entityData_.define(kDataRightLegPoseId, EntityDataSerializer::Rotations,
                     rightLegPose_);
  attributes_.ensure(AttributeId::StepHeight).setBaseValue(0.0);
  refreshDimensions();
  setPos(pos);
}

void ArmorStand::setClientFlagBit(std::int8_t bit, bool value) {
  const std::int8_t cur = entityData_.get<std::int8_t>(kDataClientFlagsId);
  const std::int8_t next =
      value ? static_cast<std::int8_t>(cur | bit)
            : static_cast<std::int8_t>(cur & ~bit);
  entityData_.set(kDataClientFlagsId, next);
}

void ArmorStand::setSmall(bool value) {
  setClientFlagBit(kClientFlagSmall, value);
  refreshDimensions();
}

void ArmorStand::setShowArms(bool value) {
  setClientFlagBit(kClientFlagShowArms, value);
}

void ArmorStand::setNoBasePlate(bool value) {
  setClientFlagBit(kClientFlagNoBasePlate, value);
}

void ArmorStand::setMarker(bool value) {
  setClientFlagBit(kClientFlagMarker, value);
  noPhysics = !hasPhysics();
  refreshDimensions();
}

EntityDimensions ArmorStand::getDimensions(Pose) const {
  if (isMarker())
    return {0.0f, 0.0f, 0.0f, true};
  const EntityDimensions base = type_->dimensions();
  if (isSmall())
    return {base.width * 0.5f, base.height * 0.5f, 0.9875f, base.fixed};
  return base;
}

void ArmorStand::travel(World &world, Vec3 input) {
  if (!hasPhysics())
    return;
  LivingEntity::travel(world, input);
}

void ArmorStand::pushEntities(World &world) {
  world.forEachEntityInFlags(
      boundingBox(), static_cast<std::uint8_t>(EntitySectionFlag::Minecart),
      [&](Entity &entity) {
        if (&entity == this || entity.isRemoved())
          return;
        if (entity.isMinecart() && distanceToSqr(entity.position) <= 0.2)
          entity.push(*this);
      });
}

void ArmorStand::causeDamage(World &world, const DamageSource &, float dmg) {
  const float next = health() - dmg;
  if (next <= 0.5f) {
    brokenByAnything(world);
    kill(world);
  } else {
    setHealth(next);
  }
}

void ArmorStand::brokenByPlayer(World &world) {
  ItemStack drop = itemStack("minecraft:armor_stand", 1);
  spawnAtLocation(world, drop);
  brokenByAnything(world);
}

void ArmorStand::brokenByAnything(World &world) {
  for (EquipmentSlot slot :
       {EquipmentSlot::MainHand, EquipmentSlot::OffHand, EquipmentSlot::Feet,
        EquipmentSlot::Legs, EquipmentSlot::Chest, EquipmentSlot::Head,
        EquipmentSlot::Body, EquipmentSlot::Saddle}) {
    const auto &stack = getItemBySlot(slot);
    if (!stack.empty()) {
      auto item = std::make_shared<ItemEntity>(
          world, position + Vec3{0.0, 1.0, 0.0}, stack);
      item->pickupDelay = 10;
      world.addEntity(item);
      setItemSlot(slot, {});
    }
  }
}

bool ArmorStand::hurtServer(World &world, const DamageSource &source,
                            float amount) {
  (void)amount;
  if (isRemoved())
    return false;
  if (source.bypassesInvulnerability) {
    kill(world);
    return false;
  }
  if (standInvisible || isMarker())
    return false;
  if (source.isExplosion) {
    brokenByAnything(world);
    kill(world);
    return false;
  }
  if (source.kind == DamageKind::InWall)
    return false;
  if (source.isFire) {
    if (source.kind == DamageKind::Lava) {
      if (health() > 0.5f)
        causeDamage(world, source, 4.0f);
    } else if (isOnFire()) {
      causeDamage(world, source, 0.15f);
    } else {
      igniteForSeconds(5.0f);
    }
    return false;
  }
  const bool allowBreak = source.canBreakArmorStand;
  const bool alwaysKills = source.alwaysKillsArmorStands;
  if (!allowBreak && !alwaysKills)
    return false;
  if (source.isCreativePlayer) {
    kill(world);
    return true;
  }
  const std::int64_t time = world.gameTime();
  if (time - lastHit > 5 && !alwaysKills) {
    lastHit = time;
  } else {
    brokenByPlayer(world);
    kill(world);
  }
  return true;
}

void ArmorStand::syncDataFields() {
  LivingEntity::syncDataFields();
  setInvisible(standInvisible);
}

} // namespace redstone
