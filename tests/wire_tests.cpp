#include "redstone/nbt.hpp"
#include "redstone/net/wire.hpp"
#include "redstone/piston.hpp"
#include <bit>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

using namespace redstone;
using namespace redstone::net;
template <class F> void rejects(F f) {
  bool caught = false;
  try {
    f();
  } catch (const ProtocolError &) {
    caught = true;
  }
  assert(caught);
}
static void scalarAndFraming() {
  Writer w;
  for (int n : {0, 127, 128, 2097151, std::numeric_limits<int>::max(), -1,
                std::numeric_limits<int>::min()})
    w.varInt(n);
  w.position({-33554432, -2048, 33554431});
  w.position({-1, -64, -17});
  w.f32(-0.125f);
  w.f64(123.125);
  w.string("minecraft:air");
  Reader r(w.data);
  for (int n : {0, 127, 128, 2097151, std::numeric_limits<int>::max(), -1,
                std::numeric_limits<int>::min()})
    assert(r.varInt() == n);
  assert((r.position() == Pos{-33554432, -2048, 33554431}));
  assert((r.position() == Pos{-1, -64, -17}));
  assert(r.f32() == -0.125f);
  assert(r.f64() == 123.125);
  assert(r.string() == "minecraft:air");
  assert(r.empty());
  rejects([&] { r.u8(); });
  Bytes bad{255, 255, 255, 255, 16};
  rejects([&] { Reader(bad).varInt(); });
  auto bytes = frame(300, w.data);
  Bytes input;
  for (std::size_t i = 0; i < bytes.size() - 1; ++i) {
    input.push_back(bytes[i]);
    auto old = input;
    assert(!popFrame(input));
    assert(input == old);
  }
  input.push_back(bytes.back());
  auto body = popFrame(input);
  assert(body && input.empty());
  Reader p(*body);
  assert(p.varInt() == 300);
  assert(p.raw(p.remaining()).size() == w.data.size());
  auto first = frame(1, {}), second = frame(2, {});
  first.insert(first.end(), second.begin(), second.end());
  assert(Reader(*popFrame(first)).varInt() == 1);
  assert(Reader(*popFrame(first)).varInt() == 2);
  bad = {128, 128, 128};
  rejects([&] { popFrame(bad); });
  bad = {0};
  rejects([&] { popFrame(bad); });
  bad = {128, 128, 128, 1};
  rejects([&] { popFrame(bad); });
}
static std::vector<int> readPalette(Reader &r, int count, bool blocks) {
  int bits = r.u8();
  std::vector<int> palette;
  if (!bits)
    return std::vector<int>(count, r.varInt());
  bool local = bits <= (blocks ? 8 : 3);
  if (local) {
    int n = r.varInt();
    assert(n > 0 && n <= 256);
    for (int i = 0; i < n; ++i)
      palette.push_back(r.varInt());
  }
  int perLong = 64 / bits;
  std::uint64_t mask = (std::uint64_t(1) << bits) - 1;
  std::vector<int> values;
  while (int(values.size()) < count) {
    auto word = std::bit_cast<std::uint64_t>(r.i64());
    for (int j = 0; j < perLong && int(values.size()) < count; ++j) {
      int n = int((word >> (j * bits)) & mask);
      if (local) {
        assert(n < int(palette.size()));
        n = palette[n];
      }
      values.push_back(n);
    }
  }
  return values;
}
static void chunks(const std::filesystem::path &assets) {
  Registry registry(assets);
  assert(registry.stateById(registry.stateId(defaultState("air"))) ==
         defaultState("air"));
  assert(registry.itemName(registry.itemId("minecraft:white_carpet")) ==
         "minecraft:white_carpet");
  rejects([&] { registry.stateById(-1); });
  rejects([&] { registry.stateId(BlockState("minecraft:missing")); });
  World world;
  world.loadChunk(-1, -2);
  // Exercise a 5-bit palette (not a divisor of 64) and negative chunk coords.
  std::vector<int> expected;
  for (int x = 0; x < 16; ++x) {
    auto s = defaultState("white_terracotta");
    world.rawSetBlock({-16 + x, -64, -32}, s);
    expected.push_back(registry.stateId(s));
  }
  for (int x = 0; x < 16; ++x) {
    auto s = defaultState("observer")
                 .with("facing", directionName(static_cast<Direction>(x % 6)))
                 .with("powered", x < 6 ? "true" : "false");
    world.rawSetBlock({-16 + x, -63, -32}, s);
  }
  for (int x = 0; x < 16; ++x) {
    auto s = defaultState("piston")
                 .with("facing", directionName(static_cast<Direction>(x % 6)))
                 .with("extended", x < 6 ? "true" : "false");
    world.rawSetBlock({-16 + x, -62, -32}, s);
  }
  auto bytes = encodeChunk(world, -1, -2, registry);
  Reader r(bytes);
  assert(r.i32() == -1);
  assert(r.i32() == -2);
  assert(r.varInt() == 3);
  for (int type : {1, 4, 5}) {
    assert(r.varInt() == type);
    int n = r.varInt();
    assert(n == 37);
    for (int i = 0; i < n; ++i)
      r.i64();
  }
  auto sectionBytes = r.raw(r.varInt());
  Reader sections(sectionBytes);
  for (int sy = -64; sy < 320; sy += 16) {
    auto start = sections.pos;
    int count = sections.i16();
    assert(count == (sy == -64 ? 48 : 0));
    assert(sections.i16() == 0);
    auto states = readPalette(sections, 4096, true);
    auto biomes = readPalette(sections, 64, false);
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          assert(states[(y * 16 + z) * 16 + x] ==
                 registry.stateId(world.getBlock({-16 + x, sy + y, -32 + z})));
    for (int b : biomes)
      assert(b == registry.biomeId("minecraft:plains"));
    if (sy != -64) {
      std::ifstream fixture(assets / "empty_section.bin", std::ios::binary);
      Bytes expectedBytes((std::istreambuf_iterator<char>(fixture)), {});
      assert(!expectedBytes.empty());
      assert(Bytes(sectionBytes.begin() + start,
                   sectionBytes.begin() + sections.pos) == expectedBytes);
    }
  }
  assert(sections.empty());
  assert(r.varInt() == 0); // block entities
  assert(r.varInt() == 1);
  assert(r.i64() == ((1LL << 26) - 1));
  assert(r.varInt() == 0);
  assert(r.varInt() == 0);
  assert(r.varInt() == 1);
  assert(r.i64() == ((1LL << 26) - 1));
  assert(r.varInt() == 26);
  for (int i = 0; i < 26; ++i) {
    assert(r.varInt() == 2048);
    for (auto b : r.raw(2048))
      assert(b == 255);
  }
  assert(r.varInt() == 0);
  assert(r.empty());
  World heightWorld;
  heightWorld.loadChunk(0, 0);
  heightWorld.rawSetBlock({0, 5, 0}, defaultState("water"));
  heightWorld.rawSetBlock({0, 10, 0}, BlockState("minecraft:oak_leaves",
                                                 {{"distance", "7"},
                                                  {"persistent", "false"},
                                                  {"waterlogged", "false"}}));
  auto heightChunk = encodeChunk(heightWorld, 0, 0, registry);
  Reader hr(heightChunk);
  hr.raw(8);
  assert(hr.varInt() == 3);
  for (int i = 0; i < 3; ++i) {
    assert(hr.varInt() == (i == 0 ? 1 : i + 3));
    int longs = hr.varInt();
    // WORLD_SURFACE/MOTION_BLOCKING include leaves; NO_LEAVES sees water.
    assert((hr.i64() & 511) == (i == 2 ? 70 : 75));
    hr.raw((longs - 1) * 8);
  }
  Reader hs(hr.raw(hr.varInt()));
  for (int i = 0; i < 24; ++i) {
    assert(hs.i16() == (i == 4 ? 2 : 0));
    assert(hs.i16() == (i == 4 ? 1 : 0));
    readPalette(hs, 4096, true);
    readPalette(hs, 64, false);
  }
  assert(hs.empty());
  auto piston = std::make_shared<PistonMovingBlockEntity>();
  piston->pos = {-1, 0, -31};
  piston->movedState = defaultState("white_terracotta");
  piston->direction = Direction::West;
  piston->extending = false;
  piston->source = true;
  piston->progress = 1.0f;
  piston->progressO = 0.5f;
  world.rawSetBlock(piston->pos,
                    defaultState("moving_piston").with("facing", "west"));
  world.setPistonEntity(piston->pos, piston);
  auto movingNbt = encodeMovingPistonNbt(*piston);
  Bytes namedNbt{10, 0, 0};
  namedNbt.insert(namedNbt.end(), movingNbt.begin() + 1, movingNbt.end());
  auto tag = nbt::parse(namedNbt);
  assert(tag.get("progress")->real == 0.5); // saves progressO, not progress
  assert(tag.get("facing")->number == 4);
  assert(tag.get("facing")->type == 1);
  assert(tag.get("source")->number == 1);
  assert(tag.get("extending")->number == 0);
  assert(tag.get("blockState")->get("Name")->text ==
         "minecraft:white_terracotta");
  auto movingChunk = encodeChunk(world, -1, -2, registry);
  // Optional binary fixture, decoded by the actual Java 26.1 PLAY codec.
  if (const char *file = std::getenv("REDSTONE_WIRE_FIXTURE")) {
    Writer record, packet;
    packet.varInt(45);
    packet.raw(bytes);
    record.i32(static_cast<std::int32_t>(packet.data.size()));
    record.raw(packet.data);
    packet.data.clear();
    packet.varInt(45);
    packet.raw(movingChunk);
    record.i32(static_cast<std::int32_t>(packet.data.size()));
    record.raw(packet.data);
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char *>(record.data.data()),
              static_cast<std::streamsize>(record.data.size()));
  }
}
int main(int argc, char **argv) {
  scalarAndFraming();
  const auto assets = argc > 1 ? std::filesystem::path(argv[1])
                               : std::filesystem::path("assets/protocol775");
  chunks(assets);
  std::cout << "wire tests passed\n";
}
