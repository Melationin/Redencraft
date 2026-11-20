#pragma once
// 模拟引擎基础数据类型：定义方向与旋转、整格坐标 `Pos` 及其扁平哈希映射、
// `java.util.Random` 兼容随机源、物理几何体（`Vec3`/`VoxelShape`/`Aabb`）、
// 实体尺寸与类型元数据、计划刻与方块事件、方块实体刻槽位以及网络编码用区块快照。
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <deque>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "redstone/block.hpp"
#include "redstone/block_tags.hpp"
#include "redstone/dimension.hpp"
#include "redstone/entity_codec.hpp"
#include "redstone/fluid.hpp"
#include "redstone/flat_hash_map.hpp"
#include "redstone/heightmap.hpp"
#include "redstone/light.hpp"
#include "redstone/poi.hpp"

namespace redstone {
// 六向方位枚举：严格保持与 `Facing` 相同的 `Down..East` 顺序，使 `directionOf()`
// 可直接重解释属性位域值而不经过字符串解析。
enum class Direction { Down, Up, North, South, West, East };
Direction opposite(Direction d);
enum class Rotation { None, Clockwise90, Clockwise180, Counterclockwise90 };
// 对应 `RotatedPillarBlock.rotatePillar`；传入状态必须拥有 `axis` 属性。
BlockState rotatePillar(BlockState state, Rotation rotation);
Direction directionFromString(std::string_view s);
std::string directionName(Direction d);
// 由于 `Facing` 与 `Direction` 共享 `Down..East` 枚举序，可直接从 `BlockState::packed()`
// 零开销转换，省去按属性名解析字符串的成本。
inline Direction directionOf(const BlockState &state,
                             Direction fallback = Direction::North) {
  return static_cast<Direction>(
      facingOf(state, static_cast<Facing>(fallback)));
}
// 补全默认 `"minecraft:"` 命名空间的规范资源定位符。
std::string canonicalName(std::string name);
// 三维整格方块坐标（对应原版 `BlockPos`）。
struct Pos {
  int x = 0, y = 0, z = 0;
  auto operator<=>(const Pos &) const = default;
  Pos relative(Direction d) const;
};
// 与计划刻哈希键保持一致的坐标混合哈希：低位直接决定哈希桶索引，
// 因此通过三路大素数乘法异或链与高低位折叠打散相邻方块坐标，减少桶碰撞。
struct PosHash {
  std::uint64_t operator()(const Pos &p) const noexcept {
    std::uint64_t hash =
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.x)) *
        0x9E3779B97F4A7C15ull;
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.y)) *
            0xC2B2AE3D27D4EB4Full;
    hash ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(p.z)) *
            0x165667B19E3779F9ull;
    return hash ^ (hash >> 32);
  }
};
// 以 `Pos` 为键的扁平开放寻址哈希表：原版中 `LevelChunk.blockEntities` 与
// `tickersInLevel` 使用 `Object2ObjectOpenHashMap` 而非有序树映射，使用 `FlatHashMap`
// 可避免红黑树节点堆分配并提升热路径局部性。
template <class T> using PosMap = FlatHashMap<Pos, T, PosHash>;
// 兼容 `java.util.Random` 的 48 位线性同余随机数流。原版 `ServerLevel.random` 使用遗留
// `RandomSource`；本类按原版源码结构实现，但逐事件精确随机序列不作为跨端兼容硬性要求
// （参见 `docs/entities.md`）。
class Random {
public:
  explicit Random(std::int64_t seed = 0) { setSeed(seed); }
  void setSeed(std::int64_t seed) {
    state_ = (static_cast<std::uint64_t>(seed) ^ 0x5deece66dULL) &
             ((1ULL << 48) - 1);
  }
  int nextInt();
  int nextInt(int bound);
  std::int64_t nextLong();
  float nextFloat();
  double nextDouble();
  double nextDouble(double min, double max) {
    return min + nextDouble() * (max - min);
  }

private:
  std::uint32_t next(int bits);
  std::uint64_t state_ = 0;
};
// 双精度三维向量：用于实体位置、速度、射线检测与位移计算（对应原版 `Vec3`）。
struct Vec3 {
  double x = 0, y = 0, z = 0;
  Vec3 operator+(const Vec3 &) const;
  Vec3 operator-(const Vec3 &) const;
  Vec3 operator*(double) const;
  Vec3 &operator+=(const Vec3 &);
  double lengthSqr() const;
  double length() const;
  double horizontalDistanceSqr() const;
  double horizontalDistance() const;
  Vec3 normalize() const;
  Vec3 multiply(double x, double y, double z) const;
  Vec3 with(double x, double y, double z) const;
  bool isFinite() const;
};
// 方块体素盒（以 0..16 像素为单位的局部坐标表示单个长方体）。
struct ShapeBox {
  double x0, y0, z0, x1, y1, z1;
};
// 组合体素形状（由若干 `ShapeBox` 组成，对应原版 `VoxelShape`）。
using VoxelShape = std::vector<ShapeBox>;
// 世界坐标轴对齐包围盒（对应原版 `AABB`，用于实体碰撞箱与空间相交查询）。
struct Aabb {
  double minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
  static Aabb ofSize(Vec3 center, double width, double height);
  static Aabb ofSize(Vec3 center, double dx, double dy, double dz);
  Aabb move(double x, double y, double z) const;
  Aabb move(const Vec3 &v) const { return move(v.x, v.y, v.z); }
  Aabb expandTowards(const Vec3 &) const;
  Aabb inflate(double x, double y, double z) const;
  Aabb inflate(double amount) const;
  Aabb deflate(double amount) const;
  bool intersects(const Aabb &) const;
  bool intersects(Pos pos) const;
  bool contains(Vec3 point) const;
  Vec3 getCenter() const;
  double getXsize() const { return maxX - minX; }
  double getYsize() const { return maxY - minY; }
  double getZsize() const { return maxZ - minZ; }
};
// 实体姿态枚举，数值与原版 26.1 协议元数据保持一致。
enum class Pose : std::uint8_t {
  Standing = 0,
  FallFlying = 1,
  Sleeping = 2,
  Swimming = 3,
  SpinAttack = 4,
  Crouching = 5,
  LongJumping = 6,
  Dying = 7,
  Croaking = 8,
  UsingTongue = 9,
  Sitting = 10,
  Roaring = 11,
  Sniffing = 12,
  Emerging = 13,
  Digging = 14,
  Sliding = 15,
  Shooting = 16,
  Inhaling = 17,
};
Pose poseFromId(int id);
// 实体碰撞箱尺寸与眼高（对应原版 `EntityDimensions`，支持按幼年/缩放比例缩放或固定尺寸）。
struct EntityDimensions {
  float width = 0.6f;
  float height = 1.8f;
  float eyeHeight = 1.53f;
  bool fixed = false;

  Aabb makeBoundingBox(Vec3 pos) const {
    return makeBoundingBox(pos.x, pos.y, pos.z);
  }
  Aabb makeBoundingBox(double x, double y, double z) const {
    const double half = static_cast<double>(width) / 2.0;
    return {x - half, y, z - half, x + half, y + static_cast<double>(height),
            z + half};
  }
  EntityDimensions scale(float s) const { return scale(s, s); }
  EntityDimensions scale(float widthScale, float heightScale) const {
    if (fixed || (widthScale == 1.0f && heightScale == 1.0f))
      return *this;
    return {width * widthScale, height * heightScale, eyeHeight * heightScale,
            false};
  }
  static EntityDimensions scalable(float width, float height) {
    return {width, height, height * 0.85f, false};
  }
  static EntityDimensions fixedBox(float width, float height) {
    return {width, height, height * 0.85f, true};
  }
  EntityDimensions withEyeHeight(float eh) const {
    return {width, height, eh, fixed};
  }
};
// 实体类型注册表条目：保存碰撞尺寸、协议 ID 及网络同步参数。
struct EntityType {
  std::string name;
  float width = 0.6f;
  float height = 1.8f;
  float eyeHeight = 1.53f;
  bool fireImmune = false;
  bool simulated = false;
  // 对应原版 `EntityType.clientTrackingRange` 与 `updateInterval`：
  // 决定 `ServerEntity` 多久向客户端同步一次受跟踪实体的位置和朝向。
  int updateInterval = 3;
  int clientTrackingRange = 5;
  int protocolId = -1;
  bool canSerialize = true;
  bool canSummon = true;
  EntityDimensions dimensions() const {
    return {width, height, eyeHeight, false};
  }
};
const std::vector<EntityType> &entityRegistry();
const EntityType *entityType(const std::string &name);
const EntityType *entityTypeByProtocolId(int protocolId);
// 物品类型注册表条目（名称与最大堆叠数）。
struct ItemType {
  std::string name;
  int maxStackSize = 64;
};
const std::vector<ItemType> &itemRegistry();
const ItemType *itemType(const std::string &name);
// 计划刻条目（对应原版 `ScheduledTick`）：按目标刻 `trigger`、优先级 `priority` 与全局单调序号 `order` 排序。
struct ScheduledTick {
  Pos pos;
  BlockRef block;
  std::int64_t trigger = 0;
  int priority = 0;
  std::int64_t order = 0;
  bool operator==(const ScheduledTick &) const = default;
};
// 单区块的方块/流体计划刻快照，供区块序列化/反序列化（`ChunkTickSnapshot`）使用。
struct ChunkTickSnapshot {
  std::vector<ScheduledTick> blockTicks;
  std::vector<std::pair<ScheduledTick, FluidRef>> fluidTicks;
};
// 对应原版 `ScheduledTick.DRAIN_ORDER` 与 `ScheduledTick.INTRA_TICK_DRAIN_ORDER`：
// 两者均实现为最小堆反向比较器（因为 `std::priority_queue` 将“较大元素”置于堆顶）。
// `DrainOrder` 在跨刻队列中按 `(trigger, priority, order)` 排出到期刻；
// `IntraOrder` 则在已归入当前刻的同刻容器内仅按 `(priority, order)` 排出。
struct ScheduledTickDrainOrder {
  bool operator()(const ScheduledTick &a, const ScheduledTick &b) const;
};
struct ScheduledTickIntraOrder {
  bool operator()(const ScheduledTick &a, const ScheduledTick &b) const;
};
// 方块事件队列条目（对应原版 `BlockEventData`，如活塞伸/缩、音符盒发声）。
struct BlockEvent {
  Pos pos;
  BlockRef block;
  int type = 0, data = 0;
  bool operator==(const BlockEvent &) const = default;
};
struct BlockEventHash {
  std::size_t operator()(const BlockEvent &event) const;
};
// 调试/诊断事件日志条目。
struct LogEntry {
  std::string kind;
  Pos pos;
  std::string detail;
};
struct PistonMovingBlockEntity;
// 对应原版 `LevelChunk.BoundTickingBlockEntity`：ticker 槽位强持有其移动活塞方块实体，
// 遍历阶段仅以 `removed` 作为唯一存活判定。强持有实体（与原版做法一致）确保即使遍历中途
// 该位置的方块实体被替换或拆除，当前 pass 已取出的引用依然有效；整轮结束后统一压缩清理失效槽位，
// 从而在保证安全的同时维持注册顺序不变。
struct PistonTickerSlot {
  Pos pos;
  std::shared_ptr<PistonMovingBlockEntity> entity;
  bool removed = false;
};
struct ContainerBlockEntity;
struct HopperBlockEntity;
// 容器方块实体刻调度器的休眠状态。
enum class ContainerSleepMode : std::uint8_t {
  Active = 0,
  SleepIndefinite = 1,
  SleepUntil = 2,
};
// 模仿 Lithium 的 `SleepingBlockEntity` / `SleepUntilTimeBlockEntityTickInvoker` 槽位：
// 在 `World::containerTickers` 中保持原有注册顺序的同时，允许空闲或处于冷却期的容器（如漏斗）
// 以零成本跳过每刻轮询，直到库存变更、邻近方块变化或附近 `ItemEntity` 事件将其唤醒。
struct ContainerTickerSlot {
  Pos pos;
  std::shared_ptr<ContainerBlockEntity> entity;
  ContainerSleepMode sleepMode = ContainerSleepMode::Active;
  std::int64_t wakeGameTime = 0;
  bool removed = false;
};
class Entity;
// 方块射线命中结果（对应原版 `BlockHitResult`）。
struct BlockHitResult {
  bool hit = false;
  Pos pos{};
  Direction direction = Direction::Up;
  Vec3 location{};
  bool inside = false;
};
// 实体射线命中结果（对应原版 `EntityHitResult`）。
struct EntityHitResult {
  Entity *entity = nullptr;
  Vec3 location{};
};
class WorldChunkManager;
// 面向网络编码器导出的区块只读快照：完整拷贝调色板、索引数组以及移动活塞方块实体，
// 使耗时的协议封包编码可以在 tick 线程之外安全执行，避免读写正在演进的活跃区块容器。
// 区段 4x4x4 Quart 生物群系调色板只读容器（索引 `(qy * 4 + qz) * 4 + qx`，共 64 格）。
// 空指针表示全区段均匀 `"minecraft:plains"`，对不含自定义生物群系的干态测试与基准保持零额外内存分配。
struct SectionBiomes {
  std::vector<std::string> palette;
  std::array<std::uint8_t, 64> indices{};
};

struct SectionSnapshot {
  std::uint16_t nonEmpty = 0;
  std::uint8_t bits = 0;
  BlockState::Id single = 0;
  std::vector<BlockState::Id> palette;
  std::vector<std::uint8_t> indices8;
  std::vector<std::uint16_t> indices16;
  std::shared_ptr<const SectionBiomes> biomes;
};
struct PistonSnapshot {
  Pos pos;
  BlockState movedState;
  Direction direction = Direction::Down;
  float progressO = 0.0f;
  bool extending = false;
  bool source = false;
};
struct ChunkSnapshot {
  int minY = 0, maxY = 0;
  std::vector<SectionSnapshot> sections;
  std::vector<PistonSnapshot> pistons;
};
// 原版 `LevelTicks<ScheduledTick>` 按已加载区块分别维护计划刻容器；`ServerLevel` 分别为
// `Block` 与 `Fluid` 各持有一个实例，因此将队列抽象为按目标类型参数化的模板。
// 完整定义放在 `engine` 实现文件中，头文件仅做前置声明以供 `World` 的 `unique_ptr` 成员使用，
// 避免将复杂内部堆/哈希结构暴露给所有包含 `engine.hpp` 的编译单元。
template <class Target> struct ChunkTicksOf;
template <class Target> struct TickQueueOf;
using BlockTickQueue = TickQueueOf<Block>;
using FluidTickQueue = TickQueueOf<Fluid>;

namespace detail {
struct WorldBehaviours;
}
} // namespace redstone
