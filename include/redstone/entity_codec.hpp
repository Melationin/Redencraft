#pragma once
/// @file entity_codec.hpp
/// @brief 定义实体区块 NBT 编解码器（含轻量持久化记录 `EntityRecord` 与通用非 AI 实体树编解码接口）。
///
/// 提供两条编解码路径：一条面向早期四类核心红石物理实体的紧凑扁平记录，以零虚表开销保留原版
/// `Entities` 列表顺序；另一条面向 Stage E-6 全量非 AI 实体的递归 NBT 编解码，支持 `Passengers` 骑乘树。

#include "redstone/nbt.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace redstone {
class World;
class Entity;
} // namespace redstone

namespace redstone::entity_codec {

/// 实体区块或单条实体 NBT 解码结果状态，用于区分缺失、未支持版本/类型与格式损坏。
enum class DecodeStatus { Ok, Missing, Unsupported, Corrupt };

/// 掉落物实体的轻量持久化快照，不依赖完整 `World` 上下文即可往返序列化。
struct ItemEntityData {
  std::array<std::int32_t, 4> uuid{};
  std::array<double, 3> pos{};
  std::array<double, 3> motion{};
  std::array<float, 2> rotation{};
  bool onGround = false;
  int fireTicks = 0;
  double fallDistance = 0.0;
  int age = 0;
  int pickupDelay = 0;
  int health = 5;
  std::string itemId;
  int itemCount = 0;
};

// 对应原版 `PrimedTnt.addAdditionalSaveData`；仅保存当前引擎在不伪造发射者或自定义方块状态编解码器的前提下可还原的状态。
struct TntEntityData {
  std::array<std::int32_t, 4> uuid{};
  std::array<double, 3> pos{};
  std::array<double, 3> motion{};
  std::array<float, 2> rotation{};
  bool onGround = false;
  int fireTicks = 0;
  double fallDistance = 0.0;
  int fuse = 80;
  float explosionPower = 4.0f;
};

// C++ 下落方块持久化支持已注册的 schema 方块状态（`Name` + 可选 `Properties`）及伤害参数；
// `TileEntityData` 与未在 26.1 注册表内建模的方块状态仍整体拒绝，以免丢失状态。
struct FallingBlockEntityData {
  std::array<std::int32_t, 4> uuid{};
  std::array<double, 3> pos{};
  std::array<double, 3> motion{};
  std::array<float, 2> rotation{};
  bool onGround = false;
  int fireTicks = 0;
  double fallDistance = 0.0;
  std::string blockId = "minecraft:sand";
  std::map<std::string, std::string> blockProperties{};
  int time = 0;
  bool dropItem = true;
  bool cancelDrop = false;
  bool hurtEntities = false;
  float fallDamagePerDistance = 0.0f;
  int fallDamageMax = 40;
};

/// 普通矿车的轻量持久化快照。
struct MinecartEntityData {
  std::array<std::int32_t, 4> uuid{};
  std::array<double, 3> pos{};
  std::array<double, 3> motion{};
  std::array<float, 2> rotation{};
  bool onGround = false;
  int fireTicks = 0;
  double fallDistance = 0.0;
  bool flipped = false;
  bool firstTick = false;
};

using EntityUuid = std::array<std::int32_t, 4>;
using EntityPayload =
    std::variant<ItemEntityData, TntEntityData, FallingBlockEntityData,
                 MinecartEntityData>;

// 每条记录对应单一的有序紧凑载荷（`std::variant`）。此处刻意不再按类型维护第二份分类型数组，
// 以避免破坏 NBT 中实体的原始排列顺序并防止双写不同步；需要特定类型的消费方应在本地自行投影筛选。
struct EntityRecord {
  EntityPayload payload;
  EntityRecord() = default;
  explicit EntityRecord(ItemEntityData value) : payload(std::move(value)) {}
  explicit EntityRecord(TntEntityData value) : payload(std::move(value)) {}
  explicit EntityRecord(FallingBlockEntityData value)
      : payload(std::move(value)) {}
  explicit EntityRecord(MinecartEntityData value)
      : payload(std::move(value)) {}
};

EntityUuid recordUuid(const EntityRecord &record);

/// 实体区块解码结果，保存解析出的有序实体记录与原始 NBT 根标签以便增量修补写回。
struct DecodedEntityChunk {
  DecodeStatus status = DecodeStatus::Corrupt;
  std::string detail;
  std::int32_t chunkX = 0, chunkZ = 0;
  std::vector<EntityRecord> records;
  nbt::RootTag root;
};

DecodedEntityChunk decodeEntityChunk(std::span<const std::uint8_t> bytes);
// 兼容性与测试辅助接口：仅替换 `Entities` 列表，按先掉落物后 TNT 的固定顺序分组。
// 生产持久化路径应使用 `patchEntitySequence` 与单一有序变体数组以保持原顺序。
bool patchEntityItems(nbt::RootTag &root,
                      std::span<const ItemEntityData> items,
                      std::span<const TntEntityData> tnts,
                      std::string &error);
// 生产环境使用的有序表示修补接口：混合类型记录在解码、安装、快照与保存全流程中均严格保留相同的 `Entities` 列表顺序。
bool patchEntitySequence(nbt::RootTag &root,
                         std::span<const EntityRecord> records,
                         std::string &error);
nbt::RootTag emptyEntityRoot(std::int32_t chunkX, std::int32_t chunkZ);
// 供持久化工作线程与探针复用的紧凑记录编码辅助函数；这些函数绝不保留或深拷贝整个实体区域根标签。
nbt::Tag encodeItemEntity(const ItemEntityData &item, std::string &error);
nbt::Tag encodeTntEntity(const TntEntityData &entity, std::string &error);
nbt::Tag encodeFallingBlockEntity(const FallingBlockEntityData &entity,
                                  std::string &error);
nbt::Tag encodeMinecartEntity(const MinecartEntityData &entity,
                              std::string &error);
bool patchEntityRecords(nbt::RootTag &root,
                        std::span<const ItemEntityData> items,
                        std::span<const TntEntityData> tnts,
                        std::string &error);
// 测试中用于对照原版风格 NBT 记录验证编解码正确性，生产持久化路径亦复用同一组解码函数。
std::optional<ItemEntityData> decodeItemRecord(const nbt::Tag &entity,
                                                std::int32_t chunkX,
                                                std::int32_t chunkZ,
                                                std::string &error);
std::optional<TntEntityData> decodeTntRecord(const nbt::Tag &entity,
                                              std::int32_t chunkX,
                                              std::int32_t chunkZ,
                                              std::string &error);
std::optional<FallingBlockEntityData>
decodeFallingBlockRecord(const nbt::Tag &entity, std::int32_t chunkX,
                          std::int32_t chunkZ, std::string &error);
std::optional<MinecartEntityData>
decodeMinecartRecord(const nbt::Tag &entity, std::int32_t chunkX,
                      std::int32_t chunkZ, std::string &error);
std::vector<std::uint8_t> encodeEntityChunk(const nbt::RootTag &root);

// Stage E-6 通用非 AI 实体 NBT 编解码，支持递归 `Passengers` 骑乘树。
// 尚未建模的 Stage E-5 AI 生物会被显式拒绝，避免静默丢弃行为数据。
struct DecodedGeneralEntity {
  DecodeStatus status = DecodeStatus::Corrupt;
  std::string detail;
  std::shared_ptr<Entity> entity;
  // 之所以额外持有包含根实体及全部已解码乘客的 `shared_ptr` 列表，是因为 `Entity::passengers_`
  // 内部仅存 `weak_ptr`；在整棵骑乘树正式注册进 `World` 之前必须由此列表保活以防乘客提前析构。
  std::vector<std::shared_ptr<Entity>> entities;
};

/// 通用实体区块解码结果，分别给出顶层根实体列表（`roots`）与平铺的全部实体保活列表（`entities`）。
struct DecodedGeneralEntityChunk {
  DecodeStatus status = DecodeStatus::Corrupt;
  std::string detail;
  std::int32_t chunkX = 0, chunkZ = 0;
  std::vector<std::shared_ptr<Entity>> roots;
  std::vector<std::shared_ptr<Entity>> entities;
  nbt::RootTag root;
};

nbt::Tag encodeEntityNbt(const Entity &entity, std::string &error);
DecodedGeneralEntity decodeEntityNbt(World &world, const nbt::Tag &tag);
DecodedGeneralEntityChunk
decodeGeneralEntityChunk(World &world, std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encodeGeneralEntityChunk(
    std::int32_t chunkX, std::int32_t chunkZ,
    std::span<const Entity *const> rootEntities, std::string &error);
/// 按先根载具、后递归乘客的顺序将解码得到的整棵骑乘树注册进 `World`。
void addEntityTreeToWorld(World &world, const std::shared_ptr<Entity> &root);

} // namespace redstone::entity_codec
