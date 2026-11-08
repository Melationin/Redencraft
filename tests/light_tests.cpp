#include "redstone/engine.hpp"
#include "redstone/light.hpp"

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
                              std::vector<std::uint16_t>(kLightVolume, 0));
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

  LightService service(-4, 3);
  const auto generation = service.submitChunk(std::move(input));
  require(generation != 0, "chunk snapshot is accepted");
  require(service.waitIdleForTesting(), "light worker finishes snapshot");
  service.drainPublished();
  auto snapshot = service.latestChunk(-2, 3);
  require(snapshot && snapshot->generation == generation,
          "published chunk keeps its generation");
  require(!snapshot->propagationComplete && !snapshot->protocolReady,
          "L-1 snapshots never claim propagation or protocol readiness");
  require(snapshot->lightSectionCount() == 5,
          "published snapshot keeps all light padding sections");
  require(snapshot->section(-4)->light(LightLayer::Block, 1, 2, 3) == 12,
          "published immutable snapshot preserves block light");
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
  LightService service(-4, 3);
  std::thread::id callbackThread;
  int callbacks = 0;
  service.setPublishCallback([&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
    callbackThread = std::this_thread::get_id();
    ++callbacks;
    require(snapshot && !snapshot->propagationComplete,
            "callback receives a complete but not propagated snapshot");
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
  require(staleService.stats().stalePublications >= 1,
          "old generation publication is counted");
}

void workerQueueAndClose() {
  LightService service(-1, 1, LightWorkerOptions{1, 1, 16, true});
  const auto accepted = service.submitChunk(chunkFixture(0, 0, -1, 1));
  const auto rejected = service.submitChunk(chunkFixture(1, 0, -1, 1));
  require(accepted != 0 && rejected == 0,
          "bounded queue rejects explicitly instead of silently dropping");
  require(service.stats().rejected == 1, "rejected light command is counted");
  service.resumeForTesting();
  require(service.waitIdleForTesting(), "paused worker drains queued work");
  require(service.stats().queueHighWater >= 1, "queue high-water is observed");
  service.close();
  require(service.submitChunk(chunkFixture(2, 0, -1, 1)) == 0,
          "closed service refuses new work");

  LightService draining(-1, 1, LightWorkerOptions{1, 4, 16, true});
  const auto generation = draining.submitChunk(chunkFixture(4, 0, -1, 1));
  require(generation != 0, "close fixture queues a snapshot");
  draining.close();
  draining.drainPublished();
  auto delivered = draining.latestChunk(4, 0);
  require(delivered && delivered->generation == generation,
          "close drains queued work before joining the worker");

  LightService batching(-1, 1, LightWorkerOptions{1000, 16, 16, true});
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

void destructorClosesWorker() {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting();
  // Do not drain or disable explicitly: ~World must stop and join the owned
  // single-writer thread before its chunk storage is destroyed.
}

void worldHooks() {
  World world;
  require(!world.lightingEnabled() && world.lightService() == nullptr,
          "production worlds keep the light worker disabled by default");
  world.loadChunk(0, 0);
  require(world.setBlock({0, 100, 0}, Blocks::SMOOTH_STONE.defaultState()),
          "place pre-enable fixture block");
  world.enableLighting();
  require(world.lightingEnabled() && world.lightService(),
          "lighting can be enabled opt-in on a world");
  auto *service = world.lightService();
  require(service->waitIdleForTesting(), "initial world snapshot completes");
  service->drainPublished();
  auto initial = service->latestChunk(0, 0);
  require(initial && initial->section(6) &&
              initial->section(6)->at(0, 4, 0).dampening == 15,
          "World load snapshot owns official section metadata");

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
    destructorClosesWorker();
    worldHooks();
    std::cout << "light tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "light test failure: " << error.what() << '\n';
    return 1;
  }
}
