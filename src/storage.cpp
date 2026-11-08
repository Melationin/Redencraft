#include "redstone/storage.hpp"
#include "redstone/entity.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
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
  const std::string name = required(t, "Name", 8).text;
  std::map<std::string, std::string> properties;
  if (auto p = t.get("Properties")) {
    if (p->type != 10)
      throw nbt::Error("invalid block properties");
    for (auto &[k, v] : p->compound) {
      if (v.type != 8)
        throw nbt::Error("invalid block property");
      properties[k] = v.text;
    }
  }
  return BlockState(name, std::move(properties));
}
double real(const Tag &t) {
  if (t.type != 5 && t.type != 6)
    throw nbt::Error("invalid real number");
  return t.real;
}
Vec3 vec3(const Tag &t, const char *name) {
  auto list = t.get(name);
  if (!list || list->type != 9 || list->list.size() != 3)
    throw nbt::Error(std::string("invalid or missing ") + name);
  return {real(list->list[0]), real(list->list[1]), real(list->list[2])};
}
Vec3 entityPosition(const Tag &tag) {
  auto pos = vec3(tag, "Pos");
  // Entity.load clamps before checking finiteness. In particular, a finite
  // huge coordinate must never reach C++'s undefined double-to-int conversion.
  pos.x = std::clamp(pos.x, -3.0000512E7, 3.0000512E7);
  pos.y = std::clamp(pos.y, -2.0E7, 2.0E7);
  pos.z = std::clamp(pos.z, -3.0000512E7, 3.0000512E7);
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
    throw nbt::Error("invalid entity position");
  return pos;
}
std::optional<Vec3> optionalVec3(const Tag &t, const char *name) {
  if (!t.get(name))
    return std::nullopt;
  return vec3(t, name);
}
int shortOr(const Tag &t, const char *name, int fallback) {
  auto value = t.get(name);
  if (!value)
    return fallback;
  if (value->type != 2)
    throw nbt::Error(std::string("invalid ") + name);
  return static_cast<int>(static_cast<std::int16_t>(value->number));
}
int intOr(const Tag &t, const char *name, int fallback) {
  auto value = t.get(name);
  if (!value)
    return fallback;
  if (value->type != 3)
    throw nbt::Error(std::string("invalid ") + name);
  return static_cast<int>(value->number);
}
bool boolOr(const Tag &t, const char *name, bool fallback) {
  auto value = t.get(name);
  if (!value)
    return fallback;
  if (value->type != 1)
    throw nbt::Error(std::string("invalid ") + name);
  return value->number != 0;
}
float floatOr(const Tag &t, const char *name, float fallback) {
  auto value = t.get(name);
  if (!value)
    return fallback;
  if (value->type != 5)
    throw nbt::Error(std::string("invalid ") + name);
  return static_cast<float>(value->real);
}
ItemStack itemStackTag(const Tag &t) {
  auto id = t.get("id");
  if (!id || id->type != 8)
    throw nbt::Error("invalid item id");
  int count = 1;
  if (auto countTag = t.get("count")) {
    if (countTag->type != 3 && countTag->type != 1)
      throw nbt::Error("invalid item count");
    count = static_cast<int>(countTag->number);
  } else if (auto legacyCount = t.get("Count")) {
    if (legacyCount->type != 1)
      throw nbt::Error("invalid item Count");
    count = static_cast<int>(legacyCount->number);
  }
  return itemStack(id->text, count);
}
void applyRotation(Entity &entity, const Tag &tag) {
  entity.yaw = entity.pitch = 0;
  if (auto rotation = tag.get("Rotation"); rotation) {
    if (rotation->type != 9 || rotation->list.size() != 2)
      throw nbt::Error("invalid entity rotation");
    entity.yaw = static_cast<float>(real(rotation->list[0]));
    entity.pitch = static_cast<float>(real(rotation->list[1]));
  }
  if (!std::isfinite(entity.yaw) || !std::isfinite(entity.pitch))
    throw nbt::Error("invalid entity rotation");
  auto motion = optionalVec3(tag, "Motion").value_or(Vec3{});
  auto component = [](double value) {
    if (std::abs(value) > 10.0)
      return 0.0; // Entity.load ignores each excessive component independently.
    if (!std::isfinite(value))
      // Unlike Java's floating-point math, C++ floor-to-int callers cannot
      // safely continue with NaN. Reject this malformed input at the boundary.
      throw nbt::Error("invalid entity motion");
    return value;
  };
  entity.deltaMovement = {component(motion.x), component(motion.y),
                          component(motion.z)};
  entity.onGround = boolOr(tag, "OnGround", false);
  entity.remainingFireTicks = shortOr(tag, "Fire", 0);
  entity.setOldPosAndRot();
}
void appendEntities(LoadedWorld &w, const Tag &list) {
  if (list.type != 9)
    throw nbt::Error("invalid entities");
  for (const auto &entity : list.list) {
    if (entity.type != 10)
      throw nbt::Error("invalid entity");
    w.entities.push_back(entity);
  }
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
        if (!s.isAir())
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
  // SerializableChunkData's legacy terrain entities use lowercase. Keep the
  // former uppercase spelling as an alias, but never append both aliases.
  if (auto es = root.get("entities"))
    appendEntities(w, *es);
  else if (auto alias = root.get("Entities"))
    appendEntities(w, *alias);
}
bool isSimpleEntity(const std::string &id) {
  auto name = id.find(':') == std::string::npos ? "minecraft:" + id : id;
  return name == "minecraft:item" || name == "minecraft:tnt" ||
         name == "minecraft:falling_block" || name == "minecraft:minecart";
}
void importEntity(World &world, const Tag &tag) {
  auto id = tag.get("id");
  if (!id || id->type != 8)
    throw nbt::Error("invalid entity id");
  if (!isSimpleEntity(id->text))
    // TODO: import the remaining registered entity types (living entities,
    // boats, TNT minecarts, projectiles). They stay in LoadedWorld for
    // reporting and are not converted to entities.
    return;
  auto pos = entityPosition(tag);
  if (!world.isChunkLoaded(chunkCoord(static_cast<int>(std::floor(pos.x))),
                            chunkCoord(static_cast<int>(std::floor(pos.z)))))
    return; // Entity records never create/load terrain chunks.
  if (id->text == "minecraft:item" || id->text == "item") {
    auto itemTag = tag.get("Item");
    ItemStack stack;
    if (itemTag) {
      if (itemTag->type != 10)
        throw nbt::Error("invalid item entity stack");
      stack = itemStackTag(*itemTag);
    }
    if (stack.empty())
      return;
    auto entity = std::make_shared<ItemEntity>(world, pos, stack);
    entity->age = shortOr(tag, "Age", 0);
    entity->pickupDelay = shortOr(tag, "PickupDelay", 0);
    entity->health = shortOr(tag, "Health", 5);
    applyRotation(*entity, tag);
    world.addEntity(entity);
    return;
  }
  if (id->text == "minecraft:tnt" || id->text == "tnt") {
    auto entity = std::make_shared<PrimedTnt>(world, pos);
    entity->fuse = shortOr(tag, "fuse", 80);
    if (auto block = tag.get("block_state")) {
      if (block->type != 10)
        throw nbt::Error("invalid tnt block_state");
      entity->blockState = state(*block);
    }
    entity->explosionPower = std::clamp(floatOr(tag, "explosion_power", 4.0f),
                                        0.0f, 128.0f);
    applyRotation(*entity, tag);
    world.addEntity(entity);
    return;
  }
  if (id->text == "minecraft:falling_block" ||
      id->text == "falling_block") {
    auto block = tag.get("BlockState");
    if (!block || block->type != 10)
      throw nbt::Error("invalid falling_block BlockState");
    auto entity =
        std::make_shared<FallingBlockEntity>(world, pos, state(*block));
    entity->time = intOr(tag, "Time", 0);
    entity->dropItem = boolOr(tag, "DropItem", true);
    entity->hurtEntities = boolOr(tag, "HurtEntities", false);
    entity->fallDamagePerDistance = floatOr(tag, "FallHurtAmount", 0.0f);
    entity->fallDamageMax = intOr(tag, "FallHurtMax", 40);
    entity->cancelDrop = boolOr(tag, "CancelDrop", false);
    applyRotation(*entity, tag);
    world.addEntity(entity);
    return;
  }
  if (id->text == "minecraft:minecart" || id->text == "minecart") {
    auto entity = std::make_shared<Minecart>(world, pos);
    entity->flipped = boolOr(tag, "FlippedRotation", false);
    // AbstractMinecart reads/writes firstTick directly as HasTicked despite
    // the counterintuitive name. Do not invert it.
    entity->firstTick = boolOr(tag, "HasTicked", false);
    applyRotation(*entity, tag);
    world.addEntity(entity);
  }
}
template <class Keep, class Consume>
void readRegions(const std::filesystem::path &regionDir, Keep keep,
                 Consume consume) {
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
      if (!keep(cx, cz))
        continue;
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
      consume(nbt::parse(payload), cx, cz);
    }
  }
}
} // namespace
std::filesystem::path defaultRegionDirectory(const std::filesystem::path &worldDir) {
  const auto modern = worldDir / "dimensions" / "minecraft" / "overworld" / "region";
  if (std::filesystem::is_directory(modern))
    return modern;
  const auto legacy = worldDir / "region";
  return std::filesystem::is_directory(legacy) ? legacy : modern;
}
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
  // 26.1 moved WorldGenSettings into namespaced SavedData. The old embedded
  // compound is accepted as a compatibility fallback for synthetic fixtures.
  const auto settingsPath = worldDir / "data" / "minecraft" / "world_gen_settings.dat";
  if (std::filesystem::exists(settingsPath)) {
    auto settings = nbt::parse(nbt::decompress(file(settingsPath), 31));
    if (integer(settings, "DataVersion") != 4786)
      throw nbt::Error("unsupported world generation DataVersion (expected 26.1: 4786)");
    w.seed = required(required(settings, "data", 10), "seed", 4).number;
  } else if (auto settings = data.get("WorldGenSettings"))
    w.seed = required(*settings, "seed", 4).number;
  // TODO: vanilla falls back to random WorldOptions when SavedData cannot be
  // read. This deterministic simulator uses zero if both seed sources are
  // absent; malformed present sources are rejected instead of randomized.
  readRegions(regionDir, [](int, int) { return true; },
              [&](const Tag &tag, int x, int z) { chunk(w, tag, x, z); });
  const std::set<std::pair<int, int>> loadedChunks(w.chunks.begin(), w.chunks.end());
  const auto entityDir = regionDir.parent_path() / "entities";
  if (std::filesystem::exists(entityDir))
    readRegions(entityDir,
                [&](int x, int z) { return loadedChunks.contains({x, z}); },
                [&](const Tag &tag, int x, int z) {
                  if (integer(tag, "DataVersion") != 4786)
                    throw nbt::Error("unsupported entity DataVersion (expected 26.1: 4786)");
                  const auto &pos = required(tag, "Position", 11).ints;
                  if (pos.size() != 2 || pos[0] != x || pos[1] != z)
                    throw nbt::Error("entity chunk Position does not match region location");
                  appendEntities(w, required(tag, "Entities", 9));
                });
  return w;
}
namespace {
// Java long addition wraps modulo 2^64 (SavedTick.java:49-51). Go through the
// unsigned type so the C++ side is defined behaviour instead of UB.
std::int64_t wrappingAdd(std::int64_t a, std::int64_t b) {
  return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) +
                                   static_cast<std::uint64_t>(b));
}
// TickPriority.byValue: values below -3 clamp to EXTREMELY_HIGH and values
// above 3 clamp to EXTREMELY_LOW (TickPriority.java:21-29).
int tickPriorityByValue(int value) {
  if (value < -3)
    return -3;
  if (value > 3)
    return 3;
  return value;
}
} // namespace
// TODO: non-piston block entities remain in LoadedWorld for reporting; their
// simulation is not implemented.
void importWorld(const LoadedWorld &input, World &world) {
  world.setGameTime(input.gameTime);
  world.setSeed(static_cast<std::uint64_t>(input.seed));
  for (auto [x, z] : input.chunks)
    world.loadChunk(x, z);
  for (auto &[pos, block] : input.blocks)
    world.rawSetBlock(pos, block);
  world.primeHeightmaps();
  // LevelChunkTicks.unpack assigns -N..-1 in saved-list order per chunk.
  // Its scheduleUnchecked path preserves duplicate saved entries. Unknown
  // vanilla block IDs cannot yet be validated against the complete registry.
  //
  // LevelChunk.java:639-641 unpacks blockTicks and fluidTicks as two separate
  // containers, so each family gets its OWN -N..-1 base: a chunk with two saved
  // block ticks and one saved fluid tick yields block orders {-2,-1} and a
  // fluid order {-1}, not one shared counter.
  const auto chunkOf = [](const SavedTickData &tick) {
    return std::make_pair(chunkCoord(tick.pos.x), chunkCoord(tick.pos.z));
  };
  std::map<std::pair<int, int>, std::int64_t> blockNext, fluidNext;
  for (const auto &tick : input.ticks) {
    if (!world.isLoaded(tick.pos))
      continue;
    auto &table = tick.fluid ? fluidNext : blockNext;
    ++table[chunkOf(tick)];
  }
  for (auto &[chunk, count] : blockNext)
    count = -count;
  for (auto &[chunk, count] : fluidNext)
    count = -count;
  for (const auto &tick : input.ticks) {
    if (!world.isLoaded(tick.pos))
      continue;
    const auto chunk = chunkOf(tick);
    const auto name = tick.type.find(':') == std::string::npos
                          ? "minecraft:" + tick.type
                          : tick.type;
    // SavedTick.unpack is `currentTick + delay` in Java's wrapping long
    // arithmetic (SavedTick.java:49-51), and 26.1 streams the delay as a
    // 32-bit int, so an extreme NBT Time must wrap exactly like the JVM
    // rather than trap on C++ signed overflow.
    const std::int64_t trigger = wrappingAdd(input.gameTime, tick.delay);
    // TickPriority.byValue clamps out-of-range values onto the nearest end
    // (TickPriority.java:21-29) instead of rejecting them.
    const int priority = tickPriorityByValue(tick.priority);
    // A schematic that recorded subTickOrder keeps its own order; vanilla
    // saves have none and use the per-family -N..-1 sequence instead.
    const std::int64_t order = tick.subTick.has_value()
                                   ? *tick.subTick
                                   : (tick.fluid ? fluidNext[chunk]
                                                 : blockNext[chunk])++;
    if (tick.fluid) {
      // ServerLevel.fluidTicks stores the Fluid identity itself, so an unknown
      // or empty name is a data error rather than a silent drop. The explicit
      // minecraft:empty entry is a real identity and is restored as one.
      const auto fluid = FluidRef(name);
      if (fluid.empty())
        throw nbt::Error("unknown fluid in fluid_ticks: " + name);
      world.restoreFluidTick({tick.pos, BlockRef{}, trigger, priority, order},
                             fluid);
      continue;
    }
    // LevelChunkTicks.unpack: scheduleUnchecked keeps duplicate saved entries.
    world.restoreScheduledTick({tick.pos, name, trigger, priority, order});
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
  std::set<std::array<std::int32_t, 4>> importedUuids;
  for (auto &tag : input.entities) {
    std::optional<std::array<std::int32_t, 4>> key;
    auto id = tag.get("id");
    if (id && id->type == 8 && isSimpleEntity(id->text)) {
      if (auto uuid = tag.get("UUID"); uuid && uuid->type == 11 && uuid->ints.size() == 4) {
        key = std::array<std::int32_t, 4>{uuid->ints[0], uuid->ints[1],
                                        uuid->ints[2], uuid->ints[3]};
        if (importedUuids.contains(*key))
          continue; // Legacy terrain records precede modern entity storage.
      }
    }
    const auto before = world.entities.size();
    importEntity(world, tag);
    if (key && world.entities.size() != before)
      importedUuids.insert(*key);
  }
}
} // namespace redstone
