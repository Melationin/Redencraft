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
  if (cells.size() != kLightVolume)
    throw std::invalid_argument("light section needs exactly 4096 cells");
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
  return palette.at(cells.at(lightIndex(x, y, z)));
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
    if (options.batchSize == 0 || options.maxQueuedCommands == 0 ||
        options.maxBlockChangesPerBatch == 0)
      throw std::invalid_argument("light worker limits must be positive");
    worker = std::thread([this] { run(); });
  }

  ~Impl() { close(); }

  std::uint64_t submitChunk(LightChunkInput input) {
    validateInput(input);
    const auto key = chunkKey(input.chunkX, input.chunkZ);
    std::lock_guard lock(mutex);
    if (!accepting) {
      ++statsRejected;
      return 0;
    }
    const auto generation = ++generations[key];
    latest.erase(key);
    if (queue.size() >= options.maxQueuedCommands) {
      ++statsRejected;
      inactive.insert(key);
      return 0;
    }
    inactive.erase(key);
    Command command;
    command.type = CommandType::Load;
    command.key = key;
    command.generation = generation;
    command.input = std::make_unique<LightChunkInput>(std::move(input));
    queue.push_back(std::move(command));
    noteEnqueueLocked();
    wake.notify_one();
    return generation;
  }

  bool reportSectionStatus(int chunkX, int chunkZ, int sectionY, bool empty) {
    const auto key = chunkKey(chunkX, chunkZ);
    std::lock_guard lock(mutex);
    auto active = generations.find(key);
    if (!accepting || active == generations.end() || inactive.contains(key))
      return false;
    const auto generation = ++active->second;
    latest.erase(key);
    if (queue.size() >= options.maxQueuedCommands) {
      ++statsRejected;
      return false;
    }
    Command command;
    command.type = CommandType::SectionStatus;
    command.key = key;
    command.generation = generation;
    command.sectionY = sectionY;
    command.empty = empty;
    queue.push_back(std::move(command));
    noteEnqueueLocked();
    wake.notify_one();
    return true;
  }

  bool reportBlockChange(LightBlockChange change) {
    const auto key = chunkKey(change.chunkX, change.chunkZ);
    std::lock_guard lock(mutex);
    auto active = generations.find(key);
    if (!accepting || active == generations.end() || inactive.contains(key))
      return false;
    if (!hasDifferentLightProperties(change.oldProperties, change.newProperties) &&
        !hasDifferentSectionLightProperties(change.wasEmpty, change.isEmpty))
      return true;
    change.generation = ++active->second;
    latest.erase(key);
    if (!queue.empty() && queue.back().type == CommandType::BlockChanges &&
        queue.back().key == key &&
        queue.back().changes.size() < options.maxBlockChangesPerBatch) {
      queue.back().generation = change.generation;
      queue.back().changes.push_back(change);
      ++pendingBlockChanges;
      return true;
    }
    if (queue.size() >= options.maxQueuedCommands) {
      rejectLocked(key);
      return false;
    }
    Command command;
    command.type = CommandType::BlockChanges;
    command.key = key;
    command.generation = change.generation;
    command.changes.push_back(change);
    queue.push_back(std::move(command));
    ++pendingBlockChanges;
    noteEnqueueLocked();
    wake.notify_one();
    return true;
  }

  bool unloadChunk(int chunkX, int chunkZ) {
    const auto key = chunkKey(chunkX, chunkZ);
    std::lock_guard lock(mutex);
    auto active = generations.find(key);
    if (active == generations.end())
      return false;
    const auto generation = ++active->second;
    latest.erase(key);
    inactive.insert(key);
    if (!accepting)
      return false;
    if (queue.size() >= options.maxQueuedCommands) {
      ++statsRejected;
      return false;
    }
    Command command;
    command.type = CommandType::Unload;
    command.key = key;
    command.generation = generation;
    queue.push_back(std::move(command));
    noteEnqueueLocked();
    wake.notify_one();
    return true;
  }

  void drainPublished() {
    std::vector<Published> ready;
    std::vector<std::shared_ptr<const LightChunkSnapshot>> callbacks;
    std::function<void(std::shared_ptr<const LightChunkSnapshot>)> callback;
    {
      std::lock_guard lock(mutex);
      ready.swap(published);
      callback = publishCallback;
      for (auto &item : ready) {
        const auto active = generations.find(item.key);
        if (active != generations.end() &&
            active->second == item.snapshot->generation &&
            !inactive.contains(item.key)) {
          latest[item.key] = item.snapshot;
          if (callback) callbacks.push_back(item.snapshot);
        } else {
          ++statsStale;
        }
      }
    }
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
    return result;
  }

  std::string workerError() const {
    std::lock_guard lock(mutex);
    return error;
  }

  bool waitIdleForTesting() {
    std::unique_lock lock(mutex);
    idle.wait(lock, [&] { return !busy && queue.empty(); });
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
  enum class CommandType { Load, SectionStatus, BlockChanges, Unload };
  struct Command {
    CommandType type = CommandType::Load;
    std::uint64_t key = 0;
    std::uint64_t generation = 0;
    int sectionY = 0;
    bool empty = false;
    std::unique_ptr<LightChunkInput> input;
    std::vector<LightBlockChange> changes;
  };
  struct Published {
    std::uint64_t key = 0;
    std::shared_ptr<const LightChunkSnapshot> snapshot;
  };

  void validateInput(const LightChunkInput &input) const {
    if (input.minBlockSectionY != minBlockSectionY ||
        input.blockSectionCount != blockSectionCount)
      throw std::invalid_argument("light chunk vertical range differs from service");
    if (input.sections.size() != static_cast<std::size_t>(blockSectionCount + 2))
      throw std::invalid_argument("light chunk needs block sections plus two padding sections");
    for (std::size_t i = 0; i < input.sections.size(); ++i) {
      if (!input.sections[i] ||
          input.sections[i]->sectionY != minBlockSectionY - 1 + static_cast<int>(i))
        throw std::invalid_argument("light chunk sections are not contiguous");
    }
  }

  std::uint64_t rejectLocked(std::uint64_t key) {
    ++statsRejected;
    latest.erase(key);
    return 0;
  }

  void noteEnqueueLocked() {
    ++statsSubmitted;
    const auto current = queue.size() + static_cast<std::size_t>(busy ? 1 : 0);
    auto previous = statsHighWater.load(std::memory_order_relaxed);
    while (current > previous &&
           !statsHighWater.compare_exchange_weak(previous, current,
                                                 std::memory_order_relaxed)) {
    }
  }

  void run() {
    std::unique_lock lock(mutex);
    for (;;) {
      wake.wait(lock, [&] { return closing || !paused; });
      if (closing && queue.empty())
        break;
      wake.wait(lock, [&] { return closing || !queue.empty(); });
      if (closing && queue.empty())
        break;
      std::vector<Command> batch;
      const std::size_t count = std::min(options.batchSize, queue.size());
      batch.reserve(count);
      std::size_t changes = 0;
      for (std::size_t i = 0; i < count; ++i) {
        changes += queue.front().changes.size();
        batch.push_back(std::move(queue.front()));
        queue.pop_front();
      }
      busy = true;
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
      pendingBlockChanges -= std::min(pendingBlockChanges, changes);
      ++statsBatches;
      statsChanges += changes;
      idle.notify_all();
      if (closing && queue.empty())
        break;
    }
  }

  void processBatch(std::vector<Command> &batch) {
    struct WorkingChunk {
      std::vector<std::shared_ptr<const LightSectionSnapshot>> sections;
      std::unordered_set<int> copiedSections;
    };
    std::unordered_map<std::uint64_t, WorkingChunk> working;
    std::unordered_set<std::uint64_t> dirty;
    std::unordered_set<std::uint64_t> removed;

    const auto ensure = [&](std::uint64_t key) -> WorkingChunk & {
      auto it = working.find(key);
      if (it != working.end())
        return it->second;
      const auto existing = internal.find(key);
      WorkingChunk next;
      if (existing != internal.end())
        next.sections = existing->second->sections;
      auto inserted = working.emplace(key, std::move(next)).first;
      return inserted->second;
    };

    for (Command &command : batch) {
      if (command.type == CommandType::Unload) {
        internal.erase(command.key);
        working.erase(command.key);
        removed.insert(command.key);
        continue;
      }
      if (command.type == CommandType::Load) {
        removed.erase(command.key);
        auto snapshot = std::make_shared<LightChunkSnapshot>();
        snapshot->chunkX = command.input->chunkX;
        snapshot->chunkZ = command.input->chunkZ;
        snapshot->minBlockSectionY = command.input->minBlockSectionY;
        snapshot->blockSectionCount = command.input->blockSectionCount;
        snapshot->generation = command.generation;
        snapshot->sections = std::move(command.input->sections);
        for (const auto &section : snapshot->sections)
          snapshot->unsupportedCells += section->unsupportedCells;
        internal[command.key] = snapshot;
        WorkingChunk next;
        next.sections = snapshot->sections;
        working[command.key] = std::move(next);
        dirty.insert(command.key);
        continue;
      }
      auto found = internal.find(command.key);
      if (found == internal.end() ||
          command.generation < found->second->generation)
        continue;
      WorkingChunk &chunk = ensure(command.key);
      if (command.type == CommandType::SectionStatus) {
        const auto index = vectorIndex(chunk.sections, command.sectionY);
        if (index >= chunk.sections.size())
          continue;
        auto section = chunk.copiedSections.contains(command.sectionY)
                           ? std::static_pointer_cast<const LightSectionSnapshot>(
                                 chunk.sections[index])
                           : std::make_shared<LightSectionSnapshot>(
                                 *chunk.sections[index]);
        auto mutableSection =
            std::const_pointer_cast<LightSectionSnapshot>(section);
        mutableSection->empty = command.empty;
        chunk.sections[index] = mutableSection;
        chunk.copiedSections.insert(command.sectionY);
      } else if (command.type == CommandType::BlockChanges) {
        for (const LightBlockChange &change : command.changes) {
          const auto index = vectorIndex(chunk.sections, change.sectionY);
          if (index >= chunk.sections.size() || change.localIndex >= kLightVolume)
            continue;
          auto mutableSection =
              chunk.copiedSections.contains(change.sectionY)
                  ? std::const_pointer_cast<LightSectionSnapshot>(
                        chunk.sections[index])
                  : std::make_shared<LightSectionSnapshot>(*chunk.sections[index]);
          auto paletteIt = std::find(mutableSection->palette.begin(),
                                    mutableSection->palette.end(),
                                    change.newProperties);
          std::uint16_t paletteIndex = 0;
          if (paletteIt == mutableSection->palette.end()) {
            if (mutableSection->palette.size() >= 65536)
              continue;
            paletteIndex = static_cast<std::uint16_t>(mutableSection->palette.size());
            mutableSection->palette.push_back(change.newProperties);
          } else {
            paletteIndex = static_cast<std::uint16_t>(
                paletteIt - mutableSection->palette.begin());
          }
          const auto oldIndex = mutableSection->cells[change.localIndex];
          if (!mutableSection->palette[oldIndex].supported)
            --mutableSection->unsupportedCells;
          mutableSection->cells[change.localIndex] = paletteIndex;
          if (!change.newProperties.supported)
            ++mutableSection->unsupportedCells;
          mutableSection->empty = change.isEmpty;
          chunk.sections[index] = mutableSection;
          chunk.copiedSections.insert(change.sectionY);
        }
      }
      dirty.insert(command.key);
    }

    // L-1 intentionally stops at the PRE_UPDATE boundary.
    // TODO(L-2a/L-2b): run block/sky propagation here and only then set
    // propagationComplete; no placeholder brightness is written in L-1.
    for (auto &[key, chunk] : working) {
      if (removed.contains(key) || !dirty.contains(key))
        continue;
      auto snapshot = std::make_shared<LightChunkSnapshot>();
      const auto old = internal.find(key);
      snapshot->chunkX = static_cast<int>(key >> 32);
      snapshot->chunkZ = static_cast<int>(static_cast<std::uint32_t>(key));
      snapshot->minBlockSectionY = minBlockSectionY;
      snapshot->blockSectionCount = blockSectionCount;
      snapshot->sections = std::move(chunk.sections);
      snapshot->generation = old == internal.end() ? 0 : old->second->generation;
      for (const auto &command : batch)
        if (command.key == key)
          snapshot->generation = std::max(snapshot->generation, command.generation);
      snapshot->unsupportedCells = 0;
      for (const auto &section : snapshot->sections)
        snapshot->unsupportedCells += section->unsupportedCells;
      internal[key] = snapshot;
      std::lock_guard lock(mutex);
      published.push_back({key, snapshot});
      ++statsPublished;
    }
  }

  int minBlockSectionY;
  int blockSectionCount;
  LightWorkerOptions options;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable idle;
  std::deque<Command> queue;
  std::unordered_map<std::uint64_t, std::uint64_t> generations;
  std::unordered_set<std::uint64_t> inactive;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      latest;
  std::unordered_map<std::uint64_t, std::shared_ptr<const LightChunkSnapshot>>
      internal;
  std::vector<Published> published;
  std::function<void(std::shared_ptr<const LightChunkSnapshot>)> publishCallback;
  std::thread worker;
  bool accepting = true;
  bool closing = false;
  bool busy = false;
  bool paused = false;
  std::size_t pendingBlockChanges = 0;
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
