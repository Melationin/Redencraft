#include "redstone/chunk_lifecycle.hpp"
#include "redstone/worldgen/proto_chunk.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
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

std::vector<std::uint8_t> makeChunkNbt(const ChunkTicketPos &position) {
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
  add(section, "Y", number(1, 0));
  add(section, "block_states", std::move(states));
  add(root.value, "sections", list(10, {std::move(section)}));
  return nbt::writeRoot(root);
}

std::string blockNameAt(const LifecycleLoadedChunk &loaded,
                        std::size_t index) {
  if (loaded.prebuiltSections && loaded.prebuiltSectionCount > 0) {
    // 生成路径零拷贝晋升：区段不再编码成 terrain::Section，直接读原型区段。
    const BlockSection &section = (*loaded.prebuiltSections)[0];
    return section.getBlockState(static_cast<int>(index & 15),
                                 static_cast<int>((index >> 8) & 15),
                                 static_cast<int>((index >> 4) & 15))
        .name();
  }
  if (loaded.sections.empty() || !loaded.sections[0].blockStates)
    return {};
  const auto &states = *loaded.sections[0].blockStates;
  const std::size_t paletteIndex = states.cell(index);
  if (paletteIndex >= states.palette.size())
    return {};
  return states.palette[paletteIndex].name;
}

struct BlockingStore final : RawChunkStore {
  StoreReadResult readChunk(const ChunkTicketPos &position) override {
    std::unique_lock lock(mutex);
    enter("read", position);
    ++active;
    maxActive = std::max(maxActive, active);
    while (blockReads) {
      readEntered = true;
      changed.notify_all();
      if (changed.wait_for(lock, std::chrono::seconds(5),
                           [this] { return !blockReads; }))
        break;
      --active;
      return {StoreReadStatus::Io, {}, "blocked read timed out"};
    }
    const StoreReadStatus status =
        statuses.contains(position) ? statuses.at(position)
                                    : StoreReadStatus::Found;
    StoreReadResult result;
    result.status = status;
    if (status == StoreReadStatus::Found)
      result.nbt = makeChunkNbt(position);
    else
      result.detail = "injected read status";
    --active;
    return result;
  }

  StoreWriteResult writeChunk(const ChunkTicketPos &position,
                              std::span<const std::uint8_t> bytes,
                              int) override {
    std::unique_lock lock(mutex);
    enter("write", position);
    ++writeCalls;
    writePayloads.emplace_back(bytes.begin(), bytes.end());
    ++active;
    maxActive = std::max(maxActive, active);
    while (blockWrites) {
      writeEntered = true;
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
      result.detail = "injected write failure";
    } else {
      result.wrote = true;
    }
    --active;
    return result;
  }

  void waitForRead(int count = 1) {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, std::chrono::seconds(5), [&] {
              return readCalls >= count;
            }),
            "timed out waiting for worker read");
  }

  void waitForWrite(int count = 1) {
    std::unique_lock lock(mutex);
    require(changed.wait_for(lock, std::chrono::seconds(5), [&] {
              return writeCalls >= count;
            }),
            "timed out waiting for worker write");
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

  void setReadStatus(const ChunkTicketPos &position, StoreReadStatus value) {
    std::lock_guard lock(mutex);
    statuses[position] = value;
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

  int maximumActive() const {
    std::lock_guard lock(mutex);
    return maxActive;
  }

  std::vector<std::vector<std::uint8_t>> writes() const {
    std::lock_guard lock(mutex);
    return writePayloads;
  }

  mutable std::mutex mutex;
  std::condition_variable changed;
  bool blockReads = false;
  bool blockWrites = false;
  bool readEntered = false;
  bool writeEntered = false;
  int failWrites = 0;
  int active = 0;
  int maxActive = 0;
  int readCalls = 0;
  int writeCalls = 0;
  std::map<ChunkTicketPos, StoreReadStatus> statuses;
  std::vector<std::string> operations;
  std::vector<std::vector<std::uint8_t>> writePayloads;

private:
  void enter(const char *kind, const ChunkTicketPos &position) {
    operations.push_back(std::string(kind) + " " + std::to_string(position.x) +
                         "," + std::to_string(position.z));
    if (std::string(kind) == "read")
      ++readCalls;
  }
};

struct Installs {
  int count = 0;
  int evicts = 0;
  int declineEvictions = 0;
  bool accept = true;
  std::vector<std::uint64_t> epochs;
  std::vector<std::string> initialBlocks;
  std::vector<std::uint64_t> evictedEpochs;
};

ChunkResidentCallbacks callbacksFor(Installs &installs) {
  ChunkResidentCallbacks callbacks;
  callbacks.install = [&](LifecycleLoadedChunk &&loaded) {
    ++installs.count;
    installs.epochs.push_back(loaded.epoch);
    installs.initialBlocks.push_back(blockNameAt(loaded, 0));
    return installs.accept;
  };
  callbacks.evict = [&](const ChunkTicketPos &, std::uint64_t epoch) {
    ++installs.evicts;
    installs.evictedEpochs.push_back(epoch);
    if (installs.declineEvictions != 0) {
      --installs.declineEvictions;
      return false;
    }
    return true;
  };
  return callbacks;
}

template <class Predicate>
bool waitUntil(ChunkLifecycleController &controller, Predicate predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    controller.drain();
    if (predicate())
      return true;
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

constexpr int LoadLevel = ticket_levels::FullChunkLevel;
constexpr int NoDemand = ticket_levels::NoTicketSource;

void testReadFailuresAndResident() {
  auto store = std::make_shared<BlockingStore>();
  const ChunkTicketPos missing{0, 0};
  const ChunkTicketPos unsupported{1, 0};
  const ChunkTicketPos corrupt{2, 0};
  const ChunkTicketPos valid{3, 0};
  store->statuses[missing] = StoreReadStatus::Missing;
  store->statuses[unsupported] = StoreReadStatus::Unsupported;
  store->statuses[corrupt] = StoreReadStatus::Corrupt;
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));

  require(controller.updateDemand(missing, LoadLevel), "accept missing demand");
  require(waitUntil(controller, [&] {
            auto status = controller.status(missing);
            return status &&
                   status->state == ChunkResidencyState::Missing;
          }),
          "missing chunk becomes Missing");
  require(controller.updateDemand(unsupported, LoadLevel),
          "accept unsupported demand");
  require(waitUntil(controller, [&] {
            auto status = controller.status(unsupported);
            return status && status->state == ChunkResidencyState::Failed &&
                   status->failure == ChunkLoadFailure::Unsupported;
          }),
          "unsupported chunk becomes explicit Failed");
  require(controller.updateDemand(corrupt, LoadLevel), "accept corrupt demand");
  require(waitUntil(controller, [&] {
            auto status = controller.status(corrupt);
            return status && status->state == ChunkResidencyState::Failed &&
                   status->failure == ChunkLoadFailure::Corrupt;
          }),
          "corrupt chunk becomes explicit Failed");
  const std::array<ChunkLevelChange, 1> validDemand{{
      {valid, NoDemand, LoadLevel},
  }};
  require(controller.applyLoadingChanges(validDemand),
          "accept loadingChanges demand");
  require(waitUntil(controller, [&] {
            auto status = controller.status(valid);
            return status &&
                   status->state == ChunkResidencyState::Resident;
          }),
          "valid chunk reaches Resident after install");
  require(installs.count == 1 && installs.initialBlocks[0] == "minecraft:stone",
          "failure paths never install an empty chunk");
  const std::array<ChunkLevelChange, 1> validRelease{{
      {valid, LoadLevel, NoDemand},
  }};
  controller.applyLoadingChanges(validRelease);
  require(controller.status(valid)->state == ChunkResidencyState::Absent &&
              installs.evicts == 1,
          "clean no-demand chunk is evicted");
  const ChunkTicketPos rejected{5, 0};
  installs.accept = false;
  require(controller.updateDemand(rejected, LoadLevel),
          "accept demand whose install will be rejected");
  require(waitUntil(controller, [&] {
            auto status = controller.status(rejected);
            return status && status->state == ChunkResidencyState::Failed &&
                   status->failure ==
                       ChunkLoadFailure::InstallRejected;
          }),
          "consumer rejection never becomes Resident");
  controller.close();
  require(store->maximumActive() == 1,
          "raw store calls remain serialized");
}

void testRetiredCoordinatesDoNotAccumulate() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));

  // A read that is blocked in the worker is cancelled, then released. The
  // control queue must retire both generations and both worker maps.
  store->setBlockReads(true);
  for (int i = 0; i < 32; ++i)
    require(controller.updateDemand({1000 + i, 40}, LoadLevel),
            "start blocked historical read");
  store->waitForRead();
  for (int i = 0; i < 32; ++i)
    controller.updateDemand({1000 + i, 40}, NoDemand);
  store->releaseReads();
  require(waitUntil(controller, [&] {
            return controller.activeEntryCountForTest() == 0;
          }),
          "cancelled blocked reads retire main entries");
  require(controller.workerLedgerCountForTest() == 0,
          "cancelled blocked reads retire worker ledgers");

  // Do not withdraw the next group until the worker has actually reported a
  // Missing/Failed status for the demanded coordinate.
  for (int i = 0; i < 64; ++i) {
    const ChunkTicketPos position{1300 + i, -40};
    store->setReadStatus(position, i % 2 == 0 ? StoreReadStatus::Missing
                                               : StoreReadStatus::Unsupported);
    require(controller.updateDemand(position, LoadLevel),
            "start failing historical read");
    require(waitUntil(controller, [&] {
              auto status = controller.status(position);
              return status && (status->state == ChunkResidencyState::Missing ||
                                status->state == ChunkResidencyState::Failed);
            }),
            "failing historical read reaches diagnostic state");
  }
  for (int i = 0; i < 64; ++i)
    controller.updateDemand({1300 + i, -40}, NoDemand);
  require(waitUntil(controller, [&] {
            return controller.activeEntryCountForTest() == 0;
          }),
          "failed reads retire after demand withdrawal");
  require(controller.workerLedgerCountForTest() == 0 &&
              controller.templateCountForTest() == 0,
          "failed reads retire worker ledgers and templates");

  // Successful resident unloads must also release every historical node.
  for (int i = 0; i < 32; ++i) {
    const ChunkTicketPos position{1600 + i, 40};
    require(controller.updateDemand(position, LoadLevel),
            "load successful historical chunk");
    require(waitUntil(controller, [&] {
              auto status = controller.status(position);
              return status &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "historical chunk reaches Resident");
    controller.updateDemand(position, NoDemand);
  }
  require(waitUntil(controller, [&] {
            return controller.activeEntryCountForTest() == 0;
          }),
          "successful unloads retire main entries");
  require(controller.workerLedgerCountForTest() == 0 &&
              controller.templateCountForTest() == 0,
          "successful unloads retire worker ledgers and templates");
  controller.close();
}

void testSamePositionGenerationsAreIsolated() {
  auto store = std::make_shared<BlockingStore>();
  store->setBlockReads(true);
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{1500, 7};
  require(controller.updateDemand(position, LoadLevel), "first blocked read");
  store->waitForRead();
  controller.updateDemand(position, NoDemand);
  store->releaseReads();
  require(controller.updateDemand(position, LoadLevel), "readd same position");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->state == ChunkResidencyState::Resident &&
                   installs.count == 1;
          }),
          "same-position readd installs only newest generation");
  require(installs.epochs.size() == 1 && installs.epochs[0] >= 3,
          "new generation receives a monotonic lifecycle token");
  // A Drop for the older generation must not remove the current template.
  require(store->writes().empty(), "readd does not write before demand drops");
  controller.updateDemand(position, NoDemand);
  require(waitUntil(controller, [&] {
            return controller.activeEntryCountForTest() == 0 &&
                   controller.workerLedgerCountForTest() == 0 &&
                   controller.templateCountForTest() == 0;
          }),
          "newest generation drops only its exact template");
  controller.close();
}

void testReadInvalidatedByReadd() {
  auto store = std::make_shared<BlockingStore>();
  store->setBlockReads(true);
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{4, -3};
  require(controller.updateDemand(position, LoadLevel), "start blocked read");
  store->waitForRead();
  require(controller.status(position)->epoch == 1 &&
              controller.status(position)->state ==
                  ChunkResidencyState::Reading,
          "first generation is reading");
  controller.updateDemand(position, NoDemand);
  require(controller.status(position)->state == ChunkResidencyState::Absent,
          "demand loss invalidates the in-flight read");
  store->releaseReads();
  require(controller.updateDemand(position, LoadLevel), "readd chunk");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->state == ChunkResidencyState::Resident &&
                   installs.count == 1;
          }),
          "readd installs only the new generation");
  require(controller.status(position)->epoch == 3 &&
              installs.epochs[0] == 3,
          "old read completion cannot install the new generation");
  controller.close();
}

void testDirtyVersionAndSavingReadd() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{7, 8};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed");

  const std::vector<BlockEdit> edits{{0, 0, Blocks::AIR.defaultState()}};
  store->setBlockWrites(true);
  require(controller.requestSave(position, 1, edits), "queue save v1");
  store->waitForWrite();
  require(controller.markDirty(position, 2), "mark dirty v2 during save v1");
  store->releaseWrites();
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status &&
                   status->state == ChunkResidencyState::Resident &&
                   status->dirtyVersion == 2 &&
                   status->savedDirtyVersion == 1;
          }),
          "save v1 ack does not clear dirty v2");
  require(controller.requestSave(position, 2, {}), "queue save v2");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status &&
                   status->state == ChunkResidencyState::Resident &&
                   status->savedDirtyVersion == 2;
          }),
          "save v2 catches up cleanly");

  store->setBlockWrites(true);
  require(controller.requestSave(position, 3, edits), "queue save before remove");
  store->waitForWrite(3);
  controller.updateDemand(position, NoDemand);
  require(controller.status(position)->state == ChunkResidencyState::Saving,
          "demand loss waits for in-flight save");
  require(controller.updateDemand(position, LoadLevel),
          "ticket return cancels pending eviction");
  store->releaseWrites();
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status &&
                   status->state == ChunkResidencyState::Resident &&
                   status->savedDirtyVersion == 3;
          }),
          "returned demand keeps resident after save");
  require(installs.evicts == 0, "ticket return cancels eviction");
  controller.close();
}

void testSaveFailureRetryThenEvict() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{-9, 2};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed");

  store->failNextWrites(1);
  const std::vector<BlockEdit> edits{{0, 0, Blocks::AIR.defaultState()}};
  require(controller.requestSave(position, 1, edits), "queue failing save");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->state == ChunkResidencyState::Resident &&
                   status->failure == ChunkLoadFailure::Io &&
                   status->dirtyVersion == 1 &&
                   status->savedDirtyVersion == 0;
          }),
          "save failure remains Resident and dirty");
  controller.updateDemand(position, NoDemand);
  require(controller.status(position)->state ==
                  ChunkResidencyState::EvictPending &&
              installs.evicts == 0,
          "failed dirty save cannot evict");
  require(controller.requestSave(position, 1, {}), "retry from retained root");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->state == ChunkResidencyState::Absent &&
                   status->savedDirtyVersion == 1 &&
                   installs.evicts == 1;
          }),
          "successful retry persists dirty data before eviction");
  controller.close();
}

void testOwnedRootSave() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{30, 31};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed before owned save");

  DecodedChunk decoded = decodeChunkNbt(makeChunkNbt(position));
  require(decoded.status == DecodeStatus::Ok,
          "owned save fixture decodes");
  const std::vector<BlockEdit> edits{{0, 0, Blocks::AIR.defaultState()}};
  require(patchBlockStates(decoded.root, edits).edited,
          "owned save fixture is patched before handoff");
  require(controller.requestSaveOwnedRoot(position, 1,
                                           std::move(decoded.root)),
          "owned root moves into save queue");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->savedDirtyVersion == 1 &&
                   status->state == ChunkResidencyState::Resident;
          }),
          "owned root save completes");
  auto writes = store->writes();
  require(writes.size() == 1, "owned root produced one write");
  DecodedChunk saved = decodeChunkNbt(writes[0]);
  const auto &states = *saved.sections[0].blockStates;
  require(states.palette[states.cell(0)].name == "minecraft:air",
          "owned root write contains the moved edit");
  controller.close();
}

void testExactSnapshotVersion() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{40, 1};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed");
  require(controller.markDirty(position, 2), "mark dirty snapshot v2");
  store->setBlockWrites(true);
  require(controller.requestSave(
              position, 1,
              std::vector<BlockEdit>{{0, 0, Blocks::AIR.defaultState()}}),
          "older v1 snapshot is still allowed to persist");
  store->waitForWrite();
  controller.updateDemand(position, NoDemand);
  store->releaseWrites();
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status &&
                   status->state == ChunkResidencyState::Resident &&
                   status->dirtyVersion == 2 &&
                   status->savedDirtyVersion == 1;
          }),
          "v1 ack does not clear dirty v2");
  require(installs.evicts == 0,
          "v1 ack cannot evict while v2 remains dirty");
  require(!controller.requestSave(position, 1, {}),
          "successfully written snapshot v1 cannot be replayed");
  controller.close();
}

void testOwnedRootCannotRegressWrittenVersion() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{41, 2};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed");

  nbt::RootTag newer = decodeChunkNbt(makeChunkNbt(position)).root;
  require(patchBlockStates(
              newer, std::vector<BlockEdit>{{0, 0, Blocks::AIR.defaultState()}})
              .edited,
          "prepare v2 owned root");
  require(controller.requestSaveOwnedRoot(position, 2, std::move(newer)),
          "persist owned root v2");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->savedDirtyVersion == 2 &&
                   status->state == ChunkResidencyState::Resident;
          }),
          "owned root v2 reaches disk");

  nbt::RootTag older = decodeChunkNbt(makeChunkNbt(position)).root;
  require(!controller.requestSaveOwnedRoot(position, 1, std::move(older)),
          "owned root v1 is rejected after v2 success");
  const auto writes = store->writes();
  require(writes.size() == 1, "rejected v1 performs no disk write");
  DecodedChunk disk = decodeChunkNbt(writes[0]);
  const auto &states = *disk.sections[0].blockStates;
  require(states.palette[states.cell(0)].name == "minecraft:air",
          "disk remains at the newer v2 payload");
  controller.close();
}

void testInvalidOwnedRootKeepsTemplate() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{42, 3};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed");

  auto waitRejected = [&] {
    return waitUntil(controller, [&] {
      auto status = controller.status(position);
      return status && status->state == ChunkResidencyState::Resident &&
             status->failure == ChunkLoadFailure::Io &&
             status->savedDirtyVersion == 0;
    });
  };

  nbt::RootTag wrongPosition = decodeChunkNbt(makeChunkNbt(position)).root;
  wrongPosition.value.get("xPos")->number = 999;
  require(controller.requestSaveOwnedRoot(position, 1,
                                           std::move(wrongPosition)),
          "wrong-position owned root request is accepted then worker-rejected");
  require(waitRejected(), "wrong-position owned root is rejected");

  nbt::RootTag wrongVersion = decodeChunkNbt(makeChunkNbt(position)).root;
  wrongVersion.value.get("DataVersion")->number = 9999;
  require(controller.requestSaveOwnedRoot(position, 1,
                                           std::move(wrongVersion)),
          "bad-version owned root retry is accepted at the same snapshot");
  require(waitRejected(), "bad-version owned root is rejected");

  nbt::RootTag wrongStatus = decodeChunkNbt(makeChunkNbt(position)).root;
  wrongStatus.value.get("Status")->text = "minecraft:features";
  require(controller.requestSaveOwnedRoot(position, 1,
                                           std::move(wrongStatus)),
          "non-FULL owned root retry is accepted at the same snapshot");
  require(waitRejected(), "non-FULL owned root is rejected");

  nbt::RootTag wrongSchema = decodeChunkNbt(makeChunkNbt(position)).root;
  wrongSchema.value.get("sections")->list[0]
      .get("block_states")->get("palette")->list[0]
      .get("Name")->text = "example:unknown_block";
  require(controller.requestSaveOwnedRoot(position, 1,
                                           std::move(wrongSchema)),
          "unsupported-schema owned root retry is accepted");
  require(waitRejected(), "unsupported-schema owned root is rejected");
  require(store->writes().empty(),
          "no rejected owned root reached disk");

  require(controller.requestSave(
              position, 1,
              std::vector<BlockEdit>{{0, 0, Blocks::AIR.defaultState()}}),
          "edit retry reuses the original worker template");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->savedDirtyVersion == 1;
          }),
          "retry after invalid owned roots succeeds");
  require(store->writes().size() == 1,
          "only the valid edit retry writes disk");
  controller.close();
}

void testEvictionRetry() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{43, 4};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed");
  installs.declineEvictions = 1;
  controller.updateDemand(position, NoDemand);
  require(controller.status(position)->state ==
                  ChunkResidencyState::EvictPending &&
              installs.evicts == 1,
          "declined eviction remains pending");
  require(controller.retryEviction(position),
          "explicit retry accepts released eviction");
  require(controller.status(position)->state == ChunkResidencyState::Absent &&
              installs.evicts == 2,
          "retry advances the pending eviction to Absent");
  controller.close();
}

void testEvictionRetryAfterClose() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{44, 5};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed");
  installs.declineEvictions = 1;
  controller.updateDemand(position, NoDemand);
  require(controller.status(position)->state ==
                  ChunkResidencyState::EvictPending,
          "eviction is pending before close");
  controller.close();
  require(controller.retryEviction(position),
          "post-close retry completes resident cleanup without enqueue");
  require(controller.status(position)->state ==
              ChunkResidencyState::Absent,
          "post-close retry reaches Absent");
  (void)controller.drain();
}

void testSameChunkWriteOrder() {
  auto store = std::make_shared<BlockingStore>();
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{11, 12};
  require(controller.updateDemand(position, LoadLevel), "load resident chunk");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "resident chunk installed");

  store->setBlockWrites(true);
  require(controller.requestSave(
              position, 1,
              std::vector<BlockEdit>{{0, 0, Blocks::AIR.defaultState()}}),
          "queue first same-chunk write");
  store->waitForWrite();
  require(controller.requestSave(
              position, 2,
              std::vector<BlockEdit>{{0, 0, Blocks::STONE.defaultState()}}),
          "queue second same-chunk write");
  store->releaseWrites();
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status &&
                   status->state == ChunkResidencyState::Resident &&
                   status->savedDirtyVersion == 2;
          }),
          "both same-chunk writes complete in order");
  auto payloads = store->writes();
  require(payloads.size() == 2, "two disk writes were observed");
  DecodedChunk first = decodeChunkNbt(payloads[0]);
  DecodedChunk second = decodeChunkNbt(payloads[1]);
  require(first.status == DecodeStatus::Ok &&
              second.status == DecodeStatus::Ok,
          "both written NBT payloads decode");
  auto nameAt = [](const DecodedChunk &chunk) {
    const auto &states = *chunk.sections[0].blockStates;
    return states.palette[states.cell(0)].name;
  };
  require(nameAt(first) == "minecraft:air" &&
              nameAt(second) == "minecraft:stone",
          "the newer generation write is the last disk payload");
  require(store->maximumActive() == 1,
          "same-region writes never overlap");
  controller.close();
}

void testCloseDrainsAndRejects() {
  auto store = std::make_shared<BlockingStore>();
  store->setBlockReads(true);
  Installs installs;
  ChunkLifecycleController controller(store, callbacksFor(installs));
  const ChunkTicketPos position{20, -20};
  require(controller.updateDemand(position, LoadLevel), "start blocked load");
  store->waitForRead();
  auto closing = std::async(std::launch::async,
                            [&] { controller.close(); });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (controller.accepting() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(!controller.accepting(), "close rejects new requests");
  require(!controller.updateDemand(ChunkTicketPos{21, -20}, LoadLevel),
          "new demand is rejected while closing");
  store->releaseReads();
  closing.get();
  require(controller.drain() != 0 &&
              controller.status(position)->state ==
                  ChunkResidencyState::Resident,
          "close drains accepted read and keeps completion for main");
  controller.close();
}

void testRealDiskFixture(const std::filesystem::path &sourceRegion,
                         const std::filesystem::path &scratch) {
  const std::filesystem::path region = scratch / "real-region";
  std::filesystem::create_directories(region);
  std::filesystem::copy_file(sourceRegion / "r.-1.-1.mca",
                             region / "r.-1.-1.mca",
                             std::filesystem::copy_options::overwrite_existing);
  const ChunkTicketPos position{-1, -1};

  {
    Installs installs;
    ChunkLifecycleController controller(region, callbacksFor(installs));
    require(controller.updateDemand(position, LoadLevel), "load real fixture");
    require(waitUntil(controller, [&] {
              auto status = controller.status(position);
              return status && status->state ==
                                   ChunkResidencyState::Resident;
            }),
            "real region load reaches Resident");
    require(installs.count == 1 &&
                installs.initialBlocks[0] == "minecraft:stone",
            "real fixture decoded the expected stone template");
    require(controller.requestSave(
                position, 1,
                std::vector<BlockEdit>{{0, 0, Blocks::AIR.defaultState()}}),
            "save real fixture edit");
    require(waitUntil(controller, [&] {
              auto status = controller.status(position);
              return status && status->savedDirtyVersion == 1 &&
                     status->state == ChunkResidencyState::Resident;
            }),
            "real fixture edit saved");
    controller.updateDemand(position, NoDemand);
    require(controller.status(position)->state == ChunkResidencyState::Absent,
            "real fixture clean chunk evicted");
    controller.close();
    controller.drain();
  }

  {
    Installs installs;
    ChunkLifecycleController controller(region, callbacksFor(installs));
    require(controller.updateDemand(position, LoadLevel),
            "reload real fixture after save");
    require(waitUntil(controller, [&] {
              auto status = controller.status(position);
              return status && status->state ==
                                   ChunkResidencyState::Resident;
            }),
            "saved real fixture reloads");
    require(installs.count == 1 &&
                installs.initialBlocks[0] == "minecraft:air",
            "disk read after save sees the edited block");
    controller.updateDemand(position, NoDemand);
    controller.close();
    controller.drain();
  }
}

void testLightPatchRoundTrip() {
  auto store = std::make_shared<BlockingStore>();
  const ChunkTicketPos position{3, -2};
  bool sawLightOn = false;
  ChunkResidentCallbacks callbacks;
  callbacks.install = [&](LifecycleLoadedChunk &&loaded) {
    sawLightOn = loaded.isLightOn;
    return true;
  };
  callbacks.evict = [](const ChunkTicketPos &, std::uint64_t) { return true; };

  ChunkLifecycleController controller(store, callbacks);
  require(controller.updateDemand(position, LoadLevel),
          "load for light patch test");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->state == ChunkResidencyState::Resident;
          }),
          "light patch test reaches Resident");
  require(sawLightOn,
          "ChunkLifecycleController exposes decoded isLightOn=true flag");

  ChunkLifecycleController::ChunkSavePayload enableLight;
  ChunkLightPatch onPatch;
  onPatch.isLightOn = true;
  SectionLightPatch sec0;
  sec0.y = 0;
  sec0.blockLight.emplace();
  sec0.blockLight->fill(0x77);
  sec0.skyLight.emplace();
  sec0.skyLight->fill(0xff);
  onPatch.sections.push_back(sec0);
  enableLight.light = onPatch;
  require(controller.requestSavePayload(position, 1, std::move(enableLight)),
          "save payload with light patch succeeds");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->savedDirtyVersion == 1;
          }),
          "light-on payload committed to store");
  auto writes = store->writes();
  require(!writes.empty(), "light-on write produced bytes");
  {
    auto decoded = decodeChunkNbt(writes.back());
    require(decoded.status == DecodeStatus::Ok && decoded.isLightOn,
            "persisted chunk retains isLightOn=true");
    require(decoded.sections.size() == 1 &&
                decoded.sections[0].blockLight &&
                (*decoded.sections[0].blockLight)[0] == 0x77 &&
                decoded.sections[0].skyLight &&
                (*decoded.sections[0].skyLight)[0] == 0xff,
            "persisted chunk carries BlockLight and SkyLight nibbles");
  }

  ChunkLifecycleController::ChunkSavePayload disableLight;
  ChunkLightPatch offPatch;
  offPatch.isLightOn = false;
  disableLight.light = offPatch;
  require(controller.requestSavePayload(position, 2, std::move(disableLight)),
          "save payload clearing light succeeds");
  require(waitUntil(controller, [&] {
            auto status = controller.status(position);
            return status && status->savedDirtyVersion == 2;
          }),
          "light-off payload committed to store");
  writes = store->writes();
  {
    auto decoded = decodeChunkNbt(writes.back());
    require(decoded.status == DecodeStatus::Ok && !decoded.isLightOn,
            "persisted light-off chunk resets isLightOn=false");
    require(decoded.sections.size() == 1 &&
                !decoded.sections[0].blockLight &&
                !decoded.sections[0].skyLight,
            "persisted light-off chunk strips stale BlockLight and SkyLight");
  }
  controller.close();
  controller.drain();
}

// 世界生成交接：磁盘缺失 -> requestGeneration -> 生成载荷 + 完整 NBT 模板 -> 普通 install ->
// 模板可直接承接后续增量存盘；撤销需求后的迟到结果必须被 epoch 拒绝。
void testGeneratedChunkHandoffAndTemplate() {
  auto store = std::make_shared<BlockingStore>();
  const ChunkTicketPos pos{4, 0};
  store->statuses[pos] = StoreReadStatus::Missing;
  Installs installs;
  ChunkResidentCallbacks callbacks = callbacksFor(installs);
  std::vector<std::uint64_t> requested;
  callbacks.requestGeneration =
      [&](const ChunkTicketPos &position, std::uint64_t epoch,
          int loadingLevel) {
        require(position == pos || position == ChunkTicketPos{5, 0},
                "generation request position");
        require(loadingLevel == LoadLevel, "generation request level");
        requested.push_back(epoch);
        return true;
      };
  ChunkLifecycleController controller(store, callbacks);
  require(controller.updateDemand(pos, LoadLevel), "generation demand");
  require(waitUntil(controller, [&] { return !requested.empty(); }),
          "missing read must request generation");
  require(controller.status(pos)->state == ChunkResidencyState::Reading,
          "generation keeps the read pending");

  const std::uint64_t epoch = requested.front();
  worldgen::ProtoChunk proto(pos, -64, 384);
  proto.setBlockState(0, -64, 0, BlockState("minecraft:stone"));
  proto.setPersistedStatus(worldgen::ChunkStatusId::Full);
  proto.setLightCorrect(false);
  nbt::RootTag root = worldgen::encodeProtoChunkToNbt(proto);
  LifecycleLoadedChunk loaded =
      worldgen::promoteProtoChunkToLoaded(std::move(proto), epoch);
  require(loaded.generated, "promoted chunk is marked generated");
  require(loaded.entityData &&
              loaded.entityData->status ==
                  EntityChunkDataResult::Status::Empty,
          "generated chunk has an empty entity slot");
  require(controller.completeGeneratedChunk(pos, epoch, std::move(loaded),
                                            std::move(root)),
          "generated chunk accepted");
  require(waitUntil(controller, [&] { return installs.count == 1; }),
          "generated chunk installed");
  require(installs.initialBlocks.front() == "minecraft:stone",
          "generated terrain reached the install callback");
  require(controller.templateCountForTest() == 1,
          "generated NBT template is owned by the I/O worker");

  // 模板可直接承接首次存盘：版本 1 被接受并写出完整 FULL 区块。
  require(controller.requestSavePayload(pos, 1, {}),
          "generated template accepts the first save");
  require(waitUntil(controller, [&] { return store->writeCalls >= 1; }),
          "generated chunk written by the I/O worker");
  const auto writes = store->writes();
  require(!writes.empty(), "generated write payload");
  const terrain::DecodedChunk decoded =
      terrain::decodeChunkNbt(std::span<const std::uint8_t>(writes.back()));
  require(decoded.status == terrain::DecodeStatus::Ok,
          "generated write decodes as a valid chunk");
  require(decoded.statusName == "minecraft:full",
          "generated write keeps FULL status");

  // 第二次交接（已是 Resident）必须被拒绝。
  require(!controller.completeGeneratedChunk(pos, epoch, LifecycleLoadedChunk{},
                                             nbt::RootTag{}),
          "second completion for a resident chunk is rejected");

  // 撤销需求后，迟到的生成结果必须被拒绝且不安装。
  const ChunkTicketPos latePos{5, 0};
  store->statuses[latePos] = StoreReadStatus::Missing;
  requested.clear();
  require(controller.updateDemand(latePos, LoadLevel), "late demand");
  require(waitUntil(controller, [&] { return !requested.empty(); }),
          "late generation requested");
  const std::uint64_t lateEpoch = requested.front();
  require(controller.updateDemand(latePos, NoDemand), "late demand retired");
  require(!controller.completeGeneratedChunk(latePos, lateEpoch,
                                             LifecycleLoadedChunk{},
                                             nbt::RootTag{}),
          "stale generated chunk is rejected");
  controller.drain();
  require(installs.count == 1, "stale generation never installs");
  controller.close();
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2 || argc == 3,
            "chunk_lifecycle_tests requires scratch [official region]");
    const std::filesystem::path scratch(argv[1]);
    std::filesystem::remove_all(scratch);
    std::filesystem::create_directories(scratch);
    testReadFailuresAndResident();
    testReadInvalidatedByReadd();
    testRetiredCoordinatesDoNotAccumulate();
    testSamePositionGenerationsAreIsolated();
    testDirtyVersionAndSavingReadd();
    testSaveFailureRetryThenEvict();
    testExactSnapshotVersion();
    testOwnedRootCannotRegressWrittenVersion();
    testInvalidOwnedRootKeepsTemplate();
    testEvictionRetry();
    testEvictionRetryAfterClose();
    testOwnedRootSave();
    testSameChunkWriteOrder();
    testCloseDrainsAndRejects();
    testLightPatchRoundTrip();
    testGeneratedChunkHandoffAndTemplate();
    if (argc == 3)
      testRealDiskFixture(argv[2], scratch);
    std::cout << "chunk lifecycle tests passed"
              << (argc == 3 ? " (with real disk fixture)" : "") << "\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "chunk lifecycle test failure: " << error.what() << '\n';
    return 1;
  }
}
