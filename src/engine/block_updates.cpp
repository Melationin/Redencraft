#include "engine/internal.hpp"

namespace redstone {
void World::setPistonEntity(Pos pos,
                            std::shared_ptr<PistonMovingBlockEntity> entity) {
  // LevelChunk.setBlockEntity: replacing an entry marks the previous one
  // removed, so a stale owner can never be ticked again.
  auto &current = pistonEntities[pos];
  if (!current) {
    const auto chunk = chunkKey(pos.x >> 4, pos.z >> 4);
    pistonPositionsByChunk_[chunk].push_back(pos);
  }
  if (current && current != entity)
    current->live = false;
  current = std::move(entity);
  // LevelChunk.updateBlockEntityTicker: rebind the ticker that already owns
  // this position (same list slot, same order) instead of appending a new one.
  if (auto it = pistonTickerSlots.find(pos); it != pistonTickerSlots.end()) {
    it->second->entity = current;
    if (pistonEntityMutationObserver)
      pistonEntityMutationObserver(pos);
    return;
  }
  auto slot = std::make_unique<PistonTickerSlot>();
  slot->pos = pos;
  slot->entity = current;
  PistonTickerSlot *raw = slot.get();
  pistonTickerSlots[pos] = raw;
  // Level.addBlockEntityTicker: a ticker created while the pass runs waits in
  // the pending list and first ticks on the following pass.
  (tickingPistonBlockEntities ? pendingPistonTickers : pistonTickers)
      .push_back(std::move(slot));
  if (pistonEntityMutationObserver)
    pistonEntityMutationObserver(pos);
}
void World::removePistonEntity(Pos pos, bool notify) {
  // LevelChunk.removeBlockEntity: the position table drops the entity at once.
  if (auto it = pistonEntities.find(pos); it != pistonEntities.end()) {
    if (it->second)
      it->second->live = false;
    pistonEntities.erase(pos);
    const auto key = chunkKey(pos.x >> 4, pos.z >> 4);
    auto positions = pistonPositionsByChunk_.find(key);
    if (positions != pistonPositionsByChunk_.end()) {
      std::erase(positions->second, pos);
      if (positions->second.empty())
        pistonPositionsByChunk_.erase(positions);
    }
  }
  // removeBlockEntityTicker: rebind the ticker to nothing. The list slot stays
  // until sweepPistonTickers(), and it keeps owning the entity until then, so
  // a pass that already referenced it cannot see it freed.
  if (auto it = pistonTickerSlots.find(pos); it != pistonTickerSlots.end()) {
    it->second->removed = true;
    pistonTickerSlots.erase(pos);
  }
  if (notify && pistonEntityMutationObserver)
    pistonEntityMutationObserver(pos);
}
void World::mergePendingPistonTickers() {
  if (pendingPistonTickers.empty())
    return;
  pistonTickers.reserve(pistonTickers.size() + pendingPistonTickers.size());
  for (auto &slot : pendingPistonTickers)
    pistonTickers.push_back(std::move(slot));
  pendingPistonTickers.clear();
}
void World::sweepPistonTickers() {
  // Level.tickBlockEntities' iterator.remove(), as one compaction: surviving
  // tickers keep their relative registration order.
  std::size_t kept = 0;
  for (std::size_t i = 0; i < pistonTickers.size(); ++i) {
    if (pistonTickers[i]->removed)
      continue;
    if (kept != i)
      pistonTickers[kept] = std::move(pistonTickers[i]);
    ++kept;
  }
  pistonTickers.resize(kept);
}

void World::setContainerEntity(Pos pos,
                               std::shared_ptr<ContainerBlockEntity> entity,
                               bool registerTicker) {
  auto &current = containerEntities[pos];
  if (!current) {
    const auto chunk = chunkKey(pos.x >> 4, pos.z >> 4);
    containerPositionsByChunk_[chunk].push_back(pos);
  }
  if (current && current != entity) {
    current->removed = true;
    current->wakeWatchers(*this);
  }
  current = std::move(entity);
  if (registerTicker && current) {
    registerContainerTicker(pos, current);
  } else if (auto it = containerTickerSlots.find(pos);
             it != containerTickerSlots.end()) {
    it->second->removed = true;
    containerTickerSlots.erase(pos);
  }
}

void World::registerContainerTicker(
    Pos pos, std::shared_ptr<ContainerBlockEntity> entity) {
  if (!entity)
    return;
  if (auto it = containerTickerSlots.find(pos); it != containerTickerSlots.end()) {
    it->second->entity = std::move(entity);
    it->second->sleepMode = ContainerSleepMode::Active;
    it->second->removed = false;
    return;
  }
  auto slot = std::make_unique<ContainerTickerSlot>();
  slot->pos = pos;
  slot->entity = std::move(entity);
  ContainerTickerSlot *raw = slot.get();
  containerTickerSlots[pos] = raw;
  (tickingContainerBlockEntities ? pendingContainerTickers : containerTickers)
      .push_back(std::move(slot));
}

void World::removeContainerEntity(Pos pos, bool dropContents, bool notify) {
  if (auto it = containerEntities.find(pos); it != containerEntities.end()) {
    auto existing = it->second;
    if (existing) {
      if (dropContents)
        dropContainerContents(*this, pos, *existing);
      existing->removed = true;
      existing->wakeWatchers(*this);
    }
    containerEntities.erase(pos);
    const auto key = chunkKey(pos.x >> 4, pos.z >> 4);
    auto positions = containerPositionsByChunk_.find(key);
    if (positions != containerPositionsByChunk_.end()) {
      std::erase(positions->second, pos);
      if (positions->second.empty())
        containerPositionsByChunk_.erase(key);
    }
  }
  if (auto it = containerTickerSlots.find(pos); it != containerTickerSlots.end()) {
    it->second->removed = true;
    containerTickerSlots.erase(pos);
  }
  if (notify) {
    updateNeighborsAt(pos, Blocks::TRAPPED_CHEST);
    updateNeighborsAt(pos.relative(Direction::Down), Blocks::TRAPPED_CHEST);
  }
}

std::shared_ptr<ContainerBlockEntity> World::containerEntity(Pos pos) const {
  auto it = containerEntities.find(pos);
  return it == containerEntities.end() ? nullptr : it->second;
}

void World::mergePendingContainerTickers() {
  if (pendingContainerTickers.empty())
    return;
  containerTickers.reserve(containerTickers.size() +
                           pendingContainerTickers.size());
  for (auto &slot : pendingContainerTickers)
    containerTickers.push_back(std::move(slot));
  pendingContainerTickers.clear();
}

void World::sweepContainerTickers() {
  std::size_t kept = 0;
  for (std::size_t i = 0; i < containerTickers.size(); ++i) {
    if (containerTickers[i]->removed)
      continue;
    if (kept != i)
      containerTickers[kept] = std::move(containerTickers[i]);
    ++kept;
  }
  containerTickers.resize(kept);
}

void World::watchHopperBlock(Pos pos,
                             const std::shared_ptr<HopperBlockEntity> &hopper) {
  if (!hopper)
    return;
  auto &list = hopperBlockWatchersByPos_[pos];
  for (const auto &w : list) {
    if (w.lock().get() == hopper.get())
      return;
  }
  list.push_back(hopper);
}

void World::watchHopperItemSection(
    std::uint64_t sectionKey,
    const std::shared_ptr<HopperBlockEntity> &hopper) {
  if (!hopper)
    return;
  auto &list = hopperItemWatchersBySection_[sectionKey];
  for (const auto &w : list) {
    if (w.lock().get() == hopper.get())
      return;
  }
  list.push_back(hopper);
}

void World::wakeHopperBlockWatchers(Pos pos) {
  auto it = hopperBlockWatchersByPos_.find(pos);
  if (it == hopperBlockWatchersByPos_.end())
    return;
  auto watchers = std::move(it->second);
  hopperBlockWatchersByPos_.erase(pos);
  for (const auto &weak : watchers) {
    if (auto h = weak.lock()) {
      if (!h->removed)
        h->wake(*this);
    }
  }
}

void World::wakeHopperItemWatchers(Vec3 pos) {
  const int sx = static_cast<int>(std::floor(pos.x)) >> 4;
  const int sy = static_cast<int>(std::floor(pos.y)) >> 4;
  const int sz = static_cast<int>(std::floor(pos.z)) >> 4;
  const std::uint64_t key = (static_cast<std::uint64_t>(sx) & 0x3FFFFFFull) << 38 |
                            (static_cast<std::uint64_t>(sz) & 0x3FFFFFFull) << 12 |
                            (static_cast<std::uint64_t>(sy) & 0xFFFull);
  auto it = hopperItemWatchersBySection_.find(key);
  if (it == hopperItemWatchersBySection_.end())
    return;
  auto watchers = std::move(it->second);
  hopperItemWatchersBySection_.erase(key);
  for (const auto &weak : watchers) {
    if (auto h = weak.lock()) {
      if (!h->removed)
        h->wake(*this);
    }
  }
}

bool World::setBlock(Pos p, BlockState state, int flags, int limit) {
  if (p.y < minY_ || p.y >= maxY_)
    return false;
  const int targetCx = chunkIndex(p.x);
  const int targetCz = chunkIndex(p.z);
  Chunk *changedChunk = chunkAt(targetCx, targetCz);
  if (!changedChunk)
    return false;
  const int targetSi = sectionIndex(p.y);
  Section &targetSection = changedChunk->sections[targetSi];
  const int localIndex =
      ((p.y & 15) << 8) | ((p.z & 15) << 4) | (p.x & 15);
  const BlockState old = targetSection.hasOnlyAir()
                             ? airState()
                             : BlockState::fromId(targetSection.get(localIndex));
  if (old == state)
    return false;
  targetSection.set(localIndex, state.id(), /*compact=*/true,
                    directResizeMutex(*changedChunk));
  updateChunkRandomTickMask(*changedChunk, chunkKey(targetCx, targetCz),
                            targetSi, targetSection.isRandomlyTicking());
  if (poiStorage_ &&
      (fastPoiTypeForState(old.id()) != PoiTypeId::None ||
       fastPoiTypeForState(state.id()) != PoiTypeId::None)) {
    poiStorage_->onBlockStateChange(PoiPos{p.x, p.y, p.z}, old.id(), state.id());
  }
  if (managedBlockMutationObserver)
    managedBlockMutationObserver(p, old, state);
  if (!changedChunk->heightmaps)
    changedChunk->heightmaps = std::make_unique<ChunkHeightmaps>(
        minY_, sectionCount() * 16);
  // The regular 16-aligned layout keeps section grouping and local indices in
  // a fixed relationship. Walk it once for all four maps: empty sections are
  // skipped by their header and non-empty sections expose only the target
  // column through Section::get. The unaligned contract keeps the existing
  // per-cell path because its y&15 local index does not match section bases.
  auto &heightmaps = *changedChunk->heightmaps;
  if ((minY_ & 15) != 0) {
    const int chunkX = chunkIndex(p.x), chunkZ = chunkIndex(p.z);
    heightmaps.update(p.x & 15, p.y, p.z & 15, state,
                      [this, chunkX, chunkZ](int x, int y, int z) {
                        return getBlock(
                            {chunkX * 16 + x, y, chunkZ * 16 + z});
                      });
  } else {
    heightmaps.updateFromColumn(
        p.x & 15, p.y, p.z & 15, state,
        [this, changedChunk](int localX, int startY, int localZ,
                            const std::array<bool, 4> &needed) {
          std::array<int, 4> firstAvailable;
          firstAvailable.fill(minY_);
          std::array<bool, 4> pending = needed;
          int remaining = 0;
          for (bool value : pending)
            remaining += value ? 1 : 0;
          if (remaining == 0 || startY < minY_)
            return firstAvailable;

          HeightmapScanStats *stats =
              heightmapScanStats.enabled ? &heightmapScanStats : nullptr;
          if (stats)
            ++stats->scans;
          const int column = ((localZ & 15) << 4) | (localX & 15);
          for (int sectionY = sectionIndex(startY);
               sectionY >= 0 && startY >= minY_ && remaining != 0;
               --sectionY) {
            if (stats)
              ++stats->visitedSections;
            const Section &section = changedChunk->sections[sectionY];
            const int sectionBase = minY_ + sectionY * 16;
            if (section.hasOnlyAir()) {
              if (stats)
                ++stats->skippedSections;
              startY = sectionBase - 1;
              continue;
            }
            for (; startY >= sectionBase && startY >= minY_ && remaining != 0;
                 --startY) {
              if (stats)
                ++stats->readCells;
              const BlockState below = BlockState::fromId(
                  section.get(((startY & 15) << 8) | column));
              const auto matches = ChunkHeightmaps::matchesFinalMaps(below);
              for (std::size_t i = 0; i < pending.size(); ++i) {
                if (pending[i] && matches[i]) {
                  firstAvailable[i] = startY + 1;
                  pending[i] = false;
                  --remaining;
                }
              }
            }
          }
          return firstAvailable;
        });
  }
  if (lightService_ &&
      lightOptical_->classForState(old.id()) !=
          lightOptical_->classForState(state.id())) {
    changedChunk->persistedLight.reset();
    const LightCellUpdate update{p.x, p.y, p.z, state.id()};
    if (inWorldTick_) {
      pendingLightCells_.push_back(update);
      if (!changedChunk->hasPendingLightCell) {
        changedChunk->hasPendingLightCell = true;
        pendingLightChunkKeys_.push_back(
            chunkKey(chunkIndex(p.x), chunkIndex(p.z)));
      }
    } else {
      const std::uint64_t seq = lightService_->reportBlockCell(update);
      if (seq > 0)
        changedChunk->requiredLightGeneration =
            std::max(changedChunk->requiredLightGeneration, seq);
    }
  }
  // TODO: reproduce dirty/client notifications, POI, and generic block-entity
  // pre-removal/creation for supported entity blocks.
  // The opt-in L-1 light handoff is below; propagation remains TODO(L-2a/L-2b).
  if (!old.sameBlock(state) && old.is(Blocks::MOVING_PISTON)) {
    if (!(flags & 256))
      pistonPreRemoveSideEffects(*this, p);
    removePistonEntity(p);
  }
  if (!old.sameBlock(state) && containerKindForState(old).has_value())
    preRemoveContainerSideEffects(*this, p, old, !(flags & 256));
  if (old.is(Blocks::COMPARATOR) && !state.is(Blocks::COMPARATOR))
    comparatorSignals_.erase(p);
  if (!old.sameBlock(state) && (flags & (1 | 64))) {
    // High-frequency fast path for rails; per-block hooks live on Block.
    if (isRailBlock(old))
      railAffectNeighborsAfterRemoval(*this, p, old, (flags & 64) != 0);
    else
      old.block().affectNeighborsAfterRemoval(*this, p, old, (flags & 64) != 0);
  }
  if (!getBlock(p).sameBlock(state))
    return false;
  if (insideFluidTick_)
    ++fluidWrites_;
  if (!(flags & 512)) {
    // LevelChunk.java:339-341: every write that survives the type check runs
    // onPlace, whatever the old block was. LiquidBlock.onPlace has no
    // same-type early return, so re-levelling water onto water (level 0 -> 1)
    // requests a fresh tick too; the sink's (pos, fluid) dedupe then decides.
    // High-frequency fast paths (air/fluid/piston/rail/falling); remaining blocks
    // dispatch via Block::onPlace function pointer.
    if (state.isAir()) {
    } else if (isLiquidBlock(state)) {
      liquidOnPlace(*this, p, state);
    } else if (piston(state)) {
      pistonOnPlace(*this, p, state, old);
    } else if (isRailBlock(state)) {
      railOnPlace(*this, p, state, old, (flags & 64) != 0);
    } else {
      if (state.block().isFalling() && !old.sameBlock(state))
        scheduleTick(p, state.block(), 2);
      state.block().onPlace(*this, p, state, old, (flags & 64) != 0);
    }
  } else if (containerKindForState(state).has_value()) {
    ensureContainerBlockEntity(*this, p, state);
  }
  if (old != state)
    wakeHopperBlockWatchers(p);
  if (getBlock(p) == state) {
    if ((flags & 2) && blockUpdateObserver)
      blockUpdateObserver(p, state, flags);
    if (flags & 1)
      updateNeighborsAt(p, old.block());
    if (!(flags & 16) && limit > 0) {
      updateIndirectNeighbourShapes(p, old, flags & ~33, limit - 1);
      updateNeighbourShapes(p, state, flags & ~33, limit - 1);
      updateIndirectNeighbourShapes(p, state, flags & ~33, limit - 1);
    }
  }
  return true;
}
std::optional<BlockState> World::coralPlacementState(Pos pos,
                                                     const BlockState &selected,
                                                     Direction clickedFace) {
  if (!isDeadCoralFan(selected) || clickedFace == Direction::Down)
    return std::nullopt;
  auto name = canonical(selected.name());
  const auto wall = name.find("_coral_wall_fan");
  if (wall != std::string::npos)
    name.replace(wall, 15, "_coral_fan");
  if (clickedFace != Direction::Up) {
    auto floor = name.find("_coral_fan");
    name.replace(floor, 10, "_coral_wall_fan");
  }
  // BaseCoralPlantTypeBlock.getStateForPlacement (which BaseCoralWallFanBlock
  // inherits) reads the fluid of the cell being filled and waterlogs the fan for
  // FluidTags.WATER && isFull() -- BaseCoralPlantTypeBlock.java:57-62. That is a
  // TAG test, not the exact-source test the rails and the walls use: falling
  // water[level=8] decodes to the flowing identity with amount 8 and still
  // satisfies both halves (FluidState.java:57-58 only checks amount == 8).
  const FluidState replacedFluid = fluidStateOf(getBlock(pos));
  const bool wet =
      fluidHasTag(replacedFluid.getType(), FluidTag::Water) && replacedFluid.isFull();
  auto state =
      defaultState(name).with("waterlogged", wet ? "true" : "false");
  if (clickedFace != Direction::Up)
    state = state.with("facing", directionName(clickedFace));
  return coralCanSurvive(*this, pos, state) ? std::optional<BlockState>(state)
                                            : std::nullopt;
}
BlockState World::updateShapeState(Pos p, const BlockState &state,
                                   Direction direction, Pos neighborPos,
                                   const BlockState &neighbor) {
  // High-frequency fast paths: air, fluids, falling tick, waterlogged fluid
  // tick; remaining per-block shape logic dispatches via Block::updateShape.
  if (state.isAir())
    return state;
  // FallingBlock.updateShape schedules on every direction, even if the block
  // currently has support. Scheduled-tick deduplication preserves the first due time.
  if (state.block().isFalling())
    scheduleTick(p, state.block(), 2);
  // LiquidBlock.updateShape: only a source on either side requests a fluid tick.
  if (isLiquidBlock(state)) {
    liquidUpdateShape(*this, p, state, neighbor);
    return state;
  }
  // SimpleWaterloggedBlock.updateShape schedules a water tick when waterlogged.
  if (state.block().isWaterloggedHost() && waterloggedOf(state))
    scheduleFluidTick(p, FluidRef(Fluids::water()),
                      Fluids::water().tickDelay());
  return state.block().updateShape(*this, p, state, direction, neighborPos,
                                   neighbor);
}
bool World::removeBlock(Pos p, bool movedByPiston) {
  // Level.java:276-280 reads the fluid BEFORE the write; for a waterlogged
  // host that is its own water source (SimpleWaterloggedBlock.getFluidState),
  // and for a liquid block it is the fluid the block already is.
  const FluidState fluidState = fluidStateOf(getBlock(p));
  return setBlock(p, fluidState.getType().createLegacyBlock(fluidState),
                  3 | (movedByPiston ? 64 : 0));
}
BlockState World::updateFromNeighbourShapes(Pos p, const BlockState &input) {
  auto state = input;
  for (auto d : shapeOrder) {
    auto neighborPos = p.relative(d);
    state = updateShapeState(p, state, d, neighborPos, getBlock(neighborPos));
  }
  return state;
}
void World::updateOrDestroy(Pos p, const BlockState &oldState,
                            const BlockState &newState, int flags, int limit) {
  if (oldState == newState)
    return;
  if (newState.isAir()) {
    // Level.destroyBlock resolves the actual current state, which can differ
    // from the earlier shape result after nested piston callbacks.
    auto current = getBlock(p);
    if (current.isAir())
      return;
    if (!(flags & 32)) {
      for (const auto &drop : blockDrops(current))
        spawnItem(p, drop.id, drop.count);
    }
    // TODO: game events and experience drops.
    // Level.destroyBlock writes the position's fluid legacy block, not air
    // (Level.java:288,298): a waterlogged host that fails its shape update
    // leaves its water behind, and a broken liquid leaves the same fluid.
    const FluidState fluidState = fluidStateOf(current);
    setBlock(p, fluidState.getType().createLegacyBlock(fluidState), 3, limit);
  } else
    setBlock(p, newState, flags & ~32, limit);
}
void World::neighborShapeChanged(Pos p, Direction d, Pos neighbor,
                                 const BlockState &state, int flags,
                                 int limit) {
  shapeUpdate(p, d, neighbor, state, flags, limit);
}
void World::updateNeighbourShapes(Pos p, const BlockState &state, int flags,
                                  int limit) {
  for (auto d : shapeOrder)
    shapeUpdate(p.relative(d), opposite(d), p, state, flags, limit);
}
void World::updateIndirectNeighbourShapes(Pos p, const BlockState &state,
                                         int flags, int limit) {
  redstoneUpdateIndirectNeighbourShapes(*this, p, state, flags, limit);
}
void World::affectNeighborsAfterRemoval(Pos p, const BlockState &state,
                                        bool movedByPiston) {
  state.block().affectNeighborsAfterRemoval(*this, p, state, movedByPiston);
}
void World::neighborChanged(Pos p, const BlockRef &source) {
  Update u{};
  u.kind = Update::Kind::Simple;
  u.pos = p;
  u.source = source;
  enqueue(std::move(u));
}
void World::neighborChanged(const BlockState &state, Pos p,
                            const BlockRef &source, bool movedByPiston) {
  Update u{};
  u.kind = Update::Kind::Full;
  u.pos = p;
  u.source = source;
  u.capturedState = state;
  u.movedByPiston = movedByPiston;
  enqueue(std::move(u));
}
void World::updateNeighborsAt(Pos p, const BlockRef &source,
                              std::optional<Direction> skip) {
  Update u{};
  u.kind = Update::Kind::Multi;
  u.pos = p;
  u.source = source;
  u.skip = skip;
  enqueue(std::move(u));
}
void World::shapeUpdate(Pos p, Direction d, Pos neighborPos,
                        const BlockState &neighbor, int flags, int limit) {
  Update u{};
  u.kind = Update::Kind::Shape;
  u.pos = p;
  u.direction = d;
  u.neighborPos = neighborPos;
  u.neighbor = neighbor;
  u.flags = flags;
  u.limit = limit;
  enqueue(std::move(u));
}
void World::enqueue(Update u) {
  bool running = updateCount_ > 0;
  bool tooMany = maxUpdates_ >= 0 && updateCount_ >= maxUpdates_;
  ++updateCount_;
  if (!tooMany) {
    if (running)
      added_.push_back(std::move(u));
    else
      stack_.push_back(std::move(u));
  } else if (updateCount_ - 1 == maxUpdates_)
    log.push_back({"neighbor_update_limit", u.pos, ""});
  if (!running)
    runUpdates();
}
void World::runUpdates() {
  try {
    while (!stack_.empty() || !added_.empty()) {
      for (auto it = added_.rbegin(); it != added_.rend(); ++it)
        stack_.push_back(std::move(*it));
      added_.clear();
      while (added_.empty()) {
        bool more = runNext(stack_.back());
        if (!more) {
          stack_.pop_back();
          break;
        }
      }
    }
  } catch (...) {
    stack_.clear();
    added_.clear();
    updateCount_ = 0;
    throw;
  }
  stack_.clear();
  added_.clear();
  updateCount_ = 0;
}
bool World::runNext(Update &u) {
  if (u.kind == Update::Kind::Simple) {
    executeNeighbor(u.pos, getBlock(u.pos), u.source, false);
    return false;
  }
  if (u.kind == Update::Kind::Full) {
    executeNeighbor(u.pos, u.capturedState, u.source, u.movedByPiston);
    return false;
  }
  if (u.kind == Update::Kind::Multi) {
    while (u.index < 6 && u.skip && neighborOrder[u.index] == *u.skip)
      ++u.index;
    if (u.index >= 6)
      return false;
    auto p = u.pos.relative(neighborOrder[u.index++]);
    auto src = u.source;
    while (u.index < 6 && u.skip && neighborOrder[u.index] == *u.skip)
      ++u.index;
    bool more = u.index < 6;
    executeNeighbor(p, getBlock(p), src, false);
    return more;
  }
  auto state = getBlock(u.pos);
  if (traceUpdates)
    log.push_back({"shape", u.pos, directionName(u.direction)});
  auto next =
      updateShapeState(u.pos, state, u.direction, u.neighborPos, u.neighbor);
  updateOrDestroy(u.pos, state, next, u.flags, u.limit);
  return false;
}
void World::executeNeighbor(Pos p, const BlockState &state,
                            const BlockRef &source, bool movedByPiston) {
  // TODO: orientation and movedByPiston for unsupported blocks.
  if (traceUpdates)
    log.push_back({"neighbor", p, source.str()});
  // High-frequency fast paths (air/piston/liquid/rail); remaining blocks
  // dispatch via Block::neighborChanged function pointer.
  if (state.isAir())
    return;
  if (piston(state))
    pistonNeighborChanged(*this, p, state);
  else if (isLiquidBlock(state))
    liquidNeighborChanged(*this, p, state);
  else if (isRailBlock(state))
    railNeighborChanged(*this, p, state, source, movedByPiston);
  else
    state.block().neighborChanged(*this, p, state, source, movedByPiston);
}
int World::getDirectSignal(Pos p, Direction d) const {
  return redstoneBlockDirectSignal(*this, p, getBlock(p), d);
}
int World::getDirectSignalTo(Pos p) const {
  int signal = 0;
  signal = std::max(signal, getDirectSignal(p.relative(Direction::Down),
                                            Direction::Down));
  if (signal >= 15)
    return signal;
  signal =
      std::max(signal, getDirectSignal(p.relative(Direction::Up), Direction::Up));
  if (signal >= 15)
    return signal;
  signal = std::max(
      signal, getDirectSignal(p.relative(Direction::North), Direction::North));
  if (signal >= 15)
    return signal;
  signal = std::max(
      signal, getDirectSignal(p.relative(Direction::South), Direction::South));
  if (signal >= 15)
    return signal;
  signal = std::max(
      signal, getDirectSignal(p.relative(Direction::West), Direction::West));
  if (signal >= 15)
    return signal;
  return std::max(
      signal, getDirectSignal(p.relative(Direction::East), Direction::East));
}
int World::getControlInputSignal(Pos p, Direction d, bool diodesOnly) const {
  const auto state = getBlock(p);
  if (diodesOnly)
    return isDiodeBlock(state) ? getDirectSignal(p, d) : 0;
  if (state.is(Blocks::REDSTONE_BLOCK))
    return 15;
  if (state.is(Blocks::REDSTONE_WIRE))
    return static_cast<int>(propertyValue(state, PropertyKind::Power));
  return isRedstoneSignalSource(*this, state)
             ? redstoneBlockDirectSignal(*this, p, state, d)
             : 0;
}
int World::getSignal(Pos p, Direction d) const {
  const auto s = getBlock(p);
  const int signal = redstoneBlockSignal(*this, p, s, d);
  return isRedstoneConductor(s) ? std::max(signal, getDirectSignalTo(p)) : signal;
}
int World::getBestNeighborSignal(Pos p) const {
  int best = 0;
  for (auto d : neighborOrder) {
    const int signal = getSignal(p.relative(d), d);
    if (signal >= 15)
      return 15;
    if (signal > best)
      best = signal;
  }
  return best;
}
bool World::hasSignal(Pos p, Direction d) const { return getSignal(p, d) > 0; }
bool World::hasNeighborSignal(Pos p) const {
  for (auto d : neighborOrder)
    if (getSignal(p.relative(d), d) > 0)
      return true;
  return false;
}
int World::comparatorOutput(Pos pos) const {
  auto it = comparatorSignals_.find(pos);
  return it == comparatorSignals_.end() ? 0 : it->second;
}
void World::setComparatorOutput(Pos pos, int value) {
  if (value == 0)
    comparatorSignals_.erase(pos);
  else
    comparatorSignals_[pos] = value;
}

void World::updateNeighbourForOutputSignal(Pos pos,
                                           const BlockRef &changedBlock) {
  for (Direction d :
       {Direction::North, Direction::East, Direction::South, Direction::West}) {
    Pos rp = pos.relative(d);
    if (!isLoaded(rp))
      continue;
    BlockState rs = getBlock(rp);
    if (rs.is(Blocks::COMPARATOR)) {
      neighborChanged(rs, rp, changedBlock, false);
    } else if (isRedstoneConductor(rs)) {
      rp = rp.relative(d);
      if (isLoaded(rp)) {
        rs = getBlock(rp);
        if (rs.is(Blocks::COMPARATOR))
          neighborChanged(rs, rp, changedBlock, false);
      }
    }
  }
}

} // namespace redstone
