#include "light/service_impl.hpp"

#include <algorithm>
#include <bit>
#include <exception>
#include <limits>
#include <map>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "light/block_light_engine.hpp"

#include <unordered_set>

namespace redstone {

using light_internal::applyThreadAffinity;
using light_internal::chunkKey;
using light_internal::currentThreadCpuMs;
using light_internal::emptyLightProperties;
using light_internal::GeometryChunk;
using light_internal::GeometryInput;
using light_internal::GrowableSpscRing;
using light_internal::LayerOutcome;
using light_internal::LayerResult;
using light_internal::LayerWorkspace;
using light_internal::LightEngineHost;
using light_internal::LightPos;
using light_internal::PendingChunk;
using light_internal::PreparedBatch;
using light_internal::WorkingChunk;

LightService::Impl::Impl(int minSection, int sectionCount,
                         LightWorkerOptions workerOptions)
    : minBlockSectionY(minSection), blockSectionCount(sectionCount),
      options(workerOptions), paused(workerOptions.startPausedForTesting) {
  if (sectionCount <= 0)
    throw std::invalid_argument("light service needs block sections");
  if (options.batchSize == 0 || options.maxQueuedCommands == 0)
    throw std::invalid_argument("light worker limits must be positive");
  if (options.batchWindowMs < 0)
    throw std::invalid_argument("light batch window must be non-negative");
  if (options.cellQueueCapacity != 0 && options.cellQueueCapacity < 2)
    throw std::invalid_argument("light cell queue capacity must be at least two");
  const std::size_t geometryCapacity =
      options.cellQueueCapacity != 0
          ? options.cellQueueCapacity
          : std::max<std::size_t>(2, options.geometryQueueBytes /
                                         sizeof(GeometryInput));
  inputQueue =
      std::make_unique<GrowableSpscRing<GeometryInput>>(geometryCapacity);
  resultQueue = std::make_unique<GrowableSpscRing<LayerResult>>(
      std::max<std::size_t>(2, options.resultQueueBytes / sizeof(LayerResult)));
  controlQueue = std::make_unique<GrowableSpscRing<PendingChunk>>(
      std::max<std::size_t>(2, options.maxQueuedCommands));
  opticalTable = defaultLightOpticalTable();
  refreshScratchMetrics();
  worker = std::thread([this] { run(); });
#if defined(_WIN32) || defined(__linux__)
  const auto affinity =
      applyThreadAffinity(worker.native_handle(), options.workerAffinityMask);
  statsWorkerAffinity.store(affinity.mask, std::memory_order_relaxed);
  statsWorkerAffinityError.store(affinity.error, std::memory_order_relaxed);
  statsBlockWorkerAffinity.store(affinity.mask, std::memory_order_relaxed);
  statsBlockWorkerAffinityError.store(affinity.error,
                                      std::memory_order_relaxed);
#endif
}

LightService::Impl::~Impl() { close(); }

bool LightService::Impl::enqueueControl(PendingChunk state) {
  if (!controlQueue->push(std::move(state))) {
    ++statsRejected;
    return false;
  }
  ++statsSubmitted;
  signalWorkerLocked(true);
  return true;
}

std::uint64_t LightService::Impl::submitChunk(LightChunkInput input) {
  if (!validInput(input)) {
    ++statsRejected;
    return 0;
  }
  if (!accepting) {
    ++statsRejected;
    return 0;
  }
  const auto key = chunkKey(input.chunkX, input.chunkZ);
  auto &active = versions[key];
  const bool newLifetime = active.inactive || active.requestRevision == 0;
  if (newLifetime) {
    if (input.lifecycleEpoch != 0)
      active.epoch = input.lifecycleEpoch;
    else
      ++active.epoch;
    active.requestRevision = 0;
    latestBlock.erase(key);
    latestSky.erase(key);
  } else if (input.lifecycleEpoch != 0 && input.lifecycleEpoch != active.epoch) {
    ++statsRejected;
    return 0;
  }
  const auto generation = nextRequestGeneration();
  ++active.requestRevision;
  active.submittedGeneration = generation;
  active.inactive = false;
  mainVersions[key] = active;
  input.lifecycleEpoch = active.epoch;
  PendingChunk state;
  state.key = key;
  state.generation = generation;
  state.inputSequence = generation;
  state.lifecycleEpoch = active.epoch;
  state.revision = active.requestRevision;
  state.load = std::make_unique<LightChunkInput>(std::move(input));
  return enqueueControl(std::move(state)) ? generation : 0;
}

std::uint64_t LightService::Impl::nextRequestGeneration() {
  return nextGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void LightService::Impl::loadGeometry(std::uint64_t key, LightChunkInput &input) {
  if (readyChunks.insert(key).second)
    justBecameReady.push_back(key);
  if (input.directGeometry) {
    geometry.erase(key);
    directChunks[key] = light_internal::DirectChunkMeta{
        input.chunkX, input.chunkZ, input.lifecycleEpoch};
    return;
  }
  directChunks.erase(key);
  GeometryChunk chunk;
  chunk.chunkX = input.chunkX;
  chunk.chunkZ = input.chunkZ;
  chunk.lifecycleEpoch = input.lifecycleEpoch;
  chunk.sections = std::move(input.geometrySections);
  geometry[key] = std::move(chunk);
}

std::optional<LightSectionView>
LightService::Impl::readDirectSection(std::uint64_t key, int sectionY) const {
  if (!options.directWorld.context)
    return std::nullopt;
  if (sectionY < minBlockSectionY ||
      sectionY >= minBlockSectionY + blockSectionCount)
    return std::nullopt;
  const auto metaIt = directChunks.find(key);
  if (metaIt == directChunks.end())
    return std::nullopt;
  return options.directWorld.readSection(
      options.directWorld.context, metaIt->second.chunkX,
      metaIt->second.chunkZ, sectionY);
}

LightService::Impl::SectionMetaSummary
LightService::Impl::sectionMetaSummary(std::uint64_t key, int sectionY) const {
  if (const GeometrySection *geo = geometrySection(key, sectionY))
    return {geo->empty, geo->unknown};
  const auto view = readDirectSection(key, sectionY);
  if (!view)
    return {};
  if (view->bits == 0) {
    const auto &props = opticalClass(classForState(view->single)).properties;
    return {view->empty, static_cast<std::uint16_t>(
                             props.supported ? 0 : kLightVolume)};
  }
  bool anyUnsupported = false;
  for (BlockState::Id sid : view->palette) {
    if (!opticalClass(classForState(sid)).properties.supported) {
      anyUnsupported = true;
      break;
    }
  }
  std::uint16_t unknown = 0;
  if (anyUnsupported) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(kLightVolume); ++i) {
      if (!opticalClass(classForState(view->atomicStateAt(i)))
               .properties.supported)
        ++unknown;
    }
  }
  return {view->empty, unknown};
}

GeometrySection *LightService::Impl::geometrySection(std::uint64_t key,
                                                     int sectionY) {
  const auto chunk = geometry.find(key);
  if (chunk == geometry.end())
    return nullptr;
  const int index = sectionY - minBlockSectionY;
  if (index < 0 || index >= blockSectionCount)
    return nullptr;
  return &chunk->second.sections[static_cast<std::size_t>(index)];
}

const GeometrySection *
LightService::Impl::geometrySection(std::uint64_t key, int sectionY) const {
  const auto chunk = geometry.find(key);
  if (chunk == geometry.end())
    return nullptr;
  const int index = sectionY - minBlockSectionY;
  if (index < 0 || index >= blockSectionCount)
    return nullptr;
  return &chunk->second.sections[static_cast<std::size_t>(index)];
}

const std::shared_ptr<LightChunkSnapshot> &
LightService::Impl::storedLayerSnapshot(LightLayer layer,
                                        std::uint64_t key) const {
  static const std::shared_ptr<LightChunkSnapshot> empty;
  const auto &source =
      layer == LightLayer::Block ? lastReadyBlock : lastReadySky;
  const auto found = source.find(key);
  return found == source.end() ? empty : found->second;
}

std::shared_ptr<LightChunkSnapshot> LightService::Impl::makeRawLayerShell(
    std::uint64_t key, int chunkX, int chunkZ, std::uint64_t generation,
    std::uint64_t epoch) const {
  auto snapshot = std::make_shared<LightChunkSnapshot>();
  snapshot->chunkX = chunkX;
  snapshot->chunkZ = chunkZ;
  snapshot->minBlockSectionY = minBlockSectionY;
  snapshot->blockSectionCount = blockSectionCount;
  snapshot->skyLightEnabled = options.hasSkyLight;
  snapshot->generation = generation;
  snapshot->lifecycleEpoch = epoch;
  snapshot->revision = 0;
  snapshot->sections.reserve(
      static_cast<std::size_t>(blockSectionCount + 2));
  for (int sectionY = minBlockSectionY - 1;
       sectionY <= minBlockSectionY + blockSectionCount; ++sectionY) {
    const SectionMetaSummary summary = sectionMetaSummary(key, sectionY);
    snapshot->sections.push_back(std::make_shared<LightSectionSnapshot>(
        sectionY, summary.empty, std::array<LightLayerRef, kLightLayerCount>{},
        summary.unknown));
  }
  return snapshot;
}

namespace {

LightLayerRef layerRefFromNibbles(const std::array<std::uint8_t, 2048> &bytes) {
  const std::uint8_t firstLow = static_cast<std::uint8_t>(bytes[0] & 0x0Fu);
  const std::uint8_t paired =
      static_cast<std::uint8_t>(firstLow | (firstLow << 4));
  bool uniform = true;
  for (std::uint8_t b : bytes) {
    if (b != paired) {
      uniform = false;
      break;
    }
  }
  if (uniform)
    return LightLayerRef(LightLayerSnapshot::uniform(firstLow));
  auto page = std::make_shared<LightLayerSnapshot::PackedPage>();
  for (std::size_t i = 0; i < bytes.size(); ++i)
    page->bytes[i].store(bytes[i], std::memory_order_relaxed);
  return LightLayerRef(std::move(page));
}

} // namespace

void LightService::Impl::applyPersistedChunkLight(
    std::uint64_t key, const PersistedChunkLight &persisted,
    std::uint64_t generation, std::uint64_t epoch,
    std::shared_ptr<LightChunkSnapshot> &blockShell,
    std::shared_ptr<LightChunkSnapshot> &skyShell) const {
  const int minLightY = minBlockSectionY - 1;
  const int maxLightY = minBlockSectionY + blockSectionCount;
  const std::size_t count = static_cast<std::size_t>(blockSectionCount + 2);
  std::vector<const PersistedLightSection *> byIndex(count, nullptr);
  for (const PersistedLightSection &sec : persisted.sections) {
    if (sec.sectionY >= minLightY && sec.sectionY <= maxLightY)
      byIndex[static_cast<std::size_t>(sec.sectionY - minLightY)] = &sec;
  }

  int chunkX = 0;
  int chunkZ = 0;
  if (const auto geoIt = geometry.find(key); geoIt != geometry.end()) {
    chunkX = geoIt->second.chunkX;
    chunkZ = geoIt->second.chunkZ;
  } else if (const auto dirIt = directChunks.find(key);
             dirIt != directChunks.end()) {
    chunkX = dirIt->second.chunkX;
    chunkZ = dirIt->second.chunkZ;
  } else {
    return;
  }

  blockShell = makeRawLayerShell(key, chunkX, chunkZ, generation, epoch);
  if (options.hasSkyLight)
    skyShell = makeRawLayerShell(key, chunkX, chunkZ, generation, epoch);

  auto sectionNonEmptyInChunk = [&](int secY) {
    if (const GeometrySection *geo = geometrySection(key, secY))
      return !geo->empty;
    if (const auto view = readDirectSection(key, secY))
      return !view->empty;
    return false;
  };

  for (std::size_t i = 0; i < count; ++i) {
    const int sectionY = minLightY + static_cast<int>(i);
    const PersistedLightSection *psec = byIndex[i];
    if (psec && psec->blockLight) {
      auto &slot = blockShell->sections[i];
      if (slot.use_count() > 1)
        slot = std::make_shared<LightSectionSnapshot>(*slot);
      auto *mutableSec = const_cast<LightSectionSnapshot *>(slot.get());
      mutableSec->layers[static_cast<std::size_t>(LightLayer::Block)] =
          layerRefFromNibbles(*psec->blockLight);
    }
    if (options.hasSkyLight && skyShell) {
      std::optional<LightLayerRef> skyRef;
      if (psec && psec->skyLight) {
        skyRef = layerRefFromNibbles(*psec->skyLight);
      } else if (sectionNonEmptyInChunk(sectionY - 1) ||
                 sectionNonEmptyInChunk(sectionY) ||
                 sectionNonEmptyInChunk(sectionY + 1)) {
        // Omitted SkyLight inside an eligible section with blocks encodes
        // Vanilla DataLayer.isEmpty() (all zeroes), not Missing/inherit-15.
        skyRef = LightLayerRef(LightLayerSnapshot::uniform(0));
      }
      if (skyRef) {
        auto &slot = skyShell->sections[i];
        if (slot.use_count() > 1)
          slot = std::make_shared<LightSectionSnapshot>(*slot);
        auto *mutableSec = const_cast<LightSectionSnapshot *>(slot.get());
        mutableSec->layers[static_cast<std::size_t>(LightLayer::Sky)] = *skyRef;
      }
    }
  }
}

void LightService::Impl::syncRawLayerGeometry(
    std::shared_ptr<LightChunkSnapshot> &snapshot, std::uint64_t key) const {
  if (!snapshot)
    return;
  for (std::size_t i = 0; i < snapshot->sections.size(); ++i) {
    const auto &cur = snapshot->sections[i];
    if (!cur)
      continue;
    const bool inBlockRange =
        cur->sectionY >= snapshot->minBlockSectionY &&
        cur->sectionY <
            snapshot->minBlockSectionY + snapshot->blockSectionCount;
    const SectionMetaSummary summary =
        inBlockRange ? sectionMetaSummary(key, cur->sectionY)
                     : SectionMetaSummary{};
    const bool targetEmpty = summary.empty;
    const std::uint16_t targetUnknown = summary.unknown;
    if (cur->empty == targetEmpty && cur->unsupportedCells == targetUnknown)
      continue;
    light_internal::mutableLayerSnapshot(snapshot);
    auto &slot = snapshot->sections[i];
    if (slot.use_count() > 1)
      slot = std::make_shared<LightSectionSnapshot>(*slot);
    auto *mutableSec = const_cast<LightSectionSnapshot *>(slot.get());
    mutableSec->empty = targetEmpty;
    mutableSec->unsupportedCells = targetUnknown;
  }
}

LightService::Impl::RawCellResult LightService::Impl::applyRawGeometryCell(
    std::uint64_t key, int sectionY, std::uint16_t localIndex,
    BlockState::Id stateId) {
  RawCellResult result;
  GeometrySection *section = geometrySection(key, sectionY);
  if (!section)
    return result;
  const std::uint16_t oldClass = section->at(localIndex);
  const std::uint16_t newClass = classForState(stateId);
  const auto &oldOptical = opticalClass(oldClass);
  const auto &newOptical = opticalClass(newClass);
  result.opticalChanged = oldOptical.properties != newOptical.properties ||
                          oldOptical.air != newOptical.air;
  if (!result.opticalChanged)
    return result;
  const bool oldEmpty = section->empty;
  if (!oldOptical.air)
    --section->nonAir;
  if (!oldOptical.properties.supported)
    --section->unknown;
  if (!newOptical.air)
    ++section->nonAir;
  if (!newOptical.properties.supported)
    ++section->unknown;
  if (section->uniform && oldClass == section->uniformClass) {
    if (newClass != section->uniformClass) {
      section->cells = std::make_unique<GeometrySection::Cells>();
      section->cells->fill(static_cast<std::uint8_t>(section->uniformClass));
      (*section->cells)[localIndex] = static_cast<std::uint8_t>(newClass);
      section->uniform = false;
    }
  } else {
    (*section->cells)[localIndex] = static_cast<std::uint8_t>(newClass);
  }
  section->empty = section->nonAir == 0;
  result.emptyChanged = oldEmpty != section->empty;
  return result;
}

void LightService::Impl::notifyWorkerSleeping() {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (workerSleeping.load(std::memory_order_relaxed)) {
    wakeSequence.fetch_add(1, std::memory_order_release);
    wakeSequence.notify_one();
  }
}

void LightService::Impl::signalWorkerLocked(bool batchWindow) {
  notifyWorkerSleeping();
  if (batchWindow)
    wake.notify_all();
}

bool LightService::Impl::applyOwnedCellLocked(std::uint64_t sequence,
                                              const LightCellUpdate &update) {
  const int sectionY = update.y >> 4;
  if (!validSection(sectionY)) {
    ++statsRejected;
    return false;
  }
  const auto key = chunkKey(update.x >> 4, update.z >> 4);
  auto active = workerVersions.find(key);
  if (active == workerVersions.end() || active->second.inactive) {
    ++statsRejected;
    return false;
  }
  auto stateIt = pending.find(key);
  if (stateIt == pending.end())
    stateIt = pending.emplace(key, PendingChunk{}).first;
  PendingChunk &state = stateIt->second;
  state.key = key;
  if (sequence <= state.inputSequence)
    return true;
  state.generation = sequence;
  state.lifecycleEpoch = active->second.epoch;
  state.revision = ++active->second.requestRevision;
  const std::uint16_t localIndex =
      static_cast<std::uint16_t>(lightIndex(update.x, update.y, update.z));
  auto &owned = state.ownedCells[changeSlot(sectionY, localIndex)];
  if (sequence > owned.sequence)
    owned = {sequence, update.state};
  notePendingLocked();
  return true;
}

bool LightService::Impl::unloadChunk(int chunkX, int chunkZ) {
  const auto key = chunkKey(chunkX, chunkZ);
  const auto active = versions.find(key);
  if (!accepting || active == versions.end() || active->second.inactive) {
    ++statsRejected;
    return false;
  }
  const auto generation = nextRequestGeneration();
  auto &version = active->second;
  ++version.epoch;
  version.requestRevision = 0;
  version.submittedGeneration = generation;
  version.inactive = true;
  latestBlock.erase(key);
  latestSky.erase(key);
  mainVersions[key] = version;
  PendingChunk state;
  state.key = key;
  state.generation = generation;
  state.lifecycleEpoch = version.epoch;
  state.unload = true;
  return enqueueControl(std::move(state));
}

void LightService::Impl::mergeChangedSections(
    std::vector<LightChunkSnapshot::LayerChange> &dest,
    const std::vector<LightChunkSnapshot::LayerChange> &extra) {
  dest.insert(dest.end(), extra.begin(), extra.end());
  std::sort(dest.begin(), dest.end(), [](const auto &a, const auto &b) {
    return a.sectionY < b.sectionY;
  });
  std::size_t write = 0;
  for (std::size_t i = 0; i < dest.size(); ++i) {
    if (write > 0 && dest[write - 1].sectionY == dest[i].sectionY) {
      dest[write - 1].sky |= dest[i].sky;
      dest[write - 1].block |= dest[i].block;
    } else {
      if (write != i)
        dest[write] = dest[i];
      ++write;
    }
  }
  dest.resize(write);
}

void LightService::Impl::drainPublished() {
  // 生成阶段有多个线程会并发排空；结果队列是单消费者，这里串行化。
  std::lock_guard<std::mutex> drainLock(drainMutex);
  if (resultQueue->empty())
    return;
  std::vector<LayerResult> coalesced;
  std::unordered_map<DrainResultKey, std::size_t, DrainResultKeyHash>
      latestResult;
  LayerResult incoming;
  while (resultQueue->pop(incoming)) {
    if (!incoming.snapshot) {
      coalesced.push_back(std::move(incoming));
      continue;
    }
    const DrainResultKey identity{incoming.layer, incoming.key,
                                  incoming.snapshot->lifecycleEpoch};
    const auto [it, inserted] =
        latestResult.emplace(identity, coalesced.size());
    if (inserted) {
      coalesced.push_back(std::move(incoming));
      continue;
    }
    LayerResult &current = coalesced[it->second];
    auto mergedChanges = current.snapshot->changedSections;
    mergeChangedSections(mergedChanges, incoming.snapshot->changedSections);
    incoming.snapshot->changedSections = std::move(mergedChanges);
    if (incoming.snapshot->deltaBaseRevision == current.snapshot->revision)
      incoming.snapshot->deltaBaseRevision =
          current.snapshot->deltaBaseRevision;
    current = std::move(incoming);
  }
  if (coalesced.empty())
    return;

  const bool hasCallback = static_cast<bool>(publishCallback);
  std::vector<std::shared_ptr<const LightChunkSnapshot>> callbacks;
  std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback;
  if (hasCallback)
    callback = publishCallback;
  for (LayerResult &item : coalesced) {
    if (!item.snapshot) {
      ++statsStale;
      continue;
    }
    const auto active = mainVersions.find(item.key);
    if (active == mainVersions.end() || active->second.inactive ||
        active->second.epoch != item.snapshot->lifecycleEpoch) {
      ++statsStale;
      continue;
    }
    auto &latestForLayer =
        item.layer == LightLayer::Block ? latestBlock : latestSky;
    const auto current = latestForLayer.find(item.key);
    if (current != latestForLayer.end() &&
        current->second->revision > item.snapshot->revision) {
      ++statsStale;
      continue;
    }
    latestForLayer[item.key] = item.snapshot;
    for (const auto &change : item.snapshot->changedSections) {
      statsReceivedBlockSections.fetch_add(change.block ? 1 : 0,
                                           std::memory_order_relaxed);
      statsReceivedSkySections.fetch_add(change.sky ? 1 : 0,
                                         std::memory_order_relaxed);
    }
    if (hasCallback)
      callbacks.push_back(std::move(item.snapshot));
  }
  if (hasCallback)
    for (const auto &snapshot : callbacks)
      callback(snapshot);
}

std::shared_ptr<const LightChunkSnapshot>
LightService::Impl::latestLayerChunk(LightLayer layer, int x, int z) const {
  const auto &source = layer == LightLayer::Block ? latestBlock : latestSky;
  const auto found = source.find(chunkKey(x, z));
  return found == source.end() ? nullptr : found->second;
}

std::optional<std::array<int, 256>>
LightService::Impl::skyColumnSourcesForTesting(int x, int z) const {
  std::lock_guard lock(mutex);
  if (busy || !pending.empty() || !ringEmpty())
    return std::nullopt;
  const auto found = skyColumnSources.find(chunkKey(x, z));
  if (found == skyColumnSources.end())
    return std::nullopt;
  return found->second;
}

std::optional<BlockLightProperties>
LightService::Impl::opticalPropertiesForTesting(int x, int y, int z) const {
  std::lock_guard lock(mutex);
  if (busy || !pending.empty() || !ringEmpty())
    return std::nullopt;
  const std::uint64_t key = chunkKey(x >> 4, z >> 4);
  if (const GeometrySection *section = geometrySection(key, y >> 4))
    return opticalClass(section->at(lightIndex(x, y, z))).properties;
  if (const auto view = readDirectSection(key, y >> 4))
    return opticalClass(classForState(view->atomicStateAt(lightIndex(x, y, z))))
        .properties;
  return std::nullopt;
}

void LightService::Impl::setPublishCallback(
    std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback) {
  std::lock_guard lock(mutex);
  publishCallback = std::move(callback);
}

LightService::Stats LightService::Impl::stats() const {
  Stats result;
  result.workerCount = 1;
  result.submitted = statsSubmitted.load(std::memory_order_relaxed);
  result.rejected = statsRejected.load(std::memory_order_relaxed);
  result.processedBatches = statsBatches.load(std::memory_order_relaxed);
  result.processedBlockChanges = statsChanges.load(std::memory_order_relaxed);
  result.droppedGeometryTasks =
      statsDroppedGeometry.load(std::memory_order_relaxed);
  result.published = statsPublished.load(std::memory_order_relaxed);
  result.stalePublications = statsStale.load(std::memory_order_relaxed);
  result.queueHighWater = statsHighWater.load(std::memory_order_relaxed);
  result.lightQueueEntries =
      statsLightQueueEntries.load(std::memory_order_relaxed);
  result.skySectionChanges = statsSkyChanges.load(std::memory_order_relaxed);
  result.blockSectionChanges =
      statsBlockChanges.load(std::memory_order_relaxed);
  result.receivedSkySections =
      statsReceivedSkySections.load(std::memory_order_relaxed);
  result.receivedBlockSections =
      statsReceivedBlockSections.load(std::memory_order_relaxed);
  result.unloadedNeighborSkips =
      statsUnloadedNeighborSkips.load(std::memory_order_relaxed);
  result.workerCpuMs = statsWorkerCpuMs.load(std::memory_order_relaxed);
  result.blockWorkerCpuMs =
      statsBlockWorkerCpuMs.load(std::memory_order_relaxed);
  result.skyWorkerCpuMs = statsSkyWorkerCpuMs.load(std::memory_order_relaxed);
  result.workerAffinityMask =
      statsWorkerAffinity.load(std::memory_order_relaxed);
  result.workerAffinityRequested =
      static_cast<std::uint64_t>(options.workerAffinityMask);
  result.workerAffinityError =
      statsWorkerAffinityError.load(std::memory_order_relaxed);
  result.blockWorkerAffinityRequested =
      static_cast<std::uint64_t>(options.workerAffinityMask);
  result.blockWorkerAffinityMask =
      statsBlockWorkerAffinity.load(std::memory_order_relaxed);
  result.blockWorkerAffinityError =
      statsBlockWorkerAffinityError.load(std::memory_order_relaxed);
  result.skyWorkerAffinityRequested =
      static_cast<std::uint64_t>(options.workerAffinityMask);
  result.skyWorkerAffinityMask =
      statsWorkerAffinity.load(std::memory_order_relaxed);
  result.skyWorkerAffinityError =
      statsWorkerAffinityError.load(std::memory_order_relaxed);
  std::lock_guard lock(mutex);
  result.pendingChunks = pendingChunkCountLocked();
  result.pendingPublications = resultQueue->size();
  return result;
}

void LightService::Impl::resetMeasurementStats() {
  statsSubmitted.store(0, std::memory_order_relaxed);
  statsRejected.store(0, std::memory_order_relaxed);
  statsDroppedGeometry.store(0, std::memory_order_relaxed);
  statsBatches.store(0, std::memory_order_relaxed);
  statsChanges.store(0, std::memory_order_relaxed);
  statsPublished.store(0, std::memory_order_relaxed);
  statsStale.store(0, std::memory_order_relaxed);
  statsHighWater.store(0, std::memory_order_relaxed);
  statsLightQueueEntries.store(0, std::memory_order_relaxed);
  statsSkyChanges.store(0, std::memory_order_relaxed);
  statsBlockChanges.store(0, std::memory_order_relaxed);
  statsReceivedSkySections.store(0, std::memory_order_relaxed);
  statsReceivedBlockSections.store(0, std::memory_order_relaxed);
  statsUnloadedNeighborSkips.store(0, std::memory_order_relaxed);
}

std::string LightService::Impl::workerError() const {
  std::lock_guard lock(mutex);
  return error;
}

bool LightService::Impl::flushAndWait(std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex);
  const bool wasFlushRequested = flushRequested;
  flushRequested = true;
  signalWorkerLocked(true);
  auto done = [&] {
    return closing || (!busy && !compacting &&
                       !compactRequested.load(std::memory_order_acquire) &&
                       pending.empty() && orderedInputEmptyLocked());
  };
  bool reached = true;
  if (timeout.count() > 0)
    reached = idle.wait_for(lock, timeout, done);
  else
    idle.wait(lock, done);
  flushRequested = wasFlushRequested;
  return reached && error.empty() && !closing;
}

bool LightService::Impl::waitForChunk(int chunkX, int chunkZ,
                                      std::uint64_t generation,
                                      std::chrono::milliseconds timeout) {
  if (generation == 0)
    return true;
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  std::unique_lock lock(mutex);
  const bool wasFlushRequested = flushRequested;
  // 跳过 30ms 合批窗口，让本区块的工作尽快进入传播。
  flushRequested = true;
  signalWorkerLocked(true);
  auto done = [&] {
    if (closing)
      return true;
    if (!orderedInputEmptyLocked())
      return false;
    const auto found = processedGeneration.find(key);
    return found != processedGeneration.end() && found->second >= generation;
  };
  bool reached = true;
  if (timeout.count() > 0)
    reached = idle.wait_for(lock, timeout, done);
  else
    idle.wait(lock, done);
  flushRequested = wasFlushRequested;
  return reached && error.empty() && !closing;
}

// 8 邻域几何是否都已注册；未就绪时推迟本区块，避免在缺少邻区块几何的拓扑上
// 传播并把结果发布出去（原版 LightEngine.checkEdge 的延迟语义）。
bool LightService::Impl::neighborsReadyFor(std::uint64_t key) const {
  const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
  const int chunkZ = static_cast<int>(static_cast<std::int32_t>(key & 0xFFFFFFFFu));
  for (int dz = -1; dz <= 1; ++dz) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (!readyChunks.contains(chunkKey(chunkX + dx, chunkZ + dz)))
        return false;
    }
  }
  return true;
}

// 收敛判据：本区块提交的输入都处理过，且它自己不在待处理集合里。
// 注意这允许之后邻居到达时再更新本区块的光照数据（原版语义：光照是可变数据）。
bool LightService::Impl::isChunkSettled(int chunkX, int chunkZ) const {
  std::lock_guard lock(mutex);
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  const auto version = versions.find(key);
  if (version == versions.end() || version->second.inactive)
    return false;
  const auto processed = processedGeneration.find(key);
  if (processed == processedGeneration.end() ||
      processed->second < version->second.submittedGeneration)
    return false;
  return !pending.contains(key);
}

bool LightService::Impl::needsTaskQueueCompact() const noexcept {
  return scratchCompactable.load(std::memory_order_acquire);
}

std::size_t LightService::Impl::scratchTaskQueueBytesForTesting() const noexcept {
  return scratchQueueBytes.load(std::memory_order_acquire);
}

bool LightService::Impl::requestTaskQueueCompact(
    CleanupExecutionGate::Lease lease) {
  std::lock_guard lock(mutex);
  if (closing || !accepting)
    return false;
  if (compactRequested.load(std::memory_order_acquire))
    return false;
  if (!scratchCompactable.load(std::memory_order_acquire))
    return false;
  compactLease = std::move(lease);
  compactRequested.store(true, std::memory_order_release);
  signalWorkerLocked(true);
  return true;
}

LightService::ChunkStatus LightService::Impl::chunkStatus(int chunkX,
                                                          int chunkZ) const {
  ChunkStatus status;
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  const auto active = versions.find(key);
  if (active == versions.end() || active->second.inactive)
    return status;

  static constexpr std::pair<int, int> kCardinals[4] = {
      {0, -1}, {0, 1}, {-1, 0}, {1, 0}};
  for (int i = 0; i < 4; ++i) {
    const auto nIt =
        versions.find(chunkKey(chunkX + kCardinals[i].first,
                               chunkZ + kCardinals[i].second));
    if (nIt == versions.end() || nIt->second.inactive)
      status.unloadedNeighborMask |= static_cast<std::uint8_t>(1u << i);
  }

  status.loaded = true;
  status.lifecycleEpoch = active->second.epoch;
  status.submittedGeneration = active->second.submittedGeneration;

  if (const auto bIt = latestBlock.find(key);
      bIt != latestBlock.end() && bIt->second &&
      bIt->second->lifecycleEpoch == status.lifecycleEpoch) {
    status.blockComplete = bIt->second->blockLightComplete;
    status.publishedBlockGeneration = bIt->second->generation;
  }
  if (options.hasSkyLight) {
    if (const auto sIt = latestSky.find(key);
        sIt != latestSky.end() && sIt->second &&
        sIt->second->lifecycleEpoch == status.lifecycleEpoch) {
      status.skyComplete = sIt->second->skyLightComplete;
      status.publishedSkyGeneration = sIt->second->generation;
    }
  } else {
    status.skyComplete = true;
  }

  const bool missingGeneration =
      status.publishedBlockGeneration < status.submittedGeneration ||
      (options.hasSkyLight &&
       status.publishedSkyGeneration < status.submittedGeneration);
  bool workerPending = false;
  if (!missingGeneration) {
    std::lock_guard lock(mutex);
    workerPending = pending.contains(key) || !resultQueue->empty() || busy ||
                    !orderedInputEmptyLocked();
  }
  status.pending = missingGeneration || workerPending;
  status.complete =
      !status.pending && status.blockComplete && status.skyComplete;
  return status;
}

bool LightService::Impl::waitIdleForTesting() {
  std::unique_lock lock(mutex);
  flushRequested = true;
  signalWorkerLocked(true);
  idle.wait(lock, [&] {
    return !busy && !compacting &&
           !compactRequested.load(std::memory_order_acquire) &&
           pending.empty() && orderedInputEmptyLocked();
  });
  flushRequested = false;
  return error.empty();
}

bool LightService::Impl::waitWorkerSleepingForTesting() {
  std::unique_lock lock(mutex);
  sleepState.wait(lock, [&] {
    return workerSleeping.load(std::memory_order_acquire) || closing;
  });
  return workerSleeping.load(std::memory_order_acquire) && !closing;
}

void LightService::Impl::resumeForTesting() {
  std::lock_guard lock(mutex);
  paused = false;
  signalWorkerLocked(true);
}

void LightService::Impl::pauseForTesting() {
  std::lock_guard lock(mutex);
  paused = true;
}

bool LightService::Impl::processChunkBatchForTesting(int chunkX, int chunkZ) {
  const auto key = chunkKey(chunkX, chunkZ);
  std::unique_lock lock(mutex);
  if (!paused || busy || closing || forcedChunk)
    return false;
  drainCellChangesLocked();
  if (!pending.contains(key))
    return false;
  const auto before = statsBatches.load(std::memory_order_relaxed);
  forcedChunk = key;
  pauseAfterBatch = true;
  paused = false;
  signalWorkerLocked(true);
  batchDone.wait(lock, [&] {
    return statsBatches.load(std::memory_order_relaxed) > before || closing;
  });
  return error.empty() &&
         statsBatches.load(std::memory_order_relaxed) > before;
}

void LightService::Impl::close() {
  acceptingCells.store(false, std::memory_order_release);
  std::thread selected;
  {
    std::lock_guard lock(mutex);
    if (!accepting && !worker.joinable())
      return;
    accepting = false;
    closing = true;
    signalWorkerLocked(true);
    selected = std::move(worker);
  }
  if (selected.joinable())
    selected.join();
  std::lock_guard lock(mutex);
  compactRequested.store(false, std::memory_order_relaxed);
  compactLease.release();
}

bool LightService::Impl::validSection(int sectionY) const {
  return sectionY >= minBlockSectionY &&
         sectionY < minBlockSectionY + blockSectionCount;
}

bool LightService::Impl::validInput(const LightChunkInput &input) const {
  if (input.minBlockSectionY != minBlockSectionY ||
      input.blockSectionCount != blockSectionCount)
    return false;
  if (input.directGeometry) {
    if (!options.directWorld.context || !options.directWorld.readSection ||
        !input.geometrySections.empty())
      return false;
  } else {
    if (input.geometrySections.size() !=
        static_cast<std::size_t>(blockSectionCount))
      return false;
    for (const GeometrySection &section : input.geometrySections) {
      if (section.uniform) {
        if (section.cells || section.uniformClass >= opticalTable->classCount())
          return false;
      } else if (!section.cells) {
        return false;
      } else {
        for (std::uint8_t classId : *section.cells)
          if (classId >= opticalTable->classCount())
            return false;
      }
    }
  }
  if (input.persistedLight) {
    std::unordered_set<int> seenYs;
    const int minLightY = input.minLightSectionY();
    const int maxLightY = input.minBlockSectionY + input.blockSectionCount;
    for (const PersistedLightSection &sec : input.persistedLight->sections) {
      if (sec.sectionY < minLightY || sec.sectionY > maxLightY ||
          !seenYs.insert(sec.sectionY).second)
        return false;
    }
  }
  return true;
}

void LightService::Impl::pushCellInput(std::uint64_t sequence,
                                       LightCellUpdate update) {
  GeometryInput input{sequence, update};
  try {
    if (!inputQueue->push(std::move(input)))
      ++statsDroppedGeometry;
  } catch (const std::bad_alloc &) {
    ++statsDroppedGeometry;
  }
}

std::uint64_t LightService::Impl::reportBlockCell(LightCellUpdate update) {
  if (!acceptingCells.load(std::memory_order_acquire)) {
    ++statsRejected;
    return 0;
  }
  const std::uint64_t sequence = nextRequestGeneration();
  pushCellInput(sequence, update);
  notifyWorkerSleeping();
  return sequence;
}

std::uint64_t LightService::Impl::reportBlockCells(
    std::span<const LightCellUpdate> updates) {
  if (updates.empty())
    return 0;
  if (!acceptingCells.load(std::memory_order_acquire)) {
    statsRejected.fetch_add(updates.size(), std::memory_order_relaxed);
    return 0;
  }
  std::uint64_t sequence =
      nextGeneration.fetch_add(updates.size(), std::memory_order_acq_rel);
  for (const LightCellUpdate &update : updates)
    pushCellInput(++sequence, update);
  notifyWorkerSleeping();
  return sequence;
}

bool LightService::Impl::ringEmpty() const { return inputQueue->empty(); }

bool LightService::Impl::orderedInputEmptyLocked() const {
  return controlQueue->empty() && ringEmpty();
}

void LightService::Impl::applyOrderedControlLocked(PendingChunk control) {
  workerVersions[control.key] = {control.lifecycleEpoch, control.revision,
                                 control.unload};
  if (control.load || control.unload) {
    pending[control.key] = std::move(control);
    return;
  }
  auto inserted = pending.try_emplace(control.key, PendingChunk{});
  PendingChunk &state = inserted.first->second;
  if (control.generation <= state.inputSequence)
    return;
  state.key = control.key;
  state.generation = control.generation;
  state.lifecycleEpoch = control.lifecycleEpoch;
  state.revision = control.revision;
  for (const auto &[slot, owned] : control.ownedCells) {
    auto &current = state.ownedCells[slot];
    if (owned.sequence > current.sequence)
      current = owned;
  }
}

void LightService::Impl::drainCellChangesLocked() {
  const std::size_t initialGeometry = inputQueue->size();
  const std::size_t initialControls = controlQueue->size();
  const std::size_t maximum = initialGeometry + initialControls;
  for (std::size_t round = 0; round < maximum; ++round) {
    const GeometryInput *geometry = inputQueue->front();
    const PendingChunk *control = controlQueue->front();
    if (!geometry && !control)
      break;
    bool takeControl =
        control && (!geometry || control->generation < geometry->sequence);
    if (takeControl) {
      geometry = inputQueue->front();
      if (geometry && geometry->sequence < control->generation)
        takeControl = false;
    }
    if (takeControl) {
      PendingChunk state;
      if (!controlQueue->pop(state))
        break;
      applyOrderedControlLocked(std::move(state));
      noteHighWaterLocked();
    } else {
      GeometryInput input;
      if (!inputQueue->pop(input))
        break;
      (void)applyOwnedCellLocked(input.sequence, input.update);
    }
  }
}

void LightService::Impl::notePendingLocked() {
  ++statsSubmitted;
  noteHighWaterLocked();
}

std::size_t LightService::Impl::pendingChunkCountLocked() const {
  return pending.size() + controlQueue->size() + busyChunks;
}

void LightService::Impl::noteHighWaterLocked() {
  const auto current = pendingChunkCountLocked();
  auto previous = statsHighWater.load(std::memory_order_relaxed);
  if (current <= previous)
    return;
  while (!statsHighWater.compare_exchange_weak(previous, current,
                                               std::memory_order_relaxed)) {
    if (current <= previous)
      return;
  }
}

void LightService::Impl::awaitWork(std::unique_lock<std::mutex> &lock) {
  for (;;) {
    if (closing ||
        (!paused && (!pending.empty() || !orderedInputEmptyLocked() ||
                     compactRequested.load(std::memory_order_acquire))))
      return;
    workerSleeping.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    sleepState.notify_all();
    const auto observed = wakeSequence.load(std::memory_order_acquire);
    if (closing ||
        (!paused && (!pending.empty() || !orderedInputEmptyLocked() ||
                     compactRequested.load(std::memory_order_acquire)))) {
      workerSleeping.store(false, std::memory_order_release);
      return;
    }
    lock.unlock();
    wakeSequence.wait(observed, std::memory_order_acquire);
    lock.lock();
    workerSleeping.store(false, std::memory_order_release);
    nextBatchCollectAt = {};
  }
}

void LightService::Impl::run() {
  const double workerCpuStart = currentThreadCpuMs();
  std::unique_lock lock(mutex);
  for (;;) {
    awaitWork(lock);
    if (closing && pending.empty() && orderedInputEmptyLocked())
      break;
    if (paused && !closing)
      continue;
    if (!options.deferUntilNeighborsReady && pending.empty() &&
        orderedInputEmptyLocked() &&
        compactRequested.exchange(false, std::memory_order_acq_rel)) {
      compacting = true;
      auto lease = std::move(compactLease);
      lock.unlock();
      compactScratchTaskQueues();
      lease.release();
      lock.lock();
      compacting = false;
      idle.notify_all();
      if (closing && pending.empty() && orderedInputEmptyLocked())
        break;
      continue;
    }
    if (options.batchWindowMs > 0 && !closing && !flushRequested &&
        !forcedChunk) {
      const auto window = std::chrono::milliseconds(options.batchWindowMs);
      const auto now = std::chrono::steady_clock::now();
      if (nextBatchCollectAt == std::chrono::steady_clock::time_point{})
        nextBatchCollectAt = now + window;
      if (now < nextBatchCollectAt) {
        wake.wait_until(lock, nextBatchCollectAt, [&] {
          return closing || paused || flushRequested || forcedChunk;
        });
        if (closing && pending.empty() && orderedInputEmptyLocked())
          break;
        if (paused && !closing)
          continue;
      }
    }
    if (options.batchWindowMs > 0)
      nextBatchCollectAt = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(options.batchWindowMs);
    drainCellChangesLocked();

    std::vector<PendingChunk> batch;
    if (forcedChunk) {
      const auto selected = pending.find(*forcedChunk);
      forcedChunk.reset();
      if (selected != pending.end()) {
        batch.push_back(std::move(selected->second));
        pending.erase(selected);
      }
    } else {
      const std::size_t count = std::min(
          {options.batchSize, options.maxQueuedCommands, pending.size()});
      batch.reserve(count);
      if (!options.deferUntilNeighborsReady) {
        for (std::size_t i = 0; i < count; ++i) {
          auto it = pending.begin();
          batch.push_back(std::move(it->second));
          pending.erase(it);
        }
      } else
      // checkEdge 的等价物：邻区块几何未就绪时先推迟本区块（最多推迟若干轮，
      // 避免世界边界上永远凑不齐 8 邻域而饿死），等邻居就绪后再传播。
      for (auto it = pending.begin();
           it != pending.end() && batch.size() < count;) {
        PendingChunk &candidate = it->second;
        const bool bringsGeometry = static_cast<bool>(candidate.load);
        if (!bringsGeometry && !neighborsReadyFor(candidate.key) &&
            ++neighborDeferrals[candidate.key] < 4) {
          ++it;
          continue;
        }
        neighborDeferrals.erase(candidate.key);
        batch.push_back(std::move(candidate));
        it = pending.erase(it);
      }
      if (batch.empty() && !pending.empty()) {
        // 所有待处理区块都被邻居阻塞：退化为处理队首，保证有进展。
        auto it = pending.begin();
        batch.push_back(std::move(it->second));
        pending.erase(it);
      }
    }
    busy = true;
    busyChunks = batch.size();
    busyMinGeneration = std::numeric_limits<std::uint64_t>::max();
    for (const PendingChunk &command : batch)
      busyMinGeneration = std::min(busyMinGeneration, command.generation);
    if (batch.empty())
      busyMinGeneration = 0;
    lock.unlock();

    try {
      processBatch(batch);
    } catch (const std::exception &failure) {
      lock.lock();
      error = failure.what();
      lock.unlock();
    } catch (...) {
      lock.lock();
      error = "unknown light worker failure";
      lock.unlock();
    }

    lock.lock();
    busy = false;
    busyChunks = 0;
    busyMinGeneration = 0;
    for (const PendingChunk &command : batch) {
      auto &processed = processedGeneration[command.key];
      processed = std::max(processed, command.generation);
    }
    // checkEdge 的重入部分：新近就绪的区块会让已处理过的邻居多出一条边界光，
    // 把它们重新入队，让边界光双向补一次（单调更新，收敛到同一不动点）。
    for (std::uint64_t readyKey : justBecameReady) {
      if (!options.deferUntilNeighborsReady) continue;
      const int cx = static_cast<int>(static_cast<std::int32_t>(readyKey >> 32));
      const int cz =
          static_cast<int>(static_cast<std::int32_t>(readyKey & 0xFFFFFFFFu));
      for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
          if (dx == 0 && dz == 0)
            continue;
          const std::uint64_t neighborKey = chunkKey(cx + dx, cz + dz);
          if (!readyChunks.contains(neighborKey) ||
              !processedGeneration.contains(neighborKey))
            continue;
          const auto version = versions.find(neighborKey);
          if (version == versions.end() || version->second.inactive)
            continue;
          PendingChunk reprocess;
          reprocess.key = neighborKey;
          reprocess.generation = nextRequestGeneration();
          reprocess.inputSequence = reprocess.generation;
          reprocess.lifecycleEpoch = version->second.epoch;
          reprocess.revision = version->second.requestRevision;
          pending[neighborKey] = std::move(reprocess);
        }
      }
    }
    justBecameReady.clear();
    // 把"本批发生变化的区块"的 8 邻居重新入队：下一批会让边界光从邻居的已存储
    // 层再播种一次，反复直到没有任何 dirty，收敛结果与批次划分无关。
    if (options.deferUntilNeighborsReady) {
      for (std::uint64_t dirtyKey : repropagateFrom) {
        const int cx =
            static_cast<int>(static_cast<std::int32_t>(dirtyKey >> 32));
        const int cz = static_cast<int>(
            static_cast<std::int32_t>(dirtyKey & 0xFFFFFFFFu));
        for (int dz = -1; dz <= 1; ++dz) {
          for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dz == 0)
              continue;
            const std::uint64_t neighborKey = chunkKey(cx + dx, cz + dz);
            if (!readyChunks.contains(neighborKey))
              continue;
            const auto version = versions.find(neighborKey);
            if (version == versions.end() || version->second.inactive)
              continue;
            PendingChunk reprocess;
            reprocess.key = neighborKey;
            reprocess.generation = nextRequestGeneration();
            reprocess.inputSequence = reprocess.generation;
            reprocess.lifecycleEpoch = version->second.epoch;
            reprocess.revision = version->second.requestRevision;
            pending[neighborKey] = std::move(reprocess);
          }
        }
      }
    }
    repropagateFrom.clear();
    ++statsBatches;
    statsWorkerCpuMs.store(currentThreadCpuMs() - workerCpuStart,
                           std::memory_order_relaxed);
    if (compactRequested.exchange(false, std::memory_order_acq_rel)) {
      compacting = true;
      auto lease = std::move(compactLease);
      lock.unlock();
      compactScratchTaskQueues();
      lease.release();
      lock.lock();
      compacting = false;
    }
    if (pauseAfterBatch) {
      pauseAfterBatch = false;
      paused = true;
    }
    idle.notify_all();
    batchDone.notify_all();
    if (closing && pending.empty() && orderedInputEmptyLocked())
      break;
  }
  compactRequested.store(false, std::memory_order_relaxed);
  compactLease.release();
}

PreparedBatch
LightService::Impl::prepareBatch(std::vector<PendingChunk> &batch) {
  PreparedBatch prepared;
  const auto touchKey = [&](std::uint64_t key, bool dirty) {
    prepared.touched.insert(key);
    if (dirty)
      prepared.dirty.insert(key);
  };

  const auto applyCells = [&](std::uint64_t key, int chunkX, int chunkZ,
                              const PendingChunk &command) {
    bool anyDirty = false;
    const bool direct = directChunks.contains(key);
    for (const auto &[slot, value] : command.ownedCells) {
      const int sectionY =
          static_cast<int>(std::bit_cast<std::int32_t>(slot) >> 12);
      const std::uint16_t localIndex =
          static_cast<std::uint16_t>(slot & 0xFFFu);
      bool dirtyCell = direct;
      if (!direct) {
        const RawCellResult change =
            applyRawGeometryCell(key, sectionY, localIndex, value.state);
        const bool geometryMissing = geometrySection(key, sectionY) == nullptr;
        dirtyCell = change.opticalChanged || change.emptyChanged ||
                    geometryMissing;
      }
      if (dirtyCell) {
        ++statsChanges;
        anyDirty = true;
        prepared.changedPositions.push_back(
            {chunkX * 16 + static_cast<int>(localIndex & 15),
             sectionY * 16 + static_cast<int>((localIndex >> 8) & 15),
             chunkZ * 16 + static_cast<int>((localIndex >> 4) & 15)});
      }
    }
    return anyDirty;
  };

  for (PendingChunk &command : batch) {
    const auto key = command.key;
    prepared.maxGeneration =
        std::max(prepared.maxGeneration, command.generation);
    if (command.unload) {
      prepared.removed.insert(key);
      readyChunks.erase(key);
      neighborDeferrals.erase(key);
      prepared.loaded.erase(key);
      prepared.touched.erase(key);
      prepared.dirty.erase(key);
      persistedLightChunks.erase(key);
      continue;
    }

    if (command.load) {
      prepared.removed.erase(key);
      const auto existingGeoIt = geometry.find(key);
      const auto existingDirectIt = directChunks.find(key);
      const auto &oldLayer = storedLayerSnapshot(LightLayer::Block, key);
      const std::uint64_t existingEpoch =
          existingGeoIt != geometry.end()
              ? existingGeoIt->second.lifecycleEpoch
              : (existingDirectIt != directChunks.end()
                     ? existingDirectIt->second.lifecycleEpoch
                     : (oldLayer ? oldLayer->lifecycleEpoch : 0));
      const bool hasExisting = existingGeoIt != geometry.end() ||
                               existingDirectIt != directChunks.end() ||
                               static_cast<bool>(oldLayer);
      const bool replacedEpoch =
          hasExisting && existingEpoch != command.lifecycleEpoch;
      const bool sameEpoch =
          hasExisting && existingEpoch == command.lifecycleEpoch;
      std::optional<GeometryChunk> previousGeo;
      if (sameEpoch && existingGeoIt != geometry.end())
        previousGeo = std::move(existingGeoIt->second);
      loadGeometry(key, *command.load);
      if (replacedEpoch) {
        lastReadyBlock.erase(key);
        lastReadySky.erase(key);
        protocolBaseBlock.erase(key);
        protocolBaseSky.erase(key);
        skyColumnSources.erase(key);
        prepared.replaced.insert(key);
      } else if (sameEpoch) {
        prepared.replaced.insert(key);
        if (previousGeo) {
          const GeometryChunk &newGeo = geometry.at(key);
          const std::size_t geoCount =
              std::min(previousGeo->sections.size(), newGeo.sections.size());
          for (std::size_t s = 0; s < geoCount; ++s) {
            const int sectionY = minBlockSectionY + static_cast<int>(s);
            const GeometrySection &oldSec = previousGeo->sections[s];
            const GeometrySection &newSec = newGeo.sections[s];
            if (oldSec.uniform && newSec.uniform) {
              const auto &oldOpt = opticalClass(oldSec.uniformClass);
              const auto &newOpt = opticalClass(newSec.uniformClass);
              if (oldOpt.properties == newOpt.properties &&
                  oldOpt.air == newOpt.air)
                continue;
            }
            for (int index = 0; index < kLightVolume; ++index) {
              const auto &oldOpt =
                  opticalClass(oldSec.at(static_cast<std::size_t>(index)));
              const auto &newOpt =
                  opticalClass(newSec.at(static_cast<std::size_t>(index)));
              if (oldOpt.properties == newOpt.properties &&
                  oldOpt.air == newOpt.air)
                continue;
              const int x = index & 15;
              const int z = (index >> 4) & 15;
              const int y = (index >> 8) & 15;
              prepared.changedPositions.push_back({newGeo.chunkX * 16 + x,
                                                   sectionY * 16 + y,
                                                   newGeo.chunkZ * 16 + z});
            }
          }
        } else if (command.load->directGeometry) {
          // Direct mode keeps no prior geometry copy to diff cell-by-cell;
          // reset light snapshots so a full re-seed/persisted load runs cleanly.
          lastReadyBlock.erase(key);
          lastReadySky.erase(key);
          protocolBaseBlock.erase(key);
          protocolBaseSky.erase(key);
          skyColumnSources.erase(key);
        }
      }
      auto &blockShell = lastReadyBlock[key];
      const bool usePersisted =
          command.load->persistedLight &&
          command.load->persistedLight->isLightOn &&
          (!sameEpoch || !blockShell);
      if (usePersisted) {
        auto &skyShell = lastReadySky[key];
        applyPersistedChunkLight(key, *command.load->persistedLight,
                                 command.generation, command.lifecycleEpoch,
                                 blockShell, skyShell);
        if (!options.hasSkyLight)
          lastReadySky.erase(key);
        prepared.persistedLoaded.insert(key);
        persistedLightChunks.insert(key);
      } else {
        persistedLightChunks.erase(key);
        if (!blockShell || replacedEpoch)
          blockShell = makeRawLayerShell(
              key, command.load->chunkX, command.load->chunkZ,
              command.generation, command.lifecycleEpoch);
        else
          syncRawLayerGeometry(blockShell, key);
        if (options.hasSkyLight) {
          auto &skyShell = lastReadySky[key];
          if (!skyShell || replacedEpoch)
            skyShell = makeRawLayerShell(
                key, command.load->chunkX, command.load->chunkZ,
                command.generation, command.lifecycleEpoch);
          else
            syncRawLayerGeometry(skyShell, key);
        } else {
          lastReadySky.erase(key);
        }
      }
      prepared.loaded.insert(key);
      touchKey(key, true);
      if (!command.ownedCells.empty()) {
        if (applyCells(key, command.load->chunkX, command.load->chunkZ,
                       command))
          persistedLightChunks.erase(key);
      }
      continue;
    }

    int chunkX = 0;
    int chunkZ = 0;
    if (const auto geoIt = geometry.find(key); geoIt != geometry.end()) {
      chunkX = geoIt->second.chunkX;
      chunkZ = geoIt->second.chunkZ;
    } else if (const auto dirIt = directChunks.find(key);
               dirIt != directChunks.end()) {
      chunkX = dirIt->second.chunkX;
      chunkZ = dirIt->second.chunkZ;
    } else {
      continue;
    }
    if (applyCells(key, chunkX, chunkZ, command)) {
      touchKey(key, true);
      persistedLightChunks.erase(key);
    }
  }

  const auto markAffected = [&](std::uint64_t key) {
    prepared.affected.insert(key);
    const int x = static_cast<int>(static_cast<std::int32_t>(key >> 32));
    const int z = static_cast<int>(static_cast<std::uint32_t>(key));
    for (int dx = -1; dx <= 1; ++dx)
      for (int dz = -1; dz <= 1; ++dz)
        prepared.affected.insert(chunkKey(x + dx, z + dz));
  };
  for (std::uint64_t key : prepared.touched)
    markAffected(key);
  for (std::uint64_t key : prepared.removed)
    markAffected(key);
  return prepared;
}

LayerWorkspace LightService::Impl::layerWorkspace(const PreparedBatch &prepared,
                                                  LightLayer layer) {
  LayerWorkspace result;
  result.removed = prepared.removed;
  auto &last = layer == LightLayer::Block ? lastReadyBlock : lastReadySky;
  for (std::uint64_t key : prepared.touched) {
    if (prepared.removed.contains(key))
      continue;
    const auto it = last.find(key);
    if (it == last.end() || !it->second)
      continue;
    syncRawLayerGeometry(it->second, key);
    const auto &ownView = it->second;
    WorkingChunk chunk;
    chunk.key = key;
    chunk.lifecycleEpoch = ownView->lifecycleEpoch;
    chunk.chunkX = ownView->chunkX;
    chunk.chunkZ = ownView->chunkZ;
    chunk.snapshot = ownView;
    chunk.inPlace = true;
    chunk.dirty = prepared.dirty.contains(key);
    if (prepared.loaded.contains(key)) {
      for (int sectionY = ownView->minLightSectionY();
           sectionY <= ownView->minBlockSectionY + ownView->blockSectionCount;
           ++sectionY)
        chunk.dirtySections.push_back(sectionY);
    }
    result.chunks.emplace(key, std::move(chunk));
  }
  return result;
}

std::uint8_t LightService::Impl::persistedBoundaryMask(
    std::uint64_t key,
    const light_internal::FlatSet<std::uint64_t, light_internal::UInt64Hash>
        &removed,
    const light_internal::FlatMap<std::uint64_t,
                                  std::shared_ptr<LightChunkSnapshot>,
                                  light_internal::UInt64Hash> &ready) const {
  const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
  const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
  std::uint8_t mask = 0;
  for (int direction = 2; direction < 6; ++direction) {
    const LightPos delta = light_internal::kDirectionOffsets[direction];
    const std::uint64_t neighborKey =
        chunkKey(chunkX + delta.x, chunkZ + delta.z);
    if (!removed.contains(neighborKey) && ready.contains(neighborKey) &&
        !persistedLightChunks.contains(neighborKey))
      mask |= static_cast<std::uint8_t>(1u << direction);
  }
  return mask;
}

LayerOutcome LightService::Impl::executeBlockLayer(
    const PreparedBatch &prepared) {
  const double cpuStart = currentThreadCpuMs();
  if (options.beforeLayerForTesting)
    options.beforeLayerForTesting(LightLayer::Block);
  LayerOutcome result;
  result.layers = layerWorkspace(prepared, LightLayer::Block);
  std::uint64_t queueEntries = 0;
  std::uint64_t unloadedSkips = 0;
  LightEngineHost host{minBlockSectionY,  blockSectionCount,
                       *opticalTable,     geometry,
                       directChunks,      options.directWorld,
                       lastReadyBlock,    lastReadySky,
                       queueEntries,      unloadedSkips};
  light_internal::BlockLightPropagator propagation(host, result.layers.chunks,
                                                   result.layers.removed,
                                                   blockScratch);
  for (const LightPos &position : prepared.changedPositions)
    propagation.enqueueCheck(position);
  for (std::uint64_t key : prepared.loaded) {
    if (prepared.persistedLoaded.contains(key)) {
      const std::uint8_t mask =
          persistedBoundaryMask(key, prepared.removed, lastReadyBlock);
      if (mask != 0) {
        propagation.seedOutgoing(key, mask);
        propagation.seedIncoming(key, mask);
      }
    } else {
      propagation.seedChunk(key);
      propagation.seedIncoming(key);
    }
  }
  for (std::uint64_t key : prepared.removed)
    propagation.clearIncomingFrom(key);
  for (std::uint64_t key : prepared.replaced)
    if (!prepared.removed.contains(key))
      propagation.clearIncomingFrom(key);
  propagation.run();
  propagation.commitLayers();
  statsLightQueueEntries.fetch_add(queueEntries, std::memory_order_relaxed);
  if (unloadedSkips > 0)
    statsUnloadedNeighborSkips.fetch_add(unloadedSkips,
                                         std::memory_order_relaxed);
  result.complete = propagation.isComplete();
  statsBlockWorkerCpuMs.fetch_add(currentThreadCpuMs() - cpuStart,
                                  std::memory_order_relaxed);
  return result;
}

LayerOutcome LightService::Impl::executeSkyLayer(const PreparedBatch &prepared) {
  const double cpuStart = currentThreadCpuMs();
  if (options.beforeLayerForTesting)
    options.beforeLayerForTesting(LightLayer::Sky);
  LayerOutcome result;
  result.layers = layerWorkspace(prepared, LightLayer::Sky);
  std::uint64_t queueEntries = 0;
  std::uint64_t unloadedSkips = 0;
  LightEngineHost host{minBlockSectionY,  blockSectionCount,
                       *opticalTable,     geometry,
                       directChunks,      options.directWorld,
                       lastReadyBlock,    lastReadySky,
                       queueEntries,      unloadedSkips};
  light_internal::SkyLightPropagator sky(host, result.layers.chunks,
                                         result.layers.removed,
                                         skyColumnSources, prepared.affected,
                                         skyScratch);
  for (std::uint64_t key : prepared.loaded) {
    if (prepared.persistedLoaded.contains(key)) {
      sky.initPersistedSources(key);
      const std::uint8_t mask =
          persistedBoundaryMask(key, prepared.removed, lastReadySky);
      if (mask != 0) {
        sky.seedOutgoing(key, mask);
        sky.seedIncoming(key, mask);
      }
    } else {
      sky.seedChunk(key);
      sky.seedIncoming(key);
    }
  }
  for (std::uint64_t key : prepared.removed)
    sky.clearIncomingFrom(key);
  for (std::uint64_t key : prepared.replaced)
    if (!prepared.removed.contains(key))
      sky.clearIncomingFrom(key);

  std::vector<LightPos> orderedChanges = prepared.changedPositions;
  std::sort(orderedChanges.begin(), orderedChanges.end(),
            [](const LightPos &left, const LightPos &right) {
              if (left.x != right.x)
                return left.x < right.x;
              if (left.z != right.z)
                return left.z < right.z;
              return left.y < right.y;
            });
  struct PendingColumn {
    std::uint64_t key;
    int x;
    int z;
    int oldSource;
    int newSource;
  };
  std::vector<PendingColumn> postSeed;
  for (std::size_t begin = 0; begin < orderedChanges.size();) {
    std::size_t end = begin + 1;
    while (end < orderedChanges.size() &&
           orderedChanges[end].x == orderedChanges[begin].x &&
           orderedChanges[end].z == orderedChanges[begin].z)
      ++end;
    const LightPos &first = orderedChanges[begin];
    const std::uint64_t key = chunkKey(first.x >> 4, first.z >> 4);
    const int localX = first.x & 15;
    const int localZ = first.z & 15;
    const int oldSource = sky.columnSource(key, localX, localZ);
    int newSource = oldSource;
    for (std::size_t i = begin; i < end; ++i)
      newSource =
          sky.updateColumnSource(key, localX, localZ, orderedChanges[i].y);
    postSeed.push_back({key, localX, localZ, oldSource, newSource});
    begin = end;
  }
  for (const PendingColumn &column : postSeed)
    sky.applyColumnSource(column.key, column.x, column.z, column.oldSource,
                          column.newSource);
  for (const LightPos &position : prepared.changedPositions)
    sky.enqueueCheck(position);
  const bool needsSkyWork =
      sky.hasPendingWork() || !prepared.changedPositions.empty() ||
      !prepared.removed.empty() || !prepared.replaced.empty() ||
      prepared.persistedLoaded.size() != prepared.loaded.size();
  if (needsSkyWork) {
    sky.syncStorageEligibility();
    sky.run();
    sky.commitLayers();
  }
  statsLightQueueEntries.fetch_add(queueEntries, std::memory_order_relaxed);
  if (unloadedSkips > 0)
    statsUnloadedNeighborSkips.fetch_add(unloadedSkips,
                                         std::memory_order_relaxed);
  for (std::uint64_t key : prepared.removed)
    skyColumnSources.erase(key);
  result.complete = sky.isComplete();
  statsSkyWorkerCpuMs.fetch_add(currentThreadCpuMs() - cpuStart,
                                std::memory_order_relaxed);
  return result;
}

void LightService::Impl::publishResult(LayerResult result) {
  while (!resultQueue->push(std::move(result))) {
    if (closing.load(std::memory_order_acquire))
      return;
    std::this_thread::yield();
  }
}

void LightService::Impl::publishIndependentLayer(const PreparedBatch &prepared,
                                                 LayerOutcome &outcome,
                                                 LightLayer layer) {
  if (layer == LightLayer::Sky && !options.hasSkyLight)
    return;
  auto &nextRevision =
      layer == LightLayer::Block ? nextBlockLayerRevision : nextSkyLayerRevision;
  auto &last = layer == LightLayer::Block ? lastReadyBlock : lastReadySky;
  auto &protocolBase =
      layer == LightLayer::Block ? protocolBaseBlock : protocolBaseSky;
  std::vector<LayerResult> ready;
  std::vector<int> changed;
  std::uint64_t batchRevision = 0;
  for (auto &[key, chunk] : outcome.layers.chunks) {
    if (chunk.propagated)
      persistedLightChunks.erase(key);
    if (!chunk.dirty || prepared.removed.contains(key))
      continue;
    if (batchRevision == 0)
      batchRevision = ++nextRevision;
    light_internal::mutableLayerSnapshot(chunk.snapshot);
    auto &view = chunk.snapshot;
    view->layer = layer;
    view->generation = prepared.maxGeneration;
    view->revision = batchRevision;
    view->lifecycleEpoch = chunk.lifecycleEpoch;
    view->blockLightComplete =
        layer == LightLayer::Block ? outcome.complete : false;
    view->skyLightComplete =
        layer == LightLayer::Sky
            ? outcome.complete
            : (!options.hasSkyLight && layer == LightLayer::Block);
    view->propagationComplete = outcome.complete;
    view->protocolReady = outcome.complete;
    std::uint32_t unsupported = 0;
    if (const auto geoIt = geometry.find(key); geoIt != geometry.end()) {
      for (const GeometrySection &sec : geoIt->second.sections)
        unsupported += sec.unknown;
    } else if (directChunks.contains(key)) {
      for (int sectionY = minBlockSectionY;
           sectionY < minBlockSectionY + blockSectionCount; ++sectionY)
        unsupported += sectionMetaSummary(key, sectionY).unknown;
    }
    view->unsupportedCells = unsupported;
    view->changedSections.clear();
    view->deltaBaseRevision = 0;

    changed.assign(chunk.dirtySections.begin(), chunk.dirtySections.end());
    auto &meta = protocolBase[key];
    if (outcome.complete) {
      if (meta.hasBase && meta.epoch == view->lifecycleEpoch &&
          meta.minBlockSectionY == view->minBlockSectionY &&
          meta.blockSectionCount == view->blockSectionCount)
        view->deltaBaseRevision = meta.revision;
      if (!meta.pendingDirty.empty() &&
          meta.pendingEpoch == view->lifecycleEpoch) {
        changed.insert(changed.end(), meta.pendingDirty.begin(),
                       meta.pendingDirty.end());
        meta.pendingDirty.clear();
        meta.pendingEpoch = 0;
      }
      meta.hasBase = true;
      meta.epoch = view->lifecycleEpoch;
      meta.revision = view->revision;
      meta.minBlockSectionY = view->minBlockSectionY;
      meta.blockSectionCount = view->blockSectionCount;
    } else {
      meta.pendingDirty.insert(meta.pendingDirty.end(), changed.begin(),
                               changed.end());
      meta.pendingEpoch = view->lifecycleEpoch;
    }
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (int sectionY : changed)
      view->changedSections.push_back(
          {sectionY, layer == LightLayer::Sky, layer == LightLayer::Block});
    if (options.collectDiagnostics) {
      const auto count = static_cast<std::uint64_t>(changed.size());
      if (layer == LightLayer::Block)
        statsBlockChanges.fetch_add(count, std::memory_order_relaxed);
      else
        statsSkyChanges.fetch_add(count, std::memory_order_relaxed);
    }

    last[key] = view;
    ready.push_back({layer, key, view});
  }
  std::sort(ready.begin(), ready.end(),
            [](const LayerResult &left, const LayerResult &right) {
              return left.key < right.key;
            });
  for (LayerResult &result : ready)
    publishResult(std::move(result));
  statsPublished.fetch_add(ready.size(), std::memory_order_relaxed);
}

void LightService::Impl::executeLayers(const PreparedBatch &prepared) {
  LayerOutcome block = executeBlockLayer(prepared);
  publishIndependentLayer(prepared, block, LightLayer::Block);
  if (options.hasSkyLight) {
    LayerOutcome sky = executeSkyLayer(prepared);
    publishIndependentLayer(prepared, sky, LightLayer::Sky);
  }
}

void LightService::Impl::processBatch(std::vector<PendingChunk> &batch) {
  PreparedBatch prepared = prepareBatch(batch);
  executeLayers(prepared);
  // 记录本批真正变化的区块，供批次结束后唤醒 8 邻居（不动点迭代）。
  for (std::uint64_t key : prepared.dirty)
    repropagateFrom.push_back(key);
  refreshScratchMetrics();
  for (std::uint64_t key : prepared.removed) {
    geometry.erase(key);
    directChunks.erase(key);
    lastReadyBlock.erase(key);
    lastReadySky.erase(key);
    protocolBaseBlock.erase(key);
    protocolBaseSky.erase(key);
  }
}

void LightService::Impl::refreshScratchMetrics() {
  const bool needsCompact = blockScratch.anyQueueNeedsCompact() ||
                            skyScratch.anyQueueNeedsCompact();
  scratchCompactable.store(needsCompact, std::memory_order_release);
  scratchQueueBytes.store(
      blockScratch.totalQueueBytes() + skyScratch.totalQueueBytes(),
      std::memory_order_release);
}

bool LightService::Impl::compactScratchTaskQueues() {
  bool shrunk = blockScratch.compactQueues();
  shrunk |= skyScratch.compactQueues();
  refreshScratchMetrics();
  return shrunk;
}

LightService::LightService(int minBlockSectionY, int blockSectionCount,
                           LightWorkerOptions options)
    : impl_(std::make_unique<Impl>(minBlockSectionY, blockSectionCount,
                                   options)) {}

LightService::~LightService() = default;

std::shared_ptr<const LightOpticalTable> LightService::opticalTable() const {
  return impl_->opticalTablePtr();
}

std::uint64_t LightService::submitChunk(LightChunkInput input) {
  return impl_->submitChunk(std::move(input));
}

std::uint64_t LightService::reportBlockCell(LightCellUpdate update) {
  return impl_->reportBlockCell(update);
}

std::uint64_t LightService::reportBlockCells(
    std::span<const LightCellUpdate> updates) {
  return impl_->reportBlockCells(updates);
}

bool LightService::unloadChunk(int chunkX, int chunkZ) {
  return impl_->unloadChunk(chunkX, chunkZ);
}

void LightService::drainPublished() { impl_->drainPublished(); }

std::shared_ptr<const LightChunkSnapshot>
LightService::latestLayerChunk(LightLayer layer, int chunkX, int chunkZ) const {
  return impl_->latestLayerChunk(layer, chunkX, chunkZ);
}

std::optional<std::array<int, 256>>
LightService::skyColumnSourcesForTesting(int chunkX, int chunkZ) const {
  return impl_->skyColumnSourcesForTesting(chunkX, chunkZ);
}

std::optional<BlockLightProperties>
LightService::opticalPropertiesForTesting(int x, int y, int z) const {
  return impl_->opticalPropertiesForTesting(x, y, z);
}

void LightService::setPublishCallback(
    std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback) {
  impl_->setPublishCallback(std::move(callback));
}

LightService::Stats LightService::stats() const { return impl_->stats(); }
LightService::ChunkStatus LightService::chunkStatus(int chunkX,
                                                    int chunkZ) const {
  return impl_->chunkStatus(chunkX, chunkZ);
}
bool LightService::flushAndWait(std::chrono::milliseconds timeout) {
  return impl_->flushAndWait(timeout);
}
bool LightService::isChunkSettled(int chunkX, int chunkZ) const {
  return impl_->isChunkSettled(chunkX, chunkZ);
}
bool LightService::waitForChunk(int chunkX, int chunkZ,
                                std::uint64_t generation,
                                std::chrono::milliseconds timeout) {
  return impl_->waitForChunk(chunkX, chunkZ, generation, timeout);
}
bool LightService::needsTaskQueueCompact() const noexcept {
  return impl_->needsTaskQueueCompact();
}
bool LightService::requestTaskQueueCompact(CleanupExecutionGate::Lease lease) {
  return impl_->requestTaskQueueCompact(std::move(lease));
}
std::size_t LightService::scratchTaskQueueBytesForTesting() const noexcept {
  return impl_->scratchTaskQueueBytesForTesting();
}
void LightService::resetMeasurementStats() { impl_->resetMeasurementStats(); }
std::string LightService::workerError() const { return impl_->workerError(); }
bool LightService::waitIdleForTesting() { return impl_->waitIdleForTesting(); }
bool LightService::waitWorkerSleepingForTesting() {
  return impl_->waitWorkerSleepingForTesting();
}
void LightService::pauseForTesting() { impl_->pauseForTesting(); }
void LightService::resumeForTesting() { impl_->resumeForTesting(); }
bool LightService::processChunkBatchForTesting(int chunkX, int chunkZ) {
  return impl_->processChunkBatchForTesting(chunkX, chunkZ);
}
void LightService::close() { impl_->close(); }

} // namespace redstone
