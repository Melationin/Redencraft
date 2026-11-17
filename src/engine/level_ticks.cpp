#include "engine/internal.hpp"

namespace redstone {
bool ScheduledTickDrainOrder::operator()(const ScheduledTick &a,
                                         const ScheduledTick &b) const {
  if (a.trigger != b.trigger)
    return a.trigger > b.trigger;
  if (a.priority != b.priority)
    return a.priority > b.priority;
  return a.order > b.order;
}
bool ScheduledTickIntraOrder::operator()(const ScheduledTick &a,
                                         const ScheduledTick &b) const {
  if (a.priority != b.priority)
    return a.priority > b.priority;
  return a.order > b.order;
}
std::size_t BlockEventHash::operator()(const BlockEvent &event) const {
  const auto posHash =
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(event.pos.x)) *
       0x9E3779B97F4A7C15ull) ^
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(event.pos.y)) *
       0xC2B2AE3D27D4EB4Full) ^
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(event.pos.z)) *
       0x165667B19E3779F9ull);
  std::size_t hash =
      static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(event.block.block()));
  hash ^= static_cast<std::size_t>(posHash ^ (posHash >> 32));
  hash ^= static_cast<std::size_t>(event.type) * 0x9E3779B1u;
  hash ^= static_cast<std::size_t>(event.data) * 0x85EBCA6Bu;
  return hash;
}


void World::scheduleTick(Pos p, const BlockRef &block, int delay,
                         int priority) {
  // LevelAccessor.createTick: trigger = gameTime + delay, subTickOrder from the
  // monotonic counter. Callers pass the Block object, so no name lookup runs.
  // when the caller already passes a canonical name.
  if (!isLoaded(p))
    return;
  scheduleTick(ScheduledTick{p, block, time_ + delay, priority,
                             nextSubTickCount()});
}
bool World::scheduleTick(const ScheduledTick &tick) {
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  const bool changed = tickQueue_->schedule(key, tick, tick.block.block());
  if (changed && tickMutationObserver)
    tickMutationObserver(tick.pos);
  return changed;
}
bool World::restoreScheduledTick(const ScheduledTick &tick) {
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  const bool changed = tickQueue_->restore(key, tick, tick.block.block());
  if (changed && tickMutationObserver)
    tickMutationObserver(tick.pos);
  return changed;
}
bool World::hasScheduledTick(Pos p, const BlockRef &block) const {
  const auto key = chunkKey(chunkIndex(p.x), chunkIndex(p.z));
  auto *container = tickQueue_->containers.find(key);
  return container && (*container)->has(p, block.block());
}
bool World::willTickThisTick(Pos p, const BlockRef &block) const {
  return tickQueue_->willTickThisTick(p, block.block());
}
std::size_t World::scheduledTickCount() const { return tickQueue_->count(); }
std::vector<ScheduledTick> World::scheduledTicks() const {
  // Deterministic snapshot: chunk order, then each chunk's own DRAIN_ORDER.
  std::vector<std::uint64_t> keys;
  keys.reserve(tickQueue_->containers.size());
  tickQueue_->containers.forEach(
      [&](std::uint64_t key, BlockTickQueue::ChunkTicks *) { keys.push_back(key); });
  std::sort(keys.begin(), keys.end());
  std::vector<ScheduledTick> ticks;
  for (auto key : keys) {
    auto ordered = (*tickQueue_->containers.find(key))->heap;
    std::sort(ordered.begin(), ordered.end(),
              [](const QueuedTick<Block> &x, const QueuedTick<Block> &y) {
                return std::tie(x.trigger, x.priority, x.order) <
                       std::tie(y.trigger, y.priority, y.order);
              });
    for (const auto &entry : ordered)
      ticks.push_back(entry.toBlockScheduledTick());
  }
  return ticks;
}
ChunkTickSnapshot World::snapshotChunkTicks(int chunkX,
                                                 int chunkZ) const {
  ChunkTickSnapshot result;
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  if (const auto *container = tickQueue_->containers.find(key)) {
    result.blockTicks.reserve((*container)->heap.size());
    for (const auto &entry : (*container)->heap)
      result.blockTicks.push_back(entry.toBlockScheduledTick());
    std::sort(result.blockTicks.begin(), result.blockTicks.end(),
              [](const ScheduledTick &a, const ScheduledTick &b) {
                return std::tie(a.trigger, a.priority, a.order) <
                       std::tie(b.trigger, b.priority, b.order);
              });
  }
  if (const auto *container = fluidTickQueue_->containers.find(key)) {
    result.fluidTicks.reserve((*container)->heap.size());
    for (const auto &entry : (*container)->heap)
      result.fluidTicks.emplace_back(entry.toFluidScheduledTick(),
                                     FluidRef(*entry.target));
    std::sort(result.fluidTicks.begin(), result.fluidTicks.end(),
              [](const auto &a, const auto &b) {
                return std::tie(a.first.trigger, a.first.priority,
                                a.first.order) <
                       std::tie(b.first.trigger, b.first.priority,
                                b.first.order);
              });
  }
  return result;
}

std::size_t World::chunkTickCount(int chunkX, int chunkZ) const {
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  std::size_t result = 0;
  if (const auto *container = tickQueue_->containers.find(key))
    result += (*container)->count();
  if (const auto *container = fluidTickQueue_->containers.find(key))
    result += (*container)->count();
  return result;
}

ChunkTickSnapshot World::takeChunkTicks(int chunkX, int chunkZ) {
  ChunkTickSnapshot result = snapshotChunkTicks(chunkX, chunkZ);
  const std::uint64_t key = chunkKey(chunkX, chunkZ);
  if (auto *container = tickQueue_->containers.find(key)) {
    (*container)->releaseStorage();
    tickQueue_->nextTickForContainer.erase(key);
  }
  if (auto *container = fluidTickQueue_->containers.find(key)) {
    (*container)->releaseStorage();
    fluidTickQueue_->nextTickForContainer.erase(key);
  }
  if (tickMutationObserver)
    tickMutationObserver({chunkX * 16, minY_, chunkZ * 16});
  return result;
}

// ---------------------------------------------------------------------------
// ServerLevel.fluidTicks: same LevelTicks structure, keyed on the Fluid
// identity instead of the Block identity.
// ---------------------------------------------------------------------------
bool World::scheduleFluidTick(Pos p, const FluidRef &fluid, int delay,
                              int priority) {
  // LevelAccessor.createTick: trigger = gameTime + delay and the same monotonic
  // subTickOrder counter the block queue uses (Level.nextSubTickCount).
  if (!isLoaded(p) || fluid.empty())
    return false;
  return scheduleFluidTick(ScheduledTick{p, BlockRef{}, time_ + delay, priority,
                                         nextSubTickCount()},
                           fluid);
}
bool World::scheduleFluidTick(const ScheduledTick &tick, const FluidRef &fluid) {
  // A default-constructed FluidRef has no identity; queueing it would leave a
  // null target for the snapshot and the dispatch to dereference. Note that
  // Fluids::empty() is a real identity, so only the null handle is rejected.
  if (fluid.empty())
    return false;
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  const bool changed = fluidTickQueue_->schedule(key, tick, fluid.fluid());
  if (changed && tickMutationObserver)
    tickMutationObserver(tick.pos);
  return changed;
}
bool World::restoreFluidTick(const ScheduledTick &tick, const FluidRef &fluid) {
  if (fluid.empty())
    return false;
  const auto key = chunkKey(chunkIndex(tick.pos.x), chunkIndex(tick.pos.z));
  const bool changed = fluidTickQueue_->restore(key, tick, fluid.fluid());
  if (changed && tickMutationObserver)
    tickMutationObserver(tick.pos);
  return changed;
}
bool World::hasFluidTick(Pos p, const FluidRef &fluid) const {
  const auto key = chunkKey(chunkIndex(p.x), chunkIndex(p.z));
  auto *container = fluidTickQueue_->containers.find(key);
  return container && (*container)->has(p, fluid.fluid());
}
std::size_t World::fluidTickCount() const { return fluidTickQueue_->count(); }
std::vector<std::pair<ScheduledTick, FluidRef>> World::fluidTicks() const {
  std::vector<std::uint64_t> keys;
  keys.reserve(fluidTickQueue_->containers.size());
  fluidTickQueue_->containers.forEach(
      [&](std::uint64_t key, FluidTickQueue::ChunkTicks *) { keys.push_back(key); });
  std::sort(keys.begin(), keys.end());
  std::vector<std::pair<ScheduledTick, FluidRef>> ticks;
  for (auto key : keys) {
    auto ordered = (*fluidTickQueue_->containers.find(key))->heap;
    std::sort(ordered.begin(), ordered.end(),
              [](const QueuedTick<Fluid> &x, const QueuedTick<Fluid> &y) {
                return std::tie(x.trigger, x.priority, x.order) <
                       std::tie(y.trigger, y.priority, y.order);
              });
    for (const auto &entry : ordered)
      ticks.emplace_back(entry.toFluidScheduledTick(), FluidRef(*entry.target));
  }
  return ticks;
}
std::int64_t World::nextSubTickCount() {
  // Level.nextSubTickCount: subTickCount++ with Java long wraparound.
  const auto value = subTickCount_;
  subTickCount_ = nextSubTick(subTickCount_);
  return value;
}
void World::blockEvent(Pos p, const BlockRef &block, int type, int data) {
  // BlockEventData carries the Block identity; the interned name is our Block.
  blockEventQueue_->add({p, block, type, data});
}
std::size_t World::blockEventCount() const { return blockEventQueue_->size(); }
std::vector<BlockEvent> World::pendingBlockEvents() const {
  return blockEventQueue_->snapshot();
}
void World::observerNotify(Pos p, const BlockState &s) {
  auto d = directionOf(s, Direction::South);
  auto q = p.relative(opposite(d));
  neighborChanged(q, s.block());
  updateNeighborsAt(q, s.block(), d);
}
// ServerLevel.tickBlock: re-read the position and only dispatch when the block
// identity still matches. A stale tick silently does nothing.
void World::tickBlockAt(Pos pos, const Block *expected) {
  if (tickMutationObserver)
    tickMutationObserver(pos);
  if (traceUpdates)
    log.push_back({"scheduled_tick", pos,
                   expected ? expected->name() : std::string{}});
  const auto s = getBlock(pos);
  if (s.blockOrNull() != expected)
    return;
  if (s.isAir())
    return;
  if (isRailBlock(s))
    railScheduledTick(*this, pos, s);
  else
    s.block().tick(*this, pos, s);
}
// ServerLevel.tickFluid: `fluidState.is(type)` is the exact identity test, not
// the "is a source" test, so a flowing tick must not run source logic and vice
// versa. Flow behaviour itself is task F-3; until then a due tick only records
// itself so the queue can be exercised.
void World::tickFluidAt(Pos pos, const Fluid *expected) {
  if (tickMutationObserver)
    tickMutationObserver(pos);
  ++fluidDispatches_;
  if (!expected)
    return;
  const auto state = getBlock(pos);
  const FluidState fluidState = fluidStateOf(state);
  // An empty fluid has an empty tick (Fluid.java:61-62). A tick registered for
  // minecraft:empty -- which the queue accepts, since it is a real identity --
  // must be consumed without touching the block, otherwise the recompute would
  // see "no neighbours" and write air over whatever is there.
  //
  // Both inert cases are still traced: an entry that reached dispatch and did
  // nothing must be distinguishable from an entry that never existed, and the
  // trace is the only place that distinction survives.
  if (expected->isEmpty() || fluidState.isEmpty()) {
    if (traceUpdates)
      log.push_back({fluidState.is(*expected) ? "fluid_tick"
                                             : "fluid_tick_skipped",
                     pos, expected->name()});
    return;
  }
  // The queue dispatched an entry; only the identity check decides whether the
  // fluid's tick logic runs, so trace both outcomes distinctly.
  if (traceUpdates)
    log.push_back({fluidState.is(*expected) ? "fluid_tick" : "fluid_tick_skipped",
                   pos, expected->name()});
  if (!fluidState.is(*expected))
    return;
  // FlowingFluid.tick: recompute a non-source level, then spread.
  insideFluidTick_ = true;
  try {
    fluidTick(*this, pos, state, fluidState);
  } catch (...) {
    insideFluidTick_ = false;
    throw;
  }
  insideFluidTick_ = false;
}
// TODO: note particles, sound, game events, skull sounds and event broadcasts.
void World::runEvents() {
  // ServerLevel.runBlockEvents: drain the insertion-ordered set, defer events
  // for chunks that are not ticking, then re-add the deferred ones.
  blockEventsToReschedule_.clear();
  while (!blockEventQueue_->empty()) {
    BlockEvent e;
    blockEventQueue_->removeFirst(e);
    if (!isLoaded(e.pos) ||
        (blockTickPositionReady && !blockTickPositionReady(e.pos))) {
      blockEventsToReschedule_.push_back(std::move(e));
      continue;
    }
    if (traceUpdates)
      log.push_back({"block_event", e.pos, e.block.str()});
    const auto s = getBlock(e.pos);
    if (e.block.block() != s.blockOrNull())
      continue;
    bool accepted = false;
    if (piston(s))
      accepted = pistonBlockEvent(*this, e.pos, s, e.type, e.data);
    else
      accepted = s.block().triggerEvent(*this, e.pos, s, e.type, e.data);
    if (accepted && blockEventObserver)
      blockEventObserver(e);
  }
  for (auto &e : blockEventsToReschedule_)
    blockEventQueue_->add(e);
}
} // namespace redstone
