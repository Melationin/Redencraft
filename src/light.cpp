#include "redstone/light.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace redstone {
namespace {

struct OfficialLightState {
  std::string_view key;
  std::uint8_t emission;
  std::uint8_t dampening;
  bool canOcclude;
  bool useShapeForLightOcclusion;
  bool propagatesSkylightDown;
};

struct LightFaceRect {
  std::int32_t u0 = 0;
  std::int32_t u1 = 0;
  std::int32_t v0 = 0;
  std::int32_t v1 = 0;
  bool operator==(const LightFaceRect &) const = default;
};

struct LightFaceShape {
  LightFaceRect down{};
  LightFaceRect up{};
  LightFaceRect north{};
  LightFaceRect south{};
  LightFaceRect west{};
  LightFaceRect east{};
  std::uint8_t present = 0;
  bool operator==(const LightFaceShape &) const = default;
};

struct OfficialLightFaceState {
  std::string_view key;
  std::uint16_t shape;
};

#include "light_metadata.inc"
#include "light_occlusion.inc"

const BlockLightProperties kUnsupportedLightProperties{};

std::uint64_t chunkKey(int x, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
         static_cast<std::uint32_t>(z);
}

bool validLightValue(std::uint8_t value) { return value <= 15; }

constexpr int kFaceScale = 1'000'000;

bool emptyShape(const BlockLightProperties &properties) {
  return !properties.supported || !properties.canOcclude ||
         !properties.useShapeForLightOcclusion;
}

const LightFaceRect *faceRect(const BlockLightProperties &properties,
                              int direction) {
  if (properties.occlusionShapeId >= std::size(kLightFaceShapes))
    return nullptr;
  const LightFaceShape &shape = kLightFaceShapes[properties.occlusionShapeId];
  if ((shape.present & (1u << direction)) == 0)
    return nullptr;
  switch (direction) {
  case 0: return &shape.down;
  case 1: return &shape.up;
  case 2: return &shape.north;
  case 3: return &shape.south;
  case 4: return &shape.west;
  default: return &shape.east;
  }
}

bool faceUnionCovers(const LightFaceRect *first, const LightFaceRect *second) {
  std::array<const LightFaceRect *, 2> rectangles{first, second};
  std::array<int, 6> xs{};
  std::size_t count = 2;
  xs[0] = 0;
  xs[1] = kFaceScale;
  for (const LightFaceRect *rectangle : rectangles) {
    if (!rectangle)
      continue;
    xs[count++] = std::clamp(rectangle->u0, 0, kFaceScale);
    xs[count++] = std::clamp(rectangle->u1, 0, kFaceScale);
  }
  std::sort(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(count));
  count = static_cast<std::size_t>(
      std::unique(xs.begin(), xs.begin() + static_cast<std::ptrdiff_t>(count)) -
      xs.begin());
  for (std::size_t i = 1; i < count; ++i) {
    if (xs[i - 1] == xs[i])
      continue;
    const int midpoint = (xs[i - 1] + xs[i]) / 2;
    std::array<std::pair<int, int>, 2> intervals{};
    std::size_t intervalsCount = 0;
    for (const LightFaceRect *rectangle : rectangles) {
      if (!rectangle || midpoint < rectangle->u0 || midpoint >= rectangle->u1)
        continue;
      intervals[intervalsCount++] = {
          std::max(0, rectangle->v0), std::min(kFaceScale, rectangle->v1)};
    }
    std::sort(intervals.begin(),
              intervals.begin() + static_cast<std::ptrdiff_t>(intervalsCount));
    int covered = 0;
    for (std::size_t j = 0; j < intervalsCount; ++j) {
      if (intervals[j].first > covered)
        return false;
      covered = std::max(covered, intervals[j].second);
    }
    if (covered < kFaceScale)
      return false;
  }
  return true;
}

bool faceShapesOcclude(const BlockLightProperties &from,
                       const BlockLightProperties &to, int direction) {
  if (!from.supported || !to.supported)
    return true;
  const LightFaceRect *first = faceRect(from, direction);
  const LightFaceRect *second = faceRect(to, direction ^ 1);
  if (!first && !second)
    return false;
  return faceUnionCovers(first, second);
}

int lightOpacity(const BlockLightProperties &properties) {
  return properties.supported ? std::max(1, int(properties.dampening)) : 16;
}

const BlockLightProperties &emptyLightProperties() {
  static const BlockLightProperties value = [] {
    BlockLightProperties properties;
    properties.supported = true;
    return properties;
  }();
  return value;
}

struct LightPos {
  int x = 0;
  int y = 0;
  int z = 0;
  bool operator==(const LightPos &) const = default;
};

struct LightPosHash {
  std::size_t operator()(const LightPos &position) const noexcept {
    std::uint64_t value = static_cast<std::uint32_t>(position.x);
    value = value * 0x9E3779B185EBCA87ull ^
            static_cast<std::uint32_t>(position.y);
    value = value * 0xC2B2AE3D27D4EB4Full ^
            static_cast<std::uint32_t>(position.z);
    return static_cast<std::size_t>(value ^ (value >> 32));
  }
};

struct SectionPos3 {
  int x = 0;
  int y = 0;
  int z = 0;
  bool operator==(const SectionPos3 &) const = default;
};

struct SectionPos3Hash {
  std::size_t operator()(const SectionPos3 &position) const noexcept {
    std::uint64_t value = static_cast<std::uint32_t>(position.x);
    value = value * 0x9E3779B185EBCA87ull ^
            static_cast<std::uint32_t>(position.y);
    value = value * 0xC2B2AE3D27D4EB4Full ^
            static_cast<std::uint32_t>(position.z);
    return static_cast<std::size_t>(value ^ (value >> 32));
  }
};

inline constexpr std::array<LightPos, 6> kDirectionOffsets{{
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}}};

LightPos offset(LightPos position, int direction) {
  const LightPos delta = kDirectionOffsets[static_cast<std::size_t>(direction)];
  return {position.x + delta.x, position.y + delta.y, position.z + delta.z};
}

template <class T>
std::size_t vectorIndex(const std::vector<T> &values, int sectionY) {
  if (values.empty())
    return values.size();
  const int minY = values.front()->sectionY;
  const int index = sectionY - minY;
  return index >= 0 && static_cast<std::size_t>(index) < values.size()
             ? static_cast<std::size_t>(index)
             : values.size();
}

} // namespace

LightLayerSnapshot LightLayerSnapshot::missing() { return {}; }

LightLayerSnapshot LightLayerSnapshot::uniform(std::uint8_t value) {
  if (!validLightValue(value))
    throw std::invalid_argument("light value must be in 0..15");
  LightLayerSnapshot result;
  result.storage_ = Storage::Uniform;
  result.uniform_ = value;
  return result;
}

LightLayerSnapshot LightLayerSnapshot::mixed(
    const std::array<std::uint8_t, kLightVolume> &values) {
  LightLayerSnapshot result;
  result.storage_ = Storage::Mixed;
  result.packed_.assign(kLightLayerBytes, 0);
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!validLightValue(values[i]))
      throw std::invalid_argument("light value must be in 0..15");
    result.packed_[i >> 1] |=
        static_cast<std::uint8_t>((values[i] & 15) << ((i & 1) * 4));
  }
  return result;
}

LightLayerSnapshot LightLayerSnapshot::mixedPacked(
    const std::array<std::uint8_t, kLightLayerBytes> &packed) {
  LightLayerSnapshot result;
  result.storage_ = Storage::Mixed;
  result.packed_.assign(packed.begin(), packed.end());
  return result;
}

std::uint8_t LightLayerSnapshot::get(int x, int y, int z) const {
  if (storage_ == Storage::Missing)
    return 0;
  if (storage_ == Storage::Uniform)
    return uniform_;
  const std::size_t index = lightIndex(x, y, z);
  return static_cast<std::uint8_t>((packed_[index >> 1] >> ((index & 1) * 4)) &
                                   15);
}

const std::uint8_t *LightLayerSnapshot::packedData() const {
  return storage_ == Storage::Mixed ? packed_.data() : nullptr;
}

bool LightLayerSnapshot::operator==(const LightLayerSnapshot &other) const {
  if (storage_ != other.storage_ || uniform_ != other.uniform_)
    return false;
  return storage_ != Storage::Mixed || packed_ == other.packed_;
}

const BlockLightProperties &unsupportedLightProperties() noexcept {
  return kUnsupportedLightProperties;
}

BlockLightProperties officialLightProperties(
    std::string_view blockName, std::string_view properties) {
  std::string key;
  key.reserve(blockName.size() + properties.size() + 1);
  key.append(blockName);
  key.push_back('\x1f');
  key.append(properties);
  const auto found = std::lower_bound(
      std::begin(kOfficialLightStates), std::end(kOfficialLightStates), key,
      [](const OfficialLightState &entry, const std::string &wanted) {
        return entry.key < wanted;
      });
  if (found == std::end(kOfficialLightStates) || found->key != key)
    return unsupportedLightProperties();
  BlockLightProperties result;
  result.emission = found->emission;
  result.dampening = found->dampening;
  result.canOcclude = found->canOcclude;
  result.useShapeForLightOcclusion = found->useShapeForLightOcclusion;
  result.propagatesSkylightDown = found->propagatesSkylightDown;
  result.supported = true;
  if (result.useShapeForLightOcclusion && result.canOcclude) {
    const auto face = std::lower_bound(
        std::begin(kOfficialLightFaceStates),
        std::end(kOfficialLightFaceStates), key,
        [](const OfficialLightFaceState &entry, const std::string &wanted) {
          return entry.key < wanted;
        });
    if (face == std::end(kOfficialLightFaceStates) || face->key != key) {
      // The optical scalar values are known, but propagation must explicitly
      // report the missing directional geometry rather than treating it as air.
      result.supported = false;
      return result;
    }
    result.occlusionShapeId = face->shape;
  }
  return result;
}

bool hasDifferentLightProperties(const BlockState &oldState,
                                 const BlockState &newState) {
  if (oldState == newState)
    return false;
  return hasDifferentLightProperties(lightPropertiesOf(oldState),
                                     lightPropertiesOf(newState));
}

bool hasDifferentLightProperties(const BlockLightProperties &oldProperties,
                                 const BlockLightProperties &newProperties) {
  // Unknown imported states cannot be proven equivalent. Report a change so a
  // future propagation stage must re-check instead of silently retaining stale
  // light. Exact Java conditions below remain independent of that fallback.
  if (!oldProperties.supported || !newProperties.supported)
    return true;
  return oldProperties.emission != newProperties.emission ||
         oldProperties.dampening != newProperties.dampening ||
         oldProperties.useShapeForLightOcclusion ||
         newProperties.useShapeForLightOcclusion;
}

bool hasDifferentSectionLightProperties(bool wasEmpty, bool isEmpty) {
  return wasEmpty != isEmpty;
}

bool lightFaceShapesOcclude(const BlockLightProperties &from,
                            const BlockLightProperties &to,
                            LightDirection direction) {
  return faceShapesOcclude(from, to, static_cast<int>(direction));
}

LightSectionSnapshot::LightSectionSnapshot(
    int section, bool is_empty, std::vector<BlockLightProperties> lightPalette,
    std::vector<std::uint16_t> lightCells,
    std::array<LightLayerSnapshot, kLightLayerCount> lightLayers)
    : sectionY(section), empty(is_empty), palette(std::move(lightPalette)),
      cells(std::move(lightCells)), layers(std::move(lightLayers)) {
  if (palette.empty())
    throw std::invalid_argument("light section needs a palette");
  if (!cells.empty() && cells.size() != kLightVolume)
    throw std::invalid_argument(
        "light section needs zero or exactly 4096 cells");
  if (cells.empty() && palette.size() != 1)
    throw std::invalid_argument(
        "uniform light section needs exactly one palette value");
  if (cells.empty()) {
    if (!palette.front().supported)
      unsupportedCells = kLightVolume;
    return;
  }
  for (std::uint16_t index : cells) {
    if (index >= palette.size())
      throw std::invalid_argument("light section cell has invalid palette index");
  }
  std::vector<std::uint16_t> counts(palette.size(), 0);
  for (std::uint16_t index : cells)
    ++counts[index];
  for (std::size_t i = 0; i < palette.size(); ++i)
    if (!palette[i].supported)
      unsupportedCells += counts[i];
}

const BlockLightProperties &LightSectionSnapshot::at(int x, int y,
                                                     int z) const {
  const auto index = cells.empty() ? 0 : cells.at(lightIndex(x, y, z));
  return palette.at(index);
}

std::uint8_t LightSectionSnapshot::light(LightLayer layer, int x, int y,
                                         int z) const {
  return layers.at(static_cast<std::size_t>(layer)).get(x, y, z);
}

const LightSectionSnapshot *LightChunkInput::section(int sectionY) const {
  if (sections.empty() || !sections.front())
    return nullptr;
  const auto index = vectorIndex(sections, sectionY);
  return index < sections.size() ? sections[index].get() : nullptr;
}

const LightSectionSnapshot *LightChunkSnapshot::section(int sectionY) const {
  if (sections.empty() || !sections.front())
    return nullptr;
  const auto index = vectorIndex(sections, sectionY);
  return index < sections.size() ? sections[index].get() : nullptr;
}

int LightChunkSnapshot::lightValue(LightLayer layer, int x, int y, int z) const {
  const int sectionY = y >> 4;
  const LightSectionSnapshot *current = section(sectionY);
  if (!current)
    return 0;
  const auto layerIndex = static_cast<std::size_t>(layer);
  if (!current->layers[layerIndex].isMissing())
    return current->light(layer, x & 15, y & 15, z & 15);
  if (layer == LightLayer::Block)
    return 0;
  if (!skyLightEnabled)
    return 0;
  for (int aboveY = sectionY + 1;
       aboveY < minBlockSectionY + blockSectionCount + 1; ++aboveY) {
    const LightSectionSnapshot *above = section(aboveY);
    if (above && !above->layers[layerIndex].isMissing())
      return above->light(LightLayer::Sky, x & 15, 0, z & 15);
  }
  return 15;
}

class LightService::Impl {
public:
  Impl(int minSection, int sectionCount, LightWorkerOptions workerOptions)
      : minBlockSectionY(minSection), blockSectionCount(sectionCount),
        options(workerOptions), paused(workerOptions.startPausedForTesting) {
    if (sectionCount <= 0)
      throw std::invalid_argument("light service needs block sections");
    if (options.batchSize == 0 || options.maxQueuedCommands == 0)
      throw std::invalid_argument("light worker limits must be positive");
    worker = std::thread([this] { run(); });
  }

  ~Impl() { close(); }

  std::uint64_t submitChunk(LightChunkInput input) {
    if (!validInput(input)) {
      ++statsRejected;
      return 0;
    }
    const auto key = chunkKey(input.chunkX, input.chunkZ);
    std::lock_guard lock(mutex);
    if (!accepting) {
      ++statsRejected;
      return 0;
    }
    auto &active = versions[key];
    const bool newLifetime = inactive.contains(key) || active.requestRevision == 0;
    if (newLifetime) {
      ++active.epoch;
      active.requestRevision = 0;
      latest.erase(key);
    }
    const auto generation = ++nextGeneration;
    ++active.requestRevision;
    inactive.erase(key);
    const bool preserveUnload = pending.contains(key) && pending.at(key).unload;
    PendingChunk state;
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = active.epoch;
    state.revision = active.requestRevision;
    state.load = std::make_unique<LightChunkInput>(std::move(input));
    // An unload enqueued before this reload remains an ordered tombstone. The
    // worker clears the old internal generation before installing this load.
    state.unload = preserveUnload;
    pending[key] = std::move(state);
    notePendingLocked();
    wake.notify_one();
    return generation;
  }

  bool reportSectionStatus(int chunkX, int chunkZ, int sectionY, bool empty) {
    if (!validSection(sectionY)) {
      ++statsRejected;
      return false;
    }
    const auto key = chunkKey(chunkX, chunkZ);
    std::lock_guard lock(mutex);
    auto active = versions.find(key);
    if (!accepting || active == versions.end() || inactive.contains(key)) {
      ++statsRejected;
      return false;
    }
    const auto generation = ++nextGeneration;
    ++active->second.requestRevision;
    PendingChunk &state = pending[key];
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = active->second.epoch;
    state.revision = active->second.requestRevision;
    state.sectionStatus[sectionY] = empty;
    notePendingLocked();
    wake.notify_one();
    return true;
  }

  bool reportBlockChange(LightBlockChange change) {
    if (!validSection(change.sectionY) || change.localIndex >= kLightVolume) {
      ++statsRejected;
      return false;
    }
    if (!hasDifferentLightProperties(change.oldProperties, change.newProperties) &&
        !hasDifferentSectionLightProperties(change.wasEmpty, change.isEmpty))
      return true;
    const auto key = chunkKey(change.chunkX, change.chunkZ);
    std::lock_guard lock(mutex);
    auto active = versions.find(key);
    if (!accepting || active == versions.end() || inactive.contains(key)) {
      ++statsRejected;
      return false;
    }
    change.generation = ++nextGeneration;
    ++active->second.requestRevision;
    PendingChunk &state = pending[key];
    state.key = key;
    state.generation = change.generation;
    state.lifecycleEpoch = active->second.epoch;
    state.revision = active->second.requestRevision;
    state.sectionStatus[change.sectionY] = change.isEmpty;
    const auto slot = changeSlot(change.sectionY, change.localIndex);
    const auto found = state.changes.find(slot);
    if (found == state.changes.end()) {
      state.changes.emplace(slot, std::move(change));
    } else {
      // Preserve the first old state (the worker's current base) while folding
      // later writes into the latest authoritative state for the same cell.
      found->second.newProperties = change.newProperties;
      found->second.isEmpty = change.isEmpty;
      found->second.generation = change.generation;
    }
    notePendingLocked();
    wake.notify_one();
    return true;
  }

  bool unloadChunk(int chunkX, int chunkZ) {
    const auto key = chunkKey(chunkX, chunkZ);
    std::lock_guard lock(mutex);
    auto active = versions.find(key);
    if (!accepting || active == versions.end()) {
      ++statsRejected;
      return false;
    }
    const auto generation = ++nextGeneration;
    ++active->second.epoch;
    active->second.requestRevision = 0;
    latest.erase(key);
    inactive.insert(key);
    PendingChunk state;
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = active->second.epoch;
    state.unload = true;
    pending[key] = std::move(state);
    notePendingLocked();
    wake.notify_one();
    return true;
  }

  void drainPublished() {
    std::vector<std::vector<std::pair<
        std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>>>
        ready;
    std::vector<std::shared_ptr<const LightChunkSnapshot>> callbacks;
    std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback;
    {
      std::lock_guard lock(mutex);
      ready.swap(published);
      callback = publishCallback;
      for (const auto &batch : ready) {
        for (const auto &[key, snapshot] : batch) {
          const auto active = versions.find(key);
          if (active != versions.end() &&
              active->second.epoch == snapshot->lifecycleEpoch &&
              !inactive.contains(key)) {
            const auto current = latest.find(key);
            if (current == latest.end() ||
                current->second->revision <= snapshot->revision) {
              latest[key] = snapshot;
              if (callback)
                callbacks.push_back(snapshot);
            }
          } else {
            ++statsStale;
          }
        }
      }
    }
    if (callback)
      for (const auto &snapshot : callbacks)
        callback(snapshot);
  }

  std::shared_ptr<const LightChunkSnapshot> latestChunk(int x, int z) const {
    std::lock_guard lock(mutex);
    const auto found = latest.find(chunkKey(x, z));
    return found == latest.end() ? nullptr : found->second;
  }

  void setPublishCallback(
      std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback) {
    std::lock_guard lock(mutex);
    publishCallback = std::move(callback);
  }

  Stats stats() const {
    Stats result;
    result.submitted = statsSubmitted.load(std::memory_order_relaxed);
    result.rejected = statsRejected.load(std::memory_order_relaxed);
    result.processedBatches = statsBatches.load(std::memory_order_relaxed);
    result.processedBlockChanges = statsChanges.load(std::memory_order_relaxed);
    result.published = statsPublished.load(std::memory_order_relaxed);
    result.stalePublications = statsStale.load(std::memory_order_relaxed);
    result.queueHighWater = statsHighWater.load(std::memory_order_relaxed);
    result.lightQueueEntries =
        statsLightQueueEntries.load(std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    result.pendingChunks = pending.size() + busyChunks;
    for (const auto &batch : published)
      result.pendingPublications += batch.size();
    return result;
  }

  std::string workerError() const {
    std::lock_guard lock(mutex);
    return error;
  }

  bool waitIdleForTesting() {
    std::unique_lock lock(mutex);
    idle.wait(lock, [&] { return !busy && pending.empty(); });
    return error.empty();
  }

  void resumeForTesting() {
    std::lock_guard lock(mutex);
    paused = false;
    wake.notify_all();
  }

  void pauseForTesting() {
    std::lock_guard lock(mutex);
    paused = true;
  }

  bool processChunkBatchForTesting(int chunkX, int chunkZ) {
    const auto key = chunkKey(chunkX, chunkZ);
    std::unique_lock lock(mutex);
    if (!paused || closing || forcedChunk || !pending.contains(key))
      return false;
    const auto before = statsBatches.load(std::memory_order_relaxed);
    forcedChunk = key;
    pauseAfterBatch = true;
    paused = false;
    wake.notify_all();
    batchDone.wait(lock, [&] {
      return statsBatches.load(std::memory_order_relaxed) > before || closing;
    });
    return error.empty() &&
           statsBatches.load(std::memory_order_relaxed) > before;
  }

  void close() {
    std::thread selected;
    {
      std::lock_guard lock(mutex);
      if (!accepting && !worker.joinable())
        return;
      accepting = false;
      closing = true;
      wake.notify_all();
      selected = std::move(worker);
    }
    if (selected.joinable())
      selected.join();
  }

private:
  struct PendingChunk {
    std::uint64_t key = 0;
    std::uint64_t generation = 0;
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t revision = 0;
    std::unique_ptr<LightChunkInput> load;
    bool unload = false;
    std::unordered_map<std::uint32_t, LightBlockChange> changes;
    std::unordered_map<int, bool> sectionStatus;
  };

  struct WorkingChunk {
    std::uint64_t key = 0;
    std::uint64_t lifecycleEpoch = 0;
    int chunkX = 0;
    int chunkZ = 0;
    std::shared_ptr<LightChunkSnapshot> snapshot;
    std::unordered_map<int, std::shared_ptr<LightSectionSnapshot>>
        mutableSections;
    bool dirty = false;

    LightSectionSnapshot *sectionForEdit(int sectionY) {
      const auto working = mutableSections.find(sectionY);
      if (working != mutableSections.end())
        return working->second.get();
      const auto index = vectorIndex(snapshot->sections, sectionY);
      if (index >= snapshot->sections.size())
        return nullptr;
      auto copy = std::make_shared<LightSectionSnapshot>(
          *snapshot->sections[index]);
      snapshot->sections[index] = copy;
      mutableSections.emplace(sectionY, copy);
      return copy.get();
    }
  };

  using LightValues = std::array<std::uint8_t, kLightVolume>;

  struct IncreaseTask {
    LightPos position;
    std::uint8_t level = 0;
    std::uint8_t directions = 0;
    bool fromEmpty = false;
    bool fromEmission = false;
  };

  struct DecreaseTask {
    LightPos position;
    std::uint8_t level = 0;
    std::uint8_t directions = 0;
  };

  class BlockLightPropagator {
  public:
    BlockLightPropagator(
        Impl &light,
        std::unordered_map<std::uint64_t, WorkingChunk> &workingChunks,
        const std::unordered_set<std::uint64_t> &removedChunks)
        : owner(light), working(workingChunks), removed(removedChunks) {}

    void enqueueCheck(LightPos position) {
      if (checks.insert(position).second)
        checkQueue.push_back(position);
    }

    void seedChunk(std::uint64_t key) {
      const LightChunkSnapshot *chunk = stateFor(key);
      if (!chunk)
        return;
      for (int sectionY = chunk->minLightSectionY();
           sectionY < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
           ++sectionY) {
        const LightSectionSnapshot *data = chunk->section(sectionY);
        if (!data)
          continue;
        const bool inBlockRange =
            sectionY >= chunk->minBlockSectionY &&
            sectionY < chunk->minBlockSectionY + chunk->blockSectionCount;
        for (int index = 0; index < kLightVolume; ++index) {
          const int x = index & 15;
          const int z = (index >> 4) & 15;
          const int y = (index >> 8) & 15;
          const BlockLightProperties &properties =
              inBlockRange ? data->at(x, y, z) : emptyLightProperties();
          if (inBlockRange && !properties.supported)
            incomplete = true;
          const LightPos position{chunk->chunkX * 16 + x,
                                  sectionY * 16 + y,
                                  chunk->chunkZ * 16 + z};
          if (inBlockRange && properties.supported && properties.emission > 0)
            enqueueIncrease(position, properties.emission, 63,
                            emptyShape(properties), true);
          const std::uint8_t stored =
              data->light(LightLayer::Block, x, y, z);
          if (stored > 0)
            enqueueIncrease(position, stored, 63, emptyShape(properties),
                            false);
        }
      }
    }

    void seedIncoming(std::uint64_t key) {
      const int chunkX = static_cast<int>(key >> 32);
      const int chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
      for (int direction = 2; direction < 6; ++direction) {
        const LightPos delta = kDirectionOffsets[direction];
        const std::uint64_t neighborKey =
            chunkKey(chunkX + delta.x, chunkZ + delta.z);
        const LightChunkSnapshot *neighbor = stateFor(neighborKey);
        if (!neighbor)
          continue;
        forEachFaceCell(neighbor, direction ^ 1, [&](LightPos position) {
          const int level = storedLevel(position);
          if (level > 1) {
            const auto &properties = stateAt(position);
            enqueueIncrease(position, static_cast<std::uint8_t>(level),
                            static_cast<std::uint8_t>(1u << (direction ^ 1)),
                            emptyShape(properties), false);
          }
        });
      }
    }

    void clearIncomingFrom(std::uint64_t removedKey) {
      const int chunkX = static_cast<int>(removedKey >> 32);
      const int chunkZ =
          static_cast<int>(static_cast<std::uint32_t>(removedKey));
      for (int direction = 2; direction < 6; ++direction) {
        const LightPos delta = kDirectionOffsets[direction];
        const std::uint64_t neighborKey =
            chunkKey(chunkX + delta.x, chunkZ + delta.z);
        const LightChunkSnapshot *neighbor = stateFor(neighborKey);
        if (!neighbor)
          continue;
        forEachFaceCell(neighbor, direction ^ 1, [&](LightPos position) {
          enqueueCheck(position);
        });
      }
    }

    void run() {
      while (!checkQueue.empty()) {
        const LightPos position = checkQueue.front();
        checkQueue.pop_front();
        checks.erase(position);
        checkNode(position);
      }
      while (!decreaseQueue.empty()) {
        const DecreaseTask task = decreaseQueue.front();
        decreaseQueue.pop_front();
        ++owner.statsLightQueueEntries;
        propagateDecrease(task);
      }
      while (!increaseQueue.empty()) {
        const IncreaseTask task = increaseQueue.front();
        increaseQueue.pop_front();
        ++owner.statsLightQueueEntries;
        propagateIncrease(task);
      }
    }

    void commitLayers() {
      for (const SectionPos3 &position : dirtySections) {
        const std::uint64_t key = chunkKey(position.x, position.z);
        WorkingChunk *chunk = ensureWorking(key);
        if (!chunk)
          continue;
        LightSectionSnapshot *section = chunk->sectionForEdit(position.y);
        if (!section)
          continue;
        const auto found = lightValues.find(position);
        if (found == lightValues.end())
          continue;
        const LightValues &values = found->second;
        if (std::all_of(values.begin(), values.end(),
                        [&](std::uint8_t value) {
                          return value == values.front();
                        }))
          section->layers[static_cast<std::size_t>(LightLayer::Block)] =
              LightLayerSnapshot::uniform(values.front());
        else
          section->layers[static_cast<std::size_t>(LightLayer::Block)] =
              LightLayerSnapshot::mixed(values);
        chunk->dirty = true;
      }
    }

    bool isComplete() const { return !incomplete; }

  private:
    const LightChunkSnapshot *stateFor(std::uint64_t key) const {
      if (removed.contains(key))
        return nullptr;
      const auto workingChunk = working.find(key);
      if (workingChunk != working.end())
        return workingChunk->second.snapshot.get();
      const auto stored = owner.internal.find(key);
      return stored == owner.internal.end() ? nullptr : stored->second.get();
    }

    const LightChunkSnapshot *stateFor(int chunkX, int chunkZ) const {
      return stateFor(chunkKey(chunkX, chunkZ));
    }

    WorkingChunk *ensureWorking(std::uint64_t key) {
      if (removed.contains(key))
        return nullptr;
      const auto found = working.find(key);
      if (found != working.end())
        return &found->second;
      const auto stored = owner.internal.find(key);
      if (stored == owner.internal.end())
        return nullptr;
      WorkingChunk next;
      next.key = key;
      next.lifecycleEpoch = stored->second->lifecycleEpoch;
      next.chunkX = stored->second->chunkX;
      next.chunkZ = stored->second->chunkZ;
      next.snapshot = std::make_shared<LightChunkSnapshot>(*stored->second);
      auto inserted = working.emplace(key, std::move(next)).first;
      return &inserted->second;
    }

    const LightSectionSnapshot *sectionAt(LightPos position) const {
      if (position.y < (owner.minBlockSectionY - 1) * 16 ||
          position.y >=
              (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16)
        return nullptr;
      const LightChunkSnapshot *chunk =
          stateFor(position.x >> 4, position.z >> 4);
      return chunk ? chunk->section(position.y >> 4) : nullptr;
    }

    const BlockLightProperties &stateAt(LightPos position) const {
      // LevelChunk.getBlockState returns air outside the playable height, while
      // block-light storage still owns one section of vertical padding above
      // and below it.
      if (position.y < owner.minBlockSectionY * 16 ||
          position.y >=
              (owner.minBlockSectionY + owner.blockSectionCount) * 16)
        return emptyLightProperties();
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return unsupportedLightProperties();
      const auto &properties =
          section->at(position.x & 15, position.y & 15, position.z & 15);
      if (!properties.supported)
        incomplete = true;
      return properties;
    }

    int storedLevel(LightPos position) const {
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return 0;
      const SectionPos3 key{position.x >> 4, position.y >> 4,
                            position.z >> 4};
      const auto found = lightValues.find(key);
      if (found != lightValues.end())
        return found->second[lightIndex(position.x, position.y, position.z)];
      return section->light(LightLayer::Block, position.x & 15,
                            position.y & 15, position.z & 15);
    }

    bool setStoredLevel(LightPos position, int level) {
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return false;
      const SectionPos3 key{position.x >> 4, position.y >> 4,
                            position.z >> 4};
      auto inserted = lightValues.try_emplace(key);
      LightValues &values = inserted.first->second;
      if (inserted.second) {
        for (int y = 0; y < 16; ++y)
          for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x)
              values[lightIndex(x, y, z)] =
                  section->light(LightLayer::Block, x, y, z);
      }
      const auto index = lightIndex(position.x, position.y, position.z);
      if (values[index] == level)
        return false;
      values[index] = static_cast<std::uint8_t>(level);
      dirtySections.insert(key);
      return true;
    }

    void checkNode(LightPos position) {
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

    void propagateIncrease(const IncreaseTask &task) {
      int fromLevel = storedLevel(task.position);
      const int targetLevel = task.level;
      if (task.fromEmission) {
        if (fromLevel < targetLevel) {
          setStoredLevel(task.position, targetLevel);
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
        const LightSectionSnapshot *toSection = sectionAt(to);
        if (!toSection)
          continue;
        const int toLevel = storedLevel(to);
        if (fromLevel - 1 <= toLevel)
          continue;
        const auto &toState = stateAt(to);
        const int newLevel = fromLevel - lightOpacity(toState);
        if (newLevel <= toLevel)
          continue;
        if (faceShapesOcclude(fromQueued, toState, direction))
          continue;
        setStoredLevel(to, newLevel);
        if (newLevel > 1) {
          enqueueIncrease(to, static_cast<std::uint8_t>(newLevel),
                          static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))),
                          emptyShape(toState), false);
        }
      }
    }

    void propagateDecrease(const DecreaseTask &task) {
      for (int direction = 0; direction < 6; ++direction) {
        if ((task.directions & (1u << direction)) == 0)
          continue;
        const LightPos to = offset(task.position, direction);
        if (!sectionAt(to))
          continue;
        const int toLevel = storedLevel(to);
        if (toLevel == 0)
          continue;
        if (toLevel <= int(task.level) - 1) {
          const auto &toState = stateAt(to);
          const int emission = toState.supported ? toState.emission : 0;
          setStoredLevel(to, 0);
          if (emission < toLevel)
            enqueueDecrease(to, static_cast<std::uint8_t>(toLevel),
                            static_cast<std::uint8_t>(63u &
                                                      ~(1u << (direction ^ 1))));
          if (emission > 0)
            enqueueIncrease(to, static_cast<std::uint8_t>(emission), 63,
                            emptyShape(toState), true);
        } else {
          enqueueIncrease(to, static_cast<std::uint8_t>(toLevel),
                          static_cast<std::uint8_t>(1u << (direction ^ 1)),
                          false, false);
        }
      }
    }

    template <class Visitor>
    void forEachFaceCell(const LightChunkSnapshot *chunk, int direction,
                         Visitor visitor) const {
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

    void enqueueIncrease(LightPos position, std::uint8_t level,
                         std::uint8_t directions, bool fromEmpty,
                         bool fromEmission) {
      if (level > 0)
        increaseQueue.push_back(
            {position, level, directions, fromEmpty, fromEmission});
    }

    void enqueueDecrease(LightPos position, std::uint8_t level,
                         std::uint8_t directions) {
      decreaseQueue.push_back({position, level, directions});
    }

    Impl &owner;
    std::unordered_map<std::uint64_t, WorkingChunk> &working;
    const std::unordered_set<std::uint64_t> &removed;
    std::unordered_set<LightPos, LightPosHash> checks;
    std::deque<LightPos> checkQueue;
    std::deque<DecreaseTask> decreaseQueue;
    std::deque<IncreaseTask> increaseQueue;
    std::unordered_map<SectionPos3, LightValues, SectionPos3Hash> lightValues;
    std::unordered_set<SectionPos3, SectionPos3Hash> dirtySections;
    mutable bool incomplete = false;
  };

  using SkyColumnSources = std::array<int, 256>;

  class SkyLightPropagator {
  public:
    SkyLightPropagator(
        Impl &light,
        std::unordered_map<std::uint64_t, WorkingChunk> &workingChunks,
        const std::unordered_set<std::uint64_t> &removedChunks,
        std::unordered_map<std::uint64_t, SkyColumnSources> &sources)
        : owner(light), working(workingChunks), removed(removedChunks),
          sourceHeights(sources) {}

    void enqueueCheck(LightPos position) {
      if (checks.insert(position).second)
        checkQueue.push_back(position);
    }

    void seedChunk(std::uint64_t key) {
      WorkingChunk *chunk = workingChunk(key);
      if (!chunk)
        return;
      SkyColumnSources sources = computeSources(*chunk);
      sourceHeights[key] = sources;
      clearLines(*chunk);
      const int minY = owner.minBlockSectionY * 16;
      const int lightBottom = (owner.minBlockSectionY - 1) * 16;
      const int lightTop =
          (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16;
      for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
          const int source = sources[HeightmapColumnKey(x, z)];
          const int first = source == minY - 1 ? lightBottom : source;
          for (int y = lightTop - 1; y >= first; --y)
            setStoredLevel({chunk->chunkX * 16 + x, y,
                            chunk->chunkZ * 16 + z}, 15);
          if (source != minY - 1)
            for (int y = source - 1; y >= lightBottom; --y)
              setStoredLevel({chunk->chunkX * 16 + x, y,
                              chunk->chunkZ * 16 + z}, 0);
          seedColumn(key, chunk, x, z, source);
        }
      }
    }

    void seedIncoming(std::uint64_t key) {
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
        forEachFaceCell(neighbor, direction ^ 1, [&](LightPos position) {
          const int level = storedLevel(position);
          if (level > 1)
            enqueueIncrease(position, static_cast<std::uint8_t>(level),
                            static_cast<std::uint8_t>(1u << (direction ^ 1)),
                            emptyShape(stateAt(position)), false);
        });
      }
    }

    void clearIncomingFrom(std::uint64_t removedKey) {
      const int chunkX = static_cast<int>(removedKey >> 32);
      const int chunkZ =
          static_cast<int>(static_cast<std::uint32_t>(removedKey));
      for (int direction = 2; direction < 6; ++direction) {
        const LightPos delta = kDirectionOffsets[direction];
        const LightChunkSnapshot *neighbor =
            stateFor(chunkKey(chunkX + delta.x, chunkZ + delta.z));
        if (!neighbor)
          continue;
        forEachFaceCell(neighbor, direction ^ 1, [&](LightPos position) {
          enqueueCheck(position);
        });
      }
    }

    void syncStorageEligibility() {
      std::unordered_set<std::uint64_t> affected;
      for (auto &[key, chunk] : working) {
        (void)chunk;
        affected.insert(key);
        const int x = static_cast<int>(key >> 32);
        const int z = static_cast<int>(static_cast<std::uint32_t>(key));
        for (int dx = -1; dx <= 1; ++dx)
          for (int dz = -1; dz <= 1; ++dz)
            affected.insert(chunkKey(x + dx, z + dz));
      }
      for (std::uint64_t key : removed) {
        const int x = static_cast<int>(key >> 32);
        const int z = static_cast<int>(static_cast<std::uint32_t>(key));
        for (int dx = -1; dx <= 1; ++dx)
          for (int dz = -1; dz <= 1; ++dz)
            affected.insert(chunkKey(x + dx, z + dz));
      }
      storedEligible.clear();
      // A section from an already-published chunk outside this batch's
      // authoritative 3x3 domains is still a writable destination. Its raw
      // layer proves the old storage rule admitted it; keeping that evidence
      // avoids treating a cross-edge target as air merely because this batch
      // did not recompute its block sections.
      for (const auto &[key, chunk] : owner.internal) {
        if (affected.contains(key) || removed.contains(key))
          continue;
        for (int y = chunk->minLightSectionY();
             y < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
             ++y) {
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
          // Preserve the same published raw destination if it entered the
          // working set for a reason other than its own section transition.
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
        const int sectionZ =
            static_cast<int>(static_cast<std::uint32_t>(key));
        for (int y = chunk.snapshot->minLightSectionY();
             y < chunk.snapshot->minBlockSectionY +
                     chunk.snapshot->blockSectionCount + 1;
             ++y) {
          bool eligible = false;
          for (int dx = -1; dx <= 1 && !eligible; ++dx)
            for (int dz = -1; dz <= 1 && !eligible; ++dz)
              for (int dy = -1; dy <= 1 && !eligible; ++dy)
                eligible = blockSectionNonEmpty(sectionX + dx, y + dy,
                                                sectionZ + dz);
          const SectionPos3 sectionKey{sectionX, y, sectionZ};
          if (eligible) {
            storedEligible.insert(sectionKey);
            allocateSection(key, y);
          } else {
            values.erase(sectionKey);
            dirtySections.erase(sectionKey);
            const auto *old = chunk.snapshot->section(y);
            if (old &&
                !old->layers[static_cast<std::size_t>(LightLayer::Sky)]
                     .isMissing()) {
              auto *edit = chunk.sectionForEdit(y);
              edit->layers[static_cast<std::size_t>(LightLayer::Sky)] =
                  LightLayerSnapshot::missing();
              chunk.dirty = true;
            }
          }
        }
      }
      storageReady = true;
    }

    int updateColumn(std::uint64_t key, int localX, int localZ) {
      WorkingChunk *chunk = workingChunk(key);
      if (!chunk)
        return owner.minBlockSectionY * 16 - 1;
      auto &sources = sourceHeights[key];
      const int column = HeightmapColumnKey(localX, localZ);
      const int oldSource = sources[column];
      const int newSource = computeSource(*chunk, localX, localZ);
      if (oldSource == newSource)
        return newSource;

      const int blockMinY = owner.minBlockSectionY * 16;
      const int lightBottom = (owner.minBlockSectionY - 1) * 16;
      if (newSource > oldSource) {
        const int oldFirst = oldSource == blockMinY - 1 ? lightBottom : oldSource;
        for (int y = oldFirst; y < newSource; ++y) {
          const LightPos position{chunk->chunkX * 16 + localX, y,
                                  chunk->chunkZ * 16 + localZ};
          const int oldLevel = storedLevel(position);
          if (oldLevel > 0) {
            setStoredLevel(position, 0);
            enqueueDecrease(position, static_cast<std::uint8_t>(oldLevel), 63);
          }
        }
      } else {
        const int newFirst = newSource == blockMinY - 1 ? lightBottom : newSource;
        for (int y = newFirst; y < oldSource; ++y) {
          const LightPos position{chunk->chunkX * 16 + localX, y,
                                  chunk->chunkZ * 16 + localZ};
          setStoredLevel(position, 15);
          enqueueIncrease(position, 15, 63, false, true);
        }
      }
      sources[column] = newSource;
      seedColumn(key, chunk, localX, localZ, newSource);
      return newSource;
    }

    void run() {
      while (!checkQueue.empty()) {
        const LightPos position = checkQueue.front();
        checkQueue.pop_front();
        checks.erase(position);
        checkNode(position);
      }
      while (!decreaseQueue.empty()) {
        const DecreaseTask task = decreaseQueue.front();
        decreaseQueue.pop_front();
        ++owner.statsLightQueueEntries;
        propagateDecrease(task);
      }
      while (!increaseQueue.empty()) {
        const IncreaseTask task = increaseQueue.front();
        increaseQueue.pop_front();
        ++owner.statsLightQueueEntries;
        propagateIncrease(task);
      }
    }

    void commitLayers() {
      std::vector<SectionPos3> ordered(dirtySections.begin(), dirtySections.end());
      std::sort(ordered.begin(), ordered.end(), [](const auto &left,
                                                   const auto &right) {
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
        LightSectionSnapshot *section = chunk->sectionForEdit(position.y);
        if (!section)
          continue;
        const auto found = values.find(position);
        if (found == values.end())
          continue;
        const LightValues &layer = found->second;
        if (std::all_of(layer.begin(), layer.end(), [&](std::uint8_t value) {
              return value == layer.front();
            }))
          section->layers[static_cast<std::size_t>(LightLayer::Sky)] =
              LightLayerSnapshot::uniform(layer.front());
        else
          section->layers[static_cast<std::size_t>(LightLayer::Sky)] =
              LightLayerSnapshot::mixed(layer);
        chunk->dirty = true;
      }
    }

    bool isComplete() const { return !incomplete; }

  private:
    static int HeightmapColumnKey(int x, int z) { return x + z * 16; }

    bool blockSectionNonEmpty(int x, int y, int z) const {
      const LightChunkSnapshot *chunk = stateFor(chunkKey(x, z));
      if (!chunk || y < chunk->minBlockSectionY ||
          y >= chunk->minBlockSectionY + chunk->blockSectionCount)
        return false;
      const LightSectionSnapshot *section = chunk->section(y);
      return section && !section->empty;
    }

    WorkingChunk *workingChunk(std::uint64_t key) {
      const auto found = working.find(key);
      if (found != working.end())
        return &found->second;
      return ensureWorking(key);
    }

    const LightChunkSnapshot *stateFor(std::uint64_t key) const {
      if (removed.contains(key))
        return nullptr;
      const auto current = working.find(key);
      if (current != working.end())
        return current->second.snapshot.get();
      const auto stored = owner.internal.find(key);
      return stored == owner.internal.end() ? nullptr : stored->second.get();
    }

    WorkingChunk *ensureWorking(std::uint64_t key) {
      if (removed.contains(key))
        return nullptr;
      const auto current = working.find(key);
      if (current != working.end())
        return &current->second;
      const auto stored = owner.internal.find(key);
      if (stored == owner.internal.end())
        return nullptr;
      WorkingChunk next;
      next.key = key;
      next.lifecycleEpoch = stored->second->lifecycleEpoch;
      next.chunkX = stored->second->chunkX;
      next.chunkZ = stored->second->chunkZ;
      next.snapshot = std::make_shared<LightChunkSnapshot>(*stored->second);
      return &working.emplace(key, std::move(next)).first->second;
    }

    const LightSectionSnapshot *sectionAt(LightPos position) const {
      if (position.y < (owner.minBlockSectionY - 1) * 16 ||
          position.y >=
              (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16)
        return nullptr;
      const LightChunkSnapshot *chunk =
          stateFor(chunkKey(position.x >> 4, position.z >> 4));
      return chunk ? chunk->section(position.y >> 4) : nullptr;
    }

    const BlockLightProperties &stateAt(LightPos position) const {
      if (position.y < owner.minBlockSectionY * 16 ||
          position.y >=
              (owner.minBlockSectionY + owner.blockSectionCount) * 16)
        return emptyLightProperties();
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return unsupportedLightProperties();
      const auto &properties =
          section->at(position.x & 15, position.y & 15, position.z & 15);
      if (!properties.supported)
        incomplete = true;
      return properties;
    }

    int storedLevel(LightPos position) const {
      const LightChunkSnapshot *chunk =
          stateFor(chunkKey(position.x >> 4, position.z >> 4));
      if (!chunk)
        return 0;
      const SectionPos3 key{position.x >> 4, position.y >> 4,
                            position.z >> 4};
      const auto found = values.find(key);
      if (found != values.end())
        return found->second[lightIndex(position.x, position.y, position.z)];
      const LightSectionSnapshot *section = chunk->section(position.y >> 4);
      if (section && !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                           .isMissing())
        return section->light(LightLayer::Sky, position.x & 15,
                              position.y & 15, position.z & 15);
      for (int aboveY = (position.y >> 4) + 1;
           aboveY < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
           ++aboveY) {
        const SectionPos3 aboveKey{position.x >> 4, aboveY, position.z >> 4};
        const auto edited = values.find(aboveKey);
        if (edited != values.end())
          return edited->second[lightIndex(position.x, aboveY * 16,
                                           position.z)];
        const LightSectionSnapshot *above = chunk->section(aboveY);
        if (above &&
            !above->layers[static_cast<std::size_t>(LightLayer::Sky)]
                 .isMissing())
          return above->light(LightLayer::Sky, position.x & 15, 0,
                              position.z & 15);
      }
      return 15;
    }

    bool setStoredLevel(LightPos position, int level) {
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return false;
      const int current = storedLevel(position);
      if (current == level)
        return false;
      const SectionPos3 key{position.x >> 4, position.y >> 4,
                            position.z >> 4};
      if (storageReady && !storedEligible.contains(key) &&
          !values.contains(key))
        return false;
      auto inserted = values.try_emplace(key);
      LightValues &layer = inserted.first->second;
      if (inserted.second) {
        if (!section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                 .isMissing()) {
          for (int y = 0; y < 16; ++y)
            for (int z = 0; z < 16; ++z)
              for (int x = 0; x < 16; ++x)
                layer[lightIndex(x, y, z)] =
                    section->light(LightLayer::Sky, x, y, z);
        } else {
          const LightChunkSnapshot *chunk =
              stateFor(chunkKey(position.x >> 4, position.z >> 4));
          for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
              int inherited = 15;
              for (int aboveY = key.y + 1;
                   aboveY < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
                   ++aboveY) {
                const SectionPos3 aboveKey{key.x, aboveY, key.z};
                const auto edited = values.find(aboveKey);
                if (edited != values.end()) {
                  inherited = edited->second[lightIndex(x, aboveY * 16, z)];
                  break;
                }
                const LightSectionSnapshot *above = chunk->section(aboveY);
                if (above &&
                    !above->layers[static_cast<std::size_t>(LightLayer::Sky)]
                         .isMissing()) {
                  inherited = above->light(LightLayer::Sky, x, 0, z);
                  break;
                }
              }
              for (int y = 0; y < 16; ++y)
                layer[lightIndex(x, y, z)] = static_cast<std::uint8_t>(inherited);
            }
          }
        }
      }
      const auto index = lightIndex(position.x, position.y, position.z);
      layer[index] = static_cast<std::uint8_t>(level);
      dirtySections.insert(key);
      return true;
    }

    void allocateSection(std::uint64_t chunkKeyValue, int sectionY) {
      const SectionPos3 key{static_cast<int>(chunkKeyValue >> 32), sectionY,
                            static_cast<int>(
                                static_cast<std::uint32_t>(chunkKeyValue))};
      if (values.contains(key))
        return;
      const LightChunkSnapshot *chunk = stateFor(chunkKeyValue);
      if (!chunk)
        return;
      const LightSectionSnapshot *section = chunk->section(sectionY);
      if (!section || !section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                          .isMissing())
        return;
      LightValues layer{};
      for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 16; ++x) {
          int inherited = 15;
          for (int aboveY = sectionY + 1;
               aboveY < chunk->minBlockSectionY + chunk->blockSectionCount + 1;
               ++aboveY) {
            const SectionPos3 aboveKey{key.x, aboveY, key.z};
            const auto edited = values.find(aboveKey);
            if (edited != values.end()) {
              inherited = edited->second[lightIndex(x, aboveY * 16, z)];
              break;
            }
            const LightSectionSnapshot *above = chunk->section(aboveY);
            if (above &&
                !above->layers[static_cast<std::size_t>(LightLayer::Sky)]
                     .isMissing()) {
              inherited = above->light(LightLayer::Sky, x, 0, z);
              break;
            }
          }
          for (int y = 0; y < 16; ++y)
            layer[lightIndex(x, y, z)] = static_cast<std::uint8_t>(inherited);
        }
      }
      values.emplace(key, layer);
      dirtySections.insert(key);
    }

    int computeSource(WorkingChunk &chunk, int x, int z) const {
      const int minY = owner.minBlockSectionY * 16;
      const int maxY =
          (owner.minBlockSectionY + owner.blockSectionCount) * 16;
      const BlockLightProperties *top = &emptyLightProperties();
      for (int y = maxY - 1; y >= minY; --y) {
        const auto &bottom = stateAt({chunk.chunkX * 16 + x, y,
                                      chunk.chunkZ * 16 + z});
        if (bottom.supported && bottom.dampening != 0)
          return y + 1;
        if (faceShapesOcclude(*top, bottom, 0))
          return y + 1;
        top = &bottom;
      }
      return minY - 1;
    }

    SkyColumnSources computeSources(WorkingChunk &chunk) const {
      SkyColumnSources result{};
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          result[HeightmapColumnKey(x, z)] = computeSource(chunk, x, z);
      return result;
    }

    void clearLines(WorkingChunk &chunk) {
      for (auto &section : chunk.snapshot->sections) {
        auto copy = std::make_shared<LightSectionSnapshot>(*section);
        copy->layers[static_cast<std::size_t>(LightLayer::Sky)] =
            LightLayerSnapshot::missing();
        section = copy;
      }
      chunk.mutableSections.clear();
      for (auto current = values.begin(); current != values.end();) {
        if (chunkKey(current->first.x, current->first.z) == chunk.key)
          current = values.erase(current);
        else
          ++current;
      }
    }

    int sourceFor(int blockX, int blockZ) {
      const int chunkX = blockX >> 4;
      const int chunkZ = blockZ >> 4;
      const std::uint64_t key = chunkKey(chunkX, chunkZ);
      auto found = sourceHeights.find(key);
      if (found == sourceHeights.end()) {
        const LightChunkSnapshot *state = stateFor(key);
        if (!state)
          return owner.minBlockSectionY * 16 - 1;
        WorkingChunk temp;
        temp.key = key;
        temp.chunkX = state->chunkX;
        temp.chunkZ = state->chunkZ;
        temp.snapshot = std::make_shared<LightChunkSnapshot>(*state);
        found = sourceHeights.emplace(key, computeSources(temp)).first;
      }
      return found->second[HeightmapColumnKey(blockX & 15, blockZ & 15)];
    }

    void seedColumn(std::uint64_t, WorkingChunk *chunk, int x, int z,
                    int source) {
      const int minY = owner.minBlockSectionY * 16;
      const int lightTop =
          (owner.minBlockSectionY + owner.blockSectionCount + 1) * 16;
      const int sentinel = minY - 1;
      const int bx = chunk->chunkX * 16 + x;
      const int bz = chunk->chunkZ * 16 + z;
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

    bool isSource(LightPos position) {
      return position.y >= sourceFor(position.x, position.z);
    }

    void checkNode(LightPos position) {
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

    void propagateIncrease(const IncreaseTask &task) {
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
        if (!sectionAt(to))
          continue;
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

    void propagateDecrease(const DecreaseTask &task) {
      const int emptySectionsBelow =
          countEmptySectionsBelowIfAtBorder(task.position);
      for (int direction = 0; direction < 6; ++direction) {
        if ((task.directions & (1u << direction)) == 0)
          continue;
        const LightPos to = offset(task.position, direction);
        if (!sectionAt(to))
          continue;
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
                          static_cast<std::uint8_t>(1u << (direction ^ 1)),
                          false, false);
        }
      }
    }

    int countEmptySectionsBelowIfAtBorder(LightPos position) const {
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

    static bool crossedSkySectionEdge(int direction, int localX,
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

    void propagateFromEmptySections(LightPos to, int direction, int level,
                                    bool increase, int emptySectionsBelow) {
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
                  static_cast<std::uint8_t>(63u & ~(1u << (direction ^ 1))),
                  true, false);
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

    template <class Visitor>
    void forEachFaceCell(const LightChunkSnapshot *chunk, int direction,
                         Visitor visitor) const {
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

    void enqueueIncrease(LightPos position, std::uint8_t level,
                         std::uint8_t directions, bool fromEmpty,
                         bool source) {
      if (level > 0)
        increaseQueue.push_back({position, level, directions, fromEmpty, source});
    }

    void enqueueDecrease(LightPos position, std::uint8_t level,
                         std::uint8_t directions) {
      decreaseQueue.push_back({position, level, directions});
    }

    Impl &owner;
    std::unordered_map<std::uint64_t, WorkingChunk> &working;
    const std::unordered_set<std::uint64_t> &removed;
    std::unordered_map<std::uint64_t, SkyColumnSources> &sourceHeights;
    std::unordered_set<LightPos, LightPosHash> checks;
    std::deque<LightPos> checkQueue;
    std::deque<DecreaseTask> decreaseQueue;
    std::deque<IncreaseTask> increaseQueue;
    std::unordered_map<SectionPos3, LightValues, SectionPos3Hash> values;
    std::unordered_set<SectionPos3, SectionPos3Hash> dirtySections;
    std::unordered_set<SectionPos3, SectionPos3Hash> storedEligible;
    bool storageReady = false;
    mutable bool incomplete = false;
  };

  bool validSection(int sectionY) const {
    return sectionY >= minBlockSectionY &&
           sectionY < minBlockSectionY + blockSectionCount;
  }

  bool validInput(const LightChunkInput &input) const {
    if (input.minBlockSectionY != minBlockSectionY ||
        input.blockSectionCount != blockSectionCount)
      return false;
    if (input.sections.size() !=
        static_cast<std::size_t>(blockSectionCount + 2))
      return false;
    for (std::size_t i = 0; i < input.sections.size(); ++i)
      if (!input.sections[i] ||
          input.sections[i]->sectionY !=
              minBlockSectionY - 1 + static_cast<int>(i))
        return false;
    return true;
  }

  static std::uint32_t changeSlot(int sectionY, std::uint16_t localIndex) {
    return (static_cast<std::uint32_t>(sectionY) << 12) | localIndex;
  }

  void notePendingLocked() {
    ++statsSubmitted;
    const auto current = pending.size() + busyChunks;
    auto previous = statsHighWater.load(std::memory_order_relaxed);
    while (current > previous &&
           !statsHighWater.compare_exchange_weak(previous, current,
                                                 std::memory_order_relaxed)) {
    }
  }

  void run() {
    std::unique_lock lock(mutex);
    for (;;) {
      wake.wait(lock,
                [&] { return closing || (!paused && !pending.empty()); });
      if (closing && pending.empty())
        break;
      if (paused && !closing)
        continue;

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
        for (std::size_t i = 0; i < count; ++i) {
          auto it = pending.begin();
          batch.push_back(std::move(it->second));
          pending.erase(it);
        }
      }
      busy = true;
      busyChunks = batch.size();
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
      ++statsBatches;
      if (pauseAfterBatch) {
        pauseAfterBatch = false;
        paused = true;
      }
      idle.notify_all();
      batchDone.notify_all();
      if (closing && pending.empty())
        break;
    }
  }

  void processBatch(std::vector<PendingChunk> &batch) {
    std::unordered_map<std::uint64_t, WorkingChunk> working;
    std::unordered_set<std::uint64_t> removed;
    std::unordered_set<std::uint64_t> replaced;
    std::unordered_set<std::uint64_t> loaded;
    std::vector<LightPos> changedPositions;
    std::uint64_t maxGeneration = 0;

    const auto ensureWorking = [&](std::uint64_t key) -> WorkingChunk * {
      if (removed.contains(key))
        return nullptr;
      const auto found = working.find(key);
      if (found != working.end())
        return &found->second;
      const auto stored = internal.find(key);
      if (stored == internal.end())
        return nullptr;
      WorkingChunk next;
      next.key = key;
      next.lifecycleEpoch = stored->second->lifecycleEpoch;
      next.chunkX = stored->second->chunkX;
      next.chunkZ = stored->second->chunkZ;
      next.snapshot = std::make_shared<LightChunkSnapshot>(*stored->second);
      auto inserted = working.emplace(key, std::move(next)).first;
      return &inserted->second;
    };

    for (PendingChunk &command : batch) {
      const auto key = command.key;
      maxGeneration = std::max(maxGeneration, command.generation);
      if (command.unload) {
        removed.insert(key);
        loaded.erase(key);
        working.erase(key);
      }

      WorkingChunk *chunk = nullptr;
      if (command.load) {
        removed.erase(key);
        auto snapshot = std::make_shared<LightChunkSnapshot>();
        snapshot->chunkX = command.load->chunkX;
        snapshot->chunkZ = command.load->chunkZ;
        snapshot->minBlockSectionY = command.load->minBlockSectionY;
        snapshot->blockSectionCount = command.load->blockSectionCount;
        snapshot->skyLightEnabled = options.hasSkyLight;
        snapshot->generation = command.generation;
        snapshot->lifecycleEpoch = command.lifecycleEpoch;
        snapshot->revision = 0;
        snapshot->sections = command.load->sections;

        const auto old = internal.find(key);
        if (old != internal.end() &&
            old->second->lifecycleEpoch != command.lifecycleEpoch)
          replaced.insert(key);
        if (old != internal.end() &&
            old->second->lifecycleEpoch == command.lifecycleEpoch) {
          // A full same-epoch input can change source columns without a per-cell
          // command. Invalidate the old cross-boundary sky dependencies before
          // rebuilding the chunk from the authoritative snapshot.
          replaced.insert(key);
          for (std::size_t i = 0;
               i < snapshot->sections.size() &&
               i < old->second->sections.size();
               ++i) {
            for (LightLayer layer : {LightLayer::Block, LightLayer::Sky}) {
              const auto index = static_cast<std::size_t>(layer);
              if (!snapshot->sections[i]->layers[index].isMissing())
                continue;
              auto section = std::make_shared<LightSectionSnapshot>(
                  *snapshot->sections[i]);
              section->layers[index] = old->second->sections[i]->layers[index];
              snapshot->sections[i] = section;
            }
          }
          // A full authoritative snapshot is also an optical delta. The old
          // light layer is retained until propagation completes, but every
          // changed property must re-check its old emission/dependency.
          for (int section = 0; section < command.load->blockSectionCount;
               ++section) {
            const int sectionY = command.load->minBlockSectionY + section;
            const LightSectionSnapshot *oldSection =
                old->second->section(sectionY);
            const LightSectionSnapshot *newSection = snapshot->section(sectionY);
            if (!oldSection || !newSection)
              continue;
            if (oldSection->isUniform() && newSection->isUniform() &&
                oldSection->at(0, 0, 0) == newSection->at(0, 0, 0))
              continue;
            for (int index = 0; index < kLightVolume; ++index) {
              const int x = index & 15;
              const int z = (index >> 4) & 15;
              const int y = (index >> 8) & 15;
              if (oldSection->at(x, y, z) == newSection->at(x, y, z))
                continue;
              changedPositions.push_back(
                  {snapshot->chunkX * 16 + x, sectionY * 16 + y,
                   snapshot->chunkZ * 16 + z});
            }
          }
        }

        if (!options.hasSkyLight) {
          for (std::size_t i = 0; i < snapshot->sections.size(); ++i) {
            const auto &layer = snapshot->sections[i]
                                    ->layers[static_cast<std::size_t>(LightLayer::Sky)];
            if (layer.isMissing() ||
                (layer.isUniform() && layer.uniformValue() == 0))
              continue;
            auto section = std::make_shared<LightSectionSnapshot>(
                *snapshot->sections[i]);
            section->layers[static_cast<std::size_t>(LightLayer::Sky)] =
                LightLayerSnapshot::uniform(0);
            snapshot->sections[i] = section;
          }
        }

        WorkingChunk next;
        next.key = key;
        next.lifecycleEpoch = command.lifecycleEpoch;
        next.chunkX = snapshot->chunkX;
        next.chunkZ = snapshot->chunkZ;
        next.snapshot = std::move(snapshot);
        next.dirty = true;
        auto inserted = working.insert_or_assign(key, std::move(next)).first;
        chunk = &inserted->second;
        loaded.insert(key);
      } else if (!command.unload) {
        chunk = ensureWorking(key);
        if (!chunk)
          continue;
      }

      if (!chunk)
        continue;
      for (const auto &[slot, change] : command.changes) {
        (void)slot;
        LightSectionSnapshot *section =
            chunk->sectionForEdit(change.sectionY);
        if (!section)
          throw std::runtime_error(
              "light change references a missing section");
        const auto paletteIt = std::find(section->palette.begin(),
                                         section->palette.end(),
                                         change.newProperties);
        std::uint16_t paletteIndex = 0;
        if (paletteIt == section->palette.end()) {
          if (section->palette.size() >= 65536)
            throw std::runtime_error("light section palette exceeds uint16");
          paletteIndex = static_cast<std::uint16_t>(section->palette.size());
          section->palette.push_back(change.newProperties);
        } else {
          paletteIndex = static_cast<std::uint16_t>(
              paletteIt - section->palette.begin());
        }
        if (section->cells.empty()) {
          section->cells.assign(kLightVolume, 0);
          if (!section->palette.front().supported)
            section->unsupportedCells = kLightVolume;
        }
        const auto oldIndex = section->cells[change.localIndex];
        if (!section->palette[oldIndex].supported)
          --section->unsupportedCells;
        section->cells[change.localIndex] = paletteIndex;
        if (!change.newProperties.supported)
          ++section->unsupportedCells;
        ++statsChanges;
        changedPositions.push_back(
            {chunk->chunkX * 16 + (change.localIndex & 15),
             change.sectionY * 16 + ((change.localIndex >> 8) & 15),
             chunk->chunkZ * 16 + ((change.localIndex >> 4) & 15)});
      }
      for (const auto &[sectionY, empty] : command.sectionStatus) {
        if (auto *section = chunk->sectionForEdit(sectionY))
          section->empty = empty;
      }
      chunk->dirty = true;
    }

    BlockLightPropagator propagation(*this, working, removed);
    for (const LightPos &position : changedPositions)
      propagation.enqueueCheck(position);
    for (std::uint64_t key : loaded) {
      propagation.seedChunk(key);
      propagation.seedIncoming(key);
    }
    for (std::uint64_t key : removed)
      propagation.clearIncomingFrom(key);
    for (std::uint64_t key : replaced)
      if (!removed.contains(key))
        propagation.clearIncomingFrom(key);
    propagation.run();
    propagation.commitLayers();

    bool blockComplete = propagation.isComplete();
    bool skyComplete = !options.hasSkyLight;
    if (options.hasSkyLight) {
      SkyLightPropagator sky(*this, working, removed, skyColumnSources);
      for (std::uint64_t key : loaded) {
        sky.seedChunk(key);
        sky.seedIncoming(key);
      }
      for (std::uint64_t key : removed)
        sky.clearIncomingFrom(key);
      for (std::uint64_t key : replaced)
        if (!removed.contains(key))
          sky.clearIncomingFrom(key);

      std::unordered_set<std::uint64_t> changedColumns;
      for (const LightPos &position : changedPositions)
        changedColumns.insert(
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(position.x))
             << 32) |
            static_cast<std::uint32_t>(position.z));
      for (std::uint64_t column : changedColumns) {
        const int x = static_cast<int>(column >> 32);
        const int z = static_cast<int>(static_cast<std::uint32_t>(column));
        sky.updateColumn(chunkKey(x >> 4, z >> 4), x & 15, z & 15);
      }
      for (const LightPos &position : changedPositions)
        sky.enqueueCheck(position);
      sky.syncStorageEligibility();
      sky.run();
      sky.commitLayers();
      skyComplete = sky.isComplete();
    }
    for (const auto &[key, chunk] : working) {
      if (removed.contains(key))
        continue;
      chunk.snapshot->unsupportedCells = 0;
      for (const auto &section : chunk.snapshot->sections)
        chunk.snapshot->unsupportedCells += section->unsupportedCells;
      if (chunk.snapshot->unsupportedCells != 0) {
        blockComplete = false;
        if (options.hasSkyLight)
          skyComplete = false;
      }
    }
    for (const auto &[key, chunk] : internal) {
      if (!removed.contains(key) && !working.contains(key) &&
          chunk->unsupportedCells != 0) {
        blockComplete = false;
        if (options.hasSkyLight)
          skyComplete = false;
      }
    }

    const std::uint64_t publicationRevision = ++nextPublicationRevision;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<LightChunkSnapshot>>>
        publication;
    for (auto &[key, chunk] : working) {
      if (removed.contains(key) || !chunk.dirty)
        continue;
      chunk.snapshot->generation = maxGeneration;
      chunk.snapshot->revision = publicationRevision;
      chunk.snapshot->lifecycleEpoch = chunk.lifecycleEpoch;
      chunk.snapshot->blockLightComplete = blockComplete;
      chunk.snapshot->skyLightComplete = skyComplete;
      chunk.snapshot->propagationComplete = blockComplete && skyComplete;
      chunk.snapshot->protocolReady = chunk.snapshot->propagationComplete;
      chunk.snapshot->changedSections.clear();
      const auto old = internal.find(key);
      for (const auto &now : chunk.snapshot->sections) {
        bool skyChanged = true;
        bool blockChanged = true;
        if (old != internal.end() &&
            old->second->lifecycleEpoch == chunk.snapshot->lifecycleEpoch) {
          const LightSectionSnapshot *before = old->second->section(now->sectionY);
          if (before == now.get()) {
            skyChanged = false;
            blockChanged = false;
          } else if (before) {
            skyChanged =
                before->layers[static_cast<std::size_t>(LightLayer::Sky)] !=
                now->layers[static_cast<std::size_t>(LightLayer::Sky)];
            blockChanged =
                before->layers[static_cast<std::size_t>(LightLayer::Block)] !=
                now->layers[static_cast<std::size_t>(LightLayer::Block)];
          }
        }
        if (skyChanged || blockChanged)
          chunk.snapshot->changedSections.push_back(
              {now->sectionY, skyChanged, blockChanged});
      }
      publication.emplace_back(key, chunk.snapshot);
    }
    if (!publication.empty()) {
      std::sort(publication.begin(), publication.end(),
                [](const auto &left, const auto &right) {
                  return left.first < right.first;
                });
      std::lock_guard lock(mutex);
      // Keep the undrained publication state bounded by one latest snapshot per
      // chunk. Removing an older occurrence preserves every remaining batch's
      // all-at-once visibility while allowing a newer revision to supersede it.
      for (auto queuedBatch = published.begin(); queuedBatch != published.end();) {
        std::erase_if(*queuedBatch, [&](const auto &entry) {
          for (auto &newEntry : publication) {
            if (newEntry.first != entry.first)
              continue;
            if (entry.second->lifecycleEpoch ==
                newEntry.second->lifecycleEpoch) {
              std::map<int, std::pair<bool, bool>> merged;
              for (const auto &change : entry.second->changedSections)
                merged[change.sectionY] = {change.sky, change.block};
              for (const auto &change : newEntry.second->changedSections) {
                auto &flags = merged[change.sectionY];
                flags.first |= change.sky;
                flags.second |= change.block;
              }
              newEntry.second->changedSections.clear();
              for (const auto &[sectionY, flags] : merged)
                newEntry.second->changedSections.push_back(
                    {sectionY, flags.first, flags.second});
            }
            return true;
          }
          return false;
        });
        if (queuedBatch->empty())
          queuedBatch = published.erase(queuedBatch);
        else
          ++queuedBatch;
      }
      std::vector<std::pair<std::uint64_t,
                            std::shared_ptr<const LightChunkSnapshot>>>
          committed;
      committed.reserve(publication.size());
      for (auto &[key, snapshot] : publication) {
        internal[key] = snapshot;
        committed.emplace_back(key, std::move(snapshot));
      }
      statsPublished.fetch_add(committed.size(), std::memory_order_relaxed);
      published.push_back(std::move(committed));
    }
    for (std::uint64_t key : removed) {
      internal.erase(key);
      skyColumnSources.erase(key);
    }
  }

  int minBlockSectionY;
  int blockSectionCount;
  LightWorkerOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable idle;
  std::condition_variable batchDone;
  std::unordered_map<std::uint64_t, PendingChunk> pending;
  struct ChunkVersion {
    std::uint64_t epoch = 0;
    std::uint64_t requestRevision = 0;
  };
  std::unordered_map<std::uint64_t, ChunkVersion> versions;
  std::unordered_set<std::uint64_t> inactive;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      latest;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      internal;
  std::unordered_map<std::uint64_t, SkyColumnSources> skyColumnSources;
  std::vector<std::vector<std::pair<
      std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>>>
      published;
  std::function<void(std::shared_ptr<const LightChunkSnapshot>)> publishCallback;
  std::thread worker;
  bool accepting = true;
  bool closing = false;
  bool busy = false;
  std::size_t busyChunks = 0;
  std::uint64_t nextGeneration = 0;
  std::uint64_t nextPublicationRevision = 0;
  bool paused = false;
  bool pauseAfterBatch = false;
  std::optional<std::uint64_t> forcedChunk;
  std::string error;
  mutable std::atomic<std::uint64_t> statsSubmitted{0};
  mutable std::atomic<std::uint64_t> statsRejected{0};
  mutable std::atomic<std::uint64_t> statsBatches{0};
  mutable std::atomic<std::uint64_t> statsChanges{0};
  mutable std::atomic<std::uint64_t> statsPublished{0};
  mutable std::atomic<std::uint64_t> statsStale{0};
  mutable std::atomic<std::uint64_t> statsHighWater{0};
  mutable std::atomic<std::uint64_t> statsLightQueueEntries{0};
};

LightService::LightService(int minBlockSectionY, int blockSectionCount,
                           LightWorkerOptions options)
    : impl_(std::make_unique<Impl>(minBlockSectionY, blockSectionCount, options)) {}

LightService::~LightService() = default;

std::uint64_t LightService::submitChunk(LightChunkInput input) {
  return impl_->submitChunk(std::move(input));
}

bool LightService::reportSectionStatus(int chunkX, int chunkZ, int sectionY,
                                       bool empty) {
  return impl_->reportSectionStatus(chunkX, chunkZ, sectionY, empty);
}

bool LightService::reportBlockChange(LightBlockChange change) {
  return impl_->reportBlockChange(change);
}

bool LightService::unloadChunk(int chunkX, int chunkZ) {
  return impl_->unloadChunk(chunkX, chunkZ);
}

void LightService::drainPublished() { impl_->drainPublished(); }

std::shared_ptr<const LightChunkSnapshot> LightService::latestChunk(
    int chunkX, int chunkZ) const {
  return impl_->latestChunk(chunkX, chunkZ);
}

void LightService::setPublishCallback(
    std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback) {
  impl_->setPublishCallback(std::move(callback));
}

LightService::Stats LightService::stats() const { return impl_->stats(); }
std::string LightService::workerError() const { return impl_->workerError(); }
bool LightService::waitIdleForTesting() { return impl_->waitIdleForTesting(); }
void LightService::pauseForTesting() { impl_->pauseForTesting(); }
void LightService::resumeForTesting() { impl_->resumeForTesting(); }
bool LightService::processChunkBatchForTesting(int chunkX, int chunkZ) {
  return impl_->processChunkBatchForTesting(chunkX, chunkZ);
}
void LightService::close() { impl_->close(); }

} // namespace redstone
