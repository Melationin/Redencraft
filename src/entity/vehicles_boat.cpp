#include "entity/internal.hpp"
#include "redstone/entity/vehicles.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace redstone {

int ContainerEntity::comparatorSignal() const {
  const int size = containerSize();
  if (size <= 0)
    return 0;
  float total = 0.0f;
  bool any = false;
  for (int i = 0; i < size; ++i) {
    const ItemStack &s = getContainerItem(i);
    if (!s.empty()) {
      any = true;
      total += static_cast<float>(s.count) /
               static_cast<float>(std::max(1, s.maxStackSize()));
    }
  }
  const float avg = total / static_cast<float>(size);
  return static_cast<int>(std::floor(avg * 14.0f)) + (any ? 1 : 0);
}

bool ContainerEntity::isContainerEmpty() const {
  for (int i = 0; i < containerSize(); ++i)
    if (!getContainerItem(i).empty())
      return false;
  return true;
}

bool ContainerEntity::isContainerFull() const {
  for (int i = 0; i < containerSize(); ++i) {
    const ItemStack &s = getContainerItem(i);
    if (s.empty() || s.count < s.maxStackSize())
      return false;
  }
  return true;
}

ItemStack ContainerEntity::addItemToContainer(World &world, ItemStack stack) {
  if (stack.empty())
    return stack;
  const int size = containerSize();
  const int maxStack = std::max(1, stack.maxStackSize());
  for (int i = 0; i < size && stack.count > 0; ++i) {
    const ItemStack &cur = getContainerItem(i);
    if (!cur.empty() && cur.id == stack.id && cur.count < maxStack) {
      const int move = std::min(stack.count, maxStack - cur.count);
      ItemStack updated = cur;
      updated.count += move;
      setContainerItem(i, std::move(updated));
      stack.count -= move;
    }
  }
  for (int i = 0; i < size && stack.count > 0; ++i) {
    if (getContainerItem(i).empty()) {
      const int move = std::min(stack.count, maxStack);
      ItemStack placed = stack;
      placed.count = move;
      setContainerItem(i, std::move(placed));
      stack.count -= move;
    }
  }
  setContainerChanged(world);
  return stack;
}

VehicleEntity::VehicleEntity(std::string typeName) : Entity(std::move(typeName)) {
  entityData_.define(kDataHurtId, EntityDataSerializer::Int, hurtTime);
  entityData_.define(kDataHurtDirId, EntityDataSerializer::Int, hurtDir);
  entityData_.define(kDataDamageId, EntityDataSerializer::Float, damage);
}

void VehicleEntity::syncDataFields() {
  entityData_.set(kDataHurtId, hurtTime);
  entityData_.set(kDataHurtDirId, hurtDir);
  entityData_.set(kDataDamageId, damage);
}

void VehicleEntity::tickHurtDecay() {
  if (hurtTime > 0)
    --hurtTime;
  if (damage > 0.0f)
    damage = std::max(0.0f, damage - 1.0f);
}

void VehicleEntity::onDestroyDrop(World &world) {
  const std::string item = dropItemId();
  if (!item.empty())
    spawnAtLocation(world, itemStack(item, 1));
}

bool VehicleEntity::hurt(World &world, DamageKind, float amount) {
  if (isRemoved())
    return true;
  hurtDir = -hurtDir;
  hurtTime = 10;
  damage += amount * 10.0f;
  if (damage > 40.0f) {
    ejectPassengers();
    onDestroyDrop(world);
    discard();
  }
  return true;
}

AbstractBoat::AbstractBoat(World &world, Vec3 pos, std::string typeName,
                           std::string dropItem)
    : VehicleEntity(std::move(typeName)), dropItemId_(std::move(dropItem)) {
  (void)world;
  blocksBuilding = true;
  entityData_.define(kDataPaddleLeftId, EntityDataSerializer::Boolean, false);
  entityData_.define(kDataPaddleRightId, EntityDataSerializer::Boolean, false);
  entityData_.define(kDataBubbleTimeId, EntityDataSerializer::Int, 0);
  setPos(pos);
  oldPosition = pos;
}

void AbstractBoat::syncDataFields() {
  VehicleEntity::syncDataFields();
  entityData_.set(kDataPaddleLeftId, paddleLeft);
  entityData_.set(kDataPaddleRightId, paddleRight);
  entityData_.set(kDataBubbleTimeId, bubbleTime);
}

void AbstractBoat::setPaddleState(bool left, bool right) {
  paddleLeft = left;
  paddleRight = right;
  syncDataFields();
}

Vec3 AbstractBoat::getPassengerAttachmentPoint(const Entity &passenger,
                                               EntityDimensions,
                                               float scale) const {
  (void)scale;
  float offset = 0.0f;
  if (passengers_.size() > 1) {
    int idx = 0;
    for (std::size_t i = 0; i < passengers_.size(); ++i) {
      if (passengers_[i].lock().get() == &passenger) {
        idx = static_cast<int>(i);
        break;
      }
    }
    offset = (idx == 0) ? 0.2f : -0.6f;
  }
  const double rad = static_cast<double>(yaw) * std::numbers::pi / 180.0;
  return {-std::sin(rad) * offset,
          typeName().ends_with("_raft") ? 0.25 : 0.0,
          std::cos(rad) * offset};
}

AbstractBoat::Status AbstractBoat::computeStatus(World &world) const {
  const Aabb box = boundingBox();
  const int x0 = static_cast<int>(std::floor(box.minX));
  const int x1 = static_cast<int>(std::ceil(box.maxX));
  const int y1 = static_cast<int>(std::ceil(box.maxY + 0.001));
  const int y0 = static_cast<int>(std::floor(box.maxY));
  const int z0 = static_cast<int>(std::floor(box.minZ));
  const int z1 = static_cast<int>(std::ceil(box.maxZ));

  bool under = false;
  for (int x = x0; x < x1; ++x)
    for (int y = y0; y < y1; ++y)
      for (int z = z0; z < z1; ++z) {
        const Pos p{x, y, z};
        const FluidState fs = resolveEntityFluidState(world.getBlock(p));
        if (entity_internal::isWaterFluidState(fs) &&
            box.maxY + 0.001 < y + redstone::fluidHeight(world, p, fs)) {
          if (!fs.isSource())
            return Status::UnderFlowingWater;
          under = true;
        }
      }
  if (under)
    return Status::UnderWater;

  const int minY = static_cast<int>(std::floor(box.minY));
  const int checkMaxY = static_cast<int>(std::ceil(box.minY + 0.001));
  bool inWater = false;
  for (int x = x0; x < x1; ++x)
    for (int y = minY; y < checkMaxY; ++y)
      for (int z = z0; z < z1; ++z) {
        const Pos p{x, y, z};
        const FluidState fs = resolveEntityFluidState(world.getBlock(p));
        if (entity_internal::isWaterFluidState(fs)) {
          const double h = y + redstone::fluidHeight(world, p, fs);
          if (box.minY < h)
            inWater = true;
        }
      }
  if (inWater)
    return Status::InWater;
  return onGround ? Status::OnLand : Status::InAir;
}

double AbstractBoat::getWaterLevelAbove(World &world) const {
  const Aabb box = boundingBox();
  const int x0 = static_cast<int>(std::floor(box.minX));
  const int x1 = static_cast<int>(std::ceil(box.maxX));
  const int y0 = static_cast<int>(std::floor(box.maxY));
  const int y1 = static_cast<int>(std::floor(box.maxY + 1.0));
  const int z0 = static_cast<int>(std::floor(box.minZ));
  const int z1 = static_cast<int>(std::ceil(box.maxZ));
  double maxWater = box.minY;
  for (int y = y0; y < y1; ++y) {
    bool foundWater = false;
    for (int x = x0; x < x1; ++x)
      for (int z = z0; z < z1; ++z) {
        const Pos p{x, y, z};
        const FluidState fs = resolveEntityFluidState(world.getBlock(p));
        if (entity_internal::isWaterFluidState(fs)) {
          foundWater = true;
          maxWater = std::max(
              maxWater,
              static_cast<double>(y) +
                  static_cast<double>(redstone::fluidHeight(world, p, fs)));
        }
      }
    if (!foundWater)
      break;
  }
  return maxWater;
}

float AbstractBoat::getGroundFriction(World &world) const {
  const Aabb box = boundingBox();
  const Pos below{static_cast<int>(std::floor((box.minX + box.maxX) * 0.5)),
                  static_cast<int>(std::floor(box.minY - 0.001)),
                  static_cast<int>(std::floor((box.minZ + box.maxZ) * 0.5))};
  const BlockState s = world.getBlock(below);
  if (s.isAir())
    return 0.9f;
  return s.block().metadata().friction;
}

void AbstractBoat::floatBoat(World &world) {
  constexpr double kGravity = 0.04;
  double vspeed = -kGravity;
  double buoyancy = 0.0;
  float invFriction = 0.05f;
  if (oldStatus_ == Status::InAir && status_ != Status::InAir &&
      status_ != Status::OnLand) {
    waterLevel_ = getWaterLevelAbove(world);
    const double targetY = waterLevel_ - dimensions().height + 0.101;
    if (world.noCollision(this, boundingBox().move(0.0, targetY - position.y, 0.0))) {
      setPos(position.x, targetY, position.z);
      deltaMovement.y = 0.0;
    }
    status_ = Status::InWater;
  } else {
    if (status_ == Status::InWater) {
      waterLevel_ = getWaterLevelAbove(world);
      buoyancy = (waterLevel_ - position.y) / dimensions().height;
      invFriction = 0.9f;
    } else if (status_ == Status::UnderFlowingWater) {
      vspeed = -7.0e-4;
      invFriction = 0.9f;
    } else if (status_ == Status::UnderWater) {
      buoyancy = 0.01;
      invFriction = 0.45f;
    } else if (status_ == Status::InAir) {
      invFriction = 0.9f;
    } else if (status_ == Status::OnLand) {
      invFriction = landFriction_;
    }
    deltaMovement = {deltaMovement.x * invFriction, deltaMovement.y + vspeed,
                     deltaMovement.z * invFriction};
    deltaRotation_ *= invFriction;
    if (buoyancy > 0.0) {
      deltaMovement.y =
          (deltaMovement.y + buoyancy * (kGravity / 0.65)) * 0.75;
    }
  }
}

void AbstractBoat::controlBoat() {
  if (passengers_.empty())
    return;
  float acceleration = 0.0f;
  if (inputLeft)
    deltaRotation_ -= 1.0f;
  if (inputRight)
    deltaRotation_ += 1.0f;
  if (inputRight != inputLeft && !inputUp && !inputDown)
    acceleration += 0.005f;
  yaw += deltaRotation_;
  if (inputUp)
    acceleration += 0.04f;
  if (inputDown)
    acceleration -= 0.005f;
  const double rad = static_cast<double>(yaw) * std::numbers::pi / 180.0;
  deltaMovement.x += std::sin(-rad) * acceleration;
  deltaMovement.z += std::cos(rad) * acceleration;
  setPaddleState((inputRight && !inputLeft) || inputUp,
                 (inputLeft && !inputRight) || inputUp);
}

void AbstractBoat::tick(World &world) {
  oldStatus_ = status_;
  status_ = computeStatus(world);
  if (status_ == Status::OnLand)
    landFriction_ = getGroundFriction(world);
  if (status_ != Status::UnderWater && status_ != Status::UnderFlowingWater)
    outOfControlTicks_ = 0.0f;
  else
    outOfControlTicks_ += 1.0f;
  if (outOfControlTicks_ >= 60.0f)
    ejectPassengers();
  tickHurtDecay();
  baseTick(world);
  floatBoat(world);
  controlBoat();
  move(world, MoverType::Self, deltaMovement);
  applyEffectsFromBlocks(world);
  const Aabb check = boundingBox().inflate(0.2, -0.01, 0.2);
  world.forEachEntityInFlags(
      check, static_cast<std::uint8_t>(EntitySectionFlag::Pushable),
      [&](Entity &other) {
        if (&other != this && !other.isRemoved() && other.isPushable())
          push(other);
      });
}

Boat::Boat(World &world, Vec3 pos, std::string typeName, std::string dropItem)
    : AbstractBoat(world, pos, std::move(typeName), std::move(dropItem)) {}

ChestBoat::ChestBoat(World &world, Vec3 pos, std::string typeName,
                     std::string dropItem)
    : AbstractBoat(world, pos, std::move(typeName), std::move(dropItem)),
      items_(27) {}

const ItemStack &ChestBoat::getContainerItem(int slot) const {
  static const ItemStack kEmpty;
  if (slot < 0 || slot >= 27)
    return kEmpty;
  return items_[static_cast<std::size_t>(slot)];
}

void ChestBoat::setContainerItem(int slot, ItemStack stack) {
  if (slot >= 0 && slot < 27)
    items_[static_cast<std::size_t>(slot)] = std::move(stack);
}

ItemStack ChestBoat::removeContainerItem(int slot, int count) {
  if (slot < 0 || slot >= 27 || count <= 0)
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

void ChestBoat::setContainerChanged(World &) {}

void ChestBoat::onDestroyDrop(World &world) {
  AbstractBoat::onDestroyDrop(world);
  for (const auto &s : items_)
    if (!s.empty())
      spawnAtLocation(world, s);
}

} // namespace redstone
