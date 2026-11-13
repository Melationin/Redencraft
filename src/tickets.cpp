#include "redstone/tickets.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace redstone {
namespace {

int sourceLevel(const std::vector<Ticket> &tickets, TicketDemand demand) {
  std::optional<int> result;
  for (const Ticket &ticket : tickets) {
    const bool participates =
        demand == TicketDemand::Loading ? ticket.type().doesLoad()
                                        : ticket.type().doesSimulate();
    if (participates && (!result || ticket.level() < *result))
      result = ticket.level();
  }
  return result.value_or(ticket_levels::NoTicketSource);
}

void appendChanges(std::vector<TicketSourceChange> &destination,
                   std::span<const TicketSourceChange> source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

} // namespace

int TicketStorage::getTicketLevelAt(const std::vector<Ticket> &tickets,
                                    TicketDemand demand) {
  return sourceLevel(tickets, demand);
}

int TicketStorage::getTicketLevelAt(const ChunkTicketPos &position,
                                    TicketDemand demand) const {
  const auto found = tickets_.find(position);
  return found == tickets_.end() ? ticket_levels::NoTicketSource
                                 : getTicketLevelAt(found->second, demand);
}

const std::vector<Ticket> &
TicketStorage::getTickets(const ChunkTicketPos &position) const {
  static const std::vector<Ticket> empty;
  const auto found = tickets_.find(position);
  return found == tickets_.end() ? empty : found->second;
}

const std::vector<Ticket> &
TicketStorage::getDeactivatedTickets(const ChunkTicketPos &position) const {
  static const std::vector<Ticket> empty;
  const auto found = deactivatedTickets_.find(position);
  return found == deactivatedTickets_.end() ? empty : found->second;
}

bool TicketStorage::sameIdentity(const Ticket &left, const Ticket &right) {
  return left.type().name == right.type().name &&
         left.level() == right.level();
}

bool TicketStorage::addTicket(const ChunkTicketPos &position, Ticket ticket,
                              TicketStorageChange *out) {
  if (out)
    *out = {};
  auto &tickets = tickets_[position];
  for (Ticket &existing : tickets) {
    if (!sameIdentity(existing, ticket))
      continue;
    existing.resetTicksLeft();
    if (out) {
      out->renewed = true;
      out->changed = true;
      out->dirty = true;
    }
    return false;
  }

  const int oldSimulation =
      getTicketLevelAt(tickets, TicketDemand::Simulation);
  const int oldLoading = getTicketLevelAt(tickets, TicketDemand::Loading);
  tickets.push_back(std::move(ticket));
  const Ticket &stored = tickets.back();

  TicketStorageChange change;
  change.sourceChanges.reserve(2);
  change.inserted = true;
  change.changed = true;
  change.dirty = true;
  if (stored.type().doesSimulate() && stored.level() < oldSimulation)
    change.sourceChanges.push_back(
        {TicketDemand::Simulation, position, stored.level(), true});
  if (stored.type().doesLoad() && stored.level() < oldLoading)
    change.sourceChanges.push_back(
        {TicketDemand::Loading, position, stored.level(), true});
  if (stored.type().name == ticket_types::Forced.name)
    forcedChunks_.insert(position);
  if (out)
    *out = std::move(change);
  return true;
}

bool TicketStorage::removeTicket(const ChunkTicketPos &position,
                                 const Ticket &ticket,
                                 TicketStorageChange *out) {
  if (out)
    *out = {};
  const auto found = tickets_.find(position);
  if (found == tickets_.end())
    return false;

  auto &tickets = found->second;
  const auto match = std::find_if(
      tickets.begin(), tickets.end(),
      [&](const Ticket &candidate) { return sameIdentity(candidate, ticket); });
  if (match == tickets.end())
    return false;
  tickets.erase(match);

  TicketStorageChange change;
  change.sourceChanges.reserve(2);
  change.removed = true;
  change.changed = true;
  change.dirty = true;
  const int newSimulation =
      getTicketLevelAt(tickets, TicketDemand::Simulation);
  const int newLoading = getTicketLevelAt(tickets, TicketDemand::Loading);
  if (ticket.type().doesSimulate())
    change.sourceChanges.push_back(
        {TicketDemand::Simulation, position, newSimulation, false});
  if (ticket.type().doesLoad())
    change.sourceChanges.push_back(
        {TicketDemand::Loading, position, newLoading, false});
  if (tickets.empty())
    tickets_.erase(found);
  if (ticket.type().name == ticket_types::Forced.name)
    updateForcedChunks();
  if (out)
    *out = std::move(change);
  return true;
}

TicketStorageChange
TicketStorage::applyBatch(std::span<const TicketOperation> operations) {
  TicketStorageChange result;
  if (operations.empty())
    return result;

  std::set<ChunkTicketPos> affected;
  for (const TicketOperation &operation : operations)
    affected.insert(operation.position);

  struct Before {
    int loading = ticket_levels::NoTicketSource;
    int simulation = ticket_levels::NoTicketSource;
  };
  std::map<ChunkTicketPos, Before> before;
  for (const ChunkTicketPos &position : affected) {
    const auto found = tickets_.find(position);
    if (found == tickets_.end())
      continue;
    before[position] = {
        getTicketLevelAt(found->second, TicketDemand::Loading),
        getTicketLevelAt(found->second, TicketDemand::Simulation)};
  }

  bool forcedTouched = false;
  for (const TicketOperation &operation : operations) {
    forcedTouched |=
        operation.ticket.type().name == ticket_types::Forced.name;
    if (operation.kind == TicketOperationKind::Add) {
      auto &tickets = tickets_[operation.position];
      const auto duplicate = std::find_if(
          tickets.begin(), tickets.end(), [&](const Ticket &candidate) {
            return sameIdentity(candidate, operation.ticket);
          });
      if (duplicate != tickets.end()) {
        duplicate->resetTicksLeft();
        result.renewed = true;
      } else {
        tickets.push_back(operation.ticket);
        result.inserted = true;
      }
      result.changed = true;
      continue;
    }

    const auto found = tickets_.find(operation.position);
    if (found == tickets_.end())
      continue;
    auto &tickets = found->second;
    const auto match = std::find_if(
        tickets.begin(), tickets.end(), [&](const Ticket &candidate) {
          return sameIdentity(candidate, operation.ticket);
        });
    if (match == tickets.end())
      continue;
    tickets.erase(match);
    if (tickets.empty())
      tickets_.erase(found);
    result.removed = true;
    result.changed = true;
  }

  if (forcedTouched)
    updateForcedChunks();
  result.dirty = result.changed;

  for (const ChunkTicketPos &position : affected) {
    const auto prior = before.find(position);
    const int oldLoading = prior == before.end()
                               ? ticket_levels::NoTicketSource
                               : prior->second.loading;
    const int oldSimulation = prior == before.end()
                                  ? ticket_levels::NoTicketSource
                                  : prior->second.simulation;
    const auto found = tickets_.find(position);
    const int newLoading =
        found == tickets_.end()
            ? ticket_levels::NoTicketSource
            : getTicketLevelAt(found->second, TicketDemand::Loading);
    const int newSimulation =
        found == tickets_.end()
            ? ticket_levels::NoTicketSource
            : getTicketLevelAt(found->second, TicketDemand::Simulation);
    if (newSimulation != oldSimulation)
      result.sourceChanges.push_back({TicketDemand::Simulation, position,
                                      newSimulation,
                                      newSimulation < oldSimulation});
    if (newLoading != oldLoading)
      result.sourceChanges.push_back({TicketDemand::Loading, position,
                                      newLoading,
                                      newLoading < oldLoading});
  }
  return result;
}

TicketStorageChange TicketStorage::removeIf(
    const std::function<bool(Ticket &, const ChunkTicketPos &)> &predicate,
    TicketMap *removedOutput) {
  TicketStorageChange result;
  bool removedForced = false;
  for (auto entry = tickets_.begin(); entry != tickets_.end();) {
    const ChunkTicketPos position = entry->first;
    auto &tickets = entry->second;
    std::size_t kept = 0;
    bool removedLoading = false;
    bool removedSimulation = false;
    // Compact in place. A purge tick with no expiry must not allocate one
    // vector per ticket-bearing chunk.
    for (std::size_t index = 0; index < tickets.size(); ++index) {
      Ticket &ticket = tickets[index];
      if (!predicate(ticket, position)) {
        if (kept != index)
          tickets[kept] = std::move(ticket);
        ++kept;
        continue;
      }
      result.removed = true;
      result.changed = true;
      removedLoading |= ticket.type().doesLoad();
      removedSimulation |= ticket.type().doesSimulate();
      removedForced |= ticket.type().name == ticket_types::Forced.name;
      if (removedOutput)
        (*removedOutput)[position].push_back(std::move(ticket));
    }

    if (kept != tickets.size())
      tickets.erase(tickets.begin() + static_cast<std::ptrdiff_t>(kept),
                    tickets.end());
    if (removedLoading || removedSimulation) {
      result.dirty = true;
      if (removedLoading)
        result.sourceChanges.push_back(
            {TicketDemand::Loading, position,
             getTicketLevelAt(tickets, TicketDemand::Loading), false});
      if (removedSimulation)
        result.sourceChanges.push_back(
            {TicketDemand::Simulation, position,
             getTicketLevelAt(tickets, TicketDemand::Simulation), false});
    }
    if (tickets.empty())
      entry = tickets_.erase(entry);
    else
      ++entry;
  }
  if (removedForced)
    updateForcedChunks();
  return result;
}

TicketStorageChange
TicketStorage::purgeTimedTickets(const TicketHolderLookup &holders) {
  bool decremented = false;
  TicketStorageChange result = removeIf(
      [&](Ticket &ticket, const ChunkTicketPos &position) {
        if (!ticket.type().hasTimeout())
          return false;
        bool canExpire = ticket.type().canExpireIfUnloaded();
        if (!canExpire) {
          const TicketHolderState holder =
              holders ? holders(position) : TicketHolderState{};
          canExpire = !holder.present || holder.readyForSaving;
        }
        if (!canExpire)
          return false;
        ticket.decreaseTicksLeft();
        decremented = true;
        return ticket.isTimedOut();
      });
  result.changed |= decremented;
  result.dirty = true;
  return result;
}

TicketStorageChange TicketStorage::deactivateTicketsOnClosing() {
  TicketMap moved;
  TicketStorageChange result = removeIf(
      [](const Ticket &ticket, const ChunkTicketPos &) {
        return ticket.type().name != ticket_types::Unknown.name;
      },
      &moved);
  for (auto &[position, tickets] : moved) {
    auto &destination = deactivatedTickets_[position];
    for (Ticket &ticket : tickets)
      destination.push_back(std::move(ticket));
  }
  return result;
}

TicketStorageChange TicketStorage::activateAllDeactivatedTickets() {
  TicketMap parked = std::move(deactivatedTickets_);
  deactivatedTickets_.clear();
  TicketStorageChange result;
  for (auto &[position, tickets] : parked) {
    for (Ticket &ticket : tickets) {
      TicketStorageChange one;
      addTicket(position, std::move(ticket), &one);
      result.inserted |= one.inserted;
      result.renewed |= one.renewed;
      result.changed |= one.changed;
      result.dirty |= one.dirty;
      appendChanges(result.sourceChanges, one.sourceChanges);
    }
  }
  return result;
}

TicketStorageChange
TicketStorage::replaceTicketLevelOfType(const TicketType &type,
                                        int newLevel) {
  std::vector<std::pair<ChunkTicketPos, Ticket>> affected;
  for (const auto &[position, tickets] : tickets_)
    for (const Ticket &ticket : tickets)
      if (ticket.type().name == type.name)
        affected.emplace_back(position, ticket);

  TicketStorageChange result;
  for (const auto &[position, ticket] : affected) {
    TicketStorageChange removed;
    TicketStorageChange added;
    removeTicket(position, ticket, &removed);
    addTicket(position, Ticket(type, newLevel), &added);
    result.inserted |= added.inserted;
    result.renewed |= added.renewed;
    result.removed |= removed.removed;
    result.changed |= removed.changed || added.changed;
    result.dirty |= removed.dirty || added.dirty;
    appendChanges(result.sourceChanges, removed.sourceChanges);
    appendChanges(result.sourceChanges, added.sourceChanges);
  }
  return result;
}

bool TicketStorage::shouldKeepDimensionActive() const {
  for (const auto &[position, tickets] : tickets_) {
    (void)position;
    for (const Ticket &ticket : tickets)
      if (ticket.type().shouldKeepDimensionActive())
        return true;
  }
  return false;
}

bool TicketStorage::hasForcedTicket(const ChunkTicketPos &position) const {
  return forcedChunks_.contains(position);
}

std::vector<std::pair<ChunkTicketPos, Ticket>>
TicketStorage::persistentTickets() const {
  std::vector<std::pair<ChunkTicketPos, Ticket>> result;
  for (const TicketMap *source : {&tickets_, &deactivatedTickets_}) {
    for (const auto &[position, tickets] : *source)
      for (const Ticket &ticket : tickets)
        if (ticket.type().persist())
          result.emplace_back(position, ticket);
  }
  return result;
}

void TicketStorage::updateForcedChunks() {
  forcedChunks_.clear();
  for (const auto &[position, tickets] : tickets_)
    for (const Ticket &ticket : tickets)
      if (ticket.type().name == ticket_types::Forced.name) {
        forcedChunks_.insert(position);
        break;
      }
}

namespace ticket_levels {

std::optional<GenerationStatus> generationStatus(int level) {
  static constexpr std::array<GenerationStatus, RadiusAroundFullChunk + 1>
      statuses{
          GenerationStatus::Full,
          GenerationStatus::InitializeLight,
          GenerationStatus::Carvers,
          GenerationStatus::Biomes,
          GenerationStatus::StructureStarts,
          GenerationStatus::StructureStarts,
          GenerationStatus::StructureStarts,
          GenerationStatus::StructureStarts,
          GenerationStatus::StructureStarts,
          GenerationStatus::StructureStarts,
          GenerationStatus::StructureStarts,
          GenerationStatus::StructureStarts,
      };
  const int distance = level - FullChunkLevel;
  if (distance > RadiusAroundFullChunk)
    return std::nullopt;
  if (distance <= 0)
    return GenerationStatus::Full;
  return statuses[static_cast<std::size_t>(distance)];
}

int generationLevel(GenerationStatus status) {
  switch (status) {
  case GenerationStatus::Empty:
  case GenerationStatus::StructureStarts:
    return 44;
  case GenerationStatus::StructureReferences:
  case GenerationStatus::Biomes:
    return 36;
  case GenerationStatus::Noise:
  case GenerationStatus::Surface:
  case GenerationStatus::Carvers:
    return 35;
  case GenerationStatus::Features:
  case GenerationStatus::InitializeLight:
    return 34;
  case GenerationStatus::Light:
  case GenerationStatus::Spawn:
  case GenerationStatus::Full:
    return 33;
  }
  return NoTicketSource;
}

const char *generationStatusName(GenerationStatus status) {
  switch (status) {
  case GenerationStatus::Empty:
    return "empty";
  case GenerationStatus::StructureStarts:
    return "structure_starts";
  case GenerationStatus::StructureReferences:
    return "structure_references";
  case GenerationStatus::Biomes:
    return "biomes";
  case GenerationStatus::Noise:
    return "noise";
  case GenerationStatus::Surface:
    return "surface";
  case GenerationStatus::Carvers:
    return "carvers";
  case GenerationStatus::Features:
    return "features";
  case GenerationStatus::InitializeLight:
    return "initialize_light";
  case GenerationStatus::Light:
    return "light";
  case GenerationStatus::Spawn:
    return "spawn";
  case GenerationStatus::Full:
    return "full";
  }
  return "unknown";
}

const char *fullStatusName(FullChunkStatus status) {
  switch (status) {
  case FullChunkStatus::Inaccessible:
    return "inaccessible";
  case FullChunkStatus::Full:
    return "full";
  case FullChunkStatus::BlockTicking:
    return "block_ticking";
  case FullChunkStatus::EntityTicking:
    return "entity_ticking";
  }
  return "unknown";
}

} // namespace ticket_levels

ChunkLevelGraph::ChunkLevelGraph(TicketDemand demand) : demand_(demand) {}

int ChunkLevelGraph::noSourceLevel() const {
  return demand_ == TicketDemand::Loading ? ticket_levels::NoTicketSource
                                          : ticket_levels::SimulationTrackerMax;
}

int ChunkLevelGraph::storedBelow() const {
  return noSourceLevel();
}

int ChunkLevelGraph::levelAt(const ChunkTicketPos &position) const {
  const auto found = levels_.find(position);
  return found == levels_.end() ? noSourceLevel() : found->second;
}

std::optional<int>
ChunkLevelGraph::sourceLevel(const ChunkTicketPos &position) const {
  const auto found = sources_.find(position);
  if (found == sources_.end())
    return std::nullopt;
  return found->second;
}

std::vector<ChunkLevelChange> ChunkLevelGraph::applySourceChanges(
    std::span<const TicketSourceChange> changes) {
  lastRecomputedCells_ = 0;
  lastChangedCells_ = 0;
  struct EffectiveSource {
    ChunkTicketPos position;
    int oldLevel;
    int newLevel;
  };
  std::map<ChunkTicketPos, int> oldEffective;
  std::set<ChunkTicketPos> touched;
  for (const TicketSourceChange &change : changes) {
    if (change.demand != demand_)
      continue;
    if (!touched.contains(change.position)) {
      const auto old = sources_.find(change.position);
      oldEffective[change.position] =
          old == sources_.end() ? storedBelow() : old->second;
      touched.insert(change.position);
    }
    const int normalized =
        std::clamp(change.sourceLevel, 0, ticket_levels::NoTicketSource);
    if (normalized >= storedBelow())
      sources_.erase(change.position);
    else
      sources_[change.position] = normalized;
  }

  std::vector<EffectiveSource> effective;
  for (const ChunkTicketPos &position : touched) {
    const auto current = sources_.find(position);
    const int oldLevel = oldEffective[position];
    const int newLevel =
        current == sources_.end() ? storedBelow() : current->second;
    if (oldLevel != newLevel)
      effective.push_back({position, oldLevel, newLevel});
  }
  if (effective.empty())
    return {};
  ++recomputeCount_;

  // The old and new influence square for each changed source is exactly its
  // Chebyshev radius storedBelow()-1-level. Merge row intervals first so large
  // view-distance batches do not insert every source square separately.
  std::map<int, std::vector<std::pair<int, int>>> rows;
  for (const EffectiveSource &change : effective) {
    const int oldRadius = change.oldLevel < storedBelow()
                              ? storedBelow() - 1 - change.oldLevel
                              : -1;
    const int newRadius = change.newLevel < storedBelow()
                              ? storedBelow() - 1 - change.newLevel
                              : -1;
    const int radius = std::max(oldRadius, newRadius);
    if (radius < 0)
      continue;
    const std::int64_t minX =
        std::max<std::int64_t>(std::numeric_limits<int>::min(),
                               static_cast<std::int64_t>(change.position.x) -
                                   radius);
    const std::int64_t maxX =
        std::min<std::int64_t>(std::numeric_limits<int>::max(),
                               static_cast<std::int64_t>(change.position.x) +
                                   radius);
    const std::int64_t minZ =
        std::max<std::int64_t>(std::numeric_limits<int>::min(),
                               static_cast<std::int64_t>(change.position.z) -
                                   radius);
    const std::int64_t maxZ =
        std::min<std::int64_t>(std::numeric_limits<int>::max(),
                               static_cast<std::int64_t>(change.position.z) +
                                   radius);
    for (std::int64_t z = minZ; z <= maxZ; ++z) {
      rows[static_cast<int>(z)].push_back(
          {static_cast<int>(minX), static_cast<int>(maxX)});
    }
  }

  std::set<ChunkTicketPos> affected;
  for (auto &[z, intervals] : rows) {
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<int, int>> merged;
    for (const auto &[first, last] : intervals) {
      if (merged.empty() || static_cast<std::int64_t>(first) > static_cast<std::int64_t>(merged.back().second) + 1) {
        merged.push_back({first, last});
      } else {
        merged.back().second = std::max(merged.back().second, last);
      }
    }
    for (const auto &[first, last] : merged)
      for (std::int64_t x = first; x <= last; ++x)
        affected.insert({static_cast<int>(x), z});
  }

  std::vector<std::vector<ChunkTicketPos>> buckets(
      static_cast<std::size_t>(storedBelow()));
  const auto seed = [&](const ChunkTicketPos &position, int level) {
    if (level >= 0 && level < storedBelow())
      buckets[static_cast<std::size_t>(level)].push_back(position);
  };

  for (const ChunkTicketPos &position : affected) {
    const auto source = sources_.find(position);
    if (source != sources_.end())
      seed(position, source->second);
  }

  for (const ChunkTicketPos &position : affected) {
    for (int dx = -1; dx <= 1; ++dx) {
      for (int dz = -1; dz <= 1; ++dz) {
        if (dx == 0 && dz == 0)
          continue;
        const std::int64_t x =
            static_cast<std::int64_t>(position.x) + dx;
        const std::int64_t z =
            static_cast<std::int64_t>(position.z) + dz;
        if (x < std::numeric_limits<int>::min() ||
            x > std::numeric_limits<int>::max() ||
            z < std::numeric_limits<int>::min() ||
            z > std::numeric_limits<int>::max())
          continue;
        const ChunkTicketPos neighbor{static_cast<int>(x),
                                      static_cast<int>(z)};
        if (affected.contains(neighbor))
          continue;
        const int boundary = levelAt(neighbor);
        if (boundary + 1 < storedBelow())
          seed(position, boundary + 1);
      }
    }
  }

  std::map<ChunkTicketPos, int> next;
  for (int level = 0; level < storedBelow(); ++level) {
    for (const ChunkTicketPos &position : buckets[static_cast<std::size_t>(level)]) {
      const auto found = next.find(position);
      if (found != next.end() && found->second <= level)
        continue;
      next[position] = level;
      if (level + 1 >= storedBelow())
        continue;
      for (int dx = -1; dx <= 1; ++dx) {
        for (int dz = -1; dz <= 1; ++dz) {
          if (dx == 0 && dz == 0)
            continue;
          const std::int64_t x =
              static_cast<std::int64_t>(position.x) + dx;
          const std::int64_t z =
              static_cast<std::int64_t>(position.z) + dz;
          if (x < std::numeric_limits<int>::min() ||
              x > std::numeric_limits<int>::max() ||
              z < std::numeric_limits<int>::min() ||
              z > std::numeric_limits<int>::max())
            continue;
          const ChunkTicketPos neighbor{static_cast<int>(x),
                                        static_cast<int>(z)};
          if (affected.contains(neighbor))
            buckets[static_cast<std::size_t>(level + 1)].push_back(neighbor);
        }
      }
    }
  }

  std::vector<ChunkLevelChange> result;
  result.reserve(affected.size());
  for (const ChunkTicketPos &position : affected) {
    const auto oldFound = levels_.find(position);
    const auto newFound = next.find(position);
    const int oldLevel =
        oldFound == levels_.end() ? storedBelow() : oldFound->second;
    const int newLevel =
        newFound == next.end() ? storedBelow() : newFound->second;
    if (oldLevel != newLevel)
      result.push_back({position, oldLevel, newLevel});
  }
  for (const ChunkTicketPos &position : affected)
    levels_.erase(position);
  for (const auto &[position, level] : next)
    levels_[position] = level;
  lastRecomputedCells_ = affected.size();
  lastChangedCells_ = result.size();
  return result;
}

TicketSystemUpdate
TicketLevelSystem::applyStorageChange(TicketStorageChange change) {
  TicketSystemUpdate result;
  result.loadingChanges = loading_.applySourceChanges(change.sourceChanges);
  result.simulationChanges =
      simulation_.applySourceChanges(change.sourceChanges);
  result.storage = std::move(change);
  return result;
}

TicketSystemUpdate TicketLevelSystem::applyBatch(
    std::span<const TicketOperation> operations) {
  return applyStorageChange(storage_.applyBatch(operations));
}
TicketStorageChange TicketStorage::restorePersistentTickets(
    std::span<const std::pair<ChunkTicketPos, Ticket>> tickets) {
  TicketStorageChange result;
  if (tickets.empty())
    return result;
  std::set<ChunkTicketPos> affected;
  std::map<ChunkTicketPos, std::pair<int, int>> oldLevels;
  for (const auto &[position, ticket] : tickets) {
    if (affected.insert(position).second)
      oldLevels.emplace(
          position,
          std::pair{getTicketLevelAt(position, TicketDemand::Simulation),
                    getTicketLevelAt(position, TicketDemand::Loading)});
    tickets_[position].push_back(ticket);
    if (ticket.type().name == ticket_types::Forced.name)
      forcedChunks_.insert(position);
    result.inserted = true;
  }
  result.changed = result.dirty = true;
  for (const ChunkTicketPos &position : affected) {
    const auto [oldSimulation, oldLoading] = oldLevels.at(position);
    const int simulation = getTicketLevelAt(position, TicketDemand::Simulation);
    const int loading = getTicketLevelAt(position, TicketDemand::Loading);
    if (simulation != oldSimulation)
      result.sourceChanges.push_back(
          {TicketDemand::Simulation, position, simulation,
           simulation < oldSimulation});
    if (loading != oldLoading)
      result.sourceChanges.push_back(
          {TicketDemand::Loading, position, loading, loading < oldLoading});
  }
  return result;
}

TicketSystemUpdate TicketLevelSystem::restorePersistentTickets(
    std::span<const std::pair<ChunkTicketPos, Ticket>> tickets) {
  return applyStorageChange(storage_.restorePersistentTickets(tickets));
}


bool TicketLevelSystem::addTicket(const ChunkTicketPos &position, Ticket ticket,
                                  TicketSystemUpdate *out) {
  TicketStorageChange change;
  const bool stored = storage_.addTicket(position, std::move(ticket), &change);
  TicketSystemUpdate applied = applyStorageChange(std::move(change));
  if (out)
    *out = std::move(applied);
  return stored;
}

bool TicketLevelSystem::removeTicket(const ChunkTicketPos &position,
                                     const Ticket &ticket,
                                     TicketSystemUpdate *out) {
  TicketStorageChange change;
  const bool removed = storage_.removeTicket(position, ticket, &change);
  TicketSystemUpdate applied = applyStorageChange(std::move(change));
  if (out)
    *out = std::move(applied);
  return removed;
}

TicketSystemUpdate
TicketLevelSystem::purgeTimedTickets(const TicketHolderLookup &holders) {
  return applyStorageChange(storage_.purgeTimedTickets(holders));
}

TicketSystemUpdate TicketLevelSystem::deactivateTicketsOnClosing() {
  return applyStorageChange(storage_.deactivateTicketsOnClosing());
}

TicketSystemUpdate TicketLevelSystem::activateAllDeactivatedTickets() {
  return applyStorageChange(storage_.activateAllDeactivatedTickets());
}

TicketSystemUpdate
TicketLevelSystem::replaceTicketLevelOfType(const TicketType &type,
                                             int newLevel) {
  return applyStorageChange(
      storage_.replaceTicketLevelOfType(type, newLevel));
}

ChunkDemandSnapshot
TicketLevelSystem::query(const ChunkTicketPos &position) const {
  ChunkDemandSnapshot result;
  result.loadingLevel = loading_.levelAt(position);
  result.simulationLevel = simulation_.levelAt(position);
  result.schedulingScope =
      ticket_levels::isLoaded(result.loadingLevel);
  result.holderFullStatus =
      ticket_levels::fullStatus(result.loadingLevel);
  result.generationStatus =
      ticket_levels::generationStatus(result.loadingLevel);
  result.blockTickingDemand =
      ticket_levels::isBlockTicking(result.simulationLevel);
  result.entityTickingDemand =
      ticket_levels::isEntityTicking(result.simulationLevel);
  return result;
}

TicketSourceManager::TicketSourceManager(int viewDistance,
                                         int simulationDistance)
    : viewDistance_(viewDistance), simulationDistance_(simulationDistance) {
  if (viewDistance < MinViewDistance || viewDistance > MaxViewDistance ||
      simulationDistance < MinViewDistance ||
      simulationDistance > MaxViewDistance)
    throw std::invalid_argument("ticket source distance must be in [2, 32]");
}

int TicketSourceManager::playerSimulationLevel() const {
  return std::max(0, ticket_levels::EntityTickingLevel -
                         simulationDistance_);
}

std::size_t
TicketSourceManager::loadingCoverage(const ChunkTicketPos &position) const {
  const auto found = loadingCoverage_.find(position);
  return found == loadingCoverage_.end() ? 0 : found->second;
}

std::size_t
TicketSourceManager::simulationCoverage(const ChunkTicketPos &position) const {
  const auto found = simulationCoverage_.find(position);
  return found == simulationCoverage_.end() ? 0 : found->second;
}

bool TicketSourceManager::inSquare(const ChunkTicketPos &center,
                                   const ChunkTicketPos &position,
                                   int radius) {
  const std::int64_t dx =
      static_cast<std::int64_t>(center.x) - position.x;
  const std::int64_t dz =
      static_cast<std::int64_t>(center.z) - position.z;
  return std::max(dx < 0 ? -dx : dx, dz < 0 ? -dz : dz) <= radius;
}

TicketSourceUpdate TicketSourceManager::applyPlayerSources(
    std::vector<TicketOperation> operations, bool stateChanged,
    std::size_t loadingChanges, std::size_t simulationChanges) {
  TicketSourceUpdate result;
  result.stateChanged = stateChanged;
  result.loadingPresenceChanges = loadingChanges;
  result.simulationPresenceChanges = simulationChanges;
  if (!operations.empty())
    result.tickets = tickets_.applyBatch(operations);
  return result;
}

TicketSourceUpdate
TicketSourceManager::addPlayer(PlayerTicketId id, ChunkTicketPos center) {
  const auto existing = players_.find(id);
  if (existing != players_.end()) {
    if (existing->second.center == center)
      return {};
    return movePlayer(id, center);
  }

  players_.emplace(id, PlayerTicketSource{center});
  std::vector<TicketOperation> operations;
  operations.reserve(static_cast<std::size_t>(
      (viewDistance_ * 2 + 1) * (viewDistance_ * 2 + 1) + 1));
  std::size_t loadingChanges = 0;
  for (int x = -viewDistance_; x <= viewDistance_; ++x) {
    for (int z = -viewDistance_; z <= viewDistance_; ++z) {
      const ChunkTicketPos position{center.x + x, center.z + z};
      auto &count = loadingCoverage_[position];
      if (count++ == 0) {
        operations.push_back(TicketOperation::add(
            position, Ticket(ticket_types::PlayerLoading,
                             PlayerLoadingLevel)));
        ++loadingChanges;
      }
    }
  }

  auto &simulation = simulationCoverage_[center];
  const bool addedSimulation = simulation++ == 0;
  if (addedSimulation) {
    operations.push_back(TicketOperation::add(
        center, Ticket(ticket_types::PlayerSimulation,
                       playerSimulationLevel())));
  }
  return applyPlayerSources(std::move(operations), true, loadingChanges,
                            addedSimulation ? 1 : 0);
}

TicketSourceUpdate
TicketSourceManager::movePlayer(PlayerTicketId id, ChunkTicketPos center) {
  const auto player = players_.find(id);
  if (player == players_.end())
    return {};
  const ChunkTicketPos oldCenter = player->second.center;
  if (oldCenter == center)
    return {};

  std::vector<TicketOperation> operations;
  operations.reserve(static_cast<std::size_t>(
      2 * (viewDistance_ * 2 + 1) * (viewDistance_ * 2 + 1) + 2));
  std::size_t loadingChanges = 0;
  std::vector<ChunkTicketPos> removedLoading;
  for (int x = -viewDistance_; x <= viewDistance_; ++x) {
    for (int z = -viewDistance_; z <= viewDistance_; ++z) {
      const ChunkTicketPos oldPosition{oldCenter.x + x, oldCenter.z + z};
      if (inSquare(center, oldPosition, viewDistance_))
        continue;
      auto found = loadingCoverage_.find(oldPosition);
      if (found == loadingCoverage_.end())
        continue;
      if (--found->second == 0) {
        operations.push_back(TicketOperation::remove(
            oldPosition, Ticket(ticket_types::PlayerLoading,
                                PlayerLoadingLevel)));
        removedLoading.push_back(oldPosition);
        ++loadingChanges;
      }
    }
  }
  for (const ChunkTicketPos &position : removedLoading)
    loadingCoverage_.erase(position);
  for (int x = -viewDistance_; x <= viewDistance_; ++x) {
    for (int z = -viewDistance_; z <= viewDistance_; ++z) {
      const ChunkTicketPos newPosition{center.x + x, center.z + z};
      if (inSquare(oldCenter, newPosition, viewDistance_))
        continue;
      auto &count = loadingCoverage_[newPosition];
      if (count++ == 0) {
        operations.push_back(TicketOperation::add(
            newPosition, Ticket(ticket_types::PlayerLoading,
                                PlayerLoadingLevel)));
        ++loadingChanges;
      }
    }
  }

  std::size_t simulationChanges = 0;
  auto oldSimulation = simulationCoverage_.find(oldCenter);
  if (oldSimulation != simulationCoverage_.end() &&
      --oldSimulation->second == 0) {
    operations.push_back(TicketOperation::remove(
        oldCenter, Ticket(ticket_types::PlayerSimulation,
                          playerSimulationLevel())));
    simulationCoverage_.erase(oldSimulation);
    ++simulationChanges;
  }
  auto &newSimulation = simulationCoverage_[center];
  if (newSimulation++ == 0) {
    operations.push_back(TicketOperation::add(
        center, Ticket(ticket_types::PlayerSimulation,
                       playerSimulationLevel())));
    ++simulationChanges;
  }
  player->second.center = center;
  return applyPlayerSources(std::move(operations), true, loadingChanges,
                            simulationChanges);
}

TicketSourceUpdate TicketSourceManager::setPlayer(PlayerTicketId id,
                                                  bool present,
                                                  ChunkTicketPos center) {
  if (!present)
    return removePlayer(id);
  const auto existing = players_.find(id);
  if (existing == players_.end())
    return addPlayer(id, center);
  return movePlayer(id, center);
}

TicketSourceUpdate TicketSourceManager::removePlayer(PlayerTicketId id) {
  const auto player = players_.find(id);
  if (player == players_.end())
    return {};
  const ChunkTicketPos center = player->second.center;
  std::vector<TicketOperation> operations;
  std::vector<ChunkTicketPos> removed;
  removed.reserve(static_cast<std::size_t>(
      (viewDistance_ * 2 + 1) * (viewDistance_ * 2 + 1)));
  for (int x = -viewDistance_; x <= viewDistance_; ++x) {
    for (int z = -viewDistance_; z <= viewDistance_; ++z) {
      const ChunkTicketPos position{center.x + x, center.z + z};
      auto found = loadingCoverage_.find(position);
      if (found == loadingCoverage_.end())
        continue;
      if (--found->second == 0) {
        operations.push_back(TicketOperation::remove(
            position,
            Ticket(ticket_types::PlayerLoading, PlayerLoadingLevel)));
        removed.push_back(position);
      }
    }
  }
  for (const ChunkTicketPos &position : removed)
    loadingCoverage_.erase(position);

  bool removedSimulation = false;
  auto simulation = simulationCoverage_.find(center);
  if (simulation != simulationCoverage_.end() && --simulation->second == 0) {
    operations.push_back(TicketOperation::remove(
        center, Ticket(ticket_types::PlayerSimulation,
                       playerSimulationLevel())));
    simulationCoverage_.erase(simulation);
    removedSimulation = true;
  }
  players_.erase(player);
  return applyPlayerSources(std::move(operations), true, removed.size(),
                            removedSimulation ? 1 : 0);
}

std::map<ChunkTicketPos, std::size_t>
TicketSourceManager::rebuildLoadingCoverage(int radius) {
  std::map<ChunkTicketPos, std::size_t> coverage;
  for (const auto &[id, player] : players_) {
    (void)id;
    for (int x = -radius; x <= radius; ++x)
      for (int z = -radius; z <= radius; ++z)
        ++coverage[{player.center.x + x, player.center.z + z}];
  }
  return coverage;
}

TicketSourceUpdate
TicketSourceManager::setViewDistance(int viewDistance) {
  if (viewDistance < MinViewDistance || viewDistance > MaxViewDistance)
    throw std::invalid_argument("view distance must be in [2, 32]");
  if (viewDistance_ == viewDistance)
    return {};
  const auto &previous = loadingCoverage_;
  auto next = rebuildLoadingCoverage(viewDistance);
  std::vector<TicketOperation> operations;
  for (const auto &[position, count] : previous) {
    (void)count;
    if (!next.contains(position))
      operations.push_back(TicketOperation::remove(
          position,
          Ticket(ticket_types::PlayerLoading, PlayerLoadingLevel)));
  }
  for (const auto &[position, count] : next) {
    (void)count;
    if (!previous.contains(position))
      operations.push_back(TicketOperation::add(
          position,
          Ticket(ticket_types::PlayerLoading, PlayerLoadingLevel)));
  }
  const std::size_t loadingChanges = operations.size();
  loadingCoverage_ = std::move(next);
  viewDistance_ = viewDistance;
  return applyPlayerSources(std::move(operations), true, loadingChanges, 0);
}

TicketSourceUpdate
TicketSourceManager::setSimulationDistance(int simulationDistance) {
  if (simulationDistance < MinViewDistance ||
      simulationDistance > MaxViewDistance)
    throw std::invalid_argument("simulation distance must be in [2, 32]");
  if (simulationDistance_ == simulationDistance)
    return {};
  const int oldLevel = playerSimulationLevel();
  simulationDistance_ = simulationDistance;
  const int newLevel = playerSimulationLevel();
  std::vector<TicketOperation> operations;
  if (oldLevel != newLevel) {
    operations.reserve(simulationCoverage_.size() * 2);
    for (const auto &[center, count] : simulationCoverage_) {
      if (count == 0)
        continue;
      operations.push_back(TicketOperation::remove(
          center, Ticket(ticket_types::PlayerSimulation, oldLevel)));
      operations.push_back(TicketOperation::add(
          center, Ticket(ticket_types::PlayerSimulation, newLevel)));
    }
  }
  const std::size_t simulationChanges = operations.size();
  return applyPlayerSources(std::move(operations), true, 0,
                            simulationChanges);
}

TicketSystemUpdate
TicketSourceManager::setForced(const ChunkTicketPos &position, bool enabled) {
  if (enabled == forced_.contains(position))
    return {};
  std::vector<TicketOperation> operations;
  if (enabled) {
    forced_.insert(position);
    operations.push_back(TicketOperation::add(
        position, Ticket(ticket_types::Forced, PlayerLoadingLevel)));
  } else {
    forced_.erase(position);
    for (const Ticket &ticket :
         tickets_.storage().getTickets(position))
      if (ticket.type().name == ticket_types::Forced.name)
        operations.push_back(TicketOperation::remove(position, ticket));
  }
  return tickets_.applyBatch(operations);
}

TicketSystemUpdate TicketSourceManager::restorePersistentTickets(
    std::span<const std::pair<ChunkTicketPos, Ticket>> records) {
  TicketSystemUpdate update = tickets_.restorePersistentTickets(records);
  for (const auto &[position, ticket] : records)
    if (ticket.type().name == ticket_types::Forced.name)
      forced_.insert(position);
  return update;
}

TicketSystemUpdate
TicketSourceManager::requestPlayerSpawn(const ChunkTicketPos &center) {
  const std::vector<TicketOperation> operations{TicketOperation::add(
      center, Ticket(ticket_types::PlayerSpawn,
                     ticket_levels::FullChunkLevel - PlayerSpawnRadius))};
  return tickets_.applyBatch(operations);
}

TicketSystemUpdate
TicketSourceManager::requestSpawnSearch(const ChunkTicketPos &center) {
  const std::vector<TicketOperation> operations{TicketOperation::add(
      center, Ticket(ticket_types::SpawnSearch,
                     ticket_levels::FullChunkLevel - SpawnSearchRadius))};
  return tickets_.applyBatch(operations);
}

TicketSystemUpdate
TicketSourceManager::tick(const TicketHolderLookup &holders) {
  return tickets_.purgeTimedTickets(holders);
}

} // namespace redstone
