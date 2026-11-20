#pragma once
// 方块与方块状态核心模型：定义轻量级 16 字节 `BlockState` 句柄、`Property`/`StateDefinition`
// 笛卡尔积状态定义与位域布局、`Block` 注册表单例、`BlockRef` 轻量句柄、`BlockBehaviours`
// 静态行为分发表，以及编译期/启动期只读分类表与属性访问器。
#include <array>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "redstone/hash.hpp"
#include "redstone/property.hpp"

namespace redstone {
class Block;
class BlockRef;
class BlockState;
class Random;
class RandomTickAccess;
class World;
enum class Direction;
struct Pos;
// 随机刻执行结果：用于区分回调已按原版逻辑处理（`Applied`）还是对应方块机制尚未建模（`Unsupported`）。
enum class RandomTickResult { Applied, Unsupported };

namespace detail {
// 注册表查找表以“扁平数组指针 + 元素计数”形式发布，使 `BlockState::fromId`、随机刻标记判定与
// 碰撞分类判定在热路径上仅需一次边界检查和数组索引加载，而无需哈希表或虚函数派发。
// 指针与计数在任何静态构造函数运行前均已零初始化（或指向零状态占位），并在每次方块注册后刷新，
// 因此在注册前、注册中与注册后都能安全解析状态。
extern const BlockState *gStateData;
extern std::uint32_t gStateCount;
extern const std::uint8_t *const gRandomTickStateFlags;
extern const std::uint32_t gRandomTickStateCount;
extern const std::uint8_t *const gCollisionKindFlags;
extern const std::uint32_t gCollisionKindCount;
// `minecraft:air` 单例指针：在空气方块完成静态注册后立即发布，从而保证默认构造的
// `BlockState` 直接指向已注册的空气状态（而非空指针占位）。
extern const Block *gAirBlock;
void registerBlock(Block &block);
void bindBlockBehaviours(Block &block);
void bindRandomTickBehaviour(Block &block);
std::uint32_t internState(const Block *block);
struct Tables;
Tables &tables();
} // namespace detail

// 返回全局 `minecraft:air` 单例；用于默认构造状态或寄存器尚未就绪时的回退。
const Block &airBlock();

// ---------------------------------------------------------------------------
// BlockState（对应 `net/minecraft/world/level/block/state/BlockBehaviour.BlockStateBase`）
//
// 每个状态是由唯一 `Block` 单例拥有的注册表实例。结构内部仅保存所属 `Block` 指针、
// 注册表状态 `id_` 与规范属性位域 `packed_`：按值拷贝只需两个机器字，相等判定等价于
// `id_` 比较，而 `state.is(Blocks::SAND)` 则是指针身份对比，直接取代原版 Java 在
// `getBlock()` 上所做的 `instanceof` 或类型相等判定。
// ---------------------------------------------------------------------------
class BlockState {
public:
  using Id = std::uint32_t;
  BlockState() : block_(detail::gAirBlock) {} // 默认构造为 `minecraft:air`（id 0）
  explicit BlockState(std::string name);
  BlockState(std::string name, std::map<std::string, std::string> properties);
  static BlockState fromId(Id value) {
    // 单次钳位加载：表首位置 0 始终是空状态/空气占位，因此越界 `id` 会无分支回退到
    // 空气状态，避免构造临时对象。
    return detail::gStateData[value < detail::gStateCount ? value : 0u];
  }
  Id id() const { return id_; }
  // 规范属性位域（按 `PropertyKind` 分配位移与位宽）。无属性方块的默认状态为 0；
  // 对于不在方块 `StateDefinition` 内的导入扩展状态无意义，因此访问器会先检查 `isSchemaState()`。
  std::uint32_t packed() const { return packed_; }
  const Block *blockOrNull() const { return block_; }
  // 对应 `AbstractBlockState.getBlock`：返回所属方块单例引用。
  const Block &block() const;
  // 对应 `BlockState.is(Block)`：按对象地址判断方块身份，替代原版的 `instanceof`。
  bool is(const Block &other) const { return block_ == &other; }
  bool sameBlock(const BlockState &other) const {
    return block_ == other.block_;
  }
  bool isAir() const;
  const std::string &name() const; // 返回带 `"minecraft:"` 前缀的规范名称
  std::string_view nameView() const;
  std::string_view shortName() const;
  std::map<std::string, std::string> properties() const;
  std::string get(const std::string &key,
                  const std::string &fallback = "") const;
  std::string_view getView(std::string_view key,
                           std::string_view fallback = std::string_view{}) const;
  bool hasProperty(std::string_view key) const;
  // 对应 `StateHolder.setValue`：按属性名切换到已注册的相邻状态（若越界则抛出异常）。
  BlockState with(std::string_view key, std::string_view value) const;
  // 通过规范 `PropertyKind` 进行状态切换：先在 `packed()` 上更新对应位域，再结合属性步长
  // 直接算出稠密变体序号与目标状态 `id`。传入不属于本方块取值列表的值将保持原状态或按实现校验。
  BlockState with(PropertyKind kind, std::uint8_t value) const;
  // 注册表 `id` 唯一标识一个状态（一状态一 ID）。
  bool operator==(const BlockState &other) const { return id_ == other.id_; }
  // 返回当前状态在所属方块稠密变体空间中的偏移索引；超出 `StateDefinition` 范围的属于
  // 导入保留的扩展状态（extension states）。
  std::uint32_t variant() const;
  bool isSchemaState() const;
  // 返回 `std::nullopt` 表示该方块的随机刻行为声明尚未建模，调用方不得静默猜测。
  std::optional<bool> isRandomlyTicking() const;
  RandomTickResult randomTick(RandomTickAccess &level, Pos pos, Random &random) const;

private:
  friend void detail::registerBlock(Block &);
  friend std::uint32_t detail::internState(const Block *);
  BlockState(const Block *owner, Id id, std::uint32_t packed = 0)
      : block_(owner), id_(id), packed_(packed) {}
  std::string_view extensionValue(std::string_view key,
                                  std::string_view fallback) const;
  bool extensionHas(std::string_view key) const;
  std::map<std::string, std::string> extensionProperties() const;
  const Block *block_ = nullptr;
  Id id_ = 0;
  std::uint32_t packed_ = 0;
};
// `detail::kNullState` 是按两个 `uint64_t` 重新解释为 `BlockState` 的静态占位，且注册表按 `id`
// 直接下标访问，因此 `BlockState` 的内存布局必须严格维持在 16 字节（8 字节对齐）。
static_assert(sizeof(BlockState) == 16 && alignof(BlockState) == 8,
              "BlockState must stay 16 bytes");

// ---------------------------------------------------------------------------
// StateDefinition（对应 `net/minecraft/world/level/block/state/StateDefinition`）
//
// 每个方块持有一组有序属性定义；其全部合法状态构成属性取值的笛卡尔积，并在模拟启动前完成注册。
// 每个状态拥有连续的稠密变体下标（`variant`），使 `StateHolder.setValue` 退化为步长加减算术，
// 直接定位已注册的相邻状态，无需动态分配或维护状态转换邻接表。
// ---------------------------------------------------------------------------
class Property {
public:
  static constexpr std::uint8_t kAbsent = 0xFF;
  Property() = default;
  Property(std::string name, std::vector<std::string> values,
           std::string defaultValue = {});
  const std::string &name() const { return name_; }
  const std::vector<std::string> &values() const { return values_; }
  std::uint32_t defaultIndex() const { return defaultIndex_; }
  bool indexOf(std::string_view value, std::uint32_t &index) const;
  // 规范属性身份与双向值映射（在构造/注册期填好）：局部索引（`localIndex`）遵循本方块自身的
  // 声明顺序（用于计算稠密变体步长），规范值（`canonicalValue`）则为所有使用该 `PropertyKind`
  // 的方块所共享（用于无分支写入 `BlockState::packed()`）。
  PropertyKind kind() const { return kind_; }
  std::uint8_t canonicalValue(std::uint32_t localIndex) const {
    return valueByLocal_[localIndex];
  }
  std::uint8_t localIndex(std::uint32_t canonicalValue) const {
    return localByValue_[canonicalValue];
  }

private:
  std::string name_;
  std::vector<std::string> values_;
  phmap::flat_hash_map<std::string, std::uint32_t, AnkerlStringHash,
                       std::equal_to<>>
      valueIndex_;
  std::uint32_t defaultIndex_ = 0;
  PropertyKind kind_ = PropertyKind::Count;
  std::array<std::uint8_t, kMaxPropertyValue + 1> valueByLocal_{};
  std::array<std::uint8_t, kMaxPropertyValue + 1> localByValue_{};
};

// 单个属性在 `BlockState::packed()` 中的位域布局描述：规范值从第 `shift` 位起占用 `width` 位，
// `stride` 为该属性在稠密变体空间中的步长权重；若方块未声明此属性则 `property` 为空。
struct PropertyLayout {
  const Property *property = nullptr; // 当方块不具备该 `PropertyKind` 时为 `nullptr`
  std::uint32_t stride = 0;
  std::uint32_t mask = 0;
  std::uint8_t shift = 0;
  std::uint8_t width = 0;
  std::uint8_t count = 0;
};

class StateDefinition {
public:
  using Properties = std::vector<Property>;
  StateDefinition() = default;
  explicit StateDefinition(Properties properties);
  StateDefinition(std::initializer_list<Property> properties)
      : StateDefinition(Properties(properties)) {}
  const Properties &properties() const { return properties_; }
  std::uint32_t stateCount() const { return stateCount_; }
  std::uint32_t defaultVariant() const { return defaultVariant_; }
  const Property *find(std::string_view name) const;
  // 未指定的属性取默认值；但若传入了不属于本定义的值或属性名则直接拒绝，
  // 防止错误数据静默回退到默认状态。
  bool variantOf(const std::map<std::string, std::string> &values,
                 std::uint32_t &variant) const;
  bool with(std::uint32_t variant, std::string_view key, std::string_view value,
            std::uint32_t &result) const;
  std::string_view value(std::uint32_t variant, std::string_view key,
                          std::string_view fallback = {}) const;
  std::map<std::string, std::string> values(std::uint32_t variant) const;
  // 枚举快速路径：按 `PropertyKind` 直接索引位域布局，完全跳过属性名二分查找。
  const PropertyLayout &layout(PropertyKind kind) const {
    return layout_[static_cast<std::size_t>(kind)];
  }
  // 计算指定稠密变体序号对应的规范属性位域编码。
  std::uint32_t packedOf(std::uint32_t variant) const;
  // 替换单个规范属性位域并同步更新稠密变体序号。若方块无此属性或该规范值不在本方块的
  // 允许取值列表中，则返回 `false`。
  bool withPacked(std::uint32_t packed, std::uint32_t variant, PropertyKind kind,
                  std::uint8_t value, std::uint32_t &nextVariant,
                  std::uint32_t &nextPacked) const;

private:
  Properties properties_; // 按属性名排序以支持二分查找
  std::vector<std::uint32_t> stride_;
  std::uint32_t stateCount_ = 1;
  std::uint32_t defaultVariant_ = 0;
  std::array<PropertyLayout, kPropertyKindCount> layout_{};
};

// 对应 `BlockBehaviour.BlockStateBase` 的光照属性字段。之所以刻意将 `canOcclude`、
// `useShapeForLightOcclusion` 与 `occlusionShapeId` 独立于碰撞体几何保存，是因为原版中
// 光照遮挡形状与物理碰撞形状是两套独立定义（例如玻璃、树叶、台阶等方块）；
// 同时通过面形 ID 保留精确的定向遮光形状数据，供光照引擎直接索引。
struct BlockLightProperties {
  std::uint8_t emission = 0;
  std::uint8_t dampening = 0;
  // 指向不可变的原版定向面形表下标（0..63）；0 表示空面形，且形状身份独立于碰撞形状。
  std::uint16_t occlusionShapeId : 6 = 0;
  std::uint16_t supported : 1 = 0;
  std::uint16_t canOcclude : 1 = 0;
  std::uint16_t useShapeForLightOcclusion : 1 = 0;
  std::uint16_t propagatesSkylightDown : 1 = 1;
  std::uint16_t : 6; // 保留填充位，保证整个结构体严格为 4 字节
  bool operator==(const BlockLightProperties &) const = default;
};
static_assert(sizeof(BlockLightProperties) == 4,
              "BlockLightProperties must stay four bytes");

// 方块类别位掩码：用于在不使用 C++ RTTI / 虚继承层次的情况下，在 O(1) 位测试中复刻
// 原版 Java 方块类继承体系的 `instanceof` 判定（如墙、栅栏、楼梯、活塞、容器等）。
enum class BlockClassFlag : std::uint64_t {
  None = 0,
  AirLike = 1ULL << 0,
  Rail = 1ULL << 1,
  Liquid = 1ULL << 2,
  Carpet = 1ULL << 3,
  Wall = 1ULL << 4,
  Fence = 1ULL << 5,
  FenceGate = 1ULL << 6,
  Stairs = 1ULL << 7,
  Slab = 1ULL << 8,
  Door = 1ULL << 9,
  Trapdoor = 1ULL << 10,
  Button = 1ULL << 11,
  PressurePlate = 1ULL << 12,
  Sign = 1ULL << 13,
  Leaves = 1ULL << 14,
  Sapling = 1ULL << 15,
  Falling = 1ULL << 16,
  CoralFan = 1ULL << 17,
  CoralWallFan = 1ULL << 18,
  Snowy = 1ULL << 19,
  Flower = 1ULL << 20,
  Vegetation = 1ULL << 21,
  DryVegetation = 1ULL << 22,
  RotatedPillar = 1ULL << 23,
  Bed = 1ULL << 24,
  Chest = 1ULL << 25,
  ShulkerBox = 1ULL << 26,
  Piston = 1ULL << 27,
  MovingPiston = 1ULL << 28,
  CopperBulb = 1ULL << 29,
  Candle = 1ULL << 30,
  Cauldron = 1ULL << 31,
  Banner = 1ULL << 32,
  Skull = 1ULL << 33,
  WaterloggedHost = 1ULL << 34,
  HasBlockEntity = 1ULL << 35,
};

// 对应 `BlockBehaviour.Properties` 子集（参见 `docs/compatibility.md`）：
// 将每方块的固有静态属性集中预存于 `Block` 单例，避免模拟时按名称分支判断。
struct BlockMetadata {
  float hardness;
  bool conductor;
  std::string pushReaction;
  bool supported = false;
  float friction = 0.6f;
  float speedFactor = 1.0f;
  float explosionResistance = 0.0f;
  bool liquid = false;
  // 对应 `BlockBehaviour.Properties.forceSolidOn()`：无论碰撞体形状如何，该状态一律视为实心。
  // 26.1 在栅栏门与 `moving_piston` 上启用此标记，使打开的栅栏门仍能阻挡流体，
  // 且移动中的活塞永远不会被当成空单元格。
  bool forceSolid = false;
  // 对应 `Block instanceof LiquidBlockContainer`：在本注册表子集中，凡声明了 `waterlogged`
  // 属性的方块均实现 `SimpleWaterloggedBlock`（也是此处唯一的 `LiquidBlockContainer`）；
  // 用单一布尔标记替代对属性声明的字符串比对（参见 `FlowingFluid.java:410-435`）。
  bool liquidContainer = false;
  bool replaceable = false;
  bool ignitedByLava = false;
  Instrument instrument = Instrument::Harp;
  std::uint64_t classFlags = 0;
};

// 用于尚未收录进官方元数据表的导入状态占位光属性：未知状态绝不会凭空猜测光照值。
const BlockLightProperties &unsupportedLightProperties() noexcept;
const BlockLightProperties &lightPropertiesOf(const BlockState &state);
BlockLightProperties officialLightProperties(
    std::string_view blockName, std::string_view properties);
// 对应 `BlockBehaviour.BlockStateBase.blocksMotion`（针对已注册方块子集）。
bool blocksMotion(const BlockState &state);
bool stateIsRandomlyTicking(const BlockState &state);
// 按 `BlockState::Id` 直接索引编译期静态随机刻布尔表：区块存储与随机刻扫描每刻要遍历大量
// 4096 格区块段，扁平字节表可实现 O(1) 过滤而无需构造/解包 `BlockState` 或访问方块元数据。
inline bool stateIsRandomlyTickingId(BlockState::Id id) {
  return id < detail::gRandomTickStateCount &&
         detail::gRandomTickStateFlags[id] != 0;
}
// 方块碰撞体静态粗分类：用于在编译期将所有官方方块状态按碰撞几何分为四类，
// 使实体碰撞检测（`world_entities.cpp`）在绝大多数空气或整块方块上走零分配快路径，
// 并精准识别可能跨格溢出的高碰撞体（如栅栏/墙体的 1.5 格高碰撞箱）。
enum class BlockCollisionKind : std::uint8_t {
  Empty = 0,
  FullCube = 1,
  Partial = 2,
  Oversized = 3,
};

// 按 `BlockState::Id` 查询静态编译期碰撞分类表；对超出内置表范围的动态/扩展状态安全回退。
inline BlockCollisionKind stateCollisionKindId(BlockState::Id id) {
  if (id == 0)
    return BlockCollisionKind::Empty;
  if (id < detail::gCollisionKindCount)
    return static_cast<BlockCollisionKind>(detail::gCollisionKindFlags[id]);
  const BlockState s = BlockState::fromId(id);
  if (s.isAir())
    return BlockCollisionKind::Empty;
  return s.isSchemaState() ? BlockCollisionKind::Partial
                           : BlockCollisionKind::FullCube;
}

bool stateBlocksMotion(const BlockState &state);
bool stateIsRedstoneConductor(const BlockState &state);
struct BlockRegistration {
  std::string name;
  BlockMetadata metadata;
};

// 对应 `BlockBehaviour` 的每方块行为分发表。原版通过上千个 `Block` 子类覆写虚方法实现行为差异；
// 本引擎避免将海量继承类暴露在头文件中，改为由每个 `Block` 单例在静态注册期通过
// `bindBlockBehaviours` 一次性绑定静态函数指针，同时把空气、流体、活塞、铁轨等最高频判断
// 保留为 `World` 中的内联快路径。
struct BlockBehaviours {
  using OnPlaceFn = void (*)(World &world, Pos pos, const BlockState &state,
                             const BlockState &oldState, bool movedByPiston);
  using AffectRemovalFn = void (*)(World &world, Pos pos,
                                   const BlockState &state,
                                   bool movedByPiston);
  using NeighborChangedFn = void (*)(World &world, Pos pos,
                                     const BlockState &state,
                                     const BlockRef &source,
                                     bool movedByPiston);
  using UpdateShapeFn = BlockState (*)(World &world, Pos pos,
                                       const BlockState &state,
                                       Direction direction, Pos neighborPos,
                                       const BlockState &neighbor);
  using ScheduledTickFn = void (*)(World &world, Pos pos,
                                   const BlockState &state);
  using BlockEventFn = bool (*)(World &world, Pos pos, const BlockState &state,
                                int type, int data);
  using RandomTickFn = RandomTickResult (*)(RandomTickAccess &level, Pos pos,
                                            const BlockState &state,
                                            Random &random);

  OnPlaceFn onPlace = nullptr;
  AffectRemovalFn affectNeighborsAfterRemoval = nullptr;
  NeighborChangedFn neighborChanged = nullptr;
  UpdateShapeFn updateShape = nullptr;
  ScheduledTickFn tick = nullptr;
  BlockEventFn triggerEvent = nullptr;
  RandomTickFn randomTick = nullptr;
};

// ---------------------------------------------------------------------------
// Block（对应 `net/minecraft/world/level/block/Block`）
//
// 进程内每种方块类型唯一的不可复制单例，直接对应 `Blocks` 类中的静态字段。
// 构造函数会自动把方块及其笛卡尔积全部状态注册到全局表中，因此方块身份等同于对象地址，
// 热路径上的方块类型判定无需触碰任何字符串。
// ---------------------------------------------------------------------------
class Block {
public:
  Block(const Block &) = delete;
  Block &operator=(const Block &) = delete;
  Block(std::string_view name, BlockMetadata metadata = {},
        StateDefinition definition = {});
  const std::string &name() const { return name_; }
  std::string_view shortName() const;
  bool is(const Block &other) const { return this == &other; }
  const BlockMetadata &metadata() const { return metadata_; }
  const StateDefinition &stateDefinition() const { return definition_; }
  BlockState defaultState() const;
  std::vector<BlockState> states() const;
  BlockState::Id firstState() const { return firstState_; }
  std::uint32_t stateCount() const { return stateCount_; }
  std::uint32_t index() const { return index_; }
  // 方块类别替代标识：对应原版运行时的 `instanceof` 检查，已在注册期一次性求值缓存。
  bool supported() const { return metadata_.supported; }
  bool isAirLike() const { return airLike_; }
  bool isRail() const { return rail_; }
  bool isLiquid() const { return liquid_; }
  bool isCarpet() const { return carpet_; }
  bool isWall() const { return wall_; }
  bool isCoralFan() const { return coralFan_; }
  bool isCoralWallFan() const { return coralWallFan_; }
  bool hasClassFlag(BlockClassFlag flag) const {
    return (metadata_.classFlags & static_cast<std::uint64_t>(flag)) != 0;
  }
  bool isFence() const { return hasClassFlag(BlockClassFlag::Fence); }
  bool isFenceGate() const { return hasClassFlag(BlockClassFlag::FenceGate); }
  bool isStairs() const { return hasClassFlag(BlockClassFlag::Stairs); }
  bool isSlab() const { return hasClassFlag(BlockClassFlag::Slab); }
  bool isDoor() const { return hasClassFlag(BlockClassFlag::Door); }
  bool isTrapdoor() const { return hasClassFlag(BlockClassFlag::Trapdoor); }
  bool isButton() const { return hasClassFlag(BlockClassFlag::Button); }
  bool isPressurePlate() const { return hasClassFlag(BlockClassFlag::PressurePlate); }
  bool isSign() const { return hasClassFlag(BlockClassFlag::Sign); }
  bool isLeaves() const { return hasClassFlag(BlockClassFlag::Leaves); }
  bool isSapling() const { return hasClassFlag(BlockClassFlag::Sapling); }
  bool isFalling() const { return hasClassFlag(BlockClassFlag::Falling); }
  bool isSnowy() const { return hasClassFlag(BlockClassFlag::Snowy); }
  bool isFlower() const { return hasClassFlag(BlockClassFlag::Flower); }
  bool isVegetation() const { return hasClassFlag(BlockClassFlag::Vegetation); }
  bool isDryVegetation() const { return hasClassFlag(BlockClassFlag::DryVegetation); }
  bool isRotatedPillar() const { return hasClassFlag(BlockClassFlag::RotatedPillar); }
  bool isBed() const { return hasClassFlag(BlockClassFlag::Bed); }
  bool isChest() const { return hasClassFlag(BlockClassFlag::Chest); }
  bool isShulkerBox() const { return hasClassFlag(BlockClassFlag::ShulkerBox); }
  bool isPiston() const { return hasClassFlag(BlockClassFlag::Piston); }
  bool isMovingPiston() const { return hasClassFlag(BlockClassFlag::MovingPiston); }
  bool isCopperBulb() const { return hasClassFlag(BlockClassFlag::CopperBulb); }
  bool isCandle() const { return hasClassFlag(BlockClassFlag::Candle); }
  bool isCauldron() const { return hasClassFlag(BlockClassFlag::Cauldron); }
  bool isBanner() const { return hasClassFlag(BlockClassFlag::Banner); }
  bool isSkull() const { return hasClassFlag(BlockClassFlag::Skull); }
  bool isWaterloggedHost() const { return hasClassFlag(BlockClassFlag::WaterloggedHost); }
  bool hasBlockEntity() const { return hasClassFlag(BlockClassFlag::HasBlockEntity); }
  // 对应 `LiquidBlockContainer`：即使方块满足或跨越 `blocksMotion`，仍可独立容纳自身流体
  // （参见 `SimpleWaterloggedBlock.java:18-36`）。
  bool isLiquidContainer() const { return metadata_.liquidContainer; }
  const BlockLightProperties &lightProperties(std::uint32_t variant) const;
  const BlockBehaviours &behaviours() const { return behaviours_; }
  void onPlace(World &world, Pos pos, const BlockState &state,
               const BlockState &oldState, bool movedByPiston) const;
  void affectNeighborsAfterRemoval(World &world, Pos pos,
                                   const BlockState &state,
                                   bool movedByPiston) const;
  void neighborChanged(World &world, Pos pos, const BlockState &state,
                       const BlockRef &source, bool movedByPiston) const;
  BlockState updateShape(World &world, Pos pos, const BlockState &state,
                         Direction direction, Pos neighborPos,
                         const BlockState &neighbor) const;
  void tick(World &world, Pos pos, const BlockState &state) const;
  bool triggerEvent(World &world, Pos pos, const BlockState &state, int type,
                    int data) const;
  RandomTickResult randomTick(RandomTickAccess &level, Pos pos,
                              const BlockState &state, Random &random) const;

private:
  friend void detail::registerBlock(Block &block);
  friend void detail::bindBlockBehaviours(Block &block);
  friend void detail::bindRandomTickBehaviour(Block &block);
  std::string name_;
  BlockMetadata metadata_;
  StateDefinition definition_;
  BlockState::Id firstState_ = 0;
  BlockState::Id defaultStateId_ = 0;
  std::uint32_t stateCount_ = 0;
  std::uint32_t index_ = 0;
  bool airLike_ = false;
  bool rail_ = false;
  bool liquid_ = false;
  bool carpet_ = false;
  bool wall_ = false;
  bool coralFan_ = false;
  bool coralWallFan_ = false;
  BlockBehaviours behaviours_{};
  std::vector<BlockLightProperties> lightProperties_;
};

// `Block` 单例轻量句柄（用于 `ScheduledTick.block` / `BlockEvent.block` / 邻居更新源）。
// 之所以用指针封装而非保存字符串，是因为计划刻与邻居更新极度高频，拷贝与相等比较只需
// 一个指针操作即可完成（与原版直接持有 `Block` 对象引用一致）。
class BlockRef {
public:
  BlockRef() = default;
  BlockRef(const Block &block) : block_(&block) {}
  // 按规范方块名解析（必要时自动注册未知占位块）。单独提供 `const char*` 与
  // `const std::string&` 重载，是为了避免调用方传入名称字面量或 `std::string` 时
  // 触发二次用户自定义隐式转换。
  BlockRef(std::string_view name);
  BlockRef(const char *name) : BlockRef(std::string_view(name ? name : "")) {}
  BlockRef(const std::string &name) : BlockRef(std::string_view(name)) {}
  const Block *block() const { return block_; }
  const Block &operator*() const { return *block_; }
  const std::string &str() const;
  std::string_view view() const { return str(); }
  bool empty() const { return block_ == nullptr; }
  bool equals(const std::string &other) const { return str() == other; }
  bool operator==(const BlockRef &other) const = default;

private:
  const Block *block_ = nullptr;
};

// ---------------------------------------------------------------------------
// 内联状态访问器（依赖完整的 `Block` 类型定义）。
// ---------------------------------------------------------------------------
inline const Block &BlockState::block() const {
  return block_ ? *block_ : airBlock();
}
inline std::uint32_t BlockState::variant() const {
  return id_ - block_->firstState();
}
inline bool BlockState::isSchemaState() const {
  return block_ != nullptr && variant() < block_->stateCount();
}
inline bool BlockState::isAir() const {
  return block_ == nullptr || block_->isAirLike();
}
inline const std::string &BlockState::name() const { return block().name(); }
inline std::string_view BlockState::nameView() const { return name(); }
inline std::string_view BlockState::shortName() const {
  return block().shortName();
}
inline bool BlockState::hasProperty(std::string_view key) const {
  if (!block_)
    return false;
  return isSchemaState() ? block_->stateDefinition().find(key) != nullptr
                         : extensionHas(key);
}
inline std::string_view BlockState::getView(std::string_view key,
                                            std::string_view fallback) const {
  if (!block_)
    return fallback;
  if (isSchemaState())
    return block_->stateDefinition().value(variant(), key, fallback);
  return extensionValue(key, fallback);
}
inline std::string BlockState::get(const std::string &key,
                                   const std::string &fallback) const {
  return std::string(getView(key, fallback));
}
inline std::map<std::string, std::string> BlockState::properties() const {
  if (!block_)
    return {};
  return isSchemaState() ? block_->stateDefinition().values(variant())
                         : extensionProperties();
}
inline std::string_view Block::shortName() const {
  constexpr std::string_view prefix = "minecraft:";
  const std::string_view n = name_;
  return n.starts_with(prefix) ? n.substr(prefix.size()) : n;
}
inline BlockState Block::defaultState() const {
  return BlockState::fromId(defaultStateId_);
}
inline BlockState airState() { return BlockState{}; }

// ---------------------------------------------------------------------------
// 枚举属性快速访问器：每次读取仅对 `BlockState::packed()` 执行一次右移和按位与。
// 字符串版本（`getView`）仅保留用于跨越注册表边界的场景（如存档/投影导入导出、协议编码与诊断输出）。
// 若目标方块未声明该属性（或为非 schema 扩展状态），则返回 `fallback`，保持与原版及旧 `getView` 一致。
// ---------------------------------------------------------------------------
inline std::uint32_t propertyValue(const BlockState &state, PropertyKind kind,
                                   std::uint32_t fallback = 0) {
  if (!state.isSchemaState())
    return fallback;
  const PropertyLayout &layout = state.block().stateDefinition().layout(kind);
  return layout.property ? (state.packed() >> layout.shift) & layout.mask
                         : fallback;
}

inline Facing facingOf(const BlockState &state, Facing fallback = Facing::North) {
  return static_cast<Facing>(propertyValue(
      state, PropertyKind::Facing, static_cast<std::uint32_t>(fallback)));
}
inline PistonType pistonTypeOf(const BlockState &state) {
  return static_cast<PistonType>(propertyValue(state, PropertyKind::Type));
}
inline WallSide wallSideOf(const BlockState &state, PropertyKind side) {
  return static_cast<WallSide>(propertyValue(state, side));
}
inline bool poweredOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Powered) != 0;
}
inline bool litOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Lit) != 0;
}
inline bool openOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Open) != 0;
}
inline bool waterloggedOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Waterlogged) != 0;
}
inline bool extendedOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Extended) != 0;
}
inline bool snowyOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Snowy) != 0;
}
inline bool unstableOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Unstable) != 0;
}
inline bool upOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Up) != 0;
}
inline bool inWallOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::InWall) != 0;
}
inline bool shortOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Short) != 0;
}

// 获取方块状态所属方块的 `BlockBehaviour.Properties` 子集与每状态光照属性（按对象/状态直接查表，无额外缓存）。
inline const BlockMetadata &blockMetadata(const BlockState &state) {
  return state.block().metadata();
}
inline const BlockLightProperties &blockLightProperties(const BlockState &state) {
  return lightPropertiesOf(state);
}

// ---------------------------------------------------------------------------
// 方块注册表单例声明（对应 `net/minecraft/world/level/block/Blocks`）。
// ---------------------------------------------------------------------------
namespace Blocks {
#include "redstone/blocks_decl.inc"
} // namespace Blocks

// 按方块名查找：`findBlockByName` 只查不注册；`blockByName` 对注册表之外的名称会注册一个
// 未受支持的占位 `Block` 单例，以确保存档或 litematic 投影导入能够保留未知方块。
const Block *findBlockByName(std::string_view name);
const Block &blockByName(std::string_view name);

bool stateNamed(const BlockState &state, std::string_view shortName);
bool stateIsAir(const BlockState &state);
BlockState defaultState(const std::string &name);
bool isSupportedBlock(const std::string &name);
std::size_t registeredStateCount();
const std::vector<BlockRegistration> &blockRegistry();

// 对应 `BlockStateBase.isRedstoneConductor`：伸出状态下的活塞底座不作为红石导体。
inline bool isRedstoneConductor(const BlockState &state) {
  const Block *block = state.blockOrNull();
  if (!block || !block->metadata().conductor)
    return false;
  if ((block->is(Blocks::PISTON) || block->is(Blocks::STICKY_PISTON)) &&
      state.getView("extended", "false") == "true")
    return false;
  return true;
}
inline bool isRegisteredSmallFlower(const BlockState &state) {
  return state.is(Blocks::DANDELION) ||
         state.is(Blocks::GOLDEN_DANDELION) ||
         state.is(Blocks::TORCHFLOWER) ||
         state.is(Blocks::POPPY) ||
         state.is(Blocks::BLUE_ORCHID) ||
         state.is(Blocks::ALLIUM) ||
         state.is(Blocks::AZURE_BLUET) ||
         state.is(Blocks::RED_TULIP) ||
         state.is(Blocks::ORANGE_TULIP) ||
         state.is(Blocks::WHITE_TULIP) ||
         state.is(Blocks::PINK_TULIP) ||
         state.is(Blocks::OXEYE_DAISY) ||
         state.is(Blocks::CORNFLOWER) ||
         state.is(Blocks::WITHER_ROSE) ||
         state.is(Blocks::LILY_OF_THE_VALLEY);
}
inline bool isWoolBlock(const BlockState &state) {
  return state.is(Blocks::WHITE_WOOL) ||
         state.is(Blocks::ORANGE_WOOL) ||
         state.is(Blocks::MAGENTA_WOOL) ||
         state.is(Blocks::LIGHT_BLUE_WOOL) ||
         state.is(Blocks::YELLOW_WOOL) ||
         state.is(Blocks::LIME_WOOL) ||
         state.is(Blocks::PINK_WOOL) ||
         state.is(Blocks::GRAY_WOOL) ||
         state.is(Blocks::LIGHT_GRAY_WOOL) ||
         state.is(Blocks::CYAN_WOOL) ||
         state.is(Blocks::PURPLE_WOOL) ||
         state.is(Blocks::BLUE_WOOL) ||
         state.is(Blocks::BROWN_WOOL) ||
         state.is(Blocks::GREEN_WOOL) ||
         state.is(Blocks::RED_WOOL) ||
         state.is(Blocks::BLACK_WOOL);
}
inline bool isWoolCarpet(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isCarpet() && block->supported();
}
inline bool isWallBlock(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isWall();
}
inline bool isFenceGate(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isFenceGate();
}
inline bool isDeadCoralWallFan(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isCoralWallFan() && block->supported();
}
inline bool isDeadCoralFan(const BlockState &state) {
  // 墙上的珊瑚扇也归入失活珊瑚扇判定：与引入注册表之前的谓词语义保持一致。
  const Block *block = state.blockOrNull();
  return block && (block->isCoralFan() || block->isCoralWallFan()) &&
         block->supported();
}
inline bool isRailBlock(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isRail();
}
inline bool isLiquidBlock(const BlockState &state) {
  const Block *block = state.blockOrNull();
  return block && block->isLiquid();
}
} // namespace redstone
