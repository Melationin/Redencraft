#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace redstone {

struct ChunkTicketPos {
  int x = 0;
  int z = 0;

  friend bool operator==(const ChunkTicketPos &, const ChunkTicketPos &) = default;
  friend auto operator<=>(const ChunkTicketPos &, const ChunkTicketPos &) = default;
  std::int64_t pack() const {
    return static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
        static_cast<std::uint32_t>(z));
  }
};

enum class TicketFlags : std::uint32_t {
  None = 0,
  Persist = 1,
  Loading = 2,
  Simulation = 4,
  KeepDimensionActive = 8,
  CanExpireIfUnloaded = 16,
};

constexpr std::uint32_t ticketFlag(TicketFlags flag) {
  return static_cast<std::uint32_t>(flag);
}
constexpr std::uint32_t operator|(TicketFlags left, TicketFlags right) {
  return ticketFlag(left) | ticketFlag(right);
}
constexpr std::uint32_t operator|(std::uint32_t left, TicketFlags right) {
  return left | ticketFlag(right);
}

struct TicketType {
  std::string_view name;
  std::int64_t timeout = 0;
  std::uint32_t flags = 0;

  bool hasTimeout() const { return timeout != 0; }
  bool persist() const { return (flags & ticketFlag(TicketFlags::Persist)) != 0; }
  bool doesLoad() const { return (flags & ticketFlag(TicketFlags::Loading)) != 0; }
  bool doesSimulate() const {
    return (flags & ticketFlag(TicketFlags::Simulation)) != 0;
  }
  bool shouldKeepDimensionActive() const {
    return (flags & ticketFlag(TicketFlags::KeepDimensionActive)) != 0;
  }
  bool canExpireIfUnloaded() const {
    return (flags & ticketFlag(TicketFlags::CanExpireIfUnloaded)) != 0;
  }
  friend bool operator==(const TicketType &, const TicketType &) = default;
};

namespace ticket_types {
inline constexpr TicketType PlayerSpawn{"minecraft:player_spawn", 20, 2};
inline constexpr TicketType SpawnSearch{"minecraft:spawn_search", 1, 2};
inline constexpr TicketType Dragon{"minecraft:dragon", 0, 6};
inline constexpr TicketType PlayerLoading{"minecraft:player_loading", 0, 2};
inline constexpr TicketType PlayerSimulation{"minecraft:player_simulation", 0, 12};
inline constexpr TicketType Forced{"minecraft:forced", 0, 15};
inline constexpr TicketType Portal{"minecraft:portal", 300, 15};
inline constexpr TicketType EnderPearl{"minecraft:ender_pearl", 40, 14};
inline constexpr TicketType Unknown{"minecraft:unknown", 1, 18};
} // namespace ticket_types

class Ticket {
public:
  Ticket(const TicketType &type, int level)
      : type_(type), level_(level), ticksLeft_(type.timeout) {}
  Ticket(const TicketType &type, int level, std::int64_t ticksLeft)
      : type_(type), level_(level), ticksLeft_(ticksLeft) {}

  const TicketType &type() const { return type_; }
  int level() const { return level_; }
  std::int64_t ticksLeft() const { return ticksLeft_; }
  void resetTicksLeft() { ticksLeft_ = type_.timeout; }
  void decreaseTicksLeft() {
    if (type_.hasTimeout())
      --ticksLeft_;
  }
  bool isTimedOut() const {
    return type_.hasTimeout() && ticksLeft_ < 0;
  }

private:
  TicketType type_;
  int level_;
  std::int64_t ticksLeft_;
};

enum class TicketDemand {
  Loading,
  Simulation,
};

// TicketStorage emits the same two listener updates as 26.1. sourceLevel is the
// raw TicketStorage value before ChunkTracker clamping (45 when absent for both
// loading and simulation). A repeated identical ticket renews without an update.
struct TicketSourceChange {
  TicketDemand demand = TicketDemand::Loading;
  ChunkTicketPos position;
  int sourceLevel = 45;
  bool onlyDecreased = false;
};

enum class TicketOperationKind {
  Add,
  Remove,
};

struct TicketOperation {
  TicketOperationKind kind = TicketOperationKind::Add;
  ChunkTicketPos position;
  Ticket ticket;

  static TicketOperation add(ChunkTicketPos position, Ticket ticket) {
    return {TicketOperationKind::Add, position, std::move(ticket)};
  }
  static TicketOperation remove(ChunkTicketPos position, Ticket ticket) {
    return {TicketOperationKind::Remove, position, std::move(ticket)};
  }
};

struct TicketStorageChange {
  bool inserted = false;
  bool renewed = false;
  bool removed = false;
  bool changed = false;
  bool dirty = false;
  std::vector<TicketSourceChange> sourceChanges;
};

struct TicketHolderState {
  bool present = false;
  bool readyForSaving = false;
};
using TicketHolderLookup =
    std::function<TicketHolderState(const ChunkTicketPos &position)>;

class TicketStorage {
public:
  bool addTicket(const ChunkTicketPos &position, Ticket ticket,
                 TicketStorageChange *out = nullptr);
  bool removeTicket(const ChunkTicketPos &position, const Ticket &ticket,
                    TicketStorageChange *out = nullptr);
  // Applies in order, then reports only final per-position source differences.
  // A batch whose source levels return to their original values emits no graph
  // update even if ticket objects were renewed along the way.
  TicketStorageChange applyBatch(std::span<const TicketOperation> operations);

  const std::vector<Ticket> &getTickets(const ChunkTicketPos &position) const;
  const std::vector<Ticket> &
  getDeactivatedTickets(const ChunkTicketPos &position) const;
  int getTicketLevelAt(const ChunkTicketPos &position,
                       TicketDemand demand) const;

  bool hasTickets() const { return !tickets_.empty(); }
  bool shouldKeepDimensionActive() const;
  bool hasForcedTicket(const ChunkTicketPos &position) const;

  // One call performs one 26.1 purge step. A holder that exists but is not
  // ready for saving pauses countdown for types without expire-if-unloaded.
  TicketStorageChange purgeTimedTickets(const TicketHolderLookup &holders);

  TicketStorageChange deactivateTicketsOnClosing();
  TicketStorageChange activateAllDeactivatedTickets();
  TicketStorageChange replaceTicketLevelOfType(const TicketType &type,
                                                int newLevel);

  std::vector<std::pair<ChunkTicketPos, Ticket>>
  persistentTickets() const;
  // Startup-only raw restore. Every input entry is appended without identity
  // deduplication or Ticket timeout renewal, and one final source diff is
  // produced for both trackers.
  TicketStorageChange restorePersistentTickets(
      std::span<const std::pair<ChunkTicketPos, Ticket>> tickets);

private:
  using TicketMap = std::map<ChunkTicketPos, std::vector<Ticket>>;

  static int getTicketLevelAt(const std::vector<Ticket> &tickets,
                              TicketDemand demand);
  static bool sameIdentity(const Ticket &left, const Ticket &right);

  TicketStorageChange removeIf(
      const std::function<bool(Ticket &, const ChunkTicketPos &)> &predicate,
      TicketMap *removedOutput = nullptr);
  void updateForcedChunks();

  TicketMap tickets_;
  TicketMap deactivatedTickets_;
  std::set<ChunkTicketPos> forcedChunks_;
};

enum class FullChunkStatus {
  Inaccessible,
  Full,
  BlockTicking,
  EntityTicking,
};

enum class GenerationStatus {
  Empty,
  StructureStarts,
  StructureReferences,
  Biomes,
  Noise,
  Surface,
  Carvers,
  Features,
  InitializeLight,
  Light,
  Spawn,
  Full,
};

namespace ticket_levels {
inline constexpr int FullChunkLevel = 33;
inline constexpr int BlockTickingLevel = 32;
inline constexpr int EntityTickingLevel = 31;
inline constexpr int RadiusAroundFullChunk = 11;
inline constexpr int MaxLevel = FullChunkLevel + RadiusAroundFullChunk;
inline constexpr int NoTicketSource = MaxLevel + 1;
inline constexpr int SimulationTrackerMax = 33;

constexpr bool isLoaded(int level) { return level <= MaxLevel; }
constexpr bool isBlockTicking(int level) {
  return level <= BlockTickingLevel;
}
constexpr bool isEntityTicking(int level) {
  return level <= EntityTickingLevel;
}
constexpr FullChunkStatus fullStatus(int level) {
  if (level <= EntityTickingLevel)
    return FullChunkStatus::EntityTicking;
  if (level <= BlockTickingLevel)
    return FullChunkStatus::BlockTicking;
  if (level <= FullChunkLevel)
    return FullChunkStatus::Full;
  return FullChunkStatus::Inaccessible;
}
std::optional<GenerationStatus> generationStatus(int level);
int generationLevel(GenerationStatus status);
const char *generationStatusName(GenerationStatus status);
const char *fullStatusName(FullChunkStatus status);
} // namespace ticket_levels

struct ChunkLevelChange {
  ChunkTicketPos position;
  int oldLevel = 0;
  int newLevel = 0;

  bool decreased() const { return newLevel < oldLevel; }
};

// Main-thread-only graph. Sources are asserted levels; every edge is one of the
// eight surrounding chunks and costs one level. A source change schedules one
// recomputation; calling tick with no source change does not scan the graph.
class ChunkLevelGraph {
public:
  explicit ChunkLevelGraph(TicketDemand demand);

  TicketDemand demand() const { return demand_; }
  int noSourceLevel() const;
  int storedBelow() const;
  int levelAt(const ChunkTicketPos &position) const;
  std::optional<int> sourceLevel(const ChunkTicketPos &position) const;

  std::vector<ChunkLevelChange>
  applySourceChanges(std::span<const TicketSourceChange> changes);
  std::uint64_t recomputeCount() const { return recomputeCount_; }
  std::size_t lastRecomputedCells() const { return lastRecomputedCells_; }
  std::size_t lastChangedCells() const { return lastChangedCells_; }
  std::size_t storedNodeCount() const { return levels_.size(); }

private:
  TicketDemand demand_;
  std::map<ChunkTicketPos, int> sources_;
  std::map<ChunkTicketPos, int> levels_;
  std::uint64_t recomputeCount_ = 0;
  std::size_t lastRecomputedCells_ = 0;
  std::size_t lastChangedCells_ = 0;
};

enum class ChunkDataResidency {
  // T-1 models demand only. Holder scope and FULL/ticking projections do not
  // imply that chunk data is resident or ready.
  NotTracked,
};

struct ChunkDemandSnapshot {
  int loadingLevel = ticket_levels::NoTicketSource;
  int simulationLevel = ticket_levels::SimulationTrackerMax;
  bool schedulingScope = false;
  FullChunkStatus holderFullStatus = FullChunkStatus::Inaccessible;
  std::optional<GenerationStatus> generationStatus;
  bool blockTickingDemand = false;
  bool entityTickingDemand = false;
  ChunkDataResidency dataResidency = ChunkDataResidency::NotTracked;
};

struct TicketSystemUpdate {
  TicketStorageChange storage;
  std::vector<ChunkLevelChange> loadingChanges;
  std::vector<ChunkLevelChange> simulationChanges;

  bool changed() const {
    return storage.changed || !loadingChanges.empty() ||
           !simulationChanges.empty();
  }
};

// Main-thread composition. It deliberately has no World, ChunkHolder, disk, or
// ticking hook: holderFullStatus is a level projection, not data residency.
class TicketLevelSystem {
public:
  bool addTicket(const ChunkTicketPos &position, Ticket ticket,
                 TicketSystemUpdate *out = nullptr);
  bool removeTicket(const ChunkTicketPos &position, const Ticket &ticket,
                    TicketSystemUpdate *out = nullptr);
  TicketSystemUpdate applyBatch(std::span<const TicketOperation> operations);
  TicketSystemUpdate restorePersistentTickets(
      std::span<const std::pair<ChunkTicketPos, Ticket>> tickets);
  TicketSystemUpdate purgeTimedTickets(const TicketHolderLookup &holders);
  TicketSystemUpdate deactivateTicketsOnClosing();
  TicketSystemUpdate activateAllDeactivatedTickets();
  TicketSystemUpdate replaceTicketLevelOfType(const TicketType &type,
                                               int newLevel);

  ChunkDemandSnapshot query(const ChunkTicketPos &position) const;
  const TicketStorage &storage() const { return storage_; }
  const ChunkLevelGraph &loadingGraph() const { return loading_; }
  const ChunkLevelGraph &simulationGraph() const { return simulation_; }

private:
  TicketSystemUpdate applyStorageChange(TicketStorageChange change);

  TicketStorage storage_;
  ChunkLevelGraph loading_{TicketDemand::Loading};
  ChunkLevelGraph simulation_{TicketDemand::Simulation};
};

constexpr bool ticketPlayerDemandActive(bool sessionPlaying, bool dead,
                                        bool closing, bool closed) {
  return sessionPlaying && !dead && !closing && !closed;
}

using PlayerTicketId = std::uint64_t;

struct PlayerTicketSource {
  ChunkTicketPos center;
};

struct TicketSourceUpdate {
  TicketSystemUpdate tickets;
  bool stateChanged = false;
  std::size_t loadingPresenceChanges = 0;
  std::size_t simulationPresenceChanges = 0;
};

// Main-thread-only demand source bookkeeping. It does not load chunks, search
// for spawns, create futures, or drive a tick gate.
class TicketSourceManager {
public:
  static constexpr int MinViewDistance = 2;
  static constexpr int MaxViewDistance = 32;
  static constexpr int PlayerLoadingLevel = 31;
  static constexpr int PlayerSpawnRadius = 3;
  static constexpr int SpawnSearchRadius = 0;

  TicketSourceManager(int viewDistance = 3, int simulationDistance = 10);

  int viewDistance() const { return viewDistance_; }
  int simulationDistance() const { return simulationDistance_; }
  int playerSimulationLevel() const;
  const std::map<PlayerTicketId, PlayerTicketSource> &players() const {
    return players_;
  }
  std::size_t loadingCoverage(const ChunkTicketPos &position) const;
  std::size_t simulationCoverage(const ChunkTicketPos &position) const;

  TicketSourceUpdate addPlayer(PlayerTicketId id, ChunkTicketPos center);
  TicketSourceUpdate movePlayer(PlayerTicketId id, ChunkTicketPos center);
  // Exact presence reconciliation: add when present, move when existing,
  // remove when absent. Safe on every server-loop iteration.
  TicketSourceUpdate setPlayer(PlayerTicketId id, bool present,
                               ChunkTicketPos center);
  TicketSourceUpdate removePlayer(PlayerTicketId id);
  TicketSourceUpdate setViewDistance(int viewDistance);
  TicketSourceUpdate setSimulationDistance(int simulationDistance);

  TicketSystemUpdate setForced(const ChunkTicketPos &position, bool forced);
  TicketSystemUpdate restorePersistentTickets(
      std::span<const std::pair<ChunkTicketPos, Ticket>> tickets);
  TicketSystemUpdate requestPlayerSpawn(const ChunkTicketPos &center);
  TicketSystemUpdate requestSpawnSearch(const ChunkTicketPos &center);
  TicketSystemUpdate tick(const TicketHolderLookup &holders = {});

  TicketLevelSystem &tickets() { return tickets_; }
  const TicketLevelSystem &tickets() const { return tickets_; }
  ChunkDemandSnapshot query(const ChunkTicketPos &position) const {
    return tickets_.query(position);
  }

private:
  static bool inSquare(const ChunkTicketPos &center,
                       const ChunkTicketPos &position, int radius);
  TicketSourceUpdate applyPlayerSources(std::vector<TicketOperation> operations,
                                        bool stateChanged,
                                        std::size_t loadingChanges,
                                        std::size_t simulationChanges);
  std::map<ChunkTicketPos, std::size_t> rebuildLoadingCoverage(int radius);

  int viewDistance_;
  int simulationDistance_;
  TicketLevelSystem tickets_;
  std::map<PlayerTicketId, PlayerTicketSource> players_;
  std::map<ChunkTicketPos, std::size_t> loadingCoverage_;
  std::map<ChunkTicketPos, std::size_t> simulationCoverage_;
  std::set<ChunkTicketPos> forced_;
};

} // namespace redstone
