#pragma once
#include "redstone/engine.hpp"
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <unordered_map>

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
// ClientboundLevelChunkWithLight payload, no packet ID. The snapshot is taken
// on the tick thread; the encoder itself only reads the copy and the immutable
// registry, so it can run on the network writer thread.
Bytes encodeChunk(const ChunkSnapshot &, int chunkX, int chunkZ,
                  const Registry &);
Bytes encodeMovingPistonNbt(const PistonSnapshot &);
// One queued clientbound packet: already framed bytes, or a producer that
// frames them on the writer thread.
struct OutPacket {
  Bytes bytes;                     // framed bytes, or payload if id >= 0
  int packetId = -1;               // >= 0: frame(id, bytes) on the writer
  std::function<Bytes()> deferred; // produced entirely off-thread (chunks)
};
} // namespace redstone::net
