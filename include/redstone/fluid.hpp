#pragma once
// 流体注册单例（`Fluid`）、轻量流体值状态（`FluidState`）、流体标签与队列引用句柄（`FluidRef`）。
// 对齐原版 `net.minecraft.world.level.material.{Fluid, FluidState, Fluids, FlowingFluid}`，
// 将静态流体模型与动态世界流动模拟（`fluid_flow.hpp`）解耦。
#include <cstdint>
#include <string>
#include <string_view>

#include "redstone/block.hpp"

// 对应 `net/minecraft/world/level/material/{Fluid,FluidState,Fluids,FlowingFluid}.java`。
//
// 每种流体在进程内以全局单例存在（类似 `Fluids` 的静态字段），而 `FluidState` 是方块状态解码出的
// `(fluid, amount, falling)` 纯值三元组。两者均为无副作用的值/只读抽象：查询永远不会修改方块或调度计划刻。
namespace redstone {

class Fluid;

// ---------------------------------------------------------------------------
// FluidState（对应 `net/minecraft/world/level/material/FluidState`）
// 解耦方块注册表编码与流体数值计算，供 `World`、流体计划刻与实体浮力共享。
// ---------------------------------------------------------------------------
class FluidState {
public:
  FluidState() = default; // 默认构造为 `minecraft:empty`，液位为 0
  FluidState(const Fluid &type, std::uint8_t amount, bool falling)
      : type_(&type), amount_(amount), falling_(falling) {}
  const Fluid &getType() const;
  // `FluidState.is(Fluid)`：精确身份匹配，而非同族判断。
  // `ServerLevel.tickFluid` 按具体变体派发计划刻，因此流动变体绝不能匹配其源头变体。
  // 在 `Fluid` 完整定义之后内联实现以便调用 `Fluid::is`。
  bool is(const Fluid &type) const;
  // `FluidState.isSource`：判断是否为源头变体，与 `amount` 无关。
  bool isSource() const;
  // `FluidState.isSourceOfType`：精确匹配指定流体身份且该身份为源头变体。
  bool isSourceOfType(const Fluid &type) const;
  bool isEmpty() const;
  bool isRandomlyTicking() const;
  RandomTickResult randomTick(RandomTickAccess &level, const Pos &pos,
                              Random &random) const;
  // `FluidState.getAmount` 派发给所属 `Fluid`：源头固定返回 8，流动变体返回存储的 `LEVEL` 属性。
  std::uint8_t getAmount() const;
  // 原始存储的 `LEVEL` 属性值（未经 `Fluid::getAmount()` 规则转换）。
  // 流动变体的 `getAmount()` 必须读取此字段，若在其中直接调用 `getAmount()` 会导致无限递归。
  std::uint8_t rawAmount() const { return amount_; }
  // `FluidState.isFull`：仅判断 `amount == 8`，与 `isSource()` 无关（例如下落满格流动水也为 true）。
  bool isFull() const { return getAmount() == 8; }
  bool isFalling() const { return falling_; }
  // `FlowingFluid.getOwnHeight`：返回自身高度 `amount / 9`。
  float getOwnHeight() const;
  // `FlowingFluid.getLegacyLevel`：返回写回方块状态时使用的 `level` 数值。
  int getLegacyLevel() const;
  bool operator==(const FluidState &other) const;
  bool operator!=(const FluidState &other) const { return !(*this == other); }

private:
  const Fluid *type_ = nullptr;
  std::uint8_t amount_ = 0;
  bool falling_ = false;
};

// ---------------------------------------------------------------------------
// Fluid（对应 `net/minecraft/world/level/material/Fluid`）
//
// 每种流体变体对应进程内唯一单例（如 `Fluids` 静态字段），允许用指针地址直接做 O(1) 身份比较。
// ---------------------------------------------------------------------------
class Fluid {
public:
  virtual ~Fluid() = default;
  const std::string &name() const { return name_; }
  std::string_view shortName() const;
  bool is(const Fluid &other) const { return this == &other; }
  // `Fluid.isEmpty`：仅 `minecraft:empty` 返回 true。
  bool isEmpty() const { return empty_; }
  // `Fluid.isSource(FluidState)`：单一有状态的源头判定入口。
  // 源头变体恒返回 true、流动变体恒返回 false，但签名保留 `FluidState` 参数，
  // 以便未来引入依赖状态的流体时无需修改所有调用点（`FlowingFluid.isSourceBlockOfThisType` 与计划刻派发均经此入口）。
  virtual bool isSource(const FluidState &) const { return source_; }
  // `Fluid.isSame`：同族判断（例如 `water` 与 `flowing_water` 互为同族），用于流动高度与混合逻辑。
  virtual bool isSame(const Fluid &other) const { return this == &other; }
  // `Fluid.getAmount`：源头返回 8，其余返回流动液位。
  virtual std::uint8_t getAmount(const FluidState &state) const = 0;
  virtual float getOwnHeight(const FluidState &state) const {
    return static_cast<float>(state.getAmount()) / 9.0f;
  }
  // `Fluid.createLegacyBlock`：将流体状态转换回可写入世界的 `BlockState`。
  virtual BlockState createLegacyBlock(const FluidState &state) const = 0;
  // 对应 `FlowingFluid.getLegacyLevel`。
  virtual int getLegacyLevel(const FluidState &state) const;
  // 对应 `FlowingFluid.getDropOff` / `getSlopeFindDistance` / `getTickDelay` 与 `canConvertToSource`。
  // 此处返回基础维度常量；依赖世界环境的变体（如下界快流熔岩、无限水/熔岩源 gamerule）由 `World` 层覆盖。
  virtual int dropOff() const { return 0; }
  virtual int slopeFindDistance() const { return 0; }
  virtual int tickDelay() const { return 0; }
  virtual bool canConvertToSource() const { return false; }
  virtual bool isRandomlyTicking() const { return false; }
  virtual RandomTickResult randomTick(RandomTickAccess &, const Pos &,
                                      const FluidState &, Random &) const {
    return RandomTickResult::Applied;
  }
  // 注册的桶物品名称；空流体为空字符串。
  const std::string &bucketItem() const { return bucketItem_; }

protected:
  Fluid(std::string_view name, bool empty, bool source, std::string bucketItem);
  // 区分当前实例属于源头还是流动变体。`getAmount()` 内部依赖此标记，
  // 且绝不能转调 `isSource(state)`，因为 `isSource` 在语义上可能依赖待求的量。
  bool isSourceVariant() const { return source_; }

private:
  std::string name_;
  std::string bucketItem_;
  bool empty_ = false;
  bool source_ = false;
};

// `WaterFluid` / `LavaFluid` 在 `isSame()`（同族关系）、衰减步长、寻坡距离、刻延迟与源头转化规则上存在差异。
// 本文件仅建模其静态属性与状态解码规则。
class WaterFluid : public Fluid {
public:
  bool isSame(const Fluid &other) const override;
  std::uint8_t getAmount(const FluidState &state) const override;
  BlockState createLegacyBlock(const FluidState &state) const override;
  // `WaterFluid` 的 `getSlopeFindDistance` / `getDropOff` / `getTickDelay` 常量分别为 4 / 1 / 5；
  // `canConvertToSource` 默认开启并受水资源转化 gamerule 控制。
  int dropOff() const override { return 1; }
  int slopeFindDistance() const override { return 4; }
  int tickDelay() const override { return 5; }
  bool canConvertToSource() const override { return true; }

protected:
  WaterFluid(std::string_view name, bool source);
};

class LavaFluid : public Fluid {
public:
  bool isSame(const Fluid &other) const override;
  std::uint8_t getAmount(const FluidState &state) const override;
  BlockState createLegacyBlock(const FluidState &state) const override;
  // `LavaFluid` 在主世界的常量为 2 / 2 / 30 且默认关闭源头转化；
  // 下界快流变体（1 / 4 / 10）在流控路径中按维度标志调整。
  int dropOff() const override { return 2; }
  int slopeFindDistance() const override { return 2; }
  int tickDelay() const override { return 30; }
  bool canConvertToSource() const override { return false; }
  bool isRandomlyTicking() const override { return true; }
  RandomTickResult randomTick(RandomTickAccess &level, const Pos &pos,
                              const FluidState &state,
                              Random &random) const override;

protected:
  LavaFluid(std::string_view name, bool source);
};

// 对应 `Fluids.java` 的静态流体字段。
// 采用函数内局部静态变量而非全局静态对象，确保注册顺序与翻译单元初始化顺序无关。
namespace Fluids {
const Fluid &empty();
const WaterFluid &flowingWater();
const WaterFluid &water();
const LavaFluid &flowingLava();
const LavaFluid &lava();
} // namespace Fluids

// 对应 `FluidTags.java:8-9`。流体标签与 `Fluid.isSame` 相互独立：`isSame` 回答“是否同族流体”，
// 而标签回答“是否属于该行为组”。`WaterFluid.canBeReplacedWith` 检查 `!other.is(FluidTags.WATER)`，
// `LavaFluid.canBeReplacedWith` 检查 `other.is(FluidTags.WATER)`，两者语义不可混用。
enum class FluidTag : std::uint8_t { Water, Lava };
bool fluidHasTag(const Fluid &fluid, FluidTag tag);

const Fluid *findFluidByName(std::string_view name);

// 对应 `BlockBehaviour.getFluidState` 的注册表子集：液体方块解码其 `level` 属性，非液体方块返回 `minecraft:empty`。
// TODO: 含水宿主方块（waterlogged hosts）返回其宿主流体状态；该部分属于任务 F-5。
FluidState fluidStateOf(const BlockState &state);

// 需要完整 `Fluid` 类型定义的内联访问器。
inline bool FluidState::is(const Fluid &type) const {
  return getType().is(type);
}

// 流体对象句柄（用于 `ScheduledTick<Fluid>` 目标与存档往返）。
// 与 `BlockRef` 一样以单指针实现零开销拷贝与比较：原版即直接在计划刻队列中保存 `Fluid` 单例引用。
class FluidRef {
public:
  FluidRef() = default;
  FluidRef(const Fluid &fluid) : fluid_(&fluid) {}
  FluidRef(std::string_view name); // 仅查找已注册流体，绝不新建注册
  const Fluid *fluid() const { return fluid_; }
  const Fluid &operator*() const { return *fluid_; }
  const std::string &str() const;
  std::string_view view() const { return str(); }
  bool empty() const { return fluid_ == nullptr; }
  bool operator==(const FluidRef &other) const = default;

private:
  const Fluid *fluid_ = nullptr;
};

} // namespace redstone
