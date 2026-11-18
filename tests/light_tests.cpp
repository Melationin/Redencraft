#include "redstone/engine.hpp"
#include "redstone/light.hpp"
#include "redstone/storage.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <sstream>
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

GeometrySection makeUniformGeometrySection(const BlockState &state) {
  const auto optical = defaultLightOpticalTable();
  LightSectionView view;
  view.empty = state.id() == airState().id();
  view.bits = 0;
  view.single = state.id();
  return makeLightGeometrySection(*optical, view);
}

GeometrySection makeIndexedGeometrySection(
    const BlockState &fallback,
    const std::vector<std::pair<Pos, BlockState>> &overrides, bool empty) {
  const auto optical = defaultLightOpticalTable();
  std::vector<BlockState::Id> palette{fallback.id()};
  std::array<std::uint8_t, kLightVolume> cells{};
  for (const auto &[pos, state] : overrides) {
    palette.push_back(state.id());
    cells[lightIndex(pos.x, pos.y, pos.z)] =
        static_cast<std::uint8_t>(palette.size() - 1);
  }
  LightSectionView view;
  view.empty = empty;
  view.bits = 8;
  view.palette = std::move(palette);
  view.cells8 = cells;
  return makeLightGeometrySection(*optical, view);
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

LightChunkInput rawEmptyChunkFixture(int chunkX, int chunkZ,
                                     int minBlockSectionY,
                                     int blockSectionCount);

LightChunkInput emptyChunkFixture(int chunkX, int chunkZ,
                                  int minBlockSectionY,
                                  int blockSectionCount) {
  return rawEmptyChunkFixture(chunkX, chunkZ, minBlockSectionY,
                              blockSectionCount);
}

LightChunkInput chunkFixture(int chunkX, int chunkZ, int minBlockSectionY,
                             int blockSectionCount) {
  auto input =
      rawEmptyChunkFixture(chunkX, chunkZ, minBlockSectionY, blockSectionCount);
  if (!input.geometrySections.empty()) {
    const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
    input.geometrySections[0] =
        makeIndexedGeometrySection(airState(), {{{1, 2, 3}, lamp}}, false);
  }
  return input;
}

LightChunkInput rawEmptyChunkFixture(int chunkX, int chunkZ,
                                    int minBlockSectionY,
                                    int blockSectionCount) {
  LightChunkInput input;
  input.chunkX = chunkX;
  input.chunkZ = chunkZ;
  input.minBlockSectionY = minBlockSectionY;
  input.blockSectionCount = blockSectionCount;
  const auto optical = defaultLightOpticalTable();
  LightSectionView air;
  air.empty = true;
  air.bits = 0;
  air.single = airState().id();
  input.geometrySections.reserve(blockSectionCount);
  for (int section = 0; section < blockSectionCount; ++section)
    input.geometrySections.push_back(makeLightGeometrySection(*optical, air));
  return input;
}

LightChunkInput skyRoofChunk(int chunkX, int chunkZ) {
  auto input = rawEmptyChunkFixture(chunkX, chunkZ, 0, 1);
  const auto optical = defaultLightOpticalTable();
  std::array<std::uint8_t, kLightVolume> cells{};
  for (int z = 0; z < 16; ++z)
    for (int x = 0; x < 16; ++x)
      cells[lightIndex(x, 8, z)] = 1;
  const std::array<BlockState::Id, 2> palette = {
      airState().id(), Blocks::STONE.defaultState().id()};
  LightSectionView view;
  view.empty = false;
  view.bits = 8;
  view.palette = palette;
  view.cells8 = cells;
  input.geometrySections[0] = makeLightGeometrySection(*optical, view);
  return input;
}

bool reportBlock(LightService &service, int chunkX, int chunkZ, Pos position,
                 const BlockState &, const BlockState &newState) {
  service.reportBlockCell({chunkX * 16 + (position.x & 15), position.y,
                           chunkZ * 16 + (position.z & 15), newState.id()});
  return true;
}

void reportOwnedCell(LightService &service, int chunkX, int chunkZ,
                     Pos position, const BlockState &state,
                     bool = false, std::uint64_t = 0) {
  service.reportBlockCell({chunkX * 16 + (position.x & 15), position.y,
                           chunkZ * 16 + (position.z & 15), state.id()});
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
  require(mixed.isMixed() && mixed.packedByteCount() != 0 &&
              mixed.packedByteCount() == 2048,
          "mixed layer owns packed nibbles");
  for (int y = 0; y < 16; ++y)
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x)
        require(mixed.get(x, y, z) == values[lightIndex(x, y, z)],
                "DataLayer uses y<<8|z<<4|x");
  const std::size_t odd = lightIndex(1, 2, 3);
  require((mixed.packedByte(odd >> 1) >> 4) == values[odd],
          "odd indices use the high nibble");
  const std::size_t even = lightIndex(0, 2, 3);
  require((mixed.packedByte(even >> 1) & 15) == values[even],
          "even indices use the low nibble");
  std::array<std::uint8_t, kLightLayerBytes> packed{};
  mixed.readPacked(packed);
  require(LightLayerSnapshot::mixedPacked(packed) == mixed,
          "2048-byte packed layer round-trips exactly");

  const auto missing = LightLayerSnapshot::missing();
  const auto zero = LightLayerSnapshot::uniform(0);
  const auto full = LightLayerSnapshot::uniform(15);
  require(missing.isMissing() && zero.isUniform() && full.isUniform() &&
              zero.stateByte() == kLightUniformFlag &&
              full.stateByte() == (kLightUniformFlag | 15),
          "missing and compact uniform state bytes remain distinct");
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
  require(input.geometrySections.size() == 3 &&
              !input.geometrySections[0].uniform &&
              input.geometrySections[1].uniform,
          "geometry sections store compact uniform or 4096-cell class arrays");

  LightService service(-4, 3, LightWorkerOptions{1000, 4096, false, false});
  const auto generation = service.submitChunk(std::move(input));
  require(generation != 0, "chunk snapshot is accepted");
  require(service.waitIdleForTesting(), "light worker finishes snapshot");
  service.drainPublished();
  auto snapshot = service.latestLayerChunk(LightLayer::Block, -2, 3);
  require(snapshot && snapshot->generation == generation,
          "published chunk keeps its generation");
  require(snapshot->skyLightComplete && snapshot->propagationComplete &&
              snapshot->protocolReady,
          "completed light layers become protocol ready");
  require(snapshot->section(-5) && snapshot->section(-3) &&
              snapshot->section(-1) && snapshot->section(-6) == nullptr &&
              snapshot->section(0) == nullptr &&
              snapshot->lightSectionCount() == 5,
          "published snapshot keeps all light padding sections and bounds");
  require(snapshot->section(-4)->light(LightLayer::Block, 1, 2, 3) == 15 &&
              snapshot->section(-4)->light(LightLayer::Sky, 1, 2, 3) == 0 &&
              service.latestLayerChunk(LightLayer::Sky, -2, 3) == nullptr,
          "disabled sky dimension leaves sky layer absent");
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
    {
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
  require(rows == 29873,
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
                snapshot->protocolReady,
            "callback receives a complete protocol-ready snapshot");
  });
  const auto firstGeneration =
      service.submitChunk(chunkFixture(3, -2, -4, 3));
  require(firstGeneration == 1, "first snapshot has generation one");
  require(service.waitIdleForTesting(), "worker reaches idle");
  service.drainPublished();
  require(callbacks == 1 && callbackThread == mainThread,
          "publication callback runs on the main thread");
  auto first = service.latestLayerChunk(LightLayer::Block, 3, -2);
  require(first && first->generation == firstGeneration,
          "first immutable snapshot is visible after drain");
  const int beforeLight =
      first->section(-4)->light(LightLayer::Block, 1, 4, 5);

  service.reportBlockCell(
      {3 * 16 + 1, (-4) * 16 + 4, (-2) * 16 + 5,
       Blocks::REDSTONE_LAMP.defaultState().with("lit", "true").id()});
  require(service.waitIdleForTesting(), "worker applies block change");
  service.drainPublished();
  auto second = service.latestLayerChunk(LightLayer::Block, 3, -2);
  require(second && second->generation == 2,
          std::string("new generation is published after a block change: ") +
              (second ? std::to_string(second->generation) : "missing"));
  const auto updatedOptical =
      service.opticalPropertiesForTesting(3 * 16 + 1, (-4) * 16 + 4, (-2) * 16 + 5);
  require(updatedOptical && updatedOptical->emission == 15 &&
              second->section(-4)->light(LightLayer::Block, 1, 4, 5) == 15,
          "worker updates owned optical metadata");
  require(beforeLight < 15,
          "previously captured light level remains below the new source");

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
  auto reloaded = staleService.latestLayerChunk(LightLayer::Block, 1, 1);
  require(reloaded && reloaded->generation == reloadGeneration,
          "stale worker result cannot replace a reloaded chunk");
}

void publicationBatchVisibility() {
  LightService service(-1, 1, LightWorkerOptions{1000, 16, true, false});
  std::vector<std::shared_ptr<const LightChunkSnapshot>> delivered;
  service.setPublishCallback([&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
    delivered.push_back(std::move(snapshot));
  });
  require(service.submitChunk(emptyChunkFixture(-1, 0, -1, 1)) != 0 &&
              service.submitChunk(emptyChunkFixture(0, 0, -1, 1)) != 0,
          "two-chunk publication batch is queued while paused");
  require(service.latestLayerChunk(LightLayer::Block, -1, 0) == nullptr &&
              service.latestLayerChunk(LightLayer::Block, 0, 0) == nullptr,
          "paused batch publishes neither chunk early");
  service.resumeForTesting();
  require(service.waitIdleForTesting(), "two-chunk publication batch completes");
  service.drainPublished();
  require(delivered.size() == 2 &&
              delivered[0]->revision == delivered[1]->revision,
          "both chunks from one worker batch become visible with one revision");
  require(service.latestLayerChunk(LightLayer::Block, 0, 0) == delivered[0] &&
              service.latestLayerChunk(LightLayer::Block, -1, 0) == delivered[1],
          "one drain publishes the complete batch in deterministic key order");
}

void coalescedProtocolDeltaIsCumulative() {
  const auto air = airState();
  const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  LightService service(0, 2, LightWorkerOptions{1000, 16, false, false});
  require(service.submitChunk(emptyChunkFixture(0, 0, 0, 2)) != 0,
          "cumulative-delta fixture loads");
  require(service.waitIdleForTesting(), "cumulative-delta baseline completes");
  service.drainPublished();
  auto baseline = service.latestLayerChunk(LightLayer::Block, 0, 0);
  require(baseline != nullptr, "cumulative-delta baseline published");
  service.pauseForTesting();
  require(reportBlock(service, 0, 0, {1, 1, 1}, air, lamp) &&
              service.processChunkBatchForTesting(0, 0),
          "first undrained revision completes");
  require(reportBlock(service, 0, 0, {1, 17, 1}, air, lamp) &&
              service.processChunkBatchForTesting(0, 0),
          "second undrained revision completes");
  service.drainPublished();
  auto latest = service.latestLayerChunk(LightLayer::Block, 0, 0);
  const auto sectionZero = std::find_if(
      latest->changedSections.begin(), latest->changedSections.end(),
      [](const auto &change) {
        return change.sectionY == 0 && change.block;
      });
  const auto sectionOne = std::find_if(
      latest->changedSections.begin(), latest->changedSections.end(),
      [](const auto &change) {
        return change.sectionY == 1 && change.block;
      });
  require(sectionZero != latest->changedSections.end() &&
              sectionOne != latest->changedSections.end(),
          "coalesced publication preserves both undrained layer deltas");
  require(latest->deltaBaseRevision == baseline->revision,
          "coalesced ready delta keeps the last drained base revision");
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
  require(service.submitChunk(LightChunkInput{}) == 0 &&
              service.submitChunk(emptyChunkFixture(0, 0, 0, 1)) == 0,
          "invalid chunk submissions are rejected explicitly");
  const auto invalidGeneration = service.stats().rejected;
  require(invalidGeneration == 2, "invalid requests are counted as rejected");
  service.resumeForTesting();
  require(service.waitIdleForTesting(), "paused worker drains coalesced work");
  service.drainPublished();
  require(service.latestLayerChunk(LightLayer::Block, 0, 0) &&
              service.latestLayerChunk(LightLayer::Block, 1, 0),
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
  auto delivered = draining.latestLayerChunk(LightLayer::Block, 4, 0);
  require(delivered && delivered->generation == generation,
          "close drains queued work before joining the worker");

  LightService batching(-1, 1, LightWorkerOptions{1000, 16, true});
  const auto loadGeneration = batching.submitChunk(chunkFixture(5, 0, -1, 1));
  require(loadGeneration == 1, "batched fixture loads once");
  for (int x = 0; x < 2; ++x)
    batching.reportBlockCell(
        {5 * 16 + x, -15, 1,
         (x == 0 ? Blocks::STONE.defaultState()
                 : Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"))
             .id()});
  batching.resumeForTesting();
  require(batching.waitIdleForTesting(), "coalesced batch completes");
  const auto batchStats = batching.stats();
  require(batchStats.processedBlockChanges == 2 &&
              batchStats.processedBatches == 1,
          "block changes share one worker batch/task");
  batching.drainPublished();
  auto batched = batching.latestLayerChunk(LightLayer::Block, 5, 0);
  require(batched && batched->generation == 3,
          "coalesced changes carry the newest generation");

  LightWorkerOptions windowedOptions;
  require(windowedOptions.batchWindowMs == 30,
          "default batch collection window is 30 ms");
  windowedOptions.batchWindowMs = 60;
  LightService windowed(0, 1, windowedOptions);
  require(windowed.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0 &&
              windowed.waitIdleForTesting(),
          "windowed fixture loads");
  require(windowed.waitWorkerSleepingForTesting(),
          "windowed worker reaches sleep before window test");
  const auto batchesBefore = windowed.stats().processedBatches;
  reportOwnedCell(windowed, 0, 0, {8, 8, 8},
                  Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  reportOwnedCell(windowed, 0, 0, {8, 8, 8}, airState());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (windowed.stats().processedBatches == batchesBefore &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  require(windowed.waitIdleForTesting(), "windowed worker finishes cleanly");
  const auto afterWindow = windowed.stats();
  require(afterWindow.processedBatches == batchesBefore + 1 &&
              afterWindow.processedBlockChanges == 0,
          "timed collection window deduplicates toggling writes in one batch");
}

void recoverableChangesAndBoundedPublication() {
  LightService service(-1, 1, LightWorkerOptions{1, 512, true});
  require(service.submitChunk(chunkFixture(9, 0, -1, 1)) != 0 &&
              service.submitChunk(chunkFixture(10, 0, -1, 1)) != 0,
          "small-capacity worker retains both pending chunks");

  const auto lampOn =
      Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  for (int x = 0; x < 4; ++x)
    service.reportBlockCell({10 * 16 + x, -15, 1, lampOn.id()});
  require(service.unloadChunk(9, 0), "unload tombstone is retained");
  require(service.stats().pendingChunks >= 2,
          "pending controls remain in the SPSC delivery queue while paused");

  service.resumeForTesting();
  require(service.waitIdleForTesting(), "recovery state drains completely");
  require(service.stats().pendingPublications >= 1,
          "completed layer results remain queued until the main drain");
  service.drainPublished();
  require(service.latestLayerChunk(LightLayer::Block, 9, 0) == nullptr,
          "delivered unload tombstone prevents old-generation resurrection");
  auto newest = service.latestLayerChunk(LightLayer::Block, 10, 0);
  const auto newestOpt =
      service.opticalPropertiesForTesting(10 * 16 + 3, -15, 1);
  require(newest && newestOpt && newestOpt->emission == 15 &&
              blockLightAt(newest, 10 * 16 + 3, -15, 1) == 15,
          "all coalesced live changes remain visible after overflow");
  require(newest->lifecycleEpoch != 0 &&
              newest->revision != 0,
          "coalesced result has a worker publication revision");

  for (int i = 0; i < 200; ++i)
    service.reportBlockCell(
        {10 * 16 + (i & 15), -16, 0,
         (i & 1 ? Blocks::STONE.defaultState() : lampOn).id()});
  require(service.waitIdleForTesting(), "long pending write reaches worker");
  require(service.stats().pendingPublications >= 1,
          "same-chunk generations remain queued until main drain");

  LightService sectionBatch(-1, 1, LightWorkerOptions{100, 16, true});
  require(sectionBatch.submitChunk(emptyChunkFixture(11, 0, -1, 1)) != 0,
          "empty section batch fixture loads");
  sectionBatch.reportBlockCell(
      {11 * 16 + 1, -15, 1, Blocks::STONE.defaultState().id()});
  sectionBatch.reportBlockCell({11 * 16 + 1, -15, 1, airState().id()});
  sectionBatch.reportBlockCell({11 * 16 + 2, -15, 1, lampOn.id()});
  sectionBatch.resumeForTesting();
  require(sectionBatch.waitIdleForTesting(), "unordered cell batch completes");
  sectionBatch.drainPublished();
  auto sectionSnapshot =
      sectionBatch.latestLayerChunk(LightLayer::Block, 11, 0);
  require(sectionSnapshot && !sectionSnapshot->section(-1)->empty,
          "final per-section state wins over historical cell emptiness");
  const auto p1 = sectionBatch.opticalPropertiesForTesting(11 * 16 + 1, -15, 1);
  const auto p2 = sectionBatch.opticalPropertiesForTesting(11 * 16 + 2, -15, 1);
  require(p1 && p2 && p1->emission == 0 && p2->emission == 15,
          "coalesced fill-clear-fill keeps final cell contents");
}

std::shared_ptr<const LightChunkSnapshot>
settledChunk(LightService &service, int chunkX, int chunkZ);

void ownedCellQueueGrowthAndWakeup() {
  LightWorkerOptions small;
  small.batchSize = 16;
  small.maxQueuedCommands = 16;
  small.startPausedForTesting = true;
  small.hasSkyLight = false;
  small.batchWindowMs = 0;
  small.cellQueueCapacity = 4;
  LightService service(-1, 1, small);
  require(service.submitChunk(emptyChunkFixture(0, 0, -1, 1)) != 0,
          "small owned-cell ring fixture loads");
  for (int x = 0; x < 6; ++x)
    reportOwnedCell(service, 0, 0, {x, -15, 1}, Blocks::STONE.defaultState());
  // This newest record crosses the initial segment boundary and targets a
  // cell queued in the first segment. It must retain global FIFO ordering.
  reportOwnedCell(service, 0, 0, {0, -15, 1},
                  Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
  require(service.stats().rejected == 0 &&
              service.stats().droppedGeometryTasks == 0,
          "a full initial geometry segment grows without rejecting work");
  service.resumeForTesting();
  require(service.waitIdleForTesting(), "accepted geometry records drain");
  service.drainPublished();
  auto snapshot = service.latestLayerChunk(LightLayer::Block, 0, 0);
  const auto p0 = service.opticalPropertiesForTesting(0, -15, 1);
  const auto p5 = service.opticalPropertiesForTesting(5, -15, 1);
  require(snapshot && p0 && p5 && p0->emission == 15 &&
              p5->dampening == 15,
          "records appended to a new segment replay in order with the newest "
          "value winning");

  LightWorkerOptions wakeOptions;
  wakeOptions.batchWindowMs = 0;
  LightService waking(0, 1, wakeOptions);
  require(waking.submitChunk(emptyChunkFixture(1, 1, 0, 1)) != 0,
          "wake fixture loads");
  (void)settledChunk(waking, 1, 1);
  require(waking.waitWorkerSleepingForTesting(),
          "worker advertises its pre-wait state");
  reportOwnedCell(waking, 1, 1, {1, 1, 1},
                  Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
  require(waking.waitIdleForTesting(),
          "one record after the sleep transition wakes the worker");
  waking.drainPublished();
  auto woken = waking.latestLayerChunk(LightLayer::Block, 1, 1);
  const auto wokenOpt = waking.opticalPropertiesForTesting(16 + 1, 1, 16 + 1);
  require(woken && wokenOpt && wokenOpt->emission == 15,
          "the wake race path publishes the single queued record");

  LightWorkerOptions closeOptions;
  closeOptions.startPausedForTesting = true;
  closeOptions.batchWindowMs = 0;
  closeOptions.cellQueueCapacity = 2;
  LightService closing(-1, 1, closeOptions);
  require(closing.submitChunk(emptyChunkFixture(3, 0, -1, 1)) != 0,
          "close fixture loads");
  for (int x = 0; x < 3; ++x)
    reportOwnedCell(closing, 3, 0, {x, -15, 1}, Blocks::STONE.defaultState());
  require(closing.stats().rejected == 0 &&
              closing.stats().droppedGeometryTasks == 0,
          "close drains records appended to additional segments");
  closing.close();
  closing.drainPublished();
  auto closed = closing.latestLayerChunk(LightLayer::Block, 3, 0);
  const auto c0 = closing.opticalPropertiesForTesting(3 * 16 + 0, -15, 1);
  const auto c1 = closing.opticalPropertiesForTesting(3 * 16 + 1, -15, 1);
  const auto c2 = closing.opticalPropertiesForTesting(3 * 16 + 2, -15, 1);
  require(closed && c0 && c1 && c2 && c0->dampening == 15 &&
              c1->dampening == 15 && c2->dampening == 15,
          "close drains every accepted record across SPSC segments");
  require(closing.stats().rejected == 0,
          "draining records accepted before close does not reject them");
  reportOwnedCell(closing, 3, 0, {3, -15, 1}, Blocks::STONE.defaultState());
  require(closing.stats().rejected == 1,
          "a cell submitted after close is rejected explicitly");
  require(closing.submitChunk(emptyChunkFixture(4, 0, -1, 1)) == 0 &&
              closing.stats().rejected == 2,
          "a chunk submitted after close is rejected explicitly");
}

void growableResultQueueHandsOffBurst() {
  LightWorkerOptions options;
  options.batchWindowMs = 0;
  options.startPausedForTesting = true;
  options.hasSkyLight = false;
  options.resultQueueBytes = 2 * 32;
  LightService service(-1, 1, options);
  for (int chunkX = 10; chunkX < 14; ++chunkX)
    require(service.submitChunk(emptyChunkFixture(chunkX, 0, -1, 1)) != 0,
            "result-growth chunk load is accepted");
  service.resumeForTesting();
  require(service.waitIdleForTesting(),
          "worker completes while the main result consumer is not draining");
  service.drainPublished();
  for (int chunkX = 10; chunkX < 14; ++chunkX)
    require(service.latestLayerChunk(LightLayer::Block, chunkX, 0) != nullptr,
            "each result appended beyond the first segment is delivered");
  require(service.stats().pendingPublications == 0,
          "the grown result ring is fully drained");
}

void segmentedQueuesPreserveConcurrentLifecycle() {
  LightWorkerOptions options;
  options.batchWindowMs = 0;
  options.hasSkyLight = false;
  options.maxQueuedCommands = 2;
  options.cellQueueCapacity = 2;
  options.resultQueueBytes = 2 * 32;
  LightService service(-1, 1, options);
  for (int chunkX = 20; chunkX < 68; ++chunkX)
    require(service.submitChunk(emptyChunkFixture(chunkX, 0, -1, 1)) != 0,
            "segmented lifecycle load remains accepted while the worker runs");
  require(service.waitIdleForTesting(),
          "concurrent segmented lifecycle controls drain completely");
  service.drainPublished();
  for (int chunkX = 20; chunkX < 68; ++chunkX)
    require(service.latestLayerChunk(LightLayer::Block, chunkX, 0) != nullptr,
            "every lifecycle control across appended segments is published");
  require(service.stats().rejected == 0 &&
              service.stats().droppedGeometryTasks == 0,
          "control growth rejects no lifecycle work");

  service.pauseForTesting();
  for (int round = 0; round < 12; ++round) {
    reportOwnedCell(service, 20, 0, {round & 15, -15, 1},
                    Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
    require(service.submitChunk(emptyChunkFixture(20, 0, -1, 1)) != 0,
            "refresh control follows geometry across segment rollover");
  }
  service.resumeForTesting();
  require(service.waitIdleForTesting(),
          "segmented geometry and refresh controls settle");
  service.drainPublished();
  auto refreshed = service.latestLayerChunk(LightLayer::Block, 20, 0);
  require(refreshed && refreshed->section(-1)->empty,
          "a later refresh consumes every preceding geometry record");
  require(service.stats().rejected == 0 &&
              service.stats().droppedGeometryTasks == 0,
          "ordered segmented input keeps invalid and dropped counts at zero");
}

void rawSubmissionAndGeometryMirror() {
  static_assert(sizeof(LightCellUpdate) == 16,
                "ordinary light input is four 32-bit words");
  static_assert(alignof(LightCellUpdate) == 16,
                "ordinary light input stays naturally aligned");

  LightWorkerOptions options;
  options.hasSkyLight = false;
  options.batchWindowMs = 0;
  options.startPausedForTesting = true;
  options.cellQueueCapacity = 8;
  LightService service(0, 1, options);
  require(service.submitChunk(rawEmptyChunkFixture(0, 0, 0, 1)) != 0,
          "raw tiny-ring fixture loads");
  service.resumeForTesting();
  require(service.waitIdleForTesting(), "raw tiny-ring fixture settles");
  service.drainPublished();
  const auto oldEpoch =
      service.latestLayerChunk(LightLayer::Block, 0, 0)->lifecycleEpoch;

  service.pauseForTesting();
  reportOwnedCell(service, 0, 0, {0, 1, 1}, Blocks::STONE.defaultState());
  reportOwnedCell(service, 0, 0, {1, 1, 1}, Blocks::STONE.defaultState());
  reportOwnedCell(service, 0, 0, {2, 1, 1},
                  Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
  require(service.submitChunk(rawEmptyChunkFixture(0, 0, 0, 1)) != 0,
          "same-boundary authoritative refresh enters");
  require(service.unloadChunk(0, 0), "same-boundary unload enters");
  require(service.submitChunk(rawEmptyChunkFixture(0, 0, 0, 1)) != 0,
          "same-boundary reload enters");
  reportOwnedCell(service, 0, 0, {5, 1, 1},
                  Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
  service.resumeForTesting();
  require(service.waitIdleForTesting(),
          "same-boundary controls and overflow records settle");
  service.drainPublished();
  const auto latest = service.latestLayerChunk(LightLayer::Block, 0, 0);
  require(latest && latest->lifecycleEpoch > oldEpoch,
          "same-boundary controls install the new lifecycle");
  const auto p0 = service.opticalPropertiesForTesting(0, 1, 1);
  const auto p1 = service.opticalPropertiesForTesting(1, 1, 1);
  const auto p2 = service.opticalPropertiesForTesting(2, 1, 1);
  const auto p5 = service.opticalPropertiesForTesting(5, 1, 1);
  require(p0 && p1 && p2 && p5 && p0->emission == 0 &&
              p1->emission == 0 && p2->emission == 0 &&
              p5->emission == 15,
          "pre-refresh ordinary/overflow input cannot replay after controls");

  LightService equivalent(0, 1);
  require(equivalent.submitChunk(rawEmptyChunkFixture(1, 0, 0, 1)) != 0,
          "raw equivalence fixture loads");
  require(equivalent.waitIdleForTesting(), "raw equivalence fixture settles");
  equivalent.drainPublished();
  reportOwnedCell(equivalent, 1, 0, {0, 1, 1},
                  Blocks::STONE.defaultState());
  require(equivalent.waitIdleForTesting(), "stone class is installed");
  equivalent.drainPublished();
  equivalent.resetMeasurementStats();
  reportOwnedCell(equivalent, 1, 0, {0, 1, 1},
                  Blocks::SMOOTH_STONE.defaultState());
  require(equivalent.waitIdleForTesting(),
          "same-class block replacement reaches the coordinator");
  require(equivalent.stats().processedBlockChanges == 0,
          "same optical class does not seed propagation");
  equivalent.drainPublished();
  reportOwnedCell(equivalent, 1, 0, {0, 1, 1},
                  Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"));
  require(equivalent.waitIdleForTesting(),
          "different optical class reaches the coordinator");
  require(equivalent.stats().processedBlockChanges == 1,
          "different optical class seeds exactly one changed cell");
}

void ownedCellLifecycleRejectsReloadStaleWork() {
  LightWorkerOptions options;
  options.batchWindowMs = 0;
  options.startPausedForTesting = true;
  options.cellQueueCapacity = 8;
  LightService service(-1, 1, options);
  auto first = emptyChunkFixture(2, 0, -1, 1);
  first.lifecycleEpoch = 7;
  require(service.submitChunk(std::move(first)) != 0,
          "first owned lifecycle loads");
  service.resumeForTesting();
  (void)settledChunk(service, 2, 0);
  service.pauseForTesting();
  reportOwnedCell(service, 2, 0, {1, -15, 1},
                  Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"),
                  false, 7);
  require(service.unloadChunk(2, 0), "owned lifecycle unloads");
  auto second = emptyChunkFixture(2, 0, -1, 1);
  second.lifecycleEpoch = 8;
  require(service.submitChunk(std::move(second)) != 0,
          "same coordinates load a new owned lifecycle");
  service.resumeForTesting();
  require(service.waitIdleForTesting(), "reload and stale records settle");
  service.drainPublished();
  auto reloaded = service.latestLayerChunk(LightLayer::Block, 2, 0);
  const auto reloadedOpt =
      service.opticalPropertiesForTesting(2 * 16 + 1, -15, 1);
  require(reloaded && reloaded->lifecycleEpoch == 8 && reloadedOpt &&
              reloadedOpt->emission == 0,
          "an old-lifetime cell cannot enter the reloaded chunk: epoch=" +
              std::to_string(reloaded ? reloaded->lifecycleEpoch : 0) +
              " emission=" +
              std::to_string(reloadedOpt ? reloadedOpt->emission : 255));
  require(service.stats().pendingChunks == 0,
          "ordered lifecycle consumes stale input without replay");
}

void authoritativeRefreshOutranksQueuedCells() {
  LightWorkerOptions options;
  options.batchWindowMs = 0;
  options.cellQueueCapacity = 8;
  LightService service(-1, 1, options);
  auto initial = emptyChunkFixture(4, 0, -1, 1);
  initial.lifecycleEpoch = 3;
  require(service.submitChunk(std::move(initial)) != 0,
          "refresh fixture loads its first generation");
  (void)settledChunk(service, 4, 0);
  service.pauseForTesting();
  reportOwnedCell(service, 4, 0, {1, -15, 1},
                  Blocks::REDSTONE_LAMP.defaultState().with("lit", "true"),
                  false, 3);
  auto refreshed = emptyChunkFixture(4, 0, -1, 1);
  refreshed.lifecycleEpoch = 3;
  require(service.submitChunk(std::move(refreshed)) != 0,
          "same-generation authoritative refresh is accepted");
  service.resumeForTesting();
  require(service.waitIdleForTesting(),
          "refresh and its older queued cell settle");
  service.drainPublished();
  auto snapshot = service.latestLayerChunk(LightLayer::Block, 4, 0);
  const auto snapshotOpt =
      service.opticalPropertiesForTesting(4 * 16 + 1, -15, 1);
  require(snapshot && snapshot->section(-1)->empty && snapshotOpt &&
              snapshotOpt->emission == 0,
          "an older ring cell cannot resurrect after authoritative refresh");
}

void wideWorldInputsRemainOwned() {
  World world;
  world.loadChunk(0, 0);
  const auto noteStates = findBlockByName("minecraft:note_block")->states();
  require(noteStates.size() >= 300, "note block supplies a wide palette fixture");
  for (int n = 0; n < 300; ++n) {
    const int x = n & 15, z = (n >> 4) & 15, y = 64 + n / 256;
    world.rawSetBlock({x, y, z}, noteStates[static_cast<std::size_t>(n)]);
  }
  const int blockSection = (64 - world.minY()) / 16;
  require(world.snapshotChunk(0, 0).sections[blockSection].bits == 16,
          "wide fixture reaches 16-bit block indices");
  const int lightSection = 64 >> 4;
  LightWorkerOptions options;
  options.batchWindowMs = 0;
  world.enableLighting(options);
  auto *service = world.lightService();
  require(service->waitIdleForTesting(), "wide authoritative input completes");
  service->drainPublished();
  const auto before = service->latestLayerChunk(LightLayer::Block, 0, 0);
  const auto beforeOptical =
      service->opticalPropertiesForTesting(0, 64, 0);
  require(before && beforeOptical && beforeOptical->emission == 0,
          "wide source input is copied into the owned geometry mirror");

  service->pauseForTesting();
  require(world.setBlock({0, 64, 0},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "wide section accepts an owned emission update");
  require(world.setBlock({0, 64, 0}, Blocks::REDSTONE_LAMP.defaultState()),
          "wide section accepts an owned clear update");
  require(world.setBlock({0, 64, 0},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "wide section accepts the latest owned update");
  require(service->processChunkBatchForTesting(0, 0),
          "wide section processes one owned batch");
  service->drainPublished();
  const auto after = service->latestLayerChunk(LightLayer::Block, 0, 0);
  const auto afterOptical =
      service->opticalPropertiesForTesting(0, 64, 0);
  require(after && afterOptical && afterOptical->emission == 15,
          "same-cell updates keep the last owned optical value");
  require(beforeOptical->emission == 0,
          "the captured pre-update optical result remains immutable");

  service->pauseForTesting();
  require(world.setBlock({0, 64, 0}, Blocks::STONE.defaultState()),
          "pre-reload owned write is queued");
  world.unloadChunk(0, 0);
  world.loadChunk(0, 0);
  service->resumeForTesting();
  require(service->waitIdleForTesting(), "same-address reload settles owned input");
  service->drainPublished();
  const auto reloaded = service->latestLayerChunk(LightLayer::Block, 0, 0);
  const auto reloadedOptical =
      service->opticalPropertiesForTesting(0, 64, 0);
  require(reloaded && reloaded->lifecycleEpoch > after->lifecycleEpoch &&
              reloadedOptical && reloadedOptical->emission == 0,
          "pre-unload cell records cannot resurrect light after reload");
  world.disableLighting();
}

std::shared_ptr<const LightChunkSnapshot>
settledChunk(LightService &service, int chunkX, int chunkZ) {
  require(service.waitIdleForTesting(), "light worker reaches a complete batch");
  service.drainPublished();
  auto block = service.latestLayerChunk(LightLayer::Block, chunkX, chunkZ);
  auto sky = service.latestLayerChunk(LightLayer::Sky, chunkX, chunkZ);
  require(block != nullptr, "settled block chunk is published");
  if (!sky)
    return block;
  // Test-only inspection facade. Production stores and publishes the two
  // layer views separately; this creates no production combined view.
  auto merged = std::make_shared<LightChunkSnapshot>(*block);
  merged->skyLightComplete = sky->skyLightComplete;
  merged->propagationComplete =
      block->propagationComplete && sky->propagationComplete;
  merged->protocolReady = block->protocolReady && sky->protocolReady;
  const std::size_t skyIndex = static_cast<std::size_t>(LightLayer::Sky);
  for (std::size_t i = 0; i < merged->sections.size(); ++i) {
    const LightSectionSnapshot *source =
        i < sky->sections.size() ? sky->sections[i].get() : nullptr;
    if (!source)
      continue;
    auto section = std::make_shared<LightSectionSnapshot>(*merged->sections[i]);
    section->layers[skyIndex] = source->layers[skyIndex];
    merged->sections[i] = std::move(section);
  }
  return merged;
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
              initial->propagationComplete && initial->protocolReady &&
              blockLightAt(initial, 8, 8, 8) == 0,
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
              open->propagationComplete && open->protocolReady,
          "open sky completes both propagated layers and becomes protocol ready");
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
  auto refreshedWest = refresh.latestLayerChunk(LightLayer::Sky, -1, 0);
  require(refreshedEast->lifecycleEpoch == epoch && refreshedWest &&
              refreshedWest->lifecycleEpoch == epoch,
          "authoritative sky refresh keeps the loading epoch");
  require(skyLightAt(refreshedWest, -1, 7, 8) == 0,
          "same-epoch roof refresh removes stale cross-boundary sky light");
  require(refresh.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "same-epoch authoritative east snapshot restores open sky");
  (void)settledChunk(refresh, 0, 0);
  refresh.drainPublished();
  auto reopenedWest = refresh.latestLayerChunk(LightLayer::Sky, -1, 0);
  require(skyLightAt(reopenedWest, -1, 7, 8) == 14,
          "same-epoch open refresh relights the neighboring roofed boundary");

  LightService topWater(0, 1);
  auto waterColumn = emptyChunkFixture(0, 0, 0, 1);
  waterColumn.geometrySections[0] =
      makeUniformGeometrySection(Blocks::WATER.defaultState());
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
  require(reportBlock(storage, 0, 0, {8, 8, 8}, stone, air),
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
          "removing the final block drops raw layers and restores inherited full sky: " +
              std::to_string(skyLightAt(released, 8, 8, 8)) + "," +
              std::to_string(released->section(0)
                                 ->layers[static_cast<std::size_t>(
                                     LightLayer::Sky)]
                                 .isMissing()) + "," +
              std::to_string(released->section(1)
                                 ->layers[static_cast<std::size_t>(
                                     LightLayer::Sky)]
                                 .isMissing()));

  LightService edge(0, 12);
  require(edge.submitChunk(emptyChunkFixture(0, 0, 0, 12)) != 0 &&
              edge.submitChunk(emptyChunkFixture(1, 0, 0, 12)) != 0 &&
              edge.submitChunk(emptyChunkFixture(2, 0, 0, 12)) != 0,
          "cross-empty-section sky fixture loads A/B/C");
  (void)settledChunk(edge, 0, 0);
  for (int chunk = 0; chunk < 2; ++chunk)
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        if (!(chunk == 0 && x == 14 && z == 8))
          require(reportBlock(edge, chunk, 0, {chunk * 16 + x, 180, z},
                              air, stone),
                  "cross-empty-section roof enters worker");
  require(reportBlock(edge, 2, 0, {40, 130, 8}, air, stone),
          "third-chunk storage anchor enters worker");
  auto edgeOpen = settledChunk(edge, 0, 0);
  edge.drainPublished();
  auto edgeTarget = edge.latestLayerChunk(LightLayer::Sky, 1, 0);
  require(edgeOpen->section(9)->layers[static_cast<std::size_t>(
              LightLayer::Sky)]
              .isMissing() &&
              !edgeTarget->section(9)->layers[static_cast<std::size_t>(
                   LightLayer::Sky)]
                   .isMissing(),
          "source edge section is missing while target section is stored");
  const std::string edgeValues =
      std::to_string(skyLightAt(edgeOpen, 15, 160, 8)) + "/" +
      std::to_string(skyLightAt(edgeTarget, 16, 160, 8)) + "/" +
      std::to_string(skyLightAt(edgeTarget, 16, 159, 8)) + "/" +
      std::to_string(skyLightAt(edgeTarget, 16, 145, 8));
  require(skyLightAt(edgeOpen, 15, 160, 8) == 14 &&
              skyLightAt(edgeTarget, 16, 160, 8) == 13 &&
              skyLightAt(edgeTarget, 16, 159, 8) == 13 &&
              skyLightAt(edgeTarget, 16, 145, 8) == 13,
          "sky copies the horizontal edge level across a missing lower section: " +
              edgeValues);
  require(reportBlock(edge, 0, 0, {14, 180, 8}, air, stone),
          "cover the only upper opening");
  auto edgeClosed = settledChunk(edge, 0, 0);
  edge.drainPublished();
  auto edgeClosedTarget = edge.latestLayerChunk(LightLayer::Sky, 1, 0);
  require(skyLightAt(edgeClosedTarget, 16, 159, 8) == 0 &&
              skyLightAt(edgeClosedTarget, 16, 145, 8) == 0,
          "closing the opening clears the compensated lower-section column");

}

bool rawSkyMissing(
    const std::shared_ptr<const LightChunkSnapshot> &snapshot,
    int sectionY) {
  const LightSectionSnapshot *section = snapshot->section(sectionY);
  return section &&
         section->layers[static_cast<std::size_t>(LightLayer::Sky)]
             .isMissing();
}

void skyLayerIndexAndMissingInheritance() {
  const auto air = airState();
  const auto stone = Blocks::STONE.defaultState();

  LightService layered(0, 16);
  require(layered.submitChunk(emptyChunkFixture(0, 0, 0, 16)) != 0,
          "missing-layer fixture loads an open chunk");
  const auto open = settledChunk(layered, 0, 0);
  for (int sectionY = 0; sectionY < 16; ++sectionY)
    require(rawSkyMissing(open, sectionY) &&
                skyLightAt(open, 8, sectionY * 16 + 8, 8) == 15,
            "an open section stays raw-missing and inherits full sky");

  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      require(reportBlock(layered, 0, 0, {x, 248, z}, air, stone),
              "top-section roof enters the worker");
  const auto roofed = settledChunk(layered, 0, 0);
  require(!rawSkyMissing(roofed, 14) && !rawSkyMissing(roofed, 15),
          "the anchor stores its section and the section below");
  for (int sectionY = 0; sectionY < 14; ++sectionY) {
    const int missingLayers = 14 - sectionY;
    require(rawSkyMissing(roofed, sectionY),
            "section below a stored plane remains raw-missing");
    const int inherited = skyLightAt(roofed, 8, sectionY * 16 + 8, 8);
    require(inherited == 0,
            "missing layers 1..14 inherit the nearest stored bottom: " +
                std::to_string(missingLayers) + " value=" +
                std::to_string(inherited));
  }

  layered.pauseForTesting();
  require(reportBlock(layered, 0, 0, {8, 248, 8}, stone, air),
          "same-batch top-layer removal is accepted");
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      require(reportBlock(layered, 0, 0, {x, 224, z}, air, stone),
              "same-batch bottom-plane insertion is accepted");
  layered.resumeForTesting();
  const auto bottomChanged = settledChunk(layered, 0, 0);
  require(rawSkyMissing(bottomChanged, 0) &&
              skyLightAt(bottomChanged, 8, 8, 8) == 0,
          "a missing section reads the current edited bottom plane");

  layered.pauseForTesting();
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      require(reportBlock(layered, 0, 0, {x, 224, z}, stone, air),
              "same-batch bottom-plane removal is accepted");
  layered.resumeForTesting();
  const auto reopened = settledChunk(layered, 0, 0);
  const int reopenedValue = skyLightAt(reopened, 8, 8, 8);
  require(rawSkyMissing(reopened, 0) && reopenedValue == 15,
          "removing the edited bottom restores higher-section inheritance: " +
              std::to_string(reopenedValue) + " missing=" +
              std::to_string(rawSkyMissing(reopened, 0)));

  // Partial roof in section 15 produces mixed inherited columns when section 0
  // activates, exercising fillInheritedMixed packed nibble initialization.
  LightService mixed(0, 16);
  require(mixed.submitChunk(emptyChunkFixture(0, 0, 0, 16)) != 0,
          "mixed-inheritance fixture loads");
  (void)settledChunk(mixed, 0, 0);
  mixed.pauseForTesting();
  for (int x = 0; x < 8; ++x)
    for (int z = 0; z < 16; ++z)
      require(reportBlock(mixed, 0, 0, {x, 248, z}, air, stone),
              "partial-roof column enters the worker");
  mixed.resumeForTesting();
  (void)settledChunk(mixed, 0, 0);
  require(reportBlock(mixed, 0, 0, {12, 8, 8}, air, stone),
          "lower activation below partial roof enters the worker");
  const auto mixedInherited = settledChunk(mixed, 0, 0);
  require(!rawSkyMissing(mixedInherited, 0) &&
              skyLightAt(mixedInherited, 1, 8, 8) == 8 &&
              skyLightAt(mixedInherited, 14, 8, 8) == 15,
          "partial roof inheritance initializes mixed columns accurately");

  LightService single(0, 16);
  require(single.submitChunk(emptyChunkFixture(0, 0, 0, 16)) != 0,
          "single-anchor fixture loads");
  (void)settledChunk(single, 0, 0);
  require(reportBlock(single, 0, 0, {8, 248, 8}, air, stone),
          "single-anchor top plane enters the worker");
  (void)settledChunk(single, 0, 0);
  require(reportBlock(single, 0, 0, {8, 248, 8}, stone, air),
          "removing the final anchor drops the working layers");
  const auto releasedLayers = settledChunk(single, 0, 0);
  require(rawSkyMissing(releasedLayers, 14) &&
              rawSkyMissing(releasedLayers, 15) &&
              skyLightAt(releasedLayers, 8, 0, 8) == 15,
          "deleting the stored plane restores inherited full sky");

  LightService cross(0, 16);
  require(cross.submitChunk(emptyChunkFixture(0, 0, 0, 16)) != 0 &&
              cross.submitChunk(emptyChunkFixture(1, 0, 0, 16)) != 0 &&
              cross.submitChunk(emptyChunkFixture(2, 0, 0, 16)) != 0,
          "cross-chunk missing-layer fixture loads");
  (void)settledChunk(cross, 0, 0);
  for (int chunk = 0; chunk < 2; ++chunk)
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        if (!(chunk == 0 && x == 14 && z == 8))
          require(reportBlock(cross, chunk, 0,
                              {chunk * 16 + x, 180, z}, air, stone),
                  "cross-chunk roof enters the worker");
  require(reportBlock(cross, 2, 0, {40, 130, 8}, air, stone),
          "cross-chunk storage anchor enters the worker");
  const auto crossOpen = settledChunk(cross, 0, 0);
  cross.drainPublished();
  const auto crossTarget = cross.latestLayerChunk(LightLayer::Sky, 1, 0);
  require(crossOpen && crossTarget && rawSkyMissing(crossOpen, 9),
          "the open source section remains raw-missing");
  require(skyLightAt(crossTarget, 16, 160, 8) == 13 &&
              skyLightAt(crossTarget, 16, 159, 8) == 13 &&
              skyLightAt(crossTarget, 16, 145, 8) == 13,
          "cross-chunk opening fills missing lower sections");
  require(reportBlock(cross, 0, 0, {14, 180, 8}, air, stone),
          "closing the cross-chunk opening enters the worker");
  (void)settledChunk(cross, 0, 0);
  cross.drainPublished();
  const auto crossClosed = cross.latestLayerChunk(LightLayer::Sky, 1, 0);
  require(skyLightAt(crossClosed, 16, 159, 8) == 0 &&
              skyLightAt(crossClosed, 16, 145, 8) == 0,
          "closing the cross-chunk opening withdraws inherited sky");
}

std::string lightLayerSignature(
    const std::shared_ptr<const LightChunkSnapshot> &snapshot) {
  if (!snapshot)
    return "<missing>";
  std::ostringstream out;
  out << snapshot->chunkX << ',' << snapshot->chunkZ << ','
      << snapshot->minBlockSectionY << ',' << snapshot->blockSectionCount
      << ',' << snapshot->skyLightEnabled << ';';
  for (const auto &section : snapshot->sections) {
    out << section->sectionY << ':' << section->empty << ':';
    for (LightLayer layer : {LightLayer::Block, LightLayer::Sky}) {
      const auto &data = section->layers[static_cast<std::size_t>(layer)];
      out << static_cast<int>(data.storage()) << ','
          << static_cast<int>(data.uniformValue()) << ','
          << data.packedByteCount() << ',';
      for (std::size_t i = 0; i < data.packedByteCount(); ++i)
        out << static_cast<int>(data.packedByte(i)) << '.';
      out << ';';
    }
  }
  return out.str();
}

int referenceSkyColumnSource(
    const LightService &service, const LightChunkSnapshot &snapshot,
    int localX, int localZ) {
  const int minY = snapshot.minBlockSectionY * 16;
  const int maxY =
      (snapshot.minBlockSectionY + snapshot.blockSectionCount) * 16;
  BlockLightProperties top = lightPropertiesOf(airState());
  for (int y = maxY - 1; y >= minY; --y) {
    const auto optical = service.opticalPropertiesForTesting(
        snapshot.chunkX * 16 + localX, y, snapshot.chunkZ * 16 + localZ);
    if (!optical)
      return y + 1;
    const auto &bottom = *optical;
    if (bottom.supported && bottom.dampening != 0)
      return y + 1;
    if (lightFaceShapesOcclude(top, bottom, LightDirection::Down))
      return y + 1;
    top = bottom;
  }
  return minY - 1;
}

void verifyIncrementalSkySources(LightService &service, const char *stage) {
  require(service.waitIdleForTesting(),
          std::string("incremental sky source fixture settles: ") + stage);
  service.drainPublished();
  for (int chunkX = 0; chunkX < 2; ++chunkX) {
    const auto snapshot = service.latestLayerChunk(LightLayer::Sky, chunkX, 0);
    const auto sources = service.skyColumnSourcesForTesting(chunkX, 0);
    require(snapshot && sources,
            std::string("sky source cache is available: ") + stage);
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x) {
        const int actual =
            (*sources)[static_cast<std::size_t>(x + z * 16)];
        const int expected = referenceSkyColumnSource(service, *snapshot, x, z);
        require(actual == expected,
                std::string("incremental sky source matches full column: ") +
                    stage + " chunk=" + std::to_string(chunkX) +
                    " x=" + std::to_string(x) +
                    " z=" + std::to_string(z) +
                    " got=" + std::to_string(actual) +
                    " expected=" + std::to_string(expected));
      }
  }
}

void incrementalSkyColumnSources() {
  const auto air = airState();
  const auto stone = Blocks::STONE.defaultState();
  const auto extendedPistonUp = Blocks::PISTON.defaultState()
                                    .with("facing", "up")
                                    .with("extended", "true");
  LightWorkerOptions options;
  LightService service(0, 2, options);
  require(service.submitChunk(emptyChunkFixture(0, 0, 0, 2)) != 0 &&
              service.submitChunk(emptyChunkFixture(1, 0, 0, 2)) != 0,
          "incremental sky source fixture loads");
  verifyIncrementalSkySources(service, "initial");

  service.pauseForTesting();
  require(reportBlock(service, 0, 0, {8, 20, 8}, air, stone) &&
              reportBlock(service, 0, 0, {8, 10, 8}, air, stone) &&
              reportBlock(service, 0, 0, {8, 10, 8}, stone, air) &&
              reportBlock(service, 0, 0, {9, 18, 8}, air, stone),
          "same-column and multi-column changes enter one batch");
  service.resumeForTesting();
  verifyIncrementalSkySources(service, "same-column coalescing");
  const auto first = service.skyColumnSourcesForTesting(0, 0);
  require(first && (*first)[8 + 8 * 16] == 21 &&
              (*first)[9 + 8 * 16] == 19,
          "final batch geometry determines each column height");

  require(reportBlock(service, 0, 0, {8, 12, 8}, air, stone),
          "non-highest blocker enters");
  verifyIncrementalSkySources(service, "non-highest add");

  require(reportBlock(service, 0, 0, {8, 16, 8}, air, stone) &&
              reportBlock(service, 0, 0, {8, 15, 8}, air, stone),
          "cross-section upper and lower candidates enter");
  verifyIncrementalSkySources(service, "cross-section candidates");
  require(reportBlock(service, 0, 0, {8, 20, 8}, stone, air),
          "current highest blocker is removed");
  verifyIncrementalSkySources(service, "highest edge fallback");
  auto sources = service.skyColumnSourcesForTesting(0, 0);
  require(sources && (*sources)[8 + 8 * 16] == 17,
          "fallback reaches the next stored edge across a section boundary");

  require(reportBlock(service, 0, 0, {8, 16, 8}, stone, air),
          "upper cross-section blocker is removed");
  verifyIncrementalSkySources(service, "upper face fallback");
  sources = service.skyColumnSourcesForTesting(0, 0);
  require(sources && (*sources)[8 + 8 * 16] == 16,
          "y-1 edge remains the lowest source after upper removal");

  require(reportBlock(service, 0, 0, {8, 15, 8}, stone, air) &&
              reportBlock(service, 0, 0, {8, 12, 8}, stone, air),
          "remaining blockers are removed");
  verifyIncrementalSkySources(service, "empty column restoration");
  sources = service.skyColumnSourcesForTesting(0, 0);
  require(sources && (*sources)[8 + 8 * 16] == -1,
          "unobstructed column restores the no-blocker sentinel");

  require(service.submitChunk(emptyChunkFixture(0, 0, 0, 2)) != 0,
          "same-epoch full refresh enters");
  verifyIncrementalSkySources(service, "same-epoch refresh");

  require(service.unloadChunk(1, 0), "source fixture unloads");
  require(service.waitIdleForTesting(), "source fixture unload settles");
  require(!service.skyColumnSourcesForTesting(1, 0),
          "unload removes the column-source cache");
  require(service.submitChunk(emptyChunkFixture(1, 0, 0, 2)) != 0,
          "source fixture reloads");
  verifyIncrementalSkySources(service, "unload and reload");

  require(reportBlock(service, 0, 0, {2, 31, 8}, air, stone) &&
              reportBlock(service, 0, 0, {3, 0, 8}, air, stone),
          "top and bottom edges enter");
  verifyIncrementalSkySources(service, "top and bottom");

  require(reportBlock(service, 0, 0, {8, 20, 8}, air, stone),
          "multi-change fallback setup enters");
  service.pauseForTesting();
  require(reportBlock(service, 0, 0, {8, 19, 8}, air, stone) &&
              reportBlock(service, 0, 0, {8, 20, 8}, stone, air),
          "same-column lower insert and highest removal share one batch");
  service.resumeForTesting();
  verifyIncrementalSkySources(service, "same-batch lower insert");
  sources = service.skyColumnSourcesForTesting(0, 0);
  require(sources && (*sources)[8 + 8 * 16] == 20,
          "highest-edge fallback includes a lower cell changed in the same batch");
  require(reportBlock(service, 0, 0, {8, 19, 8}, stone, air),
          "same-batch fallback blocker is removed");
  verifyIncrementalSkySources(service, "same-batch fallback cleanup");

  require(reportBlock(service, 0, 0, {7, 16, 8}, air, extendedPistonUp) &&
              reportBlock(service, 0, 0, {7, 15, 8}, air, extendedPistonUp),
          "extended pistons facing up enter one batch");
  verifyIncrementalSkySources(service, "piston stack");
  sources = service.skyColumnSourcesForTesting(0, 0);
  require(sources && (*sources)[7 + 8 * 16] == 16,
          "piston stack starts at its upper DOWN face");
  require(reportBlock(service, 0, 0, {7, 16, 8}, extendedPistonUp, air),
          "upper facing-up piston is removed");
  verifyIncrementalSkySources(service, "piston upper removal");
  sources = service.skyColumnSourcesForTesting(0, 0);
  require(sources && (*sources)[7 + 8 * 16] == 15,
          "fallback does not count the lower cell against itself");
  require(reportBlock(service, 0, 0, {7, 15, 8}, extendedPistonUp, air),
          "remaining facing-up piston is removed");
  verifyIncrementalSkySources(service, "piston cleanup");

  LightService negative(-1, 2, options);
  require(negative.submitChunk(emptyChunkFixture(0, 0, -1, 2)) != 0,
          "negative source fixture loads");
  require(negative.waitIdleForTesting(), "negative source fixture settles");
  negative.drainPublished();
  require(reportBlock(negative, 0, 0, {3, -8, 3}, air, stone),
          "negative-y blocker enters");
  require(negative.waitIdleForTesting(), "negative-y source update settles");
  negative.drainPublished();
  const auto negativeSources = negative.skyColumnSourcesForTesting(0, 0);
  const auto negativeSnapshot =
      negative.latestLayerChunk(LightLayer::Sky, 0, 0);
  require(negativeSources && negativeSnapshot &&
              (*negativeSources)[3 + 3 * 16] ==
                  referenceSkyColumnSource(negative, *negativeSnapshot, 3, 3) &&
              (*negativeSources)[3 + 3 * 16] == -7,
          "negative-y source matches the full column reference");
}

void splitLightWorkerParityAndIsolation() {
  const auto air = airState();
  const auto stone = Blocks::STONE.defaultState();
  const auto lamp =
      Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");

  LightWorkerOptions singleOptions;
  LightWorkerOptions doubleOptions;
  LightService single(0, 12, singleOptions);
  LightService doubled(0, 12, doubleOptions);

  auto sync = [&](const char *stage) {
    require(single.waitIdleForTesting() && doubled.waitIdleForTesting(),
            std::string("split workers did not settle: ") + stage);
    single.drainPublished();
    doubled.drainPublished();
    for (int chunkX = 0; chunkX < 3; ++chunkX) {
      const auto leftBlock =
          single.latestLayerChunk(LightLayer::Block, chunkX, 0);
      const auto leftSky =
          single.latestLayerChunk(LightLayer::Sky, chunkX, 0);
      const auto rightBlock =
          doubled.latestLayerChunk(LightLayer::Block, chunkX, 0);
      const auto rightSky =
          doubled.latestLayerChunk(LightLayer::Sky, chunkX, 0);
      require(lightLayerSignature(leftBlock) ==
                  lightLayerSignature(rightBlock) &&
                  lightLayerSignature(leftSky) ==
                      lightLayerSignature(rightSky),
              std::string("single/two worker light mismatch: ") + stage +
                  " chunk=" + std::to_string(chunkX));
      if (!rightBlock)
        continue;
      require(rightBlock->layer == LightLayer::Block &&
                  rightSky && rightSky->layer == LightLayer::Sky &&
                  rightBlock->lifecycleEpoch == rightSky->lifecycleEpoch &&
                  rightBlock->revision != 0 && rightSky->revision != 0,
              "main stores two independently revised layer views");
      for (const auto &section : rightBlock->sections)
        require(section->layers[static_cast<std::size_t>(LightLayer::Sky)]
                    .isMissing(),
                "block view never retains a combined sky cell layer");
      for (const auto &section : rightSky->sections)
        require(section->layers[static_cast<std::size_t>(LightLayer::Block)]
                    .isMissing(),
                "sky view never retains a combined block cell layer");
    }
  };
  auto submitBoth = [&](int chunkX, int minSectionY, int sectionCount) {
    auto left = rawEmptyChunkFixture(chunkX, 0, minSectionY, sectionCount);
    auto right = rawEmptyChunkFixture(chunkX, 0, minSectionY, sectionCount);
    require(single.submitChunk(std::move(left)) != 0 &&
                doubled.submitChunk(std::move(right)) != 0,
            "split workers accept the same chunk input");
  };
  auto changeBoth = [&](Pos position, const BlockState &,
                        const BlockState &newState) {
    single.reportBlockCell(
        {position.x, position.y, position.z, newState.id()});
    doubled.reportBlockCell(
        {position.x, position.y, position.z, newState.id()});
  };

  for (int chunkX = 0; chunkX < 3; ++chunkX)
    submitBoth(chunkX, 0, 12);
  sync("initial load");

  for (int chunkX = 0; chunkX < 2; ++chunkX)
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        if (!(chunkX == 0 && x == 14 && z == 8))
          changeBoth({chunkX * 16 + x, 180, z}, air, stone);
  changeBoth({40, 130, 8}, air, stone);
  sync("cross-chunk opening");
  require(skyLightAt(doubled.latestLayerChunk(LightLayer::Sky, 1, 0), 16, 160, 8) == 13,
          "two workers preserve cross-chunk missing-layer sky propagation");

  changeBoth({14, 180, 8}, air, stone);
  sync("cross-chunk close");
  require(skyLightAt(doubled.latestLayerChunk(LightLayer::Sky, 1, 0), 16, 159, 8) == 0,
          "two workers preserve cross-chunk sky withdrawal");

  changeBoth({15, 100, 8}, air, lamp);
  sync("block source add");
  require(blockLightAt(doubled.latestLayerChunk(LightLayer::Block, 1, 0), 16, 100, 8) == 14,
          "two workers preserve block-light crossing");
  changeBoth({15, 100, 8}, lamp, air);
  sync("block source remove");

  submitBoth(0, 0, 12);
  sync("same-epoch refresh");

  require(single.unloadChunk(2, 0) && doubled.unloadChunk(2, 0),
          "both workers accept unload");
  require(single.waitIdleForTesting() && doubled.waitIdleForTesting(),
          "both workers finish unload");
  single.drainPublished();
  doubled.drainPublished();
  require(single.latestLayerChunk(LightLayer::Block, 2, 0) == nullptr &&
              doubled.latestLayerChunk(LightLayer::Block, 2, 0) == nullptr &&
              single.latestLayerChunk(LightLayer::Sky, 2, 0) == nullptr &&
              doubled.latestLayerChunk(LightLayer::Sky, 2, 0) == nullptr,
          "both modes withdraw unloaded output");
  submitBoth(2, 0, 12);
  sync("reload");

  LightService negativeSingle(-1, 2, singleOptions);
  LightService negativeDouble(-1, 2, doubleOptions);
  require(negativeSingle.submitChunk(rawEmptyChunkFixture(0, 0, -1, 2)) != 0 &&
              negativeDouble.submitChunk(rawEmptyChunkFixture(0, 0, -1, 2)) != 0,
          "negative-height split fixture loads");
  require(negativeSingle.waitIdleForTesting() &&
              negativeDouble.waitIdleForTesting(),
          "negative-height split fixture settles");
  negativeSingle.drainPublished();
  negativeDouble.drainPublished();
  require(lightLayerSignature(
              negativeSingle.latestLayerChunk(LightLayer::Block, 0, 0)) ==
              lightLayerSignature(
                  negativeDouble.latestLayerChunk(LightLayer::Block, 0, 0)) &&
              lightLayerSignature(
                  negativeSingle.latestLayerChunk(LightLayer::Sky, 0, 0)) ==
                  lightLayerSignature(
                      negativeDouble.latestLayerChunk(LightLayer::Sky, 0, 0)),
          "negative padding and inherited missing layers match");

  LightWorkerOptions noSkySingle;
  LightWorkerOptions noSkyDouble;
  noSkySingle.hasSkyLight = false;
  noSkyDouble.hasSkyLight = false;
  LightService withoutSkySingle(0, 1, noSkySingle);
  LightService withoutSkyDouble(0, 1, noSkyDouble);
  require(withoutSkySingle.submitChunk(rawEmptyChunkFixture(9, 0, 0, 1)) != 0 &&
              withoutSkyDouble.submitChunk(rawEmptyChunkFixture(9, 0, 0, 1)) != 0,
          "no-sky split fixture loads");
  require(withoutSkySingle.waitIdleForTesting() &&
              withoutSkyDouble.waitIdleForTesting(),
          "no-sky split fixture settles");
  withoutSkySingle.drainPublished();
  withoutSkyDouble.drainPublished();
  const auto noSky = withoutSkyDouble.latestLayerChunk(LightLayer::Sky, 9, 0);
  const auto hasBlock =
      withoutSkyDouble.latestLayerChunk(LightLayer::Block, 9, 0);
  require(noSky == nullptr && hasBlock != nullptr &&
              withoutSkyDouble.stats().workerCount == 1,
          "a no-sky service keeps sky absent and runs one worker");

  std::atomic<bool> skyEntered{false};
  std::atomic<bool> skyRelease{false};
  LightWorkerOptions slowOptions;
  slowOptions.beforeLayerForTesting = [&](LightLayer layer) {
    if (layer != LightLayer::Sky)
      return;
    skyEntered.store(true, std::memory_order_release);
    while (!skyRelease.load(std::memory_order_acquire))
      std::this_thread::yield();
  };
  LightService slow(0, 1, slowOptions);
  require(slow.submitChunk(rawEmptyChunkFixture(20, 0, 0, 1)) != 0,
          "slow-sky fixture submits");
  while (!skyEntered.load(std::memory_order_acquire))
    std::this_thread::yield();
  // The sky hook can run before the coordinator's block worker returns. Wait
  // only for the independent block publication while sky remains blocked.
  const auto blockDeadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (slow.latestLayerChunk(LightLayer::Block, 20, 0) == nullptr &&
         std::chrono::steady_clock::now() < blockDeadline) {
    slow.drainPublished();
    std::this_thread::yield();
  }
  const bool blockPublished =
      slow.latestLayerChunk(LightLayer::Block, 20, 0) != nullptr;
  const bool skyStillBlocked =
      slow.latestLayerChunk(LightLayer::Sky, 20, 0) == nullptr;
  skyRelease.store(true, std::memory_order_release);
  require(blockPublished && skyStillBlocked,
          "finished block publishes while slow sky is still running");
  require(slow.waitIdleForTesting(), "slow-sky fixture finishes after release");
  slow.drainPublished();
  require(slow.latestLayerChunk(LightLayer::Block, 20, 0) != nullptr &&
              slow.latestLayerChunk(LightLayer::Sky, 20, 0) != nullptr,
          "slow sky publishes independently when it eventually finishes");

  LightWorkerOptions failureOptions;
  failureOptions.beforeLayerForTesting = [](LightLayer layer) {
    if (layer == LightLayer::Sky)
      throw std::runtime_error("injected sky failure");
  };
  LightService failed(0, 1, failureOptions);
  require(failed.submitChunk(rawEmptyChunkFixture(21, 0, 0, 1)) != 0,
          "failed-sky fixture submits");
  require(!failed.waitIdleForTesting(),
          "failed sky reports a failed batch");
  failed.drainPublished();
  require(failed.latestLayerChunk(LightLayer::Block, 21, 0) != nullptr &&
              failed.latestLayerChunk(LightLayer::Sky, 21, 0) == nullptr &&
              failed.workerError().find("injected sky failure") !=
                  std::string::npos,
          "failed sky leaves its independent result absent");
}

void independentMutableLayerPublication() {
  LightWorkerOptions options;
  options.batchWindowMs = 0;
  LightService service(0, 1, options);
  std::vector<std::shared_ptr<const LightChunkSnapshot>> publications;
  service.setPublishCallback(
      [&](std::shared_ptr<const LightChunkSnapshot> publication) {
        publications.push_back(std::move(publication));
      });
  require(service.submitChunk(rawEmptyChunkFixture(0, 0, 0, 1)) != 0,
          "mutable-layer fixture loads");
  require(service.waitIdleForTesting(), "mutable-layer initial batch settles");
  service.drainPublished();
  publications.clear();
  auto initialBlock =
      service.latestLayerChunk(LightLayer::Block, 0, 0);
  auto initialSky = service.latestLayerChunk(LightLayer::Sky, 0, 0);
  require(initialBlock && initialSky &&
              initialBlock->layer == LightLayer::Block &&
              initialSky->layer == LightLayer::Sky &&
              initialBlock.get() != initialSky.get(),
          "main stores separate block and sky headers");

  const auto lamp =
      Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  reportOwnedCell(service, 0, 0, {8, 8, 8}, lamp);
  require(service.waitIdleForTesting(), "first mutable page write settles");
  service.drainPublished();
  auto block1 = service.latestLayerChunk(LightLayer::Block, 0, 0);
  auto sky1 = service.latestLayerChunk(LightLayer::Sky, 0, 0);
  const auto &blockRef1 =
      block1->section(0)->layers[static_cast<std::size_t>(LightLayer::Block)];
  const auto &skyRef1 =
      sky1->section(0)->layers[static_cast<std::size_t>(LightLayer::Sky)];
  const auto blockPage1 = blockRef1.shared();
  const auto skyPage1 = skyRef1.shared();
  require(blockPage1 && blockRef1.isMixed() &&
              skyPage1 && skyRef1.isMixed(),
          "mixed block and sky layers own independent atomic pages");
  const auto blockRevision1 = block1->revision;
  const auto skyRevision1 = sky1->revision;
  const auto blockChangedCount1 = block1->changedSections.size();

  reportOwnedCell(service, 0, 0, {6, 8, 8}, lamp);
  require(service.waitIdleForTesting(), "same-page mutable write settles");
  service.drainPublished();
  auto block2 = service.latestLayerChunk(LightLayer::Block, 0, 0);
  auto sky2 = service.latestLayerChunk(LightLayer::Sky, 0, 0);
  require(block2.get() != block1.get() &&
              block1->revision == blockRevision1 &&
              block1->changedSections.size() == blockChangedCount1,
          "published header stays immutable when next batch clones for publish");
  const auto &blockRef2 =
      block2->section(0)->layers[static_cast<std::size_t>(LightLayer::Block)];
  const auto &skyRef2 =
      sky2->section(0)->layers[static_cast<std::size_t>(LightLayer::Sky)];
  require(blockRef2.shared() == blockPage1 &&
              skyRef2.shared() == skyPage1,
          "mixed values stay in the same 2048-byte page");
  require(block2->revision > blockRevision1 &&
              sky2->revision > skyRevision1,
          "each layer revision advances independently");
  require(std::any_of(block2->changedSections.begin(),
                      block2->changedSections.end(),
                      [](const auto &change) {
                        return change.sectionY == 0 && change.block;
                      }) &&
              std::any_of(sky2->changedSections.begin(),
                          sky2->changedSections.end(),
                          [](const auto &change) {
                            return change.sectionY == 0 && change.sky;
                          }),
          "dirty section metadata survives a shared mutable page rewrite");
  require(block1->section(0)
                  ->layers[static_cast<std::size_t>(LightLayer::Block)]
                  .get(6, 8, 8) ==
              block2->section(0)
                  ->layers[static_cast<std::size_t>(LightLayer::Block)]
                  .get(6, 8, 8),
          "an old header may observe the new value through the shared page");

  const LightSectionSnapshot &blockSection =
      *block2->section(0);
  const LightSectionSnapshot &skySection =
      *sky2->section(0);
  require(blockSection.layers[static_cast<std::size_t>(LightLayer::Sky)]
              .isMissing() &&
              skySection.layers[static_cast<std::size_t>(LightLayer::Block)]
                  .isMissing(),
          "published layer views contain no combined section payload");
  require(!publications.empty(),
          "independent publication reaches the main-thread drain");
  for (const auto &publication : publications) {
    require(publication->layer.has_value(),
            "production publication is tagged with exactly one layer");
    const auto latest = service.latestLayerChunk(
        *publication->layer, 0, 0);
    require(latest &&
                latest->section(0)
                        ->layers[static_cast<std::size_t>(
                            *publication->layer)]
                        .shared() ==
                    publication->section(0)
                        ->layers[static_cast<std::size_t>(
                            *publication->layer)]
                        .shared(),
            "main header retains the worker's shared layer object");
  }

  const auto oldEpoch = block2->lifecycleEpoch;
  require(service.unloadChunk(0, 0) &&
              service.waitIdleForTesting(),
          "mutable-layer fixture unloads");
  service.drainPublished();
  require(service.latestLayerChunk(LightLayer::Block, 0, 0) == nullptr &&
              service.latestLayerChunk(LightLayer::Sky, 0, 0) == nullptr,
          "unload clears both main-thread layer latest pointers");
  require(service.submitChunk(rawEmptyChunkFixture(0, 0, 0, 1)) != 0 &&
              service.waitIdleForTesting(),
          "mutable-layer fixture reloads");
  service.drainPublished();
  reportOwnedCell(service, 0, 0, {8, 8, 8}, lamp);
  require(service.waitIdleForTesting(), "new-epoch page write settles");
  service.drainPublished();
  const auto reloadedBlock =
      service.latestLayerChunk(LightLayer::Block, 0, 0);
  const auto reloadedSky =
      service.latestLayerChunk(LightLayer::Sky, 0, 0);
  require(reloadedBlock && reloadedSky &&
              reloadedBlock->lifecycleEpoch > oldEpoch &&
              reloadedSky->lifecycleEpoch == reloadedBlock->lifecycleEpoch &&
              reloadedBlock->section(0)
                      ->layers[static_cast<std::size_t>(LightLayer::Block)]
                      .shared() != blockPage1 &&
              reloadedSky->section(0)
                      ->layers[static_cast<std::size_t>(LightLayer::Sky)]
                      .shared() != skyPage1,
          "a reload allocates fresh layer pages instead of reviving an old epoch");
  require(blockRef1.isMixed() && skyRef1.isMixed(),
          "old-epoch readers retain their shared pages after reload");
}

void worldBrightnessComposesSeparateLayers() {
  World world;
  world.loadChunk(0, 0);
  LightWorkerOptions options;
  world.enableLighting(options);
  auto *service = world.lightService();
  require(service &&
              world.setBlock({0, 8, 0},
                             Blocks::REDSTONE_LAMP.defaultState().with(
                                 "lit", "true")),
          "brightness fixture enables lighting and a source");
  require(service->waitIdleForTesting(), "brightness layers settle");
  service->drainPublished();
  require(world.getBrightness({0, 8, 0}) == 15,
          "block emission dominates query-time brightness");
  require(world.getBrightness({1, 8, 0}, 10) == 14,
          "block light survives sky darkening");
  require(world.getBrightness({8, 20, 8}, 10) == 5,
          "sky brightness is darkened only during composition");
  const auto block =
      service->latestLayerChunk(LightLayer::Block, 0, 0);
  const auto sky =
      service->latestLayerChunk(LightLayer::Sky, 0, 0);
  require(block && sky && block->layer == LightLayer::Block &&
              sky->layer == LightLayer::Sky,
          "brightness composition reads the two saved layer views");
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
  auto staleWest = stale.latestLayerChunk(LightLayer::Block, -1, 0);
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
  require(blockLightAt(refresh.latestLayerChunk(LightLayer::Block, -1, 0),
                       -1, 8, 0) == 14,
          "initial source crosses the loaded boundary");

  require(refresh.submitChunk(emptyChunkFixture(0, 0, 0, 1)) != 0,
          "same-epoch authoritative snapshot removes the source");
  auto refreshedEast = settledChunk(refresh, 0, 0);
  refresh.drainPublished();
  auto refreshedWest = refresh.latestLayerChunk(LightLayer::Block, -1, 0);
  require(refreshedEast->blockLightComplete && refreshedWest,
          "authoritative removal still publishes a complete block layer");
  require(blockLightAt(refreshedEast, 0, 8, 0) == 0 &&
              blockLightAt(refreshedWest, -1, 8, 0) == 0,
          "authoritative input removes local and cross-boundary old light");

  auto withSource = emptyChunkFixture(0, 0, 0, 1);
  withSource.geometrySections[0] =
      makeIndexedGeometrySection(air, {{{0, 8, 0}, lamp}}, false);
  require(refresh.submitChunk(std::move(withSource)) != 0,
          "same-epoch authoritative snapshot adds a source");
  auto addedEast = settledChunk(refresh, 0, 0);
  refresh.drainPublished();
  auto addedWest = refresh.latestLayerChunk(LightLayer::Block, -1, 0);
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
  auto eastFirst = service.latestLayerChunk(LightLayer::Block, 0, 0);
  require(eastFirst && blockLightAt(eastFirst, 7, 8, 0) == 15 &&
              blockLightAt(eastFirst, 0, 8, 0) == 8,
          "east publication exists before the older west request is processed");

  require(service.processChunkBatchForTesting(-1, 0),
          "older west request is processed in the following batch");
  service.drainPublished();
  auto eastAfterWest = service.latestLayerChunk(LightLayer::Block, 0, 0);
  auto west = service.latestLayerChunk(LightLayer::Block, -1, 0);
  require(eastAfterWest && west &&
              eastAfterWest->revision > eastFirst->revision,
          "worker publication revision advances despite lower input generation");
  require(blockLightAt(eastAfterWest, 0, 8, 0) == 14 &&
              blockLightAt(west, -1, 8, 0) == 15,
          "west batch's cross-boundary result is not rejected for published east");
}

void negativeChunkFirstInBatch() {
  const Pos pos{-8, 8, -8};
  const auto air = airState();
  const auto lamp = Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  LightService direct(0, 1);
  require(direct.submitChunk(emptyChunkFixture(-1, -1, 0, 1)) != 0,
          "negative chunk fixture loads");
  (void)settledChunk(direct, -1, -1);
  const LightCellUpdate change{pos.x, pos.y, pos.z, lamp.id()};
  direct.reportBlockCells({&change, 1});
  require(blockLightAt(settledChunk(direct, -1, -1), pos.x, pos.y, pos.z) == 15,
          "a bulk change to (-1,-1) survives as the first and only chunk");

  World world;
  world.loadChunk(-1, -1);
  world.enableLighting();
  auto &service = *world.lightService();
  const auto initial = settledChunk(service, -1, -1);
  service.pauseForTesting();
  require(world.setBlock(pos, lamp), "negative chunk reports a queued cell");
  require(service.processChunkBatchForTesting(-1, -1),
          "single-batch request sees cells still in the ring");
  const auto updated = settledChunk(service, -1, -1);
  require(updated->revision > initial->revision &&
              blockLightAt(updated, pos.x, pos.y, pos.z) == 15 &&
              blockLightAt(initial, pos.x, pos.y, pos.z) == 0,
          "the queued negative-chunk source publishes without editing the old snapshot");
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
  require(cross.latestLayerChunk(LightLayer::Block, 0, 0) != nullptr &&
              cross.latestLayerChunk(LightLayer::Block, 0, 0)->revision ==
                  visibleBefore->revision,
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
  const auto oldEpoch =
      cross.latestLayerChunk(LightLayer::Block, -1, 0)->lifecycleEpoch;
  require(cross.unloadChunk(-1, 0), "west chunk unloads");
  require(cross.latestLayerChunk(LightLayer::Block, -1, 0) == nullptr &&
              cross.latestLayerChunk(LightLayer::Sky, -1, 0) == nullptr,
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
  const BlockState unsupported("minecraft:future_light_probe",
                                 {{"future", "value"}});
  auto input = emptyChunkFixture(0, 0, 0, 1);
  input.geometrySections[0] = makeIndexedGeometrySection(
      airState(), {{{4, 5, 6}, unsupported}}, false);
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

void frozenWorldDrainsLightResults() {
  World world;
  world.loadChunk(0, 0);
  LightWorkerOptions options;
  options.startPausedForTesting = true;
  world.enableLighting(options);
  auto *service = world.lightService();
  require(service &&
              world.setBlock({0, 20, 0},
                             Blocks::REDSTONE_LAMP.defaultState().with(
                                 "lit", "true")),
          "frozen-world light fixture queues work");
  service->resumeForTesting();
  require(service->waitIdleForTesting(), "frozen-world light work completes");
  require(service->latestLayerChunk(LightLayer::Block, 0, 0) == nullptr,
          "worker completion does not require a world tick to publish");
  world.drainLightPublications();
  require(service->latestLayerChunk(LightLayer::Block, 0, 0) != nullptr,
          "main can drain light results while the world tick is frozen");
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
  auto initial = service->latestLayerChunk(LightLayer::Block, 0, 0);
  const auto initialOptical =
      service->opticalPropertiesForTesting(0, 100, 0);
  require(initial && initial->section(6) && initialOptical &&
              initialOptical->dampening == 15,
          "World load snapshot owns official optical metadata");
  auto emptyPeer = service->latestLayerChunk(LightLayer::Block, 1, 0);
  require(emptyPeer &&
              emptyPeer->section(-3)
                  ->layers[static_cast<std::size_t>(LightLayer::Block)]
                  .isMissing() &&
              initial->section(-3)
                  ->layers[static_cast<std::size_t>(LightLayer::Block)]
                  .isMissing() &&
              initial->section(-3)->light(LightLayer::Block, 0, 0, 0) ==
                  emptyPeer->section(-3)->light(LightLayer::Block, 0, 0, 0),
          "empty block sections decode the same compact uniform value");

  const auto initialGeneration = initial->generation;
  require(world.setBlock({0, 100, 0},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "world change survives normal callbacks");
  require(service->waitIdleForTesting(), "world change reaches light worker");
  service->drainPublished();
  auto changed = service->latestLayerChunk(LightLayer::Block, 0, 0);
  const auto changedOptical =
      service->opticalPropertiesForTesting(0, 100, 0);
  require(changed && changed->generation > initialGeneration,
          "World setBlock advances the light generation");
  require(changedOptical && changedOptical->emission == 15,
          "World setBlock forwards official emission metadata");
  require(blockLightAt(changed, 0, 100, 0) == 15 &&
              blockLightAt(changed, 1, 100, 0) == 14,
          "World setBlock runs real block-light propagation");

  require(world.setBlock({0, 150, 0}, Blocks::STONE.defaultState()),
          "place into an empty block section");
  require(service->waitIdleForTesting(), "section transition reaches worker");
  service->drainPublished();
  auto occupied = service->latestLayerChunk(LightLayer::Block, 0, 0);
  require(occupied && !occupied->section(9)->empty,
          "empty-to-non-empty section status is applied");
  require(world.setBlock({0, 150, 0}, airState()),
          "clear the fresh section");
  require(service->waitIdleForTesting(), "section clear reaches worker");
  service->drainPublished();
  auto cleared = service->latestLayerChunk(LightLayer::Block, 0, 0);
  require(cleared && cleared->section(9)->empty,
          "non-empty-to-empty section status is applied");

  world.unloadChunk(0, 0);
  require(service->latestLayerChunk(LightLayer::Block, 0, 0) == nullptr,
          "unload invalidates the published light generation immediately");
  world.loadChunk(0, 0);
  require(service->waitIdleForTesting(), "reload snapshot completes");
  service->drainPublished();
  auto reloaded = service->latestLayerChunk(LightLayer::Block, 0, 0);
  require(reloaded && reloaded->generation > cleared->generation,
          "reload publishes a newer owned generation");
  require(reloaded->lightSectionCount() == 26 && reloaded->minLightSectionY() == -5,
          "default world has 24 block sections plus two light padding sections");
  world.disableLighting();
  require(!world.lightingEnabled(), "light service closes without a live World");
}

void worldLifecycleQueueGrowthIsLossless() {
  World world;
  LightWorkerOptions options;
  options.batchWindowMs = 0;
  options.maxQueuedCommands = 2;
  options.startPausedForTesting = true;
  options.hasSkyLight = false;
  world.enableLighting(options);
  auto *service = world.lightService();
  require(service != nullptr, "growth-world lighting is enabled");
  for (int chunkX = 0; chunkX < 8; ++chunkX)
    world.loadChunk(chunkX, 0);
  for (int chunkX = 0; chunkX < 4; ++chunkX)
    world.unloadChunk(chunkX, 0);
  for (int chunkX = 0; chunkX < 4; ++chunkX)
    world.loadChunk(chunkX, 0);
  world.refreshLightSnapshots();
  require(service->stats().rejected == 0,
          "World lifecycle inputs grow the control queue without rejection");
  service->resumeForTesting();
  require(service->waitIdleForTesting(),
          "paused World lifecycle burst reaches the worker");
  service->drainPublished();
  for (int chunkX = 0; chunkX < 8; ++chunkX)
    require(service->latestLayerChunk(LightLayer::Block, chunkX, 0) != nullptr,
            "every World load survives control segment growth");
  world.disableLighting();
}

void pausedWorldRecoveryAndImport() {
  World world;
  world.loadChunk(0, 0);
  world.loadChunk(1, 0);
  world.enableLighting(LightWorkerOptions{1, 16, true, false});
  auto *service = world.lightService();
  require(service, "paused world lighting is enabled");

  require(world.setBlock({0, 100, 0},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "paused world accepts an emission change");
  require(world.setBlock({17, 100, 0}, Blocks::STONE.defaultState()),
          "paused world accepts a later change");
  world.unloadChunk(0, 0);
  require(service->stats().pendingChunks >= 2,
          "World inputs remain queued while the worker is paused");
  service->resumeForTesting();
  require(service->waitIdleForTesting(), "paused World worker drains");
  service->drainPublished();
  require(service->latestLayerChunk(LightLayer::Block, 0, 0) == nullptr,
          "World unload tombstone rejects an older load publication");
  auto live = service->latestLayerChunk(LightLayer::Block, 1, 0);
  const auto liveOptical =
      service->opticalPropertiesForTesting(17, 100, 0);
  require(live && liveOptical && liveOptical->dampening == 15,
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
  auto authoritative = service->latestLayerChunk(LightLayer::Block, 1, 0);
  const auto authoritativeOptical =
      service->opticalPropertiesForTesting(16, 101, 0);
  require(authoritative && authoritativeOptical &&
              authoritativeOptical->emission == 15,
          "import updates light from owned post-raw-write data");
  require(blockLightAt(authoritative, 16, 101, 0) == 15,
          "imported source participates in block-light propagation");
  world.disableLighting();
}

void persistedLightReuseNeighborPropagationAndStatus() {
  LightService service(0, 2);

  // Chunk (0,0) spans sections 0..1 (y=0..31):
  // - Roof at y=22 blocks direct sky inside y=0..21 for the hole-free columns.
  // - Under the roof at x=15, y=20, z=8 we seed persisted BlockLight=12 and
  //   SkyLight=14, while section 0 is omitted from PersistedChunkLight.sections
  //   (all zeros in NBT -> Uniform(0) instead of false inherit-15).
  LightChunkInput persisted = emptyChunkFixture(0, 0, 0, 2);
  std::vector<std::pair<Pos, BlockState>> sec1Blocks;
  for (int lx = 0; lx < 16; ++lx) {
    for (int lz = 0; lz < 16; ++lz)
      sec1Blocks.push_back({{lx, 6, lz}, Blocks::STONE.defaultState()});
  }
  persisted.geometrySections[1] =
      makeIndexedGeometrySection(airState(), sec1Blocks, false);

  PersistedChunkLight saved;
  saved.isLightOn = true;
  PersistedLightSection savedSec1;
  savedSec1.sectionY = 1;
  savedSec1.blockLight.emplace();
  savedSec1.blockLight->fill(0);
  {
    const std::size_t idx = lightIndex(15, 4, 8); // y=20 is local y=4 in sec 1
    const std::uint8_t prev = (*savedSec1.blockLight)[idx >> 1];
    (*savedSec1.blockLight)[idx >> 1] =
        (idx & 1u) ? static_cast<std::uint8_t>((prev & 0x0Fu) | (12u << 4))
                   : static_cast<std::uint8_t>((prev & 0xF0u) | 12u);
  }
  savedSec1.skyLight.emplace();
  savedSec1.skyLight->fill(0);
  {
    const std::size_t idx = lightIndex(15, 4, 8);
    const std::uint8_t prev = (*savedSec1.skyLight)[idx >> 1];
    (*savedSec1.skyLight)[idx >> 1] =
        (idx & 1u) ? static_cast<std::uint8_t>((prev & 0x0Fu) | (14u << 4))
                   : static_cast<std::uint8_t>((prev & 0xF0u) | 14u);
  }
  saved.sections.push_back(std::move(savedSec1));
  persisted.persistedLight = std::move(saved);

  const std::uint64_t gen0 = service.submitChunk(std::move(persisted));
  require(gen0 != 0, "persisted chunk submission succeeds");
  require(service.flushAndWait(std::chrono::seconds(2)),
          "persisted chunk flushes cleanly");
  service.drainPublished();

  const auto st0 = service.chunkStatus(0, 0);
  require(st0.loaded && !st0.pending && st0.complete &&
              st0.submittedGeneration == gen0 &&
              st0.publishedBlockGeneration == gen0 &&
              st0.publishedSkyGeneration == gen0 &&
              st0.unloadedNeighborMask != 0,
          "chunkStatus reports complete persisted chunk and unloaded neighbors");

  auto snap0Block = service.latestLayerChunk(LightLayer::Block, 0, 0);
  auto snap0Sky = service.latestLayerChunk(LightLayer::Sky, 0, 0);
  require(snap0Block && snap0Sky && snap0Block->protocolReady &&
              snap0Sky->protocolReady,
          "persisted chunk snapshots are immediately protocol-complete");
  require(blockLightAt(snap0Block, 15, 20, 8) == 12,
          "persisted BlockLight nibble survives persisted load");
  require(skyLightAt(snap0Sky, 15, 20, 8) == 14,
          "persisted SkyLight nibble survives persisted load");
  require(skyLightAt(snap0Sky, 8, 8, 8) == 0,
          "omitted empty SkyLight section under roof decodes as 0 not 15");

  // Unload checks and neighbor light completion:
  // Load dark neighbor chunk (1,0) covered by a roof at y=22 so any light at
  // (16,20,8) MUST arrive from chunk (0,0)'s outgoing persisted boundary.
  LightChunkInput neighbor = emptyChunkFixture(1, 0, 0, 2);
  std::vector<std::pair<Pos, BlockState>> neighborSec1;
  for (int lx = 0; lx < 16; ++lx) {
    for (int lz = 0; lz < 16; ++lz)
      neighborSec1.push_back({{lx, 6, lz}, Blocks::STONE.defaultState()});
  }
  neighbor.geometrySections[1] =
      makeIndexedGeometrySection(airState(), neighborSec1, false);
  require(service.submitChunk(std::move(neighbor)) != 0,
          "neighbor chunk submission succeeds");
  require(service.flushAndWait(std::chrono::seconds(2)),
          "neighbor propagation flushes cleanly");
  service.drainPublished();

  auto snap1Block = service.latestLayerChunk(LightLayer::Block, 1, 0);
  auto snap1Sky = service.latestLayerChunk(LightLayer::Sky, 1, 0);
  require(snap1Block && snap1Sky, "neighbor snapshots published");
  require(blockLightAt(snap1Block, 16, 20, 8) == 11,
          "persisted chunk (0,0) BlockLight=12 seeds neighbor (1,0) to 11");
  require(skyLightAt(snap1Sky, 16, 20, 8) == 13,
          "persisted chunk (0,0) SkyLight=14 seeds neighbor (1,0) to 13");
  require(service.stats().unloadedNeighborSkips > 0,
          "unloadedNeighborSkips is observable when boundary neighbors absent");

  // Verify skyColumnSources initialized cleanly for persisted chunk: punching a
  // hole in chunk (0,0)'s roof lets sky light stream down into y=10..21.
  LightCellUpdate openHole{4, 22, 4, airState().id()};
  service.reportBlockCell(openHole);
  require(service.flushAndWait(std::chrono::seconds(2)),
          "post-persisted edit flushes");
  service.drainPublished();
  snap0Sky = service.latestLayerChunk(LightLayer::Sky, 0, 0);
  require(skyLightAt(snap0Sky, 4, 10, 4) == 15,
          "breaking roof after persisted load streams sky light to y=10");

  // Removing chunk (1,0) twice increments rejected/cleanly stays unloaded.
  service.unloadChunk(1, 0);
  service.unloadChunk(1, 0);
  require(service.flushAndWait(std::chrono::seconds(2)), "unloads flush");
  service.drainPublished();
  require(!service.chunkStatus(1, 0).loaded &&
              service.chunkStatus(1, 0).unloadedNeighborMask == 0,
          "unloaded chunk status reflects absent state");
}

void persistedNeighborsSkipRedundantSeedingAndEditPropagates() {
  LightService service(0, 2);

  auto setNibble = [](std::array<std::uint8_t, 2048> &layer, int lx, int ly,
                      int lz, int value) {
    const std::size_t idx = lightIndex(lx, ly, lz);
    const std::uint8_t prev = layer[idx >> 1];
    layer[idx >> 1] =
        (idx & 1u)
            ? static_cast<std::uint8_t>((prev & 0x0Fu) |
                                        ((value & 0x0Fu) << 4))
            : static_cast<std::uint8_t>((prev & 0xF0u) | (value & 0x0Fu));
  };

  auto makeRoofedPersisted = [&](int cx, int cz, int lx, int lz, int blockVal,
                                 int skyVal) {
    LightChunkInput chunk = emptyChunkFixture(cx, cz, 0, 2);
    std::vector<std::pair<Pos, BlockState>> sec1Blocks;
    for (int x = 0; x < 16; ++x)
      for (int z = 0; z < 16; ++z)
        sec1Blocks.push_back({{x, 6, z}, Blocks::STONE.defaultState()});
    chunk.geometrySections[1] =
        makeIndexedGeometrySection(airState(), sec1Blocks, false);

    PersistedChunkLight saved;
    saved.isLightOn = true;
    PersistedLightSection sec1;
    sec1.sectionY = 1;
    sec1.blockLight.emplace();
    sec1.blockLight->fill(0);
    setNibble(*sec1.blockLight, lx, 4, lz, blockVal);
    sec1.skyLight.emplace();
    sec1.skyLight->fill(0);
    setNibble(*sec1.skyLight, lx, 4, lz, skyVal);
    saved.sections.push_back(std::move(sec1));
    chunk.persistedLight = std::move(saved);
    return chunk;
  };

  // Same-batch adjacent persisted chunks (0,0) and (1,0) must skip border
  // seeding against each other (lightQueueEntries stays 0).
  require(service.submitChunk(makeRoofedPersisted(0, 0, 15, 8, 12, 14)) != 0,
          "submit persisted (0,0)");
  require(service.submitChunk(makeRoofedPersisted(1, 0, 0, 8, 3, 5)) != 0,
          "submit persisted (1,0)");
  require(service.flushAndWait(std::chrono::seconds(2)),
          "flush same-batch persisted neighbors");
  service.drainPublished();

  require(service.stats().lightQueueEntries == 0,
          "same-batch persisted neighbors enqueue zero propagation entries");
  auto snap1Block = service.latestLayerChunk(LightLayer::Block, 1, 0);
  auto snap1Sky = service.latestLayerChunk(LightLayer::Sky, 1, 0);
  require(blockLightAt(snap1Block, 16, 20, 8) == 3 &&
              skyLightAt(snap1Sky, 16, 20, 8) == 5,
          "persisted (1,0) retains saved boundary values next to (0,0)");

  // Cross-batch persisted neighbor (2,0) next to already-loaded persisted (1,0)
  // must also skip redundant border seeding.
  require(service.submitChunk(makeRoofedPersisted(2, 0, 15, 8, 10, 11)) != 0,
          "submit cross-batch persisted (2,0)");
  require(service.flushAndWait(std::chrono::seconds(2)),
          "flush cross-batch persisted neighbor");
  service.drainPublished();
  require(service.stats().lightQueueEntries == 0,
          "cross-batch persisted neighbor enqueues zero propagation entries");

  // Loading non-persisted dark chunk (3,0) next to persisted (2,0) must still
  // propagate (2,0)'s persisted boundary into (3,0).
  LightChunkInput unpersisted = emptyChunkFixture(3, 0, 0, 2);
  std::vector<std::pair<Pos, BlockState>> sec1Blocks;
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      sec1Blocks.push_back({{x, 6, z}, Blocks::STONE.defaultState()});
  unpersisted.geometrySections[1] =
      makeIndexedGeometrySection(airState(), sec1Blocks, false);
  require(service.submitChunk(std::move(unpersisted)) != 0,
          "submit non-persisted (3,0)");
  require(service.flushAndWait(std::chrono::seconds(2)),
          "flush non-persisted (3,0)");
  service.drainPublished();
  auto snap3Block = service.latestLayerChunk(LightLayer::Block, 3, 0);
  auto snap3Sky = service.latestLayerChunk(LightLayer::Sky, 3, 0);
  require(blockLightAt(snap3Block, 48, 20, 8) == 9 &&
              skyLightAt(snap3Sky, 48, 20, 8) == 10,
          "persisted (2,0) propagates to non-persisted neighbor (3,0)");

  // Subsequent block edit on (1,0) still triggers sky eligibility + propagation.
  service.reportBlockCell({18, 22, 8, airState().id()});
  require(service.flushAndWait(std::chrono::seconds(2)),
          "flush post-persisted edit");
  service.drainPublished();
  snap1Sky = service.latestLayerChunk(LightLayer::Sky, 1, 0);
  require(skyLightAt(snap1Sky, 18, 10, 8) == 15,
          "roof break inside persisted chunk streams sky light after skip");
}

void lowMemoryDirectLightParityAndConcurrency() {
  const auto stone = Blocks::SMOOTH_STONE.defaultState();
  const auto slab =
      Blocks::SMOOTH_STONE_SLAB.defaultState().with("type", "top");
  const auto litLamp =
      Blocks::REDSTONE_LAMP.defaultState().with("lit", "true");
  const auto darkLamp =
      Blocks::REDSTONE_LAMP.defaultState().with("lit", "false");
  const std::vector<BlockState> noteStates =
      findBlockByName("minecraft:note_block")->states();
  require(noteStates.size() >= 25, "need note_block states for palette growth");

  auto buildFixture = [&](World &w) {
    for (int cx = -1; cx <= 1; ++cx)
      w.loadChunk(cx, 0);
    for (int x = -16; x < 32; ++x)
      for (int z = 0; z < 16; ++z)
        w.rawSetBlock({x, 24, z}, stone);
    w.rawSetBlock({8, 16, 8}, slab);
    w.rawSetBlock({15, 8, 8}, litLamp);
  };

  World normal(0, 64);
  World lowMem(0, 64);
  lowMem.setLowMemoryMode(true);
  buildFixture(normal);
  buildFixture(lowMem);

  LightWorkerOptions opts;
  opts.batchWindowMs = 0;
  normal.enableLighting(opts);
  lowMem.enableLighting(opts);

  auto settleBoth = [&] {
    require(normal.lightService()->waitIdleForTesting() &&
                lowMem.lightService()->waitIdleForTesting(),
            "normal and low-memory light settle");
    normal.drainLightPublications();
    lowMem.drainLightPublications();
  };
  settleBoth();

  for (int x = -2; x <= 20; ++x) {
    for (int y : {0, 8, 15, 16, 17, 23, 25}) {
      for (int darken : {0, 10}) {
        require(lowMem.getBrightness({x, y, 8}, darken) ==
                    normal.getBrightness({x, y, 8}, darken),
                "low-memory direct light matches buffered mode on initial load");
      }
    }
  }

  // Mutate optical sources + grow a section's packed palette concurrently while
  // the worker processes batches in direct-geometry mode.
  for (int i = 0; i < 20; ++i) {
    const Pos p{i & 15, 1 + (i >> 4), 2};
    normal.setBlock(p, noteStates[static_cast<std::size_t>(i)]);
    lowMem.setBlock(p, noteStates[static_cast<std::size_t>(i)]);
  }
  require(lowMem.sectionStorageBitsForTest(0, 0, 0) == 5,
          "low-memory section palette grew through 5 bits");
  normal.setBlock({15, 8, 8}, darkLamp);
  lowMem.setBlock({15, 8, 8}, darkLamp);
  normal.setBlock({8, 24, 8}, airState());
  lowMem.setBlock({8, 24, 8}, airState());
  settleBoth();

  for (int x = -2; x <= 20; ++x) {
    for (int y : {0, 8, 15, 16, 17, 23, 25}) {
      require(lowMem.getBrightness({x, y, 8}) ==
                  normal.getBrightness({x, y, 8}),
              "low-memory direct light matches after live edits and compaction");
    }
  }
}
} // namespace

// 三态模型：Deferred 提交只注册几何（未计算），requestChunkCompute 把它推进到完整。
void deferredComputeStates() {
  LightService service(-1, 1, LightWorkerOptions{1000, 16, false, true});
  LightChunkInput deferred = chunkFixture(0, 0, -1, 1);
  deferred.completion = LightChunkInput::Completion::Deferred;
  require(service.submitChunk(std::move(deferred)) != 0,
          "deferred submission is accepted");
  require(service.waitIdleForTesting(), "deferred registration settles");

  // 未计算：几何已注册，但没有光照壳，也不发布任何快照。
  auto status = service.chunkStatus(0, 0);
  require(status.loaded && status.needsCompute && !status.needsUpdate &&
              status.pending && !status.complete,
          "deferred chunk is registered but still needs compute");
  require(!service.isChunkSettled(0, 0),
          "deferred chunk is not settled before its first compute");
  service.drainPublished();
  require(service.latestLayerChunk(LightLayer::Block, 0, 0) == nullptr,
          "deferred registration publishes no light");

  // 未计算 → 完整：首次计算后发布完整光照。
  require(service.requestChunkCompute(0, 0) != 0,
          "compute request is accepted for a registered chunk");
  require(service.waitIdleForTesting(), "first compute completes");
  service.drainPublished();
  const auto lit = service.latestLayerChunk(LightLayer::Block, 0, 0);
  require(lit && lit->propagationComplete && lit->protocolReady,
          "first compute publishes a complete block-light snapshot");
  require(lit->section(-1)->light(LightLayer::Block, 1, 2, 3) == 15,
          "first compute seeds the emitting block");
  status = service.chunkStatus(0, 0);
  require(!status.needsCompute && !status.needsUpdate && status.complete,
          "first compute moves the chunk to complete");
  require(service.isChunkSettled(0, 0), "computed chunk is settled");

  // 未注册的区块不能请求计算。
  require(service.requestChunkCompute(7, 7) == 0,
          "compute request on an unknown chunk is rejected");
}

// 计算请求必须与同区块已排队的 load 合并，而不是把它覆盖掉。
void computeRequestMergesWithPendingLoad() {
  LightService service(-1, 1, LightWorkerOptions{1000, 16, true, false});
  LightChunkInput input = chunkFixture(0, 0, -1, 1);
  input.completion = LightChunkInput::Completion::Deferred;
  require(service.submitChunk(std::move(input)) != 0,
          "deferred load is queued while paused");
  require(service.requestChunkCompute(0, 0) != 0,
          "compute request is queued behind the load");
  service.resumeForTesting();
  require(service.waitIdleForTesting(), "merged load and compute settle");
  service.drainPublished();
  const auto lit = service.latestLayerChunk(LightLayer::Block, 0, 0);
  require(lit != nullptr,
          "compute request must not overwrite the pending load");
  require(lit->section(-1)->light(LightLayer::Block, 1, 2, 3) == 15,
          "merged request computes the registered geometry");
  require(service.isChunkSettled(0, 0), "merged chunk reaches complete");
}

int main(int argc, char **argv) {
  try {
    require(argc == 2, "light_tests needs assets/light26_1");
    dataLayerLayout();
    sectionRangesAndSnapshots();
    opticalMetadata();
    opticalAssetCoverage(argv[1]);
    workerPublicationAndInvalidation();
    publicationBatchVisibility();
    coalescedProtocolDeltaIsCumulative();
    workerQueueAndClose();
    recoverableChangesAndBoundedPublication();
    ownedCellQueueGrowthAndWakeup();
    ownedCellLifecycleRejectsReloadStaleWork();
    growableResultQueueHandsOffBurst();
    segmentedQueuesPreserveConcurrentLifecycle();
    rawSubmissionAndGeometryMirror();
    authoritativeRefreshOutranksQueuedCells();
    wideWorldInputsRemainOwned();
    blockLightPropagation();
    skyLightPropagation();
    skyLayerIndexAndMissingInheritance();
    incrementalSkyColumnSources();
    splitLightWorkerParityAndIsolation();
    independentMutableLayerPublication();
    worldBrightnessComposesSeparateLayers();
    blockLightStaleAndAuthoritativeRefresh();
    blockLightVerticalPadding();
    crossBatchPublicationOrdering();
    negativeChunkFirstInBatch();
    blockLightBoundariesAndEpochs();
    directionalShapeOcclusion();
    incompleteMetadataIsExplicit();
    destructorClosesWorker();
    frozenWorldDrainsLightResults();
    worldHooks();
    worldLifecycleQueueGrowthIsLossless();
    pausedWorldRecoveryAndImport();
    persistedLightReuseNeighborPropagationAndStatus();
    persistedNeighborsSkipRedundantSeedingAndEditPropagates();
    deferredComputeStates();
    computeRequestMergesWithPendingLoad();
    lowMemoryDirectLightParityAndConcurrency();
    std::cout << "light tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "light test failure: " << error.what() << '\n';
    return 1;
  }
}
