#include "redstone/internal.hpp"
#include "redstone/shapes.hpp"

namespace redstone {
namespace {

using redstone_detail::isHorizontal;
using redstone_detail::kAllDirections;

void torchNotifyNeighbors(World &world, Pos pos, const BlockState &state) {
  const auto &source = state.block();
  for (Direction d : kAllDirections)
    world.updateNeighborsAt(pos.relative(d), source);
}

bool torchHasNeighborSignal(const World &world, Pos pos,
                            const BlockState &state) {
  if (state.is(Blocks::REDSTONE_WALL_TORCH)) {
    const Direction opp = opposite(directionOf(state));
    return world.hasSignal(pos.relative(opp), opp);
  }
  return world.hasSignal(pos.relative(Direction::Down), Direction::Down);
}

bool torchIsToggledTooFrequently(World &world, Pos pos, bool add) {
  auto &toggles = world.recentTorchToggles();
  if (add)
    toggles.push_back({pos, world.gameTime()});
  int count = 0;
  for (const auto &t : toggles) {
    if (t.pos == pos) {
      if (++count >= 8)
        return true;
    }
  }
  return false;
}

} // namespace

bool redstoneTorchCanSurvive(World &world, Pos pos) {
  const Pos below = pos.relative(Direction::Down);
  return faceSturdy(world, below, world.getBlock(below), Direction::Up,
                    SupportType::Center);
}

bool redstoneWallTorchCanSurvive(World &world, Pos pos, Direction facing) {
  if (!isHorizontal(facing))
    return false;
  const Pos support = pos.relative(opposite(facing));
  return faceSturdy(world, support, world.getBlock(support), facing,
                    SupportType::Full);
}

void redstoneTorchOnPlace(World &world, Pos pos, const BlockState &state,
                          const BlockState &, bool) {
  torchNotifyNeighbors(world, pos, state);
}

void redstoneTorchAffectRemoval(World &world, Pos pos, const BlockState &state,
                                bool movedByPiston) {
  if (!movedByPiston)
    torchNotifyNeighbors(world, pos, state);
}

void redstoneTorchNeighborChanged(World &world, Pos pos,
                                  const BlockState &state, const BlockRef &,
                                  bool) {
  if (litOf(state) == torchHasNeighborSignal(world, pos, state) &&
      !world.willTickThisTick(pos, state.block()))
    world.scheduleTick(pos, state.block(), 2);
}

BlockState redstoneTorchUpdateShape(World &world, Pos pos,
                                    const BlockState &state,
                                    Direction direction, Pos,
                                    const BlockState &) {
  if (direction == Direction::Down && !redstoneTorchCanSurvive(world, pos))
    return airState();
  return state;
}

BlockState redstoneWallTorchUpdateShape(World &world, Pos pos,
                                        const BlockState &state,
                                        Direction direction, Pos,
                                        const BlockState &) {
  if (opposite(direction) == directionOf(state) &&
      !redstoneWallTorchCanSurvive(world, pos, directionOf(state)))
    return airState();
  return state;
}

void redstoneTorchTick(World &world, Pos pos, const BlockState &state) {
  const bool neighborSignal = torchHasNeighborSignal(world, pos, state);
  auto &toggles = world.recentTorchToggles();
  while (!toggles.empty() && world.gameTime() - toggles.front().when > 60)
    toggles.pop_front();

  if (litOf(state)) {
    if (neighborSignal) {
      world.setBlock(pos, state.with(PropertyKind::Lit, std::uint8_t{0}), 3);
      if (torchIsToggledTooFrequently(world, pos, true)) {
        // TODO: levelEvent(1502) burnout sound & smoke particles.
        world.scheduleTick(pos, world.getBlock(pos).block(), 160);
      }
    }
  } else if (!neighborSignal && !torchIsToggledTooFrequently(world, pos, false)) {
    world.setBlock(pos, state.with(PropertyKind::Lit, std::uint8_t{1}), 3);
  }
}

} // namespace redstone
