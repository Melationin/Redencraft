#pragma once
#include "redstone/storage.hpp"
#include <functional>

namespace redstone {
struct LitematicOptions {
  Pos offset{0, 64, 0};
  int minY = -64, maxY = 320;
  // Caller can validate full state/property combinations against official IDs.
  std::function<void(const BlockState &)> validateState;
};
struct LitematicImport {
  LoadedWorld world;
  std::size_t replacedAnvils{}, skippedSigns{}, skippedSignBlockEntities{};
  std::size_t regeneratedEntityUuids{}, regionCount{};
  Pos min, maxExclusive;
  int sourceDataVersion{};
};
// Snapshot import: no onPlace/neighbor notifications and no invented ticks.
// Blocks/BE/ticks are relative to the minimum corner; entities to region
// Position. Only unrotated, unmirrored version 6/subversion 1 is currently
// accepted.
LitematicImport loadLitematic(const nbt::Tag &root,
                              const LitematicOptions &options = {});
LitematicImport loadLitematic(const std::filesystem::path &path,
                              const LitematicOptions &options = {});
} // namespace redstone
