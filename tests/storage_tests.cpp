#include "redstone/piston.hpp"
#include "redstone/entity.hpp"
#include "redstone/storage.hpp"
#include <cassert>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
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
  b.push_back(uint8_t(t));
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
static void u64(Bytes &b, uint64_t value) {
  for (int i = 7; i >= 0; --i)
    b.push_back(uint8_t(value >> (8 * i)));
}
static void longtag(Bytes &b, const std::string &name, int64_t value) {
  tag(b, 4, name);
  u64(b, uint64_t(value));
}
static void doubles(Bytes &b, const std::string &name,
                    std::initializer_list<double> values) {
  tag(b, 9, name);
  b.push_back(6);
  u32(b, uint32_t(values.size()));
  for (auto value : values)
    u64(b, std::bit_cast<uint64_t>(value));
}
static Bytes itemTag(int uuid, int count, bool motion = false) {
  Bytes b;
  stringtag(b, "id", "minecraft:item");
  doubles(b, "Pos", {-8.5, 64, -8.5});
  tag(b, 11, "UUID");
  u32(b, 4);
  for (int value : {0, 0, 0, uuid})
    u32(b, value);
  tag(b, 10, "Item");
  stringtag(b, "id", "minecraft:white_carpet");
  integer(b, "count", count);
  b.push_back(0);
  if (motion)
    doubles(b, "Motion", {11, -11, 0.3});
  b.push_back(0);
  return b;
}
static void entities(Bytes &b, const std::string &name,
                     const std::vector<Bytes> &values) {
  tag(b, 9, name);
  b.push_back(10);
  u32(b, uint32_t(values.size()));
  for (const auto &value : values)
    b.insert(b.end(), value.begin(), value.end());
}
static Bytes terrain(const std::vector<Bytes> &legacy = {},
                      const std::vector<Bytes> &alias = {}) {
  auto b = root();
  integer(b, "DataVersion", 4786);
  integer(b, "xPos", -1);
  integer(b, "zPos", -1);
  stringtag(b, "Status", "minecraft:full");
  if (!legacy.empty()) entities(b, "entities", legacy);
  if (!alias.empty()) entities(b, "Entities", alias);
  b.push_back(0);
  return b;
}
static Bytes entityChunk(const std::vector<Bytes> &values, int version = 4786,
                         int x = -1, int z = -1) {
  auto b = root();
  integer(b, "DataVersion", version);
  tag(b, 11, "Position");
  u32(b, 2); u32(b, x); u32(b, z);
  entities(b, "Entities", values);
  b.push_back(0);
  return b;
}
static void regionFixture(const std::filesystem::path &directory,
                          const Bytes &nbt, int compression = 2,
                          bool external = false, bool orphan = false) {
  std::filesystem::create_directories(directory);
  Bytes payload = compression == 1 ? compressed(nbt, 31)
                   : compression == 2 ? compressed(nbt, 15)
                   : compression == 4 ? lz4(nbt, false) : nbt;
  assert(payload.size() < 4091);
  Bytes region(12288);
  const int slot = orphan ? 0 : 1023;
  region[slot * 4 + 2] = 2;
  region[slot * 4 + 3] = 1;
  auto length = uint32_t(external ? 1 : payload.size() + 1);
  region[8192] = uint8_t(length >> 24);
  region[8193] = uint8_t(length >> 16);
  region[8194] = uint8_t(length >> 8);
  region[8195] = uint8_t(length);
  region[8196] = uint8_t(compression | (external ? 128 : 0));
  if (external)
    write(directory / "c.-1.-1.mcc", payload);
  else
    std::copy(payload.begin(), payload.end(), region.begin() + 8197);
  write(directory / (orphan ? "r.0.0.mca" : "r.-1.-1.mca"), region);
}
static void levelFixture(const std::filesystem::path &directory,
                         bool embedded = true) {
  std::filesystem::create_directories(directory);
  auto b = root();
  tag(b, 10, "Data");
  integer(b, "DataVersion", 4786);
  longtag(b, "Time", 42);
  longtag(b, "RandomSeed", 999); // Deliberately obsolete and ignored.
  if (embedded) {
    tag(b, 10, "WorldGenSettings");
    longtag(b, "seed", 77);
    b.push_back(0);
  }
  b.push_back(0); b.push_back(0);
  write(directory / "level.dat", compressed(b, 31));
}
static void entityStorageFixtures(const std::filesystem::path &directory) {
  levelFixture(directory);
  const auto canonicalRegion = directory / "dimensions" / "minecraft" / "overworld" / "region";
  assert(defaultRegionDirectory(directory) == canonicalRegion);
  std::filesystem::create_directories(directory / "region");
  assert(defaultRegionDirectory(directory) == directory / "region");
  std::filesystem::create_directories(canonicalRegion);
  assert(defaultRegionDirectory(directory) == canonicalRegion);
  const auto dimension = directory / "DIM-1";
  const auto settings = directory / "data" / "minecraft";
  std::filesystem::create_directories(settings);
  auto seed = root();
  integer(seed, "DataVersion", 4786);
  tag(seed, 10, "data"); longtag(seed, "seed", -123456789012345LL);
  seed.push_back(0); seed.push_back(0);
  write(settings / "world_gen_settings.dat", compressed(seed, 31));
  regionFixture(dimension / "region", terrain({itemTag(1, 2)}, {itemTag(9, 9)}));
  Bytes unknown;
  stringtag(unknown, "id", "example:unknown_without_position");
  unknown.push_back(0);
  const auto modern = entityChunk({itemTag(1, 63), itemTag(2, 3, true), unknown});
  // Unselected entity records (and even another dimension's entity directory)
  // must not create terrain chunks or be interpreted as this dimension.
  regionFixture(dimension / "entities", entityChunk({}, 1, 0, 0), 3, false, true);
  regionFixture(directory / "entities", entityChunk({}, 1));
  for (int compression : {1, 2, 3, 4}) {
    for (bool external : {false, true}) {
      regionFixture(dimension / "entities", modern, compression, external);
      auto loaded = loadAnvilWorld(directory, dimension / "region");
      assert(loaded.seed == -123456789012345LL);
      assert((loaded.chunks == std::vector<std::pair<int, int>>({{-1, -1}})));
      assert(loaded.entities.size() == 4); // Raw records retain UUID duplicates.
      World world;
      importWorld(loaded, world);
      assert(world.entities.size() == 2 && !world.isChunkLoaded(0, 0));
      auto first = dynamic_cast<ItemEntity *>(world.entities[0].get());
      auto second = dynamic_cast<ItemEntity *>(world.entities[1].get());
      assert(first && first->item.count == 2); // First UUID wins, not modern 63.
      assert(first->deltaMovement.lengthSqr() == 0 && first->yaw == 0);
      assert(second && second->item.count == 3);
      assert(second->deltaMovement.x == 0 && second->deltaMovement.y == 0 &&
             std::abs(second->deltaMovement.z - .3) < 1.0e-9);
    }
  }
  for (auto bad : {entityChunk({}, 4787), entityChunk({}, 4786, 0, -1)}) {
    regionFixture(dimension / "entities", bad);
    bool rejected = false;
    try { (void)loadAnvilWorld(directory, dimension / "region"); }
    catch (const nbt::Error &) { rejected = true; }
    assert(rejected);
  }
  regionFixture(dimension / "entities", modern);
  std::filesystem::remove(settings / "world_gen_settings.dat");
  assert(loadAnvilWorld(directory, dimension / "region").seed == 77);
  levelFixture(directory, false);
  assert(loadAnvilWorld(directory, dimension / "region").seed == 0);
}
static void entityNumericImport() {
  auto bytes = root();
  auto payload = itemTag(1, 1);
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  auto tag = nbt::parse(bytes);
  auto rejects = [&](nbt::Tag value) {
    LoadedWorld loaded;
    loaded.chunks = {{-1, -1}};
    loaded.entities = {std::move(value)};
    World world;
    bool rejected = false;
    try { importWorld(loaded, world); }
    catch (const nbt::Error &) { rejected = true; }
    assert(rejected);
  };
  auto bad = tag;
  bad.compound["Pos"].list[0].real = std::numeric_limits<double>::quiet_NaN();
  rejects(bad);
  bad = tag;
  bad.compound["Motion"] = bad.compound["Pos"];
  bad.compound["Motion"].list[1].real = std::numeric_limits<double>::quiet_NaN();
  rejects(bad);
  bad = tag;
  bad.compound["Rotation"] = bad.compound["Pos"];
  bad.compound["Rotation"].list.resize(2);
  bad.compound["Rotation"].list[0].real = std::numeric_limits<double>::infinity();
  rejects(bad);
  for (bool hasTicked : {false, true}) {
    auto cart = tag;
    cart.compound["id"].text = "minecraft:minecart";
    cart.compound["HasTicked"].type = 1;
    cart.compound["HasTicked"].number = hasTicked;
    LoadedWorld loaded;
    loaded.chunks = {{-1, -1}};
    loaded.entities = {cart};
    World world;
    importWorld(loaded, world);
    auto minecart = dynamic_cast<Minecart *>(world.entities[0].get());
    assert(minecart && minecart->firstTick == hasTicked);
  }
  for (double x : {1.0e300, -1.0e300}) {
    auto far = tag;
    far.compound["Pos"].list[0].real = x;
    LoadedWorld loaded;
    loaded.chunks = {{x > 0 ? 1875032 : -1875032, -1}};
    loaded.entities = {far};
    World world;
    importWorld(loaded, world);
    assert(world.entities.size() == 1);
    assert(world.entities[0]->position.x == (x > 0 ? 30000512 : -30000512));
    // A record whose position points at absent terrain is retained only as NBT.
    loaded.chunks = {{-1, -1}};
    World absent;
    importWorld(loaded, absent);
    assert(absent.entities.empty() && !absent.isChunkLoaded(1875032, -1));
  }
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
  // The real .mca payload carries the tick lists, so the disk reader path is
  // exercised rather than only the in-memory restore function. They are
  // siblings of block_entities inside the chunk compound.
  {
    auto tick = [&](const char *type, int x, int y, int z, int delay,
                    int priority) {
      stringtag(c, "i", type);
      integer(c, "x", x);
      integer(c, "y", y);
      integer(c, "z", z);
      integer(c, "t", delay);
      integer(c, "p", priority);
      c.push_back(0);
    };
    tag(c, 9, "block_ticks");
    c.push_back(10);
    u32(c, 1);
    tick("minecraft:smooth_stone", -16, -16, -16, 5, 0);
    tag(c, 9, "fluid_ticks");
    c.push_back(10);
    u32(c, 6);
    // A future source tick and its flowing counterpart at the same cell: the
    // two identities must both survive and stay distinct.
    tick("minecraft:water", -8, -16, -8, 5, 0);
    tick("minecraft:flowing_water", -8, -16, -8, 7, -2);
    // An already-overdue entry (negative delay) in the same chunk.
    tick("minecraft:flowing_lava", -7, -16, -8, -9, 0);
    // Duplicate (pos, fluid) pairs are kept, exactly like scheduleUnchecked.
    tick("minecraft:water", -6, -16, -8, 3, 0);
    tick("minecraft:water", -6, -16, -8, 3, 0);
    // An out-of-range priority must clamp, not fail the load.
    tick("minecraft:lava", -5, -16, -8, 1, 9);
  }
  c.push_back(0); // closes the chunk compound
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
  // The disk reader must surface both tick lists with their exact types.
  assert(w.ticks.size() == 7);
  {
    auto countFluid = 0;
    for (const auto &tick : w.ticks)
      if (tick.fluid)
        ++countFluid;
    assert(countFluid == 6);
    assert(w.ticks[0].type == "minecraft:smooth_stone" && !w.ticks[0].fluid);
    assert(w.ticks[1].type == "minecraft:water" && w.ticks[1].delay == 5);
    assert(w.ticks[2].type == "minecraft:flowing_water" &&
           w.ticks[2].priority == -2);
    assert(w.ticks[3].type == "minecraft:flowing_lava" &&
           w.ticks[3].delay == -9);
  }
  assert(w.chunks.size() == 1 && w.chunks[0] == std::make_pair(-1, -1));
  assert(w.blocks.at({-16, -16, -16}).name() == "example:unknown");
  assert(w.blocks.at({-1, -1, -1}).properties().at("foo") == "bar");
  assert(w.blocks.at({-5, 0, -16}).name() == "example:block_16");
  assert(w.blocks.at({-4, 0, -16}).name() == "example:block_1");
  World world;
  importWorld(w, world);
  // End-to-end: the same .mca bytes go through importWorld and land in the
  // dispatch queues, so a regression in the disk reader is caught here rather
  // than only in the in-memory restore path.
  assert(world.scheduledTickCount() == 1);
  assert(world.fluidTickCount() == 6);
  assert(world.gameTime() == 42);
  // Distinct exact identities at the same cell coexist.
  assert(world.hasFluidTick({-8, -16, -8}, FluidRef(Fluids::water())));
  assert(world.hasFluidTick({-8, -16, -8}, FluidRef(Fluids::flowingWater())));
  // Overdue (negative delay) and future entries both survive the restore.
  for (const auto &entry : world.fluidTicks()) {
    if (entry.first.pos == Pos{-7, -16, -8})
      assert(entry.first.trigger == 42 - 9);
    if (entry.first.pos == Pos{-8, -16, -8} &&
        entry.second.fluid() == &Fluids::water())
      assert(entry.first.trigger == 42 + 5);
    if (entry.first.pos == Pos{-5, -16, -8})
      // TickPriority.byValue clamps 9 onto EXTREMELY_LOW.
      assert(entry.first.priority == 3);
  }
  // Duplicate saved entries are kept, keyed apart only by their subTick.
  {
    auto duplicates = 0;
    for (const auto &entry : world.fluidTicks())
      if (entry.first.pos == Pos{-6, -16, -8}) {
        ++duplicates;
        assert(entry.first.trigger == 42 + 3);
      }
    assert(duplicates == 2);
  }
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
  assert(restored.scheduledTickCount() == 5);
  // A-02: the fluid queue is a SEPARATE LevelTicks container, so the saved
  // fluid_ticks entry above (chunk 0) restores with its own -N..-1 base and
  // keeps its exact fluid identity instead of being dropped.
  assert(restored.fluidTickCount() == 1);
  assert(restored.hasFluidTick({2, 82, 5}, FluidRef(Fluids::water())));
  // Exact identity: water and flowing_water never match each other.
  assert(!restored.hasFluidTick({2, 82, 5}, FluidRef(Fluids::flowingWater())));
  {
    const auto fluidTicks = restored.fluidTicks();
    assert(fluidTicks.size() == 1);
    assert(fluidTicks[0].first.trigger == 102);
    assert(fluidTicks[0].first.order == -1);
  }
  // LevelChunkTicks.unpack gives each chunk -N..-1 in saved-list order, so the
  // restored subTickOrders are negative and chunk 0 keeps both duplicates.
  std::vector<std::int64_t> restoredOrders;
  for (const auto &tick : restored.scheduledTicks()) {
    assert(tick.trigger == 102);
    restoredOrders.push_back(tick.order);
  }
  std::sort(restoredOrders.begin(), restoredOrders.end());
  assert((restoredOrders ==
          std::vector<std::int64_t>{-3, -2, -1, -1, -1}));
  restored.scheduleTick({17, 82, 5}, "minecraft:smooth_stone", 2, 0);
  assert(restored.scheduledTickCount() == 6);
  std::optional<ScheduledTick> fresh;
  for (const auto &tick : restored.scheduledTicks())
    if (tick.pos == Pos{17, 82, 5})
      fresh = tick;
  assert(fresh && fresh->order == 0);
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
  assert(moving->movedState.name() == "minecraft:stone" &&
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
    assert(loaded.blocks.at({-5, 0, -16}).name() == "example:block_16");
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
  // A-02: the two tick families get independent -N..-1 bases per saved list.
  {
    LoadedWorld mixed;
    mixed.gameTime = 100;
    mixed.chunks = {{0, 0}};
    mixed.ticks = {
        {{1, 70, 1}, "minecraft:smooth_stone", 3, 0, false},
        {{2, 70, 1}, "minecraft:smooth_stone", 4, 0, false},
        {{3, 70, 1}, "minecraft:water", 5, 0, true},
        {{4, 70, 1}, "minecraft:flowing_lava", 6, 1, true},
    };
    World world2;
    importWorld(mixed, world2);
    assert(world2.scheduledTickCount() == 2);
    assert(world2.fluidTickCount() == 2);
    // Block ticks are {-2,-1}; fluid ticks restart at {-2,-1} in their own
    // container rather than continuing the block counter.
    std::vector<std::int64_t> blockOrders, fluidOrders;
    for (const auto &tick : world2.scheduledTicks())
      blockOrders.push_back(tick.order);
    for (const auto &entry : world2.fluidTicks())
      fluidOrders.push_back(entry.first.order);
    std::sort(blockOrders.begin(), blockOrders.end());
    std::sort(fluidOrders.begin(), fluidOrders.end());
    assert((blockOrders == std::vector<std::int64_t>{-2, -1}));
    assert((fluidOrders == std::vector<std::int64_t>{-2, -1}));
    // Each entry keeps its own exact fluid and delay.
    assert(world2.hasFluidTick({3, 70, 1}, FluidRef(Fluids::water())));
    assert(world2.hasFluidTick({4, 70, 1}, FluidRef(Fluids::flowingLava())));
    assert(!world2.hasFluidTick({3, 70, 1}, FluidRef(Fluids::flowingWater())));
    for (const auto &entry : world2.fluidTicks()) {
      const auto delay = entry.first.trigger - 100;
      if (entry.first.pos == Pos{3, 70, 1})
        assert(delay == 5);
      else
        assert(delay == 6);
    }
  }
  // An explicit minecraft:empty saved entry is a real identity and restores.
  {
    LoadedWorld empties;
    empties.gameTime = 7;
    empties.chunks = {{0, 0}};
    empties.ticks = {{{5, 70, 5}, "minecraft:empty", 1, 0, true}};
    World world3;
    importWorld(empties, world3);
    assert(world3.fluidTickCount() == 1);
    assert(world3.hasFluidTick({5, 70, 5}, FluidRef(Fluids::empty())));
  }
  // SavedTick.unpack is Java wrapping arithmetic: an extreme delay must not be
  // C++ signed-overflow UB, and TickPriority.byValue clamps instead of failing.
  {
    LoadedWorld extreme;
    extreme.gameTime = std::numeric_limits<std::int64_t>::max() - 1;
    extreme.chunks = {{0, 0}};
    extreme.ticks = {
        {{1, 70, 1}, "minecraft:water", 5, 0, true},
        {{2, 70, 1}, "minecraft:flowing_water",
         std::numeric_limits<int>::max(), 99, true},
        {{3, 70, 1}, "minecraft:smooth_stone", 4, -99, false},
    };
    World wrapped;
    importWorld(extreme, wrapped);
    for (const auto &entry : wrapped.fluidTicks()) {
      if (entry.first.pos == Pos{1, 70, 1})
        assert(entry.first.priority == 0);
      // max-1 + INT_MAX wraps past INT64_MAX into the negative half.
      if (entry.first.pos == Pos{2, 70, 1}) {
        assert(entry.first.trigger < 0);
        assert(entry.first.priority == 3);
      }
    }
    for (const auto &tick : wrapped.scheduledTicks())
      if (tick.pos == Pos{3, 70, 1})
        assert(tick.priority == -3);
  }
  // Unknown and empty fluid names are data errors, not silent drops.
  for (const char *bad : {"minecraft:not_a_fluid", ""}) {
    LoadedWorld rejects;
    rejects.gameTime = 7;
    rejects.chunks = {{0, 0}};
    rejects.ticks = {{{5, 70, 5}, bad, 1, 0, true}};
    World world4;
    bool rejected = false;
    try {
      importWorld(rejects, world4);
    } catch (const nbt::Error &) {
      rejected = true;
    }
    assert(rejected);
  }
  entityStorageFixtures(p / "entity_world");
  entityNumericImport();
  std::filesystem::remove_all(p);
}
