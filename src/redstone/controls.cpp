#include "redstone/internal.hpp"
#include "redstone/entity.hpp"
#include "redstone/shapes.hpp"

#include <algorithm>

namespace redstone {
namespace {

using redstone_detail::faceAttachedConnectedDirection;
using redstone_detail::pressurePlateSignalOf;

void faceAttachedUpdateNeighbours(World &world, Pos pos,
                                  const BlockState &state) {
  const Direction opp = opposite(faceAttachedConnectedDirection(state));
  const auto &source = state.block();
  world.updateNeighborsAt(pos, source);
  world.updateNeighborsAt(pos.relative(opp), source);
}

int buttonPressedTime(const BlockState &state) {
  if (state.is(Blocks::STONE_BUTTON) ||
      state.is(Blocks::POLISHED_BLACKSTONE_BUTTON))
    return 20;
  return 30;
}

int pressurePlatePressedTime(const BlockState &state) {
  if (state.is(Blocks::LIGHT_WEIGHTED_PRESSURE_PLATE) ||
      state.is(Blocks::HEAVY_WEIGHTED_PRESSURE_PLATE))
    return 10;
  return 20;
}

int pressurePlateComputeSignalStrength(const World &world, Pos pos,
                                       const BlockState &state) {
  const Aabb box{pos.x + 1.0 / 16.0, pos.y + 0.0, pos.z + 1.0 / 16.0,
                 pos.x + 15.0 / 16.0, pos.y + 0.25, pos.z + 15.0 / 16.0};
  const auto candidates = world.entitiesIn(box);
  int count = 0;
  const bool mobsOnly = state.is(Blocks::STONE_PRESSURE_PLATE) ||
                        state.is(Blocks::POLISHED_BLACKSTONE_PRESSURE_PLATE);
  for (Entity *entity : candidates) {
    if (!entity || entity->isRemoved() || entity->isIgnoringBlockTriggers())
      continue;
    if (!mobsOnly || entity->isLiving())
      ++count;
  }
  if (count == 0)
    return 0;
  if (state.is(Blocks::LIGHT_WEIGHTED_PRESSURE_PLATE)) {
    const int capped = std::min(count, 15);
    return std::max(1, (capped * 15 + 14) / 15);
  }
  if (state.is(Blocks::HEAVY_WEIGHTED_PRESSURE_PLATE)) {
    const int capped = std::min(count, 150);
    return (capped * 15 + 149) / 150;
  }
  return 15;
}

void pressurePlateUpdateNeighbours(World &world, Pos pos,
                                   const BlockState &state) {
  world.updateNeighborsAt(pos, state.block());
  world.updateNeighborsAt(pos.relative(Direction::Down), state.block());
}

void pressurePlateApplySignal(World &world, Pos pos, const BlockState &state,
                              int oldSignal, int signal) {
  if (oldSignal != signal) {
    const bool weighted = state.is(Blocks::LIGHT_WEIGHTED_PRESSURE_PLATE) ||
                          state.is(Blocks::HEAVY_WEIGHTED_PRESSURE_PLATE);
    const BlockState next =
        weighted ? state.with(PropertyKind::Power,
                              static_cast<std::uint8_t>(signal))
                 : state.with(PropertyKind::Powered,
                              signal > 0 ? std::uint8_t{1} : std::uint8_t{0});
    world.setBlock(pos, next, 2);
    pressurePlateUpdateNeighbours(world, pos, next);
  }
  if (signal > 0)
    world.scheduleTick(pos, state.block(), pressurePlatePressedTime(state));
}

} // namespace

bool leverCanAttach(World &world, Pos pos, const BlockState &state) {
  const Direction dir = opposite(faceAttachedConnectedDirection(state));
  const Pos rel = pos.relative(dir);
  return faceSturdy(world, rel, world.getBlock(rel), opposite(dir),
                    SupportType::Full);
}

bool leverUse(World &world, Pos pos, const BlockState &state) {
  if (!state.is(Blocks::LEVER))
    return false;
  const BlockState next =
      state.with(PropertyKind::Powered,
                 poweredOf(state) ? std::uint8_t{0} : std::uint8_t{1});
  world.setBlock(pos, next, 3);
  faceAttachedUpdateNeighbours(world, pos, next);
  // TODO: lever sound and game event.
  return true;
}

bool buttonUse(World &world, Pos pos, const BlockState &state) {
  if (!state.block().isButton() || poweredOf(state))
    return false;
  const BlockState next = state.with(PropertyKind::Powered, std::uint8_t{1});
  world.setBlock(pos, next, 3);
  faceAttachedUpdateNeighbours(world, pos, next);
  world.scheduleTick(pos, next.block(), buttonPressedTime(next));
  // TODO: button sound and game event.
  return true;
}

bool pressurePlateCanSurvive(World &world, Pos pos) {
  const Pos below = pos.relative(Direction::Down);
  const BlockState belowState = world.getBlock(below);
  return faceSturdy(world, below, belowState, Direction::Up,
                    SupportType::Rigid) ||
         faceSturdy(world, below, belowState, Direction::Up,
                    SupportType::Center);
}

void targetHit(World &world, Pos pos, const BlockState &state, int power,
               int duration) {
  if (!state.is(Blocks::TARGET))
    return;
  const int clamped = std::clamp(power, 0, 15);
  world.setBlock(
      pos, state.with(PropertyKind::Power, static_cast<std::uint8_t>(clamped)),
      3);
  world.scheduleTick(pos, state.block(), duration);
}

void leverAffectRemoval(World &world, Pos pos, const BlockState &state,
                        bool movedByPiston) {
  if (!movedByPiston && poweredOf(state))
    faceAttachedUpdateNeighbours(world, pos, state);
}

BlockState leverUpdateShape(World &world, Pos pos, const BlockState &state,
                            Direction direction, Pos, const BlockState &) {
  if (opposite(faceAttachedConnectedDirection(state)) == direction &&
      !leverCanAttach(world, pos, state))
    return airState();
  return state;
}

void buttonAffectRemoval(World &world, Pos pos, const BlockState &state,
                         bool movedByPiston) {
  if (!movedByPiston && poweredOf(state))
    faceAttachedUpdateNeighbours(world, pos, state);
}

BlockState buttonUpdateShape(World &world, Pos pos, const BlockState &state,
                             Direction direction, Pos, const BlockState &) {
  if (opposite(faceAttachedConnectedDirection(state)) == direction &&
      !leverCanAttach(world, pos, state))
    return airState();
  return state;
}

void buttonTick(World &world, Pos pos, const BlockState &state) {
  if (!poweredOf(state))
    return;
  if (buttonPressedTime(state) == 30) {
    const Aabb blockBox{static_cast<double>(pos.x), static_cast<double>(pos.y),
                        static_cast<double>(pos.z), pos.x + 1.0, pos.y + 1.0,
                        pos.z + 1.0};
    for (Entity *entity : world.entitiesIn(blockBox)) {
      if (auto *arrow = dynamic_cast<AbstractArrow *>(entity);
          arrow && !arrow->isRemoved()) {
        world.scheduleTick(pos, state.block(), buttonPressedTime(state));
        return;
      }
    }
  }
  const BlockState next = state.with(PropertyKind::Powered, std::uint8_t{0});
  world.setBlock(pos, next, 3);
  faceAttachedUpdateNeighbours(world, pos, next);
  // TODO: button sound and game event.
}

void targetOnPlace(World &world, Pos pos, const BlockState &state,
                   const BlockState &oldState, bool) {
  if (!oldState.sameBlock(state) &&
      propertyValue(state, PropertyKind::Power) > 0 &&
      !world.hasScheduledTick(pos, state.block())) {
    world.setBlock(pos, state.with(PropertyKind::Power, std::uint8_t{0}), 18);
  }
}

void targetTick(World &world, Pos pos, const BlockState &state) {
  if (propertyValue(state, PropertyKind::Power) != 0)
    world.setBlock(pos, state.with(PropertyKind::Power, std::uint8_t{0}), 3);
}

void pressurePlateAffectRemoval(World &world, Pos pos, const BlockState &state,
                                bool movedByPiston) {
  if (!movedByPiston && pressurePlateSignalOf(state) > 0)
    pressurePlateUpdateNeighbours(world, pos, state);
}

BlockState pressurePlateUpdateShape(World &world, Pos pos,
                                    const BlockState &state,
                                    Direction direction, Pos,
                                    const BlockState &) {
  if (direction == Direction::Down && !pressurePlateCanSurvive(world, pos))
    return airState();
  return state;
}

void pressurePlateTick(World &world, Pos pos, const BlockState &state) {
  const int oldSignal = pressurePlateSignalOf(state);
  if (oldSignal <= 0)
    return;
  const int signal = pressurePlateComputeSignalStrength(world, pos, state);
  pressurePlateApplySignal(world, pos, state, oldSignal, signal);
}

void pressurePlateEntityInside(World &world, Pos pos, const BlockState &state,
                               Entity &entity) {
  if (entity.isIgnoringBlockTriggers())
    return;
  const int oldSignal = pressurePlateSignalOf(state);
  if (oldSignal > 0)
    return;
  const int signal = pressurePlateComputeSignalStrength(world, pos, state);
  pressurePlateApplySignal(world, pos, state, oldSignal, signal);
}

namespace {

void updateConnectedTripwireHooks(World &world, Pos wirePos,
                                  const BlockState &wireState) {
  const bool powered = wireState.getView("powered") == "true";
  for (Direction dir :
       {Direction::South, Direction::North, Direction::West, Direction::East}) {
    Pos p = wirePos;
    for (int step = 1; step <= 42; ++step) {
      p = p.relative(dir);
      const BlockState s = world.getBlock(p);
      if (s.is(Blocks::TRIPWIRE_HOOK)) {
        const Direction hookFacing = directionOf(s);
        if (hookFacing == opposite(dir) && s.getView("attached") == "true") {
          const BlockState nextHook = s.with(
              PropertyKind::Powered, powered ? std::uint8_t{1} : std::uint8_t{0});
          if (nextHook != s) {
            world.setBlock(p, nextHook, 3);
            world.updateNeighborsAt(p, Blocks::TRIPWIRE_HOOK);
            world.updateNeighborsAt(p.relative(opposite(hookFacing)),
                                    Blocks::TRIPWIRE_HOOK);
          }
        }
        break;
      }
      if (!s.is(Blocks::TRIPWIRE))
        break;
    }
  }
}

void checkTripwirePressed(World &world, Pos pos, const BlockState &state) {
  const bool wasPowered = state.getView("powered") == "true";
  const Aabb box{pos.x + 0.0, pos.y + 0.0, pos.z + 0.0,
                 pos.x + 1.0, pos.y + 0.5, pos.z + 1.0};
  bool shouldPower = false;
  for (Entity *e : world.entitiesIn(box)) {
    if (e && !e->isRemoved() && !e->isIgnoringBlockTriggers()) {
      shouldPower = true;
      break;
    }
  }
  BlockState next = state;
  if (shouldPower != wasPowered) {
    next = state.with(PropertyKind::Powered,
                      shouldPower ? std::uint8_t{1} : std::uint8_t{0});
    world.setBlock(pos, next, 3);
    updateConnectedTripwireHooks(world, pos, next);
  }
  if (shouldPower)
    world.scheduleTick(pos, Blocks::TRIPWIRE, 10);
}

} // namespace

void tripWireEntityInside(World &world, Pos pos, const BlockState &state,
                          Entity &entity) {
  if (entity.isIgnoringBlockTriggers() || state.getView("powered") == "true")
    return;
  checkTripwirePressed(world, pos, state);
}

void tripWireTick(World &world, Pos pos, const BlockState &state) {
  if (state.getView("powered") == "true")
    checkTripwirePressed(world, pos, state);
}

void redstoneOreStepOn(World &world, Pos pos, const BlockState &state,
                       Entity &entity) {
  if (entity.isShiftKeyDown())
    return;
  if (state.getView("lit") != "true") {
    world.setBlock(pos, state.with(PropertyKind::Lit, std::uint8_t{1}), 3);
    world.scheduleTick(pos, state.block(), 600);
  }
}

void redstoneOreTick(World &world, Pos pos, const BlockState &state) {
  if (state.getView("lit") == "true")
    world.setBlock(pos, state.with(PropertyKind::Lit, std::uint8_t{0}), 3);
}

} // namespace redstone
