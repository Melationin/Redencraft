#pragma once
#include <optional>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "redstone/block.hpp"

namespace redstone {
// Vanilla 26.1 tags resolved by Mojang's TagLoader; unknown tags return nullopt.
// This free function always reads the built-in snapshot; use World::hasBlockTag
// for a world that can reload packs (BLOCK_TODO.md).
std::optional<bool> blockHasTag(const Block &block, std::string_view tag);
std::optional<bool> blockHasTag(const BlockState &state, std::string_view tag);

// One immutable-at-query tag snapshot, separate per world. Pack directories are
// supplied from lowest to highest priority; reload reconstructs the graph from
// the original vanilla definitions so nested references observe replacements.
// Directories and ordinary stored/deflated ZIP packs are supported. ZIP64 and
// multi-disk ZIPs are explicitly rejected; see BLOCK_TODO.md.
class BlockTagRegistry {
public:
  struct ReloadResult {
    bool applied = false;
    std::vector<std::string> diagnostics;
  };
  BlockTagRegistry();
  ReloadResult reload(const std::vector<std::filesystem::path> &packs);
  std::optional<bool> contains(std::string_view tag, std::string_view block) const;
  const auto &entries() const { return tags_; }
private:
  std::map<std::string, std::vector<std::string>, std::less<>> tags_;
};
} // namespace redstone
