#include "redstone/internal.hpp"
#include "redstone/containers.hpp"
#include "redstone/entity.hpp"

namespace redstone {
namespace {

using redstone_detail::faceAttachedConnectedDirection;
using redstone_detail::isWireConnected;
using redstone_detail::pressurePlateSignalOf;
using redstone_detail::wirePowerOf;

bool isSignalSourceIntrinsic(const BlockState &state) {
  const Block &b = state.block();
  return &b == &Blocks::REDSTONE_BLOCK || &b == &Blocks::REDSTONE_WIRE ||
         &b == &Blocks::REDSTONE_TORCH || &b == &Blocks::REDSTONE_WALL_TORCH ||
         &b == &Blocks::REPEATER || &b == &Blocks::COMPARATOR ||
         &b == &Blocks::LEVER || b.isButton() || b.isPressurePlate() ||
         &b == &Blocks::TARGET || &b == &Blocks::OBSERVER ||
         &b == &Blocks::DETECTOR_RAIL || &b == &Blocks::DAYLIGHT_DETECTOR ||
         &b == &Blocks::LECTERN || &b == &Blocks::TRIPWIRE_HOOK ||
         &b == &Blocks::TRAPPED_CHEST;
}

} // namespace

bool isDiodeBlock(const BlockState &state) {
  return state.is(Blocks::REPEATER) || state.is(Blocks::COMPARATOR);
}

bool isRedstoneSignalSource(const World &world, const BlockState &state) {
  if (state.is(Blocks::REDSTONE_WIRE))
    return world.wireShouldSignal();
  return isSignalSourceIntrinsic(state);
}

int redstoneBlockDirectSignal(const World &world, Pos pos,
                              const BlockState &state, Direction direction) {
  if (state.is(Blocks::REDSTONE_WIRE))
    return world.wireShouldSignal()
               ? redstoneBlockSignal(world, pos, state, direction)
               : 0;
  if (state.is(Blocks::REDSTONE_TORCH) ||
      state.is(Blocks::REDSTONE_WALL_TORCH))
    return direction == Direction::Down
               ? redstoneBlockSignal(world, pos, state, direction)
               : 0;
  if (state.is(Blocks::REPEATER) || state.is(Blocks::COMPARATOR) ||
      state.is(Blocks::OBSERVER))
    return redstoneBlockSignal(world, pos, state, direction);
  if (state.is(Blocks::LEVER) || state.block().isButton())
    return (poweredOf(state) &&
            faceAttachedConnectedDirection(state) == direction)
               ? 15
               : 0;
  if (state.block().isPressurePlate())
    return direction == Direction::Up ? pressurePlateSignalOf(state) : 0;
  if (state.is(Blocks::DETECTOR_RAIL) || state.is(Blocks::LECTERN))
    return (direction == Direction::Up && poweredOf(state)) ? 15 : 0;
  if (state.is(Blocks::TRIPWIRE_HOOK))
    return (poweredOf(state) && directionOf(state) == direction) ? 15 : 0;
  if (state.is(Blocks::TRAPPED_CHEST))
    return direction == Direction::Up ? trappedChestSignal(world, pos) : 0;
  return 0;
}

int redstoneBlockSignal(const World &world, Pos pos, const BlockState &state,
                        Direction direction) {
  if (state.is(Blocks::REDSTONE_BLOCK))
    return 15;
  if (state.is(Blocks::REDSTONE_WIRE)) {
    if (!world.wireShouldSignal() || direction == Direction::Down)
      return 0;
    const int power = wirePowerOf(state);
    if (power == 0)
      return 0;
    if (direction == Direction::Up)
      return power;
    const BlockState conn = redstoneWireConnectionState(world, pos, state);
    return isWireConnected(conn, opposite(direction)) ? power : 0;
  }
  if (state.is(Blocks::REDSTONE_TORCH))
    return (litOf(state) && direction != Direction::Up) ? 15 : 0;
  if (state.is(Blocks::REDSTONE_WALL_TORCH))
    return (litOf(state) && directionOf(state) != direction) ? 15 : 0;
  if (state.is(Blocks::REPEATER))
    return (poweredOf(state) && directionOf(state) == direction) ? 15 : 0;
  if (state.is(Blocks::COMPARATOR))
    return (poweredOf(state) && directionOf(state) == direction)
               ? world.comparatorOutput(pos)
               : 0;
  if (state.is(Blocks::LEVER) || state.block().isButton())
    return poweredOf(state) ? 15 : 0;
  if (state.block().isPressurePlate())
    return pressurePlateSignalOf(state);
  if (state.is(Blocks::TARGET))
    return static_cast<int>(propertyValue(state, PropertyKind::Power));
  if (state.is(Blocks::OBSERVER))
    return (poweredOf(state) &&
            directionOf(state, Direction::South) == direction)
               ? 15
               : 0;
  if (state.is(Blocks::DETECTOR_RAIL) || state.is(Blocks::LECTERN) ||
      state.is(Blocks::TRIPWIRE_HOOK))
    return poweredOf(state) ? 15 : 0;
  if (state.is(Blocks::DAYLIGHT_DETECTOR))
    return static_cast<int>(propertyValue(state, PropertyKind::Power));
  if (state.is(Blocks::TRAPPED_CHEST))
    return trappedChestSignal(world, pos);
  return 0;
}

bool hasAnalogOutputSignal(const BlockState &state) {
  return state.is(Blocks::COMPOSTER) || state.is(Blocks::CAULDRON) ||
         state.is(Blocks::WATER_CAULDRON) || state.is(Blocks::LAVA_CAULDRON) ||
         state.is(Blocks::POWDER_SNOW_CAULDRON) || state.is(Blocks::CAKE) ||
         state.is(Blocks::RESPAWN_ANCHOR) || state.is(Blocks::BEEHIVE) ||
         state.is(Blocks::BEE_NEST) || state.block().isCopperBulb() ||
         state.is(Blocks::END_PORTAL_FRAME) || state.block().isChest() ||
         state.is(Blocks::BARREL) || state.is(Blocks::DISPENSER) ||
         state.is(Blocks::DROPPER) || state.is(Blocks::HOPPER) ||
         state.is(Blocks::DETECTOR_RAIL);
  // TODO: remaining analog-output blocks (furnace, smoker, blast_furnace,
  // brewing_stand, shulker_box, lectern, jukebox, chiseled_bookshelf,
  // decorated_pot, crafter) still lack BlockEntity storage.
}

int comparatorAnalogOutput(World &world, Pos pos, const BlockState &state,
                           Direction) {
  if (state.is(Blocks::DETECTOR_RAIL)) {
    if (propertyValue(state, PropertyKind::Powered) == 0)
      return 0;
    const Aabb box{pos.x + 0.2, static_cast<double>(pos.y), pos.z + 0.2,
                   pos.x + 0.8, pos.y + 0.8, pos.z + 0.8};
    for (Entity *entity : world.entitiesIn(box)) {
      if (entity->isRemoved())
        continue;
      if (auto *cart = dynamic_cast<Minecart *>(entity)) {
        if (auto *container = dynamic_cast<ContainerEntity *>(cart))
          return container->comparatorSignal();
      }
    }
    return 0;
  }
  if (containerKindForState(state))
    return containerComparatorSignal(world, pos, state);
  if (state.is(Blocks::COMPOSTER))
    return static_cast<int>(propertyValue(state, PropertyKind::Level));
  if (state.is(Blocks::CAULDRON))
    return 0;
  if (state.is(Blocks::WATER_CAULDRON) ||
      state.is(Blocks::POWDER_SNOW_CAULDRON))
    return static_cast<int>(propertyValue(state, PropertyKind::Level, 1));
  if (state.is(Blocks::LAVA_CAULDRON))
    return 3;
  if (state.is(Blocks::CAKE))
    return (7 - static_cast<int>(propertyValue(state, PropertyKind::Bites))) * 2;
  if (state.is(Blocks::RESPAWN_ANCHOR)) {
    const int charges =
        static_cast<int>(propertyValue(state, PropertyKind::Charges));
    return charges == 0 ? 0 : charges * 4 - 1;
  }
  if (state.is(Blocks::BEEHIVE) || state.is(Blocks::BEE_NEST))
    return static_cast<int>(propertyValue(state, PropertyKind::HoneyLevel));
  if (state.block().isCopperBulb())
    return litOf(state) ? 15 : 0;
  if (state.is(Blocks::END_PORTAL_FRAME))
    return propertyValue(state, PropertyKind::Eye) != 0 ? 15 : 0;
  return 0;
}

} // namespace redstone
