#pragma once
#include "redstone/engine.hpp"
#include "redstone/nbt.hpp"
#include <filesystem>
#include <optional>
#include <vector>
namespace redstone {
struct SavedTickData {
  Pos pos;
  std::string type;
  int delay{}, priority{};
  bool fluid{};
  // Litematica records the original ScheduledTick.subTickOrder as SubTick;
  // vanilla's own chunk format does not, and rebuilds -N..-1 in saved-list
  // order instead (LevelChunkTicks.unpack). Absent means "use the vanilla
  // per-chunk sequence", present means "keep the schematic's own order", so
  // the two rules never get mixed (LitematicaSchematic.java:1498,2834).
  std::optional<std::int64_t> subTick;
};
struct LoadedWorld {
  std::int64_t gameTime{};
  int minY = -64, maxY = 320;
  std::map<Pos, BlockState> blocks;
  std::vector<std::pair<int, int>> chunks;
  std::vector<SavedTickData> ticks;
  std::vector<nbt::Tag> blockEntities;
  std::vector<nbt::Tag> entities;
  std::int64_t seed = 0;
};
// 26.1's namespaced overworld path takes precedence over legacy world/region.
std::filesystem::path defaultRegionDirectory(const std::filesystem::path &worldDir);
// regionDir is the selected dimension's region directory; its sibling entities
// directory is read without generating/loading additional terrain chunks.
// worldDir contains level.dat and namespaced data/minecraft saved data.
LoadedWorld loadAnvilWorld(const std::filesystem::path &worldDir,
                           const std::filesystem::path &regionDir,
                           int minY = -64, int maxY = 320);
void importWorld(const LoadedWorld &input, World &world);
} // namespace redstone
