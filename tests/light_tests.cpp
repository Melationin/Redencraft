#include "redstone/engine.hpp"
#include "redstone/light.hpp"
#include "redstone/storage.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace redstone;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

LightSectionSnapshot airSection(int sectionY) {
  return LightSectionSnapshot(sectionY, true,
                              {lightPropertiesOf(airState())},
                              std::vector<std::uint16_t>{});
}

std::shared_ptr<const LightSectionSnapshot> sectionWithLayers(
    int sectionY, bool empty = false) {
  std::array<std::uint8_t, kLightVolume> sky{};
  std::array<std::uint8_t, kLightVolume> block{};
  for (int y = 0; y < 16; ++y)
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x) {
        const auto index = lightIndex(x, y, z);
        sky[index] = static_cast<std::uint8_t>((x + y + z) & 15);
        block[index] = (x == 1 && y == 2 && z == 3) ? 12 : 0;
      }
  std::array<LightLayerSnapshot, kLightLayerCount> layers = {
      LightLayerSnapshot::mixed(block), LightLayerSnapshot::mixed(sky)};
  std::vector<std::uint16_t> cells(kLightVolume, 0);
  cells[lightIndex(1, 2, 3)] = 1;
  return std::make_shared<const LightSectionSnapshot>(
      sectionY, empty,
      std::vector<BlockLightProperties>{lightPropertiesOf(airState()),
                                        lightPropertiesOf(Blocks::REDSTONE_LAMP
                                                              .defaultState()
                                                              .with("lit", "true"))},
      std::move(cells), layers);
}

std::vector<std::string> splitTabs(const std::string &line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  for (;;) {
    const auto tab = line.find('\t', start);
    fields.push_back(line.substr(
        start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos)
      return fields;
    start = tab + 1;
  }
}

LightChunkInput chunkFixture(int chunkX, int chunkZ, int minBlockSectionY,
                             int blockSectionCount) {
  LightChunkInput input;
  input.chunkX = chunkX;
  input.chunkZ = chunkZ;
  input.minBlockSectionY = minBlockSectionY;
  input.blockSectionCount = blockSectionCount;
  input.sections.push_back(
      std::make_shared<const LightSectionSnapshot>(airSection(
          minBlockSectionY - 1)));
  for (int i = 0; i < blockSectionCount; ++i) {
    if (i == 0)
      input.sections.push_back(sectionWithLayers(minBlockSectionY));
    else
      input.sections.push_back(std::make_shared<const LightSectionSnapshot>(
          airSection(minBlockSectionY + i)));
  }
  input.sections.push_back(std::make_shared<const LightSectionSnapshot>(
      airSection(minBlockSectionY + blockSectionCount)));
  return input;
}

LightChunkInput emptyChunkFixture(int chunkX, int chunkZ,
                                  int minBlockSectionY,
                                  int blockSectionCount) {
  LightChunkInput input;
  input.chunkX = chunkX;
  input.chunkZ = chunkZ;
  input.minBlockSectionY = minBlockSectionY;
  input.blockSectionCount = blockSectionCount;
  input.sections.push_back(std::make_shared<const LightSectionSnapshot>(
      airSection(minBlockSectionY - 1)));
  for (int section = 0; section < blockSectionCount; ++section)
    input.sections.push_back(std::make_shared<const LightSectionSnapshot>(
        airSection(minBlockSectionY + section)));
  input.sections.push_back(std::make_shared<const LightSectionSnapshot>(
      airSection(minBlockSectionY + blockSectionCount)));
  return input;
}

LightChunkInput skyRoofChunk(int chunkX, int chunkZ) {
  auto input = emptyChunkFixture(chunkX, chunkZ, 0, 1);
  std::vector<std::uint16_t> cells(kLightVolume, 0);
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x)
      cells[lightIndex(x, 8, z)] = 1;
  input.sections[1] = std::make_shared<const LightSectionSnapshot>(
      0, false,
      std::vector<BlockLightProperties>{
          lightPropertiesOf(airState()),
          lightPropertiesOf(Blocks::STONE.defaultState())},
      std::move(cells));
  return input;
}

bool reportBlock(LightService &service, int chunkX, int chunkZ, Pos position,
                 const BlockState &oldState, const BlockState &newState) {
  LightBlockChange change;
  change.chunkX = chunkX;
  change.chunkZ = chunkZ;
  change.sectionY = position.y >> 4;
  change.localIndex = static_cast<std::uint16_t>(
      lightIndex(position.x, position.y, position.z));
  change.oldProperties = lightPropertiesOf(oldState);
  change.newProperties = lightPropertiesOf(newState);
  change.wasEmpty = false;
  change.isEmpty = false;
  return service.reportBlockChange(change);
}

int blockLightAt(const std::shared_ptr<const LightChunkSnapshot> &snapshot,
                 int x, int y, int z) {
  if (!snapshot)
    return -1;
  const LightSectionSnapshot *section = snapshot->section(y >> 4);
  return section ? section->light(LightLayer::Block, x & 15, y & 15, z & 15)
                 : -1;
}

int skyLightAt(const std::shared_ptr<const LightChunkSnapshot> &snapshot,
               int x, int y, int z) {
  return snapshot ? snapshot->lightValue(LightLayer::Sky, x, y, z) : -1;
}

void dataLayerLayout() {
  require(kLightLayerBytes == 2048, "DataLayer is 2048 bytes");
  require(kLightVolume == 4096, "a section has 4096 cells");
  std::array<std::uint8_t, kLightVolume> values{};
  for (int y = 0; y < 16; ++y)
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x)
        values[lightIndex(x, y, z)] =
            static_cast<std::uint8_t>((x * 3 + y * 5 + z * 7) & 15);
  const auto mixed = LightLayerSnapshot::mixed(values);
  require(mixed.isMixed() && mixed.packedData() != nullptr &&
              mixed.packedByteCount() == 2048,
          "mixed layer owns packed nibbles");
  for (int y = 0; y < 16; ++y)
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x)
        require(mixed.get(x, y, z) == values[lightIndex(x, y, z)],
                "DataLayer uses y<<8|z<<4|x");
  const std::size_t odd = lightIndex(1, 2, 3);
  require((mixed.packedData()[odd >> 1] >> 4) == values[odd],
          "odd indices use the high nibble");
  const std::size_t even = lightIndex(0, 2, 3);
  require((mixed.packedData()[even >> 1] & 15) == values[even],
          "even indices use the low nibble");
  std::array<std::uint8_t, kLightLayerBytes> packed{};
  std::copy_n(mixed.packedData(), packed.size(), packed.begin());
  require(LightLayerSnapshot::mixedPacked(packed) == mixed,
          "2048-byte packed layer round-trips exactly");

  const auto missing = LightLayerSnapshot::missing();
  const auto zero = LightLayerSnapshot::uniform(0);
  const auto full = LightLayerSnapshot::uniform(15);
  require(missing.isMissing() && zero.isUniform() && full.isUniform(),
          "missing and uniform storage remain distinct");
  require(missing.packedByteCount() == 0 && zero.packedByteCount() == 0,
          "missing and uniform layers allocate no packed array");
  require(missing != zero && zero != full && full.get(7, 8, 9) == 15,
          "uniform values round-trip through the layer API");
  bool rejected = false;
  try {
    (void)LightLayerSnapshot::uniform(16);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "light values above 15 are rejected");
}

void sectionRangesAndSnapshots() {
  auto input = chunkFixture(-2, 3, -4, 3);
  require(input.minLightSectionY() == -5 && input.lightSectionCount() == 5,
          "light range is min block section - 1 with two padding sections");
  require(input.section(-5) && input.section(-3) && input.section(-1),
          "negative and boundary light sections resolve");
  require(input.section(-6) == nullptr && input.section(0) == nullptr,
          "sections outside the light range are absent");
  const auto &block = *input.sections[1];
  require(block.light(LightLayer::Sky, 1, 2, 3) == 6,
          "sky layer stores the mixed value");
  require(block.light(LightLayer::Block, 1, 2, 3) == 12,
          "block layer is independent from sky light");
  require(block.light(LightLayer::Sky, 2, 2, 3) == 7,
          "second layer value remains independent");
  require(input.sections[2]->isUniform() && input.sections[2]->cells.empty(),
          "uniform sections store one optical value without 4096 indices");
  bool rejectedUniformPalette = false;
  try {
    (void)LightSectionSnapshot(
        -2, false,
        {lightPropertiesOf(airState()),
         lightPropertiesOf(Blocks::STONE.defaultState())},
        {});
  } catch (const std::invalid_argument &) {
    rejectedUniformPalette = true;
  }
  require(rejectedUniformPalette,
          "uniform section cannot carry unused palette entries");

  LightService service(-4, 3, LightWorkerOptions{1000, 4096, false, false});
  const auto generation = service.submitChunk(std::move(input));
  require(generation != 0, "chunk snapshot is accepted");
  require(service.waitIdleForTesting(), "light worker finishes snapshot");
  service.drainPublished();
  auto snapshot = service.latestChunk(-2, 3);
  require(snapshot && snapshot->generation == generation,
          "published chunk keeps its generation");
  require(snapshot->skyLightComplete && snapshot->propagationComplete &&
              !snapshot->protocolReady,
          "completed light layers remain distinct from protocol readiness");
  require(snapshot->lightSectionCount() == 5,
          "published snapshot keeps all light padding sections");
  require(snapshot->section(-4)->light(LightLayer::Block, 1, 2, 3) == 15 &&
              snapshot->section(-4)->light(LightLayer::Sky, 1, 2, 3) == 0,
          "disabled sky dimension clears input sky data");
}

void opticalMetadata() {
  const auto &stone = lightPropertiesOf(Blocks::STONE.defaultState());
  const auto &glass = lightPropertiesOf(Blocks::GLASS.defaultState());
  const auto &lampOff = lightPropertiesOf(Blocks::REDSTONE_LAMP.defaultState());
  const auto lampOnState = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  const auto &lampOn = lightPropertiesOf(lampOnState);
  require(stone.supported && stone.emission == 0 && stone.dampening == 15 &&
              stone.canOcclude && !stone.useShapeForLightOcclusion,
          "stone metadata comes from the official table");
  require(glass.supported && glass.dampening == 0 && !glass.canOcclude &&
              glass.propagatesSkylightDown,
          "glass separates occlusion from dampening");
  require(lampOff.supported && lampOn.supported && lampOff.emission == 0 &&
              lampOn.emission == 15 && lampOn.dampening == 15,
          "redstone lamp emission is state-dependent");

  const auto water0 = Blocks::WATER.defaultState();
  const auto water1 = Blocks::WATER.defaultState().with(
      PropertyKind::Level, std::uint8_t{1});
  require(!hasDifferentLightProperties(water0, water1),
          "water level changes do not alter light properties");
  require(hasDifferentLightProperties(Blocks::REDSTONE_LAMP.defaultState(),
                                      lampOnState),
          "emission differences force a light recheck");
  auto oldProperties = stone;
  auto changed = stone;
  changed.canOcclude = false;
  require(!hasDifferentLightProperties(oldProperties, changed),
          "Java hasDifferentLightProperties keeps canOcclude separate");
  changed.useShapeForLightOcclusion = true;
  require(hasDifferentLightProperties(oldProperties, changed),
          "useShapeForLightOcclusion forces a recheck");
  require(hasDifferentLightProperties(unsupportedLightProperties(), stone),
          "unknown metadata conservatively requests a recheck");
  require(!hasDifferentSectionLightProperties(false, false) &&
              hasDifferentSectionLightProperties(false, true),
          "section emptiness transitions are independent from block properties");

  const BlockState extension("minecraft:future_light_probe",
                             {{"future", "value"}});
  require(!lightPropertiesOf(extension).supported,
          "an imported unknown state never gets guessed light metadata");
}

void opticalAssetCoverage(const std::filesystem::path &assets) {
  require(officialLightProperties(
              "minecraft:note_block",
              "instrument=banjo,note=0,powered=false").supported,
          "official lookup resolves a note block key");
  std::ifstream names(assets / "block_names.txt");
  require(names.good(), "open official light block-name list");
  std::string name;
  std::size_t blocks = 0;
  while (std::getline(names, name)) {
    if (!name.empty() && name.back() == '\r') name.pop_back();
    if (name.empty()) continue;
    const Block *block = findBlockByName(name);
    require(block != nullptr,
            "official light name is registered: " + name);
    ++blocks;
  }
  require(blocks >= 60, "the official optical fixture covers the registry subset");

  std::ifstream tsv(assets / "block_light.tsv");
  require(tsv.good(), "open official optical TSV");
  std::unordered_set<std::string> officialKeys;
  std::size_t rows = 0;
  while (std::getline(tsv, name)) {
    if (!name.empty() && name.back() == '\r') name.pop_back();
    if (name.empty()) continue;
    ++rows;
    const auto fields = splitTabs(name);
    require(fields.size() == 7,
            "official optical row has seven fields");
    const std::string &blockName = fields[0];
    const std::string &propertyText = fields[1];
    officialKeys.insert(blockName + "\t" + propertyText);
    const Block *block = findBlockByName(blockName);
    require(block != nullptr, "official optical block is registered");
    if (block->supported()) {
      std::map<std::string, std::string> propertyMap;
      std::size_t start = 0;
      while (start < propertyText.size()) {
        const auto comma = propertyText.find(',', start);
        const std::string item = propertyText.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start);
        const auto equals = item.find('=');
        require(equals != std::string::npos,
                "official optical property has a value");
        propertyMap.emplace(item.substr(0, equals), item.substr(equals + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
      std::uint32_t variant = 0;
      // Some registry-subset properties are narrower than vanilla (for example
      // note-block mob-head instruments). Those absent C++ states are not
      // fabricated here; only the intersection is checked.
      if (!block->stateDefinition().variantOf(propertyMap, variant))
        continue;
      const BlockState state =
          BlockState::fromId(block->firstState() + variant);
      const auto &metadata = lightPropertiesOf(state);
      require(metadata.supported,
              "official state resolves to official optical metadata: " + blockName +
                  "[" + propertyText + "] instrument=" +
                  state.get("instrument"));
      require(metadata.emission == std::stoi(fields[2]) &&
                  metadata.dampening == std::stoi(fields[3]) &&
                  metadata.canOcclude == (fields[4] == "1") &&
                  metadata.useShapeForLightOcclusion == (fields[5] == "1") &&
                  metadata.propagatesSkylightDown == (fields[6] == "1"),
              "official optical values match the runtime metadata: " + blockName);
    }
  }
  require(rows == 2366,
          "official optical TSV row count is pinned: " + std::to_string(rows));

  for (const BlockRegistration &registration : blockRegistry()) {
    if (!registration.metadata.supported) continue;
    const Block &block = blockByName(registration.name);
    for (const BlockState &state : block.states()) {
      std::string properties;
      for (const auto &[key, value] : state.properties()) {
        if (!properties.empty()) properties.push_back(',');
        properties += key;
        properties.push_back('=');
        properties += value;
      }
      const bool official = officialKeys.contains(
          registration.name + "\t" + properties);
      require(lightPropertiesOf(state).supported == official,
              "light metadata support exactly tracks official state identity: " +
                  registration.name + "[" + properties + "]");
    }
  }
}

void workerPublicationAndInvalidation() {
  const auto mainThread = std::this_thread::get_id();
  LightService service(-4, 3, LightWorkerOptions{1000, 4096, false, false});
  std::thread::id callbackThread;
  int callbacks = 0;
  service.setPublishCallback([&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
    callbackThread = std::this_thread::get_id();
    ++callbacks;
    require(snapshot && snapshot->propagationComplete &&
                !snapshot->protocolReady,
            "callback receives a complete snapshot before protocol readiness");
  });
  const auto firstGeneration =
      service.submitChunk(chunkFixture(3, -2, -4, 3));
  require(firstGeneration == 1, "first snapshot has generation one");
  require(service.waitIdleForTesting(), "worker reaches idle");
  service.drainPublished();
  require(callbacks == 1 && callbackThread == mainThread,
          "publication callback runs on the main thread");
  auto first = service.latestChunk(3, -2);
  require(first && first->generation == firstGeneration,
          "first immutable snapshot is visible after drain");

  LightBlockChange change;
  change.chunkX = 3;
  change.chunkZ = -2;
  change.sectionY = -4;
  change.localIndex = static_cast<std::uint16_t>(lightIndex(1, 4, 5));
  change.oldProperties = lightPropertiesOf(Blocks::REDSTONE_LAMP.defaultState());
  change.newProperties = lightPropertiesOf(
      Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
  change.wasEmpty = false;
  change.isEmpty = false;
  require(service.reportBlockChange(change), "block change enters light worker");
  require(service.waitIdleForTesting(), "worker applies block change");
  service.drainPublished();
  auto second = service.latestChunk(3, -2);
  require(second && second->generation == 2,
          "new generation is published after a block change");
  require(second->section(-4)->at(1, 4, 5).emission == 15,
          "worker updates owned optical metadata");
  require(first->section(-4)->at(1, 4, 5).emission == 0,
          "previously published snapshots remain immutable");

  LightService staleService(-1, 1);
  const auto staleGeneration = staleService.submitChunk(chunkFixture(1, 1, -1, 1));
  require(staleGeneration == 1 && staleService.waitIdleForTesting(),
          "old snapshot is published before invalidation");
  require(staleService.unloadChunk(1, 1),
          "unload advances the chunk generation");
  const auto reloadGeneration = staleService.submitChunk(chunkFixture(1, 1, -1, 1));
  require(reloadGeneration == 3, "reload receives a new generation");
  require(staleService.waitIdleForTesting(), "stale worker run completes");
  staleService.drainPublished();
  auto reloaded = staleService.latestChunk(1, 1);
  require(reloaded && reloaded->generation == reloadGeneration,
          "stale worker result cannot replace a reloaded chunk");
}

void workerQueueAndClose() {
  LightService service(-1, 1, LightWorkerOptions{1, 1, true});
  const auto first = service.submitChunk(chunkFixture(0, 0, -1, 1));
  const auto second = service.submitChunk(chunkFixture(1, 0, -1, 1));
  require(first != 0 && second != 0,
          "capacity coalesces valid chunks instead of dropping them");
  require(service.stats().rejected == 0 &&
              service.stats().pendingChunks == 2,
          "paused overflow remains owned and retryable");
  require(!service.reportSectionStatus(0, 0, 0, false),
          "out-of-range section status is rejected explicitly");
  LightBlockChange invalid;
  invalid.chunkX = 0;
  invalid.chunkZ = 0;
  invalid.sectionY = -1;
  invalid.localIndex = kLightVolume;
  require(!service.reportBlockChange(invalid),
          "out-of-range light cell is rejected explicitly");
  const auto invalidGeneration = service.stats().rejected;
  require(invalidGeneration == 2, "invalid requests are counted as rejected");
  service.resumeForTesting();
  require(service.waitIdleForTesting(), "paused worker drains coalesced work");
  service.drainPublished();
  require(service.latestChunk(0, 0) && service.latestChunk(1, 0),
          "both coalesced chunks are published after capacity recovery");
  require(service.stats().queueHighWater >= 2,
          "coalesced pending high-water is observed");
  service.close();
  require(service.submitChunk(chunkFixture(2, 0, -1, 1)) == 0 &&
              service.stats().rejected == 3,
          "closed service refuses new work");

  LightService draining(-1, 1, LightWorkerOptions{1, 4, true});
  const auto generation = draining.submitChunk(chunkFixture(4, 0, -1, 1));
  require(generation != 0, "close fixture queues a snapshot");
  draining.close();
  draining.drainPublished();
  auto delivered = draining.latestChunk(4, 0);
  require(delivered && delivered->generation == generation,
          "close drains queued work before joining the worker");

  LightService batching(-1, 1, LightWorkerOptions{1000, 16, true});
  const auto loadGeneration = batching.submitChunk(chunkFixture(5, 0, -1, 1));
  require(loadGeneration == 1, "batched fixture loads once");
  for (int x = 0; x < 2; ++x) {
    LightBlockChange change;
    change.chunkX = 5;
    change.chunkZ = 0;
    change.sectionY = -1;
    change.localIndex = static_cast<std::uint16_t>(lightIndex(x, 1, 1));
    change.oldProperties = lightPropertiesOf(airState());
    change.newProperties = lightPropertiesOf(
        x == 0 ? Blocks::STONE.defaultState()
               : Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
    change.wasEmpty = true;
    change.isEmpty = false;
    require(batching.reportBlockChange(change), "coalesced change is accepted");
  }
  batching.resumeForTesting();
  require(batching.waitIdleForTesting(), "coalesced batch completes");
  const auto batchStats = batching.stats();
  require(batchStats.processedBlockChanges == 2 &&
              batchStats.processedBatches == 1,
          "block changes share one worker batch/task");
  batching.drainPublished();
  auto batched = batching.latestChunk(5, 0);
  require(batched && batched->generation == 3,
          "coalesced changes carry the newest generation");
}

void recoverableChangesAndBoundedPublication() {
  LightService service(-1, 1, LightWorkerOptions{1, 1, true});
  require(service.submitChunk(chunkFixture(9, 0, -1, 1)) != 0 &&
              service.submitChunk(chunkFixture(10, 0, -1, 1)) != 0,
          "small-capacity worker retains both pending chunks");

  for (int x = 0; x < 4; ++x) {
    LightBlockChange change;
    change.chunkX = 10;
    change.chunkZ = 0;
    change.sectionY = -1;
    change.localIndex = static_cast<std::uint16_t>(lightIndex(x, 1, 1));
    change.oldProperties = lightPropertiesOf(airState());
    change.newProperties = lightPropertiesOf(
        Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
    change.wasEmpty = true;
    change.isEmpty = false;
    require(service.reportBlockChange(change),
            "cell write remains retryable after overflow");
  }
  require(service.unloadChunk(9, 0), "unload tombstone is retained");
  require(service.stats().pendingChunks == 2,
          "per-chunk pending state does not grow with write count");

  service.resumeForTesting();
  require(service.waitIdleForTesting(), "recovery state drains completely");
  require(service.stats().pendingPublications == 1,
          "undrained publication results are bounded per chunk");
  service.drainPublished();
  require(service.latestChunk(9, 0) == nullptr,
          "delivered unload tombstone prevents old-generation resurrection");
  auto newest = service.latestChunk(10, 0);
  require(newest && newest->section(-1)->at(3, 1, 1).emission == 15,
          "all coalesced live changes remain visible after overflow");
  require(newest->lifecycleEpoch != 0 &&
              newest->revision != 0,
          "coalesced result has a worker publication revision");

  for (int i = 0; i < 200; ++i) {
    LightBlockChange change;
    change.chunkX = 10;
    change.chunkZ = 0;
    change.sectionY = -1;
    change.localIndex = static_cast<std::uint16_t>(lightIndex(i & 15, 0, 0));
    change.oldProperties = lightPropertiesOf(airState());
    change.newProperties = lightPropertiesOf(
        i & 1 ? Blocks::STONE.defaultState()
              : Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
    change.wasEmpty = false;
    change.isEmpty = false;
    require(service.reportBlockChange(change), "long pending write is accepted");
  }
  require(service.waitIdleForTesting(), "long pending write reaches worker");
  require(service.stats().pendingPublications == 1,
          "many same-chunk generations keep only one undrained result");

  LightService sectionBatch(-1, 1, LightWorkerOptions{100, 16, true});
  auto empty = chunkFixture(11, 0, -1, 1);
  empty.sections[1] = std::make_shared<const LightSectionSnapshot>(airSection(-1));
  require(sectionBatch.submitChunk(std::move(empty)) != 0,
          "empty section batch fixture loads");
  const auto appendChange = [&](int index, const BlockState &oldState,
                                const BlockState &newState, bool wasEmpty,
                                bool isEmpty) {
    LightBlockChange change;
    change.chunkX = 11;
    change.chunkZ = 0;
    change.sectionY = -1;
    change.localIndex = static_cast<std::uint16_t>(lightIndex(index, 1, 1));
    change.oldProperties = lightPropertiesOf(oldState);
    change.newProperties = lightPropertiesOf(newState);
    change.wasEmpty = wasEmpty;
    change.isEmpty = isEmpty;
    require(sectionBatch.reportBlockChange(change),
            "section batch change is accepted");
  };
  appendChange(1, airState(), Blocks::STONE.defaultState(), true, false);
  appendChange(1, Blocks::STONE.defaultState(), airState(), false, true);
  appendChange(2, airState(),
               Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"), true,
               false);
  sectionBatch.resumeForTesting();
  require(sectionBatch.waitIdleForTesting(), "unordered cell batch completes");
  sectionBatch.drainPublished();
  auto sectionSnapshot = sectionBatch.latestChunk(11, 0);
  require(sectionSnapshot && !sectionSnapshot->section(-1)->empty,
          "final per-section state wins over historical cell emptiness");
  require(sectionSnapshot->section(-1)->at(1, 1, 1).emission == 0 &&
              sectionSnapshot->section(-1)->at(2, 1, 1).emission == 15,
          "coalesced fill-clear-fill keeps final cell contents");
}

std::shared_ptr<const LightChunkSnapshot>
settledChunk(LightService &service, int chunkX, int chunkZ) {
  require(service.waitIdleForTesting(), "light worker reaches a complete batch");
  service.drainPublished();
  auto snapshot = service.latestChunk(chunkX, chunkZ);
  require(snapshot != nullptr, "settled chunk is published");
  return snapshot;
}

void blockLightPropagation() {
  const auto air = airState();
  const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  const auto lampOff = Blocks::REDSTONE_LAMP.defaultState();
  LightService service(0, 1);
  require(service.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "block-light fixture loads");
  auto initial = settledChunk(service, 0, 0);
  require(initial->blockLightComplete && initial->skyLightComplete &&
              initial->propagationComplete &&
              !initial->protocolReady && blockLightAt(initial, 8, 8, 8) == 0,
          "empty chunk completes both propagated light layers");

  require(reportBlock(service, 0, 0, {7, 8, 8}, air, lamp),
          "source placement enters worker");
  auto lit = settledChunk(service, 0, 0);
  const std::string lit_values =
      std::to_string(blockLightAt(lit, 7, 8, 8)) + "/" +
      std::to_string(blockLightAt(lit, 8, 8, 8)) + "/" +
      std::to_string(blockLightAt(lit, 9, 8, 8)) + "/" +
      std::to_string(blockLightAt(lit, 7, 9, 8));
  require(blockLightAt(lit, 7, 8, 8) == 15 &&
              blockLightAt(lit, 8, 8, 8) == 14 &&
              blockLightAt(lit, 9, 8, 8) == 13 &&
              blockLightAt(lit, 7, 9, 8) == 14,
          "single source propagates with one-level falloff: " + lit_values);
  require(service.stats().lightQueueEntries > 0,
          "benchmark-visible counter records propagated queue entries");

  require(reportBlock(service, 0, 0, {8, 8, 8}, air,
                      Blocks::WATER.defaultState()),
          "dampening-one host enters worker");
  auto water = settledChunk(service, 0, 0);
  require(blockLightAt(water, 8, 8, 8) == 14 &&
              blockLightAt(water, 9, 8, 8) == 13,
          "max(1,dampening) is applied to water");
  require(reportBlock(service, 0, 0, {8, 8, 8},
                      Blocks::WATER.defaultState(),
                      Blocks::STONE.defaultState()),
          "opaque state change enters worker");
  auto opaque = settledChunk(service, 0, 0);
  const std::string opaque_values =
      std::to_string(blockLightAt(opaque, 7, 8, 8)) + "/" +
      std::to_string(blockLightAt(opaque, 8, 8, 8)) + "/" +
      std::to_string(blockLightAt(opaque, 9, 8, 8));
  require(blockLightAt(opaque, 8, 8, 8) == 0 &&
              blockLightAt(opaque, 9, 8, 8) < 13,
          "opaque neighbor blocks the face and downstream light: " +
              opaque_values);
  require(reportBlock(service, 0, 0, {8, 8, 8},
                      Blocks::STONE.defaultState(), air),
          "opening the cell enters worker");
  auto reopened = settledChunk(service, 0, 0);
  require(blockLightAt(reopened, 8, 8, 8) == 14 &&
              blockLightAt(reopened, 9, 8, 8) == 13,
          "opening a cell restores downstream light");

  require(reportBlock(service, 0, 0, {7, 8, 8}, lamp, lampOff),
          "source state removal enters worker");
  auto removed = settledChunk(service, 0, 0);
  require(blockLightAt(removed, 7, 8, 8) == 0,
          "unlit lamp drops its emitted light");

  require(reportBlock(service, 0, 0, {2, 8, 8}, air, lamp) &&
              reportBlock(service, 0, 0, {8, 8, 8}, air, lamp),
          "overlapping sources enter one pending batch");
  auto overlap = settledChunk(service, 0, 0);
  require(blockLightAt(overlap, 5, 8, 8) == 12,
          "overlapping light takes maximum instead of summing: " +
              std::to_string(blockLightAt(overlap, 5, 8, 8)));

  require(reportBlock(service, 0, 0, {2, 8, 8}, lamp, air) &&
              reportBlock(service, 0, 0, {8, 8, 8}, lamp, air),
          "remaining sources remove cleanly");
  auto empty = settledChunk(service, 0, 0);
  require(blockLightAt(empty, 5, 8, 8) == 0,
          "decrease propagation clears the decayed region");
}

void skyLightPropagation() {
  const auto air = airState();
  const auto stone = Blocks::STONE.defaultState();
  LightService service(0, 1);
  require(service.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "sky-light fixture loads an open chunk");
  auto open = settledChunk(service, 0, 0);
  require(open->blockLightComplete && open->skyLightComplete &&
              open->propagationComplete && !open->protocolReady,
          "open sky completes both propagated layers without protocol ready");
  require(skyLightAt(open, 0, 0, 0) == 15 &&
              skyLightAt(open, 8, 8, 8) == 15 &&
              skyLightAt(open, 15, 15, 15) == 15 &&
              skyLightAt(open, 4, -1, 4) == 15 &&
              skyLightAt(open, 4, 16, 4) == 15,
          "an all-air chunk inherits full sky into both padding directions");

  require(service.submitChunk(emptyChunkFixture(1, 0, 0, 1)) != 0,
          "east open chunk loads for cross-boundary sky");
  (void)settledChunk(service, 0, 0);

  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      require(reportBlock(service, 0, 0, {x, 8, z}, air, stone),
              "roof change enters the worker");
  auto roofed = settledChunk(service, 0, 0);
  const std::string skyRoof =
      std::to_string(skyLightAt(roofed, 8, 8, 8)) + "/" +
      std::to_string(skyLightAt(roofed, 8, 7, 8)) + "/" +
      std::to_string(skyLightAt(roofed, 8, 9, 8)) + "/" +
      std::to_string(skyLightAt(roofed, 15, 7, 8)) + "/" +
      std::to_string(skyLightAt(roofed, 8, -1, 8)) + "/" +
      std::to_string(skyLightAt(roofed, 8, 16, 8));
  require(skyLightAt(roofed, 8, 8, 8) == 0 &&
              skyLightAt(roofed, 8, 7, 8) == 7 &&
              skyLightAt(roofed, 8, 9, 8) == 15 &&
              skyLightAt(roofed, 15, 7, 8) == 14 &&
              skyLightAt(roofed, 8, -1, 8) == 7 &&
              skyLightAt(roofed, 8, 16, 8) == 15,
          "roof blocks direct sky and side light attenuates across the chunk edge: " +
              skyRoof);

  require(reportBlock(service, 0, 0, {8, 8, 8}, stone, air),
          "opening the roof enters the worker");
  auto opened = settledChunk(service, 0, 0);
  require(skyLightAt(opened, 8, 8, 8) == 15 &&
              skyLightAt(opened, 8, 7, 8) == 15 &&
              skyLightAt(opened, 8, -1, 8) == 15,
          "removing roof restores full sky down the source column");
  require(reportBlock(service, 0, 0, {8, 8, 8}, air, stone),
          "closing the roof again enters the worker");
  auto reclosed = settledChunk(service, 0, 0);
  require(skyLightAt(reclosed, 8, 7, 8) == 7 &&
              skyLightAt(reclosed, 8, -1, 8) == 7,
          "closing roof keeps side attenuation through the inherited bottom plane");

  const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  require(reportBlock(service, 0, 0, {4, 7, 4}, air, lamp),
          "block source enters a shadowed sky column");
  auto independent = settledChunk(service, 0, 0);
  require(blockLightAt(independent, 4, 7, 4) == 15 &&
              blockLightAt(independent, 5, 7, 4) == 14 &&
              skyLightAt(independent, 8, 7, 8) == 7,
          "sky and block layers propagate independently");

  LightService disabled(0, 1,
                        LightWorkerOptions{1000, 4096, false, false});
  require(disabled.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "no-sky fixture loads");
  auto withoutSky = settledChunk(disabled, 0, 0);
  require(withoutSky->skyLightComplete && withoutSky->propagationComplete &&
              skyLightAt(withoutSky, 8, 8, 8) == 0 &&
              skyLightAt(withoutSky, 8, 16, 8) == 0,
          "a dimension without sky never receives implicit full light");

  LightService negative(-1, 1);
  require(negative.submitChunk(emptyChunkFixture(0, 0, -1, 1)) != 0,
          "negative-height sky fixture loads");
  auto negativeOpen = settledChunk(negative, 0, 0);
  require(skyLightAt(negativeOpen, 3, -16, 3) == 15 &&
              skyLightAt(negativeOpen, 3, -17, 3) == 15 &&
              skyLightAt(negativeOpen, 3, 0, 3) == 15,
          "open negative-height column inherits full sky through padding");
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      require(reportBlock(negative, 0, 0, {x, -16, z}, air, stone),
              "negative-height roof writes enter worker");
  auto negativeRoof = settledChunk(negative, 0, 0);
  require(skyLightAt(negativeRoof, 3, -16, 3) == 0 &&
              skyLightAt(negativeRoof, 3, -17, 3) == 0 &&
              skyLightAt(negativeRoof, 3, -15, 3) == 15,
          "negative-y roof blocks block and bottom-padding sky");
  require(!negativeRoof->section(-2)->layers[static_cast<std::size_t>(
               LightLayer::Sky)]
               .isMissing() &&
              skyLightAt(negativeRoof, 3, -17, 3) == 0,
          "nonempty bottom section may allocate a stored lower-padding plane");

  LightService loadOrder(0, 1);
  require(loadOrder.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "shadowed east chunk loads first");
  (void)settledChunk(loadOrder, 0, 0);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      require(reportBlock(loadOrder, 0, 0, {x, 8, z}, air, stone),
              "east chunk roof enters worker");
  auto beforeWest = settledChunk(loadOrder, 0, 0);
  require(skyLightAt(beforeWest, 0, 7, 8) == 0,
          "unloaded west neighbor remains a sky-light barrier");
  require(loadOrder.submitChunk(emptyChunkFixture(-1, 0, 0, 1)) != 0,
          "west open chunk loads later");
  auto afterWest = settledChunk(loadOrder, 0, 0);
  require(skyLightAt(afterWest, 0, 7, 8) == 14,
          "later west load admits attenuated sky across the boundary");

  LightService refresh(0, 1);
  require(refresh.submitChunk(skyRoofChunk(-1, 0)) != 0 &&
              refresh.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "same-epoch sky refresh loader accepts adjacent roof/open chunks");
  auto roofedWest = settledChunk(refresh, -1, 0);
  require(skyLightAt(roofedWest, -1, 7, 8) == 14,
          "open east chunk lights the roofed west boundary before refresh");
  const auto epoch = roofedWest->lifecycleEpoch;
  require(refresh.submitChunk(skyRoofChunk(0, 0)) != 0,
          "same-epoch authoritative east snapshot adds a roof");
  auto refreshedEast = settledChunk(refresh, 0, 0);
  refresh.drainPublished();
  auto refreshedWest = refresh.latestChunk(-1, 0);
  require(refreshedEast->lifecycleEpoch == epoch && refreshedWest &&
              refreshedWest->lifecycleEpoch == epoch,
          "authoritative sky refresh keeps the loading epoch");
  require(skyLightAt(refreshedWest, -1, 7, 8) == 0,
          "same-epoch roof refresh removes stale cross-boundary sky light");
  require(refresh.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "same-epoch authoritative east snapshot restores open sky");
  (void)settledChunk(refresh, 0, 0);
  refresh.drainPublished();
  auto reopenedWest = refresh.latestChunk(-1, 0);
  require(skyLightAt(reopenedWest, -1, 7, 8) == 14,
          "same-epoch open refresh relights the neighboring roofed boundary");

  LightService topWater(0, 1);
  auto waterColumn = emptyChunkFixture(0, 0, 0, 1);
  waterColumn.sections[1] = std::make_shared<const LightSectionSnapshot>(
      0, false,
      std::vector<BlockLightProperties>{
          lightPropertiesOf(Blocks::WATER.defaultState())},
      std::vector<std::uint16_t>{});
  require(topWater.submitChunk(std::move(waterColumn)) != 0,
          "top water column loads");
  auto topWaterSnapshot = settledChunk(topWater, 0, 0);
  require(skyLightAt(topWaterSnapshot, 8, 15, 8) == 14 &&
              skyLightAt(topWaterSnapshot, 8, 14, 8) == 13 &&
              skyLightAt(topWaterSnapshot, 8, 16, 8) == 15,
          "top padding seeds attenuated sky through a dampening-one water layer");

  LightService storage(0, 1);
  require(storage.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "storage-eligibility fixture loads");
  (void)settledChunk(storage, 0, 0);
  require(reportBlock(storage, 0, 0, {8, 8, 8}, air, stone),
          "sole anchor block enters worker");
  auto anchored = settledChunk(storage, 0, 0);
  require(skyLightAt(anchored, 8, 8, 8) == 0 &&
              skyLightAt(anchored, 8, 20, 8) == 15 &&
              !anchored->section(0)->layers[static_cast<std::size_t>(
                   LightLayer::Sky)]
                   .isMissing() &&
              !anchored->section(1)->layers[static_cast<std::size_t>(
                   LightLayer::Sky)]
                   .isMissing(),
          "nonempty section allocates its sky layer and neighboring full plane");
  LightBlockChange removeAnchor;
  removeAnchor.chunkX = 0;
  removeAnchor.chunkZ = 0;
  removeAnchor.sectionY = 0;
  removeAnchor.localIndex = static_cast<std::uint16_t>(lightIndex(8, 8, 8));
  removeAnchor.oldProperties = lightPropertiesOf(stone);
  removeAnchor.newProperties = lightPropertiesOf(air);
  removeAnchor.wasEmpty = false;
  removeAnchor.isEmpty = true;
  require(storage.reportBlockChange(removeAnchor),
          "removing the sole anchor enters worker with final empty status");
  auto released = settledChunk(storage, 0, 0);
  require(skyLightAt(released, 8, 8, 8) == 15 &&
              skyLightAt(released, 8, 20, 8) == 15 &&
              released->section(0)->layers[static_cast<std::size_t>(
                  LightLayer::Sky)]
                  .isMissing() &&
              released->section(1)->layers[static_cast<std::size_t>(
                  LightLayer::Sky)]
                  .isMissing(),
          "removing the final block drops raw layers and restores inherited full sky");
  require(anchored->lightValue(LightLayer::Sky, 8, 8, 8) == 0 &&
              !anchored->section(0)->layers[static_cast<std::size_t>(
                   LightLayer::Sky)]
                   .isMissing(),
          "previous immutable publication keeps its old raw zero layer");

}

void blockLightStaleAndAuthoritativeRefresh() {
  const auto air = airState();
  const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");

  LightService stale(0, 1);
  require(stale.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "stale boundary fixture loads east chunk");
  (void)settledChunk(stale, 0, 0);
  require(reportBlock(stale, 0, 0, {0, 8, 0}, air, lamp),
          "stale boundary fixture places its old source");
  (void)settledChunk(stale, 0, 0);

  stale.pauseForTesting();
  require(stale.submitChunk(emptyChunkFixture(-1, 0, 0, 1)) != 0,
          "new west chunk and old boundary increase share one pending batch");
  require(reportBlock(stale, 0, 0, {0, 8, 0}, lamp, air),
          "old boundary source is removed before the batch resumes");
  stale.resumeForTesting();
  auto staleEast = settledChunk(stale, 0, 0);
  stale.drainPublished();
  auto staleWest = stale.latestChunk(-1, 0);
  require(staleWest != nullptr, "west chunk publishes after the shared batch");
  require(blockLightAt(staleEast, 0, 8, 0) == 0 &&
              blockLightAt(staleWest, -1, 8, 0) == 0,
          "an increase dequeued after the source darkened cannot relight either chunk");

  LightService refresh(0, 1);
  require(refresh.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0 &&
              refresh.submitChunk(emptyChunkFixture(-1, 0, 0, 1)) != 0,
          "authoritative refresh fixture loads both chunks");
  (void)settledChunk(refresh, 0, 0);
  require(reportBlock(refresh, 0, 0, {0, 8, 0}, air, lamp),
          "authoritative refresh fixture lights the boundary");
  (void)settledChunk(refresh, 0, 0);
  require(blockLightAt(refresh.latestChunk(-1, 0), -1, 8, 0) == 14,
          "initial source crosses the loaded boundary");

  require(refresh.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "same-epoch authoritative snapshot removes the source");
  auto refreshedEast = settledChunk(refresh, 0, 0);
  refresh.drainPublished();
  auto refreshedWest = refresh.latestChunk(-1, 0);
  require(refreshedEast->blockLightComplete && refreshedWest,
          "authoritative removal still publishes a complete block layer");
  require(blockLightAt(refreshedEast, 0, 8, 0) == 0 &&
              blockLightAt(refreshedWest, -1, 8, 0) == 0,
          "authoritative input removes local and cross-boundary old light");

  auto withSource = emptyChunkFixture(0, 0, 0, 1);
  std::vector<std::uint16_t> cells(kLightVolume, 0);
  cells[lightIndex(0, 8, 0)] = 1;
  withSource.sections[1] = std::make_shared<const LightSectionSnapshot>(
      0, false,
      std::vector<BlockLightProperties>{lightPropertiesOf(air),
                                        lightPropertiesOf(lamp)},
      std::move(cells));
  require(refresh.submitChunk(std::move(withSource)) != 0,
          "same-epoch authoritative snapshot adds a source");
  auto addedEast = settledChunk(refresh, 0, 0);
  refresh.drainPublished();
  auto addedWest = refresh.latestChunk(-1, 0);
  require(addedEast->blockLightComplete && addedWest,
          "authoritative source addition publishes a complete block layer");
  require(blockLightAt(addedEast, 0, 8, 0) == 15 &&
              blockLightAt(addedWest, -1, 8, 0) == 14,
          "authoritative source addition propagates across the loaded boundary");
}

void blockLightVerticalPadding() {
  auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  const auto air = airState();
  LightService service(0, 1);
  require(service.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "vertical padding fixture loads");
  (void)settledChunk(service, 0, 0);

  require(reportBlock(service, 0, 0, {8, 0, 8}, air, lamp),
          "bottom block-layer source enters worker");
  auto bottom = settledChunk(service, 0, 0);
  require(bottom->blockLightComplete && blockLightAt(bottom, 8, 0, 8) == 15 &&
              blockLightAt(bottom, 8, -1, 8) == 14 &&
              blockLightAt(bottom, 8, -2, 8) == 13,
          "block light enters and attenuates through bottom padding");
  require(reportBlock(service, 0, 0, {8, 0, 8}, lamp, air),
          "bottom source removal enters worker");
  auto bottomCleared = settledChunk(service, 0, 0);
  require(blockLightAt(bottomCleared, 8, -1, 8) == 0 &&
              blockLightAt(bottomCleared, 8, -2, 8) == 0,
          "bottom padding light decays when its source is removed");

  require(reportBlock(service, 0, 0, {8, 15, 8}, air, lamp),
          "top block-layer source enters worker");
  auto top = settledChunk(service, 0, 0);
  require(top->blockLightComplete && blockLightAt(top, 8, 15, 8) == 15 &&
              blockLightAt(top, 8, 16, 8) == 14 &&
              blockLightAt(top, 8, 17, 8) == 13,
          "block light enters and attenuates through top padding");
  require(reportBlock(service, 0, 0, {8, 15, 8}, lamp, air),
          "top source removal enters worker");
  auto topCleared = settledChunk(service, 0, 0);
  require(blockLightAt(topCleared, 8, 16, 8) == 0 &&
              blockLightAt(topCleared, 8, 17, 8) == 0,
          "top padding light decays when its source is removed");
}

void crossBatchPublicationOrdering() {
  const auto air = airState();
  const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  LightService service(0, 1);
  require(service.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0 &&
              service.submitChunk(emptyChunkFixture(-1, 0, 0, 1)) != 0,
          "cross-batch publication fixture loads adjacent chunks");
  (void)settledChunk(service, 0, 0);
  (void)settledChunk(service, -1, 0);

  service.pauseForTesting();
  require(reportBlock(service, -1, 0, {-1, 8, 0}, air, lamp),
          "older request is queued for the west chunk");
  require(reportBlock(service, 0, 0, {7, 8, 0}, air, lamp),
          "newer request is queued for the east chunk");

  require(service.processChunkBatchForTesting(0, 0),
          "newer east request is processed first in isolation");
  service.drainPublished();
  auto eastFirst = service.latestChunk(0, 0);
  require(eastFirst && blockLightAt(eastFirst, 7, 8, 0) == 15 &&
              blockLightAt(eastFirst, 0, 8, 0) == 8,
          "east publication exists before the older west request is processed");

  require(service.processChunkBatchForTesting(-1, 0),
          "older west request is processed in the following batch");
  service.drainPublished();
  auto eastAfterWest = service.latestChunk(0, 0);
  auto west = service.latestChunk(-1, 0);
  require(eastAfterWest && west &&
              eastAfterWest->revision > eastFirst->revision,
          "worker publication revision advances despite lower input generation");
  require(blockLightAt(eastAfterWest, 0, 8, 0) == 14 &&
              blockLightAt(west, -1, 8, 0) == 15,
          "west batch's cross-boundary result is not rejected for published east");
}

void blockLightBoundariesAndEpochs() {
  const auto air = airState();
  const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  LightService cross(-1, 1);
  require(cross.submitChunk(emptyChunkFixture(0, 0, -1, 1)) != 0,
          "east chunk loads first");
  (void)settledChunk(cross, 0, 0);
  require(reportBlock(cross, 0, 0, {0, -8, 0}, air, lamp),
          "negative-coordinate boundary source enters");
  auto eastOnly = settledChunk(cross, 0, 0);
  require(blockLightAt(eastOnly, 0, -8, 0) == 15 &&
              blockLightAt(eastOnly, 1, -8, 0) == 14,
          "unloaded west neighbor behaves as a barrier");

  require(cross.submitChunk(emptyChunkFixture(-1, 0, -1, 1)) != 0,
          "west chunk loads later");
  auto west = settledChunk(cross, -1, 0);
  require(blockLightAt(west, -1, -8, 0) == 14 &&
              blockLightAt(west, -2, -8, 0) == 13,
          "loading a neighbor admits light across the chunk boundary");

  cross.pauseForTesting();
  auto visibleBefore = settledChunk(cross, 0, 0);
  require(reportBlock(cross, 0, 0, {0, -8, 0}, lamp, air),
          "paused source removal is retained");
  require(cross.latestChunk(0, 0) == visibleBefore,
          "readers keep the previous complete revision while new work is pending");
  cross.resumeForTesting();
  auto changed = settledChunk(cross, 0, 0);
  require(changed->lifecycleEpoch == visibleBefore->lifecycleEpoch &&
              changed->revision > visibleBefore->revision,
          "same lifetime advances revision after a complete batch");

  require(reportBlock(cross, -1, 0, {-1, -8, 0}, air, lamp),
          "west source enters after load");
  auto across = settledChunk(cross, 0, 0);
  require(blockLightAt(across, 0, -8, 0) == 14,
          "west source propagates into the east chunk");
  const auto oldEpoch = cross.latestChunk(-1, 0)->lifecycleEpoch;
  require(cross.unloadChunk(-1, 0), "west chunk unloads");
  require(cross.latestChunk(-1, 0) == nullptr,
          "unload hides the old lifecycle immediately");
  auto decayed = settledChunk(cross, 0, 0);
  require(blockLightAt(decayed, 0, -8, 0) == 0,
          "unload removes cross-boundary light from the loaded side");

  require(cross.submitChunk(emptyChunkFixture(-1, 0, -1, 1)) != 0,
          "west chunk reloads");
  auto reloaded = settledChunk(cross, -1, 0);
  require(reloaded->lifecycleEpoch > oldEpoch,
          "reload advances lifecycle epoch");
  require(blockLightAt(settledChunk(cross, 0, 0), 0, -8, 0) == 0,
          "reloaded empty chunk does not resurrect old light");
  require(reportBlock(cross, -1, 0, {-1, -8, 0}, air, lamp),
          "new-lifetime source enters");
  auto newAcross = settledChunk(cross, 0, 0);
  require(blockLightAt(newAcross, 0, -8, 0) == 14,
          "new lifecycle propagates across the boundary");

  LightService vertical(0, 2);
  require(vertical.submitChunk(emptyChunkFixture(0, 0, 0, 2)) != 0,
          "two-section fixture loads");
  (void)settledChunk(vertical, 0, 0);
  require(reportBlock(vertical, 0, 0, {8, 15, 8}, air, lamp),
          "section-edge source enters");
  auto crossSection = settledChunk(vertical, 0, 0);
  require(blockLightAt(crossSection, 8, 16, 8) == 14 &&
              blockLightAt(crossSection, 8, 17, 8) == 13,
          "block light crosses the y-section boundary");
}

void directionalShapeOcclusion() {
  const auto &air = lightPropertiesOf(airState());
  const auto pistonNorth = Blocks::PISTON.defaultState()
                               .with("facing", "north")
                               .with("extended", "true");
  const auto &piston = lightPropertiesOf(pistonNorth);
  const std::string piston_metadata =
      "supported=" + std::to_string(piston.supported) +
      " shape=" + std::to_string(piston.occlusionShapeId) +
      " use=" + std::to_string(piston.useShapeForLightOcclusion) +
      " occlude=" + std::to_string(piston.canOcclude);
  require(piston.supported && piston.useShapeForLightOcclusion,
          "piston light shape comes from official metadata");
  require(!lightFaceShapesOcclude(air, piston, LightDirection::South),
          "piston north face is open to southbound light");
  require(lightFaceShapesOcclude(air, piston, LightDirection::North),
          "piston south face blocks northbound light: " + piston_metadata);
  const auto &retracted = lightPropertiesOf(Blocks::PISTON.defaultState());
  require(!retracted.useShapeForLightOcclusion,
          "retracted piston uses its full dampening instead of a face shape");
}

void incompleteMetadataIsExplicit() {
  auto input = emptyChunkFixture(0, 0, 0, 1);
  std::vector<std::uint16_t> cells(kLightVolume, 0);
  cells[lightIndex(4, 5, 6)] = 1;
  input.sections[1] = std::make_shared<const LightSectionSnapshot>(
      0, false,
      std::vector<BlockLightProperties>{lightPropertiesOf(airState()),
                                        BlockLightProperties{}},
      std::move(cells));
  LightService service(0, 1);
  require(service.submitChunk(std::move(input)) != 0,
          "incomplete metadata fixture loads");
  auto snapshot = settledChunk(service, 0, 0);
  require(!snapshot->blockLightComplete && snapshot->unsupportedCells == 1,
          "missing optical metadata remains explicitly incomplete");
  require(!snapshot->propagationComplete && !snapshot->protocolReady,
          "incomplete block light is never exposed as fully ready");
}

void destructorClosesWorker() {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, false});
  // Do not drain or disable explicitly: ~World must stop and join the owned
  // single-writer thread before its chunk storage is destroyed.
}

void worldHooks() {
  World world;
  require(!world.lightingEnabled() && world.lightService() == nullptr,
          "production worlds keep the light worker disabled by default");
  world.loadChunk(0, 0);
  world.loadChunk(1, 0);
  require(world.setBlock({0, 100, 0}, Blocks::SMOOTH_STONE.defaultState()),
          "place pre-enable fixture block");
  world.enableLighting(LightWorkerOptions{1000, 4096, false, false});
  require(world.lightingEnabled() && world.lightService(),
          "lighting can be enabled opt-in on a world");
  auto *service = world.lightService();
  require(service->waitIdleForTesting(), "initial world snapshot completes");
  service->drainPublished();
  auto initial = service->latestChunk(0, 0);
  require(initial && initial->section(6) &&
              initial->section(6)->at(0, 4, 0).dampening == 15,
          "World load snapshot owns official section metadata");
  auto emptyPeer = service->latestChunk(1, 0);
  require(emptyPeer && emptyPeer->section(-3)->isUniform() &&
              initial->section(-3) == emptyPeer->section(-3),
          "empty block sections reuse one immutable uniform input");

  const auto initialGeneration = initial->generation;
  require(world.setBlock({0, 100, 0},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "world change survives normal callbacks");
  require(service->waitIdleForTesting(), "world change reaches light worker");
  service->drainPublished();
  auto changed = service->latestChunk(0, 0);
  require(changed && changed->generation > initialGeneration,
          "World setBlock advances the light generation");
  require(changed->section(6)->at(0, 4, 0).emission == 15,
          "World setBlock forwards official emission metadata");
  require(blockLightAt(changed, 0, 100, 0) == 15 &&
              blockLightAt(changed, 1, 100, 0) == 14,
          "World setBlock runs real block-light propagation");

  require(world.setBlock({0, 150, 0}, Blocks::STONE.defaultState()),
          "place into an empty block section");
  require(service->waitIdleForTesting(), "section transition reaches worker");
  service->drainPublished();
  auto occupied = service->latestChunk(0, 0);
  require(occupied && !occupied->section(9)->empty,
          "empty-to-non-empty section status is applied");
  require(world.setBlock({0, 150, 0}, airState()),
          "clear the fresh section");
  require(service->waitIdleForTesting(), "section clear reaches worker");
  service->drainPublished();
  auto cleared = service->latestChunk(0, 0);
  require(cleared && cleared->section(9)->empty,
          "non-empty-to-empty section status is applied");

  world.unloadChunk(0, 0);
  require(service->latestChunk(0, 0) == nullptr,
          "unload invalidates the published light generation immediately");
  world.loadChunk(0, 0);
  require(service->waitIdleForTesting(), "reload snapshot completes");
  service->drainPublished();
  auto reloaded = service->latestChunk(0, 0);
  require(reloaded && reloaded->generation > cleared->generation,
          "reload publishes a newer owned generation");
  require(reloaded->lightSectionCount() == 26 && reloaded->minLightSectionY() == -5,
          "default world has 24 block sections plus two light padding sections");
  world.disableLighting();
  require(!world.lightingEnabled(), "light service closes without a live World");
}

void pausedWorldRecoveryAndImport() {
  World world;
  world.loadChunk(0, 0);
  world.loadChunk(1, 0);
  world.enableLighting(LightWorkerOptions{1, 1, true, false});
  auto *service = world.lightService();
  require(service, "paused world lighting is enabled");

  require(world.setBlock({0, 100, 0},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "paused world accepts an emission change");
  require(world.setBlock({17, 100, 0}, Blocks::STONE.defaultState()),
          "paused world accepts a later change");
  world.unloadChunk(0, 0);
  require(service->stats().pendingChunks == 2,
          "World keeps chunk-coalesced recovery state without waiting");
  service->resumeForTesting();
  require(service->waitIdleForTesting(), "paused World worker drains");
  service->drainPublished();
  require(service->latestChunk(0, 0) == nullptr,
          "World unload tombstone rejects an older load publication");
  auto live = service->latestChunk(1, 0);
  require(live && live->section(6)->at(1, 4, 0).dampening == 15,
          "surviving World chunk carries its coalesced cell change");

  world.refreshLightSnapshots();
  LoadedWorld imported;
  imported.gameTime = world.gameTime();
  imported.chunks.push_back({1, 0});
  imported.blocks[{16, 101, 0}] =
      Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  importWorld(imported, world);
  require(service->waitIdleForTesting(), "post-import authoritative snapshot drains");
  service->drainPublished();
  auto authoritative = service->latestChunk(1, 0);
  require(authoritative && authoritative->section(6)->at(0, 5, 0).emission == 15,
          "import updates light from owned post-raw-write data");
  require(blockLightAt(authoritative, 16, 101, 0) == 15,
          "imported source participates in block-light propagation");
  world.disableLighting();
}
} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "light_tests needs assets/light26_1");
    dataLayerLayout();
    sectionRangesAndSnapshots();
    opticalMetadata();
    opticalAssetCoverage(argv[1]);
    workerPublicationAndInvalidation();
    workerQueueAndClose();
    recoverableChangesAndBoundedPublication();
    blockLightPropagation();
    skyLightPropagation();
    blockLightStaleAndAuthoritativeRefresh();
    blockLightVerticalPadding();
    crossBatchPublicationOrdering();
    blockLightBoundariesAndEpochs();
    directionalShapeOcclusion();
    incompleteMetadataIsExplicit();
    destructorClosesWorker();
    worldHooks();
    pausedWorldRecoveryAndImport();
    std::cout << "light tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "light test failure: " << error.what() << '\n';
    return 1;
  }
}
