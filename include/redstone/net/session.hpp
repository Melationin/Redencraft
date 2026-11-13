#pragma once
#include "redstone/command.hpp"
#include "redstone/entity.hpp"
#include "redstone/net/wire.hpp"
#include <array>
#include <atomic>
#include <map>
#include <set>

namespace redstone {
class WorldClocks;
enum class BuiltinWorldClock : std::uint8_t;
}
namespace redstone::net {
// Immutable data exported by the version-pinned vanilla codecs. No network
// registry IDs are inferred from this simulator's smaller registration list.
struct ProtocolData {
  explicit ProtocolData(const std::filesystem::path &directory);
  Registry registry;
  std::vector<Bytes> configuration;
  std::map<std::string, int> packetIds, blockIds;
  std::map<std::string, int> explosionEffectIds;
  int dimensionId = 0;
  // Official 26.1 world_clock registry IDs from registry_ids.tsv. Order is
  // overworld then the_end; both registries were present in the configuration
  // export this ProtocolData actually sends.
  std::array<int, 2> worldClockIds{{-1, -1}};
  int packet(std::string_view phase, std::string_view flow,
             std::string_view name) const;
};
using Uuid = std::array<std::uint8_t, 16>;
Uuid offlineUuid(std::string_view name);
enum class Phase { Handshake, Status, Login, Configuration, Play, Closed };
// The protocol view remains capped at 10; ticket-source view distance is a
// separate server setting validated in TicketSourceManager (2..32).
inline constexpr int maxViewDistance = 10;
struct ServerOptions {
  Vec3 spawn{8.5, 66, 8.5};
  std::vector<std::pair<int, int>> chunks;
  int playerId = 1000000;
  // Client visibility and server ticket-source coverage are independent.
  int viewDistance = 3;
  int ticketViewDistance = 3;
  // Commands.hasPermission source level for this player. The current native
  // server has no op-list integration, so players are non-operators by default;
  // the local console constructs an owner-level source separately.
  int commandPermissionLevel = 0;
};
// One connection's deterministic protocol state machine, independent of socket
// IO. Every call and World::tick runs on the server thread.
class Session {
public:
  Session(World &, const ProtocolData &, ServerOptions);
  void receive(std::span<const std::uint8_t> bytes);
  void tick();
  // Managed ticketed-world only. The pointer remains server-owned for the
  // Session lifetime. Offline sessions leave it unset and keep the old empty
  // set_time map projection.
  void setWorldClockSource(const WorldClocks &, bool advanceTime);
  // ServerClockManager.modifyClock: one changed single-clock record.
  void sendWorldClockUpdate(BuiltinWorldClock);
  // Called at the chunk/entity broadcast phase of World::tick.
  void broadcastEntities();
  void entityAdded(const Entity &);
  void entityRemoved(int entityId);
  // Call after the measured tick; wall-clock duration excludes this packet.
  void sendTickTime(double milliseconds, std::int64_t tick, std::size_t samples = 1);
  void blockUpdate(Pos, const BlockState &);
  void flushBlockUpdates();
  void blockEvent(const BlockEvent &);
  void explosion(Vec3 center, float radius, int blockCount);
  void fallingBlockLanded(int entityId, Pos, const BlockState &);
  // Main-thread delivery from the light worker. Initial chunk sends wait for a
  // protocol-ready snapshot; subsequent updates are encoded from immutable
  // snapshots after this callback returns.
  void lightPublished(std::shared_ptr<const LightChunkSnapshot> snapshot);
  // Explicit World lifecycle notification. This is distinct from a tracking-
  // view edge and is safe to invoke from the server's main-thread callbacks.
  void lightChunkLifecycle(int chunkX, int chunkZ, bool loaded);
  // Opt-in managed-world availability hook. Legacy sessions never install it.
  // Resident chunks become network-visible; actual managed erasure invalidates
  // pending sends and the chunk generation token.
  void setManagedResidencyProvider(
      std::function<bool(int, int)>,
      std::function<std::vector<std::pair<int, int>>()>);
  void managedChunkResidency(int chunkX, int chunkZ, bool resident);
  bool hasManagedChunk(int chunkX, int chunkZ) const;
  // Command tree and tick state. Both owners live in the server loop and outlive
  // every session.
  void attachCommands(const command::Dispatcher *, const command::Context *);
  void sendCommands();
  // ClientboundTickingStatePacket / ClientboundTickingStepPacket.
  void sendTickingState(const TickRateManager &);
  void sendTickingStep(const TickRateManager &);
  // One command feedback line (ClientboundSystemChatPacket).
  void sendCommandMessage(const command::Message &);
  std::vector<Bytes> takeOutput();
  // Drains unframed work items for the network writer thread.
  std::vector<OutPacket> takeOutputItems();
  Phase phase() const { return phase_; }
  bool closed() const { return phase_ == Phase::Closed; }
  const std::string &error() const { return error_; }
  Vec3 playerPosition() const { return position_; }
  int playerId() const { return options_.playerId; }
  const std::array<ItemStack, 41> &inventory() const { return inventory_; }
  std::function<void(std::string_view)> diagnostic;

private:
  // ServerEntity state: VecDeltaCodec base, packed rotations and the update /
  // teleport timers. One entry per entity this session currently tracks.
  struct TrackedEntity {
    Vec3 base;
    Vec3 lastMovement;
    std::uint64_t lastMotionRevision = 0;
    std::uint8_t yRot = 0, xRot = 0;
    int tntFuse = PrimedTnt::defaultFuse;
    BlockState::Id tntBlockState = 0;
    int tickCount = 0;
    int teleportDelay = 0;
    std::uint64_t lastSeenGeneration = 0;
    bool wasOnGround = false;
  };
  void handle(Reader &);
  void play(int, Reader &);
  void join();
  void finishJoin();
  void writeWorldClock(Writer &, BuiltinWorldClock) const;
  void sendSetTime();
  void send(std::string_view name, const Writer &payload = {});
  void send(std::string_view name, Writer &&payload);
  void packet(int id, const Writer &payload = {});
  void packet(int id, Writer &&payload);
  void enqueue(OutPacket packet);
  void queue(Bytes bytes);
  void queueDeferred(std::function<Bytes()> producer,
                     std::size_t retainedBytes,
                     std::function<bool()> stillValid = {});
  void disconnect(const std::string &);
  void acknowledge(int sequence);
  void sendBlockUpdate(Pos);
  void setSlot(int slot, ItemStack stack);
  void stack(Writer &, const ItemStack &) const;
  void syncEntities();
  void sendEntitySpawn(const Entity &, TrackedEntity &);
  void sendEntityChanges(const Entity &, TrackedEntity &);
  void sendTntData(const PrimedTnt &, TrackedEntity &);
  // Refreshes the available set only. Play view packets are emitted by join()
  // after its mandatory login packet, never from configuration completion.
  void syncManagedResidency();
  void refreshChunkView();
  void sendChunkBatch();
  bool chunkVisible(std::pair<int, int>) const;
  void queuePendingChunk(std::pair<int, int>);
  void expireChunk(std::pair<int, int>);
  bool hasChunk(Pos) const;
  void positionPacket(Writer &, Vec3 pos, Vec3 motion, float yaw, float pitch);
  bool canInteract(Pos) const;
  void useItemOn(Reader &);
  void playerAction(Reader &);
  void creativeSlot(Reader &);
  void runCommand(std::string_view line);
  void sendSuggestions(int requestId, std::string_view line);
  World &world_;
  const ProtocolData &data_;
  ServerOptions options_;
  const command::Dispatcher *commands_ = nullptr;
  const command::Context *commandContext_ = nullptr;
  const WorldClocks *worldClocks_ = nullptr;
  bool advanceTime_ = true;
  Phase phase_ = Phase::Handshake;
  bool awaitingKnownPacks_ = false, finishSent_ = false,
       teleportPending_ = true;
  bool sneaking_ = false;
  Bytes input_;
  std::vector<OutPacket> output_;
  std::size_t outputBytes_ = 0;
  std::string username_, error_;
  Uuid uuid_{};
  Vec3 position_;
  float yaw_ = 0, pitch_ = 0;
  std::array<ItemStack, 41> inventory_{};
  int selectedSlot_ = 0;
  std::map<int, TrackedEntity> trackedEntities_;
  struct ChangedBlock {
    std::uint64_t section;
    std::uint32_t order;
    std::uint16_t local;
  };
  std::vector<ChangedBlock> changedBlocks_;
  std::set<std::pair<int, int>> availableChunks_, sentChunks_;
  std::vector<std::pair<int, int>> pendingChunks_;
  std::set<std::pair<int, int>> awaitingLightChunks_;
  std::function<bool(int, int)> managedResidency_;
  std::function<std::vector<std::pair<int, int>>()> managedChunks_;
  std::set<std::pair<int, int>> managedPendingResidency_;
  std::map<std::pair<int, int>, std::shared_ptr<std::atomic<bool>>>
      chunkGenerations_;
  // Independent per-layer delta bases. Each stores only the layer revision,
  // dimensions and the worker-provided dirty-section metadata that affects the
  // client; page contents are never diffed after becoming mutable.
  std::map<std::pair<int, int>, LightLayerVersion> sentChunkBlockLight_;
  std::map<std::pair<int, int>, LightLayerVersion> sentChunkSkyLight_;
  std::optional<std::pair<int, int>> chunkCenter_;
  bool awaitingChunkBatch_ = false;
  bool joinPositionPending_ = false;
  int chunksPerBatch_ = 9;
  int chunkBatchLimit_ = 9;
  int dropSpam_ = 0;
  std::int64_t connectionTicks_ = 0, keepAliveSent_ = 0;
  std::uint64_t syncGeneration_ = 0;
  std::optional<std::int64_t> keepAlive_;
};
} // namespace redstone::net
