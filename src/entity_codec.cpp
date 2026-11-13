#include "redstone/entity_codec.hpp"
#include <algorithm>

#include <cmath>
#include <type_traits>
#include <limits>
#include <optional>
#include <set>

namespace redstone::entity_codec {
namespace {

constexpr std::int32_t kDataVersion = 4786;
constexpr std::int32_t kMinChunkCoordinate = -1875000;
constexpr std::int32_t kMaxChunkCoordinate = 1874999;

const nbt::Tag *field(const nbt::Tag &tag, std::string_view name) {
  return tag.get(std::string(name));
}

std::optional<std::int64_t> integer(const nbt::Tag &tag,
                                    std::string_view name,
                                    std::uint8_t type = 3) {
  const nbt::Tag *value = field(tag, name);
  if (!value || value->type != type)
    return std::nullopt;
  return value->number;
}

const nbt::Tag *required(const nbt::Tag &tag, std::string_view name,
                         std::uint8_t type) {
  const nbt::Tag *value = field(tag, name);
  return value && value->type == type ? value : nullptr;
}

bool finiteDoubleList(const nbt::Tag *tag, std::size_t size) {
  if (!tag || tag->type != 9 || tag->listElementType != 6 ||
      tag->list.size() != size)
    return false;
  for (const nbt::Tag &value : tag->list)
    if (!std::isfinite(value.real))
      return false;
  return true;
}

bool finiteFloatList(const nbt::Tag *tag, std::size_t size,
                     std::array<float, 2> *out = nullptr) {
  if (!tag || tag->type != 9 || tag->listElementType != 5 ||
      tag->list.size() != size)
    return false;
  for (std::size_t i = 0; i < size; ++i) {
    if (!std::isfinite(tag->list[i].real))
      return false;
    if (out)
      (*out)[i] = static_cast<float>(tag->list[i].real);
  }
  return true;
}

std::int32_t floorDiv16(double value) {
  return static_cast<std::int32_t>(std::floor(value)) >> 4;
}

template <class EntityData>
bool normalizeEntityBase(EntityData &item, std::string_view kind,
                         std::string &error) {
  constexpr double kMaxHorizontalPosition = 3.0000512E7;
  constexpr double kMaxVerticalPosition = 2.0E7;
  for (double value : item.pos)
    if (!std::isfinite(value)) {
      error = std::string(kind) + " entity position is not finite";
      return false;
    }
  for (double value : item.motion)
    if (!std::isfinite(value)) {
      error = std::string(kind) + " entity motion is not finite";
      return false;
    }
  for (float value : item.rotation)
    if (!std::isfinite(value)) {
      error = std::string(kind) + " entity rotation is not finite";
      return false;
    }
  if (!std::isfinite(item.fallDistance)) {
    error = std::string(kind) + " entity fall_distance is not finite";
    return false;
  }
  item.pos[0] = std::clamp(item.pos[0], -kMaxHorizontalPosition,
                            kMaxHorizontalPosition);
  item.pos[1] = std::clamp(item.pos[1], -kMaxVerticalPosition,
                           kMaxVerticalPosition);
  item.pos[2] = std::clamp(item.pos[2], -kMaxHorizontalPosition,
                           kMaxHorizontalPosition);
  for (double &value : item.motion)
    if (std::abs(value) > 10.0)
      value = 0.0;
  return true;
}

bool normalizeItemEntity(ItemEntityData &item, std::string &error) {
  return normalizeEntityBase(item, "item", error);
}

bool normalizeTntEntity(TntEntityData &entity, std::string &error) {
  return normalizeEntityBase(entity, "tnt", error);
}

bool normalizeFallingBlockEntity(FallingBlockEntityData &entity,
                                 std::string &error) {
  return normalizeEntityBase(entity, "falling block", error);
}

bool normalizeMinecartEntity(MinecartEntityData &entity,
                             std::string &error) {
  return normalizeEntityBase(entity, "minecart", error);
}

bool decodeItem(const nbt::Tag &entity, std::int32_t chunkX,
                std::int32_t chunkZ, ItemEntityData &out,
                std::string &error) {
  if (entity.type != 10) {
    error = "entity entry is not a compound";
    return false;
  }
  const nbt::Tag *id = required(entity, "id", 8);
  if (!id || id->text != "minecraft:item") {
    error = "unsupported or missing entity id";
    return false;
  }
  static const std::set<std::string> allowed{
      "id", "UUID", "Pos", "Motion", "Rotation", "Health", "Age",
      "PickupDelay", "Item", "OnGround", "Fire", "fall_distance", "Air",
      "PortalCooldown", "Invulnerable"};
  for (const auto &[name, value] : entity.compound) {
    (void)value;
    if (!allowed.contains(name)) {
      error = "unsupported item entity field: " + name;
      return false;
    }
  }
  const nbt::Tag *uuid = required(entity, "UUID", 11);
  const nbt::Tag *pos = required(entity, "Pos", 9);
  const nbt::Tag *motion = required(entity, "Motion", 9);
  const nbt::Tag *rotation = required(entity, "Rotation", 9);
  const nbt::Tag *health = required(entity, "Health", 2);
  const nbt::Tag *age = required(entity, "Age", 2);
  const nbt::Tag *pickup = required(entity, "PickupDelay", 2);
  const nbt::Tag *stack = required(entity, "Item", 10);
  const nbt::Tag *onGround = required(entity, "OnGround", 1);
  const nbt::Tag *fire = required(entity, "Fire", 2);
  const nbt::Tag *fall = required(entity, "fall_distance", 6);
  const nbt::Tag *air = required(entity, "Air", 2);
  const nbt::Tag *portal = required(entity, "PortalCooldown", 3);
  const nbt::Tag *invulnerable = required(entity, "Invulnerable", 1);
  if (!uuid || uuid->ints.size() != 4 || !finiteDoubleList(pos, 3) ||
      !finiteDoubleList(motion, 3) || !finiteFloatList(rotation, 2) ||
      !health || !age || !pickup || !stack || !onGround || !fire || !fall ||
      !air || !portal || !invulnerable) {
    error = "item entity has missing or invalid base fields";
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i)
    out.uuid[i] = uuid->ints[i];
  for (std::size_t i = 0; i < 3; ++i) {
    out.pos[i] = pos->list[i].real;
    out.motion[i] = motion->list[i].real;
  }
  if (out.uuid == std::array<std::int32_t, 4>{}) {
    error = "item entity UUID is zero";
    return false;
  }
  finiteFloatList(rotation, 2, &out.rotation);
  if (!normalizeItemEntity(out, error))
    return false;
  if (floorDiv16(out.pos[0]) != chunkX || floorDiv16(out.pos[2]) != chunkZ) {
    error = "item entity position is outside its entity chunk";
    return false;
  }
  if (health->number < 0 || health->number > std::numeric_limits<std::int16_t>::max() ||
      age->number < std::numeric_limits<std::int16_t>::min() ||
      age->number > std::numeric_limits<std::int16_t>::max() ||
      pickup->number < std::numeric_limits<std::int16_t>::min() ||
      pickup->number > std::numeric_limits<std::int16_t>::max()) {
    error = "item entity short field is out of range";
    return false;
  }
  out.health = static_cast<int>(health->number);
  out.age = static_cast<int>(age->number);
  out.pickupDelay = static_cast<int>(pickup->number);
  if ((onGround->number != 0 && onGround->number != 1) ||
      air->number != 300 || portal->number != 0 ||
      invulnerable->number != 0 || !std::isfinite(fall->real)) {
    error = "unsupported nondefault item entity base state";
    return false;
  }
  out.onGround = onGround->number != 0;
  out.fireTicks = static_cast<int>(fire->number);
  out.fallDistance = fall->real;
  const nbt::Tag *itemId = required(*stack, "id", 8);
  const nbt::Tag *count = required(*stack, "count", 3);
  if (!itemId || itemId->text.empty() || !count || count->number <= 0 ||
      count->number > std::numeric_limits<int>::max()) {
    error = "item stack is unsupported";
    return false;
  }
  for (const auto &[name, value] : stack->compound) {
    (void)value;
    if (name != "id" && name != "count" && name != "components") {
      error = "unsupported item stack field: " + name;
      return false;
    }
  }
  if (const nbt::Tag *components = field(*stack, "components");
      components &&
      (components->type != 10 || !components->compound.empty())) {
    error = "item components are unsupported";
    return false;
  }
  out.itemId = itemId->text;
  out.itemCount = static_cast<int>(count->number);
  return true;
}

bool decodeTnt(const nbt::Tag &entity, std::int32_t chunkX,
               std::int32_t chunkZ, TntEntityData &out,
               std::string &error) {
  if (entity.type != 10) {
    error = "entity entry is not a compound";
    return false;
  }
  const nbt::Tag *id = required(entity, "id", 8);
  if (!id || id->text != "minecraft:tnt") {
    error = "unsupported or missing entity id";
    return false;
  }
  // Entity base fields plus PrimedTnt's four persisted fields. "owner" and
  // payloads are deliberately absent: they cannot be restored faithfully.
  static const std::set<std::string> allowed{
      "id", "UUID", "Pos", "Motion", "Rotation", "OnGround", "Fire",
      "fall_distance", "Air", "PortalCooldown", "Invulnerable",
      "fuse", "block_state", "explosion_power"};
  for (const auto &[name, value] : entity.compound) {
    (void)value;
    if (!allowed.contains(name)) {
      error = "unsupported primed tnt field: " + name;
      return false;
    }
  }
  const nbt::Tag *uuid = required(entity, "UUID", 11);
  const nbt::Tag *pos = required(entity, "Pos", 9);
  const nbt::Tag *motion = required(entity, "Motion", 9);
  const nbt::Tag *rotation = required(entity, "Rotation", 9);
  const nbt::Tag *onGround = required(entity, "OnGround", 1);
  const nbt::Tag *fire = required(entity, "Fire", 2);
  const nbt::Tag *fall = required(entity, "fall_distance", 6);
  const nbt::Tag *air = required(entity, "Air", 2);
  const nbt::Tag *portal = required(entity, "PortalCooldown", 3);
  const nbt::Tag *invulnerable = required(entity, "Invulnerable", 1);
  if (!uuid || uuid->ints.size() != 4 || !finiteDoubleList(pos, 3) ||
      !finiteDoubleList(motion, 3) || !finiteFloatList(rotation, 2) ||
      !onGround || !fire || !fall || !air || !portal || !invulnerable) {
    error = "primed tnt has missing or invalid base fields";
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i)
    out.uuid[i] = uuid->ints[i];
  if (out.uuid == std::array<std::int32_t, 4>{}) {
    error = "primed tnt UUID is zero";
    return false;
  }
  for (std::size_t i = 0; i < 3; ++i) {
    out.pos[i] = pos->list[i].real;
    out.motion[i] = motion->list[i].real;
  }
  finiteFloatList(rotation, 2, &out.rotation);
  if (!normalizeTntEntity(out, error))
    return false;
  if (floorDiv16(out.pos[0]) != chunkX || floorDiv16(out.pos[2]) != chunkZ) {
    error = "primed tnt position is outside its entity chunk";
    return false;
  }
  if ((onGround->number != 0 && onGround->number != 1) ||
      air->number != 300 || portal->number != 0 ||
      invulnerable->number != 0 || !std::isfinite(fall->real)) {
    error = "unsupported nondefault primed tnt base state";
    return false;
  }
  out.onGround = onGround->number != 0;
  out.fireTicks = static_cast<int>(fire->number);
  out.fallDistance = fall->real;

  const nbt::Tag *fuseTag = field(entity, "fuse");
  if (fuseTag) {
    if (fuseTag->type != 2 ||
        fuseTag->number < std::numeric_limits<std::int16_t>::min() ||
        fuseTag->number > std::numeric_limits<std::int16_t>::max()) {
      error = "primed tnt fuse is invalid";
      return false;
    }
    out.fuse = static_cast<int>(fuseTag->number);
  }
  const nbt::Tag *state = field(entity, "block_state");
  if (state) {
    const nbt::Tag *name = required(*state, "Name", 8);
    if (!name || name->text != "minecraft:tnt" ||
        state->compound.size() >
            (field(*state, "Properties") ? 2u : 1u)) {
      error = "primed tnt block_state is unsupported";
      return false;
    }
    // TntBlock registers its only property unstable; vanilla serializes the
    // default value even when no state override exists. A Name-only record is
    // also accepted as the codec default.
    if (const nbt::Tag *properties = field(*state, "Properties")) {
      if (properties->type != 10 || properties->compound.size() != 1) {
        error = "primed tnt block_state properties are unsupported";
        return false;
      }
      const nbt::Tag *unstable = required(*properties, "unstable", 8);
      if (!unstable || unstable->text != "false") {
        error = "nondefault primed tnt block_state is unsupported";
        return false;
      }
    }
  }
  const nbt::Tag *power = field(entity, "explosion_power");
  if (power) {
    if (power->type != 5 || !std::isfinite(power->real) ||
        power->real < 0.0 || power->real > 128.0) {
      error = "primed tnt explosion_power is invalid";
      return false;
    }
    out.explosionPower = static_cast<float>(power->real);
  }
  return true;
}

bool decodeFallingBlock(const nbt::Tag &entity, std::int32_t chunkX,
                         std::int32_t chunkZ, FallingBlockEntityData &out,
                         std::string &error) {
  if (entity.type != 10) {
    error = "entity entry is not a compound";
    return false;
  }
  const nbt::Tag *id = required(entity, "id", 8);
  if (!id || id->text != "minecraft:falling_block") {
    error = "unsupported or missing falling block id";
    return false;
  }
  static const std::set<std::string> allowed{
      "id", "UUID", "Pos", "Motion", "Rotation", "OnGround", "Fire",
      "fall_distance", "Air", "PortalCooldown", "Invulnerable", "BlockState",
      "Time", "DropItem", "HurtEntities", "FallHurtAmount", "FallHurtMax",
      "CancelDrop", "TileEntityData"};
  for (const auto &[name, value] : entity.compound) {
    (void)value;
    if (!allowed.contains(name)) {
      error = "unsupported falling block field: " + name;
      return false;
    }
  }
  if (field(entity, "TileEntityData")) {
    error = "falling block TileEntityData is unsupported";
    return false;
  }
  const nbt::Tag *uuid = required(entity, "UUID", 11);
  const nbt::Tag *pos = required(entity, "Pos", 9);
  const nbt::Tag *motion = required(entity, "Motion", 9);
  const nbt::Tag *rotation = required(entity, "Rotation", 9);
  const nbt::Tag *onGround = required(entity, "OnGround", 1);
  const nbt::Tag *fire = required(entity, "Fire", 2);
  const nbt::Tag *fall = required(entity, "fall_distance", 6);
  const nbt::Tag *air = required(entity, "Air", 2);
  const nbt::Tag *portal = required(entity, "PortalCooldown", 3);
  const nbt::Tag *invulnerable = required(entity, "Invulnerable", 1);
  if (!uuid || uuid->ints.size() != 4 || !finiteDoubleList(pos, 3) ||
      !finiteDoubleList(motion, 3) || !finiteFloatList(rotation, 2) ||
      !onGround || !fire || !fall || !air || !portal || !invulnerable) {
    error = "falling block has missing or invalid base fields";
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i)
    out.uuid[i] = uuid->ints[i];
  if (out.uuid == EntityUuid{}) {
    error = "falling block UUID is zero";
    return false;
  }
  for (std::size_t i = 0; i < 3; ++i) {
    out.pos[i] = pos->list[i].real;
    out.motion[i] = motion->list[i].real;
  }
  finiteFloatList(rotation, 2, &out.rotation);
  if (!normalizeFallingBlockEntity(out, error))
    return false;
  if (floorDiv16(out.pos[0]) != chunkX || floorDiv16(out.pos[2]) != chunkZ) {
    error = "falling block position is outside its entity chunk";
    return false;
  }
  if ((onGround->number != 0 && onGround->number != 1) ||
      air->number != 300 || portal->number != 0 ||
      invulnerable->number != 0 || !std::isfinite(fall->real)) {
    error = "unsupported nondefault falling block base state";
    return false;
  }
  out.onGround = onGround->number != 0;
  out.fireTicks = static_cast<int>(fire->number);
  out.fallDistance = fall->real;

  const nbt::Tag *blockState = field(entity, "BlockState");
  if (blockState) {
    const nbt::Tag *name = blockState->type == 10
                               ? required(*blockState, "Name", 8)
                               : nullptr;
    if (!name || name->text != "minecraft:sand" ||
        blockState->compound.size() != 1) {
      error = "falling block state is not the supported default sand state";
      return false;
    }
  }
  out.blockId = "minecraft:sand";

  const nbt::Tag *time = field(entity, "Time");
  if (time) {
    if (time->type != 3 || time->number < 0 ||
        time->number >= std::numeric_limits<int>::max()) {
      error = "falling block Time is invalid";
      return false;
    }
    out.time = static_cast<int>(time->number);
  }
  const auto boolField = [&](const char *name, bool defaultValue, bool &target) {
    const nbt::Tag *value = field(entity, name);
    if (!value) {
      target = defaultValue;
      return true;
    }
    if (value->type != 1 || (value->number != 0 && value->number != 1))
      return false;
    target = value->number != 0;
    return true;
  };
  if (!boolField("DropItem", true, out.dropItem) ||
      !boolField("CancelDrop", false, out.cancelDrop) ||
      !boolField("HurtEntities", false, out.hurtEntities)) {
    error = "falling block boolean field is invalid";
    return false;
  }
  const nbt::Tag *damagePerDistance = field(entity, "FallHurtAmount");
  if (damagePerDistance) {
    if (damagePerDistance->type != 5 ||
        !std::isfinite(damagePerDistance->real) ||
        static_cast<float>(damagePerDistance->real) != 0.0f) {
      error = "falling block damage behaviour is unsupported";
      return false;
    }
  }
  out.fallDamagePerDistance = 0.0f;
  const nbt::Tag *damageMax = field(entity, "FallHurtMax");
  if (damageMax) {
    if (damageMax->type != 3 || damageMax->number != 40) {
      error = "falling block damage behaviour is unsupported";
      return false;
    }
  }
  out.fallDamageMax = 40;
  if (out.hurtEntities) {
    error = "falling block HurtEntities is unsupported";
    return false;
  }
  return true;
}

bool decodeMinecart(const nbt::Tag &entity, std::int32_t chunkX,
                    std::int32_t chunkZ, MinecartEntityData &out,
                    std::string &error) {
  if (entity.type != 10) {
    error = "entity entry is not a compound";
    return false;
  }
  const nbt::Tag *id = required(entity, "id", 8);
  if (!id || id->text != "minecraft:minecart") {
    error = "unsupported or missing minecart id";
    return false;
  }
  static const std::set<std::string> allowed{
      "id", "UUID", "Pos", "Motion", "Rotation", "OnGround", "Fire",
      "fall_distance", "Air", "PortalCooldown", "Invulnerable",
      "DisplayOffset", "FlippedRotation", "HasTicked"};
  for (const auto &[name, value] : entity.compound) {
    (void)value;
    if (!allowed.contains(name)) {
      error = "unsupported minecart field: " + name;
      return false;
    }
  }
  if (field(entity, "DisplayState")) {
    error = "custom minecart display state is unsupported";
    return false;
  }
  const nbt::Tag *uuid = required(entity, "UUID", 11);
  const nbt::Tag *pos = required(entity, "Pos", 9);
  const nbt::Tag *motion = required(entity, "Motion", 9);
  const nbt::Tag *rotation = required(entity, "Rotation", 9);
  const nbt::Tag *onGround = required(entity, "OnGround", 1);
  const nbt::Tag *fire = required(entity, "Fire", 2);
  const nbt::Tag *fall = required(entity, "fall_distance", 6);
  const nbt::Tag *air = required(entity, "Air", 2);
  const nbt::Tag *portal = required(entity, "PortalCooldown", 3);
  const nbt::Tag *invulnerable = required(entity, "Invulnerable", 1);
  if (!uuid || uuid->ints.size() != 4 || !finiteDoubleList(pos, 3) ||
      !finiteDoubleList(motion, 3) || !finiteFloatList(rotation, 2) ||
      !onGround || !fire || !fall || !air || !portal || !invulnerable) {
    error = "minecart has missing or invalid base fields";
    return false;
  }
  for (std::size_t i = 0; i < 4; ++i)
    out.uuid[i] = uuid->ints[i];
  if (out.uuid == EntityUuid{}) {
    error = "minecart UUID is zero";
    return false;
  }
  for (std::size_t i = 0; i < 3; ++i) {
    out.pos[i] = pos->list[i].real;
    out.motion[i] = motion->list[i].real;
  }
  finiteFloatList(rotation, 2, &out.rotation);
  if (!normalizeMinecartEntity(out, error))
    return false;
  if (floorDiv16(out.pos[0]) != chunkX || floorDiv16(out.pos[2]) != chunkZ) {
    error = "minecart position is outside its entity chunk";
    return false;
  }
  if ((onGround->number != 0 && onGround->number != 1) ||
      air->number != 300 || portal->number != 0 ||
      invulnerable->number != 0 || !std::isfinite(fall->real)) {
    error = "unsupported nondefault minecart base state";
    return false;
  }
  out.onGround = onGround->number != 0;
  out.fireTicks = static_cast<int>(fire->number);
  out.fallDistance = fall->real;

  const auto boolField = [&](const char *name, bool defaultValue, bool &target) {
    const nbt::Tag *value = field(entity, name);
    if (!value) {
      target = defaultValue;
      return true;
    }
    if (value->type != 1 || (value->number != 0 && value->number != 1))
      return false;
    target = value->number != 0;
    return true;
  };
  if (!boolField("FlippedRotation", false, out.flipped) ||
      !boolField("HasTicked", false, out.firstTick)) {
    error = "minecart boolean field is invalid";
    return false;
  }
  const nbt::Tag *displayOffset = field(entity, "DisplayOffset");
  if (displayOffset &&
      (displayOffset->type != 3 || displayOffset->number != 6)) {
    error = "nondefault minecart DisplayOffset is unsupported";
    return false;
  }
  return true;
}

nbt::Tag makeDoubleList(std::array<double, 3> values) {
  nbt::Tag list;
  list.type = 9;
  list.listElementType = 6;
  for (double value : values) {
    nbt::Tag item;
    item.type = 6;
    item.real = value;
    list.list.push_back(std::move(item));
  }
  return list;
}

nbt::Tag makeFloatList(std::array<float, 2> values) {
  nbt::Tag list;
  list.type = 9;
  list.listElementType = 5;
  for (float value : values) {
    nbt::Tag item;
    item.type = 5;
    item.real = value;
    list.list.push_back(std::move(item));
  }
  return list;
}

void add(nbt::Tag &compound, std::string name, nbt::Tag value) {
  compound.compoundOrder.push_back(name);
  compound.compound.emplace(std::move(name), std::move(value));
}

nbt::Tag integerTag(std::uint8_t type, std::int64_t value) {
  nbt::Tag tag;
  tag.type = type;
  tag.number = value;
  return tag;
}

nbt::Tag stringTag(std::string value) {
  nbt::Tag tag;
  tag.type = 8;
  tag.text = std::move(value);
  return tag;
}

} // namespace

DecodedEntityChunk decodeEntityChunk(std::span<const std::uint8_t> bytes) {
  DecodedEntityChunk result;
  try {
    result.root = nbt::parseRoot(bytes);
  } catch (const std::exception &error) {
    result.status = DecodeStatus::Corrupt;
    result.detail = error.what();
    return result;
  }
  const auto version = integer(result.root.value, "DataVersion");
  const nbt::Tag *position = required(result.root.value, "Position", 11);
  const nbt::Tag *entities = required(result.root.value, "Entities", 9);
  // EntityStorage's envelope is a compound root with DataVersion, Position,
  // and Entities. Reject unknown root fields instead of silently preserving a
  // template that this compact codec cannot describe.
  static const std::set<std::string> rootFields{
      "DataVersion", "Position", "Entities"};
  for (const auto &[name, value] : result.root.value.compound) {
    (void)value;
    if (!rootFields.contains(name)) {
      result.status = DecodeStatus::Unsupported;
      result.detail = "unsupported entity root field: " + name;
      return result;
    }
  }
  if (!version || *version != kDataVersion) {
    result.status = DecodeStatus::Unsupported;
    result.detail = "entity DataVersion is not 4786";
    return result;
  }
  if (!position || position->ints.size() != 2) {
    result.status = DecodeStatus::Corrupt;
    result.detail = "entity Position must be IntArray[2]";
    return result;
  }
  result.chunkX = position->ints[0];
  result.chunkZ = position->ints[1];
  if (result.chunkX < kMinChunkCoordinate ||
      result.chunkX > kMaxChunkCoordinate ||
      result.chunkZ < kMinChunkCoordinate ||
      result.chunkZ > kMaxChunkCoordinate) {
    result.status = DecodeStatus::Unsupported;
    result.detail = "entity Position is outside supported world bounds";
    return result;
  }
  if (!entities ||
      (!entities->list.empty() && entities->listElementType != 10)) {
    result.status = DecodeStatus::Corrupt;
    result.detail = "Entities is not a compound list";
    return result;
  }
  std::set<std::array<std::int32_t, 4>> uuids;
  for (const nbt::Tag &entry : entities->list) {
    const nbt::Tag *id = required(entry, "id", 8);
    std::string error;
    if (id && id->text == "minecraft:tnt") {
      TntEntityData entity;
      if (!decodeTnt(entry, result.chunkX, result.chunkZ, entity, error)) {
        result.status = DecodeStatus::Unsupported;
        result.detail = std::move(error);
        return result;
      }
      if (!uuids.insert(entity.uuid).second) {
        result.status = DecodeStatus::Unsupported;
        result.detail = "duplicate entity UUID";
        return result;
      }
      result.records.emplace_back(std::move(entity));
      continue;
    }
    if (id && id->text == "minecraft:minecart") {
      MinecartEntityData entity;
      if (!decodeMinecart(entry, result.chunkX, result.chunkZ, entity,
                           error)) {
        result.status = DecodeStatus::Unsupported;
        result.detail = std::move(error);
        return result;
      }
      if (!uuids.insert(entity.uuid).second) {
        result.status = DecodeStatus::Unsupported;
        result.detail = "duplicate entity UUID";
        return result;
      }
      result.records.emplace_back(std::move(entity));
      continue;
    }
    if (id && id->text == "minecraft:falling_block") {
      FallingBlockEntityData entity;
      if (!decodeFallingBlock(entry, result.chunkX, result.chunkZ, entity,
                              error)) {
        result.status = DecodeStatus::Unsupported;
        result.detail = std::move(error);
        return result;
      }
      if (!uuids.insert(entity.uuid).second) {
        result.status = DecodeStatus::Unsupported;
        result.detail = "duplicate entity UUID";
        return result;
      }
      result.records.emplace_back(std::move(entity));
      continue;
    }
    ItemEntityData item;
    if (!decodeItem(entry, result.chunkX, result.chunkZ, item, error)) {
      result.status = DecodeStatus::Unsupported;
      result.detail = std::move(error);
      return result;
    }
    if (!uuids.insert(item.uuid).second) {
      result.status = DecodeStatus::Unsupported;
      result.detail = "duplicate entity UUID";
      return result;
    }
    result.records.emplace_back(std::move(item));
  }
  result.status = DecodeStatus::Ok;
  return result;
}

std::optional<ItemEntityData> decodeItemRecord(
    const nbt::Tag &entity, std::int32_t chunkX, std::int32_t chunkZ,
    std::string &error) {
  ItemEntityData result;
  if (!decodeItem(entity, chunkX, chunkZ, result, error))
    return std::nullopt;
  return result;
}

std::optional<TntEntityData> decodeTntRecord(
    const nbt::Tag &entity, std::int32_t chunkX, std::int32_t chunkZ,
    std::string &error) {
  TntEntityData result;
  if (!decodeTnt(entity, chunkX, chunkZ, result, error))
    return std::nullopt;
  return result;
}

std::optional<FallingBlockEntityData>
decodeFallingBlockRecord(const nbt::Tag &entity, std::int32_t chunkX,
                          std::int32_t chunkZ, std::string &error) {
  FallingBlockEntityData result;
  if (!decodeFallingBlock(entity, chunkX, chunkZ, result, error))
    return std::nullopt;
  return result;
}

std::optional<MinecartEntityData>
decodeMinecartRecord(const nbt::Tag &entity, std::int32_t chunkX,
                      std::int32_t chunkZ, std::string &error) {
  MinecartEntityData result;
  if (!decodeMinecart(entity, chunkX, chunkZ, result, error))
    return std::nullopt;
  return result;
}

nbt::Tag encodeItemEntity(const ItemEntityData &item,
                          std::string &error) {
  ItemEntityData normalized = item;
  if (!normalizeItemEntity(normalized, error))
    return {};
  if (normalized.uuid == std::array<std::int32_t, 4>{} ||
      normalized.itemId.empty() || normalized.itemCount <= 0 ||
      normalized.health < 0 ||
      normalized.health > std::numeric_limits<std::int16_t>::max() ||
      normalized.age < std::numeric_limits<std::int16_t>::min() ||
      normalized.age > std::numeric_limits<std::int16_t>::max() ||
      normalized.pickupDelay < std::numeric_limits<std::int16_t>::min() ||
      normalized.pickupDelay > std::numeric_limits<std::int16_t>::max() ||
      normalized.fireTicks < std::numeric_limits<std::int16_t>::min() ||
      normalized.fireTicks > std::numeric_limits<std::int16_t>::max()) {
    error = "item entity snapshot is invalid";
    return {};
  }
  nbt::Tag entity;
  entity.type = 10;
  add(entity, "id", stringTag("minecraft:item"));
  nbt::Tag uuid;
  uuid.type = 11;
  uuid.ints.assign(normalized.uuid.begin(), normalized.uuid.end());
  add(entity, "UUID", std::move(uuid));
  add(entity, "Pos", makeDoubleList(normalized.pos));
  add(entity, "Motion", makeDoubleList(normalized.motion));
  add(entity, "Rotation", makeFloatList(normalized.rotation));
  add(entity, "Health", integerTag(2, normalized.health));
  add(entity, "Age", integerTag(2, normalized.age));
  add(entity, "PickupDelay", integerTag(2, normalized.pickupDelay));
  nbt::Tag stack;
  stack.type = 10;
  add(stack, "id", stringTag(normalized.itemId));
  add(stack, "count", integerTag(3, normalized.itemCount));
  add(entity, "Item", std::move(stack));
  add(entity, "OnGround", integerTag(1, normalized.onGround ? 1 : 0));
  add(entity, "Fire", integerTag(2, normalized.fireTicks));
  add(entity, "fall_distance", integerTag(6, 0));
  entity.compound["fall_distance"].real = normalized.fallDistance;
  add(entity, "Air", integerTag(2, 300));
  add(entity, "PortalCooldown", integerTag(3, 0));
  add(entity, "Invulnerable", integerTag(1, 0));
  return entity;
}

nbt::Tag encodeTntEntity(const TntEntityData &source,
                         std::string &error) {
  TntEntityData entity = source;
  if (!normalizeTntEntity(entity, error) ||
      entity.uuid == std::array<std::int32_t, 4>{} ||
      entity.fuse < std::numeric_limits<std::int16_t>::min() ||
      entity.fuse > std::numeric_limits<std::int16_t>::max() ||
      !std::isfinite(entity.explosionPower) ||
      entity.explosionPower < 0.0f || entity.explosionPower > 128.0f) {
    if (error.empty())
      error = "primed tnt snapshot is invalid";
    return {};
  }
  nbt::Tag tag;
  tag.type = 10;
  add(tag, "id", stringTag("minecraft:tnt"));
  nbt::Tag uuid;
  uuid.type = 11;
  uuid.ints.assign(entity.uuid.begin(), entity.uuid.end());
  add(tag, "UUID", std::move(uuid));
  add(tag, "Pos", makeDoubleList(entity.pos));
  add(tag, "Motion", makeDoubleList(entity.motion));
  add(tag, "Rotation", makeFloatList(entity.rotation));
  add(tag, "OnGround", integerTag(1, entity.onGround ? 1 : 0));
  add(tag, "Fire", integerTag(2, entity.fireTicks));
  add(tag, "fall_distance", integerTag(6, 0));
  tag.compound["fall_distance"].real = entity.fallDistance;
  add(tag, "Air", integerTag(2, 300));
  add(tag, "PortalCooldown", integerTag(3, 0));
  add(tag, "Invulnerable", integerTag(1, 0));
  add(tag, "fuse", integerTag(2, entity.fuse));
  nbt::Tag blockState;
  blockState.type = 10;
  add(blockState, "Name", stringTag("minecraft:tnt"));
  nbt::Tag properties;
  properties.type = 10;
  add(properties, "unstable", stringTag("false"));
  add(blockState, "Properties", std::move(properties));
  add(tag, "block_state", std::move(blockState));
  if (entity.explosionPower != 4.0f) {
    nbt::Tag power;
    power.type = 5;
    power.real = entity.explosionPower;
    add(tag, "explosion_power", std::move(power));
  }
  return tag;
}

nbt::Tag encodeFallingBlockEntity(const FallingBlockEntityData &source,
                                    std::string &error) {
  FallingBlockEntityData entity = source;
  if (!normalizeFallingBlockEntity(entity, error) ||
      entity.uuid == EntityUuid{} || entity.blockId != "minecraft:sand" ||
      entity.time < 0 || entity.time >= std::numeric_limits<int>::max() ||
      entity.hurtEntities || entity.fallDamagePerDistance != 0.0f ||
      entity.fallDamageMax != 40) {
    if (error.empty())
      error = "falling block snapshot is invalid";
    return {};
  }
  nbt::Tag tag;
  tag.type = 10;
  add(tag, "id", stringTag("minecraft:falling_block"));
  nbt::Tag uuid;
  uuid.type = 11;
  uuid.ints.assign(entity.uuid.begin(), entity.uuid.end());
  add(tag, "UUID", std::move(uuid));
  add(tag, "Pos", makeDoubleList(entity.pos));
  add(tag, "Motion", makeDoubleList(entity.motion));
  add(tag, "Rotation", makeFloatList(entity.rotation));
  add(tag, "OnGround", integerTag(1, entity.onGround ? 1 : 0));
  add(tag, "Fire", integerTag(2, entity.fireTicks));
  add(tag, "fall_distance", integerTag(6, 0));
  tag.compound["fall_distance"].real = entity.fallDistance;
  add(tag, "Air", integerTag(2, 300));
  add(tag, "PortalCooldown", integerTag(3, 0));
  add(tag, "Invulnerable", integerTag(1, 0));
  nbt::Tag block;
  block.type = 10;
  add(block, "Name", stringTag("minecraft:sand"));
  add(tag, "BlockState", std::move(block));
  add(tag, "Time", integerTag(3, entity.time));
  add(tag, "DropItem", integerTag(1, entity.dropItem ? 1 : 0));
  add(tag, "HurtEntities", integerTag(1, 0));
  nbt::Tag damagePerDistance;
  damagePerDistance.type = 5;
  damagePerDistance.real = 0.0;
  add(tag, "FallHurtAmount", std::move(damagePerDistance));
  add(tag, "FallHurtMax", integerTag(3, 40));
  add(tag, "CancelDrop", integerTag(1, entity.cancelDrop ? 1 : 0));
  return tag;
}

nbt::Tag encodeMinecartEntity(const MinecartEntityData &source,
                                 std::string &error) {
  MinecartEntityData entity = source;
  if (!normalizeMinecartEntity(entity, error) ||
      entity.uuid == EntityUuid{}) {
    if (error.empty())
      error = "minecart snapshot is invalid";
    return {};
  }
  nbt::Tag tag;
  tag.type = 10;
  add(tag, "id", stringTag("minecraft:minecart"));
  nbt::Tag uuid;
  uuid.type = 11;
  uuid.ints.assign(entity.uuid.begin(), entity.uuid.end());
  add(tag, "UUID", std::move(uuid));
  add(tag, "Pos", makeDoubleList(entity.pos));
  add(tag, "Motion", makeDoubleList(entity.motion));
  add(tag, "Rotation", makeFloatList(entity.rotation));
  add(tag, "OnGround", integerTag(1, entity.onGround ? 1 : 0));
  add(tag, "Fire", integerTag(2, entity.fireTicks));
  add(tag, "fall_distance", integerTag(6, 0));
  tag.compound["fall_distance"].real = entity.fallDistance;
  add(tag, "Air", integerTag(2, 300));
  add(tag, "PortalCooldown", integerTag(3, 0));
  add(tag, "Invulnerable", integerTag(1, 0));
  add(tag, "FlippedRotation", integerTag(1, entity.flipped ? 1 : 0));
  add(tag, "HasTicked", integerTag(1, entity.firstTick ? 1 : 0));
  return tag;
}

EntityUuid recordUuid(const EntityRecord &record) {
  return std::visit([](const auto &value) { return value.uuid; },
                    record.payload);
}

bool patchEntityRecords(nbt::RootTag &root,
                        std::span<const ItemEntityData> items,
                        std::span<const TntEntityData> tnts,
                        std::string &error) {
  return patchEntityItems(root, items, tnts, error);
}

bool patchEntitySequence(nbt::RootTag &root,
                         std::span<const EntityRecord> records,
                         std::string &error) {
  error.clear();
  const auto version = integer(root.value, "DataVersion");
  const nbt::Tag *position = required(root.value, "Position", 11);
  if (!version || *version != kDataVersion || !position ||
      position->ints.size() != 2) {
    error = "entity root is not a validated 4786 chunk";
    return false;
  }
  const int chunkX = position->ints[0];
  const int chunkZ = position->ints[1];
  if (chunkX < kMinChunkCoordinate || chunkX > kMaxChunkCoordinate ||
      chunkZ < kMinChunkCoordinate || chunkZ > kMaxChunkCoordinate) {
    error = "entity root Position is outside supported world bounds";
    return false;
  }
  std::set<EntityUuid> uuids;
  nbt::Tag replacement;
  replacement.type = 9;
  replacement.listElementType = 10;
  replacement.list.reserve(records.size());
  for (const EntityRecord &record : records) {
    nbt::Tag tag;
    EntityUuid uuid{};
    std::array<double, 3> pos{};
    std::visit(
        [&](const auto &value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, ItemEntityData>)
            tag = encodeItemEntity(value, error);
          else if constexpr (std::is_same_v<T, TntEntityData>)
            tag = encodeTntEntity(value, error);
          else if constexpr (std::is_same_v<T, FallingBlockEntityData>)
            tag = encodeFallingBlockEntity(value, error);
          else
            tag = encodeMinecartEntity(value, error);
          uuid = value.uuid;
          pos = value.pos;
        },
        record.payload);
    if (tag.type == 0)
      return false;
    if (uuid == EntityUuid{} || !uuids.insert(uuid).second) {
      error = "duplicate or zero entity UUID";
      return false;
    }
    if (floorDiv16(pos[0]) != chunkX || floorDiv16(pos[2]) != chunkZ) {
      error = "entity position is outside its chunk";
      return false;
    }
    replacement.list.push_back(std::move(tag));
  }
  auto found = root.value.compound.find("Entities");
  if (found == root.value.compound.end()) {
    root.value.compoundOrder.push_back("Entities");
    root.value.compound.emplace("Entities", std::move(replacement));
  } else {
    found->second = std::move(replacement);
  }
  return true;
}

bool patchEntityItems(nbt::RootTag &root,
                      std::span<const ItemEntityData> items,
                      std::span<const TntEntityData> tnts,
                      std::string &error) {
  std::vector<EntityRecord> records;
  records.reserve(items.size() + tnts.size());
  for (const auto &item : items)
    records.emplace_back(item);
  for (const auto &tnt : tnts)
    records.emplace_back(tnt);
  return patchEntitySequence(root, records, error);
}

nbt::RootTag emptyEntityRoot(std::int32_t chunkX, std::int32_t chunkZ) {
  nbt::RootTag root;
  root.value.type = 10;
  add(root.value, "DataVersion", integerTag(3, kDataVersion));
  nbt::Tag position;
  position.type = 11;
  position.ints = {chunkX, chunkZ};
  add(root.value, "Position", std::move(position));
  nbt::Tag entities;
  entities.type = 9;
  entities.listElementType = 10;
  add(root.value, "Entities", std::move(entities));
  return root;
}

std::vector<std::uint8_t> encodeEntityChunk(const nbt::RootTag &root) {
  return nbt::writeRoot(root);
}

} // namespace redstone::entity_codec
