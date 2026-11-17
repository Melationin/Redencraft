#include "engine/internal.hpp"

namespace redstone {
namespace detail {
struct WorldBehaviours {
  // ---- onPlace ----
  static void observerOnPlace(World &world, Pos p, const BlockState &state,
                              const BlockState &oldState, bool) {
    if (!oldState.sameBlock(state) && poweredOf(state) &&
        !world.hasScheduledTick(p, state.block())) {
      const BlockState next =
          state.with(PropertyKind::Powered, std::uint8_t{0});
      world.setBlock(p, next, 18);
      world.observerNotify(p, next);
    }
  }

  static void tntOnPlace(World &world, Pos p, const BlockState &state,
                         const BlockState &oldState, bool) {
    if (!oldState.sameBlock(state) && world.hasNeighborSignal(p) &&
        world.primeTntBlock(p)) {
      // primeTntBlock performs TntBlock.prime; onPlace then removes the block.
    }
  }

  // ---- affectNeighborsAfterRemoval ----
  static void pistonHeadAffectRemoval(World &world, Pos p,
                                      const BlockState &state, bool) {
    const Pos q = p.relative(opposite(directionOf(state)));
    if (fittingBase(state, world.getBlock(q))) {
      for (const auto &drop : blockDrops(world.getBlock(q)))
        world.spawnItem(q, drop.id, drop.count);
      world.setBlock(q, airState(), 3);
    }
  }

  static void observerAffectRemoval(World &world, Pos p,
                                    const BlockState &state, bool) {
    if (poweredOf(state) && world.hasScheduledTick(p, state.block()))
      world.observerNotify(p,
                           state.with(PropertyKind::Powered, std::uint8_t{0}));
  }

  // ---- neighborChanged ----
  static void pistonHeadNeighborChanged(World &world, Pos p,
                                        const BlockState &state,
                                        const BlockRef &source, bool) {
    const Direction d = directionOf(state);
    const Pos basePos = p.relative(opposite(d));
    const BlockState base = world.getBlock(basePos);
    if (fittingBase(state, base) ||
        (base.is(Blocks::MOVING_PISTON) && facingOf(base) == facingOf(state)))
      world.neighborChanged(basePos, source.str());
  }

  static void tntNeighborChanged(World &world, Pos p, const BlockState &,
                                 const BlockRef &, bool) {
    if (world.hasNeighborSignal(p) && world.tntExplodes)
      world.primeTntBlock(p);
  }

  static void redstoneLampNeighborChanged(World &world, Pos p,
                                          const BlockState &state,
                                          const BlockRef &, bool) {
    const bool lit = litOf(state);
    if (lit != world.hasNeighborSignal(p)) {
      if (lit)
        world.scheduleTick(p, state.block(), 4);
      else
        world.setBlock(p, state.with(PropertyKind::Lit, std::uint8_t{1}), 2);
    }
  }

  static void fenceGateNeighborChanged(World &world, Pos p,
                                       const BlockState &state,
                                       const BlockRef &, bool) {
    const bool signal = world.hasNeighborSignal(p);
    const bool powered = poweredOf(state);
    if (signal != powered) {
      world.setBlock(p,
                     state
                         .with(PropertyKind::Powered,
                               signal ? std::uint8_t{1} : std::uint8_t{0})
                         .with(PropertyKind::Open,
                               signal ? std::uint8_t{1} : std::uint8_t{0}),
                     2);
      if (openOf(state) != signal) {
        (void)world.random.nextFloat();
        // TODO: fence gate sound and BLOCK_OPEN/BLOCK_CLOSE game events.
      }
    }
  }

  static void noteBlockNeighborChanged(World &world, Pos p,
                                       const BlockState &state,
                                       const BlockRef &, bool) {
    const bool signal = world.hasNeighborSignal(p);
    const bool powered = poweredOf(state);
    if (signal != powered) {
      if (signal && world.getBlock(p.relative(Direction::Up)).isAir())
        world.blockEvent(p, state.block(), 0, 0);
      world.setBlock(
          p,
          state.with(PropertyKind::Powered,
                     signal ? std::uint8_t{1} : std::uint8_t{0}),
          3);
    }
  }

  // ---- updateShape ----
  static BlockState seagrassUpdateShape(World &world, Pos p,
                                        const BlockState &state, Direction, Pos,
                                        const BlockState &) {
    const Pos below = p.relative(Direction::Down);
    const BlockState support = world.getBlock(below);
    const bool survives =
        faceSturdy(world, below, support, Direction::Up) &&
        !world.hasBlockTag(support, "minecraft:cannot_support_seagrass")
             .value_or(false);
    if (!survives)
      return airState();
    world.scheduleFluidTick(p, FluidRef(Fluids::water()),
                            Fluids::water().tickDelay());
    return state;
  }

  static BlockState tallSeagrassUpdateShape(World &world, Pos p,
                                            const BlockState &state,
                                            Direction direction, Pos,
                                            const BlockState &neighbor) {
    const bool lower = state.getView("half") == "lower";
    if (((lower && direction == Direction::Up) ||
         (!lower && direction == Direction::Down)) &&
        (!neighbor.is(Blocks::TALL_SEAGRASS) ||
         neighbor.getView("half") == state.getView("half")))
      return airState();
    const Pos below = p.relative(Direction::Down);
    const BlockState support = world.getBlock(below);
    const bool survives =
        lower
            ? faceSturdy(world, below, support, Direction::Up) &&
                  !world.hasBlockTag(support, "minecraft:cannot_support_seagrass")
                       .value_or(false)
            : support.is(Blocks::TALL_SEAGRASS) &&
                  support.getView("half") == "lower";
    return survives ? state : airState();
  }

  static BlockState checkSupportTag(World &world, Pos p,
                                    const BlockState &state,
                                    std::string_view tag) {
    const BlockState support = world.getBlock(p.relative(Direction::Down));
    return world.hasBlockTag(support, tag).value_or(false) ? state : airState();
  }

  static BlockState dryVegetationUpdateShape(World &world, Pos p,
                                             const BlockState &state,
                                             Direction, Pos,
                                             const BlockState &) {
    return checkSupportTag(world, p, state,
                           "minecraft:supports_dry_vegetation");
  }

  static BlockState witherRoseUpdateShape(World &world, Pos p,
                                          const BlockState &state, Direction,
                                          Pos, const BlockState &) {
    return checkSupportTag(world, p, state, "minecraft:supports_wither_rose");
  }

  static BlockState vegetationUpdateShape(World &world, Pos p,
                                          const BlockState &state, Direction,
                                          Pos, const BlockState &) {
    return checkSupportTag(world, p, state, "minecraft:supports_vegetation");
  }

  static BlockState carpetUpdateShape(World &world, Pos p,
                                      const BlockState &state, Direction, Pos,
                                      const BlockState &) {
    return world.getBlock(p.relative(Direction::Down)).isAir() ? airState()
                                                               : state;
  }

  static BlockState coralFanUpdateShape(World &world, Pos p,
                                        const BlockState &state,
                                        Direction direction, Pos,
                                        const BlockState &) {
    const Direction supportDirection = isDeadCoralWallFan(state)
                                           ? opposite(directionOf(state))
                                           : Direction::Down;
    if (direction == supportDirection && !coralCanSurvive(world, p, state))
      return airState();
    return state;
  }

  static BlockState snowyUpdateShape(World &world, Pos,
                                     const BlockState &state,
                                     Direction direction, Pos,
                                     const BlockState &neighbor) {
    if (direction == Direction::Up) {
      const bool snowy =
          world.hasBlockTag(neighbor, "minecraft:snow").value_or(false);
      return state.with(PropertyKind::Snowy,
                        snowy ? std::uint8_t{1} : std::uint8_t{0});
    }
    return state;
  }

  static BlockState fenceGateUpdateShape(World &world, Pos p,
                                         const BlockState &state,
                                         Direction direction, Pos,
                                         const BlockState &neighbor) {
    const Direction facing = directionOf(state);
    const bool facingZ =
        facing == Direction::North || facing == Direction::South;
    const bool directionX =
        direction == Direction::East || direction == Direction::West;
    const bool directionZ =
        direction == Direction::North || direction == Direction::South;
    if ((facingZ && directionX) || (!facingZ && directionZ)) {
      const bool inWall =
          isWallBlock(neighbor) ||
          isWallBlock(world.getBlock(p.relative(opposite(direction))));
      return state.with("in_wall", inWall ? "true" : "false");
    }
    return state;
  }

  static BlockState wallBlockUpdateShape(World &world, Pos p,
                                         const BlockState &state,
                                         Direction direction, Pos,
                                         const BlockState &neighbor) {
    if (direction != Direction::Down)
      return wallUpdateShape(world, p, state, direction, neighbor);
    return state;
  }

  static BlockState stairBlockUpdateShape(World &world, Pos p,
                                          const BlockState &state,
                                          Direction direction, Pos,
                                          const BlockState &neighbor) {
    return stairUpdateShape(world, p, state, direction, neighbor);
  }

  static BlockState fenceBlockUpdateShape(World &world, Pos p,
                                          const BlockState &state,
                                          Direction direction, Pos,
                                          const BlockState &neighbor) {
    return fenceUpdateShape(world, p, state, direction, neighbor);
  }

  static BlockState ironBarsBlockUpdateShape(World &world, Pos p,
                                             const BlockState &state,
                                             Direction direction, Pos,
                                             const BlockState &neighbor) {
    return ironBarsUpdateShape(world, p, state, direction, neighbor);
  }

  static BlockState pistonHeadUpdateShape(World &world, Pos,
                                          const BlockState &state,
                                          Direction direction, Pos neighborPos,
                                          const BlockState &) {
    if (opposite(direction) == directionOf(state)) {
      const BlockState base = world.getBlock(neighborPos);
      if (!fittingBase(state, base) &&
          !(base.is(Blocks::MOVING_PISTON) && facingOf(base) == facingOf(state)))
        return airState();
    }
    return state;
  }

  static BlockState observerUpdateShape(World &world, Pos p,
                                        const BlockState &state,
                                        Direction direction, Pos,
                                        const BlockState &) {
    if (directionOf(state, Direction::South) == direction &&
        !poweredOf(state) && !world.hasScheduledTick(p, state.block()))
      world.scheduleTick(p, state.block(), 2);
    return state;
  }

  static bool fireCanSurvive(World &world, Pos p) {
    const Pos below = p.relative(Direction::Down);
    const BlockState belowState = world.getBlock(below);
    if (faceSturdy(world, below, belowState, Direction::Up, SupportType::Full))
      return true;
    for (Direction d : {Direction::Down, Direction::Up, Direction::North,
                        Direction::South, Direction::West, Direction::East}) {
      if (world.getBlock(p.relative(d)).block().metadata().ignitedByLava)
        return true;
    }
    return false;
  }

  static bool soulFireCanSurvive(World &world, Pos p) {
    const BlockState belowState = world.getBlock(p.relative(Direction::Down));
    return world.hasBlockTag(belowState, "minecraft:soul_fire_base_blocks")
        .value_or(false);
  }

  static void fireOnPlace(World &world, Pos p, const BlockState &state,
                          const BlockState &oldState, bool) {
    if (oldState.sameBlock(state))
      return;
    if (isInPortalDimension(world)) {
      if (auto shape =
              PortalShape::findEmptyPortalShape(world, p, PortalAxis::X);
          shape.has_value()) {
        shape->createPortalBlocks(world);
        return;
      }
    }
    const bool survives = state.is(Blocks::SOUL_FIRE)
                              ? soulFireCanSurvive(world, p)
                              : fireCanSurvive(world, p);
    if (!survives)
      world.removeBlock(p, false);
  }

  static BlockState fireUpdateShape(World &world, Pos p,
                                    const BlockState &state, Direction, Pos,
                                    const BlockState &) {
    return fireCanSurvive(world, p) ? state : airState();
  }

  static BlockState soulFireUpdateShape(World &world, Pos p,
                                        const BlockState &state, Direction, Pos,
                                        const BlockState &) {
    return soulFireCanSurvive(world, p) ? state : airState();
  }

  static BlockState netherPortalUpdateShape(World &world, Pos p,
                                            const BlockState &state,
                                            Direction direction, Pos,
                                            const BlockState &neighbor) {
    const BlockAxis updateAxis = axisOfDirection(direction);
    const PortalAxis portalAxis = portalAxisOfState(state);
    const BlockAxis portalBlockAxis =
        portalAxis == PortalAxis::X ? BlockAxis::X : BlockAxis::Z;
    const bool wrongAxis =
        portalBlockAxis != updateAxis && isHorizontalDirection(direction);
    if (!wrongAxis && !neighbor.is(Blocks::NETHER_PORTAL) &&
        !PortalShape::findAnyShape(world, p, portalAxis).isComplete()) {
      return airState();
    }
    return state;
  }

  static BlockState noteBlockUpdateShape(World &world, Pos p,
                                         const BlockState &state,
                                         Direction direction, Pos,
                                         const BlockState &) {
    if (direction == Direction::Up || direction == Direction::Down) {
      const Instrument aboveInst =
          world.getBlock(p.relative(Direction::Up))
              .block()
              .metadata()
              .instrument;
      Instrument chosen = Instrument::Harp;
      if (static_cast<std::uint8_t>(aboveInst) >=
          static_cast<std::uint8_t>(Instrument::Zombie)) {
        chosen = aboveInst;
      } else {
        const Instrument belowInst =
            world.getBlock(p.relative(Direction::Down))
                .block()
                .metadata()
                .instrument;
        chosen = static_cast<std::uint8_t>(belowInst) >=
                         static_cast<std::uint8_t>(Instrument::Zombie)
                     ? Instrument::Harp
                     : belowInst;
      }
      return state.with(PropertyKind::Instrument,
                        static_cast<std::uint8_t>(chosen));
    }
    return state;
  }

  // ---- scheduled tick ----
  static void observerTick(World &world, Pos pos, const BlockState &s) {
    const bool on = poweredOf(s);
    world.setBlock(
        pos, s.with(PropertyKind::Powered, on ? std::uint8_t{0} : std::uint8_t{1}),
        2);
    if (!on)
      world.scheduleTick(pos, s.block(), 2);
    world.observerNotify(pos, s);
  }

  static void redstoneLampTick(World &world, Pos pos, const BlockState &s) {
    if (litOf(s) && !world.hasNeighborSignal(pos))
      world.setBlock(pos, s.with("lit", "false"), 2);
  }

  static void fallingBlockTick(World &world, Pos pos, const BlockState &s) {
    // TODO: FIRE tags and replaceable blocks outside the implemented registry.
    if (pos.y >= world.minY_ &&
        isReplaceable(world.getBlock(pos.relative(Direction::Down))))
      FallingBlockEntity::fall(world, pos, s);
  }

  // ---- block event ----
  static bool noteBlockTriggerEvent(World &world, Pos pos, const BlockState &s,
                                    int, int) {
    world.log.push_back(
        {"note", pos, s.get("instrument", "harp") + ":" + s.get("note", "0")});
    return true;
  }
};

void bindBlockBehaviours(Block &block) {
  auto &b = block.behaviours_;
  if (&block == &Blocks::OBSERVER) {
    b.onPlace = &WorldBehaviours::observerOnPlace;
    b.affectNeighborsAfterRemoval = &WorldBehaviours::observerAffectRemoval;
    b.updateShape = &WorldBehaviours::observerUpdateShape;
    b.tick = &WorldBehaviours::observerTick;
  } else if (&block == &Blocks::TNT) {
    b.onPlace = &WorldBehaviours::tntOnPlace;
    b.neighborChanged = &WorldBehaviours::tntNeighborChanged;
  } else if (&block == &Blocks::PISTON_HEAD) {
    b.affectNeighborsAfterRemoval = &WorldBehaviours::pistonHeadAffectRemoval;
    b.neighborChanged = &WorldBehaviours::pistonHeadNeighborChanged;
    b.updateShape = &WorldBehaviours::pistonHeadUpdateShape;
  } else if (&block == &Blocks::REDSTONE_WIRE) {
    b.onPlace = &redstoneWireOnPlace;
    b.affectNeighborsAfterRemoval = &redstoneWireAffectRemoval;
    b.neighborChanged = &redstoneWireNeighborChanged;
    b.updateShape = &redstoneWireUpdateShape;
  } else if (&block == &Blocks::REDSTONE_TORCH) {
    b.onPlace = &redstoneTorchOnPlace;
    b.affectNeighborsAfterRemoval = &redstoneTorchAffectRemoval;
    b.neighborChanged = &redstoneTorchNeighborChanged;
    b.updateShape = &redstoneTorchUpdateShape;
    b.tick = &redstoneTorchTick;
  } else if (&block == &Blocks::REDSTONE_WALL_TORCH) {
    b.onPlace = &redstoneTorchOnPlace;
    b.affectNeighborsAfterRemoval = &redstoneTorchAffectRemoval;
    b.neighborChanged = &redstoneTorchNeighborChanged;
    b.updateShape = &redstoneWallTorchUpdateShape;
    b.tick = &redstoneTorchTick;
  } else if (&block == &Blocks::REPEATER) {
    b.onPlace = &repeaterOnPlace;
    b.affectNeighborsAfterRemoval = &repeaterAffectRemoval;
    b.neighborChanged = &repeaterNeighborChanged;
    b.updateShape = &repeaterUpdateShape;
    b.tick = &repeaterTick;
  } else if (&block == &Blocks::COMPARATOR) {
    b.onPlace = &comparatorOnPlace;
    b.affectNeighborsAfterRemoval = &comparatorAffectRemoval;
    b.neighborChanged = &comparatorNeighborChanged;
    b.tick = &comparatorTick;
  } else if (&block == &Blocks::LEVER) {
    b.affectNeighborsAfterRemoval = &leverAffectRemoval;
    b.updateShape = &leverUpdateShape;
  } else if (block.isButton()) {
    b.affectNeighborsAfterRemoval = &buttonAffectRemoval;
    b.updateShape = &buttonUpdateShape;
    b.tick = &buttonTick;
  } else if (&block == &Blocks::TARGET) {
    b.onPlace = &targetOnPlace;
    b.tick = &targetTick;
  } else if (block.isPressurePlate()) {
    b.affectNeighborsAfterRemoval = &pressurePlateAffectRemoval;
    b.updateShape = &pressurePlateUpdateShape;
    b.tick = &pressurePlateTick;
  } else if (&block == &Blocks::TRIPWIRE) {
    b.tick = &tripWireTick;
  } else if (&block == &Blocks::REDSTONE_ORE ||
             &block == &Blocks::DEEPSLATE_REDSTONE_ORE) {
    b.tick = &redstoneOreTick;
  } else if (block.isDoor()) {
    b.neighborChanged = &doorNeighborChanged;
    b.updateShape = &doorUpdateShape;
  } else if (block.isTrapdoor()) {
    b.neighborChanged = &trapdoorNeighborChanged;
  } else if (block.isCopperBulb()) {
    b.onPlace = &copperBulbOnPlace;
    b.neighborChanged = &copperBulbNeighborChanged;
  } else if (&block == &Blocks::REDSTONE_LAMP) {
    b.neighborChanged = &WorldBehaviours::redstoneLampNeighborChanged;
    b.tick = &WorldBehaviours::redstoneLampTick;
  } else if (block.isChest()) {
    b.onPlace = &chestOnPlace;
    b.affectNeighborsAfterRemoval = &chestAffectRemoval;
    b.updateShape = &chestUpdateShape;
  } else if (&block == &Blocks::BARREL) {
    b.onPlace = &barrelOnPlace;
    b.affectNeighborsAfterRemoval = &barrelAffectRemoval;
    b.tick = &barrelTick;
  } else if (&block == &Blocks::DISPENSER) {
    b.onPlace = &dispenserOnPlace;
    b.affectNeighborsAfterRemoval = &dispenserAffectRemoval;
    b.neighborChanged = &dispenserNeighborChanged;
    b.tick = &dispenserTick;
  } else if (&block == &Blocks::DROPPER) {
    b.onPlace = &dispenserOnPlace;
    b.affectNeighborsAfterRemoval = &dispenserAffectRemoval;
    b.neighborChanged = &dispenserNeighborChanged;
    b.tick = &dropperTick;
  } else if (&block == &Blocks::HOPPER) {
    b.onPlace = &hopperOnPlace;
    b.affectNeighborsAfterRemoval = &hopperAffectRemoval;
    b.neighborChanged = &hopperNeighborChanged;
  } else if (&block == &Blocks::COMPOSTER) {
    b.onPlace = &composterOnPlace;
    b.tick = &composterTick;
  } else if (&block == &Blocks::NOTE_BLOCK) {
    b.neighborChanged = &WorldBehaviours::noteBlockNeighborChanged;
    b.updateShape = &WorldBehaviours::noteBlockUpdateShape;
    b.triggerEvent = &WorldBehaviours::noteBlockTriggerEvent;
  } else if (&block == &Blocks::FIRE) {
    b.onPlace = &WorldBehaviours::fireOnPlace;
    b.updateShape = &WorldBehaviours::fireUpdateShape;
    // TODO: FireBlock flammable tick/spread & burn-out remain unimplemented.
  } else if (&block == &Blocks::SOUL_FIRE) {
    b.onPlace = &WorldBehaviours::fireOnPlace;
    b.updateShape = &WorldBehaviours::soulFireUpdateShape;
  } else if (&block == &Blocks::NETHER_PORTAL) {
    b.updateShape = &WorldBehaviours::netherPortalUpdateShape;
    // TODO: NetherPortalBlock::randomTick ZombifiedPiglin spawning remains
    // unimplemented (no mob entity implementation yet).
  } else if (block.isFenceGate()) {
    b.neighborChanged = &WorldBehaviours::fenceGateNeighborChanged;
    b.updateShape = &WorldBehaviours::fenceGateUpdateShape;
  } else if (block.isWall()) {
    b.updateShape = &WorldBehaviours::wallBlockUpdateShape;
  } else if (block.isStairs()) {
    b.updateShape = &WorldBehaviours::stairBlockUpdateShape;
  } else if (block.isFence()) {
    b.updateShape = &WorldBehaviours::fenceBlockUpdateShape;
  } else if (isIronBarsBlock(block.defaultState())) {
    b.updateShape = &WorldBehaviours::ironBarsBlockUpdateShape;
  } else if (block.isCarpet()) {
    b.updateShape = &WorldBehaviours::carpetUpdateShape;
  } else if (block.isSnowy()) {
    b.updateShape = &WorldBehaviours::snowyUpdateShape;
  } else if ((block.isCoralFan() || block.isCoralWallFan()) &&
             block.supported()) {
    b.updateShape = &WorldBehaviours::coralFanUpdateShape;
  } else if (&block == &Blocks::SEAGRASS) {
    b.updateShape = &WorldBehaviours::seagrassUpdateShape;
  } else if (&block == &Blocks::TALL_SEAGRASS) {
    b.updateShape = &WorldBehaviours::tallSeagrassUpdateShape;
  } else if (&block == &Blocks::DEAD_BUSH ||
             &block == &Blocks::SHORT_DRY_GRASS ||
             &block == &Blocks::TALL_DRY_GRASS) {
    b.updateShape = &WorldBehaviours::dryVegetationUpdateShape;
  } else if (&block == &Blocks::WITHER_ROSE) {
    b.updateShape = &WorldBehaviours::witherRoseUpdateShape;
  } else if (&block == &Blocks::SHORT_GRASS || &block == &Blocks::FERN ||
             &block == &Blocks::BUSH ||
             isRegisteredSmallFlower(block.defaultState())) {
    b.updateShape = &WorldBehaviours::vegetationUpdateShape;
  }
  if (block.isFalling())
    b.tick = &WorldBehaviours::fallingBlockTick;
}
} // namespace detail

void Block::onPlace(World &world, Pos pos, const BlockState &state,
                    const BlockState &oldState, bool movedByPiston) const {
  if (behaviours_.onPlace)
    behaviours_.onPlace(world, pos, state, oldState, movedByPiston);
}

void Block::affectNeighborsAfterRemoval(World &world, Pos pos,
                                        const BlockState &state,
                                        bool movedByPiston) const {
  if (behaviours_.affectNeighborsAfterRemoval)
    behaviours_.affectNeighborsAfterRemoval(world, pos, state, movedByPiston);
}

void Block::neighborChanged(World &world, Pos pos, const BlockState &state,
                            const BlockRef &source, bool movedByPiston) const {
  if (behaviours_.neighborChanged)
    behaviours_.neighborChanged(world, pos, state, source, movedByPiston);
}

BlockState Block::updateShape(World &world, Pos pos, const BlockState &state,
                              Direction direction, Pos neighborPos,
                              const BlockState &neighbor) const {
  if (behaviours_.updateShape)
    return behaviours_.updateShape(world, pos, state, direction, neighborPos,
                                   neighbor);
  return state;
}

void Block::tick(World &world, Pos pos, const BlockState &state) const {
  if (behaviours_.tick)
    behaviours_.tick(world, pos, state);
}

bool Block::triggerEvent(World &world, Pos pos, const BlockState &state,
                         int type, int data) const {
  return behaviours_.triggerEvent &&
         behaviours_.triggerEvent(world, pos, state, type, data);
}
} // namespace redstone
