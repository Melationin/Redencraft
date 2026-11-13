#include "redstone/net/wire.hpp"
#include "redstone/command.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <bit>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
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
void Writer::varLong(std::int64_t v) {
  auto n = std::bit_cast<std::uint64_t>(v);
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
std::int64_t Reader::varLong() {
  std::uint64_t n = 0;
  for (int i = 0; i < 10; ++i) {
    auto b = u8();
    if (i == 9 && (b & 0xfe))
      throw ProtocolError("VarLong overflow");
    n |= std::uint64_t(b & 127) << (i * 7);
    if (!(b & 128))
      return std::bit_cast<std::int64_t>(n);
  }
  throw ProtocolError("VarLong too long");
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
Bytes encodeCommandTree(const redstone::command::Node &root) {
  // ClientboundCommandsPacket enumerates nodes breadth first and then writes the
  // root index; children refer to the ids assigned in that same order.
  std::vector<const redstone::command::Node *> order;
  std::map<const redstone::command::Node *, int> ids;
  std::deque<const redstone::command::Node *> queue;
  queue.push_back(&root);
  while (!queue.empty()) {
    const redstone::command::Node *node = queue.front();
    queue.pop_front();
    if (ids.contains(node))
      continue;
    ids[node] = static_cast<int>(order.size());
    order.push_back(node);
    for (const auto &child : node->children)
      queue.push_back(child.get());
  }
  constexpr std::uint8_t kFlagExecutable = 4;
  constexpr std::uint8_t kFlagRestricted = 32;
  Writer out;
  out.varInt(static_cast<std::int32_t>(order.size()));
  for (const redstone::command::Node *node : order) {
    std::uint8_t flags = 0;
    switch (node->kind) {
    case redstone::command::Node::Kind::Root:
      break;
    case redstone::command::Node::Kind::Literal:
      flags = 1;
      break;
    case redstone::command::Node::Kind::Argument:
      flags = 2;
      break;
    }
    if (node->executable())
      flags |= kFlagExecutable;
    if (node->restricted())
      flags |= kFlagRestricted;
    out.u8(flags);
    out.varInt(static_cast<std::int32_t>(node->children.size()));
    for (const auto &child : node->children)
      out.varInt(ids.at(child.get()));
    if (node->kind == redstone::command::Node::Kind::Literal) {
      out.string(node->name);
    } else if (node->kind == redstone::command::Node::Kind::Argument) {
      out.string(node->name);
      if (node->argumentKind == redstone::command::Node::ArgumentKind::Float) {
        out.varInt(1); // brigadier:float
        out.u8(3);     // ArgumentUtils.createNumberFlags(true, true)
        out.f32(static_cast<float>(node->minimum));
        out.f32(static_cast<float>(node->maximum));
      } else if (node->argumentKind ==
                 redstone::command::Node::ArgumentKind::Time) {
        out.varInt(43); // minecraft:time
        out.i32(static_cast<std::int32_t>(node->minimum));
      } else if (node->argumentKind ==
                 redstone::command::Node::ArgumentKind::Resource) {
        // Official 26.1 ArgumentTypeInfos / BuiltInRegistries probe:
        // minecraft:resource=46 and ResourceArgument.Info writes its registry
        // ResourceKey as an Identifier.
        out.varInt(46); // minecraft:resource
        out.string(node->resourceRegistry);
      } else {
        out.varInt(36); // minecraft:resource_location
        // SingletonArgumentInfo.contextFree(IdentifierArgument::id) has no
        // network payload.
      }
    }
  }
  out.varInt(0); // root index
  return out.data;
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
  // Dense caches: every codec path converts BlockState -> protocol id and
  // protocol id -> flags, so index instead of hashing a name+property string.
  std::size_t localCount = 0;
  for (const auto &[id, state] : states_)
    localCount = std::max<std::size_t>(localCount, state.id() + 1);
  protocolByLocal_.assign(localCount, -1);
  for (const auto &[id, state] : states_)
    protocolByLocal_[state.id()] = id;
  flagsByProtocol_.assign(
      static_cast<std::size_t>(states_.rbegin()->first) + 1, StateFlags{});
  for (const auto &[id, flags] : flags_)
    flagsByProtocol_[static_cast<std::size_t>(id)] = flags;
}
int Registry::stateId(const BlockState &s) const {
  const auto local = static_cast<std::size_t>(s.id());
  if (local < protocolByLocal_.size() && protocolByLocal_[local] >= 0)
    return protocolByLocal_[local];
  // Imported extension states register after the protocol table was built;
  // fall back to the name+properties key.
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
  if (id >= 0 && static_cast<std::size_t>(id) < flagsByProtocol_.size())
    return flagsByProtocol_[static_cast<std::size_t>(id)];
  auto i = flags_.find(id);
  if (i == flags_.end())
    throw ProtocolError("unknown state flags ID");
  return i->second;
}
int Registry::stateBits() const {
  return int(
      std::bit_width(static_cast<unsigned int>(states_.rbegin()->first)));
}

Bytes encodeMovingPistonNbt(const PistonSnapshot &p) {
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
namespace {
void setBit(std::vector<std::uint64_t> &bits, int index) {
  if (index < 0)
    throw ProtocolError("negative light section index");
  const auto word = static_cast<std::size_t>(index) / 64;
  if (bits.size() <= word)
    bits.resize(word + 1);
  bits[word] |= std::uint64_t{1} << (index & 63);
}

void writeBitSet(Writer &out, const std::vector<std::uint64_t> &bits) {
  out.varInt(static_cast<int>(bits.size()));
  for (std::uint64_t word : bits)
    out.i64(std::bit_cast<std::int64_t>(word));
}

std::size_t lightSnapshotBytes(const LightChunkSnapshot &snapshot) noexcept {
  std::size_t bytes = sizeof(snapshot) +
                      snapshot.sections.capacity() *
                          sizeof(std::shared_ptr<const LightSectionSnapshot>) +
                      snapshot.changedSections.capacity() *
                          sizeof(LightChunkSnapshot::LayerChange);
  for (const auto &section : snapshot.sections)
    if (section) {
      bytes += sizeof(*section);
      for (const auto &layer : section->layers)
        bytes += layer.packedByteCount();
    }
  return bytes;
}

void appendLayer(Writer &out, const LightLayerRef &layer) {
  if (layer.isMixed()) {
    std::array<std::uint8_t, kLightLayerBytes> packed{};
    layer.readPacked(packed);
    out.varInt(static_cast<int>(kLightLayerBytes));
    out.raw(packed);
    return;
  }
  if (!layer.isUniform() || layer.uniformValue() == 0)
    throw ProtocolError("light data layer is not encodable");
  const std::uint8_t value = layer.uniformValue();
  const std::uint8_t packed = static_cast<std::uint8_t>((value << 4) | value);
  Bytes bytes(kLightLayerBytes, packed);
  out.varInt(static_cast<int>(bytes.size()));
  out.raw(bytes);
}

void collectLayerSections(const LightChunkSnapshot &source, LightLayer type,
                          ProtocolLightSections &sections) {
  const std::size_t index = static_cast<std::size_t>(type);
  for (int sectionY = source.minLightSectionY();
       sectionY < source.minBlockSectionY + source.blockSectionCount + 1;
       ++sectionY) {
    const LightSectionSnapshot *section = source.section(sectionY);
    if (!section)
      continue;
    const auto &layer = section->layers[index];
    if (layer.isMissing())
      continue;
    if (layer.isUniform() && layer.uniformValue() == 0)
      sections.emptySections.push_back(sectionY);
    else
      sections.dataSections.push_back(sectionY);
  }
}

void appendLightSections(
    Writer &out, const ProtocolLightUpdate &update,
    const LightChunkSnapshot *skySourceOverride = nullptr,
    const LightChunkSnapshot *blockSourceOverride = nullptr) {
  const auto writeLayer = [&](const ProtocolLightSections &sections,
                              std::vector<std::uint64_t> &mask,
                              std::vector<std::uint64_t> &empty) {
    for (int sectionY : sections.dataSections) {
      const int sectionIndex = sectionY - update.snapshot->minLightSectionY();
      setBit(mask, sectionIndex);
    }
    for (int sectionY : sections.emptySections) {
      const int sectionIndex = sectionY - update.snapshot->minLightSectionY();
      setBit(empty, sectionIndex);
    }
  };
  std::vector<std::uint64_t> skyMask, blockMask, skyEmpty, blockEmpty;
  writeLayer(update.sky, skyMask, skyEmpty);
  writeLayer(update.block, blockMask, blockEmpty);
  writeBitSet(out, skyMask);
  writeBitSet(out, blockMask);
  writeBitSet(out, skyEmpty);
  writeBitSet(out, blockEmpty);
  const auto writeData = [&](LightLayer type,
                             const std::vector<int> &sections) {
    out.varInt(static_cast<int>(sections.size()));
    const auto layer = static_cast<std::size_t>(type);
    const LightChunkSnapshot *source =
        type == LightLayer::Sky ? skySourceOverride : blockSourceOverride;
    if (!source)
      source = update.snapshot.get();
    if (!source && !sections.empty())
      throw ProtocolError("light update has no source for its layer");
    for (int sectionY : sections) {
      const LightSectionSnapshot *section = source->section(sectionY);
      if (!section)
        throw ProtocolError("light update references a missing section");
      appendLayer(out, section->layers[layer]);
    }
  };
  writeData(LightLayer::Sky, update.sky.dataSections);
  writeData(LightLayer::Block, update.block.dataSections);
}
} // namespace

std::size_t ProtocolChunkSnapshot::retainedBytes() const noexcept {
  std::size_t bytes = sizeof(*this) + sections.capacity() * sizeof(SectionSnapshot) +
                      blockEntities.capacity() * sizeof(BlockEntity);
  for (const auto &section : sections)
    bytes += section.palette.capacity() * sizeof(BlockState::Id) +
             section.indices8.capacity() + section.indices16.capacity() * 2;
  for (const auto &entity : blockEntities)
    bytes += entity.nbt.capacity();
  if (blockLight)
    bytes += lightSnapshotBytes(*blockLight);
  if (skyLight)
    bytes += lightSnapshotBytes(*skyLight);
  return bytes;
}
ProtocolChunkSnapshot prepareChunkSnapshot(
    ChunkSnapshot snapshot, const Registry &registry,
    std::shared_ptr<const LightChunkSnapshot> blockLight,
    std::shared_ptr<const LightChunkSnapshot> skyLight) {
  ProtocolChunkSnapshot result;
  result.minY = snapshot.minY;
  result.maxY = snapshot.maxY;
  result.airId = registry.stateId(BlockState{});
  result.stateBits = registry.stateBits();
  result.biomeId = registry.biomeId("minecraft:plains");
  result.pistonTypeId = registry.blockEntityTypeId("minecraft:piston");
  result.sections = std::move(snapshot.sections);
  for (auto &section : result.sections) {
    if (section.bits == 0)
      section.single = registry.stateId(BlockState::fromId(section.single));
    else
      for (auto &id : section.palette)
        id = registry.stateId(BlockState::fromId(id));
  }
  result.blockEntities.reserve(snapshot.pistons.size());
  for (const auto &piston : snapshot.pistons)
    result.blockEntities.push_back({piston.pos, encodeMovingPistonNbt(piston)});
  const auto validate = [&](const std::shared_ptr<const LightChunkSnapshot> &view,
                            LightLayer expected) {
    if (view && (!view->protocolReady || !view->layer ||
                 *view->layer != expected ||
                 view->minLightSectionY() != snapshot.minY / 16 - 1 ||
                 view->lightSectionCount() !=
                     (snapshot.maxY - snapshot.minY) / 16 + 2))
      throw ProtocolError(
          "chunk light is unpublished, incomplete, misaligned or combined");
  };
  validate(blockLight, LightLayer::Block);
  validate(skyLight, LightLayer::Sky);
  result.blockLight = std::move(blockLight);
  result.skyLight = std::move(skyLight);
  return result;
}
Bytes encodeChunk(const ChunkSnapshot &snapshot, int cx, int cz,
                  const Registry &registry) {
  return encodeChunk(prepareChunkSnapshot(snapshot, registry), cx, cz, registry);
}
Bytes encodeChunk(const ProtocolChunkSnapshot &snapshot, int cx, int cz,
                  const Registry &registry) {
  if (snapshot.minY % 16 || snapshot.maxY % 16 ||
      snapshot.minY >= snapshot.maxY)
    throw ProtocolError("chunk height must be aligned to sections");
  for (const auto &view : {snapshot.blockLight, snapshot.skyLight})
    if (view && (view->chunkX != cx || view->chunkZ != cz))
      throw ProtocolError("chunk light belongs to a different chunk identity");
  Writer out;
  out.i32(cx);
  out.i32(cz);
  std::array<std::vector<int>, 3> heights;
  for (auto &h : heights)
    h.resize(256, 0);
  Writer sections;
  const int sectionCount = (snapshot.maxY - snapshot.minY) / 16;
  const int airId = snapshot.airId;
  const int globalBits = snapshot.stateBits;
  std::vector<int> ids(4096);
  std::vector<int> mapped;
  for (int i = 0; i < sectionCount; ++i) {
    const SectionSnapshot *snap =
        i < static_cast<int>(snapshot.sections.size()) ? &snapshot.sections[i]
                                                       : nullptr;
    if (!snap) {
      std::fill(ids.begin(), ids.end(), airId);
    } else if (snap->bits == 0) {
      const int id = static_cast<int>(snap->single);
      std::fill(ids.begin(), ids.end(), id);
    } else {
      mapped.resize(snap->palette.size());
      for (std::size_t k = 0; k < snap->palette.size(); ++k)
        mapped[k] = static_cast<int>(snap->palette[k]);
      for (int cell = 0; cell < 4096; ++cell) {
        const int paletteIndex =
            snap->bits == 8 ? snap->indices8[cell] : snap->indices16[cell];
        ids[cell] = mapped[paletteIndex];
      }
    }
    const int sy = snapshot.minY + i * 16;
    int nonEmpty = 0, fluid = 0;
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) {
          const int id = ids[(y << 8) | (z << 4) | x];
          const auto flags = registry.stateFlags(id);
          const int h = sy + y + 1 - snapshot.minY, column = z * 16 + x;
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
    blockPalette(sections, ids, globalBits);
    sections.u8(0);
    sections.varInt(snapshot.biomeId);
  }
  out.varInt(3);
  const int types[] = {1, 4, 5};
  int bits = std::bit_width(static_cast<unsigned>(snapshot.maxY - snapshot.minY));
  for (int i = 0; i < 3; ++i) {
    out.varInt(types[i]);
    packed(out, heights[i], bits, true);
  }
  out.varInt(static_cast<int>(sections.data.size()));
  out.raw(sections.data);
  out.varInt(static_cast<int>(snapshot.blockEntities.size()));
  for (const auto &p : snapshot.blockEntities) {
    out.u8(((p.pos.x & 15) << 4) | (p.pos.z & 15));
    out.i16(static_cast<std::int16_t>(p.pos.y));
    out.varInt(snapshot.pistonTypeId);
    out.raw(p.nbt);
  }
  if (snapshot.blockLight || snapshot.skyLight) {
    ProtocolLightUpdate combined;
    combined.chunkX = cx;
    combined.chunkZ = cz;
    combined.snapshot = snapshot.blockLight ? snapshot.blockLight
                                            : snapshot.skyLight;
    if (snapshot.blockLight)
      collectLayerSections(*snapshot.blockLight, LightLayer::Block,
                           combined.block);
    if (snapshot.skyLight)
      collectLayerSections(*snapshot.skyLight, LightLayer::Sky, combined.sky);
    appendLightSections(out, combined, snapshot.skyLight.get(),
                        snapshot.blockLight.get());
    return std::move(out.data);
  }
  // Explicitly unlit worlds retain the legacy visualization fallback. The
  // native server enables the light service, so published snapshots never use
  // this path.
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

std::size_t ProtocolLightUpdate::retainedBytes() const noexcept {
  return sizeof(*this) + (snapshot ? lightSnapshotBytes(*snapshot) : 0) +
         sky.dataSections.capacity() * sizeof(int) +
         sky.emptySections.capacity() * sizeof(int) +
         block.dataSections.capacity() * sizeof(int) +
         block.emptySections.capacity() * sizeof(int);
}

ProtocolLightUpdate prepareLightUpdate(
    std::shared_ptr<const LightChunkSnapshot> current,
    std::shared_ptr<const LightChunkSnapshot> previous) {
  if (!current || !current->protocolReady)
    throw ProtocolError("light update source is not protocol ready");
  if (previous &&
      (previous->chunkX != current->chunkX ||
       previous->chunkZ != current->chunkZ ||
       previous->lifecycleEpoch != current->lifecycleEpoch ||
       previous->revision >= current->revision ||
       current->deltaBaseRevision != previous->revision ||
       previous->minLightSectionY() != current->minLightSectionY() ||
       previous->lightSectionCount() != current->lightSectionCount()))
    throw ProtocolError("light update crossed a chunk lifecycle");

  ProtocolLightUpdate result;
  result.chunkX = current->chunkX;
  result.chunkZ = current->chunkZ;
  result.snapshot = std::move(current);
  const auto append = [&](int sectionY, LightLayer layer, bool changed) {
    if (!changed)
      return;
    const auto *now = result.snapshot->section(sectionY);
    if (!now)
      throw ProtocolError("light delta references a missing section");
    const auto &nowLayer = now->layers[static_cast<std::size_t>(layer)];
    ProtocolLightSections &sections =
        layer == LightLayer::Sky ? result.sky : result.block;
    if (nowLayer.isMissing()) {
      // ClientboundLightUpdatePacketData leaves both masks clear when the
      // storage layer is null. The client's chunk state removal, not an empty
      // light layer, handles that lifecycle; missing is not brightness zero.
      return;
    } else if (nowLayer.isUniform() && nowLayer.uniformValue() == 0) {
      sections.emptySections.push_back(sectionY);
    } else {
      sections.dataSections.push_back(sectionY);
    }
  };
  if (!previous) {
    for (int index = 0; index < result.snapshot->lightSectionCount(); ++index) {
      const int sectionY = result.snapshot->minLightSectionY() + index;
      append(sectionY, LightLayer::Sky, true);
      append(sectionY, LightLayer::Block, true);
    }
  } else {
    for (const auto &change : result.snapshot->changedSections) {
      append(change.sectionY, LightLayer::Sky, change.sky);
      append(change.sectionY, LightLayer::Block, change.block);
    }
  }
  return result;
}

ProtocolLightUpdate prepareLayerLightUpdate(
    std::shared_ptr<const LightChunkSnapshot> current, LightLayer layer,
    const LightLayerVersion *previous) {
  if (!current || !current->protocolReady || !current->layer ||
      *current->layer != layer)
    throw ProtocolError("layer update source is not an independent ready view");
  if (previous &&
      (previous->chunkX != current->chunkX ||
       previous->chunkZ != current->chunkZ ||
       previous->lifecycleEpoch != current->lifecycleEpoch ||
       previous->revision >= current->revision ||
       current->deltaBaseRevision != previous->revision ||
       previous->minBlockSectionY != current->minBlockSectionY ||
       previous->blockSectionCount != current->blockSectionCount))
    throw ProtocolError("layer update crossed a chunk lifecycle");

  ProtocolLightUpdate result;
  result.chunkX = current->chunkX;
  result.chunkZ = current->chunkZ;
  result.snapshot = std::move(current);
  ProtocolLightSections &sections =
      layer == LightLayer::Sky ? result.sky : result.block;
  const std::size_t index = static_cast<std::size_t>(layer);
  const auto append = [&](int sectionY) {
    const LightSectionSnapshot *now = result.snapshot->section(sectionY);
    if (!now)
      return;
    const auto &value = now->layers[index];
    if (value.isMissing())
      return;
    if (value.isUniform() && value.uniformValue() == 0)
      sections.emptySections.push_back(sectionY);
    else
      sections.dataSections.push_back(sectionY);
  };
  if (!previous) {
    for (int sectionY = result.snapshot->minLightSectionY();
         sectionY < result.snapshot->minBlockSectionY +
                        result.snapshot->blockSectionCount + 1;
         ++sectionY)
      append(sectionY);
  } else {
    for (const auto &change : result.snapshot->changedSections)
      if (layer == LightLayer::Sky ? change.sky : change.block)
        append(change.sectionY);
  }
  return result;
}

Bytes encodeLightUpdate(const ProtocolLightUpdate &update) {
  if (!update.snapshot)
    throw ProtocolError("empty light update");
  Writer out;
  // ClientboundLightUpdatePacket writes chunk x/z as VarInts.
  out.varInt(update.chunkX);
  out.varInt(update.chunkZ);
  appendLightSections(out, update);
  return std::move(out.data);
}
} // namespace redstone::net
