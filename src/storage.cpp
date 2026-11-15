#include "redstone/storage.hpp"
#include "redstone/containers.hpp"
#include "redstone/entity.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <cstdio>
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
void appendEntities(std::vector<nbt::Tag> &out, const Tag &list) {
  if (list.type != 9)
    throw nbt::Error("invalid entities");
  for (const auto &entity : list.list) {
    if (entity.type != 10)
      throw nbt::Error("invalid entity");
    out.push_back(entity);
  }
}
struct AnvilChunkSink {
  int minY = -64;
  int maxY = 320;
  bool skipNonFullChunks = false;
  std::size_t skippedNonFullChunks = 0;
  std::vector<std::pair<int, int>> &chunks;
  std::vector<SavedTickData> &ticks;
  std::vector<nbt::Tag> &blockEntities;
  std::vector<nbt::Tag> &entities;
  std::function<void(int cx, int cz)> onChunkStart;
  std::function<void(int cx, int cz, int sy, std::span<const BlockState> states,
                     std::span<const std::uint16_t, 4096> indices)>
      onSection;
  std::function<void(int cx, int cz, PersistedChunkLight &&light)>
      onPersistedLight;
};
void decodeAnvilChunk(AnvilChunkSink &sink, const Tag &root, int cx, int cz) {
  if (integer(root, "xPos") != cx || integer(root, "zPos") != cz)
    throw nbt::Error("chunk coordinates do not match region location");
  if (integer(root, "DataVersion") != 4786)
    throw nbt::Error("unsupported chunk DataVersion (expected 26.1: 4786)");
  auto status = required(root, "Status", 8).text;
  if (status != "minecraft:full" && status != "full") {
    if (sink.skipNonFullChunks) {
      ++sink.skippedNonFullChunks;
      return;
    }
    throw nbt::Error("chunk is not fully generated: " + status);
  }
  sink.chunks.emplace_back(cx, cz);
  if (sink.onChunkStart)
    sink.onChunkStart(cx, cz);
  const auto *lightOnTag = root.get("isLightOn");
  const bool isLightOn = lightOnTag && lightOnTag->type == 1 && lightOnTag->number != 0;
  PersistedChunkLight persistedLight;
  persistedLight.isLightOn = isLightOn;
  std::array<std::uint16_t, 4096> indices{};
  if (auto sections = root.get("sections")) {
    if (sections->type != 9)
      throw nbt::Error("invalid sections");
    for (auto &sec : sections->list) {
      if (sec.type != 10)
        throw nbt::Error("invalid section");
      auto sy = required(sec, "Y", 1).number;
      if (isLightOn) {
        auto readNibble = [&](const char *key)
            -> std::optional<std::array<std::uint8_t, 2048>> {
          const Tag *t = sec.get(key);
          if (!t)
            return std::nullopt;
          if (t->type != 7 || t->bytes.size() != 2048)
            throw nbt::Error(std::string("invalid ") + key + " nibble array");
          std::array<std::uint8_t, 2048> arr{};
          for (std::size_t i = 0; i < 2048; ++i)
            arr[i] = static_cast<std::uint8_t>(t->bytes[i]);
          return arr;
        };
        auto bl = readNibble("BlockLight");
        auto sl = readNibble("SkyLight");
        if (bl || sl) {
          PersistedLightSection pls;
          pls.sectionY = static_cast<int>(sy);
          pls.blockLight = bl;
          pls.skyLight = sl;
          persistedLight.sections.push_back(std::move(pls));
        }
      }
      auto bs = sec.get("block_states");
      if (!bs)
        continue;
      if (bs->type != 10)
        throw nbt::Error("invalid block_states");
      auto &pal = required(*bs, "palette", 9).list;
      if (pal.empty() || pal.size() > 4096)
        throw nbt::Error("invalid block palette");
      std::vector<BlockState> states;
      states.reserve(pal.size());
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
      if (pal.size() > 1) {
        const std::uint64_t mask = (std::uint64_t(1) << bits) - 1;
        for (int i = 0; i < 4096; i++) {
          const unsigned index =
              (static_cast<uint64_t>(data->longs[i / per]) >>
               ((i % per) * bits)) &
              mask;
          if (index >= states.size())
            throw nbt::Error("block palette index out of range");
          indices[static_cast<std::size_t>(i)] =
              static_cast<std::uint16_t>(index);
        }
      }
      if (sink.onSection)
        sink.onSection(cx, cz, static_cast<int>(sy), states, indices);
    }
  }
  if (isLightOn && sink.onPersistedLight)
    sink.onPersistedLight(cx, cz, std::move(persistedLight));
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
          sink.ticks.push_back(std::move(v));
      }
    }
  if (auto es = root.get("block_entities")) {
    if (es->type != 9)
      throw nbt::Error("invalid block entities");
    for (auto &e : es->list) {
      if (e.type != 10)
        throw nbt::Error("invalid block entity");
      sink.blockEntities.push_back(e);
    }
  }
  // SerializableChunkData's legacy terrain entities use lowercase. Keep the
  // former uppercase spelling as an alias, but never append both aliases.
  if (auto es = root.get("entities"))
    appendEntities(sink.entities, *es);
  else if (auto alias = root.get("Entities"))
    appendEntities(sink.entities, *alias);
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
    if (auto uuid = tag.get("UUID");
        uuid && uuid->type == 11 && uuid->ints.size() == 4)
      entity->uuid = {uuid->ints[0], uuid->ints[1], uuid->ints[2],
                      uuid->ints[3]};
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
    if (bytes.empty())
      continue;
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
std::filesystem::path ticketDataPath(const std::filesystem::path &regionDirectory) {
  return regionDirectory.parent_path() / "data" / "minecraft" /
         "chunk_tickets.dat";
}

namespace {
class TemporaryPath {
public:
  explicit TemporaryPath(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~TemporaryPath() {
    if (!active_)
      return;
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  TemporaryPath(const TemporaryPath &) = delete;
  TemporaryPath &operator=(const TemporaryPath &) = delete;
  const std::filesystem::path &path() const { return path_; }
  void release() { active_ = false; }
private:
  std::filesystem::path path_;
  bool active_ = true;
};

TemporaryPath createExclusiveTemp(const std::filesystem::path &target,
                                  std::span<const std::uint8_t> bytes) {
  static std::atomic<std::uint64_t> serial{0};
  const auto base = target.filename().string();
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto candidate =
        target.parent_path() /
        (base + ".tmp." + std::to_string(serial.fetch_add(1)));
    std::FILE *out = std::fopen(candidate.string().c_str(), "wbx");
    if (!out) {
      if (errno == EEXIST)
        continue;
      throw nbt::Error("cannot create temporary for " + target.string());
    }
    const std::size_t written =
        std::fwrite(bytes.data(), 1, bytes.size(), out);
    const bool flushed = std::fflush(out) == 0;
    const bool closed = std::fclose(out) == 0;
    if (written != bytes.size() || !flushed || !closed) {
      std::error_code ignored;
      std::filesystem::remove(candidate, ignored);
      throw nbt::Error("cannot write temporary for " + target.string());
    }
    return TemporaryPath(candidate);
  }
  throw nbt::Error("cannot allocate exclusive temporary for " +
                   target.string());
}

// The target is never removed before the replacement file is complete.
// Existing temporary names are never truncated or followed: fopen "x" creates
// a fresh same-directory file and fails if the candidate already exists.
void replaceWithGzip(const std::filesystem::path &target,
                     std::span<const std::uint8_t> bytes,
                     const std::filesystem::path *backup = nullptr) {
  TemporaryPath temp = createExclusiveTemp(target, bytes);
  std::error_code ec;
  if (backup && std::filesystem::exists(target)) {
    const auto backupBytes = file(target);
    TemporaryPath backupTemp = createExclusiveTemp(*backup, backupBytes);
    std::filesystem::rename(backupTemp.path(), *backup, ec);
    if (ec)
      throw nbt::Error("cannot replace backup " + backup->string() + ": " +
                       ec.message());
    backupTemp.release();
  }
  std::filesystem::rename(temp.path(), target, ec);
  if (ec)
    throw nbt::Error("cannot replace " + target.string() + ": " +
                     ec.message());
  temp.release();
}
} // namespace

namespace {
constexpr std::array<const char *, 2> kClockNames = {
    "minecraft:overworld", "minecraft:the_end"};
nbt::RootTag emptyWorldClockDocument() {
  nbt::RootTag root;
  root.name.clear();
  root.value.type = 10;
  nbt::Tag version;
  version.type = 3;
  version.number = 4786;
  root.value.compound["DataVersion"] = version;
  root.value.compoundOrder.push_back("DataVersion");
  nbt::Tag data;
  data.type = 10;
  root.value.compound["data"] = std::move(data);
  root.value.compoundOrder.push_back("data");
  return root;
}
void eraseCompoundField(nbt::Tag &compound, const std::string &name) {
  compound.compound.erase(name);
  std::erase(compound.compoundOrder, name);
}
void setCompoundField(nbt::Tag &compound, const std::string &name,
                      nbt::Tag value) {
  if (!compound.compound.contains(name))
    compound.compoundOrder.push_back(name);
  compound.compound[name] = std::move(value);
}
std::int64_t wrappingClockAdd(std::int64_t value, int increment) {
  return static_cast<std::int64_t>(
      static_cast<std::uint64_t>(value) +
      static_cast<std::uint64_t>(static_cast<std::int64_t>(increment)));
}
} // namespace

WorldClocks::WorldClocks() : document_(emptyWorldClockDocument()) {}

WorldClocks WorldClocks::load(const std::filesystem::path &path) {
  WorldClocks result;
  if (!std::filesystem::exists(path))
    return result;
  const auto bytes = file(path);
  const bool gzip =
      bytes.size() >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
  // SavedDataStorage accepts both a compressed SavedData stream and an
  // uncompressed NBT stream. New writes always use the official gzip form.
  const auto raw = gzip ? nbt::decompress(bytes, 31) : bytes;
  result.document_ = nbt::parseRoot(raw);
  const auto &root = result.document_.value;
  if (integer(root, "DataVersion") != 4786)
    throw nbt::Error(
        "unsupported world_clocks DataVersion (expected 26.1: 4786)");
  auto &data = const_cast<nbt::Tag &>(required(root, "data", 10));
  for (std::size_t i = 0; i < kClockNames.size(); ++i) {
    const nbt::Tag *clock = data.get(kClockNames[i]);
    if (!clock)
      continue;
    if (clock->type != 10)
      throw nbt::Error(std::string("invalid world clock ") + kClockNames[i]);
    WorldClockState state;
    state.totalTicks = required(*clock, "total_ticks", 4).number;
    if (const nbt::Tag *partial = clock->get("partial_tick")) {
      if (partial->type != 5)
        throw nbt::Error(std::string("invalid partial_tick for ") +
                         kClockNames[i]);
      state.partialTick = static_cast<float>(partial->real);
      if (!std::isfinite(state.partialTick))
        throw nbt::Error(std::string("non-finite partial_tick for ") +
                         kClockNames[i]);
    }
    if (const nbt::Tag *rate = clock->get("rate")) {
      if (rate->type != 5)
        throw nbt::Error(std::string("invalid rate for ") + kClockNames[i]);
      state.rate = static_cast<float>(rate->real);
      if (!std::isfinite(state.rate) || state.rate <= 0.0f)
        throw nbt::Error(std::string("non-positive rate for ") +
                         kClockNames[i]);
    }
    if (const nbt::Tag *paused = clock->get("paused")) {
      if (paused->type != 1 ||
          (paused->number != 0 && paused->number != 1))
        throw nbt::Error(std::string("invalid paused for ") + kClockNames[i]);
      state.paused = paused->number != 0;
    }
    result.states_[i] = state;
  }
  return result;
}

const WorldClockState &
WorldClocks::state(BuiltinWorldClock clock) const {
  return states_[static_cast<std::size_t>(clock)];
}

bool WorldClocks::setClock(BuiltinWorldClock clock,
                           const WorldClockState &state) {
  if (!std::isfinite(state.partialTick) ||
      !std::isfinite(state.rate) || state.rate <= 0.0f)
    throw nbt::Error("invalid world clock state");
  auto &current = states_[static_cast<std::size_t>(clock)];
  if (current.totalTicks == state.totalTicks &&
      current.partialTick == state.partialTick &&
      current.rate == state.rate && current.paused == state.paused)
    return false;
  current = state;
  dirty_ = true;
  return true;
}

void WorldClocks::tick(bool runsNormally, bool advanceTime) {
  if (!runsNormally || !advanceTime)
    return;
  for (WorldClockState &clock : states_) {
    if (clock.paused)
      continue;
    const WorldClockState before = clock;
    // ServerClockManager.ClockInstance.tick uses float for both operands,
    // then Mth.floor's float->int narrowing for the carry.
    const float partial = clock.partialTick + clock.rate;
    const double floored = std::floor(static_cast<double>(partial));
    int fullTicks = 0;
    if (std::isnan(floored))
      fullTicks = 0;
    else if (floored <= std::numeric_limits<int>::min())
      fullTicks = std::numeric_limits<int>::min();
    else if (floored >= std::numeric_limits<int>::max())
      fullTicks = std::numeric_limits<int>::max();
    else
      fullTicks = static_cast<int>(floored);
    clock.partialTick = partial - static_cast<float>(fullTicks);
    clock.totalTicks = wrappingClockAdd(clock.totalTicks, fullTicks);
    if (clock.totalTicks != before.totalTicks ||
        clock.partialTick != before.partialTick ||
        clock.rate != before.rate || clock.paused != before.paused)
      dirty_ = true;
  }
}

bool WorldClocks::saveIfDirty(const std::filesystem::path &path) {
  if (!dirty_)
    return false;
  auto &root = document_.value;
  auto &data = const_cast<nbt::Tag &>(required(root, "data", 10));
  for (std::size_t i = 0; i < kClockNames.size(); ++i) {
    nbt::Tag *clock = data.get(kClockNames[i]);
    if (!clock) {
      nbt::Tag created;
      created.type = 10;
      setCompoundField(data, kClockNames[i], std::move(created));
      clock = data.get(kClockNames[i]);
    }
    if (clock->type != 10)
      throw nbt::Error(std::string("invalid world clock ") + kClockNames[i]);
    const WorldClockState &state = states_[i];
    nbt::Tag total;
    total.type = 4;
    total.number = state.totalTicks;
    setCompoundField(*clock, "total_ticks", std::move(total));
    if (state.partialTick == 0.0f) {
      eraseCompoundField(*clock, "partial_tick");
    } else {
      nbt::Tag partial;
      partial.type = 5;
      partial.real = static_cast<double>(state.partialTick);
      setCompoundField(*clock, "partial_tick", std::move(partial));
    }
    if (state.rate == 1.0f) {
      eraseCompoundField(*clock, "rate");
    } else {
      nbt::Tag rate;
      rate.type = 5;
      rate.real = static_cast<double>(state.rate);
      setCompoundField(*clock, "rate", std::move(rate));
    }
    if (!state.paused) {
      eraseCompoundField(*clock, "paused");
    } else {
      nbt::Tag paused;
      paused.type = 1;
      paused.number = 1;
      setCompoundField(*clock, "paused", std::move(paused));
    }
  }
  const auto encoded = nbt::compress(nbt::writeRoot(document_), 31);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
    throw nbt::Error("cannot create world_clocks directory: " + ec.message());
  replaceWithGzip(path, encoded);
  dirty_ = false;
  return true;
}

std::filesystem::path defaultRegionDirectory(const std::filesystem::path &worldDir) {
  const auto modern = worldDir / "dimensions" / "minecraft" / "overworld" / "region";
  if (std::filesystem::is_directory(modern))
    return modern;
  const auto legacy = worldDir / "region";
  return std::filesystem::is_directory(legacy) ? legacy : modern;
}
WorldGameRules loadWorldGameRules(const std::filesystem::path &path) {
  WorldGameRules result;
  if (!std::filesystem::exists(path))
    return result;
  auto root = nbt::parse(nbt::decompress(file(path), 31));
  if (integer(root, "DataVersion") != 4786)
    throw nbt::Error(
        "unsupported game_rules DataVersion (expected 26.1: 4786)");
  const Tag &data = required(root, "data", 10);
  const auto readBool = [&](const char *name, bool &value) {
    const Tag *tag = data.get(name);
    if (!tag)
      return;
    if (tag->type != 1 || (tag->number != 0 && tag->number != 1))
      throw nbt::Error(std::string("invalid game rule ") + name);
    value = tag->number != 0;
  };
  readBool("minecraft:advance_time", result.advanceTime);
  readBool("minecraft:tnt_explodes", result.tntExplodes);
  readBool("minecraft:water_source_conversion",
           result.waterSourceConversion);
  readBool("minecraft:lava_source_conversion",
           result.lavaSourceConversion);
  if (const Tag *tickSpeed = data.get("minecraft:random_tick_speed")) {
    if (tickSpeed->type != 3 || tickSpeed->number < 0)
      throw nbt::Error("invalid game rule minecraft:random_tick_speed");
    result.randomTickSpeed = static_cast<int>(tickSpeed->number);
  }
  return result;
}

WorldMetadata loadWorldMetadata(const std::filesystem::path &worldDir) {
  WorldMetadata result;
  auto level = file(worldDir / "level.dat");
  result.levelRoot = nbt::parseRoot(nbt::decompress(level, 15 + 16));
  auto &root = result.levelRoot;
  auto &data = root.value;
  auto &dataTag = const_cast<Tag &>(required(data, "Data", 10));
  if (integer(dataTag, "DataVersion") != 4786)
    throw nbt::Error("unsupported world DataVersion (expected 26.1: 4786)");
  result.gameTime = required(dataTag, "Time", 4).number;
  const auto settingsPath =
      worldDir / "data" / "minecraft" / "world_gen_settings.dat";
  if (std::filesystem::exists(settingsPath)) {
    auto settings = nbt::parse(nbt::decompress(file(settingsPath), 31));
    if (integer(settings, "DataVersion") != 4786)
      throw nbt::Error(
          "unsupported world generation DataVersion (expected 26.1: 4786)");
    result.seed =
        required(required(settings, "data", 10), "seed", 4).number;
  } else if (auto settings = dataTag.get("WorldGenSettings")) {
    result.seed = required(*settings, "seed", 4).number;
  }
  result.gameRules = loadWorldGameRules(
      worldDir / "data" / "minecraft" / "game_rules.dat");
  result.worldClocks =
      WorldClocks::load(worldDir / "data" / "minecraft" / "world_clocks.dat");
  return result;
}

WorldDataPackSelection selectWorldDataPacks(const std::filesystem::path &worldDir,
                                            const nbt::RootTag &levelRoot) {
  WorldDataPackSelection result;
  const Tag *data = levelRoot.value.get("Data");
  const Tag *packs = data && data->type == 10 ? data->get("DataPacks") : nullptr;
  if (!packs) return result; // DataPackConfig.DEFAULT enables only vanilla.
  const Tag *enabled = packs->type == 10 ? packs->get("Enabled") : nullptr;
  if (!enabled || enabled->type != 9 ||
      (enabled->listElementType != 8 && !enabled->list.empty())) {
    // TODO: Match the codec's partial-error handling for malformed DataPacks.
    result.diagnostics.push_back("malformed level.dat DataPacks; using vanilla tags");
    return result;
  }
  for (const Tag &entry : enabled->list) {
    if (entry.type != 8) {
      result.diagnostics.push_back("malformed enabled data pack entry");
      continue;
    }
    const std::string &id = entry.text;
    if (id == "vanilla") continue; // Already included as the base layer.
    if (!id.starts_with("file/")) {
      // TODO: Discover built-in feature packs and other repository providers.
      result.diagnostics.push_back("unsupported enabled data pack provider: " + id);
      continue;
    }
    const std::string name = id.substr(5);
    if (name.empty() || name == "." || name == ".." ||
        name.find_first_of("/\\") != std::string::npos ||
        name.find(':') != std::string::npos) {
      result.diagnostics.push_back("invalid enabled data pack id: " + id);
      continue;
    }
    auto path = worldDir / "datapacks" / std::filesystem::u8path(name);
    if (!std::filesystem::exists(path)) {
      result.diagnostics.push_back("enabled data pack not found: " + id);
      continue;
    }
    result.paths.push_back(std::move(path));
  }
  return result;
}

void setWorldMetadataTime(WorldMetadata &metadata, std::int64_t gameTime) {
  Tag *data = metadata.levelRoot.value.get("Data");
  if (!data || data->type != 10)
    throw nbt::Error("level.dat Data root is missing");
  Tag *time = data->get("Time");
  if (!time || time->type != 4)
    throw nbt::Error("level.dat Data.Time is missing");
  time->number = gameTime;
  metadata.gameTime = gameTime;
}

namespace {
struct AnvilLevelHeader {
  std::int64_t gameTime = 0;
  std::int64_t seed = 0;
};
AnvilLevelHeader readAnvilLevelHeader(const std::filesystem::path &worldDir) {
  AnvilLevelHeader header;
  auto level = file(worldDir / "level.dat");
  auto root = nbt::parse(nbt::decompress(level, 15 + 16));
  auto &data = required(root, "Data", 10);
  if (integer(data, "DataVersion") != 4786)
    throw nbt::Error("unsupported world DataVersion (expected 26.1: 4786)");
  header.gameTime = required(data, "Time", 4).number;
  // 26.1 moved WorldGenSettings into namespaced SavedData. The old embedded
  // compound is accepted as a compatibility fallback for synthetic fixtures.
  const auto settingsPath = worldDir / "data" / "minecraft" / "world_gen_settings.dat";
  if (std::filesystem::exists(settingsPath)) {
    auto settings = nbt::parse(nbt::decompress(file(settingsPath), 31));
    if (integer(settings, "DataVersion") != 4786)
      throw nbt::Error("unsupported world generation DataVersion (expected 26.1: 4786)");
    header.seed = required(required(settings, "data", 10), "seed", 4).number;
  } else if (auto settings = data.get("WorldGenSettings"))
    header.seed = required(*settings, "seed", 4).number;
  // TODO: vanilla falls back to random WorldOptions when SavedData cannot be
  // read. This deterministic simulator uses zero if both seed sources are
  // absent; malformed present sources are rejected instead of randomized.
  return header;
}
void readAnvilRegionsWithEntities(const std::filesystem::path &regionDir,
                                  const AnvilLoadOptions &options,
                                  AnvilChunkSink &sink) {
  readRegions(regionDir,
              [&](int x, int z) {
                return !options.chunkFilter || options.chunkFilter(x, z);
              },
              [&](const Tag &tag, int x, int z) {
                decodeAnvilChunk(sink, tag, x, z);
              });
  const std::set<std::pair<int, int>> loadedChunks(sink.chunks.begin(),
                                                   sink.chunks.end());
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
                  appendEntities(sink.entities, required(tag, "Entities", 9));
                });
}
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
void importWorldStateTail(World &world, std::int64_t gameTime,
                          std::span<const SavedTickData> ticks,
                          std::span<const nbt::Tag> blockEntities,
                          std::span<const nbt::Tag> entities,
                          AnvilWorldImportSummary *summary = nullptr) {
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
  for (const auto &tick : ticks) {
    if (!world.isLoaded(tick.pos))
      continue;
    auto &table = tick.fluid ? fluidNext : blockNext;
    ++table[chunkOf(tick)];
  }
  for (auto &[chunk, count] : blockNext)
    count = -count;
  for (auto &[chunk, count] : fluidNext)
    count = -count;
  for (const auto &tick : ticks) {
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
    const std::int64_t trigger = wrappingAdd(gameTime, tick.delay);
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
      if (summary)
        ++summary->loadedFluidTicks;
      continue;
    }
    // LevelChunkTicks.unpack: scheduleUnchecked keeps duplicate saved entries.
    world.restoreScheduledTick({tick.pos, name, trigger, priority, order});
    if (summary)
      ++summary->loadedBlockTicks;
  }
  for (const auto &tag : blockEntities) {
    auto id = tag.get("id");
    if (!id || id->type != 8)
      continue;
    const std::string canonicalId =
        id->text.find(':') == std::string::npos ? "minecraft:" + id->text
                                                : id->text;
    if (canonicalId == "minecraft:piston") {
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
      if (summary)
        ++summary->loadedBlockEntities;
      continue;
    }
    if (!isContainerEntityTypeId(canonicalId))
      continue;
    const Pos bePos{integer(tag, "x"), integer(tag, "y"), integer(tag, "z")};
    if (!world.isLoaded(bePos))
      continue;
    const BlockState currentState = world.getBlock(bePos);
    const auto kind = containerKindForState(currentState);
    if (!kind || containerEntityTypeId(*kind) != canonicalId)
      continue;
    auto container = makeContainerEntityForState(currentState, bePos);
    if (!container)
      continue;
    // TODO: restore LootTable / LootTableSeed, CustomName, Lock, and full item
    // DataComponents when those systems exist.
    if (auto items = tag.get("Items")) {
      if (items->type != 9)
        throw nbt::Error("invalid container Items");
      for (const auto &entry : items->list) {
        if (entry.type != 10)
          throw nbt::Error("invalid container item entry");
        const auto *slotTag = entry.get("Slot");
        if (!slotTag || slotTag->type < 1 || slotTag->type > 4)
          throw nbt::Error("invalid container item Slot");
        const int slot =
            slotTag->type == 1
                ? static_cast<int>(static_cast<std::uint8_t>(slotTag->number))
                : static_cast<int>(slotTag->number);
        if (slot < 0 || slot >= container->containerSize())
          continue;
        container->setItem(slot, itemStackTag(entry));
      }
    }
    if (*kind == ContainerKind::Hopper) {
      auto *hopper = static_cast<HopperBlockEntity *>(container.get());
      hopper->cooldownTime = intOr(tag, "TransferCooldown", -1);
    }
    world.setContainerEntity(bePos, container, *kind == ContainerKind::Hopper);
    if (summary)
      ++summary->loadedBlockEntities;
  }
  std::set<std::array<std::int32_t, 4>> importedUuids;
  for (const auto &tag : entities) {
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
    if (world.entities.size() != before) {
      if (summary)
        ++summary->loadedEntities;
      if (key)
        importedUuids.insert(*key);
    }
  }
}
} // namespace

LoadedWorld loadAnvilWorld(const std::filesystem::path &worldDir,
                           const std::filesystem::path &regionDir,
                           const AnvilLoadOptions &options) {
  if (options.minY >= options.maxY)
    throw nbt::Error("invalid world height");
  const AnvilLevelHeader header = readAnvilLevelHeader(worldDir);
  LoadedWorld w;
  w.minY = options.minY;
  w.maxY = options.maxY;
  w.gameTime = header.gameTime;
  w.seed = header.seed;
  AnvilChunkSink sink{
      .minY = options.minY,
      .maxY = options.maxY,
      .skipNonFullChunks = options.skipNonFullChunks,
      .chunks = w.chunks,
      .ticks = w.ticks,
      .blockEntities = w.blockEntities,
      .entities = w.entities,
      .onSection =
          [&](int cx, int cz, int sy, std::span<const BlockState> states,
              std::span<const std::uint16_t, 4096> indices) {
            const int secBaseY = sy * 16;
            for (int i = 0; i < 4096; ++i) {
              const int y = secBaseY + (i >> 8);
              if (y < w.minY || y >= w.maxY)
                continue;
              const BlockState &s =
                  states.size() == 1
                      ? states[0]
                      : states[indices[static_cast<std::size_t>(i)]];
              if (!s.isAir())
                w.blocks[{cx * 16 + (i & 15), y,
                          cz * 16 + ((i >> 4) & 15)}] = s;
            }
          },
      .onPersistedLight =
          [&](int cx, int cz, PersistedChunkLight &&light) {
            const std::uint64_t key =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx))
                 << 32) |
                static_cast<std::uint32_t>(cz);
            w.persistedLights[key] = std::move(light);
          },
  };
  readAnvilRegionsWithEntities(regionDir, options, sink);
  return w;
}

LoadedWorld loadAnvilWorld(const std::filesystem::path &worldDir,
                           const std::filesystem::path &regionDir, int minY,
                           int maxY) {
  return loadAnvilWorld(worldDir, regionDir, AnvilLoadOptions{minY, maxY, false});
}

AnvilWorldImportSummary
loadAnvilWorldInto(const std::filesystem::path &worldDir,
                   const std::filesystem::path &regionDir, World &world,
                   const AnvilLoadOptions &options) {
  if (options.minY >= options.maxY)
    throw nbt::Error("invalid world height");
  const AnvilLevelHeader header = readAnvilLevelHeader(worldDir);
  world.setGameTime(header.gameTime);
  world.setSeed(static_cast<std::uint64_t>(header.seed));
  AnvilWorldImportSummary summary;
  std::vector<SavedTickData> ticks;
  std::vector<nbt::Tag> blockEntities;
  std::vector<nbt::Tag> entities;
  std::array<BlockState::Id, 4096> paletteIds{};
  AnvilChunkSink sink{
      .minY = options.minY,
      .maxY = options.maxY,
      .skipNonFullChunks = options.skipNonFullChunks,
      .chunks = summary.loadedChunks,
      .ticks = ticks,
      .blockEntities = blockEntities,
      .entities = entities,
      .onChunkStart = [&](int cx, int cz) { world.loadChunk(cx, cz); },
      .onSection =
          [&](int cx, int cz, int sy, std::span<const BlockState> states,
              std::span<const std::uint16_t, 4096> indices) {
            if (states.size() == 1 && states[0].isAir())
              return;
            bool hasContainer = false;
            for (std::size_t p = 0; p < states.size(); ++p) {
              paletteIds[p] = states[p].id();
              if (!hasContainer && containerKindForState(states[p]))
                hasContainer = true;
            }
            world.importSectionBlocks(cx, cz, sy,
                                      {paletteIds.data(), states.size()},
                                      indices);
            if (hasContainer) {
              const int secBaseY = sy * 16;
              for (int i = 0; i < 4096; ++i) {
                const int y = secBaseY + (i >> 8);
                if (y < world.minY() || y >= world.maxY())
                  continue;
                const BlockState &s =
                    states.size() == 1
                        ? states[0]
                        : states[indices[static_cast<std::size_t>(i)]];
                if (containerKindForState(s))
                  ensureContainerBlockEntity(
                      world, {cx * 16 + (i & 15), y, cz * 16 + ((i >> 4) & 15)},
                      s);
              }
            }
          },
      .onPersistedLight =
          [&](int cx, int cz, PersistedChunkLight &&light) {
            world.setChunkPersistedLight(cx, cz, std::move(light));
          },
  };
  readAnvilRegionsWithEntities(regionDir, options, sink);
  summary.skippedNonFullChunks = sink.skippedNonFullChunks;
  world.primeHeightmaps();
  world.refreshLightSnapshots();
  importWorldStateTail(world, header.gameTime, ticks, blockEntities, entities,
                       &summary);
  return summary;
}

// TODO: non-piston/non-container block entities remain in LoadedWorld for
// reporting; their simulation is not implemented.
void importWorld(const LoadedWorld &input, World &world) {
  world.setGameTime(input.gameTime);
  world.setSeed(static_cast<std::uint64_t>(input.seed));
  for (auto [x, z] : input.chunks)
    world.loadChunk(x, z);
  for (auto &[pos, block] : input.blocks) {
    world.rawSetBlock(pos, block);
    if (containerKindForState(block))
      ensureContainerBlockEntity(world, pos, block);
  }
  world.primeHeightmaps();
  for (auto [x, z] : input.chunks) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
        static_cast<std::uint32_t>(z);
    if (auto it = input.persistedLights.find(key);
        it != input.persistedLights.end())
      world.setChunkPersistedLight(x, z, it->second);
  }
  // If lighting was enabled before import, loadChunk's pre-write snapshots are
  // superseded by one complete post-import snapshot per chunk (with persisted
  // Anvil light when isLightOn=true).
  world.refreshLightSnapshots();
  importWorldStateTail(world, input.gameTime, input.ticks, input.blockEntities,
                       input.entities);
}

std::vector<PersistentTicketRecord>
loadPersistentTickets(const std::filesystem::path &path) {
  std::vector<PersistentTicketRecord> result;
  if (!std::filesystem::exists(path))
    return result;
  const auto root = nbt::parseRoot(nbt::decompress(file(path), 31));
  if (integer(root.value, "DataVersion") != 4786)
    throw nbt::Error("unsupported chunk_tickets DataVersion");
  const Tag &data = required(root.value, "data", 10);
  if (const Tag *tickets = data.get("tickets")) {
    if (tickets->type != 9)
      throw nbt::Error("chunk_tickets data.tickets is not a list");
    for (const Tag &entry : tickets->list) {
      if (entry.type != 10)
        throw nbt::Error("chunk_tickets entry is not a compound");
      const auto &positions = required(entry, "chunk_pos", 11).ints;
      if (positions.size() != 2)
        throw nbt::Error("chunk_tickets chunk_pos must be IntArray[2]");
      // Level.MAX_LEVEL_SIZE is 30,000,000. Flooring that inclusive block
      // range yields the asymmetric valid chunk range below.
      constexpr int kMinTicketChunkCoordinate = -1875000;
      constexpr int kMaxTicketChunkCoordinate = 1874999;
      if (positions[0] < kMinTicketChunkCoordinate ||
          positions[0] > kMaxTicketChunkCoordinate ||
          positions[1] < kMinTicketChunkCoordinate ||
          positions[1] > kMaxTicketChunkCoordinate)
        throw nbt::Error("chunk_tickets chunk_pos is outside world bounds");
      const auto &type = required(entry, "type", 8).text;
      if (type != ticket_types::Forced.name && type != ticket_types::Portal.name)
        throw nbt::Error("unsupported persistent ticket type: " + type);
      const auto level = required(entry, "level", 3).number;
      if (level < 0 || level > std::numeric_limits<int>::max())
        throw nbt::Error("chunk_tickets level is out of range");
      std::int64_t ticksLeft = 0;
      if (const Tag *ticks = entry.get("ticks_left")) {
        if (ticks->type != 4)
          throw nbt::Error("chunk_tickets ticks_left is not a Long");
        // Persisted countdowns are non-negative in normal 26.1 saves. This
        // also rejects INT64_MIN before any countdown arithmetic.
        if (ticks->number < 0)
          throw nbt::Error("chunk_tickets ticks_left is negative");
        ticksLeft = ticks->number;
      }
      result.push_back({positions[0], positions[1], type,
                        static_cast<int>(level), ticksLeft});
    }
  }
  // Validate all records before returning; callers can therefore apply as one
  // batch without leaving partially loaded demand.
  return result;
}

void savePersistentTickets(const std::filesystem::path &path,
                           std::span<const PersistentTicketRecord> tickets) {
  nbt::RootTag root;
  root.name = "";
  root.value.type = 10;
  Tag &data = root.value.compound["data"];
  data.type = 10;
  root.value.compoundOrder.push_back("data");
  Tag version;
  version.type = 3;
  version.number = 4786;
  root.value.compound["DataVersion"] = std::move(version);
  root.value.compoundOrder.push_back("DataVersion");
  Tag list;
  list.type = 9;
  list.listElementType = 10;
  for (const PersistentTicketRecord &record : tickets) {
    Tag entry;
    entry.type = 10;
    Tag pos;
    pos.type = 11;
    pos.ints = {record.x, record.z};
    entry.compound["chunk_pos"] = std::move(pos);
    entry.compoundOrder.push_back("chunk_pos");
    Tag type;
    type.type = 8;
    type.text = record.type;
    entry.compound["type"] = std::move(type);
    entry.compoundOrder.push_back("type");
    Tag level;
    level.type = 3;
    level.number = record.level;
    entry.compound["level"] = std::move(level);
    entry.compoundOrder.push_back("level");
    Tag ticksLeft;
    ticksLeft.type = 4;
    ticksLeft.number = record.ticksLeft;
    entry.compound["ticks_left"] = std::move(ticksLeft);
    entry.compoundOrder.push_back("ticks_left");
    list.list.push_back(std::move(entry));
  }
  data.compound["tickets"] = std::move(list);
  data.compoundOrder.push_back("tickets");
  const auto encoded = nbt::compress(nbt::writeRoot(root), 31);
  std::filesystem::create_directories(path.parent_path());
  replaceWithGzip(path, encoded);
}

void saveWorldMetadata(const std::filesystem::path &worldDir,
                       const WorldMetadata &metadata) {
  const auto encoded = nbt::compress(nbt::writeRoot(metadata.levelRoot), 31);
  const auto current = worldDir / "level.dat";
  const auto old = worldDir / "level.dat_old";
  replaceWithGzip(current, encoded, &old);
}
} // namespace redstone
