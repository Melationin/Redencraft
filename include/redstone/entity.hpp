#pragma once
#include "redstone/engine.hpp"
#include "redstone/rail.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace redstone {
struct ItemStack {
  std::string id;
  int count = 0;
  bool empty() const { return id.empty() || count <= 0; }
  int maxStackSize() const;
  bool operator==(const ItemStack &) const = default;
};
ItemStack itemStack(std::string id, int count = 1);
// Block loot evaluated with an empty tool, as used by destroyBlock,
// updateOrDestroy, pistons, and explosions. Fragile blocks (glass, coral fans,
// piston heads, bedrock) intentionally return nothing without Silk Touch.
std::vector<ItemStack> blockDrops(const BlockState &state);
// Item form used by the creative inventory and by minecart/piston drops.
std::string blockItem(const BlockState &state);
bool isReplaceable(const BlockState &state);
bool isAirState(const BlockState &state);

enum class DamageKind { Generic, Fire, Lava, Explosion };

class Entity {
public:
  explicit Entity(std::string type);
  virtual ~Entity() = default;
  Entity(const Entity &) = delete;
  Entity &operator=(const Entity &) = delete;

  virtual void tick(World &world) = 0;
  virtual bool fireImmune() const;
  virtual bool hurt(World &world, DamageKind kind, float damage);
  virtual bool ignoreExplosion(World &world) const;
  virtual bool isPushable() const { return false; }
  virtual bool isPickable() const { return false; }

  const EntityType &type() const { return *type_; }
  const std::string &typeName() const { return type_->name; }
  int id = 0;
  // Persistent UUID; never used as the runtime protocol/tracking id.
  std::array<std::int32_t, 4> uuid{};
  // EntitySectionStorage: SectionPos.asLong of the section this entity is
  // bucketed in (maintained by World, like the vanilla entity section callback).
  std::uint64_t entitySection = 0;
  // Each tracking session observes impulses independently.
  std::uint64_t motionRevision = 0;
  Vec3 position;
  Vec3 oldPosition;
  Vec3 deltaMovement;
  float yaw = 0, pitch = 0;
  float oldYaw = 0, oldPitch = 0;
  int tickCount = 0;
  bool onGround = false;
  bool horizontalCollision = false;
  bool verticalCollision = false;
  bool verticalCollisionBelow = false;
  bool noPhysics = false;
  int remainingFireTicks = 0;
  int portalCooldown = 0;
  double fallDistance = 0.0;

  struct PortalProcessState {
    bool active = false;
    Pos entryPosition{};
    int portalTime = 0;
    bool insidePortalThisTick = false;
  };

  Aabb boundingBox() const;
  Vec3 eyePosition() const { return position + Vec3{0, eyeHeight(), 0}; }
  double eyeHeight() const { return type_->eyeHeight; }
  double distanceToSqr(const Vec3 &to) const;
  void setPos(double x, double y, double z);
  void setPos(const Vec3 &pos) { setPos(pos.x, pos.y, pos.z); }
  void setDeltaMovement(const Vec3 &value) { deltaMovement = value; }
  void setDeltaMovement(double x, double y, double z) {
    deltaMovement = {x, y, z};
  }
  void addDeltaMovement(const Vec3 &value) { deltaMovement += value; }
  void setOldPosAndRot() {
    oldPosition = position;
    oldYaw = yaw;
    oldPitch = pitch;
  }
  bool isRemoved() const { return removed_; }
  void discard() { removed_ = true; }
  void kill(World &world);

  void baseTick(World &world);
  void checkBelowWorld(World &world);
  virtual int dimensionChangingDelay() const { return 300; }
  virtual bool canUsePortal(bool allowVehicles = false) const;
  bool isOnPortalCooldown() const noexcept { return portalCooldown > 0; }
  void setPortalCooldown() { portalCooldown = dimensionChangingDelay(); }
  void setPortalCooldown(int cooldown) { portalCooldown = cooldown; }
  void processPortalCooldown();
  void setAsInsidePortal(Pos pos);
  void handlePortal(World &world);
  void applyFireBlockEffect(World &world, float fireDamage);
  void clearPortalProcess() noexcept { portalProcess_ = {}; }
  const PortalProcessState &portalProcess() const noexcept {
    return portalProcess_;
  }
  void move(World &world, Vec3 movement, bool piston = false);
  void push(double x, double y, double z);
  void push(const Vec3 &value);
  virtual void push(Entity &entity);
  void igniteForSeconds(float seconds);
  void igniteForTicks(int ticks);
  void clearFire();
  void lavaHurt(World &world);
  void applyEffectsFromBlocks(World &world);
  bool isInWater(World &world) const;
  bool isInLava(World &world) const;
  double fluidHeight(World &world, bool lava) const;
  void moveTowardsClosestSpace(World &world, double x, double y, double z);
  void spawnAtLocation(World &world, const ItemStack &stack);

protected:
  Vec3 limitPistonMovement(World &world, Vec3 movement);
  bool removed_ = false;
  const EntityType *type_ = nullptr;
  std::array<double, 3> pistonDeltas_{};
  std::int64_t pistonDeltasGameTime_ = -1;

private:
  friend class World;
  bool removalObserved_ = false;
  // Used only during the callback-free collision collection and resolution.
  std::vector<ShapeBox> movementColliders_;
  PortalProcessState portalProcess_{};
};

class ItemEntity : public Entity {
public:
  ItemEntity(World &world, Vec3 pos, ItemStack stack);
  ItemEntity(World &world, Vec3 pos, ItemStack stack, Vec3 motion);
  void tick(World &world) override;
  bool fireImmune() const override;
  bool hurt(World &world, DamageKind kind, float damage) override;
  bool isPickable() const override { return true; }

  ItemStack item;
  int age = 0;
  int pickupDelay = 0;
  int health = 5;
  float bobOffs = 0;

private:
  void setFluidMovement(double multiplier);
  bool isMergable() const;
  void mergeWithNeighbours(World &world);
  void tryToMerge(World &world, ItemEntity &other);
};

class PrimedTnt : public Entity {
public:
  explicit PrimedTnt(World &world, Vec3 pos);
  // Managed install supplies the persisted direction and therefore does not
  // consume World::random's ignition mixture.
  PrimedTnt(World &world, Vec3 pos, Vec3 motion);
  void tick(World &world) override;
  bool fireImmune() const override { return true; }
  bool isPickable() const override { return !isRemoved(); }

  static constexpr int defaultFuse = 80;
  int fuse = defaultFuse;
  float explosionPower = 4.0f;
  BlockState blockState = defaultState("tnt");
  bool usedPortal = false;
};

class FallingBlockEntity : public Entity {
public:
  FallingBlockEntity(World &world, Vec3 pos, BlockState state);
  static FallingBlockEntity *fall(World &world, Pos pos,
                                  const BlockState &state);
  void tick(World &world) override;
  bool isPickable() const override { return !isRemoved(); }

  BlockState blockState;
  int time = 0;
  bool dropItem = true;
  bool cancelDrop = false;
  bool hurtEntities = false;
  int fallDamageMax = 40;
  float fallDamagePerDistance = 0;
  Pos startPos;
};

class Minecart : public Entity {
public:
  explicit Minecart(World &world, Vec3 pos);
  void tick(World &world) override;
  bool hurt(World &world, DamageKind kind, float damage) override;
  bool ignoreExplosion(World &world) const override;
  bool isPushable() const override { return true; }
  bool isPickable() const override { return !isRemoved(); }
  using Entity::push;
  void push(Entity &entity) override;
  bool isRideable() const { return true; }

  bool onRails = false;
  bool flipped = false;
  int hurtTime = 0;
  int hurtDir = 1;
  float damage = 0;
  bool firstTick = true;
  double rotationOffset = 0;

private:
  void tickOldBehavior(World &world);
  void moveAlongTrack(World &world);
  void comeOffTrack(World &world);
  void activateMinecart(World &world, Pos pos, bool powered);
  void pushAndPickupEntities(World &world);
  Vec3 naturalSlowdown(World &world, Vec3 movement) const;
  double maxSpeed(World &world) const;
  Pos currentBlockPosOrRailBelow(World &world) const;
  std::optional<Vec3> railPos(World &world, double x, double y,
                              double z) const;
};

} // namespace redstone
