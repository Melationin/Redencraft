#include "redstone/storage.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <bit>
#include <climits>
#include <cstring>
#include <fstream>
#include <limits>
#include <lz4.h>
#include <regex>
#include <set>
#include <xxhash.h>
namespace redstone {
namespace {
using nbt::Tag;
std::vector<uint8_t> file(const std::filesystem::path &p,
                          size_t max = 512 * 1024 * 1024) {
  std::ifstream f(p, std::ios::binary | std::ios::ate);
  if (!f)
    throw nbt::Error("cannot open " + p.string());
  auto n = f.tellg();
  if (n < 0 || static_cast<uint64_t>(n) > max)
    throw nbt::Error("file too large: " + p.string());
  std::vector<uint8_t> v(static_cast<size_t>(n));
  f.seekg(0);
  if (!v.empty() && !f.read(reinterpret_cast<char *>(v.data()), v.size()))
    throw nbt::Error("cannot read " + p.string());
  return v;
}
// Minecraft uses lz4-java's legacy LZ4Block stream, not standard LZ4 frames.
std::vector<uint8_t> lz4Blocks(std::span<const uint8_t> input) {
  constexpr size_t limit = 128 * 1024 * 1024;
  size_t pos = 0;
  std::vector<uint8_t> out;
  auto le = [](const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
  };
  for (;;) {
    if (input.size() - pos < 21 ||
        std::memcmp(input.data() + pos, "LZ4Block", 8) != 0)
      throw nbt::Error("invalid LZ4 block header");
    auto h = input.data() + pos;
    uint8_t token = h[8], method = token & 0xf0;
    uint32_t compressed = le(h + 9), original = le(h + 13), check = le(h + 17);
    pos += 21;
    if ((method != 0x10 && method != 0x20) ||
        original > (1u << (10 + (token & 15))) ||
        compressed > input.size() - pos ||
        (original == 0) != (compressed == 0) ||
        (method == 0x10 && original != compressed))
      throw nbt::Error("invalid LZ4 block length or method");
    if (!original) {
      if (check || pos != input.size())
        throw nbt::Error("invalid LZ4 terminator");
      return out;
    }
    if (original > limit - out.size() || compressed > INT_MAX)
      throw nbt::Error("LZ4 data exceeds limit");
    size_t start = out.size();
    out.resize(start + original);
    if (method == 0x10)
      std::copy_n(input.data() + pos, original, out.data() + start);
    else if (LZ4_decompress_safe(
                 reinterpret_cast<const char *>(input.data() + pos),
                 reinterpret_cast<char *>(out.data() + start),
                 static_cast<int>(compressed),
                 static_cast<int>(original)) != static_cast<int>(original))
      throw nbt::Error("invalid LZ4 compressed block");
    if ((XXH32(out.data() + start, original, 0x9747b28cU) & 0x0fffffffU) !=
        check)
      throw nbt::Error("LZ4 block checksum mismatch");
    pos += compressed;
  }
}
uint32_t be32(const uint8_t *p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
         p[3];
}
const Tag &required(const Tag &t, const char *name, int type) {
  auto p = t.get(name);
  if (!p || p->type != type)
    throw nbt::Error(std::string("missing or invalid ") + name);
  return *p;
}
int chunkCoord(int x) {
  int q = x / 16;
  return q - (x % 16 < 0);
}
int integer(const Tag &t, const char *n) {
  return static_cast<int>(required(t, n, 3).number);
}
BlockState state(const Tag &t) {
  BlockState s(required(t, "Name", 8).text);
  if (auto p = t.get("Properties")) {
    if (p->type != 10)
      throw nbt::Error("invalid block properties");
    for (auto &[k, v] : p->compound) {
      if (v.type != 8)
        throw nbt::Error("invalid block property");
      s.properties[k] = v.text;
    }
  }
  return s;
}
void chunk(LoadedWorld &w, const Tag &root, int cx, int cz) {
  if (integer(root, "xPos") != cx || integer(root, "zPos") != cz)
    throw nbt::Error("chunk coordinates do not match region location");
  if (integer(root, "DataVersion") != 4786)
    throw nbt::Error("unsupported chunk DataVersion (expected 26.1: 4786)");
  auto status = required(root, "Status", 8).text;
  if (status != "minecraft:full" && status != "full")
    throw nbt::Error("chunk is not fully generated: " + status);
  w.chunks.emplace_back(cx, cz);
  if (auto sections = root.get("sections")) {
    if (sections->type != 9)
      throw nbt::Error("invalid sections");
    for (auto &sec : sections->list) {
      if (sec.type != 10)
        throw nbt::Error("invalid section");
      auto sy = required(sec, "Y", 1).number;
      auto bs = sec.get("block_states");
      if (!bs)
        continue;
      if (bs->type != 10)
        throw nbt::Error("invalid block_states");
      auto &pal = required(*bs, "palette", 9).list;
      if (pal.empty() || pal.size() > 4096)
        throw nbt::Error("invalid block palette");
      std::vector<BlockState> states;
      for (auto &e : pal) {
        if (e.type != 10)
          throw nbt::Error("invalid palette entry");
        states.push_back(state(e));
      }
      unsigned bits = std::max(4u, static_cast<unsigned>(std::bit_width(
                                       static_cast<unsigned>(pal.size() - 1))));
      unsigned per = 64 / bits;
      const Tag *data = bs->get("data");
      if (pal.size() > 1 && (!data || data->type != 12 ||
                             data->longs.size() != (4096 + per - 1) / per))
        throw nbt::Error("invalid block state data length");
      if (pal.size() == 1 && data && (data->type != 12 || !data->longs.empty()))
        throw nbt::Error("unexpected single palette data");
      for (int i = 0; i < 4096; i++) {
        unsigned index = 0;
        if (pal.size() > 1)
          index = (static_cast<uint64_t>(data->longs[i / per]) >>
                   ((i % per) * bits)) &
                  ((uint64_t(1) << bits) - 1);
        if (index >= states.size())
          throw nbt::Error("block palette index out of range");
        int64_t y = sy * 16 + (i >> 8);
        if (y < w.minY || y >= w.maxY)
          continue;
        auto &s = states[index];
        if (s.name != "minecraft:air")
          w.blocks[{cx * 16 + (i & 15), static_cast<int>(y),
                    cz * 16 + ((i >> 4) & 15)}] = s;
      }
    }
  }
  for (auto [name, fluid] :
       {std::pair{"block_ticks", false}, std::pair{"fluid_ticks", true}})
    if (auto ts = root.get(name)) {
      if (ts->type != 9)
        throw nbt::Error("invalid ticks");
      for (auto &t : ts->list) {
        if (t.type != 10)
          throw nbt::Error("invalid tick");
        SavedTickData v{{integer(t, "x"), integer(t, "y"), integer(t, "z")},
                        required(t, "i", 8).text,
                        integer(t, "t"),
                        integer(t, "p"),
                        fluid};
        if (chunkCoord(v.pos.x) == cx && chunkCoord(v.pos.z) == cz)
          w.ticks.push_back(std::move(v));
      }
    }
  if (auto es = root.get("block_entities")) {
    if (es->type != 9)
      throw nbt::Error("invalid block entities");
    for (auto &e : es->list) {
      if (e.type != 10)
        throw nbt::Error("invalid block entity");
      w.blockEntities.push_back(e);
    }
  }
}
} // namespace
LoadedWorld loadAnvilWorld(const std::filesystem::path &worldDir,
                           const std::filesystem::path &regionDir, int minY,
                           int maxY) {
  if (minY >= maxY)
    throw nbt::Error("invalid world height");
  LoadedWorld w;
  w.minY = minY;
  w.maxY = maxY;
  auto level = file(worldDir / "level.dat");
  auto root = nbt::parse(nbt::decompress(level, 15 + 16));
  auto &data = required(root, "Data", 10);
  if (integer(data, "DataVersion") != 4786)
    throw nbt::Error("unsupported world DataVersion (expected 26.1: 4786)");
  w.gameTime = required(data, "Time", 4).number;
  std::regex pattern(R"(^r\.(-?[0-9]+)\.(-?[0-9]+)\.mca$)");
  std::vector<std::filesystem::path> paths;
  for (auto &entry : std::filesystem::directory_iterator(regionDir))
    if (entry.is_regular_file() &&
        std::regex_match(entry.path().filename().string(), pattern))
      paths.push_back(entry.path());
  std::sort(paths.begin(), paths.end());
  for (auto &path : paths) {
    std::smatch m;
    auto name = path.filename().string();
    std::regex_match(name, m, pattern);
    long long rx, rz;
    try {
      rx = std::stoll(m[1]);
      rz = std::stoll(m[2]);
    } catch (...) {
      throw nbt::Error("invalid region coordinates");
    }
    if (rx < -4194304 || rx > 4194303 || rz < -4194304 || rz > 4194303)
      throw nbt::Error("region coordinates outside block range");
    auto bytes = file(path);
    if (bytes.size() < 8192 || bytes.size() % 4096)
      throw nbt::Error("invalid region file size");
    std::set<size_t> occupied{0, 1};
    for (int slot = 0; slot < 1024; slot++) {
      auto location = be32(bytes.data() + slot * 4);
      if (!location)
        continue;
      size_t offset = location >> 8, sectors = location & 255;
      if (offset < 2 || !sectors || offset > bytes.size() / 4096 ||
          sectors > bytes.size() / 4096 - offset)
        throw nbt::Error("invalid region sector location");
      for (size_t k = offset; k < offset + sectors; k++)
        if (!occupied.insert(k).second)
          throw nbt::Error("overlapping region sectors");
      auto start = offset * 4096;
      uint32_t len = be32(bytes.data() + start);
      if (len < 1 || len > sectors * 4096 - 4)
        throw nbt::Error("invalid chunk length");
      uint8_t compression = bytes[start + 4];
      int cx = static_cast<int>(rx * 32 + (slot % 32)),
          cz = static_cast<int>(rz * 32 + (slot / 32));
      std::vector<uint8_t> payload;
      if (compression & 128) {
        if (len != 1)
          throw nbt::Error("invalid external chunk marker");
        payload = file(regionDir / ("c." + std::to_string(cx) + "." +
                                    std::to_string(cz) + ".mcc"),
                       128 * 1024 * 1024);
        compression &= 127;
      } else
        payload.assign(bytes.begin() + start + 5,
                       bytes.begin() + start + 4 + len);
      if (compression == 1)
        payload = nbt::decompress(payload, 31);
      else if (compression == 2)
        payload = nbt::decompress(payload, 15);
      else if (compression == 4)
        payload = lz4Blocks(payload);
      else if (compression != 3)
        throw nbt::Error("unsupported chunk compression type " +
                         std::to_string(compression));
      chunk(w, nbt::parse(payload), cx, cz);
    }
  }
  return w;
}
// TODO: Fluid ticks and non-piston block entities remain in LoadedWorld for
// reporting; their simulation is not implemented.
void importWorld(const LoadedWorld &input, World &world) {
  world.setGameTime(input.gameTime);
  for (auto [x, z] : input.chunks)
    world.loadChunk(x, z);
  for (auto &[pos, block] : input.blocks)
    world.rawSetBlock(pos, block);
  // LevelChunkTicks.unpack assigns -N..-1 in saved-list order per chunk.
  // Its scheduleUnchecked path preserves duplicate saved entries. Unknown
  // vanilla block IDs cannot yet be validated against the complete registry.
  std::map<std::pair<int, int>, std::int64_t> counts;
  for (const auto &tick : input.ticks)
    if (!tick.fluid && world.isLoaded(tick.pos))
      ++counts[{chunkCoord(tick.pos.x), chunkCoord(tick.pos.z)}];
  std::map<std::pair<int, int>, std::int64_t> next;
  for (const auto &[chunk, count] : counts)
    next[chunk] = -count;
  for (const auto &tick : input.ticks) {
    if (tick.fluid || !world.isLoaded(tick.pos))
      continue;
    const auto chunk =
        std::make_pair(chunkCoord(tick.pos.x), chunkCoord(tick.pos.z));
    const auto name = tick.type.find(':') == std::string::npos
                          ? "minecraft:" + tick.type
                          : tick.type;
    world.scheduledTicks.push_back({tick.pos, name, input.gameTime + tick.delay,
                                    tick.priority, next[chunk]++});
  }
  for (auto &tag : input.blockEntities) {
    auto id = tag.get("id");
    if (!id || id->type != 8 || id->text != "minecraft:piston")
      continue;
    auto e = std::make_shared<PistonMovingBlockEntity>();
    e->pos = {integer(tag, "x"), integer(tag, "y"), integer(tag, "z")};
    e->movedState = state(required(tag, "blockState", 10));
    auto facing = required(tag, "facing", 1).number;
    if (facing < 0 || facing > 5)
      throw nbt::Error("invalid piston facing");
    e->direction = static_cast<Direction>(facing);
    if (auto p = tag.get("progress")) {
      if (p->type != 5)
        throw nbt::Error("invalid piston progress");
      e->progress = e->progressO = static_cast<float>(p->real);
    }
    if (auto p = tag.get("extending")) {
      if (p->type != 1)
        throw nbt::Error("invalid piston extending");
      e->extending = p->number != 0;
    } else
      e->extending = false;
    if (auto p = tag.get("source")) {
      if (p->type != 1)
        throw nbt::Error("invalid piston source");
      e->source = p->number != 0;
    } else
      e->source = false;
    world.setPistonEntity(e->pos, e);
  }
}
} // namespace redstone
