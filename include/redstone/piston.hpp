#pragma once
#include "redstone/engine.hpp"
#include <memory>
#include <span>
#include <vector>

namespace redstone {
struct PistonMovingBlockEntity {
  Pos pos;
  BlockState movedState;
  Direction direction;
  bool extending;
  bool source;
  float progress = 0, progressO = 0;
  long long lastTicked = -1;
  // BlockEntity.removed: cleared when this entity is replaced at its position
  // or removed from World.pistonEntities. The tick pass reads it instead of
  // looking the position up again.
  bool live = true;
  // PistonMovingBlockEntity.getMovementDirection / getExtendedProgress.
  Direction movementDirection() const {
    return extending ? direction : opposite(direction);
  }
  float extendedProgress() const {
    return extending ? progress - 1.0f : 1.0f - progress;
  }
};
class PistonStructureResolver {
public:
  PistonStructureResolver(World &, Pos, Direction, bool);
  bool resolve();
  std::span<const Pos> toPush() const { return {push_.data(), pushCount_}; }
  const std::vector<Pos> &toDestroy() const { return destroy_; }

private:
  bool addBlockLine(Pos, Direction);
  bool addBranchingBlocks(Pos);
  World &world_;
  Pos piston_, start_;
  Direction facing_, movement_;
  bool extending_;
  // PistonStructureResolver.PUSH_LIMIT: checked before every insertion.
  std::array<Pos, 12> push_;
  std::size_t pushCount_ = 0;
  std::vector<Pos> destroy_;
};
bool pistonIsPushable(World &, Pos, const BlockState &, Direction, bool,
                      Direction);
void pistonNeighborChanged(World &, Pos, const BlockState &);
void pistonOnPlace(World &, Pos, const BlockState &, const BlockState &);
bool pistonBlockEvent(World &, Pos, const BlockState &, int, int);
// Shared piston entity movement path. It updates entity-section state and the
// managed persistence observer before control returns to World::tick.
void moveEntityByPiston(World &world, Direction pistonDirection, Entity &entity,
                        double delta, Direction movement);
void pistonTickBlockEntities(World &);
void pistonPreRemoveSideEffects(World &, Pos);
void pistonForgetWorld(World &);
} // namespace redstone
