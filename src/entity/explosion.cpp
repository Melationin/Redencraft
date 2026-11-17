#include "entity/internal.hpp"

namespace redstone {
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
  // ServerExplosion.calculateExplodedPositions. The Java HashSet iteration
  // order is deliberately not reproduced (see docs/compatibility.md): positions
  // are deduplicated with a hash set and then sorted, reproducing the previous
  // ordered-set order without one tree node per ray step.
  // TODO: Util.shuffle(targetBlocks, random), source damage attribution,
  // block-entity explosion hooks, and fire creation.
  std::vector<Pos> &toBlow = explosionTargets_;
  toBlow.clear();
  ++explosionStats.explosions;
  // Lithium-style direct-mapped cache: the ray shell samples the same block
  // positions many times, so the state/fluid/resistance lookup is memoised
  // instead of hitting the chunk map on every step. The table outlives a single
  // explosion, so a generation stamp replaces the per-call zero fill.
  if (++explosionGeneration_ == 0) {
    for (auto &entry : explosionCache_)
      entry.generation = 0;
    explosionGeneration_ = 1;
  }
  const std::uint32_t generation = explosionGeneration_;
  auto &blockCache = explosionCache_;
  constexpr std::uint64_t kBlockCacheShift = 55; // 64 - 9
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
  for (const auto &step : explosionSteps()) {
    const double stepX = step.x;
    const double stepY = step.y;
    const double stepZ = step.z;
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
        ExplosionCacheEntry &cached =
            blockCache[(key * 0x9E3779B97F4A7C15ull) >> kBlockCacheShift];
        if (cached.generation != generation || cached.key != key) {
          ++explosionStats.probes;
          const auto state = getBlock(pos);
          const bool fluid = isLiquidBlock(state) || waterloggedOf(state);
          const float blast = static_cast<float>(std::max(
              static_cast<double>(blockMetadata(state).explosionResistance),
              fluid ? 100.0 : 0.0));
          cached.key = key;
          cached.generation = generation;
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
      if (!sameBlock && remainingPower > 0 && !prevAir) {
        ++explosionStats.candidates;
        toBlow.push_back({blockX, blockY, blockZ});
      }
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
  // TODO: TRIGGER_BLOCK explosions (e.g. wind charges) toggle unpowered
  // fence gates without destroying them; this simulator only models the TNT
  // destructive explosion mode here.
  // calculateExplodedPositions yields a set: sort and deduplicate instead of
  // paying a tree node per ray step.
  std::sort(toBlow.begin(), toBlow.end());
  toBlow.erase(std::unique(toBlow.begin(), toBlow.end()), toBlow.end());
  explosionStats.destroyed += toBlow.size();
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
  if (explosionObserver)
    explosionObserver(center, radius, static_cast<int>(toBlow.size()));
}

void World::explodeTriggerBlock(Entity *source, Vec3 center, float radius) {
  (void)source;
  const int r = static_cast<int>(std::ceil(radius));
  const Pos c{static_cast<int>(std::floor(center.x)),
              static_cast<int>(std::floor(center.y)),
              static_cast<int>(std::floor(center.z))};
  const double rSqr = static_cast<double>(radius) * static_cast<double>(radius);
  for (int dx = -r; dx <= r; ++dx)
    for (int dy = -r; dy <= r; ++dy)
      for (int dz = -r; dz <= r; ++dz) {
        const Pos p{c.x + dx, c.y + dy, c.z + dz};
        const Vec3 bc{p.x + 0.5, p.y + 0.5, p.z + 0.5};
        if ((bc - center).lengthSqr() > rSqr)
          continue;
        const BlockState state = getBlock(p);
        if (state.isAir())
          continue;
        if (state.is(Blocks::LEVER)) {
          const BlockState next =
              state.with(PropertyKind::Powered,
                         poweredOf(state) ? std::uint8_t{0} : std::uint8_t{1});
          setBlock(p, next, 3);
          const Direction attached =
              [&] {
                const auto face = state.getView("face", "wall");
                if (face == "ceiling")
                  return Direction::Down;
                if (face == "floor")
                  return Direction::Up;
                return directionOf(state);
              }();
          updateNeighborsAt(p, next.name());
          updateNeighborsAt(p.relative(opposite(attached)), next.name());
        } else if (state.block().isButton()) {
          if (!poweredOf(state)) {
            const BlockState next =
                state.with(PropertyKind::Powered, std::uint8_t{1});
            setBlock(p, next, 3);
            updateNeighborsAt(p, next.name());
            scheduleTick(p, next.block(),
                         state.is(Blocks::STONE_BUTTON) ||
                                 state.is(Blocks::POLISHED_BLACKSTONE_BUTTON)
                             ? 20
                             : 30);
          }
        } else if (state.block().isFenceGate()) {
          const bool wasOpen = propertyValue(state, PropertyKind::Open) != 0;
          setBlock(p,
                   state.with(PropertyKind::Open,
                              wasOpen ? std::uint8_t{0} : std::uint8_t{1}),
                   3);
        } else if (state.block().isTrapdoor() && !state.is(Blocks::IRON_TRAPDOOR)) {
          const bool wasOpen = propertyValue(state, PropertyKind::Open) != 0;
          setBlock(p,
                   state.with(PropertyKind::Open,
                              wasOpen ? std::uint8_t{0} : std::uint8_t{1}),
                   3);
        } else if (state.block().isDoor() && !state.is(Blocks::IRON_DOOR)) {
          const bool wasOpen = propertyValue(state, PropertyKind::Open) != 0;
          const std::uint8_t nextOpen = wasOpen ? 0 : 1;
          setBlock(p, state.with(PropertyKind::Open, nextOpen), 3);
          const Pos otherHalf =
              state.getView("half", "lower") == "lower"
                  ? p.relative(Direction::Up)
                  : p.relative(Direction::Down);
          const BlockState otherState = getBlock(otherHalf);
          if (otherState.sameBlock(state))
            setBlock(otherHalf, otherState.with(PropertyKind::Open, nextOpen), 3);
        } else if (state.is(Blocks::BELL)) {
          log.push_back({"bell_ring", p, "wind_charge"});
        } else if (state.block().isCandle() && litOf(state)) {
          setBlock(p, state.with(PropertyKind::Lit, std::uint8_t{0}), 3);
        }
      }

  const double doubleRadius = radius * 2.0;
  const Aabb area{center.x - doubleRadius - 1.0, center.y - doubleRadius - 1.0,
                  center.z - doubleRadius - 1.0, center.x + doubleRadius + 1.0,
                  center.y + doubleRadius + 1.0, center.z + doubleRadius + 1.0};
  for (Entity *entity : entitiesIn(area)) {
    if (!entity || entity->isRemoved() || entity->ignoreExplosion(*this))
      continue;
    const double dist = std::sqrt(entity->distanceToSqr(center)) / doubleRadius;
    if (dist > 1.0)
      continue;
    const Vec3 origin = entity->eyePosition();
    Vec3 dir = origin - center;
    if (dir.lengthSqr() < 1.0e-12)
      dir = {0.0, 1.0, 0.0};
    else
      dir = dir.normalize();
    const double exposure = seenPercent(*this, center, *entity);
    const double knockback = (1.0 - dist) * exposure * 1.22;
    entity->push(dir * knockback);
  }
  if (explosionObserver)
    explosionObserver(center, radius, 0);
}

} // namespace redstone
