#pragma once
#include "redstone/engine.hpp"
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <unordered_map>

namespace redstone::command {
struct Node;
}
namespace redstone::net {
using Bytes = std::vector<std::uint8_t>;
struct ProtocolError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct Writer {
  Bytes data;
  void u8(std::uint8_t);
  void bool_(bool);
  void i16(std::int16_t);
  void i32(std::int32_t);
  void i64(std::int64_t);
  void f32(float);
  void f64(double);
  void varInt(std::int32_t);
  void varLong(std::int64_t);
  void string(std::string_view);
  void position(Pos);
  void raw(std::span<const std::uint8_t>);
};
struct Reader {
  explicit Reader(std::span<const std::uint8_t> input) : data(input) {}
  std::span<const std::uint8_t> data;
  std::size_t pos = 0;
  std::size_t remaining() const { return data.size() - pos; }
  bool empty() const { return remaining() == 0; }
  std::uint8_t u8();
  bool bool_();
  std::int16_t i16();
  std::int32_t i32();
  std::int64_t i64();
  float f32();
  double f64();
  std::int32_t varInt();
  std::int64_t varLong();
  std::string string(std::size_t maxBytes = 32767 * 3);
  Pos position();
  std::span<const std::uint8_t> raw(std::size_t);
};
// Minecraft's Varint21FrameDecoder permits at most three prefix bytes.
inline constexpr std::size_t maxFrameSize = 2097151;
Bytes frame(int packetId, std::span<const std::uint8_t> payload);
// Returns a packet body (including ID), consuming exactly one complete frame.
// Partial prefixes/payloads leave input untouched. No compression is
// negotiated.
std::optional<Bytes> popFrame(Bytes &input);

class Registry {
public:
  struct StateFlags {
    bool air, fluid, blocksMotion, leaves;
  };
  explicit Registry(const std::filesystem::path &directory);
  int stateId(const BlockState &) const;
  const BlockState &stateById(int) const;
  int itemId(const std::string &) const;
  std::string itemName(int) const;
  int entityTypeId(const std::string &) const;
  int blockEntityTypeId(const std::string &) const;
  int biomeId(const std::string &) const;
  int stateBits() const;
  StateFlags stateFlags(int stateId) const;

private:
  // Dense tables indexed by this simulator's BlockState::Id (and by protocol
  // state id for flags): packet paths never build a name/property string.
  std::vector<int> protocolByLocal_;
  std::vector<StateFlags> flagsByProtocol_;
  std::map<int, BlockState> states_;
  std::map<std::string, int> stateIds_, items_, entities_, blockEntities_,
      biomes_;
  std::map<int, std::string> itemNames_;
  std::map<int, StateFlags> flags_;
};
// Synchronous compatibility entry point, no packet ID. Background encoding
// must use prepareChunkSnapshot on the world thread and the frozen overload.
Bytes encodeChunk(const ChunkSnapshot &, int chunkX, int chunkZ,
                  const Registry &);
// ClientboundCommandsPacket body: node entries in breadth-first order followed by
// the root index. Only the literal and the two argument types /tick needs are
// supported (brigadier:float, minecraft:time).
Bytes encodeCommandTree(const redstone::command::Node &root);
Bytes encodeMovingPistonNbt(const PistonSnapshot &);
// Frozen on the world thread. Sections contain protocol IDs, and block entity
// NBT is owned bytes, so encoding never touches the mutable state registry.
struct ProtocolChunkSnapshot {
  struct BlockEntity {
    Pos pos;
    Bytes nbt;
  };
  int minY = 0, maxY = 0;
  int airId = 0, stateBits = 0, biomeId = 0, pistonTypeId = 0;
  std::vector<SectionSnapshot> sections;
  std::vector<BlockEntity> blockEntities;
  // Published immutable light for the same lifecycle. Null keeps the legacy
  // visualization path for worlds that have not enabled lighting.
  std::shared_ptr<const LightChunkSnapshot> light;
  std::size_t retainedBytes() const noexcept;
};
ProtocolChunkSnapshot prepareChunkSnapshot(
    ChunkSnapshot, const Registry &,
    std::shared_ptr<const LightChunkSnapshot> light = {});
Bytes encodeChunk(const ProtocolChunkSnapshot &, int chunkX, int chunkZ,
                  const Registry &);

// The main thread selects changed sections from immutable published layers;
// the writer thread packs the referenced 2048-byte layers. Missing, stored zero,
// stored full/mixed and a known old layer that must be cleared remain distinct.
struct ProtocolLightSections {
  std::vector<int> dataSections;
  std::vector<int> emptySections;
};
struct ProtocolLightUpdate {
  int chunkX = 0;
  int chunkZ = 0;
  std::shared_ptr<const LightChunkSnapshot> snapshot;
  ProtocolLightSections sky;
  ProtocolLightSections block;
  std::size_t retainedBytes() const noexcept;
};
ProtocolLightUpdate prepareLightUpdate(
    std::shared_ptr<const LightChunkSnapshot> current,
    std::shared_ptr<const LightChunkSnapshot> previous = {});
// Full packet body for ClientboundLightUpdatePacket: VarInt x/z then light data.
Bytes encodeLightUpdate(const ProtocolLightUpdate &);
// One queued clientbound packet: already framed bytes, or a producer that
// frames them on the writer thread.
struct OutPacket {
  Bytes bytes;                     // framed bytes, or payload if id >= 0
  int packetId = -1;               // >= 0: frame(id, bytes) on the writer
  std::function<Bytes()> deferred; // produced entirely off-thread (chunks)
  std::size_t retainedBytes = 0;   // captured snapshot/storage owned by deferred
  // Main-thread cancellation for unloaded/reloaded chunk generations. A
  // cancelled item produces no bytes and never observes live World state.
  std::function<bool()> stillValid;
};
inline constexpr std::size_t maxQueuedOutputBytes = 16 * 1024 * 1024;
inline constexpr std::size_t maxQueuedOutputPackets = 65536;
// Account retained capacity before encoding, not just the eventual wire size.
// Unknown producers still have a minimum charge; chunk producers supply their
// actual captured snapshot footprint. Saturate to make overflow a rejection.
inline std::size_t outPacketCharge(const OutPacket &packet) noexcept {
  std::size_t result = sizeof(OutPacket) + 6;
  const auto add = [&](std::size_t amount) {
    if (amount > maxQueuedOutputBytes ||
        result > maxQueuedOutputBytes - amount)
      result = maxQueuedOutputBytes + 1;
    else
      result += amount;
  };
  add(packet.bytes.capacity());
  add(packet.deferred && !packet.retainedBytes ? 4096 : packet.retainedBytes);
  return result;
}
} // namespace redstone::net
