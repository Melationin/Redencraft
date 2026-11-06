#pragma once
#include "redstone/engine.hpp"
#include "redstone/nbt.hpp"
#include <filesystem>
#include <vector>
namespace redstone {
struct SavedTickData {
  Pos pos;
  std::string type;
  int delay{}, priority{};
  bool fluid{};
};
struct LoadedWorld {
  std::int64_t gameTime{};
  int minY = -64, maxY = 320;
  std::map<Pos, BlockState> blocks;
  std::vector<std::pair<int, int>> chunks;
  std::vector<SavedTickData> ticks;
  std::vector<nbt::Tag> blockEntities;
};
// regionDir is the selected dimension's region directory; worldDir contains
// level.dat.
LoadedWorld loadAnvilWorld(const std::filesystem::path &worldDir,
                           const std::filesystem::path &regionDir,
                           int minY = -64, int maxY = 320);
void importWorld(const LoadedWorld &input, World &world);
} // namespace redstone
