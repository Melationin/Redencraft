#include "redstone/world_chunk_manager.hpp"
#include "redstone/entity.hpp"
#include "redstone/piston.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace redstone {
namespace {
std::int64_t wrappingAdd(std::int64_t left, std::int64_t right) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(left) +
                                   static_cast<std::uint64_t>(right));
}

std::int32_t savedDelay(std::int64_t trigger, std::int64_t currentTime) {
  const std::uint64_t difference =
      static_cast<std::uint64_t>(trigger) -
      static_cast<std::uint64_t>(currentTime);
  return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(difference));
}
terrain::CompactBlockEdit compactEdit(std::uint32_t cell,
                                       BlockState::Id stateId) {
  const auto sectionBits = static_cast<std::uint8_t>(cell >> 12);
  return {std::bit_cast<std::int8_t>(sectionBits),
          static_cast<std::uint16_t>(cell & 0xfff), stateId};
}
ChunkLifecycleController::ChunkEditPatch makeEditPatch(
    const std::map<std::uint32_t, BlockState::Id> &pending,
    const ChunkLifecycleController::ChunkEditPatch &retained) {
  auto output =
      std::make_shared<std::vector<terrain::CompactBlockEdit>>();
  if (!retained) {
    output->reserve(pending.size());
    for (const auto &[cell, stateId] : pending)
      output->push_back(compactEdit(cell, stateId));
    return output;
  }
  constexpr std::size_t kMaximumCells =
      World::kSectionCapacity * 4096u;
  output->reserve(std::min(kMaximumCells,
                           retained->size() + pending.size()));
  std::size_t oldIndex = 0;
  auto pendingIt = pending.begin();
  while (oldIndex < retained->size() && pendingIt != pending.end()) {
    const auto &old = (*retained)[oldIndex];
    const std::uint32_t oldCell = terrain::compactCellKey(old);
    const std::uint32_t pendingCell = pendingIt->first;
    if (oldCell < pendingCell) {
      output->push_back(old);
      ++oldIndex;
    } else if (pendingCell < oldCell) {
      output->push_back(compactEdit(pendingCell, pendingIt->second));
      ++pendingIt;
    } else {
      // Newer pending value wins without constructing a temporary tree.
      output->push_back(compactEdit(pendingCell, pendingIt->second));
      ++oldIndex;
      ++pendingIt;
    }
  }
  for (; oldIndex < retained->size(); ++oldIndex)
    output->push_back((*retained)[oldIndex]);
  for (; pendingIt != pending.end(); ++pendingIt)
    output->push_back(compactEdit(pendingIt->first, pendingIt->second));
  return output;
}
} // namespace

class WorldChunkManager::EntityReadStore final : public RawChunkStore {
public:
  EntityReadStore(std::shared_ptr<RawChunkStore> inner,
                  EntityChunkDataProbe entityProbe)
      : inner_(std::move(inner)), entityProbe_(std::move(entityProbe)) {}

  StoreReadResult readChunk(const ChunkTicketPos &position) override {
    StoreReadResult raw = inner_->readChunk(position);
    if (raw.status == StoreReadStatus::Found) {
      try {
        raw.entityData = entityProbe_(position);
      } catch (const std::exception &error) {
        raw.entityData = EntityChunkDataResult{
            EntityChunkDataResult::Status::Error, error.what()};
      }
    }
    return raw;
  }

  StoreWriteResult writeChunk(const ChunkTicketPos &position,
                              std::span<const std::uint8_t> nbt,
                              int timestampSeconds) override {
    return inner_->writeChunk(position, nbt, timestampSeconds);
  }

private:
  std::shared_ptr<RawChunkStore> inner_;
  EntityChunkDataProbe entityProbe_;
};

WorldChunkManager::WorldChunkManager(
    World &world, std::filesystem::path regionDirectory, int viewDistance,
    int simulationDistance, EntityChunkDataProbe entityProbe,
    std::shared_ptr<RawEntityStore> entityStore)
    : world_(world), tickets_(viewDistance, simulationDistance) {
  initializeCallbacks();
  const std::filesystem::path entityDirectory =
      regionDirectory.parent_path() / "entities";
  terrainStore_ =
      std::make_shared<RegionFileStore>(std::move(regionDirectory));
  entityStore_ = std::move(entityStore);
  if (!entityStore_)
    entityStore_ = std::make_shared<RegionEntityStore>(entityDirectory);
  (void)entityProbe;
  controller_ = std::make_unique<ChunkLifecycleController>(
      terrainStore_,
      ChunkResidentCallbacks{
          [this](LifecycleLoadedChunk &&loaded) {
            return installLoaded(std::move(loaded));
          },
          [this](const ChunkTicketPos &position, std::uint64_t epoch) {
            return evictLoaded(position, epoch);
          },
      },
      entityStore_);
  installWorldCallbacks();
}

WorldChunkManager::WorldChunkManager(
    World &world, std::shared_ptr<RawChunkStore> store, int viewDistance,
    int simulationDistance, EntityChunkDataProbe entityProbe,
    std::shared_ptr<RawEntityStore> entityStore)
    : world_(world), tickets_(viewDistance, simulationDistance) {
  initializeCallbacks();
  entityStore_ = std::move(entityStore);
  if (entityStore_) {
    terrainStore_ = std::move(store);
  } else {
    EntityChunkDataProbe resolvedProbe;
    if (entityProbe) {
      resolvedProbe = std::move(entityProbe);
    } else {
      resolvedProbe = [](const ChunkTicketPos &) {
        return EntityChunkDataResult{
            EntityChunkDataResult::Status::Unavailable,
            "injected store has no entity-region readiness probe"};
      };
    }
    terrainStore_ = std::make_shared<EntityReadStore>(
        std::move(store), std::move(resolvedProbe));
  }
  controller_ = std::make_unique<ChunkLifecycleController>(
      terrainStore_,
      ChunkResidentCallbacks{
          [this](LifecycleLoadedChunk &&loaded) {
            return installLoaded(std::move(loaded));
          },
          [this](const ChunkTicketPos &position, std::uint64_t epoch) {
            return evictLoaded(position, epoch);
          },
      },
      entityStore_);
  installWorldCallbacks();
}

WorldChunkManager::~WorldChunkManager() noexcept {
  try {
    (void)close();
  } catch (...) {
    // Explicit close() is the inspectable path. Destruction remains noexcept.
  }
}

void WorldChunkManager::installWorldCallbacks() {
  world_.managedBlockMutationObserver =
      [this](Pos pos, const BlockState &oldState,
             const BlockState &newState) {
        recordBlockMutation(pos, oldState, newState);
      };
  world_.tickMutationObserver = [this](Pos pos) { recordTickMutation(pos); };
  world_.pistonEntityMutationObserver =
      [this](Pos pos) { recordPistonMutation(pos); };
  world_.managedEntityMutationObserver =
      [this](Vec3 oldPosition, Vec3 newPosition,
             const std::array<std::int32_t, 4> &uuid, bool removed) {
        recordEntityMutation(oldPosition, newPosition, uuid, removed);
      };
  world_.blockTickPositionReady = [this](Pos pos) {
    const ChunkTicketPos key{pos.x >> 4, pos.z >> 4};
    auto found = readiness_.find(key);
    return found != readiness_.end() && found->second.blockTicking;
  };
  world_.entityTickPositionReady = [this](Pos pos) {
    const ChunkTicketPos key{pos.x >> 4, pos.z >> 4};
    auto found = readiness_.find(key);
    return found != readiness_.end() && found->second.entityTicking;
  };
  world_.managedLightPublishedObserver = [this](int chunkX, int chunkZ) {
    recordLightPublished(chunkX, chunkZ);
  };
}

void WorldChunkManager::detachWorldCallbacks() {
  world_.managedBlockMutationObserver = {};
  world_.tickMutationObserver = {};
  world_.pistonEntityMutationObserver = {};
  world_.managedEntityMutationObserver = {};
  world_.blockTickPositionReady = {};
  world_.entityTickPositionReady = {};
  world_.managedLightPublishedObserver = {};
}

void WorldChunkManager::initializeCallbacks() {
  if (world_.managedBlockMutationObserver || world_.tickMutationObserver ||
      world_.pistonEntityMutationObserver ||
      world_.managedEntityMutationObserver || world_.blockTickPositionReady ||
      world_.entityTickPositionReady || world_.managedLightPublishedObserver)
    throw std::logic_error("World already has a managed chunk owner");
}

TicketSourceUpdate WorldChunkManager::setPlayer(PlayerTicketId id, bool present,
                                                ChunkTicketPos center) {
  if (!accepting())
    return {};
  TicketSourceUpdate update = tickets_.setPlayer(id, present, center);
  applyTicketUpdate(update.tickets);
  return update;
}

TicketSourceUpdate WorldChunkManager::addPlayer(PlayerTicketId id,
                                                ChunkTicketPos center) {
  if (!accepting())
    return {};
  TicketSourceUpdate update = tickets_.addPlayer(id, center);
  applyTicketUpdate(update.tickets);
  return update;
}

TicketSourceUpdate WorldChunkManager::movePlayer(PlayerTicketId id,
                                                 ChunkTicketPos center) {
  if (!accepting())
    return {};
  TicketSourceUpdate update = tickets_.movePlayer(id, center);
  applyTicketUpdate(update.tickets);
  return update;
}

TicketSourceUpdate
WorldChunkManager::removePlayer(PlayerTicketId id) {
  if (!accepting())
    return {};
  TicketSourceUpdate update = tickets_.removePlayer(id);
  applyTicketUpdate(update.tickets);
  return update;
}

TicketSourceUpdate
WorldChunkManager::setViewDistance(int distance) {
  if (!accepting())
    return {};
  TicketSourceUpdate update = tickets_.setViewDistance(distance);
  applyTicketUpdate(update.tickets);
  return update;
}

TicketSourceUpdate
WorldChunkManager::setSimulationDistance(int distance) {
  if (!accepting())
    return {};
  TicketSourceUpdate update = tickets_.setSimulationDistance(distance);
  applyTicketUpdate(update.tickets);
  return update;
}

TicketSystemUpdate WorldChunkManager::restorePersistentTickets(
    std::span<const std::pair<ChunkTicketPos, Ticket>> tickets) {
  if (!accepting())
    return {};
  TicketSystemUpdate update = tickets_.restorePersistentTickets(tickets);
  applyTicketUpdate(update);
  return update;
}

TicketSystemUpdate
WorldChunkManager::setForced(const ChunkTicketPos &position, bool forced) {
  if (!accepting())
    return {};
  TicketSystemUpdate update = tickets_.setForced(position, forced);
  applyTicketUpdate(update);
  return update;
}

void WorldChunkManager::applyTicketUpdate(const TicketSystemUpdate &update) {
  if (!accepting())
    return;
  if (update.loadingChanges.empty() && update.simulationChanges.empty())
    return;
  observeTickTime();
  controller_->applyLoadingChanges(update.loadingChanges);
  for (const ChunkLevelChange &change : update.loadingChanges) {
    auto managed = managed_.find(change.position);
    if (managed == managed_.end()) {
      if (change.newLevel > ticket_levels::FullChunkLevel &&
          !entityMigrationPinned(change.position))
        errors_.erase(change.position);
      continue;
    }
    reclassifyCooldown(change.position);
    reclassifyDynamicRetry(change.position);
    if (change.newLevel <= ticket_levels::FullChunkLevel) {
      managed->second.transientEventRetryScheduled = false;
      continue;
    }
    // The demand edge is the only work this socket-facing path extracts. Held
    // and unrelated resident chunks remain absent from this loop.
    if (managed->second.inFlightVersion != 0) {
      enqueueDemandSaveCandidate(change.position);
    } else {
      const auto status = controller_->status(change.position);
      if (status && status->dirtyVersion > status->savedDirtyVersion)
        enqueueDemandSaveCandidate(change.position);
      else
        enqueueEvictionRetry(change.position);
    }
  }
  refreshReadinessForChanges(update.loadingChanges, update.simulationChanges);
  pump();
}

void WorldChunkManager::pump() {
  world_.drainLightPublications();
  if (!accepting())
    return;
  observeTickTime();
  controller_->drain();
  processSaveCompletions();
  pumpSaveStats_ = {};
  const std::size_t limit =
      std::min<std::size_t>(4, autosaveLimits_.maxCandidateChecks);
  processMigrationWork(limit);
  processNoDemandSaveBudget(limit);
  processDemandSaveCandidates(limit - pumpSaveStats_.candidateChecks);
  processEvictionRetries(limit - pumpSaveStats_.candidateChecks);
  pumpSaveStats_.inFlight = savingInFlight_.size();
  world_.drainLightPublications();
  processLightSaveCompletions();
}

void WorldChunkManager::serverTick(bool tickChunks, bool runsNormally) {
  if (!accepting())
    return;
  if (runsNormally || !tickChunks) {
    const TicketHolderLookup holders = [this](const ChunkTicketPos &position) {
      TicketHolderState result;
      const auto status = controller_->status(position);
      result.present =
          status &&
          (status->state == ChunkResidencyState::Reading ||
           status->state == ChunkResidencyState::Resident ||
           status->state == ChunkResidencyState::Saving ||
           status->state == ChunkResidencyState::EvictPending);
      // ChunkHolder.readyForSaving is the completed install/dependency state,
      // not RegionFile write completion. Dirty or Saving residents must still
      // age expiring tickets; eviction separately waits for savedDirtyVersion.
      result.readyForSaving =
          status &&
          (status->state == ChunkResidencyState::Resident ||
           status->state == ChunkResidencyState::Saving ||
           status->state == ChunkResidencyState::EvictPending);
      return result;
    };
    TicketSystemUpdate update = tickets_.tick(holders);
    applyTicketUpdate(update);
  }
  if (runsNormally && tickChunks)
    world_.tick();
  pump();
  if (runsNormally && tickChunks)
    processAutosaveBudget();
}

void WorldChunkManager::processSaveCompletions() {
  std::vector<ChunkTicketPos> saving(savingInFlight_.begin(),
                                     savingInFlight_.end());
  const auto now = std::chrono::steady_clock::now();
  for (const ChunkTicketPos &position : saving) {
    auto found = managed_.find(position);
    if (found == managed_.end()) {
      savingInFlight_.erase(position);
      continue;
    }
    ManagedChunk &chunk = found->second;
    if (chunk.inFlightVersion == 0) {
      savingInFlight_.erase(position);
      chunk.inFlightPatch.reset();
      continue;
    }
    const auto status = controller_->status(position);
    if (!status) {
      chunk.inFlightVersion = 0;
      chunk.inFlightPatch.reset();
      savingInFlight_.erase(position);
      continue;
    }
    const bool succeeded =
        status->savedDirtyVersion >= chunk.inFlightVersion;
    const bool failed =
        status->state == ChunkResidencyState::Resident &&
        status->failure == ChunkLoadFailure::Io &&
        status->savedDirtyVersion < chunk.inFlightVersion;
    if (!succeeded && !failed)
      continue;

    const std::uint64_t completedVersion = chunk.inFlightVersion;
    chunk.inFlightVersion = 0;
    chunk.inFlightPatch.reset();
    savingInFlight_.erase(position);
    const bool newerDirty =
        status->dirtyVersion > status->savedDirtyVersion ||
        chunk.dirtyVersion > status->savedDirtyVersion;

    if (succeeded) {
      // The worker root now contains the accepted patch. A version accepted
      // while it was in flight remains in pendingEdits and becomes a new patch.
      chunk.retainedPatch.reset();
      chunk.failedVersion = 0;
      chunk.dynamicBlocked = false;
      entityMigrationSaveCompleted(position, completedVersion, true);
      if (newerDirty) {
        if (status->demand) {
          deferSaveCandidate(position, autosaveLimits_.cooldown);
          enqueueAutosaveCandidate(position);
        } else {
          // No-demand cooldown is consumed by bounded pump work even while
          // the game tick is frozen.
          deferSaveCandidate(position, autosaveLimits_.cooldown);
        }
      } else if (!status->demand) {
        enqueueEvictionRetry(position);
      } else {
        chunk.nextSaveAttempt = now + autosaveLimits_.cooldown;
      }
    } else {
      // A failed write may have failed before mutating the worker template.
      // Keep the exact shared snapshot for retry; pendingEdits already owns the
      // newer delta and is merged on the next accepted version.
      chunk.failedVersion = completedVersion;
      entityMigrationSaveCompleted(position, completedVersion, false);
      deferSaveCandidate(position, autosaveLimits_.failureBackoff);
      // Migration cleanup must retry even for an otherwise clean source. Any
      // unrelated chunk waits for its normal server-tick budget.
      const bool migrationRetry =
          entityMigrationSources_.contains(position);
      if (status->demand || migrationRetry) {
        // A migration source can be clean apart from the pending UUID erasure;
        // route it through the existing bounded no-demand retry queue.
        if (migrationRetry && !status->demand)
          enqueueDynamicRetry(position);
        else
          enqueueAutosaveCandidate(position);
      }
      chunk.failedVersion = 0;
    }
  }
}

void WorldChunkManager::enqueueDemandSaveCandidate(
    const ChunkTicketPos &position) {
  auto found = managed_.find(position);
  if (found != managed_.end() &&
      demandSaveQueued_.insert(position).second)
    demandSaveCandidates_.push_back({position, found->second.epoch});
}

void WorldChunkManager::enqueueEvictionRetry(
    const ChunkTicketPos &position) {
  auto found = managed_.find(position);
  if (found != managed_.end() &&
      evictionRetryQueued_.insert(position).second)
    evictionRetries_.push_back({position, found->second.epoch});
}

void WorldChunkManager::processDemandSaveCandidates(std::size_t maxEntries) {
  std::size_t entries = 0;
  std::size_t submissions = 0;
  while (entries < maxEntries && !demandSaveCandidates_.empty() &&
         pumpSaveStats_.submissions + submissions <
             autosaveLimits_.maxSubmissions &&
         savingInFlight_.size() < autosaveLimits_.maxInFlightSaves) {
    const SaveCandidate candidate = demandSaveCandidates_.front();
    demandSaveCandidates_.pop_front();
    ++entries;
    auto found = managed_.find(candidate.position);
    if (found == managed_.end()) {
      demandSaveQueued_.erase(candidate.position);
      continue;
    }
    if (found->second.epoch != candidate.epoch)
      continue;
    demandSaveQueued_.erase(candidate.position);
    const ChunkTicketPos &position = candidate.position;
    if (found->second.inFlightVersion != 0)
      continue;
    ManagedChunk &chunk = found->second;
    const auto status = controller_->status(position);
    if (!status)
      continue;
    if (status->demand) {
      // Demand returned before eviction: only the real server-tick autosave
      // path may now submit this chunk.
      enqueueAutosaveCandidate(position);
      continue;
    }
    if (status->dirtyVersion <= status->savedDirtyVersion) {
      enqueueEvictionRetry(position);
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < chunk.nextSaveAttempt) {
      deferSaveCandidate(
          position, std::chrono::duration_cast<std::chrono::milliseconds>(
                        chunk.nextSaveAttempt - now));
      continue;
    }
    if (scheduleSave(position, chunk, false))
      ++submissions;
  }
  pumpSaveStats_.candidateChecks += entries;
  pumpSaveStats_.submissions += submissions;
}

void WorldChunkManager::processEvictionRetries(std::size_t maxEntries) {
  std::vector<ChunkTicketPos> retryLater;
  std::size_t entries = 0;
  while (entries < maxEntries && !evictionRetries_.empty()) {
    const SaveCandidate candidate = evictionRetries_.front();
    evictionRetries_.pop_front();
    ++entries;
    auto found = managed_.find(candidate.position);
    if (found == managed_.end()) {
      evictionRetryQueued_.erase(candidate.position);
      continue;
    }
    if (found->second.epoch != candidate.epoch)
      continue;
    evictionRetryQueued_.erase(candidate.position);
    const ChunkTicketPos &position = candidate.position;
    const auto status = controller_->status(position);
    if (!status)
      continue;
    if (status->demand) {
      enqueueAutosaveCandidate(position);
      continue;
    }
    if (status->dirtyVersion > status->savedDirtyVersion) {
      enqueueDemandSaveCandidate(position);
      continue;
    }
    if (status->state != ChunkResidencyState::EvictPending)
      continue;
    if (!controller_->retryEviction(position))
      retryLater.push_back(position);
  }
  for (const ChunkTicketPos &position : retryLater)
    enqueueEvictionRetry(position);
  pumpSaveStats_.candidateChecks += entries;
}

void WorldChunkManager::enqueueAutosaveCandidate(
    const ChunkTicketPos &position) {
  auto found = managed_.find(position);
  if (found == managed_.end() || !found->second.blocker.empty() ||
      found->second.inFlightVersion != 0 ||
      savingInFlight_.contains(position))
    return;
  ManagedChunk &chunk = found->second;
  if (chunk.cooldownScheduled)
    return;
  const auto now = std::chrono::steady_clock::now();
  if (now < chunk.nextSaveAttempt) {
    deferSaveCandidate(
        position, std::chrono::duration_cast<std::chrono::milliseconds>(
                      chunk.nextSaveAttempt - now));
    return;
  }
  if (autosaveQueued_.insert(position).second)
    autosaveCandidates_.push_back({position, chunk.epoch});
}

void WorldChunkManager::reclassifyDynamicRetry(
    const ChunkTicketPos &position) {
  auto found = managed_.find(position);
  if (found == managed_.end())
    return;
  const auto status = controller_->status(position);
  if (status && !status->demand) {
    if (heldDynamicQueued_.erase(position) &&
        noDemandDynamicQueued_.insert(position).second)
      noDemandDynamicRetries_.push_back({position, found->second.epoch});
  } else if (noDemandDynamicQueued_.erase(position) &&
             heldDynamicQueued_.insert(position).second) {
    heldDynamicRetries_.push_back({position, found->second.epoch});
  }
}

void WorldChunkManager::reclassifyCooldown(
    const ChunkTicketPos &position) {
  auto found = managed_.find(position);
  if (found == managed_.end() || !found->second.cooldownScheduled)
    return;
  ManagedChunk &chunk = found->second;
  heldSaveCooldowns_.erase({chunk.nextSaveAttempt, position});
  noDemandSaveCooldowns_.erase({chunk.nextSaveAttempt, position});
  const auto status = controller_->status(position);
  if (status && !status->demand)
    noDemandSaveCooldowns_.insert({chunk.nextSaveAttempt, position});
  else
    heldSaveCooldowns_.insert({chunk.nextSaveAttempt, position});
}

void WorldChunkManager::deferSaveCandidate(
    const ChunkTicketPos &position, std::chrono::milliseconds delay) {
  auto found = managed_.find(position);
  if (found == managed_.end())
    return;
  ManagedChunk &chunk = found->second;
  if (chunk.cooldownScheduled)
    return;
  chunk.nextSaveAttempt = std::chrono::steady_clock::now() + delay;
  chunk.cooldownScheduled = true;
  reclassifyCooldown(position);
}

void WorldChunkManager::enqueueDynamicRetry(
    const ChunkTicketPos &position) {
  auto found = managed_.find(position);
  if (found == managed_.end())
    return;
  const auto status = controller_->status(position);
  if (status && !status->demand) {
    if (noDemandDynamicQueued_.insert(position).second)
      noDemandDynamicRetries_.push_back({position, found->second.epoch});
  } else if (heldDynamicQueued_.insert(position).second) {
    heldDynamicRetries_.push_back({position, found->second.epoch});
  }
}

void WorldChunkManager::noteDynamicBlocker(
    const ChunkTicketPos &position, std::string reason) {
  auto found = managed_.find(position);
  if (found == managed_.end())
    return;
  found->second.dynamicBlocked = true;
  errors_[position] = reason;
  enqueueDynamicRetry(position);
}

void WorldChunkManager::processNoDemandSaveBudget(
    std::size_t maxEntries) {
  std::size_t entries = 0;
  std::size_t submissions = 0;
  bool dynamicChecked = false;
  while (entries < maxEntries &&
         pumpSaveStats_.submissions + submissions <
             autosaveLimits_.maxSubmissions &&
         savingInFlight_.size() < autosaveLimits_.maxInFlightSaves) {
    const auto now = std::chrono::steady_clock::now();
    if (!noDemandSaveCooldowns_.empty() &&
        noDemandSaveCooldowns_.begin()->first <= now) {
      const ChunkTicketPos position =
          noDemandSaveCooldowns_.begin()->second;
      noDemandSaveCooldowns_.erase(noDemandSaveCooldowns_.begin());
      ++entries;
      auto found = managed_.find(position);
      if (found == managed_.end())
        continue;
      const auto status = controller_->status(position);
      if (status && status->demand) {
        reclassifyCooldown(position);
        enqueueAutosaveCandidate(position);
        continue;
      }
      found->second.cooldownScheduled = false;
      if (processAutosaveCandidate(position, now))
        ++submissions;
      continue;
    }
    if (dynamicChecked || noDemandDynamicRetries_.empty())
      break;
    const SaveCandidate candidate = noDemandDynamicRetries_.front();
    noDemandDynamicRetries_.pop_front();
    dynamicChecked = true;
    ++entries;
    auto found = managed_.find(candidate.position);
    if (found == managed_.end()) {
      noDemandDynamicQueued_.erase(candidate.position);
      continue;
    }
    if (found->second.epoch != candidate.epoch ||
        !noDemandDynamicQueued_.contains(candidate.position))
      continue;
    noDemandDynamicQueued_.erase(candidate.position);
    const auto status = controller_->status(candidate.position);
    if (!status)
      continue;
    if (status->demand) {
      enqueueAutosaveCandidate(candidate.position);
      enqueueDynamicRetry(candidate.position);
      continue;
    }
    std::string reason;
    if (persistenceBlocker(candidate.position, reason)) {
      noteDynamicBlocker(candidate.position, std::move(reason));
      continue;
    }
    found->second.dynamicBlocked = false;
    if (processAutosaveCandidate(candidate.position, now))
      ++submissions;
  }
  pumpSaveStats_.candidateChecks += entries;
  pumpSaveStats_.submissions += submissions;
}

bool WorldChunkManager::processAutosaveCandidate(
    const ChunkTicketPos &position,
    std::chrono::steady_clock::time_point now) {
  auto found = managed_.find(position);
  if (found == managed_.end())
    return false;
  ManagedChunk &chunk = found->second;
  const auto status = controller_->status(position);
  if (!status || chunk.inFlightVersion != 0 ||
      status->state == ChunkResidencyState::Saving ||
      status->dirtyVersion <= status->savedDirtyVersion ||
      !chunk.blocker.empty())
    return false;
  if (now < chunk.nextSaveAttempt) {
    deferSaveCandidate(
        position, std::chrono::duration_cast<std::chrono::milliseconds>(
                      chunk.nextSaveAttempt - now));
    return false;
  }
  return scheduleSave(position, chunk, false);
}

void WorldChunkManager::processAutosaveBudget() {
  autosaveStats_ = {};
  const auto deadline = std::chrono::steady_clock::now() +
                        autosaveLimits_.candidateTimeBudget;
  autosaveStats_.inFlight = savingInFlight_.size();
  autosaveStats_.queued = autosaveCandidates_.size() +
                          heldSaveCooldowns_.size() +
                          heldDynamicRetries_.size();
  if (autosaveLimits_.maxInFlightSaves == 0 ||
      savingInFlight_.size() >= autosaveLimits_.maxInFlightSaves)
    return;

  const bool preferDynamic = preferHeldDynamic_;
  preferHeldDynamic_ = !preferHeldDynamic_;
  bool dynamicAttempted = false;
  while (autosaveStats_.candidateChecks <
             autosaveLimits_.maxCandidateChecks &&
         autosaveStats_.submissions < autosaveLimits_.maxSubmissions &&
         savingInFlight_.size() < autosaveLimits_.maxInFlightSaves) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      break;
    if (!heldSaveCooldowns_.empty() &&
        heldSaveCooldowns_.begin()->first <= now) {
      const ChunkTicketPos position = heldSaveCooldowns_.begin()->second;
      heldSaveCooldowns_.erase(heldSaveCooldowns_.begin());
      ++autosaveStats_.candidateChecks;
      auto found = managed_.find(position);
      if (found == managed_.end())
        continue;
      const auto status = controller_->status(position);
      if (status && !status->demand) {
        reclassifyCooldown(position);
        continue;
      }
      found->second.cooldownScheduled = false;
      if (processAutosaveCandidate(position, now))
        ++autosaveStats_.submissions;
      continue;
    }
    const bool takeDynamic =
        !dynamicAttempted && !heldDynamicRetries_.empty() &&
        (preferDynamic || autosaveCandidates_.empty());
    if (takeDynamic) {
      const SaveCandidate candidate = heldDynamicRetries_.front();
      heldDynamicRetries_.pop_front();
      dynamicAttempted = true;
      ++autosaveStats_.candidateChecks;
      auto found = managed_.find(candidate.position);
      if (found == managed_.end()) {
        heldDynamicQueued_.erase(candidate.position);
        continue;
      }
      if (found->second.epoch != candidate.epoch ||
          !heldDynamicQueued_.contains(candidate.position))
        continue;
      heldDynamicQueued_.erase(candidate.position);
      const auto status = controller_->status(candidate.position);
      if (!status)
        continue;
      if (!status->demand) {
        enqueueDynamicRetry(candidate.position);
        continue;
      }
      std::string reason;
      if (persistenceBlocker(candidate.position, reason)) {
        noteDynamicBlocker(candidate.position, std::move(reason));
        continue;
      }
      found->second.dynamicBlocked = false;
      if (processAutosaveCandidate(candidate.position, now))
        ++autosaveStats_.submissions;
      continue;
    }
    if (autosaveCandidates_.empty())
      break;
    const SaveCandidate candidate = autosaveCandidates_.front();
    autosaveCandidates_.pop_front();
    auto found = managed_.find(candidate.position);
    if (found != managed_.end() &&
        found->second.epoch == candidate.epoch)
      autosaveQueued_.erase(candidate.position);
    ++autosaveStats_.candidateChecks;
    if (found != managed_.end() &&
        found->second.epoch == candidate.epoch &&
        processAutosaveCandidate(candidate.position, now))
      ++autosaveStats_.submissions;
  }
  autosaveStats_.inFlight = savingInFlight_.size();
  autosaveStats_.queued = autosaveCandidates_.size() +
                          heldSaveCooldowns_.size() +
                          heldDynamicRetries_.size();
}

std::vector<std::pair<int, int>> WorldChunkManager::managedChunks() const {
  std::vector<std::pair<int, int>> result;
  result.reserve(managed_.size());
  for (const auto &[position, chunk] : managed_) {
    (void)chunk;
    result.emplace_back(position.x, position.z);
  }
  return result;
}

std::size_t WorldChunkManager::entityReadMetadataCountForTest() const {
  return 0;
}

bool WorldChunkManager::setBlock(Pos pos, BlockState state, int flags,
                                 int limit) {
  return accepting() && world_.setBlock(pos, state, flags, limit);
}

void WorldChunkManager::rawSetBlock(Pos pos, BlockState state) {
  if (accepting())
    world_.rawSetBlock(pos, state);
}

bool WorldChunkManager::retrySave(const ChunkTicketPos &position) {
  if (!accepting())
    return false;
  auto found = managed_.find(position);
  if (found == managed_.end() ||
      savingInFlight_.size() >= autosaveLimits_.maxInFlightSaves)
    return false;
  found->second.failedVersion = 0;
  const bool queued = scheduleSave(position, found->second, true);
  pump();
  return queued;
}

std::optional<ManagedChunkInfo>
WorldChunkManager::managedStatus(const ChunkTicketPos &position) const {
  if (!managed_.contains(position) && !errors_.contains(position))
    return std::nullopt;
  auto lifecycle = controller_->status(position);
  if (!lifecycle)
    return std::nullopt;
  ManagedChunkInfo result;
  result.lifecycle = std::move(*lifecycle);
  if (const LightService *light = world_.lightService())
    result.light = light->chunkStatus(position.x, position.z);
  if (auto managed = managed_.find(position); managed != managed_.end())
    result.blocker = managed->second.blocker;
  if (auto error = errors_.find(position); error != errors_.end())
    result.error = error->second;
  return result;
}

bool WorldChunkManager::isManaged(const ChunkTicketPos &position) const {
  return managed_.contains(position);
}

std::optional<ChunkLifecycleStatus>
WorldChunkManager::lifecycleStatus(const ChunkTicketPos &position) const {
  return controller_->status(position);
}
ChunkTickReadiness
WorldChunkManager::tickReadiness(const ChunkTicketPos &position) const {
  auto found = readiness_.find(position);
  if (found != readiness_.end())
    return found->second;
  return managed_.contains(position) ? computeReadiness(position)
                                     : ChunkTickReadiness{};
}


ChunkShutdownResult WorldChunkManager::close() {
  if (closed_)
    return lastShutdownResult_;
  ChunkShutdownResult result;
  if (closing_)
    return result;
  closing_ = true;
  observeTickTime();

  std::vector<ChunkTicketPos> initiallyManaged;
  initiallyManaged.reserve(managed_.size());
  for (const auto &[position, chunk] : managed_) {
    (void)chunk;
    initiallyManaged.push_back(position);
  }

  auto drain = [&] {
    controller_->drain();
    world_.drainLightPublications();
    processLightSaveCompletions();
  };
  std::set<ChunkTicketPos> unsaved;
  std::vector<ChunkTicketPos> savedCandidates;
  try {
    if (LightService *light = world_.lightService()) {
      world_.flushPendingLightCells();
      (void)light->flushAndWait(std::chrono::milliseconds{5000});
    }
    drain();
    for (;;) {
      bool queued = false;
      bool waiting = false;
      for (auto &[position, chunk] : managed_) {
        auto status = controller_->status(position);
        if (!status)
          continue;
        if (chunk.inFlightVersion != 0 &&
            status->savedDirtyVersion >= chunk.inFlightVersion) {
          const auto completedVersion = chunk.inFlightVersion;
          chunk.inFlightVersion = 0;
          chunk.inFlightPatch.reset();
          chunk.retainedPatch.reset();
          chunk.failedVersion = 0;
          entityMigrationSaveCompleted(position, completedVersion, true);
        } else if (chunk.inFlightVersion != 0 &&
                   status->state == ChunkResidencyState::Resident &&
                   status->failure == ChunkLoadFailure::Io &&
                   status->savedDirtyVersion < chunk.inFlightVersion) {
          const auto failedVersion = chunk.inFlightVersion;
          chunk.failedVersion = failedVersion;
          chunk.inFlightVersion = 0;
          chunk.inFlightPatch.reset();
          entityMigrationSaveCompleted(position, failedVersion, false);
        }
        if (!chunk.blocker.empty())
          continue;
        std::string dynamicReason;
        if (persistenceBlocker(position, dynamicReason)) {
          noteDynamicBlocker(position, std::move(dynamicReason));
          continue;
        }
        chunk.dynamicBlocked = false;
        if (chunk.inFlightVersion != 0 ||
            status->state == ChunkResidencyState::Saving) {
          waiting = true;
          continue;
        }
        if (status->dirtyVersion > status->savedDirtyVersion &&
            chunk.failedVersion != status->dirtyVersion) {
          if (scheduleSave(position, chunk, true))
            queued = true;
        }
      }
      if (queued) {
        drain();
        continue;
      }
      if (waiting) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        drain();
        continue;
      }
      break;
    }

    // Ticks installed in the last real game tick are queued only on that
    // server tick. Give accepted saves one bounded tick to reach the worker.
    if (controller_) {
      for (int pass = 0; pass < 2000; ++pass) {
        bool waiting = false;
        drain();
        for (auto &[position, chunk] : managed_) {
          const auto status = controller_->status(position);
          if (!status)
            continue;
          const bool serverTickOwned =
              chunk.dirtyVersion > status->dirtyVersion;
          if (serverTickOwned)
            (void)controller_->markDirty(position, chunk.dirtyVersion);
          const auto refreshed = controller_->status(position);
          if (!refreshed)
            continue;
          if (chunk.inFlightVersion != 0 ||
              (refreshed->state == ChunkResidencyState::Saving)) {
            waiting = true;
            continue;
          }
          if (refreshed->dirtyVersion >
                  refreshed->savedDirtyVersion &&
              chunk.failedVersion != refreshed->dirtyVersion) {
            if (scheduleSave(position, chunk, true))
              waiting = true;
          }
        }
        if (!waiting)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    for (const ChunkTicketPos &position : initiallyManaged) {
      auto found = managed_.find(position);
      if (found == managed_.end())
        continue;
      ManagedChunk &chunk = found->second;
      const auto status = controller_->status(position);
      if (!status)
        continue;
      std::string reason = chunk.blocker;
      if (reason.empty() &&
          status->dirtyVersion > status->savedDirtyVersion)
        reason = status->detail.empty() ? "dirty chunk was not saved"
                                        : status->detail;
      if (!reason.empty()) {
        unsaved.insert(position);
        ChunkShutdownIssue issue;
        issue.position = position;
        issue.state = status->state;
        issue.dirtyVersion = status->dirtyVersion;
        issue.savedDirtyVersion = status->savedDirtyVersion;
        issue.error = std::move(reason);
        result.unsavedChunks.push_back(std::move(issue));
      } else if (status->dirtyVersion != 0) {
        savedCandidates.push_back(position);
      }
    }

    if (unsaved.empty()) {
      for (const ChunkTicketPos &position : initiallyManaged) {
        if (!managed_.contains(position))
          continue;
        controller_->updateDemand(position, ticket_levels::NoTicketSource);
        (void)controller_->retryEviction(position);
        drain();
      }
      for (const ChunkTicketPos &position : initiallyManaged) {
        auto found = managed_.find(position);
        if (found == managed_.end() || unsaved.contains(position))
          continue;
        const auto status = controller_->status(position);
        ChunkShutdownIssue issue;
        issue.position = position;
        issue.state = status ? status->state : ChunkResidencyState::Absent;
        issue.dirtyVersion = status ? status->dirtyVersion : 0;
        issue.savedDirtyVersion = status ? status->savedDirtyVersion : 0;
        issue.error = found->second.blocker.empty()
                          ? "managed chunk could not be evicted during shutdown"
                          : found->second.blocker;
        unsaved.insert(position);
        result.unsavedChunks.push_back(std::move(issue));
      }
    }

    controller_->close();
    result.workerStopped = true;
    drain();
    detachWorldCallbacks();
  } catch (const std::exception &error) {
    result.fatalError = error.what();
    if (controller_)
      controller_->close();
    result.workerStopped = true;
    detachWorldCallbacks();
    for (const ChunkTicketPos &position : initiallyManaged) {
      if (unsaved.contains(position))
        continue;
      auto found = managed_.find(position);
      if (found == managed_.end())
        continue;
      const auto status = controller_->status(position);
      ChunkShutdownIssue issue;
      issue.position = position;
      issue.state = status ? status->state : ChunkResidencyState::Absent;
      issue.dirtyVersion = status ? status->dirtyVersion : 0;
      issue.savedDirtyVersion = status ? status->savedDirtyVersion : 0;
      issue.error = result.fatalError;
      unsaved.insert(position);
      result.unsavedChunks.push_back(std::move(issue));
    }
  }

  for (const ChunkTicketPos &position : savedCandidates) {
    if (!unsaved.contains(position))
      result.savedChunks.push_back(position);
  }
  result.complete = result.workerStopped && result.unsavedChunks.empty() &&
                    result.fatalError.empty();
  closed_ = true;
  lastShutdownResult_ = result;
  return result;
}

bool WorldChunkManager::installLoaded(LifecycleLoadedChunk &&loaded) {
  const ChunkTicketPos position = loaded.position;
  if (!accepting()) {
    errors_[position] = "late chunk read rejected during shutdown";
    return false;
  }
  if (loaded.hasUnsupportedBlockEntities) {
    errors_[position] = loaded.blockEntityError.empty()
                            ? "chunk has unsupported persisted block entities"
                            : loaded.blockEntityError;
    return false;
  }
  if (!loaded.entities.empty()) {
    errors_[position] = "terrain-only T-4b-1 refuses chunk with saved entities";
    return false;
  }
  EntityChunkDataResult entityData =
      loaded.entityData.value_or(EntityChunkDataResult{});
  if (entityData.status == EntityChunkDataResult::Status::Unsupported ||
      entityData.status == EntityChunkDataResult::Status::Error ||
      (entityData.status == EntityChunkDataResult::Status::NonEmpty &&
       loaded.entityRecords.empty())) {
    errors_[position] =
        "entity chunk data is not safely loadable: " + entityData.detail;
    return false;
  }

  const auto inChunk = [&](const terrain::SavedTick &tick) {
    return (tick.x >> 4) == position.x && (tick.z >> 4) == position.z;
  };
  std::size_t blockCount = 0;
  std::size_t fluidCount = 0;
  for (const terrain::SavedTick &tick : loaded.blockTicks)
    blockCount += inChunk(tick) ? 1 : 0;
  for (const terrain::SavedTick &tick : loaded.fluidTicks)
    fluidCount += inChunk(tick) ? 1 : 0;
  std::int64_t blockOrder = -static_cast<std::int64_t>(blockCount);
  std::int64_t fluidOrder = -static_cast<std::int64_t>(fluidCount);
  std::vector<ScheduledTick> restoredBlockTicks;
  std::vector<std::pair<ScheduledTick, FluidRef>> restoredFluidTicks;
  restoredBlockTicks.reserve(blockCount);
  restoredFluidTicks.reserve(fluidCount);
  for (const terrain::SavedTick &tick : loaded.blockTicks) {
    if (!inChunk(tick))
      continue;
    if (!isSupportedBlock(tick.id)) {
      errors_[position] = "unsupported saved block tick type: " + tick.id;
      return false;
    }
    const BlockRef block(tick.id);
    if (block.empty()) {
      errors_[position] = "unknown saved block tick type: " + tick.id;
      return false;
    }
    restoredBlockTicks.push_back(
        {{tick.x, tick.y, tick.z}, block,
         wrappingAdd(world_.gameTime(), tick.delay),
         std::clamp(tick.priority, -3, 3), blockOrder++});
  }
  for (const terrain::SavedTick &tick : loaded.fluidTicks) {
    if (!inChunk(tick))
      continue;
    const FluidRef fluid(tick.id);
    if (fluid.empty()) {
      errors_[position] = "unknown saved fluid tick type: " + tick.id;
      return false;
    }
    restoredFluidTicks.push_back(
        {{ScheduledTick{{tick.x, tick.y, tick.z}, BlockRef{},
                        wrappingAdd(world_.gameTime(), tick.delay),
                        std::clamp(tick.priority, -3, 3), fluidOrder++}},
         fluid});
  }

  World::Chunk chunk;
  chunk.loaded = false;
  std::set<int> writableSections;

  for (terrain::Section &loadedSection : loaded.sections) {
    if (!loadedSection.blockStates)
      continue;
    const int sectionY = loadedSection.blockStates ? loadedSection.y : 0;
    const int baseY = sectionY * 16;
    if (baseY < world_.minY() || baseY + 15 >= world_.maxY()) {
      errors_[position] = "decoded section Y is outside World height";
      return false;
    }
    const int sectionIndex = world_.sectionIndex(baseY);
    if (sectionIndex < 0 || sectionIndex >= world_.sectionCount()) {
      errors_[position] = "decoded section cannot map into World storage";
      return false;
    }
    terrain::PackedBlockStates &packed = *loadedSection.blockStates;
    World::Section &section = chunk.sections[sectionIndex];
    section.tickingBlockCount = 0;
    section.randomTickByMinisection.fill(0);
    if (packed.palette.size() == 1) {
      const BlockState::Id id = packed.palette.front().state.id();
      section.single = id;
      section.nonEmpty = BlockState::fromId(id).isAir() ? 0 : 4096;
      section.initSingleRandomTick(id);
    } else {
      const std::uint8_t targetBits =
          packed.palette.size() > 256
              ? 16
              : (world_.lowMemoryMode()
                     ? World::minBitsForPaletteSize(packed.palette.size())
                     : 8);
      auto storage = World::makeSectionStorageForBits(targetBits);
      std::vector<std::uint8_t> paletteTicking(packed.palette.size(), 0);
      for (std::size_t i = 0; i < packed.palette.size(); ++i)
        paletteTicking[i] =
            stateIsRandomlyTickingId(packed.palette[i].state.id()) ? 1 : 0;
      std::uint16_t tickingCount = 0;
      std::uint16_t nonEmpty = 0;
      bool outOfRange = false;
      World::visitSectionStorage(storage.get(), [&](auto *block) {
        using BlockT = std::remove_pointer_t<decltype(block)>;
        const std::size_t paletteCount =
            std::min(packed.palette.size(), BlockT::kPaletteCapacity);
        block->paletteSize = static_cast<std::uint16_t>(paletteCount);
        for (std::size_t i = 0; i < paletteCount; ++i) {
          block->palette[i] = packed.palette[i].state.id();
          block->paletteTicking[i] = paletteTicking[i];
        }
        for (std::size_t i = 0; i < terrain::PackedBlockStates::CellCount;
             ++i) {
          const std::size_t value = packed.cell(i);
          if (value >= paletteCount) {
            outOfRange = true;
            return;
          }
          block->setIndexAt(i, static_cast<std::uint16_t>(value));
          if (!packed.palette[value].state.isAir())
            ++nonEmpty;
          if (paletteTicking[value]) {
            ++tickingCount;
            ++section.randomTickByMinisection
                  [i / World::kRandomTickMinisectionSize];
          }
        }
      });
      if (outOfRange) {
        errors_[position] = "decoded palette cell is out of range";
        return false;
      }
      section.nonEmpty = nonEmpty;
      section.tickingBlockCount = tickingCount;
      section.storage = std::move(storage);
    }
    writableSections.insert(sectionY);
  }
  chunk.refreshRandomTickMask(world_.sectionCount());

  chunk.heightmaps = std::make_unique<ChunkHeightmaps>(
      world_.minY(), world_.sectionCount() * 16);
  int highestSectionPosition = world_.minY();
  for (int section = world_.sectionCount(); section-- > 0;) {
    if (!chunk.sections[section].hasOnlyAir()) {
      highestSectionPosition = (world_.minY() >> 4) * 16 + section * 16 + 16;
      break;
    }
  }
  chunk.heightmaps->prime(
      highestSectionPosition, [&chunk, this](int x, int y, int z) {
        const int section = world_.sectionIndex(y);
        if (section < 0 || section >= world_.sectionCount())
          return airState();
        const int index = ((y & 15) << 8) | ((z & 15) << 4) | (x & 15);
        return BlockState::fromId(chunk.sections[section].get(index));
      });

  std::vector<std::shared_ptr<PistonMovingBlockEntity>> movingPistons;
  movingPistons.reserve(loaded.movingPistons.size());
  for (const terrain::MovingPistonEntity &decoded : loaded.movingPistons) {
    if (decoded.y < world_.minY() || decoded.y >= world_.maxY()) {
      errors_[position] = "moving piston Y is outside World height";
      return false;
    }
    auto piston = std::make_shared<PistonMovingBlockEntity>();
    piston->pos = {decoded.x, decoded.y, decoded.z};
    piston->movedState = decoded.movedState;
    piston->direction = static_cast<Direction>(decoded.facing);
    piston->extending = decoded.extending;
    piston->source = decoded.source;
    piston->progress = decoded.progressO;
    piston->progressO = decoded.progressO;
    piston->lastTicked = -1;
    movingPistons.push_back(std::move(piston));
  }

  std::vector<std::shared_ptr<Entity>> storedEntities;
  storedEntities.reserve(loaded.entityRecords.size());
  std::vector<std::pair<EntityUuid, Vec3>> entityPositions;
  entityPositions.reserve(loaded.entityRecords.size());
  std::set<EntityUuid> entityUuids;
  for (const entity_codec::EntityRecord &record : loaded.entityRecords) {
    std::shared_ptr<Entity> entity;
    EntityUuid uuid{};
    Vec3 position;
    std::visit(
        [&](const auto &decoded) {
          using T = std::decay_t<decltype(decoded)>;
          uuid = decoded.uuid;
          position = {decoded.pos[0], decoded.pos[1], decoded.pos[2]};
          if constexpr (std::is_same_v<T, entity_codec::ItemEntityData>) {
            ItemStack stack = itemStack(decoded.itemId, decoded.itemCount);
            if (stack.empty()) {
              errors_[loaded.position] = "persisted item stack is empty";
              return;
            }
            auto item = std::make_shared<ItemEntity>(
                world_, position, stack,
                Vec3{decoded.motion[0], decoded.motion[1], decoded.motion[2]});
            item->yaw = decoded.rotation[0];
            item->pitch = decoded.rotation[1];
            item->onGround = decoded.onGround;
            item->remainingFireTicks = decoded.fireTicks;
            item->fallDistance = decoded.fallDistance;
            item->age = decoded.age;
            item->pickupDelay = decoded.pickupDelay;
            item->health = decoded.health;
            entity = std::move(item);
          } else if constexpr (std::is_same_v<T,
                                              entity_codec::TntEntityData>) {
            auto tnt = std::make_shared<PrimedTnt>(
                world_, position,
                Vec3{decoded.motion[0], decoded.motion[1], decoded.motion[2]});
            tnt->yaw = decoded.rotation[0];
            tnt->pitch = decoded.rotation[1];
            tnt->onGround = decoded.onGround;
            tnt->remainingFireTicks = decoded.fireTicks;
            tnt->fallDistance = decoded.fallDistance;
            tnt->fuse = decoded.fuse;
            tnt->explosionPower = decoded.explosionPower;
            tnt->blockState = defaultState("tnt");
            tnt->usedPortal = false;
            entity = std::move(tnt);
          } else if constexpr (std::is_same_v<
                                   T, entity_codec::FallingBlockEntityData>) {
            auto falling = std::make_shared<FallingBlockEntity>(
                world_, position, defaultState(decoded.blockId));
            falling->setDeltaMovement(
                Vec3{decoded.motion[0], decoded.motion[1], decoded.motion[2]});
            falling->yaw = decoded.rotation[0];
            falling->pitch = decoded.rotation[1];
            falling->onGround = decoded.onGround;
            falling->remainingFireTicks = decoded.fireTicks;
            falling->fallDistance = decoded.fallDistance;
            falling->time = decoded.time;
            falling->dropItem = decoded.dropItem;
            falling->cancelDrop = decoded.cancelDrop;
            falling->hurtEntities = decoded.hurtEntities;
            falling->fallDamagePerDistance =
                decoded.fallDamagePerDistance;
            falling->fallDamageMax = decoded.fallDamageMax;
            entity = std::move(falling);
          } else {
            auto minecart = std::make_shared<Minecart>(world_, position);
            minecart->setDeltaMovement(
                Vec3{decoded.motion[0], decoded.motion[1], decoded.motion[2]});
            minecart->yaw = decoded.rotation[0];
            minecart->pitch = decoded.rotation[1];
            minecart->onGround = decoded.onGround;
            minecart->remainingFireTicks = decoded.fireTicks;
            minecart->fallDistance = decoded.fallDistance;
            minecart->flipped = decoded.flipped;
            minecart->firstTick = decoded.firstTick;
            entity = std::move(minecart);
          }
        },
        record.payload);
    if (!entity)
      return false;
    if (uuid == EntityUuid{} || !entityUuids.insert(uuid).second ||
        world_.hasEntityUuid(uuid)) {
      errors_[loaded.position] = "duplicate or zero persisted entity UUID";
      return false;
    }
    entity->uuid = uuid;
    entityPositions.emplace_back(uuid, position);
    storedEntities.push_back(std::move(entity));
  }

  chunk.lightLifecycleEpoch = ++nextLightLifecycleEpoch_;
  if (loaded.isLightOn) {
    PersistedChunkLight persisted;
    persisted.isLightOn = true;
    const int minLightSec = (world_.minY() >> 4) - 1;
    const int maxLightSec = (world_.minY() >> 4) + world_.sectionCount();
    for (const terrain::Section &sec : loaded.sections) {
      if ((!sec.blockLight && !sec.skyLight) || sec.y < minLightSec ||
          sec.y > maxLightSec)
        continue;
      PersistedLightSection lightSec;
      lightSec.sectionY = sec.y;
      lightSec.blockLight = sec.blockLight;
      lightSec.skyLight = sec.skyLight;
      persisted.sections.push_back(std::move(lightSec));
    }
    chunk.persistedLight = std::move(persisted);
  }
  try {
    if (!world_.installManagedChunk(position.x, position.z,
                                    std::move(chunk), restoredBlockTicks,
                                    restoredFluidTicks, movingPistons,
                                    storedEntities)) {
      errors_[position] = "World already has a chunk node at this position";
      return false;
    }
  } catch (const std::exception &error) {
    errors_[position] = error.what();
    return false;
  }
  for (const auto &[uuid, position] : entityPositions)
    entityLastPositions_[uuid] = position;

  ManagedChunk managed;
  const auto status = controller_->status(position);
  managed.epoch = status ? status->epoch : loaded.epoch;
  managed.lastObservedGameTime = world_.gameTime();
  managed.entityDataReady =
      static_cast<bool>(entityStore_) ||
      entityData.status == EntityChunkDataResult::Status::Empty ||
      !loaded.entityRecords.empty();
  managed.entitySlotPresent = loaded.entitySlotPresent;
  managed.entityDataDetail = std::move(entityData.detail);
  managed.writableSections = std::move(writableSections);
  managed.initialLightComplete = loaded.isLightOn;
  managed.needsLightSave = world_.lightingEnabled() && !loaded.isLightOn;
  if (managed.needsLightSave)
    pendingLightSave_.insert(position);
  managed_.emplace(position, std::move(managed));
  if (auto targets = entityMigrationTargets_.find(position);
      targets != entityMigrationTargets_.end()) {
    auto &target = managed_.at(position);
    ++target.dirtyVersion;
    (void)controller_->markDirty(position, target.dirtyVersion);
    for (const EntityUuid &uuid : targets->second)
      enqueueMigrationWork(uuid);
    enqueueAutosaveCandidate(position);
  }
  if (blockCount != 0 || fluidCount != 0)
    tickTimeSensitive_.insert(position);
  errors_.erase(position);
  refreshReadinessAround(position);
  if (managedChunkInstalled)
    managedChunkInstalled(position);
  return true;
}

bool WorldChunkManager::evictLoaded(const ChunkTicketPos &position,
                                    std::uint64_t epoch) {
  auto found = managed_.find(position);
  if (found == managed_.end() || found->second.epoch != epoch)
    return false;
  std::string reason;
  if (entityMigrationPinned(position)) {
    errors_[position] =
        "item source remains pinned until its entity list save succeeds";
    found->second.blocker.clear();
    if (found->second.inFlightVersion == 0) {
      ++found->second.dirtyVersion;
      (void)controller_->markDirty(position, found->second.dirtyVersion);
      enqueueAutosaveCandidate(position);
    }
    return false;
  }
  if (persistenceBlocker(position, reason)) {
    noteDynamicBlocker(position, std::move(reason));
    return false;
  }
  found->second.dynamicBlocked = false;
  bool allowTransientDrop = false;
  if (transientUnloadBlocker(position, reason)) {
    if (!found->second.transientEventRetryScheduled) {
      found->second.transientEventRetryScheduled = true;
      found->second.transientEventRetryTime = world_.gameTime() + 1;
      errors_[position] = std::move(reason);
      return false;
    }
    if (world_.gameTime() < found->second.transientEventRetryTime) {
      errors_[position] = std::move(reason);
      return false;
    }
    allowTransientDrop = true;
  }
  const auto status = controller_->status(position);
  if (status &&
      status->dirtyVersion > status->savedDirtyVersion) {
    errors_[position] = "dirty managed chunk cannot be evicted";
    return false;
  }
  const World::ItemEntitySnapshotResult unloadedItems =
      world_.snapshotChunkItemEntities(position.x, position.z);
  if (!world_.removeManagedChunk(position.x, position.z, reason,
                                 allowTransientDrop)) {
    found->second.blocker = reason;
    errors_[position] = std::move(reason);
    return false;
  }
  if (!unloadedItems.hasUnsupported) {
    for (const auto &record : unloadedItems.records)
      entityLastPositions_.erase(entity_codec::recordUuid(record));
  }
  if (found->second.cooldownScheduled) {
    heldSaveCooldowns_.erase({found->second.nextSaveAttempt, position});
    noDemandSaveCooldowns_.erase({found->second.nextSaveAttempt, position});
  }
  autosaveQueued_.erase(position);
  demandSaveQueued_.erase(position);
  evictionRetryQueued_.erase(position);
  savingInFlight_.erase(position);
  heldDynamicQueued_.erase(position);
  noDemandDynamicQueued_.erase(position);
  pendingLightSave_.erase(position);
  lightNotificationQueue_.erase(position);
  tickTimeSensitive_.erase(position);
  managed_.erase(found);
  errors_.erase(position);
  refreshReadinessAround(position);
  if (managedChunkRemoved)
    managedChunkRemoved(position);
  return true;
}

void WorldChunkManager::recordBlockMutation(Pos pos,
                                            const BlockState &oldState,
                                            const BlockState &newState) {
  if (!accepting() || oldState == newState)
    return;
  const ChunkTicketPos position{pos.x >> 4, pos.z >> 4};
  auto found = managed_.find(position);
  if (found == managed_.end())
    return;
  ManagedChunk &chunk = found->second;
  const int sectionY = pos.y >> 4;
  if (!chunk.writableSections.contains(sectionY)) {
    chunk.blocker = "edited block belongs to a section absent from the save template";
    errors_[position] = chunk.blocker;
  }
  if (!isSupportedBlock(newState.name()) || !newState.isSchemaState()) {
    chunk.blocker = "edited block state is outside the persistent codec support";
    errors_[position] = chunk.blocker;
  }
  ++chunk.dirtyVersion;
  const std::uint16_t localIndex =
      static_cast<std::uint16_t>(((pos.y & 15) << 8) |
                                 ((pos.z & 15) << 4) | (pos.x & 15));
  const std::uint32_t cell =
      (static_cast<std::uint32_t>(
           static_cast<std::uint8_t>(static_cast<std::int8_t>(sectionY)))
       << 12) |
      localIndex;
  chunk.pendingEdits[cell] = newState.id();
  (void)controller_->markDirty(position, chunk.dirtyVersion);
  enqueueAutosaveCandidate(position);
}

void WorldChunkManager::recordTickMutation(Pos pos) {
  if (!accepting())
    return;
  const ChunkTicketPos position{pos.x >> 4, pos.z >> 4};
  auto found = managed_.find(position);
  if (found == managed_.end())
    return;
  ManagedChunk &chunk = found->second;
  chunk.lastObservedGameTime = world_.gameTime();
  if (world_.chunkTickCount(position.x, position.z) != 0)
    tickTimeSensitive_.insert(position);
  else
    tickTimeSensitive_.erase(position);
  ++chunk.dirtyVersion;
  (void)controller_->markDirty(position, chunk.dirtyVersion);
  enqueueAutosaveCandidate(position);
}

void WorldChunkManager::recordLightPublished(int chunkX, int chunkZ) {
  if (closed_.load(std::memory_order_acquire))
    return;
  const ChunkTicketPos position{chunkX, chunkZ};
  if (managed_.contains(position))
    lightNotificationQueue_.insert(position);
}

void WorldChunkManager::processLightSaveCompletions() {
  LightService *light = world_.lightService();
  if (!light || (lightNotificationQueue_.empty() && pendingLightSave_.empty()))
    return;
  std::set<ChunkTicketPos> candidates;
  candidates.swap(lightNotificationQueue_);
  candidates.insert(pendingLightSave_.begin(), pendingLightSave_.end());
  for (const ChunkTicketPos &position : candidates) {
    auto found = managed_.find(position);
    if (found == managed_.end()) {
      pendingLightSave_.erase(position);
      continue;
    }
    ManagedChunk &chunk = found->second;
    const World::Chunk *worldChunk = world_.chunkAt(position.x, position.z);
    if (!worldChunk) {
      pendingLightSave_.erase(position);
      continue;
    }
    const LightService::ChunkStatus lstatus =
        light->chunkStatus(position.x, position.z);
    const auto blockSnap =
        light->latestLayerChunk(LightLayer::Block, position.x, position.z);
    const bool skyEnabled = blockSnap ? blockSnap->skyLightEnabled : true;
    const auto skySnap =
        skyEnabled
            ? light->latestLayerChunk(LightLayer::Sky, position.x, position.z)
            : nullptr;
    const std::uint64_t satisfiedGen =
        skyEnabled
            ? std::min(lstatus.publishedBlockGeneration,
                       lstatus.publishedSkyGeneration)
            : lstatus.publishedBlockGeneration;
    const bool ready =
        lstatus.loaded && !lstatus.pending && lstatus.complete &&
        lstatus.lifecycleEpoch == worldChunk->lightLifecycleEpoch &&
        satisfiedGen >= worldChunk->requiredLightGeneration &&
        blockSnap &&
        blockSnap->lifecycleEpoch == worldChunk->lightLifecycleEpoch &&
        (!skyEnabled ||
         (skySnap && skySnap->lifecycleEpoch == worldChunk->lightLifecycleEpoch));
    if (!ready)
      continue;

    const std::uint64_t blockRev = blockSnap->revision;
    const std::uint64_t skyRev = skySnap ? skySnap->revision : 0;
    if (chunk.initialLightComplete && !chunk.initialLightBaselineRecorded) {
      chunk.savedBlockLightRevision = blockRev;
      chunk.savedSkyLightRevision = skyRev;
      chunk.initialLightBaselineRecorded = true;
      chunk.needsLightSave = false;
      pendingLightSave_.erase(position);
      continue;
    }
    if (chunk.needsLightSave ||
        blockRev != chunk.savedBlockLightRevision ||
        skyRev != chunk.savedSkyLightRevision) {
      chunk.needsLightSave = false;
      chunk.initialLightBaselineRecorded = true;
      pendingLightSave_.erase(position);
      ++chunk.dirtyVersion;
      (void)controller_->markDirty(position, chunk.dirtyVersion);
      enqueueAutosaveCandidate(position);
    }
  }
}

void WorldChunkManager::setMigrationTarget(
    const EntityUuid &uuid, EntityMigration &migration,
    const ChunkTicketPos &target) {
  if (migration.target && *migration.target == target)
    return;
  if (migration.target) {
    addMigrationSource(uuid, migration, *migration.target);
    auto oldTargets = entityMigrationTargets_.find(*migration.target);
    if (oldTargets != entityMigrationTargets_.end()) {
      oldTargets->second.erase(uuid);
      if (oldTargets->second.empty())
        entityMigrationTargets_.erase(oldTargets);
    }
  }
  removeMigrationSource(uuid, migration, target);
  migration.target = target;
  migration.targetEpoch = 0;
  migration.targetVersion = 0;
  migration.targetAcknowledged = false;
  entityMigrationTargets_[target].insert(uuid);
}

void WorldChunkManager::addMigrationSource(
    const EntityUuid &uuid, EntityMigration &migration,
    const ChunkTicketPos &source) {
  if (migration.clearSources.insert(source).second)
    entityMigrationSources_[source].insert(uuid);
}

void WorldChunkManager::removeMigrationSource(
    const EntityUuid &uuid, EntityMigration &migration,
    const ChunkTicketPos &source) {
  if (migration.clearSources.erase(source)) {
    auto found = entityMigrationSources_.find(source);
    if (found != entityMigrationSources_.end()) {
      found->second.erase(uuid);
      if (found->second.empty())
        entityMigrationSources_.erase(found);
    }
  }
  migration.sourceVersions.erase(source);
}

void WorldChunkManager::eraseMigration(const EntityUuid &uuid) {
  auto found = entityMigrations_.find(uuid);
  if (found == entityMigrations_.end())
    return;
  for (const ChunkTicketPos &source : found->second.clearSources) {
    auto indexed = entityMigrationSources_.find(source);
    if (indexed != entityMigrationSources_.end()) {
      indexed->second.erase(uuid);
      if (indexed->second.empty())
        entityMigrationSources_.erase(indexed);
    }
  }
  if (found->second.target) {
    auto target = entityMigrationTargets_.find(*found->second.target);
    if (target != entityMigrationTargets_.end()) {
      target->second.erase(uuid);
      if (target->second.empty())
        entityMigrationTargets_.erase(target);
    }
  }
  entityMigrations_.erase(found);
}

void WorldChunkManager::enqueueMigrationWork(const EntityUuid &uuid) {
  if (entityMigrationWorkQueued_.insert(uuid).second)
    entityMigrationWork_.push_back(uuid);
}

void WorldChunkManager::recordEntityMutation(
    Vec3 oldPosition, Vec3 newPosition,
    const EntityUuid &uuid, bool removed) {
  if (!accepting())
    return;
  const auto indexed = entityLastPositions_.find(uuid);
  const Vec3 indexedPosition =
      indexed == entityLastPositions_.end() ? oldPosition : indexed->second;
  const ChunkTicketPos oldChunk{
      static_cast<int>(std::floor(indexedPosition.x)) >> 4,
      static_cast<int>(std::floor(indexedPosition.z)) >> 4};
  const ChunkTicketPos newChunk{
      static_cast<int>(std::floor(newPosition.x)) >> 4,
      static_cast<int>(std::floor(newPosition.z)) >> 4};

  if (oldChunk != newChunk) {
    auto [entry, inserted] = entityMigrations_.try_emplace(uuid);
    EntityMigration &migration = entry->second;
    if (!inserted && migration.target != newChunk)
      setMigrationTarget(uuid, migration, newChunk);
    else if (inserted)
      setMigrationTarget(uuid, migration, newChunk);
    addMigrationSource(uuid, migration, oldChunk);
    enqueueMigrationWork(uuid);
    errors_[oldChunk] =
        "item source remains pinned until target entity save succeeds";
    errors_[newChunk] =
        "item target must save before source removal";
  }

  if (removed) {
    auto migration = entityMigrations_.find(uuid);
    if (migration != entityMigrations_.end()) {
      migration->second.removalObserved = true;
      if (migration->second.target != oldChunk)
        addMigrationSource(uuid, migration->second, oldChunk);
      if (migration->second.target != newChunk)
        addMigrationSource(uuid, migration->second, newChunk);
      auto target = managed_.find(newChunk);
      if (migration->second.target == newChunk &&
          target != managed_.end() &&
          !target->second.entitySlotPresent) {
        // Removal is confirmed by an actual empty entity-list payload, even
        // when the destination had no entity slot before the handoff.
        target->second.entitySlotPresent = true;
      }
      enqueueMigrationWork(uuid);
    }
    entityLastPositions_.erase(uuid);
  } else {
    entityLastPositions_[uuid] = newPosition;
  }

  const auto touch = [&](const ChunkTicketPos &position) {
    auto found = managed_.find(position);
    if (found == managed_.end())
      return;
    ++found->second.dirtyVersion;
    (void)controller_->markDirty(position, found->second.dirtyVersion);
    enqueueAutosaveCandidate(position);
  };
  touch(oldChunk);
  if (newChunk != oldChunk)
    touch(newChunk);
}

bool WorldChunkManager::entityMigrationAllowsSave(
    const ChunkTicketPos &position) const {
  auto sources = entityMigrationSources_.find(position);
  if (sources == entityMigrationSources_.end())
    return true;
  for (const EntityUuid &uuid : sources->second) {
    auto migration = entityMigrations_.find(uuid);
    if (migration != entityMigrations_.end() &&
        !migration->second.targetAcknowledged)
      return false;
  }
  return true;
}

bool WorldChunkManager::entityMigrationPinned(
    const ChunkTicketPos &position) const {
  auto targets = entityMigrationTargets_.find(position);
  if (targets != entityMigrationTargets_.end()) {
    for (const EntityUuid &uuid : targets->second) {
      auto migration = entityMigrations_.find(uuid);
      if (migration != entityMigrations_.end() &&
          !migration->second.targetAcknowledged)
        return true;
    }
  }
  auto sources = entityMigrationSources_.find(position);
  if (sources == entityMigrationSources_.end())
    return false;
  auto status = controller_->status(position);
  for (const EntityUuid &uuid : sources->second) {
    auto migration = entityMigrations_.find(uuid);
    if (migration == entityMigrations_.end())
      continue;
    auto version = migration->second.sourceVersions.find(position);
    if (!status ||
        (version != migration->second.sourceVersions.end() &&
         (version->second.first != status->epoch ||
          status->savedDirtyVersion < version->second.second)))
      return true;
  }
  return false;
}

void WorldChunkManager::noteEntityMigrationSave(
    const ChunkTicketPos &position, std::uint64_t epoch,
    std::uint64_t version,
    const std::set<EntityUuid> &presentUuids) {
  auto targetIds = entityMigrationTargets_.find(position);
  if (targetIds != entityMigrationTargets_.end()) {
    for (const EntityUuid &uuid : targetIds->second) {
      auto found = entityMigrations_.find(uuid);
      if (found == entityMigrations_.end())
        continue;
      EntityMigration &migration = found->second;
      if (migration.target != position || migration.targetAcknowledged)
        continue;
      const bool acceptedAbsence = migration.removalObserved &&
                                   !presentUuids.contains(uuid);
      if (!presentUuids.contains(uuid) && !acceptedAbsence)
        continue;
      migration.targetEpoch = epoch;
      migration.targetVersion = version;
      enqueueMigrationWork(uuid);
    }
  }
  auto sourceIds = entityMigrationSources_.find(position);
  if (sourceIds != entityMigrationSources_.end()) {
    for (const EntityUuid &uuid : sourceIds->second) {
      auto found = entityMigrations_.find(uuid);
      if (found == entityMigrations_.end())
        continue;
      EntityMigration &migration = found->second;
      if (!migration.clearSources.contains(position))
        continue;
      if (presentUuids.contains(uuid)) {
        // A write that still contains the UUID cannot prove this source copy
        // was removed, even if another older request had the same version.
        migration.sourceVersions.erase(position);
        continue;
      }
      migration.sourceVersions[position] = {epoch, version};
      enqueueMigrationWork(uuid);
    }
  }
}

void WorldChunkManager::entityMigrationSaveCompleted(
    const ChunkTicketPos &position, std::uint64_t version, bool success) {
  std::vector<EntityUuid> affected;
  if (auto targets = entityMigrationTargets_.find(position);
      targets != entityMigrationTargets_.end())
    affected.insert(affected.end(), targets->second.begin(), targets->second.end());
  if (auto sources = entityMigrationSources_.find(position);
      sources != entityMigrationSources_.end())
    affected.insert(affected.end(), sources->second.begin(), sources->second.end());
  std::sort(affected.begin(), affected.end());
  affected.erase(std::unique(affected.begin(), affected.end()), affected.end());

  for (const EntityUuid &uuid : affected) {
    auto found = entityMigrations_.find(uuid);
    if (found == entityMigrations_.end())
      continue;
    EntityMigration &migration = found->second;
    if (!success) {
      if (migration.target == position &&
          migration.targetVersion == version &&
          !migration.targetAcknowledged)
        migration.targetVersion = 0;
      enqueueMigrationWork(uuid);
      continue;
    }
    auto target = migration.target ? managed_.find(*migration.target)
                                   : managed_.end();
    if (migration.target == position && !migration.targetAcknowledged &&
        migration.targetVersion == version &&
        target != managed_.end() &&
        migration.targetEpoch == target->second.epoch) {
      migration.targetAcknowledged = true;
      migration.targetVersion = 0;
    }
    auto sourceVersion = migration.sourceVersions.find(position);
    auto currentSource = managed_.find(position);
    if (migration.targetAcknowledged && currentSource != managed_.end() &&
        sourceVersion != migration.sourceVersions.end() &&
        sourceVersion->second.first == currentSource->second.epoch &&
        sourceVersion->second.second == version)
      removeMigrationSource(uuid, migration, position);
    enqueueMigrationWork(uuid);
  }

  for (const EntityUuid &uuid : affected) {
    auto found = entityMigrations_.find(uuid);
    if (found != entityMigrations_.end() &&
        found->second.targetAcknowledged &&
        found->second.clearSources.empty())
      eraseMigration(uuid);
  }
}

void WorldChunkManager::processMigrationWork(std::size_t maxEntries) {
  std::size_t entries = 0;
  while (entries < maxEntries && !entityMigrationWork_.empty()) {
    const EntityUuid uuid = entityMigrationWork_.front();
    entityMigrationWork_.pop_front();
    entityMigrationWorkQueued_.erase(uuid);
    ++entries;
    ++migrationWorkVisited_;
    auto found = entityMigrations_.find(uuid);
    if (found == entityMigrations_.end())
      continue;
    EntityMigration &migration = found->second;
    if (!migration.target)
      continue;
    auto target = managed_.find(*migration.target);
    if (!migration.targetAcknowledged) {
      if (target == managed_.end()) {
        for (const ChunkTicketPos &source : migration.clearSources)
          errors_[source] =
              "item migration target chunk is not resident and readable";
        continue;
      }
      auto status = controller_->status(*migration.target);
      if (status && status->dirtyVersion > status->savedDirtyVersion)
        enqueueAutosaveCandidate(*migration.target);
      continue;
    }
    auto targetStatus = controller_->status(*migration.target);
    if (targetStatus && !targetStatus->demand &&
        targetStatus->dirtyVersion <= targetStatus->savedDirtyVersion)
      enqueueEvictionRetry(*migration.target);
    const std::vector<ChunkTicketPos> sourceSnapshot(
        migration.clearSources.begin(), migration.clearSources.end());
    for (const ChunkTicketPos &source : sourceSnapshot) {
      if (!migration.clearSources.contains(source))
        continue;
      auto sourceChunk = managed_.find(source);
      if (sourceChunk == managed_.end()) {
        errors_[source] =
            "item migration source chunk is not resident";
        continue;
      }
      if (!sourceChunk->second.entitySlotPresent) {
        removeMigrationSource(uuid, migration, source);
        continue;
      }
      auto status = controller_->status(source);
      if (!status)
        continue;
      auto version = migration.sourceVersions.find(source);
      if (version != migration.sourceVersions.end() &&
          version->second.first != sourceChunk->second.epoch)
        migration.sourceVersions.erase(version);
      // Only the completion of the exact accepted empty-list snapshot may
      // remove a source. Scheduling another save is the sole fallback here.
      if (sourceChunk->second.inFlightVersion == 0) {
        if (!status->demand)
          enqueueDynamicRetry(source);
        else
          enqueueAutosaveCandidate(source);
      }
    }
    if (migration.targetAcknowledged && migration.clearSources.empty())
      eraseMigration(uuid);
  }
}

void WorldChunkManager::recordPistonMutation(Pos pos) {
  if (!accepting())
    return;
  const ChunkTicketPos position{pos.x >> 4, pos.z >> 4};
  auto found = managed_.find(position);
  if (found == managed_.end())
    return;
  ++found->second.dirtyVersion;
  (void)controller_->markDirty(position, found->second.dirtyVersion);
  enqueueAutosaveCandidate(position);
}

void WorldChunkManager::observeTickTime() {
  const std::int64_t now = world_.gameTime();
  if (lastObservedTickTime_ == now)
    return;
  lastObservedTickTime_ = now;
  for (auto it = tickTimeSensitive_.begin(); it != tickTimeSensitive_.end();) {
    const ChunkTicketPos position = *it;
    ++tickTimeEntriesVisited_;
    auto found = managed_.find(position);
    if (found == managed_.end()) {
      it = tickTimeSensitive_.erase(it);
      continue;
    }
    ManagedChunk &chunk = found->second;
    if (chunk.lastObservedGameTime == now) {
      ++it;
      continue;
    }
    chunk.lastObservedGameTime = now;
    if (world_.chunkTickCount(position.x, position.z) == 0) {
      it = tickTimeSensitive_.erase(it);
      continue;
    }
    ++chunk.dirtyVersion;
    (void)controller_->markDirty(position, chunk.dirtyVersion);
    enqueueAutosaveCandidate(position);
    ++it;
  }
}

bool WorldChunkManager::scheduleSave(const ChunkTicketPos &position,
                                     ManagedChunk &chunk, bool forceRetry) {
  if (!chunk.blocker.empty())
    return false;
  if (!entityMigrationAllowsSave(position)) {
    errors_[position] =
        "item migration is waiting for target entity save confirmation";
    chunk.blocker.clear();
    return false;
  }
  const auto status = controller_->status(position);
  if (!status ||
      (status->state != ChunkResidencyState::Resident &&
       status->state != ChunkResidencyState::EvictPending) ||
      status->state == ChunkResidencyState::Saving ||
      chunk.inFlightVersion != 0)
    return false;
  const auto now = std::chrono::steady_clock::now();
  if (!forceRetry && now < chunk.nextSaveAttempt)
    return false;
  if (forceRetry) {
    if (chunk.cooldownScheduled) {
      heldSaveCooldowns_.erase({chunk.nextSaveAttempt, position});
      noDemandSaveCooldowns_.erase({chunk.nextSaveAttempt, position});
    }
    chunk.cooldownScheduled = false;
    chunk.nextSaveAttempt = {};
    chunk.failedVersion = 0;
  }
  if (chunk.dirtyVersion <= status->savedDirtyVersion)
    return true;
  std::string dynamicReason;
  if (persistenceBlocker(position, dynamicReason)) {
    noteDynamicBlocker(position, std::move(dynamicReason));
    return false;
  }
  chunk.dynamicBlocked = false;

  ChunkLifecycleController::ChunkEditPatch editPatch = chunk.retainedPatch;
  if (!chunk.pendingEdits.empty())
    editPatch = makeEditPatch(chunk.pendingEdits, chunk.retainedPatch);

  const std::int64_t snapshotTime = world_.gameTime();
  chunk.lastObservedGameTime = snapshotTime;
  const ChunkTickSnapshot tickSnapshot =
      world_.snapshotChunkTicks(position.x, position.z);
  ChunkLifecycleController::ChunkSavePayload payload;
  payload.edits = editPatch;
  const std::vector<PistonSnapshot> pistonSnapshot =
      world_.snapshotChunkPistons(position.x, position.z);
  std::vector<terrain::MovingPistonEntity> movingPistons;
  movingPistons.reserve(pistonSnapshot.size());
  for (const PistonSnapshot &piston : pistonSnapshot) {
    movingPistons.push_back(
        {piston.pos.x, piston.pos.y, piston.pos.z, piston.movedState,
         static_cast<std::uint8_t>(piston.direction), piston.progressO,
         piston.extending, piston.source});
  }
  payload.movingPistons = std::move(movingPistons);
  World::ItemEntitySnapshotResult itemSnapshot =
      world_.snapshotChunkItemEntities(position.x, position.z);
  if (itemSnapshot.hasUnsupported) {
    noteDynamicBlocker(position, std::move(itemSnapshot.detail));
    return false;
  }
  if (!entityStore_ && !itemSnapshot.records.empty()) {
    noteDynamicBlocker(position, "entity store is unavailable");
    return false;
  }
  std::set<EntityUuid> presentUuids;
  bool hasEntityPayload = false;
  if (!itemSnapshot.records.empty() || chunk.entitySlotPresent) {
    hasEntityPayload = true;
    if (!itemSnapshot.records.empty())
      chunk.entitySlotPresent = true;
    for (const auto &record : itemSnapshot.records)
      presentUuids.insert(entity_codec::recordUuid(record));
    payload.entityRecords = std::move(itemSnapshot.records);
  }
  ChunkLifecycleController::ChunkTickSaveData tickSave;
  tickSave.snapshotTime = snapshotTime;
  tickSave.ticks.blockTicks.emplace();
  tickSave.ticks.fluidTicks.emplace();
  tickSave.ticks.blockTicks->reserve(tickSnapshot.blockTicks.size());
  for (const ScheduledTick &tick : tickSnapshot.blockTicks) {
    tickSave.ticks.blockTicks->push_back(
        {tick.block.str(), tick.pos.x, tick.pos.y, tick.pos.z,
         savedDelay(tick.trigger, snapshotTime), tick.priority});
  }
  tickSave.ticks.fluidTicks->reserve(tickSnapshot.fluidTicks.size());
  for (const auto &[tick, fluid] : tickSnapshot.fluidTicks) {
    tickSave.ticks.fluidTicks->push_back(
        {fluid.str(), tick.pos.x, tick.pos.y, tick.pos.z,
         savedDelay(tick.trigger, snapshotTime), tick.priority});
  }
  payload.ticks = std::move(tickSave);

  if (LightService *light = world_.lightService()) {
    world_.flushPendingLightCells();
    const World::Chunk *worldChunk = world_.chunkAt(position.x, position.z);
    const std::uint64_t requiredGen =
        worldChunk ? worldChunk->requiredLightGeneration : 0;
    const LightService::ChunkStatus lstatus =
        light->chunkStatus(position.x, position.z);
    const auto blockSnap =
        light->latestLayerChunk(LightLayer::Block, position.x, position.z);
    const bool skyEnabled = blockSnap ? blockSnap->skyLightEnabled : true;
    const auto skySnap =
        skyEnabled
            ? light->latestLayerChunk(LightLayer::Sky, position.x, position.z)
            : nullptr;
    const std::uint64_t satisfiedGen =
        skyEnabled
            ? std::min(lstatus.publishedBlockGeneration,
                       lstatus.publishedSkyGeneration)
            : lstatus.publishedBlockGeneration;
    const bool lightReady =
        worldChunk && lstatus.loaded && !lstatus.pending &&
        lstatus.complete &&
        lstatus.lifecycleEpoch == worldChunk->lightLifecycleEpoch &&
        satisfiedGen >= requiredGen && blockSnap &&
        blockSnap->lifecycleEpoch == worldChunk->lightLifecycleEpoch &&
        (!skyEnabled ||
         (skySnap &&
          skySnap->lifecycleEpoch == worldChunk->lightLifecycleEpoch));
    terrain::ChunkLightPatch lightPatch;
    if (lightReady) {
      lightPatch.isLightOn = true;
      const int minLightSec = blockSnap->minLightSectionY();
      const int maxLightSec =
          blockSnap->minBlockSectionY + blockSnap->blockSectionCount;
      lightPatch.sections.reserve(
          static_cast<std::size_t>(maxLightSec - minLightSec + 1));
      auto extractLayer = [](const LightSectionSnapshot *sec,
                             LightLayer layer,
                             std::optional<std::array<std::uint8_t, 2048>> &out) {
        if (!sec)
          return;
        const auto &ref = sec->layers[static_cast<std::size_t>(layer)];
        if (ref.isUniform() && ref.uniformValue() != 0) {
          const std::uint8_t byte = static_cast<std::uint8_t>(
              ref.uniformValue() | (ref.uniformValue() << 4));
          out.emplace().fill(byte);
        } else if (ref.isMixed() && ref.shared()) {
          auto &arr = out.emplace();
          for (std::size_t i = 0; i < 2048; ++i)
            arr[i] = ref.shared()->bytes[i].load(std::memory_order_relaxed);
        }
      };
      for (int secY = minLightSec; secY <= maxLightSec; ++secY) {
        terrain::SectionLightPatch secPatch;
        secPatch.y = static_cast<std::int8_t>(secY);
        extractLayer(blockSnap->section(secY), LightLayer::Block,
                     secPatch.blockLight);
        if (skyEnabled && skySnap)
          extractLayer(skySnap->section(secY), LightLayer::Sky,
                       secPatch.skyLight);
        lightPatch.sections.push_back(std::move(secPatch));
      }
      chunk.savedBlockLightRevision = blockSnap->revision;
      chunk.savedSkyLightRevision = skySnap ? skySnap->revision : 0;
      chunk.initialLightBaselineRecorded = true;
      chunk.needsLightSave = false;
      pendingLightSave_.erase(position);
    } else {
      lightPatch.isLightOn = false;
      chunk.needsLightSave = true;
      pendingLightSave_.insert(position);
    }
    payload.light = std::move(lightPatch);
  }

  const std::uint64_t snapshotVersion = chunk.dirtyVersion;
  if (!controller_->requestSavePayload(position, snapshotVersion,
                                       std::move(payload))) {
    errors_[position] = "snapshot version was rejected by lifecycle owner";
    return false;
  }
  if (hasEntityPayload)
    noteEntityMigrationSave(position, chunk.epoch, snapshotVersion,
                            presentUuids);
  chunk.retainedPatch = std::move(editPatch);
  chunk.inFlightPatch = chunk.retainedPatch;
  chunk.pendingEdits.clear();
  chunk.inFlightVersion = snapshotVersion;
  savingInFlight_.insert(position);
  errors_.erase(position);
  return true;
}

ChunkTickReadiness WorldChunkManager::computeReadiness(
    const ChunkTicketPos &position) const {
  ChunkTickReadiness result;
  const ChunkDemandSnapshot demand = tickets_.query(position);
  result.loadingLevel = demand.loadingLevel;
  result.simulationLevel = demand.simulationLevel;
  auto center = managed_.find(position);
  result.entityDataReady =
      center != managed_.end() && center->second.entityDataReady;
  if (center != managed_.end() && !center->second.entityDataDetail.empty())
    result.limitation = center->second.entityDataDetail;
  else if (!result.entityDataReady)
    result.limitation = "entity chunk data is not known empty";

  const auto fullRing = [&](int radius) {
    for (int dx = -radius; dx <= radius; ++dx) {
      for (int dz = -radius; dz <= radius; ++dz) {
        const ChunkTicketPos nearby{position.x + dx, position.z + dz};
        if (!managed_.contains(nearby))
          return false;
        if (tickets_.query(nearby).loadingLevel >
            ticket_levels::FullChunkLevel)
          return false;
      }
    }
    return true;
  };
  result.blockTicking =
      result.entityDataReady &&
      demand.simulationLevel <= ticket_levels::BlockTickingLevel &&
      fullRing(1);
  result.entityTicking =
      result.entityDataReady &&
      demand.simulationLevel <= ticket_levels::EntityTickingLevel &&
      fullRing(2);
  return result;
}

void WorldChunkManager::refreshReadinessAround(
    const ChunkTicketPos &position) {
  for (int dx = -2; dx <= 2; ++dx) {
    for (int dz = -2; dz <= 2; ++dz) {
      const ChunkTicketPos nearby{position.x + dx, position.z + dz};
      if (managed_.contains(nearby))
        readiness_[nearby] = computeReadiness(nearby);
      else
        readiness_.erase(nearby);
    }
  }
}

void WorldChunkManager::refreshReadinessForChanges(
    std::span<const ChunkLevelChange> loading,
    std::span<const ChunkLevelChange> simulation) {
  std::set<ChunkTicketPos> affected;
  for (const ChunkLevelChange &change : loading) {
    for (int dx = -2; dx <= 2; ++dx)
      for (int dz = -2; dz <= 2; ++dz)
        affected.insert(
            {change.position.x + dx, change.position.z + dz});
  }
  for (const ChunkLevelChange &change : simulation)
    affected.insert(change.position);
  for (const ChunkTicketPos &position : affected) {
    if (managed_.contains(position))
      readiness_[position] = computeReadiness(position);
    else
      readiness_.erase(position);
  }
}

bool WorldChunkManager::persistenceBlocker(
    const ChunkTicketPos &position, std::string &reason) const {
  return world_.managedChunkPersistenceState(position.x, position.z, reason);
}

bool WorldChunkManager::transientUnloadBlocker(
    const ChunkTicketPos &position, std::string &reason) const {
  return world_.managedChunkTransientUnloadState(position.x, position.z,
                                                 reason);
}

} // namespace redstone
