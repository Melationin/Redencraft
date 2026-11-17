#pragma once
// 物品系统核心定义：包含原版 26.1 物品静态元数据、DataComponents 实例组件、
// Item 注册表索引、方块-物品映射、工具挖掘/掉落评估及 ItemStack NBT 编解码接口。
#include "redstone/block.hpp"
#include "redstone/entity/entity_data.hpp"
#include "redstone/nbt.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace redstone {

// 物品稀有度（对应原版 Rarity，决定物品名称显示颜色）。
enum class ItemRarity : std::uint8_t {
  Common = 0,
  Uncommon = 1,
  Rare = 2,
  Epic = 3,
};

// 物品可穿戴或持握的装备槽位（对应原版 EquipmentSlot）。
enum class EquipSlotKind : std::uint8_t {
  None = 0,
  MainHand = 1,
  OffHand = 2,
  Feet = 3,
  Legs = 4,
  Chest = 5,
  Head = 6,
  Body = 7,
  Saddle = 8,
};

// 持续使用物品时的动作动画类型（对应原版 ItemUseAnimation）。
enum class ItemUseAnim : std::uint8_t {
  None = 0,
  Eat = 1,
  Drink = 2,
  Block = 3,
  Bow = 4,
  Trident = 5,
  Crossbow = 6,
  Spyglass = 7,
  TootHorn = 8,
  Brush = 9,
  Bundle = 10,
  Spear = 11,
};

// 物品行为分发类别：原版通过多态子类区分不同物品交互，C++ 侧收敛为紧凑枚举
// 以便在静态数据表与 switch 分发中实现零堆分配的行为路由。
enum class ItemBehaviorKind : std::uint8_t {
  Item = 0,
  Air,
  Block,
  StandingAndWallBlock,
  DoubleHighBlock,
  PlaceOnWaterBlock,
  ScaffoldingBlock,
  GameMasterBlock,
  Sign,
  HangingSign,
  Bed,
  Banner,
  PlayerHead,
  Pickaxe,
  Axe,
  Shovel,
  Hoe,
  Sword,
  Spear,
  Shears,
  Brush,
  FlintAndSteel,
  FireCharge,
  BoneMeal,
  Honeycomb,
  Bucket,
  SolidBucket,
  MobBucket,
  Bottle,
  Minecart,
  Boat,
  Snowball,
  Egg,
  EnderPearl,
  ExperienceBottle,
  WindCharge,
  FireworkRocket,
  Bow,
  Crossbow,
  Trident,
  Mace,
  Arrow,
  SpectralArrow,
  TippedArrow,
  ArmorStand,
  ItemFrame,
  EndCrystal,
  HangingEntity,
  SpawnEgg,
  Potion,
  SplashPotion,
  LingeringPotion,
  Dye,
  InkSac,
  GlowInkSac,
  Bundle,
  Shield,
  Spyglass,
  Compass,
  Map,
  EmptyMap,
  WritableBook,
  WrittenBook,
  EnderEye,
  FishingRod,
  Lead,
  NameTag,
  DebugStick,
  KnowledgeBook,
  Instrument,
  SmithingTemplate,
  DiscFragment,
};

// 单条工具挖掘规则（对应原版 Tool.Rule）：按方块 ID 或标签覆写挖掘速度与有效掉落判定。
struct ToolRuleDef {
  std::string_view target;
  bool isTag = false;
  float speed = 1.0f;
  bool hasSpeed = false;
  std::int8_t correctForDrops = -1; // -1 表示未指定，0 表示否，1 表示是
};

// 静态物品原型元数据：存放同一物品 ID 共享的只读固有属性（如堆叠上限、耐久、食物、
// 战斗/护甲、燃料与关联方块/实体），避免每个 ItemStack 实例重复存储静态配置。
struct ItemMetadata {
  std::uint16_t id = 0;
  std::uint8_t maxStackSize = 64;
  std::uint16_t maxDamage = 0;
  ItemRarity rarity = ItemRarity::Common;
  ItemBehaviorKind behavior = ItemBehaviorKind::Item;
  std::uint8_t enchantableValue = 0;
  bool fireResistant = false;
  bool glider = false;
  bool blocksAttacks = false;
  bool canDestroyInCreative = true;
  std::uint8_t toolDamagePerBlock = 0;
  std::uint8_t weaponDamagePerAttack = 0;
  bool isFood = false;
  int nutrition = 0;
  float saturation = 0.0f;
  bool canAlwaysEat = false;
  ItemUseAnim useAnim = ItemUseAnim::None;
  float consumeSeconds = 0.0f;
  float useCooldownSeconds = 0.0f;
  float attackDamageBonus = 0.0f;
  float attackSpeedModifier = 0.0f;
  float armorValue = 0.0f;
  float armorToughness = 0.0f;
  float knockbackResistance = 0.0f;
  EquipSlotKind equipSlot = EquipSlotKind::None;
  bool equipSwappable = false;
  bool equipDamageOnHurt = false;
  float compostChance = 0.0f;
  int fuelBurnTime = 0;
  std::string_view primaryBlockName{};
  std::string_view wallBlockName{};
  std::string_view spawnEntityType{};
  std::string_view useRemainderItem{};
  std::string_view craftingRemainderItem{};
  std::string_view repairMaterial{};
  std::uint32_t toolRuleOffset = 0;
  std::uint16_t toolRuleCount = 0;
};

// 单个 ItemStack 的可变组件数据（对应 26.1 DataComponents），用于表示附魔、耐久损耗、
// 自定义名称、预设方块状态、容器内容及未显式建模的扩展 NBT 组件。
struct ItemComponentData {
  std::optional<int> maxStackSize;
  std::optional<int> maxDamage;
  int damage = 0;
  bool unbreakable = false;
  std::optional<std::string> customName;
  std::optional<std::string> itemName;
  std::optional<int> repairCost;
  std::map<std::string, int> enchantments;
  std::map<std::string, int> storedEnchantments;
  std::map<std::string, std::string> blockState;
  std::string potionId;
  std::optional<std::uint32_t> dyedColor;
  std::vector<ItemStack> containerItems;
  std::vector<ItemStack> bundleContents;
  std::vector<ItemStack> chargedProjectiles;
  std::map<std::string, nbt::Tag> extraComponents;

  bool empty() const noexcept {
    return !maxStackSize && !maxDamage && damage == 0 && !unbreakable &&
           !customName && !itemName && !repairCost && enchantments.empty() &&
           storedEnchantments.empty() && blockState.empty() &&
           potionId.empty() && !dyedColor && containerItems.empty() &&
           bundleContents.empty() && chargedProjectiles.empty() &&
           extraComponents.empty();
  }
  bool operator==(const ItemComponentData &other) const;
};

// 物品注册表中的不可变类型单例：封装 ItemMetadata 查询与工具/标签判定；
// 实例唯一存在，因此可直接通过指针地址比较物品类型是否相同。
class Item {
public:
  Item(std::string name, ItemMetadata meta);

  std::uint16_t id() const noexcept { return meta_.id; }
  const std::string &name() const noexcept { return name_; }
  std::string_view shortName() const noexcept;
  const ItemMetadata &metadata() const noexcept { return meta_; }

  int maxStackSize() const noexcept { return meta_.maxStackSize; }
  int maxDamage() const noexcept { return meta_.maxDamage; }
  bool isDamageable() const noexcept { return meta_.maxDamage > 0; }
  ItemRarity rarity() const noexcept { return meta_.rarity; }
  ItemBehaviorKind behavior() const noexcept { return meta_.behavior; }
  std::uint8_t enchantableValue() const noexcept { return meta_.enchantableValue; }
  bool fireResistant() const noexcept { return meta_.fireResistant; }
  bool glider() const noexcept { return meta_.glider; }
  bool blocksAttacks() const noexcept { return meta_.blocksAttacks; }
  bool canDestroyInCreative() const noexcept { return meta_.canDestroyInCreative; }
  int toolDamagePerBlock() const noexcept { return meta_.toolDamagePerBlock; }
  int weaponDamagePerAttack() const noexcept { return meta_.weaponDamagePerAttack; }

  bool isFood() const noexcept { return meta_.isFood; }
  int nutrition() const noexcept { return meta_.nutrition; }
  float saturation() const noexcept { return meta_.saturation; }
  bool canAlwaysEat() const noexcept { return meta_.canAlwaysEat; }
  ItemUseAnim useAnim() const noexcept { return meta_.useAnim; }
  float consumeSeconds() const noexcept { return meta_.consumeSeconds; }
  float useCooldownSeconds() const noexcept { return meta_.useCooldownSeconds; }

  float attackDamageBonus() const noexcept { return meta_.attackDamageBonus; }
  float attackSpeedModifier() const noexcept { return meta_.attackSpeedModifier; }
  float armorValue() const noexcept { return meta_.armorValue; }
  float armorToughness() const noexcept { return meta_.armorToughness; }
  float knockbackResistance() const noexcept { return meta_.knockbackResistance; }
  EquipSlotKind equipSlot() const noexcept { return meta_.equipSlot; }
  bool equipSwappable() const noexcept { return meta_.equipSwappable; }
  bool equipDamageOnHurt() const noexcept { return meta_.equipDamageOnHurt; }

  float compostChance() const noexcept { return meta_.compostChance; }
  int fuelBurnTime() const noexcept { return meta_.fuelBurnTime; }

  std::string_view primaryBlockName() const noexcept { return meta_.primaryBlockName; }
  std::string_view wallBlockName() const noexcept { return meta_.wallBlockName; }
  const Block *primaryBlock() const;
  const Block *wallBlock() const;
  std::string_view spawnEntityType() const noexcept { return meta_.spawnEntityType; }
  std::string_view useRemainderItem() const noexcept { return meta_.useRemainderItem; }
  std::string_view craftingRemainderItem() const noexcept {
    return meta_.craftingRemainderItem;
  }
  std::string_view repairMaterial() const noexcept { return meta_.repairMaterial; }

  std::span<const ToolRuleDef> toolRules() const;
  float destroySpeed(const BlockState &state) const;
  bool isCorrectToolForDrops(const BlockState &state) const;

  bool hasTag(std::string_view tag) const;
  bool operator==(const Item &other) const noexcept { return this == &other; }

private:
  std::string name_;
  ItemMetadata meta_;
};

namespace Items {
#include "redstone/items_decl.inc"
} // namespace Items

const Item &airItem();
const Item *itemByName(std::string_view name);
const Item *itemById(std::uint16_t id);
const std::vector<const Item *> &allItems();

/// 精确复刻 26.1 原版 Item.BY_BLOCK 映射（1,092 个方块 -> Item），与 Items.java 对齐，
/// 用于方块的对应物品反查（如中键拾取与部分掉落转换）。
const Item *itemForBlock(const Block &block);
const Item *itemForBlockState(const BlockState &state);

/// 结合工具规则与附魔效果评估挖掘速度、正确工具掉落资格及方块掉落物列表。
float itemDestroySpeed(const ItemStack &stack, const BlockState &state);
bool itemIsCorrectToolForDrops(const ItemStack &stack, const BlockState &state);
std::vector<ItemStack> blockDropsForTool(const BlockState &state,
                                         const ItemStack &tool);

// 查询物品堆叠在熔炉中的燃烧时间（单位：游戏刻）及是否为可用燃料。
int itemFuelBurnTime(const ItemStack &stack);
bool isFuelItem(const ItemStack &stack);

/// 非破坏性辅助函数：复制并修改 ItemStack::components，保证组件写时按值更新语义清晰。
ItemStack withItemDamage(ItemStack stack, int damage);
ItemStack withItemUnbreakable(ItemStack stack, bool unbreakable = true);
ItemStack withItemCustomName(ItemStack stack, std::string name);
ItemStack withItemBlockState(ItemStack stack,
                             std::map<std::string, std::string> props);
ItemStack withItemEnchantment(ItemStack stack, std::string enchantmentId,
                              int level);
ItemStack withItemComponents(ItemStack stack, ItemComponentData data);

/// 原版 26.1 ItemStack NBT 编解码（包含 `"components"` 复合标签），用于容器持久化与存档交互。
nbt::Tag encodeItemStackNbt(const ItemStack &stack, bool modernLowercase = true);
std::optional<ItemStack> decodeItemStackNbt(const nbt::Tag &tag);

} // namespace redstone
