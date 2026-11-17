#pragma once
/// @file entity_data.hpp
/// @brief 定义实体物品栈（`ItemStack`）与同步实体数据（`SynchedEntityData`）系统。
///
/// 之所以将 `ItemStack` 与 `SynchedEntityData` 放在同一轻量头文件中，是因为实体基类、
/// 投掷物、载具和装饰实体都需要同步物品栈与元数据字段，抽离此处可避免头文件循环依赖。
#include "redstone/engine_types.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace redstone {
class BlockState;
class Item;
struct ItemComponentData;

/// 表示世界与实体背包中的物品堆叠，附带数据组件（`ItemComponentData`）与工具/耐久查询。
struct ItemStack {
  std::string id;
  int count = 0;
  std::shared_ptr<const ItemComponentData> components{};

  bool empty() const { return id.empty() || count <= 0; }
  const Item *item() const;
  int maxStackSize() const;
  int maxDamage() const;
  int damageValue() const;
  bool isDamaged() const;
  bool isDamageableItem() const;
  bool isUnbreakable() const;
  /// 扣除工具或护甲的耐久度（遵循不可破坏标签与创造模式规则）。
  /// 若物品因耐久耗尽而损坏（数量归零）则返回 `true`。
  bool hurtAndBreak(int amount, bool instabuild = false);
  bool isSameItem(const ItemStack &other) const;
  bool isSameItemSameComponents(const ItemStack &other) const;
  float destroySpeed(const BlockState &state) const;
  bool isCorrectToolForDrops(const BlockState &state) const;
  int fuelBurnTime() const;
  float compostChance() const;
  int enchantmentLevel(std::string_view enchantmentId) const;
  bool hasNonEmptyComponents() const;
  bool operator==(const ItemStack &other) const;
};
ItemStack itemStack(std::string id, int count = 1);
ItemStack itemStack(const Item &item, int count = 1);

// 对齐原版 26.1 的 `EntityDataSerializers` 序列化器 ID，与 `assets/protocol775/entity_data_serializers.tsv` 一致。
// 用于在服务端与协议层之间唯一标识各元数据字段的编码格式。
enum class EntityDataSerializer : std::uint8_t {
  Byte = 0,
  Int = 1,
  Long = 2,
  Float = 3,
  String = 4,
  Component = 5,
  OptionalComponent = 6,
  ItemStack = 7,
  Boolean = 8,
  Rotations = 9,
  BlockPos = 10,
  OptionalBlockPos = 11,
  Direction = 12,
  OptionalLivingEntityReference = 13,
  BlockState = 14,
  OptionalBlockState = 15,
  Particle = 16,
  Particles = 17,
  VillagerData = 18,
  OptionalUnsignedInt = 19,
  Pose = 20,
  CatVariant = 21,
  CatSoundVariant = 22,
  CowVariant = 23,
  CowSoundVariant = 24,
  WolfVariant = 25,
  WolfSoundVariant = 26,
  FrogVariant = 27,
  PigVariant = 28,
  PigSoundVariant = 29,
  ChickenVariant = 30,
  ChickenSoundVariant = 31,
  ZombieNautilusVariant = 32,
  OptionalGlobalPos = 33,
  PaintingVariant = 34,
  SnifferState = 35,
  ArmadilloState = 36,
  CopperGolemState = 37,
  WeatheringCopperState = 38,
  Vector3 = 39,
  Quaternion = 40,
  ResolvableProfile = 41,
  HumanoidArm = 42,
};

/// 三维欧拉旋转角（单位为度），用于盔甲架等实体的部位姿态同步。
struct Rotations3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  bool operator==(const Rotations3f &) const = default;
};

/// 三维单精度向量，对应 `EntityDataSerializer::Vector3` 的元数据载荷。
struct Vector3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  bool operator==(const Vector3f &) const = default;
};

using EntityUuid = std::array<std::int32_t, 4>;

/// 所有受支持的同步实体元数据值类型的统一变体，避免堆分配与虚函数表开销。
using EntityDataValue = std::variant<
    std::int8_t,                // Byte（字节）
    int,                        // Int / 注册表变体索引 / HumanoidArm
    std::int64_t,               // Long（长整型）
    float,                      // Float（单精度浮点）
    std::string,                // String / Component（字符串或文本组件）
    std::optional<std::string>, // OptionalComponent（可选文本组件）
    ItemStack,                  // ItemStack（物品栈）
    bool,                       // Boolean（布尔值）
    Rotations3f,                // Rotations（三维旋转）
    Pos,                        // BlockPos（方块坐标）
    std::optional<Pos>,         // OptionalBlockPos（可选方块坐标）
    Direction,                  // Direction（朝向）
    std::optional<EntityUuid>,  // OptionalLivingEntityReference（可选生物引用）
    BlockState,                 // BlockState（方块状态）
    std::optional<BlockState>,  // OptionalBlockState（可选方块状态）
    std::optional<std::uint32_t>, // OptionalUnsignedInt（可选无符号整数）
    Pose,                       // Pose（实体姿态）
    Vector3f>;                  // Vector3（三维浮点向量）

/// 单条打包后的实体同步元数据项，供网络包编码器直接消费。
struct EntityDataEntry {
  std::uint8_t id = 0;
  EntityDataSerializer serializer = EntityDataSerializer::Byte;
  EntityDataValue value{std::int8_t{0}};
};

/// 对应原版 `SynchedEntityData`，集中管理实体需要向客户端同步的元数据槽位。
/// 通过 `dirty_` 与单调递增的 `revision_` 追踪脏数据，既能增量下发变更（`packDirty` /
/// `dirtySince`），也能在新玩家进入追踪范围时只发送非默认值（`getNonDefaultValues`）以节省带宽。
class SynchedEntityData {
public:
  /// 单个同步数据槽位的运行时状态，记录初始默认值、当前值及最近变更版本号。
  struct Item {
    std::uint8_t id = 0;
    EntityDataSerializer serializer = EntityDataSerializer::Byte;
    EntityDataValue initialValue{std::int8_t{0}};
    EntityDataValue value{std::int8_t{0}};
    bool dirty = false;
    std::uint64_t changedRevision = 0;

    bool isSetToDefault() const { return value == initialValue; }
    EntityDataEntry packed() const { return {id, serializer, value}; }
  };

  using UpdateCallback = std::function<void(std::uint8_t)>;

  explicit SynchedEntityData(UpdateCallback onUpdate = {})
      : onUpdate_(std::move(onUpdate)) {}

  /// 设置字段变更回调；实体派生类借此在元数据被更新时同步更新自身缓存（如维度、生命值缓存）。
  void setUpdateCallback(UpdateCallback onUpdate) {
    onUpdate_ = std::move(onUpdate);
  }

  template <typename T>
  void define(std::uint8_t id, EntityDataSerializer serializer, T initial) {
    defineRaw(id, serializer, EntityDataValue(std::move(initial)));
  }

  void defineRaw(std::uint8_t id, EntityDataSerializer serializer,
                 EntityDataValue initial);

  bool has(std::uint8_t id) const;

  template <typename T> const T &get(std::uint8_t id) const {
    return std::get<T>(getItem(id).value);
  }

  template <typename T>
  void set(std::uint8_t id, T newValue, bool forceDirty = false) {
    setRaw(id, EntityDataValue(std::move(newValue)), forceDirty);
  }

  void setRaw(std::uint8_t id, EntityDataValue newValue,
              bool forceDirty = false);

  bool isDirty() const noexcept { return dirty_; }
  std::uint64_t revision() const noexcept { return revision_; }
  /// 打包自上次清理以来变脏的字段并清除全局脏标记，用于单广播者场景的增量同步。
  std::vector<EntityDataEntry> packDirty();
  /// 按版本号增量提取在 `sinceRevision` 之后发生变更的条目，用于多追踪会话互不干扰地独立同步。
  std::vector<EntityDataEntry> dirtySince(std::uint64_t sinceRevision) const;
  /// 返回所有当前值不等于初始默认值的字段，用于实体初次生成包的全量精简同步。
  std::vector<EntityDataEntry> getNonDefaultValues() const;
  std::vector<EntityDataEntry> getAllValues() const;

private:
  const Item &getItem(std::uint8_t id) const;
  Item &getItem(std::uint8_t id);

  UpdateCallback onUpdate_;
  std::vector<Item> items_;
  std::uint64_t revision_ = 0;
  bool dirty_ = false;
};

} // namespace redstone
