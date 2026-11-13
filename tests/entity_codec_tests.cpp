#include "redstone/entity_codec.hpp"
#include "redstone/region.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace redstone;
using namespace redstone::entity_codec;

namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
void add(nbt::Tag &compound, std::string name, nbt::Tag value) {
  compound.compoundOrder.push_back(name);
  compound.compound.emplace(std::move(name), std::move(value));
}
nbt::Tag number(std::uint8_t type, std::int64_t value) {
  nbt::Tag tag;
  tag.type = type;
  tag.number = value;
  return tag;
}
nbt::Tag real(std::uint8_t type, double value) {
  nbt::Tag tag;
  tag.type = type;
  tag.real = value;
  return tag;
}
nbt::Tag string(std::string value) {
  nbt::Tag tag;
  tag.type = 8;
  tag.text = std::move(value);
  return tag;
}
nbt::Tag list(std::uint8_t type, std::vector<nbt::Tag> values) {
  nbt::Tag tag;
  tag.type = 9;
  tag.listElementType = type;
  tag.list = std::move(values);
  return tag;
}
nbt::Tag realList(std::uint8_t type, std::initializer_list<double> values) {
  nbt::Tag tag;
  tag.type = 9;
  tag.listElementType = type;
  for (double value : values)
    tag.list.push_back(real(type, value));
  return tag;
}

template <class T>
std::vector<T> project(const DecodedEntityChunk &decoded) {
  std::vector<T> result;
  for (const EntityRecord &record : decoded.records)
    if (const T *value = std::get_if<T>(&record.payload))
      result.push_back(*value);
  return result;
}
nbt::RootTag fixture() {
  nbt::RootTag root;
  root.value.type = 10;
  add(root.value, "DataVersion", number(3, 4786));
  nbt::Tag position;
  position.type = 11;
  position.ints = {-1, -1};
  add(root.value, "Position", std::move(position));
  nbt::Tag entity;
  entity.type = 10;
  add(entity, "id", string("minecraft:item"));
  nbt::Tag uuid;
  uuid.type = 11;
  uuid.ints = {1000665188, 1491226247, -1939110731, 1144620115};
  add(entity, "UUID", std::move(uuid));
  add(entity, "Pos", realList(6, {-13.25, -58.2, -13.75}));
  add(entity, "Motion", realList(6, {0.125, 0.0, -0.125}));
  add(entity, "Rotation", realList(5, {0.0, 0.0}));
  add(entity, "Health", number(2, 5));
  add(entity, "Age", number(2, 17));
  add(entity, "PickupDelay", number(2, 200));
  nbt::Tag item;
  item.type = 10;
  add(item, "id", string("minecraft:stone"));
  add(item, "count", number(3, 3));
  add(entity, "Item", std::move(item));
  add(entity, "OnGround", number(1, 0));
  add(entity, "Fire", number(2, 0));
  add(entity, "fall_distance", real(6, 0.0));
  add(entity, "Air", number(2, 300));
  add(entity, "PortalCooldown", number(3, 0));
  add(entity, "Invulnerable", number(1, 0));
  nbt::Tag entities;
  entities.type = 9;
  entities.listElementType = 10;
  entities.list.push_back(std::move(entity));
  add(root.value, "Entities", std::move(entities));
  return root;
}
TntEntityData tntFixture() {
  TntEntityData tnt;
  tnt.uuid = {31, 32, 33, 34};
  tnt.pos = {-13.75, -58.0, -13.25};
  tnt.motion = {0.0, 0.0, 0.0625};
  tnt.rotation = {0.0f, 0.0f};
  tnt.fuse = 47;
  tnt.explosionPower = 6.5f;
  return tnt;
}

MinecartEntityData minecartFixture() {
  MinecartEntityData entity;
  entity.uuid = {71, 72, 73, 74};
  entity.pos = {-13.25, -58.0, -13.75};
  entity.motion = {0.125, 0.02, 0.0625};
  entity.rotation = {90.0f, -10.0f};
  entity.flipped = true;
  entity.firstTick = false;
  return entity;
}

FallingBlockEntityData fallingFixture() {
  FallingBlockEntityData entity;
  entity.uuid = {51, 52, 53, 54};
  entity.pos = {-13.25, -58.0, -13.75};
  entity.motion = {0.0, -0.04, 0.0};
  entity.rotation = {0.0f, 0.0f};
  entity.time = 23;
  entity.dropItem = false;
  entity.cancelDrop = true;
  return entity;
}

ItemEntityData itemFixture(std::array<std::int32_t, 4> uuid, double x) {
  ItemEntityData item;
  item.uuid = uuid;
  item.pos = {x, -58.0, -13.5};
  item.motion = {0.125, 0.0, 0.0};
  item.rotation = {0.0f, 0.0f};
  item.age = 17;
  item.pickupDelay = 200;
  item.health = 5;
  item.itemId = "minecraft:stone";
  item.itemCount = 3;
  return item;
}

struct MixedOrderFixture {
  nbt::RootTag root;
  std::array<std::int32_t, 4> itemUuid{};
  std::array<std::int32_t, 4> tntUuid{};
  std::array<std::int32_t, 4> secondItemUuid{};
};

MixedOrderFixture mixedOrderFixture() {
  ItemEntityData first = itemFixture({1, 2, 3, 4}, -13.75);
  TntEntityData tnt = tntFixture();
  ItemEntityData second = itemFixture({41, 42, 43, 44}, -12.75);
  const std::array records{EntityRecord{first}, EntityRecord{tnt},
                           EntityRecord{second}};
  MixedOrderFixture fixture;
  fixture.root = entity_codec::emptyEntityRoot(-1, -1);
  fixture.itemUuid = first.uuid;
  fixture.tntUuid = tnt.uuid;
  fixture.secondItemUuid = second.uuid;
  std::string error;
  require(patchEntitySequence(fixture.root, records, error), error);
  return fixture;
}

void testStockStyleTntRecord() {
  // Direct tag form mirrors PrimedTnt.addAdditionalSaveData + Entity base
  // fields. It validates the codec without needing a live server restart.
  nbt::Tag entity;
  entity.type = 10;
  add(entity, "id", string("minecraft:tnt"));
  nbt::Tag uuid;
  uuid.type = 11;
  uuid.ints = {31, 32, 33, 34};
  add(entity, "UUID", std::move(uuid));
  add(entity, "Pos", realList(6, {-13.75, -58.0, -13.25}));
  add(entity, "Motion", realList(6, {0.0, 0.0, 0.0625}));
  add(entity, "Rotation", realList(5, {0.0, 0.0}));
  add(entity, "OnGround", number(1, 0));
  add(entity, "Fire", number(2, 0));
  add(entity, "fall_distance", real(6, 0.0));
  add(entity, "Air", number(2, 300));
  add(entity, "PortalCooldown", number(3, 0));
  add(entity, "Invulnerable", number(1, 0));
  add(entity, "fuse", number(2, 47));
  nbt::Tag state;
  state.type = 10;
  add(state, "Name", string("minecraft:tnt"));
  nbt::Tag properties;
  properties.type = 10;
  add(properties, "unstable", string("false"));
  add(state, "Properties", std::move(properties));
  add(entity, "block_state", std::move(state));
  add(entity, "explosion_power", real(5, 6.5));
  std::string error;
  const auto decoded =
      decodeTntRecord(entity, -1, -1, error);
  require(decoded && error.empty() && decoded->fuse == 47 &&
              decoded->explosionPower == 6.5f &&
              decoded->uuid == std::array<std::int32_t, 4>{31, 32, 33, 34},
          "stock-shape tnt fields decode");
  nbt::Tag mixed = entity;
  add(mixed, "Passengers", list(10, {}));
  require(!decodeTntRecord(mixed, -1, -1, error),
          "unsupported tnt relation is rejected as a whole");
  nbt::Tag unstable = entity;
  unstable.compound["block_state"].compound["Properties"]
      .compound["unstable"].text = "true";
  require(!decodeTntRecord(unstable, -1, -1, error),
          "nondefault tnt block state is rejected");
  std::cout << "entity_codec tnt_stock_fields=1 properties_default=1 "
               "unsupported_relation=1"
            << std::endl;
}

void testTntRoundTrip() {
  const TntEntityData tnt = tntFixture();
  nbt::RootTag root = entity_codec::emptyEntityRoot(-1, -1);
  std::string error;
  require(patchEntityItems(root, {}, {&tnt, 1}, error), error);
  DecodedEntityChunk decoded = decodeEntityChunk(encodeEntityChunk(root));
  const auto tnts = project<TntEntityData>(decoded);
  require(decoded.status == DecodeStatus::Ok && decoded.records.size() == 1 &&
              tnts.size() == 1,
          decoded.detail);
  const auto &roundTrip = tnts[0];
  require(roundTrip.uuid == tnt.uuid && roundTrip.pos == tnt.pos &&
              roundTrip.motion == tnt.motion &&
              roundTrip.fuse == tnt.fuse &&
              roundTrip.explosionPower == tnt.explosionPower,
          "primed tnt fields round-trip");
  const auto &savedState =
      root.value.compound["Entities"].list[0].compound["block_state"];
  require(savedState.compound.at("Name").text == "minecraft:tnt" &&
              savedState.compound.at("Properties")
                      .compound.at("unstable").text == "false",
          "codec writes vanilla default TNT block state properties");

  nbt::RootTag unknown = root;
  add(unknown.value.compound["Entities"].list[0], "Owner", number(11, 0));
  require(decodeEntityChunk(encodeEntityChunk(unknown)).status ==
              DecodeStatus::Unsupported,
          "tnt owner is explicitly unsupported");
  nbt::RootTag custom = root;
  nbt::Tag state;
  state.type = 10;
  add(state, "Name", string("minecraft:stone"));
  custom.value.compound["Entities"].list[0].compound["block_state"] =
      std::move(state);
  require(decodeEntityChunk(encodeEntityChunk(custom)).status ==
              DecodeStatus::Unsupported,
          "custom tnt block state is rejected atomically");
  std::cout << "entity_codec tnt_roundtrip=1 owner=unsupported "
               "custom_state=unsupported" << std::endl;
}

void testMixedEntitySequenceOrder() {
  MixedOrderFixture fixture = mixedOrderFixture();
  DecodedEntityChunk decoded =
      decodeEntityChunk(encodeEntityChunk(fixture.root));
  require(decoded.status == DecodeStatus::Ok && decoded.records.size() == 3,
          decoded.detail);
  const auto *first =
      std::get_if<ItemEntityData>(&decoded.records[0].payload);
  const auto *tnt =
      std::get_if<TntEntityData>(&decoded.records[1].payload);
  const auto *second =
      std::get_if<ItemEntityData>(&decoded.records[2].payload);
  require(first && first->uuid == fixture.itemUuid && tnt &&
              tnt->uuid == fixture.tntUuid && second &&
              second->uuid == fixture.secondItemUuid,
          "mixed item/tnt sequence survives decode and encode");
  std::cout << "entity_codec mixed_order=1" << std::endl;
}

void testFallingBlockRoundTrip() {
  FallingBlockEntityData falling = fallingFixture();
  nbt::RootTag root = entity_codec::emptyEntityRoot(-1, -1);
  std::string error;
  const std::array records{EntityRecord{falling}};
  require(patchEntitySequence(root, records, error), error);
  DecodedEntityChunk decoded = decodeEntityChunk(encodeEntityChunk(root));
  const auto values = project<FallingBlockEntityData>(decoded);
  require(decoded.status == DecodeStatus::Ok && values.size() == 1,
          decoded.detail);
  const auto &value = values[0];
  require(value.uuid == falling.uuid && value.pos == falling.pos &&
              value.motion == falling.motion && value.time == 23 &&
              !value.dropItem && value.cancelDrop && !value.hurtEntities &&
              value.blockId == "minecraft:sand",
          "supported falling block fields round-trip");
  const auto &saved = root.value.compound["Entities"].list[0];
  require(saved.compound.at("BlockState").compound.at("Name").text ==
                  "minecraft:sand" &&
              saved.compound.at("BlockState").compound.size() == 1 &&
              saved.compound.at("Time").number == 23 &&
              saved.compound.at("DropItem").number == 0 &&
              saved.compound.at("CancelDrop").number == 1 &&
              saved.compound.at("HurtEntities").number == 0 &&
              saved.compound.at("FallHurtMax").number == 40,
          "falling block writes the vanilla sand state shape");

  nbt::RootTag tile = root;
  nbt::Tag tileData;
  tileData.type = 10;
  add(tile.value.compound["Entities"].list[0], "TileEntityData",
      std::move(tileData));
  require(decodeEntityChunk(encodeEntityChunk(tile)).status ==
              DecodeStatus::Unsupported,
          "falling block TileEntityData is rejected atomically");
  nbt::RootTag unsupported = root;
  unsupported.value.compound["Entities"].list[0]
      .compound["BlockState"].compound["Name"].text = "minecraft:gravel";
  require(decodeEntityChunk(encodeEntityChunk(unsupported)).status ==
              DecodeStatus::Unsupported,
          "unsupported falling block state is rejected atomically");
  nbt::RootTag hurt = root;
  hurt.value.compound["Entities"].list[0]
      .compound["HurtEntities"].number = 1;
  require(decodeEntityChunk(encodeEntityChunk(hurt)).status ==
              DecodeStatus::Unsupported,
          "falling block damage state is rejected");
  std::cout << "entity_codec falling_stock=1 tile_data=unsupported "
               "unsupported_state=rejected damage=rejected\n";
}

void testMixedEntitySequenceWithFalling() {
  ItemEntityData first = itemFixture({1, 2, 3, 4}, -13.75);
  FallingBlockEntityData falling = fallingFixture();
  TntEntityData tnt = tntFixture();
  ItemEntityData last = itemFixture({61, 62, 63, 64}, -12.75);
  nbt::RootTag root = entity_codec::emptyEntityRoot(-1, -1);
  const std::array records{EntityRecord{first}, EntityRecord{falling},
                           EntityRecord{tnt}, EntityRecord{last}};
  std::string error;
  require(patchEntitySequence(root, records, error), error);
  DecodedEntityChunk decoded = decodeEntityChunk(encodeEntityChunk(root));
  require(decoded.status == DecodeStatus::Ok && decoded.records.size() == 4,
          decoded.detail);
  const auto *fallingValue =
      std::get_if<FallingBlockEntityData>(&decoded.records[1].payload);
  require(std::holds_alternative<ItemEntityData>(decoded.records[0].payload) &&
              fallingValue && fallingValue->uuid == falling.uuid &&
              std::holds_alternative<TntEntityData>(decoded.records[2].payload) &&
              std::holds_alternative<ItemEntityData>(decoded.records[3].payload),
          "item/falling/tnt/item order survives codec");
  std::cout << "entity_codec mixed_falling_order=1" << std::endl;
}

void testMinecartRoundTrip() {
  MinecartEntityData minecart = minecartFixture();
  nbt::RootTag root = entity_codec::emptyEntityRoot(-1, -1);
  std::string error;
  const std::array records{EntityRecord{minecart}};
  require(patchEntitySequence(root, records, error), error);
  DecodedEntityChunk decoded = decodeEntityChunk(encodeEntityChunk(root));
  const auto values = project<MinecartEntityData>(decoded);
  require(decoded.status == DecodeStatus::Ok && values.size() == 1,
          decoded.detail);
  const auto &value = values[0];
  require(value.uuid == minecart.uuid && value.pos == minecart.pos &&
              value.motion == minecart.motion &&
              value.rotation == minecart.rotation && value.flipped &&
              !value.firstTick,
          "ordinary empty minecart fields round-trip");
  const auto &saved = root.value.compound["Entities"].list[0];
  require(saved.compound.at("HasTicked").number == 0 &&
              saved.compound.at("FlippedRotation").number == 1 &&
              !saved.compound.contains("DisplayState") &&
              !saved.compound.contains("DisplayOffset"),
          "minecart writes vanilla empty-cart fields");
  nbt::RootTag display = root;
  nbt::Tag displayState;
  displayState.type = 10;
  add(display.value.compound["Entities"].list[0], "DisplayState",
      std::move(displayState));
  require(decodeEntityChunk(encodeEntityChunk(display)).status ==
              DecodeStatus::Unsupported,
          "custom minecart display state is rejected atomically");
  nbt::RootTag offset = root;
  offset.value.compound["Entities"].list[0]
      .compound["DisplayOffset"] = number(3, 7);
  require(decodeEntityChunk(encodeEntityChunk(offset)).status ==
              DecodeStatus::Unsupported,
          "nondefault minecart display offset is rejected atomically");
  nbt::RootTag passengers = root;
  add(passengers.value.compound["Entities"].list[0], "Passengers", list(10, {}));
  require(decodeEntityChunk(encodeEntityChunk(passengers)).status ==
              DecodeStatus::Unsupported,
          "minecart passengers are rejected atomically");
  std::cout << "entity_codec minecart_roundtrip=1 display=unsupported "
               "offset=unsupported passengers=unsupported"
            << std::endl;
}

void testMinecartSequenceOrder() {
  ItemEntityData item = itemFixture({81, 82, 83, 84}, -13.75);
  MinecartEntityData minecart = minecartFixture();
  TntEntityData tnt = tntFixture();
  FallingBlockEntityData falling = fallingFixture();
  nbt::RootTag root = entity_codec::emptyEntityRoot(-1, -1);
  const std::array records{EntityRecord{item}, EntityRecord{minecart},
                           EntityRecord{tnt}, EntityRecord{falling}};
  std::string error;
  require(patchEntitySequence(root, records, error), error);
  DecodedEntityChunk decoded = decodeEntityChunk(encodeEntityChunk(root));
  require(decoded.status == DecodeStatus::Ok && decoded.records.size() == 4 &&
              std::holds_alternative<ItemEntityData>(
                  decoded.records[0].payload) &&
              std::holds_alternative<MinecartEntityData>(
                  decoded.records[1].payload) &&
              std::holds_alternative<TntEntityData>(
                  decoded.records[2].payload) &&
              std::holds_alternative<FallingBlockEntityData>(
                  decoded.records[3].payload),
          "item/minecart/tnt/falling sequence survives codec");
  std::cout << "entity_codec mixed_minecart_order=1" << std::endl;
}

void testRoundTrip() {
  nbt::RootTag root = fixture();
  const auto bytes = encodeEntityChunk(root);
  DecodedEntityChunk decoded = decodeEntityChunk(bytes);
  const auto items = project<ItemEntityData>(decoded);
  require(decoded.status == DecodeStatus::Ok && items.size() == 1,
          decoded.detail);
  const auto &item = items[0];
  require(item.uuid == std::array<std::int32_t, 4>{
                            1000665188, 1491226247, -1939110731, 1144620115} &&
              item.pos == std::array<double, 3>{-13.25, -58.2, -13.75} &&
              item.motion == std::array<double, 3>{0.125, 0.0, -0.125} &&
              item.age == 17 && item.pickupDelay == 200 &&
              item.health == 5 && item.itemId == "minecraft:stone" &&
              item.itemCount == 3,
          "official item fields round-trip");
  std::string error;
  require(patchEntityItems(root, {}, {}, error) && error.empty(),
          "empty entity snapshot patches");
  require(decodeEntityChunk(encodeEntityChunk(root)).records.empty(),
          "empty entity snapshot persists as an empty list");
}
void testRejections() {
  nbt::RootTag unknown = fixture();
  add(unknown.value.compound["Entities"].list[0], "Owner", number(11, 0));
  require(decodeEntityChunk(encodeEntityChunk(unknown)).status ==
              DecodeStatus::Unsupported,
          "unknown entity fields are rejected");
  nbt::RootTag nondefault = fixture();
  nondefault.value.compound["Entities"].list[0].compound["Air"].number = 299;
  require(decodeEntityChunk(encodeEntityChunk(nondefault)).status ==
              DecodeStatus::Unsupported,
          "nondefault unsupported Air is rejected");
  nbt::RootTag components = fixture();
  nbt::Tag values;
  values.type = 10;
  add(values, "minecraft:custom", string("x"));
  add(components.value.compound["Entities"].list[0].compound["Item"],
      "components", std::move(values));
  require(decodeEntityChunk(encodeEntityChunk(components)).status ==
              DecodeStatus::Unsupported,
          "item components are rejected");
  nbt::RootTag duplicate = fixture();
  nbt::Tag copy = duplicate.value.compound["Entities"].list[0];
  duplicate.value.compound["Entities"].list.push_back(std::move(copy));
  require(decodeEntityChunk(encodeEntityChunk(duplicate)).status ==
              DecodeStatus::Unsupported,
          "duplicate persisted UUIDs are rejected");
  nbt::RootTag wrong = fixture();
  wrong.value.compound["Entities"].list[0].compound["Pos"].list[0].real = 0;
  require(decodeEntityChunk(encodeEntityChunk(wrong)).status ==
              DecodeStatus::Unsupported,
          "entity positions outside the entity chunk are rejected");

  nbt::RootTag hugeMotion = fixture();
  hugeMotion.value.compound["Entities"]
      .list[0].compound["Motion"].list[0].real = 1.0e300;
  const auto motionDecoded = decodeEntityChunk(encodeEntityChunk(hugeMotion));
  require(motionDecoded.status == DecodeStatus::Ok &&
              project<ItemEntityData>(motionDecoded)[0].motion[0] == 0.0,
          "oversized finite motion is zeroed like Entity.load");

  nbt::RootTag hugePosition = fixture();
  hugePosition.value.compound["Entities"]
      .list[0].compound["Pos"].list[0].real = 1.0e300;
  require(decodeEntityChunk(encodeEntityChunk(hugePosition)).status ==
              DecodeStatus::Unsupported,
          "extreme finite position is clamped before out-of-chunk rejection");

  nbt::RootTag nanPosition = fixture();
  nanPosition.value.compound["Entities"]
      .list[0].compound["Pos"].list[0].real =
      std::numeric_limits<double>::quiet_NaN();
  require(decodeEntityChunk(encodeEntityChunk(nanPosition)).status ==
              DecodeStatus::Unsupported,
          "NaN positions are rejected");

  ItemEntityData patchHuge;
  patchHuge.uuid = {20, 21, 22, 23};
  patchHuge.pos = {1.0e300, 0.0, -1.0};
  patchHuge.itemId = "minecraft:stone";
  patchHuge.itemCount = 1;
  nbt::RootTag patchRoot = fixture();
  std::string patchError;
  require(!patchEntityItems(patchRoot, {&patchHuge, 1}, {}, patchError) &&
              patchError.find("chunk") != std::string::npos,
          "save rejects extreme position before integer conversion");
  std::cout << "entity_codec motion_clamped=1 huge_position=rejected "
               "nan=rejected\n";
}
void testOfficial(const std::filesystem::path &regionDirectory) {
  const auto raw = region::readChunkNbt(regionDirectory, -1, -1);
  require(raw.status == region::ReadStatus::Found, raw.detail);
  DecodedEntityChunk decoded = decodeEntityChunk(raw.nbt);
  const auto items = project<ItemEntityData>(decoded);
  require(decoded.status == DecodeStatus::Ok && decoded.chunkX == -1 &&
              decoded.chunkZ == -1 && items.size() == 1 &&
              items[0].age == 17 && items[0].pickupDelay == 200 &&
              items[0].itemCount == 3,
          "official 26.1 item fixture decodes");
  std::cout << "official_item age=" << items[0].age
            << " pickup=" << items[0].pickupDelay << "\n";
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--decode-tnt") {
      std::ifstream input(argv[2], std::ios::binary);
      require(input.good(), "open official-generated TNT NBT");
      const std::vector<std::uint8_t> bytes{
          std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
      DecodedEntityChunk decoded = decodeEntityChunk(bytes);
      require(decoded.status == DecodeStatus::Ok &&
                  decoded.records.size() == 3 &&
                  std::holds_alternative<ItemEntityData>(
                      decoded.records[0].payload) &&
                  std::holds_alternative<TntEntityData>(
                      decoded.records[1].payload) &&
                  std::get<TntEntityData>(decoded.records[1].payload).uuid ==
                      std::array<std::int32_t, 4>{31, 32, 33, 34} &&
                  std::get<TntEntityData>(decoded.records[1].payload).fuse ==
                      47 &&
                  std::get<TntEntityData>(decoded.records[1].payload).motion ==
                      std::array<double, 3>{0.0, 0.0, 0.0625} &&
                  std::holds_alternative<ItemEntityData>(
                      decoded.records[2].payload),
              decoded.detail);
      std::cout << "entity_codec official_codec_input=1 order=item,tnt,item"
                << std::endl;
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--decode-falling") {
      std::ifstream input(argv[2], std::ios::binary);
      require(input.good(), "open official-generated falling block NBT");
      const std::vector<std::uint8_t> bytes{
          std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
      DecodedEntityChunk decoded = decodeEntityChunk(bytes);
      require(decoded.status == DecodeStatus::Ok &&
                  decoded.records.size() == 4 &&
                  std::holds_alternative<ItemEntityData>(
                      decoded.records[0].payload) &&
                  std::holds_alternative<FallingBlockEntityData>(
                      decoded.records[1].payload) &&
                  std::get<FallingBlockEntityData>(
                      decoded.records[1].payload).uuid ==
                      std::array<std::int32_t, 4>{51, 52, 53, 54} &&
                  std::holds_alternative<TntEntityData>(
                      decoded.records[2].payload) &&
                  std::holds_alternative<ItemEntityData>(
                      decoded.records[3].payload),
              decoded.detail);
      std::cout << "entity_codec official_falling_input=1"
                   " order=item,falling,tnt,item" << std::endl;
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--decode-minecart") {
      std::ifstream input(argv[2], std::ios::binary);
      require(input.good(), "open official-generated minecart NBT");
      const std::vector<std::uint8_t> bytes{
          std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
      DecodedEntityChunk decoded = decodeEntityChunk(bytes);
      require(decoded.status == DecodeStatus::Ok &&
                  decoded.records.size() == 4 &&
                  std::holds_alternative<ItemEntityData>(
                      decoded.records[0].payload) &&
                  std::holds_alternative<MinecartEntityData>(
                      decoded.records[1].payload) &&
                  std::get<MinecartEntityData>(
                      decoded.records[1].payload).uuid ==
                      std::array<std::int32_t, 4>{71, 72, 73, 74} &&
                  std::holds_alternative<TntEntityData>(
                      decoded.records[2].payload) &&
                  std::holds_alternative<FallingBlockEntityData>(
                      decoded.records[3].payload),
              decoded.detail);
      std::cout << "entity_codec official_minecart_input=1"
                   " order=item,minecart,tnt,falling" << std::endl;
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--emit-minecart") {
      ItemEntityData item = itemFixture({81, 82, 83, 84}, -13.75);
      MinecartEntityData minecart = minecartFixture();
      TntEntityData tnt = tntFixture();
      FallingBlockEntityData falling = fallingFixture();
      nbt::RootTag root = entity_codec::emptyEntityRoot(-1, -1);
      const std::array records{EntityRecord{item}, EntityRecord{minecart},
                               EntityRecord{tnt}, EntityRecord{falling}};
      std::string error;
      require(patchEntitySequence(root, records, error), error);
      const auto bytes = encodeEntityChunk(root);
      std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
      require(output.good(), "write C++ minecart codec fixture");
      std::cout << "entity_codec emitted mixed minecart NBT bytes="
                << bytes.size() << std::endl;
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--emit-falling") {
      ItemEntityData first = itemFixture({1, 2, 3, 4}, -13.75);
      FallingBlockEntityData falling = fallingFixture();
      TntEntityData tnt = tntFixture();
      ItemEntityData last = itemFixture({61, 62, 63, 64}, -12.75);
      nbt::RootTag root = entity_codec::emptyEntityRoot(-1, -1);
      const std::array records{EntityRecord{first}, EntityRecord{falling},
                               EntityRecord{tnt}, EntityRecord{last}};
      std::string error;
      require(patchEntitySequence(root, records, error), error);
      const auto bytes = encodeEntityChunk(root);
      std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
      require(output.good(), "write C++ falling block codec fixture");
      std::cout << "entity_codec emitted mixed falling NBT bytes="
                << bytes.size() << std::endl;
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--emit-tnt") {
      MixedOrderFixture fixture = mixedOrderFixture();
      const auto bytes = encodeEntityChunk(fixture.root);
      std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
      output.write(reinterpret_cast<const char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
      require(output.good(), "write C++ TNT codec fixture");
      std::cout << "entity_codec emitted mixed item/tnt NBT bytes="
                << bytes.size() << std::endl;
      return 0;
    }
    testRoundTrip();
    if (argc == 1) {
      testStockStyleTntRecord();
      testTntRoundTrip();
      testMixedEntitySequenceOrder();
      testFallingBlockRoundTrip();
      testMixedEntitySequenceWithFalling();
      testMinecartRoundTrip();
      testMinecartSequenceOrder();
    }
    testRejections();
    if (argc == 2)
      testOfficial(argv[1]);
    std::cout << "entity codec tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "entity codec test failure: " << error.what() << '\n';
    return 1;
  }
}
