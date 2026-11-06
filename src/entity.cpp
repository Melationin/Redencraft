#include "redstone/entity.hpp"
#include "redstone/shapes.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>

namespace redstone {
namespace {
constexpr double EPS = 1.0e-7;
// Mth.floor: truncate-and-adjust, avoiding a libm floor call in the
// explosion ray loop (coordinates stay well inside the int range).
inline int floorToInt(double value) {
  const int truncated = static_cast<int>(value);
  return truncated - (value < static_cast<double>(truncated) ? 1 : 0);
}
constexpr double COLLISION_EPS = 1.0e-5;
std::string canonical(std::string n) {
  if (n.find(':') == std::string::npos)
    n = "minecraft:" + n;
  return n;
}
bool air(const BlockState &s) {
  return s.isAir();
}
const VoxelShape &shapeAt(World &world, Pos pos, const BlockState &state,
                          VoxelShape &scratch) {
  if (!state.is(Blocks::MOVING_PISTON))
    return collisionShape(state);
  scratch = collisionShape(world, pos, state);
  return scratch;
}
bool fullCube(World &world, Pos pos) {
  const auto state = world.getBlock(pos);
  if (state.isAir())
    return false;
  VoxelShape scratch;
  const auto &shape = shapeAt(world, pos, state, scratch);
  if (shape.empty())
    return false;
  for (auto &box : shape)
    if (!(box.x0 <= 0 && box.y0 <= 0 && box.z0 <= 0 && box.x1 >= 16 &&
          box.y1 >= 16 && box.z1 >= 16))
      return false;
  return true;
}
double boxMin(const ShapeBox &b, int axis) {
  return axis == 0 ? b.x0 : axis == 1 ? b.y0 : b.z0;
}
double boxMax(const ShapeBox &b, int axis) {
  return axis == 0 ? b.x1 : axis == 1 ? b.y1 : b.z1;
}
double aabbMin(const Aabb &b, int axis) {
  return axis == 0 ? b.minX : axis == 1 ? b.minY : b.minZ;
}
double aabbMax(const Aabb &b, int axis) {
  return axis == 0 ? b.maxX : axis == 1 ? b.maxY : b.maxZ;
}
bool overlapsOtherAxes(const Aabb &box, const ShapeBox &shape, int axis) {
  for (int other = 0; other < 3; ++other) {
    if (other == axis)
      continue;
    if (!(aabbMax(box, other) > boxMin(shape, other) + EPS &&
          aabbMin(box, other) < boxMax(shape, other) - EPS))
      return false;
  }
  return true;
}
double collideAxis(int axis, const Aabb &box,
                   const std::vector<ShapeBox> &colliders, double distance) {
  for (const auto &part : colliders) {
    if (std::abs(distance) < EPS)
      return 0;
    if (!overlapsOtherAxes(box, part, axis))
      continue;
    if (distance > 0) {
      if (boxMin(part, axis) < aabbMax(box, axis) - EPS)
        continue;
      distance = std::min(distance, boxMin(part, axis) - aabbMax(box, axis));
    } else {
      if (boxMax(part, axis) > aabbMin(box, axis) + EPS)
        continue;
      distance = std::max(distance, boxMax(part, axis) - aabbMin(box, axis));
    }
  }
  return distance;
}
bool rayBox(Vec3 from, Vec3 to, const ShapeBox &box) {
  double tmin = 0, tmax = 1;
  const double lo[3] = {box.x0, box.y0, box.z0};
  const double hi[3] = {box.x1, box.y1, box.z1};
  const double start[3] = {from.x, from.y, from.z};
  const double delta[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
  for (int axis = 0; axis < 3; ++axis) {
    if (std::abs(delta[axis]) < EPS) {
      if (start[axis] < lo[axis] || start[axis] > hi[axis])
        return false;
      continue;
    }
    double a = (lo[axis] - start[axis]) / delta[axis];
    double b = (hi[axis] - start[axis]) / delta[axis];
    if (a > b)
      std::swap(a, b);
    tmin = std::max(tmin, a);
    tmax = std::min(tmax, b);
    if (tmin > tmax)
      return false;
  }
  return tmax >= tmin;
}
double clamp(double value, double min, double max) {
  return std::max(min, std::min(max, value));
}
float wrapDegrees(double value) {
  double result = std::fmod(value + 180.0, 360.0);
  if (result < 0)
    result += 360.0;
  return static_cast<float>(result - 180.0);
}
bool isFullSupport(World &world, Pos pos) {
  const auto state = world.getBlock(pos);
  if (state.isAir() || isLiquidBlock(state) || isRailBlock(state))
    return false;
  VoxelShape scratch;
  const auto &shape = shapeAt(world, pos, state, scratch);
  for (auto &box : shape)
    if (box.x0 <= 0 && box.y0 <= 0 && box.z0 <= 0 && box.x1 >= 16 &&
        box.y1 >= 16 && box.z1 >= 16)
      return true;
  return false;
}
// Block.isFaceSturdy(... SupportType.RIGID) rejects rails/fluids and blocks
// without a full top face. The wall's top face is only a 4..12 post, so rails
// cannot sit on it.
bool canSupportRail(World &world, Pos pos) {
  return isFullSupport(world, pos);
}
bool canSurvive(World &world, Pos pos, const BlockState &state) {
  if (isWoolCarpet(state))
    return !air(world.getBlock(pos.relative(Direction::Down)));
  if (isDeadCoralFan(state))
    return coralCanSurvive(world, pos, state);
  if (state.is(Blocks::PISTON_HEAD)) {
    auto d = directionFromString(state.get("facing", "north"));
    auto base = world.getBlock(pos.relative(opposite(d)));
    return base.is(Blocks::PISTON) || base.is(Blocks::STICKY_PISTON) ||
           (base.is(Blocks::MOVING_PISTON) &&
            base.get("facing", "north") == state.get("facing", "north"));
  }
  if (isRailBlock(state))
    return railCanSurvive(world, pos, state);
  return true;
}
bool isFree(const BlockState &state) {
  return air(state) || isReplaceable(state);
}
// Reused query buffer: Level.noCollision does not allocate per call either.
std::vector<ShapeBox> &scratchBoxes() {
  thread_local std::vector<ShapeBox> boxes;
  return boxes;
}
// SectionPos.blockToSectionCoord
int blockToSection(double coordinate) {
  return static_cast<int>(std::floor(coordinate)) >> 4;
}
// SectionPos.asLong(x, y, z): x in the top 26 bits, z next 26, y lowest 12.
std::uint64_t sectionPosKey(int x, int y, int z) {
  return (static_cast<std::uint64_t>(x) & 0x3FFFFFFull) << 38 |
         (static_cast<std::uint64_t>(z) & 0x3FFFFFFull) << 12 |
         (static_cast<std::uint64_t>(y) & 0xFFFull);
}
int sectionKeyX(std::uint64_t key) {
  return (static_cast<int>(key >> 38) & 0x3FFFFFF) << 6 >> 6;
}
int sectionKeyY(std::uint64_t key) {
  return (static_cast<int>(key) & 0xFFF) << 20 >> 20;
}
int sectionKeyZ(std::uint64_t key) {
  return (static_cast<int>(key >> 12) & 0x3FFFFFF) << 6 >> 6;
}
std::uint64_t entitySectionKey(const Vec3 &position) {
  return sectionPosKey(blockToSection(position.x),
                       blockToSection(position.y),
                       blockToSection(position.z));
}
// EntitySectionStorage.forEachAccessibleNonEmptySection: the XZ search area is
// grown by 2 blocks and downward by 4 so entities whose bounding box crosses a
// section border are still visited (CHONKY_ENTITY_SEARCH_GRACE = 2).
template <class F>
void forEachEntitySection(
    const std::map<std::uint64_t, std::vector<Entity *>> &sections,
    const Aabb &box, F &&visit) {
  if (sections.empty())
    return;
  const int xMin = blockToSection(box.minX - 2.0);
  const int yMin = blockToSection(box.minY - 4.0);
  const int zMin = blockToSection(box.minZ - 2.0);
  const int xMax = blockToSection(box.maxX + 2.0);
  const int yMax = blockToSection(box.maxY);
  const int zMax = blockToSection(box.maxZ + 2.0);
  for (int x = xMin; x <= xMax; ++x) {
    const auto lowest = sectionPosKey(x, 0, 0);
    const auto highest = sectionPosKey(x, -1, -1) + 1;
    for (auto it = sections.lower_bound(lowest);
         it != sections.end() && it->first < highest; ++it) {
      const int y = sectionKeyY(it->first);
      const int z = sectionKeyZ(it->first);
      if (y < yMin || y > yMax || z < zMin || z > zMax)
        continue;
      visit(it->second);
    }
  }
}
} // namespace

int ItemStack::maxStackSize() const {
  if (auto *entry = itemType(id))
    return entry->maxStackSize;
  return 64;
}
ItemStack itemStack(std::string id, int count) {
  if (id.empty() || count <= 0)
    return {};
  return {canonical(std::move(id)), count};
}
std::string blockItem(const BlockState &state) {
  const auto name = state.name();
  if (name == "minecraft:air" || name == "minecraft:cave_air" ||
      name == "minecraft:void_air" || name == "minecraft:water" ||
      name == "minecraft:lava" || name == "minecraft:moving_piston" ||
      name == "minecraft:piston_head")
    return {};
  if (isDeadCoralWallFan(state)) {
    auto item = name;
    auto pos = item.rfind("_wall_fan");
    if (pos != std::string::npos)
      item.replace(pos, 9, "_fan");
    return item;
  }
  return name;
}
std::vector<ItemStack> blockDrops(const BlockState &state) {
  const auto name = state.name();
  if (name == "minecraft:air" || name == "minecraft:cave_air" ||
      name == "minecraft:void_air" || name == "minecraft:water" ||
      name == "minecraft:lava" || name == "minecraft:moving_piston" ||
      name == "minecraft:piston_head" || name == "minecraft:bedrock")
    return {};
  if (name == "minecraft:glass" || isDeadCoralFan(state))
    return {}; // Silk Touch only; the empty tool used here never has it.
  if (name == "minecraft:tnt" &&
      state.get("unstable", "false") == "true")
    return {};
  if (name == "minecraft:grass_block")
    return {itemStack("minecraft:dirt", 1)};
  auto item = blockItem(state);
  return item.empty() ? std::vector<ItemStack>{}
                      : std::vector<ItemStack>{itemStack(item, 1)};
}
bool isReplaceable(const BlockState &state) {
  return air(state) || state.is(Blocks::WATER) || state.is(Blocks::LAVA);
}
bool isAirState(const BlockState &state) { return air(state); }

Entity::Entity(std::string type) : type_(entityType(type)) {
  if (!type_)
    throw std::invalid_argument("unregistered entity type: " + type);
}
bool Entity::fireImmune() const { return type_->fireImmune; }
bool Entity::hurt(World &, DamageKind, float) { return false; }
bool Entity::ignoreExplosion(World &) const { return false; }
Aabb Entity::boundingBox() const {
  return Aabb::ofSize(position, type_->width, type_->height);
}
double Entity::distanceToSqr(const Vec3 &to) const {
  Vec3 d = position - to;
  return d.lengthSqr();
}
void Entity::setPos(double x, double y, double z) { position = {x, y, z}; }
void Entity::kill(World &) { discard(); }
void Entity::checkBelowWorld(World &world) {
  if (position.y < world.minY() - 64.0)
    discard();
}
void Entity::push(double x, double y, double z) {
  if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z))
    deltaMovement += Vec3{x, y, z};
}
void Entity::push(const Vec3 &value) { push(value.x, value.y, value.z); }
void Entity::push(Entity &entity) {
  if (!entity.noPhysics && !noPhysics) {
    double xa = entity.position.x - position.x;
    double za = entity.position.z - position.z;
    double dd = std::max(std::abs(xa), std::abs(za));
    if (dd >= 0.01f) {
      dd = std::sqrt(dd);
      xa /= dd;
      za /= dd;
      double pow = 1.0 / dd;
      if (pow > 1)
        pow = 1;
      xa *= pow * 0.05f;
      za *= pow * 0.05f;
      if (isPushable())
        push(-xa, 0, -za);
      if (entity.isPushable())
        entity.push(xa, 0, za);
    }
  }
}
void Entity::igniteForTicks(int ticks) {
  if (remainingFireTicks < ticks)
    remainingFireTicks = ticks;
}
void Entity::igniteForSeconds(float seconds) {
  igniteForTicks(static_cast<int>(std::floor(seconds * 20.0f)));
}
void Entity::clearFire() { remainingFireTicks = std::min(0, remainingFireTicks); }
void Entity::lavaHurt(World &world) {
  if (fireImmune())
    return;
  igniteForSeconds(15.0f);
  hurt(world, DamageKind::Lava, 4.0f);
}
void Entity::baseTick(World &world) {
  if (remainingFireTicks > 0) {
    if (fireImmune())
      clearFire();
    else {
      if (remainingFireTicks % 20 == 0 && !isInLava(world))
        hurt(world, DamageKind::Fire, 1.0f);
      --remainingFireTicks;
    }
  }
  if (isInLava(world))
    fallDistance *= 0.5f;
  checkBelowWorld(world);
}
Vec3 Entity::limitPistonMovement(World &world, Vec3 movement) {
  if (movement.lengthSqr() <= 1.0e-7)
    return movement;
  std::int64_t time = world.gameTime();
  if (time != pistonDeltasGameTime_) {
    pistonDeltas_.fill(0.0);
    pistonDeltasGameTime_ = time;
  }
  auto restrict = [&](int axis, double amount) {
    double next =
        std::clamp(amount + pistonDeltas_[axis], -0.51, 0.51);
    amount = next - pistonDeltas_[axis];
    pistonDeltas_[axis] = next;
    return amount;
  };
  if (movement.x != 0.0) {
    double x = restrict(0, movement.x);
    return std::abs(x) <= 1.0e-5 ? Vec3{} : Vec3{x, 0, 0};
  }
  if (movement.y != 0.0) {
    double y = restrict(1, movement.y);
    return std::abs(y) <= 1.0e-5 ? Vec3{} : Vec3{0, y, 0};
  }
  if (movement.z != 0.0) {
    double z = restrict(2, movement.z);
    return std::abs(z) <= 1.0e-5 ? Vec3{} : Vec3{0, 0, z};
  }
  return {};
}
void Entity::move(World &world, Vec3 movement, bool piston) {
  if (noPhysics) {
    setPos(position + movement);
    horizontalCollision = false;
    verticalCollision = false;
    verticalCollisionBelow = false;
    return;
  }
  if (piston) {
    movement = limitPistonMovement(world, movement);
    if (movement.x == 0 && movement.y == 0 && movement.z == 0)
      return;
  }
  Aabb box = boundingBox();
  std::vector<ShapeBox> colliders;
  world.collectCollisionBoxes(box.expandTowards(movement), colliders);
  Vec3 resolved;
  auto step = [&](int axis, double value) {
    if (value == 0)
      return;
    double result = collideAxis(axis, box.move(resolved.x, resolved.y,
                                               resolved.z),
                                colliders, value);
    if (axis == 0)
      resolved.x = result;
    else if (axis == 1)
      resolved.y = result;
    else
      resolved.z = result;
  };
  // Direction.axisStepOrder: Y first, then the larger horizontal component.
  step(1, movement.y);
  if (std::abs(movement.x) < std::abs(movement.z)) {
    step(2, movement.z);
    step(0, movement.x);
  } else {
    step(0, movement.x);
    step(2, movement.z);
  }
  if (resolved.lengthSqr() > EPS ||
      (movement - resolved).lengthSqr() < EPS)
    setPos(position + resolved);
  bool xCollision = std::abs(movement.x - resolved.x) > COLLISION_EPS;
  bool zCollision = std::abs(movement.z - resolved.z) > COLLISION_EPS;
  horizontalCollision = xCollision || zCollision;
  verticalCollision = std::abs(movement.y - resolved.y) > COLLISION_EPS;
  verticalCollisionBelow = verticalCollision && movement.y < 0;
  onGround = verticalCollisionBelow;
  if (horizontalCollision) {
    if (xCollision)
      deltaMovement.x = 0;
    if (zCollision)
      deltaMovement.z = 0;
  }
  if (verticalCollision && movement.y != resolved.y) {
    auto effectPos = Pos{static_cast<int>(std::floor(position.x)),
                         static_cast<int>(std::floor(position.y - 0.2)),
                         static_cast<int>(std::floor(position.z))};
    auto effectState = world.getBlock(effectPos);
    if (effectState.is(Blocks::SLIME_BLOCK)) {
      if (deltaMovement.y < 0)
        deltaMovement.y = -deltaMovement.y * 0.8;
    } else
      deltaMovement.y = 0;
  }
  Pos feet{static_cast<int>(std::floor(position.x)),
           static_cast<int>(std::floor(position.y)),
           static_cast<int>(std::floor(position.z))};
  auto speedState = world.getBlock(feet);
  float speedFactor = blockMetadata(speedState).speedFactor;
  // TODO: track Entity.mainSupportingBlockPos for entities standing across a
  // block edge; getOnPos(offset) can use that support's X/Z rather than the
  // entity center. The offset overrides themselves are preserved below.
  // AbstractMinecart.getBlockSpeedFactor returns 1 on rails instead of
  // consulting the supporting block.
  if (!(typeName() == "minecraft:minecart" && isRailBlock(speedState)) &&
      !speedState.is(Blocks::WATER))
    speedFactor = speedFactor == 1.0f
                      ? blockMetadata(world.getBlock(
                            Pos{feet.x,
                                static_cast<int>(std::floor(
                                    position.y - (typeName() == "minecraft:item"
                                                      ? 0.999999f
                                                      : 0.500001f))),
                                feet.z}))
                            .speedFactor
                      : speedFactor;
  deltaMovement.x *= speedFactor;
  deltaMovement.z *= speedFactor;
}
void Entity::moveTowardsClosestSpace(World &world, double x, double y,
                                     double z) {
  Pos pos{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)),
          static_cast<int>(std::floor(z))};
  Vec3 delta{x - pos.x, y - pos.y, z - pos.z};
  Direction closestDirection = Direction::Up;
  double closest = std::numeric_limits<double>::max();
  for (auto direction : {Direction::North, Direction::South, Direction::West,
                         Direction::East, Direction::Up}) {
    auto neighbor = pos.relative(direction);
    if (!world.isLoaded(neighbor) || !fullCube(world, neighbor)) {
      int axis = direction == Direction::North || direction == Direction::South
                     ? 2
                     : direction == Direction::West || direction == Direction::East
                           ? 0
                           : 1;
      double d = axis == 0 ? delta.x : axis == 1 ? delta.y : delta.z;
      bool positive = direction == Direction::South ||
                      direction == Direction::East || direction == Direction::Up;
      double oriented = positive ? 1.0 - d : d;
      if (oriented < closest) {
        closest = oriented;
        closestDirection = direction;
      }
    }
  }
  double speed = world.random.nextFloat() * 0.2 + 0.1;
  Vec3 scaled = deltaMovement * 0.75;
  if (closestDirection == Direction::West)
    deltaMovement = {-speed, scaled.y, scaled.z};
  else if (closestDirection == Direction::East)
    deltaMovement = {speed, scaled.y, scaled.z};
  else if (closestDirection == Direction::Down)
    deltaMovement = {scaled.x, -speed, scaled.z};
  else if (closestDirection == Direction::Up)
    deltaMovement = {scaled.x, speed, scaled.z};
  else if (closestDirection == Direction::North)
    deltaMovement = {scaled.x, scaled.y, -speed};
  else
    deltaMovement = {scaled.x, scaled.y, speed};
}
double Entity::fluidHeight(World &world, bool lava) const {
  Aabb box = boundingBox().deflate(0.001);
  int x0 = static_cast<int>(std::floor(box.minX)),
      y0 = static_cast<int>(std::floor(box.minY)),
      z0 = static_cast<int>(std::floor(box.minZ));
  int x1 = static_cast<int>(std::ceil(box.maxX)) - 1,
      y1 = static_cast<int>(std::ceil(box.maxY)) - 1,
      z1 = static_cast<int>(std::ceil(box.maxZ)) - 1;
  double height = 0;
  for (int x = x0; x <= x1; ++x)
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z) {
        auto state = world.getBlock({x, y, z});
        bool water =
            state.is(Blocks::WATER) ||
            state.get("waterlogged", "false") == "true";
        bool lavaState = state.is(Blocks::LAVA);
        if ((lava && lavaState) || (!lava && water)) {
          int level = 0;
          try {
            level = std::stoi(state.get("level", "0"));
          } catch (...) {
            level = 0;
          }
          // FlowingFluid.getAmount: source (level 0) has 8/9 height, flowing
          // level N has (8-N)/9. A same fluid above raises it to a full block;
          // that detail only matters for tall columns and is left to the
          // fluid-tick TODO.
          double amount = level == 0 ? 8.0 : std::clamp(8.0 - level, 0.0, 8.0);
          double top = y + amount / 9.0;
          if (box.minY <= top)
            height = std::max(height, top - box.minY);
        }
      }
  return height;
}
bool Entity::isInWater(World &world) const {
  return fluidHeight(world, false) > 0;
}
bool Entity::isInLava(World &world) const {
  return fluidHeight(world, true) > 0;
}
// TODO: block entityInside effects other than lava and detector rails (fire,
// powder snow, cobweb, honey slide), step sounds/game events, and movement-path
// intersection checks. Only the current AABB is queried, which matches the old
// minecart behavior but not every Entity.checkInsideBlocks caller.
void Entity::applyEffectsFromBlocks(World &world) {
  if (isRemoved() || noPhysics ||
      !world.isChunkLoaded(static_cast<int>(std::floor(position.x / 16)),
                           static_cast<int>(std::floor(position.z / 16))))
    return;
  if (onGround) {
    auto effectPos = Pos{static_cast<int>(std::floor(position.x)),
                         static_cast<int>(std::floor(position.y - 0.2)),
                         static_cast<int>(std::floor(position.z))};
    auto effectState = world.getBlock(effectPos);
    if (effectState.is(Blocks::SLIME_BLOCK) &&
        std::abs(deltaMovement.y) < 0.1) {
      double scale = 0.4 + std::abs(deltaMovement.y) * 0.2;
      deltaMovement.x *= scale;
      deltaMovement.z *= scale;
    }
  }
  Aabb box = boundingBox().deflate(1.0e-5);
  int x0 = static_cast<int>(std::floor(box.minX)),
      y0 = static_cast<int>(std::floor(box.minY)),
      z0 = static_cast<int>(std::floor(box.minZ));
  int x1 = static_cast<int>(std::ceil(box.maxX)) - 1,
      y1 = static_cast<int>(std::ceil(box.maxY)) - 1,
      z1 = static_cast<int>(std::ceil(box.maxZ)) - 1;
  for (int x = x0; x <= x1; ++x)
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z) {
        Pos pos{x, y, z};
        auto state = world.getBlock(pos);
        if (state.is(Blocks::LAVA)) {
          lavaHurt(world);
          if (isRemoved())
            return;
        }
        if (state.is(Blocks::DETECTOR_RAIL))
          railEntityInside(world, pos, state, *this);
      }
}
void Entity::spawnAtLocation(World &world, const ItemStack &stack) {
  if (!stack.empty())
    world.spawnItemAt(position, stack.id, stack.count);
}

ItemEntity::ItemEntity(World &world, Vec3 pos, ItemStack stack)
    : ItemEntity(world, pos, std::move(stack),
                 Vec3{world.random.nextDouble() * 0.2 - 0.1, 0.2,
                      world.random.nextDouble() * 0.2 - 0.1}) {}
ItemEntity::ItemEntity(World &world, Vec3 pos, ItemStack stack, Vec3 motion)
    : Entity("minecraft:item"), item(std::move(stack)) {
  setPos(pos);
  setDeltaMovement(motion);
  yaw = world.random.nextFloat() * 360.0f;
  bobOffs = world.random.nextFloat() * static_cast<float>(std::numbers::pi) *
            2.0f;
  oldPosition = position;
}
void ItemEntity::setFluidMovement(double multiplier) {
  deltaMovement.x *= multiplier;
  deltaMovement.y += deltaMovement.y < 0.06 ? 5.0e-4 : 0.0;
  deltaMovement.z *= multiplier;
}
bool ItemEntity::isMergable() const {
  return !isRemoved() && pickupDelay != 32767 && age != -32768 && age < 6000 &&
         item.count < item.maxStackSize();
}
bool ItemEntity::fireImmune() const { return Entity::fireImmune(); }
bool ItemEntity::hurt(World &, DamageKind, float damage) {
  if (isRemoved() || item.empty())
    return false;
  health = static_cast<int>(health - damage);
  if (health <= 0)
    discard();
  return true;
}
void ItemEntity::tryToMerge(World &world, ItemEntity &other) {
  if (other.isMergable() && item.id == other.item.id &&
      item.count + other.item.count <= other.item.maxStackSize()) {
    ItemEntity *to;
    ItemEntity *from;
    if (other.item.count < item.count) {
      to = this;
      from = &other;
    } else {
      to = &other;
      from = this;
    }
    int max = to->item.maxStackSize();
    int delta = std::min(std::min(max, 64) - to->item.count, from->item.count);
    to->item.count += delta;
    from->item.count -= delta;
    to->pickupDelay = std::max(to->pickupDelay, from->pickupDelay);
    to->age = std::min(to->age, from->age);
    if (from->item.count <= 0)
      from->discard();
  }
  (void)world;
}
void ItemEntity::mergeWithNeighbours(World &world) {
  if (!isMergable())
    return;
  auto box = boundingBox().inflate(0.5, 0.0, 0.5);
  for (Entity *entity : world.entitiesIn(box)) {
    auto *other = dynamic_cast<ItemEntity *>(entity);
    if (!other || other == this || !other->isMergable())
      continue;
    tryToMerge(world, *other);
    if (isRemoved())
      break;
  }
}
// TODO: ItemEntity.playerTouch pickup, Thrower/Owner tracking, ItemStack
// components, and item.onDestroyed effects require a player/inventory model.
void ItemEntity::tick(World &world) {
  if (item.empty()) {
    discard();
    return;
  }
  baseTick(world);
  if (isRemoved())
    return;
  if (pickupDelay > 0 && pickupDelay != 32767)
    --pickupDelay;
  oldPosition = position;
  if (isInWater(world) && fluidHeight(world, false) > 0.1)
    setFluidMovement(0.99);
  else if (isInLava(world) && fluidHeight(world, true) > 0.1)
    setFluidMovement(0.95);
  else
    deltaMovement.y -= 0.04;
  if (isRemoved())
    return;
  noPhysics = !world.noCollision(boundingBox().deflate(1.0e-7));
  if (noPhysics)
    moveTowardsClosestSpace(
        world, position.x, (boundingBox().minY + boundingBox().maxY) / 2.0,
        position.z);
  if (onGround && deltaMovement.horizontalDistanceSqr() <= 1.0e-5 &&
      (tickCount + id) % 4 != 0) {
    applyEffectsFromBlocks(world);
  } else {
    move(world, deltaMovement);
    applyEffectsFromBlocks(world);
    if (isRemoved())
      return;
    double friction = 0.98;
    if (onGround) {
      auto below = Pos{static_cast<int>(std::floor(position.x)),
                       // ItemEntity overrides the movement-support offset;
                       // carpet must use its underlying block's friction.
                       static_cast<int>(std::floor(position.y - 0.999999f)),
                       static_cast<int>(std::floor(position.z))};
      friction = blockMetadata(world.getBlock(below)).friction * 0.98;
    }
    deltaMovement = deltaMovement.multiply(friction, 0.98, friction);
    if (onGround && deltaMovement.y < 0)
      deltaMovement.y *= -0.5;
  }
  bool moved = static_cast<int>(std::floor(oldPosition.x)) !=
                   static_cast<int>(std::floor(position.x)) ||
               static_cast<int>(std::floor(oldPosition.y)) !=
                   static_cast<int>(std::floor(position.y)) ||
               static_cast<int>(std::floor(oldPosition.z)) !=
                   static_cast<int>(std::floor(position.z));
  int rate = moved ? 2 : 40;
  if (tickCount % rate == 0 && isMergable())
    mergeWithNeighbours(world);
  if (age != -32768)
    ++age;
  if (age >= 6000)
    discard();
}

PrimedTnt::PrimedTnt(World &world, Vec3 pos) : Entity("minecraft:tnt") {
  setPos(pos);
  double rotation = world.random.nextDouble() * std::numbers::pi * 2.0;
  setDeltaMovement(-std::sin(rotation) * 0.02, 0.2,
                   -std::cos(rotation) * 0.02);
  oldPosition = position;
}
void PrimedTnt::tick(World &world) {
  deltaMovement.y -= 0.04;
  move(world, deltaMovement);
  applyEffectsFromBlocks(world);
  deltaMovement = deltaMovement * 0.98;
  if (onGround)
    deltaMovement = deltaMovement.multiply(0.7, -0.5, 0.7);
  --fuse;
  if (fuse <= 0) {
    discard();
    if (world.tntExplodes)
      // Entity.getY(0.0625) scales by the TNT's bounding-box height.
      world.explode(this, {position.x, position.y + type().height * 0.0625, position.z},
                    explosionPower, false);
  }
}

FallingBlockEntity::FallingBlockEntity(World &world, Vec3 pos,
                                       BlockState state)
    : Entity("minecraft:falling_block"), blockState(std::move(state)) {
  setPos(pos);
  oldPosition = position;
  startPos = {static_cast<int>(std::floor(pos.x)),
              static_cast<int>(std::floor(pos.y)),
              static_cast<int>(std::floor(pos.z))};
  (void)world;
}
FallingBlockEntity *FallingBlockEntity::fall(World &world, Pos pos,
                                             const BlockState &state) {
  auto entity = std::make_shared<FallingBlockEntity>(
      world, Vec3{pos.x + 0.5, double(pos.y), pos.z + 0.5}, state);
  world.setBlock(pos, airState(), 3);
  world.addEntity(entity);
  world.log.push_back({"entity_spawn", pos,
                       "minecraft:falling_block:" + state.name()});
  return entity.get();
}
// TODO: concrete powder water handling, anvil damage/fall damage, TileEntityData
// merging, portals, ENTITY_DROPS gamerule (currently vanilla default true),
// and Fallable callbacks beyond SandBlock's inherited no-op hooks.
void FallingBlockEntity::tick(World &world) {
  if (isAirState(blockState)) {
    discard();
    return;
  }
  ++time;
  deltaMovement.y -= 0.04;
  move(world, deltaMovement);
  applyEffectsFromBlocks(world);
  if (isRemoved())
    return;
  Pos pos{static_cast<int>(std::floor(position.x)),
          static_cast<int>(std::floor(position.y)),
          static_cast<int>(std::floor(position.z))};
  if (!onGround) {
    if ((time > 100 && (pos.y <= world.minY() || pos.y >= world.maxY())) ||
        time > 600) {
      if (dropItem)
        spawnAtLocation(world, itemStack(blockItem(blockState), 1));
      discard();
    }
  } else {
    auto current = world.getBlock(pos);
    deltaMovement = deltaMovement.multiply(0.7, -0.5, 0.7);
    if (!current.is(Blocks::MOVING_PISTON)) {
      if (!cancelDrop) {
        bool mayReplace = isReplaceable(current);
        bool wouldContinueFalling =
            isFree(world.getBlock(pos.relative(Direction::Down)));
        bool wouldSurvive =
            canSurvive(world, pos, blockState) && !wouldContinueFalling;
        if (mayReplace && wouldSurvive) {
          if (world.setBlock(pos, blockState, 3)) {
            if (world.fallingBlockLandObserver)
              world.fallingBlockLandObserver(id, pos, world.getBlock(pos));
            discard();
            // Fallable.onLand is inherited no-op for SandBlock. Final drag is
            // still applied below, even after discard, matching vanilla.
          } else if (dropItem) {
            discard();
            spawnAtLocation(world, itemStack(blockItem(blockState), 1));
          }
        } else {
          discard();
          if (dropItem)
            spawnAtLocation(world, itemStack(blockItem(blockState), 1));
        }
      } else
        discard();
    }
  }
  deltaMovement = deltaMovement * 0.98;
}

Minecart::Minecart(World &world, Vec3 pos) : Entity("minecraft:minecart") {
  setPos(pos);
  oldPosition = position;
  (void)world;
}
bool Minecart::hurt(World &world, DamageKind, float amount) {
  if (isRemoved())
    return true;
  hurtDir = -hurtDir;
  hurtTime = 10;
  damage += amount * 10.0f;
  if (damage > 40.0f) {
    discard();
    spawnAtLocation(world, itemStack("minecraft:minecart", 1));
  }
  return true;
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
  if (hurtTime > 0)
    --hurtTime;
  if (damage > 0)
    damage = std::max(0.0f, damage - 1.0f);
  checkBelowWorld(world);
  tickOldBehavior(world);
  if (isRemoved())
    return;
  if (isInLava(world)) {
    igniteForSeconds(15.0f);
    lavaHurt(world);
    fallDistance *= 0.5f;
  }
  firstTick = false;
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
                       state.get("powered", "false") == "true");
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
    powerTrack = state.get("powered", "false") == "true";
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
// TODO: passengers, chest/furnace/hopper/TNT minecart variants, the
// experimental NewMinecartBehavior, and entity collision damage.
void Minecart::pushAndPickupEntities(World &world) {
  Aabb hitbox = boundingBox().inflate(0.2f, 0.0, 0.2f);
  if (deltaMovement.horizontalDistanceSqr() >= 0.01) {
    for (Entity *entity : world.entitiesIn(hitbox))
      if (entity != this &&
          (dynamic_cast<Minecart *>(entity) || entity->isPushable()))
        entity->push(*this);
  } else {
    for (Entity *entity : world.entitiesIn(hitbox))
      if (entity != this && entity->isPushable())
        entity->push(*this);
  }
}

std::shared_ptr<Entity> World::addEntity(std::shared_ptr<Entity> entity) {
  entity->id = nextEntityId++;
  entities.push_back(entity);
  entitySectionAdd(*entity);
  return entity;
}
void World::entitySectionAdd(Entity &entity) {
  entity.entitySection = entitySectionKey(entity.position);
  entitySections_[entity.entitySection].push_back(&entity);
}
void World::entitySectionRemove(Entity &entity) {
  auto it = entitySections_.find(entity.entitySection);
  if (it == entitySections_.end())
    return;
  auto &list = it->second;
  list.erase(std::remove(list.begin(), list.end(), &entity), list.end());
  if (list.empty())
    entitySections_.erase(it);
}
void World::entitySectionUpdate(Entity &entity) {
  const auto key = entitySectionKey(entity.position);
  if (key == entity.entitySection)
    return;
  entitySectionRemove(entity);
  entitySectionAdd(entity);
}
void World::removeEntity(const std::shared_ptr<Entity> &entity) {
  if (entity)
    entity->discard();
}
std::vector<Entity *> World::entitiesIn(const Aabb &box) const {
  std::vector<Entity *> result;
  forEachEntitySection(entitySections_, box,
                       [&](const std::vector<Entity *> &section) {
                         for (Entity *entity : section)
                           if (entity && !entity->isRemoved() &&
                               entity->boundingBox().intersects(box))
                             result.push_back(entity);
                       });
  // Keep the previous spawn-order results: section iteration order must not
  // leak into merge/push/explosion iteration order.
  std::sort(result.begin(), result.end(),
            [](const Entity *a, const Entity *b) { return a->id < b->id; });
  return result;
}
// TODO: entity-to-entity collisions and the vehicle/passenger tick recursion
// (ServerLevel.tickPassenger). Simple entities have no passengers yet.
void World::tickEntities() {
  auto snapshot = entities;
  for (auto &entity : snapshot) {
    if (!entity || entity->isRemoved())
      continue;
    // Entity-ticking visibility belongs to the X/Z chunk, including sections
    // above/below build height. isLoaded(BlockPos) also bounds Y and would
    // freeze falling entities before they can reach Entity.onBelowWorld.
    if (!isChunkLoaded(static_cast<int>(std::floor(entity->position.x / 16)),
                        static_cast<int>(std::floor(entity->position.z / 16))))
      continue;
    entity->setOldPosAndRot();
    ++entity->tickCount;
    entity->tick(*this);
    // TransientEntitySectionManager: moved entities change their bucket.
    if (!entity->isRemoved())
      entitySectionUpdate(*entity);
  }
  for (auto it = entities.begin(); it != entities.end();) {
    if (!*it || (*it)->isRemoved()) {
      if (*it)
        entitySectionRemove(**it);
      it = entities.erase(it);
    } else
      ++it;
  }
}
void World::collectCollisionBoxes(const Aabb &box, std::vector<ShapeBox> &out) {
  out.clear();
  // BlockCollisions visits the surrounding shell too: walls extend upward by
  // half a block and moving pistons can expose shapes in adjacent cells.
  const int x0 = static_cast<int>(std::floor(box.minX - EPS)) - 1;
  const int y0 = static_cast<int>(std::floor(box.minY - EPS)) - 1;
  const int z0 = static_cast<int>(std::floor(box.minZ - EPS)) - 1;
  const int x1 = static_cast<int>(std::floor(box.maxX + EPS)) + 1;
  const int y1 = static_cast<int>(std::floor(box.maxY + EPS)) + 1;
  const int z1 = static_cast<int>(std::floor(box.maxZ + EPS)) + 1;
  VoxelShape scratch;
  for (int x = x0; x <= x1; ++x)
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z) {
        const int shellAxes = int(x == x0 || x == x1) +
                              int(y == y0 || y == y1) +
                              int(z == z0 || z == z1);
        if (shellAxes == 3)
          continue;
        const BlockState state = getBlock({x, y, z});
        if (state.isAir())
          continue;
        if (shellAxes == 2 && !state.is(Blocks::MOVING_PISTON))
          continue;
        VoxelShape moving;
        const VoxelShape &shape = state.is(Blocks::MOVING_PISTON)
                                     ? (moving = collisionShape(*this, {x, y, z}, state), moving)
                                     : collisionShape(state);
        if (shape.empty())
          continue;
        if (shellAxes == 1 &&
            std::none_of(shape.begin(), shape.end(), [](const auto &part) {
              return part.x0 < 0 || part.y0 < 0 || part.z0 < 0 ||
                     part.x1 > 16 || part.y1 > 16 || part.z1 > 16;
            }))
          continue;
        for (const auto &b : shape) {
          if (Aabb{b.x0 / 16.0 + x, b.y0 / 16.0 + y, b.z0 / 16.0 + z,
                   b.x1 / 16.0 + x, b.y1 / 16.0 + y, b.z1 / 16.0 + z}
                  .intersects(box))
            out.push_back({b.x0 / 16.0 + x, b.y0 / 16.0 + y,
                           b.z0 / 16.0 + z, b.x1 / 16.0 + x,
                           b.y1 / 16.0 + y, b.z1 / 16.0 + z});
        }
      }
}
std::vector<VoxelShape> World::blockCollisions(const Aabb &box) {
  std::vector<VoxelShape> result;
  // BlockCollisions also visits the surrounding shell. Walls extend upward
  // by half a block, and moving pistons can expose shapes in adjacent cells.
  int x0 = static_cast<int>(std::floor(box.minX - EPS)) - 1,
      y0 = static_cast<int>(std::floor(box.minY - EPS)) - 1,
      z0 = static_cast<int>(std::floor(box.minZ - EPS)) - 1;
  int x1 = static_cast<int>(std::floor(box.maxX + EPS)) + 1,
      y1 = static_cast<int>(std::floor(box.maxY + EPS)) + 1,
      z1 = static_cast<int>(std::floor(box.maxZ + EPS)) + 1;
  for (int x = x0; x <= x1; ++x)
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z) {
        Pos p{x, y, z};
        if (!isLoaded(p))
          continue;
        int shellAxes = int(x == x0 || x == x1) +
                        int(y == y0 || y == y1) +
                        int(z == z0 || z == z1);
        if (shellAxes == 3)
          continue;
        auto state = getBlock(p);
        if (shellAxes == 2 && !state.is(Blocks::MOVING_PISTON))
          continue;
        auto shape = collisionShape(*this, p, state);
        if (shape.empty())
          continue;
        if (shellAxes == 1 &&
            std::none_of(shape.begin(), shape.end(), [](const auto &part) {
              return part.x0 < 0 || part.y0 < 0 || part.z0 < 0 ||
                     part.x1 > 16 || part.y1 > 16 || part.z1 > 16;
            }))
          continue;
        VoxelShape offset;
        for (auto b : shape)
          if (Aabb{b.x0 / 16.0 + x, b.y0 / 16.0 + y, b.z0 / 16.0 + z,
                   b.x1 / 16.0 + x, b.y1 / 16.0 + y, b.z1 / 16.0 + z}
                  .intersects(box))
            offset.push_back({b.x0 / 16.0 + x, b.y0 / 16.0 + y,
                              b.z0 / 16.0 + z, b.x1 / 16.0 + x,
                              b.y1 / 16.0 + y, b.z1 / 16.0 + z});
        if (!offset.empty())
          result.push_back(std::move(offset));
      }
  return result;
}
bool World::noCollision(const Aabb &box) {
  auto &boxes = scratchBoxes();
  collectCollisionBoxes(box, boxes);
  for (const auto &b : boxes)
    if (box.maxX > b.x0 + EPS && box.minX < b.x1 - EPS &&
        box.maxY > b.y0 + EPS && box.minY < b.y1 - EPS &&
        box.maxZ > b.z0 + EPS && box.minZ < b.z1 - EPS)
      return false;
  return true;
}
bool World::clip(Vec3 from, Vec3 to) {
  int x0 = static_cast<int>(std::floor(std::min(from.x, to.x))),
      y0 = static_cast<int>(std::floor(std::min(from.y, to.y))),
      z0 = static_cast<int>(std::floor(std::min(from.z, to.z)));
  int x1 = static_cast<int>(std::floor(std::max(from.x, to.x))),
      y1 = static_cast<int>(std::floor(std::max(from.y, to.y))),
      z1 = static_cast<int>(std::floor(std::max(from.z, to.z)));
  for (int x = x0; x <= x1; ++x)
    for (int y = y0; y <= y1; ++y)
      for (int z = z0; z <= z1; ++z) {
        Pos p{x, y, z};
        if (!isLoaded(p))
          continue;
        const BlockState state = getBlock(p);
        if (state.isAir())
          continue;
        VoxelShape moving;
        const VoxelShape &shape =
            state.is(Blocks::MOVING_PISTON)
                ? (moving = collisionShape(*this, p, state), moving)
                : collisionShape(state);
        for (const auto &b : shape) {
          ShapeBox box{b.x0 / 16.0 + x, b.y0 / 16.0 + y, b.z0 / 16.0 + z,
                       b.x1 / 16.0 + x, b.y1 / 16.0 + y, b.z1 / 16.0 + z};
          if (rayBox(from, to, box))
            return true;
        }
      }
  return false;
}
void World::spawnItem(Pos pos, const std::string &item, int count,
                      bool defaultPickupDelay) {
  if (item.empty() || count <= 0)
    return;
  double halfHeight = 0.125;
  Vec3 at{pos.x + 0.5 + random.nextDouble(-0.25, 0.25),
          pos.y + 0.5 + random.nextDouble(-0.25, 0.25) - halfHeight,
          pos.z + 0.5 + random.nextDouble(-0.25, 0.25)};
  spawnItemAt(at, item, count, defaultPickupDelay);
}
void World::spawnItemAt(Vec3 pos, const std::string &item, int count,
                        bool defaultPickupDelay) {
  if (item.empty() || count <= 0)
    return;
  auto entity = std::make_shared<ItemEntity>(*this, pos, itemStack(item, count));
  if (defaultPickupDelay)
    entity->pickupDelay = 10;
  addEntity(entity);
  log.push_back({"entity_spawn",
                 {static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))},
                 "minecraft:item:" + canonical(item)});
}
void World::spawnPrimedTnt(Vec3 pos, int fuse) {
  auto entity = std::make_shared<PrimedTnt>(*this, pos);
  entity->fuse = fuse;
  addEntity(entity);
  log.push_back({"entity_spawn",
                 {static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))},
                 "minecraft:tnt"});
}
void World::spawnFallingBlock(Vec3 pos, const BlockState &state) {
  auto entity = std::make_shared<FallingBlockEntity>(*this, pos, state);
  addEntity(entity);
  log.push_back({"entity_spawn",
                 {static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))},
                 "minecraft:falling_block:" + state.name()});
}
void World::spawnMinecart(Vec3 pos) {
  auto entity = std::make_shared<Minecart>(*this, pos);
  addEntity(entity);
  log.push_back({"entity_spawn",
                 {static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))},
                 "minecraft:minecart"});
}
bool World::primeTntBlock(Pos pos) {
  if (!tntExplodes)
    return false;
  spawnPrimedTnt({pos.x + 0.5, double(pos.y), pos.z + 0.5}, 80);
  setBlock(pos, airState(), 3);
  return true;
}

namespace {
double seenPercent(World &world, Vec3 center, Entity &entity) {
  Aabb bb = entity.boundingBox();
  double xs = 1.0 / ((bb.maxX - bb.minX) * 2.0 + 1.0);
  double ys = 1.0 / ((bb.maxY - bb.minY) * 2.0 + 1.0);
  double zs = 1.0 / ((bb.maxZ - bb.minZ) * 2.0 + 1.0);
  double xOffset = (1.0 - std::floor(1.0 / xs) * xs) / 2.0;
  double zOffset = (1.0 - std::floor(1.0 / zs) * zs) / 2.0;
  if (xs < 0 || ys < 0 || zs < 0)
    return 0;
  int hits = 0, count = 0;
  for (double xx = 0; xx <= 1.0; xx += xs)
    for (double yy = 0; yy <= 1.0; yy += ys)
      for (double zz = 0; zz <= 1.0; zz += zs) {
        Vec3 from{bb.minX + xx * (bb.maxX - bb.minX) + xOffset,
                  bb.minY + yy * (bb.maxY - bb.minY),
                  bb.minZ + zz * (bb.maxZ - bb.minZ) + zOffset};
        if (!world.clip(from, center))
          ++hits;
        ++count;
      }
  return count ? static_cast<double>(hits) / count : 0;
}
struct ExplosionStack {
  Pos pos;
  ItemStack stack;
};
void addExplosionStack(std::vector<ExplosionStack> &stacks,
                       const ItemStack &input, Pos pos) {
  ItemStack remaining = input;
  for (auto &entry : stacks) {
    if (entry.stack.id != remaining.id)
      continue;
    if (entry.stack.count + remaining.count > entry.stack.maxStackSize())
      continue;
    int delta = std::min(std::min(entry.stack.maxStackSize(), 16) -
                             entry.stack.count,
                         remaining.count);
    entry.stack.count += delta;
    remaining.count -= delta;
    if (remaining.count <= 0)
      return;
  }
  if (remaining.count > 0)
    stacks.push_back({pos, remaining});
}
} // namespace

void World::explode(Entity *source, Vec3 center, float radius, bool fire) {
  (void)source; // Direct/indirect source identity only affects player credit.
  log.push_back({"explosion",
                 {static_cast<int>(std::floor(center.x)),
                  static_cast<int>(std::floor(center.y)),
                  static_cast<int>(std::floor(center.z))},
                 "power=" + std::to_string(radius)});
  // ServerExplosion.calculateExplodedPositions. The Java HashSet iteration
  // order is deliberately not reproduced (see docs/compatibility.md): positions
  // are deduplicated with a hash set and then sorted, reproducing the previous
  // ordered-set order without one tree node per ray step.
  // TODO: Util.shuffle(targetBlocks, random), source damage attribution,
  // block-entity explosion hooks, and fire creation.
  std::vector<Pos> toBlow;
  toBlow.reserve(1024);
  // Lithium-style direct-mapped cache: the ray shell samples the same block
  // positions many times, so the state/fluid/resistance lookup is memoised
  // instead of hitting the chunk map on every step.
  struct CachedBlock {
    std::uint64_t key = 0;
    BlockState::Id state = 0;
    float resistance = 0.0f;
    bool present = false;
    bool air = true;
    bool fluid = false;
    bool loaded = false;
  };
  constexpr std::size_t kBlockCacheSize = 512;
  constexpr std::uint64_t kBlockCacheShift = 55; // 64 - 9
  std::array<CachedBlock, kBlockCacheSize> blockCache{};
  const auto blockKey = [](Pos p) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) &
            0x3FFFFFFull)
               << 38 |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) &
            0x3FFFFFFull)
               << 12 |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) &
            0xFFFull);
  };
  constexpr int size = 16;
  for (int xx = 0; xx < size; ++xx)
    for (int yy = 0; yy < size; ++yy)
      for (int zz = 0; zz < size; ++zz) {
        if (xx != 0 && xx != 15 && yy != 0 && yy != 15 && zz != 0 &&
            zz != 15)
          continue;
        double xd = xx / 15.0 * 2.0 - 1.0;
        double yd = yy / 15.0 * 2.0 - 1.0;
        double zd = zz / 15.0 * 2.0 - 1.0;
        double d = std::sqrt(xd * xd + yd * yd + zd * zd);
        xd /= d;
        yd /= d;
        zd /= d;
        // ServerExplosionMixin.performRayCast: the step vector is constant per
        // ray, so the 0.3 scaling is hoisted out of the stepping loop.
        const double stepX = xd * 0.3;
        const double stepY = yd * 0.3;
        const double stepZ = zd * 0.3;
        double remainingPower = radius * (0.7f + random.nextFloat() * 0.6f);
        double xp = center.x, yp = center.y, zp = center.z;
        // A 0.3 step usually stays inside the same block. Reuse the previous
        // block's resistance and air flags instead of probing the cache again
        // (lithium's prevX/prevY/prevZ shortcut).
        int prevX = std::numeric_limits<int>::min();
        int prevY = std::numeric_limits<int>::min();
        int prevZ = std::numeric_limits<int>::min();
        float prevResistance = 0.0f;
        bool prevAir = true;
        bool prevFluid = false;
        while (remainingPower > 0) {
          const int blockX = floorToInt(xp);
          const int blockY = floorToInt(yp);
          const int blockZ = floorToInt(zp);
          const bool sameBlock =
              blockX == prevX && blockY == prevY && blockZ == prevZ;
          if (!sameBlock) {
            const Pos pos{blockX, blockY, blockZ};
            const std::uint64_t key = blockKey(pos);
            CachedBlock &cached =
                blockCache[(key * 0x9E3779B97F4A7C15ull) >> kBlockCacheShift];
            if (!cached.present || cached.key != key) {
              const auto state = getBlock(pos);
              const bool fluid =
                  isLiquidBlock(state) ||
                  state.getView("waterlogged", "false") == "true";
              const float blast = static_cast<float>(std::max(
                  static_cast<double>(blockMetadata(state).explosionResistance),
                  fluid ? 100.0 : 0.0));
              cached.key = key;
              cached.present = true;
              cached.state = state.id();
              cached.air = state.isAir();
              cached.fluid = fluid;
              // Cache the per-step loss, (resistance + 0.3) * 0.3.
              cached.resistance = (blast + 0.3f) * 0.3f;
              cached.loaded = isLoaded(pos);
            }
            if (!cached.loaded)
              break;
            prevX = blockX;
            prevY = blockY;
            prevZ = blockZ;
            prevResistance = cached.resistance;
            prevAir = cached.air;
            prevFluid = cached.fluid;
          }
          if (!prevAir || prevFluid)
            remainingPower -= prevResistance;
          // Lithium "skip air": air positions are never destroyed by
          // interactWithBlocks, and fire/counting are not modelled here. Later
          // steps are strictly weaker, so only the first step inside a block
          // can add it: repeated same-block steps only produced duplicates.
          if (!sameBlock && remainingPower > 0 && !prevAir)
            toBlow.push_back({blockX, blockY, blockZ});
          xp += stepX;
          yp += stepY;
          zp += stepZ;
          // ServerExplosionMine: constant fall-off per step.
          remainingPower -= 0.22500001;
        }
      }
  // ServerExplosion.hurtEntities.
  if (radius >= 1.0e-5f) {
    double doubleRadius = radius * 2.0;
    Aabb area{std::floor(center.x - doubleRadius - 1.0),
              std::floor(center.y - doubleRadius - 1.0),
              std::floor(center.z - doubleRadius - 1.0),
              std::floor(center.x + doubleRadius + 1.0) + 1.0,
              std::floor(center.y + doubleRadius + 1.0) + 1.0,
              std::floor(center.z + doubleRadius + 1.0) + 1.0};
    auto candidates = entitiesIn(area);
    for (Entity *entity : candidates) {
      if (!entity || entity->isRemoved() || entity->ignoreExplosion(*this))
        continue;
      double dist = std::sqrt(entity->distanceToSqr(center)) / doubleRadius;
      if (dist > 1.0)
        continue;
      Vec3 origin = dynamic_cast<PrimedTnt *>(entity)
                        ? entity->position
                        : entity->eyePosition();
      Vec3 direction = (origin - center).normalize();
      double exposure = seenPercent(*this, center, *entity);
      double power = (1.0 - dist) * exposure;
      float damage =
          static_cast<float>((power * power + power) / 2.0 * 7.0 * doubleRadius +
                             1.0);
      entity->hurt(*this, DamageKind::Explosion, damage);
      if (entity->isRemoved())
        continue;
      double knockbackPower = (1.0 - dist) * exposure;
      entity->push(direction * knockbackPower);
    }
  }
  // ServerExplosion.interactWithBlocks.
  // calculateExplodedPositions yields a set: sort and deduplicate instead of
  // paying a tree node per ray step.
  std::sort(toBlow.begin(), toBlow.end());
  toBlow.erase(std::unique(toBlow.begin(), toBlow.end()), toBlow.end());
  std::vector<ExplosionStack> stacks;
  for (const auto pos : toBlow) {
    const auto state = getBlock(pos);
    if (state.isAir())
      continue;
    if (state.is(Blocks::TNT)) {
      setBlock(pos, airState(), 3);
      if (tntExplodes) {
        int fuse = 80;
        int randomized = random.nextInt(fuse / 4) + fuse / 8;
        spawnPrimedTnt({pos.x + 0.5, double(pos.y), pos.z + 0.5}, randomized);
      }
      continue;
    }
    for (const auto &drop : blockDrops(state))
      addExplosionStack(stacks, drop, pos);
    setBlock(pos, airState(), 3);
  }
  for (const auto &entry : stacks)
    spawnItem(entry.pos, entry.stack.id, entry.stack.count);
  if (fire) {
    // TODO: BaseFireBlock needs a registered fire block before explosion fire
    // can be reproduced. Vanilla TNT passes fire=false.
  }
}

} // namespace redstone
