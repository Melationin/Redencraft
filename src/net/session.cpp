#include "redstone/net/session.hpp"
#include "redstone/fluid_flow.hpp"
#include "redstone/piston.hpp"
#include "redstone/redstone.hpp"
#include "redstone/shapes.hpp"
#include "redstone/storage.hpp"
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
bool isBucketItem(std::string_view name) {
  return name == "minecraft:bucket" || name == "minecraft:water_bucket" ||
         name == "minecraft:lava_bucket";
}
std::string_view placementBlockNameForItem(std::string_view itemName) {
  if (itemName == "minecraft:redstone")
    return "minecraft:redstone_wire";
  return itemName;
}
bool canReplaceForPlacement(const BlockState &state, const ItemStack &held,
                            double hitY, Direction face,
                            bool replacingClickedOnBlock) {
  if (state.block().isSlab()) {
    if (state.getView("type") == "double" || held.id != state.block().name())
      return false;
    if (!replacingClickedOnBlock)
      return true;
    const bool above = hitY > 0.5;
    const bool bottomSlab = state.getView("type") == "bottom";
    const bool horizontal = face == Direction::North ||
                            face == Direction::South ||
                            face == Direction::East || face == Direction::West;
    return bottomSlab ? (face == Direction::Up || (above && horizontal))
                      : (face == Direction::Down || (!above && horizontal));
  }
  return isReplaceable(state);
}
bool isAxe(std::string_view name) {
  return name == "minecraft:wooden_axe" || name == "minecraft:copper_axe" ||
         name == "minecraft:stone_axe" || name == "minecraft:golden_axe" ||
         name == "minecraft:iron_axe" || name == "minecraft:diamond_axe" ||
         name == "minecraft:netherite_axe";
}
const Block *strippedLog(const BlockState &state) {
  if (state.is(Blocks::OAK_LOG)) return &Blocks::STRIPPED_OAK_LOG;
  if (state.is(Blocks::SPRUCE_LOG)) return &Blocks::STRIPPED_SPRUCE_LOG;
  if (state.is(Blocks::BIRCH_LOG)) return &Blocks::STRIPPED_BIRCH_LOG;
  if (state.is(Blocks::JUNGLE_LOG)) return &Blocks::STRIPPED_JUNGLE_LOG;
  if (state.is(Blocks::ACACIA_LOG)) return &Blocks::STRIPPED_ACACIA_LOG;
  if (state.is(Blocks::CHERRY_LOG)) return &Blocks::STRIPPED_CHERRY_LOG;
  if (state.is(Blocks::DARK_OAK_LOG)) return &Blocks::STRIPPED_DARK_OAK_LOG;
  if (state.is(Blocks::PALE_OAK_LOG)) return &Blocks::STRIPPED_PALE_OAK_LOG;
  if (state.is(Blocks::MANGROVE_LOG)) return &Blocks::STRIPPED_MANGROVE_LOG;
  if (state.is(Blocks::BAMBOO_BLOCK)) return &Blocks::STRIPPED_BAMBOO_BLOCK;
  if (state.is(Blocks::OAK_WOOD)) return &Blocks::STRIPPED_OAK_WOOD;
  if (state.is(Blocks::SPRUCE_WOOD)) return &Blocks::STRIPPED_SPRUCE_WOOD;
  if (state.is(Blocks::BIRCH_WOOD)) return &Blocks::STRIPPED_BIRCH_WOOD;
  if (state.is(Blocks::JUNGLE_WOOD)) return &Blocks::STRIPPED_JUNGLE_WOOD;
  if (state.is(Blocks::ACACIA_WOOD)) return &Blocks::STRIPPED_ACACIA_WOOD;
  if (state.is(Blocks::CHERRY_WOOD)) return &Blocks::STRIPPED_CHERRY_WOOD;
  if (state.is(Blocks::DARK_OAK_WOOD)) return &Blocks::STRIPPED_DARK_OAK_WOOD;
  if (state.is(Blocks::PALE_OAK_WOOD)) return &Blocks::STRIPPED_PALE_OAK_WOOD;
  if (state.is(Blocks::MANGROVE_WOOD)) return &Blocks::STRIPPED_MANGROVE_WOOD;
  return nullptr;
}
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

// Keep all non-block registry tags from the original 26.1 packet. Replace the
// block registry's payload using IDs from blocks.tsv, never local IDs.
Bytes replaceBlockTagPacket(const Bytes &original, const ProtocolData &data,
                            const BlockTagRegistry &tags, int packetId) {
  auto copy = original;
  auto body = popFrame(copy);
  if (!body || !copy.empty()) throw ProtocolError("invalid update_tags frame");
  Reader in(*body);
  (void)in.varInt();
  const int count = in.varInt();
  if (count < 0 || count > 10000) throw ProtocolError("invalid tag registry count");
  std::vector<Bytes> other;
  bool foundBlock = false;
  for (int i = 0; i < count; ++i) {
    const auto start = in.pos;
    const auto registry = in.string();
    const int tagCount = in.varInt();
    if (tagCount < 0 || tagCount > 100000) throw ProtocolError("invalid tag count");
    for (int t = 0; t < tagCount; ++t) {
      (void)in.string();
      const int members = in.varInt();
      if (members < 0 || members > 100000) throw ProtocolError("invalid tag member count");
      for (int j = 0; j < members; ++j) (void)in.varInt();
    }
    if (registry == "minecraft:block") {
      if (foundBlock) throw ProtocolError("duplicate block tag registry");
      foundBlock = true;
    } else {
      other.emplace_back(body->begin() + start, body->begin() + in.pos);
    }
  }
  if (!in.empty()) throw ProtocolError("trailing update_tags payload");
  Writer out;
  out.varInt(static_cast<int>(other.size()) + 1);
  for (const auto &entry : other) out.raw(entry);
  out.string("minecraft:block");
  out.varInt(static_cast<int>(tags.entries().size()));
  for (const auto &[name, members] : tags.entries()) {
    out.string(name);
    out.varInt(static_cast<int>(members.size()));
    for (const auto &member : members) {
      const auto found = data.blockIds.find(member);
      if (found == data.blockIds.end()) throw ProtocolError("unknown block in tag: " + member);
      out.varInt(found->second);
    }
  }
  return frame(packetId, out.data);
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
    if (f.size() != 3)
      continue;
    if (f[0] == "minecraft:dimension_type") {
      const int id = std::stoi(f[1]);
      const std::string normalized = normalizeDimensionId(f[2]);
      dimensionTypeIds.emplace(normalized, id);
      if (normalized == "minecraft:overworld") {
        dimensionId = id;
        overworld = true;
      }
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
  const auto registryDataId = packetIds.find(
      key("configuration", "clientbound", "registry_data"));
  if (registryDataId == packetIds.end())
    throw ProtocolError("missing registry_data packet ID");
  bool worldClockRegistryFrame = false;
  bool timelineRegistryFrame = false;
  Reader reader(bytes);
  while (!reader.empty()) {
    int length = reader.i32();
    if (length <= 0 || length > static_cast<int>(maxFrameSize))
      throw ProtocolError("invalid configuration packet length");
    auto body = reader.raw(length);
    Reader packetReader(body);
    int id = packetReader.varInt();
    if (id == registryDataId->second) {
      Reader registry(body.subspan(packetReader.pos));
      const std::string registryName = registry.string();
      if (registryName == "minecraft:timeline") {
        if (timelineRegistryFrame)
          throw ProtocolError("duplicate timeline registry_data frame");
        timelineRegistryFrame = true;
      }
      if (registryName == "minecraft:world_clock") {
        if (worldClockRegistryFrame)
          throw ProtocolError("duplicate world_clock registry_data frame");
        worldClockRegistryFrame = true;
        const int count = registry.varInt();
        if (count != 2)
          throw ProtocolError("world_clock registry must contain two entries");
        std::array<bool, 2> seen{};
        for (int holderId = 0; holderId < count; ++holderId) {
          const std::string entry = registry.string();
          int target = -1;
          if (entry == "minecraft:overworld")
            target = 0;
          else if (entry == "minecraft:the_end")
            target = 1;
          else
            throw ProtocolError("unsupported world_clock registry entry: " +
                                entry);
          if (seen[target])
            throw ProtocolError("duplicate world_clock registry entry: " +
                                entry);
          seen[target] = true;
          // The current official WorldClock.DIRECT_CODEC is an empty compound.
          // Optional<Tag> is present and encoded as TAG_Compound followed by
          // its immediate TAG_End. Reject richer profiles instead of silently
          // assigning an ID for a codec this server does not implement.
          if (!registry.bool_() || registry.u8() != 10 ||
              registry.u8() != 0)
            throw ProtocolError("unsupported world_clock registry payload");
          worldClockIds[target] = holderId;
        }
        if (!seen[0] || !seen[1] || !registry.empty())
          throw ProtocolError("invalid world_clock registry frame");
      }
    }
    configuration.push_back(frame(id, body.subspan(packetReader.pos)));
  }
  if (!worldClockRegistryFrame)
    throw ProtocolError(
        "configuration is missing the world_clock registry frame");
  if (!timelineRegistryFrame)
    throw ProtocolError(
        "configuration is missing the timeline registry frame");
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

int ProtocolData::dimensionTypeId(std::string_view id) const {
  if (!id.empty()) {
    try {
      const std::string normalized = normalizeDimensionId(id);
      if (auto it = dimensionTypeIds.find(normalized);
          it != dimensionTypeIds.end()) {
        return it->second;
      }
    } catch (const std::invalid_argument &) {
    }
  }
  return dimensionId;
}

Session::Session(World &world, const ProtocolData &data, ServerOptions options)
    : world_(&world), data_(data), options_(std::move(options)),
      currentDimensionId_(options_.dimensionId.empty()
                                ? world.dimensionId()
                                : normalizeDimensionId(options_.dimensionId)),
      currentDimensionTypeId_(
          options_.dimensionTypeId.empty()
              ? (world.dimension().id.empty() ? currentDimensionId_
                                              : world.dimension().id)
              : normalizeDimensionId(options_.dimensionTypeId)),
      position_(options_.spawn) {
  options_.viewDistance = std::clamp(options_.viewDistance, 2, maxViewDistance);
  availableChunks_.insert(options_.chunks.begin(), options_.chunks.end());
  if (availableChunks_.size() > 64)
    chunksPerBatch_ = chunkBatchLimit_ = 2;
}
void Session::setWorldClockSource(const WorldClocks &clocks,
                                  bool advanceTime) {
  worldClocks_ = &clocks;
  advanceTime_ = advanceTime;
}
void Session::writeWorldClock(Writer &out,
                              BuiltinWorldClock clock) const {
  const std::size_t index = static_cast<std::size_t>(clock);
  const WorldClockState &state = worldClocks_->state(clock);
  // ClientboundSetTimePacket / ClockNetworkState: registry holder identity,
  // VarLong total, partial, then the effective rate. A paused clock and the
  // global advance_time rule both advertise zero; tick-freeze is represented
  // separately by the existing ticking-state packet and does not change this.
  out.varInt(data_.worldClockIds[index]);
  out.varLong(state.totalTicks);
  out.f32(state.partialTick);
  out.f32(state.paused || !advanceTime_ ? 0.0f : state.rate);
}
void Session::sendSetTime() {
  Writer out;
  out.i64(world_->gameTime());
  if (!worldClocks_) {
    out.varInt(0);
    send("set_time", std::move(out));
    return;
  }
  out.varInt(2);
  writeWorldClock(out, BuiltinWorldClock::Overworld);
  writeWorldClock(out, BuiltinWorldClock::TheEnd);
  send("set_time", std::move(out));
}
void Session::sendWorldClockUpdate(BuiltinWorldClock clock) {
  if (!worldClocks_ || phase_ != Phase::Play)
    return;
  Writer out;
  out.i64(world_->gameTime());
  out.varInt(1);
  writeWorldClock(out, clock);
  send("set_time", std::move(out));
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
                            std::size_t retainedBytes,
                            std::function<bool()> stillValid) {
  enqueue(OutPacket{{}, -1, std::move(producer), retainedBytes,
                    std::move(stillValid)});
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
    if (item.stillValid && !item.stillValid())
      continue;
    Bytes produced = item.deferred ? item.deferred()
                      : item.packetId >= 0 ? frame(item.packetId, item.bytes)
                                           : std::move(item.bytes);
    if (!produced.empty())
      packets.push_back(std::move(produced));
  }
  return packets;
}
void Session::disconnect(const std::string &reason) {
  if (closed())
    return;
  closeOpenContainer(false);
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
      for (const auto &bytes : data_.configuration) {
        auto copy = bytes;
        const auto body = popFrame(copy);
        if (!body) throw ProtocolError("invalid configuration frame");
        Reader packet(*body);
        const int packetId = packet.varInt();
        if (world_->customBlockTags() && packetId == data_.packet("configuration", "clientbound", "update_tags"))
          queue(replaceBlockTagPacket(bytes, data_, *world_->customBlockTags(), packetId));
        else
          queue(bytes);
      }
      configuredBlockTagRevision_ = world_->blockTagRevision();
      send("finish_configuration");
      finishSent_ = true;
    } else if (id == 3) {
      if (!finishSent_ || !in.empty())
        throw ProtocolError("early configuration finish");
      phase_ = Phase::Play;
      syncManagedResidency();
      join();
      if (configuredBlockTagRevision_ != world_->blockTagRevision())
        sendBlockTagUpdate();
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
void Session::writeCommonPlayerSpawnInfo(Writer &out, const std::string &dimId,
                                         int dimTypeRegistryId,
                                         std::int64_t hashedSeed, bool isFlat,
                                         int seaLevel) const {
  out.varInt(dimTypeRegistryId);
  out.string(dimId);
  out.i64(hashedSeed);
  out.u8(1);
  out.u8(255);
  out.bool_(false);
  out.bool_(isFlat);
  out.bool_(false);
  out.varInt(0);
  out.varInt(seaLevel);
}

void Session::resetDimensionViewState() {
  for (auto &[pos, token] : chunkGenerations_)
    if (token)
      token->store(false, std::memory_order_release);
  chunkGenerations_.clear();
  trackedEntities_.clear();
  changedBlocks_.clear();
  sentChunks_.clear();
  pendingChunks_.clear();
  awaitingLightChunks_.clear();
  availableChunks_.clear();
  managedPendingResidency_.clear();
  sentChunkBlockLight_.clear();
  sentChunkSkyLight_.clear();
  chunkCenter_.reset();
  awaitingChunkBatch_ = false;
  joinPositionPending_ = false;
}

void Session::join() {
  const int height = world_->maxY() - world_->minY();
  if (height < kDimensionMinHeight || (height & 15) != 0 ||
      (world_->minY() & 15) != 0)
    throw ProtocolError("network dimension height must be section-aligned");
  Writer out;
  out.i32(options_.playerId);
  out.bool_(false);
  std::vector<std::string> dims;
  dims.reserve(options_.availableDimensions.size() + 1);
  bool hasCurrent = false;
  for (const auto &raw : options_.availableDimensions) {
    const std::string normalized = normalizeDimensionId(raw);
    if (std::find(dims.begin(), dims.end(), normalized) == dims.end())
      dims.push_back(normalized);
    if (normalized == currentDimensionId_)
      hasCurrent = true;
  }
  if (!hasCurrent)
    dims.insert(dims.begin(), currentDimensionId_);
  out.varInt(static_cast<int>(dims.size()));
  for (const auto &dim : dims)
    out.string(dim);
  out.varInt(8);
  out.varInt(options_.viewDistance);
  out.varInt(options_.viewDistance);
  out.bool_(false);
  out.bool_(true);
  out.bool_(false);
  writeCommonPlayerSpawnInfo(
      out, currentDimensionId_, data_.dimensionTypeId(currentDimensionTypeId_),
      options_.hashedSeed, options_.isFlat, options_.seaLevel);
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
  out.string(currentDimensionId_);
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
  if (awaitingLightChunks_.empty())
    finishJoin();
  else
    joinPositionPending_ = true;
}
void Session::finishJoin() {
  joinPositionPending_ = false;
  respawning_ = false;
  Writer out;
  sendSetTime();
  constexpr const char *initial[]{
      "smooth_stone",        "sticky_piston",  "slime_block",
      "white_carpet",        "redstone_block", "observer",
      "dead_tube_coral_fan", "honey_block",    "tnt"};
  for (int i = 0; i < 9; ++i)
    setSlot(i, itemStack(initial[i], 64));
  out = {};
  out.varInt(pendingTeleportId_);
  positionPacket(out, position_, {}, yaw_, pitch_);
  send("player_position", out);
  broadcastEntities();
  // TODO: recipes, commands, achievements, player avatars and
  // player physics/pushes. This endpoint is a creative editor of loaded chunks.
}

void Session::finishRespawn() {
  joinPositionPending_ = false;
  respawning_ = false;
  Writer out;
  out.varInt(pendingTeleportId_);
  positionPacket(out, position_, {}, yaw_, pitch_);
  send("player_position", out);
  broadcastEntities();
}

void Session::switchDimension(World &targetWorld, const DimensionStem &stem,
                              Vec3 spawnPos, float yaw, float pitch) {
  const int height = targetWorld.maxY() - targetWorld.minY();
  if (height < kDimensionMinHeight || (height & 15) != 0 ||
      (targetWorld.minY() & 15) != 0)
    throw ProtocolError("network dimension height must be section-aligned");
  if (openContainer_)
    closeOpenContainer(true);
  resetDimensionViewState();
  world_ = &targetWorld;
  currentDimensionId_ =
      stem.id.empty() ? targetWorld.dimensionId() : normalizeDimensionId(stem.id);
  currentDimensionTypeId_ = !stem.type.id.empty()
                                ? normalizeDimensionId(stem.type.id)
                                : (!stem.typeId.empty()
                                       ? normalizeDimensionId(stem.typeId)
                                       : normalizeDimensionId(targetWorld.dimension().id));
  position_ = spawnPos;
  yaw_ = yaw;
  pitch_ = pitch;
  teleportPending_ = true;
  ++pendingTeleportId_;
  respawning_ = true;
  if (phase_ != Phase::Play)
    return;
  if (world_->customBlockTags() &&
      configuredBlockTagRevision_ != world_->blockTagRevision()) {
    sendBlockTagUpdate();
    configuredBlockTagRevision_ = world_->blockTagRevision();
  }
  Writer out;
  writeCommonPlayerSpawnInfo(
      out, currentDimensionId_, data_.dimensionTypeId(currentDimensionTypeId_),
      options_.hashedSeed, options_.isFlat, options_.seaLevel);
  out.u8(3); // ClientboundRespawnPacket.KEEP_ALL_DATA
  send("respawn", out);
  out = {};
  out.string(currentDimensionId_);
  out.position({int(std::floor(position_.x)), int(std::floor(position_.y)),
                int(std::floor(position_.z))});
  out.f32(0);
  out.f32(0);
  send("set_default_spawn_position", out);
  out = {};
  out.u8(15);
  out.f32(0.05f);
  out.f32(0.1f);
  send("player_abilities", out);
  sendSetTime();
  out = {};
  out.varInt(options_.viewDistance);
  send("set_chunk_cache_radius", out);
  if (managedResidency_) {
    syncManagedResidency();
  } else {
    const int cx0 = chunkCoord(position_.x);
    const int cz0 = chunkCoord(position_.z);
    for (int dx = -options_.viewDistance; dx <= options_.viewDistance; ++dx)
      for (int dz = -options_.viewDistance; dz <= options_.viewDistance; ++dz)
        if (world_->isChunkLoaded(cx0 + dx, cz0 + dz))
          availableChunks_.emplace(cx0 + dx, cz0 + dz);
  }
  refreshChunkView();
  out = {};
  out.u8(13);
  out.f32(0);
  send("game_event", out);
  sendChunkBatch();
  if (awaitingLightChunks_.empty())
    finishRespawn();
  else
    joinPositionPending_ = true;
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
  if (teleportPending_ || !world_->isLoaded(p) || !hasChunk(p))
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
              world_->getBlock(unpackSectionBlock(section, local)));
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
    out.varInt(data_.registry.stateId(world_->getBlock(p)));
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
        (!isSupportedBlock(std::string(placementBlockNameForItem(item.id))) &&
         item.id != "minecraft:minecart" && !isAxe(item.id) &&
         !isBucketItem(item.id) && !isCompostableItem(item.id) &&
         item.id != "minecraft:bone_meal" &&
         item.id != "minecraft:flint_and_steel")) {
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
        world_->isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      dropSpam_ += 20;
      world_->spawnItemAt(position_ + Vec3{0, 1.3, 0}, item.id, item.count);
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
      const auto old = world_->getBlock(p);
      // Creative destruction has no drops, including the counterpart base of
      // a piston head. Keep setBlock's normal update/removal paths.
      if (old.is(Blocks::PISTON_HEAD)) {
        const auto basePos =
            p.relative(opposite(directionFromString(old.get("facing"))));
        const auto base = world_->getBlock(basePos);
        if ((old.get("type", "normal") == "sticky"
                 ? base.is(Blocks::STICKY_PISTON)
                 : base.is(Blocks::PISTON)) &&
            base.get("extended") == "true" &&
            base.get("facing") == old.get("facing"))
          world_->setBlock(basePos, airState());
      }
      // ServerPlayerGameMode.destroyBlock -> Level.removeBlock(pos, false):
      // creative breaking keeps the water of a waterlogged host.
      world_->removeBlock(p);
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
        world_->isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      int count = action == 3 ? item.count : 1;
      world_->spawnItemAt(position_ + Vec3{0, 1.3, 0}, item.id, count);
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
  const auto old = world_->getBlock(clicked);
  const auto held = inventory_[hand ? 40 : selectedSlot_];
  const bool clickedReplaceable =
      canReplaceForPlacement(old, held, hitY, face, true);
  const Pos target = clickedReplaceable ? clicked : clicked.relative(face);
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
    world_->setBlock(clicked, next, 10);
    (void)world_->random.nextFloat();
    // TODO: fence gate sound and BLOCK_OPEN/BLOCK_CLOSE game events.
  } else if (canInteract(clicked) && !sneaking_ &&
      old.is(Blocks::NOTE_BLOCK)) {
    auto tuned = old.with(
        "note", std::to_string((std::stoi(old.get("note", "0")) + 1) % 25));
    world_->setBlock(clicked, tuned, 3);
    if (isAirState(world_->getBlock(clicked.relative(Direction::Up))))
      world_->blockEvent(clicked, old.name(), 0, 0);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::REDSTONE_WIRE) &&
             redstoneWireUse(*world_, clicked, old)) {
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::REPEATER)) {
    repeaterUse(*world_, clicked, old);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::COMPARATOR)) {
    comparatorUse(*world_, clicked, old);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::LEVER)) {
    leverUse(*world_, clicked, old);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.block().isButton()) {
    buttonUse(*world_, clicked, old);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::COMPOSTER)) {
    const int level = std::stoi(std::string(old.getView("level", "0")));
    if (level == 8) {
      world_->setBlock(clicked, old.with("level", "0"), 3);
      world_->spawnItemAt(
          {clicked.x + 0.5, clicked.y + 1.01, clicked.z + 0.5},
          "minecraft:bone_meal", 1);
    } else if (level < 7 && !held.empty() && isCompostableItem(held.id)) {
      const float chance = composterChance(held.id);
      if ((level == 0 && chance > 0.0f) || world_->random.nextDouble() < chance) {
        const int nextLevel = level + 1;
        world_->setBlock(
            clicked, old.with("level", std::to_string(nextLevel)), 3);
        if (nextLevel == 7)
          world_->scheduleTick(clicked, Blocks::COMPOSTER, 20);
      }
    }
  } else if (canInteract(clicked) && !sneaking_ &&
             containerKindForState(old).has_value()) {
    openContainerScreen(clicked, old);
  } else if (canInteract(clicked) && isAxe(held.id) && strippedLog(old)) {
    // AxeItem.getStripped retains the source axis; this endpoint is creative.
    const auto stripped = strippedLog(old)->defaultState().with(
        "axis", old.getView("axis"));
    world_->setBlock(clicked, stripped, 11);
    // TODO: axe sound, block-change game event and advancement criterion.
  } else if (canInteract(clicked) && !held.empty() &&
             held.id == "minecraft:minecart") {
    if (isRailBlock(old)) {
      const double raised = railShapeIsSlope(railShapeOf(old)) ? 0.5 : 0;
      world_->spawnMinecart(
          {clicked.x + 0.5, clicked.y + 0.0625 + raised, clicked.z + 0.5});
    }
  } else if (canInteract(clicked) && !held.empty() && isBucketItem(held.id)) {
    // BucketItem uses server-side raycast on use_item, not block placement.
  } else if (canInteract(clicked) && canInteract(target) && !held.empty() &&
             canReplaceForPlacement(world_->getBlock(target), held, hitY, face,
                                    clickedReplaceable)) {
    const std::string placeBlockId(placementBlockNameForItem(held.id));
    if (isSupportedBlock(placeBlockId) && placeBlockId != "minecraft:air" &&
        placeBlockId != "minecraft:water" && placeBlockId != "minecraft:lava") {
      auto state = defaultState(placeBlockId);
      bool valid = true;
      bool isDoubleSlab = false;
      if (state.is(Blocks::REDSTONE_WIRE)) {
        state = redstoneWirePlacementState(*world_, target);
        valid = redstoneWireCanSurvive(*world_, target);
      } else if (state.is(Blocks::REDSTONE_TORCH)) {
        auto directions = lookingDirections(yaw_, pitch_);
        if (target != clicked) {
          auto it =
              std::find(directions.begin(), directions.end(), opposite(face));
          std::rotate(directions.begin(), it, it + 1);
        }
        valid = false;
        for (Direction d : directions) {
          if (d == Direction::Up)
            continue;
          if (d == Direction::Down) {
            if (redstoneTorchCanSurvive(*world_, target)) {
              state = Blocks::REDSTONE_TORCH.defaultState();
              valid = true;
              break;
            }
          } else {
            const Direction facing = opposite(d);
            if (redstoneWallTorchCanSurvive(*world_, target, facing)) {
              state = Blocks::REDSTONE_WALL_TORCH.defaultState().with(
                  "facing", directionName(facing));
              valid = true;
              break;
            }
          }
        }
      } else if (state.is(Blocks::LEVER) || state.block().isButton()) {
        auto directions = lookingDirections(yaw_, pitch_);
        if (target != clicked) {
          auto it =
              std::find(directions.begin(), directions.end(), opposite(face));
          std::rotate(directions.begin(), it, it + 1);
        }
        valid = false;
        const auto base = state;
        for (Direction d : directions) {
          BlockState candidate;
          if (d == Direction::Up || d == Direction::Down) {
            candidate =
                base.with("face", d == Direction::Up ? "ceiling" : "floor")
                    .with("facing", directionName(horizontal(yaw_)));
          } else {
            candidate = base.with("face", "wall")
                            .with("facing", directionName(opposite(d)));
          }
          if (leverCanAttach(*world_, target, candidate)) {
            state = candidate;
            valid = true;
            break;
          }
        }
      } else if (state.is(Blocks::REPEATER)) {
        state = repeaterPlacementState(*world_, target, horizontal(yaw_));
        valid = diodeCanSurvive(*world_, target);
      } else if (state.is(Blocks::COMPARATOR)) {
        state = comparatorPlacementState(horizontal(yaw_));
        valid = diodeCanSurvive(*world_, target);
      } else if (isDeadCoralFan(state)) {
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
            wall = world_->coralPlacementState(target, state, opposite(d));
            if (wall)
              break;
          }
        valid = false;
        for (Direction d : directions) {
          if (d == Direction::Up)
            continue;
          auto candidate =
              d == Direction::Down
                  ? world_->coralPlacementState(target, state, Direction::Up)
                  : wall;
          if (candidate) {
            state = *candidate;
            valid = true;
            break;
          }
        }
      } else if (state.block().isSlab()) {
        const auto existing = world_->getBlock(target);
        if (existing.is(state.block())) {
          // SlabBlock.getStateForPlacement: merging into a single slab drops
          // waterlogging (double slabs cannot be waterlogged).
          state = existing.with("type", "double").with("waterlogged", "false");
          isDoubleSlab = true;
        } else {
          const bool top =
              face == Direction::Down ||
              (face != Direction::Up && static_cast<double>(hitY) > 0.5);
          state = state.with("type", top ? "top" : "bottom");
        }
      } else if (state.block().isStairs()) {
        const bool top =
            face == Direction::Down ||
            (face != Direction::Up && static_cast<double>(hitY) > 0.5);
        state = state.with("facing", directionName(horizontal(yaw_)))
                     .with("half", top ? "top" : "bottom");
      } else if (isFenceGate(state)) {
        const bool powered = world_->hasNeighborSignal(target);
        state = state.with("facing", directionName(horizontal(yaw_)))
                     .with("open", powered ? "true" : "false")
                     .with("powered", powered ? "true" : "false");
        state = world_->updateFromNeighbourShapes(target, state);
      } else if (state.block().isRotatedPillar()) {
        // RotatedPillarBlock.getStateForPlacement uses the clicked face axis.
        state = state.with("axis", face == Direction::Up || face == Direction::Down
                                       ? "y"
                                       : face == Direction::East || face == Direction::West
                                             ? "x" : "z");
      } else if (isAnyChestBlock(state)) {
        state = chestPlacementState(*world_, target, state, horizontal(yaw_),
                                    face, sneaking_);
      } else if (state.is(Blocks::BARREL)) {
        state = barrelPlacementState(state, lookingDirections(yaw_, pitch_)[0]);
      } else if (state.is(Blocks::DISPENSER) || state.is(Blocks::DROPPER)) {
        state =
            dispenserPlacementState(state, lookingDirections(yaw_, pitch_)[0]);
      } else if (state.is(Blocks::HOPPER)) {
        state = hopperPlacementState(*world_, target, state, face);
      } else if (state.hasProperty("facing")) {
        const auto dir3d = lookingDirections(yaw_, pitch_)[0];
        const auto candidate =
            state.is(Blocks::OBSERVER) ? dir3d : opposite(dir3d);
        const auto *facingProp = state.block().stateDefinition().find("facing");
        std::uint32_t idx = 0;
        const auto finalDir =
            (facingProp && facingProp->indexOf(directionName(candidate), idx))
                ? candidate
                : (state.is(Blocks::OBSERVER) ? horizontal(yaw_)
                                              : opposite(horizontal(yaw_)));
        state = state.with("facing", directionName(finalDir));
      } else if (isRailBlock(state)) {
        const auto look = horizontal(yaw_);
        state = state.with("shape",
                           (look == Direction::East || look == Direction::West)
                               ? "east_west"
                               : "north_south");
      }
      if (state.block().isPressurePlate())
        valid = pressurePlateCanSurvive(*world_, target);
      if (isWoolCarpet(state))
        valid = !isAirState(world_->getBlock(target.relative(Direction::Down)));
      if (isRailBlock(state))
        valid = railCanSurvive(*world_, target, state);
      // getStateForPlacement initializes these neighbor-derived properties.
      // Do not call observer.updateShape speculatively: it schedules a pulse
      // before the observer even exists in the world.
      if (isWallBlock(state) || state.block().isStairs() ||
          state.block().isFence() || isIronBarsBlock(state) ||
          state.is(Blocks::GRASS_BLOCK) || state.is(Blocks::MYCELIUM) ||
          state.is(Blocks::PODZOL) || state.is(Blocks::NOTE_BLOCK))
        state = world_->updateFromNeighbourShapes(target, state);
      // H-01: getStateForPlacement reads the fluid of the cell being replaced,
      // and the two host families ask different questions. BaseRailBlock
      // .java:139-147 and WallBlock.java:112-131 test the exact WATER source;
      // BaseCoralPlantTypeBlock.java:57-62 (inherited by BaseCoralWallFanBlock)
      // tests FluidTags.WATER plus isFull(), so a falling water[level=8] still
      // waterlogs the fan. A full water source lands waterlogged either way.
      if (state.hasProperty("waterlogged") && !isDoubleSlab) {
        // BlockPlaceContext.getClickedPos() is the CELL BEING REPLACED, which
        // is `target`, not the face that was clicked on (BlockPlaceContext
        // .java:48-51). Water is replaceable, so the two coincide exactly for
        // the water case this rule cares about.
        const auto replacedFluid = fluidStateOf(world_->getBlock(target));
        const bool wet =
            isDeadCoralFan(state)
                ? (fluidHasTag(replacedFluid.getType(), FluidTag::Water) &&
                   replacedFluid.isFull())
                : replacedFluid.is(Fluids::water());
        state = state.with("waterlogged", wet ? "true" : "false");
      }
      if (state.is(Blocks::REDSTONE_LAMP))
        state = state.with("lit",
                           world_->hasNeighborSignal(target) ? "true" : "false");
      if (valid) {
        const Aabb player{position_.x - .3,  position_.y,
                          position_.z - .3,  position_.x + .3,
                          position_.y + 1.8, position_.z + .3};
        for (const auto &box : collisionShape(*world_, target, state)) {
          const Aabb placed{target.x + box.x0 / 16, target.y + box.y0 / 16,
                            target.z + box.z0 / 16, target.x + box.x1 / 16,
                            target.y + box.y1 / 16, target.z + box.z1 / 16};
          if (placed.intersects(player))
            valid = false;
          for (Entity *entity : world_->entitiesIn(placed))
            if (entity->typeName() == "minecraft:tnt" ||
                entity->typeName() == "minecraft:falling_block" ||
                entity->typeName() == "minecraft:minecart")
              valid = false;
        }
        if (valid && !isAirState(state)) {
          world_->setBlock(target, state, 11);
          if (state.is(Blocks::REPEATER) || state.is(Blocks::COMPARATOR))
            diodeSetPlacedBy(*world_, target, state);
        }
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

namespace {
struct PovHit {
  bool hit = false;
  Pos pos{};
  Direction face = Direction::Up;
};

void clipAabb(double minX, double minY, double minZ, double maxX, double maxY,
              double maxZ, Vec3 from, double dx, double dy, double dz,
              double &scale, std::optional<Direction> &dir) {
  auto clipPoint = [&](double da, double db, double dc, double point,
                       double minB, double maxB, double minC, double maxC,
                       Direction newDir, double fromA, double fromB,
                       double fromC) {
    const double s = (point - fromA) / da;
    const double pb = fromB + s * db;
    const double pc = fromC + s * dc;
    if (0.0 < s && s < scale && minB - 1.0e-7 < pb && pb < maxB + 1.0e-7 &&
        minC - 1.0e-7 < pc && pc < maxC + 1.0e-7) {
      scale = s;
      dir = newDir;
    }
  };
  if (dx > 1.0e-7)
    clipPoint(dx, dy, dz, minX, minY, maxY, minZ, maxZ, Direction::West, from.x,
              from.y, from.z);
  else if (dx < -1.0e-7)
    clipPoint(dx, dy, dz, maxX, minY, maxY, minZ, maxZ, Direction::East, from.x,
              from.y, from.z);
  if (dy > 1.0e-7)
    clipPoint(dy, dz, dx, minY, minZ, maxZ, minX, maxX, Direction::Down, from.y,
              from.z, from.x);
  else if (dy < -1.0e-7)
    clipPoint(dy, dz, dx, maxY, minZ, maxZ, minX, maxX, Direction::Up, from.y,
              from.z, from.x);
  if (dz > 1.0e-7)
    clipPoint(dz, dx, dy, minZ, minX, maxX, minY, maxY, Direction::North,
              from.z, from.x, from.y);
  else if (dz < -1.0e-7)
    clipPoint(dz, dx, dy, maxZ, minX, maxX, minY, maxY, Direction::South,
              from.z, from.x, from.y);
}

PovHit povRaycast(World &world, Vec3 from, Vec3 to, bool sourceFluids,
                  const std::function<bool(Pos)> &canVisit) {
  const double totalDx = to.x - from.x;
  const double totalDy = to.y - from.y;
  const double totalDz = to.z - from.z;
  if (totalDx * totalDx + totalDy * totalDy + totalDz * totalDz < 1.0e-12)
    return {};
  const auto testCell = [&](Pos pos) -> PovHit {
    if (!canVisit(pos))
      return {};
    const BlockState state = world.getBlock(pos);
    double bestScale = 1.0;
    std::optional<Direction> bestDir;
    for (const ShapeBox &b : outlineShape(state)) {
      clipAabb(pos.x + b.x0 / 16.0, pos.y + b.y0 / 16.0, pos.z + b.z0 / 16.0,
               pos.x + b.x1 / 16.0, pos.y + b.y1 / 16.0, pos.z + b.z1 / 16.0,
               from, totalDx, totalDy, totalDz, bestScale, bestDir);
    }
    if (sourceFluids) {
      const FluidState fluid = fluidStateOf(state);
      if (!fluid.isEmpty() && fluid.isSource()) {
        const float height = fluidHeight(world, pos, fluid);
        clipAabb(pos.x, pos.y, pos.z, pos.x + 1.0, pos.y + height, pos.z + 1.0,
                 from, totalDx, totalDy, totalDz, bestScale, bestDir);
      }
    }
    if (bestDir)
      return {true, pos, *bestDir};
    return {};
  };

  // BlockGetter.traverseBlocks (BlockGetter.java:119-167).
  const double toX = to.x - (-1.0e-7) * totalDx;
  const double toY = to.y - (-1.0e-7) * totalDy;
  const double toZ = to.z - (-1.0e-7) * totalDz;
  const double fromX = from.x + (-1.0e-7) * totalDx;
  const double fromY = from.y + (-1.0e-7) * totalDy;
  const double fromZ = from.z + (-1.0e-7) * totalDz;
  int cx = static_cast<int>(std::floor(fromX));
  int cy = static_cast<int>(std::floor(fromY));
  int cz = static_cast<int>(std::floor(fromZ));
  if (auto hit = testCell({cx, cy, cz}); hit.hit)
    return hit;

  const double dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
  const auto sign = [](double v) { return (v > 0.0) - (v < 0.0); };
  const auto frac = [](double v) { return v - std::floor(v); };
  const int sx = sign(dx), sy = sign(dy), sz = sign(dz);
  const double tDeltaX = sx == 0 ? 1.0e30 : sx / dx;
  const double tDeltaY = sy == 0 ? 1.0e30 : sy / dy;
  const double tDeltaZ = sz == 0 ? 1.0e30 : sz / dz;
  double tX = tDeltaX * (sx > 0 ? 1.0 - frac(fromX) : frac(fromX));
  double tY = tDeltaY * (sy > 0 ? 1.0 - frac(fromY) : frac(fromY));
  double tZ = tDeltaZ * (sz > 0 ? 1.0 - frac(fromZ) : frac(fromZ));

  for (int step = 0; step < 64 && (tX <= 1.0 || tY <= 1.0 || tZ <= 1.0); ++step) {
    if (tX < tY) {
      if (tX < tZ) {
        cx += sx;
        tX += tDeltaX;
      } else {
        cz += sz;
        tZ += tDeltaZ;
      }
    } else if (tY < tZ) {
      cy += sy;
      tY += tDeltaY;
    } else {
      cz += sz;
      tZ += tDeltaZ;
    }
    if (auto hit = testCell({cx, cy, cz}); hit.hit)
      return hit;
  }
  return {};
}
} // namespace

void Session::useItem(Reader &in) {
  const int hand = in.varInt();
  const int sequence = in.varInt();
  const float yaw = in.f32();
  const float pitch = in.f32();
  if (hand < 0 || hand > 1 || sequence < 0 || !in.empty() ||
      !std::isfinite(yaw) || !std::isfinite(pitch))
    throw ProtocolError("invalid item use");
  yaw_ = yaw;
  pitch_ = pitch;
  const auto held = inventory_[hand ? 40 : selectedSlot_];
  if (isBucketItem(held.id)) {
    const double yawRad = -yaw_ * (std::numbers::pi / 180.0);
    const double pitchRad = pitch_ * (std::numbers::pi / 180.0);
    const double cosPitch = std::cos(pitchRad);
    const Vec3 view{std::sin(yawRad) * cosPitch, -std::sin(pitchRad),
                    std::cos(yawRad) * cosPitch};
    const Vec3 eye = position_ + Vec3{0, sneaking_ ? 1.27 : 1.62, 0};
    // Player creative blockInteractionRange = 4.5 + 0.5 = 5.0.
    const Vec3 to = eye + view * 5.0;
    const auto hit = povRaycast(
        *world_, eye, to, held.id == "minecraft:bucket",
        [&](Pos p) { return world_->isLoaded(p) && hasChunk(p); });
    if (hit.hit && canInteract(hit.pos)) {
      if (held.id == "minecraft:bucket") {
        const BlockState state = world_->getBlock(hit.pos);
        std::string taken;
        if (state.block().isLiquidContainer() && state.block().isWaterloggedHost() &&
            waterloggedOf(state)) {
          world_->setBlock(hit.pos,
                          state.with(PropertyKind::Waterlogged, std::uint8_t{0}),
                          3);
          taken = "minecraft:water_bucket";
        } else if (isLiquidBlock(state)) {
          const FluidState fluid = fluidStateOf(state);
          if (fluid.isSource() && !fluid.isEmpty()) {
            taken = fluid.is(Fluids::lava()) ? "minecraft:lava_bucket"
                                              : "minecraft:water_bucket";
            world_->setBlock(hit.pos, airState(), 11);
          }
        }
        if (!taken.empty()) {
          bool present = inventory_[40].id == taken;
          for (int i = 0; i <= 35 && !present; ++i)
            present = inventory_[i].id == taken;
          if (!present) {
            for (int i = 0; i <= 35; ++i) {
              if (inventory_[i].empty()) {
                setSlot(i, itemStack(taken));
                break;
              }
            }
          }
          sendBlockUpdate(hit.pos);
        }
      } else {
        const bool isWater = held.id == "minecraft:water_bucket";
        const BlockState clickedState = world_->getBlock(hit.pos);
        const Pos initialPos = (isWater && clickedState.block().isLiquidContainer())
                                   ? hit.pos
                                   : hit.pos.relative(hit.face);
        const auto tryEmpty = [&](auto &self, Pos p, bool allowFallback) -> bool {
          if (!canInteract(p))
            return false;
          const BlockState s = world_->getBlock(p);
          const bool mayReplace = isReplaceable(s) || !blocksMotion(s);
          const bool canContainer =
              isWater && s.block().isLiquidContainer() &&
              canPlaceLiquid(s, Fluids::water());
          const bool canInside =
              s.isAir() || ((mayReplace || canContainer) && (!sneaking_ || !allowFallback));
          if (!canInside)
            return allowFallback && self(self, hit.pos.relative(hit.face), false);
          if (isWater && s.block().isLiquidContainer() &&
              s.block().isWaterloggedHost()) {
            placeLiquid(*world_, p, s, fluidStateOf(defaultState("water")));
            sendBlockUpdate(p);
            return true;
          }
          if (!s.isAir() && mayReplace && !isLiquidBlock(s))
            world_->updateOrDestroy(p, s, airState(), 3, 512);
          const BlockState fluidBlock =
              defaultState(isWater ? "water" : "lava");
          const bool ok = world_->setBlock(p, fluidBlock, 11) ||
                          fluidStateOf(s).isSource();
          if (ok)
            sendBlockUpdate(p);
          return ok;
        };
        tryEmpty(tryEmpty, initialPos, true);
        sendBlockUpdate(hit.pos);
      }
    }
  }
  acknowledge(sequence);
}

namespace {
void skipHashedStack(Reader &in) {
  if (!in.bool_())
    return;
  (void)in.varInt(); // item id
  (void)in.varInt(); // count
  const int added = in.varInt();
  if (added < 0 || added > 256)
    throw ProtocolError("invalid hashed stack components");
  for (int i = 0; i < added; ++i) {
    (void)in.varInt();
    (void)in.i32();
  }
  const int removed = in.varInt();
  if (removed < 0 || removed > 256)
    throw ProtocolError("invalid hashed stack removals");
  for (int i = 0; i < removed; ++i)
    (void)in.varInt();
}

int containerMenuTypeId(ContainerKind kind, int slotCount) {
  switch (kind) {
  case ContainerKind::Chest:
  case ContainerKind::TrappedChest:
  case ContainerKind::Barrel:
    return slotCount > 27 ? 5 : 2; // generic_9x6 or generic_9x3
  case ContainerKind::Dispenser:
  case ContainerKind::Dropper:
    return 6; // generic_3x3
  case ContainerKind::Hopper:
    return 16; // hopper
  }
  return 2;
}

std::string containerTitle(ContainerKind kind, int slotCount) {
  switch (kind) {
  case ContainerKind::Chest:
    return slotCount > 27 ? "Large Chest" : "Chest";
  case ContainerKind::TrappedChest:
    return slotCount > 27 ? "Large Chest" : "Trapped Chest";
  case ContainerKind::Barrel:
    return "Barrel";
  case ContainerKind::Dispenser:
    return "Dispenser";
  case ContainerKind::Dropper:
    return "Dropper";
  case ContainerKind::Hopper:
    return "Item Hopper";
  }
  return "Container";
}

std::uint64_t viewModSignature(const ResolvedContainerView &view) {
  const std::uint64_t a = view.first ? view.first->modCount : 0;
  const std::uint64_t b = view.second ? view.second->modCount : 0;
  return a ^ (b * 0x9e3779b97f4a7c15ull);
}
} // namespace

void Session::sendContainerContent() {
  if (!openContainer_ || phase_ != Phase::Play)
    return;
  ResolvedContainerView view =
      resolveContainerView(*world_, openContainer_->pos, false);
  if (!view.valid() || view.size() != openContainer_->slotCount) {
    closeOpenContainer(true);
    return;
  }
  const int n = openContainer_->slotCount;
  Writer out;
  out.varInt(openContainer_->containerId);
  out.varInt(openContainer_->stateId);
  out.varInt(n + 36);
  for (int i = 0; i < n; ++i)
    stack(out, view.getItem(i));
  for (int i = 9; i <= 35; ++i)
    stack(out, inventory_[i]);
  for (int i = 0; i <= 8; ++i)
    stack(out, inventory_[i]);
  stack(out, carriedItem_);
  send("container_set_content", std::move(out));
  openContainer_->lastSeenModCount = viewModSignature(view);
}

void Session::openContainerScreen(Pos pos, const BlockState &state) {
  const auto kind = containerKindForState(state);
  if (!kind)
    return;
  ensureContainerBlockEntity(*world_, pos, state);
  ResolvedContainerView view = resolveContainerView(*world_, pos, false);
  if (!view.valid())
    return;
  if (openContainer_)
    closeOpenContainer(false);

  const int slotCount = view.size();
  const int menuId = containerMenuTypeId(*kind, slotCount);
  const int cid = nextContainerId_;
  nextContainerId_ = (nextContainerId_ % 100) + 1;

  OpenContainer opened;
  opened.containerId = cid;
  opened.menuTypeId = menuId;
  opened.pos = pos;
  opened.kind = *kind;
  opened.slotCount = slotCount;
  opened.stateId = 1;
  opened.lastSeenModCount = viewModSignature(view);
  openContainer_ = std::move(opened);

  containerUpdateOpenCount(*world_, pos, state, +1);

  Writer openOut;
  openOut.varInt(cid);
  openOut.varInt(menuId);
  nbtText(openOut, containerTitle(*kind, slotCount));
  send("open_screen", std::move(openOut));
  sendContainerContent();
}

void Session::closeOpenContainer(bool sendPacket) {
  if (!openContainer_)
    return;
  const int cid = openContainer_->containerId;
  const Pos pos = openContainer_->pos;
  openContainer_.reset();
  carriedItem_ = {};
  if (world_->isLoaded(pos)) {
    const BlockState state = world_->getBlock(pos);
    if (containerKindForState(state))
      containerUpdateOpenCount(*world_, pos, state, -1);
  }
  if (sendPacket && phase_ == Phase::Play) {
    Writer out;
    out.varInt(cid);
    send("container_close", std::move(out));
  }
  // Sync player hotbar/main inventory after closing a container menu.
  for (int i = 0; i <= 35; ++i)
    setSlot(i, inventory_[i]);
}

void Session::syncOpenContainer() {
  if (!openContainer_ || phase_ != Phase::Play)
    return;
  if (!world_->isLoaded(openContainer_->pos) || !hasChunk(openContainer_->pos)) {
    closeOpenContainer(true);
    return;
  }
  const BlockState state = world_->getBlock(openContainer_->pos);
  const auto kind = containerKindForState(state);
  if (!kind || *kind != openContainer_->kind) {
    closeOpenContainer(true);
    return;
  }
  const ResolvedContainerView view =
      resolveContainerView(*world_, openContainer_->pos, false);
  if (!view.valid() || view.size() != openContainer_->slotCount) {
    closeOpenContainer(true);
    return;
  }
  const std::uint64_t sig = viewModSignature(view);
  if (sig != openContainer_->lastSeenModCount) {
    ++openContainer_->stateId;
    sendContainerContent();
  }
}

void Session::containerClose(Reader &in) {
  const int cid = in.varInt();
  if (!in.empty())
    throw ProtocolError("invalid container_close");
  if (openContainer_ && cid == openContainer_->containerId)
    closeOpenContainer(false);
}

void Session::containerClick(Reader &in) {
  const int cid = in.varInt();
  (void)in.varInt(); // stateId
  const int slotNum = in.i16();
  const int buttonNum = static_cast<int>(static_cast<std::int8_t>(in.u8()));
  const int input = in.varInt();
  const int changedCount = in.varInt();
  if (changedCount < 0 || changedCount > 128)
    throw ProtocolError("invalid container_click slot count");
  for (int i = 0; i < changedCount; ++i) {
    (void)in.i16();
    skipHashedStack(in);
  }
  skipHashedStack(in); // carried item
  if (!in.empty())
    throw ProtocolError("trailing container_click data");

  if (!openContainer_ || cid != openContainer_->containerId)
    return;
  if (!world_->isLoaded(openContainer_->pos)) {
    closeOpenContainer(true);
    return;
  }
  ResolvedContainerView view =
      resolveContainerView(*world_, openContainer_->pos, false);
  if (!view.valid() || view.size() != openContainer_->slotCount) {
    closeOpenContainer(true);
    return;
  }

  const int n = openContainer_->slotCount;
  const int totalSlots = n + 36;
  bool containerMutated = false;

  auto getMenuSlot = [&](int menuSlot) -> ItemStack {
    if (menuSlot >= 0 && menuSlot < n)
      return view.getItem(menuSlot);
    if (menuSlot >= n && menuSlot < n + 27)
      return inventory_[9 + (menuSlot - n)];
    if (menuSlot >= n + 27 && menuSlot < totalSlots)
      return inventory_[menuSlot - (n + 27)];
    return {};
  };
  auto setMenuSlot = [&](int menuSlot, const ItemStack &val) {
    if (menuSlot >= 0 && menuSlot < n) {
      view.setItem(*world_, menuSlot, val);
      containerMutated = true;
    } else if (menuSlot >= n && menuSlot < n + 27) {
      inventory_[9 + (menuSlot - n)] = val;
    } else if (menuSlot >= n + 27 && menuSlot < totalSlots) {
      inventory_[menuSlot - (n + 27)] = val;
    }
  };
  auto canMerge = [](const ItemStack &a, const ItemStack &b) {
    return !a.empty() && !b.empty() && a.id == b.id &&
           a.count < a.maxStackSize();
  };
  auto insertRange = [&](ItemStack &moving, int startSlot, int endExclusive) {
    for (int s = startSlot; s < endExclusive && !moving.empty(); ++s) {
      ItemStack cur = getMenuSlot(s);
      if (canMerge(cur, moving)) {
        const int add = std::min(moving.count, cur.maxStackSize() - cur.count);
        if (add > 0) {
          cur.count += add;
          moving.count -= add;
          if (moving.count <= 0)
            moving = {};
          setMenuSlot(s, cur);
        }
      }
    }
    for (int s = startSlot; s < endExclusive && !moving.empty(); ++s) {
      if (getMenuSlot(s).empty()) {
        setMenuSlot(s, moving);
        moving = {};
      }
    }
  };

  if (input == 0) { // PICKUP
    if (slotNum == -999 && !carriedItem_.empty() &&
        world_->isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      const int dropCount = (buttonNum == 0) ? carriedItem_.count : 1;
      world_->spawnItemAt(position_ + Vec3{0, 1.3, 0}, carriedItem_.id,
                         dropCount);
      carriedItem_.count -= dropCount;
      if (carriedItem_.count <= 0)
        carriedItem_ = {};
    } else if (slotNum >= 0 && slotNum < totalSlots) {
      ItemStack slotStack = getMenuSlot(slotNum);
      if (buttonNum == 0) {
        if (carriedItem_.empty()) {
          carriedItem_ = slotStack;
          setMenuSlot(slotNum, {});
        } else if (slotStack.empty()) {
          setMenuSlot(slotNum, carriedItem_);
          carriedItem_ = {};
        } else if (canMerge(slotStack, carriedItem_)) {
          const int add =
              std::min(carriedItem_.count,
                       slotStack.maxStackSize() - slotStack.count);
          slotStack.count += add;
          carriedItem_.count -= add;
          if (carriedItem_.count <= 0)
            carriedItem_ = {};
          setMenuSlot(slotNum, slotStack);
        } else {
          setMenuSlot(slotNum, carriedItem_);
          carriedItem_ = slotStack;
        }
      } else if (buttonNum == 1) {
        if (carriedItem_.empty() && !slotStack.empty()) {
          const int take = (slotStack.count + 1) / 2;
          carriedItem_ = itemStack(slotStack.id, take);
          slotStack.count -= take;
          setMenuSlot(slotNum, slotStack.count > 0 ? slotStack : ItemStack{});
        } else if (!carriedItem_.empty()) {
          if (slotStack.empty()) {
            setMenuSlot(slotNum, itemStack(carriedItem_.id, 1));
            if (--carriedItem_.count <= 0)
              carriedItem_ = {};
          } else if (canMerge(slotStack, carriedItem_)) {
            slotStack.count += 1;
            setMenuSlot(slotNum, slotStack);
            if (--carriedItem_.count <= 0)
              carriedItem_ = {};
          } else {
            setMenuSlot(slotNum, carriedItem_);
            carriedItem_ = slotStack;
          }
        }
      }
    }
  } else if (input == 1 && slotNum >= 0 && slotNum < totalSlots) { // QUICK_MOVE
    ItemStack moving = getMenuSlot(slotNum);
    if (!moving.empty()) {
      if (slotNum < n) {
        insertRange(moving, n, totalSlots);
      } else {
        insertRange(moving, 0, n);
      }
      setMenuSlot(slotNum, moving);
    }
  } else if (input == 2 && slotNum >= 0 && slotNum < totalSlots) { // SWAP
    const int invIdx =
        (buttonNum >= 0 && buttonNum <= 8)
            ? buttonNum
            : (buttonNum == 40 ? 40 : -1);
    if (invIdx >= 0) {
      ItemStack a = getMenuSlot(slotNum);
      ItemStack b = inventory_[invIdx];
      inventory_[invIdx] = a;
      setMenuSlot(slotNum, b);
    }
  } else if (input == 3 && slotNum >= 0 && slotNum < totalSlots &&
             carriedItem_.empty()) { // CLONE
    ItemStack s = getMenuSlot(slotNum);
    if (!s.empty())
      carriedItem_ = itemStack(s.id, s.maxStackSize());
  } else if (input == 4 && slotNum >= 0 && slotNum < totalSlots &&
             carriedItem_.empty()) { // THROW
    ItemStack s = getMenuSlot(slotNum);
    if (!s.empty() &&
        world_->isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      const int dropCount = (buttonNum == 1) ? s.count : 1;
      world_->spawnItemAt(position_ + Vec3{0, 1.3, 0}, s.id, dropCount);
      s.count -= dropCount;
      setMenuSlot(slotNum, s.count > 0 ? s : ItemStack{});
    }
  } else if (input == 5) { // QUICK_CRAFT
    const int header = buttonNum & 3;
    if (header == 0) {
      openContainer_->quickCraftMode = 1;
      openContainer_->quickCraftEvent = (buttonNum >> 2) & 3;
      openContainer_->quickCraftSlots.clear();
    } else if (header == 1 && openContainer_->quickCraftMode == 1 &&
               slotNum >= 0 && slotNum < totalSlots) {
      if (std::find(openContainer_->quickCraftSlots.begin(),
                    openContainer_->quickCraftSlots.end(),
                    slotNum) == openContainer_->quickCraftSlots.end())
        openContainer_->quickCraftSlots.push_back(slotNum);
    } else if (header == 2 && openContainer_->quickCraftMode == 1) {
      if (!carriedItem_.empty() && !openContainer_->quickCraftSlots.empty()) {
        std::vector<int> eligible;
        for (int s : openContainer_->quickCraftSlots) {
          ItemStack cur = getMenuSlot(s);
          if (cur.empty() || canMerge(cur, carriedItem_))
            eligible.push_back(s);
        }
        if (!eligible.empty()) {
          const int count = static_cast<int>(eligible.size());
          for (int s : eligible) {
            if (carriedItem_.empty())
              break;
            int add = 0;
            if (openContainer_->quickCraftEvent == 0)
              add = std::max(1, carriedItem_.count / count);
            else if (openContainer_->quickCraftEvent == 1)
              add = 1;
            else
              add = carriedItem_.maxStackSize();
            ItemStack cur = getMenuSlot(s);
            const int cap = cur.empty() ? carriedItem_.maxStackSize()
                                        : cur.maxStackSize() - cur.count;
            const int place =
                openContainer_->quickCraftEvent == 2
                    ? cap
                    : std::min({add, cap, carriedItem_.count});
            if (place > 0) {
              setMenuSlot(s, itemStack(carriedItem_.id,
                                       (cur.empty() ? 0 : cur.count) + place));
              if (openContainer_->quickCraftEvent != 2)
                carriedItem_.count -= place;
            }
          }
          if (openContainer_->quickCraftEvent == 2 || carriedItem_.count <= 0)
            carriedItem_ = {};
        }
      }
      openContainer_->quickCraftMode = -1;
      openContainer_->quickCraftSlots.clear();
    }
  } else if (input == 6 && !carriedItem_.empty()) { // PICKUP_ALL
    for (int pass = 0; pass < 2; ++pass) {
      for (int s = 0;
           s < totalSlots && carriedItem_.count < carriedItem_.maxStackSize();
           ++s) {
        ItemStack cur = getMenuSlot(s);
        if (cur.empty() || cur.id != carriedItem_.id)
          continue;
        if (pass == 0 && cur.count == cur.maxStackSize())
          continue;
        const int take = std::min(cur.count,
                                  carriedItem_.maxStackSize() - carriedItem_.count);
        carriedItem_.count += take;
        cur.count -= take;
        setMenuSlot(s, cur.count > 0 ? cur : ItemStack{});
      }
    }
  }

  if (containerMutated)
    view.setChanged(*world_);
  ++openContainer_->stateId;
  sendContainerContent();
}
void Session::play(int id, Reader &in) {
  const auto is = [&](std::string_view name) {
    return id == data_.packet("play", "serverbound", name);
  };
  if (is("accept_teleportation")) {
    if (in.varInt() == pendingTeleportId_)
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
    auto item = blockItem(world_->getBlock(p));
    if (canInteract(p) && !item.empty())
      setSlot(selectedSlot_, itemStack(item));
  } else if (is("use_item")) {
    useItem(in);
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
  } else if (is("container_click")) {
    containerClick(in);
  } else if (is("container_close")) {
    containerClose(in);
  } else if (is("client_tick_end") || is("player_loaded") ||
             is("player_abilities") || is("client_information") ||
             is("custom_payload") || is("swing") || is("player_command") ||
             is("chat_session_update") || is("pong")) {
    // These packets do not modify this creative editor's simulated blocks.
    // TODO: player actions, chat/session state and sprint.
  } else if (diagnostic)
    diagnostic("TODO: unsupported play packet ignored");
}
bool Session::hasManagedChunk(int chunkX, int chunkZ) const {
  return availableChunks_.contains({chunkX, chunkZ});
}

bool Session::hasChunk(Pos p) const {
  return sentChunks_.contains({chunkCoord(p.x), chunkCoord(p.z)});
}
bool Session::chunkVisible(std::pair<int, int> chunk) const {
  if (!chunkCenter_)
    return false;
  return std::abs(chunk.first - chunkCenter_->first) <= options_.viewDistance &&
         std::abs(chunk.second - chunkCenter_->second) <= options_.viewDistance;
}
void Session::queuePendingChunk(std::pair<int, int> chunk) {
  if (sentChunks_.contains(chunk) || !availableChunks_.contains(chunk) ||
      !chunkVisible(chunk) ||
      !world_->isChunkLoaded(chunk.first, chunk.second) ||
      std::find(pendingChunks_.begin(), pendingChunks_.end(), chunk) !=
          pendingChunks_.end())
    return;
  pendingChunks_.push_back(chunk);
}
void Session::expireChunk(std::pair<int, int> chunk) {
  const auto token = chunkGenerations_.find(chunk);
  if (token != chunkGenerations_.end())
    token->second->store(false, std::memory_order_release);
  chunkGenerations_.erase(chunk);
  sentChunkBlockLight_.erase(chunk);
  sentChunkSkyLight_.erase(chunk);
  awaitingLightChunks_.erase(chunk);
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
    expireChunk(*it);
    it = sentChunks_.erase(it);
  }
  pendingChunks_.clear();
  awaitingLightChunks_.clear();
  for (auto p : availableChunks_)
    if (visible(p) && !sentChunks_.contains(p) &&
        world_->isChunkLoaded(p.first, p.second))
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
  // A chunk can join after the worker completed but before the next world tick
  // drains publications. This is a nonblocking ownership handoff, never a wait
  // for propagation, and keeps the join packet order chunk-before-position.
  if (world_->lightingEnabled() && world_->lightService())
    world_->lightService()->drainPublished();
  if (awaitingChunkBatch_ || pendingChunks_.empty() || closed())
    return;
  if (world_->lightingEnabled()) {
    const auto *service = world_->lightService();
    bool ready = false;
    for (const auto chunk : pendingChunks_) {
      const auto block =
          service->latestLayerChunk(LightLayer::Block, chunk.first, chunk.second);
      const auto sky =
          service->latestLayerChunk(LightLayer::Sky, chunk.first, chunk.second);
      if (block && block->protocolReady &&
          (!block->skyLightEnabled || (sky && sky->protocolReady))) {
        ready = true;
        break;
      }
    }
    if (!ready) {
      for (const auto chunk : pendingChunks_)
        if (world_->isChunkLoaded(chunk.first, chunk.second))
          awaitingLightChunks_.insert(chunk);
      pendingChunks_.clear();
      return;
    }
  }
  send("chunk_batch_start");
  const int chunkPacket =
      data_.packet(phaseName(phase_), "clientbound", "level_chunk_with_light");
  int count = 0;
  std::size_t bytes = 0;
  while (!pendingChunks_.empty() && count < chunksPerBatch_ &&
         bytes < 2 * 1024 * 1024) {
    const auto [x, z] = pendingChunks_.front();
    pendingChunks_.erase(pendingChunks_.begin());
    if (!world_->isChunkLoaded(x, z))
      continue;
    std::shared_ptr<const LightChunkSnapshot> blockLight;
    std::shared_ptr<const LightChunkSnapshot> skyLight;
    if (world_->lightingEnabled()) {
      const auto *service = world_->lightService();
      blockLight = service->latestLayerChunk(LightLayer::Block, x, z);
      skyLight = service->latestLayerChunk(LightLayer::Sky, x, z);
      if (!blockLight || !blockLight->protocolReady ||
          (blockLight->skyLightEnabled &&
           (!skyLight || !skyLight->protocolReady))) {
        awaitingLightChunks_.insert({x, z});
        continue;
      }
    }
    // Snapshot on the tick thread (the world is not thread safe), then let the
    // writer thread run the palette/bit packing. The batch-start ordering is
    // preserved because queue items are drained in order.
    auto snapshot = prepareChunkSnapshot(world_->snapshotChunk(x, z),
                                         data_.registry, blockLight, skyLight);
    for (const auto &section : snapshot.sections)
      bytes += section.palette.size() * sizeof(BlockState::Id) +
               section.indices8.size() + section.indices16.size() * 2 + 64;
    const auto &registry = data_.registry;
    const auto retainedBytes = snapshot.retainedBytes();
    const auto generation = std::make_shared<std::atomic<bool>>(true);
    // A submitted chunk belongs to the FIFO batch counted by chunk_batch_start
    // and chunk_batch_finished. It must not be cancelled independently. A later
    // forget_level_chunk removes it in order; only dynamic light updates use the
    // lifecycle token for cancellation.
    queueDeferred([snapshot = std::move(snapshot), chunkPacket, x, z,
                   &registry]() {
      return frame(chunkPacket, encodeChunk(snapshot, x, z, registry));
    }, retainedBytes);
    if (closed())
      return;
    sentChunks_.emplace(x, z);
    chunkGenerations_[{x, z}] = generation;
    if (blockLight)
      sentChunkBlockLight_[{x, z}] = lightLayerVersion(*blockLight);
    if (skyLight)
      sentChunkSkyLight_[{x, z}] = lightLayerVersion(*skyLight);
    awaitingLightChunks_.erase({x, z});
    ++count;
  }
  Writer out;
  out.varInt(count);
  send("chunk_batch_finished", out);
  awaitingChunkBatch_ = true;
}
void Session::lightPublished(
    std::shared_ptr<const LightChunkSnapshot> snapshot) {
  try {
    if (closed() || phase_ != Phase::Play || !snapshot ||
        !snapshot->protocolReady || !snapshot->layer)
      return;
    const std::pair key{snapshot->chunkX, snapshot->chunkZ};
    if (awaitingLightChunks_.contains(key)) {
      const auto *service = world_->lightService();
      const auto block = service
                             ? service->latestLayerChunk(LightLayer::Block,
                                                         key.first, key.second)
                             : nullptr;
      const auto sky = service
                           ? service->latestLayerChunk(LightLayer::Sky,
                                                       key.first, key.second)
                           : nullptr;
      if (block && block->protocolReady &&
          (!block->skyLightEnabled || (sky && sky->protocolReady))) {
        awaitingLightChunks_.erase(key);
        queuePendingChunk(key);
      }
      return;
    }
    if (!sentChunks_.contains(key))
      return;
    auto &byLayer = *snapshot->layer == LightLayer::Block
                        ? sentChunkBlockLight_
                        : sentChunkSkyLight_;
    auto previous = byLayer.find(key);
    if (previous == byLayer.end())
      return;
    if (snapshot->revision <= previous->second.revision)
      return;
    if (snapshot->lifecycleEpoch != previous->second.lifecycleEpoch ||
        snapshot->chunkX != previous->second.chunkX ||
        snapshot->chunkZ != previous->second.chunkZ)
      throw ProtocolError("stale light layer lifecycle reached the session");
    auto update = snapshot->deltaBaseRevision == previous->second.revision
                      ? prepareLayerLightUpdate(snapshot, *snapshot->layer,
                                               &previous->second)
                      : prepareLayerLightUpdate(snapshot, *snapshot->layer);
    previous->second = lightLayerVersion(*snapshot);
    if (update.sky.dataSections.empty() &&
        update.sky.emptySections.empty() &&
        update.block.dataSections.empty() &&
        update.block.emptySections.empty())
      return;
    const auto generation = chunkGenerations_.find(key);
    if (generation == chunkGenerations_.end())
      return;
    const auto token = generation->second;
    const int packetId =
        data_.packet(phaseName(phase_), "clientbound", "light_update");
    const auto retained = update.retainedBytes();
    queueDeferred([update = std::move(update), packetId, token]() {
      if (!token->load(std::memory_order_acquire))
        return Bytes{};
      return frame(packetId, encodeLightUpdate(update));
    }, retained, [token] {
      return token->load(std::memory_order_acquire);
    });
  } catch (const ProtocolError &) {
    disconnect("Light update failed for this connection");
  }
}
void Session::lightChunkLifecycle(int chunkX, int chunkZ, bool loaded) {
  if (closed() || phase_ != Phase::Play)
    return;
  const std::pair key{chunkX, chunkZ};
  if (!loaded) {
    const bool hadSentChunk = sentChunks_.contains(key);
    expireChunk(key);
    std::erase(pendingChunks_, key);
    awaitingLightChunks_.erase(key);
    if (hadSentChunk) {
      sentChunks_.erase(key);
      try {
        Writer out;
        // FriendlyByteBuf.writeChunkPos = ChunkPos.toLong, x low bits.
        out.i32(chunkZ);
        out.i32(chunkX);
        send("forget_level_chunk", out);
      } catch (const ProtocolError &) {
        disconnect("Explicit chunk unload failed for this connection");
      }
    }
    return;
  }
  if (!availableChunks_.contains(key) || !chunkVisible(key) ||
      !world_->isChunkLoaded(chunkX, chunkZ))
    return;
  const auto *service = world_->lightService();
  const auto block = service
                         ? service->latestLayerChunk(LightLayer::Block,
                                                     chunkX, chunkZ)
                         : nullptr;
  const auto sky = service
                       ? service->latestLayerChunk(LightLayer::Sky, chunkX,
                                                   chunkZ)
                       : nullptr;
  if (block && block->protocolReady &&
      (!block->skyLightEnabled || (sky && sky->protocolReady)))
    queuePendingChunk(key);
  else
    awaitingLightChunks_.insert(key);
}

void Session::setManagedResidencyProvider(
    std::function<bool(int, int)> resident,
    std::function<std::vector<std::pair<int, int>>()> chunks) {
  managedResidency_ = std::move(resident);
  managedChunks_ = std::move(chunks);
  if (closed())
    return;
  if (phase_ != Phase::Play) {
    // Provider installation commonly happens during Handshake. Do not encode
    // Play packets before the configuration transition. The first join() syncs
    // the current resident snapshot.
    return;
  }
  syncManagedResidency();
}

void Session::syncManagedResidency() {
  if (!managedChunks_)
    return;
  std::set<std::pair<int, int>> snapshot;
  for (const auto &chunk : managedChunks_())
    if (!managedResidency_ ||
        managedResidency_(chunk.first, chunk.second))
      snapshot.insert(chunk);
  for (const auto &chunk : managedPendingResidency_)
    snapshot.insert(chunk);
  managedPendingResidency_.clear();
  availableChunks_ = std::move(snapshot);
}

void Session::managedChunkResidency(int chunkX, int chunkZ, bool resident) {
  if (closed())
    return;
  const std::pair key{chunkX, chunkZ};
  if (phase_ != Phase::Play) {
    if (resident)
      managedPendingResidency_.insert(key);
    else
      managedPendingResidency_.erase(key);
    return;
  }
  if (resident) {
    availableChunks_.insert(key);
    if (chunkCenter_ && chunkVisible(key) &&
        world_->isChunkLoaded(chunkX, chunkZ))
      queuePendingChunk(key);
    return;
  }
  availableChunks_.erase(key);
  expireChunk(key);
  std::erase(pendingChunks_, key);
  awaitingLightChunks_.erase(key);
  if (sentChunks_.erase(key) != 0) {
    try {
      Writer out;
      out.i32(chunkZ);
      out.i32(chunkX);
      send("forget_level_chunk", out);
    } catch (const ProtocolError &) {
      disconnect("Explicit chunk unload failed for this connection");
    }
  }
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
  source.permissionLevel = options_.commandPermissionLevel;
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
void Session::sendBlockTagUpdate() {
  if (phase_ != Phase::Play || !world_->customBlockTags()) return;
  for (const auto &bytes : data_.configuration) {
    auto copy = bytes;
    const auto body = popFrame(copy);
    if (!body) throw ProtocolError("invalid configuration frame");
    Reader packet(*body);
    if (packet.varInt() == data_.packet("configuration", "clientbound", "update_tags")) {
      queue(replaceBlockTagPacket(bytes, data_, *world_->customBlockTags(),
                                  data_.packet("play", "clientbound", "update_tags")));
      return;
    }
  }
  throw ProtocolError("missing configuration update_tags");
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
    source.permissionLevel = options_.commandPermissionLevel;
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
    candidates = world_->entitiesInSectionRange(
        chunkCenter_->first - range, chunkCenter_->first + range,
        chunkCenter_->second - range, chunkCenter_->second + range);
  } else {
    candidates.reserve(world_->entities.size());
    for (const auto &ptr : world_->entities)
      candidates.push_back(ptr.get());
  }
  for (const Entity *candidate : candidates) {
    if (!candidate)
      continue;
    const auto &e = *candidate;
    if (e.isRemoved() ||
        !world_->isChunkLoaded(chunkCoord(e.position.x),
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
      !world_->isChunkLoaded(chunkCoord(entity.position.x),
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
    if (connectionTicks_ % 20 == 0)
      sendSetTime();
    refreshChunkView();
    syncOpenContainer();
    sendChunkBatch();
    if (joinPositionPending_ && !sentChunks_.empty()) {
      if (respawning_)
        finishRespawn();
      else
        finishJoin();
    }
  } catch (const ProtocolError &) {
    disconnect("Unsupported entity state for protocol 775");
  }
}
} // namespace redstone::net
