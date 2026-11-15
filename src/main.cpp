#include "redstone/allocator.hpp"
#include "redstone/engine.hpp"
#include "redstone/entity.hpp"
#include "redstone/storage.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
int number(const char *text) {
  int n{};
  std::string_view s(text);
  auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), n);
  if (error != std::errc{} || end != s.data() + s.size())
    throw std::runtime_error("invalid integer: " + std::string(s));
  return n;
}
int chunk(int n) { return n >= 0 ? n / 16 : -1 - ((-1 - n) / 16); }
std::string quoted(std::string_view s) {
  std::string out = "\"";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += c;
  }
  return out + '"';
}
void snapshot(const redstone::World &world) {
  std::cout << "{\"time\":" << world.gameTime() << ",\"blocks\":[";
  bool first = true;
  for (auto &[p, s] : world.allBlocks()) {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << "{\"x\":" << p.x << ",\"y\":" << p.y << ",\"z\":" << p.z
              << ",\"name\":" << quoted(s.name()) << ",\"properties\":{";
    bool prop = true;
    for (auto &[k, v] : s.properties()) {
      if (!prop)
        std::cout << ',';
      prop = false;
      std::cout << quoted(k) << ':' << quoted(v);
    }
    std::cout << "}}";
  }
  std::cout << "],\"entities\":[";
  first = true;
  for (auto &entity : world.entities) {
    if (entity->isRemoved())
      continue;
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << "{\"id\":" << quoted(entity->typeName())
              << ",\"x\":" << entity->position.x
              << ",\"y\":" << entity->position.y
              << ",\"z\":" << entity->position.z
              << ",\"vx\":" << entity->deltaMovement.x
              << ",\"vy\":" << entity->deltaMovement.y
              << ",\"vz\":" << entity->deltaMovement.z;
    if (auto *item = dynamic_cast<redstone::ItemEntity *>(entity.get()))
      std::cout << ",\"item\":" << quoted(item->item.id)
                << ",\"count\":" << item->item.count
                << ",\"age\":" << item->age;
    else if (auto *tnt = dynamic_cast<redstone::PrimedTnt *>(entity.get()))
      std::cout << ",\"fuse\":" << tnt->fuse;
    else if (auto *falling =
                 dynamic_cast<redstone::FallingBlockEntity *>(entity.get()))
      std::cout << ",\"block\":" << quoted(falling->blockState.name());
    std::cout << "}";
  }
  std::cout << "]}\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    std::filesystem::path worldDir, regionDir;
    std::string dimensionId;
    std::set<std::pair<int, int>> forced;
    int ticks = 0, minY = -64, maxY = 320;
    bool explicitBounds = false;
    bool demo = false, dump = false;
    for (int i = 1; i < argc; i++) {
      std::string arg = argv[i];
      auto next = [&]() {
        if (++i >= argc)
          throw std::runtime_error("missing argument for " + arg);
        return argv[i];
      };
      if (arg == "--world")
        worldDir = next();
      else if (arg == "--region")
        regionDir = next();
      else if (arg == "--dimension")
        dimensionId = redstone::normalizeDimensionId(next());
      else if (arg == "--ticks")
        ticks = number(next());
      else if (arg == "--min-y") {
        minY = number(next());
        explicitBounds = true;
      } else if (arg == "--max-y") {
        maxY = number(next());
        explicitBounds = true;
      } else if (arg == "--force") {
        int x = number(next()), z = number(next());
        forced.emplace(x, z);
      } else if (arg == "--demo")
        demo = true;
      else if (arg == "--dump")
        dump = true;
      else if (arg == "--help") {
        std::cout
            << "redstone_cli --world <save> [--dimension <ns:path>] "
               "[--region <dimension/region>] "
               "[--force <cx> <cz>] [--ticks N] [--dump]\n"
               "redstone_cli --demo [--ticks N] [--dump]\n"
               "--force may repeat; default loads every chunk found. --dump "
               "writes JSON snapshots to stdout.\n";
        return 0;
      } else
        throw std::runtime_error("unknown option: " + arg);
    }
    if (ticks < 0 || minY >= maxY)
      throw std::runtime_error("invalid tick count or build height");
    if (demo == !worldDir.empty())
      throw std::runtime_error("specify exactly one of --demo or --world");
    std::optional<redstone::DimensionStem> selectedStem;
    if (!demo && !dimensionId.empty()) {
      const auto meta = redstone::loadWorldMetadata(worldDir);
      if (const auto *found = meta.findDimension(dimensionId)) {
        selectedStem = *found;
      } else if (const auto builtin =
                     redstone::findBuiltinDimensionType(dimensionId)) {
        selectedStem = redstone::DimensionStem{dimensionId, dimensionId, *builtin};
      } else {
        throw std::runtime_error("unknown dimension: " + dimensionId);
      }
      if (!explicitBounds) {
        minY = selectedStem->type.minY;
        maxY = selectedStem->type.maxY();
      }
    }
    redstone::World world =
        (selectedStem && !explicitBounds)
            ? redstone::World(selectedStem->type, 1000000, selectedStem->id)
            : redstone::World(minY, maxY);
    if (selectedStem && explicitBounds) {
      auto adjusted = selectedStem->type;
      adjusted.minY = minY;
      adjusted.height = maxY - minY;
      adjusted.logicalHeight = std::min(adjusted.logicalHeight, adjusted.height);
      world.setDimension(adjusted, selectedStem->id);
    }
    std::cerr << "allocator=" << redstone::memory::backend() << '\n';
    if (demo) {
      world.loadChunk(0, 0);
      world.rawSetBlock(
          {4, 0, 4}, redstone::defaultState("observer").with("facing", "east"));
      world.rawSetBlock({2, 0, 4},
                        redstone::defaultState("piston").with("facing", "up"));
      world.rawSetBlock({3, 0, 4}, redstone::defaultState("smooth_stone"));
      world.setBlock({5, 0, 4}, redstone::defaultState("smooth_stone"));
    } else {
      if (regionDir.empty()) {
        regionDir = selectedStem
                        ? redstone::dimensionRegionDirectory(worldDir,
                                                             selectedStem->id)
                        : redstone::defaultRegionDirectory(worldDir);
      }
      auto data = redstone::loadAnvilWorld(worldDir, regionDir, minY, maxY);
      if (!forced.empty()) {
        auto wanted = [&](redstone::Pos p) {
          return forced.contains({chunk(p.x), chunk(p.z)});
        };
        std::erase_if(data.chunks, [&](auto c) { return !forced.contains(c); });
        std::erase_if(data.blocks,
                      [&](auto &entry) { return !wanted(entry.first); });
        std::erase_if(data.ticks, [&](auto &t) { return !wanted(t.pos); });
        std::erase_if(data.blockEntities, [&](auto &e) {
          auto x = e.get("x"), y = e.get("y"), z = e.get("z");
          return !x || !y || !z ||
                 !wanted({static_cast<int>(x->number),
                          static_cast<int>(y->number),
                          static_cast<int>(z->number)});
        });
        // importWorld validates/clamps entity Pos and checks loaded X/Z
        // chunks. Do not cast untrusted raw NBT doubles to int here.
      }
      redstone::importWorld(data, world);
      std::size_t unsupported = 0, fluid = 0, otherBE = 0, otherEntities = 0;
      std::size_t importedEntities = world.entities.size();
      for (auto &[p, s] : data.blocks)
        if (!redstone::isSupportedBlock(s.name()))
          ++unsupported;
      for (auto &t : data.ticks)
        if (t.fluid)
          ++fluid;
      for (auto &e : data.blockEntities) {
        auto id = e.get("id");
        if (!id || id->text != "minecraft:piston")
          ++otherBE;
      }
      for (auto &e : data.entities) {
        auto id = e.get("id");
        if (!id ||
            (id->text != "minecraft:item" && id->text != "minecraft:tnt" &&
             id->text != "minecraft:falling_block" &&
             id->text != "minecraft:minecart"))
          ++otherEntities;
      }
      std::cerr << "loaded_chunks=" << data.chunks.size()
                << " blocks=" << data.blocks.size()
                << " unsupported_blocks=" << unsupported
                << " fluid_ticks_not_simulated=" << fluid
                << " other_block_entities_not_simulated=" << otherBE
                << " simple_entities_imported=" << importedEntities
                << " other_entities_not_simulated=" << otherEntities << '\n';
    }
    if (dump)
      snapshot(world);
    for (int i = 0; i < ticks; i++) {
      world.tick();
      if (dump)
        snapshot(world);
      for (auto &e : world.log)
        std::cerr << world.gameTime() << ' ' << e.kind << " (" << e.pos.x << ','
                  << e.pos.y << ',' << e.pos.z << ") " << e.detail << '\n';
      world.log.clear();
    }
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
