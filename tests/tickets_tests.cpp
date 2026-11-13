#include "redstone/tickets.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace redstone;

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

ChunkTicketPos pos(int x, int z) { return {x, z}; }

const Ticket *findTicket(const std::vector<Ticket> &tickets,
                         const TicketType &type, int level) {
  const auto found = std::find_if(
      tickets.begin(), tickets.end(), [&](const Ticket &ticket) {
        return ticket.type().name == type.name && ticket.level() == level;
      });
  return found == tickets.end() ? nullptr : &*found;
}

void constantsAndGenerationBoundaries() {
  static_assert(ticket_levels::FullChunkLevel == 33);
  static_assert(ticket_levels::BlockTickingLevel == 32);
  static_assert(ticket_levels::EntityTickingLevel == 31);
  static_assert(ticket_levels::RadiusAroundFullChunk == 11);
  static_assert(ticket_levels::MaxLevel == 44);
  static_assert(ticket_levels::NoTicketSource == 45);
  static_assert(ticket_levels::SimulationTrackerMax == 33);

  require(ticket_levels::fullStatus(30) == FullChunkStatus::EntityTicking &&
              ticket_levels::fullStatus(31) == FullChunkStatus::EntityTicking &&
              ticket_levels::fullStatus(32) == FullChunkStatus::BlockTicking &&
              ticket_levels::fullStatus(33) == FullChunkStatus::Full &&
              ticket_levels::fullStatus(44) == FullChunkStatus::Inaccessible &&
              ticket_levels::fullStatus(45) == FullChunkStatus::Inaccessible,
          "full/block/entity and inaccessible level boundaries");
  require(ticket_levels::isLoaded(44) && !ticket_levels::isLoaded(45),
          "loaded means scheduling scope through MAX_LEVEL only");
  require(ticket_levels::generationStatus(33) == GenerationStatus::Full &&
              ticket_levels::generationStatus(34) ==
                  GenerationStatus::InitializeLight &&
              ticket_levels::generationStatus(35) == GenerationStatus::Carvers &&
              ticket_levels::generationStatus(36) == GenerationStatus::Biomes &&
              ticket_levels::generationStatus(37) ==
                  GenerationStatus::StructureStarts &&
              ticket_levels::generationStatus(44) ==
                  GenerationStatus::StructureStarts &&
              !ticket_levels::generationStatus(45),
          "FULL accumulated dependency boundaries match 26.1");

  require(ticket_levels::generationLevel(GenerationStatus::Empty) == 44 &&
              ticket_levels::generationLevel(
                  GenerationStatus::StructureStarts) == 44 &&
              ticket_levels::generationLevel(
                  GenerationStatus::StructureReferences) == 36 &&
              ticket_levels::generationLevel(GenerationStatus::Biomes) == 36 &&
              ticket_levels::generationLevel(GenerationStatus::Noise) == 35 &&
              ticket_levels::generationLevel(GenerationStatus::Carvers) == 35 &&
              ticket_levels::generationLevel(GenerationStatus::Features) == 34 &&
              ticket_levels::generationLevel(
                  GenerationStatus::InitializeLight) == 34 &&
              ticket_levels::generationLevel(GenerationStatus::Light) == 33 &&
              ticket_levels::generationLevel(GenerationStatus::Full) == 33,
          "status-to-level boundaries match ChunkPyramid byStatus");

  const std::array<TicketType, 9> types{
      ticket_types::PlayerSpawn, ticket_types::SpawnSearch,
      ticket_types::Dragon,      ticket_types::PlayerLoading,
      ticket_types::PlayerSimulation, ticket_types::Forced,
      ticket_types::Portal,      ticket_types::EnderPearl,
      ticket_types::Unknown};
  require(types[0].timeout == 20 && types[0].flags == 2 &&
              types[1].timeout == 1 && types[1].flags == 2 &&
              types[2].timeout == 0 && types[2].flags == 6 &&
              types[3].flags == 2 && types[4].flags == 12 &&
              types[5].flags == 15 && types[6].timeout == 300 &&
              types[6].flags == 15 && types[7].timeout == 40 &&
              types[7].flags == 14 && types[8].timeout == 1 &&
              types[8].flags == 18,
          "26.1 registered ticket type timeout/flags");
  require(ticket_types::Forced.persist() &&
              ticket_types::Forced.doesLoad() &&
              ticket_types::Forced.doesSimulate() &&
              ticket_types::Forced.shouldKeepDimensionActive() &&
              !ticket_types::Unknown.doesSimulate() &&
              ticket_types::Unknown.canExpireIfUnloaded(),
          "ticket flag accessors follow exact bits");
}

void storageRenewalExactRemovalAndSeparateSources() {
  TicketStorage storage;
  const ChunkTicketPos center = pos(4, -7);
  TicketStorageChange added;
  require(storage.addTicket(center, Ticket(ticket_types::Portal, 7), &added) &&
              added.inserted && added.sourceChanges.size() == 2 &&
              added.sourceChanges[0].demand == TicketDemand::Simulation &&
              added.sourceChanges[1].demand == TicketDemand::Loading,
          "new ticket emits simulation then loading source updates");
  require(storage.getTicketLevelAt(center, TicketDemand::Loading) == 7 &&
              storage.getTicketLevelAt(center, TicketDemand::Simulation) == 7,
          "both ticket layers see the source");
  require(added.sourceChanges[0].sourceLevel == 7 &&
              added.sourceChanges[0].onlyDecreased &&
              added.sourceChanges[1].sourceLevel == 7 &&
              added.sourceChanges[1].onlyDecreased,
          "new lower source updates preserve the 26.1 decrease hint");

  TicketStorageChange renewed;
  require(!storage.addTicket(center, Ticket(ticket_types::Portal, 7), &renewed),
          "same type and level is deduplicated");
  require(renewed.renewed && renewed.changed && !renewed.inserted &&
              renewed.sourceChanges.empty(),
          "duplicate renewal does not emit a level update");
  const Ticket *portal = findTicket(storage.getTickets(center),
                                    ticket_types::Portal, 7);
  require(portal && portal->ticksLeft() == 300,
          "duplicate submission resets timeout to type timeout");

  Ticket loadingOnly(ticket_types::PlayerLoading, 20);
  Ticket simulationOnly(ticket_types::PlayerSimulation, 10);
  TicketStorageChange loadingOnlyChange;
  TicketStorageChange simulationOnlyChange;
  require(storage.addTicket(center, loadingOnly, &loadingOnlyChange) &&
              storage.addTicket(center, simulationOnly,
                                &simulationOnlyChange),
          "loading-only and simulation-only sources coexist");
  require(loadingOnlyChange.sourceChanges.empty() &&
              simulationOnlyChange.sourceChanges.empty(),
          "higher or non-participating additions emit no source update");
  require(storage.getTicketLevelAt(center, TicketDemand::Loading) == 7 &&
              storage.getTicketLevelAt(center, TicketDemand::Simulation) == 7 &&
              storage.getTickets(center).size() == 3,
          "lower participating sources remain independent per layer");

  TicketStorageChange removed;
  require(storage.removeTicket(center, Ticket(ticket_types::Portal, 7),
                               &removed) &&
              removed.removed && removed.sourceChanges.size() == 2,
          "removal is exact by type and level");
  require(storage.getTicketLevelAt(center, TicketDemand::Loading) == 20 &&
              storage.getTicketLevelAt(center, TicketDemand::Simulation) == 10,
          "removing a ticket raises each raw source to its remaining minimum");
  require(removed.sourceChanges[0].demand == TicketDemand::Simulation &&
              removed.sourceChanges[0].sourceLevel == 10 &&
              !removed.sourceChanges[0].onlyDecreased &&
              removed.sourceChanges[1].demand == TicketDemand::Loading &&
              removed.sourceChanges[1].sourceLevel == 20 &&
              !removed.sourceChanges[1].onlyDecreased,
          "removal emits simulation then loading with recomputed raw levels");
  require(!storage.removeTicket(center, Ticket(ticket_types::Portal, 7)),
          "removing the same identity twice reports no mutation");

  TicketStorage high;
  const ChunkTicketPos highPosition = pos(99, -99);
  TicketStorageChange highAdd;
  require(high.addTicket(highPosition,
                         Ticket(ticket_types::PlayerLoading, 50), &highAdd) &&
              high.getTicketLevelAt(highPosition, TicketDemand::Loading) == 50,
          "raw participating ticket level above 45 is not replaced by no-ticket");
  require(highAdd.sourceChanges.empty(),
          "a higher-than-default participating source cannot announce a decrease");
  TicketStorageChange highRemove;
  require(high.removeTicket(highPosition,
                            Ticket(ticket_types::PlayerLoading, 50),
                            &highRemove) &&
              highRemove.sourceChanges.size() == 1 &&
              highRemove.sourceChanges[0].sourceLevel == 45,
          "removing the high source reports the true no-ticket fallback");
}

void graphPropagationOverlapAndRemoval() {
  TicketLevelSystem system;
  const ChunkTicketPos a = pos(-2, -3);
  const ChunkTicketPos b = pos(-1, -2);
  require(system.addTicket(a, Ticket(ticket_types::Forced, 31)) &&
              system.addTicket(b, Ticket(ticket_types::Forced, 30)),
          "negative-coordinate overlapping sources are installed");

  const auto aView = system.query(a);
  const auto bView = system.query(b);
  const auto diagonal = system.query(pos(-1, -3));
  require(aView.loadingLevel == 31 && bView.loadingLevel == 30 &&
              diagonal.loadingLevel == 31,
          "overlapping loading sources take the minimum Chebyshev distance");
  require(aView.simulationLevel == 31 && bView.simulationLevel == 30 &&
              diagonal.simulationLevel == 31,
          "simulation graph uses the same eight-neighbor rule independently");

  const int oldB = bView.loadingLevel;
  const int oldDiagonal = diagonal.loadingLevel;
  TicketSystemUpdate removed;
  require(system.removeTicket(b, Ticket(ticket_types::Forced, 30), &removed),
          "one overlapping source is removed exactly");
  const int newB = system.query(b).loadingLevel;
  const int newDiagonal = system.query(pos(-1, -3)).loadingLevel;
  require(newB == 32 && newB > oldB && newDiagonal == 32 &&
              newDiagonal > oldDiagonal,
          "removing a source raises affected levels");
  require(std::any_of(removed.loadingChanges.begin(),
                      removed.loadingChanges.end(), [](const auto &change) {
                        return change.decreased() == false;
                      }),
          "source removal produces increase changes rather than deletion only");
  const auto counts = system.loadingGraph().recomputeCount();
  TicketSystemUpdate empty = system.purgeTimedTickets({});
  require(!empty.changed() &&
              system.loadingGraph().recomputeCount() == counts &&
              system.simulationGraph().recomputeCount() == counts,
          "a no-source-change tick does not rescan either level graph");

  require(system.removeTicket(a, Ticket(ticket_types::Forced, 31)),
          "last source is removed");
  require(system.query(a).loadingLevel == 45 &&
              system.query(a).simulationLevel == 33,
          "raw loading no-ticket is 45 while simulation clamps to 33");
}

void demandsRemainSeparate() {
  TicketLevelSystem system;
  const ChunkTicketPos center = pos(5, 5);
  require(system.addTicket(center,
                           Ticket(ticket_types::PlayerLoading, 20)) &&
              system.addTicket(center,
                               Ticket(ticket_types::PlayerSimulation, 31)),
          "loading-only and simulation-only demands coexist");
  const auto query = system.query(center);
  require(query.loadingLevel == 20 && query.simulationLevel == 31,
          "each graph retains its own no-ticket and source policy");
  require(query.schedulingScope &&
              query.holderFullStatus == FullChunkStatus::EntityTicking &&
              query.blockTickingDemand && query.entityTickingDemand &&
              query.dataResidency == ChunkDataResidency::NotTracked,
          "holder projection and simulation demand do not imply data residency");

  require(system.removeTicket(center,
                              Ticket(ticket_types::PlayerLoading, 20)),
          "loading-only source can be removed independently");
  const auto simulationOnly = system.query(center);
  require(simulationOnly.loadingLevel == 45 &&
              !simulationOnly.schedulingScope &&
              simulationOnly.simulationLevel == 31 &&
              simulationOnly.blockTickingDemand,
          "simulation demand survives after loading demand is gone");

  require(system.removeTicket(center,
                              Ticket(ticket_types::PlayerSimulation, 31)),
          "simulation-only source can be removed independently");
  const auto none = system.query(center);
  require(none.loadingLevel == 45 && none.simulationLevel == 33 &&
              !none.blockTickingDemand && !none.entityTickingDemand &&
              !none.generationStatus,
          "no-ticket values are 45 for raw loading and 33 for simulation");
}

void purgeCountdownAndHolderPredicate() {
  TicketLevelSystem system;
  const ChunkTicketPos paused = pos(0, 0);
  const ChunkTicketPos unloaded = pos(10, 0);
  const ChunkTicketPos ready = pos(20, 0);
  const ChunkTicketPos absent = pos(30, 0);
  const ChunkTicketPos noTimeout = pos(40, 0);
  require(system.addTicket(paused, Ticket(ticket_types::Portal, 7, 1)) &&
              system.addTicket(unloaded,
                               Ticket(ticket_types::Unknown, 7, 1)) &&
              system.addTicket(ready, Ticket(ticket_types::Portal, 8, 1)) &&
              system.addTicket(absent,
                               Ticket(ticket_types::Portal, 9, 1)) &&
              system.addTicket(noTimeout,
                               Ticket(ticket_types::Forced, 10, -3)),
          "purge fixture installs all timeout branches");

  const TicketHolderLookup lookup = [&](const ChunkTicketPos &position) {
    if (position == paused)
      return TicketHolderState{true, false};
    if (position == unloaded)
      return TicketHolderState{true, false};
    if (position == ready)
      return TicketHolderState{true, true};
    return TicketHolderState{};
  };

  TicketSystemUpdate first = system.purgeTimedTickets(lookup);
  require(system.storage().getTickets(paused).front().ticksLeft() == 1 &&
              system.storage().getTickets(unloaded).front().ticksLeft() == 0 &&
              system.storage().getTickets(ready).front().ticksLeft() == 0 &&
              system.storage().getTickets(absent).front().ticksLeft() == 0 &&
              system.storage().getTickets(noTimeout).front().ticksLeft() == -3,
          "holder exists/not-ready pauses only types that lack expire-if-unloaded");
  require(!first.storage.removed && first.storage.changed,
          "reaching zero is a countdown change, not removal");

  TicketSystemUpdate second = system.purgeTimedTickets(lookup);
  require(second.storage.removed &&
              system.storage().getTickets(unloaded).empty() &&
              system.storage().getTickets(ready).empty() &&
              system.storage().getTickets(absent).empty(),
          "tickets are removed only after ticksLeft becomes negative");
  require(system.storage().getTickets(paused).front().ticksLeft() == 1,
          "paused holder remains untouched across purge ticks");
  require(system.storage().getTickets(noTimeout).front().ticksLeft() == -3,
          "no-timeout tickets never decrement or expire");

  const auto oldRecompute = system.loadingGraph().recomputeCount();
  TicketSystemUpdate idle = system.purgeTimedTickets(lookup);
  require(!idle.changed() &&
              system.loadingGraph().recomputeCount() == oldRecompute,
          "an unchanged purge tick does not rescan the level graph");
}

void restoredForcedTicketCanBeRemovedExactly() {
  TicketSourceManager manager(3, 10);
  const ChunkTicketPos forcedPosition = pos(-13, -16);
  const ChunkTicketPos portalPosition = pos(20, 24);
  const std::vector<std::pair<ChunkTicketPos, Ticket>> restored{
      {forcedPosition, Ticket(ticket_types::Forced, 31)},
      {portalPosition, Ticket(ticket_types::Portal, 9, 257)},
      {portalPosition, Ticket(ticket_types::Portal, 9, 123)}};
  (void)manager.restorePersistentTickets(restored);
  require(manager.query(forcedPosition).loadingLevel == 31 &&
              manager.query(forcedPosition).simulationLevel == 31 &&
              manager.query(portalPosition).loadingLevel == 9 &&
              manager.query(portalPosition).simulationLevel == 9,
          "restored tickets install loading and simulation demand");
  const auto &restoredPortals =
      manager.tickets().storage().getTickets(portalPosition);
  require(restoredPortals.size() == 2 &&
              restoredPortals[0].level() == 9 &&
              restoredPortals[0].ticksLeft() == 257 &&
              restoredPortals[1].level() == 9 &&
              restoredPortals[1].ticksLeft() == 123,
          "persistent restore preserves duplicate identities and tick values");
  const TicketSystemUpdate removed = manager.setForced(forcedPosition, false);
  require(removed.storage.removed &&
              manager.tickets().storage().getTickets(forcedPosition).empty() &&
              manager.tickets().storage().getTickets(portalPosition).size() == 2 &&
              manager.query(forcedPosition).loadingLevel == 45 &&
              manager.query(forcedPosition).simulationLevel == 33,
          "setForced(false) removes only restored forced identities");
}

void deactivationAndActivation() {
  TicketLevelSystem system;
  const ChunkTicketPos center = pos(-4, 8);
  require(system.addTicket(center, Ticket(ticket_types::Forced, 5)) &&
              system.addTicket(center,
                               Ticket(ticket_types::Unknown, 20)) &&
              system.query(center).loadingLevel == 5,
          "active ticket fixture has mixed source types");
  TicketSystemUpdate deactivated = system.deactivateTicketsOnClosing();
  require(deactivated.storage.removed &&
              system.storage().getTickets(center).size() == 1 &&
              system.storage().getTickets(center).front().type().name ==
                  ticket_types::Unknown.name &&
              system.storage().getDeactivatedTickets(center).size() == 1 &&
              !system.storage().hasForcedTicket(center),
          "closing deactivates every active type except UNKNOWN");
  require(system.query(center).loadingLevel == 20,
          "deactivated tickets stop contributing graph sources");

  TicketSystemUpdate activated = system.activateAllDeactivatedTickets();
  require(activated.storage.inserted &&
              system.storage().getDeactivatedTickets(center).empty() &&
              system.storage().hasForcedTicket(center) &&
              system.query(center).loadingLevel == 5,
          "activateAllDeactivatedTickets restores source demand");
  auto persistent = system.storage().persistentTickets();
  require(persistent.size() == 1 &&
              persistent.front().second.type().name ==
                  ticket_types::Forced.name,
          "persistent packing keeps PERSIST tickets from active/deactivated maps");
}

void levelReplacementAndExactRemoval() {
  TicketLevelSystem system;
  const ChunkTicketPos center = pos(3, 3);
  require(system.addTicket(center, Ticket(ticket_types::Portal, 7)) &&
              system.addTicket(center, Ticket(ticket_types::Portal, 6)) &&
              system.query(center).loadingLevel == 6,
          "same type with a different level remains a distinct ticket");
  TicketSystemUpdate replaced =
      system.replaceTicketLevelOfType(ticket_types::Portal, 4);
  require(replaced.storage.changed &&
              system.storage().getTickets(center).size() == 1 &&
              system.query(center).loadingLevel == 4,
          "replace coalesces duplicate type/level results");
  require(system.removeTicket(center, Ticket(ticket_types::Portal, 4)),
          "replacement ticket removes exactly");
  require(system.storage().getTickets(center).empty(),
          "no portal ticket remains after exact removal");
}

void playerSourceOverlapAndLastLeave() {
  TicketSourceManager manager(3, 10);
  const auto first = manager.addPlayer(1, pos(0, 0));
  const auto second = manager.addPlayer(2, pos(4, 0));
  require(first.stateChanged && second.stateChanged &&
              first.loadingPresenceChanges == 49 &&
              first.simulationPresenceChanges == 1 &&
              second.loadingPresenceChanges == 28 &&
              second.simulationPresenceChanges == 1,
          "view radii install every newly covered chunk once");
  require(manager.loadingCoverage(pos(1, 0)) == 2 &&
              manager.loadingCoverage(pos(0, 0)) == 1 &&
              manager.loadingCoverage(pos(7, 0)) == 1 &&
              manager.simulationCoverage(pos(0, 0)) == 1 &&
              manager.simulationCoverage(pos(4, 0)) == 1,
          "overlapping view coverage and center simulation sources are counted");

  const auto loadRecomputes = manager.tickets().loadingGraph().recomputeCount();
  const auto simulationRecomputes =
      manager.tickets().simulationGraph().recomputeCount();
  const auto noOp = manager.addPlayer(1, pos(0, 0));
  require(!noOp.stateChanged && !noOp.tickets.changed() &&
              manager.tickets().loadingGraph().recomputeCount() ==
                  loadRecomputes &&
              manager.tickets().simulationGraph().recomputeCount() ==
                  simulationRecomputes,
          "same-player same-chunk add is a no-op");

  const auto removedFirst = manager.removePlayer(1);
  require(removedFirst.loadingPresenceChanges == 28 &&
              removedFirst.simulationPresenceChanges == 1 &&
              manager.loadingCoverage(pos(1, 0)) == 1 &&
              manager.loadingCoverage(pos(0, 0)) == 0 &&
              manager.loadingCoverage(pos(-3, 0)) == 0 &&
              manager.simulationCoverage(pos(0, 0)) == 0 &&
              manager.simulationCoverage(pos(4, 0)) == 1,
          "removing one player retains overlap owned by the other");
  require(manager.removePlayer(2).loadingPresenceChanges == 49,
          "the final player releases all remaining loading coverage");
  require(manager.loadingCoverage(pos(1, 0)) == 0 &&
              manager.simulationCoverage(pos(4, 0)) == 0 &&
              manager.query(pos(0, 0)).loadingLevel == 45,
          "no player coverage remains after the last leave");
}

void playerMovementAndDistanceChanges() {
  TicketSourceManager manager(3, 10);
  require(manager.addPlayer(7, pos(0, 0)).stateChanged,
          "movement fixture player joins");
  const auto loadingBefore = manager.tickets().loadingGraph().recomputeCount();
  const auto simulationBefore =
      manager.tickets().simulationGraph().recomputeCount();
  require(!manager.movePlayer(7, pos(0, 0)).stateChanged,
          "same-chunk move is ignored");

  const auto moved = manager.movePlayer(7, pos(1, 0));
  require(moved.loadingPresenceChanges == 14 &&
              moved.simulationPresenceChanges == 2 &&
              manager.loadingCoverage(pos(-3, 0)) == 0 &&
              manager.loadingCoverage(pos(4, 0)) == 1 &&
              manager.loadingCoverage(pos(0, 0)) == 1 &&
              manager.query(pos(1, 0)).simulationLevel == 21,
          "one-chunk movement emits only the changed coverage strips");
  require(manager.tickets().loadingGraph().recomputeCount() ==
              loadingBefore + 1 &&
              manager.tickets().simulationGraph().recomputeCount() ==
                  simulationBefore + 1,
          "one movement event submits one batch to each affected graph");

  require(!manager.setViewDistance(3).stateChanged,
          "unchanged view distance is a no-op");
  const auto beforeView = manager.tickets().loadingGraph().recomputeCount();
  const auto beforeViewSimulation =
      manager.tickets().simulationGraph().recomputeCount();
  const auto view = manager.setViewDistance(4);
  require(view.stateChanged && view.loadingPresenceChanges == 32 &&
              manager.viewDistance() == 4 &&
              manager.loadingCoverage(pos(-3, 0)) == 1 &&
              manager.loadingCoverage(pos(5, 0)) == 1 &&
              manager.tickets().loadingGraph().recomputeCount() ==
                  beforeView + 1 &&
              manager.tickets().simulationGraph().recomputeCount() ==
                  beforeViewSimulation,
          "view distance updates only loading coverage in one batch");

  const auto loadingLevel = manager.query(pos(1, 0)).loadingLevel;
  const auto beforeSimulation =
      manager.tickets().simulationGraph().recomputeCount();
  const auto beforeDistanceLoading =
      manager.tickets().loadingGraph().recomputeCount();
  require(view.loadingPresenceChanges == 32 &&
              view.tickets.storage.sourceChanges.size() == 32,
          "view-distance return count is captured before operation move");
  const auto simulation = manager.setSimulationDistance(12);
  require(simulation.simulationPresenceChanges == 2 &&
              simulation.tickets.storage.changed &&
              manager.query(pos(1, 0)).simulationLevel == 19 &&
              manager.query(pos(1, 0)).loadingLevel == loadingLevel &&
              manager.tickets().simulationGraph().recomputeCount() ==
                  beforeSimulation + 1 &&
              manager.tickets().loadingGraph().recomputeCount() ==
                  beforeDistanceLoading,
          "simulation distance changes simulation demand only");
  require(!manager.setSimulationDistance(12).stateChanged,
          "unchanged simulation distance is a no-op");

  bool rejected = false;
  try {
    (void)manager.setViewDistance(33);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "view distance above 32 is rejected");
}

void disconnectedPlayerIdIsNotReused() {
  TicketSourceManager manager(3, 10);
  require(manager.addPlayer(1, pos(0, 0)).stateChanged &&
              manager.addPlayer(2, pos(4, 0)).stateChanged,
          "two simultaneous player sources install");
  require(manager.removePlayer(1).stateChanged,
          "first player departure removes only its exact source key");
  require(manager.addPlayer(3, pos(4, 0)).stateChanged &&
              manager.loadingCoverage(pos(4, 0)) == 2 &&
              manager.simulationCoverage(pos(4, 0)) == 2,
          "reconnect uses a fresh key and does not overwrite the live player");
  require(manager.loadingCoverage(pos(4, 0)) == 2 &&
              manager.simulationCoverage(pos(4, 0)) == 2,
          "two co-located fresh player keys retain both ticket counts");
  require(manager.removePlayer(3).stateChanged &&
              manager.loadingCoverage(pos(4, 0)) == 1 &&
              manager.simulationCoverage(pos(4, 0)) == 1,
          "removing reconnect key leaves the other co-located player covered");
  require(manager.removePlayer(2).stateChanged &&
              manager.loadingCoverage(pos(4, 0)) == 0 &&
              manager.simulationCoverage(pos(4, 0)) == 0 &&
              manager.query(pos(4, 0)).loadingLevel == 45,
          "the last co-located player removes the final shared ticket");
}

void playerConnectionPolicy() {
  require(!ticketPlayerDemandActive(false, false, false, false),
          "handshake, status and login phases have no player demand");
  require(ticketPlayerDemandActive(true, false, false, false),
          "an active Play session has player demand");
  require(!ticketPlayerDemandActive(true, true, false, false),
          "dead connections drop player demand");
  require(!ticketPlayerDemandActive(true, false, true, false),
          "closing connections drop player demand");
  require(!ticketPlayerDemandActive(true, false, false, true),
          "closed sessions drop player demand");
}

void playerPresenceReconciliation() {
  TicketSourceManager manager(2, 10);
  const ChunkTicketPos first{0, 0};
  const ChunkTicketPos second{5, 0};
  const auto absent = manager.setPlayer(42, false, first);
  require(!absent.stateChanged &&
              manager.tickets().loadingGraph().recomputeCount() == 0 &&
              manager.tickets().simulationGraph().recomputeCount() == 0,
          "absent reconcile for an unknown player is a no-op");

  const auto added = manager.setPlayer(42, true, first);
  const auto same = manager.setPlayer(42, true, first);
  require(added.stateChanged && !same.stateChanged &&
              manager.loadingCoverage(first) == 1 &&
              manager.simulationCoverage(first) == 1,
          "setPlayer adds once and same position is idempotent");

  const auto moved = manager.setPlayer(42, true, second);
  require(moved.stateChanged && manager.loadingCoverage(first) == 0 &&
              manager.loadingCoverage(second) == 1 &&
              manager.simulationCoverage(first) == 0 &&
              manager.simulationCoverage(second) == 1,
          "setPlayer moves an existing demand source exactly");

  const auto removed = manager.setPlayer(42, false, second);
  require(removed.stateChanged && manager.loadingCoverage(second) == 0 &&
              manager.simulationCoverage(second) == 0 &&
              manager.query(second).loadingLevel == 45,
          "setPlayer false removes every ticket for the player");
  require(!manager.setPlayer(42, false, second).stateChanged,
          "repeated absent reconciliation is a no-op");
}

void forcedSpawnAndOfflineTickets() {
  TicketSourceManager manager(2, 10);
  const ChunkTicketPos center = pos(-9, 12);
  const auto recomputes = manager.tickets().loadingGraph().recomputeCount();
  const auto forced = manager.setForced(center, true);
  require(forced.storage.inserted &&
              manager.query(center).loadingLevel == 31 &&
              manager.query(center).simulationLevel == 31,
          "FORCED level31 creates loading and simulation demand");
  require(!manager.setForced(center, true).changed() &&
              manager.tickets().loadingGraph().recomputeCount() ==
                  recomputes + 1,
          "FORCED add is idempotent");
  require(manager.setForced(center, false).storage.removed,
          "FORCED can be removed exactly");

  const auto spawn = manager.requestPlayerSpawn(center);
  require(spawn.storage.inserted &&
              findTicket(manager.tickets().storage().getTickets(center),
                         ticket_types::PlayerSpawn, 30) != nullptr &&
              manager.query(center).loadingLevel == 30,
          "PLAYER_SPAWN radius3 requests level30 at the center");
  const auto search = manager.requestSpawnSearch(center);
  require(search.storage.inserted &&
              findTicket(manager.tickets().storage().getTickets(center),
                         ticket_types::SpawnSearch, 33) != nullptr,
          "SPAWN_SEARCH radius0 requests level33 without loading a chunk");

  (void)manager.tick({});
  const auto *spawnTicket = findTicket(
      manager.tickets().storage().getTickets(center),
      ticket_types::PlayerSpawn, 30);
  const auto *searchTicket = findTicket(
      manager.tickets().storage().getTickets(center),
      ticket_types::SpawnSearch, 33);
  require(spawnTicket && spawnTicket->ticksLeft() == 19 &&
              searchTicket && searchTicket->ticksLeft() == 0,
          "spawn timers decrement but survive at zero");
  (void)manager.tick({});
  require(findTicket(manager.tickets().storage().getTickets(center),
                     ticket_types::SpawnSearch, 33) == nullptr,
          "SPAWN_SEARCH is removed only after ticksLeft becomes negative");
  for (int tick = 0; tick < 19; ++tick)
    (void)manager.tick({});
  require(manager.tickets().storage().getTickets(center).empty(),
          "PLAYER_SPAWN expires after its unloaded timeout");

  TicketSourceManager offline(3, 10);
  const auto empty = offline.tick({});
  require(!empty.changed() &&
              offline.tickets().loadingGraph().recomputeCount() == 0 &&
              offline.tickets().simulationGraph().recomputeCount() == 0,
          "offline ticks do not create or refresh ticket demand");
}

int referenceLevel(const std::map<ChunkTicketPos, int> &sources,
                   const ChunkTicketPos &position, int limit) {
  int result = limit;
  for (const auto &[source, level] : sources) {
    const int distance =
        std::max(std::abs(source.x - position.x),
                 std::abs(source.z - position.z));
    result = std::min(result, level + distance);
  }
  return result;
}

void batchApiAndLocalRecomputation() {
  TicketLevelSystem system;
  const ChunkTicketPos far = pos(-1000, -1000);
  require(system.addTicket(far, Ticket(ticket_types::Forced, 0)),
          "far reference source is installed");
  const std::size_t loadingRecomputes = system.loadingGraph().recomputeCount();
  const std::size_t simulationRecomputes =
      system.simulationGraph().recomputeCount();

  std::vector<TicketOperation> view;
  view.reserve(65 * 65);
  for (int x = -32; x <= 32; ++x)
    for (int z = -32; z <= 32; ++z)
      view.push_back(TicketOperation::add(
          pos(1000 + x, 1000 + z),
          Ticket(ticket_types::PlayerLoading, 31)));
  TicketSystemUpdate added = system.applyBatch(view);
  require(added.storage.inserted && added.storage.sourceChanges.size() == 4225,
          "view-distance batch coalesces thousands of source changes");
  require(system.loadingGraph().recomputeCount() == loadingRecomputes + 1,
          "one batch submits one loading graph recomputation");
  require(system.loadingGraph().lastRecomputedCells() == 91 * 91,
          "view-distance batch visits only its old/new influence union");
  require(added.loadingChanges.size() == 91 * 91,
          "first view-distance source set installs the whole reachable band");
  require(system.simulationGraph().recomputeCount() == simulationRecomputes &&
              system.simulationGraph().lastRecomputedCells() == 0,
          "loading-only batch does not touch the simulation graph");
  require(system.query(far).loadingLevel == 0 &&
              system.query(pos(1000, 1000)).loadingLevel == 31 &&
              system.query(pos(1045, 1045)).loadingLevel == 44,
          "far and view-distance regions retain independent local levels");
  std::cout << "ticket view32 sources=4225 loading_recomputes="
            << system.loadingGraph().recomputeCount() - loadingRecomputes
            << " loading_cells=" << system.loadingGraph().lastRecomputedCells()
            << " simulation_recomputes="
            << system.simulationGraph().recomputeCount() - simulationRecomputes
            << "\n";

  const std::size_t beforeMove = system.loadingGraph().recomputeCount();
  std::vector<TicketOperation> move{
      TicketOperation::remove(pos(968, 1000),
                              Ticket(ticket_types::PlayerLoading, 31)),
      TicketOperation::add(pos(967, 1000),
                           Ticket(ticket_types::PlayerLoading, 31))};
  TicketSystemUpdate moved = system.applyBatch(move);
  require(moved.storage.sourceChanges.size() == 2 &&
              system.loadingGraph().recomputeCount() == beforeMove + 1,
          "one-cell source move submits one batch to the loading graph");
  require(system.loadingGraph().lastRecomputedCells() == 28 * 27,
          "one-cell move recomputes only the two overlapping influence strips");
  require(system.query(far).loadingLevel == 0,
          "far unchanged region remains outside the moved strip");
  std::cout << "ticket one_move source_changes="
            << moved.storage.sourceChanges.size()
            << " loading_recomputes="
            << system.loadingGraph().recomputeCount() - beforeMove
            << " loading_cells=" << system.loadingGraph().lastRecomputedCells()
            << "\n";

  const std::size_t loadingBefore = system.loadingGraph().recomputeCount();
  const std::size_t simulationBefore =
      system.simulationGraph().recomputeCount();
  std::vector<TicketOperation> netZero{
      TicketOperation::add(pos(500, 500),
                           Ticket(ticket_types::PlayerLoading, 50)),
      TicketOperation::remove(pos(500, 500),
                              Ticket(ticket_types::PlayerLoading, 50))};
  TicketSystemUpdate noChange = system.applyBatch(netZero);
  require(noChange.storage.changed && noChange.storage.sourceChanges.empty(),
          "batch reports only final per-source differences");
  require(system.loadingGraph().recomputeCount() == loadingBefore &&
              system.simulationGraph().recomputeCount() == simulationBefore,
          "net-zero batch does not submit either level graph");
}

void sourceManagerScenarioStatistics() {
  TicketSourceManager manager(32, 10);
  const auto replaced = manager.addPlayer(100, pos(0, 0));
  const auto loadingRecomputes =
      manager.tickets().loadingGraph().recomputeCount();
  const auto simulationRecomputes =
      manager.tickets().simulationGraph().recomputeCount();
  const auto moved = manager.movePlayer(100, pos(1, 0));
  std::cout << "ticket view32_player loading_presence="
            << replaced.loadingPresenceChanges
            << " simulation_presence=" << replaced.simulationPresenceChanges
            << " move_loading_presence=" << moved.loadingPresenceChanges
            << " move_simulation_presence="
            << moved.simulationPresenceChanges
            << " move_loading_recomputes="
            << manager.tickets().loadingGraph().recomputeCount() -
                   loadingRecomputes
            << " move_loading_cells="
            << manager.tickets().loadingGraph().lastRecomputedCells()
            << " move_simulation_recomputes="
            << manager.tickets().simulationGraph().recomputeCount() -
                   simulationRecomputes
            << " move_simulation_cells="
            << manager.tickets().simulationGraph().lastRecomputedCells()
            << "\n";
  require(replaced.loadingPresenceChanges == 65 * 65 &&
              moved.loadingPresenceChanges == 130 &&
              manager.tickets().loadingGraph().recomputeCount() ==
                  loadingRecomputes + 1 &&
              manager.tickets().simulationGraph().recomputeCount() ==
                  simulationRecomputes + 1,
          "view32 player add and move emit one graph recomputation each");
}

void randomLocalGraphMatchesFullReference() {
  std::mt19937 random(0x5eed1234u);
  std::uniform_int_distribution<int> coordinate(-6, 6);
  std::uniform_int_distribution<int> choose(0, 99);

  for (TicketDemand demand :
       {TicketDemand::Loading, TicketDemand::Simulation}) {
    ChunkLevelGraph graph(demand);
    std::map<ChunkTicketPos, int> sources;
    const int limit = graph.storedBelow();
    for (int round = 0; round < 240; ++round) {
      const ChunkTicketPos position{coordinate(random), coordinate(random)};
      int source = limit;
      if (sources.contains(position) && choose(random) < 35) {
        sources.erase(position);
      } else {
        source = static_cast<int>(random() % static_cast<unsigned>(limit + 8));
        sources[position] = source;
      }
      const TicketSourceChange change{demand, position, source, false};
      (void)graph.applySourceChanges(std::span(&change, 1));
      for (int x = -10; x <= 10; ++x)
        for (int z = -10; z <= 10; ++z) {
          const ChunkTicketPos query{x, z};
          require(graph.levelAt(query) ==
                      referenceLevel(sources, query, limit),
                  "local graph batch matches the full Chebyshev reference");
        }
    }
  }
}

} // namespace

int main() {
  try {
    constantsAndGenerationBoundaries();
    storageRenewalExactRemovalAndSeparateSources();
    graphPropagationOverlapAndRemoval();
    demandsRemainSeparate();
    purgeCountdownAndHolderPredicate();
    deactivationAndActivation();
    levelReplacementAndExactRemoval();
    batchApiAndLocalRecomputation();
    randomLocalGraphMatchesFullReference();
    sourceManagerScenarioStatistics();
    playerSourceOverlapAndLastLeave();
    playerMovementAndDistanceChanges();
    forcedSpawnAndOfflineTickets();
    playerPresenceReconciliation();
    playerConnectionPolicy();
    disconnectedPlayerIdIsNotReused();
    restoredForcedTicketCanBeRemovedExactly();
    std::cout << "ticket tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "ticket test failure: " << error.what() << '\n';
    return 1;
  }
}
