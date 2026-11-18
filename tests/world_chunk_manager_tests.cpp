#include "redstone/entity.hpp"
#include "redstone/piston.hpp"
#include "redstone/storage.hpp"
#include "redstone/world_chunk_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace redstone;
using namespace redstone::terrain;

namespace {

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::vector<entity_codec::ItemEntityData>
itemsOf(const entity_codec::DecodedEntityChunk &decoded) {
  std::vector<entity_codec::ItemEntityData> result;
  for (const auto &record : decoded.records)
    if (const auto *item =
            std::get_if<entity_codec::ItemEntityData>(&record.payload))
      result.push_back(*item);
  return result;
}
std::vector<entity_codec::TntEntityData>
tntsOf(const entity_codec::DecodedEntityChunk &decoded) {
  std::vector<entity_codec::TntEntityData> result;
  for (const auto &record : decoded.records)
    if (const auto *tnt =
            std::get_if<entity_codec::TntEntityData>(&record.payload))
      result.push_back(*tnt);
  return result;
}

void add(nbt::Tag &compound, std::string key, nbt::Tag value) {
  compound.compound.emplace(key, value);
  compound.compoundOrder.push_back(std::move(key));
}

nbt::Tag number(std::uint8_t type, std::int64_t value) {
  nbt::Tag tag;
  tag.type = type;
  tag.number = value;
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

std::vector<std::int64_t> packedCells(
    const std::vector<std::uint16_t> &cells, unsigned bits) {
  const std::size_t perLong = 64 / bits;
  std::vector<std::int64_t> result((cells.size() + perLong - 1) / perLong, 0);
  const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    auto &word = result[i / perLong];
    const std::size_t offset = (i % perLong) * bits;
    word = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(word) & ~(mask << offset)) |
        (static_cast<std::uint64_t>(cells[i]) << offset));
  }
  return result;
}

std::vector<std::uint8_t>
makeMultiPaletteChunkNbt(const ChunkTicketPos &position) {
  nbt::RootTag root;
  root.name = "Level";
  root.value.type = 10;
  add(root.value, "DataVersion", number(3, 4786));
  add(root.value, "xPos", number(3, position.x));
  add(root.value, "zPos", number(3, position.z));
  add(root.value, "Status", string("minecraft:full"));

  nbt::Tag stone;
  stone.type = 10;
  add(stone, "Name", string("minecraft:stone"));
  nbt::Tag glass;
  glass.type = 10;
  add(glass, "Name", string("minecraft:glass"));
  nbt::Tag states;
  states.type = 10;
  add(states, "palette",
      list(10, {std::move(stone), std::move(glass)}));
  std::vector<std::uint16_t> cells(4096, 0);
  cells[0] = 1;
  cells[17] = 1;
  nbt::Tag data;
  data.type = 12;
  data.longs = packedCells(cells, 4);
  add(states, "data", std::move(data));
  nbt::Tag section;
  section.type = 10;
  add(section, "Y", number(1, 0));
  add(section, "block_states", std::move(states));
  add(root.value, "sections", list(10, {std::move(section)}));
  return nbt::writeRoot(root);
}

enum class DynamicKind { None, BlockEntity, Entity, BlockTick };

std::vector<std::uint8_t>
makeChunkNbt(const ChunkTicketPos &position,
             DynamicKind dynamic = DynamicKind::None, int sectionY = 0);

nbt::Tag savedTickTag(std::string id, Pos pos, int delay, int priority) {
  nbt::Tag tick;
  tick.type = 10;
  add(tick, "i", string(std::move(id)));
  add(tick, "x", number(3, pos.x));
  add(tick, "y", number(3, pos.y));
  add(tick, "z", number(3, pos.z));
  add(tick, "t", number(3, delay));
  add(tick, "p", number(3, priority));
  return tick;
}

std::vector<std::uint8_t> makeSavedTickChunkNbt(
    const ChunkTicketPos &position) {
  nbt::RootTag root = nbt::parseRoot(makeChunkNbt(position));
  const int x = position.x * 16;
  const int z = position.z * 16;
  add(root.value, "block_ticks",
      list(10, {
                savedTickTag("minecraft:stone", {x, 0, z}, 5, 1),
                savedTickTag("minecraft:stone", {x, 0, z}, 5, 1),
                savedTickTag("minecraft:stone", {x + 1, 0, z}, 5, 1),
            }));
  add(root.value, "fluid_ticks",
      list(10, {
                savedTickTag("minecraft:water", {x + 2, 0, z}, 7, -1),
            }));
  return nbt::writeRoot(root);
}

std::vector<std::uint8_t>
makeChunkNbt(const ChunkTicketPos &position, DynamicKind dynamic,
             int sectionY) {
  nbt::RootTag root;
  root.name = "Level";
  root.value.type = 10;
  add(root.value, "DataVersion", number(3, 4786));
  add(root.value, "xPos", number(3, position.x));
  add(root.value, "zPos", number(3, position.z));
  add(root.value, "Status", string("minecraft:full"));
  add(root.value, "isLightOn", number(1, 1));

  nbt::Tag stone;
  stone.type = 10;
  add(stone, "Name", string("minecraft:stone"));
  nbt::Tag states;
  states.type = 10;
  add(states, "palette", list(10, {std::move(stone)}));
  nbt::Tag section;
  section.type = 10;
  add(section, "Y", number(1, sectionY));
  add(section, "block_states", std::move(states));
  add(root.value, "sections", list(10, {std::move(section)}));

  nbt::Tag unknown;
  unknown.type = 10;
  add(unknown, "preserved", number(4, 0x123456789abcdefLL));
  add(root.value, "world_cycle_extension", std::move(unknown));

  if (dynamic == DynamicKind::BlockEntity) {
    nbt::Tag entity;
    entity.type = 10;
    add(entity, "id", string("minecraft:chest"));
    add(entity, "x", number(3, position.x * 16));
    add(root.value, "block_entities", list(10, {std::move(entity)}));
  } else if (dynamic == DynamicKind::Entity) {
    nbt::Tag entity;
    entity.type = 10;
    add(entity, "id", string("minecraft:item"));
    add(entity, "x", number(3, position.x * 16));
    add(root.value, "entities", list(10, {std::move(entity)}));
  } else if (dynamic == DynamicKind::BlockTick) {
    nbt::Tag tick;
    tick.type = 10;
    add(tick, "i", string("minecraft:stone"));
    add(tick, "x", number(3, position.x * 16));
    add(tick, "y", number(3, 0));
    add(tick, "z", number(3, position.z * 16));
    add(tick, "t", number(3, 1));
    add(tick, "p", number(3, 0));
    add(root.value, "block_ticks", list(10, {std::move(tick)}));
  }
  return nbt::writeRoot(root);
}

std::vector<std::uint8_t> makeFallingChunkNbt(const ChunkTicketPos &position) {
  nbt::RootTag root;
  root.name = "Level";
  root.value.type = 10;
  add(root.value, "DataVersion", number(3, 4786));
  add(root.value, "xPos", number(3, position.x));
  add(root.value, "zPos", number(3, position.z));
  add(root.value, "Status", string("minecraft:full"));
  add(root.value, "isLightOn", number(1, 1));
  nbt::Tag air;
  air.type = 10;
  add(air, "Name", string("minecraft:air"));
  nbt::Tag stone;
  stone.type = 10;
  add(stone, "Name", string("minecraft:stone"));
  nbt::Tag states;
  states.type = 10;
  add(states, "palette", list(10, {std::move(air), std::move(stone)}));
  std::vector<std::uint16_t> cells(4096, 0);
  for (std::size_t i = 0; i < 256; ++i)
    cells[i] = 1;
  nbt::Tag data;
  data.type = 12;
  data.longs = packedCells(cells, 4);
  add(states, "data", std::move(data));
  nbt::Tag section;
  section.type = 10;
  add(section, "Y", number(1, 0));
  add(section, "block_states", std::move(states));
  add(root.value, "sections", list(10, {std::move(section)}));
  return nbt::writeRoot(root);
}

EntityChunkDataResult knownEmptyEntityData(const ChunkTicketPos &) {
  return {EntityChunkDataResult::Status::Empty, {}};
}

struct FakeStore final : RawChunkStore {
  StoreReadResult readChunk(const ChunkTicketPos &position) override {
    std::unique_lock lock(mutex);
    ++readCalls;
    ++active;
    maxActive = std::max(maxActive, active);
    while (blockReads) {
      changed.notify_all();
      if (changed.wait_for(lock, std::chrono::seconds(5),
                           [this] { return !blockReads; }))
        break;
      --active;
      return {StoreReadStatus::Io, {}, "blocked read timed out"};
    }
    StoreReadResult result;
    auto found = payloads.find(position);
    if (found == payloads.end()) {
      result.status = StoreReadStatus::Missing;
      result.detail = "not found";
    } else {
      result.status = StoreReadStatus::Found;
      result.nbt = found->second;
    }
    --active;
    changed.notify_all();
    return result;
  }

  StoreWriteResult writeChunk(const ChunkTicketPos &position,
                              std::span<const std::uint8_t> bytes,
                              int) override {
    std::unique_lock lock(mutex);
    ++writeCalls;
    ++active;
    maxActive = std::max(maxActive, active);
    writePayloads.emplace_back(bytes.begin(), bytes.end());
    while (blockWrites) {
      changed.notify_all();
      if (changed.wait_for(lock, std::chrono::seconds(5),
                           [this] { return !blockWrites; }))
        break;
      --active;
      return {false, false, "blocked write timed out"};
    }
    StoreWriteResult result;
    if (failWrites != 0) {
      --failWrites;
      result.wrote = false;
      result.detail = "injected save failure";
    } else {
      payloads[position].assign(bytes.begin(), bytes.end());
      ++successfulWrites;
      result.wrote = true;
    }
    --active;
    changed.notify_all();
    return result;
  }

  void setPayload(const ChunkTicketPos &position,
                  std::vector<std::uint8_t> bytes) {
    std::lock_guard lock(mutex);
    payloads[position] = std::move(bytes);
  }

  void erasePayload(const ChunkTicketPos &position) {
    std::lock_guard lock(mutex);
    payloads.erase(position);
  }

  void setBlockReads(bool value) {
    std::lock_guard lock(mutex);
    blockReads = value;
    changed.notify_all();
  }

  void setBlockWrites(bool value) {
    std::lock_guard lock(mutex);
    blockWrites = value;
    changed.notify_all();
  }

  void failNextWrites(int count) {
    std::lock_guard lock(mutex);
    failWrites = count;
  }

  void releaseReads() {
    std::lock_guard lock(mutex);
    blockReads = false;
    changed.notify_all();
  }

  void releaseWrites() {
    std::lock_guard lock(mutex);
    blockWrites = false;
    changed.notify_all();
  }

  void waitForRead(int count) {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, std::chrono::seconds(5), [&] {
              return readCalls >= count;
            }),
            "timed out waiting for managed read");
  }

  void waitForWrite(int count) {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, std::chrono::seconds(5), [&] {
              return writeCalls >= count;
            }),
            "timed out waiting for managed write");
  }

  std::vector<std::vector<std::uint8_t>> writes() const {
    std::lock_guard lock(mutex);
    return writePayloads;
  }

  std::optional<std::vector<std::uint8_t>>
  latestPayload(const ChunkTicketPos &position) const {
    std::lock_guard lock(mutex);
    const auto found = payloads.find(position);
    if (found == payloads.end())
      return std::nullopt;
    return found->second;
  }

  int successes() const {
    std::lock_guard lock(mutex);
    return successfulWrites;
  }

  int writeCallCount() const {
    std::lock_guard lock(mutex);
    return writeCalls;
  }

  int maximumActive() const {
    std::lock_guard lock(mutex);
    return maxActive;
  }

  mutable std::mutex mutex;
  std::condition_variable changed;
  std::map<ChunkTicketPos, std::vector<std::uint8_t>> payloads;
  std::vector<std::vector<std::uint8_t>> writePayloads;
  bool blockReads = false;
  bool blockWrites = false;
  int failWrites = 0;
  int successfulWrites = 0;
  int readCalls = 0;
  int writeCalls = 0;
  int active = 0;
  int maxActive = 0;
};

class FakeEntityStore final : public RawEntityStore {
public:
  StoreReadResult readEntities(const ChunkTicketPos &position) override {
    std::lock_guard lock(mutex_);
    const auto found = payloads_.find(position);
    if (found == payloads_.end())
      return {StoreReadStatus::Missing, {}, "missing entity slot"};
    return {StoreReadStatus::Found, found->second, {}};
  }

  StoreWriteResult writeEntities(
      const ChunkTicketPos &position, std::span<const std::uint8_t> bytes,
      int) override {
    std::unique_lock lock(mutex_);
    while (blockWrites_) {
      changed_.notify_all();
      if (changed_.wait_for(lock, std::chrono::seconds(5),
                            [this] { return !blockWrites_; }))
        break;
      return {false, false, "blocked entity write timed out"};
    }
    ++writeCalls_;
    writePositions_.push_back(position);
    auto positionFailure = positionalFailWrites_.find(position);
    if (positionFailure != positionalFailWrites_.end() &&
        positionFailure->second != 0) {
      --positionFailure->second;
      ++activePositionalFailures_[position];
      return {false, false, "injected positional entity save failure"};
    }
    if (failWrites_ != 0) {
      --failWrites_;
      return {false, false, "injected entity save failure"};
    }
    payloads_[position].assign(bytes.begin(), bytes.end());
    activePositionalFailures_.erase(position);
    ++successfulWrites_;
    writePayloads_.emplace_back(bytes.begin(), bytes.end());
    return {true, false, {}};
  }

  void setPayload(const ChunkTicketPos &position,
                  std::vector<std::uint8_t> bytes) {
    std::lock_guard lock(mutex_);
    payloads_[position] = std::move(bytes);
  }
  void failNextWrites(int count) {
    std::lock_guard lock(mutex_);
    failWrites_ = count;
  }
  void failNextWriteFor(const ChunkTicketPos &position) {
    std::lock_guard lock(mutex_);
    ++positionalFailWrites_[position];
  }
  void setBlockWrites(bool blocked) {
    std::lock_guard lock(mutex_);
    blockWrites_ = blocked;
    changed_.notify_all();
  }
  int activePositionalFailures(
      const ChunkTicketPos &position) const {
    std::lock_guard lock(mutex_);
    const auto found = activePositionalFailures_.find(position);
    return found == activePositionalFailures_.end() ? 0 : found->second;
  }
  std::vector<std::vector<std::uint8_t>> writes() const {
    std::lock_guard lock(mutex_);
    return writePayloads_;
  }
  std::vector<ChunkTicketPos> writePositions() const {
    std::lock_guard lock(mutex_);
    return writePositions_;
  }
  int writeCalls() const {
    std::lock_guard lock(mutex_);
    return writeCalls_;
  }
  int successes() const {
    std::lock_guard lock(mutex_);
    return successfulWrites_;
  }
  std::optional<std::vector<std::uint8_t>> payload(
      const ChunkTicketPos &position) const {
    std::lock_guard lock(mutex_);
    const auto found = payloads_.find(position);
    if (found == payloads_.end())
      return std::nullopt;
    return found->second;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool blockWrites_ = false;
  std::map<ChunkTicketPos, std::vector<std::uint8_t>> payloads_;
  std::vector<std::vector<std::uint8_t>> writePayloads_;
  std::vector<ChunkTicketPos> writePositions_;
  std::map<ChunkTicketPos, int> positionalFailWrites_;
  std::map<ChunkTicketPos, int> activePositionalFailures_;
  int failWrites_ = 0;
  int writeCalls_ = 0;
  int successfulWrites_ = 0;
};

void seedNeighborhood(const std::shared_ptr<FakeStore> &store,
                      const ChunkTicketPos &center, int radius) {
  for (int dx = -radius; dx <= radius; ++dx) {
    for (int dz = -radius; dz <= radius; ++dz) {
      const ChunkTicketPos position{center.x + dx, center.z + dz};
      store->setPayload(position, makeChunkNbt(position));
    }
  }
}

void seedFullNeighborhood(const std::shared_ptr<FakeStore> &store,
                          const ChunkTicketPos &center) {
  seedNeighborhood(store, center, 1);
}

template <class Predicate>
bool waitManager(WorldChunkManager &manager, Predicate predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(8);
  for (;;) {
    manager.pump();
    if (predicate())
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::string blockNameAtPosition(World &world, Pos pos) {
  return std::string(world.getBlock(pos).name());
}

void testRestoredPersistentTicketsDriveResidency(
    const std::filesystem::path &sourceRegion,
    const std::filesystem::path &scratch) {
  const std::filesystem::path region = scratch / "restored-ticket-region";
  std::filesystem::create_directories(region);
  std::filesystem::copy_file(sourceRegion / "r.-1.-1.mca",
                             region / "r.-1.-1.mca",
                             std::filesystem::copy_options::overwrite_existing);
  const auto ticketPath = ticketDataPath(region);
  savePersistentTickets(
      ticketPath,
      std::vector<PersistentTicketRecord>{
          {-1, -1, "minecraft:forced", 31, 0}});
  const auto records = loadPersistentTickets(ticketPath);
  require(records.size() == 1 && records.front().x == -1 &&
              records.front().z == -1,
          "persistent forced record reads back before installation");
  World world(-64, 320);
  {
    WorldChunkManager manager(world, region);
    std::vector<std::pair<ChunkTicketPos, Ticket>> restore;
    for (const PersistentTicketRecord &record : records)
      restore.emplace_back(
          ChunkTicketPos{record.x, record.z},
          Ticket(record.type == ticket_types::Forced.name
                     ? ticket_types::Forced
                     : ticket_types::Portal,
                 record.level, record.ticksLeft));
    (void)manager.restorePersistentTickets(restore);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(-1, -1) &&
                     manager.ticketSources().query({-1, -1}).loadingLevel == 31;
            }),
            "restored persistent forced ticket installs FULL chunk");
    manager.setForced({-1, -1}, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(-1, -1) &&
                     manager.ticketSources().query({-1, -1}).loadingLevel == 45;
            }),
            "clearing restored forced ticket releases the chunk exactly");
    require(manager.close().complete,
            "restored-ticket manager shutdown completes");
  }
}

void testRealWorldDiskCycle(const std::filesystem::path &sourceRegion,
                            const std::filesystem::path &scratch) {
  const std::filesystem::path region = scratch / "real-world-region";
  std::filesystem::create_directories(region);
  std::filesystem::copy_file(sourceRegion / "r.-1.-1.mca",
                             region / "r.-1.-1.mca",
                             std::filesystem::copy_options::overwrite_existing);
  World world(-64, 320);
  world.enableLighting();
  int lightLoads = 0, lightUnloads = 0;
  std::vector<std::uint64_t> publishedLightEpochs;
  world.lightChunkLifecycleObserver = [&](int, int, bool loaded) {
    if (loaded)
      ++lightLoads;
    else
      ++lightUnloads;
  };
  world.lightPublicationObserver =
      [&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        if (snapshot->chunkX == -1 && snapshot->chunkZ == -1)
          publishedLightEpochs.push_back(snapshot->lifecycleEpoch);
      };
  const ChunkTicketPos ticket{-1, -1};
  const Pos target{-16, 0, -16};
  const std::int64_t initialTime = world.gameTime();
  {
    WorldChunkManager manager(world, region);
    manager.setForced(ticket, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(ticket);
              return status &&
                     status->state == ChunkResidencyState::Resident &&
                     world.isChunkLoaded(ticket.x, ticket.z);
            }),
            "real fixture does not reach managed Resident");
    require(blockNameAtPosition(world, target) == "minecraft:stone",
            "managed World install exposes the terrain palette");
    require(waitManager(manager, [&] {
              return !publishedLightEpochs.empty();
            }),
            "managed install publishes its first light epoch");
    const std::uint64_t firstLightEpoch = publishedLightEpochs.front();
    require(manager.setBlock(target, Blocks::GLASS.defaultState()),
            "managed World edit succeeds");
    require(world.getBlock(target).name() == "minecraft:glass",
            "managed World edit is visible before save");
    manager.setForced(ticket, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(ticket.x, ticket.z);
            }),
            "clean managed ticket release saves and actually unloads");
    require(lightLoads == 1 && lightUnloads == 1,
            "managed install/unload emits one light lifecycle edge each");
    require(world.getBlock(target).isAir(),
            "actual unload invalidates the World section read cache");
    ScheduledTick restore{{-16, 0, -16}, Blocks::STONE.defaultState().block(),
                          0};
    require(!world.restoreScheduledTick(restore),
            "managed unload removes the chunk tick container");
    DecodedChunk disk = decodeChunkAt(region, ticket.x, ticket.z);
    require(disk.status == DecodeStatus::Ok &&
                disk.root.value.get("world_cycle_extension"),
            "managed save preserves the untouched NBT template");

    manager.setForced(ticket, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(ticket.x, ticket.z) &&
                     world.getBlock(target).name() == "minecraft:glass";
            }),
            "reissued ticket reloads the saved terrain edit");
    require(waitManager(manager, [&] {
              return std::any_of(
                  publishedLightEpochs.begin(), publishedLightEpochs.end(),
                  [firstLightEpoch](std::uint64_t epoch) {
                    return epoch > firstLightEpoch;
                  });
            }),
            "managed reload uses a newer monotonic light lifecycle epoch");
    manager.setForced(ticket, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(ticket.x, ticket.z);
            }),
            "second managed release unloads");
    manager.close();
  }
  require(world.gameTime() == initialTime,
          "managed lifecycle never advances gameTime");
  world.disableLighting();
}

void testDynamicChunksAreRejected() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos blockEntity{1, 0};
  const ChunkTicketPos entity{2, 0};
  store->setPayload(blockEntity,
                    makeChunkNbt(blockEntity, DynamicKind::BlockEntity));
  store->setPayload(entity, makeChunkNbt(entity, DynamicKind::Entity));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(blockEntity, true);
    manager.setForced(entity, true);
    require(waitManager(manager, [&] {
              auto a = manager.lifecycleStatus(blockEntity);
              auto b = manager.lifecycleStatus(entity);
              return a && b &&
                     a->state == ChunkResidencyState::Failed &&
                     b->state == ChunkResidencyState::Failed;
            }),
            "block-entity/entity chunks are explicitly rejected");
    require(!world.isChunkLoaded(blockEntity.x, blockEntity.z) &&
                !world.isChunkLoaded(entity.x, entity.z),
            "rejected dynamic chunks never create World air chunks");
    require(manager.managedStatus(blockEntity)->error.find("block entities") !=
                std::string::npos &&
                manager.managedStatus(entity)->error.find("entities") !=
                    std::string::npos,
            "dynamic rejection names the unsupported persistent data");
    manager.setForced(blockEntity, false);
    manager.setForced(entity, false);
    require(waitManager(manager, [&] {
              return !manager.managedStatus(blockEntity) &&
                     !manager.managedStatus(entity);
            }),
            "nonresident rejected coordinates release historical diagnostics");
    require(manager.lifecycleCoordinateCountForTest() == 0,
            "rejected nonresident coordinates retain no lifecycle token");
    manager.close();
  }
}

void testManagedPaletteInstall() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{7, 0};
  store->setPayload(position, makeMultiPaletteChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "multi-palette managed fixture reaches Resident");
    const Pos glassPos{16 * 7, 0, 0};
    const Pos stonePos{16 * 7, 1, 1};
    require(world.getBlock(glassPos).name() == "minecraft:glass" &&
                world.getBlock(stonePos).name() == "minecraft:stone",
            "managed install expands the palette into World section storage");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "multi-palette managed chunk cleanly unloads");
    manager.close();
  }
}

void testStaleManagedLoad() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{4, 0};
  store->setPayload(position, makeChunkNbt(position));
  store->setBlockReads(true);
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    store->waitForRead(1);
    manager.setForced(position, false);
    manager.setForced(position, true);
    store->releaseReads();
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident &&
                     world.isChunkLoaded(position.x, position.z);
            }),
            "reissued managed ticket installs only the new read generation");
    require(manager.lifecycleStatus(position)->epoch >= 3,
            "stale managed read is separated by epoch");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "stale-load test cleanup unloads");
    manager.close();
  }
}

void testSaveDuringEditAndFailure() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{5, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    AutosaveLimits immediate;
    immediate.cooldown = std::chrono::milliseconds(0);
    immediate.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(immediate);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "managed save fixture reaches Resident");
    const Pos first{16 * 5, 0, 0};
    const Pos second{16 * 5 + 1, 0, 0};
    require(manager.setBlock(first, Blocks::AIR.defaultState()),
            "first managed edit succeeds");
    store->setBlockWrites(true);
    manager.setForced(position, false);
    store->waitForWrite(1);
    manager.rawSetBlock(second, Blocks::GLASS.defaultState());
    require(world.getBlock(second).name() == "minecraft:glass",
            "rawSetBlock edit lands while save v1 is blocked");
    store->releaseWrites();
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z) &&
                     store->successes() >= 2;
            }),
            "new dirty edit is saved before managed unload");
    auto writes = store->writes();
    DecodedChunk latest = decodeChunkNbt(writes.back());
    const auto &states = *latest.sections[0].blockStates;
    require(states.palette[states.cell(0)].name == "minecraft:air" &&
                states.palette[states.cell(1)].name == "minecraft:glass",
            "both save generations are present in the final disk payload");
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     world.getBlock(first).isAir() &&
                     world.getBlock(second).name() == "minecraft:glass";
            }),
            "reload restores edits made during an in-flight save");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "save-during-edit cleanup unloads");
    manager.close();
  }

  store = std::make_shared<FakeStore>();
  store->setPayload(position, makeChunkNbt(position));
  store->failNextWrites(1);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "save-failure fixture reaches Resident");
    const Pos target{16 * 5, 0, 0};
    require(manager.setBlock(target, Blocks::AIR.defaultState()),
            "save-failure edit succeeds");
    require(world.scheduleTick(
                ScheduledTick{{target.x + 2, 0, target.z},
                              Blocks::STONE.defaultState().block(),
                              world.gameTime() + 9, 0, 50}),
            "save-failure tick change succeeds");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status && status->state == ChunkResidencyState::Resident &&
                     status->failure == ChunkLoadFailure::Io &&
                     world.isChunkLoaded(position.x, position.z);
            }),
            "save failure leaves the World chunk resident and dirty");
    require(manager.retrySave(position), "explicit managed retry is accepted");
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z) &&
                     store->successes() == 1;
            }),
            "retry persists dirty terrain before managed unload");
    DecodedChunk retrySaved = decodeChunkNbt(store->writes().back());
    require(retrySaved.blockTicks.size() == 1 &&
                retrySaved.blockTicks[0].delay == 9,
            "failed tick save retains and retries the pending tick list");
    manager.close();
  }
}

void testHeldDirtyAutosaveIsBudgetedAndIdleDoesNotScan() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{15, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 4;
    limits.maxSubmissions = 1;
    limits.maxInFlightSaves = 1;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::seconds(10);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "held autosave fixture reaches Resident");
    require(manager.setBlock({15 * 16, 0, 0},
                             Blocks::AIR.defaultState()),
            "held autosave edit succeeds");
    manager.pump();
    require(store->writeCallCount() == 0,
            "socket-facing pump does not spend the held autosave budget");
    store->setBlockWrites(true);
    const auto beforeTick = std::chrono::steady_clock::now();
    manager.serverTick(true, true);
    const auto elapsed = std::chrono::steady_clock::now() - beforeTick;
    require(elapsed < std::chrono::seconds(1),
            "autosave submission never waits for the blocked I/O worker");
    const AutosaveStats submitted = manager.lastAutosaveStats();
    std::cout << "autosave held checks=" << submitted.candidateChecks
              << " submissions=" << submitted.submissions
              << " inflight=" << submitted.inFlight << std::endl;
    require(submitted.submissions == 1 && submitted.inFlight == 1 &&
                world.isChunkLoaded(position.x, position.z),
            "one held dirty chunk is submitted within the per-tick budget");
    store->waitForWrite(1);
    manager.pump();
    require(world.isChunkLoaded(position.x, position.z),
            "in-flight held autosave keeps the chunk resident");
    store->releaseWrites();
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && status->state == ChunkResidencyState::Resident &&
                     status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "held dirty autosave completes naturally");
    const int writesAfterSave = store->writeCallCount();
    for (int i = 0; i < 4; ++i)
      manager.serverTick(true, true);
    std::cout << "autosave idle checks="
              << manager.lastAutosaveStats().candidateChecks
              << " writes=" << store->writeCallCount() << std::endl;
    require(manager.lastAutosaveStats().candidateChecks == 0 &&
                store->writeCallCount() == writesAfterSave,
            "idle server ticks do not scan resident chunks or resave");

    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "held autosave fixture unloads cleanly");
    require(manager.close().complete,
            "held autosave shutdown completes");
  }
}

void testAutosaveBudgetAndLocalDynamicBlockers() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos firstEntityChunk{20, 0};
  const ChunkTicketPos secondEntityChunk{21, 0};
  const ChunkTicketPos freeChunk{22, 0};
  seedNeighborhood(store, firstEntityChunk, 2);
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 8;
    limits.maxSubmissions = 1;
    limits.maxInFlightSaves = 1;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    for (const ChunkTicketPos &position :
         {firstEntityChunk, secondEntityChunk, freeChunk})
      manager.setForced(position, true);
    require(waitManager(manager, [&] {
              for (const ChunkTicketPos &position :
                   {firstEntityChunk, secondEntityChunk, freeChunk}) {
                const auto status = manager.lifecycleStatus(position);
                if (!status ||
                    status->state != ChunkResidencyState::Resident)
                  return false;
              }
              return true;
            }),
            "local blocker fixture installs all three chunks");

    auto firstBlocker = std::make_shared<PrimedTnt>(
        world, Vec3{firstEntityChunk.x * 16 + 8.5, 0.5,
                    firstEntityChunk.z * 16 + 8.5});
    auto secondBlocker = std::make_shared<PrimedTnt>(
        world, Vec3{secondEntityChunk.x * 16 + 8.5, 0.5,
                    secondEntityChunk.z * 16 + 8.5});
    world.addEntity(firstBlocker);
    world.addEntity(secondBlocker);

    require(manager.setBlock({firstEntityChunk.x * 16, 0, 0},
                             Blocks::AIR.defaultState()) &&
                manager.setBlock({secondEntityChunk.x * 16, 0, 0},
                                 Blocks::AIR.defaultState()) &&
                manager.setBlock({freeChunk.x * 16, 0, 0},
                                 Blocks::AIR.defaultState()),
            "three chunks become independently dirty");
    manager.serverTick(true, true);
    const AutosaveStats bounded = manager.lastAutosaveStats();
    std::cout << "autosave blocked-prefix checks="
              << bounded.candidateChecks << " submissions="
              << bounded.submissions << " inflight=" << bounded.inFlight
              << std::endl;
    require(bounded.candidateChecks >= 2 && bounded.submissions == 1,
            "blocked local candidates consume bounded checks without starvation");
    require(waitManager(manager, [&] {
              return store->successes() == 1;
            }),
            "one unblocked neighbor saves while two chunks remain blocked");
    auto wrote = store->writes();
    require(!wrote.empty() &&
                decodeChunkNbt(wrote.back()).chunkX == freeChunk.x,
            "the saved chunk is the unblocked neighbor");
    require(manager.lifecycleStatus(firstEntityChunk)->dirtyVersion >
                manager.lifecycleStatus(firstEntityChunk)->savedDirtyVersion &&
                manager.lifecycleStatus(secondEntityChunk)->dirtyVersion >
                    manager.lifecycleStatus(secondEntityChunk)->savedDirtyVersion,
            "piston and entity blockers remain local to their chunks");

    world.removeEntity(firstBlocker);
    world.removeEntity(secondBlocker);
    world.tick();
    limits.maxSubmissions = 4;
    limits.maxInFlightSaves = 4;
    manager.setAutosaveLimitsForTest(limits);
    for (int pass = 0; pass < 4 && store->successes() < 3; ++pass) {
      const int before = store->successes();
      manager.serverTick(true, true);
      require(waitManager(manager, [&] {
                return store->successes() > before;
              }),
              "a bounded dynamic-retry pass makes forward progress");
    }
    require(store->successes() == 3,
            "dynamic blockers save normally after they disappear");
    for (const ChunkTicketPos &position :
         {firstEntityChunk, secondEntityChunk, freeChunk})
      manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(firstEntityChunk.x, firstEntityChunk.z) &&
                     !world.isChunkLoaded(secondEntityChunk.x, secondEntityChunk.z) &&
                     !world.isChunkLoaded(freeChunk.x, freeChunk.z);
            }),
            "local blocker fixture unloads after saving");
    require(manager.close().complete,
            "local blocker shutdown completes");
  }
}

void testFailedAutosaveRetainsAndMergesNewerPatch() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{30, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 4;
    limits.maxSubmissions = 1;
    limits.maxInFlightSaves = 1;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "failed autosave fixture reaches Resident");
    const Pos first{position.x * 16, 0, 0};
    const Pos second{position.x * 16 + 1, 0, 0};
    require(manager.setBlock(first, Blocks::AIR.defaultState()),
            "autosave v1 edit succeeds");
    store->failNextWrites(1);
    store->setBlockWrites(true);
    manager.serverTick(true, true);
    store->waitForWrite(1);
    require(manager.setBlock(second, Blocks::GLASS.defaultState()),
            "autosave v2 edit lands while v1 is blocked");
    store->releaseWrites();
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && status->state == ChunkResidencyState::Resident &&
                     status->failure == ChunkLoadFailure::Io &&
                     status->dirtyVersion > status->savedDirtyVersion;
            }),
            "failed snapshot leaves the held chunk resident and dirty");
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && store->successes() == 1 &&
                     status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "retried newer snapshot reaches the disk");
    auto wrote = store->writes();
    std::cout << "autosave retry writes=" << wrote.size()
              << " successes=" << store->successes() << std::endl;
    require(wrote.size() == 2,
            "one failed and one successful write were captured");
    DecodedChunk saved = decodeChunkNbt(wrote.back());
    const auto &states = *saved.sections[0].blockStates;
    require(states.palette[states.cell(0)].name == "minecraft:air" &&
                states.palette[states.cell(1)].name == "minecraft:glass",
            "failed v1 edits are merged with v2 without depending on root mutation");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "failed autosave fixture unloads after retry");
    require(manager.close().complete,
            "failed autosave shutdown completes");
  }
}

void testRealEntityRegionRejectsInstall(
    const std::filesystem::path &sourceRegion,
    const std::filesystem::path &scratch) {
  const std::filesystem::path base = scratch / "entity-reject";
  const std::filesystem::path region = base / "region";
  const std::filesystem::path entities = base / "entities";
  std::filesystem::create_directories(region);
  std::filesystem::create_directories(entities);
  std::filesystem::copy_file(sourceRegion / "r.-1.-1.mca",
                             region / "r.-1.-1.mca",
                             std::filesystem::copy_options::overwrite_existing);
  nbt::RootTag entityRoot;
  entityRoot.name = "Entities";
  entityRoot.value.type = 10;
  nbt::Tag entity;
  entity.type = 10;
  add(entity, "id", string("minecraft:item"));
  add(entityRoot.value, "Entities", list(10, {std::move(entity)}));
  (void)region::writeChunkNbt(entities, -1, -1,
                              nbt::writeRoot(entityRoot), 100);
  const auto malformedBefore = region::readChunkNbt(entities, -1, -1).nbt;
  const ChunkTicketPos position{-1, -1};
  World world(-64, 320);
  {
    WorldChunkManager manager(world, region);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status && status->state == ChunkResidencyState::Failed;
            }),
            "nonempty sibling entity region rejects managed installation");
    require(!world.isChunkLoaded(position.x, position.z),
            "entity-region rejection does not install empty entity data");
    auto info = manager.managedStatus(position);
    require(info &&
                ((info->error.find("entity") != std::string::npos) ||
                 (info->lifecycle.detail.find("entity") !=
                  std::string::npos)),
            "entity-region rejection reports its unsupported data");
    (void)manager.close();
    require(region::readChunkNbt(entities, -1, -1).nbt ==
                malformedBefore,
            "rejected unsupported entity data is not overwritten");
  }
}

void testRealTickDiskCycle(const std::filesystem::path &sourceRegion,
                           const std::filesystem::path &scratch) {
  const std::filesystem::path region = scratch / "real-tick-region";
  std::filesystem::create_directories(region);
  std::filesystem::copy_file(sourceRegion / "r.-1.-1.mca",
                             region / "r.-1.-1.mca",
                             std::filesystem::copy_options::overwrite_existing);
  const ChunkTicketPos position{-1, -1};
  World world(-64, 320);
  {
    WorldChunkManager manager(world, region);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z);
            }),
            "real Java tick fixture reaches World Resident");
    ChunkTickSnapshot ticks = world.snapshotChunkTicks(position.x, position.z);
    require(ticks.blockTicks.size() == 3 && ticks.fluidTicks.size() == 1,
            "real Java block/fluid tick lists load");
    require(ticks.blockTicks[0].order == -3 &&
                ticks.blockTicks[1].order == -2 &&
                ticks.blockTicks[2].order == -1 &&
                ticks.fluidTicks[0].first.order == -1,
            "real Java lists unpack independent duplicate ordering");
    const Pos edit{-13, 0, -16};
    require(manager.setBlock(edit, Blocks::GLASS.defaultState()),
            "real tick fixture terrain edit succeeds");
    ChunkShutdownResult shutdown = manager.close();
    require(shutdown.complete && shutdown.unsavedChunks.empty(),
            "held-ticket real tick chunk saves and shuts down");
    require(!world.isChunkLoaded(position.x, position.z),
            "real tick chunk actually unloads after tick save");
  }

  {
    WorldChunkManager manager(world, region);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z);
            }),
            "real tick chunk reloads after save");
    ChunkTickSnapshot ticks = world.snapshotChunkTicks(position.x, position.z);
    require(ticks.blockTicks.size() == 3 && ticks.fluidTicks.size() == 1 &&
                ticks.blockTicks[0].trigger == 5 &&
                ticks.fluidTicks[0].first.trigger == 7,
            "reloaded Java tick delays and priorities remain stable");
    require(world.getBlock({-13, 0, -16}).name() == "minecraft:glass",
            "reloaded tick chunk preserves its terrain edit");
    ChunkShutdownResult shutdown = manager.close();
    require(shutdown.complete, "reloaded tick chunk shutdown completes");
  }
}

void testCloseHeldTicketReal(const std::filesystem::path &sourceRegion,
                             const std::filesystem::path &scratch) {
  const std::filesystem::path region = scratch / "held-close-region";
  std::filesystem::create_directories(region);
  std::filesystem::copy_file(sourceRegion / "r.-1.-1.mca",
                             region / "r.-1.-1.mca",
                             std::filesystem::copy_options::overwrite_existing);
  const ChunkTicketPos position{-1, -1};
  const Pos target{-16, 0, -16};
  World world(-64, 320);
  {
    WorldChunkManager manager(world, region);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "held-ticket close fixture reaches Resident");
    require(manager.setBlock(target, Blocks::GLASS.defaultState()),
            "held-ticket edit succeeds");
    ChunkShutdownResult shutdown = manager.close();
    require(shutdown.complete && shutdown.workerStopped &&
                shutdown.unsavedChunks.empty() &&
                shutdown.savedChunks.size() == 1 &&
                shutdown.savedChunks[0] == position &&
                !world.isChunkLoaded(position.x, position.z),
            "explicit close saves held-ticket dirty data before stopping");
    require(manager.lastShutdownResult().complete,
            "explicit close exposes its shutdown result");
  }

  {
    WorldChunkManager manager(world, region);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     world.getBlock(target).name() == "minecraft:glass";
            }),
            "new manager reads the held-ticket shutdown edit");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "new manager unloads clean held-close fixture");
    ChunkShutdownResult shutdown = manager.close();
    require(shutdown.complete, "clean manager shutdown completes");
  }
}

void testCloseDrainsPendingSnapshot() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{8, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "close-v1 fixture reaches Resident");
    const Pos first{16 * 8, 0, 0};
    const Pos second{16 * 8 + 1, 0, 0};
    require(manager.setBlock(first, Blocks::AIR.defaultState()),
            "close-v1 first edit succeeds");
    store->setBlockWrites(true);
    manager.setForced(position, false);
    store->waitForWrite(1);
    manager.rawSetBlock(second, Blocks::GLASS.defaultState());

    auto closing = std::async(std::launch::async,
                              [&] { return manager.close(); });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (manager.accepting() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(!manager.accepting(), "close stops new demand immediately");
    store->releaseWrites();
    ChunkShutdownResult shutdown = closing.get();
    require(shutdown.complete && shutdown.workerStopped &&
                shutdown.unsavedChunks.empty() &&
                !world.isChunkLoaded(position.x, position.z),
            "close waits for v1 and then persists pending v2");
    require(store->successes() >= 2,
            "close issued both snapshot versions");
  }

  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    const Pos first{16 * 8, 0, 0};
    const Pos second{16 * 8 + 1, 0, 0};
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     world.getBlock(first).isAir() &&
                     world.getBlock(second).name() == "minecraft:glass";
            }),
            "reload after shutdown sees v1 and pending v2 edits");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "close-v1 reload cleanup unloads");
    require(manager.close().complete,
            "close-v1 cleanup shutdown completes");
  }
}

void testCloseFailureAndLateReadReports() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{9, 0};
  store->setPayload(position, makeChunkNbt(position));
  store->failNextWrites(1);
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "close-failure fixture reaches Resident");
    const Pos target{16 * 9, 0, 0};
    require(manager.setBlock(target, Blocks::AIR.defaultState()),
            "close-failure edit succeeds");
    ChunkShutdownResult shutdown = manager.close();
    require(!shutdown.complete && shutdown.workerStopped &&
                shutdown.unsavedChunks.size() == 1 &&
                shutdown.unsavedChunks[0].position == position &&
                !shutdown.unsavedChunks[0].error.empty() &&
                world.isChunkLoaded(position.x, position.z),
            "failed close reports and preserves the resident dirty chunk");
  }

  auto lateStore = std::make_shared<FakeStore>();
  const ChunkTicketPos latePosition{10, 0};
  lateStore->setPayload(latePosition, makeChunkNbt(latePosition));
  lateStore->setBlockReads(true);
  World lateWorld(-64, 320);
  {
    WorldChunkManager manager(lateWorld, lateStore, 3, 10, knownEmptyEntityData);
    manager.setForced(latePosition, true);
    lateStore->waitForRead(1);
    auto closing = std::async(std::launch::async,
                              [&] { return manager.close(); });
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (manager.accepting() &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lateStore->releaseReads();
    ChunkShutdownResult shutdown = closing.get();
    require(shutdown.complete &&
                !lateWorld.isChunkLoaded(latePosition.x, latePosition.z),
            "late read is rejected rather than installed during close");
  }
}

void testReadinessRadiiAndMissingNeighbor() {
  const ChunkTicketPos center{-1, -1};
  auto store = std::make_shared<FakeStore>();
  seedNeighborhood(store, center, 1);
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(center, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(center.x, center.z) &&
                     manager.tickReadiness(center).blockTicking;
            }),
            "3x3 installed ring reaches block ticking at negative coordinates");
    const ChunkTickReadiness readiness = manager.tickReadiness(center);
    require(readiness.loadingLevel <= ticket_levels::FullChunkLevel &&
                readiness.simulationLevel <= ticket_levels::BlockTickingLevel &&
                readiness.simulationLevel <= ticket_levels::EntityTickingLevel &&
                !readiness.entityTicking,
            "simulation entity level alone does not satisfy the +2 FULL ring");
    require(manager.close().complete, "3x3 readiness shutdown completes");
  }

  const ChunkTicketPos missingCenter{20, -2};
  store = std::make_shared<FakeStore>();
  seedNeighborhood(store, missingCenter, 1);
  store->erasePayload({missingCenter.x + 1, missingCenter.z + 1});
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(missingCenter, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(missingCenter.x, missingCenter.z);
            }),
            "center installs even while a FULL neighbor is missing");
    const ChunkTickReadiness readiness = manager.tickReadiness(missingCenter);
    require(!readiness.blockTicking &&
                readiness.loadingLevel <= ticket_levels::FullChunkLevel &&
                readiness.simulationLevel <= ticket_levels::BlockTickingLevel,
            "ticket levels remain sufficient but missing FULL neighbor blocks ticking");
    require(manager.close().complete, "missing-neighbor shutdown completes");
  }

  const ChunkTicketPos entityCenter{40, 2};
  store = std::make_shared<FakeStore>();
  seedNeighborhood(store, entityCenter, 2);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(entityCenter, true);
    require(waitManager(manager, [&] {
              const ChunkTickReadiness readiness =
                  manager.tickReadiness(entityCenter);
              return readiness.blockTicking && readiness.entityTicking;
            }),
            "5x5 installed FULL ring enables entity ticking readiness");
    require(manager.close().complete, "entity-radius shutdown completes");
  }
}

void testDueTicksWaitForReadinessAndResumeOrder() {
  const ChunkTicketPos center{-5, -2};
  const ChunkTicketPos missing{center.x + 1, center.z + 1};
  auto store = std::make_shared<FakeStore>();
  seedNeighborhood(store, center, 1);
  store->setPayload(center, makeSavedTickChunkNbt(center));
  store->erasePayload(missing);
  World world(-64, 320);
  world.traceUpdates = true;
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(center, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(center.x, center.z) &&
                     !manager.tickReadiness(center).blockTicking;
            }),
            "due-tick fixture starts resident but not block-ticking");
    world.setGameTime(5);
    manager.serverTick(true, true);
    require(world.snapshotChunkTicks(center.x, center.z).blockTicks.size() == 3 &&
                world.snapshotChunkTicks(center.x, center.z).fluidTicks.size() == 1,
            "due ticks remain queued while neighborhood is not ready");
    require(manager.close().complete,
            "paused due-tick shutdown saves and completes");
  }

  store->setPayload(missing, makeChunkNbt(missing));
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(center, true);
    require(waitManager(manager, [&] {
              return manager.tickReadiness(center).blockTicking;
            }),
            "tick resumes only after the +1 FULL ring installs");
    ChunkTickSnapshot before = world.snapshotChunkTicks(center.x, center.z);
    require(before.blockTicks.size() == 3 &&
                before.blockTicks[0].order == -3 &&
                before.blockTicks[1].order == -2 &&
                before.blockTicks[2].order == -1,
            "paused ticks retain original same-trigger order");
    world.log.clear();
    manager.serverTick(true, true);
    require(world.snapshotChunkTicks(center.x, center.z).blockTicks.empty() &&
                world.snapshotChunkTicks(center.x, center.z).fluidTicks.empty(),
            "resumed due ticks execute");
    std::vector<Pos> dispatched;
    for (const LogEntry &entry : world.log)
      if (entry.kind == "scheduled_tick")
        dispatched.push_back(entry.pos);
    require(dispatched.size() == 3 &&
                dispatched[0] == before.blockTicks[0].pos &&
                dispatched[1] == before.blockTicks[1].pos &&
                dispatched[2] == before.blockTicks[2].pos,
            "resumed block ticks drain in preserved sub-tick order");
    require(manager.close().complete,
            "resumed tick shutdown completes");
  }
}

void testServerTickOwnsTimeoutAging() {
  auto store = std::make_shared<FakeStore>();
  World world(-64, 320);
  WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
  const ChunkTicketPos position{60, 60};
  TicketSystemUpdate update = manager.ticketSources().requestSpawnSearch(position);
  manager.applyTicketUpdate(update);
  require(waitManager(manager, [&] {
            auto status = manager.lifecycleStatus(position);
            return status &&
                   (status->state == ChunkResidencyState::Missing ||
                    status->state == ChunkResidencyState::Failed);
          }),
          "timed spawn-search holder settles absent");
  const auto &tickets =
      manager.ticketSources().tickets().storage().getTickets(position);
  require(!tickets.empty(), "spawn-search timed ticket exists");
  const std::int64_t initialTicks = tickets.front().ticksLeft();
  for (int i = 0; i < 5; ++i)
    manager.pump();
  require(manager.ticketSources()
              .tickets()
              .storage()
              .getTickets(position)
              .front()
              .ticksLeft() == initialTicks,
          "repeated pump does not age timed tickets");
  const std::int64_t beforePaused = world.gameTime();
  manager.serverTick(true, false);
  require(world.gameTime() == beforePaused &&
              manager.ticketSources()
                      .tickets()
                      .storage()
                      .getTickets(position)
                      .front()
                      .ticksLeft() == initialTicks,
          "paused server tick neither advances time nor ages ticket");
  manager.serverTick(true, true);
  require(world.gameTime() == beforePaused + 1 &&
              manager.ticketSources()
                      .tickets()
                      .storage()
                      .getTickets(position)
                      .front()
                      .ticksLeft() < initialTicks,
          "real server tick ages the timed ticket once");
  (void)manager.close();
}

void testEntityRegionProbeAndPositionGate() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos rejected{70, 0};
  store->setPayload(rejected, makeChunkNbt(rejected));
  World world(-64, 320);
  {
    EntityChunkDataProbe nonEmpty = [](const ChunkTicketPos &) {
      return EntityChunkDataResult{
          EntityChunkDataResult::Status::NonEmpty,
          "test entity region contains one entity"};
    };
    WorldChunkManager manager(world, store, 3, 10, nonEmpty);
    manager.setForced(rejected, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(rejected);
              return status && status->state == ChunkResidencyState::Failed;
            }),
            "nonempty entity-region data rejects managed install");
    require(!world.isChunkLoaded(rejected.x, rejected.z),
            "entity-data rejection does not create an air chunk");
    (void)manager.close();
  }

  auto gateStore = std::make_shared<FakeStore>();
  const ChunkTicketPos center{80, 0};
  seedNeighborhood(gateStore, center, 1);
  {
    WorldChunkManager manager(world, gateStore, 3, 10, knownEmptyEntityData);
    manager.setForced(center, true);
    require(waitManager(manager, [&] {
              return manager.tickReadiness(center).blockTicking;
            }),
            "entity position-gate fixture reaches block ticking");
    const Vec3 spawn{center.x * 16 + 0.5, 2.0, center.z * 16 + 0.5};
    world.spawnItemAt(spawn, "minecraft:stone");
    require(!world.entities.empty(), "runtime entity exists for gate test");
    const int before = world.entities.back()->tickCount;
    manager.serverTick(true, true);
    require(world.entities.back()->tickCount == before,
            "entity remains paused without the +2 entity-ticking ring");
    world.entities.back()->discard();
    world.tickEntities();
    (void)manager.close();
  }
}

void testReadMetadataReleaseBurstAndLongTravel() {
  auto store = std::make_shared<FakeStore>();
  World world(-64, 320);
  WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
  const auto before = manager.entityReadMetadataCountForTest();
  for (int x = 100; x < 300; ++x) {
    const ChunkTicketPos missing{x, 50};
    manager.setForced(missing, true);
    manager.pump();
  }
  require(waitManager(manager, [&] {
            return manager.entityReadMetadataCountForTest() == before;
          }),
          "burst missing reads release every per-read entity metadata record");
  for (int x = 400; x < 500; ++x)
    (void)manager.tickReadiness({x, 50});
  require(manager.readinessCacheSizeForTest() == 0,
          "long travel through missing chunks retains no readiness entries");
  (void)manager.close();
}

void testOfflineAndBlockEventGate() {
  World world(-64, 320);
  world.loadChunk(0, 0);
  require(world.restoreScheduledTick(
              ScheduledTick{{0, 0, 0}, Blocks::STONE.defaultState().block(),
                            0, 0, 1}),
          "legacy offline due tick installs");
  world.blockEvent({0, 0, 0}, BlockRef(Blocks::STONE), 0, 0);
  world.tick();
  require(world.snapshotChunkTicks(0, 0).blockTicks.empty() &&
              world.blockEventCount() == 0,
          "empty readiness hooks preserve legacy offline tick/event behavior");

  world.blockTickPositionReady = [](Pos) { return false; };
  world.blockEvent({0, 0, 0}, BlockRef(Blocks::STONE), 0, 0);
  world.tick();
  require(world.blockEventCount() == 1,
          "unready block event is retained for later rescheduling");
  world.blockTickPositionReady = [](Pos) { return true; };
  world.tick();
  require(world.blockEventCount() == 0,
          "rescheduled block event runs when readiness returns");
  world.blockTickPositionReady = {};
}

void testTimeoutAgesDirtyDuringInFlightSave() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{90, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
  TicketSystemUpdate timed =
      manager.ticketSources().requestSpawnSearch(position);
  manager.applyTicketUpdate(timed);
  require(waitManager(manager, [&] {
            auto status = manager.lifecycleStatus(position);
            return status && status->state == ChunkResidencyState::Resident;
          }),
          "timed holder fixture reaches Resident");
  const Pos edit{16 * 90, 0, 0};
  require(manager.setBlock(edit, Blocks::AIR.defaultState()),
          "timed holder dirty edit succeeds");
  store->setBlockWrites(true);
  require(manager.retrySave(position), "explicit save starts for timed holder");
  store->waitForWrite(1);
  const std::int64_t initialTicks =
      manager.ticketSources()
          .tickets()
          .storage()
          .getTickets(position)
          .front()
          .ticksLeft();
  manager.serverTick(true, true);
  const auto &afterTick =
      manager.ticketSources().tickets().storage().getTickets(position);
  require(!afterTick.empty() &&
              afterTick.front().ticksLeft() < initialTicks,
          "dirty resident holder is installed/ready while its save is in flight");
  store->releaseWrites();
  require(waitManager(manager, [&] {
            auto status = manager.lifecycleStatus(position);
            return status && status->state == ChunkResidencyState::Resident &&
                   status->dirtyVersion <= status->savedDirtyVersion;
          }),
          "timed holder save settles cleanly");
  manager.serverTick(true, true);
  require(manager.ticketSources()
              .tickets()
              .storage()
              .getTickets(position)
              .empty(),
          "ready holder consumes its final tick and expires after the save");
  (void)manager.close();
}

void testPerChunkTickSnapshotTake() {
  World world(-64, 320);
  world.loadChunk(0, 0);
  world.loadChunk(1, 0);
  require(world.scheduleTick(
              ScheduledTick{{0, 0, 0}, Blocks::STONE.defaultState().block(),
                            10, 0, 1}),
          "first per-chunk block tick schedules");
  require(world.scheduleFluidTick(
              ScheduledTick{{16, 0, 0}, BlockRef{}, 10, 0, 2},
              FluidRef(Fluids::water())),
          "second per-chunk fluid tick schedules");
  ChunkTickSnapshot first = world.snapshotChunkTicks(0, 0);
  ChunkTickSnapshot second = world.snapshotChunkTicks(1, 0);
  require(first.blockTicks.size() == 1 && first.fluidTicks.empty() &&
              second.blockTicks.empty() && second.fluidTicks.size() == 1,
          "per-chunk snapshots do not enumerate the dimension");
  ChunkTickSnapshot taken = world.takeChunkTicks(0, 0);
  require(taken.blockTicks.size() == 1 &&
              world.chunkTickCount(0, 0) == 0 &&
              world.chunkTickCount(1, 0) == 1,
          "taking one chunk tick list leaves other chunks untouched");
}

void testSavedTicksPauseDirtyAndOrder() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{11, 0};
  seedFullNeighborhood(store, position);
  store->setPayload(position, makeSavedTickChunkNbt(position));
  World world(-64, 320);
  world.setGameTime(100);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident &&
                     manager.tickReadiness(position).blockTicking;
            }),
            "saved-tick managed fixture reaches block-ticking Resident");
    ChunkTickSnapshot first = world.snapshotChunkTicks(position.x, position.z);
    require(first.blockTicks.size() == 3 && first.fluidTicks.size() == 1,
            "saved duplicate block/fluid ticks load into World containers");
    std::string orderMessage = "tick triggers/orders:";
    for (const auto &tick : first.blockTicks)
      orderMessage += " b" + std::to_string(tick.trigger) + ":" +
                      std::to_string(tick.order);
    for (const auto &entry : first.fluidTicks)
      orderMessage += " f" + std::to_string(entry.first.trigger) + ":" +
                      std::to_string(entry.first.order);
    require(first.blockTicks[0].trigger == 105 &&
                first.blockTicks[1].trigger == 105 &&
                first.blockTicks[2].trigger == 105 &&
                first.blockTicks[0].order == -3 &&
                first.blockTicks[1].order == -2 &&
                first.blockTicks[2].order == -1 &&
                first.fluidTicks[0].first.order == -1,
            "block and fluid use independent -N..-1 saved-list order; " +
                orderMessage);
    require(manager.lifecycleStatus(position)->dirtyVersion == 0,
            "initial saved-list unpack is not a dirty edit");

    world.setGameTime(102);
    manager.pump();
    require(manager.lifecycleStatus(position)->dirtyVersion > 0,
            "resident time passage marks nonempty tick lists dirty");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "tick-time dirty chunk saves and unloads");
    auto writes = store->writes();
    DecodedChunk saved = decodeChunkNbt(writes.back());
    require(saved.blockTicks.size() == 3 && saved.fluidTicks.size() == 1 &&
                saved.blockTicks[0].delay == 3 &&
                saved.blockTicks[1].delay == 3 &&
                saved.fluidTicks[0].delay == 5,
            "save stores remaining delay at the current game time");
    require(!saved.root.value.get("block_ticks")->list[0].get("SubTick"),
            "chunk tick save does not invent SubTick");

    world.setGameTime(122);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z);
            }),
            "tick fixture reloads after unloaded time");
    ChunkTickSnapshot resumed =
        world.snapshotChunkTicks(position.x, position.z);
    require(resumed.blockTicks.size() == 3 &&
                resumed.blockTicks[0].trigger == 125 &&
                resumed.blockTicks[0].order == -3 &&
                resumed.fluidTicks[0].first.trigger == 127,
            "unloaded time is frozen and same-trigger order resumes");

    const Pos later{position.x * 16 + 3, 0, 0};
    require(world.scheduleTick(
                ScheduledTick{later, Blocks::STONE.defaultState().block(),
                              world.gameTime() + 50, 0, 777}),
            "schedule a tick without a block edit");
    require(manager.lifecycleStatus(position)->dirtyVersion >
                manager.lifecycleStatus(position)->savedDirtyVersion,
            "tick-only scheduling advances dirty version");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "tick-only dirty list saves before unload");
    saved = decodeChunkNbt(store->writes().back());
    require(saved.blockTicks.size() == 4,
            "current complete tick list reaches disk after tick-only dirty");

    world.setGameTime(172);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z);
            }),
            "tick fixture reloads for execution test");
    const auto beforeExecution = manager.lifecycleStatus(position)->savedDirtyVersion;
    world.setGameTime(177);
    world.tick();
    manager.pump();
    require(manager.lifecycleStatus(position)->dirtyVersion > beforeExecution,
            "tick execution removal is captured as dirty");
    require(world.snapshotChunkTicks(position.x, position.z).blockTicks.size() == 1,
            "only the later non-due block tick remains after execution");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "executed tick state saves before unload");
    saved = decodeChunkNbt(store->writes().back());
    require(saved.blockTicks.size() == 1 &&
                saved.blockTicks[0].delay == 44 &&
                saved.fluidTicks.empty(),
            "execution removal persists the remaining delay and an empty fluid list; blocks=" +
                std::to_string(saved.blockTicks.size()) +
                " delay=" +
                (saved.blockTicks.empty()
                     ? std::string("none")
                     : std::to_string(saved.blockTicks[0].delay)) +
                " fluids=" + std::to_string(saved.fluidTicks.size()));
    require(manager.close().complete, "tick lifecycle shutdown completes");
  }
}

void testTickDelayJavaIntConversion() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{14, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "tick-delay fixture reaches Resident");
    const Pos pos{16 * 14, 0, 0};
    require(world.scheduleTick(
                ScheduledTick{pos, Blocks::STONE.defaultState().block(),
                              std::numeric_limits<std::int64_t>::max(), 0, 1}),
            "extreme tick trigger schedules");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "extreme tick trigger saves and unloads");
    DecodedChunk saved = decodeChunkNbt(store->writes().back());
    require(saved.blockTicks.size() == 1 && saved.blockTicks[0].delay == -1,
            "tick-to-saved delay uses Java int truncation, not saturation");
    require(manager.close().complete,
            "tick-delay conversion shutdown completes");
  }
}

void testTickEditDuringSave() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{12, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    AutosaveLimits immediate;
    immediate.cooldown = std::chrono::milliseconds(0);
    immediate.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(immediate);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "tick-during-save fixture reaches Resident");
    const int x = position.x * 16;
    require(world.scheduleTick(
                ScheduledTick{{x, 0, 0}, Blocks::STONE.defaultState().block(),
                              world.gameTime() + 10, 0, 1}),
            "first tick edit schedules");
    store->setBlockWrites(true);
    manager.setForced(position, false);
    store->waitForWrite(1);
    require(world.scheduleTick(
                ScheduledTick{{x + 1, 0, 0},
                              Blocks::STONE.defaultState().block(),
                              world.gameTime() + 20, 0, 2}),
            "later tick edit schedules while save v1 is blocked");
    store->releaseWrites();
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z) &&
                     store->successes() >= 2;
            }),
            "later tick edit saves before unload");
    DecodedChunk saved = decodeChunkNbt(store->writes().back());
    require(saved.blockTicks.size() == 2,
            "old ack does not clear a later tick-only dirty version");
    require(manager.close().complete, "tick-during-save shutdown completes");
  }
}

void testRepeatedPumpDoesNotRescanTimeSensitiveChunks() {
  auto store = std::make_shared<FakeStore>();
  const std::vector<ChunkTicketPos> positions{{40, 0}, {50, 0}, {60, 0}};
  for (const ChunkTicketPos &position : positions)
    store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    for (const ChunkTicketPos &position : positions)
      manager.setForced(position, true);
    require(waitManager(manager, [&] {
              for (const ChunkTicketPos &position : positions)
                if (!world.isChunkLoaded(position.x, position.z))
                  return false;
              return true;
            }),
            "repeated-pump fixture installs all tick-sensitive chunks");
    for (const ChunkTicketPos &position : positions)
      require(world.scheduleTick(
                  ScheduledTick{{position.x * 16, 0, position.z * 16},
                                Blocks::STONE.defaultState().block(),
                                world.gameTime() + 100, 0, 1}),
              "tick-sensitive chunk schedules one future tick");
    manager.pump();
    world.setGameTime(world.gameTime() + 1);
    const auto before = manager.tickTimeEntriesVisitedForTest();
    manager.pump();
    const auto afterAdvance = manager.tickTimeEntriesVisitedForTest();
    require(afterAdvance - before == positions.size(),
            "one game-time advance visits each tick-sensitive chunk once");

    (void)manager.setPlayer(77, true, ChunkTicketPos{70, 0});
    const auto afterPlayerAdd = manager.tickTimeEntriesVisitedForTest();
    for (int i = 0; i < 20; ++i) {
      (void)manager.setPlayer(77, true, ChunkTicketPos{70, 0});
      manager.pump();
    }
    std::cout << "autosave repeated-pump checks="
              << manager.tickTimeEntriesVisitedForTest() - afterPlayerAdd
              << " writes=" << store->writeCallCount() << std::endl;
    require(manager.tickTimeEntriesVisitedForTest() == afterPlayerAdd &&
                store->writeCallCount() == 0,
            "unchanged setPlayer and pump do not rescan or autosave held dirt");

    manager.removePlayer(77);
    for (const ChunkTicketPos &position : positions)
      manager.setForced(position, false);
    require(waitManager(manager, [&] {
              for (const ChunkTicketPos &position : positions)
                if (world.isChunkLoaded(position.x, position.z))
                  return false;
              return true;
            }),
            "repeated-pump fixture unloads");
    require(manager.close().complete,
            "repeated-pump shutdown completes");
  }
}

void testReacquiredDemandCannotBypassAutosaveTick() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos active{80, 0};
  const ChunkTicketPos reacquired{90, 0};
  store->setPayload(active, makeChunkNbt(active));
  store->setPayload(reacquired, makeChunkNbt(reacquired));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 4;
    limits.maxSubmissions = 1;
    limits.maxInFlightSaves = 1;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(active, true);
    manager.setForced(reacquired, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(active.x, active.z) &&
                     world.isChunkLoaded(reacquired.x, reacquired.z);
            }),
            "reacquired-demand fixture installs both chunks");
    require(manager.setBlock({active.x * 16, 0, 0},
                             Blocks::AIR.defaultState()) &&
                manager.setBlock({reacquired.x * 16, 0, 0},
                                 Blocks::AIR.defaultState()),
            "reacquired-demand fixture makes both chunks dirty");

    store->setBlockWrites(true);
    manager.serverTick(true, true);
    store->waitForWrite(1);
    manager.setForced(reacquired, false);
    manager.setForced(reacquired, true);
    store->releaseWrites();
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(active);
              return status && status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "the first in-flight chunk completes");
    const auto reacquiredStatus = manager.lifecycleStatus(reacquired);
    std::cout << "autosave reacquired writes=" << store->writeCallCount()
              << " dirty=" << reacquiredStatus->dirtyVersion
              << " saved=" << reacquiredStatus->savedDirtyVersion << std::endl;
    require(store->writeCallCount() == 1 && reacquiredStatus->demand &&
                reacquiredStatus->dirtyVersion >
                    reacquiredStatus->savedDirtyVersion,
            "reacquired demand waits for a real server tick");

    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(reacquired);
              return status && store->successes() == 2 &&
                     status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "the held-path server tick saves the reacquired chunk");
    manager.setForced(active, false);
    manager.setForced(reacquired, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(active.x, active.z) &&
                     !world.isChunkLoaded(reacquired.x, reacquired.z);
            }),
            "reacquired-demand fixture unloads");
    require(manager.close().complete,
            "reacquired-demand shutdown completes");
  }
}

void testCooldownPromotionConsumesCandidateBudget() {
  auto store = std::make_shared<FakeStore>();
  const std::vector<ChunkTicketPos> positions{{110, 0}, {120, 0}, {130, 0}};
  for (const ChunkTicketPos &position : positions)
    store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 8;
    limits.maxSubmissions = 3;
    limits.maxInFlightSaves = 3;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(50);
    manager.setAutosaveLimitsForTest(limits);
    for (const ChunkTicketPos &position : positions)
      manager.setForced(position, true);
    require(waitManager(manager, [&] {
              for (const ChunkTicketPos &position : positions)
                if (!world.isChunkLoaded(position.x, position.z))
                  return false;
              return true;
            }),
            "cooldown fixture installs all chunks");
    for (const ChunkTicketPos &position : positions)
      require(manager.setBlock({position.x * 16, 0, 0},
                               Blocks::AIR.defaultState()),
              "cooldown v1 edit succeeds");

    store->setBlockWrites(true);
    manager.serverTick(true, true);
    store->waitForWrite(1);
    for (const ChunkTicketPos &position : positions)
      require(manager.setBlock({position.x * 16, 0, 0},
                               Blocks::GLASS.defaultState()),
              "cooldown v2 edit lands while v1 writes are blocked");
    store->releaseWrites();
    require(waitManager(manager, [&] {
              if (store->successes() != 3)
                return false;
              for (const ChunkTicketPos &position : positions) {
                const auto status = manager.lifecycleStatus(position);
                if (!status || status->state != ChunkResidencyState::Resident ||
                    status->dirtyVersion <= status->savedDirtyVersion)
                  return false;
              }
              return true;
            }),
            "three newer versions enter success cooldown");
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    limits.maxCandidateChecks = 1;
    limits.maxSubmissions = 1;
    manager.setAutosaveLimitsForTest(limits);
    manager.serverTick(true, true);
    const AutosaveStats promotion = manager.lastAutosaveStats();
    std::cout << "autosave cooldown checks=" << promotion.candidateChecks
              << " submissions=" << promotion.submissions
              << " queued=" << promotion.queued << std::endl;
    require(promotion.candidateChecks == 1 &&
                promotion.submissions == 1 && promotion.queued == 2,
            "expired cooldown promotions consume exactly the remaining budget");
    require(manager.close().complete,
            "cooldown fixture closes with held dirty chunks");
  }
}

void testRepeatedFailurePatchIsDeduplicatedByCell() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{140, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 4;
    limits.maxSubmissions = 1;
    limits.maxInFlightSaves = 1;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "repeated-failure fixture reaches Resident");
    const Pos target{position.x * 16, 0, 0};
    store->failNextWrites(3);
    for (int attempt = 0; attempt < 3; ++attempt) {
      require(manager.setBlock(
                  target, attempt == 1 ? Blocks::AIR.defaultState()
                                       : Blocks::GLASS.defaultState()),
              "repeated-failure edit succeeds");
      require(manager.pendingEditEntriesForTest(position) == 1 &&
                  manager.pendingEditPayloadBytesForTest(position) ==
                      WorldChunkManager::compactEditBytes,
              "pending same-cell edits stay one compact eight-byte record");
      manager.serverTick(true, true);
      store->waitForWrite(attempt + 1);
      require(waitManager(manager, [&] {
                const auto status = manager.lifecycleStatus(position);
                return status &&
                       status->failure == ChunkLoadFailure::Io &&
                       status->dirtyVersion > status->savedDirtyVersion;
              }),
              "repeated failure leaves the chunk resident and dirty");
      require(manager.retainedPatchEntriesForTest(position) == 1 &&
                  manager.retainedPatchPayloadBytesForTest(position) ==
                      WorldChunkManager::compactEditBytes,
              "one repeatedly edited cell keeps one compact retained record");
    }
    std::cout << "autosave deduplicated retained="
              << manager.retainedPatchEntriesForTest(position) << std::endl;

    require(manager.setBlock(target, Blocks::AIR.defaultState()),
            "final deduplicated edit succeeds");
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && store->successes() == 1 &&
                     status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "deduplicated patch reaches disk");
    auto wrote = store->writes();
    require(wrote.size() == 4,
            "three failed and one successful write were captured");
    DecodedChunk saved = decodeChunkNbt(wrote.back());
    const auto &states = *saved.sections[0].blockStates;
    require(states.palette[states.cell(0)].name == "minecraft:air",
            "latest value in the deduplicated cell wins");
    require(manager.close().complete,
            "deduplicated-patch shutdown completes");
  }
}

void testCompactEditTransferAndTiming() {
  static_assert(WorldChunkManager::compactEditBytes == 8,
                "manager save records must stay eight bytes");

  auto negativeStore = std::make_shared<FakeStore>();
  const ChunkTicketPos negativePosition{140, -3};
  negativeStore->setPayload(
      negativePosition,
      makeChunkNbt(negativePosition, DynamicKind::None, -4));
  World negativeWorld(-64, 320);
  {
    WorldChunkManager manager(negativeWorld, negativeStore, 2, 10,
                              knownEmptyEntityData);
    AutosaveLimits immediate;
    immediate.cooldown = std::chrono::milliseconds(0);
    immediate.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(immediate);
    manager.setForced(negativePosition, true);
    require(waitManager(manager, [&] {
              return negativeWorld.isChunkLoaded(negativePosition.x,
                                                  negativePosition.z);
            }),
            "negative-section compact fixture installs");
    const Pos target{negativePosition.x * 16, -64,
                     negativePosition.z * 16};
    manager.rawSetBlock(target, Blocks::GLASS.defaultState());
    manager.rawSetBlock(target, Blocks::AIR.defaultState());
    require(manager.pendingEditEntriesForTest(negativePosition) == 1 &&
                manager.pendingEditPayloadBytesForTest(negativePosition) == 8,
            "same-cell negative-section edit keeps one compact record");
    manager.setForced(negativePosition, false);
    require(waitManager(manager, [&] {
              return !negativeWorld.isChunkLoaded(negativePosition.x,
                                                   negativePosition.z);
            }),
            "negative-section compact fixture saves and unloads");
    DecodedChunk saved = decodeChunkNbt(negativeStore->writes().back());
    const auto section = std::find_if(
        saved.sections.begin(), saved.sections.end(),
        [](const Section &value) { return value.y == -4; });
    require(saved.status == DecodeStatus::Ok && section != saved.sections.end() &&
                section->blockStates->palette[section->blockStates->cell(0)]
                        .name == "minecraft:air",
            "negative section/local key reaches the correct saved cell");
    require(manager.close().complete,
            "negative-section compact fixture closes");
  }

  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{150, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z);
            }),
            "compact timing fixture installs");
    const auto edit = [&](int x, int y, int z, const BlockState &state) {
      manager.rawSetBlock(
          {position.x * 16 + x, y, position.z * 16 + z}, state);
    };

    const auto sparseStart = std::chrono::steady_clock::now();
    for (int i = 0; i < 64; ++i)
      edit(i & 15, (i >> 4) & 3, i & 15,
           (i & 1) ? Blocks::GLASS.defaultState()
                   : Blocks::AIR.defaultState());
    const auto sparseEnd = std::chrono::steady_clock::now();

    const auto denseStart = std::chrono::steady_clock::now();
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          edit(x, y, z,
               ((x + y + z) & 1) ? Blocks::GLASS.defaultState()
                                  : Blocks::AIR.defaultState());
    const auto denseEnd = std::chrono::steady_clock::now();

    const auto sparseMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(sparseEnd -
                                                               sparseStart)
            .count();
    const auto denseMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(denseEnd -
                                                              denseStart)
            .count();
    std::cout << "compact_edits record_bytes="
              << WorldChunkManager::compactEditBytes
              << " sparse_calls=64 sparse_us=" << sparseMicros
              << " dense_calls=4096 dense_us=" << denseMicros
              << " pending_bytes="
              << manager.pendingEditPayloadBytesForTest(position)
              << std::endl;
    require(manager.pendingEditEntriesForTest(position) == 4096 &&
                manager.pendingEditPayloadBytesForTest(position) == 32768,
            "dense section remains exactly 4096 eight-byte pending records");

    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "dense compact transfer saves and unloads");
    const auto beforeIdle = store->writeCallCount();
    const auto idleCandidates = manager.lastPumpSaveStatsForTest();
    manager.pump();
    require(store->writeCallCount() == beforeIdle,
            "idle compact-edit manager performs no save work");
    const auto afterIdle = manager.lastPumpSaveStatsForTest();
    require(beforeIdle == store->writeCallCount() &&
                afterIdle.candidateChecks == idleCandidates.candidateChecks,
            "idle compact-edit manager visits no additional save candidates");
    require(manager.close().complete, "compact timing fixture closes");
  }
}

void testRetainedCompactMerge() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{180, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z);
            }),
            "compact merge fixture installs");
    const Pos first{position.x * 16, 0, 0};
    const Pos second{position.x * 16 + 1, 0, 0};
    manager.rawSetBlock(first, Blocks::AIR.defaultState());
    manager.rawSetBlock(second, Blocks::GLASS.defaultState());
    store->failNextWrites(1);
    manager.serverTick(true, true);
    store->waitForWrite(1);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && status->failure == ChunkLoadFailure::Io;
            }),
            "compact merge fixture reaches failed retained version");
    require(manager.retainedPatchEntriesForTest(position) == 2 &&
                manager.retainedPatchPayloadBytesForTest(position) == 16,
            "two distinct failed cells retain two compact records");
    manager.rawSetBlock(first, Blocks::STONE.defaultState());
    manager.rawSetBlock(second, Blocks::AIR.defaultState());
    manager.rawSetBlock({position.x * 16 + 2, 0, 0},
                        Blocks::GLASS.defaultState());
    require(manager.pendingEditEntriesForTest(position) == 3 &&
                manager.pendingEditPayloadBytesForTest(position) == 24,
            "newer pending changes are compact and cell-deduplicated");
    require(manager.retrySave(position), "compact retained merge retries");
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && store->successes() == 1 &&
                     status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "compact retained merge reaches disk");
    DecodedChunk saved = decodeChunkNbt(store->writes().back());
    const auto &states = *saved.sections[0].blockStates;
    require(states.palette[states.cell(0)].name == "minecraft:stone" &&
                states.palette[states.cell(1)].name == "minecraft:air" &&
                states.palette[states.cell(2)].name == "minecraft:glass",
            "latest edits win while old retained-only delta is preserved");
    require(manager.close().complete, "compact merge fixture closes");
  }

}

void testRetainedCompactUnknownStateBlocksSave() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{190, 0};
  store->setPayload(position, makeChunkNbt(position));
  World world(-64, 320);
  ChunkResidentCallbacks installTemplate{
      [](LifecycleLoadedChunk &&) { return true; },
      [](const ChunkTicketPos &, std::uint64_t) { return true; },
      {}};
  ChunkLifecycleController controller(store, installTemplate);
  require(controller.updateDemand(position, ticket_levels::FullChunkLevel),
          "unknown compact state loads its template");
  bool resident = false;
  const auto loadDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < loadDeadline && !resident) {
    (void)controller.drain();
    const auto status = controller.status(position);
    resident = status && status->state == ChunkResidencyState::Resident;
    if (!resident)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(resident, "unknown compact state template reaches Resident");
  auto unknown = std::make_shared<std::vector<terrain::CompactBlockEdit>>();
  unknown->push_back({0, 0, std::numeric_limits<BlockState::Id>::max()});
  ChunkLifecycleController::ChunkSavePayload payload;
  payload.edits = std::move(unknown);
  const std::uint64_t version = controller.status(position)->epoch;
  require(controller.requestSavePayload(position, version,
                                       std::move(payload)),
          "unknown compact state is accepted as an immutable payload");
  bool failed = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline && !failed) {
    (void)controller.drain();
    const auto status = controller.status(position);
    failed = status && status->failure == ChunkLoadFailure::Io &&
             status->dirtyVersion == version && status->savedDirtyVersion == 0;
    if (!failed)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  require(failed,
          "unknown compact state rejects write and remains dirty, not air");
  require(store->writeCallCount() == 0,
          "unknown compact state never reaches region write");
  controller.close();
}

void testFrozenPumpExpiresNoDemandCooldownAndFailure() {
  const ChunkTicketPos successPosition{160, 0};
  const ChunkTicketPos failurePosition{170, 0};
  auto store = std::make_shared<FakeStore>();
  store->setPayload(successPosition, makeChunkNbt(successPosition));
  store->setPayload(failurePosition, makeChunkNbt(failurePosition));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 4;
    limits.maxSubmissions = 1;
    limits.maxInFlightSaves = 1;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(30);
    limits.failureBackoff = std::chrono::milliseconds(30);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(successPosition, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(successPosition.x,
                                         successPosition.z);
            }),
            "frozen cooldown fixture reaches Resident");
    const Pos target{successPosition.x * 16, 0, 0};
    require(manager.setBlock(target, Blocks::AIR.defaultState()),
            "frozen cooldown v1 edit succeeds");
    store->setBlockWrites(true);
    manager.serverTick(true, true);
    store->waitForWrite(1);
    manager.setForced(successPosition, false);
    require(manager.setBlock(target, Blocks::GLASS.defaultState()),
            "frozen cooldown v2 edit lands while v1 is blocked");
    store->releaseWrites();
    require(waitManager(manager, [&] {
              return store->successes() == 1;
            }),
            "frozen cooldown v1 completes");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    std::size_t frozenChecks = 0;
    for (int i = 0; i < 100 &&
                    world.isChunkLoaded(successPosition.x,
                                        successPosition.z);
         ++i) {
      manager.pump();
      frozenChecks = std::max(
          frozenChecks, manager.lastPumpSaveStatsForTest().candidateChecks);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!world.isChunkLoaded(successPosition.x, successPosition.z) &&
                store->successes() == 2,
            "frozen pump saves and evicts a no-demand cooldown");
    require(store->writeCallCount() == 2,
            "frozen cooldown path performs exactly two accepted snapshots");
    require(manager.lastPumpSaveStatsForTest().candidateChecks <= 4,
            "frozen cooldown work remains bounded per pump");

    manager.setForced(failurePosition, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(failurePosition.x,
                                         failurePosition.z);
            }),
            "frozen failure fixture reaches Resident");
    const Pos failureTarget{failurePosition.x * 16, 0, 0};
    require(manager.setBlock(failureTarget, Blocks::AIR.defaultState()),
            "frozen failure v1 edit succeeds");
    store->failNextWrites(1);
    store->setBlockWrites(true);
    manager.serverTick(true, true);
    store->waitForWrite(2);
    manager.setForced(failurePosition, false);
    require(manager.setBlock(failureTarget, Blocks::GLASS.defaultState()),
            "frozen failure v2 edit lands while v1 is blocked");
    store->releaseWrites();
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(failurePosition);
              return status && status->failure == ChunkLoadFailure::Io &&
                     store->writeCallCount() == 3;
            }),
            "frozen failure leaves a no-demand dirty chunk");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    for (int i = 0;
         i < 100 &&
         world.isChunkLoaded(failurePosition.x, failurePosition.z);
         ++i) {
      manager.pump();
      frozenChecks = std::max(
          frozenChecks, manager.lastPumpSaveStatsForTest().candidateChecks);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!world.isChunkLoaded(failurePosition.x, failurePosition.z) &&
                store->successes() == 3,
            "frozen pump retries failed no-demand work after backoff");
    std::cout << "autosave frozen-pump max-checks=" << frozenChecks
              << " successes=" << store->successes() << std::endl;
    require(frozenChecks <= 4,
            "frozen cooldown and failure advancement never exceed pump budget");
    require(manager.close().complete,
            "frozen no-demand cooldown/failure fixture closes");
  }
}

void testDynamicBlockerBacklogRefillsWithinBudget() {
  auto store = std::make_shared<FakeStore>();
  std::vector<ChunkTicketPos> positions;
  for (int x = 180; x < 185; ++x) {
    const ChunkTicketPos position{x, 0};
    positions.push_back(position);
    store->setPayload(position, makeChunkNbt(position));
  }
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 4;
    limits.maxSubmissions = 1;
    limits.maxInFlightSaves = 1;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    std::vector<std::shared_ptr<PrimedTnt>> blockers;
    for (const ChunkTicketPos &position : positions) {
      manager.setForced(position, true);
      blockers.push_back(nullptr);
    }
    require(waitManager(manager, [&] {
              for (const ChunkTicketPos &position : positions)
                if (!world.isChunkLoaded(position.x, position.z))
                  return false;
              return true;
            }),
            "dynamic-backlog fixture installs all chunks");
    for (std::size_t i = 0; i < positions.size(); ++i) {
      const ChunkTicketPos &position = positions[i];
      require(manager.setBlock({position.x * 16, 0, 0},
                               Blocks::AIR.defaultState()),
              "dynamic-backlog dirty edit succeeds");
      blockers[i] = std::make_shared<PrimedTnt>(
          world, Vec3{position.x * 16 + 8.5, 0.5, 8.5});
      world.addEntity(blockers[i]);
    }
    for (const ChunkTicketPos &position : positions)
      manager.setForced(position, false);
    for (int i = 0; i < 20; ++i)
      manager.pump();
    require(store->writeCallCount() == 0,
            "a dynamic-blocker backlog performs no writes");
    for (const auto &blocker : blockers)
      world.removeEntity(blocker);
    world.tick();

    std::size_t maximumChecks = 0;
    for (int i = 0; i < 200; ++i) {
      manager.pump();
      maximumChecks = std::max(
          maximumChecks,
          manager.lastPumpSaveStatsForTest().candidateChecks);
      bool loaded = false;
      for (const ChunkTicketPos &position : positions)
        loaded = loaded || world.isChunkLoaded(position.x, position.z);
      if (!loaded)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool anyLoaded = false;
    for (const ChunkTicketPos &position : positions)
      anyLoaded = anyLoaded || world.isChunkLoaded(position.x, position.z);
    std::cout << "autosave dynamic-backlog max-checks=" << maximumChecks
              << " successes=" << store->successes() << std::endl;
    require(!anyLoaded &&
                store->successes() == static_cast<int>(positions.size()) &&
                maximumChecks <= 4,
            "dynamic retry backlog drains within the bounded pump budget");
    require(manager.close().complete,
            "dynamic-backlog fixture closes");
  }
}

void testInjectedStorePistonProgressMarksDirty() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{35, 0};
  const Pos pistonPos{position.x * 16, 0, position.z * 16};
  nbt::RootTag root = decodeChunkNbt(makeChunkNbt(position)).root;
  const BlockState moving =
      Blocks::MOVING_PISTON.defaultState().with("facing", "east");
  const BlockEdit movingEdit{0, 0, moving};
  require(patchBlockStates(root, {&movingEdit, 1}).edited,
          "injected piston fixture terrain patches");
  const MovingPistonEntity piston{
      pistonPos.x, pistonPos.y, pistonPos.z, Blocks::STONE.defaultState(), 5,
      0.0f, true, false};
  std::string patchError;
  require(patchMovingPistonEntities(root, {&piston, 1}, patchError),
          patchError);
  store->setPayload(position, encodeChunkNbt(root));

  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 2, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     world.pistonEntities.contains(pistonPos);
            }),
            "injected-store moving piston installs");
    const auto before = manager.lifecycleStatus(position);
    auto entity = world.pistonEntities.at(pistonPos);
    entity->progress = 0.25f;
    entity->progressO = 0.25f;
    world.setPistonEntity(pistonPos, entity);
    const auto after = manager.lifecycleStatus(position);
    require(before && after && after->dirtyVersion > before->dirtyVersion &&
                world.getBlock(pistonPos).name() == "minecraft:moving_piston" &&
                world.chunkTickCount(position.x, position.z) == 0,
            "BE-only change dirties the injected-store managed chunk");
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && store->successes() == 1 &&
                     status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "BE-only dirty version saves through RawChunkStore manager");
    const auto wrote = store->writes();
    DecodedChunk saved = decodeChunkNbt(wrote.back());
    require(wrote.size() == 1 && saved.movingPistons.size() == 1 &&
                saved.movingPistons[0].progressO == 0.25f,
            "BE-only save carries the updated progressO");
    std::cout << "moving_piston injected-store progressO="
              << saved.movingPistons[0].progressO << std::endl;
    require(manager.close().complete,
            "injected-store piston manager closes");
  }
  require(!world.pistonEntityMutationObserver,
          "manager close detaches piston mutation observer");
}

void testRealMovingPistonLifecycle(
    const std::filesystem::path &sourceRegion,
    const std::filesystem::path &scratch) {
  const std::filesystem::path region = scratch / "moving-piston-region";
  std::filesystem::create_directories(region);
  for (const auto &entry : std::filesystem::directory_iterator(sourceRegion))
    if (entry.is_regular_file() && entry.path().extension() == ".mca")
      std::filesystem::copy_file(
          entry.path(), region / entry.path().filename(),
          std::filesystem::copy_options::overwrite_existing);

  const ChunkTicketPos chunk{-1, -1};
  const Pos head{-13, -58, -14};
  const Pos moved{-12, -58, -14};
  World world(-64, 320);
  {
    WorldChunkManager manager(world, region, 2, 10, knownEmptyEntityData);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 8;
    limits.maxSubmissions = 2;
    limits.maxInFlightSaves = 2;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);

    const Ticket loadingOnly(ticket_types::PlayerLoading,
                             TicketSourceManager::PlayerLoadingLevel);
    const std::array<TicketOperation, 1> addLoading{
        TicketOperation::add(chunk, loadingOnly)};
    TicketSystemUpdate loadOnly =
        manager.ticketSources().tickets().applyBatch(addLoading);
    manager.applyTicketUpdate(loadOnly);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(chunk.x, chunk.z) &&
                     world.pistonEntities.size() == 2;
            }),
            "official moving-piston chunk installs compact block entities");
    require(world.pistonEntities.contains(head) &&
                world.pistonEntities.contains(moved) &&
                world.getBlock(head).name() == "minecraft:moving_piston" &&
                world.getBlock(moved).name() == "minecraft:moving_piston",
            "official moving-piston terrain and BE positions pair exactly");
    const ChunkTickReadiness noSimulation = manager.tickReadiness(chunk);
    require(!noSimulation.blockTicking,
            "loading-only chunk lacks simulation-neighborhood readiness; "
            "loading=" + std::to_string(noSimulation.loadingLevel) +
                " simulation=" + std::to_string(noSimulation.simulationLevel));
    manager.serverTick(true, true);
    require(world.pistonEntities.at(head)->progress == 0.0f &&
                world.pistonEntities.at(moved)->progress == 0.0f,
            "moving-piston ticker does not advance without simulation readiness");

    const std::array<TicketOperation, 1> removeLoadingOperation{
        TicketOperation::remove(chunk, loadingOnly)};
    TicketSystemUpdate removeLoading =
        manager.ticketSources().tickets().applyBatch(removeLoadingOperation);
    manager.applyTicketUpdate(removeLoading);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(chunk.x, chunk.z);
            }),
            "unmodified official piston fixture evicts cleanly");
    manager.setPlayer(1, true, chunk);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(chunk.x, chunk.z) &&
                     manager.tickReadiness(chunk).blockTicking;
            }),
            "simulation ticket installs the ready neighborhood");
    manager.serverTick(true, true);
    require(world.pistonEntities.contains(head) &&
                world.pistonEntities.contains(moved) &&
                world.pistonEntities.at(head)->progress == 0.5f &&
                world.pistonEntities.at(moved)->progress == 0.5f,
            "ready piston ticker advances both active entities in order");
    const bool midSaved = waitManager(manager, [&] {
      const auto status = manager.lifecycleStatus(chunk);
      return status && status->dirtyVersion <= status->savedDirtyVersion;
    });
    const auto midStatus = manager.lifecycleStatus(chunk);
    require(midSaved,
            "mid-motion piston snapshot saves: state=" +
                std::to_string(midStatus ? static_cast<int>(midStatus->state)
                                         : -1) +
                " dirty=" +
                std::to_string(midStatus ? midStatus->dirtyVersion : 0) +
                " saved=" +
                std::to_string(midStatus ? midStatus->savedDirtyVersion : 0) +
                " detail=" +
                (midStatus ? midStatus->detail : std::string{"missing"}));
    manager.removePlayer(1);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(chunk.x, chunk.z);
            }),
            "mid-motion piston chunk unloads after saving");

    DecodedChunk mid = decodeChunkAt(region, chunk.x, chunk.z);
    require(mid.status == DecodeStatus::Ok &&
                !mid.hasUnsupportedBlockEntities &&
                mid.movingPistons.size() == 2 &&
                mid.movingPistons[0].progressO == 0.0f &&
                mid.movingPistons[1].progressO == 0.0f,
            "disk contains both moving piston records at saved progressO");
    const auto midCopy = scratch / "moving-piston-mid";
    std::filesystem::create_directories(midCopy);
    std::filesystem::copy_file(
        region / "r.-1.-1.mca", midCopy / "r.-1.-1.mca",
        std::filesystem::copy_options::overwrite_existing);

    manager.setForced(chunk, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(chunk.x, chunk.z) &&
                     world.pistonEntities.size() == 2 &&
                     world.pistonEntities.at(head)->progress == 0.0f;
            }),
            "restart-equivalent load restores progress from progressO");
    manager.setPlayer(1, true, chunk);
    require(waitManager(manager, [&] {
              return manager.tickReadiness(chunk).blockTicking;
            }),
            "reloaded piston regains simulation readiness");
    manager.serverTick(true, true);
    require(world.pistonEntities.at(head)->progress == 0.5f,
            "restored piston advances its first half-step");
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(chunk);
              return status && status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "restored mid-step saves");
    manager.serverTick(true, true);
    require(world.pistonEntities.contains(head) &&
                world.pistonEntities.at(head)->progress == 1.0f &&
                world.pistonEntities.at(moved)->progress == 1.0f,
            "restored piston reaches progress 1 before completion tick");
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(chunk);
              return status && status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "progress-1 piston snapshot saves");
    manager.serverTick(true, true);
    require(!world.pistonEntities.contains(head) &&
                !world.pistonEntities.contains(moved) &&
                world.getBlock(head).name() == "minecraft:piston_head" &&
                world.getBlock(moved).name() == "minecraft:stone",
            "next restored piston tick completes into final blocks; "
            "remaining=" + std::to_string(world.pistonEntities.size()) +
                " head=" + world.getBlock(head).name() +
                " moved=" + world.getBlock(moved).name());
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(chunk);
              return status && status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "completed piston removal saves in the same terrain version");
    manager.removePlayer(1);
    manager.setForced(chunk, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(chunk.x, chunk.z);
            }),
            "completed piston chunk unloads");
    DecodedChunk completed = decodeChunkAt(region, chunk.x, chunk.z);
    require(completed.status == DecodeStatus::Ok &&
                completed.movingPistons.empty() &&
                !completed.hasUnsupportedBlockEntities,
            "completed disk chunk has no stale moving-piston block entities");
    std::cout << "moving_piston mid_records=" << mid.movingPistons.size()
              << " completed_records=" << completed.movingPistons.size()
              << std::endl;
    require(manager.close().complete,
            "moving-piston lifecycle shutdown completes");
  }
}

entity_codec::ItemEntityData itemAt(const ChunkTicketPos &chunk,
                                        std::array<std::int32_t, 4> uuid,
                                        double x, double z) {
  entity_codec::ItemEntityData item;
  item.uuid = uuid;
  item.pos = {x, 1.5, z};
  item.motion = {0.0, 0.0, 0.0};
  item.rotation = {0.0f, 0.0f};
  item.age = 17;
  item.pickupDelay = 200;
  item.health = 5;
  item.itemId = "minecraft:stone";
  item.itemCount = 3;
  (void)chunk;
  return item;
}

void testLifecycleHistoryReclaimed() {
  auto terrain = std::make_shared<FakeStore>();
  const ChunkTicketPos center{1600, -1600};
  terrain->setPayload(center, makeChunkNbt(center));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData);
    for (int i = 0; i < 96; ++i) {
      const ChunkTicketPos position{1800 + i, -1800};
      manager.setForced(position, false);
    }
    require(manager.lifecycleCoordinateCountForTest() == 0,
            "historical no-demand changes create no lifecycle coordinates");
    manager.setForced(center, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(center.x, center.z);
            }),
            "history fixture reaches Resident");
    require(manager.lifecycleCoordinateCountForTest() == 1,
            "resident history has one coordinate token");
    const std::uint64_t firstEpoch =
        manager.lifecycleStatus(center)->epoch;
    manager.setForced(center, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(center.x, center.z);
            }),
            "history fixture actually unloads");
    require(manager.lifecycleCoordinateCountForTest() == 0,
            "real unload releases its lifecycle coordinate token");
    manager.setForced(center, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(center.x, center.z);
            }),
            "same coordinate demand reloads");
    require(manager.lifecycleCoordinateCountForTest() == 1 &&
                manager.lifecycleStatus(center) &&
                manager.lifecycleStatus(center)->epoch > firstEpoch,
            "reload starts a newer lifecycle generation");
    manager.setForced(center, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(center.x, center.z) &&
                     manager.lifecycleCoordinateCountForTest() == 0;
            }),
            "second unload releases the new lifecycle generation");
    require(manager.close().complete, "lifecycle history manager closes");
    std::cout << "lifecycle_history unload_clear=1 no_demand_no_entry=1"
              << std::endl;
  }
}

void testTntAndItemShareEntityStoreLifecycle() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos position{-103, 71};
  terrain->setPayload(position, makeChunkNbt(position));
  const auto item = itemAt(position, {51, 52, 53, 54},
                           position.x * 16 + 2.0,
                           position.z * 16 + 2.0);
  entity_codec::TntEntityData tnt;
  tnt.uuid = {61, 62, 63, 64};
  tnt.pos = {position.x * 16 + 3.0, 2.0, position.z * 16 + 3.0};
  tnt.motion = {0.0, 0.0, 0.0};
  tnt.rotation = {0.0f, 0.0f};
  tnt.fuse = 47;
  tnt.explosionPower = 6.5f;
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(position.x, position.z);
  const std::array mixedRecords{entity_codec::EntityRecord{tnt},
                                 entity_codec::EntityRecord{item}};
  require(entity_codec::patchEntitySequence(root, mixedRecords, error), error);
  entities->setPayload(position, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(position, true);
    const auto rngBefore = [&] {
      auto probe = world.random;
      return probe.nextLong();
    }();
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     world.entities.size() == 2;
            }),
            "item and tnt install from one compact record");
    const auto rngAfter = [&] {
      auto probe = world.random;
      return probe.nextLong();
    }();
    require(rngBefore == rngAfter,
            "managed tnt load does not consume World random");
    require(world.chunkEntityOrderForTest(position.x, position.z) ==
                std::vector<std::array<std::int32_t, 4>>{
                    tnt.uuid, item.uuid},
            "mixed install keeps TNT before the following item");
    require(manager.entityPositionLedgerSizeForTest() == 2,
            "mixed install indexes both entity UUIDs");
    PrimedTnt *loadedTnt = nullptr;
    for (const auto &entity : world.entities) {
      if (auto *candidate = dynamic_cast<PrimedTnt *>(entity.get()))
        loadedTnt = candidate;
    }
    require(loadedTnt && loadedTnt->uuid == tnt.uuid &&
                loadedTnt->fuse == 47 &&
                loadedTnt->explosionPower == 6.5f,
            "managed tnt retains fuse and UUID");
    loadedTnt->fuse = 18;
    loadedTnt->setDeltaMovement(0.25, -0.5, 0.125);
    // This follows the same public managed movement hook used by a real tick;
    // the TNT record itself does not have an item-specific age counter.
    const Vec3 tntPosition = loadedTnt->position;
    world.managedEntityMutationObserver(tntPosition, tntPosition,
                                        loadedTnt->uuid, false);
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "mixed item/tnt snapshot saves");
    const auto writes = entities->writes();
    require(!writes.empty(), "mixed entity payload was written");
    const auto saved = entity_codec::decodeEntityChunk(writes.back());
    require(saved.status == entity_codec::DecodeStatus::Ok &&
                saved.records.size() == 2 &&
                std::holds_alternative<entity_codec::TntEntityData>(
                    saved.records[0].payload) &&
                entity_codec::recordUuid(saved.records[0]) == tnt.uuid &&
                std::holds_alternative<entity_codec::ItemEntityData>(
                    saved.records[1].payload) &&
                entity_codec::recordUuid(saved.records[1]) == item.uuid &&
                tntsOf(saved)[0].fuse == 18 &&
                tntsOf(saved)[0].motion ==
                    std::array<double, 3>{.25, -.5, .125},
            "mixed payload preserves order, item and live tnt state");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "mixed entity chunk unloads");
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     world.entities.size() == 2;
            }),
            "mixed entity chunk reloads");
    PrimedTnt *reloadedTnt = nullptr;
    for (const auto &entity : world.entities) {
      if (auto *candidate = dynamic_cast<PrimedTnt *>(entity.get()))
        reloadedTnt = candidate;
    }
    require(reloadedTnt && reloadedTnt->uuid == tnt.uuid &&
                reloadedTnt->fuse == 18 &&
                reloadedTnt->deltaMovement.x == .25 &&
                reloadedTnt->deltaMovement.y == -.5 &&
                reloadedTnt->deltaMovement.z == .125,
            "reloaded tnt retains UUID, fuse and motion");
    auto tntShared = std::find_if(
        world.entities.begin(), world.entities.end(), [](const auto &entity) {
          return dynamic_cast<PrimedTnt *>(entity.get()) != nullptr;
        });
    require(tntShared != world.entities.end(),
            "reloaded tnt remains live");
    (*tntShared)->discard();
    world.tickEntities();
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto data = entities->payload(position);
              if (!data)
                return false;
              const auto decoded = entity_codec::decodeEntityChunk(*data);
              return decoded.status == entity_codec::DecodeStatus::Ok &&
                     tntsOf(decoded).empty();
            }),
            "tnt disappearance persists no tnt record");
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z);
            }),
            "tnt disappearance/unload completes");
    require(manager.entityPositionLedgerSizeForTest() == 0,
            "mixed TNT/item unload clears both UUID ledger entries");
    require(manager.close().complete, "mixed item/tnt manager closes");
    std::cout << "tnt_entity mixed_install=1 rng_unchanged=1 saved_fuse=18 "
               "reload=1 disappearance=1 order=1 ledger_clear=1"
              << std::endl;
  }
}

void testFallingBlockEntityStoreLifecycle() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos target{-113, 77};
  const ChunkTicketPos neighbor{-110, 77};
  terrain->setPayload(target, makeFallingChunkNbt(target));
  terrain->setPayload(neighbor, makeChunkNbt(neighbor));

  entity_codec::ItemEntityData item = itemAt(
      target, {71, 72, 73, 74}, target.x * 16 + 2.0, target.z * 16 + 2.0);
  entity_codec::FallingBlockEntityData falling;
  falling.uuid = {81, 82, 83, 84};
  falling.pos = {target.x * 16 + 4.5, 20.0, target.z * 16 + 4.5};
  falling.motion = {0.125, -0.04, 0.0625};
  falling.rotation = {0.0f, 0.0f};
  falling.time = 3;
  falling.dropItem = false;
  falling.cancelDrop = false;
  entity_codec::TntEntityData tnt;
  tnt.uuid = {91, 92, 93, 94};
  tnt.pos = {target.x * 16 + 6.5, 2.0, target.z * 16 + 6.5};
  tnt.motion = {0.0, 0.0, 0.0};
  tnt.rotation = {0.0f, 0.0f};
  tnt.fuse = 1000;
  nbt::RootTag root = entity_codec::emptyEntityRoot(target.x, target.z);
  const std::array records{entity_codec::EntityRecord{item},
                           entity_codec::EntityRecord{falling},
                           entity_codec::EntityRecord{tnt}};
  std::string error;
  require(entity_codec::patchEntitySequence(root, records, error), error);
  entities->setPayload(target, entity_codec::encodeEntityChunk(root));

  entity_codec::ItemEntityData resident =
      itemAt(neighbor, {101, 102, 103, 104}, neighbor.x * 16 + 1.0,
             neighbor.z * 16 + 1.0);
  nbt::RootTag residentRoot =
      entity_codec::emptyEntityRoot(neighbor.x, neighbor.z);
  const std::array residentRecord{entity_codec::EntityRecord{resident}};
  require(entity_codec::patchEntitySequence(residentRoot, residentRecord,
                                             error),
          error);
  entities->setPayload(neighbor,
                       entity_codec::encodeEntityChunk(residentRoot));
  seedNeighborhood(terrain, target, 3);
  terrain->setPayload(target, makeFallingChunkNbt(target));

  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 32;
    limits.maxSubmissions = 4;
    limits.maxInFlightSaves = 4;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(target, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(target.x, target.z) &&
                     world.entities.size() >= 3;
            }),
            "interleaved item/falling/tnt records install in one chunk");
    const FallingBlockEntity *initialFalling = nullptr;
    for (const auto &entity : world.entities)
      if (const auto *candidate =
              dynamic_cast<const FallingBlockEntity *>(entity.get());
          candidate && candidate->uuid == falling.uuid)
        initialFalling = candidate;
    require(initialFalling &&
                initialFalling->deltaMovement.x == falling.motion[0] &&
                initialFalling->deltaMovement.y == falling.motion[1] &&
                initialFalling->deltaMovement.z == falling.motion[2] &&
                initialFalling->deltaMovement.x != 0.0 &&
                initialFalling->deltaMovement.y != 0.0 &&
                initialFalling->deltaMovement.z != 0.0,
            "initial falling block install restores all motion axes");
    manager.setForced(neighbor, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(neighbor.x, neighbor.z) &&
                     world.entities.size() >= 4;
            }),
            "neighboring resident entity chunk loads independently");
    for (int dx = -2; dx <= 2; ++dx)
      for (int dz = -2; dz <= 2; ++dz)
        if (dx != 0 || dz != 0)
          manager.setForced({target.x + dx, target.z + dz}, true);
    require(world.chunkEntityOrderForTest(target.x, target.z) ==
                std::vector<std::array<std::int32_t, 4>>{
                    item.uuid, falling.uuid, tnt.uuid},
            "manager preserves item/falling/tnt order");
    require(manager.entityPositionLedgerSizeForTest() >= 3,
            "resident entity chunks have UUID ledger entries");

    manager.setPlayer(1, true, target);
    const bool becameEntityTicking = waitManager(manager, [&] {
      return manager.tickReadiness(target).entityTicking;
    });
    const auto readiness = manager.tickReadiness(target);
    require(becameEntityTicking,
            "falling block chunk becomes entity-ticking: loading=" +
                std::to_string(readiness.loadingLevel) + " simulation=" +
                std::to_string(readiness.simulationLevel) + " ready=" +
                std::to_string(readiness.entityDataReady));
    for (int i = 0; i < 8; ++i) {
      manager.serverTick(true, true);
      manager.pump();
    }
    manager.removePlayer(1);
    bool savedInFlight = false;
    for (int i = 0; i < 100 && !savedInFlight; ++i) {
      manager.serverTick(false, true);
      manager.pump();
      manager.retrySave(target);
      const auto status = manager.lifecycleStatus(target);
      savedInFlight =
          status && status->dirtyVersion <= status->savedDirtyVersion;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto inFlightStatus = manager.lifecycleStatus(target);
    require(savedInFlight,
            "falling block in-flight state saves: dirty=" +
                std::to_string(inFlightStatus ? inFlightStatus->dirtyVersion : 0) +
                " saved=" +
                std::to_string(inFlightStatus
                                   ? inFlightStatus->savedDirtyVersion
                                   : 0) +
                " blocker=" + (manager.managedStatus(target)
                                   ? manager.managedStatus(target)->blocker
                                   : std::string{"missing"}) +
                " error=" + (manager.managedStatus(target)
                                 ? manager.managedStatus(target)->error
                                 : std::string{"missing"}) +
                " state=" +
                std::to_string(inFlightStatus
                                   ? static_cast<int>(inFlightStatus->state)
                                   : -1));

    manager.removePlayer(1);
    for (int dx = -2; dx <= 2; ++dx)
      for (int dz = -2; dz <= 2; ++dz)
        if (dx != 0 || dz != 0)
          manager.setForced({target.x + dx, target.z + dz}, false);
    manager.setForced(neighbor, true);
    manager.setForced(target, false);
    const bool targetUnloadedResidentNeighbor = waitManager(manager, [&] {
      return !world.isChunkLoaded(target.x, target.z) &&
             world.isChunkLoaded(neighbor.x, neighbor.z) &&
             manager.entityPositionLedgerSizeForTest() == 1;
    });
    require(targetUnloadedResidentNeighbor,
            "falling block unload leaves the neighboring chunk resident: "
            "target_loaded=" +
                std::to_string(world.isChunkLoaded(target.x, target.z)) +
                " neighbor_loaded=" +
                std::to_string(world.isChunkLoaded(neighbor.x, neighbor.z)) +
                " ledger=" +
                std::to_string(manager.entityPositionLedgerSizeForTest()) +
                " blocker=" +
                (manager.managedStatus(target)
                     ? manager.managedStatus(target)->blocker
                     : std::string{"missing"}) +
                " error=" +
                (manager.managedStatus(target)
                     ? manager.managedStatus(target)->error
                     : std::string{"missing"}));
    const auto midData = entities->payload(target);
    require(midData.has_value(), "falling block chunk remains on disk");
    const auto mid = entity_codec::decodeEntityChunk(*midData);
    const auto midFalling = [&] {
      for (const auto &record : mid.records)
        if (const auto *value =
                std::get_if<entity_codec::FallingBlockEntityData>(
                    &record.payload))
          return *value;
      return entity_codec::FallingBlockEntityData{};
    }();
    require(mid.status == entity_codec::DecodeStatus::Ok &&
                midFalling.uuid == falling.uuid && midFalling.time >= 4 &&
                midFalling.motion[0] != 0.0 &&
                midFalling.motion[1] != 0.0 &&
                midFalling.motion[2] != 0.0,
            "saved falling block keeps UUID, Time and all motion axes");

    manager.setForced(target, true);
    manager.setPlayer(1, true, target);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(target.x, target.z) &&
                     manager.tickReadiness(target).entityTicking &&
                     world.entities.size() == 4;
            }),
            "falling block reloads into the same resident world");
    FallingBlockEntity *reloadedFalling = nullptr;
    for (const auto &entity : world.entities)
      if (auto *candidate =
              dynamic_cast<FallingBlockEntity *>(entity.get()))
        reloadedFalling = candidate;
    require(reloadedFalling && reloadedFalling->uuid == falling.uuid &&
                reloadedFalling->deltaMovement.x == midFalling.motion[0] &&
                reloadedFalling->deltaMovement.y == midFalling.motion[1] &&
                reloadedFalling->deltaMovement.z == midFalling.motion[2] &&
                reloadedFalling->deltaMovement.x != 0.0 &&
                reloadedFalling->deltaMovement.y != 0.0 &&
                reloadedFalling->deltaMovement.z != 0.0,
            "reloaded falling block restores saved motion before its next tick");
    bool landed = false;
    for (int i = 0; i < 80 && !landed; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      for (int x = 0; x < 16 && !landed; ++x)
        for (int z = 0; z < 16 && !landed; ++z)
          landed =
              world.getBlock({target.x * 16 + x, 1, target.z * 16 + z})
                  .name() == "minecraft:sand";
    }
    require(landed, "falling sand continues after reload and lands");
    for (const auto &entity : world.entities)
      require(dynamic_cast<FallingBlockEntity *>(entity.get()) == nullptr,
              "landed falling block entity is gone");
    manager.removePlayer(1);
    bool landingSaved = false;
    for (int i = 0; i < 100 && !landingSaved; ++i) {
      manager.serverTick(false, true);
      manager.pump();
      manager.retrySave(target);
      const auto status = manager.lifecycleStatus(target);
      landingSaved =
          status && status->dirtyVersion <= status->savedDirtyVersion;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(landingSaved, "landing persists the removed falling block");
    manager.setForced(target, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(target.x, target.z) &&
                     manager.entityPositionLedgerSizeForTest() == 1;
            }),
            "landed falling block unload clears only its chunk ledger");
    const auto finalData = entities->payload(target);
    require(finalData.has_value(), "landed chunk entity data exists");
    const auto finalDecoded = entity_codec::decodeEntityChunk(*finalData);
    for (const auto &record : finalDecoded.records)
      require(!std::holds_alternative<entity_codec::FallingBlockEntityData>(
                  record.payload),
              "landed entity is not resurrected on disk");
    manager.setForced(target, false);
    manager.setForced(neighbor, false);
    for (int dx = -2; dx <= 2; ++dx)
      for (int dz = -2; dz <= 2; ++dz)
        if (dx != 0 || dz != 0)
          manager.setForced({target.x + dx, target.z + dz}, false);
    require(manager.close().complete, "falling block manager closes");
    std::cout << "falling_block mixed_order=1 unload_scoped=1 resume_land=1 "
                 "reload_no_revive=1 ledger_clear=1"
              << std::endl;
  }
}

void testUnsupportedFallingBlockDoesNotOverwrite() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos position{117, -93};
  terrain->setPayload(position, makeChunkNbt(position));
  entity_codec::FallingBlockEntityData falling;
  falling.uuid = {111, 112, 113, 114};
  falling.pos = {position.x * 16 + 2.0, 20.0,
                 position.z * 16 + 2.0};
  nbt::RootTag root = entity_codec::emptyEntityRoot(position.x, position.z);
  const std::array record{entity_codec::EntityRecord{falling}};
  std::string error;
  require(entity_codec::patchEntitySequence(root, record, error), error);
  nbt::Tag tileData;
  tileData.type = 10;
  add(root.value.compound["Entities"].list[0], "TileEntityData",
      std::move(tileData));
  const auto original = entity_codec::encodeEntityChunk(root);
  entities->setPayload(position, original);
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(position);
              return status && status->state == ChunkResidencyState::Failed;
            }),
            "unsupported TileEntityData fails the whole chunk install");
    manager.serverTick(true, true);
    manager.pump();
    require(entities->payload(position) == original,
            "unsupported falling block payload is not overwritten");
    require(!world.isChunkLoaded(position.x, position.z),
            "unsupported falling block does not create an empty chunk");
    manager.setForced(position, false);
    require(manager.close().complete,
            "unsupported falling block manager closes");
    std::cout << "falling_block tile_data=refused payload_unchanged=1"
              << std::endl;
  }
}

void testMinecartEntityStoreLifecycle() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos position{-131, 45};
  seedNeighborhood(terrain, position, 3);
  terrain->setPayload(position, makeChunkNbt(position));
  entity_codec::MinecartEntityData minecart;
  minecart.uuid = {121, 122, 123, 124};
  minecart.pos = {position.x * 16 + 4.5, 0.0625,
                  position.z * 16 + 4.5};
  minecart.motion = {0.125, 0.0, 0.0625};
  minecart.rotation = {90.0f, 0.0f};
  minecart.flipped = true;
  minecart.firstTick = false;
  nbt::RootTag root = entity_codec::emptyEntityRoot(position.x, position.z);
  const std::array record{entity_codec::EntityRecord{minecart}};
  std::string error;
  require(entity_codec::patchEntitySequence(root, record, error), error);
  entities->setPayload(position, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     world.entities.size() == 1;
            }),
            "empty minecart installs from its compact entity record");
    Minecart *loaded = nullptr;
    for (const auto &entity : world.entities)
      if (auto *candidate = dynamic_cast<Minecart *>(entity.get()))
        loaded = candidate;
    require(loaded && loaded->uuid == minecart.uuid &&
                loaded->deltaMovement.x == 0.125 && loaded->deltaMovement.y == 0.0 && loaded->deltaMovement.z == 0.0625 &&
                loaded->flipped && !loaded->firstTick,
            "minecart install restores Motion and persisted flags");
    const Pos rail{position.x * 16 + 4, 0, position.z * 16 + 4};
    require(manager.setBlock(
                rail, defaultState("rail").with("shape", "east_west"), 3),
            "minecart fixture installs an east-west rail");
    bool railSnapshotSaved = false;
    for (int i = 0; i < 120 && !railSnapshotSaved; ++i) {
      manager.serverTick(false, true);
      manager.pump();
      manager.retrySave(position);
      const auto status = manager.lifecycleStatus(position);
      railSnapshotSaved =
          status && status->dirtyVersion <= status->savedDirtyVersion;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(railSnapshotSaved,
            "minecart and rail snapshot save: " +
                (manager.lifecycleStatus(position)
                     ? "dirty=" + std::to_string(manager.lifecycleStatus(position)->dirtyVersion) +
                           " saved=" + std::to_string(manager.lifecycleStatus(position)->savedDirtyVersion)
                     : std::string{"missing"}) +
                (manager.managedStatus(position)
                     ? " blocker=" + manager.managedStatus(position)->blocker +
                           " error=" + manager.managedStatus(position)->error
                     : std::string{"missing"}));
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z) &&
                     manager.entityPositionLedgerSizeForTest() == 0;
            }),
            "minecart unload clears its chunk ledger");
    const auto saved = entities->payload(position);
    require(saved.has_value(), "minecart entity slot remains on disk");
    const auto decoded = entity_codec::decodeEntityChunk(*saved);
    const auto carts = [&] {
      std::vector<entity_codec::MinecartEntityData> result;
      for (const auto &record : decoded.records)
        if (const auto *value =
                std::get_if<entity_codec::MinecartEntityData>(
                    &record.payload))
          result.push_back(*value);
      return result;
    }();
    require(decoded.status == entity_codec::DecodeStatus::Ok &&
                carts.size() == 1 && carts[0].uuid == minecart.uuid &&
                carts[0].firstTick == minecart.firstTick &&
                carts[0].flipped && carts[0].motion[0] != 0.0 &&
                carts[0].motion[2] != 0.0,
            "minecart save keeps UUID, flags and motion");

    for (int dx = -2; dx <= 2; ++dx)
      for (int dz = -2; dz <= 2; ++dz)
        if (dx != 0 || dz != 0)
          manager.setForced({position.x + dx, position.z + dz}, true);
    manager.setForced(position, true);
    manager.setPlayer(1, true, position);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     manager.tickReadiness(position).entityTicking;
            }),
            "reloaded minecart neighborhood is entity-ready");
    Minecart *reloaded = nullptr;
    for (const auto &entity : world.entities)
      if (auto *candidate = dynamic_cast<Minecart *>(entity.get()))
        reloaded = candidate;
    require(reloaded && reloaded->deltaMovement.x != 0.0 &&
                reloaded->deltaMovement.y == 0.0 &&
                reloaded->deltaMovement.z != 0.0,
            "reloaded minecart restores motion before its next tick");
    const Vec3 before = reloaded->position;
    for (int i = 0; i < 20; ++i) {
      manager.serverTick(true, true);
      manager.pump();
    }
    require(reloaded->position.x != before.x ||
                reloaded->position.z != before.z,
            "reloaded empty minecart continues running on its rail");
    bool runSaved = false;
    for (int i = 0; i < 120 && !runSaved; ++i) {
      manager.serverTick(false, true);
      manager.pump();
      manager.retrySave(position);
      const auto status = manager.lifecycleStatus(position);
      runSaved = status && status->dirtyVersion <= status->savedDirtyVersion;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(runSaved, "minecart rail run persists before unload");
    manager.removePlayer(1);
    for (int dx = -2; dx <= 2; ++dx)
      for (int dz = -2; dz <= 2; ++dz)
        if (dx != 0 || dz != 0)
          manager.setForced({position.x + dx, position.z + dz}, false);
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z) &&
                     manager.entityPositionLedgerSizeForTest() == 0;
            }),
            "minecart final unload clears its ledger");
    require(manager.close().complete, "minecart manager closes");
    std::cout << "minecart empty_install=1 motion=1 rail_run=1 reload=1 "
                 "ledger_clear=1"
              << std::endl;
  }
}

void testInjectedItemSaveFailureKeepsResident() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos position{45, 0};
  terrain->setPayload(position, makeChunkNbt(position));
  const std::array<std::int32_t, 4> uuid{
      1, 2, 3, 9001};
  const auto item = itemAt(position, uuid, position.x * 16 + 1.25,
                           position.z * 16 + 1.75);
  nbt::RootTag entityRoot =
      entity_codec::emptyEntityRoot(position.x, position.z);
  std::string error;
  require(entity_codec::patchEntityItems(entityRoot, {&item, 1}, {}, error),
          error);
  entities->setPayload(position, entity_codec::encodeEntityChunk(entityRoot));

  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    AutosaveLimits limits;
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::seconds(10);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(position.x, position.z) &&
                     world.entities.size() == 1;
            }),
            "injected item entity installs");
    auto loaded = std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(loaded && loaded->uuid == uuid,
            "injected item preserves persisted UUID");
    loaded->age = 25;
    require(manager.setBlock({position.x * 16, 0, 0},
                             Blocks::AIR.defaultState()),
            "item save fixture has a dirty terrain version");
    entities->failNextWrites(1);
    manager.setForced(position, false);
    const bool failedResident = waitManager(manager, [&] {
      const auto status = manager.lifecycleStatus(position);
      return status && status->state == ChunkResidencyState::Resident &&
             status->failure == ChunkLoadFailure::Io &&
             status->dirtyVersion > status->savedDirtyVersion &&
             world.isChunkLoaded(position.x, position.z) &&
             world.entities.size() == 1;
    });
    const auto failedStatus = manager.lifecycleStatus(position);
    require(failedResident,
            "entity write failure keeps the dirty item chunk resident; "
            "state=" + std::to_string(failedStatus
                                          ? static_cast<int>(failedStatus->state)
                                          : -1) +
                " failure=" +
                std::to_string(failedStatus
                                   ? static_cast<int>(failedStatus->failure)
                                   : -1) +
                " dirty=" +
                std::to_string(failedStatus ? failedStatus->dirtyVersion : 0) +
                " saved=" +
                std::to_string(failedStatus ? failedStatus->savedDirtyVersion
                                           : 0) +
                " detail=" +
                (failedStatus ? failedStatus->detail : std::string{"missing"}) +
                " entity_writes=" + std::to_string(entities->writeCalls()) +
                " entity_successes=" + std::to_string(entities->successes()));
    require(entities->successes() == 0,
            "failed entity write did not claim success");
    const auto beforeRetry = manager.managedStatus(position);
    const bool retryStarted = manager.retrySave(position);
    require(retryStarted,
            "item entity save can be retried; state=" +
                std::to_string(beforeRetry
                                   ? static_cast<int>(
                                         beforeRetry->lifecycle.state)
                                   : -1) +
                " blocker=" +
                (beforeRetry ? beforeRetry->blocker : std::string{"missing"}) +
                " error=" +
                (beforeRetry ? beforeRetry->error : std::string{"missing"}));
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(position.x, position.z) &&
                     entities->successes() == 1;
            }),
            "entity retry saves both regions before eviction");
    auto writes = entities->writes();
    require(writes.size() == 1,
            "one successful entity-region write was captured");
    const auto saved = entity_codec::decodeEntityChunk(writes.back());
    require(saved.status == entity_codec::DecodeStatus::Ok &&
                itemsOf(saved).size() == 1 && itemsOf(saved)[0].uuid == uuid &&
                itemsOf(saved)[0].age == 25,
            "retried entity payload preserves UUID and advanced age");
    std::cout << "item_entity failure_retry=1 age="
              << itemsOf(saved)[0].age << std::endl;
    require(manager.close().complete,
            "injected item failure fixture closes");
  }
}

void testCrossChunkItemMigrationPersistsOnce() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{-54, -37};
  const ChunkTicketPos destination{-53, -37};
  seedNeighborhood(terrain, source, 3);
  const std::array<std::int32_t, 4> movingUuid{41, 42, 43, 44};
  const std::array<std::int32_t, 4> existingUuid{51, 52, 53, 54};
  const auto moving =
      itemAt(source, movingUuid, source.x * 16 + 15.9,
             source.z * 16 + 2.5);
  const auto existing =
      itemAt(destination, existingUuid, destination.x * 16 + 2.25,
             destination.z * 16 + 2.75);
  std::string error;
  nbt::RootTag sourceRoot =
      entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(sourceRoot, {&moving, 1}, {}, error),
          error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(sourceRoot));
  nbt::RootTag destinationRoot =
      entity_codec::emptyEntityRoot(destination.x, destination.z);
  require(entity_codec::patchEntityItems(destinationRoot, {&existing, 1},
                                         {}, error),
          error);
  entities->setPayload(destination,
                       entity_codec::encodeEntityChunk(destinationRoot));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(source, true);
    manager.setForced(destination, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     world.isChunkLoaded(destination.x, destination.z) &&
                     world.entities.size() == 2;
            }),
            "negative-coordinate migration fixture installs both items");
    ItemEntity *movingEntity = nullptr;
    for (const auto &entity : world.entities) {
      auto *item = dynamic_cast<ItemEntity *>(entity.get());
      if (item && item->uuid == movingUuid) {
        movingEntity = item;
        break;
      }
    }
    require(movingEntity != nullptr, "moving item is live in fixture");
    entities->failNextWriteFor(source);
    world.moveEntityForManagedTest(
        *movingEntity,
        {destination.x * 16 + 3.1, 1.5, destination.z * 16 + 2.5});
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto target = entities->payload(destination);
              if (!target)
                return false;
              const auto decoded = entity_codec::decodeEntityChunk(*target);
              return decoded.status == entity_codec::DecodeStatus::Ok &&
                     itemsOf(decoded).size() == 2;
            }),
            "target entity save confirms migration and preserves old item");
    const auto writePositions = entities->writePositions();
    require(!writePositions.empty() &&
                writePositions.front() == destination,
            "target entity write precedes source cleanup");
    for (int i = 0;
         i < 100 && entities->activePositionalFailures(source) == 0; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto sourceAfterFailure = entities->payload(source);
    require(sourceAfterFailure &&
                itemsOf(entity_codec::decodeEntityChunk(
                            *sourceAfterFailure))
                        .size() == 1 &&
                entities->activePositionalFailures(source) == 1,
            "source entity failure leaves its item on disk");
    manager.setForced(source, false);
    manager.setForced(destination, false);
    const auto shutdown = manager.close();
    require(shutdown.complete,
            "target-unloaded source cleanup closes cleanly");
    require(!world.isChunkLoaded(destination.x, destination.z),
            "confirmed target is unloaded after ticket removal");
    const auto sourceCleared = entities->payload(source);
    require(sourceCleared &&
                itemsOf(entity_codec::decodeEntityChunk(*sourceCleared)).empty(),
            "source cleanup completes after target unload and write failure");
    require(manager.entityPositionLedgerSizeForTest() == 0,
            "unload clears the entity position ledger");
    std::cout << "item_entity migration negative=1 merged=1 source_cleared=1 "
                 "target_unloaded=1" << std::endl;
  }
}

void testMigrationWorkIsEventDrivenWithUnreadyTargets() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{83, -27};
  seedNeighborhood(terrain, source, 3);
  std::vector<entity_codec::ItemEntityData> items;
  for (int i = 0; i < 8; ++i) {
    items.push_back(itemAt(source, {201 + i, 202 + i, 203 + i, 204 + i},
                            source.x * 16 + 1.0 + i,
                            source.z * 16 + 1.5 + i));
  }
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(root, items, {}, error), error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(source, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     world.entities.size() == items.size();
            }),
            "event-driven migration fixture installs items");
    for (int i = 0; i < 8; ++i) {
      auto entity =
          std::dynamic_pointer_cast<ItemEntity>(world.entities[i]);
      require(entity != nullptr, "event-driven migration item exists");
      const ChunkTicketPos absentTarget{source.x + 20 + i, source.z};
      world.moveEntityForManagedTest(
          *entity, {absentTarget.x * 16 + 0.5, 1.5,
                    absentTarget.z * 16 + 0.5});
    }
    for (int i = 0; i < 4; ++i)
      manager.pump();
    require(manager.migrationWorkQueuedForTest() == 0,
            "unready migration work drains once");
    const std::uint64_t visited = manager.migrationWorkVisitedForTest();
    for (int i = 0; i < 128; ++i)
      manager.pump();
    require(manager.migrationWorkVisitedForTest() == visited,
            "idle pumps do not rescan unready migrations");
    const auto shutdown = manager.close();
    require(!shutdown.complete &&
                !shutdown.unsavedChunks.empty(),
            "unready migration is reported unsaved at shutdown");
    const auto sourceData = entities->payload(source);
    require(sourceData &&
                itemsOf(entity_codec::decodeEntityChunk(*sourceData)).size() ==
                    items.size(),
            "unready target does not erase source records");
    std::cout << "item_entity migration unready_batch=8 idle_scans=0"
              << std::endl;
  }
}

void testCrossChunkItemMigrationReloadsOnce() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{-59, -41};
  const ChunkTicketPos destination{-58, -41};
  seedNeighborhood(terrain, source, 3);
  const std::array<std::int32_t, 4> uuid{71, 72, 73, 74};
  const auto item = itemAt(source, uuid, source.x * 16 + 15.9,
                           source.z * 16 + 1.5);
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(source, true);
    manager.setForced(destination, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     world.isChunkLoaded(destination.x, destination.z) &&
                     world.entities.size() == 1;
            }),
            "reload migration fixture installs the item");
    ItemEntity *movingEntity = nullptr;
    for (const auto &entity : world.entities) {
      auto *candidate = dynamic_cast<ItemEntity *>(entity.get());
      if (candidate && candidate->uuid == uuid) {
        movingEntity = candidate;
        break;
      }
    }
    require(movingEntity != nullptr, "reload migration item is live");
    world.moveEntityForManagedTest(
        *movingEntity,
        {destination.x * 16 + 0.5, 1.5, destination.z * 16 + 1.5});
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto destinationData = entities->payload(destination);
              return destinationData &&
                     itemsOf(entity_codec::decodeEntityChunk(
                                 *destinationData))
                             .size() == 1;
            }),
            "target receives the moved item before reload");
    bool sourceEmptied = false;
    for (int i = 0; i < 100 && !sourceEmptied; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      const auto sourceData = entities->payload(source);
      sourceEmptied =
          sourceData &&
          itemsOf(entity_codec::decodeEntityChunk(*sourceData)).empty();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(sourceEmptied, "source empties before reload");
    manager.setForced(source, false);
    manager.setForced(destination, false);
    const auto shutdown = manager.close();
    require(shutdown.complete, "reload migration fixture closes cleanly");
  }
  {
    World reopened(-64, 320);
    WorldChunkManager manager(reopened, terrain, 2, 10,
                              knownEmptyEntityData, entities);
    manager.setForced(source, true);
    manager.setForced(destination, true);
    require(waitManager(manager, [&] {
              return reopened.isChunkLoaded(source.x, source.z) &&
                     reopened.isChunkLoaded(destination.x, destination.z) &&
                     reopened.entities.size() == 1;
            }),
            "reopened chunks contain exactly one migrated item");
    const auto *loaded = dynamic_cast<ItemEntity *>(reopened.entities[0].get());
    require(loaded && loaded->uuid == uuid,
            "reopened migrated item keeps its UUID");
    manager.setForced(source, false);
    manager.setForced(destination, false);
    require(waitManager(manager, [&] {
              return !reopened.isChunkLoaded(source.x, source.z) &&
                     !reopened.isChunkLoaded(destination.x, destination.z);
            }),
            "reopened migrated chunks evict");
    require(manager.close().complete, "reopened migration fixture closes");
    std::cout << "item_entity migration reload=1 uuid_preserved=1"
              << std::endl;
  }
}

void testManagedEntityTickDetectsExternalSectionMove() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{-79, 37};
  const ChunkTicketPos destination{-78, 37};
  seedNeighborhood(terrain, source, 3);
  const std::array<std::int32_t, 4> uuid{111, 112, 113, 114};
  const auto item =
      itemAt(source, uuid, source.x * 16 + 2.0, source.z * 16 + 2.0);
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setPlayer(1, true, source);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     manager.tickReadiness(source).entityTicking &&
                     world.entities.size() == 1;
            }),
            "real-tick migration fixture becomes entity tick ready");
    auto moving =
        std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(moving && moving->uuid == uuid,
            "real-tick migration item is live");
    moving->setPos(destination.x * 16 + 0.5, 64.0,
                   destination.z * 16 + 0.5);
    moving->deltaMovement = {};
    world.tickEntities();
    const ChunkTicketPos movedTo{
        static_cast<int>(std::floor(moving->position.x)) >> 4,
        static_cast<int>(std::floor(moving->position.z)) >> 4};
    require(movedTo == destination,
            "real entity tick leaves item in destination chunk");
    bool persisted = false;
    for (int i = 0; i < 100; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      const auto destinationData = entities->payload(destination);
      const auto sourceData = entities->payload(source);
      persisted =
          destinationData &&
          itemsOf(entity_codec::decodeEntityChunk(*destinationData)).size() == 1 &&
          sourceData &&
          itemsOf(entity_codec::decodeEntityChunk(*sourceData)).empty();
      if (persisted)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(persisted,
            "real tick external move reaches target-first migration");
    require(manager.close().complete,
            "real-tick migration shutdown completes");
    std::cout << "item_entity migration real_tick=1 target_first=1"
              << std::endl;
  }
}

void testCrossChunkItemMigrationIgnoresStaleTargetAck() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos first{-91, 45};
  const ChunkTicketPos second{-90, 45};
  const ChunkTicketPos third{-89, 45};
  seedNeighborhood(terrain, first, 4);
  const std::array<std::int32_t, 4> uuid{121, 122, 123, 124};
  const auto item = itemAt(first, uuid, first.x * 16 + 15.9,
                           first.z * 16 + 1.5);
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(first.x, first.z);
  require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
  entities->setPayload(first, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(first, true);
    manager.setForced(second, true);
    manager.setForced(third, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(first.x, first.z) &&
                     world.isChunkLoaded(second.x, second.z) &&
                     world.isChunkLoaded(third.x, third.z) &&
                     world.entities.size() == 1;
            }),
            "multi-hop migration fixture installs");
    auto moving =
        std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(moving && moving->uuid == uuid,
            "multi-hop migration item is live");
    entities->setBlockWrites(true);
    world.moveEntityForManagedTest(
        *moving, {second.x * 16 + 0.5, 1.5, second.z * 16 + 1.5});
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(second);
              return status &&
                     status->state == ChunkResidencyState::Saving;
            }),
            "first destination write is in flight");
    world.moveEntityForManagedTest(
        *moving, {third.x * 16 + 0.5, 1.5, third.z * 16 + 1.5});
    entities->setBlockWrites(false);
    for (int i = 0; i < 100; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      const auto thirdData = entities->payload(third);
      const auto firstData = entities->payload(first);
      if (thirdData && itemsOf(entity_codec::decodeEntityChunk(*thirdData)).size() == 1 &&
          firstData && itemsOf(entity_codec::decodeEntityChunk(*firstData)).empty())
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto shutdown = manager.close();
    require(shutdown.complete,
            "stale acknowledgement multi-hop shuts down cleanly");
    const auto firstData = entities->payload(first);
    const auto secondData = entities->payload(second);
    const auto thirdData = entities->payload(third);
    require(firstData && itemsOf(entity_codec::decodeEntityChunk(*firstData)).empty(),
            "multi-hop clears the original source");
    require(!secondData || itemsOf(entity_codec::decodeEntityChunk(*secondData)).empty(),
            "stale intermediate target write is cleaned before release");
    require(thirdData && itemsOf(entity_codec::decodeEntityChunk(*thirdData)).size() == 1,
            "multi-hop persists the UUID only at the final target");
    const auto positions = entities->writePositions();
    const auto finalTarget = std::find(positions.begin(), positions.end(), third);
    const auto originalSource = std::find(positions.begin(), positions.end(), first);
    require(finalTarget != positions.end() &&
                originalSource != positions.end() &&
                finalTarget < originalSource,
            "stale intermediate acknowledgement cannot precede final target write");
    std::cout << "item_entity migration stale_ack=0 multi_hop=1"
              << std::endl;
  }
}

void testManagedEntityVelocityCrossesChunkBoundary() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{-84, 53};
  seedNeighborhood(terrain, source, 3);
  const std::array<std::int32_t, 4> uuid{131, 132, 133, 134};
  const auto item =
      itemAt(source, uuid, source.x * 16 + 15.7, source.z * 16 + 7.5);
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setPlayer(1, true, source);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     manager.tickReadiness(source).entityTicking &&
                     world.entities.size() == 1;
            }),
            "moving entity fixture becomes tick ready");
    auto moving =
        std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(moving && moving->uuid == uuid,
            "moving entity fixture has a UUID");
    moving->setPos(source.x * 16 + 15.7, 64.0,
                   source.z * 16 + 7.5);
    moving->deltaMovement = {1.0, 0.0, 0.0};
    moving->onGround = false;
    world.tickEntities();
    const ChunkTicketPos destination{
        static_cast<int>(std::floor(moving->position.x)) >> 4,
        static_cast<int>(std::floor(moving->position.z)) >> 4};
    require(destination.x == source.x + 1 && destination.z == source.z,
            "entity movement enters the next chunk");
    bool migrated = false;
    for (int i = 0; i < 100; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      const auto targetData = entities->payload(destination);
      const auto sourceData = entities->payload(source);
      migrated =
          targetData &&
          itemsOf(entity_codec::decodeEntityChunk(*targetData)).size() == 1 &&
          sourceData &&
          itemsOf(entity_codec::decodeEntityChunk(*sourceData)).empty();
      if (migrated)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(migrated, "entity velocity movement saves target before clearing source");
    require(manager.close().complete,
            "velocity migration shutdown completes");
    std::cout << "item_entity migration velocity_crossing=1" << std::endl;
  }
}

void testPistonMovesEntityAfterEntityTick() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{-99, 61};
  const ChunkTicketPos destination{-98, 61};
  seedNeighborhood(terrain, source, 3);
  const std::array<std::int32_t, 4> uuid{141, 142, 143, 144};
  const auto item = itemAt(source, uuid, source.x * 16 + 15.5,
                           source.z * 16 + 8.0);
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setPlayer(1, true, source);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     world.isChunkLoaded(destination.x, destination.z) &&
                     manager.tickReadiness(source).entityTicking &&
                     manager.tickReadiness(source).blockTicking &&
                     world.entities.size() == 1;
            }),
            "piston movement fixture becomes tick ready");
    auto moving =
        std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(moving && moving->uuid == uuid,
            "piston movement item has the expected UUID");
    moving->setPos(source.x * 16 + 15.75, 64.0,
                   source.z * 16 + 8.0);
    // Model the real World::tick order: the entity has already ticked, then a
    // piston's Entity::move path executes before the manager can pump.
    world.tickEntities();
    moveEntityByPiston(world, Direction::East, *moving, 0.3, Direction::East);
    const ChunkTicketPos movedTo{
        static_cast<int>(std::floor(moving->position.x)) >> 4,
        static_cast<int>(std::floor(moving->position.z)) >> 4};
    require(movedTo == destination,
            "piston moves the entity after its own entity tick");
    manager.removePlayer(1);
    manager.setForced(destination, false);
    manager.setForced(source, false);
    bool migrated = false;
    for (int i = 0; i < 100; ++i) {
      manager.pump();
      const auto targetData = entities->payload(destination);
      const auto sourceData = entities->payload(source);
      migrated =
          targetData &&
          itemsOf(entity_codec::decodeEntityChunk(*targetData)).size() == 1 &&
          sourceData &&
          itemsOf(entity_codec::decodeEntityChunk(*sourceData)).empty();
      if (migrated)
        break;
      manager.serverTick(true, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(migrated,
            "piston movement is persisted before source eviction");
    require(manager.close().complete,
            "piston migration shutdown completes");
    std::cout << "item_entity migration piston_after_tick=1" << std::endl;
  }
}

void testCrossChunkItemMigrationReturnsToSource() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{-67, 21};
  const ChunkTicketPos destination{-66, 21};
  seedNeighborhood(terrain, source, 3);
  const std::array<std::int32_t, 4> uuid{91, 92, 93, 94};
  const auto item =
      itemAt(source, uuid, source.x * 16 + 15.9, source.z * 16 + 1.5);
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(source, true);
    manager.setForced(destination, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     world.isChunkLoaded(destination.x, destination.z) &&
                     world.entities.size() == 1;
            }),
            "return-trip migration fixture installs");
    auto moving =
        std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(moving && moving->uuid == uuid,
            "return-trip item has the expected UUID");
    const Vec3 original = moving->position;
    world.moveEntityForManagedTest(
        *moving, {destination.x * 16 + 0.5, 1.5,
                  destination.z * 16 + 1.5});
    world.moveEntityForManagedTest(*moving, original);
    for (int i = 0; i < 30; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(manager.close().complete,
            "return-to-source migration is not permanently blocked");
    const auto sourceData = entities->payload(source);
    const auto destinationData = entities->payload(destination);
    require(sourceData &&
                itemsOf(entity_codec::decodeEntityChunk(*sourceData)).size() == 1 &&
                (!destinationData ||
                 itemsOf(entity_codec::decodeEntityChunk(*destinationData)).empty()),
            "return-to-source leaves exactly one persisted item");
    std::cout << "item_entity migration return_to_source=1" << std::endl;
  }
}

void testCrossChunkItemMigrationRemovedBeforeCommit() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{-75, 33};
  const ChunkTicketPos destination{-74, 33};
  seedNeighborhood(terrain, source, 3);
  const std::array<std::int32_t, 4> uuid{101, 102, 103, 104};
  const auto item =
      itemAt(source, uuid, source.x * 16 + 15.9, source.z * 16 + 1.5);
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(source, true);
    manager.setForced(destination, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     world.isChunkLoaded(destination.x, destination.z) &&
                     world.entities.size() == 1;
            }),
            "pre-commit removal fixture installs item");
    auto moving =
        std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(moving && moving->uuid == uuid,
            "pre-commit removal item is live");
    world.moveEntityForManagedTest(
        *moving, {destination.x * 16 + 0.5, 1.5,
                  destination.z * 16 + 1.5});
    world.removeEntity(moving);
    world.tickEntities();
    for (int i = 0; i < 30; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto shutdown = manager.close();
    if (!shutdown.complete) {
      std::cerr << "removal-close unsaved=" << shutdown.unsavedChunks.size()
                << " fatal=" << shutdown.fatalError << std::endl;
      for (const auto &issue : shutdown.unsavedChunks)
        std::cerr << "removal-close issue " << issue.position.x << ','
                  << issue.position.z << " state=" << static_cast<int>(issue.state)
                  << " dirty=" << issue.dirtyVersion << " saved="
                  << issue.savedDirtyVersion << " " << issue.error << std::endl;
    }
    require(shutdown.complete,
            "pre-commit removal retries cleanly");
    const auto sourceData = entities->payload(source);
    const auto destinationData = entities->payload(destination);
    require(sourceData &&
                itemsOf(entity_codec::decodeEntityChunk(*sourceData)).empty() &&
                (!destinationData ||
                 itemsOf(entity_codec::decodeEntityChunk(*destinationData)).empty()),
            "pre-commit removal does not resurrect the item");
    std::cout << "item_entity migration removed_before_commit=1"
              << std::endl;
  }
}

void testCrossChunkItemMigrationFailureKeepsSource() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos source{-63, 4};
  const ChunkTicketPos destination{-62, 4};
  seedNeighborhood(terrain, source, 3);
  const std::array<std::int32_t, 4> uuid{61, 62, 63, 64};
  const auto item =
      itemAt(source, uuid, source.x * 16 + 15.9, source.z * 16 + 1.5);
  std::string error;
  nbt::RootTag root = entity_codec::emptyEntityRoot(source.x, source.z);
  require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
  entities->setPayload(source, entity_codec::encodeEntityChunk(root));
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(source, true);
    manager.setForced(destination, true);
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(source.x, source.z) &&
                     world.isChunkLoaded(destination.x, destination.z) &&
                     world.entities.size() == 1;
            }),
            "failing migration source/target install");
    ItemEntity *movingEntity = nullptr;
    for (const auto &entity : world.entities) {
      auto *item = dynamic_cast<ItemEntity *>(entity.get());
      if (item && item->uuid == uuid) {
        movingEntity = item;
        break;
      }
    }
    require(movingEntity != nullptr, "failing migration item is live");
    entities->failNextWrites(1);
    world.moveEntityForManagedTest(
        *movingEntity,
        {destination.x * 16 + 0.1, 1.5, destination.z * 16 + 1.5});
    manager.serverTick(true, true);
    for (int i = 0; i < 30; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(entities->payload(source).has_value(),
            "target entity failure leaves source data on disk");
    require(manager.close().complete,
            "failed migration close retries and succeeds");
    require(entities->payload(source).has_value() ||
                entities->successes() >= 2,
            "close retains or retries source data");
    std::cout << "item_entity migration_failure=1 source_retained=1"
              << std::endl;
  }
}

void testDuplicateItemUuidRejectsSecondChunk() {
  auto terrain = std::make_shared<FakeStore>();
  auto entities = std::make_shared<FakeEntityStore>();
  const ChunkTicketPos first{60, 0};
  const ChunkTicketPos second{61, 0};
  const std::array<std::int32_t, 4> uuid{9, 10, 11, 12};
  terrain->setPayload(first, makeChunkNbt(first));
  terrain->setPayload(second, makeChunkNbt(second));
  std::string error;
  for (const ChunkTicketPos &position : {first, second}) {
    nbt::RootTag root =
        entity_codec::emptyEntityRoot(position.x, position.z);
    const auto item = itemAt(
        position, uuid, position.x * 16 + 2.0, position.z * 16 + 2.0);
    require(entity_codec::patchEntityItems(root, {&item, 1}, {}, error), error);
    entities->setPayload(position,
                         entity_codec::encodeEntityChunk(root));
  }
  World world(-64, 320);
  {
    WorldChunkManager manager(world, terrain, 2, 10, knownEmptyEntityData,
                              entities);
    manager.setForced(first, true);
    manager.setForced(second, true);
    require(waitManager(manager, [&] {
              if (world.entities.size() != 1)
                return false;
              const auto a = manager.lifecycleStatus(first);
              const auto b = manager.lifecycleStatus(second);
              return a && b &&
                     ((a->state == ChunkResidencyState::Resident &&
                       b->state == ChunkResidencyState::Failed) ||
                      (b->state == ChunkResidencyState::Resident &&
                       a->state == ChunkResidencyState::Failed));
            }),
            "duplicate item UUID permits only one chunk install");
    const auto firstStatus = manager.lifecycleStatus(first);
    const ChunkTicketPos failed =
        firstStatus && firstStatus->state == ChunkResidencyState::Failed
            ? first
            : second;
    require(manager.managedStatus(failed) &&
                manager.managedStatus(failed)->error.find("UUID") !=
                    std::string::npos,
            "duplicate item UUID reports an explicit install error");
    manager.setForced(first, false);
    manager.setForced(second, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(first.x, first.z) &&
                     !world.isChunkLoaded(second.x, second.z);
            }),
            "duplicate-UUID fixture unloads its one installed chunk");
    require(manager.close().complete,
            "duplicate-UUID fixture closes");
    std::cout << "item_entity duplicate_uuid_rejected=1" << std::endl;
  }
}

void testRealItemEntityLifecycle(
    const std::filesystem::path &sourceWorld,
    const std::filesystem::path &scratch) {
  const auto sourceDimension =
      sourceWorld / "dimensions" / "minecraft" / "overworld";
  const auto base = scratch / "item-world";
  const auto region = base / "region";
  const auto entities = base / "entities";
  std::filesystem::create_directories(region);
  std::filesystem::create_directories(entities);
  for (const auto &entry :
       std::filesystem::directory_iterator(sourceDimension / "region"))
    if (entry.is_regular_file() && entry.path().extension() == ".mca")
      std::filesystem::copy_file(
          entry.path(), region / entry.path().filename(),
          std::filesystem::copy_options::overwrite_existing);
  for (const auto &entry :
       std::filesystem::directory_iterator(sourceDimension / "entities"))
    if (entry.is_regular_file() && entry.path().extension() == ".mca")
      std::filesystem::copy_file(
          entry.path(), entities / entry.path().filename(),
          std::filesystem::copy_options::overwrite_existing);

  const ChunkTicketPos chunk{-1, -1};
  const std::array<std::int32_t, 4> uuid{
      1000665188, 1491226247, -1939110731, 1144620115};
  World world(-64, 320);
  {
    WorldChunkManager manager(world, region, 2, 10);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 8;
    limits.maxSubmissions = 2;
    limits.maxInFlightSaves = 2;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    const Ticket loadingOnly(ticket_types::PlayerLoading,
                             TicketSourceManager::PlayerLoadingLevel);
    const std::array<TicketOperation, 1> loadItem{
        TicketOperation::add(chunk, loadingOnly)};
    manager.applyTicketUpdate(
        manager.ticketSources().tickets().applyBatch(loadItem));
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(chunk.x, chunk.z) &&
                     world.entities.size() == 1;
            }),
            "official item entity installs with its chunk");
    auto item = std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(item && item->uuid == uuid && item->age == 17 &&
                item->pickupDelay == 200 && item->health == 5 &&
                item->item.id == "minecraft:stone" && item->item.count == 3,
            "official item fields and UUID survive load");
    require(!manager.tickReadiness(chunk).entityTicking,
            "forced loading level does not enable entity ticking");
    manager.serverTick(true, true);
    require(item->age == 17 && item->pickupDelay == 200,
            "paused item does not advance Age or PickupDelay");

    manager.setPlayer(1, true, chunk);
    require(waitManager(manager, [&] {
              return manager.tickReadiness(chunk).entityTicking;
            }),
            "simulation view enables item ticking");
    manager.serverTick(true, true);
    require(item->age == 18 && item->pickupDelay == 199 && item->uuid == uuid,
            "ready item tick advances Age/PickupDelay and preserves UUID");
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(chunk);
              return status && status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "item tick saves terrain and entity data in one version");
    manager.removePlayer(1);
    const std::array<TicketOperation, 1> unloadItem{
        TicketOperation::remove(chunk, loadingOnly)};
    manager.applyTicketUpdate(
        manager.ticketSources().tickets().applyBatch(unloadItem));
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(chunk.x, chunk.z);
            }),
            "item chunk unloads after both region writes");

    const auto raw = region::readChunkNbt(entities, chunk.x, chunk.z);
    require(raw.status == region::ReadStatus::Found, raw.detail);
    entity_codec::DecodedEntityChunk mid = entity_codec::decodeEntityChunk(raw.nbt);
    require(mid.status == entity_codec::DecodeStatus::Ok &&
                itemsOf(mid).size() == 1 && itemsOf(mid)[0].uuid == uuid &&
                itemsOf(mid)[0].age == 18 && itemsOf(mid)[0].pickupDelay == 199,
            "entity region preserves UUID and advanced item state");
    const auto midEntities = scratch / "item-mid-entities";
    std::filesystem::create_directories(midEntities);
    std::filesystem::copy_file(
        entities / "r.-1.-1.mca", midEntities / "r.-1.-1.mca",
        std::filesystem::copy_options::overwrite_existing);

    manager.applyTicketUpdate(
        manager.ticketSources().tickets().applyBatch(loadItem));
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(chunk.x, chunk.z) &&
                     world.entities.size() == 1;
            }),
            "item reloads from its entity region");
    item = std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(item && item->uuid == uuid && item->age == 18,
            "reloaded item keeps persistent UUID and state");
    world.removeEntity(item);
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto status = manager.lifecycleStatus(chunk);
              return world.entities.empty() && status &&
                     status->dirtyVersion <= status->savedDirtyVersion;
            }),
            "last item removal persists an empty entity list");
    manager.applyTicketUpdate(
        manager.ticketSources().tickets().applyBatch(unloadItem));
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(chunk.x, chunk.z);
            }),
            "empty item chunk unloads");
    const auto emptyRaw = region::readChunkNbt(entities, chunk.x, chunk.z);
    require(emptyRaw.status == region::ReadStatus::Found, emptyRaw.detail);
    const auto empty = entity_codec::decodeEntityChunk(emptyRaw.nbt);
    require(empty.status == entity_codec::DecodeStatus::Ok &&
                itemsOf(empty).empty(),
            "empty entity region root clears the old item");

    manager.applyTicketUpdate(
        manager.ticketSources().tickets().applyBatch(loadItem));
    require(waitManager(manager, [&] {
              return world.isChunkLoaded(chunk.x, chunk.z);
            }),
            "empty item chunk reloads");
    std::cout << "item_entity saved_age=18 uuid_preserved=1 removed=1"
              << std::endl;
    require(world.entities.empty(),
            "removed item does not resurrect after reload");
    manager.applyTicketUpdate(
        manager.ticketSources().tickets().applyBatch(unloadItem));
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(chunk.x, chunk.z);
            }),
            "final item cleanup unloads");
    require(manager.close().complete,
            "item entity lifecycle shutdown completes");
  }
}

void testRealItemEntityCrossRegionMigration(
    const std::filesystem::path &sourceWorld,
    const std::filesystem::path &scratch) {
  const auto sourceDimension =
      sourceWorld / "dimensions" / "minecraft" / "overworld";
  const auto base = scratch / "item-migration-world";
  const auto region = base / "region";
  const auto entitiesDir = base / "entities";
  std::filesystem::create_directories(region);
  std::filesystem::create_directories(entitiesDir);
  for (const auto &entry :
       std::filesystem::directory_iterator(sourceDimension / "region"))
    if (entry.is_regular_file() && entry.path().extension() == ".mca")
      std::filesystem::copy_file(
          entry.path(), region / entry.path().filename(),
          std::filesystem::copy_options::overwrite_existing);
  for (const auto &entry :
       std::filesystem::directory_iterator(sourceDimension / "entities"))
    if (entry.is_regular_file() && entry.path().extension() == ".mca")
      std::filesystem::copy_file(
          entry.path(), entitiesDir / entry.path().filename(),
          std::filesystem::copy_options::overwrite_existing);

  const std::array<std::int32_t, 4> uuid{
      1000665188, 1491226247, -1939110731, 1144620115};
  const auto emptyTerrainRoot = [](const ChunkTicketPos &position) {
    nbt::RootTag root;
    root.name = "Level";
    root.value.type = 10;
    add(root.value, "DataVersion", number(3, 4786));
    add(root.value, "xPos", number(3, position.x));
    add(root.value, "zPos", number(3, position.z));
    add(root.value, "Status", string("minecraft:full"));
    add(root.value, "sections", list(10, {}));
    return nbt::writeRoot(root);
  };
  const ChunkTicketPos source{-1, -1};
  const ChunkTicketPos destination{0, -1};
  for (int dx = -2; dx <= 2; ++dx)
    for (int dz = -2; dz <= 2; ++dz) {
      const ChunkTicketPos position{source.x + dx, source.z + dz};
      if (position == source)
        continue;
      (void)region::writeChunkNbt(region, position.x, position.z,
                                  emptyTerrainRoot(position),
                                  static_cast<int>(std::time(nullptr)));
    }
  nbt::RootTag destinationEntities =
      entity_codec::emptyEntityRoot(destination.x, destination.z);
  (void)region::writeChunkNbt(entitiesDir, destination.x, destination.z,
                              entity_codec::encodeEntityChunk(destinationEntities),
                              static_cast<int>(std::time(nullptr)));

  World world(-64, 320);
  {
    WorldChunkManager manager(world, region, 2, 10);
    AutosaveLimits limits;
    limits.maxCandidateChecks = 8;
    limits.maxSubmissions = 2;
    limits.maxInFlightSaves = 2;
    limits.candidateTimeBudget = std::chrono::milliseconds(50);
    limits.cooldown = std::chrono::milliseconds(0);
    limits.failureBackoff = std::chrono::milliseconds(0);
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(source, true);
    manager.setForced(destination, true);
    const bool installed = waitManager(manager, [&] {
      return world.isChunkLoaded(destination.x, destination.z) &&
             world.entities.size() == 1;
    });

    require(installed,
            "official item cross-region fixture installs target chunk");
    auto item =
        std::dynamic_pointer_cast<ItemEntity>(world.entities.front());
    require(item && item->uuid == uuid, "official migration item UUID");
    world.moveEntityForManagedTest(
        *item, {destination.x * 16 + 1.5, item->position.y,
                destination.z * 16 + 1.5});
    manager.serverTick(true, true);
    require(waitManager(manager, [&] {
              const auto raw =
                  region::readChunkNbt(entitiesDir, destination.x,
                                       destination.z);
              return raw.status == region::ReadStatus::Found &&
                     itemsOf(entity_codec::decodeEntityChunk(raw.nbt)).size() == 1;
            }),
            "official target entity slot gets the migrated UUID");
    bool realSourceCleared = false;
    for (int i = 0; i < 100 && !realSourceCleared; ++i) {
      manager.serverTick(true, true);
      manager.pump();
      const auto raw =
          region::readChunkNbt(entitiesDir, source.x, source.z);
      realSourceCleared =
          raw.status == region::ReadStatus::Found &&
          itemsOf(entity_codec::decodeEntityChunk(raw.nbt)).empty();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(realSourceCleared,
            "official source entity slot is cleared after target ack");
    manager.setForced(source, false);
    manager.setForced(destination, false);
    require(waitManager(manager, [&] {
              return !world.isChunkLoaded(destination.x, destination.z);
            }),
            "official migrated target chunk evicts");
    require(manager.close().complete, "official migration fixture closes");
  }
  std::cout << "item_entity migration real_region=1 target_then_source=1"
            << std::endl;
}

void testTransientBlockEventDoesNotBecomeStickyBlocker() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos position{13, 0};
  seedFullNeighborhood(store, position);
  World world(-64, 320);
  {
    WorldChunkManager manager(world, store, 3, 10, knownEmptyEntityData);
    manager.setForced(position, true);
    require(waitManager(manager, [&] {
              auto status = manager.lifecycleStatus(position);
              return status &&
                     status->state == ChunkResidencyState::Resident &&
                     manager.tickReadiness(position).blockTicking;
            }),
            "block-event fixture reaches block-ticking Resident");
    const Pos target{16 * 13, 0, 0};
    require(manager.setBlock(target, Blocks::AIR.defaultState()),
            "block-event dirty edit succeeds");
    world.blockEvent(target, BlockRef(Blocks::STONE), 0, 0);
    manager.setForced(position, false);
    require(waitManager(manager, [&] {
              return store->successes() >= 1 &&
                     world.isChunkLoaded(position.x, position.z);
            }),
            "block event blocks unload but not terrain/tick save");
    require(manager.managedStatus(position)->blocker.empty(),
            "transient block event is not a sticky persistence blocker");
    world.tick();
    const bool evictedAfterEvent = waitManager(manager, [&] {
      return !world.isChunkLoaded(position.x, position.z);
    });
    auto afterEventStatus = manager.managedStatus(position);
    require(evictedAfterEvent,
            "processed block event allows pending eviction to proceed; state=" +
                std::to_string(afterEventStatus
                                   ? static_cast<int>(
                                         afterEventStatus->lifecycle.state)
                                   : -1) +
                " blocker=" +
                (afterEventStatus ? afterEventStatus->blocker : std::string{}) +
                " error=" +
                (afterEventStatus ? afterEventStatus->error : std::string{}) +
                " events=" + std::to_string(world.blockEventCount()));
    require(manager.close().complete,
            "transient block-event shutdown completes");
  }
}

int extractNibbleAt(const std::optional<std::array<std::uint8_t, 2048>> &bytes,
                    int localX, int localY, int localZ) {
  if (!bytes)
    return 0;
  const std::size_t idx =
      (static_cast<std::size_t>(localY) << 8) |
      (static_cast<std::size_t>(localZ) << 4) |
      static_cast<std::size_t>(localX);
  const std::uint8_t packed = (*bytes)[idx >> 1];
  return (idx & 1u) ? ((packed >> 4) & 0x0Fu) : (packed & 0x0Fu);
}

std::vector<std::uint8_t> makeTwoSectionAirChunkNbt(const ChunkTicketPos &p) {
  nbt::RootTag root;
  root.name = "Level";
  root.value.type = 10;
  add(root.value, "DataVersion", number(3, 4786));
  add(root.value, "xPos", number(3, p.x));
  add(root.value, "zPos", number(3, p.z));
  add(root.value, "Status", string("minecraft:full"));

  auto makeUniformSec = [](int sectionY, std::string blockName) {
    nbt::Tag entry;
    entry.type = 10;
    add(entry, "Name", string(std::move(blockName)));
    nbt::Tag states;
    states.type = 10;
    add(states, "palette", list(10, {std::move(entry)}));
    nbt::Tag section;
    section.type = 10;
    add(section, "Y", number(1, sectionY));
    add(section, "block_states", std::move(states));
    return section;
  };
  add(root.value, "sections",
      list(10, {makeUniformSec(0, "minecraft:stone"),
                makeUniformSec(1, "minecraft:air")}));
  return nbt::writeRoot(root);
}

void testManagedLightPlaceBreakAnvilRoundTripAndCleanReload() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos pos{0, 0};
  store->setPayload(pos, makeTwoSectionAirChunkNbt(pos));

  {
    World world(0, 64);
    world.enableLighting();
    WorldChunkManager manager(world, store);
    manager.setForced(pos, true);
    require(waitManager(manager, [&] {
              auto st = manager.managedStatus(pos);
              return st && st->lifecycle.state == ChunkResidencyState::Resident &&
                     st->light && st->light->complete;
            }),
            "initial light completes");

    // Place a redstone lamp and a stone roof above another column.
    const Pos oldLamp{4, 20, 4};
    const Pos finalLamp{15, 20, 8};
    const Pos roofPos{8, 25, 8};
    require(manager.setBlock(oldLamp, Blocks::REDSTONE_LAMP.defaultState()
                                          .with("lit", "true")),
            "place initial lamp");
    require(manager.setBlock(oldLamp, Blocks::AIR.defaultState()),
            "break initial lamp");
    require(manager.setBlock(finalLamp, Blocks::REDSTONE_LAMP.defaultState()
                                            .with("lit", "true")),
            "place final lamp");
    require(manager.setBlock(roofPos, Blocks::STONE.defaultState()),
            "place roof block");

    const ChunkShutdownResult shutdown = manager.close();
    require(shutdown.complete && shutdown.unsavedChunks.empty(),
            "clean light shutdown completes without unsaved chunks");
    world.disableLighting();
  }

  auto savedBytes = store->latestPayload(pos);
  require(savedBytes.has_value(), "saved NBT exists in store");
  const DecodedChunk decoded = decodeChunkNbt(*savedBytes);
  require(decoded.status == DecodeStatus::Ok && decoded.isLightOn,
          "clean shutdown persists isLightOn=1");
  const auto sec1 = std::find_if(
      decoded.sections.begin(), decoded.sections.end(),
      [](const terrain::Section &s) { return s.y == 1; });
  require(sec1 != decoded.sections.end() && sec1->blockLight && sec1->skyLight,
          "section 1 carries both BlockLight and SkyLight");
  require(extractNibbleAt(sec1->blockLight, 4, 4, 4) == 0,
          "broken lamp leaves BlockLight=0");
  require(extractNibbleAt(sec1->blockLight, 15, 4, 8) == 15,
          "final lamp persists BlockLight=15 in nibble array");
  require(extractNibbleAt(sec1->skyLight, 8, 9, 8) == 0,
          "opaque stone roof persists SkyLight=0 in cell");
  require(extractNibbleAt(sec1->skyLight, 14, 4, 8) == 15,
          "open sky column adjacent to lamp persists SkyLight=15 in nibble array (got " +
              std::to_string(extractNibbleAt(sec1->skyLight, 14, 4, 8)) + ")");

  // Reload into a fresh World + WorldChunkManager: brightness must be valid
  // immediately without recomputing from scratch, and untouched reload must not
  // spuriously dirty/save the chunk.
  const int writesBeforeReload = store->writeCallCount();
  {
    World world(0, 64);
    world.enableLighting();
    WorldChunkManager manager(world, store);
    AutosaveLimits limits;
    limits.cooldown = std::chrono::milliseconds{0};
    manager.setAutosaveLimitsForTest(limits);
    manager.setForced(pos, true);
    require(waitManager(manager, [&] {
              auto st = manager.managedStatus(pos);
              return st && st->lifecycle.state == ChunkResidencyState::Resident &&
                     st->light && st->light->complete;
            }),
            "reloaded persisted-light chunk completes");
    auto blockSnap =
        world.lightService()->latestLayerChunk(LightLayer::Block, 0, 0);
    auto skySnap =
        world.lightService()->latestLayerChunk(LightLayer::Sky, 0, 0);
    const auto findSec1 = [](const std::shared_ptr<const LightChunkSnapshot> &snap)
        -> std::shared_ptr<const LightSectionSnapshot> {
      if (!snap)
        return nullptr;
      for (const auto &sec : snap->sections)
        if (sec && sec->sectionY == 1)
          return sec;
      return nullptr;
    };
    const auto bSec1 = findSec1(blockSnap);
    const auto sSec1 = findSec1(skySnap);
    require(bSec1 && sSec1,
            "reloaded persisted-light chunk exposes section 1 snapshots");
    require(bSec1->light(LightLayer::Block, 15, 4, 8) == 15 &&
                bSec1->light(LightLayer::Block, 4, 4, 4) == 0 &&
                sSec1->light(LightLayer::Sky, 8, 9, 8) == 0 &&
                sSec1->light(LightLayer::Sky, 14, 4, 8) == 15,
            "reloaded chunk preserves persisted block and sky nibbles (got b15=" +
                std::to_string(bSec1->light(LightLayer::Block, 15, 4, 8)) +
                " b4=" +
                std::to_string(bSec1->light(LightLayer::Block, 4, 4, 4)) +
                " sRoof=" +
                std::to_string(sSec1->light(LightLayer::Sky, 8, 9, 8)) +
                " s14=" +
                std::to_string(sSec1->light(LightLayer::Sky, 14, 4, 8)) + ")");
    for (int i = 0; i < 5; ++i) {
      manager.serverTick(true, true);
      manager.pump();
    }
    auto statusAfterIdle = manager.managedStatus(pos);
    require(statusAfterIdle &&
                statusAfterIdle->lifecycle.dirtyVersion == 0 &&
                store->writeCallCount() == writesBeforeReload,
            "untouched isLightOn=1 reload stays clean without spurious saves");
    require(manager.close().complete, "clean reload closes cleanly");
    require(store->writeCallCount() == writesBeforeReload,
            "clean reload writes nothing on close");
    world.disableLighting();
  }
}

void testManagedLightDelayedNeighborCompletion() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos eastNeighbor{1, 0};
  const ChunkTicketPos origin{0, 0};

  auto makeStoneCeilingChunk = [](const ChunkTicketPos &p) {
    nbt::RootTag root;
    root.name = "Level";
    root.value.type = 10;
    add(root.value, "DataVersion", number(3, 4786));
    add(root.value, "xPos", number(3, p.x));
    add(root.value, "zPos", number(3, p.z));
    add(root.value, "Status", string("minecraft:full"));
    nbt::Tag air;
    air.type = 10;
    add(air, "Name", string("minecraft:air"));
    nbt::Tag stone;
    stone.type = 10;
    add(stone, "Name", string("minecraft:stone"));
    nbt::Tag sec1States;
    sec1States.type = 10;
    add(sec1States, "palette", list(10, {air, stone}));
    std::vector<std::uint16_t> cells(4096, 0);
    // Solid ceiling at local y=9 (world y=25) blocking sky light below.
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        cells[(9u << 8) | (static_cast<unsigned>(z) << 4) |
              static_cast<unsigned>(x)] = 1;
    nbt::Tag data;
    data.type = 12;
    data.longs = packedCells(cells, 4);
    add(sec1States, "data", std::move(data));
    nbt::Tag sec1;
    sec1.type = 10;
    add(sec1, "Y", number(1, 1));
    add(sec1, "block_states", std::move(sec1States));
    add(root.value, "sections", list(10, {std::move(sec1)}));
    return nbt::writeRoot(root);
  };

  store->setPayload(origin, makeStoneCeilingChunk(origin));
  store->setPayload(eastNeighbor, makeStoneCeilingChunk(eastNeighbor));

  World world(0, 64);
  world.enableLighting();
  WorldChunkManager manager(world, store);
  manager.setForced(origin, true);
  require(waitManager(manager, [&] {
            auto st = manager.managedStatus(origin);
            return st && st->lifecycle.state == ChunkResidencyState::Resident &&
                   st->light && st->light->complete;
          }),
          "origin chunk reaches complete alone");
  require(manager.setBlock({15, 20, 8}, Blocks::REDSTONE_LAMP.defaultState()
                                            .with("lit", "true")),
          "place boundary lamp in origin");
  require(waitManager(manager, [&] {
            auto st = manager.managedStatus(origin);
            return st && st->light && st->light->complete &&
                   !st->light->pending;
          }),
          "origin chunk finishes light with east neighbor unloaded");
  require(world.lightService()->stats().unloadedNeighborSkips > 0 &&
              manager.managedStatus(origin)->light->unloadedNeighborMask != 0,
          "unloaded neighbor boundary skips are observable");

  // Load dark east neighbor later: origin's boundary light must propagate across
  // to (16, 20, 8) and get persisted on shutdown.
  manager.setForced(eastNeighbor, true);
  require(waitManager(manager, [&] {
            auto st0 = manager.managedStatus(origin);
            auto st1 = manager.managedStatus(eastNeighbor);
            return st1 &&
                   st1->lifecycle.state == ChunkResidencyState::Resident &&
                   st1->light && st1->light->complete && !st1->light->pending &&
                   st0 && st0->light && st0->light->complete &&
                   world.getBrightness({16, 20, 8}) == 14;
          }),
          "late-loaded dark neighbor receives cross-border block light 14");
  require(manager.close().complete, "neighbor completion test closes cleanly");
  auto savedEast = store->latestPayload(eastNeighbor);
  require(savedEast.has_value(), "east neighbor persisted on shutdown");
  const DecodedChunk decodedEast = decodeChunkNbt(*savedEast);
  require(decodedEast.isLightOn, "east neighbor saved isLightOn=1");
  const auto eastSec1 = std::find_if(
      decodedEast.sections.begin(), decodedEast.sections.end(),
      [](const terrain::Section &s) { return s.y == 1; });
  require(eastSec1 != decodedEast.sections.end() &&
              extractNibbleAt(eastSec1->blockLight, 0, 4, 8) == 14,
          "east neighbor disk nibble records cross-border BlockLight=14");
  world.disableLighting();
}

void testManagedLightBacklogAutosaveNonBlocking() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos pos{0, 0};
  store->setPayload(pos, makeTwoSectionAirChunkNbt(pos));

  World world(0, 64);
  world.enableLighting();
  WorldChunkManager manager(world, store);
  AutosaveLimits limits;
  limits.cooldown = std::chrono::milliseconds{0};
  limits.failureBackoff = std::chrono::milliseconds{0};
  manager.setAutosaveLimitsForTest(limits);
  manager.setForced(pos, true);
  require(waitManager(manager, [&] {
            auto st = manager.managedStatus(pos);
            return st && st->lifecycle.state == ChunkResidencyState::Resident &&
                   st->light && st->light->complete;
          }),
          "initial light settles");

  // Cleanly save initial isLightOn=1 first so store carries stale nibbles.
  for (int i = 0; i < 4 && store->successes() == 0; ++i) {
    manager.serverTick(true, true);
    manager.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  require(waitManager(manager, [&] {
            return store->successes() >= 1;
          }),
          "initial light-on save completes");

  // Now pause LightService, edit block, and force an online autosave. It must
  // NOT block on LightService; it must write isLightOn=0 with cleared nibbles.
  world.lightService()->pauseForTesting();
  require(manager.setBlock({8, 20, 8}, Blocks::REDSTONE_LAMP.defaultState()
                                           .with("lit", "true")),
          "mutate block while light worker is paused");
  auto pausedStatus = manager.managedStatus(pos);
  require(pausedStatus && pausedStatus->light && pausedStatus->light->pending,
          "managedStatus exposes pending light backlog");

  const int writesBeforePausedSave = store->successes();
  for (int i = 0; i < 8 && store->successes() == writesBeforePausedSave;
       ++i) {
    manager.serverTick(true, true);
    manager.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  require(waitManager(manager, [&] {
            return store->successes() > writesBeforePausedSave;
          }),
          "nonblocking autosave writes while LightService is paused");
  {
    auto interimBytes = store->latestPayload(pos);
    require(interimBytes.has_value(), "interim paused save payload present");
    const DecodedChunk decoded = decodeChunkNbt(*interimBytes);
    require(decoded.status == DecodeStatus::Ok && !decoded.isLightOn,
            "pending-light autosave writes isLightOn=0 instead of blocking");
    for (const auto &sec : decoded.sections)
      require(!sec.blockLight && !sec.skyLight,
              "isLightOn=0 save clears potentially stale light nibbles");
  }

  // Resume LightService: once converged, pump()/serverTick() must automatically
  // mark the chunk for light persistence and write isLightOn=1.
  const int writesBeforeResume = store->successes();
  world.lightService()->resumeForTesting();
  require(world.lightService()->flushAndWait(std::chrono::seconds(2)),
          "resumed light flushes cleanly");
  for (int i = 0; i < 10 && store->successes() == writesBeforeResume; ++i) {
    manager.serverTick(true, true);
    manager.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  require(waitManager(manager, [&] {
            return store->successes() > writesBeforeResume;
          }),
          "settled light schedules follow-up isLightOn=1 save");
  {
    auto settledBytes = store->latestPayload(pos);
    const DecodedChunk decoded = decodeChunkNbt(*settledBytes);
    require(decoded.status == DecodeStatus::Ok && decoded.isLightOn,
            "converged follow-up save restores isLightOn=1");
    const auto sec1 = std::find_if(
        decoded.sections.begin(), decoded.sections.end(),
        [](const terrain::Section &s) { return s.y == 1; });
    require(sec1 != decoded.sections.end() &&
                extractNibbleAt(sec1->blockLight, 8, 4, 8) == 15,
            "converged follow-up save carries BlockLight=15 nibble");
  }
  require(manager.close().complete, "backlog test closes cleanly");
  world.disableLighting();
}

void testManagedLightUnloadReloadStaleEpochIsolation() {
  auto store = std::make_shared<FakeStore>();
  const ChunkTicketPos pos{0, 0};
  store->setPayload(pos, makeTwoSectionAirChunkNbt(pos));

  World world(0, 64);
  world.enableLighting();
  WorldChunkManager manager(world, store);
  manager.setForced(pos, true);
  require(waitManager(manager, [&] {
            auto st = manager.managedStatus(pos);
            return st && st->lifecycle.state == ChunkResidencyState::Resident &&
                   st->light && st->light->complete;
          }),
          "epoch1 reaches complete");
  const std::uint64_t epoch1 =
      manager.managedStatus(pos)->light->lifecycleEpoch;

  // Queue work in epoch1 while paused, unload cleanly, reload into epoch2, then
  // resume and ensure epoch2 settles cleanly without old-epoch pollution.
  world.lightService()->pauseForTesting();
  require(manager.setBlock({8, 20, 8}, Blocks::REDSTONE_LAMP.defaultState()
                                           .with("lit", "true")),
          "queue epoch1 block edit while paused");
  manager.setForced(pos, false);
  require(waitManager(manager, [&] {
            return !world.isChunkLoaded(pos.x, pos.z) &&
                   !manager.isManaged(pos);
          }),
          "dirty chunk saves (isLightOn=0) and unloads cleanly even while paused");
  manager.setForced(pos, true);
  require(waitManager(manager, [&] {
            return world.isChunkLoaded(pos.x, pos.z) &&
                   manager.isManaged(pos);
          }),
          "reloaded chunk enters epoch2");
  require(manager.setBlock({8, 20, 8}, Blocks::AIR.defaultState()),
          "break lamp in epoch2 before worker resumes");
  world.lightService()->resumeForTesting();
  require(waitManager(manager, [&] {
            auto st = manager.managedStatus(pos);
            return st && st->lifecycle.state == ChunkResidencyState::Resident &&
                   st->light && st->light->complete && !st->light->pending &&
                   st->light->lifecycleEpoch > epoch1;
          }),
          "epoch2 completes at strictly higher epoch");
  require(world.getBrightness({8, 20, 8}) == 15,
          "epoch2 sees sky light 15 and no phantom block light from epoch1");
  require(manager.close().complete, "stale-epoch reload test closes cleanly");
  world.disableLighting();
}

void testRealWorldLightPersistenceCycle(
    const std::filesystem::path &sourceRegion,
    const std::filesystem::path &scratch) {
  const std::filesystem::path region = scratch / "real-world-light-region";
  std::filesystem::create_directories(region);
  std::filesystem::copy_file(sourceRegion / "r.-1.-1.mca",
                             region / "r.-1.-1.mca",
                             std::filesystem::copy_options::overwrite_existing);
  const ChunkTicketPos ticket{-1, -1};
  const Pos glowstonePos{-16, 1, -16};

  {
    World world(-64, 320);
    world.enableLighting();
    WorldChunkManager manager(world, region);
    manager.setForced(ticket, true);
    require(waitManager(manager, [&] {
              auto st = manager.managedStatus(ticket);
              return st && st->lifecycle.state == ChunkResidencyState::Resident &&
                     st->light && st->light->complete;
            }),
            "real 26.1 chunk completes initial lighting");
    require(manager.setBlock(glowstonePos, Blocks::REDSTONE_LAMP.defaultState()
                                               .with("lit", "true")),
            "place lit lamp in real 26.1 chunk");
    require(manager.close().complete,
            "real 26.1 chunk cleanly persists light on shutdown");
    world.disableLighting();
  }

  const DecodedChunk disk = decodeChunkAt(region, ticket.x, ticket.z);
  require(disk.status == DecodeStatus::Ok && disk.isLightOn,
          "real 26.1 region chunk on disk carries isLightOn=1 after shutdown");
  const auto sec0 = std::find_if(
      disk.sections.begin(), disk.sections.end(),
      [](const terrain::Section &s) { return s.y == 0; });
  require(sec0 != disk.sections.end() && sec0->blockLight &&
              extractNibbleAt(sec0->blockLight, 0, 1, 0) == 15,
          "real 26.1 region section 0 carries BlockLight=15 at placed glowstone");

  // Reload the persisted real 26.1 region via a second World + WorldChunkManager
  // and verify persisted BlockLight/SkyLight reuse + clean idle reload.
  {
    World reloaded(-64, 320);
    reloaded.enableLighting();
    WorldChunkManager manager(reloaded, region);
    manager.setForced(ticket, true);
    require(waitManager(manager, [&] {
              auto st = manager.managedStatus(ticket);
              return st && st->lifecycle.state == ChunkResidencyState::Resident &&
                     st->light && st->light->complete;
            }),
            "reloaded real 26.1 chunk completes persisted light");
    require(reloaded.getBrightness(glowstonePos) == 15,
            "reloaded real 26.1 chunk preserves BlockLight=15 at glowstone");
    require(manager.close().complete,
            "reloaded real 26.1 chunk closes cleanly");
    reloaded.disableLighting();
  }

  // If the real fixture parent directory includes level.dat, also test full
  // offline loadAnvilWorld -> importWorld -> enableLighting against the saved
  // region directory.
  if (std::filesystem::exists(sourceRegion.parent_path() / "level.dat")) {
    World reloaded(-64, 320);
    LoadedWorld loaded = loadAnvilWorld(sourceRegion.parent_path(), region);
    importWorld(loaded, reloaded);
    reloaded.enableLighting();
    require(reloaded.lightService()->flushAndWait(std::chrono::seconds(2)),
            "imported real 26.1 persisted light settles cleanly");
    reloaded.drainLightPublications();
    require(reloaded.getBrightness(glowstonePos) == 15 &&
                reloaded.lightService()->chunkStatus(ticket.x, ticket.z).complete,
            "importWorld preserves persisted isLightOn=1 light on enableLighting");
    reloaded.disableLighting();
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "compact") {
      const std::filesystem::path scratch(argv[2]);
      std::filesystem::remove_all(scratch);
      std::filesystem::create_directories(scratch);
      testRepeatedFailurePatchIsDeduplicatedByCell();
      testCompactEditTransferAndTiming();
      testRetainedCompactUnknownStateBlocksSave();
      std::cout << "compact edit transfer tests passed" << std::endl;
      return 0;
    }
    require(argc >= 2 && argc <= 6,
            "world_chunk_manager_tests requires scratch [terrain] [tick] [piston] [item world]");
    const std::filesystem::path scratch(argv[1]);
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    testDynamicChunksAreRejected();
    testManagedPaletteInstall();
    testStaleManagedLoad();
    testSaveDuringEditAndFailure();
    testReadMetadataReleaseBurstAndLongTravel();
    testOfflineAndBlockEventGate();
    testTimeoutAgesDirtyDuringInFlightSave();
    testPerChunkTickSnapshotTake();
    testReadinessRadiiAndMissingNeighbor();
    testDueTicksWaitForReadinessAndResumeOrder();
    testServerTickOwnsTimeoutAging();
    testEntityRegionProbeAndPositionGate();
    testSavedTicksPauseDirtyAndOrder();
    testTickDelayJavaIntConversion();
    testTickEditDuringSave();
    testHeldDirtyAutosaveIsBudgetedAndIdleDoesNotScan();
    testAutosaveBudgetAndLocalDynamicBlockers();
    testFailedAutosaveRetainsAndMergesNewerPatch();
    testRepeatedPumpDoesNotRescanTimeSensitiveChunks();
    testReacquiredDemandCannotBypassAutosaveTick();
    testCooldownPromotionConsumesCandidateBudget();
    testRepeatedFailurePatchIsDeduplicatedByCell();
    testFrozenPumpExpiresNoDemandCooldownAndFailure();
    testDynamicBlockerBacklogRefillsWithinBudget();
    testTransientBlockEventDoesNotBecomeStickyBlocker();
    testCloseDrainsPendingSnapshot();
    testCloseFailureAndLateReadReports();
    testManagedLightPlaceBreakAnvilRoundTripAndCleanReload();
    testManagedLightDelayedNeighborCompletion();
    testManagedLightBacklogAutosaveNonBlocking();
    testManagedLightUnloadReloadStaleEpochIsolation();
    if (argc >= 3) {
      testRealWorldDiskCycle(argv[2], scratch);
      testRealWorldLightPersistenceCycle(argv[2], scratch);
      testRestoredPersistentTicketsDriveResidency(argv[2], scratch);
      testCloseHeldTicketReal(argv[2], scratch);
      testRealEntityRegionRejectsInstall(argv[2], scratch);
    }
    if (argc >= 4)
      testRealTickDiskCycle(argv[3], scratch);
    testInjectedStorePistonProgressMarksDirty();
    testLifecycleHistoryReclaimed();
    testTntAndItemShareEntityStoreLifecycle();
    testFallingBlockEntityStoreLifecycle();
    testUnsupportedFallingBlockDoesNotOverwrite();
    testMinecartEntityStoreLifecycle();
    testInjectedItemSaveFailureKeepsResident();
    testCrossChunkItemMigrationPersistsOnce();
    testManagedEntityTickDetectsExternalSectionMove();
    testManagedEntityVelocityCrossesChunkBoundary();
    testPistonMovesEntityAfterEntityTick();
    testCrossChunkItemMigrationIgnoresStaleTargetAck();
    testCrossChunkItemMigrationReturnsToSource();
    testCrossChunkItemMigrationRemovedBeforeCommit();
    testCrossChunkItemMigrationFailureKeepsSource();
    testMigrationWorkIsEventDrivenWithUnreadyTargets();
    testCrossChunkItemMigrationReloadsOnce();
    testDuplicateItemUuidRejectsSecondChunk();
    if (argc >= 5)
      testRealMovingPistonLifecycle(argv[4], scratch);
    if (argc == 6) {
      testRealItemEntityLifecycle(argv[5], scratch);
      testRealItemEntityCrossRegionMigration(argv[5], scratch);
    }
    std::cout << "world chunk manager tests passed"
              << (argc >= 3 ? " (with real World disk fixture)" : "")
              << std::endl;
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "world chunk manager test failure: " << error.what() << '\n';
    return 1;
  }
}
