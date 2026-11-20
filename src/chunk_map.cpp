#include "redstone/chunk_map.hpp"

#include "redstone/engine.hpp"
#include "redstone/worldgen/noise_chunk_generator.hpp"
#include "redstone/worldgen/worldgen_scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <type_traits>

namespace redstone {
namespace {

std::uint8_t computeNineColor(ChunkTicketPos pos) noexcept {
  const int mx = ((pos.x % 3) + 3) % 3;
  const int mz = ((pos.z % 3) + 3) % 3;
  return static_cast<std::uint8_t>(mx * 3 + mz);
}

LightSectionView sectionToLightViewAtomic(const BlockSection &data) noexcept {
  LightSectionView view;
  const std::uint16_t nonEmpty =
      std::atomic_ref<std::uint16_t>(
          const_cast<std::uint16_t &>(data.nonEmpty))
          .load(std::memory_order_relaxed);
  view.empty = nonEmpty == 0;
  const SectionStorage *base = data.storage.get();
  if (!base) {
    view.bits = 0;
    view.single = data.single;
    return view;
  }
  const std::uint16_t paletteSize =
      std::atomic_ref<std::uint16_t>(
          const_cast<std::uint16_t &>(base->paletteSize))
          .load(std::memory_order_acquire);
  view.bits = base->bits;
  view.storageOwner = data.storage;
  visitSectionStorage(base, [&](const auto *block) {
    view.palette =
        std::span<const BlockState::Id>(block->palette.data(), paletteSize);
    using BlockT = std::remove_cv_t<std::remove_pointer_t<decltype(block)>>;
    if constexpr (std::is_same_v<BlockT, SectionStorage8>) {
      view.cells8 = std::span<const std::uint8_t>(block->indices.data(),
                                                  block->indices.size());
    } else if constexpr (std::is_same_v<BlockT, SectionStorage16>) {
      view.cells16 = std::span<const std::uint16_t>(block->indices.data(),
                                                    block->indices.size());
    } else {
      view.packedWords = std::span<const std::uint64_t>(block->words.data(),
                                                        block->words.size());
    }
  });
  return view;
}

} // namespace

// ---------------------------------------------------------------------------
// WorldChunk
// ---------------------------------------------------------------------------

void WorldChunk::refreshRandomTickMask(int count) {
  std::uint32_t mask = 0;
  for (int i = 0; i < count; ++i) {
    if (sections[static_cast<std::size_t>(i)].isRandomlyTicking())
      mask |= (std::uint32_t{1} << i);
  }
  randomTickingSectionMask = mask;
}

void WorldChunk::refreshPersistedLightIndex(int minSectionY,
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

int WorldChunk::getHeight(HeightmapType type, int x, int z) const {
  if (!heightmaps)
    return minY_ - 1;
  if (type == HeightmapType::WorldSurfaceWG)
    type = HeightmapType::WorldSurface;
  else if (type == HeightmapType::OceanFloorWG)
    type = HeightmapType::OceanFloor;
  const Heightmap *hm = heightmaps->find(type);
  return hm ? (hm->firstAvailable(x & 15, z & 15) - 1) : (minY_ - 1);
}

// ---------------------------------------------------------------------------
// ChunkHolder
// ---------------------------------------------------------------------------

ChunkHolder::ChunkHolder(ChunkTicketPos pos, std::uint64_t key,
                         std::uint8_t color, ChunkMap *owner) noexcept
    : pos_(pos), key_(key), color_(color), owner_(owner) {}

ChunkHolder::~ChunkHolder() {
  std::unique_lock lock(chunkMtx_);
  releaseOutgoingLocked();
}

void ChunkHolder::releaseOutgoingLocked() noexcept {
  if (kind_ == HeldChunkKind::Proto) {
    if (proto_) {
      proto_->markInvalidated();
      proto_->resizeMutex = nullptr;
    }
    proto_.~shared_ptr<worldgen::ProtoChunk>();
    kind_ = HeldChunkKind::Empty;
  } else if (kind_ == HeldChunkKind::World) {
    WorldChunk *raw = world_.get();
    if (raw) {
      raw->markInvalidated();
      if (owner_ != nullptr)
        owner_->invalidateWorldCaches(key_, raw);
      raw->resizeMutex = nullptr;
    }
    world_.~shared_ptr<WorldChunk>();
    kind_ = HeldChunkKind::Empty;
  }
}

std::shared_ptr<BaseChunk> ChunkHolder::chunkShared() const {
  std::shared_lock lock(chunkMtx_);
  if (kind_ == HeldChunkKind::Proto)
    return proto_;
  if (kind_ == HeldChunkKind::World)
    return world_;
  return nullptr;
}

std::shared_ptr<worldgen::ProtoChunk> ChunkHolder::protoShared() const {
  std::shared_lock lock(chunkMtx_);
  return kind_ == HeldChunkKind::Proto ? proto_ : nullptr;
}

std::shared_ptr<WorldChunk> ChunkHolder::worldShared() const {
  std::shared_lock lock(chunkMtx_);
  return kind_ == HeldChunkKind::World ? world_ : nullptr;
}

void ChunkHolder::setProto(std::shared_ptr<worldgen::ProtoChunk> proto) {
  std::unique_lock lock(chunkMtx_);
  releaseOutgoingLocked();
  if (proto) {
    proto->clearInvalidated();
    proto->resizeMutex = &chunkMtx_;
    new (&proto_) std::shared_ptr<worldgen::ProtoChunk>(std::move(proto));
    kind_ = HeldChunkKind::Proto;
  }
}

void ChunkHolder::setWorld(std::shared_ptr<WorldChunk> world) {
  std::unique_lock lock(chunkMtx_);
  releaseOutgoingLocked();
  if (world) {
    world->clearInvalidated();
    world->resizeMutex = &chunkMtx_;
    WorldChunk *raw = world.get();
    new (&world_) std::shared_ptr<WorldChunk>(std::move(world));
    kind_ = HeldChunkKind::World;
    if (owner_ != nullptr && raw->loaded)
      owner_->registerWorldIndex(key_, raw);
  }
}

void ChunkHolder::resetChunk() {
  std::unique_lock lock(chunkMtx_);
  releaseOutgoingLocked();
}

bool ChunkHolder::tryEvictProto() {
  std::unique_lock lock(chunkMtx_);
  if (evicted_.load(std::memory_order_acquire) ||
      kind_ != HeldChunkKind::Proto) {
    return false;
  }
  evicted_.store(true, std::memory_order_release);
  releaseOutgoingLocked();
  return true;
}

std::shared_ptr<WorldChunk> ChunkHolder::promoteProtoToWorld(
    int minY, int maxY,
    std::vector<Pos> *outPostProcessing,
    std::vector<terrain::SavedTick> *outBlockTicks,
    std::vector<terrain::SavedTick> *outFluidTicks,
    std::vector<terrain::MovingPistonEntity> *outMovingPistons,
    std::vector<nbt::Tag> *outEntities) {
  std::unique_lock wlock(chunkMtx_);
  if (kind_ != HeldChunkKind::Proto || !proto_)
    return kind_ == HeldChunkKind::World ? world_ : nullptr;
  std::shared_ptr<worldgen::ProtoChunk> proto = std::move(proto_);

  auto worldChunk = std::make_shared<WorldChunk>();
  worldChunk->initialize(pos_, minY, maxY - minY);
  worldChunk->setDefaultBiome(std::string(proto->defaultBiome()));
  worldChunk->loaded = true;
  worldChunk->loadEpoch = proto->loadEpoch;
  worldChunk->lightEpoch = proto->lightEpoch;
  worldChunk->lightLifecycleEpoch =
      proto->lightEpoch != 0 ? proto->lightEpoch : 1;
  worldChunk->requiredLightGeneration = proto->lightSubmissionGeneration();
  worldChunk->setLightCorrect(proto->isLightCorrect());

  const int count = worldChunk->sectionCount();
  const int protoSections = proto->sectionCount();
  const int minSecY = proto->minSectionY();

  if (outPostProcessing != nullptr) {
    outPostProcessing->clear();
    const auto &pp = proto->postProcessing();
    for (std::size_t s = 0; s < pp.size(); ++s) {
      const int secY = minSecY + static_cast<int>(s);
      for (std::int16_t packed : pp[s]) {
        outPostProcessing->push_back(
            worldgen::ProtoChunk::unpackOffsetCoordinates(packed, pos_, secY));
      }
    }
  }
  if (outBlockTicks != nullptr)
    *outBlockTicks = proto->blockTicks();
  if (outFluidTicks != nullptr)
    *outFluidTicks = proto->fluidTicks();
  if (outMovingPistons != nullptr)
    *outMovingPistons = proto->movingPistons();
  if (outEntities != nullptr)
    *outEntities = proto->entities();

  auto &protoBase = static_cast<BaseChunk &>(*proto);
  const bool exclusive =
      proto.use_count() == 1 &&
      generationRefCount.load(std::memory_order_acquire) == 0;

  for (int i = 0; i < count && i < protoSections; ++i) {
    const std::size_t idx = static_cast<std::size_t>(i);
    if (exclusive) {
      worldChunk->sections[idx] = std::move(protoBase.sections[idx]);
      worldChunk->biomeSections[idx] = std::move(protoBase.biomeSections[idx]);
    } else {
      worldChunk->sections[idx] = protoBase.sections[idx].share();
      worldChunk->biomeSections[idx] = protoBase.biomeSections[idx];
    }
    worldChunk->sections[idx].setSectionY(
        static_cast<std::int8_t>((minY >> 4) + i));
    // 光照权威数据在 LightService 中，运行态 WorldChunk 区段不保留生成期光照层。
    worldChunk->sections[idx].blockLight.reset();
    worldChunk->sections[idx].skyLight.reset();
  }
  worldChunk->refreshRandomTickMask(count);

  if (auto finalMaps = proto->takeFinalHeightmaps()) {
    worldChunk->heightmaps =
        std::make_unique<ChunkHeightmaps>(std::move(*finalMaps));
  } else {
    worldChunk->heightmaps =
        std::make_unique<ChunkHeightmaps>(minY, count * 16);
  }

  if (exclusive) {
    worldChunk->setStructureStartsRaw(proto->takeStructureStartsRaw());
  } else {
    worldChunk->setStructureStartsRaw(
        worldgen::cloneChunkStructureDataRaw(proto->structureStartsRaw()));
  }

  if (proto->isLightCorrect()) {
    PersistedChunkLight persisted;
    persisted.isLightOn = true;
    for (int i = 0; i < protoSections; ++i) {
      const BlockSection &sec = proto->section(i);
      if (!sec.blockLight.has_value() && !sec.skyLight.has_value())
        continue;
      PersistedLightSection ls;
      ls.sectionY = sec.sectionY();
      ls.blockLight = sec.blockLight.copy();
      ls.skyLight = sec.skyLight.copy();
      persisted.sections.push_back(std::move(ls));
    }
    worldChunk->persistedLight = std::move(persisted);
    worldChunk->refreshPersistedLightIndex(minY >> 4, count);
  }

  // 联合体从 ProtoChunk 原子切换为 WorldChunk，并置位旧 ProtoChunk 失效标志。
  proto->markInvalidated();
  proto->resizeMutex = nullptr;
  proto_.~shared_ptr<worldgen::ProtoChunk>();
  worldChunk->clearInvalidated();
  worldChunk->resizeMutex = &chunkMtx_;
  WorldChunk *raw = worldChunk.get();
  new (&world_) std::shared_ptr<WorldChunk>(worldChunk);
  kind_ = HeldChunkKind::World;
  if (owner_ != nullptr && raw->loaded)
    owner_->registerWorldIndex(key_, raw);
  return worldChunk;
}

std::optional<LightSectionView>
ChunkHolder::readLightSection(int sectionY, int minBlockSectionY,
                              int blockSectionCount) const {
  const int index = sectionY - minBlockSectionY;
  if (index < 0 || index >= blockSectionCount)
    return std::nullopt;
  std::shared_lock lock(chunkMtx_);
  const BaseChunk *chunk = nullptr;
  if (kind_ == HeldChunkKind::Proto)
    chunk = proto_.get();
  else if (kind_ == HeldChunkKind::World)
    chunk = world_.get();
  if (!chunk || chunk->isInvalidated() || index >= chunk->sectionCount())
    return std::nullopt;
  return sectionToLightViewAtomic(
      chunk->sections[static_cast<std::size_t>(index)]);
}

ChunkHolder::GenTaskState &ChunkHolder::ensureGenState() {
  std::lock_guard lock(genInitMtx_);
  if (!gen_)
    gen_ = std::make_unique<GenTaskState>();
  return *gen_;
}

void ChunkHolder::releaseGenStateIfIdle() {
  std::lock_guard lock(genInitMtx_);
  if (!gen_)
    return;
  bool canReset = false;
  {
    std::lock_guard glock(gen_->mtx);
    canReset = gen_->execState == ExecState::Idle && !gen_->isActive &&
               gen_->initState != GenTaskState::InitState::Initializing &&
               gen_->initWaiters == 0 &&
               gen_->pendingDeps.load(std::memory_order_acquire) == 0 &&
               gen_->stagePinnedDeps.empty();
  }
  if (canReset)
    gen_.reset();
}

// ---------------------------------------------------------------------------
// ChunkMap
// ---------------------------------------------------------------------------

ChunkMap::ChunkMap(World *world) : world_(world) {}

ChunkMap::~ChunkMap() {
  disableWorldGen();
  // 先清主线程索引，再清理所有 holder 持有的区块。
  worldIndex_.clear();
  holders_.for_each_m([](HolderMap::value_type &entry) {
    if (entry.second) {
      entry.second->markEvicted();
      entry.second->resetChunk();
    }
  });
  holders_.clear();
}

std::shared_ptr<ChunkHolder> ChunkMap::findHolder(ChunkTicketPos pos) const {
  return findHolder(packChunkKey(pos.x, pos.z));
}

std::shared_ptr<ChunkHolder> ChunkMap::findHolder(std::uint64_t key) const {
  std::shared_ptr<ChunkHolder> out;
  holders_.if_contains(key, [&](const HolderMap::value_type &entry) {
    out = entry.second;
  });
  return out;
}

std::shared_ptr<ChunkHolder> ChunkMap::getOrCreateHolder(ChunkTicketPos pos) {
  return getOrCreateHolder(packChunkKey(pos.x, pos.z));
}

std::shared_ptr<ChunkHolder> ChunkMap::getOrCreateHolder(std::uint64_t key) {
  std::shared_ptr<ChunkHolder> out;
  const ChunkTicketPos pos = unpackChunkKey(key);
  holders_.lazy_emplace_l(
      key,
      [&](const HolderMap::value_type &entry) { out = entry.second; },
      [&](const HolderMap::constructor &ctor) {
        out = std::make_shared<ChunkHolder>(pos, key, computeNineColor(pos),
                                            this);
        ctor(key, out);
      });
  return out;
}

bool ChunkMap::eraseHolderIf(std::uint64_t key,
                             const std::function<bool(ChunkHolder &)> &pred) {
  return holders_.erase_if(key, [&](HolderMap::value_type &entry) {
    return entry.second && pred(*entry.second);
  });
}

void ChunkMap::registerWorldIndex(std::uint64_t key, WorldChunk *chunk) {
  if (chunk != nullptr)
    worldIndex_[key] = chunk;
}

void ChunkMap::invalidateWorldCaches(std::uint64_t key, WorldChunk *expected) {
  auto it = worldIndex_.find(key);
  if (it != worldIndex_.end() && (expected == nullptr || it->second == expected))
    worldIndex_.erase(it);
  if (world_ != nullptr)
    world_->invalidateChunkCaches(key, expected);
}

std::shared_ptr<ChunkHolder> ChunkMap::installWorldChunk(
    int x, int z, std::shared_ptr<WorldChunk> chunk) {
  const std::uint64_t key = packChunkKey(x, z);
  auto holder = getOrCreateHolder(key);
  holder->clearEvicted();
  holder->setWorld(std::move(chunk));
  return holder;
}

bool ChunkMap::unloadWorldChunk(int x, int z, bool &wasLoaded) {
  const std::uint64_t key = packChunkKey(x, z);
  wasLoaded = false;
  auto it = worldIndex_.find(key);
  if (it != worldIndex_.end()) {
    WorldChunk *chunk = it->second;
    wasLoaded = chunk != nullptr && chunk->loaded;
    if (chunk != nullptr)
      chunk->loaded = false;
    invalidateWorldCaches(key, chunk);
    return true;
  }
  if (auto holder = findHolder(key)) {
    if (auto world = holder->worldShared()) {
      wasLoaded = world->loaded;
      world->loaded = false;
      invalidateWorldCaches(key, world.get());
      return true;
    }
  }
  return false;
}

bool ChunkMap::evictChunk(int x, int z) {
  const std::uint64_t key = packChunkKey(x, z);
  auto holder = findHolder(key);
  if (!holder)
    return false;
  holder->markEvicted();
  holder->resetChunk();
  holders_.erase_if(key, [&](HolderMap::value_type &entry) {
    return entry.second == holder;
  });
  return true;
}

worldgen::WorldGenScheduler &ChunkMap::enableWorldGen(
    worldgen::ChunkGenerator &generator, std::int64_t worldSeed,
    RawChunkStore *diskStore,
    std::shared_ptr<worldgen::LightOpticalContext> lightContext,
    worldgen::GeneratedLightQueue *lightQueue,
    worldgen::WorldGenSchedulerConfig config) {
  scheduler_ = std::make_unique<worldgen::WorldGenScheduler>(
      *this, generator, worldSeed, diskStore, std::move(lightContext),
      lightQueue, config);
  return *scheduler_;
}

void ChunkMap::disableWorldGen() { scheduler_.reset(); }

LightSectionView ChunkMap::directReadSection(int chunkX, int chunkZ,
                                             int sectionY) const noexcept {
  if (world_ == nullptr)
    return LightSectionView{};
  auto holder = findHolder(packChunkKey(chunkX, chunkZ));
  if (!holder)
    return LightSectionView{};
  auto view = holder->readLightSection(sectionY, world_->minY() >> 4,
                                       world_->sectionCount());
  return view.value_or(LightSectionView{});
}

LightSectionView ChunkMap::directReadSectionCallback(const void *context,
                                                     int chunkX, int chunkZ,
                                                     int sectionY) noexcept {
  const auto *self = static_cast<const ChunkMap *>(context);
  return self ? self->directReadSection(chunkX, chunkZ, sectionY)
              : LightSectionView{};
}

std::size_t ChunkMap::holderCount() const { return holders_.size(); }

} // namespace redstone
