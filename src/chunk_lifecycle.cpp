#include "redstone/chunk_lifecycle.hpp"
#include <atomic>

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace redstone {
namespace {

bool rootMatchesPosition(const nbt::RootTag &root,
                         const ChunkTicketPos &position, std::string &error) {
  if (root.value.type != 10) {
    error = "owned NBT root is not a compound";
    return false;
  }
  const nbt::Tag *x = root.value.get("xPos");
  const nbt::Tag *z = root.value.get("zPos");
  if (!x || !z || x->type != 3 || z->type != 3 ||
      x->number != position.x || z->number != position.z) {
    error = "owned NBT root coordinates do not match save position";
    return false;
  }
  return true;
}

ChunkLoadFailure mapReadFailure(StoreReadStatus status) {
  switch (status) {
  case StoreReadStatus::Missing:
    return ChunkLoadFailure::Missing;
  case StoreReadStatus::Unsupported:
    return ChunkLoadFailure::Unsupported;
  case StoreReadStatus::Corrupt:
    return ChunkLoadFailure::Corrupt;
  case StoreReadStatus::Found:
  case StoreReadStatus::Io:
    return ChunkLoadFailure::Io;
  }
  return ChunkLoadFailure::Io;
}

} // namespace

RegionFileStore::RegionFileStore(std::filesystem::path regionDirectory)
    : regionDirectory_(std::move(regionDirectory)) {}

StoreReadResult RegionFileStore::readChunk(const ChunkTicketPos &position) {
  try {
    region::ReadResult raw =
        region::readChunkNbt(regionDirectory_, position.x, position.z);
    StoreReadResult result;
    result.status =
        raw.status == region::ReadStatus::Found
            ? StoreReadStatus::Found
            : raw.status == region::ReadStatus::Missing
                  ? StoreReadStatus::Missing
                  : raw.status == region::ReadStatus::Unsupported
                        ? StoreReadStatus::Unsupported
                        : StoreReadStatus::Corrupt;
    result.nbt = std::move(raw.nbt);
    result.detail = std::move(raw.detail);
    return result;
  } catch (const std::exception &error) {
    return {StoreReadStatus::Io, {}, error.what()};
  }
}

StoreWriteResult RegionFileStore::writeChunk(
    const ChunkTicketPos &position, std::span<const std::uint8_t> bytes,
    int timestampSeconds) {
  try {
    region::WriteResult raw = region::writeChunkNbt(
        regionDirectory_, position.x, position.z, bytes, timestampSeconds);
    return {raw.wrote, raw.cleanupPending,
            raw.cleanupPending ? raw.warning : std::string{}};
  } catch (const std::exception &error) {
    return {false, false, error.what()};
  }
}

SerializedChunkStore::SerializedChunkStore(
    std::shared_ptr<RawChunkStore> inner)
    : inner_(std::move(inner)) {}

StoreReadResult SerializedChunkStore::readChunk(
    const ChunkTicketPos &position) {
  std::lock_guard lock(mutex_);
  return inner_->readChunk(position);
}

StoreWriteResult SerializedChunkStore::writeChunk(
    const ChunkTicketPos &position, std::span<const std::uint8_t> bytes,
    int timestampSeconds) {
  std::lock_guard lock(mutex_);
  return inner_->writeChunk(position, bytes, timestampSeconds);
}

RegionEntityStore::RegionEntityStore(
    std::filesystem::path entityDirectory)
    : entityDirectory_(std::move(entityDirectory)) {}

StoreReadResult RegionEntityStore::readEntities(
    const ChunkTicketPos &position) {
  try {
    region::ReadResult raw =
        region::readChunkNbt(entityDirectory_, position.x, position.z);
    StoreReadResult result;
    result.status =
        raw.status == region::ReadStatus::Found
            ? StoreReadStatus::Found
            : raw.status == region::ReadStatus::Missing
                  ? StoreReadStatus::Missing
                  : raw.status == region::ReadStatus::Unsupported
                        ? StoreReadStatus::Unsupported
                        : StoreReadStatus::Corrupt;
    result.nbt = std::move(raw.nbt);
    result.detail = std::move(raw.detail);
    return result;
  } catch (const std::exception &error) {
    return {StoreReadStatus::Io, {}, error.what()};
  }
}

StoreWriteResult RegionEntityStore::writeEntities(
    const ChunkTicketPos &position, std::span<const std::uint8_t> bytes,
    int timestampSeconds) {
  try {
    region::WriteResult raw = region::writeChunkNbt(
        entityDirectory_, position.x, position.z, bytes, timestampSeconds);
    return {raw.wrote, raw.cleanupPending,
            raw.cleanupPending ? raw.warning : std::string{}};
  } catch (const std::exception &error) {
    return {false, false, error.what()};
  }
}

class ChunkLifecycleController::Impl {
private:
  struct WorkerLedgerSample {
    std::size_t epochs = 0;
    std::size_t written = 0;
    std::size_t templates = 0;
    std::size_t entityTemplates = 0;
  };
  struct Entry {
    ChunkLifecycleStatus status;
    bool resident = false;
    bool evictAfterSave = false;
    std::size_t inFlightSaves = 0;
    std::uint64_t lastAcceptedSnapshotVersion = 0;
    std::uint64_t workerWrittenVersion = 0;
  };

public:
  Impl(std::shared_ptr<RawChunkStore> store,
       std::shared_ptr<RawEntityStore> entityStore,
       ChunkResidentCallbacks callbacks)
      : store_(std::move(store)), entityStore_(std::move(entityStore)),
        callbacks_(std::move(callbacks)) {
    if (!store_)
      throw std::invalid_argument("chunk lifecycle requires a raw store");
    worker_ = std::thread([this] { runWorker(); });
  }

  ~Impl() { close(); }

  bool updateDemand(const ChunkTicketPos &position, int loadingLevel) {
    if (!accepting())
      return false;
    const bool newDemand = loadingLevel <= ticket_levels::FullChunkLevel;
    auto found = entries_.find(position);
    if (found == entries_.end()) {
      if (!newDemand)
        return true;
      Entry &entry = entries_[position];
      entry.status.demandLevel = loadingLevel;
      entry.status.demand = true;
      return beginRead(position, entry);
    }
    Entry &entry = found->second;
    const bool oldDemand = entry.status.demand;
    entry.status.demandLevel = loadingLevel;
    entry.status.demand = newDemand;
    if (oldDemand == newDemand)
      return true;
    if (newDemand) {
      entry.evictAfterSave = false;
      if (!entry.resident &&
          (entry.status.state == ChunkResidencyState::Failed ||
           entry.status.state == ChunkResidencyState::Missing))
        return beginRead(position, entry);
      if (entry.resident) {
        if (entry.status.state != ChunkResidencyState::Saving)
          entry.status.state = ChunkResidencyState::Resident;
        return true;
      }
      if (entry.status.state == ChunkResidencyState::Reading)
        return true;
      return beginRead(position, entry);
    }

    if (!entry.resident && entry.inFlightSaves == 0 &&
        (entry.status.state == ChunkResidencyState::Failed ||
         entry.status.state == ChunkResidencyState::Missing ||
         entry.status.state == ChunkResidencyState::Absent)) {
      enqueue(Command::drop(position, entry.status.epoch));
      entries_.erase(found);
      return true;
    }
    if (entry.status.state == ChunkResidencyState::Reading) {
      const std::uint64_t invalidEpoch = ++nextEpoch_;
      entry.status.epoch = invalidEpoch;
      entry.status.state = ChunkResidencyState::Absent;
      entry.status.failure = ChunkLoadFailure::None;
      entry.status.detail.clear();
      enqueue(Command::invalidate(position, invalidEpoch));
      enqueue(Command::drop(position, invalidEpoch));
      pendingGenerated_.erase(position);
      entries_.erase(found);
      return true;
    }
    if (entry.status.state == ChunkResidencyState::Saving) {
      entry.evictAfterSave = true;
      return true;
    }
    if (entry.resident) {
      entry.evictAfterSave = true;
      entry.status.state = ChunkResidencyState::EvictPending;
      attemptEvict(position, entry);
    }
    return true;
  }

  bool applyLoadingChanges(std::span<const ChunkLevelChange> changes) {
    bool accepted = true;
    for (const ChunkLevelChange &change : changes)
      accepted = updateDemand(change.position, change.newLevel) && accepted;
    return accepted;
  }

  bool markDirty(const ChunkTicketPos &position, std::uint64_t version) {
    auto found = entries_.find(position);
    if (found == entries_.end() || !found->second.resident ||
        !accepting())
      return false;
    Entry &entry = found->second;
    entry.status.dirtyVersion =
        std::max(entry.status.dirtyVersion, version);
    if (!entry.status.demand &&
        entry.status.state == ChunkResidencyState::EvictPending)
      entry.status.state = ChunkResidencyState::Resident;
    return true;
  }

  bool acceptSnapshotVersion(Entry &entry,
                             std::uint64_t snapshotVersion) {
    if (snapshotVersion == 0 ||
        snapshotVersion < entry.lastAcceptedSnapshotVersion ||
        snapshotVersion <= entry.workerWrittenVersion ||
        (snapshotVersion == entry.lastAcceptedSnapshotVersion &&
         entry.inFlightSaves != 0))
      return false;
    entry.lastAcceptedSnapshotVersion = snapshotVersion;
    return true;
  }

  bool requestSave(const ChunkTicketPos &position,
                   std::uint64_t snapshotVersion,
                   std::vector<terrain::BlockEdit> edits) {
    // Expanded BlockEdit input remains fixture compatibility only. Normalize
    // it into the same ordered, deduplicated compact representation used by
    // WorldChunkManager's production path.
    std::map<std::uint32_t, BlockState::Id> cells;
    for (const terrain::BlockEdit &edit : edits) {
      const auto compact = terrain::compactEdit(edit);
      cells[terrain::compactCellKey(compact)] = compact.stateId;
    }
    std::vector<terrain::CompactBlockEdit> compact;
    compact.reserve(cells.size());
    for (const auto &[cell, stateId] : cells) {
      const auto sectionBits = static_cast<std::uint8_t>(cell >> 12);
      compact.push_back({std::bit_cast<std::int8_t>(sectionBits),
                         static_cast<std::uint16_t>(cell & 0xfff), stateId});
    }
    ChunkSavePayload payload;
    if (!compact.empty())
      payload.edits =
          std::make_shared<const std::vector<terrain::CompactBlockEdit>>(
              std::move(compact));
    return requestSavePayload(position, snapshotVersion, std::move(payload));
  }

  bool requestSavePayload(const ChunkTicketPos &position,
                          std::uint64_t snapshotVersion,
                          ChunkSavePayload payload) {
    auto found = entries_.find(position);
    if (found == entries_.end() || !found->second.resident ||
        !accepting())
      return false;
    Entry &entry = found->second;
    if (!acceptSnapshotVersion(entry, snapshotVersion))
      return false;
    entry.status.dirtyVersion =
        std::max(entry.status.dirtyVersion, snapshotVersion);
    Command command = Command::save(position, entry.status.epoch,
                                    snapshotVersion, false);
    command.edits = std::move(payload.edits);
    command.ticks = std::move(payload.ticks);
    command.movingPistons = std::move(payload.movingPistons);
    command.entityRecords = std::move(payload.entityRecords);
    command.light = std::move(payload.light);
    if (!enqueue(std::move(command)))
      return false;
    ++entry.inFlightSaves;
    entry.status.state = ChunkResidencyState::Saving;
    entry.status.detail.clear();
    if (!entry.status.demand)
      entry.evictAfterSave = true;
    return true;
  }

  bool requestSaveOwnedRoot(const ChunkTicketPos &position,
                            std::uint64_t snapshotVersion,
                            nbt::RootTag &&root) {
    auto found = entries_.find(position);
    if (found == entries_.end() || !found->second.resident ||
        !accepting())
      return false;
    Entry &entry = found->second;
    if (!acceptSnapshotVersion(entry, snapshotVersion))
      return false;
    entry.status.dirtyVersion =
        std::max(entry.status.dirtyVersion, snapshotVersion);
    Command command = Command::save(position, entry.status.epoch,
                                    snapshotVersion, true);
    command.root.emplace(std::move(root));
    if (!enqueue(std::move(command)))
      return false;
    ++entry.inFlightSaves;
    entry.status.state = ChunkResidencyState::Saving;
    entry.status.detail.clear();
    if (!entry.status.demand)
      entry.evictAfterSave = true;
    return true;
  }

  bool completeGeneratedChunk(const ChunkTicketPos &position,
                              std::uint64_t epoch,
                              LifecycleLoadedChunk &&loaded,
                              nbt::RootTag &&root) {
    auto found = entries_.find(position);
    if (found == entries_.end() || found->second.status.epoch != epoch ||
        found->second.status.state != ChunkResidencyState::Reading ||
        !found->second.status.demand || !accepting())
      return false;
    PendingGenerated pending;
    pending.epoch = epoch;
    pending.loaded = std::move(loaded);
    pendingGenerated_[position] = std::move(pending);
    Command command = Command::installGenerated(position, epoch);
    command.root.emplace(std::move(root));
    if (!enqueue(std::move(command))) {
      pendingGenerated_.erase(position);
      return false;
    }
    return true;
  }

  void failGeneratedChunk(const ChunkTicketPos &position,
                          std::uint64_t epoch, std::string detail) {
    auto found = entries_.find(position);
    if (found == entries_.end() || found->second.status.epoch != epoch ||
        found->second.status.state != ChunkResidencyState::Reading ||
        !found->second.status.demand)
      return;
    pendingGenerated_.erase(position);
    if (detail.empty())
      detail = "world generation failed";
    handleLoadFailure(position, found->second, ChunkLoadFailure::Corrupt,
                      std::move(detail));
  }

  bool retryEviction(const ChunkTicketPos &position) {
    auto found = entries_.find(position);
    if (found == entries_.end() ||
        found->second.status.state != ChunkResidencyState::EvictPending)
      return false;
    return attemptEvict(position, found->second);
  }

  std::size_t drain() {
    std::deque<Result> completed;
    {
      std::lock_guard lock(resultsMutex_);
      completed.swap(results_);
    }
    std::size_t processed = 0;
    for (Result &result : completed) {
      ++processed;
      auto found = entries_.find(result.position);
      if (found == entries_.end() ||
          found->second.status.epoch != result.epoch) {
        if (result.kind == ResultKind::LoadReady ||
            result.kind == ResultKind::LoadFailed ||
            result.kind == ResultKind::GenerationReady)
          enqueue(Command::drop(result.position, result.epoch));
        if (result.kind == ResultKind::GenerationReady ||
            result.kind == ResultKind::GenerationFailed) {
          auto pending = pendingGenerated_.find(result.position);
          if (pending != pendingGenerated_.end() &&
              pending->second.epoch == result.epoch)
            pendingGenerated_.erase(pending);
        }
        continue;
      }
      Entry &entry = found->second;
      if (result.kind == ResultKind::LoadReady) {
        handleLoadReady(result.position, entry, std::move(result.loaded));
      } else if (result.kind == ResultKind::LoadFailed) {
        handleLoadFailure(result.position, entry, result.failure,
                          std::move(result.detail));
      } else if (result.kind == ResultKind::GenerationNeeded) {
        bool started = false;
        try {
          started = callbacks_.requestGeneration &&
                    callbacks_.requestGeneration(result.position,
                                                 result.epoch,
                                                 entry.status.demandLevel);
        } catch (...) {
          started = false;
        }
        if (!started)
          handleLoadFailure(result.position, entry, ChunkLoadFailure::Missing,
                            std::move(result.detail));
      } else if (result.kind == ResultKind::GenerationReady) {
        auto pending = pendingGenerated_.find(result.position);
        if (pending != pendingGenerated_.end() &&
            pending->second.epoch == result.epoch) {
          LifecycleLoadedChunk loaded = std::move(pending->second.loaded);
          pendingGenerated_.erase(pending);
          handleLoadReady(result.position, entry, std::move(loaded));
        } else {
          handleLoadFailure(result.position, entry, ChunkLoadFailure::Io,
                            "generated chunk payload is missing");
        }
      } else if (result.kind == ResultKind::GenerationFailed) {
        pendingGenerated_.erase(result.position);
        handleLoadFailure(result.position, entry, result.failure,
                          std::move(result.detail));
      } else if (result.kind == ResultKind::SaveDone) {
        handleSaveResult(result.position, entry, result);
      }
      if (result.kind == ResultKind::DropDone) {
        auto current = entries_.find(result.position);
        if (current != entries_.end() &&
            current->second.status.epoch == result.epoch &&
            !current->second.status.demand &&
            !current->second.resident && current->second.inFlightSaves == 0) {
          pendingGenerated_.erase(result.position);
          entries_.erase(current);
        }
      }
    }
    return processed;
  }

  std::optional<ChunkLifecycleStatus>
  status(const ChunkTicketPos &position) const {
    auto found = entries_.find(position);
    if (found == entries_.end())
      return ChunkLifecycleStatus{};
    return found->second.status;
  }

  bool accepting() const {
    std::lock_guard lock(commandMutex_);
    return !stopping_;
  }

  std::size_t activeEntryCountForTest() const { return entries_.size(); }
  std::size_t workerLedgerCountForTest() {
    const WorkerLedgerSample sample = sampleWorkerLedger();
    return sample.epochs + sample.written;
  }
  std::size_t templateCountForTest() {
    const WorkerLedgerSample sample = sampleWorkerLedger();
    return sample.templates + sample.entityTemplates;
  }

  void close() {
    {
      std::lock_guard lock(commandMutex_);
      if (stopping_)
        return;
      stopping_ = true;
    }
    commandReady_.notify_all();
    if (worker_.joinable())
      worker_.join();
    // No worker remains to consume DropTemplate. Release its owned roots and
    // version bookkeeping now; main-side status remains available for cleanup.
    templates_.clear();
    entityTemplates_.clear();
    workerEpochs_.clear();
    workerWrittenVersions_.clear();
    pendingGenerated_.clear();
  }

private:
  enum class CommandKind {
    Load,
    Save,
    Invalidate,
    DropTemplate,
    LedgerSample,
    InstallGenerated,
  };
  enum class ResultKind {
    LoadReady,
    LoadFailed,
    SaveDone,
    DropDone,
    // 磁盘上没有该区块：主线程决定是否交给世界生成。
    GenerationNeeded,
    // I/O worker 已持有生成模板，可以安装。
    GenerationReady,
    GenerationFailed,
  };

  struct Command {
    CommandKind kind = CommandKind::Load;
    ChunkTicketPos position;
    std::uint64_t epoch = 0;
    std::uint64_t dirtyVersion = 0;
    bool ownsRoot = false;
    std::optional<nbt::RootTag> root;
    ChunkEditPatch edits;
    std::optional<ChunkTickSaveData> ticks;
    std::optional<std::vector<terrain::MovingPistonEntity>> movingPistons;
    std::optional<std::vector<entity_codec::EntityRecord>> entityRecords;
    std::optional<terrain::ChunkLightPatch> light;

    static Command load(const ChunkTicketPos &position, std::uint64_t epoch) {
      Command result;
      result.kind = CommandKind::Load;
      result.position = position;
      result.epoch = epoch;
      return result;
    }
    static Command save(const ChunkTicketPos &position, std::uint64_t epoch,
                        std::uint64_t dirtyVersion, bool ownsRoot) {
      Command result;
      result.kind = CommandKind::Save;
      result.position = position;
      result.epoch = epoch;
      result.dirtyVersion = dirtyVersion;
      result.ownsRoot = ownsRoot;
      return result;
    }
    static Command invalidate(const ChunkTicketPos &position,
                              std::uint64_t epoch) {
      Command result;
      result.kind = CommandKind::Invalidate;
      result.position = position;
      result.epoch = epoch;
      return result;
    }
    static Command drop(const ChunkTicketPos &position, std::uint64_t epoch) {
      Command result;
      result.kind = CommandKind::DropTemplate;
      result.position = position;
      result.epoch = epoch;
      return result;
    }
    static Command ledgerSample() {
      Command result;
      result.kind = CommandKind::LedgerSample;
      return result;
    }
    static Command installGenerated(const ChunkTicketPos &position,
                                    std::uint64_t epoch) {
      Command result;
      result.kind = CommandKind::InstallGenerated;
      result.position = position;
      result.epoch = epoch;
      return result;
    }
  };

  struct Result {
    ResultKind kind = ResultKind::LoadFailed;
    ChunkTicketPos position;
    std::uint64_t epoch = 0;
    LifecycleLoadedChunk loaded;
    ChunkLoadFailure failure = ChunkLoadFailure::None;
    bool saveSuccess = false;
    std::uint64_t dirtyVersion = 0;
    bool cleanupPending = false;
    std::string detail;
  };

  struct WorkerTemplate {
    std::uint64_t epoch = 0;
    nbt::RootTag root;
  };

  struct WorkerEntityTemplate {
    std::uint64_t epoch = 0;
    nbt::RootTag root;
  };

  // 生成线程已产出、等待 worker 装好模板后由主线程安装的载荷。只在主线程访问。
  struct PendingGenerated {
    std::uint64_t epoch = 0;
    LifecycleLoadedChunk loaded;
  };

  bool beginRead(const ChunkTicketPos &position, Entry &entry) {
    const std::uint64_t nextEpoch = ++nextEpoch_;
    const int demandLevel = entry.status.demandLevel;
    entry.status = {};
    entry.status.epoch = nextEpoch;
    entry.status.demandLevel = demandLevel;
    entry.status.demand = true;
    entry.status.state = ChunkResidencyState::Reading;
    entry.resident = false;
    entry.evictAfterSave = false;
    entry.inFlightSaves = 0;
    entry.lastAcceptedSnapshotVersion = 0;
    entry.workerWrittenVersion = 0;
    if (!enqueue(Command::load(position, nextEpoch))) {
      entry.status.state = ChunkResidencyState::Failed;
      entry.status.failure = ChunkLoadFailure::Io;
      entry.status.detail = "controller closed before load submission";
      return false;
    }
    return true;
  }

  bool enqueue(Command command) {
    {
      std::lock_guard lock(commandMutex_);
      if (stopping_)
        return false;
      commands_.push_back(std::move(command));
    }
    commandReady_.notify_one();
    return true;
  }

  void post(Result result) {
    std::lock_guard lock(resultsMutex_);
    results_.push_back(std::move(result));
  }

  WorkerLedgerSample sampleWorkerLedger() {
    WorkerLedgerSample empty;
    {
      std::lock_guard lock(sampleMutex_);
      sample_.reset();
    }
    if (!enqueue(Command::ledgerSample()))
      return empty;
    std::unique_lock lock(sampleMutex_);
    sampleReady_.wait(lock, [this] { return sample_.has_value(); });
    WorkerLedgerSample result = *sample_;
    sample_.reset();
    return result;
  }

  bool advanceEpoch(const ChunkTicketPos &position, std::uint64_t epoch,
                    bool createIfMissing) {
    auto found = workerEpochs_.find(position);
    if (found == workerEpochs_.end()) {
      if (!createIfMissing)
        return false;
      workerEpochs_.emplace(position, epoch);
      return true;
    }
    if (epoch < found->second)
      return false;
    found->second = epoch;
    return true;
  }

  void runWorker() {
    for (;;) {
      Command command;
      {
        std::unique_lock lock(commandMutex_);
        commandReady_.wait(lock, [this] {
          return stopping_ || !commands_.empty();
        });
        if (commands_.empty()) {
          if (stopping_)
            return;
          continue;
        }
        command = std::move(commands_.front());
        commands_.pop_front();
      }
      process(std::move(command));
    }
  }

  void process(Command command) {
    if (command.kind == CommandKind::LedgerSample) {
      WorkerLedgerSample sample;
      sample.epochs = workerEpochs_.size();
      sample.written = workerWrittenVersions_.size();
      sample.templates = templates_.size();
      sample.entityTemplates = entityTemplates_.size();
      {
        std::lock_guard lock(sampleMutex_);
        sample_ = sample;
      }
      sampleReady_.notify_all();
      return;
    }
    if (command.kind == CommandKind::Invalidate) {
      auto latest = workerEpochs_.find(command.position);
      if (latest != workerEpochs_.end() &&
          command.epoch >= latest->second)
        latest->second = command.epoch;
      // An invalidation is an old-generation control. It must not create a
      // worker ledger for an already retired coordinate.
      auto found = templates_.find(command.position);
      if (found != templates_.end() &&
          found->second.epoch < command.epoch)
        templates_.erase(found);
      auto entities = entityTemplates_.find(command.position);
      if (entities != entityTemplates_.end() &&
          entities->second.epoch < command.epoch)
        entityTemplates_.erase(entities);
      return;
    }
    if (command.kind == CommandKind::DropTemplate) {
      auto found = templates_.find(command.position);
      if (found != templates_.end() &&
          found->second.epoch == command.epoch)
        templates_.erase(found);
      auto entities = entityTemplates_.find(command.position);
      if (entities != entityTemplates_.end() &&
          entities->second.epoch == command.epoch)
        entityTemplates_.erase(entities);
      auto epoch = workerEpochs_.find(command.position);
      if (epoch != workerEpochs_.end() &&
          epoch->second == command.epoch)
        workerEpochs_.erase(epoch);
      auto written = workerWrittenVersions_.find(command.position);
      // An invalidation drop owns the just-retired generation; it may remove
      // the version written by that generation even though the written token
      // was recorded before the invalidation epoch was allocated.
      if (written != workerWrittenVersions_.end() &&
          written->second.first <= command.epoch)
        workerWrittenVersions_.erase(written);
      Result result;
      result.kind = ResultKind::DropDone;
      result.position = command.position;
      result.epoch = command.epoch;
      post(std::move(result));
      return;
    }
    if (command.kind == CommandKind::Load) {
      processLoad(std::move(command));
      return;
    }
    if (command.kind == CommandKind::InstallGenerated) {
      processInstallGenerated(std::move(command));
      return;
    }
    processSave(std::move(command));
  }

  void processLoad(Command command) {
    auto previous = workerEpochs_.find(command.position);
    if (previous != workerEpochs_.end() && previous->second > command.epoch)
      return;
    // A load at this coordinate starts a fresh worker lifecycle. Remove the
    // previous generation's template/version before the read can complete.
    if (previous != workerEpochs_.end() && previous->second < command.epoch) {
      templates_.erase(command.position);
      entityTemplates_.erase(command.position);
      workerWrittenVersions_.erase(command.position);
    }
    auto &epoch = workerEpochs_[command.position];
    epoch = command.epoch;
    workerWrittenVersions_[command.position] = {command.epoch, 0};
    StoreReadResult raw;
    try {
      raw = store_->readChunk(command.position);
    } catch (const std::exception &error) {
      Result result;
      result.kind = ResultKind::LoadFailed;
      result.position = command.position;
      result.epoch = command.epoch;
      result.failure = ChunkLoadFailure::Io;
      result.detail = error.what();
      post(std::move(result));
      return;
    }
    if (raw.status != StoreReadStatus::Found) {
      Result result;
      result.position = command.position;
      result.epoch = command.epoch;
      if (raw.status == StoreReadStatus::Missing) {
        // 缺失区块按 26.1 语义交给世界生成：主线程在 drain() 中决定是否有生成器，
        // 没有生成器时仍走原来的 Missing 失败路径。
        result.kind = ResultKind::GenerationNeeded;
        result.detail = std::move(raw.detail);
      } else {
        result.kind = ResultKind::LoadFailed;
        result.failure = mapReadFailure(raw.status);
        result.detail = std::move(raw.detail);
      }
      post(std::move(result));
      return;
    }
    terrain::DecodeOptions decodeOptions;
    // 世界生成会产出已注册但尚未实现模拟的方块；受管加载必须能读回自己写出的区块。
    // 未注册名称/损坏调色板仍由解码器拒绝。
    decodeOptions.allowUnimplementedBlocks = true;
    terrain::DecodedChunk decoded =
        terrain::decodeChunkNbt(raw.nbt, decodeOptions);
    if (decoded.status == terrain::DecodeStatus::Ok &&
        (decoded.chunkX != command.position.x ||
         decoded.chunkZ != command.position.z)) {
      Result result;
      result.kind = ResultKind::LoadFailed;
      result.position = command.position;
      result.epoch = command.epoch;
      result.failure = ChunkLoadFailure::Corrupt;
      result.detail = "chunk NBT coordinates do not match requested slot";
      post(std::move(result));
      return;
    }
    if (decoded.status != terrain::DecodeStatus::Ok) {
      Result result;
      result.kind = ResultKind::LoadFailed;
      result.position = command.position;
      result.epoch = command.epoch;
      result.failure =
          decoded.status == terrain::DecodeStatus::Missing
              ? ChunkLoadFailure::Missing
              : decoded.status == terrain::DecodeStatus::Unsupported
                    ? ChunkLoadFailure::Unsupported
                    : ChunkLoadFailure::Corrupt;
      result.detail = std::move(decoded.detail);
      post(std::move(result));
      return;
    }

    std::vector<entity_codec::EntityRecord> entityRecords;
    bool entitySlotPresent = false;
    if (entityStore_) {
      StoreReadResult entities;
      try {
        entities = entityStore_->readEntities(command.position);
      } catch (const std::exception &error) {
        Result result;
        result.kind = ResultKind::LoadFailed;
        result.position = command.position;
        result.epoch = command.epoch;
        result.failure = ChunkLoadFailure::Io;
        result.detail = error.what();
        post(std::move(result));
        return;
      }
      if (entities.status == StoreReadStatus::Found) {
        entity_codec::DecodedEntityChunk decodedEntities =
            entity_codec::decodeEntityChunk(entities.nbt);
        if (decodedEntities.status != entity_codec::DecodeStatus::Ok ||
            decodedEntities.chunkX != command.position.x ||
            decodedEntities.chunkZ != command.position.z) {
          Result result;
          result.kind = ResultKind::LoadFailed;
          result.position = command.position;
          result.epoch = command.epoch;
          result.failure =
              decodedEntities.status == entity_codec::DecodeStatus::Unsupported
                  ? ChunkLoadFailure::Unsupported
                  : ChunkLoadFailure::Corrupt;
          result.detail = decodedEntities.status ==
                                  entity_codec::DecodeStatus::Ok
                              ? "entity chunk Position does not match slot"
                              : decodedEntities.detail;
          post(std::move(result));
          return;
        }
        entityTemplates_[command.position] =
            WorkerEntityTemplate{command.epoch,
                                 std::move(decodedEntities.root)};
        entityRecords = std::move(decodedEntities.records);
        entitySlotPresent = true;
      } else if (entities.status != StoreReadStatus::Missing) {
        Result result;
        result.kind = ResultKind::LoadFailed;
        result.position = command.position;
        result.epoch = command.epoch;
        result.failure = mapReadFailure(entities.status);
        result.detail = std::move(entities.detail);
        post(std::move(result));
        return;
      }
    }
    templates_[command.position] =
        WorkerTemplate{command.epoch, std::move(decoded.root)};
    Result result;
    result.kind = ResultKind::LoadReady;
    result.position = command.position;
    result.epoch = command.epoch;
    result.loaded.position = command.position;
    result.loaded.epoch = command.epoch;
    if (entityStore_) {
      result.loaded.entityData = EntityChunkDataResult{
          entityRecords.empty() ? EntityChunkDataResult::Status::Empty
                                : EntityChunkDataResult::Status::NonEmpty,
          {}};
    } else {
      result.loaded.entityData = std::move(raw.entityData);
    }
    result.loaded.chunkX = decoded.chunkX;
    result.loaded.chunkZ = decoded.chunkZ;
    result.loaded.statusName = std::move(decoded.statusName);
    result.loaded.isLightOn = decoded.isLightOn;
    result.loaded.sections = std::move(decoded.sections);
    result.loaded.blockTicks = std::move(decoded.blockTicks);
    result.loaded.fluidTicks = std::move(decoded.fluidTicks);
    result.loaded.movingPistons = std::move(decoded.movingPistons);
    result.loaded.hasUnsupportedBlockEntities =
        decoded.hasUnsupportedBlockEntities;
    result.loaded.blockEntityError = std::move(decoded.blockEntityError);
    result.loaded.entityRecords = std::move(entityRecords);
    result.loaded.entitySlotPresent = entitySlotPresent;
    result.loaded.entities = std::move(decoded.entities);
    post(std::move(result));
  }

  // 校验世界生成交付的完整 NBT 模板并交给 worker 持有：后续增量保存直接在该模板上打补丁。
  // 校验失败不安装区块，避免把不受支持的生成状态写进存档。
  void processInstallGenerated(Command command) {
    if (!advanceEpoch(command.position, command.epoch, false))
      return;
    std::string error;
    if (!command.root ||
        !rootMatchesPosition(*command.root, command.position, error) ||
        !terrain::validateChunkRootForSave(*command.root, error,
                                           /*allowRegisteredUnsupported=*/true)) {
      Result result;
      result.kind = ResultKind::GenerationFailed;
      result.position = command.position;
      result.epoch = command.epoch;
      result.failure = ChunkLoadFailure::Corrupt;
      result.detail =
          error.empty() ? "generated chunk has no NBT template" : error;
      post(std::move(result));
      return;
    }
    templates_[command.position] =
        WorkerTemplate{command.epoch, std::move(*command.root)};
    Result result;
    result.kind = ResultKind::GenerationReady;
    result.position = command.position;
    result.epoch = command.epoch;
    post(std::move(result));
  }

  void processSave(Command command) {
    if (!advanceEpoch(command.position, command.epoch, false))
      return;
    auto writtenEntry = workerWrittenVersions_.find(command.position);
    if (writtenEntry == workerWrittenVersions_.end() ||
        writtenEntry->second.first != command.epoch) {
      Result result;
      result.kind = ResultKind::SaveDone;
      result.position = command.position;
      result.epoch = command.epoch;
      result.dirtyVersion = command.dirtyVersion;
      result.detail = "save command belongs to a retired worker lifecycle";
      post(std::move(result));
      return;
    }
    const std::uint64_t writtenVersion = writtenEntry->second.second;
    if (command.dirtyVersion == 0 ||
        (writtenVersion != 0 &&
         command.dirtyVersion <= writtenVersion)) {
      Result result;
      result.kind = ResultKind::SaveDone;
      result.position = command.position;
      result.epoch = command.epoch;
      result.dirtyVersion = command.dirtyVersion;
      result.detail = "save snapshot version would regress the worker root";
      post(std::move(result));
      return;
    }

    if (command.root) {
      std::string validationError;
      if (!rootMatchesPosition(*command.root, command.position,
                               validationError) ||
          !terrain::validateChunkRootForSave(*command.root,
                                             validationError)) {
        Result result;
        result.kind = ResultKind::SaveDone;
        result.position = command.position;
        result.epoch = command.epoch;
        result.dirtyVersion = command.dirtyVersion;
        result.detail = std::move(validationError);
        post(std::move(result));
        return;
      }
      templates_[command.position] =
          WorkerTemplate{command.epoch, std::move(*command.root)};
    }
    auto found = templates_.find(command.position);
    if (found == templates_.end() ||
        found->second.epoch != command.epoch) {
      Result result;
      result.kind = ResultKind::SaveDone;
      result.position = command.position;
      result.epoch = command.epoch;
      result.dirtyVersion = command.dirtyVersion;
      result.detail = "no matching worker-owned NBT template";
      post(std::move(result));
      return;
    }

    std::string rootError;
    if (!rootMatchesPosition(found->second.root, command.position,
                             rootError)) {
      Result result;
      result.kind = ResultKind::SaveDone;
      result.position = command.position;
      result.epoch = command.epoch;
      result.dirtyVersion = command.dirtyVersion;
      result.detail = std::move(rootError);
      post(std::move(result));
      return;
    }

    StoreWriteResult write;
    std::optional<std::vector<std::uint8_t>> entityBytes;
    try {
      if (command.edits && !command.edits->empty()) {
        terrain::PatchResult patch =
            terrain::patchBlockStatesCompact(found->second.root,
                                             *command.edits);
        if (!patch.edited && !patch.error.empty()) {
          Result result;
          result.kind = ResultKind::SaveDone;
          result.position = command.position;
          result.epoch = command.epoch;
          result.dirtyVersion = command.dirtyVersion;
          result.detail = std::move(patch.error);
          post(std::move(result));
          return;
        }
      }
      if (command.movingPistons) {
        std::string blockEntityError;
        if (!terrain::patchMovingPistonEntities(
                found->second.root, *command.movingPistons,
                blockEntityError)) {
          Result result;
          result.kind = ResultKind::SaveDone;
          result.position = command.position;
          result.epoch = command.epoch;
          result.dirtyVersion = command.dirtyVersion;
          result.detail = std::move(blockEntityError);
          post(std::move(result));
          return;
        }
      }
      if (command.ticks) {
        std::string tickError;
        if (!terrain::patchChunkTicks(found->second.root,
                                      command.ticks->ticks, tickError)) {
          Result result;
          result.kind = ResultKind::SaveDone;
          result.position = command.position;
          result.epoch = command.epoch;
          result.dirtyVersion = command.dirtyVersion;
          result.detail = std::move(tickError);
          post(std::move(result));
          return;
        }
      }
      if (command.light) {
        std::string lightError;
        if (!terrain::patchChunkLight(found->second.root, *command.light,
                                      lightError)) {
          Result result;
          result.kind = ResultKind::SaveDone;
          result.position = command.position;
          result.epoch = command.epoch;
          result.dirtyVersion = command.dirtyVersion;
          result.detail = std::move(lightError);
          post(std::move(result));
          return;
        }
      }

      if (command.entityRecords && entityStore_) {
        nbt::RootTag *entityRoot = nullptr;
        auto existingEntity = entityTemplates_.find(command.position);
        nbt::RootTag freshEntity;
        if (existingEntity != entityTemplates_.end() &&
            existingEntity->second.epoch == command.epoch) {
          entityRoot = &existingEntity->second.root;
        } else {
          freshEntity = entity_codec::emptyEntityRoot(
              command.position.x, command.position.z);
          entityRoot = &freshEntity;
        }
        std::string entityError;
        if (!entity_codec::patchEntitySequence(*entityRoot,
                                               *command.entityRecords,
                                               entityError)) {
          Result result;
          result.kind = ResultKind::SaveDone;
          result.position = command.position;
          result.epoch = command.epoch;
          result.dirtyVersion = command.dirtyVersion;
          result.detail = std::move(entityError);
          post(std::move(result));
          return;
        }
        entityBytes = entity_codec::encodeEntityChunk(*entityRoot);
        if (existingEntity == entityTemplates_.end() ||
            existingEntity->second.epoch != command.epoch)
          entityTemplates_[command.position] =
              WorkerEntityTemplate{command.epoch, std::move(freshEntity)};
      }

      std::vector<std::uint8_t> bytes =
          terrain::encodeChunkNbt(found->second.root);
      const int timestamp = static_cast<int>(std::time(nullptr));
      write = store_->writeChunk(command.position, bytes, timestamp);
      if (write.wrote && entityBytes) {
        StoreWriteResult entityWrite = entityStore_->writeEntities(
            command.position, *entityBytes, timestamp);
        write.cleanupPending =
            write.cleanupPending || entityWrite.cleanupPending;
        if (!entityWrite.wrote) {
          write.wrote = false;
          write.detail = entityWrite.detail.empty()
                             ? "entity region write failed"
                             : std::move(entityWrite.detail);
        }
      }
    } catch (const std::exception &error) {
      Result result;
      result.kind = ResultKind::SaveDone;
      result.position = command.position;
      result.epoch = command.epoch;
      result.dirtyVersion = command.dirtyVersion;
      result.detail = error.what();
      post(std::move(result));
      return;
    }
    Result result;
    result.kind = ResultKind::SaveDone;
    result.position = command.position;
    result.epoch = command.epoch;
    result.dirtyVersion = command.dirtyVersion;
    result.saveSuccess = write.wrote;
    result.cleanupPending = write.cleanupPending;
    result.detail = std::move(write.detail);
    if (write.wrote) {
      auto written = workerWrittenVersions_.find(command.position);
      if (written != workerWrittenVersions_.end() &&
          written->second.first == command.epoch)
        written->second.second =
            std::max(written->second.second, command.dirtyVersion);
    }
    post(std::move(result));
  }

  void handleLoadReady(const ChunkTicketPos &position, Entry &entry,
                       LifecycleLoadedChunk &&loaded) {
    if (entry.status.state != ChunkResidencyState::Reading ||
        !entry.status.demand) {
      enqueue(Command::drop(position, entry.status.epoch));
      entries_.erase(position);
      return;
    }
    bool installed = false;
    try {
      installed = callbacks_.install &&
                  callbacks_.install(std::move(loaded));
    } catch (const std::exception &error) {
      entry.status.detail = error.what();
    }
    if (!installed) {
      entry.status.state = ChunkResidencyState::Failed;
      entry.status.failure = ChunkLoadFailure::InstallRejected;
      if (entry.status.detail.empty())
        entry.status.detail = "resident install callback rejected chunk";
      enqueue(Command::drop(position, entry.status.epoch));
      return;
    }
    entry.resident = true;
    entry.status.state = ChunkResidencyState::Resident;
    entry.status.failure = ChunkLoadFailure::None;
    entry.status.detail.clear();
  }

  void handleLoadFailure(const ChunkTicketPos &position, Entry &entry,
                         ChunkLoadFailure failure, std::string detail) {
    pendingGenerated_.erase(position);
    entry.resident = false;
    entry.status.failure = failure;
    entry.status.detail = std::move(detail);
    entry.status.state = failure == ChunkLoadFailure::Missing
                             ? ChunkResidencyState::Missing
                             : ChunkResidencyState::Failed;
    enqueue(Command::drop(position, entry.status.epoch));
    // Keep the failed generation visible while demand remains so callers can
    // diagnose it; a later no-demand update retires it through updateDemand.
  }

  void handleSaveResult(const ChunkTicketPos &position, Entry &entry,
                        const Result &result) {
    if (entry.inFlightSaves != 0)
      --entry.inFlightSaves;
    if (result.saveSuccess) {
      entry.status.savedDirtyVersion =
          std::max(entry.status.savedDirtyVersion, result.dirtyVersion);
      entry.workerWrittenVersion =
          std::max(entry.workerWrittenVersion, result.dirtyVersion);
      entry.status.failure = ChunkLoadFailure::None;
      entry.status.detail = result.detail;
      if (entry.status.dirtyVersion > entry.status.savedDirtyVersion) {
        entry.status.state = entry.inFlightSaves == 0
                                 ? ChunkResidencyState::Resident
                                 : ChunkResidencyState::Saving;
        return;
      }
      if (entry.inFlightSaves != 0) {
        entry.status.state = ChunkResidencyState::Saving;
        return;
      }
      if (entry.evictAfterSave && !entry.status.demand) {
        entry.status.state = ChunkResidencyState::EvictPending;
        attemptEvict(position, entry);
      } else {
        entry.status.state = ChunkResidencyState::Resident;
      }
      return;
    }

    entry.status.failure = ChunkLoadFailure::Io;
    entry.status.detail = result.detail.empty() ? "chunk save failed"
                                                : result.detail;
    entry.status.state = entry.inFlightSaves == 0
                             ? ChunkResidencyState::Resident
                             : ChunkResidencyState::Saving;
  }

  bool attemptEvict(const ChunkTicketPos &position, Entry &entry) {
    if (!entry.resident || entry.status.demand ||
        entry.status.dirtyVersion > entry.status.savedDirtyVersion ||
        entry.inFlightSaves != 0)
      return false;
    entry.status.state = ChunkResidencyState::EvictPending;
    bool evicted = false;
    try {
      evicted = callbacks_.evict &&
                callbacks_.evict(position, entry.status.epoch);
    } catch (const std::exception &error) {
      entry.status.detail = error.what();
    }
    if (!evicted) {
      if (entry.status.detail.empty())
        entry.status.detail = "resident evict callback declined";
      return false;
    }
    const std::uint64_t epoch = entry.status.epoch;
    entry.resident = false;
    entry.evictAfterSave = false;
    entry.status.state = ChunkResidencyState::Absent;
    entry.status.failure = ChunkLoadFailure::None;
    entry.status.detail.clear();
    enqueue(Command::drop(position, epoch));
    // Keep the clean tombstone until the worker has consumed DropTemplate.
    // In-flight save acknowledgements remain attributable to this generation;
    // drain erases it once the drop result is observed.
    return true;
  }

  std::shared_ptr<RawChunkStore> store_;
  std::shared_ptr<RawEntityStore> entityStore_;
  ChunkResidentCallbacks callbacks_;

  mutable std::mutex commandMutex_;
  std::condition_variable commandReady_;
  std::deque<Command> commands_;
  bool stopping_ = false;
  std::thread worker_;

  std::mutex resultsMutex_;
  std::deque<Result> results_;

  std::map<ChunkTicketPos, Entry> entries_;
  std::map<ChunkTicketPos, WorkerTemplate> templates_;
  std::map<ChunkTicketPos, WorkerEntityTemplate> entityTemplates_;
  std::map<ChunkTicketPos, PendingGenerated> pendingGenerated_;
  std::map<ChunkTicketPos, std::uint64_t> workerEpochs_;
  // Version is paired with the worker lifecycle epoch; a late save cannot
  // make a newer same-coordinate generation look written.
  std::map<ChunkTicketPos, std::pair<std::uint64_t, std::uint64_t>>
      workerWrittenVersions_;
  std::uint64_t nextEpoch_ = 0;

  std::mutex sampleMutex_;
  std::condition_variable sampleReady_;
  std::optional<WorkerLedgerSample> sample_;
};

ChunkLifecycleController::ChunkLifecycleController(
    std::filesystem::path regionDirectory, ChunkResidentCallbacks callbacks) {
  const auto entityDirectory = regionDirectory.parent_path() / "entities";
  impl_ = std::make_unique<Impl>(
      std::make_shared<RegionFileStore>(std::move(regionDirectory)),
      std::make_shared<RegionEntityStore>(entityDirectory),
      std::move(callbacks));
}

ChunkLifecycleController::ChunkLifecycleController(
    std::shared_ptr<RawChunkStore> store, ChunkResidentCallbacks callbacks,
    std::shared_ptr<RawEntityStore> entityStore)
    : impl_(std::make_unique<Impl>(std::move(store), std::move(entityStore),
                                   std::move(callbacks))) {}

ChunkLifecycleController::~ChunkLifecycleController() = default;

bool ChunkLifecycleController::updateDemand(const ChunkTicketPos &position,
                                            int loadingLevel) {
  return impl_->updateDemand(position, loadingLevel);
}

bool ChunkLifecycleController::applyLoadingChanges(
    std::span<const ChunkLevelChange> changes) {
  return impl_->applyLoadingChanges(changes);
}

bool ChunkLifecycleController::markDirty(const ChunkTicketPos &position,
                                         std::uint64_t version) {
  return impl_->markDirty(position, version);
}

bool ChunkLifecycleController::requestSave(
    const ChunkTicketPos &position, std::uint64_t snapshotVersion,
    std::vector<terrain::BlockEdit> edits) {
  return impl_->requestSave(position, snapshotVersion, std::move(edits));
}

bool ChunkLifecycleController::requestSavePayload(
    const ChunkTicketPos &position, std::uint64_t snapshotVersion,
    ChunkSavePayload payload) {
  return impl_->requestSavePayload(position, snapshotVersion,
                                   std::move(payload));
}

bool ChunkLifecycleController::requestSaveOwnedRoot(
    const ChunkTicketPos &position, std::uint64_t snapshotVersion,
    nbt::RootTag &&root) {
  return impl_->requestSaveOwnedRoot(position, snapshotVersion,
                                     std::move(root));
}

bool ChunkLifecycleController::completeGeneratedChunk(
    const ChunkTicketPos &position, std::uint64_t epoch,
    LifecycleLoadedChunk &&loaded, nbt::RootTag &&root) {
  return impl_->completeGeneratedChunk(position, epoch, std::move(loaded),
                                       std::move(root));
}

void ChunkLifecycleController::failGeneratedChunk(
    const ChunkTicketPos &position, std::uint64_t epoch,
    std::string detail) {
  impl_->failGeneratedChunk(position, epoch, std::move(detail));
}

bool ChunkLifecycleController::retryEviction(
    const ChunkTicketPos &position) {
  return impl_->retryEviction(position);
}

std::size_t ChunkLifecycleController::drain() { return impl_->drain(); }

std::optional<ChunkLifecycleStatus>
ChunkLifecycleController::status(const ChunkTicketPos &position) const {
  return impl_->status(position);
}

std::size_t ChunkLifecycleController::activeEntryCountForTest() const {
  return impl_->activeEntryCountForTest();
}
std::size_t ChunkLifecycleController::workerLedgerCountForTest() {
  return impl_->workerLedgerCountForTest();
}
std::size_t ChunkLifecycleController::templateCountForTest() {
  return impl_->templateCountForTest();
}

bool ChunkLifecycleController::accepting() const {
  return impl_->accepting();
}

void ChunkLifecycleController::close() { impl_->close(); }

} // namespace redstone
