# Piston update-order audit

Compared against the local Java 26.1 sources `net/minecraft/world/level/block/piston/PistonBaseBlock.java`, `PistonStructureResolver.java`, and `PistonMovingBlockEntity.java`. Scope is the server-side dry block model and the supported simple entities; players/living entities, sounds, game events, fluid simulation, and experimental redstone orientation remain outside this audit's implemented behavior.

`PistonBaseBlock.moveBlocks` (Java lines 260–355) removes a retracting head before resolving, snapshots pushed states, destroys blocks in reverse resolver order, creates moving placeholders and their block entities in reverse push order, then creates the extending head. It clears vacated positions, performs their shape updates, notifies destroyed positions, notifies pushed source positions in reverse order, and finally notifies the head. `src/piston.cpp` follows those phases and Java update flags 276, 18, 324, and 82. The piston tests exercise the observable notification order for a destroyed block followed by two pushed sources and the head.

The user explicitly waived exact hash-table iteration order. Java's `deleteAfterMove` is a `HashMap`; this implementation uses insertion order for the vacated-position clearing and shape-update loops. Branched movements can therefore notify those vacated positions in a different order and yield different outcomes when those updates interact. This exception does not apply to the ordered destroy, push, or head notification phases.

Java's `triggerEvent` (lines 150–219) checks power again before events, finalizes the existing moving head before placing the retracting moving base, registers its block entity before neighbor notifications and shape updates, and handles sticky pull/drop after those notifications. The C++ path follows that sequence. Sound and activation/deactivation game events are not modeled.

Java `PistonMovingBlockEntity.finalTick` and `tick` (lines 276–337) update progress and remove the block entity before inspecting or changing the world block. Normal completion uses flags 340 and 3 for an invalid resulting shape, or 67 followed by neighborChanged for a valid one; forced finalization uses flags 3 and then neighborChanged. The C++ implementation follows those phases for supported blocks. The non-final branch now runs `moveCollidedEntities` and `moveStuckEntities` before advancing progress, including `PistonMath` swept areas, `MoverType.PISTON` movement limiting, NOCLIP collision suppression, slime `causeBounce`, and honey horizontal sticking. Simple-entity block collision and slime-block landing bounce live in `src/entity.cpp`.

The air check in `isPushable` (Java `PistonBaseBlock.java` lines 225–237) precedes the outward-facing build-height boundary check. Tests cover empty and occupied positions at both height boundaries, including resolving a block into boundary air.

Moving block-entity tickers are held in registration order. Java `Level.tickBlockEntities` (lines 533–556) appends tickers created during a pass to a pending list for the next pass. Java `LevelChunk.updateBlockEntityTicker` (lines 721–735) instead rebinds a still-registered position's wrapper when its entity is replaced; if its turn is still ahead in the current pass, the replacement ticks at that original position in the sequence. `LevelChunk.removeBlockEntity` (lines 490–503, 522–526) removes that wrapper, so a later registration is pending. The C++ ticker slots preserve these distinctions and skip removed slots. Tests inject reentrant registrations and replacements during a completion to verify all three cases. An optional completion observer is an instrumentation seam for this test and is unset by default.

The completion trace test verifies entity removal, neighbor-shape resolution, final setBlock, then self-neighbor notification. It also checks `lastTicked`, `progressO`, and progress over the two movement ticks and the subsequent finalization tick. These stage trace entries are emitted only when update tracing is enabled.

The entity phase was added between block events and block-entity ticking, matching `ServerLevel.tick`. `EntityTickList` insertion-order iteration, deferred additions, removal skipping, and chunk-unload pausing are covered by `entity_tests.cpp`. TNT explosions run inside the entity phase and may destroy blocks, spawn item entities, and prime further TNT; newly spawned entities wait for the next tick, matching Java's active/passive tick-list swap. Moving-piston displacement now runs in the block-entity phase after all entity ticks, so a slime block hit in this phase sets velocity that the entity applies on the next tick; `piston_tests.cpp` checks the one-tick -1.0 launch and the carpet-duper westward launch, and `validation/vanilla26_1/item_motion_validation.py` records the corresponding server X-velocity sequence. The server hit tick can shift by one with command scheduling, so no absolute server-run position is asserted.

## Uncommitted entity/rail review (2026-11-06)

Source review found and corrected these additional differences. The new checks
below are C++ regression tests against the local 26.1 source behavior; they have
not been run as new vanilla-server differential cases. No graphical client was
started for this review.

- `BlockCollisions` scans a one-block shell around the queried AABB, including
  large collision shapes on shell faces and moving pistons on shell edges. The
  old C++ scan omitted the upper half of walls and moving-block shapes extending
  into an adjacent cell. `collisionShapesOutsideTheirBlock` checks both actual
  displacement and overlap queries for a wall and a moving stone block.
- `Entity.getBlockSpeedFactor` uses `getBlockPosBelowThatAffectsMyMovement`:
  offset `0.500001F` normally and `0.999999F` for items. `ItemEntity.tick` also
  uses this position for friction. Previously both paths sampled just below the
  feet, so carpet hid the underlying honey/slime block. `itemMovementSupport`
  checks honey slowdown and slime landing-bounce/stepOn/friction ordering.
  Tracking `mainSupportingBlockPos` at block edges remains a `// TODO`.
- `Entity.moveTowardsClosestSpace` must treat an empty collision shape as an
  available exit. The old full-cube test returned true for empty non-air shapes.
  `escapeThroughEmptyCollision` checks an embedded item's only exit through a
  rail cell.
- `BaseRailBlock.canSurvive` uses `SupportType.RIGID`, the top face's outer
  two-pixel ring, rather than requiring a full collision volume.
  `railPistonSupport` permits upward piston heads and downward extended bases,
  while rejecting walls, honey, carpet and an upward extended base.
- `RailState.place` applies its powered/unpowered corner precedence within the
  original null-shape branch even after assigning `defaultShape`. An extra C++
  `!shape` condition skipped this precedence and prevented T-junction switching.
  `railJunctionPowerSwitch` checks south-east / north-east / south-east under
  signal-source notifications with power off / on / off.
- `PistonMovingBlockEntity.tick` adds `0.5F`, pushes collided/stuck entities, and
  only then clamps its stored progress. Clamping before those pushes changed
  the sweep when NBT supplies nonstandard progress such as `0.75F`.
  `importedFractionalProgressSweep` checks the full sweep to temporary progress
  `1.25F`, stored progress `1.0F`, and deferred completion.
- `ItemEntity.hurtServer` casts after subtracting damage (`health =
  (int)(health - damage)`). Casting damage first incorrectly ignored sub-unit
  damage, including fractional explosion damage. `fractionalItemDamage` checks
  both a health decrement and removal when the truncated result reaches zero.
- Entity ticking visibility is the X/Z chunk's status, not the build-height
  range. Reusing `isLoaded(BlockPos)` froze entities at min/max build height
  and prevented void removal. `entityHeightBoundaries` checks ticking above
  height, falling below minY, removal below minY-64 and an unloaded neighbor
  chunk remaining paused. Block effects and protocol tracking use the same
  X/Z loading distinction.
  Falling-block timeout also translates Java's inclusive maxY to C++'s
  exclusive maxY. TNT keeps its separate tick path (no generic void removal)
  and its explosion center uses `Entity.getY(0.0625)`, scaled by entity height.

## Optional protocol broadcast ordering (2026-11-06)

The protocol endpoint observes dirty block positions at `Level.sendBlockUpdated`
and successful block events after their callbacks. Ordinary changes are not sent
there immediately: `ServerLevel.tick` runs scheduled ticks, then
`ServerChunkCache.tick`, then block events, entities, and block entities.
`ServerChunkCache.tickChunks` broadcasts after chunk ticks, and
`ChunkHolder.broadcastChanges` reads the current state at broadcast time.
`World::blockUpdatesBroadcastObserver` matches that pre-event stage (random
chunk ticks are still TODO). Event/entity/block-entity changes wait for the next
broadcast. Early source-air packets would destroy the original structure before
the client's piston event can resolve it. `protocol_tests` asserts no ordinary
block update follows the piston event in its movement-start tick, verifies
coalescing/current-state reads, and checks that a network mapping failure cannot
skip the remaining block-entity phase. Prediction corrections remain immediate.

Placement no longer calls generic `updateFromNeighbourShapes` on observers,
which would enqueue a spurious pulse. Protocol tests distinguish fresh placement
from an actual observed-face change. These are source-based C++ assertions and
official Java codec checks; no real client animation test was run.
