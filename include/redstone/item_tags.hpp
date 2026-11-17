#pragma once
// 原版 26.1 物品标签（ItemTags）注册表与查询接口，供工具规则、配方及物品行为判定使用。
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {
class Item;

// 从 assets/protocol775/configuration.bin 提取的原版 26.1 物品标签（共 207 个）；
// 未知标签返回 std::nullopt，以便调用方区分“标签不存在”与“物品不属于该标签”。
std::optional<bool> itemHasTag(const Item &item, std::string_view tag);
std::optional<bool> itemHasTag(std::string_view itemId, std::string_view tag);
std::vector<std::string_view> itemsInTag(std::string_view tag);

// 物品标签注册表：集中保存标签到物品 ID 列表的映射，支持 string_view 透明查找，
// 避免在高频工具规则和物品交互判定中构造临时 std::string。
class ItemTagRegistry {
public:
  ItemTagRegistry();
  std::optional<bool> contains(std::string_view tag, std::string_view item) const;
  const auto &entries() const { return tags_; }

private:
  std::map<std::string, std::vector<std::string>, std::less<>> tags_;
};
} // namespace redstone
