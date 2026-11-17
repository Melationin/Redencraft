#include "engine/internal.hpp"

namespace redstone {
namespace {
int readNibbleLight(const std::optional<std::array<std::uint8_t, 2048>> &layer,
                    Pos pos) {
  if (!layer)
    return 0;
  const std::size_t idx = lightIndex(pos.x, pos.y, pos.z);
  const std::uint8_t byteVal = (*layer)[idx >> 1];
  return static_cast<int>((idx & 1u) ? (byteVal >> 4) : (byteVal & 0x0fu));
}

void dispatchRandomTickCell(World &world, RandomTickAccess &access, Pos pos,
                            const BlockState &state) {
  if (state.isRandomlyTicking().value_or(true)) {
    ++world.randomTickStats.blocksTicked;
    const RandomTickResult res = state.randomTick(access, pos, world.random);
    if (res == RandomTickResult::Applied)
      ++world.randomTickStats.ticksApplied;
    else {
      ++world.randomTickStats.ticksUnsupported;
      if (world.unsupportedRandomTickObserver)
        world.unsupportedRandomTickObserver(pos, state, false);
    }
  }
  const FluidState fluid = fluidStateOf(state);
  if (fluid.isRandomlyTicking()) {
    ++world.randomTickStats.fluidsTicked;
    const RandomTickResult res = fluid.randomTick(access, pos, world.random);
    if (res == RandomTickResult::Applied)
      ++world.randomTickStats.ticksApplied;
    else {
      ++world.randomTickStats.ticksUnsupported;
      if (world.unsupportedRandomTickObserver)
        world.unsupportedRandomTickObserver(pos, state, true);
    }
  }
}

[[noreturn]] [[gnu::cold]] void throwRandomTickError(const char *msg) {
  throw std::logic_error(msg);
}

inline void dispatchRandomTickIndex(World &world, RandomTickAccess &access,
                                    int sectionBlockX, int sectionBaseY,
                                    int sectionBlockZ, int blockIndex,
                                    BlockState::Id id) {
  const Pos targetPos{sectionBlockX + (blockIndex & 15),
                      sectionBaseY + ((blockIndex >> 8) & 15),
                      sectionBlockZ + ((blockIndex >> 4) & 15)};
  dispatchRandomTickCell(world, access, targetPos, BlockState::fromId(id));
}

class WorldRandomTickAccess final : public RandomTickAccess {
public:
  explicit WorldRandomTickAccess(World &world) : world_(world) {}

  BlockState getBlock(Pos pos) const override { return world_.getBlock(pos); }

  std::optional<bool> isFullFluid(Pos pos) const override {
    const auto state = world_.getBlockIfLoaded(pos);
    if (!state)
      return std::nullopt;
    return fluidStateOf(*state).isFull();
  }

  std::optional<bool> isWaterFluid(Pos pos) const override {
    const auto state = world_.getBlockIfLoaded(pos);
    if (!state)
      return std::nullopt;
    return fluidHasTag(fluidStateOf(*state).getType(), FluidTag::Water);
  }

  std::optional<int> lightBlockIntoUp(BlockState from,
                                      Pos above) const override {
    const auto to = world_.getBlockIfLoaded(above);
    if (!to)
      return std::nullopt;
    return lightBlockIntoUpFromStates(from, *to);
  }

  std::optional<int> maxLocalRawBrightness(Pos pos) const override {
    return world_.rawBrightness(pos);
  }

  std::optional<bool> isSnowySetting(Pos above) const override {
    const auto state = world_.getBlockIfLoaded(above);
    if (!state)
      return std::nullopt;
    return world_.hasBlockTag(*state, "minecraft:snow");
  }

  std::optional<bool> canSpreadGrassSurvive(BlockState state,
                                            Pos above) const override {
    const auto aboveState = world_.getBlockIfLoaded(above);
    if (!aboveState)
      return std::nullopt;
    return canStayAliveFromAboveState(state, *aboveState);
  }

  SpreadTargetResult canSpreadGrassTarget(BlockState spreadDefault,
                                          Pos targetAbove) const override {
    const auto aboveState = world_.getBlockIfLoaded(targetAbove);
    if (!aboveState)
      return SpreadTargetResult::Unsupported;
    const auto survives = canStayAliveFromAboveState(spreadDefault, *aboveState);
    if (!survives)
      return SpreadTargetResult::Unsupported;
    if (!*survives)
      return SpreadTargetResult::CannotSpread;
    if (fluidHasTag(fluidStateOf(*aboveState).getType(), FluidTag::Water))
      return SpreadTargetResult::CannotSpread;
    const auto snowy = world_.hasBlockTag(*aboveState, "minecraft:snow");
    if (!snowy)
      return SpreadTargetResult::Unsupported;
    return *snowy ? SpreadTargetResult::SpreadSnowy
                  : SpreadTargetResult::SpreadClear;
  }

  void setBlockAndUpdate(Pos pos, BlockState state) override {
    world_.setBlock(pos, state, 3, 512);
  }

  void setBlock(Pos pos, BlockState state, int flags) override {
    world_.setBlock(pos, state, flags, 512);
  }

  std::optional<bool> growSaplingTree(Pos, BlockState, Random &) override {
    // TODO: TreeGrower / world generation is not implemented yet.
    return std::nullopt;
  }

  std::optional<bool> canMushroomSurvive(Pos pos,
                                         BlockState) const override {
    const Pos belowPos = pos.relative(Direction::Down);
    const auto belowState = world_.getBlockIfLoaded(belowPos);
    if (!belowState)
      return std::nullopt;
    const auto overrides = world_.hasBlockTag(
        *belowState, "minecraft:overrides_mushroom_light_requirement");
    if (overrides && *overrides)
      return true;
    const auto brightness = world_.rawBrightness(pos);
    if (!brightness)
      return std::nullopt;
    return *brightness < 13 && isSolidRender(*belowState);
  }

  int minY() const override { return world_.minY(); }
  int maxBlockY() const override { return world_.maxY() - 1; }
  bool isLoaded(Pos pos) const override { return world_.isLoaded(pos); }
  bool hasChunkAt(Pos pos) const override { return inLoadedColumn(pos); }
  std::optional<int> blockBrightness(Pos pos) const override {
    return world_.blockBrightness(pos);
  }
  std::optional<bool> hasBlockTag(BlockState state,
                                  std::string_view tag) const override {
    return world_.hasBlockTag(state, tag);
  }
  bool isFaceSturdy(Pos pos, BlockState state, Direction dir) const override {
    return faceSturdy(world_, pos, state, dir);
  }
  bool isFaceFullBlockSupportOrCollision(Pos pos, BlockState state,
                                         Direction dir) const override {
    // Support and collision shapes are both checked via faceSturdy for full
    // face attachment; leaves are solid collision cubes in registered shapes.
    return faceSturdy(world_, pos, state, dir);
  }
  bool hasCollisionIntersecting(Pos pos, double x0, double y0, double z0,
                                double x1, double y1,
                                double z1) const override {
    const BlockState state = world_.getBlock(pos);
    const auto intersects = [&](const VoxelShape &boxes) {
      for (const ShapeBox &b : boxes) {
        if (b.x1 > x0 && b.x0 < x1 && b.y1 > y0 && b.y0 < y1 && b.z1 > z0 &&
            b.z0 < z1)
          return true;
      }
      return false;
    };
    return state.is(Blocks::MOVING_PISTON)
               ? intersects(collisionShape(world_, pos, state))
               : intersects(collisionShape(state));
  }
  void removeBlock(Pos pos, bool movedByPiston) override {
    world_.setBlock(pos, Blocks::AIR.defaultState(),
                    movedByPiston ? 67 : 3, 512);
  }
  void neighborChanged(Pos pos, const BlockRef &source) override {
    world_.neighborChanged(pos, source);
  }
  void scheduleTick(Pos pos, const BlockRef &block, int delay) override {
    world_.scheduleTick(pos, block, delay);
  }
  bool hasScheduledTick(Pos pos, const BlockRef &block) const override {
    return world_.hasScheduledTick(pos, block);
  }
  bool canSpreadFireAround(Pos pos) const override {
    return world_.anyPlayerCloseEnoughForFireSpread(pos);
  }
  bool spreadVinesEnabled() const override { return world_.spreadVines; }
  bool waterEvaporates() const override { return world_.waterEvaporates; }
  std::optional<bool> isRainingAt(Pos) const override {
    // TODO: weather and biome precipitation are not implemented yet.
    return std::nullopt;
  }

private:
  static std::optional<int> lightBlockIntoUpFromStates(BlockState from,
                                                       BlockState to) {
    const BlockLightProperties &fromProps = blockLightProperties(from);
    const BlockLightProperties &toProps = blockLightProperties(to);
    if (!fromProps.supported || !toProps.supported)
      return std::nullopt;
    if (lightFaceShapesOcclude(fromProps, toProps, LightDirection::Up))
      return 15;
    return static_cast<int>(toProps.dampening);
  }

  static std::optional<bool> canStayAliveFromAboveState(BlockState state,
                                                        BlockState aboveState) {
    if (aboveState.is(Blocks::SNOW)) {
      if (!aboveState.hasProperty("layers"))
        return std::nullopt;
      if (aboveState.getView("layers") == "1")
        return true;
    }
    if (fluidStateOf(aboveState).isFull())
      return false;
    const auto blocked = lightBlockIntoUpFromStates(state, aboveState);
    if (!blocked)
      return std::nullopt;
    return *blocked < 15;
  }

  bool inLoadedColumn(Pos pos) const {
    return world_.isChunkLoaded(pos.x >> 4, pos.z >> 4);
  }

  World &world_;
};
} // namespace

bool World::anyPlayerCloseEnoughForFireSpread(Pos pos) const {
  if (fireSpreadRadiusAroundPlayer == -1)
    return true;
  if (fireSpreadRadiusAroundPlayer <= 0)
    return false;
  const double maxDistSq = static_cast<double>(fireSpreadRadiusAroundPlayer) *
                           static_cast<double>(fireSpreadRadiusAroundPlayer);
  const double tx = static_cast<double>(pos.x) + 0.5;
  const double ty = static_cast<double>(pos.y) + 0.5;
  const double tz = static_cast<double>(pos.z) + 0.5;
  for (const Vec3 &playerPos : fireSpreadPlayerPositions) {
    const double dx = playerPos.x - tx;
    const double dy = playerPos.y - ty;
    const double dz = playerPos.z - tz;
    if (dx * dx + dy * dy + dz * dz < maxDistSq)
      return true;
  }
  for (const auto &entity : entities) {
    if (!entity || !entity->isPlayer() || entity->isRemoved())
      continue;
    const double dx = entity->position.x - tx;
    const double dy = entity->position.y - ty;
    const double dz = entity->position.z - tz;
    if (dx * dx + dy * dy + dz * dz < maxDistSq)
      return true;
  }
  return false;
}

template <class ChunkPtr>
static std::optional<int> readWorldBrightness(const LightService *lightService,
                                              ChunkPtr chunk, Pos pos, int cx,
                                              int cz, int minY, int maxY,
                                              bool blockOnly) {
  if (!chunk)
    return std::nullopt;
  if (lightService) {
    const auto blockView = lightService->latestLayerChunk(LightLayer::Block, cx, cz);
    if (blockView) {
      if (blockOnly) {
        if (pos.y >= maxY || pos.y < minY)
          return 0;
        return blockView->lightValue(LightLayer::Block, pos.x, pos.y, pos.z);
      }
      const auto skyView = lightService->latestLayerChunk(LightLayer::Sky, cx, cz);
      if (skyView) {
        if (pos.y >= maxY)
          return skyView->skyLightEnabled ? 15 : 0;
        if (pos.y < minY)
          return 0;
        const int block = blockView->lightValue(LightLayer::Block, pos.x, pos.y, pos.z);
        const int sky = skyView->lightValue(LightLayer::Sky, pos.x, pos.y, pos.z);
        return std::max(block, sky);
      }
    }
  }
  if (chunk->persistedLight && chunk->persistedLight->isLightOn) {
    if (pos.y >= maxY)
      return blockOnly ? 0 : 15;
    if (pos.y < minY)
      return 0;
    const int syRel = (pos.y >> 4) - (minY >> 4);
    if (syRel < 0 ||
        syRel >= static_cast<int>(chunk->persistedLightSectionIndex.size()))
      return 0;
    const int secIdx =
        chunk->persistedLightSectionIndex[static_cast<std::size_t>(syRel)];
    if (secIdx < 0)
      return 0;
    const PersistedLightSection &sec =
        chunk->persistedLight->sections[static_cast<std::size_t>(secIdx)];
    const int block = readNibbleLight(sec.blockLight, pos);
    if (blockOnly)
      return block;
    return std::max(block, readNibbleLight(sec.skyLight, pos));
  }
  return std::nullopt;
}

std::optional<int> World::rawBrightness(Pos pos) const {
  const int cx = chunkIndex(pos.x);
  const int cz = chunkIndex(pos.z);
  return readWorldBrightness(lightService_.get(), chunkAt(cx, cz), pos, cx, cz,
                             minY_, maxY_, false);
}

std::optional<int> World::blockBrightness(Pos pos) const {
  const int cx = chunkIndex(pos.x);
  const int cz = chunkIndex(pos.z);
  return readWorldBrightness(lightService_.get(), chunkAt(cx, cz), pos, cx, cz,
                             minY_, maxY_, true);
}

std::uint16_t World::sectionTickingBlockCountForTest(int chunkX, int sectionY,
                                                    int chunkZ) const {
  const Chunk *c = chunkAt(chunkX, chunkZ);
  if (!c)
    return 0;
  const int idx = sectionY - (minY_ >> 4);
  if (idx < 0 || idx >= sectionCount())
    return 0;
  return c->sections[static_cast<std::size_t>(idx)].tickingBlockCount;
}

std::uint8_t World::sectionStorageBitsForTest(int chunkX, int sectionY,
                                              int chunkZ) const {
  const Chunk *c = chunkAt(chunkX, chunkZ);
  if (!c)
    return 0;
  const int idx = sectionY - (minY_ >> 4);
  if (idx < 0 || idx >= sectionCount())
    return 0;
  return c->sections[static_cast<std::size_t>(idx)].bits();
}

std::array<std::uint8_t, World::kRandomTickMinisectionCount>
World::sectionRandomTickMinisectionsForTest(int chunkX, int sectionY,
                                            int chunkZ) const {
  std::array<std::uint8_t, kRandomTickMinisectionCount> out{};
  const Chunk *c = chunkAt(chunkX, chunkZ);
  if (!c)
    return out;
  const int idx = sectionY - (minY_ >> 4);
  if (idx < 0 || idx >= sectionCount())
    return out;
  return c->sections[static_cast<std::size_t>(idx)].randomTickByMinisection;
}

int World::nextRandomBlockIndex() {
  // Level.getBlockRandomPos LCG packed directly into YZX cell index [0..4095].
  randValue_ = randValue_ * 3u + 1013904223u;
  const std::uint32_t r = randValue_ >> 2;
  return static_cast<int>((r & 15u) | ((r >> 8) & 0xf00u) |
                          ((r >> 4) & 0xf0u));
}

[[gnu::noinline]] void World::tickSparseRandomTickHit(
    int chunkX, int sectionBaseY, int chunkZ, Section &section, int nth,
    RandomTickAccess &access) {
  int mini = 0;
  for (; mini < kRandomTickMinisectionCount; ++mini) {
    const int countInMini =
        section.randomTickByMinisection[static_cast<std::size_t>(mini)];
    if (nth >= countInMini)
      nth -= countInMini;
    else
      break;
  }
  if (mini >= kRandomTickMinisectionCount)
    throwRandomTickError(
        "random-tick minisection count mismatch during sparse lookup");
  const int sectionBlockX = chunkX * 16;
  const int sectionBlockZ = chunkZ * 16;
  const int start = mini * kRandomTickMinisectionSize;
  const SectionStorage *base = section.storage.get();
  if (!base) {
    if (!stateIsRandomlyTickingId(section.single))
      throwRandomTickError(
          "single-value section marked ticking without ticking state");
    const int blockIndex = start + nth;
    if (blockIndex >= kLightVolume)
      throwRandomTickError("random-tick single-value index out of bounds");
    dispatchRandomTickIndex(*this, access, sectionBlockX, sectionBaseY,
                            sectionBlockZ, blockIndex, section.single);
    return;
  }
  const int end = std::min(start + kRandomTickMinisectionSize, kLightVolume);
  const bool found = visitSectionStorage(base, [&](const auto *block) -> bool {
    for (int blockIndex = start; blockIndex < end; ++blockIndex) {
      const auto idx = block->indexAt(static_cast<std::size_t>(blockIndex));
      if (block->paletteTicking[idx] && nth-- == 0) {
        dispatchRandomTickIndex(*this, access, sectionBlockX, sectionBaseY,
                                sectionBlockZ, blockIndex, block->palette[idx]);
        return true;
      }
    }
    return false;
  });
  if (!found)
    throwRandomTickError(
        "failed to find nth randomly ticking block inside section");
}

[[gnu::noinline]] void World::tickDenseRandomTickSection(
    int chunkX, int sectionBaseY, int chunkZ, Section &section, int tickSpeed,
    RandomTickAccess &access) {
  ++randomTickStats.sectionsDensePath;
  randomTickStats.positionsProbed += static_cast<std::uint64_t>(tickSpeed);
  const int sectionBlockX = chunkX * 16;
  const int sectionBlockZ = chunkZ * 16;
  for (int p = 0; p < tickSpeed; ++p) {
    const int blockIndex = nextRandomBlockIndex();
    BlockState::Id id = 0;
    if (section.tryGetRandomlyTicking(blockIndex, id)) {
      dispatchRandomTickIndex(*this, access, sectionBlockX, sectionBaseY,
                              sectionBlockZ, blockIndex, id);
    }
  }
}

void World::tickRandomChunkSection(int chunkX, int sectionBaseY, int chunkZ,
                                   Section &section, int tickSpeed,
                                   RandomTickAccess &access) {
  std::uint16_t tickingCount = section.tickingBlockCount;
  if (tickingCount == 0)
    return;
  ++randomTickStats.sectionsVisited;

  // Lithium ServerLevelMixin: when ticking blocks <= 384, draw random index r;
  // only if r < tickingCount (N/4096 chance) scan from minisection offset.
  if (tickingCount <= kRandomTickSparseThreshold) {
    ++randomTickStats.sectionsSparsePath;
    randomTickStats.positionsProbed += static_cast<std::uint64_t>(tickSpeed);
    for (int p = 0; p < tickSpeed; ++p) {
      const int r = nextRandomBlockIndex();
      if (r < tickingCount) {
        tickSparseRandomTickHit(chunkX, sectionBaseY, chunkZ, section, r,
                                access);
        tickingCount = section.tickingBlockCount;
      }
    }
    return;
  }

  tickDenseRandomTickSection(chunkX, sectionBaseY, chunkZ, section, tickSpeed,
                             access);
}

void World::tickChunks() {
  // TODO: weather/precipitation/ice-and-snow and remaining chunk-tick systems
  // are absent (requires biomes, climate, and weather state).
  if (randomTickSpeed <= 0 || randomTickingChunks_.empty())
    return;
  if (randomTickingDirty_) {
    std::sort(randomTickingChunks_.begin(), randomTickingChunks_.end(),
              [](const TickingChunkEntry &a, const TickingChunkEntry &b) {
                return a.key < b.key;
              });
    for (std::size_t i = 0; i < randomTickingChunks_.size(); ++i) {
      randomTickingChunks_[i].chunk->randomTickIndex =
          static_cast<std::uint32_t>(i);
    }
    randomTickingDirty_ = false;
  }
  randomTickChunkScratch_ = randomTickingChunks_;
  WorldRandomTickAccess access(*this);
  const int maxSections = sectionCount();
  for (const TickingChunkEntry &entry : randomTickChunkScratch_) {
    Chunk *chunk = entry.chunk;
    if (!chunk->loaded)
      continue;
    std::uint32_t mask = chunk->randomTickingSectionMask;
    if (mask == 0)
      continue;
    if (entityTickPositionReady &&
        !entityTickPositionReady(Pos{entry.x * 16, minY_, entry.z * 16}))
      continue;
    ++randomTickStats.chunksVisited;
    while (mask != 0) {
      const int si = std::countr_zero(mask);
      mask &= (mask - 1);
      if (si >= maxSections)
        break;
      Section &section = chunk->sections[static_cast<std::size_t>(si)];
      if (!section.isRandomlyTicking())
        continue;
      tickRandomChunkSection(entry.x, minY_ + si * 16, entry.z, section,
                             randomTickSpeed, access);
    }
  }
}

// TODO: weather, biomes, and the remaining world systems are absent.
void World::tick() {
  const auto worldTickStart = std::chrono::steady_clock::now();
  struct TickLightGuard {
    World &world;
    ~TickLightGuard() {
      world.inWorldTick_ = false;
      world.flushPendingLightCells();
    }
  };
  inWorldTick_ = true;
  TickLightGuard tickLightGuard{*this};
  ++time_;
  handlingTick_ = true;
  // ServerLevel.tick: blockTicks.tick(gameTime, 65536, this::tickBlock).
  // LevelTicks collects the due ticks first, then runs them; ticks scheduled
  // during execution stay pending for the next tick.
  tickQueue_->tick(*this, time_, 65536,
                   [this](Pos pos, const Block *block) {
                     tickBlockAt(pos, block);
                   });
  // ServerLevel.tick: fluidTicks.tick(gameTime, 65536, this::tickFluid) runs
  // right after blockTicks and before ChunkSource.tick, with its own budget.
  fluidTickQueue_->tick(*this, time_, 65536,
                        [this](Pos pos, const Fluid *fluid) {
                          tickFluidAt(pos, fluid);
                        });
  // ServerLevel.tick -> ServerChunkCache.tick -> ServerChunkCache.tickChunks:
  // random block/fluid ticks run after scheduled ticks and before broadcasting
  // dirty block updates.
  const auto rtStart = std::chrono::steady_clock::now();
  tickChunks();
  const auto rtEnd = std::chrono::steady_clock::now();
  randomTickStats.lastTickNs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(rtEnd - rtStart)
          .count());
  // ServerChunkCache.tick broadcasts ChunkHolder's dirty block positions
  // after scheduled ticks and before block events. Event-induced piston
  // source changes stay pending until the following chunk broadcast phase.
  if (lightService_)
    lightService_->drainPublished();
  if (blockUpdatesBroadcastObserver)
    blockUpdatesBroadcastObserver();
  // ServerChunkCache.tick: ChunkMap.tick/ServerEntity.sendChanges follows
  // chunk broadcasts and precedes block events and this tick's entity motion.
  if (entityTrackingBroadcastObserver)
    entityTrackingBroadcastObserver();
  runEvents();
  handlingTick_ = false;
  // ServerLevel.tick: entities, then block entities.
  tickEntities();
  if (traceUpdates)
    log.push_back({"block_entities", {}, ""});
  pistonTickBlockEntities(*this);
  tickContainerBlockEntities(*this);
  if (blockEntityTicker)
    blockEntityTicker(*this);
  // Observe removals caused by block entities without altering the core's
  // existing cleanup timing. Network trackers must not wait one more tick.
  broadcastEntityRemovals();
  if (cleanup_.autoAdvance && cleanup_.manager) {
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - worldTickStart)
            .count();
    cleanup_.manager->advanceTick(elapsedMs);
  }
}
} // namespace redstone
