#include "redstone/net/session.hpp"
#include "redstone/piston.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <numbers>
#include <sstream>
#include <utility>

namespace redstone::net {
namespace {
std::string key(std::string_view phase, std::string_view flow,
                std::string_view name) {
  return std::string(phase) + "/" + std::string(flow) +
         "/minecraft:" + std::string(name);
}
std::vector<std::string> fields(const std::string &line) {
  std::vector<std::string> out;
  std::istringstream stream(line);
  for (std::string s; std::getline(stream, s, '\t');)
    out.push_back(s);
  return out;
}
void nbtText(Writer &out, const std::string &text) {
  out.u8(8);
  out.i16(static_cast<std::int16_t>(text.size()));
  out.raw({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}
// Network NBT for Component.translatable(key, args): a compound with "translate"
// and, when present, "with" as a list of string tags. The client resolves the
// key from its own language files, exactly like a vanilla server's packet.
void nbtTranslate(Writer &out, const std::string &key,
                  const std::vector<std::string> &args) {
  const auto string_tag = [&](std::string_view value) {
    out.i16(static_cast<std::int16_t>(value.size()));
    out.raw({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
  };
  const auto named = [&](std::uint8_t tag, std::string_view name) {
    out.u8(tag);
    string_tag(name);
  };
  out.u8(10); // TAG_Compound, unnamed root.
  named(8, "translate");
  string_tag(key);
  if (!args.empty()) {
    named(9, "with");
    out.u8(8); // list of TAG_String
    out.i32(static_cast<std::int32_t>(args.size()));
    for (const auto &arg : args)
      string_tag(arg);
  }
  out.u8(0); // TAG_End
}
void uuid(Writer &out, const Uuid &id) { out.raw(id); }
int chunkCoord(double coordinate) {
  return static_cast<int>(std::floor(coordinate / 16));
}
// SectionPos.sectionRelativePos: x << 8 | z << 4 | y.
std::uint16_t packSectionLocal(Pos p) {
  return static_cast<std::uint16_t>(((p.x & 15) << 8) | ((p.z & 15) << 4) |
                                    (p.y & 15));
}
// SectionPos.asLong: x << 42 | z << 20 | y, each 22/22/20 bits.
std::int64_t packSectionPos(Pos p) {
  const std::uint64_t x = static_cast<std::uint64_t>(p.x >> 4) & 0x3ffffful;
  const std::uint64_t y = static_cast<std::uint64_t>(p.y >> 4) & 0xfffffull;
  const std::uint64_t z = static_cast<std::uint64_t>(p.z >> 4) & 0x3ffffful;
  return static_cast<std::int64_t>((x << 42) | (z << 20) | y);
}
Pos unpackSectionBlock(std::int64_t section, std::uint16_t local) {
  const auto signExtend = [](std::uint64_t value, int bits) {
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    value &= mask;
    return static_cast<std::int64_t>(
        value & (std::uint64_t{1} << (bits - 1)) ? value | ~mask : value);
  };
  const auto s = static_cast<std::uint64_t>(section);
  return Pos{static_cast<int>(signExtend(s >> 42, 22) * 16 + ((local >> 8) & 15)),
             static_cast<int>(signExtend(s, 20) * 16 + (local & 15)),
             static_cast<int>(signExtend(s >> 20, 22) * 16 + ((local >> 4) & 15))};
}
std::uint8_t packDegrees(float degrees) {
  // Mth.packDegrees floors the float result before narrowing to a byte.
  return static_cast<std::uint8_t>(
      static_cast<int>(std::floor(degrees * 256.0f / 360.0f)));
}

// ChunkHolder's fastutil ShortOpenHashSet uses linear probing and iterates
// zero first, then the occupied slots in descending order. A section has at
// most 4096 local positions, so its largest table is 8192 slots.
class SectionChanges {
public:
  void reset() {
    table_ = std::span(tableA_).first(32);
    std::ranges::fill(table_, std::uint16_t{0});
    size_ = 0;
    hasZero_ = false;
  }
  void add(std::uint16_t local) {
    if (local == 0) {
      if (std::exchange(hasZero_, true))
        return;
    } else if (!insert(table_, local)) {
      return;
    }
    if (size_++ < static_cast<int>(table_.size() * 3 / 4))
      return;
    auto &storage = table_.data() == tableA_.data() ? tableB_ : tableA_;
    auto grown = std::span(storage).first(table_.size() * 2);
    std::ranges::fill(grown, std::uint16_t{0});
    for (auto it = table_.rbegin(); it != table_.rend(); ++it)
      if (*it != 0)
        insert(grown, *it);
    table_ = grown;
  }
  int size() const { return size_; }
  template <typename Emit> void forEach(Emit emit) const {
    if (hasZero_)
      emit(0);
    for (auto it = table_.rbegin(); it != table_.rend(); ++it)
      if (*it != 0)
        emit(*it);
  }

private:
  static bool insert(std::span<std::uint16_t> table, std::uint16_t local) {
    const auto hash = std::uint32_t{local} * 0x9e3779b9u;
    const auto mask = table.size() - 1;
    auto slot = (hash ^ (hash >> 16)) & mask;
    while (table[slot] != 0 && table[slot] != local)
      slot = (slot + 1) & mask;
    if (table[slot] == local)
      return false;
    table[slot] = local;
    return true;
  }

  std::array<std::uint16_t, 8192> tableA_, tableB_;
  std::span<std::uint16_t> table_;
  int size_;
  bool hasZero_;
};
Direction horizontal(float yaw) {
  constexpr Direction directions[]{Direction::South, Direction::West,
                                   Direction::North, Direction::East};
  return directions[static_cast<int>(std::floor(yaw / 90.0f + 0.5f)) & 3];
}
std::array<Direction, 6> lookingDirections(float yaw, float pitch) {
  const double yr = yaw * std::numbers::pi / 180,
               pr = pitch * std::numbers::pi / 180;
  const double x = -std::sin(yr) * std::cos(pr), y = -std::sin(pr),
               z = std::cos(yr) * std::cos(pr);
  const auto dx = x > 0 ? Direction::East : Direction::West;
  const auto dy = y > 0 ? Direction::Up : Direction::Down;
  const auto dz = z > 0 ? Direction::South : Direction::North;
  std::array<Direction, 3> first;
  if (std::abs(x) > std::abs(z)) {
    if (std::abs(y) > std::abs(x))
      first = {dy, dx, dz};
    else if (std::abs(z) > std::abs(y))
      first = {dx, dz, dy};
    else
      first = {dx, dy, dz};
  } else {
    if (std::abs(y) > std::abs(z))
      first = {dy, dz, dx};
    else if (std::abs(x) > std::abs(y))
      first = {dz, dx, dy};
    else
      first = {dz, dy, dx};
  }
  return {first[0],           first[1],           first[2],
          opposite(first[2]), opposite(first[1]), opposite(first[0])};
}
std::string phaseName(Phase p) {
  switch (p) {
  case Phase::Login:
    return "login";
  case Phase::Configuration:
    return "configuration";
  case Phase::Play:
    return "play";
  case Phase::Status:
    return "status";
  default:
    return "handshake";
  }
}
void lpVector(Writer &out, Vec3 value) {
  auto clean = [](double n) {
    return std::isnan(n) ? 0.0
                         : std::clamp(n, -1.7179869183E10, 1.7179869183E10);
  };
  const double x = clean(value.x), y = clean(value.y), z = clean(value.z);
  const double length = std::max({std::abs(x), std::abs(y), std::abs(z)});
  if (length < 3.051944088384301E-5) {
    out.u8(0);
    return;
  }
  const auto scale = static_cast<std::uint64_t>(std::ceil(length));
  const auto pack = [scale](double n) {
    return static_cast<std::uint64_t>(
        std::floor((n / scale * 0.5 + 0.5) * 32766.0 + 0.5));
  };
  const bool partial = scale > 3;
  auto bits = (scale & 3) | (partial ? 4 : 0) | (pack(x) << 3) |
              (pack(y) << 18) | (pack(z) << 33);
  out.u8(static_cast<std::uint8_t>(bits));
  out.u8(static_cast<std::uint8_t>(bits >> 8));
  out.i32(static_cast<std::int32_t>(bits >> 16));
  if (partial)
    out.varInt(static_cast<std::int32_t>(scale >> 2));
}
} // namespace

ProtocolData::ProtocolData(const std::filesystem::path &directory)
    : registry(directory) {
  std::ifstream ids(directory / "packet_ids.tsv");
  if (!ids)
    throw ProtocolError("missing packet_ids.tsv");
  for (std::string line; std::getline(ids, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto f = fields(line);
    if (f.size() == 4)
      packetIds.emplace(f[0] + "/" + f[1] + "/" + f[2], std::stoi(f[3]));
  }
  std::ifstream blocks(directory / "blocks.tsv");
  if (!blocks)
    throw ProtocolError("missing blocks.tsv");
  for (std::string line; std::getline(blocks, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto f = fields(line);
    if (f.size() == 2)
      blockIds.emplace(f[1], std::stoi(f[0]));
  }
  std::ifstream registries(directory / "registry_ids.tsv");
  bool overworld = false;
  for (std::string line; std::getline(registries, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto f = fields(line);
    if (f.size() == 3 && f[0] == "minecraft:dimension_type" &&
        f[2] == "minecraft:overworld") {
      dimensionId = std::stoi(f[1]);
      overworld = true;
    }
  }
  if (!overworld)
    throw ProtocolError("missing overworld dimension registry ID");
  std::ifstream effects(directory / "explosion_effects.tsv");
  for (std::string line; std::getline(effects, line);) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto f = fields(line);
    if (f.size() == 2)
      explosionEffectIds.emplace(f[0], std::stoi(f[1]));
  }
  for (const auto *name : {"explosion", "explosion_emitter", "poof", "smoke", "sound"})
    if (!explosionEffectIds.contains(name))
      throw ProtocolError("missing explosion effect ID in explosion_effects.tsv");
  std::ifstream config(directory / "configuration.bin", std::ios::binary);
  if (!config)
    throw ProtocolError(
        "missing configuration.bin; run validation/protocol775/export.ps1");
  Bytes bytes((std::istreambuf_iterator<char>(config)), {});
  if (bytes.size() > 16 * 1024 * 1024)
    throw ProtocolError("configuration data too large");
  Reader reader(bytes);
  while (!reader.empty()) {
    int length = reader.i32();
    if (length <= 0 || length > static_cast<int>(maxFrameSize))
      throw ProtocolError("invalid configuration packet length");
    auto body = reader.raw(length);
    Reader packetReader(body);
    int id = packetReader.varInt();
    configuration.push_back(frame(id, body.subspan(packetReader.pos)));
  }
}
int ProtocolData::packet(std::string_view phase, std::string_view flow,
                         std::string_view name) const {
  auto it = packetIds.find(key(phase, flow, name));
  if (it == packetIds.end())
    throw ProtocolError("unknown packet " + key(phase, flow, name));
  return it->second;
}

Uuid offlineUuid(std::string_view name) {
  // UUID.nameUUIDFromBytes("OfflinePlayer:" + name), MD5 with RFC4122 v3 bits.
  std::string text = "OfflinePlayer:" + std::string(name);
  Bytes bytes(text.begin(), text.end());
  const std::uint64_t bitLength = bytes.size() * 8;
  bytes.push_back(0x80);
  while (bytes.size() % 64 != 56)
    bytes.push_back(0);
  for (int i = 0; i < 8; ++i)
    bytes.push_back(static_cast<std::uint8_t>(bitLength >> (8 * i)));
  constexpr int shift[]{7, 12, 17, 22, 5, 9,  14, 20,
                        4, 11, 16, 23, 6, 10, 15, 21};
  std::array<std::uint32_t, 4> h{0x67452301, 0xefcdab89, 0x98badcfe,
                                 0x10325476};
  for (std::size_t offset = 0; offset < bytes.size(); offset += 64) {
    std::uint32_t words[16]{};
    for (int i = 0; i < 64; ++i)
      words[i / 4] |= std::uint32_t(bytes[offset + i]) << (8 * (i % 4));
    auto [a, b, c, d] = h;
    for (int i = 0; i < 64; ++i) {
      std::uint32_t f;
      int g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const auto k = static_cast<std::uint32_t>(
          std::floor(std::abs(std::sin(double(i + 1))) * 4294967296.0));
      auto temp = d;
      d = c;
      c = b;
      b += std::rotl(a + f + k + words[g], shift[(i / 16) * 4 + i % 4]);
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
  }
  Uuid result{};
  for (int i = 0; i < 16; ++i)
    result[i] = static_cast<std::uint8_t>(h[i / 4] >> (8 * (i % 4)));
  result[6] = (result[6] & 15) | 0x30;
  result[8] = (result[8] & 63) | 0x80;
  return result;
}

Session::Session(World &world, const ProtocolData &data, ServerOptions options)
    : world_(world), data_(data), options_(std::move(options)),
      position_(options_.spawn) {
  options_.viewDistance = std::clamp(options_.viewDistance, 2, maxViewDistance);
  availableChunks_.insert(options_.chunks.begin(), options_.chunks.end());
  if (availableChunks_.size() > 64)
    chunksPerBatch_ = chunkBatchLimit_ = 2;
}
void Session::packet(int id, const Writer &payload) {
  if (closed())
    return;
  // Framing is a full payload copy; leave it to the writer thread.
  enqueue(OutPacket{payload.data, id, {}});
}
void Session::packet(int id, Writer &&payload) {
  if (!closed())
    enqueue(OutPacket{std::move(payload.data), id, {}});
}
void Session::enqueue(OutPacket item) {
  if (closed())
    return;
  const std::size_t charge = outPacketCharge(item);
  if (output_.size() >= maxQueuedOutputPackets ||
      charge > maxQueuedOutputBytes ||
      outputBytes_ > maxQueuedOutputBytes - charge) {
    output_.clear();
    outputBytes_ = 0;
    error_ = "Connection output backlog exceeded";
    phase_ = Phase::Closed;
    if (diagnostic)
      diagnostic(error_);
    return;
  }
  outputBytes_ += charge;
  output_.push_back(std::move(item));
}
void Session::queue(Bytes bytes) {
  enqueue(OutPacket{std::move(bytes), -1, {}});
}
void Session::queueDeferred(std::function<Bytes()> producer,
                            std::size_t retainedBytes) {
  enqueue(OutPacket{{}, -1, std::move(producer), retainedBytes});
}
void Session::send(std::string_view name, const Writer &payload) {
  if (closed())
    return;
  packet(data_.packet(phaseName(phase_), "clientbound", name), payload);
}
void Session::send(std::string_view name, Writer &&payload) {
  if (!closed())
    packet(data_.packet(phaseName(phase_), "clientbound", name),
           std::move(payload));
}
std::vector<OutPacket> Session::takeOutputItems() {
  outputBytes_ = 0;
  return std::exchange(output_, {});
}
std::vector<Bytes> Session::takeOutput() {
  std::vector<OutPacket> items = takeOutputItems();
  std::vector<Bytes> packets;
  packets.reserve(items.size());
  for (auto &item : items) {
    if (item.deferred)
      packets.push_back(item.deferred());
    else if (item.packetId >= 0)
      packets.push_back(frame(item.packetId, item.bytes));
    else
      packets.push_back(std::move(item.bytes));
  }
  return packets;
}
void Session::disconnect(const std::string &reason) {
  if (closed())
    return;
  error_ = reason;
  Writer payload;
  if (phase_ == Phase::Login) {
    payload.string("{\"text\":\"" + reason + "\"}");
    send("login_disconnect", payload);
  } else if (phase_ == Phase::Play || phase_ == Phase::Configuration) {
    nbtText(payload, reason);
    send("disconnect", payload);
  }
  phase_ = Phase::Closed;
  if (diagnostic)
    diagnostic(reason);
}
void Session::receive(std::span<const std::uint8_t> bytes) {
  if (closed())
    return;
  try {
    if (input_.size() + bytes.size() > 2 * maxFrameSize)
      throw ProtocolError("input backlog exceeded");
    input_.insert(input_.end(), bytes.begin(), bytes.end());
    int count = 0;
    while (auto body = popFrame(input_)) {
      if (++count > 4096)
        throw ProtocolError("too many packets in one read");
      Reader reader(*body);
      handle(reader);
      if (closed())
        break;
    }
  } catch (const std::exception &) {
    // Avoid copying attacker-controlled parser strings into JSON/NBT errors.
    disconnect("Invalid or unsupported protocol packet");
  }
}
void Session::handle(Reader &in) {
  const int id = in.varInt();
  if (phase_ == Phase::Handshake) {
    if (id != 0)
      throw ProtocolError("expected handshake");
    const int version = in.varInt();
    (void)in.string(255 * 3);
    (void)in.i16();
    const int next = in.varInt();
    if (!in.empty() || (next != 1 && next != 2))
      throw ProtocolError("invalid handshake");
    phase_ = next == 1 ? Phase::Status : Phase::Login;
    if (next == 2 && version != 775)
      disconnect("Requires Minecraft 26.1 protocol 775");
  } else if (phase_ == Phase::Status) {
    if (id == 0 && in.empty()) {
      Writer out;
      out.string("{\"version\":{\"name\":\"26.1 "
                 "C++\",\"protocol\":775},\"players\":{\"max\":8,\"online\":0},"
                 "\"description\":{\"text\":\"C++ redstone "
                 "simulator\"},\"enforcesSecureChat\":false}");
      send("status_response", out);
    } else if (id == 1) {
      Writer out;
      out.i64(in.i64());
      if (!in.empty())
        throw ProtocolError("bad ping");
      send("pong_response", out);
      phase_ = Phase::Closed;
    } else
      throw ProtocolError("bad status packet");
  } else if (phase_ == Phase::Login) {
    if (id == 0 && username_.empty()) {
      username_ = in.string(16);
      if (username_.empty() ||
          !std::all_of(username_.begin(), username_.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_';
          }))
        throw ProtocolError("invalid username");
      (void)in.raw(16);
      if (!in.empty())
        throw ProtocolError("bad hello");
      uuid_ = offlineUuid(username_);
      Writer out;
      uuid(out, uuid_);
      out.string(username_);
      out.varInt(0);
      send("login_finished", out);
    } else if (id == 3 && !username_.empty() && in.empty()) {
      phase_ = Phase::Configuration;
      // Offer no known packs: all registry values in the exported stream are
      // present. This also avoids trusting a different patch client's cache.
      Writer out;
      out.varInt(0);
      send("select_known_packs", out);
      awaitingKnownPacks_ = true;
    } else
      throw ProtocolError("unexpected login packet");
  } else if (phase_ == Phase::Configuration) {
    if (id ==
        data_.packet("configuration", "serverbound", "select_known_packs")) {
      if (!awaitingKnownPacks_ || in.varInt() != 0 || !in.empty())
        throw ProtocolError("bad known packs reply");
      awaitingKnownPacks_ = false;
      for (const auto &bytes : data_.configuration)
        queue(bytes);
      send("finish_configuration");
      finishSent_ = true;
    } else if (id == 3) {
      if (!finishSent_ || !in.empty())
        throw ProtocolError("early configuration finish");
      phase_ = Phase::Play;
      join();
    } else if (id != 0 && id != 2 && id != 5)
      throw ProtocolError("unsupported configuration packet");
    // Client information, brand payload and optional pong need no world state.
  } else if (phase_ == Phase::Play)
    play(id, in);
}

void Session::positionPacket(Writer &out, Vec3 pos, Vec3 motion, float yaw,
                             float pitch) {
  out.f64(pos.x);
  out.f64(pos.y);
  out.f64(pos.z);
  out.f64(motion.x);
  out.f64(motion.y);
  out.f64(motion.z);
  out.f32(yaw);
  out.f32(pitch);
  out.i32(0); // no relative flags
}
void Session::join() {
  if (world_.minY() != -64 || world_.maxY() != 320)
    throw ProtocolError("network overworld height must be -64..320");
  Writer out;
  out.i32(options_.playerId);
  out.bool_(false);
  out.varInt(1);
  out.string("minecraft:overworld");
  out.varInt(8);
  out.varInt(options_.viewDistance);
  out.varInt(options_.viewDistance);
  out.bool_(false);
  out.bool_(true);
  out.bool_(false);
  out.varInt(data_.dimensionId);
  out.string("minecraft:overworld");
  out.i64(0);
  out.u8(1);
  out.u8(255);
  out.bool_(false);
  out.bool_(true);
  out.bool_(false);
  out.varInt(0);
  out.varInt(63);
  out.bool_(false);
  send("login", out);
  out = {};
  out.u8(0x0d);
  out.varInt(1);
  uuid(out, uuid_);
  out.string(username_);
  out.varInt(0);
  out.varInt(1);
  out.bool_(true);
  send("player_info_update", out);
  out = {};
  out.u8(15);
  out.f32(0.05f);
  out.f32(0.1f);
  send("player_abilities", out);
  out = {};
  out.string("minecraft:overworld");
  out.position({int(std::floor(position_.x)), int(std::floor(position_.y)),
                int(std::floor(position_.z))});
  out.f32(0);
  out.f32(0);
  send("set_default_spawn_position", out);
  out = {};
  out.varInt(options_.viewDistance);
  send("set_chunk_cache_radius", out);
  sendCommands();
  if (commandContext_ && commandContext_->ticks) {
    // ServerTickRateManager.updateJoiningPlayer.
    sendTickingState(*commandContext_->ticks);
    sendTickingStep(*commandContext_->ticks);
  }
  refreshChunkView();
  out = {};
  out.u8(13);
  out.f32(0);
  send("game_event", out);
  sendChunkBatch();
  out = {};
  out.i64(world_.gameTime());
  out.varInt(0);
  send("set_time", out);
  constexpr const char *initial[]{
      "smooth_stone",        "sticky_piston",  "slime_block",
      "white_carpet",        "redstone_block", "observer",
      "dead_tube_coral_fan", "honey_block",    "tnt"};
  for (int i = 0; i < 9; ++i)
    setSlot(i, itemStack(initial[i], 64));
  out = {};
  out.varInt(1);
  positionPacket(out, position_, {}, yaw_, pitch_);
  send("player_position", out);
  broadcastEntities();
  // TODO: recipes, commands, world clocks, achievements, player avatars and
  // player physics/pushes. This endpoint is a creative editor of loaded chunks.
}
void Session::stack(Writer &out, const ItemStack &item) const {
  if (item.empty()) {
    out.varInt(0);
    return;
  }
  out.varInt(item.count);
  out.varInt(data_.registry.itemId(item.id));
  out.varInt(0);
  out.varInt(0);
}
void Session::setSlot(int slot, ItemStack item) {
  inventory_.at(slot) = std::move(item);
  Writer out;
  out.varInt(slot);
  stack(out, inventory_[slot]);
  send("set_player_inventory", out);
}
bool Session::canInteract(Pos p) const {
  if (teleportPending_ || !world_.isLoaded(p) || !hasChunk(p))
    return false;
  Vec3 eye = position_ + Vec3{0, 1.62, 0};
  const auto dx = std::max({double(p.x) - eye.x, eye.x - (p.x + 1.0), 0.0});
  const auto dy = std::max({double(p.y) - eye.y, eye.y - (p.y + 1.0), 0.0});
  const auto dz = std::max({double(p.z) - eye.z, eye.z - (p.z + 1.0), 0.0});
  return dx * dx + dy * dy + dz * dz <
         36; // base 4.5 + creative .5 + server margin 1
}
void Session::blockUpdate(Pos p, const BlockState &) {
  if (phase_ == Phase::Play && hasChunk(p))
    changedBlocks_.push_back({static_cast<std::uint64_t>(packSectionPos(p)),
                              static_cast<std::uint32_t>(changedBlocks_.size()),
                              packSectionLocal(p)});
}
void Session::flushBlockUpdates() {
  if (changedBlocks_.empty())
    return;
  // ChunkHolder.broadcastChanges accumulates one SectionPos -> ShortSet map and
  // emits a single block_update when a section only has one change, otherwise
  // one section_blocks_update. The states are read from the current section at
  // broadcast time, like ClientboundSectionBlocksUpdatePacket's constructor.
  auto changes = std::exchange(changedBlocks_, {});
  // TODO: vanilla's cross-chunk order depends on ChunkHolder identity hashes.
  // Use coordinate order across chunks and vanilla's bottom-up order within each.
  std::sort(changes.begin(), changes.end(), [](const auto &a, const auto &b) {
    constexpr auto signs = (1ull << 63) | (1ull << 41) | (1ull << 19);
    if (a.section != b.section)
      return (a.section ^ signs) < (b.section ^ signs);
    return a.order < b.order;
  });
  SectionChanges locals;
  for (std::size_t i = 0; i < changes.size();) {
    const auto section = static_cast<std::int64_t>(changes[i].section);
    std::size_t j = i + 1;
    while (j < changes.size() && changes[j].section == changes[i].section)
      ++j;
    const auto first = changes[i].local;
    const Pos firstPos = unpackSectionBlock(section, first);
    if (j - i == 1) {
      sendBlockUpdate(firstPos);
      i = j;
      continue;
    }
    // Every position in a section belongs to the same chunk.
    if (hasChunk(firstPos)) {
      locals.reset();
      for (auto k = i; k < j; ++k)
        locals.add(changes[k].local);
      if (locals.size() == 1) {
        sendBlockUpdate(firstPos);
        i = j;
        continue;
      }
      Writer out;
      out.i64(section);
      out.varInt(locals.size());
      try {
        locals.forEach([&](std::uint16_t local) {
          const auto stateId = data_.registry.stateId(
              world_.getBlock(unpackSectionBlock(section, local)));
          out.varLong((static_cast<std::int64_t>(stateId) << 12) | local);
        });
      } catch (const ProtocolError &) {
        disconnect("Unsupported block state for protocol 775");
        return;
      }
      send("section_blocks_update", std::move(out));
    }
    i = j;
  }
  // Reuse capacity unless a callback recorded another change during broadcast.
  if (changedBlocks_.empty()) {
    changes.clear();
    changedBlocks_.swap(changes);
  }
}
void Session::sendBlockUpdate(Pos p) {
  if (phase_ != Phase::Play || !hasChunk(p))
    return;
  try {
    Writer out;
    out.position(p);
    out.varInt(data_.registry.stateId(world_.getBlock(p)));
    send("block_update", std::move(out));
  } catch (const ProtocolError &) {
    disconnect("Unsupported block state for protocol 775");
  }
}
void Session::blockEvent(const BlockEvent &event) {
  const Vec3 origin{double(event.pos.x), double(event.pos.y), double(event.pos.z)};
  if (phase_ != Phase::Play || (position_ - origin).lengthSqr() >= 64.0 * 64.0)
    return;
  const auto block = data_.blockIds.find(event.block.str());
  if (block == data_.blockIds.end()) {
    disconnect("Unsupported block event for protocol 775");
    return;
  }
  Writer out;
  out.position(event.pos);
  out.u8(static_cast<std::uint8_t>(event.type));
  out.u8(static_cast<std::uint8_t>(event.data));
  out.varInt(block->second);
  send("block_event", std::move(out));
}
void Session::explosion(Vec3 center, float radius, int blockCount) {
  if (phase_ != Phase::Play || (position_ - center).lengthSqr() >= 64.0 * 64.0)
    return;
  const auto &ids = data_.explosionEffectIds;
  Writer out;
  out.f64(center.x);
  out.f64(center.y);
  out.f64(center.z);
  out.f32(radius);
  out.i32(blockCount);
  // TODO: protocol players are not simulated entities and have no server-side
  // explosion knockback yet.
  out.bool_(false);
  out.varInt(ids.at(radius < 2.0f ? "explosion" : "explosion_emitter"));
  out.varInt(ids.at("sound") + 1); // Registry holder, 0 reserved for inline.
  out.varInt(2);
  // Level.DEFAULT_EXPLOSION_BLOCK_PARTICLES, each with weight 1.
  out.varInt(ids.at("poof"));
  out.f32(0.5f);
  out.f32(1.0f);
  out.varInt(1);
  out.varInt(ids.at("smoke"));
  out.f32(1.0f);
  out.f32(1.0f);
  out.varInt(1);
  send("explode", std::move(out));
}
void Session::fallingBlockLanded(int entityId, Pos pos, const BlockState &) {
  // FallingBlockEntity.sendToTrackingPlayers precedes discard. Ordinary chunk
  // dirty broadcasts remain in their existing earlier tick phase.
  if (trackedEntities_.contains(entityId))
    sendBlockUpdate(pos);
}
void Session::acknowledge(int sequence) {
  if (sequence < 0)
    throw ProtocolError("negative prediction sequence");
  Writer out;
  out.varInt(sequence);
  send("block_changed_ack", out);
}
void Session::creativeSlot(Reader &in) {
  const int slot = in.i16(), count = in.varInt();
  ItemStack item;
  if (count < 0 || count > 64)
    throw ProtocolError("invalid stack count");
  if (count > 0) {
    item = itemStack(data_.registry.itemName(in.varInt()), count);
    if (count > item.maxStackSize())
      throw ProtocolError("stack exceeds item maximum");
    const int added = in.varInt(), removed = in.varInt();
    if (added < 0 || removed < 0 || added > 256 || removed > 256)
      throw ProtocolError("invalid components");
    for (int i = 0; i < added; ++i) {
      (void)in.varInt();
      int size = in.varInt();
      if (size < 0)
        throw ProtocolError("invalid component size");
      (void)in.raw(size);
    }
    for (int i = 0; i < removed; ++i)
      (void)in.varInt();
    // TODO: custom block-state/item components require a real item component
    // model. Reject rather than silently losing their placement semantics.
    if (added || removed ||
        (!isSupportedBlock(item.id) && item.id != "minecraft:minecart")) {
      item = {};
      if (diagnostic)
        diagnostic("TODO: unsupported creative item or custom components");
    }
  }
  if (!in.empty())
    throw ProtocolError("trailing creative slot data");
  if (slot >= 36 && slot <= 44)
    setSlot(slot - 36, item);
  else if (slot >= 9 && slot <= 35)
    setSlot(slot, item);
  else if (slot == 45)
    setSlot(40, item);
  else if (slot == -1) {
    if (!item.empty() && dropSpam_ < 200 && !teleportPending_ &&
        world_.isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      dropSpam_ += 20;
      world_.spawnItemAt(position_ + Vec3{0, 1.3, 0}, item.id, item.count);
    }
  }
}
void Session::playerAction(Reader &in) {
  const int action = in.varInt();
  const Pos p = in.position();
  const int face = in.u8(), sequence = in.varInt();
  if (face > 5 || action < 0 || sequence < 0 || !in.empty())
    throw ProtocolError("invalid action");
  if (action <= 2) {
    if (canInteract(p) && action == 0) {
      const auto old = world_.getBlock(p);
      // Creative destruction has no drops, including the counterpart base of
      // a piston head. Keep setBlock's normal update/removal paths.
      if (old.is(Blocks::PISTON_HEAD)) {
        const auto basePos =
            p.relative(opposite(directionFromString(old.get("facing"))));
        const auto base = world_.getBlock(basePos);
        if ((old.get("type", "normal") == "sticky"
                 ? base.is(Blocks::STICKY_PISTON)
                 : base.is(Blocks::PISTON)) &&
            base.get("extended") == "true" &&
            base.get("facing") == old.get("facing"))
          world_.setBlock(basePos, airState());
      }
      world_.setBlock(p, airState());
    }
    sendBlockUpdate(p);
    acknowledge(sequence);
  } else if (action == 6) {
    const auto held = inventory_[selectedSlot_];
    setSlot(selectedSlot_, inventory_[40]);
    setSlot(40, held);
  } else if (action == 3 || action == 4) {
    auto item = inventory_[selectedSlot_];
    if (!item.empty() && !teleportPending_ &&
        world_.isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      int count = action == 3 ? item.count : 1;
      world_.spawnItemAt(position_ + Vec3{0, 1.3, 0}, item.id, count);
      item.count -= count;
      setSlot(selectedSlot_, item);
    }
  } else if (diagnostic)
    diagnostic("TODO: unsupported player action");
}
void Session::useItemOn(Reader &in) {
  const int hand = in.varInt();
  const Pos clicked = in.position();
  const int faceId = in.varInt();
  const float hitX = in.f32(), hitY = in.f32(), hitZ = in.f32();
  (void)in.bool_();
  (void)in.bool_();
  const int sequence = in.varInt();
  if (hand < 0 || hand > 1 || faceId < 0 || faceId > 5 || !in.empty() ||
      sequence < 0 || !std::isfinite(hitX) || !std::isfinite(hitY) ||
      !std::isfinite(hitZ) || std::abs(hitX - .5) > 1.0000001 ||
      std::abs(hitY - .5) > 1.0000001 || std::abs(hitZ - .5) > 1.0000001)
    throw ProtocolError("invalid block hit");
  const Direction face = static_cast<Direction>(faceId);
  const auto old = world_.getBlock(clicked);
  const Pos target = isReplaceable(old) ? clicked : clicked.relative(face);
  const auto held = inventory_[hand ? 40 : selectedSlot_];
  if (canInteract(clicked) && !sneaking_ && isFenceGate(old)) {
    auto next = old;
    if (old.getView("open", "false") == "true") {
      next = next.with("open", "false");
    } else {
      const auto playerFacing = horizontal(yaw_);
      if (directionFromString(old.getView("facing", "north")) == opposite(playerFacing))
        next = next.with("facing", directionName(playerFacing));
      next = next.with("open", "true");
    }
    world_.setBlock(clicked, next, 10);
    (void)world_.random.nextFloat();
    // TODO: fence gate sound and BLOCK_OPEN/BLOCK_CLOSE game events.
  } else if (canInteract(clicked) && !sneaking_ &&
      old.is(Blocks::NOTE_BLOCK)) {
    auto tuned = old.with(
        "note", std::to_string((std::stoi(old.get("note", "0")) + 1) % 25));
    world_.setBlock(clicked, tuned, 3);
    if (isAirState(world_.getBlock(clicked.relative(Direction::Up))))
      world_.blockEvent(clicked, old.name(), 0, 0);
  } else if (canInteract(clicked) && !held.empty() &&
             held.id == "minecraft:minecart") {
    if (isRailBlock(old)) {
      const double raised = railShapeIsSlope(railShapeOf(old)) ? 0.5 : 0;
      world_.spawnMinecart(
          {clicked.x + 0.5, clicked.y + 0.0625 + raised, clicked.z + 0.5});
    }
  } else if (canInteract(clicked) && canInteract(target) &&
             isReplaceable(world_.getBlock(target)) && !held.empty()) {
    if (isSupportedBlock(held.id) && held.id != "minecraft:air" &&
        held.id != "minecraft:water" && held.id != "minecraft:lava") {
      auto state = defaultState(held.id);
      bool valid = true;
      if (isDeadCoralFan(state)) {
        auto directions = lookingDirections(yaw_, pitch_);
        // BlockPlaceContext prioritizes the clicked face only when placing
        // adjacent to a non-replaceable clicked block.
        if (target != clicked) {
          auto it =
              std::find(directions.begin(), directions.end(), opposite(face));
          std::rotate(directions.begin(), it, it + 1);
        }
        std::optional<BlockState> wall;
        for (Direction d : directions)
          if (d != Direction::Up && d != Direction::Down) {
            wall = world_.coralPlacementState(target, state, opposite(d));
            if (wall)
              break;
          }
        valid = false;
        for (Direction d : directions) {
          if (d == Direction::Up)
            continue;
          auto candidate =
              d == Direction::Down
                  ? world_.coralPlacementState(target, state, Direction::Up)
                  : wall;
          if (candidate) {
            state = *candidate;
            valid = true;
            break;
          }
        }
      } else if (isFenceGate(state)) {
        const bool powered = world_.hasNeighborSignal(target);
        state = state.with("facing", directionName(horizontal(yaw_)))
                     .with("open", powered ? "true" : "false")
                     .with("powered", powered ? "true" : "false");
        state = world_.updateFromNeighbourShapes(target, state);
      } else if (state.hasProperty("facing")) {
        const auto look = lookingDirections(yaw_, pitch_)[0];
        state = state.with("facing",
                           directionName(state.is(Blocks::OBSERVER)
                                             ? look
                                             : opposite(look)));
      } else if (isRailBlock(state)) {
        const auto look = horizontal(yaw_);
        state = state.with("shape",
                           (look == Direction::East || look == Direction::West)
                               ? "east_west"
                               : "north_south");
      }
      if (state.hasProperty("waterlogged"))
        state = state.with("waterlogged", "false");
      if (isWoolCarpet(state))
        valid = !isAirState(world_.getBlock(target.relative(Direction::Down)));
      if (isRailBlock(state))
        valid = railCanSurvive(world_, target, state);
      // getStateForPlacement initializes these neighbor-derived properties.
      // Do not call observer.updateShape speculatively: it schedules a pulse
      // before the observer even exists in the world.
      if (isWallBlock(state) || state.is(Blocks::GRASS_BLOCK) ||
          state.is(Blocks::NOTE_BLOCK))
        state = world_.updateFromNeighbourShapes(target, state);
      if (state.is(Blocks::REDSTONE_LAMP))
        state = state.with("lit",
                           world_.hasNeighborSignal(target) ? "true" : "false");
      if (valid) {
        const Aabb player{position_.x - .3,  position_.y,
                          position_.z - .3,  position_.x + .3,
                          position_.y + 1.8, position_.z + .3};
        for (const auto &box : collisionShape(world_, target, state)) {
          const Aabb placed{target.x + box.x0 / 16, target.y + box.y0 / 16,
                            target.z + box.z0 / 16, target.x + box.x1 / 16,
                            target.y + box.y1 / 16, target.z + box.z1 / 16};
          if (placed.intersects(player))
            valid = false;
          for (Entity *entity : world_.entitiesIn(placed))
            if (entity->typeName() == "minecraft:tnt" ||
                entity->typeName() == "minecraft:falling_block" ||
                entity->typeName() == "minecraft:minecart")
              valid = false;
        }
        if (valid && !isAirState(state))
          world_.setBlock(target, state, 11);
      }
      // TODO: other players' placement collision, exact Mth trigonometric
      // lookup ties, replaceable-by-item predicates, skull note overrides and
      // arbitrary items. Never force-load a chunk on placement.
    } else if (diagnostic)
      diagnostic("TODO: unsupported placement item");
  }
  sendBlockUpdate(clicked);
  sendBlockUpdate(target);
  acknowledge(sequence);
}
void Session::play(int id, Reader &in) {
  const auto is = [&](std::string_view name) {
    return id == data_.packet("play", "serverbound", name);
  };
  if (is("accept_teleportation")) {
    if (in.varInt() == 1)
      teleportPending_ = false;
    if (!in.empty())
      throw ProtocolError("bad teleport ack");
  } else if (is("keep_alive")) {
    if (!keepAlive_ || in.i64() != *keepAlive_ || !in.empty())
      throw ProtocolError("bad keepalive");
    keepAlive_.reset();
  } else if (is("ping_request")) {
    Writer out;
    out.i64(in.i64());
    send("pong_response", out);
  } else if (is("move_player_pos") || is("move_player_pos_rot") ||
             is("move_player_rot") || is("move_player_status_only")) {
    Vec3 pos = position_;
    float yaw = yaw_, pitch = pitch_;
    if (is("move_player_pos") || is("move_player_pos_rot")) {
      pos.x = in.f64();
      pos.y = in.f64();
      pos.z = in.f64();
    }
    if (is("move_player_rot") || is("move_player_pos_rot")) {
      yaw = in.f32();
      pitch = in.f32();
    }
    (void)in.u8();
    if (!in.empty() || !std::isfinite(pos.x) || !std::isfinite(pos.y) ||
        !std::isfinite(pos.z) || !std::isfinite(yaw) || !std::isfinite(pitch) ||
        std::abs(pos.x) > 29999984 || std::abs(pos.z) > 29999984 ||
        std::abs(pos.y) > 20000000)
      throw ProtocolError("invalid movement");
    if (!teleportPending_) {
      position_ = pos;
      yaw_ = std::remainder(yaw, 360.0f);
      pitch_ = std::clamp(pitch, -90.0f, 90.0f);
    }
    // TODO: ServerPlayer authoritative physics and anti-cheat. Network view
    // follows movement; simulation chunk loading remains explicitly fixed.
  } else if (is("set_carried_item")) {
    int slot = in.i16();
    if (slot < 0 || slot > 8 || !in.empty())
      throw ProtocolError("invalid held slot");
    selectedSlot_ = slot;
  } else if (is("set_creative_mode_slot"))
    creativeSlot(in);
  else if (is("player_action"))
    playerAction(in);
  else if (is("use_item_on"))
    useItemOn(in);
  else if (is("pick_item_from_block")) {
    Pos p = in.position();
    (void)in.bool_();
    if (!in.empty())
      throw ProtocolError("invalid pick");
    auto item = blockItem(world_.getBlock(p));
    if (canInteract(p) && !item.empty())
      setSlot(selectedSlot_, itemStack(item));
  } else if (is("use_item")) {
    (void)in.varInt();
    int sequence = in.varInt();
    (void)in.f32();
    (void)in.f32();
    acknowledge(sequence);
  } else if (is("chat_command")) {
    const std::string line = in.string();
    if (!in.empty())
      throw ProtocolError("invalid chat command");
    runCommand(line);
  } else if (is("command_suggestion")) {
    const int requestId = in.varInt();
    const std::string line = in.string(32500);
    if (!in.empty())
      throw ProtocolError("invalid command suggestion request");
    sendSuggestions(requestId, line);
  } else if (is("player_input")) {
    const auto flags = in.u8();
    if (!in.empty())
      throw ProtocolError("bad player input");
    sneaking_ = (flags & 32) != 0;
  } else if (is("chunk_batch_received")) {
    const float desired = in.f32();
    if (!in.empty() || !std::isfinite(desired) || desired <= 0)
      throw ProtocolError("invalid chunk batch acknowledgement");
    awaitingChunkBatch_ = false;
    chunksPerBatch_ =
        static_cast<int>(std::clamp(desired, 1.0f, float(chunkBatchLimit_)));
  } else if (is("client_tick_end") || is("player_loaded") ||
             is("player_abilities") || is("client_information") ||
             is("custom_payload") || is("swing") || is("player_command") ||
             is("container_close") || is("chat_session_update") || is("pong")) {
    // These packets do not modify this creative editor's simulated blocks.
    // TODO: player actions, general containers, chat/session state and sprint.
  } else if (diagnostic)
    diagnostic("TODO: unsupported play packet ignored");
}
bool Session::hasChunk(Pos p) const {
  return sentChunks_.contains({chunkCoord(p.x), chunkCoord(p.z)});
}
void Session::refreshChunkView() {
  const std::pair center{chunkCoord(position_.x), chunkCoord(position_.z)};
  if (chunkCenter_ == center)
    return;
  chunkCenter_ = center;
  Writer out;
  out.varInt(center.first);
  out.varInt(center.second);
  send("set_chunk_cache_center", out);
  const auto visible = [&](std::pair<int, int> p) {
    return std::abs(p.first - center.first) <= options_.viewDistance &&
           std::abs(p.second - center.second) <= options_.viewDistance;
  };
  for (auto it = sentChunks_.begin(); it != sentChunks_.end();) {
    if (visible(*it)) {
      ++it;
      continue;
    }
    out = {};
    // FriendlyByteBuf.writeChunkPos = ChunkPos.toLong, x in the low bits.
    out.i32(it->second);
    out.i32(it->first);
    send("forget_level_chunk", out);
    it = sentChunks_.erase(it);
  }
  pendingChunks_.clear();
  for (auto p : availableChunks_)
    if (visible(p) && !sentChunks_.contains(p) &&
        world_.isChunkLoaded(p.first, p.second))
      pendingChunks_.push_back(p);
  std::sort(pendingChunks_.begin(), pendingChunks_.end(), [&](auto a, auto b) {
    const auto distance = [&](auto p) {
      const auto dx = p.first - center.first, dz = p.second - center.second;
      return dx * dx + dz * dz;
    };
    if (distance(a) != distance(b))
      return distance(a) < distance(b);
    return a < b;
  });
  // TODO: vanilla rounded tracking-view boundary and client settings radius;
  // this fixed square is only the visualization view, not simulation loading.
}
void Session::sendChunkBatch() {
  if (awaitingChunkBatch_ || pendingChunks_.empty() || closed())
    return;
  send("chunk_batch_start");
  const int chunkPacket =
      data_.packet(phaseName(phase_), "clientbound", "level_chunk_with_light");
  int count = 0;
  std::size_t bytes = 0;
  while (!pendingChunks_.empty() && count < chunksPerBatch_ &&
         bytes < 2 * 1024 * 1024) {
    const auto [x, z] = pendingChunks_.front();
    pendingChunks_.erase(pendingChunks_.begin());
    if (!world_.isChunkLoaded(x, z))
      continue;
    // Snapshot on the tick thread (the world is not thread safe), then let the
    // writer thread run the palette/bit packing. The batch-start ordering is
    // preserved because queue items are drained in order.
    auto snapshot = prepareChunkSnapshot(world_.snapshotChunk(x, z), data_.registry);
    for (const auto &section : snapshot.sections)
      bytes += section.palette.size() * sizeof(BlockState::Id) +
               section.indices8.size() + section.indices16.size() * 2 + 64;
    const auto &registry = data_.registry;
    const auto retainedBytes = snapshot.retainedBytes();
    queueDeferred([snapshot = std::move(snapshot), chunkPacket, x, z,
                   &registry]() {
      return frame(chunkPacket, encodeChunk(snapshot, x, z, registry));
    }, retainedBytes);
    if (closed())
      return;
    sentChunks_.emplace(x, z);
    ++count;
  }
  Writer out;
  out.varInt(count);
  send("chunk_batch_finished", out);
  awaitingChunkBatch_ = true;
}
void Session::sendTickTime(double milliseconds, std::int64_t tick, std::size_t samples) {
  if (phase_ != Phase::Play || !std::isfinite(milliseconds) || milliseconds < 0)
    return;
  try {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "Tick " << tick << ": " << std::fixed << std::setprecision(3)
         << milliseconds << " ms (1s avg, " << samples << " ticks)";
    Writer out;
    // ComponentSerialization accepts a network NBT StringTag for literal text.
    nbtText(out, text.str());
    out.bool_(false); // Ordinary chat, as requested by the user.
    send("system_chat", out);
  } catch (const ProtocolError &) {
    disconnect("Tick timing packet failed");
  }
}
void Session::attachCommands(const command::Dispatcher *dispatcher,
                             const command::Context *context) {
  commands_ = dispatcher;
  commandContext_ = context;
}
void Session::sendCommandMessage(const command::Message &message) {
  Writer out;
  nbtTranslate(out, message.key, message.args);
  out.bool_(false); // ClientboundSystemChatPacket(component, overlay=false).
  send("system_chat", out);
}
void Session::runCommand(std::string_view line) {
  if (!commands_ || !commandContext_)
    return;
  command::Source source;
  // TODO: vanilla checks the op list / permission level per player; this
  // server has no operator list yet, so the local player is an admin.
  source.permissionLevel = 4;
  source.name = username_;
  if (commands_->run(line, source, *commandContext_) < 0) {
    // The client parses against the exported tree before sending, so this is a
    // malformed or unknown line: brigadier would answer with a syntax error.
    Writer out;
    nbtText(out, "Unknown or incomplete command");
    out.bool_(false);
    send("system_chat", out);
    return;
  }
  // TODO: broadcast=true lines go to every player with the command permission in
  // vanilla; this server has a single local player, so they only echo here.
  for (const auto &message : source.messages)
    sendCommandMessage(message);
}
void Session::sendSuggestions(int requestId, std::string_view line) {
  if (!commands_ || !commandContext_ || phase_ != Phase::Play)
    return;
  try {
    // ServerGamePacketListenerImpl.handleCustomCommandSuggestions skips a leading
    // slash with StringReader.skip, which leaves the string intact, so every
    // replacement offset is still relative to the text the client sent.
    const bool slash = !line.empty() && line.front() == '/';
    const std::string_view body = slash ? line.substr(1) : line;
    command::Source source;
    source.permissionLevel = 4;
    source.name = username_;
    auto completion = commands_->suggestions(body, source);
    if (slash)
      ++completion.start;
    Writer out;
    out.varInt(requestId);
    out.varInt(completion.start);
    out.varInt(completion.length);
    out.varInt(static_cast<std::int32_t>(completion.texts.size()));
    for (const auto &text : completion.texts) {
      out.string(text);
      out.bool_(false); // Optional<Component> tooltip: none.
    }
    send("command_suggestions", out);
  } catch (const ProtocolError &) {
    disconnect("Command suggestion packet failed");
  }
}
void Session::sendCommands() {
  if (!commands_ || phase_ != Phase::Play)
    return;
  try {
    Writer out;
    const auto tree = encodeCommandTree(commands_->root());
    out.raw(tree);
    send("commands", out);
  } catch (const ProtocolError &) {
    disconnect("Command tree packet failed");
  }
}
void Session::sendTickingState(const TickRateManager &ticks) {
  if (phase_ != Phase::Play)
    return;
  Writer out;
  out.f32(ticks.tickrate());
  out.bool_(ticks.isFrozen());
  send("ticking_state", out);
}
void Session::sendTickingStep(const TickRateManager &ticks) {
  if (phase_ != Phase::Play)
    return;
  Writer out;
  out.varInt(ticks.frozenTicksToRun());
  send("ticking_step", out);
}
void Session::sendEntitySpawn(const Entity &e, TrackedEntity &tracked) {
  Writer out;
  out.varInt(e.id);
  uuid(out, offlineUuid("entity_" + std::to_string(e.id)));
  out.varInt(data_.registry.entityTypeId(e.typeName()));
  out.f64(e.position.x);
  out.f64(e.position.y);
  out.f64(e.position.z);
  lpVector(out, e.deltaMovement);
  out.u8(packDegrees(e.pitch));
  out.u8(packDegrees(e.yaw));
  out.u8(0);
  const auto *falling = dynamic_cast<const FallingBlockEntity *>(&e);
  out.varInt(falling ? data_.registry.stateId(falling->blockState) : 0);
  send("bundle_delimiter");
  send("add_entity", std::move(out));

  // ServerEntity pairs the spawn with the non-default synced data.
  if (auto item = dynamic_cast<const ItemEntity *>(&e)) {
    Writer values;
    values.varInt(e.id);
    values.u8(8);
    values.varInt(7);
    stack(values, item->item);
    values.u8(255);
    send("set_entity_data", std::move(values));
  } else if (auto tnt = dynamic_cast<const PrimedTnt *>(&e)) {
    tracked.tntBlockState = Blocks::TNT.defaultState().id();
    sendTntData(*tnt, tracked);
  } else if (falling && falling->startPos != Pos{}) {
    Writer values;
    values.varInt(e.id);
    values.u8(8);
    values.varInt(10); // BLOCK_POS, see entity_data_serializers.tsv.
    values.position(falling->startPos);
    values.u8(255);
    send("set_entity_data", std::move(values));
  }
  send("bundle_delimiter");

  tracked.base = e.position;
  tracked.lastMovement = e.deltaMovement;
  tracked.lastMotionRevision = e.motionRevision;
  tracked.yRot = packDegrees(e.yaw);
  tracked.xRot = packDegrees(e.pitch);
  tracked.tickCount = 0;
  tracked.teleportDelay = 0;
  tracked.wasOnGround = e.onGround;
}

void Session::sendTntData(const PrimedTnt &tnt, TrackedEntity &tracked) {
  const bool fuseChanged = tnt.fuse != tracked.tntFuse;
  const bool stateChanged = tnt.blockState.id() != tracked.tntBlockState;
  if (!fuseChanged && !stateChanged)
    return;
  Writer values;
  values.varInt(tnt.id);
  if (fuseChanged) {
    values.u8(8);
    values.varInt(1); // INT
    values.varInt(tnt.fuse);
  }
  if (stateChanged) {
    values.u8(9);
    values.varInt(14); // BLOCK_STATE
    values.varInt(data_.registry.stateId(tnt.blockState));
  }
  values.u8(255);
  send("set_entity_data", std::move(values));
  tracked.tntFuse = tnt.fuse;
  tracked.tntBlockState = tnt.blockState.id();
}

void Session::sendEntityChanges(const Entity &e, TrackedEntity &tracked) {
  // ServerEntity.sendChanges: movement is encoded as a 1/4096 delta against the
  // last sent position (VecDeltaCodec), rotation as 1/256 turns, and a
  // full entity_position_sync only when the delta overflows a short or the
  // ground state changes.
  // TODO: new minecart interpolation and dirty metadata for item stacks.
  // TODO: the separate post-update hurtMarked velocity packet.
  const int interval = e.type().updateInterval > 0 ? e.type().updateInterval : 3;
  const auto *tnt = dynamic_cast<const PrimedTnt *>(&e);
  const bool dirty = tnt && (tnt->fuse != tracked.tntFuse ||
                             tnt->blockState.id() != tracked.tntBlockState);
  if (tracked.tickCount % interval == 0 || dirty ||
      e.motionRevision != tracked.lastMotionRevision) {
    tracked.lastMotionRevision = e.motionRevision;
    const auto motionDiff = (e.deltaMovement - tracked.lastMovement).lengthSqr();
    // All currently supported entity types track deltas. Send motion before
    // the position packet, even when that packet is a full position sync.
    if (motionDiff > 1e-7 || (motionDiff > 0 && e.deltaMovement.lengthSqr() == 0)) {
      tracked.lastMovement = e.deltaMovement;
      Writer out;
      out.varInt(e.id);
      lpVector(out, e.deltaMovement);
      send("set_entity_motion", std::move(out));
    }
    const std::uint8_t yRot = packDegrees(e.yaw);
    const std::uint8_t xRot = packDegrees(e.pitch);
    const bool rotationChanged =
        std::abs(int(yRot) - int(tracked.yRot)) >= 1 ||
        std::abs(int(xRot) - int(tracked.xRot)) >= 1;
    const Vec3 offset = e.position - tracked.base;
    const bool positionChanged = offset.lengthSqr() >= 7.6293945e-6;
    const bool position = positionChanged || tracked.tickCount % 60 == 0;
    const auto encode = [](double value) {
      const double scaled = value * 4096.0;
      const double integral = std::floor(scaled);
      return static_cast<std::int64_t>(integral) + (scaled - integral >= 0.5);
    };
    const std::int64_t xa = encode(e.position.x) - encode(tracked.base.x);
    const std::int64_t ya = encode(e.position.y) - encode(tracked.base.y);
    const std::int64_t za = encode(e.position.z) - encode(tracked.base.z);
    const bool deltaTooBig = xa < -32768 || xa > 32767 || ya < -32768 ||
                             ya > 32767 || za < -32768 || za > 32767;
    ++tracked.teleportDelay;
    bool sentPosition = false, sentRotation = false;
    if (deltaTooBig || tracked.teleportDelay > 400 ||
        tracked.wasOnGround != e.onGround) {
      tracked.wasOnGround = e.onGround;
      tracked.teleportDelay = 0;
      Writer out;
      out.varInt(e.id);
      out.f64(e.position.x);
      out.f64(e.position.y);
      out.f64(e.position.z);
      out.f64(e.deltaMovement.x);
      out.f64(e.deltaMovement.y);
      out.f64(e.deltaMovement.z);
      out.f32(e.yaw);
      out.f32(e.pitch);
      out.bool_(e.onGround);
      send("entity_position_sync", std::move(out));
      sentPosition = sentRotation = true;
    } else if (position && rotationChanged) {
      Writer out;
      out.varInt(e.id);
      out.i16(static_cast<std::int16_t>(xa));
      out.i16(static_cast<std::int16_t>(ya));
      out.i16(static_cast<std::int16_t>(za));
      out.u8(yRot);
      out.u8(xRot);
      out.bool_(e.onGround);
      send("move_entity_pos_rot", std::move(out));
      sentPosition = sentRotation = true;
    } else if (position) {
      Writer out;
      out.varInt(e.id);
      out.i16(static_cast<std::int16_t>(xa));
      out.i16(static_cast<std::int16_t>(ya));
      out.i16(static_cast<std::int16_t>(za));
      out.bool_(e.onGround);
      send("move_entity_pos", std::move(out));
      sentPosition = true;
    } else if (rotationChanged) {
      Writer out;
      out.varInt(e.id);
      out.u8(yRot);
      out.u8(xRot);
      out.bool_(e.onGround);
      send("move_entity_rot", std::move(out));
      sentRotation = true;
    }
    if (sentPosition)
      tracked.base = e.position;
    if (sentRotation) {
      tracked.yRot = yRot;
      tracked.xRot = xRot;
    }
    if (dirty)
      sendTntData(*tnt, tracked);
  }
  ++tracked.tickCount;
}

void Session::syncEntities() {
  // ChunkMap.TrackedEntity only updates entities inside the player's tracking
  // view; the entity section storage makes that a small per-column scan.
  ++syncGeneration_;
  std::vector<Entity *> candidates;
  if (chunkCenter_) {
    const int range = options_.viewDistance;
    candidates = world_.entitiesInSectionRange(
        chunkCenter_->first - range, chunkCenter_->first + range,
        chunkCenter_->second - range, chunkCenter_->second + range);
  } else {
    candidates.reserve(world_.entities.size());
    for (const auto &ptr : world_.entities)
      candidates.push_back(ptr.get());
  }
  for (const Entity *candidate : candidates) {
    if (!candidate)
      continue;
    const auto &e = *candidate;
    if (e.isRemoved() ||
        !world_.isChunkLoaded(chunkCoord(e.position.x),
                              chunkCoord(e.position.z)) ||
        !sentChunks_.contains(
            {chunkCoord(e.position.x), chunkCoord(e.position.z)}))
      continue;
    const auto [tracked, inserted] = trackedEntities_.try_emplace(e.id);
    if (inserted) {
      sendEntitySpawn(e, tracked->second);
      tracked->second.lastSeenGeneration = syncGeneration_;
    } else {
      tracked->second.lastSeenGeneration = syncGeneration_;
      sendEntityChanges(e, tracked->second);
    }
  }
  Writer removed;
  int count = 0;
  for (const auto &[id, tracked] : trackedEntities_)
    if (tracked.lastSeenGeneration != syncGeneration_)
      ++count;
  if (count) {
    removed.varInt(count);
    for (const auto &[id, tracked] : trackedEntities_)
      if (tracked.lastSeenGeneration != syncGeneration_)
        removed.varInt(id);
    send("remove_entities", std::move(removed));
  }
  std::erase_if(trackedEntities_, [&](const auto &entry) {
    return entry.second.lastSeenGeneration != syncGeneration_;
  });
}
void Session::broadcastEntities() {
  if (phase_ != Phase::Play)
    return;
  try {
    syncEntities();
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
void Session::entityAdded(const Entity &entity) {
  if (phase_ != Phase::Play || entity.isRemoved() ||
      !world_.isChunkLoaded(chunkCoord(entity.position.x),
                            chunkCoord(entity.position.z)) ||
      !sentChunks_.contains({chunkCoord(entity.position.x),
                             chunkCoord(entity.position.z)}))
    return;
  try {
    const auto [tracked, inserted] = trackedEntities_.try_emplace(entity.id);
    if (inserted) {
      sendEntitySpawn(entity, tracked->second);
      tracked->second.lastSeenGeneration = syncGeneration_;
    }
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
void Session::entityRemoved(int entityId) {
  if (phase_ != Phase::Play)
    return;
  const auto tracked = trackedEntities_.find(entityId);
  if (tracked == trackedEntities_.end())
    return;
  try {
    Writer out;
    out.varInt(1);
    out.varInt(entityId);
    send("remove_entities", std::move(out));
    trackedEntities_.erase(tracked);
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
void Session::tick() {
  if (closed())
    return;
  try {
    ++connectionTicks_;
    if (dropSpam_ > 0)
      --dropSpam_;
    if (phase_ != Phase::Play) {
      if (connectionTicks_ > 600)
        disconnect("Login timed out");
      return;
    }
    if (keepAlive_ && connectionTicks_ - keepAliveSent_ > 600) {
      disconnect("Keepalive timed out");
      return;
    }
    if (!keepAlive_ && connectionTicks_ - keepAliveSent_ >= 200) {
      keepAlive_ = connectionTicks_;
      keepAliveSent_ = connectionTicks_;
      Writer out;
      out.i64(*keepAlive_);
      send("keep_alive", out);
    }
    if (connectionTicks_ % 20 == 0) {
      Writer out;
      out.i64(world_.gameTime());
      out.varInt(0);
      send("set_time", out);
    }
    refreshChunkView();
    sendChunkBatch();
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
} // namespace redstone::net
