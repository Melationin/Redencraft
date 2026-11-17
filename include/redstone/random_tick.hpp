#pragma once
// 随机刻世界访问抽象接口：将随机刻方块逻辑（草方块蔓延、火蔓延、树苗生长、蘑菇存活等）
// 与 `World` 存储解耦，并在光照、流体或标签等系统未就绪时通过 `std::nullopt` 显式上报缺失。

#include "redstone/engine.hpp"

namespace redstone {
// 面向未来完整 `ServerLevel` 集成的随机刻查询与写入抽象接口。
// 之所以将其抽象为虚基类而非直接耦合 `World`，一方面是为了隔离尚未完全就绪的子系统，
// 另一方面便于单元测试按场景注入轻量实现。
// 返回 `std::nullopt` 表示所需的注册表、光照、流体或标签信息当前不可用；
// 按照项目约束，决不允许用猜测值代替不可用的光照、流体或标签信息。
class RandomTickAccess {
public:
  virtual ~RandomTickAccess() = default;
  virtual BlockState getBlock(Pos pos) const = 0;
  virtual std::optional<bool> isFullFluid(Pos pos) const = 0;
  virtual std::optional<bool> isWaterFluid(Pos pos) const = 0;
  // 对应 `LightEngine.getLightBlockInto(from, to, UP, to.getLightDampening())`，
  // 计算从 `from` 向上传播到 `above` 位置时的光照衰减量。
  virtual std::optional<int> lightBlockIntoUp(BlockState from, Pos above) const = 0;
  virtual std::optional<int> maxLocalRawBrightness(Pos pos) const = 0;
  virtual std::optional<bool> isSnowySetting(Pos above) const = 0;
  // 草方块/菌丝向目标位置蔓延的判定结果：区分未支持、不可蔓延、普通蔓延与带雪蔓延。
  enum class SpreadTargetResult {
    Unsupported,
    CannotSpread,
    SpreadClear,
    SpreadSnowy,
  };
  virtual std::optional<bool> canSpreadGrassSurvive(BlockState state,
                                                    Pos above) const;
  virtual SpreadTargetResult canSpreadGrassTarget(BlockState spreadDefault,
                                                  Pos targetAbove) const;
  virtual void setBlockAndUpdate(Pos pos, BlockState state) = 0;
  virtual void setBlock(Pos pos, BlockState state, int flags) = 0;
  // 当 `TreeGrower` 或世界生成尚未可用时返回 `std::nullopt`。
  virtual std::optional<bool> growSaplingTree(Pos pos, BlockState state, Random &random) = 0;
  // `MushroomBlock.canSurvive` 依赖当前原始亮度、实心不透明渲染判定以及世界级的
  // `overrides_mushroom_light_requirement` 标签；缺失时返回 `std::nullopt`。
  virtual std::optional<bool> canMushroomSurvive(Pos, BlockState) const {
    return std::nullopt;
  }
  virtual int minY() const { return -64; }
  // 含边界的最高方块 Y 坐标（对应 `LevelHeightAccessor.getMaxY`）。
  virtual int maxBlockY() const { return 319; }
  virtual bool isInsideBuildHeight(int y) const {
    return y >= minY() && y <= maxBlockY();
  }
  virtual bool isLoaded(Pos) const { return false; }
  virtual bool hasChunkAt(Pos) const { return false; }
  virtual std::optional<int> blockBrightness(Pos) const {
    return std::nullopt;
  }
  virtual std::optional<bool> hasBlockTag(BlockState, std::string_view) const {
    return std::nullopt;
  }
  virtual bool isFaceSturdy(Pos, BlockState, Direction) const { return false; }
  virtual bool isFaceFullBlockSupportOrCollision(Pos, BlockState,
                                                 Direction) const {
    return false;
  }
  virtual bool hasCollisionIntersecting(Pos, double, double, double, double,
                                        double, double) const {
    return false;
  }
  // 判断方块是否具备完整实心遮光渲染（受支持、可遮光且光衰减为 15）。
  virtual bool isSolidRender(BlockState state) const {
    const BlockLightProperties &lp = lightPropertiesOf(state);
    return lp.supported && lp.canOcclude && lp.dampening == 15;
  }
  virtual void removeBlock(Pos pos, bool) {
    setBlock(pos, Blocks::AIR.defaultState(), 3);
  }
  virtual void neighborChanged(Pos, const BlockRef &) {}
  virtual void scheduleTick(Pos, const BlockRef &, int) {}
  virtual bool hasScheduledTick(Pos, const BlockRef &) const { return false; }
  virtual bool canSpreadFireAround(Pos) const { return false; }
  virtual bool spreadVinesEnabled() const { return true; }
  virtual bool waterEvaporates() const { return false; }
  virtual std::optional<bool> isRainingAt(Pos) const { return std::nullopt; }
};

// 根据周围可燃方块朝向计算在 `pos` 放置普通火（`minecraft:fire`）时的初始附着状态。
BlockState baseFireStateForPlacement(const RandomTickAccess &level, Pos pos);
} // namespace redstone
