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

#include "light_metadata.inc"

const BlockLightProperties kUnsupportedLightProperties{};

std::uint64_t chunkKey(int x, int z) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
         static_cast<std::uint32_t>(z);
}

bool validLightValue(std::uint8_t value) { return value <= 15; }

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
    const auto generation = ++generations[key];
    latest.erase(key);
    inactive.erase(key);
    const bool preserveUnload = pending.contains(key) && pending.at(key).unload;
    PendingChunk state;
    state.key = key;
    state.generation = generation;
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
    auto active = generations.find(key);
    if (!accepting || active == generations.end() || inactive.contains(key)) {
      ++statsRejected;
      return false;
    }
    const auto generation = ++active->second;
    latest.erase(key);
    PendingChunk &state = pending[key];
    state.key = key;
    state.generation = generation;
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
    auto active = generations.find(key);
    if (!accepting || active == generations.end() || inactive.contains(key)) {
      ++statsRejected;
      return false;
    }
    change.generation = ++active->second;
    latest.erase(key);
    PendingChunk &state = pending[key];
    state.key = key;
    state.generation = change.generation;
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
    auto active = generations.find(key);
    if (!accepting || active == generations.end()) {
      ++statsRejected;
      return false;
    }
    const auto generation = ++active->second;
    latest.erase(key);
    inactive.insert(key);
    PendingChunk state;
    state.key = key;
    state.generation = generation;
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
        const auto active = generations.find(key);
        if (active != generations.end() &&
            active->second == snapshot->generation &&
            !inactive.contains(key)) {
          latest[key] = snapshot;
          if (callback)
            callbacks.push_back(std::move(snapshot));
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
    std::unique_ptr<LightChunkInput> load;
    bool unload = false;
    std::unordered_map<std::uint32_t, LightBlockChange> changes;
    std::unordered_map<int, bool> sectionStatus;
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
    for (PendingChunk &command : batch) {
      const auto key = command.key;
      if (command.unload) {
        internal.erase(key);
        if (!command.load)
          continue;
      }

      const auto old = internal.find(key);
      if (!command.load &&
          (old == internal.end() ||
           command.generation < old->second->generation))
        continue;

      std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;
      int chunkX = 0;
      int chunkZ = 0;
      if (command.load) {
        chunkX = command.load->chunkX;
        chunkZ = command.load->chunkZ;
        sections = command.load->sections;
      } else {
        chunkX = static_cast<int>(key >> 32);
        chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
        sections = old->second->sections;
      }

      std::unordered_map<int, std::shared_ptr<LightSectionSnapshot>>
          mutableSections;
      const auto mutableSection = [&](int sectionY) -> LightSectionSnapshot * {
        const auto working = mutableSections.find(sectionY);
        if (working != mutableSections.end())
          return working->second.get();
        const auto index = vectorIndex(sections, sectionY);
        if (index >= sections.size())
          return nullptr;
        // Allocate a genuinely mutable worker-owned copy. The previous
        // const_cast of make_shared<const T> would mutate a const object.
        auto copy = std::make_shared<LightSectionSnapshot>(*sections[index]);
        sections[index] = copy;
        auto inserted = mutableSections.emplace(sectionY, std::move(copy)).first;
        return inserted->second.get();
      };

      for (const auto &[slot, change] : command.changes) {
        (void)slot;
        LightSectionSnapshot *section = mutableSection(change.sectionY);
        if (!section)
          throw std::runtime_error("light change references a missing section");
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
          paletteIndex =
              static_cast<std::uint16_t>(paletteIt - section->palette.begin());
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
      }
      // Cell-key iteration is intentionally unordered. Apply the final empty
      // state only after every cell write, using the state captured at enqueue
      // time, so a historical intermediate emptiness cannot win.
      for (const auto &[sectionY, empty] : command.sectionStatus) {
        if (auto *section = mutableSection(sectionY))
          section->empty = empty;
      }

      auto snapshot = std::make_shared<LightChunkSnapshot>();
      snapshot->chunkX = chunkX;
      snapshot->chunkZ = chunkZ;
      snapshot->minBlockSectionY = minBlockSectionY;
      snapshot->blockSectionCount = blockSectionCount;
      snapshot->generation = command.generation;
      snapshot->sections = std::move(sections);
      for (const auto &section : snapshot->sections)
        snapshot->unsupportedCells += section->unsupportedCells;
      internal[key] = snapshot;

      std::lock_guard lock(mutex);
      published[key] = snapshot;
      ++statsPublished;
    }
  }

  int minBlockSectionY;
  int blockSectionCount;
  LightWorkerOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable idle;
  std::unordered_map<std::uint64_t, PendingChunk> pending;
  std::unordered_map<std::uint64_t, std::uint64_t> generations;
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
  bool paused = false;
  std::string error;
  mutable std::atomic<std::uint64_t> statsSubmitted{0};
  mutable std::atomic<std::uint64_t> statsRejected{0};
  mutable std::atomic<std::uint64_t> statsBatches{0};
  mutable std::atomic<std::uint64_t> statsChanges{0};
  mutable std::atomic<std::uint64_t> statsPublished{0};
  mutable std::atomic<std::uint64_t> statsStale{0};
  mutable std::atomic<std::uint64_t> statsHighWater{0};
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
void LightService::resumeForTesting() { impl_->resumeForTesting(); }
void LightService::close() { impl_->close(); }

} // namespace redstone
