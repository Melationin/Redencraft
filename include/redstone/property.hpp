#pragma once
// 方块状态属性规范标识与常用枚举：为 `BlockState::packed()` 位域编码提供统一的
// 属性身份枚举（`PropertyKind`）与强类型属性取值枚举，消除模拟热路径上的字符串查找。
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace redstone {

// 方块状态属性的规范种类标识。原版通过 `BlockStateProperties.FACING` 等静态对象引用
// 识别属性身份；本枚举即为模拟器所建模属性在 C++ 中的对应身份。
//
// 每种 `PropertyKind` 拥有统一的规范值域：虽然不同方块声明的同名属性取值子集可能不同，
// 但在注册期每个方块都会把自身的局部取值表映射到这套规范值上，从而使任意方块状态的属性值
// 都能直接压缩进 `BlockState::packed()` 的位域中，在热路径上仅凭一次移位与掩码完成读写，
// 而无需按属性名做哈希或二分查找。
enum class PropertyKind : std::uint8_t {
  Facing,
  Type,
  Shape,
  Instrument,
  Note,
  Level,
  Extended,
  InWall,
  Lit,
  Open,
  Powered,
  Short,
  Snowy,
  Unstable,
  Up,
  Waterlogged,
  WallEast,
  WallNorth,
  WallSouth,
  WallWest,
  // 对应 `RotatedPillarBlock.AXIS`（`x` / `y` / `z`），最初为玄武岩（`basalt`）等柱状方块引入。
  Axis,
  Stage,
  Distance,
  Persistent,
  Triggered,
  BedPart,
  Occupied,
  Half,
  Age,
  Attached,
  Attachment,
  Berries,
  Bites,
  Bloom,
  Bottom,
  CanSummon,
  Candles,
  Charges,
  Conditional,
  CopperGolemPose,
  Cracked,
  Crafting,
  CreakingHeartState,
  Delay,
  Disarmed,
  Down,
  Drag,
  Dusted,
  Eggs,
  Enabled,
  Eye,
  Face,
  FlowerAmount,
  Hanging,
  HasBook,
  HasBottle0,
  HasBottle1,
  HasBottle2,
  HasRecord,
  Hatch,
  Hinge,
  HoneyLevel,
  Hydration,
  Inverted,
  Layers,
  Leaves,
  Locked,
  Mode,
  Moisture,
  Natural,
  Ominous,
  Orientation,
  Pickles,
  Power,
  Rotation,
  SculkSensorPhase,
  SegmentAmount,
  Shrieking,
  SideChain,
  SignalFire,
  Slot0Occupied,
  Slot1Occupied,
  Slot2Occupied,
  Slot3Occupied,
  Slot4Occupied,
  Slot5Occupied,
  Thickness,
  Tilt,
  Tip,
  TrialSpawnerState,
  VaultState,
  VerticalDirection,
  Count
};
inline constexpr std::size_t kPropertyKindCount =
    static_cast<std::size_t>(PropertyKind::Count);
// 规范属性值的上界：保证任一属性的最大规范值都能装入 5 位无符号数，且其局部映射数组定长可控。
inline constexpr std::uint8_t kMaxPropertyValue = 31;

// 高频方块的强类型属性枚举：与 `state.cpp` 中的规范值序号一一对应，供模拟逻辑直接使用。
enum class Facing : std::uint8_t { Down, Up, North, South, West, East };
enum class PistonType : std::uint8_t { Normal, Sticky };
enum class RailShape : std::uint8_t {
  NorthSouth,
  EastWest,
  AscendingEast,
  AscendingWest,
  AscendingNorth,
  AscendingSouth,
  SouthEast,
  SouthWest,
  NorthWest,
  NorthEast
};
enum class Instrument : std::uint8_t {
  Harp,
  Basedrum,
  Snare,
  Hat,
  Bass,
  Flute,
  Bell,
  Guitar,
  Chime,
  Xylophone,
  IronXylophone,
  CowBell,
  Didgeridoo,
  Bit,
  Banjo,
  Pling,
  Trumpet,
  TrumpetExposed,
  TrumpetOxidized,
  TrumpetWeathered,
  Zombie,
  Skeleton,
  Creeper,
  Dragon,
  WitherSkeleton,
  Piglin,
  CustomHead
};
enum class WallSide : std::uint8_t { None, Low, Tall };

// 面向注册表与导入边界的名称解析函数：仅在方块静态注册期或外部状态解析时执行，
// 绝不进入逐刻模拟的热路径。
bool propertyKindFromName(std::string_view name, PropertyKind &out);
bool propertyValueFromName(PropertyKind kind, std::string_view value,
                           std::uint8_t &out);

} // namespace redstone
