#pragma once
#include "redstone/engine.hpp"
#include <memory>
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
  // Cleared when the entity leaves World.pistonEntities, so the ticker slot
  // sweep does not need a map lookup per entity per tick.
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
  const std::vector<Pos> &toPush() const { return push_; }
  const std::vector<Pos> &toDestroy() const { return destroy_; }

private:
  bool addBlockLine(Pos, Direction);
  bool addBranchingBlocks(Pos);
  World &world_;
  Pos piston_, start_;
  Direction facing_, movement_;
  bool extending_;
  std::vector<Pos> push_, destroy_;
};
bool pistonIsPushable(World &, Pos, const BlockState &, Direction, bool,
                      Direction);
void pistonNeighborChanged(World &, Pos, const BlockState &);
void pistonOnPlace(World &, Pos, const BlockState &, const BlockState &);
bool pistonBlockEvent(World &, Pos, const BlockState &, int, int);
void pistonTickBlockEntities(World &);
void pistonPreRemoveSideEffects(World &, Pos);
void pistonForgetWorld(World &);
} // namespace redstone
