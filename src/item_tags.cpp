#include "redstone/item_tags.hpp"
#include "redstone/item.hpp"

#include <algorithm>
#include <unordered_map>

namespace redstone {
namespace {
#include "item_tags.inc"

std::string canonicalItemTag(std::string_view name) {
  if (!name.empty() && name.front() == '#')
    name.remove_prefix(1);
  if (name.find(':') != std::string_view::npos)
    return std::string(name);
  return std::string("minecraft:") + std::string(name);
}

std::string canonicalItemId(std::string_view name) {
  if (name.find(':') != std::string_view::npos)
    return std::string(name);
  return std::string("minecraft:") + std::string(name);
}

const ItemTagRegistry &vanillaItemTags() {
  static const ItemTagRegistry instance;
  return instance;
}
} // namespace

ItemTagRegistry::ItemTagRegistry() {
  for (const auto &entry : kVanillaItemTagSources) {
    std::vector<std::string> members;
    members.reserve(entry.items.size());
    for (std::string_view item : entry.items)
      members.emplace_back(item);
    std::sort(members.begin(), members.end());
    members.erase(std::unique(members.begin(), members.end()), members.end());
    tags_.emplace(std::string(entry.tag), std::move(members));
  }
}

std::optional<bool> ItemTagRegistry::contains(std::string_view tag,
                                            std::string_view item) const {
  const std::string tagKey = canonicalItemTag(tag);
  const auto it = tags_.find(tagKey);
  if (it == tags_.end())
    return std::nullopt;
  const std::string itemKey = canonicalItemId(item);
  return std::binary_search(it->second.begin(), it->second.end(), itemKey);
}

std::optional<bool> itemHasTag(const Item &item, std::string_view tag) {
  return vanillaItemTags().contains(tag, item.name());
}

std::optional<bool> itemHasTag(std::string_view itemId, std::string_view tag) {
  return vanillaItemTags().contains(tag, itemId);
}

std::vector<std::string_view> itemsInTag(std::string_view tag) {
  const std::string key = canonicalItemTag(tag);
  const auto &entries = vanillaItemTags().entries();
  const auto it = entries.find(key);
  if (it == entries.end())
    return {};
  std::vector<std::string_view> out;
  out.reserve(it->second.size());
  for (const auto &member : it->second)
    out.push_back(member);
  return out;
}

} // namespace redstone
