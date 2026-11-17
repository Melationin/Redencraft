#pragma once
// 红石元件与信号系统接口：声明计划刻优先级常量、原版红石信号/模拟量查询函数、
// 红石元件放置与交互辅助函数，以及绑定到 `BlockBehaviours` 的原版 `BlockBehaviour` 回调。
#include "redstone/engine.hpp"

namespace redstone {

// 原版 `TickPriority` 整数编码（`TickPriority.java:6-13`），数值越小优先级越高，
// 用于中继器、比较器等二极管元件在同刻计划刻队列中按原版确定性顺序执行。
inline constexpr int kTickPriorityExtremelyHigh = -3;
inline constexpr int kTickPriorityVeryHigh = -2;
inline constexpr int kTickPriorityHigh = -1;
inline constexpr int kTickPriorityNormal = 0;

// 对齐原版 `BlockStateBase` / `SignalGetter` 的红石信号查询接口，分别区分直接充能（强信号）与常规信号（弱信号）。
bool isDiodeBlock(const BlockState &state);
bool isRedstoneSignalSource(const World &world, const BlockState &state);
int redstoneBlockDirectSignal(const World &world, Pos pos,
                              const BlockState &state, Direction direction);
int redstoneBlockSignal(const World &world, Pos pos, const BlockState &state,
                        Direction direction);
bool hasAnalogOutputSignal(const BlockState &state);
int comparatorAnalogOutput(World &world, Pos pos, const BlockState &state,
                           Direction direction);

// `RedStoneWireBlock` 辅助函数（`DefaultRedstoneWireEvaluator`，非实验性红石线）。
BlockState redstoneWireConnectionState(const World &world, Pos pos,
                                       const BlockState &state);
BlockState redstoneWirePlacementState(const World &world, Pos pos);
bool redstoneWireCanSurvive(World &world, Pos pos);
bool redstoneWireUse(World &world, Pos pos, const BlockState &state);
// 红石粉沿台阶/方块上下爬坡时会与斜角方向的红石粉相连，普通六向直接邻居形状更新无法覆盖，
// 因此需要独立函数沿已连接的水平邻格上下方传播间接形状更新。
void redstoneUpdateIndirectNeighbourShapes(World &world, Pos pos,
                                           const BlockState &state, int flags,
                                           int limit);

// 红石火把辅助函数（站立火把与墙上火把）。
bool redstoneTorchCanSurvive(World &world, Pos pos);
bool redstoneWallTorchCanSurvive(World &world, Pos pos, Direction facing);

// 中继器与比较器辅助函数（存活检查、放置朝向、延迟初始化与右键交互）。
bool diodeCanSurvive(World &world, Pos pos);
BlockState repeaterPlacementState(const World &world, Pos pos,
                                  Direction playerHorizontal);
BlockState comparatorPlacementState(Direction playerHorizontal);
void diodeSetPlacedBy(World &world, Pos pos, const BlockState &state);
bool repeaterUse(World &world, Pos pos, const BlockState &state);
bool comparatorUse(World &world, Pos pos, const BlockState &state);

// 拉杆、按钮、压力板与标靶辅助函数。
bool leverCanAttach(World &world, Pos pos, const BlockState &state);
bool leverUse(World &world, Pos pos, const BlockState &state);
bool buttonUse(World &world, Pos pos, const BlockState &state);
bool pressurePlateCanSurvive(World &world, Pos pos);
void targetHit(World &world, Pos pos, const BlockState &state, int power,
               int duration = 8);

// 在 `bindBlockBehaviours` 中绑定到各方块单例的 `BlockBehaviour` 回调：
// 严格分离放置回调、移除后回调、邻居变化、形状更新与计划刻，不将其混用为泛化邻居刷新。
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
void pressurePlateEntityInside(World &world, Pos pos, const BlockState &state,
                               Entity &entity);
void tripWireEntityInside(World &world, Pos pos, const BlockState &state,
                          Entity &entity);
void tripWireTick(World &world, Pos pos, const BlockState &state);
void redstoneOreStepOn(World &world, Pos pos, const BlockState &state,
                       Entity &entity);
void redstoneOreTick(World &world, Pos pos, const BlockState &state);

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
