#include "redstone/piston.hpp"
#include "redstone/storage.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <lz4.h>
#include <xxhash.h>
#include <zlib.h>
using namespace redstone;
using Bytes = std::vector<uint8_t>;
static void u32(Bytes &b, uint32_t v) {
  for (int i = 3; i >= 0; --i)
    b.push_back(uint8_t(v >> (i * 8)));
}
static void str(Bytes &b, const std::string &s) {
  b.push_back(uint8_t(s.size() >> 8));
  b.push_back(uint8_t(s.size()));
  b.insert(b.end(), s.begin(), s.end());
}
static void tag(Bytes &b, int t, const std::string &n) {
  b.push_back(t);
  str(b, n);
}
static void integer(Bytes &b, const std::string &n, int v) {
  tag(b, 3, n);
  u32(b, uint32_t(v));
}
static void stringtag(Bytes &b, const std::string &n, const std::string &v) {
  tag(b, 8, n);
  str(b, v);
}
static Bytes root() { return {10, 0, 0}; }
static void write(const std::filesystem::path &p, const Bytes &b) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char *>(b.data()), b.size());
}
static Bytes compressed(const Bytes &b, int bits) {
  z_stream z{};
  if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, bits, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK)
    throw std::runtime_error("deflate init");
  Bytes out(4096);
  z.next_in = (Bytef *)b.data();
  z.avail_in = (uInt)b.size();
  z.next_out = out.data();
  z.avail_out = (uInt)out.size();
  if (deflate(&z, Z_FINISH) != Z_STREAM_END)
    throw std::runtime_error("deflate");
  out.resize(z.total_out);
  deflateEnd(&z);
  return out;
}
static void le32(Bytes &b, uint32_t v) {
  for (int i = 0; i < 4; i++)
    b.push_back(uint8_t(v >> (i * 8)));
}
static Bytes lz4(const Bytes &input, bool raw) {
  Bytes packed(LZ4_compressBound((int)input.size()));
  int n =
      LZ4_compress_default((const char *)input.data(), (char *)packed.data(),
                           (int)input.size(), (int)packed.size());
  if (n <= 0)
    throw std::runtime_error("LZ4 compression failed");
  packed.resize(n);
  if (raw)
    packed = input;
  Bytes b;
  auto header = [&](int method, uint32_t clen, uint32_t olen, uint32_t hash) {
    for (char ch : std::string("LZ4Block"))
      b.push_back(ch);
    b.push_back(uint8_t(method | 6));
    le32(b, clen);
    le32(b, olen);
    le32(b, hash);
  };
  header(raw ? 0x10 : 0x20, (uint32_t)packed.size(), (uint32_t)input.size(),
         XXH32(input.data(), input.size(), 0x9747b28cU) & 0x0fffffffU);
  b.insert(b.end(), packed.begin(), packed.end());
  header(0x10, 0, 0, 0);
  return b;
}
int main() {
  auto p = std::filesystem::temp_directory_path() / "redstone_storage_fixture";
  std::filesystem::create_directories(p / "region");
  Bytes level = root();
  tag(level, 10, "Data");
  integer(level, "DataVersion", 4786);
  tag(level, 4, "Time");
  for (int i = 0; i < 7; i++)
    level.push_back(0);
  level.push_back(42);
  level.push_back(0);
  level.push_back(0);
  write(p / "level.dat", compressed(level, 31));
  Bytes c = root();
  integer(c, "DataVersion", 4786);
  integer(c, "xPos", -1);
  integer(c, "zPos", -1);
  stringtag(c, "Status", "minecraft:full");
  tag(c, 9, "sections");
  c.push_back(10);
  u32(c, 2);
  tag(c, 1, "Y");
  c.push_back(255);
  tag(c, 10, "block_states");
  tag(c, 9, "palette");
  c.push_back(10);
  u32(c, 1);
  stringtag(c, "Name", "example:unknown");
  tag(c, 10, "Properties");
  stringtag(c, "foo", "bar");
  c.push_back(0);
  c.push_back(0);
  c.push_back(0);
  c.push_back(0);
  tag(c, 1, "Y");
  c.push_back(0);
  tag(c, 10, "block_states");
  tag(c, 9, "palette");
  c.push_back(10);
  u32(c, 17);
  for (int i = 0; i < 17; i++) {
    stringtag(c, "Name", "example:block_" + std::to_string(i));
    c.push_back(0);
  }
  tag(c, 12, "data");
  u32(c, 342);
  for (int i = 0; i < 342; i++) {
    uint64_t value = i == 0 ? (uint64_t(16) << 55) : (i == 1 ? 1 : 0);
    for (int j = 7; j >= 0; j--)
      c.push_back(uint8_t(value >> (j * 8)));
  }
  c.push_back(0);
  c.push_back(0);
  tag(c, 9, "block_entities");
  c.push_back(10);
  u32(c, 1);
  stringtag(c, "id", "minecraft:piston");
  integer(c, "x", -16);
  integer(c, "y", 0);
  integer(c, "z", -16);
  tag(c, 10, "blockState");
  stringtag(c, "Name", "minecraft:stone");
  c.push_back(0);
  tag(c, 1, "facing");
  c.push_back(5);
  tag(c, 5, "progress");
  u32(c, 0x3f000000);
  tag(c, 1, "extending");
  c.push_back(1);
  tag(c, 1, "source");
  c.push_back(0);
  c.push_back(0);
  c.push_back(0);
  auto data = compressed(c, 15);
  Bytes region(12288);
  int slot = 1023;
  region[slot * 4 + 2] = 2;
  region[slot * 4 + 3] = 1;
  auto len = uint32_t(data.size() + 1);
  region[8192] = uint8_t(len >> 24);
  region[8193] = uint8_t(len >> 16);
  region[8194] = uint8_t(len >> 8);
  region[8195] = uint8_t(len);
  region[8196] = 2;
  std::copy(data.begin(), data.end(), region.begin() + 8197);
  write(p / "region" / "r.-1.-1.mca", region);
  auto w = loadAnvilWorld(p, p / "region");
  assert(w.gameTime == 42);
  assert(w.chunks.size() == 1 && w.chunks[0] == std::make_pair(-1, -1));
  assert(w.blocks.at({-16, -16, -16}).name == "example:unknown");
  assert(w.blocks.at({-1, -1, -1}).properties.at("foo") == "bar");
  assert(w.blocks.at({-5, 0, -16}).name == "example:block_16");
  assert(w.blocks.at({-4, 0, -16}).name == "example:block_1");
  World world;
  importWorld(w, world);
  LoadedWorld saved;
  saved.gameTime = 100;
  saved.chunks = {{-1, 0}, {0, 0}, {1, 0}};
  saved.ticks = {{{0, 82, 5}, "minecraft:smooth_stone", 2, 0, false},
                 {{-1, 82, 5}, "minecraft:smooth_stone", 2, 0, false},
                 {{1, 82, 5}, "minecraft:smooth_stone", 2, 0, false},
                 {{0, 82, 5}, "minecraft:smooth_stone", 2, 0, false},
                 {{2, 82, 5}, "minecraft:water", 2, 0, true},
                 {{16, 82, 5}, "minecraft:smooth_stone", 2, 0, false}};
  World restored;
  importWorld(saved, restored);
  assert(restored.scheduledTicks.size() == 5);
  assert(restored.scheduledTicks[0].order == -3);
  assert(restored.scheduledTicks[1].order == -1);
  assert(restored.scheduledTicks[2].order == -2);
  assert(restored.scheduledTicks[3].order == -1);
  assert(restored.scheduledTicks[4].order == -1);
  assert(restored.scheduledTicks[0].trigger == 102);
  restored.scheduleTick({17, 82, 5}, "minecraft:smooth_stone", 2, 0);
  assert(restored.scheduledTicks.back().order == 0);
  restored.traceUpdates = true;
  restored.tick();
  restored.tick();
  std::vector<Pos> restoredOrder;
  for (const auto &entry : restored.log)
    if (entry.kind == "scheduled_tick")
      restoredOrder.push_back(entry.pos);
  assert(restoredOrder.size() == 6);
  assert((restoredOrder[0] == Pos{0, 82, 5}));
  assert((restoredOrder[1] == Pos{1, 82, 5}));
  assert((restoredOrder[5] == Pos{17, 82, 5}));
  auto moving = world.pistonEntities.at({-16, 0, -16});
  assert(moving->movedState.name == "minecraft:stone" &&
         moving->direction == Direction::East && moving->progress == 0.5f &&
         moving->extending);
  for (bool raw : {false, true}) {
    auto l = lz4(c, raw);
    assert(l.size() < 4091);
    Bytes r(12288);
    r[slot * 4 + 2] = 2;
    r[slot * 4 + 3] = 1;
    auto length = uint32_t(l.size() + 1);
    r[8192] = uint8_t(length >> 24);
    r[8193] = uint8_t(length >> 16);
    r[8194] = uint8_t(length >> 8);
    r[8195] = uint8_t(length);
    r[8196] = 4;
    std::copy(l.begin(), l.end(), r.begin() + 8197);
    write(p / "region" / "r.-1.-1.mca", r);
    auto loaded = loadAnvilWorld(p, p / "region");
    assert(loaded.blocks.at({-5, 0, -16}).name == "example:block_16");
    r[8197 + 17] ^= 1;
    write(p / "region" / "r.-1.-1.mca", r);
    bool bad = false;
    try {
      loadAnvilWorld(p, p / "region");
    } catch (const nbt::Error &) {
      bad = true;
    }
    assert(bad);
  }
  region[8195] = 255;
  write(p / "region" / "r.-1.-1.mca", region);
  bool failed = false;
  try {
    loadAnvilWorld(p, p / "region");
  } catch (const nbt::Error &) {
    failed = true;
  }
  assert(failed);
  failed = false;
  try {
    nbt::parse(Bytes{10, 0, 0, 3, 0, 1, 'x', 0});
  } catch (const nbt::Error &) {
    failed = true;
  }
  assert(failed);
  std::filesystem::remove_all(p);
}
