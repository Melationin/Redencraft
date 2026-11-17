#include "redstone/entity_codec.hpp"
#include "redstone/entity.hpp"
#include "redstone/item.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace redstone::entity_codec {
namespace {

constexpr int kDataVersion = 4786;
constexpr int kMaxPassengerDepth = 32;

void addTag(nbt::Tag &compound, std::string name, nbt::Tag value) {
  compound.compoundOrder.push_back(name);
  compound.compound.emplace(std::move(name), std::move(value));
}

nbt::Tag intTag(std::uint8_t type, std::int64_t value) {
  nbt::Tag t;
  t.type = type;
  t.number = value;
  return t;
}

nbt::Tag realTag(std::uint8_t type, double value) {
  nbt::Tag t;
  t.type = type;
  t.real = value;
  return t;
}

nbt::Tag strTag(std::string value) {
  nbt::Tag t;
  t.type = 8;
  t.text = std::move(value);
  return t;
}

template <std::size_t N>
nbt::Tag makeRealList(const std::array<double, N> &values, std::uint8_t elemType) {
  nbt::Tag list;
  list.type = 9;
  list.listElementType = elemType;
  list.list.reserve(N);
  for (double v : values)
    list.list.push_back(realTag(elemType, v));
  return list;
}

nbt::Tag makeFloatList(std::initializer_list<float> values) {
  nbt::Tag list;
  list.type = 9;
  list.listElementType = 5;
  list.list.reserve(values.size());
  for (float v : values)
    list.list.push_back(realTag(5, v));
  return list;
}

nbt::Tag makeIntArray(std::initializer_list<std::int32_t> values) {
  nbt::Tag tag;
  tag.type = 11;
  tag.ints.assign(values.begin(), values.end());
  return tag;
}

const nbt::Tag *field(const nbt::Tag &compound, std::string_view key) {
  if (compound.type != 10)
    return nullptr;
  auto it = compound.compound.find(std::string(key));
  return it == compound.compound.end() ? nullptr : &it->second;
}

std::optional<std::int64_t> integerField(const nbt::Tag &compound,
                                         std::string_view key) {
  const nbt::Tag *t = field(compound, key);
  if (!t || (t->type != 1 && t->type != 2 && t->type != 3 && t->type != 4))
    return std::nullopt;
  return t->number;
}

std::optional<double> realField(const nbt::Tag &compound, std::string_view key) {
  const nbt::Tag *t = field(compound, key);
  if (!t)
    return std::nullopt;
  if (t->type == 5 || t->type == 6)
    return t->real;
  if (t->type >= 1 && t->type <= 4)
    return static_cast<double>(t->number);
  return std::nullopt;
}

bool readVec3List(const nbt::Tag &compound, std::string_view key, Vec3 &out) {
  const nbt::Tag *t = field(compound, key);
  if (!t || t->type != 9 || t->list.size() != 3)
    return false;
  std::array<double, 3> vals{};
  for (std::size_t i = 0; i < 3; ++i) {
    const nbt::Tag &e = t->list[i];
    if (e.type != 5 && e.type != 6)
      return false;
    if (!std::isfinite(e.real))
      return false;
    vals[i] = e.real;
  }
  out = {vals[0], vals[1], vals[2]};
  return true;
}

bool readRot2List(const nbt::Tag &compound, std::string_view key, float &yaw,
                  float &pitch) {
  const nbt::Tag *t = field(compound, key);
  if (!t) {
    yaw = 0.0f;
    pitch = 0.0f;
    return true;
  }
  if (t->type != 9 || t->list.size() != 2)
    return false;
  for (const nbt::Tag &e : t->list) {
    if ((e.type != 5 && e.type != 6) || !std::isfinite(e.real))
      return false;
  }
  yaw = static_cast<float>(t->list[0].real);
  pitch = static_cast<float>(t->list[1].real);
  return true;
}

nbt::Tag encodeBlockStateTag(const BlockState &state) {
  nbt::Tag tag;
  tag.type = 10;
  const BlockState effective = state.isAir() ? airState() : state;
  addTag(tag, "Name",
         strTag(state.isAir() ? "minecraft:air" : state.name()));
  const auto props = effective.properties();
  if (!props.empty()) {
    nbt::Tag ptag;
    ptag.type = 10;
    for (const auto &[k, v] : props)
      addTag(ptag, k, strTag(v));
    addTag(tag, "Properties", std::move(ptag));
  }
  return tag;
}

bool decodeBlockStateTag(const nbt::Tag &tag, BlockState &out) {
  if (tag.type != 10)
    return false;
  const nbt::Tag *nameTag = field(tag, "Name");
  if (!nameTag || nameTag->type != 8 || nameTag->text.empty())
    return false;
  const Block *block = findBlockByName(nameTag->text);
  if (!block || !block->supported())
    return false;
  std::map<std::string, std::string> props;
  if (const nbt::Tag *ptag = field(tag, "Properties")) {
    if (ptag->type != 10)
      return false;
    for (const auto &[k, v] : ptag->compound) {
      if (v.type != 8)
        return false;
      props.emplace(k, v.text);
    }
  }
  std::uint32_t variant = 0;
  if (!block->stateDefinition().variantOf(props, variant))
    return false;
  out = BlockState::fromId(block->firstState() + variant);
  return true;
}

nbt::Tag encodeItemStackTag(const ItemStack &stack) {
  return encodeItemStackNbt(stack, true);
}

bool decodeItemStackTag(const nbt::Tag &tag, ItemStack &out) {
  if (tag.type != 10)
    return false;
  const nbt::Tag *idTag = field(tag, "id");
  if (!idTag || idTag->type != 8 || idTag->text.empty())
    return false;
  const auto cnt = integerField(tag, "count");
  if (!cnt || *cnt <= 0 || *cnt > 99)
    return false;
  auto decoded = decodeItemStackNbt(tag);
  if (!decoded)
    return false;
  out = std::move(*decoded);
  return true;
}

void encodeContainerItems(nbt::Tag &entityTag, const ContainerEntity &c) {
  nbt::Tag list;
  list.type = 9;
  list.listElementType = 10;
  const int size = c.containerSize();
  for (int slot = 0; slot < size; ++slot) {
    const ItemStack &stack = c.getContainerItem(slot);
    if (stack.empty())
      continue;
    nbt::Tag entry = encodeItemStackTag(stack);
    addTag(entry, "Slot", intTag(1, slot));
    list.list.push_back(std::move(entry));
  }
  addTag(entityTag, "Items", std::move(list));
}

bool decodeContainerItems(const nbt::Tag &entityTag, ContainerEntity &c,
                          World &world, std::string &error) {
  const nbt::Tag *list = field(entityTag, "Items");
  if (!list)
    return true;
  if (list->type != 9) {
    error = "container entity Items must be TAG_List";
    return false;
  }
  const int size = c.containerSize();
  for (const nbt::Tag &entry : list->list) {
    const auto slot = integerField(entry, "Slot");
    ItemStack stack;
    if (!slot || *slot < 0 || *slot >= size ||
        !decodeItemStackTag(entry, stack)) {
      error = "invalid container item slot entry";
      return false;
    }
    c.setContainerItem(static_cast<int>(*slot), std::move(stack));
  }
  c.setContainerChanged(world);
  return true;
}

nbt::Tag encodeRotations(const Rotations3f &r) {
  return makeFloatList({r.x, r.y, r.z});
}

bool decodeRotations(const nbt::Tag *tag, Rotations3f &out) {
  if (!tag)
    return true;
  if (tag->type != 9 || tag->list.size() != 3)
    return false;
  for (const nbt::Tag &e : tag->list) {
    if ((e.type != 5 && e.type != 6) || !std::isfinite(e.real))
      return false;
  }
  out = {static_cast<float>(tag->list[0].real),
         static_cast<float>(tag->list[1].real),
         static_cast<float>(tag->list[2].real)};
  return true;
}

constexpr std::pair<EquipmentSlot, const char *> kEquipmentSlotNames[] = {
    {EquipmentSlot::MainHand, "mainhand"},
    {EquipmentSlot::OffHand, "offhand"},
    {EquipmentSlot::Feet, "feet"},
    {EquipmentSlot::Legs, "legs"},
    {EquipmentSlot::Chest, "chest"},
    {EquipmentSlot::Head, "head"},
    {EquipmentSlot::Body, "body"},
    {EquipmentSlot::Saddle, "saddle"},
};

nbt::Tag encodeEntityRecursive(const Entity &entity, std::string &error,
                               int depth) {
  if (depth > kMaxPassengerDepth) {
    error = "entity passenger hierarchy exceeds max depth";
    return {};
  }
  if (entity.isPlayer()) {
    error = "player entities are not chunk-serialized";
    return {};
  }
  const std::string &typeId = entity.typeName();
  const bool isSupportedBoat =
      typeId == "minecraft:bamboo_raft" ||
      typeId == "minecraft:bamboo_chest_raft" ||
      typeId.ends_with("_boat") || typeId.ends_with("_chest_boat");
  const bool isKnownNonAi =
      typeId == "minecraft:item" || typeId == "minecraft:tnt" ||
      typeId == "minecraft:falling_block" || typeId == "minecraft:minecart" ||
      typeId == "minecraft:chest_minecart" ||
      typeId == "minecraft:hopper_minecart" ||
      typeId == "minecraft:tnt_minecart" ||
      typeId == "minecraft:furnace_minecart" || typeId == "minecraft:arrow" ||
      typeId == "minecraft:spectral_arrow" || typeId == "minecraft:trident" ||
      typeId == "minecraft:snowball" || typeId == "minecraft:egg" ||
      typeId == "minecraft:ender_pearl" ||
      typeId == "minecraft:experience_bottle" ||
      typeId == "minecraft:wind_charge" || typeId == "minecraft:item_frame" ||
      typeId == "minecraft:glow_item_frame" ||
      typeId == "minecraft:end_crystal" ||
      typeId == "minecraft:experience_orb" ||
      typeId == "minecraft:armor_stand" || isSupportedBoat;
  if (!isKnownNonAi) {
    // TODO: Stage E-5 Mob AI entity persistence is not implemented yet.
    error = "unsupported entity type for codec: " + typeId;
    return {};
  }

  nbt::Tag tag;
  tag.type = 10;
  addTag(tag, "id", strTag(typeId));
  addTag(tag, "UUID",
         makeIntArray({entity.uuid[0], entity.uuid[1], entity.uuid[2],
                       entity.uuid[3]}));
  addTag(tag, "Pos",
         makeRealList<3>(
             {entity.position.x, entity.position.y, entity.position.z}, 6));
  addTag(tag, "Motion",
         makeRealList<3>({entity.deltaMovement.x, entity.deltaMovement.y,
                          entity.deltaMovement.z},
                         6));
  addTag(tag, "Rotation", makeFloatList({entity.yaw, entity.pitch}));
  addTag(tag, "OnGround", intTag(1, entity.onGround ? 1 : 0));
  addTag(tag, "Fire",
         intTag(2, std::clamp<int>(entity.remainingFireTicks, -32768, 32767)));
  addTag(tag, "fall_distance", realTag(6, entity.fallDistance));
  addTag(tag, "Air", intTag(2, entity.airSupply()));
  addTag(tag, "PortalCooldown", intTag(3, entity.portalCooldown));
  addTag(tag, "Invulnerable", intTag(1, 0));
  if (entity.isNoGravity())
    addTag(tag, "NoGravity", intTag(1, 1));
  if (entity.isSilent())
    addTag(tag, "Silent", intTag(1, 1));
  if (entity.isInvisible())
    addTag(tag, "Invisible", intTag(1, 1));

  if (const auto *item = dynamic_cast<const ItemEntity *>(&entity)) {
    addTag(tag, "Health", intTag(2, item->health));
    addTag(tag, "Age", intTag(2, item->age));
    addTag(tag, "PickupDelay", intTag(2, item->pickupDelay));
    addTag(tag, "Item", encodeItemStackTag(item->item));
  } else if (const auto *tnt = dynamic_cast<const PrimedTnt *>(&entity)) {
    addTag(tag, "fuse", intTag(2, tnt->fuse));
    addTag(tag, "block_state", encodeBlockStateTag(tnt->blockState));
    addTag(tag, "explosion_power", realTag(5, tnt->explosionPower));
  } else if (const auto *falling =
                 dynamic_cast<const FallingBlockEntity *>(&entity)) {
    addTag(tag, "BlockState", encodeBlockStateTag(falling->blockState));
    addTag(tag, "Time", intTag(3, falling->time));
    addTag(tag, "DropItem", intTag(1, falling->dropItem ? 1 : 0));
    addTag(tag, "HurtEntities", intTag(1, falling->hurtEntities ? 1 : 0));
    addTag(tag, "FallHurtAmount", realTag(5, falling->fallDamagePerDistance));
    addTag(tag, "FallHurtMax", intTag(3, falling->fallDamageMax));
    addTag(tag, "CancelDrop", intTag(1, falling->cancelDrop ? 1 : 0));
    if (falling->blockData.has_value())
      addTag(tag, "TileEntityData", *falling->blockData);
  } else if (const auto *cart = dynamic_cast<const Minecart *>(&entity)) {
    addTag(tag, "DisplayOffset", intTag(3, cart->displayOffset));
    addTag(tag, "FlippedRotation", intTag(1, cart->flipped ? 1 : 0));
    addTag(tag, "HasTicked", intTag(1, cart->firstTick ? 1 : 0));
    if (cart->customDisplayBlock.has_value())
      addTag(tag, "DisplayState",
             encodeBlockStateTag(*cart->customDisplayBlock));
    if (const auto *container = dynamic_cast<const ContainerEntity *>(cart))
      encodeContainerItems(tag, *container);
    if (const auto *hopper = dynamic_cast<const MinecartHopper *>(cart))
      addTag(tag, "Enabled", intTag(1, hopper->enabled ? 1 : 0));
    if (const auto *tntCart = dynamic_cast<const MinecartTNT *>(cart)) {
      addTag(tag, "fuse", intTag(3, tntCart->fuse));
      addTag(tag, "explosion_power", realTag(5, tntCart->explosionPowerBase));
      addTag(tag, "explosion_speed_factor",
             realTag(5, tntCart->explosionSpeedFactor));
    }
    if (const auto *furnace = dynamic_cast<const MinecartFurnace *>(cart)) {
      addTag(tag, "Fuel", intTag(2, furnace->fuel));
      addTag(tag, "PushX", realTag(6, furnace->pushForce.x));
      addTag(tag, "PushZ", realTag(6, furnace->pushForce.z));
    }
  } else if (const auto *boat = dynamic_cast<const AbstractBoat *>(&entity)) {
    if (const auto *container = dynamic_cast<const ContainerEntity *>(boat))
      encodeContainerItems(tag, *container);
  } else if (const auto *proj = dynamic_cast<const Projectile *>(&entity)) {
    if (proj->ownerUuid() != std::array<std::int32_t, 4>{}) {
      const auto &ou = proj->ownerUuid();
      addTag(tag, "Owner", makeIntArray({ou[0], ou[1], ou[2], ou[3]}));
    }
    if (const auto *arrow = dynamic_cast<const AbstractArrow *>(proj)) {
      addTag(tag, "pickup", intTag(1, static_cast<int>(arrow->pickup)));
      addTag(tag, "shake", intTag(1, arrow->shakeTime));
      addTag(tag, "damage", realTag(6, arrow->baseDamage()));
      addTag(tag, "crit", intTag(1, arrow->isCritArrow() ? 1 : 0));
      addTag(tag, "PierceLevel", intTag(1, arrow->pierceLevel()));
      addTag(tag, "inGround", intTag(1, arrow->isInGround() ? 1 : 0));
      addTag(tag, "life", intTag(2, arrow->life()));
      if (const auto *spec = dynamic_cast<const SpectralArrow *>(arrow))
        addTag(tag, "Duration", intTag(3, spec->duration));
    } else if (const auto *thrown =
                   dynamic_cast<const ThrowableItemProjectile *>(proj)) {
      if (!thrown->item.empty())
        addTag(tag, "Item", encodeItemStackTag(thrown->item));
    }
  } else if (const auto *frame = dynamic_cast<const ItemFrame *>(&entity)) {
    addTag(tag, "block_pos",
           makeIntArray({frame->attachPos().x, frame->attachPos().y,
                         frame->attachPos().z}));
    addTag(tag, "Facing",
           intTag(1, static_cast<int>(frame->attachDirection())));
    addTag(tag, "ItemRotation", intTag(1, frame->rotation()));
    addTag(tag, "ItemDropChance", realTag(5, frame->dropChance));
    addTag(tag, "Fixed", intTag(1, frame->fixed ? 1 : 0));
    if (!frame->framedItem().empty())
      addTag(tag, "Item", encodeItemStackTag(frame->framedItem()));
  } else if (const auto *crystal = dynamic_cast<const EndCrystal *>(&entity)) {
    addTag(tag, "ShowBottom", intTag(1, crystal->showBottom ? 1 : 0));
    if (crystal->beamTarget.has_value()) {
      addTag(tag, "beam_target",
             makeIntArray({crystal->beamTarget->x, crystal->beamTarget->y,
                           crystal->beamTarget->z}));
    }
  } else if (const auto *orb = dynamic_cast<const ExperienceOrb *>(&entity)) {
    addTag(tag, "Age", intTag(2, orb->age));
    addTag(tag, "Health", intTag(2, orb->health));
    addTag(tag, "Value", intTag(2, orb->xpValue));
    addTag(tag, "Count", intTag(3, orb->count));
  } else if (const auto *stand = dynamic_cast<const ArmorStand *>(&entity)) {
    addTag(tag, "Health", realTag(5, stand->health()));
    addTag(tag, "Small", intTag(1, stand->isSmall() ? 1 : 0));
    addTag(tag, "ShowArms", intTag(1, stand->showArms() ? 1 : 0));
    addTag(tag, "NoBasePlate", intTag(1, !stand->showBasePlate() ? 1 : 0));
    addTag(tag, "Marker", intTag(1, stand->isMarker() ? 1 : 0));
    addTag(tag, "DisabledSlots", intTag(3, stand->disabledSlots));

    nbt::Tag pose;
    pose.type = 10;
    addTag(pose, "Head", encodeRotations(stand->headPose()));
    addTag(pose, "Body", encodeRotations(stand->bodyPose()));
    addTag(pose, "LeftArm", encodeRotations(stand->leftArmPose()));
    addTag(pose, "RightArm", encodeRotations(stand->rightArmPose()));
    addTag(pose, "LeftLeg", encodeRotations(stand->leftLegPose()));
    addTag(pose, "RightLeg", encodeRotations(stand->rightLegPose()));
    addTag(tag, "Pose", std::move(pose));

    nbt::Tag eq;
    eq.type = 10;
    bool hasAnyEq = false;
    for (const auto &[slot, key] : kEquipmentSlotNames) {
      const ItemStack &s = stand->getItemBySlot(slot);
      if (!s.empty()) {
        addTag(eq, key, encodeItemStackTag(s));
        hasAnyEq = true;
      }
    }
    if (hasAnyEq)
      addTag(tag, "equipment", std::move(eq));
  }

  const auto passengers = entity.getPassengers();
  if (!passengers.empty()) {
    nbt::Tag pList;
    pList.type = 9;
    pList.listElementType = 10;
    pList.list.reserve(passengers.size());
    for (const auto &p : passengers) {
      if (!p || p->isRemoved())
        continue;
      nbt::Tag pt = encodeEntityRecursive(*p, error, depth + 1);
      if (pt.type == 0)
        return {};
      pList.list.push_back(std::move(pt));
    }
    if (!pList.list.empty())
      addTag(tag, "Passengers", std::move(pList));
  }

  return tag;
}

DecodedGeneralEntity decodeEntityRecursive(World &world, const nbt::Tag &tag,
                                           int depth) {
  DecodedGeneralEntity out;
  if (depth > kMaxPassengerDepth) {
    out.status = DecodeStatus::Corrupt;
    out.detail = "entity Passengers depth limit exceeded";
    return out;
  }
  if (tag.type != 10) {
    out.status = DecodeStatus::Corrupt;
    out.detail = "entity entry must be TAG_Compound";
    return out;
  }
  const nbt::Tag *idTag = field(tag, "id");
  if (!idTag || idTag->type != 8 || idTag->text.empty()) {
    out.status = DecodeStatus::Corrupt;
    out.detail = "entity missing id string";
    return out;
  }
  const std::string &typeId = idTag->text;

  Vec3 pos{};
  Vec3 motion{};
  if (!readVec3List(tag, "Pos", pos)) {
    out.status = DecodeStatus::Corrupt;
    out.detail = "entity missing valid Pos";
    return out;
  }
  if (field(tag, "Motion") && !readVec3List(tag, "Motion", motion)) {
    out.status = DecodeStatus::Corrupt;
    out.detail = "entity invalid Motion";
    return out;
  }
  float yaw = 0.0f;
  float pitch = 0.0f;
  if (!readRot2List(tag, "Rotation", yaw, pitch)) {
    out.status = DecodeStatus::Corrupt;
    out.detail = "entity invalid Rotation";
    return out;
  }

  std::shared_ptr<Entity> ent;
  const bool isChestBoat =
      typeId == "minecraft:bamboo_chest_raft" || typeId.ends_with("_chest_boat");
  const bool isPlainBoat =
      !isChestBoat &&
      (typeId == "minecraft:bamboo_raft" || typeId.ends_with("_boat"));

  if (typeId == "minecraft:item") {
    const nbt::Tag *itemTag = field(tag, "Item");
    ItemStack stack;
    if (!itemTag || !decodeItemStackTag(*itemTag, stack)) {
      out.status = DecodeStatus::Corrupt;
      out.detail = "item entity missing valid Item stack";
      return out;
    }
    auto e = std::make_shared<ItemEntity>(world, pos, std::move(stack), motion);
    if (auto age = integerField(tag, "Age"))
      e->age = static_cast<int>(*age);
    if (auto pd = integerField(tag, "PickupDelay"))
      e->pickupDelay = static_cast<int>(*pd);
    if (auto h = integerField(tag, "Health"))
      e->health = static_cast<int>(*h);
    ent = std::move(e);
  } else if (typeId == "minecraft:tnt") {
    auto e = std::make_shared<PrimedTnt>(world, pos, motion);
    if (auto f = integerField(tag, "fuse"))
      e->fuse = static_cast<int>(*f);
    if (const nbt::Tag *bs = field(tag, "block_state")) {
      BlockState parsed;
      if (!decodeBlockStateTag(*bs, parsed)) {
        out.status = DecodeStatus::Unsupported;
        out.detail = "unsupported tnt block_state";
        return out;
      }
      e->blockState = parsed;
    }
    if (auto power = realField(tag, "explosion_power"))
      e->explosionPower = static_cast<float>(*power);
    ent = std::move(e);
  } else if (typeId == "minecraft:falling_block") {
    BlockState state = defaultState("sand");
    if (const nbt::Tag *bs = field(tag, "BlockState")) {
      if (!decodeBlockStateTag(*bs, state)) {
        out.status = DecodeStatus::Unsupported;
        out.detail = "unsupported falling_block BlockState";
        return out;
      }
    }
    auto e = std::make_shared<FallingBlockEntity>(world, pos, state);
    if (auto t = integerField(tag, "Time"))
      e->time = static_cast<int>(*t);
    if (auto d = integerField(tag, "DropItem"))
      e->dropItem = *d != 0;
    if (auto c = integerField(tag, "CancelDrop"))
      e->cancelDrop = *c != 0;
    if (auto h = integerField(tag, "HurtEntities"))
      e->hurtEntities = *h != 0;
    if (auto fha = realField(tag, "FallHurtAmount"))
      e->fallDamagePerDistance = static_cast<float>(*fha);
    if (auto fhm = integerField(tag, "FallHurtMax"))
      e->fallDamageMax = static_cast<int>(*fhm);
    if (const nbt::Tag *td = field(tag, "TileEntityData"))
      e->blockData = *td;
    ent = std::move(e);
  } else if (typeId == "minecraft:minecart" ||
             typeId == "minecraft:chest_minecart" ||
             typeId == "minecraft:hopper_minecart" ||
             typeId == "minecraft:tnt_minecart" ||
             typeId == "minecraft:furnace_minecart") {
    std::shared_ptr<Minecart> e;
    if (typeId == "minecraft:chest_minecart")
      e = std::make_shared<MinecartChest>(world, pos);
    else if (typeId == "minecraft:hopper_minecart")
      e = std::make_shared<MinecartHopper>(world, pos);
    else if (typeId == "minecraft:tnt_minecart")
      e = std::make_shared<MinecartTNT>(world, pos);
    else if (typeId == "minecraft:furnace_minecart")
      e = std::make_shared<MinecartFurnace>(world, pos);
    else
      e = std::make_shared<Minecart>(world, pos);

    if (auto dOff = integerField(tag, "DisplayOffset"))
      e->displayOffset = static_cast<int>(*dOff);
    if (auto fl = integerField(tag, "FlippedRotation"))
      e->flipped = *fl != 0;
    if (auto ht = integerField(tag, "HasTicked"))
      e->firstTick = *ht != 0;
    if (const nbt::Tag *ds = field(tag, "DisplayState")) {
      BlockState custom;
      if (!decodeBlockStateTag(*ds, custom)) {
        out.status = DecodeStatus::Unsupported;
        out.detail = "unsupported minecart DisplayState";
        return out;
      }
      e->customDisplayBlock = custom;
    }
    if (auto *container = dynamic_cast<ContainerEntity *>(e.get())) {
      if (!decodeContainerItems(tag, *container, world, out.detail)) {
        out.status = DecodeStatus::Corrupt;
        return out;
      }
    }
    if (auto *hopper = dynamic_cast<MinecartHopper *>(e.get())) {
      if (auto en = integerField(tag, "Enabled"))
        hopper->enabled = *en != 0;
    }
    if (auto *tntCart = dynamic_cast<MinecartTNT *>(e.get())) {
      if (auto f = integerField(tag, "fuse"))
        tntCart->fuse = static_cast<int>(*f);
      if (auto p = realField(tag, "explosion_power"))
        tntCart->explosionPowerBase = static_cast<float>(*p);
      if (auto sf = realField(tag, "explosion_speed_factor"))
        tntCart->explosionSpeedFactor = static_cast<float>(*sf);
    }
    if (auto *furnace = dynamic_cast<MinecartFurnace *>(e.get())) {
      if (auto fu = integerField(tag, "Fuel"))
        furnace->fuel = static_cast<int>(*fu);
      auto px = realField(tag, "PushX");
      auto pz = realField(tag, "PushZ");
      if (px || pz)
        furnace->pushForce = {px.value_or(0.0), 0.0, pz.value_or(0.0)};
    }
    ent = std::move(e);
  } else if ((isChestBoat || isPlainBoat) && entityType(typeId) != nullptr) {
    if (isChestBoat) {
      auto e = std::make_shared<ChestBoat>(world, pos, typeId, typeId);
      if (!decodeContainerItems(tag, *e, world, out.detail)) {
        out.status = DecodeStatus::Corrupt;
        return out;
      }
      ent = std::move(e);
    } else {
      ent = std::make_shared<Boat>(world, pos, typeId, typeId);
    }
  } else if (typeId == "minecraft:arrow" ||
             typeId == "minecraft:spectral_arrow" ||
             typeId == "minecraft:trident") {
    std::shared_ptr<AbstractArrow> e;
    if (typeId == "minecraft:spectral_arrow")
      e = std::make_shared<SpectralArrow>();
    else if (typeId == "minecraft:trident")
      e = std::make_shared<ThrownTrident>();
    else
      e = std::make_shared<Arrow>();
    if (auto pk = integerField(tag, "pickup"))
      e->pickup = static_cast<AbstractArrow::Pickup>(
          std::clamp<std::int64_t>(*pk, 0, 2));
    if (auto sh = integerField(tag, "shake"))
      e->shakeTime = static_cast<int>(*sh);
    if (auto dmg = realField(tag, "damage"))
      e->setBaseDamage(*dmg);
    if (auto cr = integerField(tag, "crit"))
      e->setCritArrow(*cr != 0);
    if (auto pl = integerField(tag, "PierceLevel"))
      e->setPierceLevel(static_cast<std::uint8_t>(*pl));
    if (auto ig = integerField(tag, "inGround"))
      e->setInGround(*ig != 0);
    if (auto lf = integerField(tag, "life"))
      e->setLife(static_cast<int>(*lf));
    if (auto *spec = dynamic_cast<SpectralArrow *>(e.get())) {
      if (auto dur = integerField(tag, "Duration"))
        spec->duration = static_cast<int>(*dur);
    }
    ent = std::move(e);
  } else if (typeId == "minecraft:snowball" || typeId == "minecraft:egg" ||
             typeId == "minecraft:ender_pearl" ||
             typeId == "minecraft:experience_bottle" ||
             typeId == "minecraft:wind_charge") {
    if (typeId == "minecraft:wind_charge") {
      ent = std::make_shared<WindCharge>();
    } else {
      std::shared_ptr<ThrowableItemProjectile> e;
      if (typeId == "minecraft:snowball")
        e = std::make_shared<Snowball>();
      else if (typeId == "minecraft:egg")
        e = std::make_shared<ThrownEgg>();
      else if (typeId == "minecraft:ender_pearl")
        e = std::make_shared<ThrownEnderpearl>();
      else
        e = std::make_shared<ThrownExperienceBottle>();
      if (const nbt::Tag *it = field(tag, "Item")) {
        ItemStack stack;
        if (decodeItemStackTag(*it, stack))
          e->item = std::move(stack);
      }
      ent = std::move(e);
    }
  } else if (typeId == "minecraft:item_frame" ||
             typeId == "minecraft:glow_item_frame") {
    Pos attachPos{static_cast<int>(std::floor(pos.x)),
                  static_cast<int>(std::floor(pos.y)),
                  static_cast<int>(std::floor(pos.z))};
    if (const nbt::Tag *bp = field(tag, "block_pos")) {
      if (bp->type == 11 && bp->ints.size() == 3)
        attachPos = {bp->ints[0], bp->ints[1], bp->ints[2]};
    }
    Direction dir = Direction::South;
    if (auto f = integerField(tag, "Facing"))
      dir = static_cast<Direction>(std::clamp<std::int64_t>(*f, 0, 5));
    auto e = std::make_shared<ItemFrame>(
        attachPos, dir, typeId == "minecraft:glow_item_frame");
    if (const nbt::Tag *it = field(tag, "Item")) {
      ItemStack stack;
      if (decodeItemStackTag(*it, stack))
        e->setFramedItem(world, std::move(stack));
    }
    if (auto rot = integerField(tag, "ItemRotation"))
      e->setRotation(world, static_cast<int>(*rot));
    if (auto dc = realField(tag, "ItemDropChance"))
      e->dropChance = static_cast<float>(*dc);
    if (auto fx = integerField(tag, "Fixed"))
      e->fixed = *fx != 0;
    ent = std::move(e);
  } else if (typeId == "minecraft:end_crystal") {
    auto e = std::make_shared<EndCrystal>(world, pos);
    if (auto sb = integerField(tag, "ShowBottom"))
      e->showBottom = *sb != 0;
    if (const nbt::Tag *bt = field(tag, "beam_target")) {
      if (bt->type == 11 && bt->ints.size() == 3)
        e->beamTarget = Pos{bt->ints[0], bt->ints[1], bt->ints[2]};
    }
    ent = std::move(e);
  } else if (typeId == "minecraft:experience_orb") {
    const int xpVal =
        static_cast<int>(integerField(tag, "Value").value_or(1));
    auto e = std::make_shared<ExperienceOrb>(world, pos, xpVal);
    if (auto ag = integerField(tag, "Age"))
      e->age = static_cast<int>(*ag);
    if (auto h = integerField(tag, "Health"))
      e->health = static_cast<int>(*h);
    if (auto cnt = integerField(tag, "Count"))
      e->count = std::max(1, static_cast<int>(*cnt));
    ent = std::move(e);
  } else if (typeId == "minecraft:armor_stand") {
    auto e = std::make_shared<ArmorStand>(world, pos);
    if (auto h = realField(tag, "Health"))
      e->setHealth(static_cast<float>(*h));
    if (auto sm = integerField(tag, "Small"))
      e->setSmall(*sm != 0);
    if (auto sa = integerField(tag, "ShowArms"))
      e->setShowArms(*sa != 0);
    if (auto nbp = integerField(tag, "NoBasePlate"))
      e->setNoBasePlate(*nbp != 0);
    if (auto mk = integerField(tag, "Marker"))
      e->setMarker(*mk != 0);
    if (auto ds = integerField(tag, "DisabledSlots"))
      e->disabledSlots = static_cast<int>(*ds);
    if (const nbt::Tag *poseTag = field(tag, "Pose")) {
      Rotations3f r;
      if (decodeRotations(field(*poseTag, "Head"), r))
        e->setHeadPose(r);
      if (decodeRotations(field(*poseTag, "Body"), r))
        e->setBodyPose(r);
      if (decodeRotations(field(*poseTag, "LeftArm"), r))
        e->setLeftArmPose(r);
      if (decodeRotations(field(*poseTag, "RightArm"), r))
        e->setRightArmPose(r);
      if (decodeRotations(field(*poseTag, "LeftLeg"), r))
        e->setLeftLegPose(r);
      if (decodeRotations(field(*poseTag, "RightLeg"), r))
        e->setRightLegPose(r);
    }
    if (const nbt::Tag *eq = field(tag, "equipment")) {
      for (const auto &[slot, key] : kEquipmentSlotNames) {
        if (const nbt::Tag *st = field(*eq, key)) {
          ItemStack stack;
          if (decodeItemStackTag(*st, stack))
            e->setItemSlot(slot, std::move(stack));
        }
      }
    }
    ent = std::move(e);
  } else {
    // TODO: Stage E-5 Mob AI entity decoding is not implemented yet.
    out.status = DecodeStatus::Unsupported;
    out.detail = "unsupported entity type: " + typeId;
    return out;
  }

  ent->setPos(pos);
  ent->deltaMovement = motion;
  ent->yaw = yaw;
  ent->pitch = pitch;
  if (const nbt::Tag *u = field(tag, "UUID")) {
    if (u->type != 11 || u->ints.size() != 4) {
      out.status = DecodeStatus::Corrupt;
      out.detail = "entity UUID must be IntArray[4]";
      return out;
    }
    ent->uuid = {u->ints[0], u->ints[1], u->ints[2], u->ints[3]};
  }
  if (auto og = integerField(tag, "OnGround"))
    ent->onGround = *og != 0;
  if (auto fr = integerField(tag, "Fire"))
    ent->remainingFireTicks = static_cast<int>(*fr);
  if (auto fd = realField(tag, "fall_distance"))
    ent->fallDistance = *fd;
  if (auto air = integerField(tag, "Air"))
    ent->setAirSupply(static_cast<int>(*air));
  if (auto pc = integerField(tag, "PortalCooldown"))
    ent->portalCooldown = static_cast<int>(*pc);
  if (auto ng = integerField(tag, "NoGravity"))
    ent->setNoGravity(*ng != 0);
  if (auto si = integerField(tag, "Silent"))
    ent->setSilent(*si != 0);
  if (auto inv = integerField(tag, "Invisible"))
    ent->setInvisible(*inv != 0);
  if (auto *proj = dynamic_cast<Projectile *>(ent.get())) {
    if (const nbt::Tag *ow = field(tag, "Owner")) {
      if (ow->type == 11 && ow->ints.size() == 4)
        proj->setOwnerUuid({ow->ints[0], ow->ints[1], ow->ints[2], ow->ints[3]});
    }
  }
  ent->syncDataFields();

  out.status = DecodeStatus::Ok;
  out.entity = ent;
  out.entities.push_back(ent);

  if (const nbt::Tag *passengers = field(tag, "Passengers")) {
    if (passengers->type != 9) {
      out.status = DecodeStatus::Corrupt;
      out.detail = "entity Passengers must be TAG_List";
      out.entity.reset();
      out.entities.clear();
      return out;
    }
    for (const nbt::Tag &pTag : passengers->list) {
      DecodedGeneralEntity child =
          decodeEntityRecursive(world, pTag, depth + 1);
      if (child.status != DecodeStatus::Ok || !child.entity) {
        out.status = child.status;
        out.detail = std::move(child.detail);
        out.entity.reset();
        out.entities.clear();
        return out;
      }
      child.entity->startRiding(*ent, true, false);
      for (auto &owned : child.entities)
        out.entities.push_back(std::move(owned));
    }
  }

  return out;
}

} // namespace

nbt::Tag encodeEntityNbt(const Entity &entity, std::string &error) {
  error.clear();
  return encodeEntityRecursive(entity, error, 0);
}

DecodedGeneralEntity decodeEntityNbt(World &world, const nbt::Tag &tag) {
  return decodeEntityRecursive(world, tag, 0);
}

DecodedGeneralEntityChunk
decodeGeneralEntityChunk(World &world, std::span<const std::uint8_t> bytes) {
  DecodedGeneralEntityChunk result;
  try {
    result.root = nbt::parseRoot(bytes);
  } catch (const std::exception &error) {
    result.status = DecodeStatus::Corrupt;
    result.detail = error.what();
    return result;
  }
  const auto version = integerField(result.root.value, "DataVersion");
  const nbt::Tag *position = field(result.root.value, "Position");
  const nbt::Tag *entities = field(result.root.value, "Entities");
  if (!version || *version != kDataVersion) {
    result.status = DecodeStatus::Unsupported;
    result.detail = "entity DataVersion is not 4786";
    return result;
  }
  if (!position || position->type != 11 || position->ints.size() != 2 ||
      !entities || entities->type != 9) {
    result.status = DecodeStatus::Corrupt;
    result.detail = "invalid entity chunk root Position or Entities";
    return result;
  }
  result.chunkX = position->ints[0];
  result.chunkZ = position->ints[1];
  for (const nbt::Tag &entry : entities->list) {
    DecodedGeneralEntity decoded = decodeEntityNbt(world, entry);
    if (decoded.status != DecodeStatus::Ok || !decoded.entity) {
      result.status = decoded.status;
      result.detail = std::move(decoded.detail);
      result.roots.clear();
      result.entities.clear();
      return result;
    }
    result.roots.push_back(std::move(decoded.entity));
    for (auto &owned : decoded.entities)
      result.entities.push_back(std::move(owned));
  }
  result.status = DecodeStatus::Ok;
  return result;
}

std::vector<std::uint8_t> encodeGeneralEntityChunk(
    std::int32_t chunkX, std::int32_t chunkZ,
    std::span<const Entity *const> rootEntities, std::string &error) {
  error.clear();
  nbt::RootTag root = emptyEntityRoot(chunkX, chunkZ);
  nbt::Tag &list = root.value.compound.at("Entities");
  std::set<std::array<std::int32_t, 4>> seenUuids;
  for (const Entity *e : rootEntities) {
    if (!e || e->isRemoved() || e->isPlayer() || e->isPassenger())
      continue;
    if (e->uuid == std::array<std::int32_t, 4>{} ||
        !seenUuids.insert(e->uuid).second) {
      error = "duplicate or zero entity UUID";
      return {};
    }
    nbt::Tag encoded = encodeEntityNbt(*e, error);
    if (encoded.type == 0)
      return {};
    list.list.push_back(std::move(encoded));
  }
  return nbt::writeRoot(root);
}

void addEntityTreeToWorld(World &world, const std::shared_ptr<Entity> &root) {
  if (!root || root->isRemoved())
    return;
  world.addEntity(root);
  for (const auto &passenger : root->getPassengers()) {
    if (passenger)
      addEntityTreeToWorld(world, passenger);
  }
}

} // namespace redstone::entity_codec
