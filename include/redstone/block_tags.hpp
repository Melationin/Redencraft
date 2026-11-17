#pragma once
// 方块标签系统：提供原版 26.1 静态内建标签的快查接口，以及支持数据包重载的
// 每世界独立 `BlockTagRegistry` 标签快照。
#include <optional>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include "redstone/block.hpp"

namespace redstone {
// 由 Mojang `TagLoader` 解析出的原版 26.1 方块标签；未知标签名返回 `std::nullopt`
// （不对其做默认 false 猜测）。
// 这两个自由函数始终读取编译期固化的内置快照；若需要支持数据包重载的世界级标签，
// 请改用 `World::hasBlockTag`（参见 `BLOCK_TODO.md`）。
std::optional<bool> blockHasTag(const Block &block, std::string_view tag);
std::optional<bool> blockHasTag(const BlockState &state, std::string_view tag);

// 查询期不可变的方块标签快照，每个世界各自持有一份，互不干扰。
// 之所以要在 `reload` 时从原版原始定义重建依赖图，是因为数据包可能以 `replace: true`
// 覆盖低优先级包的标签，而嵌套引用（`#tag`）必须在重放后重新拓扑展开才能观察到替换结果。
// 传入的 `packs` 目录或 ZIP 需按优先级从低到高排列；支持普通目录与 Stored/Deflate ZIP，
// 显式拒绝 ZIP64 与分卷 ZIP（参见 `BLOCK_TODO.md`）。
class BlockTagRegistry {
public:
  // 标签重载结果：记录是否成功应用，以及解析/校验过程中产生的诊断信息。
  struct ReloadResult {
    bool applied = false;
    std::vector<std::string> diagnostics;
  };
  BlockTagRegistry();
  // 按从低到高顺序加载数据包中的 `data/<ns>/tags/block/*.json`，重建并替换当前标签表。
  ReloadResult reload(const std::vector<std::filesystem::path> &packs);
  // 查询指定标签是否包含某方块；标签不存在时返回 `std::nullopt`。
  std::optional<bool> contains(std::string_view tag, std::string_view block) const;
  const auto &entries() const { return tags_; }
private:
  std::map<std::string, std::vector<std::string>, std::less<>> tags_;
};
} // namespace redstone
