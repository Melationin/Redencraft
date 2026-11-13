#include "light/sky_light_engine.hpp"

#include <algorithm>
#include <utility>

namespace redstone::light_internal {
namespace {

template <class Visitor>
void forEachFaceCell(const LightEngineHost &owner,
                     const LightChunkSnapshot *chunk, int direction,
                     Visitor visitor) {
  const int minX = chunk->chunkX * 16;
  const int minZ = chunk->chunkZ * 16;
  const int minY = (owner.minBlockSectionY - 1) * 16;
  const int maxY =
      (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16;
  if (direction == 2 || direction == 3) {
    const int z = direction == 2 ? minZ : minZ + 15;
    for (int y = minY; y < maxY; ++y)
      for (int x = 0; x < 16; ++x)
        visitor(LightPos{minX + x, y, z});
  } else if (direction == 4 || direction == 5) {
    const int x = direction == 4 ? minX : minX + 15;
    for (int y = minY; y < maxY; ++y)
      for (int z = 0; z < 16; ++z)
        visitor(LightPos{x, y, minZ + z});
  }
}

} // namespace

SkyLightPropagator::SkyLightPropagator(
    LightEngineHost &light,
    FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> &workingChunks,
    const FlatSet<std::uint64_t, UInt64Hash> &removedChunks,
    FlatMap<std::uint64_t, SkyColumnSources, UInt64Hash> &sources,
    const FlatSet<std::uint64_t, UInt64Hash> &affectedChunks,
    SkyPropagatorScratch &scratchBuffers)
    : owner(light), working(workingChunks), removed(removedChunks),
      sourceHeights(sources), affected(affectedChunks),
      checks(scratchBuffers.checks), checkQueue(scratchBuffers.checkQueue),
      decreaseQueue(scratchBuffers.decreaseQueue),
      increaseQueue(scratchBuffers.increaseQueue),
      currentDecreaseQueue(scratchBuffers.currentDecreaseQueue),
      currentIncreaseQueue(scratchBuffers.currentIncreaseQueue),
      layersByChunk(scratchBuffers.layersByChunk),
      dirtySections(scratchBuffers.dirtySections),
      storedEligible(scratchBuffers.storedEligible) {
  scratchBuffers.clear();
}

void SkyLightPropagator::enqueueCheck(LightPos position) {
  if (checks.insert(position).second)
    checkQueue.push(position);
}

void SkyLightPropagator::seedChunk(std::uint64_t key) {
  WorkingChunk *chunk = workingChunk(key);
  if (!chunk)
    return;
  const int chunkX = chunk->chunkX;
  const int chunkZ = chunk->chunkZ;
  const SkyColumnSources sources = computeSources(key);
  sourceHeights[key] = sources;
  clearLines(*chunk);
  const int minY = owner.minBlockSectionY * 16;
  const int lightBottom = (owner.minBlockSectionY - 1) * 16;
  const int sentinel = minY - 1;
  // Lowest y that still receives full sky light for each column. A column
  // without any obstruction is lit through the bottom padding as well.
  std::array<int, 256> litFrom{};
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x) {
      const int source = sources[HeightmapColumnKey(x, z)];
      litFrom[HeightmapColumnKey(x, z)] =
          source == sentinel ? lightBottom : source;
    }
  for (int sectionY = owner.minBlockSectionY - 1;
       sectionY <= owner.minBlockSectionY + owner.blockSectionCount;
       ++sectionY) {
    LightValues *layer = bulkValues(key, sectionY);
    if (!layer)
      continue;
    const int baseY = sectionY * 16;
    int minFirst = std::numeric_limits<int>::max();
    int maxFirst = std::numeric_limits<int>::min();
    for (int column = 0; column < 256; ++column) {
      const int first = litFrom[column] - baseY;
      minFirst = std::min(minFirst, first);
      maxFirst = std::max(maxFirst, first);
    }
    if (maxFirst <= 0) {
      layer->fill(15);
    } else if (minFirst >= 16) {
      layer->fill(0);
    } else {
      for (int index = 0; index < kLightVolume; ++index) {
        const int x = index & 15;
        const int z = (index >> 4) & 15;
        const int y = (index >> 8) & 15;
        (*layer)[static_cast<std::size_t>(index)] =
            baseY + y >= litFrom[HeightmapColumnKey(x, z)] ? 15 : 0;
      }
    }
    dirtySections.insert(
        {static_cast<int>(key >> 32), sectionY,
         static_cast<int>(static_cast<std::uint32_t>(key))});
  }
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x)
      seedColumn(chunkX, chunkZ, x, z, sources[HeightmapColumnKey(x, z)]);
}

void SkyLightPropagator::initPersistedSources(std::uint64_t key) {
  sourceHeights[key] = computeSources(key);
}

void SkyLightPropagator::seedIncoming(std::uint64_t key) {
  const LightChunkSnapshot *chunk = stateFor(key);
  if (!chunk)
    return;
  for (int direction = 2; direction < 6; ++direction) {
    const LightPos delta = kDirectionOffsets[direction];
    const std::uint64_t neighborKey =
        chunkKey(chunk->chunkX + delta.x, chunk->chunkZ + delta.z);
    const LightChunkSnapshot *neighbor = stateFor(neighborKey);
    if (!neighbor)
      continue;
    forEachFaceCell(owner, neighbor, direction ^ 1, [&](LightPos position) {
      const int level = storedLevel(position);
      if (level > 1)
        enqueueIncrease(position, static_cast<std::uint8_t>(level),
                        static_cast<std::uint8_t>(1u << (direction ^ 1)),
                        emptyShape(stateAt(position)), false);
    });
  }
}

void SkyLightPropagator::seedOutgoing(std::uint64_t key) {
  const LightChunkSnapshot *chunk = stateFor(key);
  if (!chunk)
    return;
  for (int direction = 2; direction < 6; ++direction) {
    const LightPos delta = kDirectionOffsets[direction];
    const LightChunkSnapshot *neighbor =
        stateFor(chunkKey(chunk->chunkX + delta.x, chunk->chunkZ + delta.z));
    if (!neighbor)
      continue;
    forEachFaceCell(owner, chunk, direction, [&](LightPos position) {
      const int level = storedLevel(position);
      if (level <= 1)
        return;
      const LightPos to = offset(position, direction);
      if (level - 1 <= storedLevel(to))
        return;
      enqueueIncrease(position, static_cast<std::uint8_t>(level),
                      static_cast<std::uint8_t>(1u << direction),
                      emptyShape(stateAt(position)), false);
    });
  }
}

void SkyLightPropagator::clearIncomingFrom(std::uint64_t removedKey) {
  const int chunkX = static_cast<int>(removedKey >> 32);
  const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(removedKey));
  for (int direction = 2; direction < 6; ++direction) {
    const LightPos delta = kDirectionOffsets[direction];
    const LightChunkSnapshot *neighbor =
        stateFor(chunkKey(chunkX + delta.x, chunkZ + delta.z));
    if (!neighbor)
      continue;
    forEachFaceCell(owner, neighbor, direction ^ 1, [&](LightPos position) {
      enqueueCheck(position);
    });
  }
}

void SkyLightPropagator::syncStorageEligibility() {
  storedEligible.clear();
  eligibleValid = false;
  for (const auto &[key, chunk] : owner.lastReadySky) {
    if (affected.contains(key) || removed.contains(key))
      continue;
    for (int y = chunk->minLightSectionY();
         y < chunk->minBlockSectionY + chunk->blockSectionCount + 1; ++y) {
      const LightSectionSnapshot *section = chunk->section(y);
      if (section &&
          !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
               .isMissing())
        storedEligible.insert(
            {static_cast<int>(key >> 32), y,
             static_cast<int>(static_cast<std::uint32_t>(key))});
    }
  }
  for (std::uint64_t key : affected)
    (void)ensureWorking(key);

  for (auto &[key, chunk] : working) {
    if (removed.contains(key))
      continue;
    if (!affected.contains(key)) {
      for (int y = chunk.snapshot->minLightSectionY();
           y < chunk.snapshot->minBlockSectionY +
                   chunk.snapshot->blockSectionCount + 1;
           ++y) {
        const LightSectionSnapshot *section = chunk.snapshot->section(y);
        if (section &&
            !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                 .isMissing())
          storedEligible.insert({chunk.chunkX, y, chunk.chunkZ});
      }
      continue;
    }
    const int sectionX = static_cast<int>(key >> 32);
    const int sectionZ = static_cast<int>(static_cast<std::uint32_t>(key));
    for (int y = chunk.snapshot->minLightSectionY();
         y < chunk.snapshot->minBlockSectionY +
                 chunk.snapshot->blockSectionCount + 1;
         ++y) {
      bool eligible = false;
      for (int dx = -1; dx <= 1 && !eligible; ++dx)
        for (int dz = -1; dz <= 1 && !eligible; ++dz)
          for (int dy = -1; dy <= 1 && !eligible; ++dy)
            eligible =
                blockSectionNonEmpty(sectionX + dx, y + dy, sectionZ + dz);
      const SectionPos3 sectionKey{sectionX, y, sectionZ};
      if (eligible) {
        storedEligible.insert(sectionKey);
        allocateSection(key, y);
      } else {
        dropWorkingLayer(key, y);
        invalidateInheritedBases(key);
        dirtySections.erase(sectionKey);
        const auto *old = chunk.snapshot->section(y);
        if (old &&
            !old->layers[static_cast<std::size_t>(LightLayer::Sky)]
                 .isMissing()) {
          auto *edit = chunk.sectionForEdit(y);
          edit->layers[static_cast<std::size_t>(LightLayer::Sky)] =
              LightLayerSnapshot::missing();
          chunk.dirtySections.push_back(y);
          chunk.dirty = true;
        }
      }
    }
  }
  storageReady = true;
}

int SkyLightPropagator::columnSource(std::uint64_t key, int localX,
                                     int localZ) {
  WorkingChunk *chunk = workingChunk(key);
  if (!chunk)
    return owner.minBlockSectionY * 16 - 1;
  return sourcesForWorking(key, *chunk)[HeightmapColumnKey(localX, localZ)];
}

SkyColumnSources &SkyLightPropagator::sourcesForWorking(std::uint64_t key,
                                                        WorkingChunk &) {
  auto found = sourceHeights.find(key);
  if (found == sourceHeights.end())
    found = sourceHeights.emplace(key, computeSources(key)).first;
  return found->second;
}

int SkyLightPropagator::updateColumnSource(std::uint64_t key, int localX,
                                           int localZ, int changedY) {
  WorkingChunk *chunk = workingChunk(key);
  if (!chunk)
    return owner.minBlockSectionY * 16 - 1;
  auto &sources = sourcesForWorking(key, *chunk);
  const int column = HeightmapColumnKey(localX, localZ);
  int source = sources[column];
  if (changedY + 1 < source)
    return source;

  const auto updateEdge = [&](int edgeY) {
    if (edgeY < owner.minBlockSectionY * 16)
      return;
    const int upperY = edgeY + 1;
    const auto &top = sourcePropertyAt(key, localX, upperY, localZ);
    const auto &bottom = sourcePropertyAt(key, localX, edgeY, localZ);
    if (isSourceEdgeOccluded(top, bottom)) {
      if (upperY > source)
        source = upperY;
    } else if (upperY == source) {
      source = findSourceBelow(key, localX, localZ, edgeY);
    }
  };
  updateEdge(changedY);
  updateEdge(changedY - 1);
  sources[column] = source;
  return source;
}

void SkyLightPropagator::applyColumnSource(std::uint64_t key, int localX,
                                           int localZ, int oldSource,
                                           int newSource) {
  WorkingChunk *chunk = workingChunk(key);
  if (!chunk || oldSource == newSource)
    return;
  const int chunkX = chunk->chunkX;
  const int chunkZ = chunk->chunkZ;
  const int blockMinY = owner.minBlockSectionY * 16;
  const int lightBottom = (owner.minBlockSectionY - 1) * 16;
  if (newSource > oldSource) {
    const int oldFirst = oldSource == blockMinY - 1 ? lightBottom : oldSource;
    for (int y = oldFirst; y < newSource; ++y) {
      const LightPos position{chunkX * 16 + localX, y,
                              chunkZ * 16 + localZ};
      const int oldLevel = storedLevel(position);
      if (oldLevel > 0) {
        setStoredLevel(position, 0);
        enqueueDecrease(position, static_cast<std::uint8_t>(oldLevel), 63);
      }
    }
  } else {
    const int newFirst = newSource == blockMinY - 1 ? lightBottom : newSource;
    for (int y = newFirst; y < oldSource; ++y) {
      const LightPos position{chunkX * 16 + localX, y,
                              chunkZ * 16 + localZ};
      setStoredLevel(position, 15);
      enqueueIncrease(position, 15, 63, false, true);
    }
  }
  seedColumn(chunkX, chunkZ, localX, localZ, newSource);
}

void SkyLightPropagator::run() {
  while (!checkQueue.empty()) {
    const LightPos position = checkQueue.pop();
    checks.erase(position);
    checkNode(position);
  }
  while (!currentDecreaseQueue.empty() || !decreaseQueue.empty()) {
    if (currentDecreaseQueue.empty()) {
      const DecreaseTask first = decreaseQueue.pop();
      currentDecreaseQueue.push(std::move(first));
      activeDecreaseSection =
          sectionOf(currentDecreaseQueue.front().position);
    }
    DecreaseTask task = currentDecreaseQueue.pop();
    ++owner.lightQueueEntries;
    propagateDecrease(task);
    if (currentDecreaseQueue.empty())
      activeDecreaseSection.reset();
  }
  while (!currentIncreaseQueue.empty() || !increaseQueue.empty()) {
    if (currentIncreaseQueue.empty()) {
      const IncreaseTask first = increaseQueue.pop();
      currentIncreaseQueue.push(std::move(first));
      activeIncreaseSection =
          sectionOf(currentIncreaseQueue.front().position);
    }
    IncreaseTask task = currentIncreaseQueue.pop();
    ++owner.lightQueueEntries;
    propagateIncrease(task);
    if (currentIncreaseQueue.empty())
      activeIncreaseSection.reset();
  }
}

void SkyLightPropagator::commitLayers() {
  std::vector<SectionPos3> ordered(dirtySections.begin(), dirtySections.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const auto &left, const auto &right) {
              if (left.x != right.x)
                return left.x < right.x;
              if (left.z != right.z)
                return left.z < right.z;
              return left.y > right.y;
            });
  for (const SectionPos3 &position : ordered) {
    WorkingChunk *chunk = ensureWorking(chunkKey(position.x, position.z));
    if (!chunk)
      continue;
    const LightSectionSnapshot *current = chunk->snapshot->section(position.y);
    if (!current)
      continue;
    const LightValues *layer =
        workingLayer(chunkKey(position.x, position.z), position.y);
    if (!layer)
      continue;
    const auto uniform = layer->uniformValue();
    const LightLayerRef payload =
        uniform && layer->ref().isMixed()
            ? LightLayerRef(LightLayerSnapshot::uniform(*uniform))
            : layer->ref();
    if (!current->layers[static_cast<std::size_t>(LightLayer::Sky)].samePayload(
            payload)) {
      if (LightSectionSnapshot *section = chunk->sectionForEdit(position.y))
        section->layers[static_cast<std::size_t>(LightLayer::Sky)] = payload;
    }
    chunk->dirtySections.push_back(position.y);
    chunk->dirty = true;
  }
}

int SkyLightPropagator::workingSectionIndex(int sectionY) const {
  const std::int64_t index =
      static_cast<std::int64_t>(sectionY) -
      static_cast<std::int64_t>(owner.minBlockSectionY - 1);
  return index >= 0 && index < owner.blockSectionCount + 2
             ? static_cast<int>(index)
             : -1;
}

SkyLightPropagator::WorkingLayers *
SkyLightPropagator::findWorkingLayers(std::uint64_t chunkKeyValue) const {
  ChunkCacheEntry &entry = chunkCache_[chunkL1Slot(chunkKeyValue)];
  if (entry.key == chunkKeyValue && entry.layersValid)
    return entry.layers;
  const auto found = layersByChunk.find(chunkKeyValue);
  if (found == layersByChunk.end())
    return nullptr;
  WorkingLayers *ptr = found->second.get();
  if (entry.key != chunkKeyValue)
    entry = ChunkCacheEntry{chunkKeyValue, nullptr, ptr, false, true};
  else {
    entry.layers = ptr;
    entry.layersValid = true;
  }
  return ptr;
}

SkyLightPropagator::WorkingLayers &
SkyLightPropagator::workingLayers(std::uint64_t chunkKeyValue) const {
  ChunkCacheEntry &entry = chunkCache_[chunkL1Slot(chunkKeyValue)];
  if (entry.key == chunkKeyValue && entry.layersValid)
    return *entry.layers;
  const auto found = layersByChunk.find(chunkKeyValue);
  WorkingLayers *ptr = nullptr;
  if (found != layersByChunk.end()) {
    ptr = found->second.get();
  } else {
    auto inserted = layersByChunk.try_emplace(
        chunkKeyValue,
        std::make_unique<WorkingLayers>(
            static_cast<std::size_t>(owner.blockSectionCount + 2)));
    ptr = inserted.first->second.get();
  }
  if (entry.key != chunkKeyValue)
    entry = ChunkCacheEntry{chunkKeyValue, nullptr, ptr, false, true};
  else {
    entry.layers = ptr;
    entry.layersValid = true;
  }
  return *ptr;
}

LightValues *SkyLightPropagator::workingLayer(std::uint64_t chunkKeyValue,
                                              int sectionY) {
  return const_cast<LightValues *>(
      std::as_const(*this).workingLayer(chunkKeyValue, sectionY));
}

const LightValues *SkyLightPropagator::workingLayer(std::uint64_t chunkKeyValue,
                                                    int sectionY) const {
  const int index = workingSectionIndex(sectionY);
  if (index < 0)
    return nullptr;
  const WorkingLayers *layers = findWorkingLayers(chunkKeyValue);
  if (!layers)
    return nullptr;
  return layers->sections[static_cast<std::size_t>(index)].get();
}

void SkyLightPropagator::invalidateInheritedBases(
    std::uint64_t chunkKeyValue) const {
  WorkingLayers *layers = findWorkingLayers(chunkKeyValue);
  if (!layers)
    return;
  std::fill(layers->inherited.begin(), layers->inherited.end(),
            kUnknownInheritedSection);
}

LightValues *SkyLightPropagator::insertWorkingLayer(std::uint64_t chunkKeyValue,
                                                    int sectionY,
                                                    LightValues values) {
  const int index = workingSectionIndex(sectionY);
  if (index < 0)
    return nullptr;
  WorkingLayers &layers = workingLayers(chunkKeyValue);
  std::unique_ptr<LightValues> &slot =
      layers.sections[static_cast<std::size_t>(index)];
  if (slot)
    return slot.get();
  slot = std::make_unique<LightValues>(std::move(values));
  invalidateInheritedBases(chunkKeyValue);
  ++sectionCacheGen_;
  return slot.get();
}

void SkyLightPropagator::dropWorkingLayer(std::uint64_t chunkKeyValue,
                                          int sectionY) {
  const int index = workingSectionIndex(sectionY);
  if (index < 0)
    return;
  WorkingLayers *layers = findWorkingLayers(chunkKeyValue);
  if (!layers)
    return;
  auto &slot = layers->sections[static_cast<std::size_t>(index)];
  if (!slot)
    return;
  slot.reset();
  invalidateInheritedBases(chunkKeyValue);
  ++sectionCacheGen_;
}

void SkyLightPropagator::dropAllWorkingLayers(std::uint64_t chunkKeyValue) {
  ChunkCacheEntry &entry = chunkCache_[chunkL1Slot(chunkKeyValue)];
  if (entry.key == chunkKeyValue) {
    entry.layers = nullptr;
    entry.layersValid = false;
  }
  ++sectionCacheGen_;
  layersByChunk.erase(chunkKeyValue);
}

int SkyLightPropagator::inheritedBase(WorkingLayers &layers,
                                      const LightChunkSnapshot *chunk,
                                      int sectionY) const {
  const int index = workingSectionIndex(sectionY);
  if (index < 0)
    return kNoInheritedSection;
  const std::size_t cacheIndex = static_cast<std::size_t>(index);
  if (layers.inherited[cacheIndex] != kUnknownInheritedSection)
    return layers.inherited[cacheIndex];
  int result = kNoInheritedSection;
  for (int aboveY = sectionY + 1;
       aboveY < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
       ++aboveY) {
    const int aboveIndex = workingSectionIndex(aboveY);
    if (aboveIndex < 0)
      continue;
    if (layers.sections[static_cast<std::size_t>(aboveIndex)]) {
      result = aboveY;
      break;
    }
    const LightSectionSnapshot *above = chunk->section(aboveY);
    if (above &&
        !above->layers[static_cast<std::size_t>(LightLayer::Sky)].isMissing()) {
      result = aboveY;
      break;
    }
  }
  layers.inherited[cacheIndex] = result;
  return result;
}

LightValues
SkyLightPropagator::initialWorkingLayer(std::uint64_t chunkKeyValue,
                                        const LightChunkSnapshot *chunk,
                                        int sectionY) const {
  LightValues layer{};
  const LightSectionSnapshot *section = chunk->section(sectionY);
  if (section &&
      !section->layers[static_cast<std::size_t>(LightLayer::Sky)].isMissing())
    return LightValues::fromLayer(
        section->layers[static_cast<std::size_t>(LightLayer::Sky)]);

  WorkingLayers &layers = workingLayers(chunkKeyValue);
  const int baseY = inheritedBase(layers, chunk, sectionY);
  const int baseIndex =
      baseY == kNoInheritedSection ? -1 : workingSectionIndex(baseY);
  const LightValues *workingBase =
      baseIndex < 0
          ? nullptr
          : layers.sections[static_cast<std::size_t>(baseIndex)].get();
  const LightSectionSnapshot *rawBase =
      baseY == kNoInheritedSection ? nullptr : chunk->section(baseY);
  if (workingBase) {
    if (const auto uniform = workingBase->uniformValue()) {
      layer.fill(*uniform);
      return layer;
    }
  } else if (!rawBase ||
             rawBase->layers[static_cast<std::size_t>(LightLayer::Sky)]
                 .isMissing()) {
    layer.fill(15);
    return layer;
  } else if (rawBase->layers[static_cast<std::size_t>(LightLayer::Sky)]
                 .isUniform()) {
    layer.fill(rawBase->layers[static_cast<std::size_t>(LightLayer::Sky)]
                   .uniformValue());
    return layer;
  }
  std::array<std::uint8_t, 256> cols{};
  bool allEqual = true;
  std::uint8_t first = 15;
  for (int z = 0; z < 16; ++z) {
    for (int x = 0; x < 16; ++x) {
      const std::uint8_t inherited =
          workingBase ? (*workingBase)[lightIndex(x, 0, z)]
                      : rawBase->light(LightLayer::Sky, x, 0, z);
      const std::size_t colIndex = static_cast<std::size_t>((z << 4) | x);
      cols[colIndex] = inherited;
      if (colIndex == 0)
        first = inherited;
      else if (inherited != first)
        allEqual = false;
    }
  }
  if (allEqual) {
    layer.fill(first);
    return layer;
  }
  layer.fillInheritedMixed(cols);
  return layer;
}

bool SkyLightPropagator::blockSectionNonEmpty(int x, int y, int z) const {
  const auto key = chunkKey(x, z);
  if (const GeometrySection *geometry = owner.geometrySection(key, y))
    return !geometry->empty;
  const LightChunkSnapshot *chunk = stateFor(key);
  if (!chunk || y < chunk->minBlockSectionY ||
      y >= chunk->minBlockSectionY + chunk->blockSectionCount)
    return false;
  const LightSectionSnapshot *section = chunk->section(y);
  return section && !section->empty;
}

WorkingChunk *SkyLightPropagator::workingChunk(std::uint64_t key) {
  const auto found = working.find(key);
  if (found != working.end())
    return &found->second;
  return ensureWorking(key);
}

const LightChunkSnapshot *
SkyLightPropagator::stateFor(std::uint64_t key) const {
  ChunkCacheEntry &entry = chunkCache_[chunkL1Slot(key)];
  if (entry.key == key && entry.snapshotValid)
    return entry.snapshot;
  const LightChunkSnapshot *result = nullptr;
  if (!removed.contains(key)) {
    const auto current = working.find(key);
    if (current != working.end())
      result = current->second.snapshot.get();
    else {
      const auto &stored = owner.storedLayerSnapshot(LightLayer::Sky, key);
      if (stored)
        result = stored.get();
    }
  }
  if (entry.key != key)
    entry = ChunkCacheEntry{key, result, nullptr, true, false};
  else {
    entry.snapshot = result;
    entry.snapshotValid = true;
  }
  return result;
}

WorkingChunk *SkyLightPropagator::ensureWorking(std::uint64_t key) {
  if (removed.contains(key))
    return nullptr;
  const auto current = working.find(key);
  if (current != working.end())
    return &current->second;
  const auto &stored = owner.storedLayerSnapshot(LightLayer::Sky, key);
  if (!stored)
    return nullptr;
  WorkingChunk next;
  next.key = key;
  next.lifecycleEpoch = stored->lifecycleEpoch;
  next.chunkX = stored->chunkX;
  next.chunkZ = stored->chunkZ;
  next.inPlace = true;
  next.snapshot = stored;
  ChunkCacheEntry &entry = chunkCache_[chunkL1Slot(key)];
  if (entry.key == key)
    entry.snapshotValid = false;
  ++sectionCacheGen_;
  return &working.emplace(key, std::move(next)).first->second;
}

const SkyLightPropagator::SectionCacheEntry &
SkyLightPropagator::lookupSection(LightPos position) const {
  const SectionPos3 section{position.x >> 4, position.y >> 4,
                            position.z >> 4};
  SectionCacheEntry &entry = sectionCache_[sectionL1Slot(section)];
  if (entry.generation == sectionCacheGen_ && entry.section == section)
    return entry;
  const std::uint64_t key = chunkKey(section.x, section.z);
  const LightChunkSnapshot *chunk = stateFor(key);
  WorkingLayers *layers = findWorkingLayers(key);
  const int index = workingSectionIndex(section.y);
  LightValues *values =
      (layers && index >= 0) ? layers->sections[static_cast<std::size_t>(index)].get()
                             : nullptr;
  const LightSectionSnapshot *snapshot =
      chunk ? chunk->section(section.y) : nullptr;
  entry.section = section;
  entry.generation = sectionCacheGen_;
  entry.chunk = chunk;
  entry.snapshot = snapshot;
  entry.layers = layers;
  entry.values = values;
  return entry;
}

const LightSectionSnapshot *
SkyLightPropagator::sectionAt(LightPos position) const {
  if (position.y < (owner.minBlockSectionY - 1) * 16 ||
      position.y >=
          (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16)
    return nullptr;
  return lookupSection(position).snapshot;
}

const BlockLightProperties &
SkyLightPropagator::stateAt(LightPos position) const {
  return owner.blockPropertiesAt(position.x, position.y, position.z,
                                 incomplete);
}

int SkyLightPropagator::storedLevel(LightPos position) const {
  const SectionCacheEntry &entry = lookupSection(position);
  if (entry.values)
    return (*entry.values)[lightIndex(position.x, position.y, position.z)];
  const LightChunkSnapshot *chunk = entry.chunk;
  if (!chunk)
    return 0;
  const LightSectionSnapshot *section = entry.snapshot;
  if (section &&
      !section->layers[static_cast<std::size_t>(LightLayer::Sky)].isMissing())
    return section->light(LightLayer::Sky, position.x & 15, position.y & 15,
                          position.z & 15);
  WorkingLayers &layers =
      entry.layers ? *entry.layers
                   : workingLayers(chunkKey(position.x >> 4, position.z >> 4));
  const int baseY = inheritedBase(layers, chunk, position.y >> 4);
  if (baseY == kNoInheritedSection)
    return 15;
  const int baseIndex = workingSectionIndex(baseY);
  if (const LightValues *layer =
          baseIndex < 0
              ? nullptr
              : layers.sections[static_cast<std::size_t>(baseIndex)].get())
    return (*layer)[lightIndex(position.x, 0, position.z)];
  const LightSectionSnapshot *above = chunk->section(baseY);
  return above &&
                 !above->layers[static_cast<std::size_t>(LightLayer::Sky)]
                      .isMissing()
             ? above->light(LightLayer::Sky, position.x & 15, 0,
                            position.z & 15)
             : 15;
}

bool SkyLightPropagator::eligibleHere(int sectionX, int sectionY,
                                      int sectionZ) {
  const std::uint64_t packed =
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sectionX)) << 32) |
      static_cast<std::uint32_t>(sectionZ);
  if (eligibleValid && eligibleChunkKey == packed &&
      eligibleSectionY == sectionY)
    return eligibleResult;
  eligibleChunkKey = packed;
  eligibleSectionY = sectionY;
  eligibleResult = storedEligible.contains({sectionX, sectionY, sectionZ});
  eligibleValid = true;
  return eligibleResult;
}

LightValues *SkyLightPropagator::bulkValues(std::uint64_t chunkKeyValue,
                                            int sectionY) {
  if (sectionY < owner.minBlockSectionY - 1 ||
      sectionY > owner.minBlockSectionY + owner.blockSectionCount)
    return nullptr;
  if (LightValues *layer = workingLayer(chunkKeyValue, sectionY))
    return layer;
  const int sectionX =
      static_cast<int>(static_cast<std::int32_t>(chunkKeyValue >> 32));
  const int sectionZ =
      static_cast<int>(static_cast<std::uint32_t>(chunkKeyValue));
  if (storageReady && !eligibleHere(sectionX, sectionY, sectionZ))
    return nullptr;
  const LightChunkSnapshot *chunk = stateFor(chunkKeyValue);
  const LightSectionSnapshot *section = chunk ? chunk->section(sectionY) : nullptr;
  if (!section)
    return nullptr;
  return insertWorkingLayer(
      chunkKeyValue, sectionY,
      initialWorkingLayer(chunkKeyValue, chunk, sectionY));
}

bool SkyLightPropagator::setStoredLevel(LightPos position, int level) {
  const SectionCacheEntry &entry = lookupSection(position);
  if (!entry.snapshot)
    return false;
  const int current = storedLevel(position);
  if (current == level)
    return false;
  const SectionPos3 key{position.x >> 4, position.y >> 4, position.z >> 4};
  const std::uint64_t chunkKeyValue = chunkKey(key.x, key.z);
  LightValues *layer = entry.values;
  if (!layer) {
    if (storageReady && !eligibleHere(key.x, key.y, key.z))
      return false;
    const LightChunkSnapshot *chunk = entry.chunk;
    if (!chunk)
      return false;
    layer = insertWorkingLayer(
        chunkKeyValue, key.y, initialWorkingLayer(chunkKeyValue, chunk, key.y));
    if (!layer)
      return false;
  }
  const auto index = lightIndex(position.x, position.y, position.z);
  (*layer)[index] = static_cast<std::uint8_t>(level);
  dirtySections.insert(key);
  return true;
}

void SkyLightPropagator::allocateSection(std::uint64_t chunkKeyValue,
                                         int sectionY) {
  const SectionPos3 key{
      static_cast<int>(chunkKeyValue >> 32), sectionY,
      static_cast<int>(static_cast<std::uint32_t>(chunkKeyValue))};
  if (workingLayer(chunkKeyValue, sectionY))
    return;
  const LightChunkSnapshot *chunk = stateFor(chunkKeyValue);
  if (!chunk)
    return;
  const LightSectionSnapshot *section = chunk->section(sectionY);
  if (!section ||
      !section->layers[static_cast<std::size_t>(LightLayer::Sky)].isMissing())
    return;
  insertWorkingLayer(chunkKeyValue, sectionY,
                     initialWorkingLayer(chunkKeyValue, chunk, sectionY));
  dirtySections.insert(key);
}

const BlockLightProperties &SkyLightPropagator::sourcePropertyAt(
    std::uint64_t key, int x, int y, int z) const {
  const int minY = owner.minBlockSectionY * 16;
  const int maxY = (owner.minBlockSectionY + owner.blockSectionCount) * 16;
  if (y < minY || y >= maxY)
    return emptyLightProperties();
  const GeometrySection *geometry = owner.geometrySection(key, y >> 4);
  if (!geometry)
    return unsupportedLightProperties();
  const std::uint16_t classId =
      geometry->uniform ? geometry->uniformClass
                        : geometry->at(lightIndex(x, y, z));
  return owner.opticalClass(classId).properties;
}

bool SkyLightPropagator::sourceEmptyGeometry(
    const GeometrySection *geometry) const {
  if (!geometry || !geometry->empty || !geometry->uniform)
    return false;
  const auto &properties = owner.opticalClass(geometry->uniformClass).properties;
  return properties.supported && properties.dampening == 0 &&
         !hasLightFaceRect(properties, 0) && !hasLightFaceRect(properties, 1);
}

bool SkyLightPropagator::isSourceEdgeOccluded(
    const BlockLightProperties &top, const BlockLightProperties &bottom) {
  if (bottom.supported && bottom.dampening != 0)
    return true;
  return faceShapesOcclude(top, bottom, 0);
}

int SkyLightPropagator::findSourceBelow(std::uint64_t key, int x, int z,
                                        int topY) const {
  const int minY = owner.minBlockSectionY * 16;
  const int firstBottomY = topY - 1;
  if (firstBottomY < minY)
    return minY - 1;
  const int firstSectionY = firstBottomY >> 4;
  const int bottomSectionY = minY >> 4;
  const BlockLightProperties *topState = &sourcePropertyAt(key, x, topY, z);
  for (int sectionY = firstSectionY; sectionY >= bottomSectionY; --sectionY) {
    const GeometrySection *geometry = owner.geometrySection(key, sectionY);
    const int baseY = sectionY * 16;
    const int localStart = sectionY == firstSectionY ? (firstBottomY & 15) : 15;
    if (sourceEmptyGeometry(geometry)) {
      const auto &empty = emptyLightProperties();
      if (isSourceEdgeOccluded(*topState, empty))
        return baseY + localStart + 1;
      topState = &empty;
      continue;
    }
    if (!geometry)
      return baseY + localStart + 1;
    for (int localY = localStart; localY >= 0; --localY) {
      const auto &bottom = sourcePropertyAt(key, x, baseY + localY, z);
      if (isSourceEdgeOccluded(*topState, bottom))
        return baseY + localY + 1;
      topState = &bottom;
    }
  }
  return minY - 1;
}

int SkyLightPropagator::computeSource(std::uint64_t key, int x, int z) const {
  const int minY = owner.minBlockSectionY * 16;
  const int topSectionY = owner.minBlockSectionY + owner.blockSectionCount - 1;
  const int bottomSectionY = owner.minBlockSectionY;
  const auto &empty = emptyLightProperties();
  const BlockLightProperties *top = &empty;
  for (int sectionY = topSectionY; sectionY >= bottomSectionY; --sectionY) {
    const GeometrySection *geometry = owner.geometrySection(key, sectionY);
    const int baseY = sectionY * 16;
    if (sourceEmptyGeometry(geometry)) {
      if (isSourceEdgeOccluded(*top, empty))
        return baseY + 16;
      top = &empty;
      continue;
    }
    if (!geometry)
      return baseY + 16;
    for (int localY = 15; localY >= 0; --localY) {
      const auto &bottom = sourcePropertyAt(key, x, baseY + localY, z);
      if (isSourceEdgeOccluded(*top, bottom))
        return baseY + localY + 1;
      top = &bottom;
    }
  }
  return minY - 1;
}

SkyColumnSources
SkyLightPropagator::computeSources(std::uint64_t key) const {
  SkyColumnSources result{};
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x)
      result[HeightmapColumnKey(x, z)] = computeSource(key, x, z);
  return result;
}

void SkyLightPropagator::clearLines(WorkingChunk &chunk) {
  const std::size_t sectionCount = chunk.snapshot->sections.size();
  for (std::size_t i = 0; i < sectionCount; ++i) {
    const auto &section = chunk.snapshot->sections[i];
    if (!section)
      continue;
    if (!section->layers[static_cast<std::size_t>(LightLayer::Sky)]
             .isMissing()) {
      if (auto *editable = chunk.sectionForEdit(section->sectionY))
        editable->layers[static_cast<std::size_t>(LightLayer::Sky)] =
            LightLayerSnapshot::missing();
    }
    if (chunk.inPlace)
      chunk.dirtySections.push_back(section->sectionY);
  }
  dropAllWorkingLayers(chunkKey(chunk.chunkX, chunk.chunkZ));
  invalidateInheritedBases(chunkKey(chunk.chunkX, chunk.chunkZ));
}

int SkyLightPropagator::sourceFor(int blockX, int blockZ) {
  const int chunkX = blockX >> 4;
  const int chunkZ = blockZ >> 4;
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  auto found = sourceHeights.find(key);
  if (found == sourceHeights.end()) {
    if (!owner.geometry.contains(key))
      return owner.minBlockSectionY * 16 - 1;
    found = sourceHeights.emplace(key, computeSources(key)).first;
  }
  return found->second[HeightmapColumnKey(blockX & 15, blockZ & 15)];
}

void SkyLightPropagator::seedColumn(int chunkX, int chunkZ, int x, int z,
                                    int source) {
  const int minY = owner.minBlockSectionY * 16;
  const int lightTop =
      (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16;
  const int sentinel = minY - 1;
  const int bx = chunkX * 16 + x;
  const int bz = chunkZ * 16 + z;
  const int north = sourceFor(bx, bz - 1);
  const int south = sourceFor(bx, bz + 1);
  const int west = sourceFor(bx - 1, bz);
  const int east = sourceFor(bx + 1, bz);
  const int neighborHighest = std::max({north, south, west, east});
  const int first = source == sentinel ? minY : source;
  for (int y = lightTop - 1; y >= first; --y) {
    if (y != source && y >= neighborHighest)
      continue;
    std::uint8_t directions = 0;
    if (y == source)
      directions |= 1u << 0;
    if (y < north)
      directions |= 1u << 2;
    if (y < south)
      directions |= 1u << 3;
    if (y < west)
      directions |= 1u << 4;
    if (y < east)
      directions |= 1u << 5;
    if (directions)
      enqueueIncrease({bx, y, bz}, 15, directions, false, true);
  }
}

bool SkyLightPropagator::isSource(LightPos position) {
  return position.y >= sourceFor(position.x, position.z);
}

void SkyLightPropagator::checkNode(LightPos position) {
  if (!sectionAt(position))
    return;
  if (isSource(position)) {
    enqueueDecrease(position, 15, 63u & ~(1u << 1));
    enqueueIncrease(position, 15, 63u & ~(1u << 1), false, true);
  } else {
    const int oldLevel = storedLevel(position);
    if (oldLevel > 0) {
      setStoredLevel(position, 0);
      enqueueDecrease(position, static_cast<std::uint8_t>(oldLevel), 63);
    } else {
      enqueueDecrease(position, 1, 63);
    }
  }
}

void SkyLightPropagator::propagateIncrease(const IncreaseTask &task) {
  int fromLevel = storedLevel(task.position);
  if (task.fromEmission) {
    if (fromLevel < task.level) {
      setStoredLevel(task.position, task.level);
      fromLevel = task.level;
    }
  }
  if (fromLevel != task.level)
    return;
  const int emptySectionsBelow =
      countEmptySectionsBelowIfAtBorder(task.position);
  const auto &fromQueued =
      task.fromEmpty ? emptyLightProperties() : stateAt(task.position);
  for (int direction = 0; direction < 6; ++direction) {
    if ((task.directions & (1u << direction)) == 0)
      continue;
    const LightPos to = offset(task.position, direction);
    if (!sectionAt(to)) {
      if ((to.x >> 4) != (task.position.x >> 4) ||
          (to.z >> 4) != (task.position.z >> 4))
        ++owner.unloadedNeighborSkips;
      continue;
    }
    const int toLevel = storedLevel(to);
    if (fromLevel - 1 <= toLevel)
      continue;
    const auto &toState = stateAt(to);
    const int newLevel = fromLevel - lightOpacity(toState);
    if (newLevel <= toLevel || faceShapesOcclude(fromQueued, toState, direction))
      continue;
    if (!setStoredLevel(to, newLevel))
      continue;
    if (newLevel > 1)
      enqueueIncrease(to, static_cast<std::uint8_t>(newLevel),
                      static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))),
                      emptyShape(toState), false);
    propagateFromEmptySections(to, direction, newLevel, true,
                               emptySectionsBelow);
  }
}

void SkyLightPropagator::propagateDecrease(const DecreaseTask &task) {
  const int emptySectionsBelow =
      countEmptySectionsBelowIfAtBorder(task.position);
  for (int direction = 0; direction < 6; ++direction) {
    if ((task.directions & (1u << direction)) == 0)
      continue;
    const LightPos to = offset(task.position, direction);
    if (!sectionAt(to)) {
      if ((to.x >> 4) != (task.position.x >> 4) ||
          (to.z >> 4) != (task.position.z >> 4))
        ++owner.unloadedNeighborSkips;
      continue;
    }
    if (isSource(to)) {
      enqueueIncrease(to, 15, 63, false, true);
      continue;
    }
    const int toLevel = storedLevel(to);
    if (toLevel == 0)
      continue;
    if (toLevel <= int(task.level) - 1) {
      if (!setStoredLevel(to, 0))
        continue;
      enqueueDecrease(to, static_cast<std::uint8_t>(toLevel),
                      static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))));
      propagateFromEmptySections(to, direction, toLevel, false,
                                 emptySectionsBelow);
    } else {
      enqueueIncrease(to, static_cast<std::uint8_t>(toLevel),
                      static_cast<std::uint8_t>(1u << (direction ^ 1)), false,
                      false);
    }
  }
}

int SkyLightPropagator::countEmptySectionsBelowIfAtBorder(
    LightPos position) const {
  if ((position.y & 15) != 0)
    return 0;
  const int localX = position.x & 15;
  const int localZ = position.z & 15;
  if (localX != 0 && localX != 15 && localZ != 0 && localZ != 15)
    return 0;
  const int sectionX = position.x >> 4;
  const int sectionZ = position.z >> 4;
  int sectionY = position.y >> 4;
  int count = 0;
  while (sectionY - count - 1 >= owner.minBlockSectionY - 1) {
    const SectionPos3 key{sectionX, sectionY - count - 1, sectionZ};
    if (storedEligible.contains(key))
      break;
    ++count;
  }
  return count;
}

bool SkyLightPropagator::crossedSkySectionEdge(int direction, int localX,
                                               int localZ) {
  if (direction == 2)
    return localZ == 15;
  if (direction == 3)
    return localZ == 0;
  if (direction == 4)
    return localX == 15;
  if (direction == 5)
    return localX == 0;
  return false;
}

void SkyLightPropagator::propagateFromEmptySections(LightPos to, int direction,
                                                    int level, bool increase,
                                                    int emptySectionsBelow) {
  if (emptySectionsBelow == 0 ||
      !crossedSkySectionEdge(direction, to.x & 15, to.z & 15))
    return;
  const int sectionX = to.x >> 4;
  const int sectionZ = to.z >> 4;
  int sectionY = (to.y >> 4) - 1;
  const int bottomSectionY = sectionY - emptySectionsBelow + 1;
  while (sectionY >= bottomSectionY) {
    const SectionPos3 key{sectionX, sectionY, sectionZ};
    if (!storedEligible.contains(key)) {
      --sectionY;
      continue;
    }
    for (int localY = 15; localY >= 0; --localY) {
      const LightPos position{to.x, sectionY * 16 + localY, to.z};
      if (increase) {
        if (setStoredLevel(position, level) && level > 1)
          enqueueIncrease(
              position, static_cast<std::uint8_t>(level),
              static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))), true,
              false);
      } else {
        if (storedLevel(position) == 0)
          continue;
        if (setStoredLevel(position, 0))
          enqueueDecrease(
              position, static_cast<std::uint8_t>(level),
              static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))));
      }
    }
    --sectionY;
  }
}

void SkyLightPropagator::enqueueIncrease(LightPos position, std::uint8_t level,
                                         std::uint8_t directions,
                                         bool fromEmpty, bool source) {
  if (level == 0)
    return;
  IncreaseTask task{position, level, directions, fromEmpty, source};
  if (activeIncreaseSection && sectionOf(position) == *activeIncreaseSection)
    currentIncreaseQueue.push(std::move(task));
  else
    increaseQueue.push(std::move(task));
}

void SkyLightPropagator::enqueueDecrease(LightPos position, std::uint8_t level,
                                         std::uint8_t directions) {
  DecreaseTask task{position, level, directions};
  if (activeDecreaseSection && sectionOf(position) == *activeDecreaseSection)
    currentDecreaseQueue.push(std::move(task));
  else
    decreaseQueue.push(std::move(task));
}

} // namespace redstone::light_internal
