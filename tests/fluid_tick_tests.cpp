#include "redstone/engine.hpp"
#include "redstone/fluid.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

// F-2: ServerLevel keeps two independent LevelTicks queues (`blockTicks` and
// `fluidTicks`), each collected and drained in the same tick with its own 65536
// budget. The fluid queue dispatches on the *exact* fluid identity
// (`fluidState.is(type)`, ServerLevel.tickFluid), so a source tick must not fire
// flowing logic and the two fluids must never share entries.

using namespace redstone;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

void loadAround(World &world) {
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z)
      world.loadChunk(x, z);
}

BlockState waterLevel(int level) {
  return Blocks::WATER.defaultState().with(
      PropertyKind::Level, static_cast<std::uint8_t>(level));
}

// The two queues are separate containers: scheduling one kind must not appear in
// the other's count, snapshot or dedupe set.
void queuesAreIndependent() {
  World world;
  loadAround(world);
  const Pos waterPos{0, 64, 0};
  const Pos sandPos{2, 64, 0};
  world.rawSetBlock(waterPos, waterLevel(0));

  require(world.scheduledTickCount() == 0, "block queue starts empty");
  require(world.fluidTickCount() == 0, "fluid queue starts empty");

  // Sand's onPlace schedules a block tick at delay 2. The position must start
  // empty so the state really changes and the callback fires.
  world.setBlock(sandPos, Blocks::SAND.defaultState());
  require(world.scheduledTickCount() == 1, "block tick scheduled");
  require(world.fluidTickCount() == 0, "block tick did not enter the fluid queue");

  world.scheduleFluidTick(waterPos, Fluids::water(), 1);
  require(world.fluidTickCount() == 1, "fluid tick scheduled");
  require(world.scheduledTickCount() == 1, "fluid tick did not enter the block queue");

  // Each queue dedupes on (pos, identity) independently.
  require(!world.scheduleFluidTick(waterPos, Fluids::water(), 5),
          "duplicate fluid tick is rejected");
  require(world.hasFluidTick(waterPos, Fluids::water()), "fluid tick is present");
  require(!world.hasFluidTick(waterPos, Fluids::lava()),
          "the same position with another fluid is not the same tick");
  // A second fluid at the same position is a distinct entry.
  require(world.scheduleFluidTick(waterPos, Fluids::lava(), 1),
          "another fluid at the same position is a separate entry");
  require(world.fluidTickCount() == 2, "two fluid entries at one position");
}

// ServerLevel.tick runs blockTicks, then fluidTicks, then the chunk stage, then
// block events. This drives all four in one tick and asserts the observed order,
// so moving the fluid phase before the block phase or after the events fails.
// The trace log is the single timeline: the broadcast hook appends to it, and
// traceUpdates already records the block tick, the fluid tick and the event.
void phaseOrderWithinOneTick() {
  World world;
  loadAround(world);
  const Pos blockPos{0, 64, 0};
  const Pos fluidPos{1, 64, 0};
  const Pos notePos{2, 64, 0};
  world.rawSetBlock(blockPos, Blocks::SMOOTH_STONE.defaultState());
  world.rawSetBlock(fluidPos, waterLevel(3));
  world.rawSetBlock(notePos, Blocks::NOTE_BLOCK.defaultState());
  world.traceUpdates = true;

  // The chunk broadcast has no built-in trace entry, so record it in place.
  world.blockUpdatesBroadcastObserver = [&world] {
    world.log.push_back({"broadcast", {}, ""});
  };
  // A note block event, which runEvents accepts and traces as "block_event".
  world.blockEvent(notePos, Blocks::NOTE_BLOCK, 0, 0);

  // Both ticks are due on the very next tick: trigger 0 is already <= gameTime
  // once World::tick advances it.
  require(world.scheduleTick(ScheduledTick{blockPos, BlockRef{Blocks::SMOOTH_STONE},
                                           0, 0, 0}),
          "schedule block tick");
  require(world.scheduleFluidTick(fluidPos, Fluids::flowingWater(), 0),
          "schedule fluid tick");

  world.tick();

  auto order = std::vector<std::string>{};
  for (const auto &entry : world.log) {
    if (entry.kind == "scheduled_tick")
      order.push_back("block_tick");
    else if (entry.kind == "fluid_tick")
      order.push_back("fluid_tick");
    else if (entry.kind == "broadcast")
      order.push_back("broadcast");
    else if (entry.kind == "block_event")
      order.push_back("block_event");
  }
  require(order.size() == 4,
          "one block tick, one fluid tick, one broadcast and one block event");

  const auto indexOf = [&order](const std::string &name) {
    for (std::size_t i = 0; i < order.size(); ++i)
      if (order[i] == name)
        return i;
    return order.size();
  };
  require(indexOf("block_tick") < indexOf("fluid_tick"),
          "the block tick phase precedes the fluid tick phase");
  require(indexOf("fluid_tick") < indexOf("broadcast"),
          "the fluid tick phase precedes the chunk broadcast");
  require(indexOf("broadcast") < indexOf("block_event"),
          "the chunk broadcast precedes block events");

  // Both queues were collected in this same tick; neither was deferred.
  require(world.scheduledTickCount() == 0, "the block queue drained this tick");
  require(world.fluidTickCount() == 0, "the fluid queue drained this tick");
}

// A fluid tick that is already due at the start of the tick is collected and run
// in that same tick rather than waiting one more tick.
void dueFluidTickRunsInTheSameTick() {
  World world;
  loadAround(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0));
  world.traceUpdates = true;
  require(world.scheduleFluidTick(pos, Fluids::water(), 0), "schedule due tick");
  world.tick();
  bool dispatched = false;
  for (const auto &entry : world.log)
    if (entry.kind == "fluid_tick" && entry.pos == pos)
      dispatched = true;
  require(dispatched, "a due fluid tick dispatches on the first tick");
}


// F-7: an explicit minecraft:empty tick is a REAL identity and the queue
// accepts it, so it must be consumed observably. Fluid.java:61-62's empty
// fluid tick does nothing: the entry drains, no block is written.
void explicitEmptyTickIsInert() {
  World world;
  loadAround(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0));
  world.rawSetBlock({0, 65, 0}, Blocks::AIR.defaultState());
  const auto before = world.getBlock(pos);
  world.traceUpdates = true;
  require(world.scheduleFluidTick(pos, Fluids::empty(), 1),
          "the queue accepts an explicit empty identity");
  world.tick();
  require(world.fluidTickCount() == 0, "the empty tick was consumed");
  // It is not re-scheduled and it did not run the flow algorithm.
  bool ran = false;
  for (const auto &entry : world.log)
    if (entry.kind == "fluid_tick") ran = true;
  require(!ran, "an empty fluid tick must not reach the flow logic");
  require(world.getBlock(pos) == before,
          "an empty fluid tick writes no block");
}

// F-7: a tick that was queued and then sees AIR at dispatch. The world still
// holds a matching identity only if it decodes to it, so this covers the
// 'read air' path: the entry drains and the cell is untouched.
void staleTickReadingAirIsInert() {
  World world;
  loadAround(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0));
  world.traceUpdates = true;
  require(world.scheduleFluidTick(pos, Fluids::water(), 1),
          "schedule a water tick");
  // Clear the cell before the tick runs: dispatch now reads air.
  world.rawSetBlock(pos, Blocks::AIR.defaultState());
  world.tick();
  require(world.fluidTickCount() == 0, "the stale entry was drained");
  bool dispatched = false, skipped = false;
  for (const auto &entry : world.log) {
    if (entry.kind == "fluid_tick" && entry.pos == pos) dispatched = true;
    if (entry.kind == "fluid_tick_skipped" && entry.pos == pos) skipped = true;
  }
  require(!dispatched, "a tick that now reads air does not run the fluid logic");
  require(skipped, "the skipped dispatch is traced, not silently dropped");
  require(world.getBlock(pos).isAir(),
          "a skipped tick writes no block (the cell stays air)");
}
// ServerLevel.tickFluid re-reads the position and skips a stale identity: a tick
// recorded for water must not run when the block is now lava.
void staleIdentityIsSkipped() {
  World world;
  loadAround(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0));
  world.traceUpdates = true;
  require(world.scheduleFluidTick(pos, Fluids::water(), 1), "schedule water tick");

  // Replace the water with lava before the tick runs.
  world.rawSetBlock(pos, Blocks::LAVA.defaultState());
  world.tick();
  require(world.fluidTickCount() == 0, "the entry was drained");
  bool dispatched = false;
  for (const auto &entry : world.log)
    if (entry.kind == "fluid_tick")
      dispatched = true;
  require(!dispatched, "a stale water tick did not dispatch against lava");
}

// A flowing tick must not match the source fluid at the same position: the
// dispatch uses exact identity, not the isSame family test.
void flowingVersusSource() {
  World world;
  loadAround(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0)); // source water
  world.traceUpdates = true;

  require(world.scheduleFluidTick(pos, Fluids::flowingWater(), 1),
          "schedule flowing tick against a source block");
  world.tick();
  bool dispatched = false;
  for (const auto &entry : world.log)
    if (entry.kind == "fluid_tick")
      dispatched = true;
  require(!dispatched, "a flowing_water tick does not fire for source water");

  // The exact identity does fire.
  world.log.clear();
  require(world.scheduleFluidTick(pos, Fluids::water(), 1), "schedule source tick");
  world.tick();
  for (const auto &entry : world.log)
    if (entry.kind == "fluid_tick")
      dispatched = true;
  require(dispatched, "the source tick fires for source water");
}

// LevelTicks keeps pending ticks for a chunk that is not ticking and resumes
// when it ticks again. The chunk is genuinely unloaded here, and the entry must
// dispatch exactly once after the reload.
void unloadedChunkHoldsTicks() {
  World world;
  loadAround(world);
  const Pos pos{3, 64, 3};
  world.rawSetBlock(pos, waterLevel(0));
  world.traceUpdates = true;
  require(world.scheduleFluidTick(pos, Fluids::water(), 2), "schedule far tick");
  require(world.fluidTickCount() == 1, "tick is pending");

  // Unload before it is due: the container stays registered and the entry must
  // survive both the due tick and several further ticks.
  world.unloadChunk(0, 0);
  for (int i = 0; i < 5; ++i)
    world.tick();
  require(world.fluidTickCount() == 1,
          "a tick for an unloaded chunk is retained, not dropped or run");

  // Reloading resumes it exactly once.
  world.loadChunk(0, 0);
  world.log.clear();
  world.tick();
  int dispatched = 0;
  for (const auto &entry : world.log)
    if (entry.kind == "fluid_tick" && entry.pos == pos)
      ++dispatched;
  require(dispatched == 1, "the retained tick dispatches exactly once");
  // The retained entry itself is gone; a new continuation tick at the same
  // position is expected now that F-5 wires setBlock and the spread re-schedules.
  for (const auto &entry : world.fluidTicks())
    require(!(entry.first.pos == pos && entry.first.trigger == 5),
            "the retained entry (trigger 5) was consumed");
}

// LevelTicks.tick(tick, 65536, ...): each queue enforces its own 65536 cap per
// tick. Both queues are loaded beyond the cap so neither budget can cover the
// other's backlog, and the second tick drains the remainder.
void budgetsAreSeparate() {
  World world;
  // 256 blocks along x and 256 along y gives 65536 distinct positions per z
  // layer; two z layers give more than the cap.
  for (int cx = 0; cx < 16; ++cx)
    for (int cz = 0; cz <= 1; ++cz)
      world.loadChunk(cx, cz);

  constexpr int kEntries = 65537;
  const auto posOf = [](int i) {
    return Pos{i & 255, 64 + ((i >> 8) & 255), i >> 16};
  };
  // Both queues are filled *before* the first tick, so a single shared per-tick
  // budget would be visible: blockTicks runs first and would starve fluidTicks.
  // Draining one queue before filling the other could not distinguish that.
  // This case is about the queue budget, not fluid behaviour, so it queues the
  // inert empty fluid: its dispatch returns before any spread, which keeps the
  // counts exact now that F-5 lets a real fluid tick write blocks and queue more.
  for (int i = 0; i < kEntries; ++i) {
    const Pos pos = posOf(i);
    require(world.scheduleFluidTick(pos, FluidRef(Fluids::empty()), 0),
            "schedule fluid tick " + std::to_string(i));
    require(world.scheduleTick(ScheduledTick{pos, BlockRef{Blocks::SMOOTH_STONE},
                                             0, 0, 0}),
            "schedule block tick " + std::to_string(i));
  }
  require(world.fluidTickCount() == static_cast<std::size_t>(kEntries),
          "all fluid ticks are queued");
  require(world.scheduledTickCount() == static_cast<std::size_t>(kEntries),
          "all block ticks are queued");

  // One tick: each queue spends its own 65536 and keeps exactly one entry, so
  // neither budget could have been consumed by the other queue.
  world.tick();
  require(world.scheduledTickCount() == 1,
          "the block queue consumes 65536 with the fluid backlog still present");
  require(world.fluidTickCount() == 1,
          "the fluid queue still consumes 65536 in that same tick");

  world.tick();
  require(world.scheduledTickCount() == 0, "the block queue drains its last entry");
  require(world.fluidTickCount() == 0, "the fluid queue drains its last entry");
}

// Restored fluid ticks (LevelChunkTicks.scheduleUnchecked) keep duplicates, the
// same way saved block lists do.
void restoredTicksAreUnchecked() {
  World world;
  loadAround(world);
  const Pos pos{0, 64, 0};
  world.rawSetBlock(pos, waterLevel(0));
  ScheduledTick tick{pos, BlockRef{}, 5, 0, -1};
  require(world.restoreFluidTick(tick, Fluids::water()), "restore first");
  require(world.restoreFluidTick(tick, Fluids::water()), "restore duplicate");
  require(world.fluidTickCount() == 2, "duplicates stay in the queue");
  require(world.hasFluidTick(pos, Fluids::water()), "the dedupe set holds one");
}

// Scheduling into a chunk that was never loaded must be dropped, not crash.
void scheduleIntoUnloadedChunkFails() {
  World world;
  world.loadChunk(0, 0);
  require(!world.scheduleFluidTick({128, 64, 0}, Fluids::water(), 1),
          "scheduling into an unloaded chunk is dropped");
  require(world.fluidTickCount() == 0, "nothing was queued");
}

// A default-constructed FluidRef carries no identity. Queueing it would leave a
// null target for the snapshot to dereference, so every entry point must reject
// it -- while Fluids::empty() stays a legitimate identity.
void nullFluidHandleIsRejected() {
  World world;
  world.loadChunk(0, 0);
  const Pos pos{0, 64, 0};
  const FluidRef nullHandle;

  require(nullHandle.empty(), "the default handle is empty");
  require(!world.scheduleFluidTick(pos, nullHandle, 1),
          "the delay overload rejects a null handle");
  require(!world.scheduleFluidTick(ScheduledTick{pos, BlockRef{}, 1, 0, 0},
                                   nullHandle),
          "the explicit-tick overload rejects a null handle");
  require(!world.restoreFluidTick(ScheduledTick{pos, BlockRef{}, 1, 0, -1},
                                  nullHandle),
          "restore rejects a null handle");
  require(world.fluidTickCount() == 0, "nothing entered the queue");
  require(world.fluidTicks().empty(), "the snapshot stays empty");
  require(!world.hasFluidTick(pos, nullHandle), "no entry is reported");

  // An unknown name also resolves to a null handle rather than registering.
  const FluidRef unknown{"minecraft:not_a_fluid"};
  require(unknown.empty(), "an unknown name yields an empty handle");
  require(!world.scheduleFluidTick(pos, unknown, 1),
          "an unknown name cannot be scheduled");

  // Fluids::empty() is a real registered identity, so it passes the null-handle
  // gate. This case only covers queuing and the snapshot; what a dispatch at an
  // air position does is not asserted here.
  require(!FluidRef(Fluids::empty()).empty(),
          "the registered empty fluid is a valid identity");
  require(world.scheduleFluidTick(pos, FluidRef(Fluids::empty()), 1),
          "the empty fluid can be scheduled");
  require(world.fluidTickCount() == 1, "the empty-fluid entry is queued");
  const auto ticks = world.fluidTicks();
  require(ticks.size() == 1 &&
              ticks[0].second.str() == "minecraft:empty",
          "the snapshot reports the empty fluid without dereferencing null");
}

// The snapshot reports the pending fluid ticks with their fluid identity and
// drain order, so tools and tests can inspect the queue without draining it.
void snapshotReportsFluidTicks() {
  World world;
  loadAround(world);
  const Pos a{0, 64, 0};
  const Pos b{1, 64, 0};
  world.rawSetBlock(a, waterLevel(0));
  world.rawSetBlock(b, Blocks::LAVA.defaultState());
  require(world.scheduleFluidTick(a, Fluids::water(), 9), "schedule a");
  require(world.scheduleFluidTick(b, Fluids::lava(), 3), "schedule b");
  const auto ticks = world.fluidTicks();
  require(ticks.size() == 2, "snapshot has both ticks");
  require(ticks[0].first.trigger < ticks[1].first.trigger,
          "snapshot is ordered by trigger");
  require(ticks[0].second.str() == "minecraft:lava", "first is lava");
  require(ticks[1].second.str() == "minecraft:water", "second is water");
  require(world.fluidTickCount() == 2, "the snapshot did not drain the queue");
}
} // namespace

int main() {
  try {
    queuesAreIndependent();
    phaseOrderWithinOneTick();
    dueFluidTickRunsInTheSameTick();
    staleIdentityIsSkipped();
    explicitEmptyTickIsInert();
    staleTickReadingAirIsInert();
    flowingVersusSource();
    unloadedChunkHoldsTicks();
    budgetsAreSeparate();
    restoredTicksAreUnchecked();
    scheduleIntoUnloadedChunkFails();
    nullFluidHandleIsRejected();
    snapshotReportsFluidTicks();
    std::cout << "fluid tick tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluid tick test failure: " << error.what() << "\n";
    return 1;
  }
}
