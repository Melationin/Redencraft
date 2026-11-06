#pragma once
#include "redstone/entity.hpp"
#include "redstone/net/wire.hpp"
#include <array>
#include <set>

namespace redstone::net {
// Immutable data exported by the version-pinned vanilla codecs. No network
// registry IDs are inferred from this simulator's smaller registration list.
struct ProtocolData {
  explicit ProtocolData(const std::filesystem::path &directory);
  Registry registry;
  std::vector<Bytes> configuration;
  std::map<std::string, int> packetIds, blockIds;
  int dimensionId = 0;
  int packet(std::string_view phase, std::string_view flow,
             std::string_view name) const;
};
using Uuid = std::array<std::uint8_t, 16>;
Uuid offlineUuid(std::string_view name);
enum class Phase { Handshake, Status, Login, Configuration, Play, Closed };
struct ServerOptions {
  Vec3 spawn{8.5, 66, 8.5};
  std::vector<std::pair<int, int>> chunks;
  int playerId = 1000000;
  int viewDistance = 3;
};
// One connection's deterministic protocol state machine, independent of socket
// IO. Every call and World::tick runs on the server thread.
class Session {
public:
  Session(World &, const ProtocolData &, ServerOptions);
  void receive(std::span<const std::uint8_t> bytes);
  void tick();
  // Call after the measured tick; wall-clock duration excludes this packet.
  void sendTickTime(double milliseconds, std::int64_t tick);
  void blockUpdate(Pos, const BlockState &);
  void flushBlockUpdates();
  void blockEvent(const BlockEvent &);
  void fallingBlockLanded(int entityId, Pos, const BlockState &);
  std::vector<Bytes> takeOutput();
  Phase phase() const { return phase_; }
  bool closed() const { return phase_ == Phase::Closed; }
  const std::string &error() const { return error_; }
  Vec3 playerPosition() const { return position_; }
  const std::array<ItemStack, 41> &inventory() const { return inventory_; }
  std::function<void(std::string_view)> diagnostic;

private:
  void handle(Reader &);
  void play(int, Reader &);
  void join();
  void send(std::string_view name, const Writer &payload = {});
  void packet(int id, const Writer &payload = {});
  void queue(Bytes bytes);
  void disconnect(const std::string &);
  void correct(Pos);
  void acknowledge(int sequence);
  void setSlot(int slot, ItemStack stack);
  void stack(Writer &, const ItemStack &) const;
  void syncEntities();
  void refreshChunkView();
  void sendChunkBatch();
  bool hasChunk(Pos) const;
  void positionPacket(Writer &, Vec3 pos, Vec3 motion, float yaw, float pitch);
  bool canInteract(Pos) const;
  void useItemOn(Reader &);
  void playerAction(Reader &);
  void creativeSlot(Reader &);
  World &world_;
  const ProtocolData &data_;
  ServerOptions options_;
  Phase phase_ = Phase::Handshake;
  bool awaitingKnownPacks_ = false, finishSent_ = false,
       teleportPending_ = true;
  bool sneaking_ = false;
  Bytes input_;
  std::vector<Bytes> output_;
  std::size_t outputBytes_ = 0;
  std::string username_, error_;
  Uuid uuid_{};
  Vec3 position_;
  float yaw_ = 0, pitch_ = 0;
  std::array<ItemStack, 41> inventory_{};
  int selectedSlot_ = 0;
  std::set<int> knownEntities_;
  std::set<Pos> changedBlocks_;
  std::set<std::pair<int, int>> availableChunks_, sentChunks_;
  std::vector<std::pair<int, int>> pendingChunks_;
  std::optional<std::pair<int, int>> chunkCenter_;
  bool awaitingChunkBatch_ = false;
  int chunksPerBatch_ = 9;
  int chunkBatchLimit_ = 9;
  int dropSpam_ = 0;
  std::int64_t connectionTicks_ = 0, keepAliveSent_ = 0;
  std::optional<std::int64_t> keepAlive_;
};
} // namespace redstone::net
