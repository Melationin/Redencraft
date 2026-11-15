#include "redstone/redstone.hpp"
#include "redstone/fluid.hpp"
#include "redstone/shapes.hpp"

namespace redstone {
namespace {

void copperBulbCheckAndFlip(World &world, Pos pos, const BlockState &state) {
  const bool signal = world.hasNeighborSignal(pos);
  if (signal != poweredOf(state)) {
    BlockState next = state;
    if (!poweredOf(state)) {
      next = next.with(PropertyKind::Lit,
                       litOf(state) ? std::uint8_t{0} : std::uint8_t{1});
      // TODO: copper bulb sound event.
    }
    world.setBlock(
        pos,
        next.with(PropertyKind::Powered,
                  signal ? std::uint8_t{1} : std::uint8_t{0}),
        3);
  }
}

} // namespace

void doorNeighborChanged(World &world, Pos pos, const BlockState &state,
                         const BlockRef &source, bool) {
  if (state.is(source.block() ? *source.block() : Blocks::AIR))
    return;
  const bool isLower = state.getView("half", "lower") == "lower";
  const bool signal =
      world.hasNeighborSignal(pos) ||
      world.hasNeighborSignal(
          pos.relative(isLower ? Direction::Up : Direction::Down));
  if (signal != poweredOf(state)) {
    const BlockState next =
        state
            .with(PropertyKind::Powered,
                  signal ? std::uint8_t{1} : std::uint8_t{0})
            .with(PropertyKind::Open,
                  signal ? std::uint8_t{1} : std::uint8_t{0});
    world.setBlock(pos, next, 2);
  }
}

BlockState doorUpdateShape(World &world, Pos, const BlockState &state,
                           Direction direction, Pos neighborPos,
                           const BlockState &neighbor) {
  const bool isLower = state.getView("half", "lower") == "lower";
  const bool matchAxis =
      (direction == Direction::Up && isLower) ||
      (direction == Direction::Down && !isLower);
  if (!matchAxis) {
    if (isLower && direction == Direction::Down &&
        !faceSturdy(world, neighborPos, neighbor, Direction::Up,
                    SupportType::Full))
      return airState();
    return state;
  }
  if (neighbor.sameBlock(state) &&
      neighbor.getView("half") != state.getView("half")) {
    return neighbor.with("half", isLower ? "lower" : "upper");
  }
  return airState();
}

void trapdoorNeighborChanged(World &world, Pos pos, const BlockState &state,
                             const BlockRef &, bool) {
  const bool signal = world.hasNeighborSignal(pos);
  if (signal != poweredOf(state)) {
    BlockState next =
        state.with(PropertyKind::Powered,
                   signal ? std::uint8_t{1} : std::uint8_t{0});
    if (openOf(state) != signal)
      next = next.with(PropertyKind::Open,
                       signal ? std::uint8_t{1} : std::uint8_t{0});
    world.setBlock(pos, next, 2);
    if (waterloggedOf(next))
      world.scheduleFluidTick(pos, FluidRef(Fluids::water()),
                              Fluids::water().tickDelay());
  }
}

void copperBulbOnPlace(World &world, Pos pos, const BlockState &state,
                       const BlockState &oldState, bool) {
  if (!oldState.sameBlock(state))
    copperBulbCheckAndFlip(world, pos, state);
}

void copperBulbNeighborChanged(World &world, Pos pos, const BlockState &state,
                               const BlockRef &, bool) {
  copperBulbCheckAndFlip(world, pos, state);
}

} // namespace redstone
