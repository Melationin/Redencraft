#include "light/block_light_engine.hpp"

#include <algorithm>
#include <bit>
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
  if (direction == 0 || direction == 1) {
    const int y = direction == 0 ? minY : maxY - 1;
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        visitor(LightPos{minX + x, y, minZ + z});
  } else if (direction == 2 || direction == 3) {
    const int z = direction == 2 ? minZ : minZ + 15;
    for (int y = minY; y < maxY; ++y)
      for (int x = 0; x < 16; ++x)
        visitor(LightPos{minX + x, y, z});
  } else {
    const int x = direction == 4 ? minX : minX + 15;
    for (int y = minY; y < maxY; ++y)
      for (int z = 0; z < 16; ++z)
        visitor(LightPos{x, y, minZ + z});
  }
}

} // namespace

BlockLightPropagator::BlockLightPropagator(
    LightEngineHost &light,
    FlatMap<std::uint64_t, WorkingChunk, UInt64Hash> &workingChunks,
    const FlatSet<std::uint64_t, UInt64Hash> &removedChunks,
    BlockPropagatorScratch &scratchBuffers)
    : owner(light), working(workingChunks), removed(removedChunks),
      checks(scratchBuffers.checks), checkQueue(scratchBuffers.checkQueue),
      decreaseQueue(scratchBuffers.decreaseQueue),
      increaseQueue(scratchBuffers.increaseQueue),
      currentDecreaseQueue(scratchBuffers.currentDecreaseQueue),
      currentIncreaseQueue(scratchBuffers.currentIncreaseQueue),
      lightValues(scratchBuffers.lightValues),
      dirtySectionMasks(scratchBuffers.dirtySectionMasks),
      dirtySectionsOverflow(scratchBuffers.dirtySectionsOverflow) {
  scratchBuffers.clear();
}

void BlockLightPropagator::enqueueCheck(LightPos position) {
  if (checks.insert(position).second)
    checkQueue.push(position);
}

void BlockLightPropagator::seedChunk(std::uint64_t key) {
  const LightChunkSnapshot *chunk = stateFor(key);
  if (!chunk)
    return;
  for (int sectionY = chunk->minLightSectionY();
       sectionY < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
       ++sectionY) {
    const LightSectionSnapshot *data = chunk->section(sectionY);
    if (!data)
      continue;
    const auto &blockLayer =
        data->layers[static_cast<std::size_t>(LightLayer::Block)];
    const bool hasStoredLight =
        !blockLayer.isMissing() &&
        !(blockLayer.isUniform() && blockLayer.uniformValue() == 0);
    const bool inBlockRange =
        sectionY >= chunk->minBlockSectionY &&
        sectionY < chunk->minBlockSectionY + chunk->blockSectionCount;
    if (!inBlockRange) {
      if (!hasStoredLight)
        continue;
      const auto &empty = emptyLightProperties();
      for (int index = 0; index < kLightVolume; ++index) {
        const int x = index & 15;
        const int z = (index >> 4) & 15;
        const int y = (index >> 8) & 15;
        const std::uint8_t stored = data->light(LightLayer::Block, x, y, z);
        if (stored > 0)
          enqueueIncrease({chunk->chunkX * 16 + x, sectionY * 16 + y,
                           chunk->chunkZ * 16 + z},
                          stored, 63, emptyShape(empty), false);
      }
      continue;
    }

    const std::optional<LightSectionView> direct =
        owner.directSectionView(key, sectionY);
    if (!direct) {
      incomplete = true;
      if (!hasStoredLight)
        continue;
      for (int index = 0; index < kLightVolume; ++index) {
        const int x = index & 15;
        const int z = (index >> 4) & 15;
        const int y = (index >> 8) & 15;
        const std::uint8_t stored = data->light(LightLayer::Block, x, y, z);
        if (stored > 0)
          enqueueIncrease({chunk->chunkX * 16 + x, sectionY * 16 + y,
                           chunk->chunkZ * 16 + z},
                          stored, 63, true, false);
      }
      continue;
    }

    if (direct->bits == 0) {
      const BlockLightProperties &props = owner.propertiesForState(direct->single);
      if (!props.supported)
        incomplete = true;
      const bool emits = props.supported && props.emission > 0;
      if (!emits && !hasStoredLight)
        continue;
      const bool isEmptyShape = emptyShape(props);
      for (int index = 0; index < kLightVolume; ++index) {
        const int x = index & 15;
        const int z = (index >> 4) & 15;
        const int y = (index >> 8) & 15;
        const LightPos position{chunk->chunkX * 16 + x, sectionY * 16 + y,
                                chunk->chunkZ * 16 + z};
        if (emits)
          enqueueIncrease(position, props.emission, 63, isEmptyShape, true);
        if (hasStoredLight) {
          const std::uint8_t stored = data->light(LightLayer::Block, x, y, z);
          if (stored > 0)
            enqueueIncrease(position, stored, 63, isEmptyShape, false);
        }
      }
      continue;
    }

    if (direct->empty && !hasStoredLight)
      continue;

    bool anyEmission = false;
    bool anyUnsupported = false;
    for (BlockState::Id sid : direct->palette) {
      const auto &props = owner.propertiesForState(sid);
      if (!props.supported)
        anyUnsupported = true;
      else if (props.emission > 0)
        anyEmission = true;
      if (anyEmission && anyUnsupported)
        break;
    }
    if (!anyEmission && !anyUnsupported && !hasStoredLight)
      continue;

    for (int index = 0; index < kLightVolume; ++index) {
      const int x = index & 15;
      const int z = (index >> 4) & 15;
      const int y = (index >> 8) & 15;
      const BlockLightProperties &properties = owner.propertiesForState(
          direct->atomicStateAt(static_cast<std::size_t>(index)));
      if (!properties.supported)
        incomplete = true;
      const LightPos position{chunk->chunkX * 16 + x, sectionY * 16 + y,
                              chunk->chunkZ * 16 + z};
      if (properties.supported && properties.emission > 0)
        enqueueIncrease(position, properties.emission, 63,
                        emptyShape(properties), true);
      if (hasStoredLight) {
        const std::uint8_t stored = data->light(LightLayer::Block, x, y, z);
        if (stored > 0)
          enqueueIncrease(position, stored, 63, emptyShape(properties), false);
      }
    }
  }
}

void BlockLightPropagator::seedIncoming(std::uint64_t key,
                                        std::uint8_t directionMask) {
  const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
  const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
  const int minLightY = owner.minBlockSectionY - 1;
  const int maxLightY = owner.minBlockSectionY + owner.blockSectionCount;
  for (int direction = 2; direction < 6; ++direction) {
    if ((directionMask & (1u << direction)) == 0)
      continue;
    const LightPos delta = kDirectionOffsets[direction];
    const std::uint64_t neighborKey =
        chunkKey(chunkX + delta.x, chunkZ + delta.z);
    const LightChunkSnapshot *neighbor = stateFor(neighborKey);
    if (!neighbor)
      continue;
    const int nChunkX = neighbor->chunkX;
    const int nChunkZ = neighbor->chunkZ;
    const int nMinX = nChunkX * 16;
    const int nMinZ = nChunkZ * 16;
    const int faceDir = direction ^ 1;
    const std::uint8_t outMask = static_cast<std::uint8_t>(1u << faceDir);
    for (int sectionY = minLightY; sectionY <= maxLightY; ++sectionY) {
      const LightSectionSnapshot *sec = neighbor->section(sectionY);
      if (!sec)
        continue;
      const auto lvIt =
          lightValues.find(SectionPos3{nChunkX, sectionY, nChunkZ});
      const LightValues *lv =
          (lvIt != lightValues.end()) ? &lvIt->second : nullptr;
      const auto &layer =
          sec->layers[static_cast<std::size_t>(LightLayer::Block)];
      if (!lv && (layer.isMissing() ||
                  (layer.isUniform() && layer.uniformValue() <= 1)))
        continue;
      const int baseY = sectionY * 16;
      if (faceDir == 2 || faceDir == 3) {
        const int localZ = (faceDir == 2) ? 0 : 15;
        const int worldZ = nMinZ + localZ;
        for (int y = 0; y < 16; ++y) {
          for (int x = 0; x < 16; ++x) {
            const int level =
                lv ? (*lv)[lightIndex(x, y, localZ)]
                   : layer.get(x, y, localZ);
            if (level > 1) {
              const LightPos pos{nMinX + x, baseY + y, worldZ};
              enqueueIncrease(pos, static_cast<std::uint8_t>(level), outMask,
                              emptyShape(stateAt(pos)), false);
            }
          }
        }
      } else {
        const int localX = (faceDir == 4) ? 0 : 15;
        const int worldX = nMinX + localX;
        for (int y = 0; y < 16; ++y) {
          for (int z = 0; z < 16; ++z) {
            const int level =
                lv ? (*lv)[lightIndex(localX, y, z)]
                   : layer.get(localX, y, z);
            if (level > 1) {
              const LightPos pos{worldX, baseY + y, nMinZ + z};
              enqueueIncrease(pos, static_cast<std::uint8_t>(level), outMask,
                              emptyShape(stateAt(pos)), false);
            }
          }
        }
      }
    }
  }
}

void BlockLightPropagator::seedOutgoing(std::uint64_t key,
                                        std::uint8_t directionMask) {
  const LightChunkSnapshot *chunk = stateFor(key);
  if (!chunk)
    return;
  for (int direction = 2; direction < 6; ++direction) {
    if ((directionMask & (1u << direction)) == 0)
      continue;
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
      const auto &properties = stateAt(position);
      enqueueIncrease(position, static_cast<std::uint8_t>(level),
                      static_cast<std::uint8_t>(1u << direction),
                      emptyShape(properties), false);
    });
  }
}

void BlockLightPropagator::clearIncomingFrom(std::uint64_t removedKey) {
  const int chunkX = static_cast<int>(removedKey >> 32);
  const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(removedKey));
  for (int direction = 2; direction < 6; ++direction) {
    const LightPos delta = kDirectionOffsets[direction];
    const std::uint64_t neighborKey =
        chunkKey(chunkX + delta.x, chunkZ + delta.z);
    const LightChunkSnapshot *neighbor = stateFor(neighborKey);
    if (!neighbor)
      continue;
    forEachFaceCell(owner, neighbor, direction ^ 1, [&](LightPos position) {
      enqueueCheck(position);
    });
  }
}

void BlockLightPropagator::run() {
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

void BlockLightPropagator::commitLayers() {
  const auto commitOne = [&](const SectionPos3 &position, std::uint64_t key) {
    WorkingChunk *chunk = ensureWorking(key);
    if (!chunk)
      return;
    const LightSectionSnapshot *current = chunk->snapshot->section(position.y);
    if (!current)
      return;
    const auto found = lightValues.find(position);
    if (found == lightValues.end())
      return;
    const LightValues &values = found->second;
    const auto uniform = values.uniformValue();
    const LightLayerRef payload =
        uniform && values.ref().isMixed()
            ? LightLayerRef(LightLayerSnapshot::uniform(*uniform))
            : values.ref();
    if (!current->layers[static_cast<std::size_t>(LightLayer::Block)]
             .samePayload(payload)) {
      if (LightSectionSnapshot *section = chunk->sectionForEdit(position.y))
        section->layers[static_cast<std::size_t>(LightLayer::Block)] = payload;
    }
    chunk->dirtySections.push_back(position.y);
    chunk->dirty = true;
    chunk->propagated = true;
  };
  const int minLightY = owner.minBlockSectionY - 1;
  for (const auto &[key, mask] : dirtySectionMasks) {
    const int chunkX = static_cast<int>(static_cast<std::int32_t>(key >> 32));
    const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
    std::uint64_t m = mask;
    while (m != 0) {
      const int idx = std::countr_zero(m);
      m &= m - 1;
      commitOne({chunkX, minLightY + idx, chunkZ}, key);
    }
  }
  for (const SectionPos3 &position : dirtySectionsOverflow)
    commitOne(position, chunkKey(position.x, position.z));
}

const LightChunkSnapshot *
BlockLightPropagator::stateFor(std::uint64_t key) const {
  const std::size_t slot = chunkL1Slot(key);
  ChunkStateCacheEntry &entry = chunkStateCache_[slot];
  if (entry.valid && entry.key == key)
    return entry.snapshot;
  const LightChunkSnapshot *result = nullptr;
  if (!removed.contains(key)) {
    const auto workingChunk = working.find(key);
    if (workingChunk != working.end()) {
      result = workingChunk->second.snapshot.get();
    } else {
      const auto &stored = owner.storedLayerSnapshot(LightLayer::Block, key);
      result = stored ? stored.get() : nullptr;
    }
  }
  entry.key = key;
  entry.snapshot = result;
  entry.valid = true;
  return result;
}

const LightChunkSnapshot *BlockLightPropagator::stateFor(int chunkX,
                                                         int chunkZ) const {
  return stateFor(chunkKey(chunkX, chunkZ));
}

WorkingChunk *BlockLightPropagator::ensureWorking(std::uint64_t key) {
  if (removed.contains(key))
    return nullptr;
  const auto found = working.find(key);
  if (found != working.end())
    return &found->second;
  const auto &stored = owner.storedLayerSnapshot(LightLayer::Block, key);
  if (!stored)
    return nullptr;
  WorkingChunk next;
  next.key = key;
  next.lifecycleEpoch = stored->lifecycleEpoch;
  next.chunkX = stored->chunkX;
  next.chunkZ = stored->chunkZ;
  next.inPlace = true;
  next.snapshot = stored;
  auto inserted = working.emplace(key, std::move(next)).first;
  chunkStateCache_[chunkL1Slot(key)].valid = false;
  ++snapshotCacheEpoch_;
  return &inserted->second;
}

const BlockLightPropagator::SectionCacheEntry &
BlockLightPropagator::lookupSection(LightPos position) const {
  static const SectionCacheEntry kOutOfRange{};
  if (position.y < (owner.minBlockSectionY - 1) * 16 ||
      position.y >=
          (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16)
    return kOutOfRange;
  const SectionPos3 sec{position.x >> 4, position.y >> 4, position.z >> 4};
  SectionCacheEntry &entry = sectionCache_[sectionL1Slot(sec)];
  const std::uint64_t epoch = cacheEpoch();
  if (entry.epoch == epoch && entry.section == sec)
    return entry;
  const LightChunkSnapshot *chunk = stateFor(sec.x, sec.z);
  const LightSectionSnapshot *snapshot = chunk ? chunk->section(sec.y) : nullptr;
  const auto found = lightValues.find(sec);
  entry.section = sec;
  entry.epoch = epoch;
  entry.snapshot = snapshot;
  entry.values = found == lightValues.end()
                     ? nullptr
                     : const_cast<LightValues *>(&found->second);
  return entry;
}

const BlockLightPropagator::SectionCacheEntry &
BlockLightPropagator::lookupNeighborSection(
    const SectionCacheEntry &fromEntry, SectionPos3 fromSec,
    LightPos to) const {
  if ((to.x >> 4) == fromSec.x && (to.y >> 4) == fromSec.y &&
      (to.z >> 4) == fromSec.z)
    return fromEntry;
  return lookupSection(to);
}

const LightSectionSnapshot *
BlockLightPropagator::sectionAt(LightPos position) const {
  return lookupSection(position).snapshot;
}

const BlockLightProperties &
BlockLightPropagator::stateAt(LightPos position) const {
  return owner.blockPropertiesAt(position.x, position.y, position.z,
                                 incomplete);
}

int BlockLightPropagator::storedLevelInEntry(const SectionCacheEntry &entry,
                                             LightPos position) const {
  if (!entry.snapshot)
    return 0;
  if (entry.values)
    return (*entry.values)[lightIndex(position.x, position.y, position.z)];
  return entry.snapshot->light(LightLayer::Block, position.x & 15,
                               position.y & 15, position.z & 15);
}

int BlockLightPropagator::storedLevel(LightPos position) const {
  return storedLevelInEntry(lookupSection(position), position);
}

void BlockLightPropagator::markSectionDirty(SectionPos3 section) {
  const int idx = section.y - (owner.minBlockSectionY - 1);
  if (idx >= 0 && idx < 64) {
    const std::uint64_t cKey = chunkKey(section.x, section.z);
    if (lastDirtyEpoch_ != dirtySectionMasks.rehashEpoch() ||
        lastDirtyKey_ != cKey || !lastDirtyMask_) {
      lastDirtyMask_ = &dirtySectionMasks[cKey];
      lastDirtyKey_ = cKey;
      lastDirtyEpoch_ = dirtySectionMasks.rehashEpoch();
    }
    *lastDirtyMask_ |= (std::uint64_t{1} << idx);
  } else {
    dirtySectionsOverflow.insert(section);
  }
}

bool BlockLightPropagator::setStoredLevelInEntry(const SectionCacheEntry &entry,
                                                 LightPos position, int level,
                                                 bool &insertedSection) {
  insertedSection = false;
  if (!entry.snapshot)
    return false;
  const SectionPos3 key{position.x >> 4, position.y >> 4, position.z >> 4};
  LightValues *values = entry.values;
  if (!values) {
    auto inserted = lightValues.try_emplace(
        key, LightValues::fromLayer(
                 entry.snapshot->layers[static_cast<std::size_t>(
                     LightLayer::Block)]));
    values = &inserted.first->second;
    SectionCacheEntry &slot = sectionCache_[sectionL1Slot(key)];
    slot.section = key;
    slot.epoch = cacheEpoch();
    slot.snapshot = entry.snapshot;
    slot.values = values;
    insertedSection = true;
  }
  const auto index = lightIndex(position.x, position.y, position.z);
  if ((*values)[index] == level)
    return false;
  (*values)[index] = static_cast<std::uint8_t>(level);
  markSectionDirty(key);
  return true;
}

bool BlockLightPropagator::setStoredLevel(LightPos position, int level) {
  bool insertedSection = false;
  return setStoredLevelInEntry(lookupSection(position), position, level,
                               insertedSection);
}

void BlockLightPropagator::checkNode(LightPos position) {
  if (!sectionAt(position))
    return;
  const auto &properties = stateAt(position);
  const int emission = properties.supported ? properties.emission : 0;
  const int oldLevel = storedLevel(position);
  if (emission < oldLevel) {
    setStoredLevel(position, 0);
    enqueueDecrease(position, static_cast<std::uint8_t>(oldLevel), 63);
  } else {
    enqueueDecrease(position, 1, 63);
  }
  if (emission > 0)
    enqueueIncrease(position, static_cast<std::uint8_t>(emission), 63,
                    emptyShape(properties), true);
}

void BlockLightPropagator::propagateIncrease(const IncreaseTask &task) {
  const SectionPos3 fromSec = sectionOf(task.position);
  const SectionCacheEntry *fromEntry = &lookupSection(task.position);
  int fromLevel = storedLevelInEntry(*fromEntry, task.position);
  const int targetLevel = task.level;
  if (task.fromEmission) {
    if (fromLevel < targetLevel) {
      bool insertedSection = false;
      setStoredLevelInEntry(*fromEntry, task.position, targetLevel,
                            insertedSection);
      if (insertedSection)
        fromEntry = &lookupSection(task.position);
      fromLevel = targetLevel;
    }
  }
  // LightEngine.propagateIncreases reads the current stored level when the
  // task leaves the queue. A task seeded before a decrease must not carry
  // its old level into a neighbor after the source has already gone dark.
  if (fromLevel != targetLevel)
    return;
  const auto &fromQueued =
      task.fromEmpty ? emptyLightProperties() : stateAt(task.position);
  for (int direction = 0; direction < 6; ++direction) {
    if ((task.directions & (1u << direction)) == 0)
      continue;
    const LightPos to = offset(task.position, direction);
    const SectionCacheEntry &toEntry =
        lookupNeighborSection(*fromEntry, fromSec, to);
    if (!toEntry.snapshot) {
      if ((to.x >> 4) != fromSec.x || (to.z >> 4) != fromSec.z) {
        // 目标区块还没算过（“未计算”）：把它记进收件箱，等它计算时再应用，
        // 而不是丢掉这次传播后靠 seedIncoming 重新推导。
        const auto &toState = stateAt(to);
        const int incoming = fromLevel - lightOpacity(toState);
        if (incoming > 0 &&
            owner.deferIncoming(false, to, direction,
                                static_cast<std::uint8_t>(incoming)))
          continue;
        ++owner.unloadedNeighborSkips;
      }
      continue;
    }
    const int toLevel = storedLevelInEntry(toEntry, to);
    if (fromLevel - 1 <= toLevel)
      continue;
    const auto &toState = stateAt(to);
    const int newLevel = fromLevel - lightOpacity(toState);
    if (newLevel <= toLevel)
      continue;
    if (faceShapesOcclude(fromQueued, toState, direction))
      continue;
    bool insertedSection = false;
    setStoredLevelInEntry(toEntry, to, newLevel, insertedSection);
    if (insertedSection)
      fromEntry = &lookupSection(task.position);
    if (newLevel > 1) {
      enqueueIncrease(to, static_cast<std::uint8_t>(newLevel),
                      static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))),
                      emptyShape(toState), false);
    }
  }
}

void BlockLightPropagator::propagateDecrease(const DecreaseTask &task) {
  const SectionPos3 fromSec = sectionOf(task.position);
  const SectionCacheEntry *fromEntry = &lookupSection(task.position);
  for (int direction = 0; direction < 6; ++direction) {
    if ((task.directions & (1u << direction)) == 0)
      continue;
    const LightPos to = offset(task.position, direction);
    const SectionCacheEntry &toEntry =
        lookupNeighborSection(*fromEntry, fromSec, to);
    if (!toEntry.snapshot)
      continue;
    const int toLevel = storedLevelInEntry(toEntry, to);
    if (toLevel == 0)
      continue;
    if (toLevel <= int(task.level) - 1) {
      const auto &toState = stateAt(to);
      const int emission = toState.supported ? toState.emission : 0;
      bool insertedSection = false;
      setStoredLevelInEntry(toEntry, to, 0, insertedSection);
      if (insertedSection)
        fromEntry = &lookupSection(task.position);
      if (emission < toLevel)
        enqueueDecrease(
            to, static_cast<std::uint8_t>(toLevel),
            static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))));
      if (emission > 0)
        enqueueIncrease(to, static_cast<std::uint8_t>(emission), 63,
                        emptyShape(toState), true);
    } else {
      enqueueIncrease(to, static_cast<std::uint8_t>(toLevel),
                      static_cast<std::uint8_t>(1u << (direction ^ 1)), false,
                      false);
    }
  }
}

void BlockLightPropagator::applyDeferred(LightPos position, std::uint8_t level) {
  if (!sectionAt(position))
    return;
  if (storedLevel(position) >= static_cast<int>(level))
    return;
  setStoredLevel(position, level);
  enqueueIncrease(position, level, 63, emptyShape(stateAt(position)), false);
}

void BlockLightPropagator::enqueueIncrease(LightPos position, std::uint8_t level,
                                           std::uint8_t directions,
                                           bool fromEmpty, bool fromEmission) {
  if (level == 0)
    return;
  IncreaseTask task{position, level, directions, fromEmpty, fromEmission};
  if (activeIncreaseSection && sectionOf(position) == *activeIncreaseSection)
    currentIncreaseQueue.push(std::move(task));
  else
    increaseQueue.push(std::move(task));
}

void BlockLightPropagator::enqueueDecrease(LightPos position, std::uint8_t level,
                                           std::uint8_t directions) {
  DecreaseTask task{position, level, directions};
  if (activeDecreaseSection && sectionOf(position) == *activeDecreaseSection)
    currentDecreaseQueue.push(std::move(task));
  else
    decreaseQueue.push(std::move(task));
}

} // namespace redstone::light_internal
