#include "redstone/item.hpp"
#include "redstone/block_tags.hpp"
#include "redstone/entity.hpp"
#include "redstone/item_tags.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace redstone {
namespace {

struct ItemRegistryStorage {
  std::vector<const Item *> byId;
  std::unordered_map<std::string, const Item *> byName;
};

ItemRegistryStorage &registryStorage() {
  static ItemRegistryStorage *storage = new ItemRegistryStorage();
  return *storage;
}

std::string canonicalId(std::string_view name) {
  if (name.empty())
    return {};
  if (name.find(':') != std::string_view::npos)
    return std::string(name);
  return std::string("minecraft:") + std::string(name);
}

bool nbtTagsEqual(const nbt::Tag &a, const nbt::Tag &b) {
  if (a.type != b.type)
    return false;
  switch (a.type) {
  case 1:
  case 2:
  case 3:
  case 4:
    return a.number == b.number;
  case 5:
  case 6:
    return a.real == b.real || (std::isnan(a.real) && std::isnan(b.real));
  case 7:
    return a.bytes == b.bytes;
  case 8:
    return a.text == b.text;
  case 9:
    if (a.listElementType != b.listElementType || a.list.size() != b.list.size())
      return false;
    for (std::size_t i = 0; i < a.list.size(); ++i) {
      if (!nbtTagsEqual(a.list[i], b.list[i]))
        return false;
    }
    return true;
  case 10:
    if (a.compound.size() != b.compound.size())
      return false;
    for (const auto &[k, v] : a.compound) {
      auto it = b.compound.find(k);
      if (it == b.compound.end() || !nbtTagsEqual(v, it->second))
        return false;
    }
    return true;
  case 11:
    return a.ints == b.ints;
  case 12:
    return a.longs == b.longs;
  default:
    return true;
  }
}

void addNbtField(nbt::Tag &compound, std::string key, nbt::Tag value) {
  compound.compoundOrder.push_back(key);
  compound.compound.emplace(std::move(key), std::move(value));
}

nbt::Tag intTag(std::uint8_t type, std::int64_t val) {
  nbt::Tag t;
  t.type = type;
  t.number = val;
  return t;
}

nbt::Tag strTag(std::string val) {
  nbt::Tag t;
  t.type = 8;
  t.text = std::move(val);
  return t;
}

const nbt::Tag *compoundField(const nbt::Tag &tag, std::string_view key) {
  if (tag.type != 10)
    return nullptr;
  auto it = tag.compound.find(std::string(key));
  return it == tag.compound.end() ? nullptr : &it->second;
}

std::optional<std::int64_t> compoundInt(const nbt::Tag &tag,
                                        std::string_view key) {
  const nbt::Tag *f = compoundField(tag, key);
  if (!f || f->type < 1 || f->type > 4)
    return std::nullopt;
  return f->number;
}

std::shared_ptr<ItemComponentData> cloneOrCreateComponents(const ItemStack &s) {
  if (s.components)
    return std::make_shared<ItemComponentData>(*s.components);
  return std::make_shared<ItemComponentData>();
}

} // namespace

#include "items_registry.inc"

Item::Item(std::string name, ItemMetadata meta)
    : name_(std::move(name)), meta_(meta) {
  auto &storage = registryStorage();
  if (storage.byId.size() <= meta_.id)
    storage.byId.resize(meta_.id + 1, nullptr);
  storage.byId[meta_.id] = this;
  storage.byName.emplace(name_, this);
}

std::string_view Item::shortName() const noexcept {
  const auto colon = name_.find(':');
  return colon == std::string::npos ? std::string_view(name_)
                                    : std::string_view(name_).substr(colon + 1);
}

const Block *Item::primaryBlock() const {
  if (meta_.primaryBlockName.empty())
    return nullptr;
  const BlockState s = defaultState(std::string(meta_.primaryBlockName));
  if (s.name() != meta_.primaryBlockName)
    return nullptr;
  return &s.block();
}

const Block *Item::wallBlock() const {
  if (meta_.wallBlockName.empty())
    return nullptr;
  const BlockState s = defaultState(std::string(meta_.wallBlockName));
  if (s.name() != meta_.wallBlockName)
    return nullptr;
  return &s.block();
}

std::span<const ToolRuleDef> Item::toolRules() const {
  if (meta_.toolRuleCount == 0)
    return {};
  return std::span<const ToolRuleDef>(kToolRules + meta_.toolRuleOffset,
                                      meta_.toolRuleCount);
}

float Item::destroySpeed(const BlockState &state) const {
  for (const ToolRuleDef &rule : toolRules()) {
    if (!rule.hasSpeed)
      continue;
    const bool matches = rule.isTag
                             ? blockHasTag(state, rule.target).value_or(false)
                             : (state.name() == rule.target);
    if (matches)
      return rule.speed;
  }
  return 1.0f;
}

bool Item::isCorrectToolForDrops(const BlockState &state) const {
  for (const ToolRuleDef &rule : toolRules()) {
    if (rule.correctForDrops < 0)
      continue;
    const bool matches = rule.isTag
                             ? blockHasTag(state, rule.target).value_or(false)
                             : (state.name() == rule.target);
    if (matches)
      return rule.correctForDrops != 0;
  }
  return false;
}

bool Item::hasTag(std::string_view tag) const {
  return itemHasTag(*this, tag).value_or(false);
}

bool ItemComponentData::operator==(const ItemComponentData &other) const {
  if (maxStackSize != other.maxStackSize || maxDamage != other.maxDamage ||
      damage != other.damage || unbreakable != other.unbreakable ||
      customName != other.customName || itemName != other.itemName ||
      repairCost != other.repairCost || enchantments != other.enchantments ||
      storedEnchantments != other.storedEnchantments ||
      blockState != other.blockState || potionId != other.potionId ||
      dyedColor != other.dyedColor || containerItems != other.containerItems ||
      bundleContents != other.bundleContents ||
      chargedProjectiles != other.chargedProjectiles ||
      extraComponents.size() != other.extraComponents.size())
    return false;
  for (const auto &[k, v] : extraComponents) {
    auto it = other.extraComponents.find(k);
    if (it == other.extraComponents.end() || !nbtTagsEqual(v, it->second))
      return false;
  }
  return true;
}

const Item &airItem() { return Items::AIR; }

const Item *itemByName(std::string_view name) {
  if (name.empty())
    return nullptr;
  const std::string key = canonicalId(name);
  const auto &m = registryStorage().byName;
  const auto it = m.find(key);
  return it == m.end() ? nullptr : it->second;
}

const Item *itemById(std::uint16_t id) {
  const auto &v = registryStorage().byId;
  return id < v.size() ? v[id] : nullptr;
}

const std::vector<const Item *> &allItems() {
  static const std::vector<const Item *> list = [] {
    std::vector<const Item *> out;
    out.reserve(registryStorage().byId.size());
    for (const Item *ptr : registryStorage().byId) {
      if (ptr)
        out.push_back(ptr);
    }
    return out;
  }();
  return list;
}

const Item *itemForBlock(const Block &block) {
  static const std::unordered_map<std::string_view, const Item *> byBlockName =
      [] {
        std::unordered_map<std::string_view, const Item *> m;
        m.reserve(std::size(kBlockToItemEntries));
        for (const auto &entry : kBlockToItemEntries)
          m.emplace(entry.block, entry.item);
        return m;
      }();
  const auto it = byBlockName.find(block.name());
  return it == byBlockName.end() ? nullptr : it->second;
}

const Item *itemForBlockState(const BlockState &state) {
  return itemForBlock(state.block());
}

// ---------------------------------------------------------------------------
// ItemStack methods
// ---------------------------------------------------------------------------
const Item *ItemStack::item() const { return itemByName(id); }

int ItemStack::maxStackSize() const {
  if (components && components->maxStackSize)
    return std::clamp(*components->maxStackSize, 1, 99);
  if (const Item *it = item())
    return it->maxStackSize();
  return 64;
}

int ItemStack::maxDamage() const {
  if (components && components->maxDamage)
    return std::max(0, *components->maxDamage);
  if (const Item *it = item())
    return it->maxDamage();
  return 0;
}

int ItemStack::damageValue() const {
  return components ? std::max(0, components->damage) : 0;
}

bool ItemStack::isDamaged() const {
  return isDamageableItem() && damageValue() > 0;
}

bool ItemStack::isDamageableItem() const {
  return !empty() && maxDamage() > 0 && !isUnbreakable();
}

bool ItemStack::isUnbreakable() const {
  return components && components->unbreakable;
}

bool ItemStack::hurtAndBreak(int amount, bool instabuild) {
  if (instabuild || amount <= 0 || !isDamageableItem())
    return false;
  const int limit = maxDamage();
  const int nextDamage = damageValue() + amount;
  if (nextDamage >= limit) {
    --count;
    if (count <= 0) {
      *this = {};
      return true;
    }
    auto copy = cloneOrCreateComponents(*this);
    copy->damage = 0;
    components = copy->empty() ? nullptr : std::move(copy);
    return true;
  }
  auto copy = cloneOrCreateComponents(*this);
  copy->damage = nextDamage;
  components = std::move(copy);
  return false;
}

bool ItemStack::isSameItem(const ItemStack &other) const {
  if (empty() && other.empty())
    return true;
  return !empty() && !other.empty() && id == other.id;
}

bool ItemStack::isSameItemSameComponents(const ItemStack &other) const {
  if (empty() && other.empty())
    return true;
  if (empty() || other.empty() || id != other.id)
    return false;
  const bool aEmpty = !components || components->empty();
  const bool bEmpty = !other.components || other.components->empty();
  if (aEmpty && bEmpty)
    return true;
  if (aEmpty || bEmpty)
    return false;
  return *components == *other.components;
}

float ItemStack::destroySpeed(const BlockState &state) const {
  return itemDestroySpeed(*this, state);
}

bool ItemStack::isCorrectToolForDrops(const BlockState &state) const {
  return itemIsCorrectToolForDrops(*this, state);
}

int ItemStack::fuelBurnTime() const { return itemFuelBurnTime(*this); }

float ItemStack::compostChance() const {
  if (empty())
    return 0.0f;
  if (const Item *it = item())
    return it->compostChance();
  return 0.0f;
}

int ItemStack::enchantmentLevel(std::string_view enchantmentId) const {
  if (!components || components->enchantments.empty())
    return 0;
  const std::string key = canonicalId(enchantmentId);
  const auto it = components->enchantments.find(key);
  return it == components->enchantments.end() ? 0 : std::max(0, it->second);
}

bool ItemStack::hasNonEmptyComponents() const {
  return components && !components->empty();
}

bool ItemStack::operator==(const ItemStack &other) const {
  if (count != other.count || id != other.id)
    return false;
  const bool aEmpty = !components || components->empty();
  const bool bEmpty = !other.components || other.components->empty();
  if (aEmpty && bEmpty)
    return true;
  if (aEmpty || bEmpty)
    return false;
  return *components == *other.components;
}

ItemStack itemStack(std::string id, int count) {
  if (id.empty() || count <= 0)
    return {};
  return {canonicalId(id), count, nullptr};
}

ItemStack itemStack(const Item &item, int count) {
  if (item.id() == 0 || count <= 0)
    return {};
  return {item.name(), count, nullptr};
}

float itemDestroySpeed(const ItemStack &stack, const BlockState &state) {
  if (stack.empty())
    return 1.0f;
  const Item *it = stack.item();
  if (!it)
    return 1.0f;
  return it->destroySpeed(state);
}

bool itemIsCorrectToolForDrops(const ItemStack &stack, const BlockState &state) {
  if (stack.empty())
    return false;
  const Item *it = stack.item();
  return it && it->isCorrectToolForDrops(state);
}

std::vector<ItemStack> blockDropsForTool(const BlockState &state,
                                         const ItemStack &tool) {
  if (state.isAir() || state.is(Blocks::WATER) || state.is(Blocks::LAVA) ||
      state.is(Blocks::MOVING_PISTON) || state.is(Blocks::PISTON_HEAD) ||
      state.is(Blocks::BEDROCK) || state.is(Blocks::FIRE) ||
      state.is(Blocks::SOUL_FIRE) || state.is(Blocks::NETHER_PORTAL) ||
      state.is(Blocks::END_PORTAL) || state.is(Blocks::END_GATEWAY))
    return {};
  if (state.is(Blocks::TNT) && unstableOf(state))
    return {};

  // Check if tool is required (vanilla requiresCorrectToolForDrops on blocks).
  // Evaluate #minecraft:incorrect_for_* tags + #minecraft:needs_*_tool tags.
  const bool needsWooden = false; // stone/diamond/iron tiers tracked below
  (void)needsWooden;
  if (blockHasTag(state, "minecraft:needs_diamond_tool").value_or(false) &&
      !tool.isCorrectToolForDrops(state))
    return {};
  if (blockHasTag(state, "minecraft:needs_iron_tool").value_or(false) &&
      !tool.isCorrectToolForDrops(state))
    return {};
  if (blockHasTag(state, "minecraft:needs_stone_tool").value_or(false) &&
      !tool.isCorrectToolForDrops(state))
    return {};

  const bool hasSilkTouch = tool.enchantmentLevel("minecraft:silk_touch") > 0;
  if (hasSilkTouch) {
    if (const Item *selfItem = itemForBlockState(state))
      return {itemStack(*selfItem, 1)};
  }

  return blockDrops(state);
}

int itemFuelBurnTime(const ItemStack &stack) {
  if (stack.empty())
    return 0;
  const Item *it = stack.item();
  return it ? it->fuelBurnTime() : 0;
}

bool isFuelItem(const ItemStack &stack) { return itemFuelBurnTime(stack) > 0; }

ItemStack withItemDamage(ItemStack stack, int damage) {
  if (stack.empty())
    return stack;
  auto data = cloneOrCreateComponents(stack);
  data->damage = std::max(0, damage);
  stack.components = data->empty() ? nullptr : std::move(data);
  return stack;
}

ItemStack withItemUnbreakable(ItemStack stack, bool unbreakable) {
  if (stack.empty())
    return stack;
  auto data = cloneOrCreateComponents(stack);
  data->unbreakable = unbreakable;
  stack.components = data->empty() ? nullptr : std::move(data);
  return stack;
}

ItemStack withItemCustomName(ItemStack stack, std::string name) {
  if (stack.empty())
    return stack;
  auto data = cloneOrCreateComponents(stack);
  data->customName = std::move(name);
  stack.components = std::move(data);
  return stack;
}

ItemStack withItemBlockState(ItemStack stack,
                             std::map<std::string, std::string> props) {
  if (stack.empty())
    return stack;
  auto data = cloneOrCreateComponents(stack);
  data->blockState = std::move(props);
  stack.components = data->empty() ? nullptr : std::move(data);
  return stack;
}

ItemStack withItemEnchantment(ItemStack stack, std::string enchantmentId,
                              int level) {
  if (stack.empty())
    return stack;
  auto data = cloneOrCreateComponents(stack);
  const std::string key = canonicalId(enchantmentId);
  if (level <= 0)
    data->enchantments.erase(key);
  else
    data->enchantments[key] = level;
  stack.components = data->empty() ? nullptr : std::move(data);
  return stack;
}

ItemStack withItemComponents(ItemStack stack, ItemComponentData data) {
  if (stack.empty())
    return stack;
  if (data.empty())
    stack.components.reset();
  else
    stack.components = std::make_shared<ItemComponentData>(std::move(data));
  return stack;
}

// ---------------------------------------------------------------------------
// Vanilla 26.1 ItemStack NBT encode / decode (with DataComponents)
// ---------------------------------------------------------------------------
nbt::Tag encodeItemStackNbt(const ItemStack &stack, bool modernLowercase) {
  nbt::Tag tag;
  tag.type = 10;
  if (stack.empty())
    return tag;
  addNbtField(tag, "id", strTag(stack.id));
  if (modernLowercase)
    addNbtField(tag, "count", intTag(3, stack.count));
  else
    addNbtField(tag, "Count", intTag(1, stack.count));

  if (!stack.components || stack.components->empty())
    return tag;

  const ItemComponentData &c = *stack.components;
  nbt::Tag comp;
  comp.type = 10;
  if (c.maxStackSize)
    addNbtField(comp, "minecraft:max_stack_size", intTag(3, *c.maxStackSize));
  if (c.maxDamage)
    addNbtField(comp, "minecraft:max_damage", intTag(3, *c.maxDamage));
  if (c.damage > 0)
    addNbtField(comp, "minecraft:damage", intTag(3, c.damage));
  if (c.unbreakable) {
    nbt::Tag unit;
    unit.type = 10;
    addNbtField(comp, "minecraft:unbreakable", std::move(unit));
  }
  if (c.customName)
    addNbtField(comp, "minecraft:custom_name", strTag(*c.customName));
  if (c.itemName)
    addNbtField(comp, "minecraft:item_name", strTag(*c.itemName));
  if (c.repairCost)
    addNbtField(comp, "minecraft:repair_cost", intTag(3, *c.repairCost));
  if (!c.enchantments.empty()) {
    nbt::Tag enchs;
    enchs.type = 10;
    for (const auto &[eid, lvl] : c.enchantments)
      addNbtField(enchs, eid, intTag(3, lvl));
    addNbtField(comp, "minecraft:enchantments", std::move(enchs));
  }
  if (!c.storedEnchantments.empty()) {
    nbt::Tag enchs;
    enchs.type = 10;
    for (const auto &[eid, lvl] : c.storedEnchantments)
      addNbtField(enchs, eid, intTag(3, lvl));
    addNbtField(comp, "minecraft:stored_enchantments", std::move(enchs));
  }
  if (!c.blockState.empty()) {
    nbt::Tag bs;
    bs.type = 10;
    for (const auto &[k, v] : c.blockState)
      addNbtField(bs, k, strTag(v));
    addNbtField(comp, "minecraft:block_state", std::move(bs));
  }
  if (!c.potionId.empty()) {
    nbt::Tag pot;
    pot.type = 10;
    addNbtField(pot, "potion", strTag(c.potionId));
    addNbtField(comp, "minecraft:potion_contents", std::move(pot));
  }
  if (c.dyedColor)
    addNbtField(comp, "minecraft:dyed_color",
                intTag(3, static_cast<std::int32_t>(*c.dyedColor)));
  if (!c.containerItems.empty()) {
    nbt::Tag list;
    list.type = 9;
    list.listElementType = 10;
    for (int i = 0; i < static_cast<int>(c.containerItems.size()); ++i) {
      if (c.containerItems[i].empty())
        continue;
      nbt::Tag entry;
      entry.type = 10;
      addNbtField(entry, "slot", intTag(3, i));
      addNbtField(entry, "item", encodeItemStackNbt(c.containerItems[i], true));
      list.list.push_back(std::move(entry));
    }
    addNbtField(comp, "minecraft:container", std::move(list));
  }
  if (!c.bundleContents.empty()) {
    nbt::Tag list;
    list.type = 9;
    list.listElementType = 10;
    for (const auto &entry : c.bundleContents) {
      if (!entry.empty())
        list.list.push_back(encodeItemStackNbt(entry, true));
    }
    addNbtField(comp, "minecraft:bundle_contents", std::move(list));
  }
  if (!c.chargedProjectiles.empty()) {
    nbt::Tag list;
    list.type = 9;
    list.listElementType = 10;
    for (const auto &entry : c.chargedProjectiles) {
      if (!entry.empty())
        list.list.push_back(encodeItemStackNbt(entry, true));
    }
    addNbtField(comp, "minecraft:charged_projectiles", std::move(list));
  }
  for (const auto &[k, v] : c.extraComponents) {
    if (comp.compound.find(k) == comp.compound.end())
      addNbtField(comp, k, v);
  }
  if (!comp.compound.empty())
    addNbtField(tag, "components", std::move(comp));
  return tag;
}

std::optional<ItemStack> decodeItemStackNbt(const nbt::Tag &tag) {
  if (tag.type != 10)
    return std::nullopt;
  const nbt::Tag *idTag = compoundField(tag, "id");
  if (!idTag || idTag->type != 8 || idTag->text.empty())
    return std::nullopt;
  const auto countVal = compoundInt(tag, "count")
                            ? compoundInt(tag, "count")
                            : compoundInt(tag, "Count");
  const int cnt = countVal ? static_cast<int>(*countVal) : 1;
  if (cnt <= 0)
    return ItemStack{};

  ItemStack stack = itemStack(idTag->text, cnt);
  const nbt::Tag *comp = compoundField(tag, "components");
  if (!comp || comp->type != 10 || comp->compound.empty())
    return stack;

  ItemComponentData data;
  for (const auto &[rawKey, val] : comp->compound) {
    const std::string key = canonicalId(rawKey);
    if (key == "minecraft:max_stack_size" && val.type >= 1 && val.type <= 4) {
      data.maxStackSize = std::clamp(static_cast<int>(val.number), 1, 99);
    } else if (key == "minecraft:max_damage" && val.type >= 1 && val.type <= 4) {
      data.maxDamage = std::max(0, static_cast<int>(val.number));
    } else if (key == "minecraft:damage" && val.type >= 1 && val.type <= 4) {
      data.damage = std::max(0, static_cast<int>(val.number));
    } else if (key == "minecraft:unbreakable") {
      data.unbreakable = true;
    } else if (key == "minecraft:custom_name" && val.type == 8) {
      data.customName = val.text;
    } else if (key == "minecraft:item_name" && val.type == 8) {
      data.itemName = val.text;
    } else if (key == "minecraft:repair_cost" && val.type >= 1 && val.type <= 4) {
      data.repairCost = static_cast<int>(val.number);
    } else if (key == "minecraft:enchantments" && val.type == 10) {
      for (const auto &[ek, ev] : val.compound) {
        if (ek == "levels" && ev.type == 10) {
          for (const auto &[lek, lev] : ev.compound) {
            if (lev.type >= 1 && lev.type <= 4)
              data.enchantments[canonicalId(lek)] = static_cast<int>(lev.number);
          }
        } else if (ev.type >= 1 && ev.type <= 4) {
          data.enchantments[canonicalId(ek)] = static_cast<int>(ev.number);
        }
      }
    } else if (key == "minecraft:stored_enchantments" && val.type == 10) {
      for (const auto &[ek, ev] : val.compound) {
        if (ev.type >= 1 && ev.type <= 4)
          data.storedEnchantments[canonicalId(ek)] = static_cast<int>(ev.number);
      }
    } else if (key == "minecraft:block_state" && val.type == 10) {
      for (const auto &[pk, pv] : val.compound) {
        if (pv.type == 8)
          data.blockState[pk] = pv.text;
      }
    } else if (key == "minecraft:potion_contents") {
      if (val.type == 8) {
        data.potionId = canonicalId(val.text);
      } else if (val.type == 10) {
        const nbt::Tag *pt = compoundField(val, "potion");
        if (pt && pt->type == 8)
          data.potionId = canonicalId(pt->text);
        else
          data.extraComponents.emplace(key, val);
      }
    } else if (key == "minecraft:dyed_color") {
      if (val.type >= 1 && val.type <= 4) {
        data.dyedColor = static_cast<std::uint32_t>(val.number);
      } else if (val.type == 10) {
        if (auto rgb = compoundInt(val, "rgb"))
          data.dyedColor = static_cast<std::uint32_t>(*rgb);
      }
    } else if (key == "minecraft:container" && val.type == 9) {
      for (const auto &entry : val.list) {
        if (entry.type != 10)
          continue;
        const auto slotOpt = compoundInt(entry, "slot");
        const nbt::Tag *itemTag = compoundField(entry, "item");
        if (!slotOpt || !itemTag || *slotOpt < 0 || *slotOpt > 255)
          continue;
        auto decoded = decodeItemStackNbt(*itemTag);
        if (!decoded)
          continue;
        if (static_cast<std::size_t>(*slotOpt) >= data.containerItems.size())
          data.containerItems.resize(static_cast<std::size_t>(*slotOpt) + 1);
        data.containerItems[static_cast<std::size_t>(*slotOpt)] =
            std::move(*decoded);
      }
    } else if (key == "minecraft:bundle_contents" && val.type == 9) {
      for (const auto &entry : val.list) {
        if (auto decoded = decodeItemStackNbt(entry))
          data.bundleContents.push_back(std::move(*decoded));
      }
    } else if (key == "minecraft:charged_projectiles" && val.type == 9) {
      for (const auto &entry : val.list) {
        if (auto decoded = decodeItemStackNbt(entry))
          data.chargedProjectiles.push_back(std::move(*decoded));
      }
    } else {
      data.extraComponents.emplace(key, val);
    }
  }
  return withItemComponents(std::move(stack), std::move(data));
}

} // namespace redstone
