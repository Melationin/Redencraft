#pragma once

#include "redstone/engine.hpp"

namespace redstone {
// Queries supplied by the eventual ServerLevel integration. nullopt means that
// the needed registry, lighting, fluid or tag information is unavailable.
// Never replace unavailable lighting, fluid or tag information with a guess.
class RandomTickAccess {
public:
  virtual ~RandomTickAccess() = default;
  virtual BlockState getBlock(Pos pos) const = 0;
  virtual std::optional<bool> isFullFluid(Pos pos) const = 0;
  virtual std::optional<bool> isWaterFluid(Pos pos) const = 0;
  // LightEngine.getLightBlockInto(from, to, UP, to.getLightDampening()).
  virtual std::optional<int> lightBlockIntoUp(BlockState from, Pos above) const = 0;
  virtual std::optional<int> maxLocalRawBrightness(Pos pos) const = 0;
  virtual std::optional<bool> isSnowySetting(Pos above) const = 0;
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
  // nullopt when TreeGrower / world generation is unavailable.
  virtual std::optional<bool> growSaplingTree(Pos pos, BlockState state, Random &random) = 0;
  // MushroomBlock.canSurvive requires current raw light, solid rendering and
  // the world's overrides_mushroom_light_requirement tag.
  virtual std::optional<bool> canMushroomSurvive(Pos, BlockState) const {
    return std::nullopt;
  }
  virtual int minY() const { return -64; }
  // Inclusive maximum block Y coordinate (LevelHeightAccessor.getMaxY).
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

BlockState baseFireStateForPlacement(const RandomTickAccess &level, Pos pos);
} // namespace redstone
