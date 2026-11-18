#include "engine/internal.hpp"

#include "redstone/light/chunk_input.hpp"

namespace redstone {
World::World(int minY, int maxY, int maxNeighborUpdates)
    : minY_(minY), maxY_(maxY), maxUpdates_(maxNeighborUpdates),
      tickQueue_(std::make_unique<BlockTickQueue>()),
      fluidTickQueue_(std::make_unique<FluidTickQueue>()),
      poiStorage_(std::make_unique<PoiStorage>(minY, maxY)),
      blockEventQueue_(std::make_unique<BlockEventQueue>()) {
  // Chunk section storage is a fixed inline array; reject a taller dimension
  // instead of silently truncating sections or indexing out of bounds.
  if (sectionCount() > kSectionCapacity)
    throw std::invalid_argument(
        "World: vertical range needs more than kSectionCapacity sections");
  dimension_.minY = minY;
  dimension_.height = maxY - minY;
  dimension_.logicalHeight = dimension_.height;
  cleanup_.manager = std::make_shared<PeriodicCleanupManager>();
  registerTickCleanupTasks();
}

World::CleanupRegistryHandle::~CleanupRegistryHandle() { unregisterAll(); }

World::CleanupRegistryHandle &
World::CleanupRegistryHandle::operator=(CleanupRegistryHandle &&other) noexcept {
  if (this != &other) {
    unregisterAll();
    manager = std::move(other.manager);
    autoAdvance = other.autoAdvance;
    blockTickTaskId = std::exchange(other.blockTickTaskId, 0);
    fluidTickTaskId = std::exchange(other.fluidTickTaskId, 0);
    lightTaskId = std::exchange(other.lightTaskId, 0);
  }
  return *this;
}

void World::CleanupRegistryHandle::unregisterAll() {
  if (!manager)
    return;
  if (lightTaskId != 0) {
    manager->unregisterTask(lightTaskId);
    lightTaskId = 0;
  }
  if (blockTickTaskId != 0) {
    manager->unregisterTask(blockTickTaskId);
    blockTickTaskId = 0;
  }
  if (fluidTickTaskId != 0) {
    manager->unregisterTask(fluidTickTaskId);
    fluidTickTaskId = 0;
  }
}

void World::registerTickCleanupTasks() {
  if (!cleanup_.manager)
    return;
  if (cleanup_.blockTickTaskId == 0 && tickQueue_) {
    BlockTickQueue *queue = tickQueue_.get();
    cleanup_.blockTickTaskId = cleanup_.manager->registerTask(
        "block_ticks", [queue](CleanupExecutionGate::Lease &) {
          queue->compactContainers();
          return true;
        });
  }
  if (cleanup_.fluidTickTaskId == 0 && fluidTickQueue_) {
    FluidTickQueue *queue = fluidTickQueue_.get();
    cleanup_.fluidTickTaskId = cleanup_.manager->registerTask(
        "fluid_ticks", [queue](CleanupExecutionGate::Lease &) {
          queue->compactContainers();
          return true;
        });
  }
}

void World::registerLightCleanupTask() {
  if (!cleanup_.manager || !lightService_ || cleanup_.lightTaskId != 0)
    return;
  LightService *service = lightService_.get();
  cleanup_.lightTaskId = cleanup_.manager->registerTask(
      "light_task_queues",
      [service](CleanupExecutionGate::Lease &lease) {
        if (!service->needsTaskQueueCompact())
          return false;
        return service->requestTaskQueueCompact(std::move(lease));
      });
}

void World::unregisterLightCleanupTask() {
  if (cleanup_.manager && cleanup_.lightTaskId != 0) {
    cleanup_.manager->unregisterTask(cleanup_.lightTaskId);
    cleanup_.lightTaskId = 0;
  }
}

void World::setCleanupManager(std::shared_ptr<PeriodicCleanupManager> manager,
                              bool autoAdvance) {
  cleanup_.unregisterAll();
  cleanup_.manager =
      manager ? std::move(manager) : std::make_shared<PeriodicCleanupManager>();
  cleanup_.autoAdvance = autoAdvance;
  registerTickCleanupTasks();
  registerLightCleanupTask();
}

World::World(DimensionTypeSpec dimension, int maxNeighborUpdates,
             std::string_view dimensionId)
    : World(dimension.minY, dimension.maxY(), maxNeighborUpdates) {
  validateEngineDimensionType(dimension, kSectionCapacity);
  setDimension(std::move(dimension), dimensionId);
}

void World::setDimension(DimensionTypeSpec dimension,
                         std::string_view dimensionId) {
  validateEngineDimensionType(dimension, kSectionCapacity);
  if (dimension.minY != minY_ || dimension.maxY() != maxY_) {
    throw std::invalid_argument(
        "World::setDimension: vertical range does not match world bounds");
  }
  const std::string resolvedId = dimensionId.empty()
                                     ? (dimension.id.empty()
                                            ? std::string("minecraft:overworld")
                                            : normalizeDimensionId(dimension.id))
                                     : normalizeDimensionId(dimensionId);
  dimension_ = std::move(dimension);
  if (dimension_.id.empty())
    dimension_.id = resolvedId;
  dimensionId_ = resolvedId;
  waterEvaporates = dimension_.waterEvaporates;
  fastLava = dimension_.fastLava;
}
World::~World() {
  disableLighting();
  cleanup_.unregisterAll();
}
World::World(World &&) noexcept = default;
World &World::operator=(World &&) noexcept = default;

BlockTagRegistry::ReloadResult World::reloadBlockTags(const std::vector<std::filesystem::path> &packs) {
  auto next = std::make_shared<BlockTagRegistry>();
  auto result = next->reload(packs);
  if (result.applied) {
    blockTags_ = std::move(next);
    ++blockTagRevision_;
  }
  return result;
}
std::optional<bool> World::hasBlockTag(const BlockState &state, std::string_view tag) const {
  return blockTags_ ? blockTags_->contains(tag, state.nameView()) : blockHasTag(state, tag);
}


void World::setSeed(std::uint64_t value) {
  seed = value;
  random.setSeed(static_cast<std::int64_t>(value));
  randValue_ = static_cast<std::uint32_t>(
      (value ^ (value >> 32)) * 0x9E3779B9u + 1013904223u);
}

const World::Chunk *World::chunkAtCached(int x, int z) const {
  const auto key = chunkKey(x, z);
  ChunkCacheSlot &slot = blockReadCache_.chunks[(key * 0x9E3779B97F4A7C15ull) >> 58];
  if (slot.key == key)
    return slot.chunk && slot.chunk->loaded ? slot.chunk : nullptr;
  auto it = chunks_.find(key);
  slot.key = key;
  slot.chunk = it == chunks_.end() ? nullptr : &it->second;
  return slot.chunk && slot.chunk->loaded ? slot.chunk : nullptr;
}
std::uint64_t World::chunkLoadEpochForTest(int x, int z) const {
  const Chunk *chunk = chunkAt(x, z);
  return chunk ? chunk->loadEpoch : 0;
}

std::uint64_t World::chunkLightEpochForTest(int x, int z) const {
  const Chunk *chunk = chunkAt(x, z);
  return chunk ? chunk->lightLifecycleEpoch : 0;
}

std::uint64_t World::chunkRequiredLightGenerationForTest(int x, int z) const {
  const Chunk *chunk = chunkAt(x, z);
  return chunk ? chunk->requiredLightGeneration : 0;
}

World::Chunk *World::chunkAt(int x, int z) {
  return const_cast<Chunk *>(chunkAtCached(x, z));
}
const World::Chunk *World::chunkAt(int x, int z) const {
  return chunkAtCached(x, z);
}
void World::Chunk::refreshRandomTickMask(int count) {
  std::uint32_t mask = 0;
  for (int i = 0; i < count; ++i) {
    if (sections[static_cast<std::size_t>(i)].isRandomlyTicking())
      mask |= (std::uint32_t{1} << i);
  }
  randomTickingSectionMask = mask;
}
void World::Chunk::refreshPersistedLightIndex(int minSectionY,
                                              int numSections) {
  persistedLightSectionIndex.fill(-1);
  if (!persistedLight || !persistedLight->isLightOn)
    return;
  for (std::size_t i = 0; i < persistedLight->sections.size(); ++i) {
    const int rel = persistedLight->sections[i].sectionY - minSectionY;
    if (rel >= 0 && rel < numSections &&
        persistedLightSectionIndex[static_cast<std::size_t>(rel)] < 0) {
      persistedLightSectionIndex[static_cast<std::size_t>(rel)] =
          static_cast<std::int16_t>(i);
    }
  }
}
bool World::installManagedChunk(
    int x, int z, Chunk &&chunk,
    const std::vector<ScheduledTick> &blockTicks,
    const std::vector<std::pair<ScheduledTick, FluidRef>> &fluidTicks,
    std::span<const std::shared_ptr<PistonMovingBlockEntity>> pistons,
    std::span<const std::shared_ptr<Entity>> itemEntities,
    std::span<const std::shared_ptr<Entity>> tntEntities) {
  const std::uint64_t key = chunkKey(x, z);
  if (chunks_.contains(key))
    return false;
  chunk.loaded = false;
  chunk.randomTickIndex = Chunk::kInvalidRandomTickIndex;
  chunk.initialize({x, z}, minY_, maxY_ - minY_);
  const std::uint64_t loadEpoch = chunk.lightEpoch;
  decltype(chunks_)::iterator insertedIt;
  {
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    insertedIt = chunks_.emplace(key, std::move(chunk)).first;
  }
  insertedIt->second.refreshPersistedLightIndex(minY_ >> 4, sectionCount());
  try {
    tickQueue_->addContainer(key);
    fluidTickQueue_->addContainer(key);
    for (const ScheduledTick &tick : blockTicks) {
      if (!tickQueue_->restore(key, tick, tick.block.block()))
        throw std::logic_error("failed to restore block tick");
    }
    for (const auto &[tick, fluid] : fluidTicks) {
      if (!fluidTickQueue_->restore(key, tick, fluid.fluid()))
        throw std::logic_error("failed to restore fluid tick");
    }
    // Block entities and tickers are installed before loadChunk publishes the
    // optical snapshot and lifecycle edge.
    for (const auto &piston : pistons) {
      if (!piston)
        throw std::logic_error("null moving piston entity");
      setPistonEntity(piston->pos, piston);
    }
    for (const auto &entity : itemEntities) {
      if (!entity)
        throw std::logic_error("null persisted item entity");
      if (!entityUuids_.insert(entity->uuid).second)
        throw std::logic_error("duplicate entity UUID during install");
      entity->id = allocateEntityId();
      entity->removalObserved_ = false;
      entities.push_back(entity);
      entitySectionAdd(*entity);
    }
    for (const auto &entity : tntEntities) {
      if (!entity)
        throw std::logic_error("null persisted tnt entity");
      if (!entityUuids_.insert(entity->uuid).second)
        throw std::logic_error("duplicate entity UUID during install");
      entity->id = allocateEntityId();
      entity->removalObserved_ = false;
      entities.push_back(entity);
      entitySectionAdd(*entity);
    }
    // 生成期已由主线程提交过光照几何与首次计算请求：直接接管，不再整份重传，
    // 只把光照生命周期代次对齐到引擎那一代（与 installProtoChunkIntoWorld 同语义）。
    const bool lightAlreadySubmitted =
        insertedIt->second.requiredLightGeneration != 0 && lightService_;
    loadChunk(x, z, /*registerLight=*/!lightAlreadySubmitted, loadEpoch);
    if (lightAlreadySubmitted) {
      ++generatedLightAdoptions_;
      alignChunkLightLifecycle(x, z);
      // 低内存模式：生成期登记的是 ProtoChunk 句柄，安装后 World 数据与它完全一致，
      // 直接切成直读；非低内存模式没有直读源，退回值快照。
      if (!adoptChunkLightDirectGeometry(x, z))
        reloadChunkLight(x, z);
    }
    reconcilePoiFromChunk(x, z, insertedIt->second);
  } catch (...) {
    std::vector<std::shared_ptr<Entity>> insertedEntities;
    insertedEntities.reserve(itemEntities.size() + tntEntities.size());
    insertedEntities.insert(insertedEntities.end(), itemEntities.begin(),
                            itemEntities.end());
    insertedEntities.insert(insertedEntities.end(), tntEntities.begin(),
                            tntEntities.end());
    for (const auto &entity : insertedEntities) {
      if (!entity ||
          std::find(entities.begin(), entities.end(), entity) == entities.end())
        continue;
      entityUuids_.erase(entity->uuid);
      entitySectionRemove(*entity);
      std::erase(entities, entity);
    }
    for (const auto &piston : pistons)
      if (piston)
        removePistonEntity(piston->pos, false);
    sweepPistonTickers();
    tickQueue_->removeContainer(key);
    fluidTickQueue_->removeContainer(key);
    removeRandomTickingChunk(insertedIt->second);
    {
      std::unique_lock<std::shared_mutex> resizeLock;
      if (std::shared_mutex *mu = directResizeMutex())
        resizeLock = std::unique_lock<std::shared_mutex>(*mu);
      chunks_.erase(insertedIt);
    }
    blockReadCache_.reset();
    throw;
  }
  return true;
}

bool World::managedChunkTransientUnloadState(
    int x, int z, std::string &reason) const {
  if (handlingTick_) {
    reason = "chunk lifecycle cannot evict during the World tick";
    return true;
  }
  const std::uint64_t key = chunkKey(x, z);
  for (const BlockEvent &event : blockEventQueue_->order) {
    if (chunkKey(event.pos.x >> 4, event.pos.z >> 4) == key) {
      reason = "chunk has a transient pending block event";
      return true;
    }
  }
  for (const BlockEvent &event : blockEventsToReschedule_) {
    if (chunkKey(event.pos.x >> 4, event.pos.z >> 4) == key) {
      reason = "chunk has a transient deferred block event";
      return true;
    }
  }
  return false;
}

bool World::managedChunkPersistenceState(int x, int z,
                                         std::string &reason) const {
  const std::uint64_t key = chunkKey(x, z);
  if (entityCountsByChunk_.contains(key)) {
    for (const Entity *entity : entitiesInSectionRange(x, x, z, z)) {
      if (!dynamic_cast<const ItemEntity *>(entity) &&
          !dynamic_cast<const PrimedTnt *>(entity) &&
          !dynamic_cast<const FallingBlockEntity *>(entity) &&
          !dynamic_cast<const Minecart *>(entity)) {
        reason =
            "chunk contains an entity type outside supported persistence";
        return true;
      }
    }
  }
  if (containerPositionsByChunk_.contains(key)) {
    reason = "chunk contains container block entities outside supported "
             "managed persistence";
    return true;
  }
  return false;
}

bool World::removeManagedChunk(int x, int z, std::string &error,
                               bool allowTransientDrop) {
  if (managedChunkPersistenceState(x, z, error) ||
      (!allowTransientDrop &&
       managedChunkTransientUnloadState(x, z, error)))
    return false;
  const std::uint64_t key = chunkKey(x, z);
  auto found = chunks_.find(key);
  if (found == chunks_.end()) {
    error = "managed chunk is not resident in World";
    return false;
  }
  // Invalidate every direct chunk/section pointer before node destruction.
  blockReadCache_.reset();
  unloadChunk(x, z);
  tickQueue_->removeContainer(key);
  fluidTickQueue_->removeContainer(key);
  std::erase_if(blockEventQueue_->order, [key](const BlockEvent &event) {
    return chunkKey(event.pos.x >> 4, event.pos.z >> 4) == key;
  });
  blockEventQueue_->index.clear();
  for (const BlockEvent &event : blockEventQueue_->order)
    blockEventQueue_->index.insert(event);
  std::erase_if(blockEventsToReschedule_, [key](const BlockEvent &event) {
    return chunkKey(event.pos.x >> 4, event.pos.z >> 4) == key;
  });
  std::vector<Pos> pistonPositions;
  if (auto indexed = pistonPositionsByChunk_.find(key);
      indexed != pistonPositionsByChunk_.end())
    pistonPositions = indexed->second;
  for (const Pos &pos : pistonPositions)
    removePistonEntity(pos, false);
  if (!pistonPositions.empty())
    sweepPistonTickers();
  std::vector<Pos> containerPositions;
  if (auto indexed = containerPositionsByChunk_.find(key);
      indexed != containerPositionsByChunk_.end())
    containerPositions = indexed->second;
  for (const Pos &pos : containerPositions)
    removeContainerEntity(pos, false, false);
  if (!containerPositions.empty())
    sweepContainerTickers();
  const std::vector<Entity *> chunkEntities =
      entitiesInSectionRange(x, x, z, z);
  for (Entity *entity : chunkEntities) {
    entityUuids_.erase(entity->uuid);
    entitySectionRemove(*entity);
    std::erase_if(entities, [entity](const std::shared_ptr<Entity> &owned) {
      return owned.get() == entity;
    });
  }
  removeRandomTickingChunk(found->second);
  if (poiStorage_)
    poiStorage_->removeChunk(x, z);
  {
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    chunks_.erase(found);
  }
  blockReadCache_.reset();
  return true;
}

void World::addRandomTickingChunk(Chunk &chunk, std::uint64_t key) {
  if (chunk.randomTickIndex != Chunk::kInvalidRandomTickIndex)
    return;
  chunk.randomTickIndex = static_cast<std::uint32_t>(randomTickingChunks_.size());
  const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
  const int chunkZ =
      static_cast<int>(static_cast<std::int32_t>(key & 0xffffffffu));
  randomTickingChunks_.push_back(TickingChunkEntry{key, &chunk, chunkX, chunkZ});
  randomTickingDirty_ = true;
}

void World::removeRandomTickingChunk(Chunk &chunk) {
  const std::uint32_t idx = chunk.randomTickIndex;
  if (idx == Chunk::kInvalidRandomTickIndex)
    return;
  const std::size_t lastIdx = randomTickingChunks_.size() - 1;
  if (idx != lastIdx) {
    TickingChunkEntry moved = randomTickingChunks_[lastIdx];
    randomTickingChunks_[idx] = moved;
    moved.chunk->randomTickIndex = idx;
    randomTickingDirty_ = true;
  }
  randomTickingChunks_.pop_back();
  chunk.randomTickIndex = Chunk::kInvalidRandomTickIndex;
}

void World::updateChunkRandomTickMask(Chunk &chunk, std::uint64_t key,
                                      int sectionIdx, bool sectionTicking) {
  if (sectionIdx < 0 || sectionIdx >= kSectionCapacity)
    return;
  const std::uint32_t bit = std::uint32_t{1} << sectionIdx;
  const std::uint32_t prevMask = chunk.randomTickingSectionMask;
  const std::uint32_t nextMask =
      sectionTicking ? (prevMask | bit) : (prevMask & ~bit);
  if (nextMask == prevMask)
    return;
  chunk.randomTickingSectionMask = nextMask;
  if (!chunk.loaded)
    return;
  if (prevMask == 0 && nextMask != 0)
    addRandomTickingChunk(chunk, key);
  else if (prevMask != 0 && nextMask == 0)
    removeRandomTickingChunk(chunk);
}

void World::loadChunk(int x, int z, bool registerLight,
                      std::uint64_t lightEpoch) {
  const auto key = chunkKey(x, z);
  // 已经常驻：节点、高度图、刻容器、光照登记、生命周期代次全部就位，重复调用
  // 不该有任何副作用（尤其不该再向光照引擎提交一次）。
  // chunks_ 是 unordered_map，节点地址稳定，读缓存里的 Chunk* 仍然有效。
  const auto existing = chunks_.find(key);
  if (existing != chunks_.end() && existing->second.loaded)
    return;
  blockReadCache_.sectionIndex = -1;
  auto it = existing;
  if (it == chunks_.end()) {
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    it = chunks_.emplace(key, Chunk{}).first;
  }
  auto &c = it->second;
  c.initialize({x, z}, minY_, maxY_ - minY_);
  const bool wasLoaded = c.loaded;
  c.loaded = true;
  // 接管流水线登记时（registerLight=false）不动代次，由对齐函数决定；
  // 受管安装会显式传入生成/读取请求的加载生命周期，保证 Proto -> World 不变。
  if (!wasLoaded && registerLight) {
    if (lightEpoch != 0)
      c.lightLifecycleEpoch = lightEpoch;
    else
      ++c.lightLifecycleEpoch;
  }
  if (lightEpoch != 0) {
    c.loadEpoch = lightEpoch;
    c.lightEpoch = lightEpoch;
  }
  if (c.randomTickingSectionMask != 0)
    addRandomTickingChunk(c, key);
  if (!c.heightmaps)
    c.heightmaps = std::make_unique<ChunkHeightmaps>(minY_, sectionCount() * 16);
  // Publish the fresh node so a cached miss cannot mask the load.
  ChunkCacheSlot &slot = blockReadCache_.chunks[(key * 0x9E3779B97F4A7C15ull) >> 58];
  slot.key = key;
  slot.chunk = &c;
  // LevelTicks.addContainer: every ticking chunk owns exactly one tick
  // container in each queue. Reloading a chunk keeps the pending ticks it
  // already had.
  tickQueue_->addContainer(chunkKey(x, z));
  fluidTickQueue_->addContainer(chunkKey(x, z));
  if (lightService_) {
    flushPendingLightCells();
    if (registerLight) {
      if (auto input = makeLightChunkInput(x, z)) {
        const std::uint64_t seq = lightService_->submitChunk(std::move(*input));
        if (seq > 0)
          c.requiredLightGeneration = std::max(c.requiredLightGeneration, seq);
        c.persistedLight.reset();
      }
    }
    if (!wasLoaded && lightChunkLifecycleObserver)
      lightChunkLifecycleObserver(x, z, true);
  }
}

void World::alignChunkLightLifecycle(int x, int z) {
  if (!lightService_)
    return;
  Chunk *chunk = chunkAt(x, z);
  if (!chunk)
    return;
  const LightService::ChunkStatus status = lightService_->chunkStatus(x, z);
  if (status.loaded && status.lifecycleEpoch != 0)
    chunk->lightLifecycleEpoch = status.lifecycleEpoch;
}
void World::unloadChunk(int x, int z) {
  blockReadCache_.sectionIndex = -1;
  const auto key = chunkKey(x, z);
  auto it = chunks_.find(key);
  const bool wasLoaded = it != chunks_.end() && it->second.loaded;
  if (wasLoaded) {
    it->second.loaded = false;
    removeRandomTickingChunk(it->second);
  }
  if (lightService_) {
    flushPendingLightCells();
    // The lifecycle tombstone invalidates both full snapshots and queued owned
    // cell records from this loaded lifetime.
    lightService_->unloadChunk(x, z);
    if (wasLoaded && lightChunkLifecycleObserver)
      lightChunkLifecycleObserver(x, z, false);
  }
}

LightSectionView World::sectionToLightView(const Section &data,
                                           bool atomicHeader) noexcept {
  LightSectionView view;
  const std::uint16_t nonEmpty =
      atomicHeader
          ? std::atomic_ref<std::uint16_t>(
                const_cast<std::uint16_t &>(data.nonEmpty))
                .load(std::memory_order_relaxed)
          : data.nonEmpty;
  view.empty = nonEmpty == 0;
  const SectionStorage *base = data.storage.get();
  if (!base) {
    view.bits = 0;
    view.single = data.single;
    return view;
  }
  const std::uint16_t paletteSize =
      atomicHeader
          ? std::atomic_ref<std::uint16_t>(
                const_cast<std::uint16_t &>(base->paletteSize))
                .load(std::memory_order_acquire)
          : base->paletteSize;
  view.bits = base->bits;
  if (atomicHeader)
    view.storageOwner = data.storage;
  visitSectionStorage(base, [&](const auto *block) {
    view.palette =
        std::span<const BlockState::Id>(block->palette.data(), paletteSize);
    if constexpr (std::is_same_v<std::remove_cv_t<
                                     std::remove_pointer_t<decltype(block)>>,
                                 SectionStorage8>) {
      view.cells8 = std::span<const std::uint8_t>(block->indices.data(),
                                                  block->indices.size());
    } else if constexpr (std::is_same_v<
                             std::remove_cv_t<
                                 std::remove_pointer_t<decltype(block)>>,
                             SectionStorage16>) {
      view.cells16 = std::span<const std::uint16_t>(block->indices.data(),
                                                    block->indices.size());
    } else {
      view.packedWords = std::span<const std::uint64_t>(block->words.data(),
                                                        block->words.size());
    }
  });
  return view;
}

LightSectionView World::directReadSection(const void *context, int chunkX,
                                          int chunkZ, int sectionY) noexcept {
  const auto *self = static_cast<const World *>(context);
  const int idx = sectionY - (self->minY_ >> 4);
  if (idx < 0 || idx >= self->sectionCount())
    return LightSectionView{};
  std::shared_lock<std::shared_mutex> lock(*self->lightDirectMutex_);
  const auto it = self->chunks_.find(chunkKey(chunkX, chunkZ));
  if (it == self->chunks_.end())
    return LightSectionView{};
  return sectionToLightView(
      it->second.sections[static_cast<std::size_t>(idx)], true);
}

std::unique_ptr<LightChunkInput> World::makeLightChunkInput(int x, int z) const {
  const Chunk *chunk = chunkAt(x, z);
  if (!chunk || !lightService_)
    return nullptr;

  auto input = std::make_unique<LightChunkInput>();
  input->chunkX = x;
  input->chunkZ = z;
  input->lifecycleEpoch = chunk->lightLifecycleEpoch;
  input->minBlockSectionY = minY_ >> 4;
  input->blockSectionCount = sectionCount();
  input->persistedLight = chunk->persistedLight;
  if (lowMemoryMode_) {
    input->directGeometry = true;
    return input;
  }
  const auto optical = lightService_->opticalTable();
  if (!optical)
    throw std::logic_error("light service has no optical table");
  input->geometrySections.reserve(static_cast<std::size_t>(sectionCount()));
  for (int section = 0; section < sectionCount(); ++section) {
    const Section &data = chunk->sections[section];
    input->geometrySections.push_back(
        makeLightGeometrySection(*optical, sectionToLightView(data, false)));
  }
  return input;
}

void World::drainLightPublications() {
  if (lightService_)
    lightService_->drainPublished();
}

int World::getBrightness(Pos pos, int skyDarken) const {
  if (!lightService_ || !isLoaded(pos))
    return 0;
  int block = 0;
  int sky = 0;
  if (const auto view = lightService_->latestLayerChunk(
          LightLayer::Block, chunkIndex(pos.x), chunkIndex(pos.z)))
    block = view->lightValue(LightLayer::Block, pos.x, pos.y, pos.z);
  if (const auto view = lightService_->latestLayerChunk(
          LightLayer::Sky, chunkIndex(pos.x), chunkIndex(pos.z)))
    sky = view->lightValue(LightLayer::Sky, pos.x, pos.y, pos.z);
  sky = std::max(0, sky - std::max(0, skyDarken));
  return std::max(block, sky);
}

void World::setLowMemoryMode(bool enabled) {
  if (lowMemoryMode_ == enabled)
    return;
  if (lightService_)
    throw std::logic_error(
        "cannot change low-memory mode while lighting is active");
  lowMemoryMode_ = enabled;
}

bool World::adoptChunkLightDirectGeometry(int x, int z) {
  if (!lightService_)
    return false;
  // 非低内存模式没有 World 直读源，交给调用方重新提交值快照。
  if (!lowMemoryMode_)
    return false;
  if (!lightService_->adoptDirectGeometry(x, z))
    return false;
  alignChunkLightLifecycle(x, z);
  return true;
}

void World::enableLighting(LightWorkerOptions options) {
  if (lightService_)
    return;
  // Heightmaps and the existing simulation permit an unaligned minY. The light
  // handoff alone requires section alignment because its keys use y >> 4.
  if ((minY_ & 15) != 0)
    throw std::invalid_argument(
        "lighting requires minY aligned to a 16-block section");
  options.hasSkyLight = options.hasSkyLight && dimension_.hasSkyLight;
  if (lowMemoryMode_ && !options.directWorld.context) {
    options.directWorld.context = this;
    options.directWorld.resizeMutex = lightDirectMutex_.get();
    options.directWorld.readSection = &World::directReadSection;
  }
  lightService_ = std::make_unique<LightService>(
      minY_ >> 4, sectionCount(), options);
  lightOptical_ = lightService_->opticalTable().get();
  registerLightCleanupTask();
  lightService_->setPublishCallback(
      [this](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        if (managedLightPublishedObserver && snapshot)
          managedLightPublishedObserver(snapshot->chunkX, snapshot->chunkZ);
        if (lightPublicationObserver)
          lightPublicationObserver(std::move(snapshot));
      });
  refreshLightSnapshots();
}

void World::flushPendingLightCells() {
  if (!lightService_ || pendingLightCells_.empty())
    return;
  const std::uint64_t seq =
      lightService_->reportBlockCells(pendingLightCells_);
  for (std::uint64_t key : pendingLightChunkKeys_) {
    if (auto it = chunks_.find(key); it != chunks_.end()) {
      it->second.hasPendingLightCell = false;
      if (seq > 0)
        it->second.requiredLightGeneration =
            std::max(it->second.requiredLightGeneration, seq);
    }
  }
  pendingLightChunkKeys_.clear();
  pendingLightCells_.clear();
}

void World::setChunkPersistedLight(int x, int z, PersistedChunkLight persisted) {
  if (Chunk *c = chunkAt(x, z)) {
    c->persistedLight = std::move(persisted);
    c->refreshPersistedLightIndex(minY_ >> 4, sectionCount());
  }
}

void World::reloadChunkLight(int x, int z) {
  if (!lightService_)
    return;
  flushPendingLightCells();
  Chunk *chunk = chunkAt(x, z);
  if (!chunk)
    return;
  if (auto input = makeLightChunkInput(x, z)) {
    const std::uint64_t seq = lightService_->submitChunk(std::move(*input));
    if (seq > 0)
      chunk->requiredLightGeneration =
          std::max(chunk->requiredLightGeneration, seq);
    chunk->persistedLight.reset();
  }
}

std::uint64_t World::submitGeneratedChunkLight(LightChunkInput input) {
  if (!lightService_)
    return 0;
  return lightService_->submitChunk(std::move(input));
}

std::uint64_t World::requestGeneratedChunkLightCompute(int x, int z) {
  if (!lightService_)
    return 0;
  return lightService_->requestChunkCompute(x, z);
}

bool World::adoptSectionStorage(int x, int z, int sectionY,
                                 BlockSection &&section) {
  Chunk *chunk = chunkAt(x, z);
  if (!chunk)
    return false;
  const int targetSection = World::sectionIndex(sectionY * 16);
  if (targetSection < 0 || targetSection >= sectionCount())
    return false;
  const std::size_t index = static_cast<std::size_t>(targetSection);
  blockReadCache_.sectionIndex = -1;
  Section &destination = chunk->sections[index];
  {
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    destination = std::move(section);
    destination.setSectionY(static_cast<std::int8_t>(sectionY));
    // 光照的权威数据在 LightService 里；World 区段不保留原型携带的层。
    destination.blockLight.reset();
    destination.skyLight.reset();
  }
  const std::uint64_t key = chunkKey(x, z);
  if (poiStorage_) {
    std::vector<std::pair<std::uint16_t, PoiTypeId>> found;
    bool hasPoi = false;
    for (int cell = 0; cell < kLightVolume; ++cell) {
      const PoiTypeId poi = fastPoiTypeForState(destination.get(cell));
      if (poi == PoiTypeId::None)
        continue;
      hasPoi = true;
      found.emplace_back(static_cast<std::uint16_t>(cell), poi);
    }
    if (hasPoi)
      poiStorage_->reconcileSectionFromScan(x, z, targetSection, found, false);
    else
      poiStorage_->markSectionValidEmpty(x, z, targetSection);
  }
  updateChunkRandomTickMask(*chunk, key, targetSection,
                            destination.isRandomlyTicking());
  chunk->persistedLight.reset();
  return true;
}

void World::refreshLightSnapshots() {
  if (!lightService_)
    return;
  flushPendingLightCells();
  std::vector<std::uint64_t> keys;
  keys.reserve(chunks_.size());
  for (const auto &[key, chunk] : chunks_)
    if (chunk.loaded)
      keys.push_back(key);
  std::sort(keys.begin(), keys.end());
  for (std::uint64_t key : keys) {
    const int chunkX = static_cast<int>(key >> 32);
    const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
    if (auto input = makeLightChunkInput(chunkX, chunkZ)) {
      const std::uint64_t seq = lightService_->submitChunk(std::move(*input));
      if (Chunk *c = chunkAt(chunkX, chunkZ)) {
        if (seq > 0)
          c->requiredLightGeneration =
              std::max(c->requiredLightGeneration, seq);
        c->persistedLight.reset();
      }
    }
  }
}

void World::disableLighting() {
  unregisterLightCleanupTask();
  for (std::uint64_t key : pendingLightChunkKeys_) {
    if (auto it = chunks_.find(key); it != chunks_.end())
      it->second.hasPendingLightCell = false;
  }
  pendingLightChunkKeys_.clear();
  pendingLightCells_.clear();
  lightOptical_ = nullptr;
  lightService_.reset();
}
bool World::isChunkLoaded(int x, int z) const { return chunkAt(x, z) != nullptr; }
bool World::isLoaded(Pos p) const {
  // Level.getBlockState: bounds check, then the chunk cache (hash) lookup.
  return p.y >= minY_ && p.y < maxY_ &&
         chunkAt(chunkIndex(p.x), chunkIndex(p.z)) != nullptr;
}
const Heightmap *World::heightmap(int chunkX, int chunkZ,
                                  HeightmapType type) const {
  const Chunk *chunk = chunkAt(chunkX, chunkZ);
  return chunk && chunk->heightmaps ? chunk->heightmaps->find(type) : nullptr;
}
int World::height(HeightmapType type, int blockX, int blockZ) const {
  const Heightmap *map = heightmap(chunkIndex(blockX), chunkIndex(blockZ), type);
  return map ? map->height(blockX & 15, blockZ & 15) : minY_ - 1;
}
void World::primeHeightmaps() {
  for (auto &[key, chunk] : chunks_) {
    if (!chunk.loaded)
      continue;
    primeHeightmaps(static_cast<int>(key >> 32),
                    static_cast<int>(static_cast<std::uint32_t>(key)));
  }
}

void World::primeHeightmaps(int chunkX, int chunkZ) {
  auto found = chunks_.find(chunkKey(chunkX, chunkZ));
  if (found == chunks_.end() || !found->second.loaded)
    return;
  Chunk &chunk = found->second;
  if (!chunk.heightmaps)
    chunk.heightmaps =
        std::make_unique<ChunkHeightmaps>(minY_, sectionCount() * 16);
  int highestSectionPosition = minY_;
  for (int section = sectionCount(); section-- > 0;) {
    if (!chunk.sections[section].hasOnlyAir()) {
      highestSectionPosition = (minY_ >> 4) * 16 + section * 16 + 16;
      break;
    }
  }
  chunk.heightmaps->prime(
      highestSectionPosition, [this, chunkX, chunkZ](int x, int y, int z) {
        return getBlock({chunkX * 16 + x, y, chunkZ * 16 + z});
      });
}
const World::Section *World::loadedSectionAt(Pos p) const {
  if (p.y < minY_ || p.y >= maxY_)
    return nullptr;
  const int x = chunkIndex(p.x), z = chunkIndex(p.z);
  const auto key = chunkKey(x, z);
  const int index = sectionIndex(p.y);
  auto &cache = blockReadCache_;
  if (cache.sectionIndex == index && cache.sectionKey == key)
    return cache.section;
  const Chunk *c = chunkAt(x, z);
  if (!c || index >= sectionCount())
    return nullptr;
  const Section *s = &c->sections[index];
  cache.sectionKey = key;
  cache.sectionIndex = index;
  cache.section = s;
  return s;
}
std::optional<BlockState> World::getBlockIfLoaded(Pos p) const {
  if (p.y < minY_ || p.y >= maxY_)
    return isChunkLoaded(chunkIndex(p.x), chunkIndex(p.z))
               ? std::optional<BlockState>(airState())
               : std::nullopt;
  const Section *s = loadedSectionAt(p);
  if (!s)
    return std::nullopt;
  if (s->hasOnlyAir())
    return airState();
  const int cell = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  return BlockState::fromId(s->get(cell));
}
BlockState World::getBlock(Pos p) const {
  const Section *s = loadedSectionAt(p);
  if (!s || s->hasOnlyAir())
    return airState();
  const int cell = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  return BlockState::fromId(s->get(cell));
}
ChunkSnapshot World::snapshotChunk(int chunkX, int chunkZ) const {
  ChunkSnapshot snapshot;
  snapshot.minY = minY_;
  snapshot.maxY = maxY_;
  snapshot.sections.resize(static_cast<std::size_t>(sectionCount()));
  if (const Chunk *chunk = chunkAt(chunkX, chunkZ)) {
    for (std::size_t i = 0;
         i < snapshot.sections.size() && i < chunk->sections.size(); ++i) {
      // An untouched section is a default Section (single air, no storage), so
      // it copies over the snapshot's default entry without any branch.
      const Section &section = chunk->sections[i];
      SectionSnapshot &out = snapshot.sections[i];
      out.nonEmpty = section.nonEmpty;
      out.bits = section.bits();
      out.single = section.single;
      out.biomes = chunk->biomeSections[i];
      // The snapshot stays an owning copy: the network encoder runs off the
      // tick thread, so it must not read the live container.
      if (const SectionStorage *base = section.storage.get()) {
        visitSectionStorage(base, [&](const auto *block) {
          out.palette.assign(block->palette.begin(),
                             block->palette.begin() + block->paletteSize);
          if (block->bits <= 8) {
            out.bits = 8;
            out.indices8.resize(static_cast<std::size_t>(kLightVolume));
            for (std::size_t k = 0; k < kLightVolume; ++k)
              out.indices8[k] = static_cast<std::uint8_t>(block->indexAt(k));
          } else {
            out.bits = 16;
            out.indices16.resize(static_cast<std::size_t>(kLightVolume));
            for (std::size_t k = 0; k < kLightVolume; ++k)
              out.indices16[k] = block->indexAt(k);
          }
        });
      }
    }
  }
  snapshot.pistons = snapshotChunkPistons(chunkX, chunkZ);
  return snapshot;
}

std::string_view World::getNoiseBiome(int quartX, int quartY, int quartZ) const {
  const int chunkX = quartX >= 0 ? (quartX / 4) : ((quartX - 3) / 4);
  const int chunkZ = quartZ >= 0 ? (quartZ / 4) : ((quartZ - 3) / 4);
  const Chunk *chunk = chunkAt(chunkX, chunkZ);
  if (!chunk)
    return "minecraft:plains";
  const int minQuartY = minY_ >= 0 ? (minY_ / 4) : ((minY_ - 3) / 4);
  const int maxQuartY = (maxY_ >= 0 ? (maxY_ / 4) : ((maxY_ - 3) / 4)) - 1;
  const int clampedY = std::clamp(quartY, minQuartY, maxQuartY);
  const int secIdx = sectionIndex(clampedY * 4);
  if (secIdx < 0 || secIdx >= static_cast<int>(chunk->sections.size()))
    return "minecraft:plains";
  const auto &biomes = chunk->biomeSections[static_cast<std::size_t>(secIdx)];
  if (!biomes || biomes->palette.empty())
    return "minecraft:plains";
  const int lqx = quartX & 3;
  const int lqy = clampedY & 3;
  const int lqz = quartZ & 3;
  const std::uint8_t idx =
      biomes->indices[static_cast<std::size_t>((lqy * 4 + lqz) * 4 + lqx)];
  if (idx >= biomes->palette.size())
    return "minecraft:plains";
  return biomes->palette[idx];
}

void World::setSectionBiomes(int chunkX, int sectionY, int chunkZ,
                             std::shared_ptr<SectionBiomes> biomes) {
  // 只保证节点存在：设置群系不是光照输入变更，不应该顺手向光照引擎登记一次
  // （生成晋升路径正处在“方块尚未导入完”的中间态，那会触发一次错误的计算）。
  loadChunk(chunkX, chunkZ, /*registerLight=*/false);
  Chunk *chunk = chunkAt(chunkX, chunkZ);
  if (!chunk)
    return;
  const int secIdx = sectionIndex(sectionY * 16);
  if (secIdx < 0 || secIdx >= static_cast<int>(chunk->sections.size()))
    return;
  chunk->biomeSections[static_cast<std::size_t>(secIdx)] = std::move(biomes);
}
std::vector<PistonSnapshot>
World::snapshotChunkPistons(int chunkX, int chunkZ) const {
  std::vector<PistonSnapshot> result;
  const auto indexed = pistonPositionsByChunk_.find(chunkKey(chunkX, chunkZ));
  if (indexed == pistonPositionsByChunk_.end())
    return result;
  result.reserve(indexed->second.size());
  for (const Pos &pos : indexed->second) {
    const auto found = pistonEntities.find(pos);
    if (found == pistonEntities.end() || !found->second)
      continue;
    const auto &entity = *found->second;
    result.push_back(PistonSnapshot{pos, entity.movedState, entity.direction,
                                    entity.progressO, entity.extending,
                                    entity.source});
  }
  std::sort(result.begin(), result.end(),
            [](const PistonSnapshot &a, const PistonSnapshot &b) {
              return a.pos < b.pos;
            });
  return result;
}
void World::reconcilePoiFromChunk(int cx, int cz, const Chunk &chunk) {
  if (!poiStorage_)
    return;
  const int count = sectionCount();
  std::vector<std::pair<std::uint16_t, PoiTypeId>> found;
  for (int si = 0; si < count; ++si) {
    const Section &sec = chunk.sections[static_cast<std::size_t>(si)];
    if (!sec.storage) {
      const PoiTypeId singlePoi = fastPoiTypeForState(sec.single);
      if (singlePoi == PoiTypeId::None) {
        poiStorage_->markSectionValidEmpty(cx, cz, si);
        continue;
      }
      found.resize(static_cast<std::size_t>(kLightVolume));
      for (int i = 0; i < kLightVolume; ++i)
        found[static_cast<std::size_t>(i)] = {
            static_cast<std::uint16_t>(i), singlePoi};
      poiStorage_->reconcileSectionFromScan(cx, cz, si, found, false);
      continue;
    }

    found.clear();
    bool hasAnyPoi = false;
    visitSectionStorage(sec.storage.get(), [&](const auto *typed) {
      using BlockT = std::remove_pointer_t<decltype(typed)>;
      std::array<PoiTypeId, BlockT::kPaletteCapacity> palettePoi{};
      const std::size_t paletteCount =
          std::min<std::size_t>(typed->paletteSize, BlockT::kPaletteCapacity);
      for (std::size_t p = 0; p < paletteCount; ++p) {
        const PoiTypeId poi = fastPoiTypeForState(typed->palette[p]);
        palettePoi[p] = poi;
        if (poi != PoiTypeId::None)
          hasAnyPoi = true;
      }
      if (!hasAnyPoi)
        return;
      for (int i = 0; i < kLightVolume; ++i) {
        const PoiTypeId poi =
            palettePoi[typed->indexAt(static_cast<std::size_t>(i))];
        if (poi != PoiTypeId::None)
          found.emplace_back(static_cast<std::uint16_t>(i), poi);
      }
    });
    if (!hasAnyPoi) {
      poiStorage_->markSectionValidEmpty(cx, cz, si);
    } else {
      poiStorage_->reconcileSectionFromScan(cx, cz, si, found, false);
    }
  }
}

void World::rawSetBlock(Pos p, BlockState state) {
  if (p.y < minY_ || p.y >= maxY_)
    return;
  const int cx = chunkIndex(p.x);
  const int cz = chunkIndex(p.z);
  Chunk *c = chunkAt(cx, cz);
  if (!c)
    return;
  // Writing the value a fresh section already holds (air, state id 0) is a
  // no-op in Section::set, so an untouched section never materialises storage.
  const int si = sectionIndex(p.y);
  Section &section = c->sections[si];
  const int index = ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  const BlockState::Id previous =
      section.set(index, state.id(), lowMemoryMode_, directResizeMutex());
  if (previous != state.id()) {
    updateChunkRandomTickMask(*c, chunkKey(cx, cz), si,
                              section.isRandomlyTicking());
    c->persistedLight.reset();
    if (poiStorage_ &&
        (fastPoiTypeForState(previous) != PoiTypeId::None ||
         fastPoiTypeForState(state.id()) != PoiTypeId::None)) {
      poiStorage_->onBlockStateChange(PoiPos{p.x, p.y, p.z}, previous,
                                      state.id());
    }
    if (managedBlockMutationObserver) {
      const BlockState old = BlockState::fromId(previous);
      managedBlockMutationObserver(p, old, state);
    }
    if (lightService_ &&
        lightOptical_->classForState(previous) !=
            lightOptical_->classForState(state.id())) {
      const LightCellUpdate update{p.x, p.y, p.z, state.id()};
      const std::uint64_t seq = lightService_->reportBlockCell(update);
      if (seq > 0)
        c->requiredLightGeneration = std::max(c->requiredLightGeneration, seq);
    }
  }
}
void World::importSectionBlocks(
    int cx, int cz, int sectionY, std::span<const BlockState::Id> palette,
    std::span<const std::uint16_t, kLightVolume> indices) {
  if (palette.empty() || palette.size() > static_cast<std::size_t>(kLightVolume))
    return;
  Chunk *c = chunkAt(cx, cz);
  if (!c)
    return;
  const int secBaseY = sectionY * 16;
  if ((minY_ & 15) != 0 || secBaseY < minY_ || secBaseY + 15 >= maxY_) {
    for (int i = 0; i < kLightVolume; ++i) {
      const BlockState::Id sid =
          palette.size() == 1 ? palette[0] : palette[indices[i]];
      const BlockState s = BlockState::fromId(sid);
      if (s.isAir())
        continue;
      const int y = secBaseY + (i >> 8);
      if (y >= minY_ && y < maxY_)
        rawSetBlock({cx * 16 + (i & 15), y, cz * 16 + ((i >> 4) & 15)}, s);
    }
    return;
  }
  const int si = sectionIndex(secBaseY);
  Section &sec = c->sections[si];
  blockReadCache_.sectionIndex = -1;
  if (palette.size() == 1) {
    const BlockState::Id sid = palette[0];
    if (BlockState::fromId(sid).isAir()) {
      if (poiStorage_)
        poiStorage_->markSectionValidEmpty(cx, cz, si);
      return;
    }
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    sec.storage.reset();
    sec.single = sid;
    sec.nonEmpty = static_cast<std::uint16_t>(kLightVolume);
    sec.initSingleRandomTick(sid);
    if (poiStorage_) {
      const PoiTypeId singlePoi = fastPoiTypeForState(sid);
      if (singlePoi == PoiTypeId::None) {
        poiStorage_->markSectionValidEmpty(cx, cz, si);
      } else {
        std::vector<std::pair<std::uint16_t, PoiTypeId>> found(kLightVolume);
        for (int i = 0; i < kLightVolume; ++i)
          found[static_cast<std::size_t>(i)] = {
              static_cast<std::uint16_t>(i), singlePoi};
        poiStorage_->reconcileSectionFromScan(cx, cz, si, found, false);
      }
    }
  } else {
    const std::uint8_t targetBits =
        palette.size() > 256
            ? 16
            : (lowMemoryMode_ ? minBitsForPaletteSize(palette.size()) : 8);
    auto block = makeSectionStorageForBits(targetBits);
    std::uint16_t nonEmpty = 0;
    std::uint16_t tickingCount = 0;
    sec.randomTickByMinisection.fill(0);
    std::vector<std::pair<std::uint16_t, PoiTypeId>> poiFound;
    bool hasAnyPoi = false;
    visitSectionStorage(block.get(), [&](auto *typed) {
      using BlockT = std::remove_pointer_t<decltype(typed)>;
      const std::size_t paletteCount =
          std::min(palette.size(), BlockT::kPaletteCapacity);
      typed->paletteSize = static_cast<std::uint16_t>(paletteCount);
      std::array<bool, kLightVolume> isNonAir{};
      std::array<PoiTypeId, kLightVolume> palettePoi{};
      for (std::size_t p = 0; p < paletteCount; ++p) {
        const BlockState::Id sid = palette[p];
        typed->palette[p] = sid;
        typed->paletteTicking[p] = stateIsRandomlyTickingId(sid) ? 1 : 0;
        isNonAir[p] = !BlockState::fromId(sid).isAir();
        const PoiTypeId poi = fastPoiTypeForState(sid);
        palettePoi[p] = poi;
        if (poi != PoiTypeId::None)
          hasAnyPoi = true;
      }
      for (int i = 0; i < kLightVolume; ++i) {
        const std::uint16_t pi = indices[static_cast<std::size_t>(i)];
        typed->setIndexAt(static_cast<std::size_t>(i), pi);
        if (isNonAir[pi])
          ++nonEmpty;
        if (typed->paletteTicking[pi]) {
          ++tickingCount;
          ++sec.randomTickByMinisection[static_cast<std::size_t>(i) /
                                        kRandomTickMinisectionSize];
        }
        if (hasAnyPoi && palettePoi[pi] != PoiTypeId::None)
          poiFound.emplace_back(static_cast<std::uint16_t>(i), palettePoi[pi]);
      }
    });
    std::unique_lock<std::shared_mutex> resizeLock;
    if (std::shared_mutex *mu = directResizeMutex())
      resizeLock = std::unique_lock<std::shared_mutex>(*mu);
    sec.nonEmpty = nonEmpty;
    sec.tickingBlockCount = tickingCount;
    if (nonEmpty == 0) {
      sec.storage.reset();
      sec.single = 0;
    } else {
      sec.storage = std::move(block);
    }
    if (poiStorage_) {
      if (!hasAnyPoi) {
        poiStorage_->markSectionValidEmpty(cx, cz, si);
      } else {
        poiStorage_->reconcileSectionFromScan(cx, cz, si, poiFound, false);
      }
    }
  }
  updateChunkRandomTickMask(*c, chunkKey(cx, cz), si, sec.isRandomlyTicking());
  c->persistedLight.reset();
}
std::map<Pos, BlockState> World::allBlocks() const {
  std::map<Pos, BlockState> result;
  for (const auto &[key, c] : chunks_) {
    if (!c.loaded)
      continue;
    const int chunkX = static_cast<int>(key >> 32);
    const int chunkZ = static_cast<int>(key & 0xffffffffu);
    for (int si = 0; si < sectionCount(); ++si) {
      const Section &s = c.sections[si];
      if (s.hasOnlyAir())
        continue;
      for (int i = 0; i < 4096; ++i) {
        const auto id = s.get(i);
        BlockState state = BlockState::fromId(id);
        if (state.isAir())
          continue;
        result.emplace(Pos{chunkX * 16 + (i & 15),
                           minY_ + static_cast<int>(si) * 16 +
                               ((i >> 8) & 15),
                           chunkZ * 16 + ((i >> 4) & 15)},
                       state);
      }
    }
  }
  return result;
}
} // namespace redstone
