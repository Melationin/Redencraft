#include "entity/internal.hpp"
#include "redstone/item.hpp"

namespace redstone {
ItemEntity::ItemEntity(World &world, Vec3 pos, ItemStack stack)
    : ItemEntity(world, pos, std::move(stack),
                 Vec3{world.random.nextDouble() * 0.2 - 0.1, 0.2,
                      world.random.nextDouble() * 0.2 - 0.1}) {}
ItemEntity::ItemEntity(World &world, Vec3 pos, ItemStack stack, Vec3 motion)
    : Entity("minecraft:item"), item(std::move(stack)) {
  entityData_.define(kDataItemId, EntityDataSerializer::ItemStack, item);
  setPos(pos);
  setDeltaMovement(motion);
  yaw = world.random.nextFloat() * 360.0f;
  bobOffs = world.random.nextFloat() * static_cast<float>(std::numbers::pi) *
            2.0f;
  oldPosition = position;
}
void ItemEntity::syncDataFields() {
  entityData_.set(kDataItemId, item);
}
void ItemEntity::setFluidMovement(double multiplier) {
  deltaMovement.x *= multiplier;
  deltaMovement.y += deltaMovement.y < 0.06 ? 5.0e-4 : 0.0;
  deltaMovement.z *= multiplier;
}
bool ItemEntity::isMergable() const {
  return !isRemoved() && pickupDelay != 32767 && age != -32768 && age < 6000 &&
         item.count < item.maxStackSize();
}
bool ItemEntity::fireImmune() const {
  if (const Item *it = item.item(); it && it->fireResistant())
    return true;
  return Entity::fireImmune();
}
bool ItemEntity::hurt(World &, DamageKind kind, float damage) {
  if (isRemoved() || item.empty())
    return false;
  if ((kind == DamageKind::Fire || kind == DamageKind::Lava) && fireImmune())
    return false;
  health = static_cast<int>(health - damage);
  if (health <= 0)
    discard();
  return true;
}
void ItemEntity::tryToMerge(World &world, ItemEntity &other) {
  if (other.isMergable() && item.isSameItemSameComponents(other.item) &&
      item.count + other.item.count <= other.item.maxStackSize()) {
    ItemEntity *to;
    ItemEntity *from;
    if (other.item.count < item.count) {
      to = this;
      from = &other;
    } else {
      to = &other;
      from = this;
    }
    int max = to->item.maxStackSize();
    int delta = std::min(std::min(max, 64) - to->item.count, from->item.count);
    to->item.count += delta;
    from->item.count -= delta;
    to->pickupDelay = std::max(to->pickupDelay, from->pickupDelay);
    to->age = std::min(to->age, from->age);
    if (from->item.count <= 0)
      from->discard();
  }
  (void)world;
}
void ItemEntity::mergeWithNeighbours(World &world) {
  if (!isMergable())
    return;
  auto box = boundingBox().inflate(0.5, 0.0, 0.5);
  world.forEachEntityInFlags(
      box, static_cast<std::uint8_t>(EntitySectionFlag::Item),
      [&](Entity &entity) -> bool {
        if (&entity == this || !entity.isItemEntity())
          return true;
        auto &other = static_cast<ItemEntity &>(entity);
        if (!other.isMergable())
          return true;
        tryToMerge(world, other);
        return !isRemoved();
      });
}
// TODO: ItemEntity.playerTouch pickup, Thrower/Owner tracking, ItemStack
// components, and item.onDestroyed effects require a player/inventory model.
void ItemEntity::tick(World &world) {
  if (item.empty()) {
    discard();
    return;
  }
  baseTick(world);
  if (isRemoved())
    return;
  if (pickupDelay > 0 && pickupDelay != 32767)
    --pickupDelay;
  oldPosition = position;
  if (isInWater(world) && fluidHeight(world, false) > 0.1)
    setFluidMovement(0.99);
  else if (isInLava(world) && fluidHeight(world, true) > 0.1)
    setFluidMovement(0.95);
  else
    deltaMovement.y -= 0.04;
  if (isRemoved())
    return;
  noPhysics = !world.noCollision(boundingBox().deflate(1.0e-7));
  if (noPhysics)
    moveTowardsClosestSpace(
        world, position.x, (boundingBox().minY + boundingBox().maxY) / 2.0,
        position.z);
  if (onGround && deltaMovement.horizontalDistanceSqr() <= 1.0e-5 &&
      (tickCount + id) % 4 != 0) {
    applyEffectsFromBlocks(world);
  } else {
    move(world, deltaMovement);
    if (position.x != oldPosition.x || position.y != oldPosition.y ||
        position.z != oldPosition.z)
      world.wakeHopperItemWatchers(position);
    applyEffectsFromBlocks(world);
    if (isRemoved())
      return;
    double friction = 0.98;
    if (onGround) {
      auto below = Pos{static_cast<int>(std::floor(position.x)),
                       // ItemEntity overrides the movement-support offset;
                       // carpet must use its underlying block's friction.
                       static_cast<int>(std::floor(position.y - 0.999999f)),
                       static_cast<int>(std::floor(position.z))};
      friction = blockMetadata(world.getBlock(below)).friction * 0.98;
    }
    deltaMovement = deltaMovement.multiply(friction, 0.98, friction);
    if (onGround && deltaMovement.y < 0)
      deltaMovement.y *= -0.5;
  }
  bool moved = static_cast<int>(std::floor(oldPosition.x)) !=
                   static_cast<int>(std::floor(position.x)) ||
               static_cast<int>(std::floor(oldPosition.y)) !=
                   static_cast<int>(std::floor(position.y)) ||
               static_cast<int>(std::floor(oldPosition.z)) !=
                   static_cast<int>(std::floor(position.z));
  int rate = moved ? 2 : 40;
  if (tickCount % rate == 0 && isMergable())
    mergeWithNeighbours(world);
  if (age != -32768)
    ++age;
  if (age >= 6000)
    discard();
}

PrimedTnt::PrimedTnt(World &world, Vec3 pos) : Entity("minecraft:tnt") {
  entityData_.define(kDataFuseId, EntityDataSerializer::Int, fuse);
  entityData_.define(kDataBlockStateId, EntityDataSerializer::BlockState,
                     blockState);
  setPos(pos);
  double rotation = world.random.nextDouble() * std::numbers::pi * 2.0;
  setDeltaMovement(-std::sin(rotation) * 0.02, 0.2,
                   -std::cos(rotation) * 0.02);
  oldPosition = position;
}
PrimedTnt::PrimedTnt(World &, Vec3 pos, Vec3 motion)
    : Entity("minecraft:tnt") {
  entityData_.define(kDataFuseId, EntityDataSerializer::Int, fuse);
  entityData_.define(kDataBlockStateId, EntityDataSerializer::BlockState,
                     blockState);
  setPos(pos);
  setDeltaMovement(motion);
  oldPosition = position;
}
void PrimedTnt::syncDataFields() {
  entityData_.set(kDataFuseId, fuse);
  entityData_.set(kDataBlockStateId, blockState);
}
void PrimedTnt::tick(World &world) {
  handlePortal(world);
  deltaMovement.y -= 0.04;
  move(world, deltaMovement);
  applyEffectsFromBlocks(world);
  deltaMovement = deltaMovement * 0.98;
  if (onGround)
    deltaMovement = deltaMovement.multiply(0.7, -0.5, 0.7);
  // World::tickEntities owns both the normal moved notification and the
  // removal notification after tick(), including a same-tick cross-chunk
  // disappearance. Notifying here would update the ledger twice.
  --fuse;
  if (fuse <= 0) {
    discard();
    world.notifyEntityRemoved(*this);
    if (world.tntExplodes)
      // Entity.getY(0.0625) scales by the TNT's bounding-box height.
      world.explode(this, {position.x, position.y + type().height * 0.0625, position.z},
                    explosionPower, false);
  }
}

FallingBlockEntity::FallingBlockEntity(World &world, Vec3 pos,
                                       BlockState state)
    : Entity("minecraft:falling_block"), blockState(std::move(state)) {
  setPos(pos);
  oldPosition = position;
  startPos = {static_cast<int>(std::floor(pos.x)),
              static_cast<int>(std::floor(pos.y)),
              static_cast<int>(std::floor(pos.z))};
  entityData_.define(kDataStartPosId, EntityDataSerializer::BlockPos, startPos);
  (void)world;
}
void FallingBlockEntity::syncDataFields() {
  entityData_.set(kDataStartPosId, startPos);
}
namespace {
bool isAnvilBlock(const BlockState &state) {
  return state.is(Blocks::ANVIL) || state.is(Blocks::CHIPPED_ANVIL) ||
         state.is(Blocks::DAMAGED_ANVIL);
}

std::optional<BlockState> damageAnvilState(const BlockState &state) {
  const std::string facing = state.get("facing", "north");
  if (state.is(Blocks::ANVIL))
    return defaultState("chipped_anvil").with("facing", facing);
  if (state.is(Blocks::CHIPPED_ANVIL))
    return defaultState("damaged_anvil").with("facing", facing);
  return std::nullopt;
}

bool isConcretePowderState(const BlockState &state) {
  const std::string_view name = state.name();
  return name.size() > 16 && name.ends_with("_concrete_powder");
}

BlockState solidifiedConcreteState(const BlockState &powder) {
  std::string name = powder.name();
  const auto suffix = name.rfind("_powder");
  if (suffix != std::string::npos)
    name.resize(suffix);
  return BlockState(name);
}

bool touchesWaterForConcrete(const World &world, Pos pos,
                             const BlockState &replaced) {
  if (entity_internal::isWaterFluidState(fluidStateOf(replaced)))
    return true;
  for (Direction d : {Direction::Up, Direction::North, Direction::South,
                      Direction::West, Direction::East}) {
    const Pos nPos = pos.relative(d);
    const BlockState nState = world.getBlock(nPos);
    if (entity_internal::isWaterFluidState(fluidStateOf(nState)))
      return true;
  }
  return false;
}

void restoreFallingBlockData(World &world, Pos pos, const BlockState &state,
                             const nbt::Tag &blockData) {
  if (!state.block().hasBlockEntity())
    return;
  if (auto container = world.containerEntity(pos)) {
    if (const auto *items = blockData.get("Items")) {
      for (const auto &entry : items->list) {
        const auto *slotTag = entry.get("Slot");
        const auto *idTag = entry.get("id");
        const auto *countTag = entry.get("count");
        if (!countTag)
          countTag = entry.get("Count");
        if (!slotTag || !idTag)
          continue;
        const int slot = static_cast<int>(slotTag->number);
        const int count =
            countTag ? std::max(1, static_cast<int>(countTag->number)) : 1;
        if (slot >= 0 && slot < container->containerSize())
          container->setItem(slot, itemStack(idTag->text, count));
      }
      container->setChanged(world);
    }
  }
}
} // namespace

FallingBlockEntity *FallingBlockEntity::fall(World &world, Pos pos,
                                             const BlockState &state) {
  BlockState fallen = state;
  if (fallen.getView("waterlogged", "") == "true")
    fallen = fallen.with("waterlogged", "false");
  auto entity = std::make_shared<FallingBlockEntity>(
      world, Vec3{pos.x + 0.5, double(pos.y), pos.z + 0.5}, fallen);
  if (isAnvilBlock(fallen)) {
    entity->setHurtsEntities(2.0f, 40);
  } else if (fallen.is(Blocks::POINTED_DRIPSTONE)) {
    entity->setHurtsEntities(6.0f, 40);
  }
  world.setBlock(pos,
                 entity_internal::isWaterFluidState(fluidStateOf(state))
                     ? defaultState("water")
                     : airState(),
                 3);
  world.addEntity(entity);
  world.log.push_back({"entity_spawn", pos,
                       "minecraft:falling_block:" + fallen.name()});
  return entity.get();
}

bool FallingBlockEntity::causeFallDamage(World &world, double fallDist,
                                         float damageMultiplier,
                                         DamageKind kind) {
  (void)damageMultiplier;
  (void)kind;
  if (!hurtEntities)
    return false;
  const int fallDistInt = static_cast<int>(std::ceil(fallDist - 1.0));
  if (fallDistInt < 0)
    return false;
  const float damage = static_cast<float>(std::min(
      static_cast<int>(std::floor(fallDistInt * fallDamagePerDistance)),
      fallDamageMax));
  const DamageKind dmgKind =
      isAnvilBlock(blockState)
          ? DamageKind::Anvil
          : (blockState.is(Blocks::POINTED_DRIPSTONE)
                 ? DamageKind::FallingStalactite
                 : DamageKind::FallingBlock);
  if (damage > 0.0f) {
    for (Entity *target : world.entitiesIn(boundingBox())) {
      if (!target || target == this || target->isRemoved() ||
          !target->isLiving())
        continue;
      target->hurt(world, dmgKind, damage);
    }
  }
  if (isAnvilBlock(blockState) && damage > 0.0f &&
      world.random.nextFloat() < 0.05f + fallDistInt * 0.05f) {
    if (auto damaged = damageAnvilState(blockState))
      blockState = *damaged;
    else
      cancelDrop = true;
  }
  return false;
}

void FallingBlockEntity::tick(World &world) {
  if (isAirState(blockState)) {
    discard();
    return;
  }
  handlePortal(world);
  ++time;
  deltaMovement.y -= 0.04;
  move(world, deltaMovement);
  applyEffectsFromBlocks(world);
  if (isRemoved())
    return;
  Pos pos{static_cast<int>(std::floor(position.x)),
          static_cast<int>(std::floor(position.y)),
          static_cast<int>(std::floor(position.z))};
  const bool isConcrete = isConcretePowderState(blockState);
  bool stuckInWater =
      isConcrete &&
      entity_internal::isWaterFluidState(fluidStateOf(world.getBlock(pos)));
  if (isConcrete && deltaMovement.lengthSqr() > 1.0) {
    const auto hit = world.clipBlock(oldPosition, position, true);
    if (hit.hit &&
        entity_internal::isWaterFluidState(fluidStateOf(world.getBlock(hit.pos)))) {
      pos = hit.pos;
      stuckInWater = true;
    }
  }
  if (!onGround && !stuckInWater) {
    if ((time > 100 && (pos.y <= world.minY() || pos.y >= world.maxY())) ||
        time > 600) {
      if (dropItem)
        spawnAtLocation(world, itemStack(blockItem(blockState), 1));
      discard();
    }
  } else {
    auto current = world.getBlock(pos);
    deltaMovement = deltaMovement.multiply(0.7, -0.5, 0.7);
    if (!current.is(Blocks::MOVING_PISTON)) {
      if (!cancelDrop) {
        bool mayReplace = isReplaceable(current);
        bool wouldContinueFalling =
            isFree(world.getBlock(pos.relative(Direction::Down))) &&
            (!isConcrete || !stuckInWater);
        bool wouldSurvive =
            canSurvive(world, pos, blockState) && !wouldContinueFalling;
        if (mayReplace && wouldSurvive) {
          BlockState finalState = blockState;
          if (finalState.getView("waterlogged", "") == "false" &&
              entity_internal::isWaterFluidState(
                  fluidStateOf(world.getBlock(pos)))) {
            finalState = finalState.with("waterlogged", "true");
          }
          if (isConcrete && touchesWaterForConcrete(world, pos, current)) {
            finalState = solidifiedConcreteState(finalState);
          }
          if (world.setBlock(pos, finalState, 3)) {
            if (isAnvilBlock(finalState))
              world.log.push_back({"anvil_land", pos, finalState.name()});
            if (blockData.has_value())
              restoreFallingBlockData(world, pos, finalState, *blockData);
            if (world.fallingBlockLandObserver)
              world.fallingBlockLandObserver(id, pos, world.getBlock(pos));
            discard();
          } else if (dropItem) {
            discard();
            spawnAtLocation(world, itemStack(blockItem(blockState), 1));
          }
        } else {
          discard();
          if (dropItem)
            spawnAtLocation(world, itemStack(blockItem(blockState), 1));
        }
      } else {
        if (isAnvilBlock(blockState))
          world.log.push_back({"anvil_broken", pos, blockState.name()});
        discard();
      }
    }
  }
  deltaMovement = deltaMovement * 0.98;
}
} // namespace redstone
