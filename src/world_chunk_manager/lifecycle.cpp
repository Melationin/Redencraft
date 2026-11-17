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
  world_.enderPearlTicketObserver = [this](int chunkX, int chunkZ) {
    addEnderPearlTicket({chunkX, chunkZ}, 2);
    return ticket_types::EnderPearl.timeout;
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
  world_.enderPearlTicketObserver = {};
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

TicketSystemUpdate
WorldChunkManager::addPortalTicket(const ChunkTicketPos &position, int radius) {
  if (!accepting())
    return {};
  TicketSystemUpdate update;
  tickets_.tickets().addTicket(
      position,
      Ticket(ticket_types::Portal, ticket_levels::FullChunkLevel - radius),
      &update);
  applyTicketUpdate(update);
  return update;
}

TicketSystemUpdate
WorldChunkManager::addEnderPearlTicket(const ChunkTicketPos &position,
                                       int radius) {
  if (!accepting())
    return {};
  TicketSystemUpdate update;
  tickets_.tickets().addTicket(
      position,
      Ticket(ticket_types::EnderPearl, ticket_levels::FullChunkLevel - radius),
      &update);
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
} // namespace redstone
