#pragma once
#include "redstone/storage.hpp"
#include <functional>
#include <map>

namespace redstone {
struct LitematicOptions {
  Pos offset{0, 64, 0};
  int minY = -64, maxY = 320;
  // Caller can validate full state/property combinations against official IDs.
  std::function<void(const BlockState &)> validateState;
  // Preview import for projections that use blocks this simulator does not
  // implement. Every such state becomes smooth_stone and its block entity is
  // dropped; LitematicImport::substitutedBlocks reports what was replaced so the
  // substitution is never silent. Off by default: an unimplemented block is an
  // error, not a guess.
  bool substituteUnsupported = false;
};
struct LitematicImport {
  LoadedWorld world;
  std::size_t replacedAnvils{}, skippedSigns{}, skippedSignBlockEntities{};
  std::size_t regeneratedEntityUuids{}, regionCount{};
  std::size_t skippedUnsupportedBlockEntities{};
  std::size_t skippedUnsupportedEntities{};
  // Source block name -> number of cells placed as smooth_stone.
  std::map<std::string, std::size_t> substitutedBlocks;
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
