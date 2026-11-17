#include "net/session_internal.hpp"

namespace redstone::net {
void Session::acknowledge(int sequence) {
  if (sequence < 0)
    throw ProtocolError("negative prediction sequence");
  Writer out;
  out.varInt(sequence);
  send("block_changed_ack", out);
}
void Session::creativeSlot(Reader &in) {
  const int slot = in.i16(), count = in.varInt();
  ItemStack item;
  if (count < 0 || count > 64)
    throw ProtocolError("invalid stack count");
  if (count > 0) {
    item = itemStack(data_.registry.itemName(in.varInt()), count);
    const int added = in.varInt(), removed = in.varInt();
    if (added < 0 || removed < 0 || added > 256 || removed > 256)
      throw ProtocolError("invalid components");
    ItemComponentData compData;
    bool hasUnsupportedComponent = (removed > 0);
    for (int i = 0; i < added; ++i) {
      const int compId = in.varInt();
      int size = in.varInt();
      if (size < 0)
        throw ProtocolError("invalid component size");
      auto rawBytes = in.raw(size);
      // Decode simple primitive 26.1 DataComponents IDs (DataComponents.java):
      // 1=max_stack_size (VarInt), 2=max_damage (VarInt), 3=damage (VarInt),
      // 4=unbreakable (Unit), 19=repair_cost (VarInt), 44=dyed_color (i32).
      try {
        Reader cr{rawBytes};
        if (compId == 1) {
          compData.maxStackSize = std::clamp(cr.varInt(), 1, 99);
          if (!cr.empty())
            hasUnsupportedComponent = true;
        } else if (compId == 2) {
          compData.maxDamage = std::max(0, cr.varInt());
          if (!cr.empty())
            hasUnsupportedComponent = true;
        } else if (compId == 3) {
          compData.damage = std::max(0, cr.varInt());
          if (!cr.empty())
            hasUnsupportedComponent = true;
        } else if (compId == 4) {
          if (!cr.empty())
            hasUnsupportedComponent = true;
          else
            compData.unbreakable = true;
        } else if (compId == 19) {
          compData.repairCost = std::max(0, cr.varInt());
          if (!cr.empty())
            hasUnsupportedComponent = true;
        } else if (compId == 44) {
          compData.dyedColor = static_cast<std::uint32_t>(cr.i32());
          if (!cr.empty())
            hasUnsupportedComponent = true;
        } else {
          hasUnsupportedComponent = true;
        }
      } catch (...) {
        hasUnsupportedComponent = true;
      }
    }
    for (int i = 0; i < removed; ++i)
      (void)in.varInt();
    if (!compData.empty())
      item = withItemComponents(std::move(item), std::move(compData));
    const Item *itemDef = item.item();
    if (hasUnsupportedComponent || !itemDef || !isCreativeItemSupported(*itemDef)) {
      item = {};
      if (diagnostic)
        diagnostic("TODO: unsupported creative item or custom components");
    } else if (count > item.maxStackSize()) {
      throw ProtocolError("stack exceeds item maximum");
    }
  }
  if (!in.empty())
    throw ProtocolError("trailing creative slot data");
  if (slot >= 36 && slot <= 44)
    setSlot(slot - 36, item);
  else if (slot >= 9 && slot <= 35)
    setSlot(slot, item);
  else if (slot == 45)
    setSlot(40, item);
  else if (slot == -1) {
    if (!item.empty() && dropSpam_ < 200 && !teleportPending_ &&
        world_->isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      dropSpam_ += 20;
      world_->spawnItemAt(position_ + Vec3{0, 1.3, 0}, item.id, item.count);
    }
  }
}
void Session::playerAction(Reader &in) {
  const int action = in.varInt();
  const Pos p = in.position();
  const int face = in.u8(), sequence = in.varInt();
  if (face > 5 || action < 0 || sequence < 0 || !in.empty())
    throw ProtocolError("invalid action");
  if (action <= 2) {
    if (canInteract(p) && action == 0) {
      const auto old = world_->getBlock(p);
      // Creative destruction has no drops, including the counterpart base of
      // a piston head. Keep setBlock's normal update/removal paths.
      if (old.is(Blocks::PISTON_HEAD)) {
        const auto basePos =
            p.relative(opposite(directionFromString(old.get("facing"))));
        const auto base = world_->getBlock(basePos);
        if ((old.get("type", "normal") == "sticky"
                 ? base.is(Blocks::STICKY_PISTON)
                 : base.is(Blocks::PISTON)) &&
            base.get("extended") == "true" &&
            base.get("facing") == old.get("facing"))
          world_->setBlock(basePos, airState());
      }
      // ServerPlayerGameMode.destroyBlock -> Level.removeBlock(pos, false):
      // creative breaking keeps the water of a waterlogged host.
      world_->removeBlock(p);
    }
    sendBlockUpdate(p);
    acknowledge(sequence);
  } else if (action == 6) {
    const auto held = inventory_[selectedSlot_];
    setSlot(selectedSlot_, inventory_[40]);
    setSlot(40, held);
  } else if (action == 3 || action == 4) {
    auto item = inventory_[selectedSlot_];
    if (!item.empty() && !teleportPending_ &&
        world_->isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      int count = action == 3 ? item.count : 1;
      world_->spawnItemAt(position_ + Vec3{0, 1.3, 0}, item.id, count);
      item.count -= count;
      setSlot(selectedSlot_, item);
    }
  } else if (diagnostic)
    diagnostic("TODO: unsupported player action");
}
void Session::useItemOn(Reader &in) {
  const int hand = in.varInt();
  const Pos clicked = in.position();
  const int faceId = in.varInt();
  const float hitX = in.f32(), hitY = in.f32(), hitZ = in.f32();
  (void)in.bool_();
  (void)in.bool_();
  const int sequence = in.varInt();
  if (hand < 0 || hand > 1 || faceId < 0 || faceId > 5 || !in.empty() ||
      sequence < 0 || !std::isfinite(hitX) || !std::isfinite(hitY) ||
      !std::isfinite(hitZ) || std::abs(hitX - .5) > 1.0000001 ||
      std::abs(hitY - .5) > 1.0000001 || std::abs(hitZ - .5) > 1.0000001)
    throw ProtocolError("invalid block hit");
  const Direction face = static_cast<Direction>(faceId);
  const auto old = world_->getBlock(clicked);
  const int handSlot = hand ? 40 : selectedSlot_;
  ItemStack held = inventory_[handSlot];
  const bool clickedReplaceable =
      canReplaceForPlacement(old, held, hitY, face, true);
  const Pos target = clickedReplaceable ? clicked : clicked.relative(face);
  const bool creativeMode = !playerEntity_ || playerEntity_->creative;
  const auto tryItemUseOn = [&](Pos usePos) -> bool {
    if (held.empty() || !canInteract(usePos))
      return false;
    ItemUseOnParams params;
    params.clickedPos = usePos;
    params.clickedFace = face;
    params.clickLocation = {usePos.x + hitX, usePos.y + hitY, usePos.z + hitZ};
    params.horizontalFacing = horizontal(yaw_);
    params.playerYaw = yaw_;
    params.playerPitch = pitch_;
    params.creative = creativeMode;
    params.secondaryActive = sneaking_;
    params.mainHand = (hand == 0);
    params.player = playerEntity_.get();
    ItemStack updatedHeld = held;
    const ItemUseResult res = useItemOnBlock(*world_, updatedHeld, params);
    if (!res.consumesAction())
      return false;
    if (updatedHeld != held) {
      setSlot(handSlot, std::move(updatedHeld));
    }
    for (Pos p : res.updatedBlocks)
      sendBlockUpdate(p);
    return true;
  };
  if (canInteract(clicked) && !sneaking_ && isFenceGate(old)) {
    auto next = old;
    if (old.getView("open", "false") == "true") {
      next = next.with("open", "false");
    } else {
      const auto playerFacing = horizontal(yaw_);
      if (directionFromString(old.getView("facing", "north")) == opposite(playerFacing))
        next = next.with("facing", directionName(playerFacing));
      next = next.with("open", "true");
    }
    world_->setBlock(clicked, next, 10);
    (void)world_->random.nextFloat();
    // TODO: fence gate sound and BLOCK_OPEN/BLOCK_CLOSE game events.
  } else if (canInteract(clicked) && !sneaking_ &&
      old.is(Blocks::NOTE_BLOCK)) {
    auto tuned = old.with(
        "note", std::to_string((std::stoi(old.get("note", "0")) + 1) % 25));
    world_->setBlock(clicked, tuned, 3);
    if (isAirState(world_->getBlock(clicked.relative(Direction::Up))))
      world_->blockEvent(clicked, old.name(), 0, 0);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::REDSTONE_WIRE) &&
             redstoneWireUse(*world_, clicked, old)) {
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::REPEATER)) {
    repeaterUse(*world_, clicked, old);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::COMPARATOR)) {
    comparatorUse(*world_, clicked, old);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::LEVER)) {
    leverUse(*world_, clicked, old);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.block().isButton()) {
    buttonUse(*world_, clicked, old);
  } else if (canInteract(clicked) && !sneaking_ &&
             old.is(Blocks::COMPOSTER)) {
    const int level = std::stoi(std::string(old.getView("level", "0")));
    if (level == 8) {
      world_->setBlock(clicked, old.with("level", "0"), 3);
      world_->spawnItemAt(
          {clicked.x + 0.5, clicked.y + 1.01, clicked.z + 0.5},
          "minecraft:bone_meal", 1);
    } else if (level < 7 && !held.empty() && isCompostableItem(held.id)) {
      const float chance = composterChance(held.id);
      if ((level == 0 && chance > 0.0f) || world_->random.nextDouble() < chance) {
        const int nextLevel = level + 1;
        world_->setBlock(
            clicked, old.with("level", std::to_string(nextLevel)), 3);
        if (nextLevel == 7)
          world_->scheduleTick(clicked, Blocks::COMPOSTER, 20);
      }
    }
  } else if (canInteract(clicked) && !sneaking_ &&
             containerKindForState(old).has_value()) {
    openContainerScreen(clicked, old);
  } else if (canInteract(clicked) && tryItemUseOn(clicked)) {
    // Handled via 26.1 Item::useOn (Axe, Shovel, Hoe, Honeycomb, FlintAndSteel,
    // FireCharge, BoneMeal, Minecart, ArmorStand, ItemFrame, EndCrystal,
    // SolidBucket).
  } else if (canInteract(clicked) && !held.empty() && isBucketItem(held.id)) {
    // BucketItem uses server-side raycast on use_item, not block placement.
  } else if (canInteract(clicked) && canInteract(target) && !held.empty() &&
             canReplaceForPlacement(world_->getBlock(target), held, hitY, face,
                                    clickedReplaceable)) {
    const std::string placeBlockId(placementBlockNameForItem(held.id));
    if (isSupportedBlock(placeBlockId) && placeBlockId != "minecraft:air" &&
        placeBlockId != "minecraft:water" && placeBlockId != "minecraft:lava") {
      auto state = defaultState(placeBlockId);
      bool valid = true;
      bool isDoubleSlab = false;
      if (state.is(Blocks::REDSTONE_WIRE)) {
        state = redstoneWirePlacementState(*world_, target);
        valid = redstoneWireCanSurvive(*world_, target);
      } else if (state.is(Blocks::REDSTONE_TORCH)) {
        auto directions = lookingDirections(yaw_, pitch_);
        if (target != clicked) {
          auto it =
              std::find(directions.begin(), directions.end(), opposite(face));
          std::rotate(directions.begin(), it, it + 1);
        }
        valid = false;
        for (Direction d : directions) {
          if (d == Direction::Up)
            continue;
          if (d == Direction::Down) {
            if (redstoneTorchCanSurvive(*world_, target)) {
              state = Blocks::REDSTONE_TORCH.defaultState();
              valid = true;
              break;
            }
          } else {
            const Direction facing = opposite(d);
            if (redstoneWallTorchCanSurvive(*world_, target, facing)) {
              state = Blocks::REDSTONE_WALL_TORCH.defaultState().with(
                  "facing", directionName(facing));
              valid = true;
              break;
            }
          }
        }
      } else if (state.is(Blocks::LEVER) || state.block().isButton()) {
        auto directions = lookingDirections(yaw_, pitch_);
        if (target != clicked) {
          auto it =
              std::find(directions.begin(), directions.end(), opposite(face));
          std::rotate(directions.begin(), it, it + 1);
        }
        valid = false;
        const auto base = state;
        for (Direction d : directions) {
          BlockState candidate;
          if (d == Direction::Up || d == Direction::Down) {
            candidate =
                base.with("face", d == Direction::Up ? "ceiling" : "floor")
                    .with("facing", directionName(horizontal(yaw_)));
          } else {
            candidate = base.with("face", "wall")
                            .with("facing", directionName(opposite(d)));
          }
          if (leverCanAttach(*world_, target, candidate)) {
            state = candidate;
            valid = true;
            break;
          }
        }
      } else if (state.is(Blocks::REPEATER)) {
        state = repeaterPlacementState(*world_, target, horizontal(yaw_));
        valid = diodeCanSurvive(*world_, target);
      } else if (state.is(Blocks::COMPARATOR)) {
        state = comparatorPlacementState(horizontal(yaw_));
        valid = diodeCanSurvive(*world_, target);
      } else if (isDeadCoralFan(state)) {
        auto directions = lookingDirections(yaw_, pitch_);
        // BlockPlaceContext prioritizes the clicked face only when placing
        // adjacent to a non-replaceable clicked block.
        if (target != clicked) {
          auto it =
              std::find(directions.begin(), directions.end(), opposite(face));
          std::rotate(directions.begin(), it, it + 1);
        }
        std::optional<BlockState> wall;
        for (Direction d : directions)
          if (d != Direction::Up && d != Direction::Down) {
            wall = world_->coralPlacementState(target, state, opposite(d));
            if (wall)
              break;
          }
        valid = false;
        for (Direction d : directions) {
          if (d == Direction::Up)
            continue;
          auto candidate =
              d == Direction::Down
                  ? world_->coralPlacementState(target, state, Direction::Up)
                  : wall;
          if (candidate) {
            state = *candidate;
            valid = true;
            break;
          }
        }
      } else if (state.block().isSlab()) {
        const auto existing = world_->getBlock(target);
        if (existing.is(state.block())) {
          // SlabBlock.getStateForPlacement: merging into a single slab drops
          // waterlogging (double slabs cannot be waterlogged).
          state = existing.with("type", "double").with("waterlogged", "false");
          isDoubleSlab = true;
        } else {
          const bool top =
              face == Direction::Down ||
              (face != Direction::Up && static_cast<double>(hitY) > 0.5);
          state = state.with("type", top ? "top" : "bottom");
        }
      } else if (state.block().isStairs()) {
        const bool top =
            face == Direction::Down ||
            (face != Direction::Up && static_cast<double>(hitY) > 0.5);
        state = state.with("facing", directionName(horizontal(yaw_)))
                     .with("half", top ? "top" : "bottom");
      } else if (isFenceGate(state)) {
        const bool powered = world_->hasNeighborSignal(target);
        state = state.with("facing", directionName(horizontal(yaw_)))
                     .with("open", powered ? "true" : "false")
                     .with("powered", powered ? "true" : "false");
        state = world_->updateFromNeighbourShapes(target, state);
      } else if (state.block().isRotatedPillar()) {
        // RotatedPillarBlock.getStateForPlacement uses the clicked face axis.
        state = state.with("axis", face == Direction::Up || face == Direction::Down
                                       ? "y"
                                       : face == Direction::East || face == Direction::West
                                             ? "x" : "z");
      } else if (isAnyChestBlock(state)) {
        state = chestPlacementState(*world_, target, state, horizontal(yaw_),
                                    face, sneaking_);
      } else if (state.is(Blocks::BARREL)) {
        state = barrelPlacementState(state, lookingDirections(yaw_, pitch_)[0]);
      } else if (state.is(Blocks::DISPENSER) || state.is(Blocks::DROPPER)) {
        state =
            dispenserPlacementState(state, lookingDirections(yaw_, pitch_)[0]);
      } else if (state.is(Blocks::HOPPER)) {
        state = hopperPlacementState(*world_, target, state, face);
      } else if (state.hasProperty("facing")) {
        const auto dir3d = lookingDirections(yaw_, pitch_)[0];
        const auto candidate =
            state.is(Blocks::OBSERVER) ? dir3d : opposite(dir3d);
        const auto *facingProp = state.block().stateDefinition().find("facing");
        std::uint32_t idx = 0;
        const auto finalDir =
            (facingProp && facingProp->indexOf(directionName(candidate), idx))
                ? candidate
                : (state.is(Blocks::OBSERVER) ? horizontal(yaw_)
                                              : opposite(horizontal(yaw_)));
        state = state.with("facing", directionName(finalDir));
      } else if (isRailBlock(state)) {
        const auto look = horizontal(yaw_);
        state = state.with("shape",
                           (look == Direction::East || look == Direction::West)
                               ? "east_west"
                               : "north_south");
      }
      if (state.block().isPressurePlate())
        valid = pressurePlateCanSurvive(*world_, target);
      if (isWoolCarpet(state))
        valid = !isAirState(world_->getBlock(target.relative(Direction::Down)));
      if (isRailBlock(state))
        valid = railCanSurvive(*world_, target, state);
      // getStateForPlacement initializes these neighbor-derived properties.
      // Do not call observer.updateShape speculatively: it schedules a pulse
      // before the observer even exists in the world.
      if (isWallBlock(state) || state.block().isStairs() ||
          state.block().isFence() || isIronBarsBlock(state) ||
          state.is(Blocks::GRASS_BLOCK) || state.is(Blocks::MYCELIUM) ||
          state.is(Blocks::PODZOL) || state.is(Blocks::NOTE_BLOCK))
        state = world_->updateFromNeighbourShapes(target, state);
      // H-01: getStateForPlacement reads the fluid of the cell being replaced,
      // and the two host families ask different questions. BaseRailBlock
      // .java:139-147 and WallBlock.java:112-131 test the exact WATER source;
      // BaseCoralPlantTypeBlock.java:57-62 (inherited by BaseCoralWallFanBlock)
      // tests FluidTags.WATER plus isFull(), so a falling water[level=8] still
      // waterlogs the fan. A full water source lands waterlogged either way.
      if (state.hasProperty("waterlogged") && !isDoubleSlab) {
        // BlockPlaceContext.getClickedPos() is the CELL BEING REPLACED, which
        // is `target`, not the face that was clicked on (BlockPlaceContext
        // .java:48-51). Water is replaceable, so the two coincide exactly for
        // the water case this rule cares about.
        const auto replacedFluid = fluidStateOf(world_->getBlock(target));
        const bool wet =
            isDeadCoralFan(state)
                ? (fluidHasTag(replacedFluid.getType(), FluidTag::Water) &&
                   replacedFluid.isFull())
                : replacedFluid.is(Fluids::water());
        state = state.with("waterlogged", wet ? "true" : "false");
      }
      if (state.is(Blocks::REDSTONE_LAMP))
        state = state.with("lit",
                           world_->hasNeighborSignal(target) ? "true" : "false");
      if (valid) {
        const Aabb player{position_.x - .3,  position_.y,
                          position_.z - .3,  position_.x + .3,
                          position_.y + 1.8, position_.z + .3};
        for (const auto &box : collisionShape(*world_, target, state)) {
          const Aabb placed{target.x + box.x0 / 16, target.y + box.y0 / 16,
                            target.z + box.z0 / 16, target.x + box.x1 / 16,
                            target.y + box.y1 / 16, target.z + box.z1 / 16};
          if (placed.intersects(player))
            valid = false;
          for (Entity *entity : world_->entitiesIn(placed))
            if (entity->typeName() == "minecraft:tnt" ||
                entity->typeName() == "minecraft:falling_block" ||
                entity->typeName() == "minecraft:minecart")
              valid = false;
        }
        if (valid && !isAirState(state)) {
          state = applyItemBlockStateComponent(held, state);
          world_->setBlock(target, state, 11);
          if (state.is(Blocks::REPEATER) || state.is(Blocks::COMPARATOR))
            diodeSetPlacedBy(*world_, target, state);
          if (!creativeMode) {
            ItemStack nextHeld = held;
            --nextHeld.count;
            setSlot(handSlot, nextHeld.count > 0 ? nextHeld : ItemStack{});
          }
        }
      }
      // TODO: other players' placement collision, exact Mth trigonometric
      // lookup ties, replaceable-by-item predicates, skull note overrides and
      // arbitrary items. Never force-load a chunk on placement.
    } else if (diagnostic)
      diagnostic("TODO: unsupported placement item");
  }
  sendBlockUpdate(clicked);
  sendBlockUpdate(target);
  acknowledge(sequence);
}

namespace {
struct PovHit {
  bool hit = false;
  Pos pos{};
  Direction face = Direction::Up;
};

void clipAabb(double minX, double minY, double minZ, double maxX, double maxY,
              double maxZ, Vec3 from, double dx, double dy, double dz,
              double &scale, std::optional<Direction> &dir) {
  auto clipPoint = [&](double da, double db, double dc, double point,
                       double minB, double maxB, double minC, double maxC,
                       Direction newDir, double fromA, double fromB,
                       double fromC) {
    const double s = (point - fromA) / da;
    const double pb = fromB + s * db;
    const double pc = fromC + s * dc;
    if (0.0 < s && s < scale && minB - 1.0e-7 < pb && pb < maxB + 1.0e-7 &&
        minC - 1.0e-7 < pc && pc < maxC + 1.0e-7) {
      scale = s;
      dir = newDir;
    }
  };
  if (dx > 1.0e-7)
    clipPoint(dx, dy, dz, minX, minY, maxY, minZ, maxZ, Direction::West, from.x,
              from.y, from.z);
  else if (dx < -1.0e-7)
    clipPoint(dx, dy, dz, maxX, minY, maxY, minZ, maxZ, Direction::East, from.x,
              from.y, from.z);
  if (dy > 1.0e-7)
    clipPoint(dy, dz, dx, minY, minZ, maxZ, minX, maxX, Direction::Down, from.y,
              from.z, from.x);
  else if (dy < -1.0e-7)
    clipPoint(dy, dz, dx, maxY, minZ, maxZ, minX, maxX, Direction::Up, from.y,
              from.z, from.x);
  if (dz > 1.0e-7)
    clipPoint(dz, dx, dy, minZ, minX, maxX, minY, maxY, Direction::North,
              from.z, from.x, from.y);
  else if (dz < -1.0e-7)
    clipPoint(dz, dx, dy, maxZ, minX, maxX, minY, maxY, Direction::South,
              from.z, from.x, from.y);
}

PovHit povRaycast(World &world, Vec3 from, Vec3 to, bool sourceFluids,
                  const std::function<bool(Pos)> &canVisit) {
  const double totalDx = to.x - from.x;
  const double totalDy = to.y - from.y;
  const double totalDz = to.z - from.z;
  if (totalDx * totalDx + totalDy * totalDy + totalDz * totalDz < 1.0e-12)
    return {};
  const auto testCell = [&](Pos pos) -> PovHit {
    if (!canVisit(pos))
      return {};
    const BlockState state = world.getBlock(pos);
    double bestScale = 1.0;
    std::optional<Direction> bestDir;
    for (const ShapeBox &b : outlineShape(state)) {
      clipAabb(pos.x + b.x0 / 16.0, pos.y + b.y0 / 16.0, pos.z + b.z0 / 16.0,
               pos.x + b.x1 / 16.0, pos.y + b.y1 / 16.0, pos.z + b.z1 / 16.0,
               from, totalDx, totalDy, totalDz, bestScale, bestDir);
    }
    if (sourceFluids) {
      const FluidState fluid = fluidStateOf(state);
      if (!fluid.isEmpty() && fluid.isSource()) {
        const float height = fluidHeight(world, pos, fluid);
        clipAabb(pos.x, pos.y, pos.z, pos.x + 1.0, pos.y + height, pos.z + 1.0,
                 from, totalDx, totalDy, totalDz, bestScale, bestDir);
      }
    }
    if (bestDir)
      return {true, pos, *bestDir};
    return {};
  };

  // BlockGetter.traverseBlocks (BlockGetter.java:119-167).
  const double toX = to.x - (-1.0e-7) * totalDx;
  const double toY = to.y - (-1.0e-7) * totalDy;
  const double toZ = to.z - (-1.0e-7) * totalDz;
  const double fromX = from.x + (-1.0e-7) * totalDx;
  const double fromY = from.y + (-1.0e-7) * totalDy;
  const double fromZ = from.z + (-1.0e-7) * totalDz;
  int cx = static_cast<int>(std::floor(fromX));
  int cy = static_cast<int>(std::floor(fromY));
  int cz = static_cast<int>(std::floor(fromZ));
  if (auto hit = testCell({cx, cy, cz}); hit.hit)
    return hit;

  const double dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
  const auto sign = [](double v) { return (v > 0.0) - (v < 0.0); };
  const auto frac = [](double v) { return v - std::floor(v); };
  const int sx = sign(dx), sy = sign(dy), sz = sign(dz);
  const double tDeltaX = sx == 0 ? 1.0e30 : sx / dx;
  const double tDeltaY = sy == 0 ? 1.0e30 : sy / dy;
  const double tDeltaZ = sz == 0 ? 1.0e30 : sz / dz;
  double tX = tDeltaX * (sx > 0 ? 1.0 - frac(fromX) : frac(fromX));
  double tY = tDeltaY * (sy > 0 ? 1.0 - frac(fromY) : frac(fromY));
  double tZ = tDeltaZ * (sz > 0 ? 1.0 - frac(fromZ) : frac(fromZ));

  for (int step = 0; step < 64 && (tX <= 1.0 || tY <= 1.0 || tZ <= 1.0); ++step) {
    if (tX < tY) {
      if (tX < tZ) {
        cx += sx;
        tX += tDeltaX;
      } else {
        cz += sz;
        tZ += tDeltaZ;
      }
    } else if (tY < tZ) {
      cy += sy;
      tY += tDeltaY;
    } else {
      cz += sz;
      tZ += tDeltaZ;
    }
    if (auto hit = testCell({cx, cy, cz}); hit.hit)
      return hit;
  }
  return {};
}
} // namespace

void Session::useItem(Reader &in) {
  const int hand = in.varInt();
  const int sequence = in.varInt();
  const float yaw = in.f32();
  const float pitch = in.f32();
  if (hand < 0 || hand > 1 || sequence < 0 || !in.empty() ||
      !std::isfinite(yaw) || !std::isfinite(pitch))
    throw ProtocolError("invalid item use");
  yaw_ = yaw;
  pitch_ = pitch;
  const int handSlot = hand ? 40 : selectedSlot_;
  const auto held = inventory_[handSlot];
  if (isBucketItem(held.id)) {
    const double yawRad = -yaw_ * (std::numbers::pi / 180.0);
    const double pitchRad = pitch_ * (std::numbers::pi / 180.0);
    const double cosPitch = std::cos(pitchRad);
    const Vec3 view{std::sin(yawRad) * cosPitch, -std::sin(pitchRad),
                    std::cos(yawRad) * cosPitch};
    const Vec3 eye = position_ + Vec3{0, sneaking_ ? 1.27 : 1.62, 0};
    // Player creative blockInteractionRange = 4.5 + 0.5 = 5.0.
    const Vec3 to = eye + view * 5.0;
    const auto hit = povRaycast(
        *world_, eye, to, held.id == "minecraft:bucket",
        [&](Pos p) { return world_->isLoaded(p) && hasChunk(p); });
    if (hit.hit && canInteract(hit.pos)) {
      if (held.id == "minecraft:bucket") {
        const BlockState state = world_->getBlock(hit.pos);
        std::string taken;
        if (state.block().isLiquidContainer() && state.block().isWaterloggedHost() &&
            waterloggedOf(state)) {
          world_->setBlock(hit.pos,
                          state.with(PropertyKind::Waterlogged, std::uint8_t{0}),
                          3);
          taken = "minecraft:water_bucket";
        } else if (isLiquidBlock(state)) {
          const FluidState fluid = fluidStateOf(state);
          if (fluid.isSource() && !fluid.isEmpty()) {
            taken = fluid.is(Fluids::lava()) ? "minecraft:lava_bucket"
                                              : "minecraft:water_bucket";
            world_->setBlock(hit.pos, airState(), 11);
          }
        }
        if (!taken.empty()) {
          bool present = inventory_[40].id == taken;
          for (int i = 0; i <= 35 && !present; ++i)
            present = inventory_[i].id == taken;
          if (!present) {
            for (int i = 0; i <= 35; ++i) {
              if (inventory_[i].empty()) {
                setSlot(i, itemStack(taken));
                break;
              }
            }
          }
          sendBlockUpdate(hit.pos);
        }
      } else {
        const bool isWater = held.id == "minecraft:water_bucket";
        const BlockState clickedState = world_->getBlock(hit.pos);
        const Pos initialPos = (isWater && clickedState.block().isLiquidContainer())
                                   ? hit.pos
                                   : hit.pos.relative(hit.face);
        const auto tryEmpty = [&](auto &self, Pos p, bool allowFallback) -> bool {
          if (!canInteract(p))
            return false;
          const BlockState s = world_->getBlock(p);
          const bool mayReplace = isReplaceable(s) || !blocksMotion(s);
          const bool canContainer =
              isWater && s.block().isLiquidContainer() &&
              canPlaceLiquid(s, Fluids::water());
          const bool canInside =
              s.isAir() || ((mayReplace || canContainer) && (!sneaking_ || !allowFallback));
          if (!canInside)
            return allowFallback && self(self, hit.pos.relative(hit.face), false);
          if (isWater && s.block().isLiquidContainer() &&
              s.block().isWaterloggedHost()) {
            placeLiquid(*world_, p, s, fluidStateOf(defaultState("water")));
            sendBlockUpdate(p);
            return true;
          }
          if (!s.isAir() && mayReplace && !isLiquidBlock(s))
            world_->updateOrDestroy(p, s, airState(), 3, 512);
          const BlockState fluidBlock =
              defaultState(isWater ? "water" : "lava");
          const bool ok = world_->setBlock(p, fluidBlock, 11) ||
                          fluidStateOf(s).isSource();
          if (ok)
            sendBlockUpdate(p);
          return ok;
        };
        tryEmpty(tryEmpty, initialPos, true);
        sendBlockUpdate(hit.pos);
      }
    }
  } else if (!held.empty() && !teleportPending_) {
    ItemUseParams params;
    params.eyePos = position_ + Vec3{0, sneaking_ ? 1.27 : 1.62, 0};
    params.yaw = yaw_;
    params.pitch = pitch_;
    params.creative = !playerEntity_ || playerEntity_->creative;
    params.sneaking = sneaking_;
    params.reach = 5.0;
    params.player = playerEntity_.get();
    ItemStack updated = held;
    const ItemUseResult res = useItemInAir(*world_, updated, params);
    if (res.consumesAction()) {
      if (updated != held)
        setSlot(handSlot, std::move(updated));
      for (Pos p : res.updatedBlocks)
        sendBlockUpdate(p);
    }
  }
  acknowledge(sequence);
}

namespace {
void skipHashedStack(Reader &in) {
  if (!in.bool_())
    return;
  (void)in.varInt(); // item id
  (void)in.varInt(); // count
  const int added = in.varInt();
  if (added < 0 || added > 256)
    throw ProtocolError("invalid hashed stack components");
  for (int i = 0; i < added; ++i) {
    (void)in.varInt();
    (void)in.i32();
  }
  const int removed = in.varInt();
  if (removed < 0 || removed > 256)
    throw ProtocolError("invalid hashed stack removals");
  for (int i = 0; i < removed; ++i)
    (void)in.varInt();
}

int containerMenuTypeId(ContainerKind kind, int slotCount) {
  switch (kind) {
  case ContainerKind::Chest:
  case ContainerKind::TrappedChest:
  case ContainerKind::Barrel:
    return slotCount > 27 ? 5 : 2; // generic_9x6 or generic_9x3
  case ContainerKind::Dispenser:
  case ContainerKind::Dropper:
    return 6; // generic_3x3
  case ContainerKind::Hopper:
    return 16; // hopper
  }
  return 2;
}

std::string containerTitle(ContainerKind kind, int slotCount) {
  switch (kind) {
  case ContainerKind::Chest:
    return slotCount > 27 ? "Large Chest" : "Chest";
  case ContainerKind::TrappedChest:
    return slotCount > 27 ? "Large Chest" : "Trapped Chest";
  case ContainerKind::Barrel:
    return "Barrel";
  case ContainerKind::Dispenser:
    return "Dispenser";
  case ContainerKind::Dropper:
    return "Dropper";
  case ContainerKind::Hopper:
    return "Item Hopper";
  }
  return "Container";
}

std::uint64_t viewModSignature(const ResolvedContainerView &view) {
  const std::uint64_t a = view.first ? view.first->modCount : 0;
  const std::uint64_t b = view.second ? view.second->modCount : 0;
  return a ^ (b * 0x9e3779b97f4a7c15ull);
}
} // namespace

void Session::sendContainerContent() {
  if (!openContainer_ || phase_ != Phase::Play)
    return;
  ResolvedContainerView view =
      resolveContainerView(*world_, openContainer_->pos, false);
  if (!view.valid() || view.size() != openContainer_->slotCount) {
    closeOpenContainer(true);
    return;
  }
  const int n = openContainer_->slotCount;
  Writer out;
  out.varInt(openContainer_->containerId);
  out.varInt(openContainer_->stateId);
  out.varInt(n + 36);
  for (int i = 0; i < n; ++i)
    stack(out, view.getItem(i));
  for (int i = 9; i <= 35; ++i)
    stack(out, inventory_[i]);
  for (int i = 0; i <= 8; ++i)
    stack(out, inventory_[i]);
  stack(out, carriedItem_);
  send("container_set_content", std::move(out));
  openContainer_->lastSeenModCount = viewModSignature(view);
}

void Session::openContainerScreen(Pos pos, const BlockState &state) {
  const auto kind = containerKindForState(state);
  if (!kind)
    return;
  ensureContainerBlockEntity(*world_, pos, state);
  ResolvedContainerView view = resolveContainerView(*world_, pos, false);
  if (!view.valid())
    return;
  if (openContainer_)
    closeOpenContainer(false);

  const int slotCount = view.size();
  const int menuId = containerMenuTypeId(*kind, slotCount);
  const int cid = nextContainerId_;
  nextContainerId_ = (nextContainerId_ % 100) + 1;

  OpenContainer opened;
  opened.containerId = cid;
  opened.menuTypeId = menuId;
  opened.pos = pos;
  opened.kind = *kind;
  opened.slotCount = slotCount;
  opened.stateId = 1;
  opened.lastSeenModCount = viewModSignature(view);
  openContainer_ = std::move(opened);

  containerUpdateOpenCount(*world_, pos, state, +1);

  Writer openOut;
  openOut.varInt(cid);
  openOut.varInt(menuId);
  nbtText(openOut, containerTitle(*kind, slotCount));
  send("open_screen", std::move(openOut));
  sendContainerContent();
}

void Session::closeOpenContainer(bool sendPacket) {
  if (!openContainer_)
    return;
  const int cid = openContainer_->containerId;
  const Pos pos = openContainer_->pos;
  openContainer_.reset();
  carriedItem_ = {};
  if (world_->isLoaded(pos)) {
    const BlockState state = world_->getBlock(pos);
    if (containerKindForState(state))
      containerUpdateOpenCount(*world_, pos, state, -1);
  }
  if (sendPacket && phase_ == Phase::Play) {
    Writer out;
    out.varInt(cid);
    send("container_close", std::move(out));
  }
  // Sync player hotbar/main inventory after closing a container menu.
  for (int i = 0; i <= 35; ++i)
    setSlot(i, inventory_[i]);
}

void Session::syncOpenContainer() {
  if (!openContainer_ || phase_ != Phase::Play)
    return;
  if (!world_->isLoaded(openContainer_->pos) || !hasChunk(openContainer_->pos)) {
    closeOpenContainer(true);
    return;
  }
  const BlockState state = world_->getBlock(openContainer_->pos);
  const auto kind = containerKindForState(state);
  if (!kind || *kind != openContainer_->kind) {
    closeOpenContainer(true);
    return;
  }
  const ResolvedContainerView view =
      resolveContainerView(*world_, openContainer_->pos, false);
  if (!view.valid() || view.size() != openContainer_->slotCount) {
    closeOpenContainer(true);
    return;
  }
  const std::uint64_t sig = viewModSignature(view);
  if (sig != openContainer_->lastSeenModCount) {
    ++openContainer_->stateId;
    sendContainerContent();
  }
}

void Session::containerClose(Reader &in) {
  const int cid = in.varInt();
  if (!in.empty())
    throw ProtocolError("invalid container_close");
  if (openContainer_ && cid == openContainer_->containerId)
    closeOpenContainer(false);
}

void Session::containerClick(Reader &in) {
  const int cid = in.varInt();
  (void)in.varInt(); // stateId
  const int slotNum = in.i16();
  const int buttonNum = static_cast<int>(static_cast<std::int8_t>(in.u8()));
  const int input = in.varInt();
  const int changedCount = in.varInt();
  if (changedCount < 0 || changedCount > 128)
    throw ProtocolError("invalid container_click slot count");
  for (int i = 0; i < changedCount; ++i) {
    (void)in.i16();
    skipHashedStack(in);
  }
  skipHashedStack(in); // carried item
  if (!in.empty())
    throw ProtocolError("trailing container_click data");

  if (!openContainer_ || cid != openContainer_->containerId)
    return;
  if (!world_->isLoaded(openContainer_->pos)) {
    closeOpenContainer(true);
    return;
  }
  ResolvedContainerView view =
      resolveContainerView(*world_, openContainer_->pos, false);
  if (!view.valid() || view.size() != openContainer_->slotCount) {
    closeOpenContainer(true);
    return;
  }

  const int n = openContainer_->slotCount;
  const int totalSlots = n + 36;
  bool containerMutated = false;

  auto getMenuSlot = [&](int menuSlot) -> ItemStack {
    if (menuSlot >= 0 && menuSlot < n)
      return view.getItem(menuSlot);
    if (menuSlot >= n && menuSlot < n + 27)
      return inventory_[9 + (menuSlot - n)];
    if (menuSlot >= n + 27 && menuSlot < totalSlots)
      return inventory_[menuSlot - (n + 27)];
    return {};
  };
  auto setMenuSlot = [&](int menuSlot, const ItemStack &val) {
    if (menuSlot >= 0 && menuSlot < n) {
      view.setItem(*world_, menuSlot, val);
      containerMutated = true;
    } else if (menuSlot >= n && menuSlot < n + 27) {
      inventory_[9 + (menuSlot - n)] = val;
    } else if (menuSlot >= n + 27 && menuSlot < totalSlots) {
      inventory_[menuSlot - (n + 27)] = val;
    }
  };
  auto canMerge = [](const ItemStack &a, const ItemStack &b) {
    return !a.empty() && !b.empty() && a.id == b.id &&
           a.count < a.maxStackSize();
  };
  auto insertRange = [&](ItemStack &moving, int startSlot, int endExclusive) {
    for (int s = startSlot; s < endExclusive && !moving.empty(); ++s) {
      ItemStack cur = getMenuSlot(s);
      if (canMerge(cur, moving)) {
        const int add = std::min(moving.count, cur.maxStackSize() - cur.count);
        if (add > 0) {
          cur.count += add;
          moving.count -= add;
          if (moving.count <= 0)
            moving = {};
          setMenuSlot(s, cur);
        }
      }
    }
    for (int s = startSlot; s < endExclusive && !moving.empty(); ++s) {
      if (getMenuSlot(s).empty()) {
        setMenuSlot(s, moving);
        moving = {};
      }
    }
  };

  if (input == 0) { // PICKUP
    if (slotNum == -999 && !carriedItem_.empty() &&
        world_->isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      const int dropCount = (buttonNum == 0) ? carriedItem_.count : 1;
      world_->spawnItemAt(position_ + Vec3{0, 1.3, 0}, carriedItem_.id,
                         dropCount);
      carriedItem_.count -= dropCount;
      if (carriedItem_.count <= 0)
        carriedItem_ = {};
    } else if (slotNum >= 0 && slotNum < totalSlots) {
      ItemStack slotStack = getMenuSlot(slotNum);
      if (buttonNum == 0) {
        if (carriedItem_.empty()) {
          carriedItem_ = slotStack;
          setMenuSlot(slotNum, {});
        } else if (slotStack.empty()) {
          setMenuSlot(slotNum, carriedItem_);
          carriedItem_ = {};
        } else if (canMerge(slotStack, carriedItem_)) {
          const int add =
              std::min(carriedItem_.count,
                       slotStack.maxStackSize() - slotStack.count);
          slotStack.count += add;
          carriedItem_.count -= add;
          if (carriedItem_.count <= 0)
            carriedItem_ = {};
          setMenuSlot(slotNum, slotStack);
        } else {
          setMenuSlot(slotNum, carriedItem_);
          carriedItem_ = slotStack;
        }
      } else if (buttonNum == 1) {
        if (carriedItem_.empty() && !slotStack.empty()) {
          const int take = (slotStack.count + 1) / 2;
          carriedItem_ = itemStack(slotStack.id, take);
          slotStack.count -= take;
          setMenuSlot(slotNum, slotStack.count > 0 ? slotStack : ItemStack{});
        } else if (!carriedItem_.empty()) {
          if (slotStack.empty()) {
            setMenuSlot(slotNum, itemStack(carriedItem_.id, 1));
            if (--carriedItem_.count <= 0)
              carriedItem_ = {};
          } else if (canMerge(slotStack, carriedItem_)) {
            slotStack.count += 1;
            setMenuSlot(slotNum, slotStack);
            if (--carriedItem_.count <= 0)
              carriedItem_ = {};
          } else {
            setMenuSlot(slotNum, carriedItem_);
            carriedItem_ = slotStack;
          }
        }
      }
    }
  } else if (input == 1 && slotNum >= 0 && slotNum < totalSlots) { // QUICK_MOVE
    ItemStack moving = getMenuSlot(slotNum);
    if (!moving.empty()) {
      if (slotNum < n) {
        insertRange(moving, n, totalSlots);
      } else {
        insertRange(moving, 0, n);
      }
      setMenuSlot(slotNum, moving);
    }
  } else if (input == 2 && slotNum >= 0 && slotNum < totalSlots) { // SWAP
    const int invIdx =
        (buttonNum >= 0 && buttonNum <= 8)
            ? buttonNum
            : (buttonNum == 40 ? 40 : -1);
    if (invIdx >= 0) {
      ItemStack a = getMenuSlot(slotNum);
      ItemStack b = inventory_[invIdx];
      inventory_[invIdx] = a;
      setMenuSlot(slotNum, b);
    }
  } else if (input == 3 && slotNum >= 0 && slotNum < totalSlots &&
             carriedItem_.empty()) { // CLONE
    ItemStack s = getMenuSlot(slotNum);
    if (!s.empty())
      carriedItem_ = itemStack(s.id, s.maxStackSize());
  } else if (input == 4 && slotNum >= 0 && slotNum < totalSlots &&
             carriedItem_.empty()) { // THROW
    ItemStack s = getMenuSlot(slotNum);
    if (!s.empty() &&
        world_->isChunkLoaded(chunkCoord(position_.x),
                             chunkCoord(position_.z))) {
      const int dropCount = (buttonNum == 1) ? s.count : 1;
      world_->spawnItemAt(position_ + Vec3{0, 1.3, 0}, s.id, dropCount);
      s.count -= dropCount;
      setMenuSlot(slotNum, s.count > 0 ? s : ItemStack{});
    }
  } else if (input == 5) { // QUICK_CRAFT
    const int header = buttonNum & 3;
    if (header == 0) {
      openContainer_->quickCraftMode = 1;
      openContainer_->quickCraftEvent = (buttonNum >> 2) & 3;
      openContainer_->quickCraftSlots.clear();
    } else if (header == 1 && openContainer_->quickCraftMode == 1 &&
               slotNum >= 0 && slotNum < totalSlots) {
      if (std::find(openContainer_->quickCraftSlots.begin(),
                    openContainer_->quickCraftSlots.end(),
                    slotNum) == openContainer_->quickCraftSlots.end())
        openContainer_->quickCraftSlots.push_back(slotNum);
    } else if (header == 2 && openContainer_->quickCraftMode == 1) {
      if (!carriedItem_.empty() && !openContainer_->quickCraftSlots.empty()) {
        std::vector<int> eligible;
        for (int s : openContainer_->quickCraftSlots) {
          ItemStack cur = getMenuSlot(s);
          if (cur.empty() || canMerge(cur, carriedItem_))
            eligible.push_back(s);
        }
        if (!eligible.empty()) {
          const int count = static_cast<int>(eligible.size());
          for (int s : eligible) {
            if (carriedItem_.empty())
              break;
            int add = 0;
            if (openContainer_->quickCraftEvent == 0)
              add = std::max(1, carriedItem_.count / count);
            else if (openContainer_->quickCraftEvent == 1)
              add = 1;
            else
              add = carriedItem_.maxStackSize();
            ItemStack cur = getMenuSlot(s);
            const int cap = cur.empty() ? carriedItem_.maxStackSize()
                                        : cur.maxStackSize() - cur.count;
            const int place =
                openContainer_->quickCraftEvent == 2
                    ? cap
                    : std::min({add, cap, carriedItem_.count});
            if (place > 0) {
              setMenuSlot(s, itemStack(carriedItem_.id,
                                       (cur.empty() ? 0 : cur.count) + place));
              if (openContainer_->quickCraftEvent != 2)
                carriedItem_.count -= place;
            }
          }
          if (openContainer_->quickCraftEvent == 2 || carriedItem_.count <= 0)
            carriedItem_ = {};
        }
      }
      openContainer_->quickCraftMode = -1;
      openContainer_->quickCraftSlots.clear();
    }
  } else if (input == 6 && !carriedItem_.empty()) { // PICKUP_ALL
    for (int pass = 0; pass < 2; ++pass) {
      for (int s = 0;
           s < totalSlots && carriedItem_.count < carriedItem_.maxStackSize();
           ++s) {
        ItemStack cur = getMenuSlot(s);
        if (cur.empty() || cur.id != carriedItem_.id)
          continue;
        if (pass == 0 && cur.count == cur.maxStackSize())
          continue;
        const int take = std::min(cur.count,
                                  carriedItem_.maxStackSize() - carriedItem_.count);
        carriedItem_.count += take;
        cur.count -= take;
        setMenuSlot(s, cur.count > 0 ? cur : ItemStack{});
      }
    }
  }

  if (containerMutated)
    view.setChanged(*world_);
  ++openContainer_->stateId;
  sendContainerContent();
}
} // namespace redstone::net
