#include "redstone/internal.hpp"
#include "redstone/entity.hpp"
#include "redstone/shapes.hpp"

#include <algorithm>

namespace redstone {
namespace {

using redstone_detail::clockWise;
using redstone_detail::counterClockWise;
using redstone_detail::directionAxis;
using redstone_detail::dropAndRemoveBlock;
using redstone_detail::kAllDirections;
using redstone_detail::wirePowerOf;

int delayOf(const BlockState &state) {
  return static_cast<int>(propertyValue(state, PropertyKind::Delay, 1));
}

bool comparatorSubtractOf(const BlockState &state) {
  return propertyValue(state, PropertyKind::Mode) == 1;
}

void diodeUpdateNeighborsInFront(World &world, Pos pos,
                                 const BlockState &state) {
  const Direction dir = directionOf(state);
  const Pos frontPos = pos.relative(opposite(dir));
  world.neighborChanged(frontPos, state.block());
  world.updateNeighborsAtExceptFromFacing(frontPos, state.block(), dir);
}

bool diodeShouldPrioritize(const World &world, Pos pos,
                           const BlockState &state) {
  const Direction frontDir = opposite(directionOf(state));
  const BlockState frontState = world.getBlock(pos.relative(frontDir));
  return isDiodeBlock(frontState) && directionOf(frontState) != frontDir;
}

int diodeGetInputSignal(const World &world, Pos pos, const BlockState &state) {
  const Direction dir = directionOf(state);
  const Pos targetPos = pos.relative(dir);
  const int input = world.getSignal(targetPos, dir);
  if (input >= 15)
    return input;
  const BlockState targetState = world.getBlock(targetPos);
  return std::max(input, targetState.is(Blocks::REDSTONE_WIRE)
                             ? wirePowerOf(targetState)
                             : 0);
}

int diodeGetAlternateSignal(const World &world, Pos pos,
                            const BlockState &state, bool diodesOnly) {
  const Direction dir = directionOf(state);
  const Direction cw = clockWise(dir);
  const Direction ccw = counterClockWise(dir);
  return std::max(
      world.getControlInputSignal(pos.relative(cw), cw, diodesOnly),
      world.getControlInputSignal(pos.relative(ccw), ccw, diodesOnly));
}

bool repeaterIsLocked(const World &world, Pos pos, const BlockState &state) {
  return diodeGetAlternateSignal(world, pos, state, true) > 0;
}

int comparatorGetInputSignal(World &world, Pos pos, const BlockState &state) {
  int result = diodeGetInputSignal(world, pos, state);
  const Direction dir = directionOf(state);
  Pos targetPos = pos.relative(dir);
  BlockState targetState = world.getBlock(targetPos);
  if (hasAnalogOutputSignal(targetState)) {
    result = comparatorAnalogOutput(world, targetPos, targetState, opposite(dir));
  } else if (result < 15 && isRedstoneConductor(targetState)) {
    targetPos = targetPos.relative(dir);
    targetState = world.getBlock(targetPos);
    const Aabb frameBox{static_cast<double>(targetPos.x),
                        static_cast<double>(targetPos.y),
                        static_cast<double>(targetPos.z),
                        targetPos.x + 1.0, targetPos.y + 1.0,
                        targetPos.z + 1.0};
    ItemFrame *matchedFrame = nullptr;
    int matchedCount = 0;
    for (Entity *entity : world.entitiesIn(frameBox)) {
      if (auto *frame = dynamic_cast<ItemFrame *>(entity);
          frame && !frame->isRemoved() && frame->attachDirection() == dir) {
        matchedFrame = frame;
        ++matchedCount;
      }
    }
    const int frameSignal =
        (matchedCount == 1 && matchedFrame) ? matchedFrame->getAnalogOutput()
                                            : -1;
    const int blockSignal =
        hasAnalogOutputSignal(targetState)
            ? comparatorAnalogOutput(world, targetPos, targetState,
                                     opposite(dir))
            : -1;
    const int combined = std::max(frameSignal, blockSignal);
    if (combined >= 0)
      result = combined;
  }
  return result;
}

int comparatorCalculateOutput(World &world, Pos pos, const BlockState &state) {
  const int input = comparatorGetInputSignal(world, pos, state);
  if (input == 0)
    return 0;
  const int side = diodeGetAlternateSignal(world, pos, state, false);
  if (side > input)
    return 0;
  return comparatorSubtractOf(state) ? input - side : input;
}

bool comparatorShouldTurnOn(World &world, Pos pos, const BlockState &state) {
  const int input = comparatorGetInputSignal(world, pos, state);
  if (input == 0)
    return false;
  const int side = diodeGetAlternateSignal(world, pos, state, false);
  if (input > side)
    return true;
  return input == side && !comparatorSubtractOf(state);
}

void comparatorRefreshOutputState(World &world, Pos pos,
                                  const BlockState &state) {
  const int outputValue = comparatorCalculateOutput(world, pos, state);
  const int oldValue = world.comparatorOutput(pos);
  world.setComparatorOutput(pos, outputValue);
  if (oldValue != outputValue || !comparatorSubtractOf(state)) {
    const bool sourceOn = comparatorShouldTurnOn(world, pos, state);
    const bool isOn = poweredOf(state);
    if (isOn && !sourceOn) {
      world.setBlock(pos, state.with(PropertyKind::Powered, std::uint8_t{0}), 2);
    } else if (!isOn && sourceOn) {
      world.setBlock(pos, state.with(PropertyKind::Powered, std::uint8_t{1}), 2);
    }
    diodeUpdateNeighborsInFront(world, pos, state);
  }
}

bool diodeHandleDestroyIfCantSurvive(World &world, Pos pos,
                                     const BlockState &state) {
  if (diodeCanSurvive(world, pos))
    return false;
  const auto &selfBlock = state.block();
  dropAndRemoveBlock(world, pos, state);
  for (Direction d : kAllDirections)
    world.updateNeighborsAt(pos.relative(d), selfBlock);
  return true;
}

} // namespace

bool diodeCanSurvive(World &world, Pos pos) {
  const Pos below = pos.relative(Direction::Down);
  return faceSturdy(world, below, world.getBlock(below), Direction::Up,
                    SupportType::Rigid);
}

BlockState repeaterPlacementState(const World &world, Pos pos,
                                  Direction playerHorizontal) {
  const BlockState base = Blocks::REPEATER.defaultState().with(
      PropertyKind::Facing,
      static_cast<std::uint8_t>(opposite(playerHorizontal)));
  return base.with(PropertyKind::Locked,
                   repeaterIsLocked(world, pos, base) ? std::uint8_t{1}
                                                      : std::uint8_t{0});
}

BlockState comparatorPlacementState(Direction playerHorizontal) {
  return Blocks::COMPARATOR.defaultState().with(
      PropertyKind::Facing,
      static_cast<std::uint8_t>(opposite(playerHorizontal)));
}

void diodeSetPlacedBy(World &world, Pos pos, const BlockState &state) {
  if (state.is(Blocks::REPEATER)) {
    if (diodeGetInputSignal(world, pos, state) > 0)
      world.scheduleTick(pos, state.block(), 1);
  } else if (state.is(Blocks::COMPARATOR)) {
    if (comparatorShouldTurnOn(world, pos, state))
      world.scheduleTick(pos, state.block(), 1);
  }
}

bool repeaterUse(World &world, Pos pos, const BlockState &state) {
  if (!state.is(Blocks::REPEATER))
    return false;
  const int nextDelay = (delayOf(state) % 4) + 1;
  world.setBlock(
      pos, state.with(PropertyKind::Delay, static_cast<std::uint8_t>(nextDelay)),
      3);
  return true;
}

bool comparatorUse(World &world, Pos pos, const BlockState &state) {
  if (!state.is(Blocks::COMPARATOR))
    return false;
  const std::uint8_t nextMode = comparatorSubtractOf(state) ? 0 : 1;
  const BlockState next = state.with(PropertyKind::Mode, nextMode);
  // TODO: comparator click sound.
  world.setBlock(pos, next, 2);
  comparatorRefreshOutputState(world, pos, next);
  return true;
}

void repeaterOnPlace(World &world, Pos pos, const BlockState &state,
                     const BlockState &, bool) {
  diodeUpdateNeighborsInFront(world, pos, state);
}

void repeaterAffectRemoval(World &world, Pos pos, const BlockState &state,
                           bool movedByPiston) {
  if (!movedByPiston)
    diodeUpdateNeighborsInFront(world, pos, state);
}

void repeaterNeighborChanged(World &world, Pos pos, const BlockState &state,
                             const BlockRef &, bool) {
  if (diodeHandleDestroyIfCantSurvive(world, pos, state))
    return;
  if (!repeaterIsLocked(world, pos, state)) {
    const bool on = poweredOf(state);
    const bool shouldTurnOn = diodeGetInputSignal(world, pos, state) > 0;
    if (on != shouldTurnOn && !world.willTickThisTick(pos, state.block())) {
      int priority = kTickPriorityHigh;
      if (diodeShouldPrioritize(world, pos, state))
        priority = kTickPriorityExtremelyHigh;
      else if (on)
        priority = kTickPriorityVeryHigh;
      world.scheduleTick(pos, state.block(), delayOf(state) * 2, priority);
    }
  }
}

BlockState repeaterUpdateShape(World &world, Pos pos, const BlockState &state,
                               Direction direction, Pos neighborPos,
                               const BlockState &neighbor) {
  (void)neighborPos;
  (void)neighbor;
  if (direction == Direction::Down && !diodeCanSurvive(world, pos))
    return airState();
  if (directionAxis(direction) != directionAxis(directionOf(state)))
    return state.with(PropertyKind::Locked,
                      repeaterIsLocked(world, pos, state) ? std::uint8_t{1}
                                                          : std::uint8_t{0});
  return state;
}

void repeaterTick(World &world, Pos pos, const BlockState &state) {
  if (!repeaterIsLocked(world, pos, state)) {
    const bool on = poweredOf(state);
    const bool shouldTurnOn = diodeGetInputSignal(world, pos, state) > 0;
    if (on && !shouldTurnOn) {
      world.setBlock(pos, state.with(PropertyKind::Powered, std::uint8_t{0}), 2);
    } else if (!on) {
      world.setBlock(pos, state.with(PropertyKind::Powered, std::uint8_t{1}), 2);
      if (!shouldTurnOn)
        world.scheduleTick(pos, state.block(), delayOf(state) * 2,
                           kTickPriorityVeryHigh);
    }
  }
}

void comparatorOnPlace(World &world, Pos pos, const BlockState &state,
                       const BlockState &, bool) {
  diodeUpdateNeighborsInFront(world, pos, state);
}

void comparatorAffectRemoval(World &world, Pos pos, const BlockState &state,
                             bool movedByPiston) {
  if (!movedByPiston)
    diodeUpdateNeighborsInFront(world, pos, state);
}

void comparatorNeighborChanged(World &world, Pos pos, const BlockState &state,
                               const BlockRef &, bool) {
  if (diodeHandleDestroyIfCantSurvive(world, pos, state))
    return;
  if (!world.willTickThisTick(pos, state.block())) {
    const int outputValue = comparatorCalculateOutput(world, pos, state);
    const int oldValue = world.comparatorOutput(pos);
    if (outputValue != oldValue ||
        poweredOf(state) != comparatorShouldTurnOn(world, pos, state)) {
      const int priority = diodeShouldPrioritize(world, pos, state)
                               ? kTickPriorityHigh
                               : kTickPriorityNormal;
      world.scheduleTick(pos, state.block(), 2, priority);
    }
  }
}

void comparatorTick(World &world, Pos pos, const BlockState &state) {
  comparatorRefreshOutputState(world, pos, state);
}

} // namespace redstone
