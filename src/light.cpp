#include "redstone/light.hpp"

#include <algorithm>
#include <bit>
#include <array>
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
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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

struct ThreadAffinityResult {
  std::uint64_t mask = 0;
  std::uint32_t error = 0;
};

double currentThreadCpuMs() {
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
    return 0;
  const auto ticks = [](const FILETIME &value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
           value.dwLowDateTime;
  };
  return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
  return 0;
#endif
}

ThreadAffinityResult applyThreadAffinity(void *threadHandle,
                                        std::uintptr_t mask) {
#ifdef _WIN32
  if (!mask)
    return {};
  const HANDLE thread = static_cast<HANDLE>(threadHandle);
  GROUP_AFFINITY current{};
  if (!GetThreadGroupAffinity(thread, &current))
    return {0, GetLastError()};
  GROUP_AFFINITY requested = current;
  requested.Mask = static_cast<KAFFINITY>(mask);
  if (!SetThreadGroupAffinity(thread, &requested, nullptr)) {
    return {0, GetLastError()};
  }
  GROUP_AFFINITY group{};
  return {GetThreadGroupAffinity(thread, &group)
              ? static_cast<std::uint64_t>(group.Mask)
              : static_cast<std::uint64_t>(mask),
          0};
#else
  (void)threadHandle;
  (void)mask;
  return {};
#endif
}

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
  friend class LightService;

  struct OwnedCell {
    std::uint64_t sequence = 0;
    BlockLightProperties properties;
    bool sectionEmpty = false;
  };
  struct SectionStatusValue {
    std::uint64_t sequence = 0;
    bool empty = false;
  };
  struct CoalescedCell {
    std::uint16_t localIndex = 0;
    OwnedCell value;
  };
  struct CoalescedSection {
    std::uint64_t chunkKey = 0;
    std::uint64_t lifecycleEpoch = 0;
    int sectionY = 0;
    std::uint64_t sectionSequence = 0;
    bool sectionEmpty = false;
    std::vector<CoalescedCell> cells;
  };

public:
  Impl(int minSection, int sectionCount, LightWorkerOptions workerOptions)
      : minBlockSectionY(minSection), blockSectionCount(sectionCount),
        options(workerOptions), paused(workerOptions.startPausedForTesting) {
    if (sectionCount <= 0)
      throw std::invalid_argument("light service needs block sections");
    if (options.batchSize == 0 || options.maxQueuedCommands == 0)
      throw std::invalid_argument("light worker limits must be positive");
    if (options.cellQueueCapacity < 2 ||
        !std::has_single_bit(options.cellQueueCapacity))
      throw std::invalid_argument("light cell queue capacity must be a power of two");
    ring.resize(options.cellQueueCapacity);
    ringMask = options.cellQueueCapacity - 1;
    ringBatchLimit = std::min<std::size_t>(1024, ring.size());
    worker = std::thread([this] { run(); });
#ifdef _WIN32
    const auto affinity =
        applyThreadAffinity(worker.native_handle(), options.workerAffinityMask);
    statsWorkerAffinity.store(affinity.mask, std::memory_order_relaxed);
    statsWorkerAffinityError.store(affinity.error,
                                   std::memory_order_relaxed);
#endif
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
    const bool newLifetime = active.inactive || active.requestRevision == 0;
    if (newLifetime) {
      if (input.lifecycleEpoch != 0)
        active.epoch = input.lifecycleEpoch;
      else
        ++active.epoch;
      active.requestRevision = 0;
      latest.erase(key);
    } else if (input.lifecycleEpoch != 0 &&
               input.lifecycleEpoch != active.epoch) {
      ++statsRejected;
      return 0;
    }
    const auto generation = nextRequestGeneration();
    ++active.requestRevision;
    active.inactive = false;
    const bool preserveUnload = pending.contains(key) && pending.at(key).unload;
    PendingChunk state;
    state.key = key;
    state.generation = generation;
    // This full snapshot includes every cell change up to `generation`.
    // Older ring/block records must not be replayed on top of it.
    state.inputSequence = generation;
    state.lifecycleEpoch = active.epoch;
    state.revision = active.requestRevision;
    state.load = std::make_unique<LightChunkInput>(std::move(input));
    // An unload enqueued before this reload remains an ordered tombstone. The
    // worker clears the old internal generation before installing this load.
    state.unload = preserveUnload;
    pending[key] = std::move(state);
    notePendingLocked();
    signalWorkerLocked(true);
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
    if (!accepting || active == versions.end() || active->second.inactive) {
      ++statsRejected;
      return false;
    }
    const auto generation = nextRequestGeneration();
    ++active->second.requestRevision;
    PendingChunk &state = pending[key];
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = active->second.epoch;
    state.revision = active->second.requestRevision;
    state.sectionStatus[sectionY] = {generation, empty};
    notePendingLocked();
    signalWorkerLocked(true);
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
    return applyBlockChange(change);
  }

  // Bulk form: the caller coalesced a tick's writes into one batch grouped by
  // chunk, so the per-cell version/pending/section lookups collapse into one set
  // of lookups per chunk-section and the mutex is taken once for the whole tick.
  void reportBlockChanges(const std::vector<LightBlockChange> &changes) {
    if (changes.empty())
      return;
    std::lock_guard lock(mutex);
    std::optional<std::uint64_t> currentKey;
    ChunkVersion *active = nullptr;
    PendingChunk *state = nullptr;
    int currentSection = std::numeric_limits<int>::min();
    std::uint64_t applied = 0;
    for (const auto &source : changes) {
      if (!validSection(source.sectionY) ||
          source.localIndex >= kLightVolume) {
        ++statsRejected;
        continue;
      }
      const auto key = chunkKey(source.chunkX, source.chunkZ);
      if (key != currentKey) {
        currentKey = key;
        currentSection = std::numeric_limits<int>::min();
        active = nullptr;
        state = nullptr;
        auto found = versions.find(key);
        if (accepting && found != versions.end() && !found->second.inactive) {
          active = &found->second;
          auto stateIt = pending.find(key);
          if (stateIt == pending.end())
            stateIt = pending.emplace(key, PendingChunk{}).first;
          state = &stateIt->second;
          state->key = key;
          state->lifecycleEpoch = active->epoch;
        }
      }
      if (!state) {
        ++statsRejected;
        continue;
      }
      const auto generation = nextRequestGeneration();
      if (generation <= state->inputSequence)
        continue;
      if (source.sectionY != currentSection) {
        currentSection = source.sectionY;
        state->sectionStatus[currentSection] = {generation, source.isEmpty};
      }
      ++active->requestRevision;
      state->generation = generation;
      state->revision = active->requestRevision;
      const auto slot = changeSlot(source.sectionY, source.localIndex);
      const auto found = state->changes.find(slot);
      if (found == state->changes.end()) {
        LightBlockChange stored = source;
        stored.generation = generation;
        state->changes.emplace(slot, std::move(stored));
      } else {
        found->second.newProperties = source.newProperties;
        found->second.isEmpty = source.isEmpty;
        found->second.generation = generation;
      }
      ++applied;
    }
    if (applied != 0) {
      ++statsSubmitted;
      noteHighWaterLocked();
    }
    if (applied != 0)
      signalWorkerLocked();
  }

  std::uint64_t nextRequestGeneration() {
    return nextGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  // Atomic wait is used for the cell ring so the producer does not take the
  // service mutex per block write. The sequence increment is published after
  // the ring head; a worker that captured an older sequence cannot miss it.
  void notifyWorkerSleeping() {
    wakeSequence.fetch_add(1, std::memory_order_release);
    if (workerSleeping.exchange(false, std::memory_order_acq_rel))
      wakeSequence.notify_one();
  }

  void signalWorkerLocked(bool batchWindow = false) {
    notifyWorkerSleeping();
    if (batchWindow)
      wake.notify_all();
  }

  bool applyOwnedCellLocked(const LightCellUpdate &update) {
    if (!validSection(update.sectionY) || update.localIndex >= kLightVolume) {
      ++statsRejected;
      return false;
    }
    auto active = versions.find(update.chunkKey);
    if (active == versions.end() || active->second.inactive ||
        (update.lifecycleEpoch != 0 &&
         update.lifecycleEpoch != active->second.epoch)) {
      ++statsRejected;
      return false;
    }
    auto stateIt = pending.find(update.chunkKey);
    if (stateIt == pending.end())
      stateIt = pending.emplace(update.chunkKey, PendingChunk{}).first;
    PendingChunk &state = stateIt->second;
    state.key = update.chunkKey;
    if (update.sequence <= state.inputSequence)
      return true;
    state.generation = update.sequence;
    state.lifecycleEpoch = active->second.epoch;
    state.revision = ++active->second.requestRevision;
    auto &status = state.sectionStatus[update.sectionY];
    if (update.sequence > status.sequence)
      status = {update.sequence, update.sectionEmpty};
    const auto slot = changeSlot(update.sectionY, update.localIndex);
    auto &owned = state.ownedCells[slot];
    if (update.sequence > owned.sequence)
      owned = {update.sequence, update.properties, update.sectionEmpty};
    notePendingLocked();
    return true;
  }

  void enqueueCoalescedLocked(const CoalescedSection *sections,
                              std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      const CoalescedSection &section = sections[i];
      if (!validSection(section.sectionY)) {
        ++statsRejected;
        continue;
      }
      for (const CoalescedCell &cell : section.cells) {
        LightCellUpdate update;
        update.chunkKey = section.chunkKey;
        update.lifecycleEpoch = section.lifecycleEpoch;
        update.sequence = cell.value.sequence;
        update.sectionY = section.sectionY;
        update.localIndex = cell.localIndex;
        update.properties = cell.value.properties;
        update.sectionEmpty = cell.value.sectionEmpty;
        applyOwnedCellLocked(update);
      }
    }
  }

  // Shared merge step of the single and the bulk report path. Must be called
  // with `mutex` held and a validated section/local index.
  bool applyBlockChangeLocked(LightBlockChange &change) {
    const auto key = chunkKey(change.chunkX, change.chunkZ);
    auto active = versions.find(key);
    if (!accepting || active == versions.end() || active->second.inactive) {
      ++statsRejected;
      return false;
    }
    change.generation = nextRequestGeneration();
    auto stateIt = pending.find(key);
    if (stateIt == pending.end())
      stateIt = pending.emplace(key, PendingChunk{}).first;
    PendingChunk &state = stateIt->second;
    if (change.generation <= state.inputSequence)
      return true;
    ++active->second.requestRevision;
    state.key = key;
    state.generation = change.generation;
    state.lifecycleEpoch = active->second.epoch;
    state.revision = active->second.requestRevision;
    state.sectionStatus[change.sectionY] = {change.generation, change.isEmpty};
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
    return true;
  }

  bool applyBlockChange(LightBlockChange change) {
    std::lock_guard lock(mutex);
    const bool applied = applyBlockChangeLocked(change);
    if (applied)
      signalWorkerLocked();
    return applied;
  }

  bool unloadChunk(int chunkX, int chunkZ) {
    const auto key = chunkKey(chunkX, chunkZ);
    std::lock_guard lock(mutex);
    auto active = versions.find(key);
    if (!accepting || active == versions.end()) {
      ++statsRejected;
      return false;
    }
    const auto generation = nextRequestGeneration();
    ++active->second.epoch;
    active->second.requestRevision = 0;
    latest.erase(key);
    active->second.inactive = true;
    PendingChunk state;
    state.key = key;
    state.generation = generation;
    state.lifecycleEpoch = active->second.epoch;
    state.unload = true;
    pending[key] = std::move(state);
    notePendingLocked();
    signalWorkerLocked(true);
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
              !active->second.inactive) {
            const auto current = latest.find(key);
            if (current == latest.end() ||
                current->second->revision <= snapshot->revision) {
              latest[key] = snapshot;
              for (const auto &change : snapshot->changedSections) {
                if (change.sky)
                  statsReceivedSkySections.fetch_add(
                      1, std::memory_order_relaxed);
                if (change.block)
                  statsReceivedBlockSections.fetch_add(
                      1, std::memory_order_relaxed);
              }
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
    result.skySectionChanges =
        statsSkyChanges.load(std::memory_order_relaxed);
    result.blockSectionChanges =
        statsBlockChanges.load(std::memory_order_relaxed);
    result.receivedSkySections =
        statsReceivedSkySections.load(std::memory_order_relaxed);
    result.receivedBlockSections =
        statsReceivedBlockSections.load(std::memory_order_relaxed);
    result.workerCpuMs = statsWorkerCpuMs.load(std::memory_order_relaxed);
    result.workerAffinityMask =
        statsWorkerAffinity.load(std::memory_order_relaxed);
    result.workerAffinityRequested =
        static_cast<std::uint64_t>(options.workerAffinityMask);
    result.workerAffinityError =
        statsWorkerAffinityError.load(std::memory_order_relaxed);
    std::lock_guard lock(mutex);
    result.pendingChunks = pending.size() + busyChunks;
    for (const auto &batch : published)
      result.pendingPublications += batch.size();
    return result;
  }

  void resetMeasurementStats() {
    statsSubmitted.store(0, std::memory_order_relaxed);
    statsRejected.store(0, std::memory_order_relaxed);
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
  }

  std::string workerError() const {
    std::lock_guard lock(mutex);
    return error;
  }

  bool waitIdleForTesting() {
    std::unique_lock lock(mutex);
    // The ring is drained by the worker, so idleness also means no queued cell
    // is still waiting to be coalesced into pending.
    idle.wait(lock, [&] {
      return !busy && pending.empty() && ringEmpty();
    });
    return error.empty();
  }

  bool waitWorkerSleepingForTesting() {
    std::unique_lock lock(mutex);
    sleepState.wait(lock, [&] {
      return workerSleeping.load(std::memory_order_acquire) || closing;
    });
    return workerSleeping.load(std::memory_order_acquire) && !closing;
  }

  void resumeForTesting() {
    std::lock_guard lock(mutex);
    paused = false;
    signalWorkerLocked(true);
  }

  void pauseForTesting() {
    std::lock_guard lock(mutex);
    paused = true;
  }

  bool processChunkBatchForTesting(int chunkX, int chunkZ) {
    const auto key = chunkKey(chunkX, chunkZ);
    std::unique_lock lock(mutex);
    if (!paused || busy || closing || forcedChunk)
      return false;
    // The paused worker cannot consume the ring. Coalesce its queued cells
    // under the same lock before checking for work on the requested chunk.
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

  void close() {
    // Close the hot-path gate before joining the worker. Records already in
    // the ring remain owned input and are deliberately exempt from the later
    // `accepting == false` check during draining.
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
  }

private:
  struct PendingChunk {
    std::uint64_t key = 0;
    std::uint64_t generation = 0;
    // Generation of the newest full authoritative snapshot. Every record at or
    // below this sequence is already represented by that snapshot.
    std::uint64_t inputSequence = 0;
    std::uint64_t lifecycleEpoch = 0;
    std::uint64_t revision = 0;
    std::unique_ptr<LightChunkInput> load;
    bool unload = false;
    std::unordered_map<std::uint32_t, LightBlockChange> changes;
    // Owned hot-path hand-off: one latest value per dirty cell. A full ring
    // merges here under the same sequence ordering as the normal worker drain.
    std::unordered_map<std::uint32_t, OwnedCell> ownedCells;
    std::unordered_map<int, SectionStatusValue> sectionStatus;
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
      const SkyColumnSources sources = computeSources(*chunk);
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
      // One scalar lookup per section instead of a chunk/section/value lookup
      // for every one of the 4096 cells. Every cell of every light section is
      // covered by its column's lit or dark range, so the working layer can be
      // filled directly; the per-cell stored-level comparison of the old path
      // correctly skipped the writes that already equalled the inherited value.
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
          std::fill(layer->begin(), layer->end(), std::uint8_t{15});
        } else if (minFirst >= 16) {
          std::fill(layer->begin(), layer->end(), std::uint8_t{0});
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
          seedColumn(key, chunk, x, z, sources[HeightmapColumnKey(x, z)]);
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
      eligibleValid = false;
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
            cacheValid = false;
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

    // Seeding walks one section at a time, so this layer cache turns the two
    // hash lookups of every storedLevel (working chunk, working section) into a
    // pointer compare. Any mutation of the value map invalidates it.
    const LightValues *cachedLayer(std::uint64_t chunkKeyValue, int sectionY) {
      if (cacheValid && cachedChunkKey == chunkKeyValue &&
          cachedSectionY == sectionY)
        return cachedLayerPtr;
      cacheValid = false;
      const SectionPos3 key{static_cast<int>(chunkKeyValue >> 32), sectionY,
                            static_cast<int>(
                                static_cast<std::uint32_t>(chunkKeyValue))};
      const auto found = values.find(key);
      if (found == values.end())
        return nullptr;
      cachedChunkKey = chunkKeyValue;
      cachedSectionY = sectionY;
      cachedLayerPtr = &found->second;
      cacheValid = true;
      return cachedLayerPtr;
    }

    const LightChunkSnapshot *stateFor(std::uint64_t key) const {
      if (cachedStateValid && cachedStateKey == key)
        return cachedStatePtr;
      cachedStateValid = false;
      if (removed.contains(key))
        return nullptr;
      const LightChunkSnapshot *result = nullptr;
      const auto current = working.find(key);
      if (current != working.end())
        result = current->second.snapshot.get();
      else {
        const auto stored = owner.internal.find(key);
        if (stored != owner.internal.end())
          result = stored->second.get();
      }
      cachedStateKey = key;
      cachedStatePtr = result;
      cachedStateValid = true;
      return result;
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
      cachedStateValid = false;
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
      const std::uint64_t chunkKeyValue =
          chunkKey(position.x >> 4, position.z >> 4);
      const int sectionY = position.y >> 4;
      if (cacheValid && cachedChunkKey == chunkKeyValue &&
          cachedSectionY == sectionY)
        return (*cachedLayerPtr)[lightIndex(position.x, position.y, position.z)];
      const LightChunkSnapshot *chunk = stateFor(chunkKeyValue);
      if (!chunk)
        return 0;
      const SectionPos3 key{position.x >> 4, sectionY, position.z >> 4};
      const auto found = values.find(key);
      if (found != values.end()) {
        cacheValid = true;
        cachedChunkKey = chunkKeyValue;
        cachedSectionY = sectionY;
        cachedLayerPtr = &found->second;
        return found->second[lightIndex(position.x, position.y, position.z)];
      }
      const LightSectionSnapshot *section = chunk->section(sectionY);
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

    // Bulk seeding needs the working layer of a whole section without the
    // per-cell value lookup. The eligibility rule of setStoredLevel still
    // applies: before this batch's storage eligibility is known every section
    // is writable, afterwards only eligible sections (or sections that already
    // hold a working value) are.
    LightValues *bulkValues(std::uint64_t chunkKeyValue, int sectionY) {
      if (sectionY < owner.minBlockSectionY - 1 ||
          sectionY > owner.minBlockSectionY + owner.blockSectionCount)
        return nullptr;
      const SectionPos3 key{static_cast<int>(chunkKeyValue >> 32), sectionY,
                            static_cast<int>(
                                static_cast<std::uint32_t>(chunkKeyValue))};
      const auto found = values.find(key);
      if (found != values.end())
        return &found->second;
      if (storageReady && !eligibleHere(key.x, key.y, key.z))
        return nullptr;
      const LightChunkSnapshot *chunk = stateFor(chunkKeyValue);
      const LightSectionSnapshot *section =
          chunk ? chunk->section(sectionY) : nullptr;
      if (!section)
        return nullptr;
      LightValues layer{};
      if (!section->layers[static_cast<std::size_t>(LightLayer::Sky)]
               .isMissing()) {
        for (int y = 0; y < 16; ++y)
          for (int z = 0; z < 16; ++z)
            for (int x = 0; x < 16; ++x)
              layer[lightIndex(x, y, z)] =
                  section->light(LightLayer::Sky, x, y, z);
      } else {
        for (int z = 0; z < 16; ++z) {
          for (int x = 0; x < 16; ++x) {
            int inherited = 15;
            for (int aboveY = sectionY + 1;
                 aboveY <
                 chunk->minBlockSectionY + chunk->blockSectionCount + 1;
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
      cacheValid = false;
      return &values.emplace(key, layer).first->second;
    }

    // Flat probe of the current batch's storedEligible set. A seeding pass
    // probes once per light section (26 times per chunk), so a single set
    // lookup is already cheap; the helper only keeps bulkValues readable.
    bool eligibleHere(int sectionX, int sectionY, int sectionZ) {
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

    bool setStoredLevel(LightPos position, int level) {
      const LightSectionSnapshot *section = sectionAt(position);
      if (!section)
        return false;
      const int current = storedLevel(position);
      if (current == level)
        return false;
      const SectionPos3 key{position.x >> 4, position.y >> 4,
                            position.z >> 4};
      if (storageReady && !eligibleHere(key.x, key.y, key.z) &&
          !values.contains(key))
        return false;
      auto inserted = values.try_emplace(key);
      cacheValid = false;
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
        const auto &bottom =
            chunk.snapshot->section(y >> 4)->at(x, y & 15, z);
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
      cachedStateValid = false;
      // Seeding always rebuilds one chunk's layers at a time; erase only that
      // chunk's sections instead of scanning every working value of the batch.
      for (int sectionY = owner.minBlockSectionY - 1;
           sectionY <= owner.minBlockSectionY + owner.blockSectionCount;
           ++sectionY)
        values.erase({chunk.chunkX, sectionY, chunk.chunkZ});
      cacheValid = false;
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
    // Layer cache for storedLevel, see cachedLayer.
    mutable bool cacheValid = false;
    mutable std::uint64_t cachedChunkKey = 0;
    mutable int cachedSectionY = 0;
    mutable const LightValues *cachedLayerPtr = nullptr;
    // stateFor is called for every cell too; cache its chunk lookup and drop
    // the cache whenever the working set or a section is replaced.
    mutable bool cachedStateValid = false;
    mutable std::uint64_t cachedStateKey = 0;
    mutable const LightChunkSnapshot *cachedStatePtr = nullptr;
    // Same idea for the per-batch storage eligibility: setStoredLevel probes it
    // for every cell it writes, but the answer only changes per section.
    mutable bool eligibleValid = false;
    mutable std::uint64_t eligibleChunkKey = 0;
    mutable int eligibleSectionY = 0;
    mutable bool eligibleResult = false;
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

  void reportBlockCell(LightCellUpdate update) {
    if (!validSection(update.sectionY) || update.localIndex >= kLightVolume) {
      ++statsRejected;
      return;
    }
    if (!acceptingCells.load(std::memory_order_acquire)) {
      ++statsRejected;
      return;
    }
    update.sequence = nextRequestGeneration();
    const auto head = ringHead.load(std::memory_order_relaxed);
    const auto tail = ringTail.load(std::memory_order_acquire);
    if (static_cast<std::size_t>(head - tail) >= ring.size()) {
      // The lock-free path is saturated. Preserve the owned record in the same
      // pending map the worker consumes; sequence ordering prevents an older
      // ring entry drained later from overwriting this newer value.
      std::lock_guard lock(mutex);
      applyOwnedCellLocked(update);
      signalWorkerLocked(true);
      return;
    }
    ring[static_cast<std::size_t>(head) & ringMask] = update;
    ringHead.store(head + 1, std::memory_order_release);
    notifyWorkerSleeping();
  }

  bool ringEmpty() const {
    return ringHead.load(std::memory_order_acquire) ==
           ringTail.load(std::memory_order_relaxed);
  }

  // Copy at most one bounded ring batch into the consumer's scratch buffer.
  void drainRing(std::vector<LightCellUpdate> &out) {
    const auto head = ringHead.load(std::memory_order_acquire);
    const auto tail = ringTail.load(std::memory_order_relaxed);
    std::size_t count = static_cast<std::size_t>(head - tail);
    count = std::min(count, ringBatchLimit);
    out.clear();
    if (count == 0)
      return;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
      out.push_back(
          ring[(static_cast<std::size_t>(tail) + i) & ringMask]);
    ringTail.store(tail + static_cast<std::uint32_t>(count),
                   std::memory_order_release);
  }

  // Collapse a drained ring batch to the newest owned value for each cell.
  void groupCellsLocked(const std::vector<LightCellUpdate> &cells,
                        std::vector<CoalescedSection> &sections) {
    sections.clear();
    if (cells.empty())
      return;
    sortedCells = cells;
    std::sort(sortedCells.begin(), sortedCells.end(),
              [](const LightCellUpdate &a, const LightCellUpdate &b) {
                if (a.chunkKey != b.chunkKey)
                  return a.chunkKey < b.chunkKey;
                if (a.lifecycleEpoch != b.lifecycleEpoch)
                  return a.lifecycleEpoch < b.lifecycleEpoch;
                if (a.sectionY != b.sectionY)
                  return a.sectionY < b.sectionY;
                if (a.localIndex != b.localIndex)
                  return a.localIndex < b.localIndex;
                return a.sequence < b.sequence;
              });
    for (const LightCellUpdate &cell : sortedCells) {
      if (sections.empty() || sections.back().chunkKey != cell.chunkKey ||
          sections.back().lifecycleEpoch != cell.lifecycleEpoch ||
          sections.back().sectionY != cell.sectionY) {
        sections.push_back({});
        sections.back().chunkKey = cell.chunkKey;
        sections.back().lifecycleEpoch = cell.lifecycleEpoch;
        sections.back().sectionY = cell.sectionY;
      }
      CoalescedSection &section = sections.back();
      if (cell.sequence > section.sectionSequence) {
        section.sectionSequence = cell.sequence;
        section.sectionEmpty = cell.sectionEmpty;
      }
      OwnedCell value{cell.sequence, cell.properties, cell.sectionEmpty};
      if (!section.cells.empty() &&
          section.cells.back().localIndex == cell.localIndex) {
        section.cells.back().value = value;
      } else {
        section.cells.push_back({cell.localIndex, value});
      }
    }
  }

  void drainCellChangesLocked() {
    // Bound the drain to one ring's worth of batches so continuous writes
    // cannot keep the worker here indefinitely.
    const std::size_t maxRounds =
        (ring.size() + ringBatchLimit - 1) / ringBatchLimit;
    for (std::size_t round = 0; round < maxRounds && !ringEmpty(); ++round) {
      drainRing(ringBatch);
      if (ringBatch.empty())
        break;
      groupCellsLocked(ringBatch, coalescedSections);
      enqueueCoalescedLocked(coalescedSections.data(), coalescedSections.size());
    }
  }

  void notePendingLocked() {
    ++statsSubmitted;
    noteHighWaterLocked();
  }

  // The queue length only grows when a chunk first becomes pending, so the CAS
  // is skipped whenever the current length is already at or below the mark.
  void noteHighWaterLocked() {
    const auto current = pending.size() + busyChunks;
    auto previous = statsHighWater.load(std::memory_order_relaxed);
    if (current <= previous)
      return;
    while (!statsHighWater.compare_exchange_weak(previous, current,
                                                 std::memory_order_relaxed)) {
      if (current <= previous)
        return;
    }
  }

  void awaitWork(std::unique_lock<std::mutex> &lock) {
    for (;;) {
      if (closing || (!paused && (!pending.empty() || !ringEmpty())))
        return;
      workerSleeping.store(true, std::memory_order_release);
      sleepState.notify_all();
      // Capture after advertising sleep, then re-check. A producer on either
      // side publishes the ring before advancing nextGeneration. If it runs
      // before the capture, the re-check sees its work; if it runs after, the
      // atomic wait is released by the newer sequence.
      const auto observed = wakeSequence.load(std::memory_order_acquire);
      if (closing || (!paused && (!pending.empty() || !ringEmpty()))) {
        workerSleeping.store(false, std::memory_order_release);
        return;
      }
      lock.unlock();
      wakeSequence.wait(observed, std::memory_order_acquire);
      lock.lock();
      workerSleeping.store(false, std::memory_order_release);
    }
  }

  void run() {
    const double workerCpuStart = currentThreadCpuMs();
    std::unique_lock lock(mutex);
    for (;;) {
      awaitWork(lock);
      if (closing && pending.empty() && ringEmpty())
        break;
      if (paused && !closing)
        continue;
      // Coalesce before taking a batch. A batch clones every section it
      // touches for copy-on-write publication, so waiting a few milliseconds
      // for more writes turns many small batches into one larger batch and
      // clones each hot section far less often. The window closes early as
      // soon as the batch is full, and closing must never wait.
      if (!closing && !forcedChunk && options.batchWindowMs > 0 &&
          pending.size() < options.batchSize) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(options.batchWindowMs);
        while (!closing && !paused && !forcedChunk &&
               pending.size() < options.batchSize &&
               std::chrono::steady_clock::now() < deadline)
          wake.wait_until(lock, deadline);
      }
      if (paused && !closing)
        continue;
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
      statsWorkerCpuMs.store(currentThreadCpuMs() - workerCpuStart,
                             std::memory_order_relaxed);
      if (pauseAfterBatch) {
        pauseAfterBatch = false;
        paused = true;
      }
      idle.notify_all();
      batchDone.notify_all();
      if (closing && pending.empty() && ringEmpty())
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
      for (const auto &[slot, value] : command.ownedCells) {
        const int sectionY =
            static_cast<int>(std::bit_cast<std::int32_t>(slot) >> 12);
        const std::uint16_t localIndex =
            static_cast<std::uint16_t>(slot & 0xFFFu);
        LightSectionSnapshot *section = chunk->sectionForEdit(sectionY);
        if (!section)
          throw std::runtime_error("light change references a missing section");
        const auto paletteIt = std::find(section->palette.begin(),
                                         section->palette.end(),
                                         value.properties);
        std::uint16_t paletteIndex = 0;
        if (paletteIt == section->palette.end()) {
          if (section->palette.size() >= 65536)
            throw std::runtime_error("light section palette exceeds uint16");
          paletteIndex = static_cast<std::uint16_t>(section->palette.size());
          section->palette.push_back(value.properties);
        } else {
          paletteIndex = static_cast<std::uint16_t>(
              paletteIt - section->palette.begin());
        }
        if (section->cells.empty()) {
          section->cells.assign(kLightVolume, 0);
          if (!section->palette.front().supported)
            section->unsupportedCells = kLightVolume;
        }
        const auto oldIndex = section->cells[localIndex];
        if (!section->palette[oldIndex].supported)
          --section->unsupportedCells;
        section->cells[localIndex] = paletteIndex;
        if (!section->palette[paletteIndex].supported)
          ++section->unsupportedCells;
        ++statsChanges;
        changedPositions.push_back(
            {chunk->chunkX * 16 + static_cast<int>(localIndex & 15),
             sectionY * 16 + static_cast<int>((localIndex >> 8) & 15),
             chunk->chunkZ * 16 + static_cast<int>((localIndex >> 4) & 15)});
      }
      for (const auto &[sectionY, status] : command.sectionStatus) {
        if (auto *section = chunk->sectionForEdit(sectionY))
          section->empty = status.empty;
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
    std::uint64_t skySectionChanges = 0, blockSectionChanges = 0;
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
      chunk.snapshot->deltaBaseRevision = 0;
      if (chunk.snapshot->protocolReady) {
        const auto base = lastReady.find(key);
        if (base != lastReady.end() && base->second &&
            base->second->lifecycleEpoch == chunk.snapshot->lifecycleEpoch &&
            base->second->minLightSectionY() ==
                chunk.snapshot->minLightSectionY() &&
            base->second->lightSectionCount() ==
                chunk.snapshot->lightSectionCount())
          chunk.snapshot->deltaBaseRevision = base->second->revision;
        for (const auto &now : chunk.snapshot->sections) {
          bool skyChanged = true;
          bool blockChanged = true;
          if (chunk.snapshot->deltaBaseRevision != 0) {
            const LightSectionSnapshot *before =
                base->second->section(now->sectionY);
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
          skySectionChanges += skyChanged ? 1 : 0;
          blockSectionChanges += blockChanged ? 1 : 0;
        }
      }
      publication.emplace_back(key, chunk.snapshot);
    }
    if (options.collectDiagnostics) {
      statsSkyChanges.fetch_add(skySectionChanges, std::memory_order_relaxed);
      statsBlockChanges.fetch_add(blockSectionChanges,
                                  std::memory_order_relaxed);
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
            if (entry.second->protocolReady &&
                newEntry.second->protocolReady &&
                entry.second->lifecycleEpoch ==
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
              newEntry.second->deltaBaseRevision =
                  entry.second->deltaBaseRevision;
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
        if (snapshot->protocolReady)
          lastReady[key] = snapshot;
        committed.emplace_back(key, std::move(snapshot));
      }
      statsPublished.fetch_add(committed.size(), std::memory_order_relaxed);
      published.push_back(std::move(committed));
    }
    for (std::uint64_t key : removed) {
      internal.erase(key);
      lastReady.erase(key);
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
  std::condition_variable sleepState;
  std::unordered_map<std::uint64_t, PendingChunk> pending;
  // Single-producer owned-cell ring. A full ring hands the newest owned record
  // to the pending map; the generation sequence prevents a later ring drain
  // from replacing it with older input.
  std::vector<LightCellUpdate> ring;
  std::size_t ringMask = 0;
  std::size_t ringBatchLimit = 1024;
  std::atomic<std::uint32_t> ringHead{0};
  std::atomic<std::uint32_t> ringTail{0};
  std::vector<LightCellUpdate> ringBatch;
  std::vector<LightCellUpdate> sortedCells;
  std::vector<CoalescedSection> coalescedSections;
  struct ChunkVersion {
    std::uint64_t epoch = 0;
    std::uint64_t requestRevision = 0;
    // Merged from the former inactive set: block-change reporting probed it
    // with a second hash lookup on every write.
    bool inactive = false;
  };
  std::unordered_map<std::uint64_t, ChunkVersion> versions;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      latest;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      internal;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      lastReady;
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
  std::atomic<std::uint64_t> nextGeneration{0};
  std::atomic<std::uint64_t> wakeSequence{0};
  // Only the worker sets this before waiting. Producers exchange it before an
  // atomic notify, so notify-before-wait cannot strand a generation.
  std::atomic<bool> workerSleeping{false};
  std::atomic<bool> acceptingCells{true};
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
  mutable std::atomic<std::uint64_t> statsSkyChanges{0};
  mutable std::atomic<std::uint64_t> statsBlockChanges{0};
  mutable std::atomic<std::uint64_t> statsReceivedSkySections{0};
  mutable std::atomic<std::uint64_t> statsReceivedBlockSections{0};
  mutable std::atomic<double> statsWorkerCpuMs{0};
  mutable std::atomic<std::uint64_t> statsWorkerAffinity{0};
  mutable std::atomic<std::uint32_t> statsWorkerAffinityError{0};
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

void LightService::reportBlockChanges(
    const std::vector<LightBlockChange> &changes) {
  impl_->reportBlockChanges(changes);
}

void LightService::reportBlockCell(LightCellUpdate update) {
  impl_->reportBlockCell(update);
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
void LightService::resetMeasurementStats() {
  impl_->resetMeasurementStats();
}
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
