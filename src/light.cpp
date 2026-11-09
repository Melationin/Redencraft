#include "redstone/light.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
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
    const bool newLifetime = inactive.contains(key) || active.revision == 0;
    if (newLifetime) {
      ++active.epoch;
      active.revision = 0;
      latest.erase(key);
    }
    const auto generation = ++nextGeneration;
    ++active.revision;
    inactive.erase(key);
    const bool preserveUnload = pending.contains(key) && pending.at(key).unload;
    PendingChunk state;
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = active.epoch;
    state.revision = active.revision;
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
    ++active->second.revision;
    PendingChunk &state = pending[key];
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = active->second.epoch;
    state.revision = active->second.revision;
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
    ++active->second.revision;
    PendingChunk &state = pending[key];
    state.key = key;
    state.generation = change.generation;
    state.lifecycleEpoch = active->second.epoch;
    state.revision = active->second.revision;
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
    active->second.revision = 0;
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
    std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
        ready;
    std::vector<std::shared_ptr<const LightChunkSnapshot>> callbacks;
    std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback;
    {
      std::lock_guard lock(mutex);
      ready.swap(published);
      callback = publishCallback;
      for (auto &[key, snapshot] : ready) {
        const auto active = versions.find(key);
        if (active != versions.end() &&
            active->second.epoch == snapshot->lifecycleEpoch &&
            !inactive.contains(key)) {
          const auto current = latest.find(key);
          if (current == latest.end() ||
              current->second->revision <= snapshot->revision) {
            latest[key] = snapshot;
            if (callback)
              callbacks.push_back(std::move(snapshot));
          }
        } else {
          ++statsStale;
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
    result.pendingPublications = published.size();
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
      for (int section = 0; section < owner.blockSectionCount; ++section) {
        const LightSectionSnapshot *data =
            chunk->section(owner.minBlockSectionY + section);
        if (!data)
          continue;
        for (int index = 0; index < kLightVolume; ++index) {
          const int x = index & 15;
          const int z = (index >> 4) & 15;
          const int y = (index >> 8) & 15;
          const BlockLightProperties &properties = data->at(x, y, z);
          if (!properties.supported)
            incomplete = true;
          const LightPos position{chunk->chunkX * 16 + x,
                                  (owner.minBlockSectionY + section) * 16 + y,
                                  chunk->chunkZ * 16 + z};
          if (properties.supported && properties.emission > 0)
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
      if (position.y < owner.minBlockSectionY * 16 ||
          position.y >= (owner.minBlockSectionY + owner.blockSectionCount) * 16)
        return nullptr;
      const LightChunkSnapshot *chunk =
          stateFor(position.x >> 4, position.z >> 4);
      return chunk ? chunk->section(position.y >> 4) : nullptr;
    }

    const BlockLightProperties &stateAt(LightPos position) const {
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
      if (task.fromEmission) {
        const int stored = storedLevel(task.position);
        if (stored < task.level)
          setStoredLevel(task.position, task.level);
      }
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
        if (int(task.level) - 1 <= toLevel)
          continue;
        const auto &toState = stateAt(to);
        const int newLevel = int(task.level) - lightOpacity(toState);
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
      const int minY = owner.minBlockSectionY * 16;
      const int maxY = (owner.minBlockSectionY + owner.blockSectionCount) * 16;
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
      const std::size_t count = std::min(
          {options.batchSize, options.maxQueuedCommands, pending.size()});
      batch.reserve(count);
      for (std::size_t i = 0; i < count; ++i) {
        auto it = pending.begin();
        batch.push_back(std::move(it->second));
        pending.erase(it);
      }
      busy = true;
      busyChunks = count;
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
      idle.notify_all();
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
        snapshot->generation = command.generation;
        snapshot->lifecycleEpoch = command.lifecycleEpoch;
        snapshot->revision = command.revision;
        snapshot->sections = command.load->sections;

        const auto old = internal.find(key);
        if (old != internal.end() &&
            old->second->lifecycleEpoch != command.lifecycleEpoch)
          replaced.insert(key);
        if (old != internal.end() &&
            old->second->lifecycleEpoch == command.lifecycleEpoch) {
          for (std::size_t i = 0;
               i < snapshot->sections.size() &&
               i < old->second->sections.size();
               ++i) {
            if (!snapshot->sections[i]->layers[
                    static_cast<std::size_t>(LightLayer::Block)].isMissing())
              continue;
            auto section = std::make_shared<LightSectionSnapshot>(
                *snapshot->sections[i]);
            section->layers[static_cast<std::size_t>(LightLayer::Block)] =
                old->second->sections[i]->layers[
                    static_cast<std::size_t>(LightLayer::Block)];
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

    bool metadataComplete = propagation.isComplete();
    for (const auto &[key, chunk] : working) {
      if (removed.contains(key))
        continue;
      chunk.snapshot->unsupportedCells = 0;
      for (const auto &section : chunk.snapshot->sections)
        chunk.snapshot->unsupportedCells += section->unsupportedCells;
      if (chunk.snapshot->unsupportedCells != 0)
        metadataComplete = false;
    }
    for (const auto &[key, chunk] : internal) {
      if (!removed.contains(key) && !working.contains(key) &&
          chunk->unsupportedCells != 0)
        metadataComplete = false;
    }

    for (auto &[key, chunk] : working) {
      if (removed.contains(key) || !chunk.dirty)
        continue;
      chunk.snapshot->generation = maxGeneration;
      chunk.snapshot->revision = maxGeneration;
      chunk.snapshot->lifecycleEpoch = chunk.lifecycleEpoch;
      chunk.snapshot->blockLightComplete = metadataComplete;
      chunk.snapshot->propagationComplete = false;
      chunk.snapshot->protocolReady = false;
      std::shared_ptr<const LightChunkSnapshot> immutable = chunk.snapshot;
      internal[key] = immutable;
      std::lock_guard lock(mutex);
      published[key] = immutable;
      ++statsPublished;
    }
    for (std::uint64_t key : removed)
      internal.erase(key);
  }

  int minBlockSectionY;
  int blockSectionCount;
  LightWorkerOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable idle;
  std::unordered_map<std::uint64_t, PendingChunk> pending;
  struct ChunkVersion {
    std::uint64_t epoch = 0;
    std::uint64_t revision = 0;
  };
  std::unordered_map<std::uint64_t, ChunkVersion> versions;
  std::unordered_set<std::uint64_t> inactive;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      latest;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      internal;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      published;
  std::function<void(std::shared_ptr<const LightChunkSnapshot>)> publishCallback;
  std::thread worker;
  bool accepting = true;
  bool closing = false;
  bool busy = false;
  std::size_t busyChunks = 0;
  std::uint64_t nextGeneration = 0;
  bool paused = false;
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
void LightService::close() { impl_->close(); }

} // namespace redstone
