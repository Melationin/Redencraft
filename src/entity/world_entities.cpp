#include "entity/internal.hpp"

namespace redstone {
int World::allocateEntityId() { return nextEntityId++; }
bool World::hasEntityUuid(
    const std::array<std::int32_t, 4> &uuid) const {
  return entityUuids_.contains(uuid);
}
std::shared_ptr<Entity> World::addEntity(std::shared_ptr<Entity> entity) {
  return addEntityWithId(std::move(entity), 0);
}

std::shared_ptr<Entity> World::addEntityWithId(std::shared_ptr<Entity> entity,
                                               int fixedId) {
  entity->syncDataFields();
  if (fixedId > 0) {
    entity->id = fixedId;
    nextEntityId = std::max(nextEntityId, fixedId + 1);
  } else {
    entity->id = allocateEntityId();
  }
  if (entity->uuid == std::array<std::int32_t, 4>{}) {
    do {
      entity->uuid = generateEntityUuid();
    } while (hasEntityUuid(entity->uuid));
  } else if (hasEntityUuid(entity->uuid)) {
    throw std::logic_error("duplicate entity UUID");
  }
  if (!entityUuids_.insert(entity->uuid).second)
    throw std::logic_error("generated duplicate entity UUID");
  entity->removalObserved_ = false;
  entities.push_back(entity);
  entitySectionAdd(*entity);
  if (dynamic_cast<ItemEntity *>(entity.get()))
    wakeHopperItemWatchers(entity->position);
  if (managedEntityMutationObserver)
    managedEntityMutationObserver(entity->position, entity->position,
                                  entity->uuid, false);
  if (entityAddedObserver)
    entityAddedObserver(*entity);
  return entity;
}
void World::notifyEntityRemoved(Entity &entity) {
  if (entityRemovedObserver && !entity.removalObserved_) {
    entity.removalObserved_ = true;
    entityRemovedObserver(entity.id);
  }
}
void World::broadcastEntityRemovals() {
  // TODO: vanilla's levelCallback.onRemove runs inside setRemoved/discard.
  // Observe cleanup/end-of-block-entity removals for now; finer intra-phase
  // ordering still requires a world-aware entity lifecycle callback.
  if (entityRemovedObserver)
    for (const auto &entity : entities)
      if (entity && entity->isRemoved())
        notifyEntityRemoved(*entity);
}
void EntitySectionBucket::BitPlane::pushBack(bool value, std::size_t oldSize) {
  if (oldSize < 64u) {
    if (value) {
      inlineWord |= (1ull << oldSize);
      ++count;
    }
    return;
  }
  if (overflowWords.empty()) {
    overflowWords.push_back(inlineWord);
    overflowWords.push_back(0ull);
    inlineWord = 0;
  }
  const std::size_t wordIdx = oldSize >> 6u;
  const std::size_t bitIdx = oldSize & 63u;
  if (wordIdx >= overflowWords.size())
    overflowWords.resize(wordIdx + 1u, 0ull);
  if (value) {
    overflowWords[wordIdx] |= (1ull << bitIdx);
    ++count;
  }
}

void EntitySectionBucket::BitPlane::eraseAt(std::size_t idx,
                                            std::size_t oldSize) {
  if (idx >= oldSize)
    return;
  auto shiftWordSpan = [idx](std::uint64_t *words, std::size_t numWords) {
    const std::size_t targetWord = idx >> 6u;
    const std::size_t targetBit = idx & 63u;
    const std::uint64_t lowMask =
        targetBit == 0u ? 0ull : ((1ull << targetBit) - 1ull);
    const std::uint64_t w = words[targetWord];
    words[targetWord] = (w & lowMask) | ((w >> 1u) & ~lowMask);
    if (targetWord + 1u < numWords)
      words[targetWord] |= (words[targetWord + 1u] & 1ull) << 63u;
    for (std::size_t k = targetWord + 1u; k < numWords; ++k) {
      words[k] >>= 1u;
      if (k + 1u < numWords)
        words[k] |= (words[k + 1u] & 1ull) << 63u;
    }
  };
  if (test(idx) && count > 0u)
    --count;
  if (overflowWords.empty()) {
    shiftWordSpan(&inlineWord, 1u);
    return;
  }
  shiftWordSpan(overflowWords.data(), overflowWords.size());
  const std::size_t newSize = oldSize - 1u;
  if (newSize <= 64u) {
    inlineWord = overflowWords[0];
    overflowWords.clear();
  } else {
    const std::size_t newWords = (newSize + 63u) >> 6u;
    overflowWords.resize(newWords);
  }
}

EntitySectionBucket::~EntitySectionBucket() {
  for (Entity *entity : entities) {
    if (entity && entity->sectionBucket_ == this) {
      entity->sectionBucket_ = nullptr;
      entity->entitySectionIndex = Entity::kInvalidSectionIndex;
    }
  }
}

void EntitySectionBucket::append(Entity &entity, const Aabb &box,
                                 bool isActive, bool isPushable,
                                 bool isHardCollider, bool isMinecart,
                                 bool isItem) {
  const std::size_t oldSize = entities.size();
  entities.push_back(&entity);
  boxes.push_back(box);
  active.pushBack(isActive, oldSize);
  pushable.pushBack(isPushable, oldSize);
  hardCollider.pushBack(isHardCollider, oldSize);
  minecart.pushBack(isMinecart, oldSize);
  item.pushBack(isItem, oldSize);
}

void EntitySectionBucket::eraseAt(std::size_t idx) {
  const std::size_t oldSize = entities.size();
  if (idx >= oldSize)
    return;
  Entity *removed = entities[idx];
  entities.erase(entities.begin() + static_cast<std::ptrdiff_t>(idx));
  boxes.erase(boxes.begin() + static_cast<std::ptrdiff_t>(idx));
  active.eraseAt(idx, oldSize);
  pushable.eraseAt(idx, oldSize);
  hardCollider.eraseAt(idx, oldSize);
  minecart.eraseAt(idx, oldSize);
  item.eraseAt(idx, oldSize);
  for (std::size_t i = idx; i < entities.size(); ++i) {
    if (entities[i])
      entities[i]->entitySectionIndex = static_cast<std::uint32_t>(i);
  }
  if (removed && removed->sectionBucket_ == this) {
    removed->sectionBucket_ = nullptr;
    removed->entitySectionIndex = Entity::kInvalidSectionIndex;
  }
}

void World::entitySectionAdd(Entity &entity) {
  if (entity.sectionBucket_ != nullptr)
    entitySectionRemove(entity);
  entity.entitySection = entitySectionKey(entity.position);
  auto &slot = entitySections_[entity.entitySection];
  if (!slot)
    slot = std::make_unique<EntitySectionBucket>();
  const std::size_t index = slot->size();
  slot->append(entity, entity.boundingBox(), !entity.isRemoved(),
               entity.isPushable(), entity.canBeCollidedWith(),
               entity.isMinecart(), entity.isItemEntity());
  entity.entitySectionIndex = static_cast<std::uint32_t>(index);
  entity.sectionBucket_ = slot.get();
  if (!entity.isPlayer()) {
    const auto key = chunkKey(sectionKeyX(entity.entitySection),
                              sectionKeyZ(entity.entitySection));
    ++entityCountsByChunk_[key];
  }
}
void World::entitySectionRemove(Entity &entity) {
  auto it = entitySections_.find(entity.entitySection);
  if (it == entitySections_.end() || !it->second) {
    entity.sectionBucket_ = nullptr;
    entity.entitySectionIndex = Entity::kInvalidSectionIndex;
    return;
  }
  EntitySectionBucket &bucket = *it->second;
  std::size_t idx = entity.entitySectionIndex;
  if (idx >= bucket.entities.size() || bucket.entities[idx] != &entity) {
    auto found =
        std::find(bucket.entities.begin(), bucket.entities.end(), &entity);
    if (found == bucket.entities.end())
      return;
    idx = static_cast<std::size_t>(found - bucket.entities.begin());
  }
  bucket.eraseAt(idx);
  if (bucket.entities.empty())
    entitySections_.erase(it);
  if (!entity.isPlayer()) {
    const auto chunk = chunkKey(sectionKeyX(entity.entitySection),
                                sectionKeyZ(entity.entitySection));
    auto count = entityCountsByChunk_.find(chunk);
    if (count != entityCountsByChunk_.end() && --count->second == 0)
      entityCountsByChunk_.erase(count);
  }
}
void World::entitySectionUpdate(Entity &entity) {
  const auto key = entitySectionKey(entity.position);
  if (key == entity.entitySection && entity.sectionBucket_ != nullptr &&
      entity.entitySectionIndex < entity.sectionBucket_->size() &&
      entity.sectionBucket_->entities[entity.entitySectionIndex] == &entity) {
    EntitySectionBucket &bucket = *entity.sectionBucket_;
    const std::size_t idx = entity.entitySectionIndex;
    bucket.boxes[idx] = entity.boundingBox();
    bucket.setFlag(idx, EntitySectionFlag::Active, !entity.isRemoved());
    bucket.setFlag(idx, EntitySectionFlag::Pushable, entity.isPushable());
    bucket.setFlag(idx, EntitySectionFlag::HardCollider,
                   entity.canBeCollidedWith());
    return;
  }
  entitySectionRemove(entity);
  entitySectionAdd(entity);
}
void World::removeEntity(const std::shared_ptr<Entity> &entity) {
  if (entity)
    entity->discard();
}
bool World::detachEntityForDimensionTransfer(
    const std::shared_ptr<Entity> &entity) {
  if (!entity)
    return false;
  auto it = std::find(entities.begin(), entities.end(), entity);
  if (it == entities.end())
    return false;
  if (managedEntityMutationObserver)
    managedEntityMutationObserver(entity->position, entity->position,
                                  entity->uuid, true);
  notifyEntityRemoved(*entity);
  entityUuids_.erase(entity->uuid);
  entitySectionRemove(*entity);
  entities.erase(it);
  entity->removed_ = false;
  entity->removalObserved_ = false;
  return true;
}
void World::queuePortalTransfer(const Entity &entity, Pos entryPos) {
  if (entity.id > 0)
    pendingPortalTransferIds_.emplace_back(entity.id, entryPos);
}
std::vector<World::PendingPortalTransfer> World::takePendingPortalTransfers() {
  std::vector<PendingPortalTransfer> out;
  if (pendingPortalTransferIds_.empty())
    return out;
  auto queued = std::move(pendingPortalTransferIds_);
  pendingPortalTransferIds_.clear();
  out.reserve(queued.size());
  for (const auto &[entityId, entryPos] : queued) {
    for (const auto &entity : entities) {
      if (entity && entity->id == entityId && !entity->isRemoved()) {
        out.push_back({entity, entryPos});
        break;
      }
    }
  }
  return out;
}
std::vector<Entity *> World::entitiesIn(const Aabb &box) const {
  std::vector<Entity *> result;
  forEachEntityInFlags(
      box, static_cast<std::uint8_t>(EntitySectionFlag::Active),
      [&](Entity &entity) { result.push_back(&entity); });
  return result;
}
bool World::visitEntitiesInFlags(const Aabb &box, std::uint8_t anyFlagsMask,
                                 void *ctx,
                                 bool (*callback)(void *, Entity &)) const {
  const bool activeOnly =
      anyFlagsMask == 0 ||
      anyFlagsMask == static_cast<std::uint8_t>(EntitySectionFlag::Active);
  const bool wantPushable =
      (anyFlagsMask & static_cast<std::uint8_t>(EntitySectionFlag::Pushable)) !=
      0;
  const bool wantHardCollider =
      (anyFlagsMask &
       static_cast<std::uint8_t>(EntitySectionFlag::HardCollider)) != 0;
  const bool wantMinecart =
      (anyFlagsMask & static_cast<std::uint8_t>(EntitySectionFlag::Minecart)) !=
      0;
  const bool wantItem =
      (anyFlagsMask & static_cast<std::uint8_t>(EntitySectionFlag::Item)) != 0;
  bool completed = true;
  forEachEntitySection(
      entitySections_, box, [&](const EntitySectionBucket &bucket) -> bool {
        if (bucket.active.empty())
          return true;
        if (!activeOnly) {
          const bool anyCandidate =
              (wantPushable && !bucket.pushable.empty()) ||
              (wantHardCollider && !bucket.hardCollider.empty()) ||
              (wantMinecart && !bucket.minecart.empty()) ||
              (wantItem && !bucket.item.empty());
          if (!anyCandidate)
            return true;
        }
        const std::size_t words = bucket.wordCount();
        for (std::size_t w = 0; w < words; ++w) {
          std::uint64_t word = bucket.active.wordAt(w);
          if (word == 0ull)
            continue;
          if (!activeOnly) {
            std::uint64_t typeWord = 0ull;
            if (wantPushable)
              typeWord |= bucket.pushable.wordAt(w);
            if (wantHardCollider)
              typeWord |= bucket.hardCollider.wordAt(w);
            if (wantMinecart)
              typeWord |= bucket.minecart.wordAt(w);
            if (wantItem)
              typeWord |= bucket.item.wordAt(w);
            word &= typeWord;
          }
          while (word != 0ull) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(word));
            word &= (word - 1ull);
            const std::size_t idx = (w << 6u) | bit;
            if (!bucket.boxes[idx].intersects(box))
              continue;
            Entity *entity = bucket.entities[idx];
            if (!entity || entity->isRemoved())
              continue;
            if (!callback(ctx, *entity)) {
              completed = false;
              return false;
            }
          }
        }
        return true;
      });
  return completed;
}
int World::pushLivingEntities(Entity &self, const Aabb &box, bool applyPush,
                              bool selfCanReceivePush, bool selfPassenger,
                              const Entity *selfRoot, double sx, double sz,
                              double &selfAccX, double &selfAccZ,
                              bool &anySelfPush) const {
  int crammingCount = 0;
  auto scanBucket = [&](const EntitySectionBucket &bucket) {
    if (bucket.active.empty() || bucket.pushable.empty())
      return;
    const std::size_t words = bucket.wordCount();
    const Aabb *boxes = bucket.boxes.data();
    Entity *const *entities = bucket.entities.data();
    for (std::size_t w = 0; w < words; ++w) {
      std::uint64_t word = bucket.active.wordAt(w) & bucket.pushable.wordAt(w);
      while (word != 0ull) {
        const unsigned bit = static_cast<unsigned>(std::countr_zero(word));
        word &= (word - 1ull);
        const std::size_t idx = (w << 6u) | bit;
        if (!boxes[idx].intersects(box))
          continue;
        Entity *other = entities[idx];
        if (!other || other == &self)
          continue;
        // Player spectators may be toggled directly without syncSectionPushable.
        if (other->isPlayer() && !other->isPushable())
          continue;
        const bool otherPassenger = other->isPassenger();
        if (!otherPassenger)
          ++crammingCount;
        if (!applyPush || other->noPhysics)
          continue;
        if ((selfPassenger || otherPassenger) &&
            selfRoot == &other->getRootVehicle())
          continue;
        double dx = other->position.x - sx;
        double dz = other->position.z - sz;
        double distMax = std::max(std::abs(dx), std::abs(dz));
        if (distMax < 0.01)
          continue;
        distMax = std::sqrt(distMax);
        dx /= distMax;
        dz /= distMax;
        double inv = 1.0 / distMax;
        if (inv > 1.0)
          inv = 1.0;
        dx *= inv * 0.05;
        dz *= inv * 0.05;
        if (selfCanReceivePush) {
          selfAccX -= dx;
          selfAccZ -= dz;
          anySelfPush = true;
        }
        if (!other->isVehicle()) {
          other->deltaMovement.x += dx;
          other->deltaMovement.z += dz;
          ++other->motionRevision;
        }
      }
    }
  };
  if (entitySections_.empty())
    return 0;
  const int xMin = blockToSection(box.minX - 2.0);
  const int yMin = blockToSection(box.minY - 4.0);
  const int zMin = blockToSection(box.minZ - 2.0);
  const int xMax = blockToSection(box.maxX + 2.0);
  const int yMax = blockToSection(box.maxY);
  const int zMax = blockToSection(box.maxZ + 2.0);
  if (self.sectionBucket_ != nullptr && xMin == xMax && yMin == yMax &&
      zMin == zMax && self.entitySection == sectionPosKey(xMin, yMin, zMin)) {
    scanBucket(*self.sectionBucket_);
    return crammingCount;
  }
  if (entitySections_.size() == 1u) {
    const auto &only = *entitySections_.begin();
    if (!only.second)
      return 0;
    const int x = sectionKeyX(only.first);
    const int y = sectionKeyY(only.first);
    const int z = sectionKeyZ(only.first);
    if (x >= xMin && x <= xMax && y >= yMin && y <= yMax && z >= zMin &&
        z <= zMax) {
      scanBucket(*only.second);
    }
    return crammingCount;
  }
  forEachEntitySection(entitySections_, box, scanBucket);
  return crammingCount;
}
std::vector<Entity *> World::entitiesInSectionRange(int minX, int maxX, int minZ,
                                                     int maxZ) const {
  std::vector<Entity *> result;
  for (int x = minX; x <= maxX; ++x) {
    const auto lowest = sectionPosKey(x, 0, 0);
    const auto highest = sectionPosKey(x, -1, -1);
    for (auto it = entitySections_.lower_bound(lowest);
         it != entitySections_.end() && it->first <= highest; ++it) {
      if (!it->second)
        continue;
      const int z = sectionKeyZ(it->first);
      if (z < minZ || z > maxZ)
        continue;
      for (Entity *entity : it->second->entities)
        if (entity && !entity->isRemoved())
          result.push_back(entity);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const Entity *a, const Entity *b) { return a->id < b->id; });
  return result;
}

void World::synchronizeEntityAfterMove(Entity &entity, Vec3 previousPosition) {
  entitySectionUpdate(entity);
  if (dynamic_cast<ItemEntity *>(&entity))
    wakeHopperItemWatchers(entity.position);
  if (managedEntityMutationObserver)
    managedEntityMutationObserver(previousPosition, entity.position, entity.uuid,
                                  entity.isRemoved());
}

void World::moveEntityForManagedTest(const Entity &entity, Vec3 to) {
  const Vec3 from = entity.position;
  auto mutableEntity = std::find_if(
      entities.begin(), entities.end(),
      [&](const std::shared_ptr<Entity> &candidate) {
        return candidate.get() == &entity;
      });
  if (mutableEntity == entities.end())
    throw std::logic_error("fixture entity is not registered in World");
  (*mutableEntity)->setPos(to);
  entitySectionUpdate(**mutableEntity);
  if (managedEntityMutationObserver)
    managedEntityMutationObserver(from, (*mutableEntity)->position,
                                  (*mutableEntity)->uuid, false);
}

std::vector<std::array<std::int32_t, 4>>
World::chunkEntityOrderForTest(int chunkX, int chunkZ) const {
  const auto snapshot = snapshotChunkItemEntities(chunkX, chunkZ);
  std::vector<std::array<std::int32_t, 4>> result;
  result.reserve(snapshot.records.size());
  for (const auto &record : snapshot.records)
    result.push_back(entity_codec::recordUuid(record));
  return result;
}

World::ItemEntitySnapshotResult
World::snapshotChunkItemEntities(int chunkX, int chunkZ) const {
  ItemEntitySnapshotResult result;
  const auto entities =
      entitiesInSectionRange(chunkX, chunkX, chunkZ, chunkZ);
  for (const Entity *entity : entities) {
    if (!entity || entity->isPlayer())
      continue;
    if (const auto *tnt = dynamic_cast<const PrimedTnt *>(entity)) {
      if (tnt->uuid == entity_codec::EntityUuid{} || tnt->usedPortal ||
          tnt->blockState.id() != defaultState("tnt").id() ||
          !std::isfinite(tnt->explosionPower) || tnt->explosionPower < 0.0f ||
          tnt->explosionPower > 128.0f) {
        result.hasUnsupported = true;
        result.detail = "primed tnt has unsupported persisted state";
        return result;
      }
      entity_codec::TntEntityData record;
      record.uuid = tnt->uuid;
      record.pos = {tnt->position.x, tnt->position.y, tnt->position.z};
      record.motion = {tnt->deltaMovement.x, tnt->deltaMovement.y,
                       tnt->deltaMovement.z};
      record.rotation = {tnt->yaw, tnt->pitch};
      record.onGround = tnt->onGround;
      record.fireTicks = tnt->remainingFireTicks;
      record.fallDistance = tnt->fallDistance;
      record.fuse = tnt->fuse;
      record.explosionPower = tnt->explosionPower;
      result.records.emplace_back(std::move(record));
      continue;
    }
    if (const auto *minecart = dynamic_cast<const Minecart *>(entity)) {
      if (minecart->uuid == entity_codec::EntityUuid{}) {
        result.hasUnsupported = true;
        result.detail = "minecart has no persistable UUID";
        return result;
      }
      entity_codec::MinecartEntityData record;
      record.uuid = minecart->uuid;
      record.pos = {minecart->position.x, minecart->position.y,
                    minecart->position.z};
      record.motion = {minecart->deltaMovement.x, minecart->deltaMovement.y,
                       minecart->deltaMovement.z};
      record.rotation = {minecart->yaw, minecart->pitch};
      record.onGround = minecart->onGround;
      record.fireTicks = minecart->remainingFireTicks;
      record.fallDistance = minecart->fallDistance;
      record.flipped = minecart->flipped;
      record.firstTick = minecart->firstTick;
      result.records.emplace_back(std::move(record));
      continue;
    }
    if (const auto *falling =
            dynamic_cast<const FallingBlockEntity *>(entity)) {
      if (falling->uuid == entity_codec::EntityUuid{} ||
          falling->blockState != defaultState("sand") ||
          falling->time < 0 ||
          falling->time >= std::numeric_limits<int>::max() ||
          falling->hurtEntities || falling->fallDamagePerDistance != 0.0f ||
          falling->fallDamageMax != 40) {
        result.hasUnsupported = true;
        result.detail =
            "falling block has unsupported persisted state";
        return result;
      }
      entity_codec::FallingBlockEntityData record;
      record.uuid = falling->uuid;
      record.pos = {falling->position.x, falling->position.y,
                    falling->position.z};
      record.motion = {falling->deltaMovement.x, falling->deltaMovement.y,
                       falling->deltaMovement.z};
      record.rotation = {falling->yaw, falling->pitch};
      record.onGround = falling->onGround;
      record.fireTicks = falling->remainingFireTicks;
      record.fallDistance = falling->fallDistance;
      record.blockId = falling->blockState.name();
      record.time = falling->time;
      record.dropItem = falling->dropItem;
      record.cancelDrop = falling->cancelDrop;
      record.hurtEntities = falling->hurtEntities;
      record.fallDamagePerDistance = falling->fallDamagePerDistance;
      record.fallDamageMax = falling->fallDamageMax;
      result.records.emplace_back(std::move(record));
      continue;
    }
    const auto *item = dynamic_cast<const ItemEntity *>(entity);
    if (!item) {
      result.hasUnsupported = true;
      result.detail = "chunk contains an entity type outside persistence";
      return result;
    }
    if (item->item.empty() || item->item.count <= 0 ||
        item->uuid == entity_codec::EntityUuid{}) {
      result.hasUnsupported = true;
      result.detail = "item entity has no persistable item/UUID";
      return result;
    }
    entity_codec::ItemEntityData record;
    record.uuid = item->uuid;
    record.pos = {item->position.x, item->position.y, item->position.z};
    record.motion = {item->deltaMovement.x, item->deltaMovement.y,
                     item->deltaMovement.z};
    record.rotation = {item->yaw, item->pitch};
    record.onGround = item->onGround;
    record.fireTicks = item->remainingFireTicks;
    record.fallDistance = item->fallDistance;
    record.age = item->age;
    record.pickupDelay = item->pickupDelay;
    record.health = item->health;
    record.itemId = item->item.id;
    record.itemCount = item->item.count;
    result.records.emplace_back(std::move(record));
  }
  return result;
}

namespace {
void tickPassengerEntity(World &world, Entity &vehicle, Entity &passenger) {
  if (passenger.isRemoved() || passenger.getVehicle().get() != &vehicle) {
    passenger.stopRiding();
    return;
  }
  const int chunkX = static_cast<int>(std::floor(passenger.position.x / 16));
  const int chunkZ = static_cast<int>(std::floor(passenger.position.z / 16));
  if (!world.isChunkLoaded(chunkX, chunkZ))
    return;
  if (world.entityTickPositionReady &&
      !world.entityTickPositionReady(
          {static_cast<int>(std::floor(passenger.position.x)),
           static_cast<int>(std::floor(passenger.position.y)),
           static_cast<int>(std::floor(passenger.position.z))}))
    return;
  const Vec3 previousPosition = passenger.position;
  passenger.setOldPosAndRot();
  ++passenger.tickCount;
  passenger.rideTick(world);
  passenger.syncDataFields();
  if (!passenger.isRemoved()) {
    world.synchronizeEntityAfterMove(passenger, previousPosition);
  }
  for (const auto &child : passenger.getPassengers()) {
    if (child)
      tickPassengerEntity(world, passenger, *child);
  }
}
} // namespace

void World::tickEntities() {
  auto snapshot = entities;
  for (auto &entity : snapshot) {
    if (!entity || entity->isRemoved())
      continue;
    if (entity->isPassenger())
      continue;
    // Entity-ticking visibility belongs to the X/Z chunk, including sections
    // above/below build height. isLoaded(BlockPos) also bounds Y and would
    // freeze falling entities before they can reach Entity.onBelowWorld.
    const int chunkX =
        static_cast<int>(std::floor(entity->position.x / 16));
    const int chunkZ =
        static_cast<int>(std::floor(entity->position.z / 16));
    if (!isChunkLoaded(chunkX, chunkZ))
      continue;
    if (entityTickPositionReady &&
        !entityTickPositionReady(
            {static_cast<int>(std::floor(entity->position.x)),
             static_cast<int>(std::floor(entity->position.y)),
             static_cast<int>(std::floor(entity->position.z))}))
      continue;
    const Vec3 previousPosition = entity->position;
    entity->setOldPosAndRot();
    ++entity->tickCount;
    entity->tick(*this);
    entity->syncDataFields();
    for (const auto &passenger : entity->getPassengers()) {
      if (passenger)
        tickPassengerEntity(*this, *entity, *passenger);
    }
    // TransientEntitySectionManager: moved entities change their bucket.
    if (!entity->isRemoved()) {
      entitySectionUpdate(*entity);
      if (managedEntityMutationObserver)
        managedEntityMutationObserver(previousPosition, entity->position,
                                      entity->uuid, false);
    }
  }
  const auto newEnd = std::remove_if(
      entities.begin(), entities.end(), [this](const auto &entity) {
        if (entity && !entity->isRemoved())
          return false;
        if (entity) {
          entity->unRide();
          if (managedEntityMutationObserver)
            managedEntityMutationObserver(entity->position, entity->position,
                                          entity->uuid, true);
          notifyEntityRemoved(*entity);
          entityUuids_.erase(entity->uuid);
          entitySectionRemove(*entity);
        }
        return true;
      });
  entities.erase(newEnd, entities.end());
}
void World::collectCollisionBoxes(const Aabb &box, std::vector<ShapeBox> &out) {
  out.clear();
  // BlockCollisions visits the surrounding shell too: walls extend upward by
  // half a block and moving pistons can expose shapes in adjacent cells.
  const int x0 = floorToInt(box.minX - EPS) - 1;
  const int y0 = floorToInt(box.minY - EPS) - 1;
  const int z0 = floorToInt(box.minZ - EPS) - 1;
  const int x1 = floorToInt(box.maxX + EPS) + 1;
  const int y1 = floorToInt(box.maxY + EPS) + 1;
  const int z1 = floorToInt(box.maxZ + EPS) + 1;
  const int yClamped0 = std::max(y0, minY_);
  const int yClamped1 = std::min(y1, maxY_ - 1);
  if (yClamped0 > yClamped1)
    return;
  const int cx0 = chunkIndex(x0);
  const int cx1 = chunkIndex(x1);
  const int cz0 = chunkIndex(z0);
  const int cz1 = chunkIndex(z1);
  const int sec0 = sectionIndex(yClamped0);
  const int sec1 = sectionIndex(yClamped1);
  const bool haveMovingPistons = !pistonEntities.empty();

  auto processCell = [&](int x, int y, int z, BlockState::Id stateId,
                         int shellAxes) {
    if (shellAxes == 3)
      return;
    const BlockCollisionKind kind = stateCollisionKindId(stateId);
    if (kind == BlockCollisionKind::Empty && !haveMovingPistons)
      return;
    if (kind == BlockCollisionKind::FullCube) {
      if (shellAxes != 0)
        return;
      const double dx0 = static_cast<double>(x);
      const double dy0 = static_cast<double>(y);
      const double dz0 = static_cast<double>(z);
      if (dx0 + 1.0 > box.minX && dx0 < box.maxX && dy0 + 1.0 > box.minY &&
          dy0 < box.maxY && dz0 + 1.0 > box.minZ && dz0 < box.maxZ) {
        out.push_back({dx0, dy0, dz0, dx0 + 1.0, dy0 + 1.0, dz0 + 1.0});
      }
      return;
    }
    const BlockState state = BlockState::fromId(stateId);
    const bool isPiston = haveMovingPistons && state.is(Blocks::MOVING_PISTON);
    if (kind == BlockCollisionKind::Empty && !isPiston)
      return;
    if (shellAxes == 2 && !isPiston)
      return;
    if (shellAxes == 1 && kind != BlockCollisionKind::Oversized && !isPiston)
      return;
    VoxelShape moving;
    const VoxelShape &shape =
        isPiston ? (moving = collisionShape(*this, {x, y, z}, state), moving)
                 : collisionShape(state);
    if (shape.empty())
      return;
    for (const auto &b : shape) {
      const double bx0 = b.x0 / 16.0 + x;
      const double by0 = b.y0 / 16.0 + y;
      const double bz0 = b.z0 / 16.0 + z;
      const double bx1 = b.x1 / 16.0 + x;
      const double by1 = b.y1 / 16.0 + y;
      const double bz1 = b.z1 / 16.0 + z;
      if (bx1 > box.minX && bx0 < box.maxX && by1 > box.minY &&
          by0 < box.maxY && bz1 > box.minZ && bz0 < box.maxZ) {
        out.push_back({bx0, by0, bz0, bx1, by1, bz1});
      }
    }
  };

  for (int cx = cx0; cx <= cx1; ++cx) {
    const int baseX = cx * 16;
    const int lx0 = std::max(0, x0 - baseX);
    const int lx1 = std::min(15, x1 - baseX);
    for (int cz = cz0; cz <= cz1; ++cz) {
      const Chunk *chunk = chunkAtCached(cx, cz);
      if (!chunk || !chunk->loaded)
        continue;
      const int baseZ = cz * 16;
      const int lz0 = std::max(0, z0 - baseZ);
      const int lz1 = std::min(15, z1 - baseZ);
      for (int secIdx = sec0; secIdx <= sec1; ++secIdx) {
        const Section &sec = chunk->sections[static_cast<std::size_t>(secIdx)];
        if (sec.hasOnlyAir())
          continue;
        const int baseY = minY_ + secIdx * 16;
        const int ly0 = std::max(0, yClamped0 - baseY);
        const int ly1 = std::min(15, yClamped1 - baseY);
        if (!sec.storage) {
          const BlockState::Id singleId = sec.single;
          const BlockCollisionKind kind = stateCollisionKindId(singleId);
          if (kind == BlockCollisionKind::Empty && !haveMovingPistons)
            continue;
          for (int lx = lx0; lx <= lx1; ++lx) {
            const int x = baseX + lx;
            const int sx = int(x == x0 || x == x1);
            for (int ly = ly0; ly <= ly1; ++ly) {
              const int y = baseY + ly;
              const int sxy = sx + int(y == y0 || y == y1);
              for (int lz = lz0; lz <= lz1; ++lz) {
                const int z = baseZ + lz;
                processCell(x, y, z, singleId, sxy + int(z == z0 || z == z1));
              }
            }
          }
          continue;
        }
        visitSectionStorage(sec.storage.get(), [&](const auto *blk) {
          for (int ly = ly0; ly <= ly1; ++ly) {
            const int y = baseY + ly;
            const int sy = int(y == y0 || y == y1);
            const int yOffset = ly << 8;
            for (int lz = lz0; lz <= lz1; ++lz) {
              const int z = baseZ + lz;
              const int syz = sy + int(z == z0 || z == z1);
              const int rowOffset = yOffset | (lz << 4);
              for (int lx = lx0; lx <= lx1; ++lx) {
                const int x = baseX + lx;
                const BlockState::Id stateId =
                    blk->palette[blk->indexAt(static_cast<std::size_t>(rowOffset | lx))];
                if (stateId == 0)
                  continue;
                processCell(x, y, z, stateId, syz + int(x == x0 || x == x1));
              }
            }
          }
        });
      }
    }
  }
}

std::optional<double> World::tryCollideSupportingBlockY(const Aabb &box,
                                                      double movementY) const {
  if (movementY >= 0.0)
    return std::nullopt;
  const int bx = floorToInt((box.minX + box.maxX) * 0.5);
  const int by = floorToInt(box.minY - EPS);
  const int bz = floorToInt((box.minZ + box.maxZ) * 0.5);
  const Section *sec = loadedSectionAt({bx, by, bz});
  if (!sec || sec->hasOnlyAir())
    return std::nullopt;
  const int cell = ((by & 15) << 8) | ((bz & 15) << 4) | (bx & 15);
  const BlockState::Id id = readSectionCell(*sec, cell);
  if (stateCollisionKindId(id) != BlockCollisionKind::FullCube)
    return std::nullopt;
  const double dx0 = static_cast<double>(bx);
  const double dz0 = static_cast<double>(bz);
  if (!(box.maxX > dx0 + EPS && box.minX < dx0 + 1.0 - EPS &&
        box.maxZ > dz0 + EPS && box.minZ < dz0 + 1.0 - EPS)) {
    return std::nullopt;
  }
  const double top = static_cast<double>(by + 1);
  if (top > box.minY + EPS)
    return std::nullopt;
  const double clipped = std::max(movementY, top - box.minY);
  if (std::abs(clipped) < EPS)
    return 0.0;
  return std::nullopt;
}

double World::collideBlocksOnAxis(int axis, const Aabb &box,
                                  double distance) const {
  if (std::abs(distance) < EPS)
    return 0.0;
  const int t1Axis = (axis == 0) ? 1 : 0;
  const int t2Axis = (axis == 2) ? 1 : 2;
  const int t1_0 = floorToInt(aabbMin(box, t1Axis) - EPS) - 1;
  const int t1_1 = floorToInt(aabbMax(box, t1Axis) + EPS) + 1;
  const int t2_0 = floorToInt(aabbMin(box, t2Axis) - EPS) - 1;
  const int t2_1 = floorToInt(aabbMax(box, t2Axis) + EPS) + 1;

  const bool positive = distance > 0.0;
  const int step = positive ? 1 : -1;
  const int aStart = positive ? (floorToInt(aabbMin(box, axis) - EPS) - 1)
                              : (floorToInt(aabbMax(box, axis) + EPS) + 1);
  auto computeAEnd = [&](double d) noexcept -> int {
    return positive ? (floorToInt(aabbMax(box, axis) + d + EPS) + 1)
                    : (floorToInt(aabbMin(box, axis) + d - EPS) - 1);
  };
  int aEnd = computeAEnd(distance);
  const bool haveMovingPistons = !pistonEntities.empty();

  auto collideSinglePart = [&](const ShapeBox &part) noexcept {
    if (!overlapsOtherAxes(box, part, axis))
      return;
    if (positive) {
      if (boxMin(part, axis) < aabbMax(box, axis) - EPS)
        return;
      const double next = boxMin(part, axis) - aabbMax(box, axis);
      if (next < distance) {
        distance = next;
        aEnd = computeAEnd(distance);
      }
    } else {
      if (boxMax(part, axis) > aabbMin(box, axis) + EPS)
        return;
      const double next = boxMax(part, axis) - aabbMin(box, axis);
      if (next > distance) {
        distance = next;
        aEnd = computeAEnd(distance);
      }
    }
  };

  auto testCell = [&](int x, int y, int z, BlockState::Id id, int shellAxes) {
    if (id == 0 || shellAxes == 3)
      return;
    const BlockCollisionKind kind = stateCollisionKindId(id);
    if (kind == BlockCollisionKind::FullCube) {
      if (shellAxes != 0)
        return;
      const double dx0 = static_cast<double>(x);
      const double dy0 = static_cast<double>(y);
      const double dz0 = static_cast<double>(z);
      collideSinglePart({dx0, dy0, dz0, dx0 + 1.0, dy0 + 1.0, dz0 + 1.0});
      return;
    }
    if (kind == BlockCollisionKind::Empty && !haveMovingPistons)
      return;
    const BlockState state = BlockState::fromId(id);
    const bool isPiston = haveMovingPistons && state.is(Blocks::MOVING_PISTON);
    if (kind == BlockCollisionKind::Empty && !isPiston)
      return;
    if (shellAxes == 2 && !isPiston)
      return;
    if (shellAxes == 1 && kind != BlockCollisionKind::Oversized && !isPiston)
      return;
    VoxelShape moving;
    const VoxelShape &shape =
        isPiston ? (moving = collisionShape(const_cast<World &>(*this),
                                             {x, y, z}, state),
                    moving)
                 : collisionShape(state);
    for (const auto &b : shape) {
      collideSinglePart({b.x0 / 16.0 + x, b.y0 / 16.0 + y, b.z0 / 16.0 + z,
                         b.x1 / 16.0 + x, b.y1 / 16.0 + y, b.z1 / 16.0 + z});
    }
  };

  if (axis == 1) {
    const int yMinBound = std::max(positive ? aStart : aEnd, minY_);
    const int yMaxBound = std::min(positive ? aEnd : aStart, maxY_ - 1);
    if (yMinBound > yMaxBound)
      return distance;
    const int cx0 = chunkIndex(t1_0);
    const int cx1 = chunkIndex(t1_1);
    const int cz0 = chunkIndex(t2_0);
    const int cz1 = chunkIndex(t2_1);
    const int y0Edge = aStart;
    for (int cx = cx0; cx <= cx1; ++cx) {
      const int baseX = cx * 16;
      const int lx0 = std::max(0, t1_0 - baseX);
      const int lx1 = std::min(15, t1_1 - baseX);
      for (int cz = cz0; cz <= cz1; ++cz) {
        const Chunk *chunk = chunkAtCached(cx, cz);
        if (!chunk || !chunk->loaded)
          continue;
        const int baseZ = cz * 16;
        const int lz0 = std::max(0, t2_0 - baseZ);
        const int lz1 = std::min(15, t2_1 - baseZ);
        int curY = positive ? std::max(aStart, minY_) : std::min(aStart, maxY_ - 1);
        while (positive ? (curY <= std::min(aEnd, maxY_ - 1))
                        : (curY >= std::max(aEnd, minY_))) {
          const int secIdx = sectionIndex(curY);
          const int secBaseY = minY_ + secIdx * 16;
          const Section &sec =
              chunk->sections[static_cast<std::size_t>(secIdx)];
          if (sec.hasOnlyAir()) {
            curY = positive ? (secBaseY + 16) : (secBaseY - 1);
            continue;
          }
          const int lyStart = curY - secBaseY;
          const int lyStop = positive
                                 ? std::min(15, std::min(aEnd, maxY_ - 1) - secBaseY)
                                 : std::max(0, std::max(aEnd, minY_) - secBaseY);
          for (int ly = lyStart;
               positive ? (ly <= lyStop) : (ly >= lyStop); ly += step) {
            const int y = secBaseY + ly;
            if (positive ? (y > aEnd) : (y < aEnd))
              break;
            const int sy = int(y == y0Edge || y == aEnd);
            const int yOff = ly << 8;
            for (int lz = lz0; lz <= lz1; ++lz) {
              const int z = baseZ + lz;
              const int syz = sy + int(z == t2_0 || z == t2_1);
              const int rowOff = yOff | (lz << 4);
              for (int lx = lx0; lx <= lx1; ++lx) {
                const int x = baseX + lx;
                const BlockState::Id id = readSectionCell(sec, rowOff | lx);
                testCell(x, y, z, id, syz + int(x == t1_0 || x == t1_1));
                if (std::abs(distance) < EPS)
                  return 0.0;
              }
            }
          }
          curY = positive ? (secBaseY + 16) : (secBaseY - 1);
        }
      }
    }
    return std::abs(distance) < EPS ? 0.0 : distance;
  }

  // Horizontal axis: axis == 0 (X: t1=Y, t2=Z) or axis == 2 (Z: t1=X, t2=Y).
  const int y0 = (axis == 0) ? t1_0 : t2_0;
  const int y1 = (axis == 0) ? t1_1 : t2_1;
  const int yClamped0 = std::max(y0, minY_);
  const int yClamped1 = std::min(y1, maxY_ - 1);
  if (yClamped0 > yClamped1)
    return distance;
  const int sec0 = sectionIndex(yClamped0);
  const int sec1 = sectionIndex(yClamped1);
  const int other0 = (axis == 0) ? t2_0 : t1_0;
  const int other1 = (axis == 0) ? t2_1 : t1_1;
  const int cOther0 = chunkIndex(other0);
  const int cOther1 = chunkIndex(other1);

  int cAlong = chunkIndex(aStart);
  while (positive ? (cAlong <= chunkIndex(aEnd)) : (cAlong >= chunkIndex(aEnd))) {
    const int baseAlong = cAlong * 16;
    const int curEndChunk = chunkIndex(aEnd);
    if (positive ? (cAlong > curEndChunk) : (cAlong < curEndChunk))
      break;
    const int laStart = positive ? std::max(0, aStart - baseAlong)
                                 : std::min(15, aStart - baseAlong);
    for (int cOther = cOther0; cOther <= cOther1; ++cOther) {
      const int cx = (axis == 0) ? cAlong : cOther;
      const int cz = (axis == 0) ? cOther : cAlong;
      const Chunk *chunk = chunkAtCached(cx, cz);
      if (!chunk || !chunk->loaded)
        continue;
      const int baseOther = cOther * 16;
      const int lo0 = std::max(0, other0 - baseOther);
      const int lo1 = std::min(15, other1 - baseOther);
      for (int secIdx = sec0; secIdx <= sec1; ++secIdx) {
        const Section &sec = chunk->sections[static_cast<std::size_t>(secIdx)];
        if (sec.hasOnlyAir())
          continue;
        const int baseY = minY_ + secIdx * 16;
        const int ly0 = std::max(0, yClamped0 - baseY);
        const int ly1 = std::min(15, yClamped1 - baseY);
        for (int la = laStart;
             positive ? (la <= std::min(15, aEnd - baseAlong))
                      : (la >= std::max(0, aEnd - baseAlong));
             la += step) {
          const int a = baseAlong + la;
          if (positive ? (a > aEnd) : (a < aEnd))
            break;
          const int sa = int(a == aStart || a == aEnd);
          for (int ly = ly0; ly <= ly1; ++ly) {
            const int y = baseY + ly;
            const int say = sa + int(y == y0 || y == y1);
            const int yOff = ly << 8;
            for (int lo = lo0; lo <= lo1; ++lo) {
              const int otherCoord = baseOther + lo;
              const int x = (axis == 0) ? a : otherCoord;
              const int z = (axis == 0) ? otherCoord : a;
              const int cell = yOff | ((z & 15) << 4) | (x & 15);
              const BlockState::Id id = readSectionCell(sec, cell);
              testCell(x, y, z, id, say + int(otherCoord == other0 || otherCoord == other1));
              if (std::abs(distance) < EPS)
                return 0.0;
            }
          }
        }
      }
    }
    cAlong += step;
  }
  return std::abs(distance) < EPS ? 0.0 : distance;
}

bool World::isStepUpFullyBlocked(const Aabb &groundedBox, float maxStep,
                                 Vec3 movementStep) const {
  if (maxStep <= 0.0f)
    return false;
  const int yMinCell = floorToInt(groundedBox.minY + EPS);
  const int yMaxCell =
      floorToInt(groundedBox.maxY + static_cast<double>(maxStep) - EPS);
  if (yMinCell < minY_ || yMaxCell >= maxY_ || yMinCell > yMaxCell)
    return false;

  auto isFullWallColumn = [&](int bx, int bz) noexcept -> bool {
    const Chunk *chunk = chunkAtCached(chunkIndex(bx), chunkIndex(bz));
    if (!chunk || !chunk->loaded)
      return false;
    const int lx = bx & 15;
    const int lz = bz & 15;
    for (int by = yMinCell; by <= yMaxCell; ++by) {
      const Section &sec =
          chunk->sections[static_cast<std::size_t>(sectionIndex(by))];
      if (sec.hasOnlyAir())
        return false;
      const int cell = ((by & 15) << 8) | (lz << 4) | lx;
      if (stateCollisionKindId(readSectionCell(sec, cell)) !=
          BlockCollisionKind::FullCube)
        return false;
    }
    return true;
  };

  if (std::abs(movementStep.x) > 1.0)
    return false;
  const int zMin = floorToInt(groundedBox.minZ + EPS);
  const int zMax = floorToInt(groundedBox.maxZ - EPS);
  if (movementStep.x != 0.0) {
    const int checkX =
        movementStep.x > 0.0
            ? floorToInt(groundedBox.maxX + movementStep.x + EPS)
            : floorToInt(groundedBox.minX + movementStep.x - EPS);
    for (int bz = zMin; bz <= zMax; ++bz) {
      if (!isFullWallColumn(checkX, bz))
        return false;
    }
  } else {
    // Only skip if both sides were collided OR movement.x was 0, handled by Z check.
  }

  const double shiftedMinX =
      std::min(groundedBox.minX, groundedBox.minX + movementStep.x);
  const double shiftedMaxX =
      std::max(groundedBox.maxX, groundedBox.maxX + movementStep.x);
  const int xMin = floorToInt(shiftedMinX + EPS);
  const int xMax = floorToInt(shiftedMaxX - EPS);
  if (movementStep.z != 0.0) {
    const int checkZ =
        movementStep.z > 0.0
            ? floorToInt(groundedBox.maxZ + movementStep.z + EPS)
            : floorToInt(groundedBox.minZ + movementStep.z - EPS);
    for (int bx = xMin; bx <= xMax; ++bx) {
      if (!isFullWallColumn(bx, checkZ))
        return false;
    }
  }
  return true;
}
std::vector<VoxelShape> World::blockCollisions(const Aabb &box) {
  std::vector<VoxelShape> result;
  // BlockCollisions also visits the surrounding shell. Walls extend upward
  // by half a block, and moving pistons can expose shapes in adjacent cells.
  int x0 = static_cast<int>(std::floor(box.minX - EPS)) - 1,
      y0 = static_cast<int>(std::floor(box.minY - EPS)) - 1,
      z0 = static_cast<int>(std::floor(box.minZ - EPS)) - 1;
  int x1 = static_cast<int>(std::floor(box.maxX + EPS)) + 1,
      y1 = static_cast<int>(std::floor(box.maxY + EPS)) + 1,
      z1 = static_cast<int>(std::floor(box.maxZ + EPS)) + 1;
  for (int x = x0; x <= x1; ++x)
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z) {
        Pos p{x, y, z};
        if (!isLoaded(p))
          continue;
        int shellAxes = int(x == x0 || x == x1) +
                        int(y == y0 || y == y1) +
                        int(z == z0 || z == z1);
        if (shellAxes == 3)
          continue;
        auto state = getBlock(p);
        if (shellAxes == 2 && !state.is(Blocks::MOVING_PISTON))
          continue;
        auto shape = collisionShape(*this, p, state);
        if (shape.empty())
          continue;
        if (shellAxes == 1 &&
            std::none_of(shape.begin(), shape.end(), [](const auto &part) {
              return part.x0 < 0 || part.y0 < 0 || part.z0 < 0 ||
                     part.x1 > 16 || part.y1 > 16 || part.z1 > 16;
            }))
          continue;
        VoxelShape offset;
        for (auto b : shape)
          if (Aabb{b.x0 / 16.0 + x, b.y0 / 16.0 + y, b.z0 / 16.0 + z,
                   b.x1 / 16.0 + x, b.y1 / 16.0 + y, b.z1 / 16.0 + z}
                  .intersects(box))
            offset.push_back({b.x0 / 16.0 + x, b.y0 / 16.0 + y,
                              b.z0 / 16.0 + z, b.x1 / 16.0 + x,
                              b.y1 / 16.0 + y, b.z1 / 16.0 + z});
        if (!offset.empty())
          result.push_back(std::move(offset));
      }
  return result;
}
bool World::noCollision(const Aabb &box) {
  auto &boxes = scratchBoxes();
  collectCollisionBoxes(box, boxes);
  for (const auto &b : boxes)
    if (box.maxX > b.x0 + EPS && box.minX < b.x1 - EPS &&
        box.maxY > b.y0 + EPS && box.minY < b.y1 - EPS &&
        box.maxZ > b.z0 + EPS && box.minZ < b.z1 - EPS)
      return false;
  return true;
}
std::vector<ShapeBox> World::getEntityCollisions(const Entity *source,
                                                 const Aabb &box) const {
  std::vector<ShapeBox> result;
  collectEntityCollisions(source, box, result);
  return result;
}
void World::collectEntityCollisions(const Entity *source, const Aabb &box,
                                    std::vector<ShapeBox> &out) const {
  if (box.getXsize() < 1.0e-7 || box.getYsize() < 1.0e-7 ||
      box.getZsize() < 1.0e-7)
    return;
  const Aabb inflated = box.inflate(1.0e-7);
  const bool customSource = source != nullptr && source->hasCustomEntityCollision();
  forEachEntitySection(
      entitySections_, inflated, [&](const EntitySectionBucket &bucket) {
        if (bucket.active.empty())
          return;
        if (bucket.hardCollider.empty() &&
            (!customSource || bucket.pushable.empty()))
          return;
        const std::size_t words = bucket.wordCount();
        for (std::size_t w = 0; w < words; ++w) {
          std::uint64_t filter = bucket.hardCollider.wordAt(w);
          if (customSource)
            filter |= bucket.pushable.wordAt(w);
          std::uint64_t word = bucket.active.wordAt(w) & filter;
          while (word != 0ull) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(word));
            word &= (word - 1ull);
            const std::size_t idx = (w << 6u) | bit;
            const Aabb &otherBox = bucket.boxes[idx];
            if (!otherBox.intersects(inflated))
              continue;
            if (source != nullptr) {
              const Entity *other = bucket.entities[idx];
              if (!other || other == source || other->isRemoved())
                continue;
              if (!source->canCollideWith(*other))
                continue;
            }
            out.push_back({otherBox.minX, otherBox.minY, otherBox.minZ,
                           otherBox.maxX, otherBox.maxY, otherBox.maxZ});
          }
        }
      });
}
bool World::hasEntityCollision(const Entity *source, const Aabb &box) const {
  if (box.getXsize() < 1.0e-7 || box.getYsize() < 1.0e-7 ||
      box.getZsize() < 1.0e-7)
    return false;
  const Aabb inflated = box.inflate(1.0e-7);
  const bool customSource = source != nullptr && source->hasCustomEntityCollision();
  bool collided = false;
  forEachEntitySection(
      entitySections_, inflated, [&](const EntitySectionBucket &bucket) -> bool {
        if (bucket.active.empty())
          return true;
        if (bucket.hardCollider.empty() &&
            (!customSource || bucket.pushable.empty()))
          return true;
        const std::size_t words = bucket.wordCount();
        for (std::size_t w = 0; w < words; ++w) {
          std::uint64_t filter = bucket.hardCollider.wordAt(w);
          if (customSource)
            filter |= bucket.pushable.wordAt(w);
          std::uint64_t word = bucket.active.wordAt(w) & filter;
          while (word != 0ull) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(word));
            word &= (word - 1ull);
            const std::size_t idx = (w << 6u) | bit;
            const Aabb &otherBox = bucket.boxes[idx];
            if (!otherBox.intersects(inflated))
              continue;
            if (!(box.maxX > otherBox.minX + EPS &&
                  box.minX < otherBox.maxX - EPS &&
                  box.maxY > otherBox.minY + EPS &&
                  box.minY < otherBox.maxY - EPS &&
                  box.maxZ > otherBox.minZ + EPS &&
                  box.minZ < otherBox.maxZ - EPS))
              continue;
            if (source != nullptr) {
              const Entity *other = bucket.entities[idx];
              if (!other || other == source || other->isRemoved())
                continue;
              if (!source->canCollideWith(*other))
                continue;
            }
            collided = true;
            return false;
          }
        }
        return true;
      });
  return collided;
}
bool World::noCollision(const Entity *source, const Aabb &box) {
  if (!noCollision(box))
    return false;
  return !hasEntityCollision(source, box);
}
std::optional<Pos> World::findSupportingBlock(const Entity &source,
                                              const Aabb &box) {
  std::optional<Pos> mainSupport;
  double mainDist = std::numeric_limits<double>::max();
  const int x0 = static_cast<int>(std::floor(box.minX - EPS)) - 1;
  const int y0 = static_cast<int>(std::floor(box.minY - EPS)) - 1;
  const int z0 = static_cast<int>(std::floor(box.minZ - EPS)) - 1;
  const int x1 = static_cast<int>(std::floor(box.maxX + EPS)) + 1;
  const int y1 = static_cast<int>(std::floor(box.maxY + EPS)) + 1;
  const int z1 = static_cast<int>(std::floor(box.maxZ + EPS)) + 1;
  for (int x = x0; x <= x1; ++x) {
    for (int y = y0; y <= y1; ++y) {
      for (int z = z0; z <= z1; ++z) {
        const Pos pos{x, y, z};
        if (!isLoaded(pos))
          continue;
        const BlockState state = getBlock(pos);
        if (state.isAir())
          continue;
        const auto shape = collisionShape(*this, pos, state);
        if (shape.empty())
          continue;
        bool hits = false;
        for (const auto &b : shape) {
          if (Aabb{b.x0 / 16.0 + x, b.y0 / 16.0 + y, b.z0 / 16.0 + z,
                   b.x1 / 16.0 + x, b.y1 / 16.0 + y, b.z1 / 16.0 + z}
                  .intersects(box)) {
            hits = true;
            break;
          }
        }
        if (!hits)
          continue;
        const double dx = (x + 0.5) - source.position.x;
        const double dy = (y + 0.5) - source.position.y;
        const double dz = (z + 0.5) - source.position.z;
        const double dist = dx * dx + dy * dy + dz * dz;
        if (dist < mainDist ||
            (dist == mainDist &&
             (!mainSupport.has_value() ||
              std::tie(pos.x, pos.y, pos.z) <
                  std::tie(mainSupport->x, mainSupport->y, mainSupport->z)))) {
          mainDist = dist;
          mainSupport = pos;
        }
      }
    }
  }
  return mainSupport;
}
std::optional<Vec3> World::findFreePosition(const Entity *source,
                                            const Aabb &allowedCenters,
                                            Vec3 preferredCenter, double sizeX,
                                            double sizeY, double sizeZ) {
  (void)source;
  const double halfX = sizeX * 0.5, halfY = sizeY * 0.5, halfZ = sizeZ * 0.5;
  const Vec3 clamped{
      std::clamp(preferredCenter.x, allowedCenters.minX, allowedCenters.maxX),
      std::clamp(preferredCenter.y, allowedCenters.minY, allowedCenters.maxY),
      std::clamp(preferredCenter.z, allowedCenters.minZ, allowedCenters.maxZ)};
  if (noCollision(Aabb::ofSize(clamped, sizeX, sizeY, sizeZ)))
    return clamped;
  const auto &boxes = scratchBoxes();
  auto &mutableBoxes = const_cast<std::vector<ShapeBox> &>(boxes);
  collectCollisionBoxes(allowedCenters.inflate(halfX, halfY, halfZ),
                        mutableBoxes);
  std::optional<Vec3> best;
  double bestDist = std::numeric_limits<double>::max();
  for (int ix = 0; ix <= 2; ++ix) {
    const double cx =
        ix == 0 ? allowedCenters.minX
                : (ix == 1 ? clamped.x : allowedCenters.maxX);
    for (int iy = 0; iy <= 2; ++iy) {
      const double cy =
          iy == 0 ? allowedCenters.minY
                  : (iy == 1 ? clamped.y : allowedCenters.maxY);
      for (int iz = 0; iz <= 2; ++iz) {
        const double cz =
            iz == 0 ? allowedCenters.minZ
                    : (iz == 1 ? clamped.z : allowedCenters.maxZ);
        const Vec3 candidate{cx, cy, cz};
        const Aabb test = Aabb::ofSize(candidate, sizeX, sizeY, sizeZ);
        if (!noCollision(test))
          continue;
        const double d = (candidate - preferredCenter).lengthSqr();
        if (d < bestDist) {
          bestDist = d;
          best = candidate;
        }
      }
    }
  }
  return best;
}
namespace {
struct BoxHitInfo {
  double t = 0.0;
  Direction face = Direction::Up;
  Vec3 location{};
};

std::optional<BoxHitInfo> clipSegmentBox(Vec3 from, Vec3 to, double x0,
                                         double y0, double z0, double x1,
                                         double y1, double z1) {
  const double dx = to.x - from.x;
  const double dy = to.y - from.y;
  const double dz = to.z - from.z;
  double tMin = 0.0;
  double tMax = 1.0;
  Direction hitFace = Direction::Up;
  const auto checkAxis = [&](double start, double delta, double minB,
                             double maxB, Direction negFace,
                             Direction posFace) {
    if (std::abs(delta) < 1.0e-7)
      return start >= minB && start <= maxB;
    double inv = 1.0 / delta;
    double t1 = (minB - start) * inv;
    double t2 = (maxB - start) * inv;
    Direction nearFace = negFace;
    if (t1 > t2) {
      std::swap(t1, t2);
      nearFace = posFace;
    }
    if (t1 > tMin) {
      tMin = t1;
      hitFace = nearFace;
    }
    tMax = std::min(tMax, t2);
    return tMin <= tMax;
  };
  if (!checkAxis(from.x, dx, x0, x1, Direction::West, Direction::East) ||
      !checkAxis(from.y, dy, y0, y1, Direction::Down, Direction::Up) ||
      !checkAxis(from.z, dz, z0, z1, Direction::North, Direction::South)) {
    return std::nullopt;
  }
  return BoxHitInfo{
      tMin, hitFace, {from.x + dx * tMin, from.y + dy * tMin, from.z + dz * tMin}};
}
} // namespace

BlockHitResult World::clipBlock(Vec3 from, Vec3 to, bool fluidsOnly) {
  ++clipCalls;
  const double rawX = to.x - from.x;
  const double rawY = to.y - from.y;
  const double rawZ = to.z - from.z;
  const double fromX = from.x - rawX * 1.0e-7;
  const double fromY = from.y - rawY * 1.0e-7;
  const double fromZ = from.z - rawZ * 1.0e-7;
  const double toX = to.x + rawX * 1.0e-7;
  const double toY = to.y + rawY * 1.0e-7;
  const double toZ = to.z + rawZ * 1.0e-7;

  const auto hits = [&](int x, int y, int z) -> std::optional<BlockHitResult> {
    ++clipCells;
    const Pos p{x, y, z};
    const BlockState state = getBlock(p);
    if (fluidsOnly) {
      const FluidState fs = resolveEntityFluidState(state);
      if (!fs.isEmpty() && fs.isSource()) {
        const float h = static_cast<float>(redstone::fluidHeight(*this, p, fs));
        if (auto info =
                clipSegmentBox(from, to, x, y, z, x + 1.0, y + h, z + 1.0)) {
          return BlockHitResult{true, p, info->face, info->location,
                                info->t == 0.0};
        }
      }
      return std::nullopt;
    }
    if (state.isAir())
      return std::nullopt;
    VoxelShape moving;
    const VoxelShape &shape = shapeAt(*this, p, state, moving);
    std::optional<BoxHitInfo> best;
    for (const auto &b : shape) {
      if (auto info = clipSegmentBox(from, to, b.x0 / 16.0 + x, b.y0 / 16.0 + y,
                                     b.z0 / 16.0 + z, b.x1 / 16.0 + x,
                                     b.y1 / 16.0 + y, b.z1 / 16.0 + z)) {
        if (!best.has_value() || info->t < best->t)
          best = info;
      }
    }
    if (!best.has_value())
      return std::nullopt;
    return BlockHitResult{true, p, best->face, best->location, best->t == 0.0};
  };

  int cellX = floorToInt(fromX);
  int cellY = floorToInt(fromY);
  int cellZ = floorToInt(fromZ);
  if (auto r = hits(cellX, cellY, cellZ))
    return *r;

  const double dx = toX - fromX;
  const double dy = toY - fromY;
  const double dz = toZ - fromZ;
  const int stepX = dx == 0.0 ? 0 : (dx > 0.0 ? 1 : -1);
  const int stepY = dy == 0.0 ? 0 : (dy > 0.0 ? 1 : -1);
  const int stepZ = dz == 0.0 ? 0 : (dz > 0.0 ? 1 : -1);
  const double deltaX =
      stepX == 0 ? std::numeric_limits<double>::max() : stepX / dx;
  const double deltaY =
      stepY == 0 ? std::numeric_limits<double>::max() : stepY / dy;
  const double deltaZ =
      stepZ == 0 ? std::numeric_limits<double>::max() : stepZ / dz;
  const double fracX = fromX - std::floor(fromX);
  const double fracY = fromY - std::floor(fromY);
  const double fracZ = fromZ - std::floor(fromZ);
  double nextX = deltaX * (stepX > 0 ? 1.0 - fracX : fracX);
  double nextY = deltaY * (stepY > 0 ? 1.0 - fracY : fracY);
  double nextZ = deltaZ * (stepZ > 0 ? 1.0 - fracZ : fracZ);

  while (nextX <= 1.0 || nextY <= 1.0 || nextZ <= 1.0) {
    if (nextX < nextY) {
      if (nextX < nextZ) {
        cellX += stepX;
        nextX += deltaX;
      } else {
        cellZ += stepZ;
        nextZ += deltaZ;
      }
    } else if (nextY < nextZ) {
      cellY += stepY;
      nextY += deltaY;
    } else {
      cellZ += stepZ;
      nextZ += deltaZ;
    }
    if (auto r = hits(cellX, cellY, cellZ))
      return *r;
  }
  return {};
}

bool World::clip(Vec3 from, Vec3 to) { return clipBlock(from, to, false).hit; }

std::optional<EntityHitResult> World::clipEntity(
    const Entity *source, Vec3 from, Vec3 to, const Aabb &searchBox,
    const std::function<bool(const Entity &)> &filter) const {
  double closestDistSqr = std::numeric_limits<double>::max();
  std::optional<EntityHitResult> best;
  for (Entity *candidate : entitiesIn(searchBox)) {
    if (!candidate || candidate == source || candidate->isRemoved())
      continue;
    if (filter && !filter(*candidate))
      continue;
    const Aabb bb = candidate->boundingBox().inflate(0.3);
    if (bb.contains(from)) {
      return EntityHitResult{candidate, from};
    }
    if (auto hit = clipSegmentBox(from, to, bb.minX, bb.minY, bb.minZ, bb.maxX,
                                  bb.maxY, bb.maxZ)) {
      const double distSqr = (hit->location - from).lengthSqr();
      if (distSqr < closestDistSqr) {
        closestDistSqr = distSqr;
        best = EntityHitResult{candidate, hit->location};
      }
    }
  }
  return best;
}
void World::spawnItem(Pos pos, const std::string &item, int count,
                      bool defaultPickupDelay) {
  if (item.empty() || count <= 0)
    return;
  double halfHeight = 0.125;
  Vec3 at{pos.x + 0.5 + random.nextDouble(-0.25, 0.25),
          pos.y + 0.5 + random.nextDouble(-0.25, 0.25) - halfHeight,
          pos.z + 0.5 + random.nextDouble(-0.25, 0.25)};
  spawnItemAt(at, item, count, defaultPickupDelay);
}
void World::spawnItemAt(Vec3 pos, const std::string &item, int count,
                        bool defaultPickupDelay) {
  if (item.empty() || count <= 0)
    return;
  auto entity = std::make_shared<ItemEntity>(*this, pos, itemStack(item, count));
  if (defaultPickupDelay)
    entity->pickupDelay = 10;
  addEntity(entity);
  log.push_back({"entity_spawn",
                 {static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))},
                 "minecraft:item:" + canonical(item)});
}
void World::spawnPrimedTnt(Vec3 pos, int fuse) {
  auto entity = std::make_shared<PrimedTnt>(*this, pos);
  entity->fuse = fuse;
  addEntity(entity);
  log.push_back({"entity_spawn",
                 {static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))},
                 "minecraft:tnt"});
}
void World::spawnFallingBlock(Vec3 pos, const BlockState &state) {
  auto entity = std::make_shared<FallingBlockEntity>(*this, pos, state);
  addEntity(entity);
  log.push_back({"entity_spawn",
                 {static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))},
                 "minecraft:falling_block:" + state.name()});
}
void World::spawnMinecart(Vec3 pos) {
  auto entity = std::make_shared<Minecart>(*this, pos);
  addEntity(entity);
  log.push_back({"entity_spawn",
                 {static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))},
                 "minecraft:minecart"});
}
bool World::primeTntBlock(Pos pos) {
  if (!tntExplodes)
    return false;
  spawnPrimedTnt({pos.x + 0.5, double(pos.y), pos.z + 0.5}, 80);
  setBlock(pos, airState(), 3);
  return true;
}
} // namespace redstone
