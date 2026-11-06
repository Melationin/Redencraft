#include "redstone/net/session.hpp"
#include "redstone/piston.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
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
void uuid(Writer &out, const Uuid &id) { out.raw(id); }
int chunkCoord(double coordinate) {
  return static_cast<int>(std::floor(coordinate / 16));
}
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
  options_.viewDistance = std::clamp(options_.viewDistance, 2, 8);
  availableChunks_.insert(options_.chunks.begin(), options_.chunks.end());
  if (availableChunks_.size() > 64)
    chunksPerBatch_ = chunkBatchLimit_ = 2;
}
void Session::packet(int id, const Writer &payload) {
  if (!closed())
    queue(frame(id, payload.data));
}
void Session::queue(Bytes bytes) {
  if (closed())
    return;
  if (outputBytes_ + bytes.size() > 16 * 1024 * 1024) {
    output_.clear();
    outputBytes_ = 0;
    error_ = "Connection output backlog exceeded";
    phase_ = Phase::Closed;
    if (diagnostic)
      diagnostic(error_);
    return;
  }
  outputBytes_ += bytes.size();
  output_.push_back(std::move(bytes));
}
void Session::send(std::string_view name, const Writer &payload) {
  if (closed())
    return;
  packet(data_.packet(phaseName(phase_), "clientbound", name), payload);
}
std::vector<Bytes> Session::takeOutput() {
  outputBytes_ = 0;
  return std::exchange(output_, {});
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
  syncEntities();
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
    changedBlocks_.insert(p);
}
void Session::flushBlockUpdates() {
  auto positions = std::exchange(changedBlocks_, {});
  for (Pos p : positions)
    correct(p);
}
void Session::correct(Pos p) {
  if (phase_ != Phase::Play || !hasChunk(p))
    return;
  try {
    Writer out;
    out.position(p);
    out.varInt(data_.registry.stateId(world_.getBlock(p)));
    send("block_update", out);
  } catch (const ProtocolError &) {
    disconnect("Unsupported block state for protocol 775");
  }
}
void Session::blockEvent(const BlockEvent &event) {
  if (phase_ != Phase::Play || !hasChunk(event.pos))
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
  send("block_event", out);
}
void Session::fallingBlockLanded(int entityId, Pos pos, const BlockState &) {
  // FallingBlockEntity.sendToTrackingPlayers precedes discard. Ordinary chunk
  // dirty broadcasts remain in their existing earlier tick phase.
  if (knownEntities_.contains(entityId))
    correct(pos);
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
    correct(p);
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
  if (canInteract(clicked) && !sneaking_ &&
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
  correct(clicked);
  correct(target);
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
  int count = 0;
  std::size_t bytes = 0;
  while (!pendingChunks_.empty() && count < chunksPerBatch_ &&
         bytes < 2 * 1024 * 1024) {
    const auto [x, z] = pendingChunks_.front();
    pendingChunks_.erase(pendingChunks_.begin());
    if (!world_.isChunkLoaded(x, z))
      continue;
    Writer out;
    out.data = encodeChunk(world_, x, z, data_.registry);
    bytes += out.data.size();
    send("level_chunk_with_light", out);
    sentChunks_.emplace(x, z);
    ++count;
  }
  Writer out;
  out.varInt(count);
  send("chunk_batch_finished", out);
  awaitingChunkBatch_ = true;
}
void Session::sendTickTime(double milliseconds, std::int64_t tick) {
  if (phase_ != Phase::Play || !std::isfinite(milliseconds) || milliseconds < 0)
    return;
  try {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << "Tick " << tick << ": " << std::fixed << std::setprecision(3)
         << milliseconds << " ms";
    Writer out;
    // ComponentSerialization accepts a network NBT StringTag for literal text.
    nbtText(out, text.str());
    out.bool_(false); // Ordinary chat, as requested by the user.
    send("system_chat", out);
  } catch (const ProtocolError &) {
    disconnect("Tick timing packet failed");
  }
}
void Session::syncEntities() {
  std::set<int> active;
  for (const auto &ptr : world_.entities) {
    const auto &e = *ptr;
    if (e.isRemoved() ||
        !world_.isChunkLoaded(chunkCoord(e.position.x),
                              chunkCoord(e.position.z)) ||
        !sentChunks_.contains(
            {chunkCoord(e.position.x), chunkCoord(e.position.z)}))
      continue;
    active.insert(e.id);
    Writer out;
    if (!knownEntities_.contains(e.id)) {
      out.varInt(e.id);
      uuid(out, offlineUuid("entity_" + std::to_string(e.id)));
      out.varInt(data_.registry.entityTypeId(e.typeName()));
      out.f64(e.position.x);
      out.f64(e.position.y);
      out.f64(e.position.z);
      lpVector(out, e.deltaMovement);
      out.u8(static_cast<std::uint8_t>(int(std::floor(e.pitch * 256 / 360))));
      out.u8(static_cast<std::uint8_t>(int(std::floor(e.yaw * 256 / 360))));
      out.u8(0);
      const auto *falling = dynamic_cast<const FallingBlockEntity *>(&e);
      out.varInt(falling ? data_.registry.stateId(falling->blockState) : 0);
      send("add_entity", out);
    } else {
      out.varInt(e.id);
      positionPacket(out, e.position, e.deltaMovement, e.yaw, e.pitch);
      out.bool_(e.onGround);
      send("teleport_entity", out);
    }
    out = {};
    out.varInt(e.id);
    if (auto item = dynamic_cast<const ItemEntity *>(&e)) {
      out.u8(8);
      out.varInt(7);
      stack(out, item->item);
    } else if (auto tnt = dynamic_cast<const PrimedTnt *>(&e)) {
      out.u8(8);
      out.varInt(1);
      out.varInt(tnt->fuse);
      out.u8(9);
      out.varInt(14);
      out.varInt(data_.registry.stateId(tnt->blockState));
    }
    out.u8(255);
    send("set_entity_data", out);
  }
  Writer removed;
  int count = 0;
  for (int id : knownEntities_)
    if (!active.contains(id))
      ++count;
  if (count) {
    removed.varInt(count);
    for (int id : knownEntities_)
      if (!active.contains(id))
        removed.varInt(id);
    send("remove_entities", removed);
  }
  knownEntities_ = std::move(active);
  // TODO: vanilla ServerEntity dirty metadata/range/relative interpolation and
  // minecart track interpolation; absolute sync retains authoritative physics.
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
    syncEntities();
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
} // namespace redstone::net
