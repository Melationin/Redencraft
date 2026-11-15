#include "redstone/block_tags.hpp"
#include "redstone/engine.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <set>
#include <string>
#include <vector>
#include <zlib.h>

using namespace redstone;
namespace {
void zipPack(const std::filesystem::path &path, const std::string &name,
             const std::string &contents, bool compressed) {
  std::vector<unsigned char> payload(contents.begin(), contents.end());
  if (compressed) {
    z_stream z{};
    assert(deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::vector<unsigned char> encoded(compressBound(contents.size()));
    z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(contents.data()));
    z.avail_in = static_cast<uInt>(contents.size());
    z.next_out = encoded.data(); z.avail_out = static_cast<uInt>(encoded.size());
    assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
    encoded.resize(z.total_out);
    deflateEnd(&z);
    payload = std::move(encoded);
  }
  const auto checksum = crc32(0, reinterpret_cast<const Bytef *>(contents.data()), contents.size());
  std::vector<unsigned char> bytes;
  const auto u16 = [&](std::uint16_t x) { bytes.push_back(x & 255); bytes.push_back(x >> 8); };
  const auto u32 = [&](std::uint32_t x) { u16(x & 65535); u16(x >> 16); };
  const auto method = compressed ? 8 : 0;
  u32(0x04034b50); u16(20); u16(0); u16(method); u16(0); u16(0);
  u32(checksum); u32(payload.size()); u32(contents.size()); u16(name.size()); u16(0);
  bytes.insert(bytes.end(), name.begin(), name.end());
  bytes.insert(bytes.end(), payload.begin(), payload.end());
  const auto central = bytes.size();
  u32(0x02014b50); u16(20); u16(20); u16(0); u16(method); u16(0); u16(0);
  u32(checksum); u32(payload.size()); u32(contents.size()); u16(name.size());
  u16(0); u16(0); u16(0); u16(0); u32(0); u32(0);
  bytes.insert(bytes.end(), name.begin(), name.end());
  const auto directorySize = bytes.size() - central;
  u32(0x06054b50); u16(0); u16(0); u16(1); u16(1);
  u32(directorySize); u32(central); u16(0);
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  assert(out.good());
}
}
int main(int argc, char **argv) {
  assert(argc == 3);
  std::ifstream in(argv[1]);
  assert(in);
  std::ifstream blockFile(argv[2]);
  assert(blockFile);
  std::vector<std::string> blockNames;
  std::string blockLine;
  while (std::getline(blockFile, blockLine)) {
    if (!blockLine.empty() && blockLine.back() == '\r') blockLine.pop_back();
    const auto tab = blockLine.find('\t');
    assert(tab != std::string::npos);
    blockNames.push_back(blockLine.substr(tab + 1));
  }
  std::string line;
  std::size_t tags = 0, memberships = 0;
  BlockTagRegistry reloaded;
  const auto vanilla = reloaded.reload({});
  assert(vanilla.applied && vanilla.diagnostics.empty());
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto tab = line.find('\t');
    assert(tab != std::string::npos);
    const auto tag = line.substr(0, tab);
    std::istringstream members(line.substr(tab + 1));
    std::set<std::string> expected;
    std::string member;
    while (std::getline(members, member, ',')) {
      assert(!member.empty());
      // Known names that have not yet been implemented get an unsupported
      // singleton; their vanilla tag identity is still independently known.
      const Block &block = blockByName(member);
      assert(blockHasTag(block, tag) == true);
      assert(reloaded.contains(tag, member) == true);
      expected.insert(member);
      ++memberships;
    }
    for (const auto &name : blockNames)
      assert(reloaded.contains(tag, name) == expected.contains(name));
    ++tags;
  }
  assert(tags == 248 && memberships == 3998);
  assert(blockHasTag(Blocks::STONE, "minecraft:logs") == false);
  assert(blockHasTag(Blocks::STONE, "other:logs") == std::nullopt);

  const auto base = std::filesystem::temp_directory_path() /
      ("redstone-block-tags-test-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto low = base / "low/data/minecraft/tags/block";
  const auto high = base / "high/data/minecraft/tags/block";
  std::filesystem::create_directories(low);
  std::filesystem::create_directories(high);
  auto put = [](const auto &path, const auto &data) { std::ofstream(path) << data; };
  put(low / "dirt.json", R"({"values":["minecraft:stone"]})");
  put(high / "dirt.json", R"({"replace":true,"values":["minecraft:glass"]})");
  put(high / "custom.json", R"({"values":["#minecraft:dirt",{"id":"minecraft:not_a_block","required":false},"minecraft:stone"]})");
  put(high / "snow.json", R"({"replace":true,"values":["minecraft:stone"]})");
  put(high / "optional.json", R"({"values":[{"id":"#minecraft:not_a_tag","required":false},"minecraft:dirt"]})");
  // A malformed higher-priority resource must not clear lower-priority data.
  put(high / "logs.json", R"({"replace":true,"values":[]) trailing)");
  auto loaded = reloaded.reload({base / "low", base / "high"});
  assert(loaded.applied && !loaded.diagnostics.empty());
  assert(reloaded.contains("minecraft:dirt", "minecraft:glass") == true);
  assert(reloaded.contains("minecraft:dirt", "minecraft:stone") == false);
  assert(reloaded.contains("minecraft:substrate_overworld", "minecraft:glass") == true);
  assert(reloaded.contains("minecraft:custom", "minecraft:glass") == true);
  assert(reloaded.contains("minecraft:custom", "minecraft:stone") == true);
  assert(reloaded.contains("minecraft:custom", "minecraft:not_a_block") == false);
  assert(reloaded.contains("minecraft:optional", "minecraft:dirt") == true);
  assert(reloaded.contains("minecraft:logs", "minecraft:oak_log") == true);
  World world;
  world.loadChunk(0, 0);
  assert(world.reloadBlockTags({base / "low", base / "high"}).applied);
  const Pos pos{1, 10, 1};
  world.rawSetBlock(pos, Blocks::PODZOL.defaultState());
  world.setBlock(pos.relative(Direction::Up), Blocks::STONE.defaultState());
  assert(world.getBlock(pos).getView("snowy") == "true");
  assert(world.reloadBlockTags({}).applied);
  world.setBlock(pos.relative(Direction::Up), Blocks::DIRT.defaultState());
  assert(world.getBlock(pos).getView("snowy") == "false");
  // Vegetation checks the current world's support tag for changes from any
  // direction, including when a datapack replaces the vanilla tag.
  put(high / "supports_vegetation.json", R"({"replace":true,"values":["minecraft:stone"]})");
  assert(world.reloadBlockTags({base / "high"}).applied);
  const Pos plant{4, 10, 4};
  world.rawSetBlock(plant.relative(Direction::Down), Blocks::STONE.defaultState());
  world.rawSetBlock(plant, Blocks::SHORT_GRASS.defaultState());
  world.setBlock(plant.relative(Direction::East), Blocks::DIRT.defaultState());
  assert(world.getBlock(plant).is(Blocks::SHORT_GRASS));
  assert(world.reloadBlockTags({}).applied);
  world.setBlock(plant.relative(Direction::East), Blocks::STONE.defaultState());
  assert(world.getBlock(plant).isAir());
  put(high / "cannot_support_seagrass.json", R"({"replace":true,"values":["minecraft:stone"]})");
  assert(world.reloadBlockTags({base / "high"}).applied);
  world.rawSetBlock(plant, Blocks::SEAGRASS.defaultState());
  world.setBlock(plant.relative(Direction::East), Blocks::DIRT.defaultState());
  assert(world.getBlock(plant).is(Blocks::WATER));
  put(high / "supports_wither_rose.json", R"({"replace":true,"values":["minecraft:stone"]})");
  assert(world.reloadBlockTags({base / "high"}).applied);
  world.rawSetBlock(plant, Blocks::WITHER_ROSE.defaultState());
  world.setBlock(plant.relative(Direction::East), Blocks::STONE.defaultState());
  assert(world.getBlock(plant).is(Blocks::WITHER_ROSE));
  assert(world.reloadBlockTags({}).applied);
  world.setBlock(plant.relative(Direction::East), Blocks::DIRT.defaultState());
  assert(world.getBlock(plant).isAir());
  World untouched;
  assert(untouched.hasBlockTag(Blocks::STONE.defaultState(), "minecraft:snow") == false);
  std::filesystem::remove(high / "logs.json");
  for (bool compressed : {false, true}) {
    const auto zip = base / (compressed ? "compressed.zip" : "stored.zip");
    zipPack(zip, "data/minecraft/tags/block/dirt.json",
            R"({"replace":true,"values":["minecraft:oak_log"]})", compressed);
    loaded = reloaded.reload({base / "low", zip});
    assert(loaded.applied && loaded.diagnostics.empty());
    assert(reloaded.contains("minecraft:dirt", "minecraft:oak_log") == true);
    assert(reloaded.contains("minecraft:substrate_overworld", "minecraft:oak_log") == true);
    assert(reloaded.contains("minecraft:dirt", "minecraft:stone") == false);
  }
  put(high / "custom.json", R"({"values":["minecraft:missing"]})");
  loaded = reloaded.reload({base / "low", base / "high"});
  assert(loaded.applied && !loaded.diagnostics.empty());
  assert(reloaded.contains("minecraft:custom", "minecraft:stone") == std::nullopt);
  loaded = reloaded.reload({base / "not-a-pack"});
  assert(!loaded.applied && !loaded.diagnostics.empty());
  assert(reloaded.contains("minecraft:dirt", "minecraft:glass") == true);
  loaded = reloaded.reload({});
  assert(loaded.applied && reloaded.contains("minecraft:dirt", "minecraft:dirt") == true);
  std::filesystem::remove_all(base);
}
