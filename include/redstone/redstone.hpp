#pragma once
#include "redstone/engine.hpp"

namespace redstone {

// Vanilla TickPriority integer encoding (TickPriority.java:6-13).
inline constexpr int kTickPriorityExtremelyHigh = -3;
inline constexpr int kTickPriorityVeryHigh = -2;
inline constexpr int kTickPriorityHigh = -1;
inline constexpr int kTickPriorityNormal = 0;

// Signal queries matching Vanilla BlockStateBase / SignalGetter.
bool isDiodeBlock(const BlockState &state);
bool isRedstoneSignalSource(const World &world, const BlockState &state);
int redstoneBlockDirectSignal(const World &world, Pos pos,
                              const BlockState &state, Direction direction);
int redstoneBlockSignal(const World &world, Pos pos, const BlockState &state,
                        Direction direction);
bool hasAnalogOutputSignal(const BlockState &state);
int comparatorAnalogOutput(World &world, Pos pos, const BlockState &state,
                           Direction direction);

// RedStoneWireBlock helpers (DefaultRedstoneWireEvaluator, non-experimental).
BlockState redstoneWireConnectionState(const World &world, Pos pos,
                                       const BlockState &state);
BlockState redstoneWirePlacementState(const World &world, Pos pos);
bool redstoneWireCanSurvive(World &world, Pos pos);
bool redstoneWireUse(World &world, Pos pos, const BlockState &state);
void redstoneUpdateIndirectNeighbourShapes(World &world, Pos pos,
                                           const BlockState &state, int flags,
                                           int limit);

// Redstone torch helpers (standing + wall).
bool redstoneTorchCanSurvive(World &world, Pos pos);
bool redstoneWallTorchCanSurvive(World &world, Pos pos, Direction facing);

// Repeater & comparator helpers.
bool diodeCanSurvive(World &world, Pos pos);
BlockState repeaterPlacementState(const World &world, Pos pos,
                                  Direction playerHorizontal);
BlockState comparatorPlacementState(Direction playerHorizontal);
void diodeSetPlacedBy(World &world, Pos pos, const BlockState &state);
bool repeaterUse(World &world, Pos pos, const BlockState &state);
bool comparatorUse(World &world, Pos pos, const BlockState &state);

// Lever, button, pressure plate, target helpers.
bool leverCanAttach(World &world, Pos pos, const BlockState &state);
bool leverUse(World &world, Pos pos, const BlockState &state);
bool buttonUse(World &world, Pos pos, const BlockState &state);
bool pressurePlateCanSurvive(World &world, Pos pos);
void targetHit(World &world, Pos pos, const BlockState &state, int power,
               int duration = 8);

// BlockBehaviour callbacks bound in bindBlockBehaviours.
void redstoneWireOnPlace(World &world, Pos pos, const BlockState &state,
                         const BlockState &oldState, bool movedByPiston);
void redstoneWireAffectRemoval(World &world, Pos pos, const BlockState &state,
                               bool movedByPiston);
void redstoneWireNeighborChanged(World &world, Pos pos, const BlockState &state,
                                 const BlockRef &source, bool movedByPiston);
BlockState redstoneWireUpdateShape(World &world, Pos pos,
                                   const BlockState &state, Direction direction,
                                   Pos neighborPos, const BlockState &neighbor);

void redstoneTorchOnPlace(World &world, Pos pos, const BlockState &state,
                          const BlockState &oldState, bool movedByPiston);
void redstoneTorchAffectRemoval(World &world, Pos pos, const BlockState &state,
                                bool movedByPiston);
void redstoneTorchNeighborChanged(World &world, Pos pos,
                                  const BlockState &state,
                                  const BlockRef &source, bool movedByPiston);
BlockState redstoneTorchUpdateShape(World &world, Pos pos,
                                    const BlockState &state,
                                    Direction direction, Pos neighborPos,
                                    const BlockState &neighbor);
BlockState redstoneWallTorchUpdateShape(World &world, Pos pos,
                                        const BlockState &state,
                                        Direction direction, Pos neighborPos,
                                        const BlockState &neighbor);
void redstoneTorchTick(World &world, Pos pos, const BlockState &state);

void repeaterOnPlace(World &world, Pos pos, const BlockState &state,
                     const BlockState &oldState, bool movedByPiston);
void repeaterAffectRemoval(World &world, Pos pos, const BlockState &state,
                           bool movedByPiston);
void repeaterNeighborChanged(World &world, Pos pos, const BlockState &state,
                             const BlockRef &source, bool movedByPiston);
BlockState repeaterUpdateShape(World &world, Pos pos, const BlockState &state,
                               Direction direction, Pos neighborPos,
                               const BlockState &neighbor);
void repeaterTick(World &world, Pos pos, const BlockState &state);

void comparatorOnPlace(World &world, Pos pos, const BlockState &state,
                       const BlockState &oldState, bool movedByPiston);
void comparatorAffectRemoval(World &world, Pos pos, const BlockState &state,
                             bool movedByPiston);
void comparatorNeighborChanged(World &world, Pos pos, const BlockState &state,
                               const BlockRef &source, bool movedByPiston);
void comparatorTick(World &world, Pos pos, const BlockState &state);

void leverAffectRemoval(World &world, Pos pos, const BlockState &state,
                        bool movedByPiston);
BlockState leverUpdateShape(World &world, Pos pos, const BlockState &state,
                            Direction direction, Pos neighborPos,
                            const BlockState &neighbor);

void buttonAffectRemoval(World &world, Pos pos, const BlockState &state,
                         bool movedByPiston);
BlockState buttonUpdateShape(World &world, Pos pos, const BlockState &state,
                             Direction direction, Pos neighborPos,
                             const BlockState &neighbor);
void buttonTick(World &world, Pos pos, const BlockState &state);

void targetOnPlace(World &world, Pos pos, const BlockState &state,
                   const BlockState &oldState, bool movedByPiston);
void targetTick(World &world, Pos pos, const BlockState &state);

void pressurePlateAffectRemoval(World &world, Pos pos, const BlockState &state,
                                bool movedByPiston);
BlockState pressurePlateUpdateShape(World &world, Pos pos,
                                    const BlockState &state,
                                    Direction direction, Pos neighborPos,
                                    const BlockState &neighbor);
void pressurePlateTick(World &world, Pos pos, const BlockState &state);

void doorNeighborChanged(World &world, Pos pos, const BlockState &state,
                         const BlockRef &source, bool movedByPiston);
BlockState doorUpdateShape(World &world, Pos pos, const BlockState &state,
                           Direction direction, Pos neighborPos,
                           const BlockState &neighbor);
void trapdoorNeighborChanged(World &world, Pos pos, const BlockState &state,
                             const BlockRef &source, bool movedByPiston);
void copperBulbOnPlace(World &world, Pos pos, const BlockState &state,
                       const BlockState &oldState, bool movedByPiston);
void copperBulbNeighborChanged(World &world, Pos pos, const BlockState &state,
                               const BlockRef &source, bool movedByPiston);

} // namespace redstone
