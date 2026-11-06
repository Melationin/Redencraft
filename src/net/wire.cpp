#include "redstone/net/wire.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <limits>
#include <sstream>

namespace redstone::net {
void Writer::u8(std::uint8_t v) { data.push_back(v); }
void Writer::bool_(bool v) { u8(v ? 1 : 0); }
void Writer::i16(std::int16_t v) {
  auto n = std::bit_cast<std::uint16_t>(v);
  u8(n >> 8);
  u8(n & 255);
}
void Writer::i32(std::int32_t v) {
  auto n = std::bit_cast<std::uint32_t>(v);
  for (int s = 24; s >= 0; s -= 8)
    u8((n >> s) & 255);
}
void Writer::i64(std::int64_t v) {
  auto n = std::bit_cast<std::uint64_t>(v);
  for (int s = 56; s >= 0; s -= 8)
    u8((n >> s) & 255);
}
void Writer::f32(float v) { i32(std::bit_cast<std::int32_t>(v)); }
void Writer::f64(double v) { i64(std::bit_cast<std::int64_t>(v)); }
void Writer::varInt(std::int32_t v) {
  auto n = std::bit_cast<std::uint32_t>(v);
  do {
    auto b = n & 127;
    n >>= 7;
    u8(static_cast<std::uint8_t>(b | (n ? 128 : 0)));
  } while (n);
}
void Writer::string(std::string_view v) {
  if (v.size() > 32767 * 3)
    throw ProtocolError("string too large");
  varInt(static_cast<int>(v.size()));
  raw({reinterpret_cast<const std::uint8_t *>(v.data()), v.size()});
}
void Writer::position(Pos p) {
  auto v = ((std::uint64_t(p.x) & 0x3ffffff) << 38) |
           ((std::uint64_t(p.z) & 0x3ffffff) << 12) |
           (std::uint64_t(p.y) & 0xfff);
  i64(std::bit_cast<std::int64_t>(v));
}
void Writer::raw(std::span<const std::uint8_t> v) {
  data.insert(data.end(), v.begin(), v.end());
}
std::span<const std::uint8_t> Reader::raw(std::size_t n) {
  if (n > remaining())
    throw ProtocolError("truncated packet");
  auto result = data.subspan(pos, n);
  pos += n;
  return result;
}
std::uint8_t Reader::u8() { return raw(1)[0]; }
bool Reader::bool_() { return u8() != 0; }
std::int16_t Reader::i16() {
  auto a = u8();
  auto b = u8();
  return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>((a << 8) | b));
}
std::int32_t Reader::i32() {
  std::uint32_t n = 0;
  for (auto b : raw(4))
    n = (n << 8) | b;
  return std::bit_cast<std::int32_t>(n);
}
std::int64_t Reader::i64() {
  std::uint64_t n = 0;
  for (auto b : raw(8))
    n = (n << 8) | b;
  return std::bit_cast<std::int64_t>(n);
}
float Reader::f32() { return std::bit_cast<float>(i32()); }
double Reader::f64() { return std::bit_cast<double>(i64()); }
std::int32_t Reader::varInt() {
  std::uint32_t n = 0;
  for (int i = 0; i < 5; ++i) {
    auto b = u8();
    if (i == 4 && (b & 0xf0))
      throw ProtocolError("VarInt overflow");
    n |= std::uint32_t(b & 127) << (i * 7);
    if (!(b & 128))
      return std::bit_cast<std::int32_t>(n);
  }
  throw ProtocolError("VarInt too long");
}
std::string Reader::string(std::size_t maxBytes) {
  int n = varInt();
  if (n < 0 || std::size_t(n) > maxBytes)
    throw ProtocolError("invalid string size");
  auto bytes = raw(n);
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}
Pos Reader::position() {
  auto n = std::bit_cast<std::uint64_t>(i64());
  auto sx = [](std::uint32_t v, int bits) {
    auto sign = std::uint32_t(1) << (bits - 1);
    return static_cast<int>((v ^ sign) - sign);
  };
  return {sx(static_cast<std::uint32_t>(n >> 38), 26), sx(n & 0xfff, 12),
          sx((n >> 12) & 0x3ffffff, 26)};
}
Bytes frame(int id, std::span<const std::uint8_t> payload) {
  Writer b;
  b.varInt(id);
  b.raw(payload);
  if (b.data.size() > maxFrameSize)
    throw ProtocolError("frame too large");
  Writer w;
  w.varInt(static_cast<int>(b.data.size()));
  w.raw(b.data);
  return std::move(w.data);
}
std::optional<Bytes> popFrame(Bytes &input) {
  std::uint32_t n = 0;
  std::size_t prefix = 0;
  for (; prefix < 3; ++prefix) {
    if (prefix >= input.size())
      return {};
    auto b = input[prefix];
    n |= std::uint32_t(b & 127) << (prefix * 7);
    if (!(b & 128)) {
      ++prefix;
      break;
    }
    if (prefix == 2)
      throw ProtocolError("frame prefix too long");
  }
  if (!n || n > maxFrameSize)
    throw ProtocolError("invalid frame size");
  if (input.size() - prefix < n)
    return {};
  Bytes out(input.begin() + prefix, input.begin() + prefix + n);
  input.erase(input.begin(), input.begin() + prefix + n);
  return out;
}

namespace {
std::string key(const BlockState &s) {
  std::string k = s.name();
  for (auto &[n, v] : s.properties())
    k += "[" + n + "=" + v + "]";
  return k;
}
void loadNames(const std::filesystem::path &file,
               std::map<std::string, int> &out, bool required = true) {
  std::ifstream in(file);
  if (!in) {
    if (required)
      throw ProtocolError("missing registry: " + file.string());
    return;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    if (line.back() == '\r')
      line.pop_back();
    auto tab = line.find('\t');
    if (tab == std::string::npos)
      throw ProtocolError("invalid registry row");
    auto end = line.find('\t', tab + 1);
    auto name = line.substr(tab + 1, end - tab - 1);
    int id = std::stoi(line.substr(0, tab));
    if (id < 0 || !out.emplace(name, id).second)
      throw ProtocolError("duplicate registry name");
  }
}
int lookup(const std::map<std::string, int> &m, const std::string &name) {
  auto i = m.find(name);
  if (i == m.end())
    throw ProtocolError("unknown registry value: " + name);
  return i->second;
}
void nbtName(Writer &w, int tag, std::string_view name) {
  w.u8(static_cast<std::uint8_t>(tag));
  w.i16(static_cast<std::int16_t>(name.size()));
  w.raw({reinterpret_cast<const std::uint8_t *>(name.data()), name.size()});
}
void nbtString(Writer &w, std::string_view name, std::string_view value) {
  nbtName(w, 8, name);
  w.i16(static_cast<std::int16_t>(value.size()));
  w.raw({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
}
// SimpleBitStorage: values never straddle a long boundary.
void packed(Writer &w, std::span<const int> values, int bits, bool length) {
  const int perLong = 64 / bits;
  const int words = static_cast<int>((values.size() + perLong - 1) / perLong);
  if (length)
    w.varInt(words);
  for (int a = 0; a < words; ++a) {
    std::uint64_t word = 0;
    for (int j = 0; j < perLong && a * perLong + j < int(values.size()); ++j)
      word |= std::uint64_t(values[a * perLong + j]) << (j * bits);
    w.i64(std::bit_cast<std::int64_t>(word));
  }
}
void blockPalette(Writer &w, const std::vector<int> &ids, int globalBits) {
  std::map<int, int> indices;
  std::vector<int> palette, values;
  for (auto id : ids) {
    auto [i, inserted] = indices.emplace(id, static_cast<int>(palette.size()));
    if (inserted)
      palette.push_back(id);
    values.push_back(i->second);
  }
  if (palette.size() == 1) {
    w.u8(0);
    w.varInt(palette[0]);
    return;
  }
  int bits = std::max(4, int(std::bit_width(palette.size() - 1)));
  if (bits > 8) {
    w.u8(static_cast<std::uint8_t>(globalBits));
    packed(w, ids, globalBits, false);
  } else {
    w.u8(static_cast<std::uint8_t>(bits));
    w.varInt(static_cast<int>(palette.size()));
    for (auto id : palette)
      w.varInt(id);
    packed(w, values, bits, false);
  }
}
} // namespace
Registry::Registry(const std::filesystem::path &dir) {
  std::ifstream in(dir / "block_states.tsv");
  if (!in)
    throw ProtocolError("missing block_states.tsv");
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    if (line.back() == '\r')
      line.pop_back();
    auto a = line.find('\t');
    auto b = line.find('\t', a + 1);
    if (a == std::string::npos)
      throw ProtocolError("invalid block state row");
    int id = std::stoi(line.substr(0, a));
    const std::string name = line.substr(a + 1, b - a - 1);
    std::map<std::string, std::string> properties;
    if (b != std::string::npos) {
      std::istringstream props(line.substr(b + 1));
      std::string p;
      while (std::getline(props, p, ',')) {
        auto eq = p.find('=');
        if (eq == std::string::npos) {
          if (!p.empty())
            throw ProtocolError("invalid state property");
        } else
          properties.emplace(p.substr(0, eq), p.substr(eq + 1));
      }
    }
    BlockState s(name, std::move(properties));
    if (id < 0 || !states_.emplace(id, s).second ||
        !stateIds_.emplace(key(s), id).second)
      throw ProtocolError("duplicate state: " + key(s));
  }
  loadNames(dir / "items.tsv", items_);
  for (auto &[name, id] : items_)
    itemNames_.emplace(id, name);
  loadNames(dir / "entities.tsv", entities_);
  loadNames(dir / "biomes.tsv", biomes_, false);
  loadNames(dir / "block_entity_types.tsv", blockEntities_, false);
  if (states_.empty())
    throw ProtocolError("empty states registry");
  std::ifstream flagFile(dir / "state_flags.tsv");
  if (!flagFile)
    throw ProtocolError("missing state_flags.tsv");
  while (std::getline(flagFile, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream row(line);
    int id, a, f, m, l;
    if (!(row >> id >> a >> f >> m >> l) || !states_.contains(id) ||
        !flags_.emplace(id, StateFlags{a != 0, f != 0, m != 0, l != 0}).second)
      throw ProtocolError("invalid state flags row");
  }
  if (flags_.size() != states_.size())
    throw ProtocolError("incomplete state flags");
}
int Registry::stateId(const BlockState &s) const {
  auto i = stateIds_.find(key(s));
  if (i == stateIds_.end())
    throw ProtocolError("unknown block state: " + key(s));
  return i->second;
}
const BlockState &Registry::stateById(int id) const {
  auto i = states_.find(id);
  if (i == states_.end())
    throw ProtocolError("unknown block state ID");
  return i->second;
}
int Registry::itemId(const std::string &n) const { return lookup(items_, n); }
std::string Registry::itemName(int id) const {
  auto i = itemNames_.find(id);
  if (i == itemNames_.end())
    throw ProtocolError("unknown item ID");
  return i->second;
}
int Registry::entityTypeId(const std::string &n) const {
  return lookup(entities_, n);
}
int Registry::blockEntityTypeId(const std::string &n) const {
  return lookup(blockEntities_, n);
}
int Registry::biomeId(const std::string &n) const { return lookup(biomes_, n); }
Registry::StateFlags Registry::stateFlags(int id) const {
  auto i = flags_.find(id);
  if (i == flags_.end())
    throw ProtocolError("unknown state flags ID");
  return i->second;
}
int Registry::stateBits() const {
  return int(
      std::bit_width(static_cast<unsigned int>(states_.rbegin()->first)));
}

Bytes encodeMovingPistonNbt(const PistonMovingBlockEntity &p) {
  Writer w;
  w.u8(10);
  nbtName(w, 10, "blockState");
  nbtString(w, "Name", p.movedState.name());
  if (!p.movedState.properties().empty()) {
    nbtName(w, 10, "Properties");
    for (auto &[k, v] : p.movedState.properties())
      nbtString(w, k, v);
    w.u8(0);
  }
  w.u8(0);
  nbtName(w, 1, "facing"); // Direction.LEGACY_ID_CODEC is Codec.BYTE.
  w.u8(static_cast<std::uint8_t>(p.direction));
  nbtName(w, 5, "progress");
  w.f32(p.progressO);
  nbtName(w, 1, "extending");
  w.bool_(p.extending);
  nbtName(w, 1, "source");
  w.bool_(p.source);
  w.u8(0);
  return std::move(w.data);
}
Bytes encodeChunk(World &world, int cx, int cz, const Registry &registry) {
  if (world.minY() % 16 || world.maxY() % 16 || world.minY() >= world.maxY())
    throw ProtocolError("chunk height must be aligned to sections");
  Writer out;
  out.i32(cx);
  out.i32(cz);
  std::array<std::vector<int>, 3> heights;
  for (auto &h : heights)
    h.resize(256, 0);
  Writer sections;
  int sectionCount = (world.maxY() - world.minY()) / 16;
  for (int sy = world.minY(); sy < world.maxY(); sy += 16) {
    std::vector<int> ids;
    ids.reserve(4096);
    int nonEmpty = 0, fluid = 0;
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          auto s = world.getBlock({cx * 16 + x, sy + y, cz * 16 + z});
          int id = registry.stateId(s);
          ids.push_back(id);
          auto flags = registry.stateFlags(id);
          int h = sy + y + 1 - world.minY(), column = z * 16 + x;
          if (!flags.air) {
            ++nonEmpty;
            heights[0][column] = h;
          }
          if (flags.fluid)
            ++fluid;
          if (flags.blocksMotion || flags.fluid) {
            heights[1][column] = h;
            if (!flags.leaves)
              heights[2][column] = h;
          }
        }
    sections.i16(static_cast<std::int16_t>(nonEmpty));
    sections.i16(static_cast<std::int16_t>(fluid));
    blockPalette(sections, ids, registry.stateBits());
    sections.u8(0);
    sections.varInt(registry.biomeId("minecraft:plains"));
  }
  out.varInt(3);
  const int types[] = {1, 4, 5};
  int bits = std::bit_width(static_cast<unsigned>(world.maxY() - world.minY()));
  for (int i = 0; i < 3; ++i) {
    out.varInt(types[i]);
    packed(out, heights[i], bits, true);
  }
  out.varInt(static_cast<int>(sections.data.size()));
  out.raw(sections.data);
  std::vector<std::shared_ptr<PistonMovingBlockEntity>> pistons;
  for (auto &[p, be] : world.pistonEntities)
    if (p.x >= cx * 16 && p.x < cx * 16 + 16 && p.z >= cz * 16 &&
        p.z < cz * 16 + 16)
      pistons.push_back(be);
  out.varInt(static_cast<int>(pistons.size()));
  for (auto &p : pistons) {
    out.u8(((p->pos.x & 15) << 4) | (p->pos.z & 15));
    out.i16(static_cast<std::int16_t>(p->pos.y));
    out.varInt(registry.blockEntityTypeId("minecraft:piston"));
    out.raw(encodeMovingPistonNbt(*p));
  }
  // TODO: replace visualization light with a real light engine. A full sky
  // layer and empty block layer keep the optional viewer usable meanwhile.
  if (sectionCount > 60)
    throw ProtocolError("unsupported light section count");
  auto mask = (std::uint64_t(1) << (sectionCount + 2)) - 1;
  out.varInt(1);
  out.i64(static_cast<std::int64_t>(mask));
  out.varInt(0);
  out.varInt(0);
  out.varInt(1);
  out.i64(static_cast<std::int64_t>(mask));
  out.varInt(sectionCount + 2);
  Bytes sky(2048, 255);
  for (int i = 0; i < sectionCount + 2; ++i) {
    out.varInt(2048);
    out.raw(sky);
  }
  out.varInt(0);
  return std::move(out.data);
}
} // namespace redstone::net
