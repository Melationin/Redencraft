#pragma once

#include "redstone/nbt.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace redstone::entity_codec {

enum class DecodeStatus { Ok, Missing, Unsupported, Corrupt };

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

// PrimedTnt.addAdditionalSaveData, limited to state this engine can restore
// without inventing an owner or a custom block-state codec.
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

// The C++ falling-block path currently provides the normal sand behaviour:
// gravity, free fall, landing and drop-on-obstruction. The codec keeps only
// this exact default sand state; red sand, gravel, damage, tile data, custom
// states and other unmodelled variants must be rejected as a whole.
struct FallingBlockEntityData {
  std::array<std::int32_t, 4> uuid{};
  std::array<double, 3> pos{};
  std::array<double, 3> motion{};
  std::array<float, 2> rotation{};
  bool onGround = false;
  int fireTicks = 0;
  double fallDistance = 0.0;
  std::string blockId = "minecraft:sand";
  int time = 0;
  bool dropItem = true;
  bool cancelDrop = false;
  bool hurtEntities = false;
  float fallDamagePerDistance = 0.0f;
  int fallDamageMax = 40;
};

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

// A single ordered compact payload per record. There is deliberately no
// second typed array for any kind; consumers that need a type project locally.
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

struct DecodedEntityChunk {
  DecodeStatus status = DecodeStatus::Corrupt;
  std::string detail;
  std::int32_t chunkX = 0, chunkZ = 0;
  std::vector<EntityRecord> records;
  nbt::RootTag root;
};

DecodedEntityChunk decodeEntityChunk(std::span<const std::uint8_t> bytes);
// Compatibility/test helper. Replaces only Entities, grouping item then TNT.
// Production uses patchEntitySequence and one ordered variant vector.
bool patchEntityItems(nbt::RootTag &root,
                      std::span<const ItemEntityData> items,
                      std::span<const TntEntityData> tnts,
                      std::string &error);
// Ordered production representation. Mixed records retain the same Entities
// list order through decode, install, snapshot, and save.
bool patchEntitySequence(nbt::RootTag &root,
                         std::span<const EntityRecord> records,
                         std::string &error);
nbt::RootTag emptyEntityRoot(std::int32_t chunkX, std::int32_t chunkZ);
// Shared compact-record storage used by the persistence worker and probes.
// These helpers never retain or clone an entity-region root.
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
// Tests use these only to prove the codec against a stock-style NBT record.
// Production persistence uses the same functions.
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

} // namespace redstone::entity_codec
