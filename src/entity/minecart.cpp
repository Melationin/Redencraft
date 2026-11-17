#include "entity/internal.hpp"
#include "redstone/entity/vehicles.hpp"

namespace redstone {
Minecart::Minecart(World &world, Vec3 pos, std::string typeName)
    : VehicleEntity(std::move(typeName)) {
  entityData_.define(kDataCustomDisplayBlockId,
                     EntityDataSerializer::OptionalBlockState,
                     customDisplayBlock);
  entityData_.define(kDataDisplayOffsetId, EntityDataSerializer::Int,
                     displayOffset);
  setPos(pos);
  oldPosition = position;
  (void)world;
}
void Minecart::syncDataFields() {
  VehicleEntity::syncDataFields();
  entityData_.set(kDataCustomDisplayBlockId,
                  customDisplayBlock.has_value()
                      ? customDisplayBlock
                      : std::optional<BlockState>(defaultDisplayBlock()));
  entityData_.set(kDataDisplayOffsetId, displayOffset);
}
float Minecart::getBlockSpeedFactor(World &world) const {
  const Pos feet{static_cast<int>(std::floor(position.x)),
                 static_cast<int>(std::floor(position.y)),
                 static_cast<int>(std::floor(position.z))};
  const BlockState block = world.getBlock(feet);
  if (isRailBlock(block))
    return 1.0f;
  return Entity::getBlockSpeedFactor(world);
}
Vec3 Minecart::getPassengerAttachmentPoint(const Entity &,
                                           EntityDimensions dims,
                                           float scale) const {
  const bool flippedBox = false;
  (void)flippedBox;
  const double offset = static_cast<double>(dims.height) * 0.75 - 0.5;
  return {0.0, offset * static_cast<double>(scale), 0.0};
}
bool Minecart::ignoreExplosion(World &) const { return false; }
void Minecart::push(Entity &entity) {
  double xa = entity.position.x - position.x;
  double za = entity.position.z - position.z;
  double dd = xa * xa + za * za;
  if (dd >= 1.0e-4f) {
    dd = std::sqrt(dd);
    xa /= dd;
    za /= dd;
    double pow = 1.0 / dd;
    if (pow > 1)
      pow = 1;
    xa *= pow * 0.1f * 0.5;
    za *= pow * 0.1f * 0.5;
    if (auto *other = dynamic_cast<Minecart *>(&entity)) {
      double xo = other->position.x - position.x;
      double zo = other->position.z - position.z;
      Vec3 dir{xo, 0, zo};
      dir = dir.normalize();
      Vec3 facing{std::cos(yaw * std::numbers::pi / 180.0), 0,
                  std::sin(yaw * std::numbers::pi / 180.0)};
      facing = facing.normalize();
      double dotValue =
          std::abs(dir.x * facing.x + dir.z * facing.z);
      if (dotValue >= 0.8) {
        Vec3 movement = deltaMovement;
        Vec3 entityMovement = other->deltaMovement;
        double xdd = (entityMovement.x + movement.x) / 2.0;
        double zdd = (entityMovement.z + movement.z) / 2.0;
        deltaMovement = movement.multiply(0.2, 1.0, 0.2);
        push(xdd - xa, 0, zdd - za);
        other->deltaMovement = entityMovement.multiply(0.2, 1.0, 0.2);
        other->push(xdd + xa, 0, zdd + za);
      }
    } else {
      push(-xa, 0, -za);
      entity.push(xa / 4.0, 0, za / 4.0);
    }
  }
}
Pos Minecart::currentBlockPosOrRailBelow(World &world) const {
  int xt = static_cast<int>(std::floor(position.x));
  int yt = static_cast<int>(std::floor(position.y));
  int zt = static_cast<int>(std::floor(position.z));
  if (isRailBlock(world.getBlock({xt, yt - 1, zt})))
    --yt;
  return {xt, yt, zt};
}
double Minecart::maxSpeed(World &world) const {
  return isInWater(world) ? 0.2 : 0.4;
}
Vec3 Minecart::naturalSlowdown(World &world, Vec3 movement) const {
  double factor = 0.96;
  Vec3 result = movement.multiply(factor, 0.0, factor);
  if (isInWater(world))
    result = result * 0.95;
  return result;
}
void Minecart::tick(World &world) {
  tickHurtDecay();
  checkBelowWorld(world);
  handlePortal(world);
  if (world.minecartImprovements)
    tickNewBehavior(world);
  else
    tickOldBehavior(world);
  if (isRemoved())
    return;
  if (isInLava(world)) {
    igniteForSeconds(15.0f);
    lavaHurt(world);
    fallDistance *= 0.5;
  }
  firstTick = false;
}

void Minecart::makeStepAlongTrack(World &, Pos, RailShape, double &left) {
  left = 0.0;
}

void Minecart::tickNewBehavior(World &world) {
  deltaMovement.y -= isInWater(world) ? 0.005 : 0.04;
  constexpr double kExperimentalMaxSpeed = 1.0;
  int stepsLeft = 8;
  double movementLeft = deltaMovement.horizontalDistance();
  do {
    const Pos railPosBlock = currentBlockPosOrRailBelow(world);
    const BlockState state = world.getBlock(railPosBlock);
    onRails = isRailBlock(state);
    if (!onRails) {
      comeOffTrack(world);
      break;
    }
    fallDistance = 0.0;
    if (state.is(Blocks::ACTIVATOR_RAIL))
      activateMinecart(world, railPosBlock, poweredOf(state));
    const RailShape shape = railShapeOf(state);
    Vec3 vel = {deltaMovement.x, 0.0, deltaMovement.z};
    if (state.is(Blocks::POWERED_RAIL)) {
      if (poweredOf(state)) {
        const double h = vel.horizontalDistance();
        if (h > 0.01)
          vel += Vec3{vel.x / h * 0.06, 0.0, vel.z / h * 0.06};
        else {
          if (shape == RailShape::EastWest) {
            if (isRedstoneConductor(
                    world.getBlock(railPosBlock.relative(Direction::West))))
              vel.x = 0.02;
            else if (isRedstoneConductor(
                         world.getBlock(railPosBlock.relative(Direction::East))))
              vel.x = -0.02;
          } else if (shape == RailShape::NorthSouth) {
            if (isRedstoneConductor(
                    world.getBlock(railPosBlock.relative(Direction::North))))
              vel.z = 0.02;
            else if (isRedstoneConductor(world.getBlock(
                         railPosBlock.relative(Direction::South))))
              vel.z = -0.02;
          }
        }
      } else {
        const double h = vel.horizontalDistance();
        vel = h < 0.03 ? Vec3{} : vel * 0.5;
      }
    }
    vel = naturalSlowdown(world, vel);
    const double speed = vel.horizontalDistance();
    if (speed > kExperimentalMaxSpeed)
      vel = vel * (kExperimentalMaxSpeed / speed);
    deltaMovement = {vel.x, deltaMovement.y, vel.z};

    const double stepDist = std::min(vel.horizontalDistance(), 0.5);
    if (stepDist > 1.0e-7) {
      const Vec3 unit{vel.x / std::max(1.0e-9, vel.horizontalDistance()), 0.0,
                      vel.z / std::max(1.0e-9, vel.horizontalDistance())};
      move(world, MoverType::Self, unit * stepDist);
      movementLeft -= stepDist;
      const Pos curRail = currentBlockPosOrRailBelow(world);
      if (auto snapped = railPos(world, position.x, curRail.y, position.z))
        setPos(position.x, snapped->y, position.z);
      double tmpLeft = movementLeft;
      makeStepAlongTrack(world, curRail, shape, tmpLeft);
    } else {
      break;
    }
  } while (movementLeft > 1.0e-5 && --stepsLeft > 0 && isAlive());
  applyEffectsFromBlocks(world);
  pushAndPickupEntities(world);
}
void Minecart::tickOldBehavior(World &world) {
  deltaMovement.y -= isInWater(world) ? 0.005 : 0.04;
  Pos railPos = currentBlockPosOrRailBelow(world);
  auto state = world.getBlock(railPos);
  onRails = isRailBlock(state);
  if (onRails) {
    moveAlongTrack(world);
    if (state.is(Blocks::ACTIVATOR_RAIL))
      activateMinecart(world, railPos,
                       poweredOf(state));
  } else
    comeOffTrack(world);
  applyEffectsFromBlocks(world);
  pitch = 0;
  double xDiff = oldPosition.x - position.x;
  double zDiff = oldPosition.z - position.z;
  if (xDiff * xDiff + zDiff * zDiff > 0.001) {
    yaw = static_cast<float>(std::atan2(zDiff, xDiff) * 180.0 /
                             std::numbers::pi);
    if (flipped)
      yaw += 180.0f;
  }
  float rotDiff = wrapDegrees(static_cast<double>(yaw) - oldYaw);
  if (rotDiff < -170.0f || rotDiff >= 170.0f) {
    yaw += 180.0f;
    flipped = !flipped;
  }
  yaw = std::fmod(yaw, 360.0f);
  pitch = std::fmod(pitch, 360.0f);
  pushAndPickupEntities(world);
}
void Minecart::comeOffTrack(World &world) {
  double max = maxSpeed(world);
  Vec3 movement = deltaMovement;
  deltaMovement = {clamp(movement.x, -max, max), movement.y,
                   clamp(movement.z, -max, max)};
  if (onGround)
    deltaMovement = deltaMovement * 0.5;
  move(world, deltaMovement);
  if (!onGround)
    deltaMovement = deltaMovement * 0.95;
}
void Minecart::moveAlongTrack(World &world) {
  Pos pos = currentBlockPosOrRailBelow(world);
  auto state = world.getBlock(pos);
  fallDistance = 0;
  double x = position.x, y = position.y, z = position.z;
  auto oldPos = railPos(world, x, y, z);
  y = pos.y;
  bool powerTrack = false, haltTrack = false;
  if (state.is(Blocks::POWERED_RAIL)) {
    powerTrack = poweredOf(state);
    haltTrack = !powerTrack;
  }
  double slideSpeed = 0.0078125;
  if (isInWater(world))
    slideSpeed *= 0.2;
  RailShape shape = railShapeOf(state);
  auto addVelocity = [&](Vec3 v) { deltaMovement += v; };
  switch (shape) {
  case RailShape::AscendingEast:
    addVelocity({-slideSpeed, 0, 0});
    ++y;
    break;
  case RailShape::AscendingWest:
    addVelocity({slideSpeed, 0, 0});
    ++y;
    break;
  case RailShape::AscendingNorth:
    addVelocity({0, 0, slideSpeed});
    ++y;
    break;
  case RailShape::AscendingSouth:
    addVelocity({0, 0, -slideSpeed});
    ++y;
    break;
  default:
    break;
  }
  Vec3 movement = deltaMovement;
  auto exits = railExits(shape);
  Pos exit0 = exits.first, exit1 = exits.second;
  double xD = exit1.x - exit0.x, zD = exit1.z - exit0.z;
  double length = std::sqrt(xD * xD + zD * zD);
  double flip = movement.x * xD + movement.z * zD;
  if (flip < 0) {
    xD = -xD;
    zD = -zD;
  }
  double pow = std::min(2.0, movement.horizontalDistance());
  deltaMovement = {pow * xD / length, movement.y, pow * zD / length};
  if (haltTrack) {
    double speedLength = deltaMovement.horizontalDistance();
    if (speedLength < 0.03)
      deltaMovement = {};
    else
      deltaMovement = deltaMovement.multiply(0.5, 0.0, 0.5);
  }
  double x0 = pos.x + 0.5 + exit0.x * 0.5;
  double z0 = pos.z + 0.5 + exit0.z * 0.5;
  double x1 = pos.x + 0.5 + exit1.x * 0.5;
  double z1 = pos.z + 0.5 + exit1.z * 0.5;
  xD = x1 - x0;
  zD = z1 - z0;
  double progress;
  if (xD == 0)
    progress = z - pos.z;
  else if (zD == 0)
    progress = x - pos.x;
  else {
    double xx = x - x0, zz = z - z0;
    progress = (xx * xD + zz * zD) * 2.0;
  }
  x = x0 + xD * progress;
  z = z0 + zD * progress;
  setPos(x, y, z);
  double scale = 1.0;
  double max = maxSpeed(world);
  movement = deltaMovement;
  move(world, {clamp(scale * movement.x, -max, max), 0.0,
               clamp(scale * movement.z, -max, max)});
  if (exit0.y != 0 &&
      static_cast<int>(std::floor(position.x)) - pos.x == exit0.x &&
      static_cast<int>(std::floor(position.z)) - pos.z == exit0.z)
    setPos(position.x, position.y + exit0.y, position.z);
  else if (exit1.y != 0 &&
           static_cast<int>(std::floor(position.x)) - pos.x == exit1.x &&
           static_cast<int>(std::floor(position.z)) - pos.z == exit1.z)
    setPos(position.x, position.y + exit1.y, position.z);
  deltaMovement = naturalSlowdown(world, deltaMovement);
  auto newPos = railPos(world, position.x, position.y, position.z);
  if (newPos && oldPos) {
    double speed = (oldPos->y - newPos->y) * 0.05;
    double otherPow = deltaMovement.horizontalDistance();
    if (otherPow > 0)
      deltaMovement =
          deltaMovement.multiply((otherPow + speed) / otherPow, 1.0,
                                 (otherPow + speed) / otherPow);
    setPos(position.x, newPos->y, position.z);
  }
  int xn = static_cast<int>(std::floor(position.x));
  int zn = static_cast<int>(std::floor(position.z));
  if (xn != pos.x || zn != pos.z) {
    double otherPow = deltaMovement.horizontalDistance();
    deltaMovement = {otherPow * (xn - pos.x), deltaMovement.y,
                     otherPow * (zn - pos.z)};
  }
  if (powerTrack) {
    double speedLength = deltaMovement.horizontalDistance();
    if (speedLength > 0.01)
      deltaMovement +=
          Vec3{deltaMovement.x / speedLength * 0.06, 0,
               deltaMovement.z / speedLength * 0.06};
    else {
      double dx = deltaMovement.x, dz = deltaMovement.z;
      if (shape == RailShape::EastWest) {
        if (isRedstoneConductor(world.getBlock(pos.relative(Direction::West))))
          dx = 0.02;
        else if (isRedstoneConductor(world.getBlock(pos.relative(Direction::East))))
          dx = -0.02;
      } else if (shape == RailShape::NorthSouth) {
        if (isRedstoneConductor(world.getBlock(pos.relative(Direction::North))))
          dz = 0.02;
        else if (isRedstoneConductor(world.getBlock(pos.relative(Direction::South))))
          dz = -0.02;
      }
      deltaMovement = {dx, deltaMovement.y, dz};
    }
  }
}
std::optional<Vec3> Minecart::railPos(World &world, double x, double y,
                                      double z) const {
  int xt = static_cast<int>(std::floor(x));
  int yt = static_cast<int>(std::floor(y));
  int zt = static_cast<int>(std::floor(z));
  if (isRailBlock(world.getBlock({xt, yt - 1, zt})))
    --yt;
  auto state = world.getBlock({xt, yt, zt});
  if (!isRailBlock(state))
    return std::nullopt;
  RailShape shape = railShapeOf(state);
  auto exits = railExits(shape);
  Pos exit0 = exits.first, exit1 = exits.second;
  double x0 = xt + 0.5 + exit0.x * 0.5;
  double y0 = yt + 0.0625 + exit0.y * 0.5;
  double z0 = zt + 0.5 + exit0.z * 0.5;
  double x1 = xt + 0.5 + exit1.x * 0.5;
  double y1 = yt + 0.0625 + exit1.y * 0.5;
  double z1 = zt + 0.5 + exit1.z * 0.5;
  double xD = x1 - x0;
  double yD = (y1 - y0) * 2.0;
  double zD = z1 - z0;
  double progress;
  if (xD == 0)
    progress = z - zt;
  else if (zD == 0)
    progress = x - xt;
  else {
    double xx = x - x0, zz = z - z0;
    progress = (xx * xD + zz * zD) * 2.0;
  }
  x = x0 + xD * progress;
  y = y0 + yD * progress;
  z = z0 + zD * progress;
  if (yD < 0)
    y += 1.0;
  else if (yD > 0)
    y += 0.5;
  return Vec3{x, y, z};
}
void Minecart::activateMinecart(World &, Pos, bool powered) {
  if (powered && hurtTime == 0) {
    hurtDir = -hurtDir;
    hurtTime = 10;
    damage = 50.0f;
  }
}
void Minecart::pushAndPickupEntities(World &world) {
  Aabb hitbox = boundingBox().inflate(0.2f, 0.0, 0.2f);
  if (deltaMovement.horizontalDistanceSqr() >= 0.01) {
    const std::uint8_t mask =
        static_cast<std::uint8_t>(EntitySectionFlag::Pushable) |
        static_cast<std::uint8_t>(EntitySectionFlag::Minecart);
    world.forEachEntityInFlags(hitbox, mask, [&](Entity &entity) {
      if (&entity != this && (entity.isMinecart() || entity.isPushable()))
        entity.push(*this);
    });
  } else {
    world.forEachEntityInFlags(
        hitbox, static_cast<std::uint8_t>(EntitySectionFlag::Pushable),
        [&](Entity &entity) {
          if (&entity != this && entity.isPushable())
            entity.push(*this);
        });
  }
}

AbstractMinecartContainer::AbstractMinecartContainer(World &world, Vec3 pos,
                                                     std::string typeName,
                                                     int size)
    : Minecart(world, pos, std::move(typeName)), items_(size) {}

const ItemStack &AbstractMinecartContainer::getContainerItem(int slot) const {
  static const ItemStack kEmpty;
  if (slot < 0 || slot >= static_cast<int>(items_.size()))
    return kEmpty;
  return items_[static_cast<std::size_t>(slot)];
}

void AbstractMinecartContainer::setContainerItem(int slot, ItemStack stack) {
  if (slot >= 0 && slot < static_cast<int>(items_.size()))
    items_[static_cast<std::size_t>(slot)] = std::move(stack);
}

ItemStack AbstractMinecartContainer::removeContainerItem(int slot, int count) {
  if (slot < 0 || slot >= static_cast<int>(items_.size()) || count <= 0)
    return {};
  auto &s = items_[static_cast<std::size_t>(slot)];
  if (s.empty())
    return {};
  const int take = std::min(count, s.count);
  ItemStack out = s;
  out.count = take;
  s.count -= take;
  if (s.count <= 0)
    s = {};
  return out;
}

void AbstractMinecartContainer::setContainerChanged(World &) {}

Vec3 AbstractMinecartContainer::naturalSlowdown(World &world,
                                                Vec3 movement) const {
  int freeSlots = 0;
  for (const auto &s : items_)
    if (s.empty())
      ++freeSlots;
  const int size = std::max(1, static_cast<int>(items_.size()));
  const double fillRatio = 1.0 - static_cast<double>(freeSlots) / size;
  const double factor = 0.98 + (1.0 - fillRatio) * 0.01;
  Vec3 result = movement.multiply(factor, 0.0, factor);
  if (isInWater(world))
    result = result * 0.95;
  return result;
}

void AbstractMinecartContainer::onDestroyDrop(World &world) {
  Minecart::onDestroyDrop(world);
  for (const auto &s : items_)
    if (!s.empty())
      spawnAtLocation(world, s);
}

MinecartChest::MinecartChest(World &world, Vec3 pos)
    : AbstractMinecartContainer(world, pos, "minecraft:chest_minecart", 27) {}

MinecartHopper::MinecartHopper(World &world, Vec3 pos)
    : AbstractMinecartContainer(world, pos, "minecraft:hopper_minecart", 5) {}

void MinecartHopper::activateMinecart(World &, Pos, bool powered) {
  enabled = !powered;
}

void MinecartHopper::makeStepAlongTrack(World &world, Pos, RailShape,
                                        double &) {
  tryConsumeItems(world);
}

void MinecartHopper::tick(World &world) {
  consumedItemThisFrame_ = false;
  Minecart::tick(world);
  tryConsumeItems(world);
}

void MinecartHopper::tryConsumeItems(World &world) {
  if (!isRemoved() && enabled && !consumedItemThisFrame_ &&
      suckInItems(world)) {
    consumedItemThisFrame_ = true;
  }
}

bool MinecartHopper::suckInItems(World &world) {
  if (isContainerFull())
    return false;
  const Pos abovePos{static_cast<int>(std::floor(position.x)),
                     static_cast<int>(std::floor(position.y + 1.0 + 0.5)),
                     static_cast<int>(std::floor(position.z))};
  if (auto container = world.containerEntity(abovePos)) {
    for (int i = 0; i < container->containerSize(); ++i) {
      const ItemStack &s = container->getItem(i);
      if (s.empty())
        continue;
      ItemStack single = container->removeItem(i, 1);
      ItemStack rem = addItemToContainer(world, single);
      if (rem.empty()) {
        container->setChanged(world);
        return true;
      }
      container->setItem(i, s);
    }
    return false;
  }
  const Aabb suckBox = boundingBox().inflate(0.25, 0.0, 0.25);
  bool sucked = false;
  world.forEachEntityInFlags(
      suckBox, static_cast<std::uint8_t>(EntitySectionFlag::Item),
      [&](Entity &e) -> bool {
        if (!e.isItemEntity() || e.isRemoved())
          return true;
        auto &itemEnt = static_cast<ItemEntity &>(e);
        if (itemEnt.item.empty())
          return true;
        ItemStack rem = addItemToContainer(world, itemEnt.item);
        if (rem.count != itemEnt.item.count) {
          if (rem.empty())
            itemEnt.discard();
          else
            itemEnt.item = rem;
          sucked = true;
          return false;
        }
        return true;
      });
  return sucked;
}

MinecartTNT::MinecartTNT(World &world, Vec3 pos)
    : Minecart(world, pos, "minecraft:tnt_minecart") {}

void MinecartTNT::primeFuse() {
  fuse = 80;
}

void MinecartTNT::activateMinecart(World &, Pos, bool powered) {
  if (powered && fuse < 0)
    primeFuse();
}

void MinecartTNT::explode(World &world, double speedSqr) {
  if (!world.tntExplodes) {
    if (isPrimed())
      discard();
    return;
  }
  const double clampedSpeed = std::min(std::sqrt(speedSqr), 5.0);
  const float power =
      explosionPowerBase +
      explosionSpeedFactor *
          static_cast<float>(world.random.nextDouble() * 1.5 * clampedSpeed);
  // Rail protection matches MinecartTNT.getBlockExplosionResistance when primed.
  if (fuse < 0)
    fuse = 0;
  discard();
  world.explode(this, position, power, false);
}

void MinecartTNT::tick(World &world) {
  Minecart::tick(world);
  if (fuse > 0) {
    --fuse;
  } else if (fuse == 0) {
    explode(world, deltaMovement.horizontalDistanceSqr());
    return;
  }
  if (horizontalCollision) {
    const double speedSqr = deltaMovement.horizontalDistanceSqr();
    if (speedSqr >= 0.01)
      explode(world, speedSqr);
  }
}

bool MinecartTNT::hurt(World &world, DamageKind kind, float amount) {
  if (kind == DamageKind::Fire || kind == DamageKind::Lava) {
    if (fuse < 0)
      fuse = world.random.nextInt(20) + world.random.nextInt(20);
  } else if (kind == DamageKind::Explosion) {
    explode(world, deltaMovement.horizontalDistanceSqr());
    return true;
  }
  return Minecart::hurt(world, kind, amount);
}

bool MinecartTNT::causeFallDamage(World &world, double fallDist, float,
                                  DamageKind) {
  if (fallDist >= 3.0) {
    const double power = fallDist / 10.0;
    explode(world, power * power);
  }
  return false;
}

MinecartFurnace::MinecartFurnace(World &world, Vec3 pos)
    : Minecart(world, pos, "minecraft:furnace_minecart") {
  entityData_.define(kDataFuelId, EntityDataSerializer::Boolean, false);
}

void MinecartFurnace::syncDataFields() {
  Minecart::syncDataFields();
  entityData_.set(kDataFuelId, fuel > 0);
}

void MinecartFurnace::tick(World &world) {
  Minecart::tick(world);
  if (fuel > 0)
    --fuel;
  if (fuel <= 0)
    pushForce = {};
  syncDataFields();
}

double MinecartFurnace::maxSpeed(World &world) const {
  return isInWater(world) ? Minecart::maxSpeed(world) * 0.75
                          : Minecart::maxSpeed(world) * 0.5;
}

Vec3 MinecartFurnace::naturalSlowdown(World &world, Vec3 movement) const {
  Vec3 next;
  if (pushForce.lengthSqr() > 1.0e-7) {
    next = movement.multiply(0.8, 0.0, 0.8) + pushForce;
    if (isInWater(world))
      next = next * 0.1;
  } else {
    next = movement.multiply(0.98, 0.0, 0.98);
  }
  return Minecart::naturalSlowdown(world, next);
}

bool MinecartFurnace::addFuel(Vec3 interactingPos, const std::string &itemId) {
  if ((itemId == "minecraft:coal" || itemId == "minecraft:charcoal") &&
      fuel + 3600 <= 32000) {
    fuel += 3600;
    if (fuel > 0) {
      const Vec3 d = position - interactingPos;
      pushForce = {d.x, 0.0, d.z};
    }
    syncDataFields();
    return true;
  }
  return false;
}
} // namespace redstone
