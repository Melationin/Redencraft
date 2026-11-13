#include "redstone/net/session.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace redstone;
using namespace redstone::net;

namespace {
std::vector<Bytes> records;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

void hello(Session &session) {
  Writer out;
  out.varInt(775);
  out.string("localhost");
  out.i16(25565);
  out.varInt(2);
  session.receive(frame(0, out.data));
}

std::vector<Bytes> drain(Session &session) {
  std::vector<Bytes> packets;
  for (auto &framed : session.takeOutput()) {
    auto body = popFrame(framed);
    require(body && framed.empty(), "clientbound frame has one packet");
    packets.push_back(std::move(*body));
  }
  return packets;
}

std::vector<Bytes> join(Session &session, const ProtocolData &data) {
  hello(session);
  Writer out;
  out.string("LightTester");
  out.raw(Uuid{});
  session.receive(frame(0, out.data));
  (void)drain(session);
  session.receive(frame(3, {}));
  (void)drain(session);
  out = {};
  out.varInt(0);
  session.receive(frame(7, out.data));
  (void)drain(session);
  session.receive(frame(3, {}));
  require(session.phase() == Phase::Play, "session reaches play");
  std::vector<Bytes> joined;
  for (auto &body : drain(session)) {
    records.push_back(std::move(body));
    joined.push_back(records.back());
  }

  out = {};
  out.varInt(1);
  session.receive(frame(
      data.packet("play", "serverbound", "accept_teleportation"), out.data));
  out = {};
  out.f32(9);
  session.receive(frame(
      data.packet("play", "serverbound", "chunk_batch_received"), out.data));
  (void)drain(session);
  return joined;
}

std::vector<Bytes> drainPlay(Session &session) {
  auto packets = drain(session);
  for (const auto &body : packets)
    records.push_back(body);
  return packets;
}

std::vector<std::uint64_t> readBitSet(Reader &reader) {
  const int words = reader.varInt();
  require(words >= 0 && words <= 8, "light mask word count is bounded");
  std::vector<std::uint64_t> bits(static_cast<std::size_t>(words));
  for (auto &word : bits)
    word = std::bit_cast<std::uint64_t>(reader.i64());
  return bits;
}

void setBit(std::vector<std::uint64_t> &bits, int index) {
  const auto word = static_cast<std::size_t>(index) / 64;
  if (bits.size() <= word)
    bits.resize(word + 1);
  bits[word] |= std::uint64_t{1} << (index & 63);
}

int bitCount(const std::vector<std::uint64_t> &bits) {
  int count = 0;
  for (auto word : bits)
    count += std::popcount(word);
  return count;
}

void expectedLayer(const LightChunkSnapshot &current,
                   const LightChunkSnapshot *previous, LightLayer layer,
                   std::vector<std::uint64_t> &data,
                   std::vector<std::uint64_t> &empty,
                   std::vector<int> &dataSections) {
  std::unordered_set<int> dirty;
  if (previous)
    for (const auto &change : current.changedSections)
      if (layer == LightLayer::Sky ? change.sky : change.block)
        dirty.insert(change.sectionY);
  for (int index = 0; index < current.lightSectionCount(); ++index) {
    const int sectionY = current.minLightSectionY() + index;
    if (previous && !dirty.contains(sectionY))
      continue;
    const auto &now = current.section(sectionY)
                          ->layers[static_cast<std::size_t>(layer)];
    if (now.isMissing()) {
      // The official packet leaves both masks clear for a null DataLayer.
      continue;
    } else if (now.isUniform() && now.uniformValue() == 0) {
      setBit(empty, index);
    } else {
      setBit(data, index);
      dataSections.push_back(sectionY);
    }
  }
}

void verifyLayerPayload(Reader &reader, const LightChunkSnapshot &current,
                        LightLayer layer,
                        const std::vector<int> &dataSections) {
  require(reader.varInt() == static_cast<int>(dataSections.size()),
          "layer update count matches its data mask");
  for (int sectionY : dataSections) {
    const auto &value = current.section(sectionY)
                            ->layers[static_cast<std::size_t>(layer)];
    require(reader.varInt() == int(kLightLayerBytes),
            "each DataLayer is exactly 2048 bytes");
    const auto payload = reader.raw(kLightLayerBytes);
    for (std::size_t byte = 0; byte < payload.size(); ++byte) {
      std::uint8_t expected = 0;
      if (value.isMixed()) {
        expected = value.packedByte(byte);
      } else {
        require(value.isUniform() && value.uniformValue() != 0,
                "data mask references nonempty uniform data");
        expected = static_cast<std::uint8_t>((value.uniformValue() << 4) |
                                             value.uniformValue());
      }
      require(payload[byte] == expected,
              "wire nibble bytes match the immutable layer");
    }
  }
}

void verifyLightData(Reader &reader, const LightChunkSnapshot &current,
                     const LightChunkSnapshot *previous,
                     std::string_view phase) {
  const auto skyData = readBitSet(reader);
  const auto blockData = readBitSet(reader);
  const auto skyEmpty = readBitSet(reader);
  const auto blockEmpty = readBitSet(reader);
  std::vector<std::uint64_t> expectedSkyData, expectedBlockData,
      expectedSkyEmpty, expectedBlockEmpty;
  std::vector<int> skySections, blockSections;
  expectedLayer(current, previous, LightLayer::Sky, expectedSkyData,
                expectedSkyEmpty, skySections);
  expectedLayer(current, previous, LightLayer::Block, expectedBlockData,
                expectedBlockEmpty, blockSections);
  require(skyData == expectedSkyData && blockData == expectedBlockData &&
              skyEmpty == expectedSkyEmpty && blockEmpty == expectedBlockEmpty,
          std::string("four light masks distinguish storage in ") +
              std::string(phase));
  require(bitCount(skyData) + bitCount(blockData) <=
              current.lightSectionCount() * 2,
          "light masks cannot repeat a section");
  require((skyData.size() == 0 || (skyData.back() >> 63) == 0),
          "light masks use canonical trimmed long arrays");
  verifyLayerPayload(reader, current, LightLayer::Sky, skySections);
  verifyLayerPayload(reader, current, LightLayer::Block, blockSections);
  require(reader.empty(), "light packet has no trailing bytes");
}

std::shared_ptr<const LightChunkSnapshot>
combinedForTesting(LightService &service, int x, int z) {
  const auto block = service.latestLayerChunk(LightLayer::Block, x, z);
  const auto sky = service.latestLayerChunk(LightLayer::Sky, x, z);
  if (!block || !sky)
    return block ? block : sky;
  auto merged = std::make_shared<LightChunkSnapshot>(*block);
  const std::size_t skyIndex = static_cast<std::size_t>(LightLayer::Sky);
  for (std::size_t i = 0; i < merged->sections.size(); ++i) {
    if (i >= sky->sections.size() || !sky->sections[i])
      continue;
    auto section = std::make_shared<LightSectionSnapshot>(*merged->sections[i]);
    section->layers[skyIndex] = sky->sections[i]->layers[skyIndex];
    merged->sections[i] = std::move(section);
  }
  return merged;
}

void verifyChunkLight(Reader &reader, const LightChunkSnapshot &light) {
  const int heightCount = reader.varInt();
  require(heightCount == 3, "chunk packet carries three heightmaps");
  for (int i = 0; i < heightCount; ++i) {
    (void)reader.varInt();
    const int words = reader.varInt();
    require(words >= 0 && words < 100000, "heightmap size is bounded");
    (void)reader.raw(static_cast<std::size_t>(words) * sizeof(std::int64_t));
  }
  (void)reader.raw(static_cast<std::size_t>(reader.varInt()));
  require(reader.varInt() == 0,
          "light fixture contains no variable-size block entities");
  verifyLightData(reader, light, nullptr, "initial chunk");
}

Bytes packetOf(std::vector<Bytes> &packets, int id) {
  for (auto it = packets.begin(); it != packets.end(); ++it) {
    Reader reader(*it);
    if (reader.varInt() == id) {
      Bytes result = std::move(*it);
      packets.erase(it);
      return result;
    }
  }
  return {};
}

void moveTo(Session &session, const ProtocolData &data, double x, double z) {
  Writer out;
  out.f64(x);
  out.f64(70);
  out.f64(z);
  out.u8(0);
  session.receive(
      frame(data.packet("play", "serverbound", "move_player_pos"), out.data));
  session.tick();
}

void strictInitialDynamicClearAndReload(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(),
          "initial light batch completes without waiting on a socket");
  service->drainPublished();
  auto initial = combinedForTesting(*service, 0, 0);
  require(initial && initial->protocolReady, "initial chunk light is ready");

  ServerOptions options;
  options.chunks.push_back({0, 0});
  options.spawn = {8.5, 70, 8.5};
  options.viewDistance = 2;
  Session session(world, data, options);
  world.lightPublicationObserver =
      [&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        session.lightPublished(std::move(snapshot));
      };
  auto packets = join(session, data);

  auto chunkBody = packetOf(packets, data.packet("play", "clientbound",
                                                  "level_chunk_with_light"));
  require(!chunkBody.empty(), "initial chunk-with-light packet is sent");
  Reader chunk(chunkBody);
  require(chunk.varInt() == data.packet("play", "clientbound",
                                        "level_chunk_with_light"),
          "chunk packet id is the exported 775 id");
  require(chunk.i32() == 0 && chunk.i32() == 0,
          "chunk packet carries signed chunk coordinates");
  verifyChunkLight(chunk, *initial);

  require(world.setBlock({1, 8, 8},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "light source enters the world normally");
  require(service->waitIdleForTesting(), "dynamic light batch completes");
  world.tick();
  auto changed = combinedForTesting(*service, 0, 0);
  require(changed && changed->revision > initial->revision,
          "world tick publishes the dynamic revision");
  packets = drainPlay(session);
  auto updateBody = packetOf(
      packets, data.packet("play", "clientbound", "light_update"));
  require(!updateBody.empty(), "dynamic light_update is sent at chunk broadcast");
  Reader update(updateBody);
  require(update.varInt() == data.packet("play", "clientbound", "light_update"),
          "light_update packet id is the exported 775 id");
  require(update.varInt() == 0 && update.varInt() == 0,
          "light_update uses VarInt chunk coordinates");
  verifyLightData(update, *changed, initial.get(), "dynamic update");

  require(world.setBlock({1, 8, 8}, Blocks::AIR.defaultState()),
          "light source removal enters the world normally");
  require(service->waitIdleForTesting(), "clear light batch completes");
  world.tick();
  auto cleared = combinedForTesting(*service, 0, 0);
  packets = drainPlay(session);
  auto clearBody = packetOf(
      packets, data.packet("play", "clientbound", "light_update"));
  require(!clearBody.empty(), "successful source removal sends light_update");
  Reader clear(clearBody);
  require(clear.varInt() == data.packet("play", "clientbound", "light_update") &&
              clear.varInt() == 0 && clear.varInt() == 0,
          "clear packet header is exact");
  verifyLightData(clear, *cleared, changed.get(), "layer clear");

  // Queue a real immutable update, then move the player out of the chunk
  // before the writer drains it. The generation token must cancel the stale
  // light packet while the forget packet remains deliverable.
  auto stale = std::make_shared<LightChunkSnapshot>(*cleared);
  stale->revision = cleared->revision + 100;
  stale->deltaBaseRevision = cleared->revision;
  auto changedSection = std::make_shared<LightSectionSnapshot>(
      *stale->sections[1]);
  changedSection->layers[static_cast<std::size_t>(LightLayer::Block)] =
      LightLayerSnapshot::uniform(7);
  stale->sections[1] = std::move(changedSection);
  stale->changedSections = {{0, false, true}};
  session.lightPublished(stale);
  moveTo(session, data, 500.5, 8.5);
  packets = drainPlay(session);
  require(packetOf(packets, data.packet("play", "clientbound",
                                        "light_update")).empty(),
          "unloaded queued light revision is cancelled before encoding");
  require(!packetOf(packets, data.packet("play", "clientbound",
                                         "forget_level_chunk")).empty(),
          "unload still sends its normal forget packet");

  moveTo(session, data, 8.5, 8.5);
  packets = drainPlay(session);
  auto reloadBody = packetOf(packets, data.packet("play", "clientbound",
                                                   "level_chunk_with_light"));
  require(!reloadBody.empty(), "reload queues a fresh chunk-with-light packet");
  Reader reload(reloadBody);
  require(reload.varInt() == data.packet("play", "clientbound",
                                         "level_chunk_with_light") &&
              reload.i32() == 0 && reload.i32() == 0,
          "reload packet keeps the chunk identity");
  verifyChunkLight(reload, *combinedForTesting(*service, 0, 0));

  Writer ack;
  ack.f32(9);
  session.receive(frame(
      data.packet("play", "serverbound", "chunk_batch_received"), ack.data));
  moveTo(session, data, 500.5, 8.5);
  (void)drainPlay(session);
  moveTo(session, data, 8.5, 8.5);
  moveTo(session, data, 500.5, 8.5);
  packets = drainPlay(session);
  int started = 0, chunks = 0, finished = 0, finishedCount = -1, forgotten = 0;
  for (int index = 0; index < static_cast<int>(packets.size()); ++index) {
    Reader packet(packets[index]);
    const int id = packet.varInt();
    if (id == data.packet("play", "clientbound", "chunk_batch_start")) {
      started = index + 1;
    } else if (id == data.packet("play", "clientbound",
                                 "level_chunk_with_light")) {
      ++chunks;
    } else if (id == data.packet("play", "clientbound",
                                 "chunk_batch_finished")) {
      finished = index + 1;
      finishedCount = packet.varInt();
    } else if (id == data.packet("play", "clientbound",
                                 "forget_level_chunk")) {
      forgotten = index + 1;
    }
  }
  require(started && chunks == 1 && finished && finishedCount == chunks &&
              started < finished && finished < forgotten,
          "first-packet backlog keeps start/chunk/finished counts before forget");
}

void readyIncompleteReadyDelta(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(), "delta-gap initial batch");
  service->drainPublished();
  auto initial = combinedForTesting(*service, 0, 0);
  require(initial && initial->protocolReady, "delta-gap initial snapshot ready");
  ServerOptions options;
  options.chunks.push_back({0, 0});
  options.spawn = {8.5, 70, 8.5};
  options.viewDistance = 2;
  Session session(world, data, options);
  world.lightPublicationObserver =
      [&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        session.lightPublished(std::move(snapshot));
      };
  (void)join(session, data);

  // S changes while an unknown optical state makes the same worker revision
  // incomplete. Session correctly ignores that drained revision.
  service->pauseForTesting();
  require(world.setBlock({2, 8, 2},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")) &&
              world.setBlock({3, 8, 3},
                             BlockState("minecraft:unmapped_delta_gap")),
          "supported and unknown delta-gap blocks enter");
  require(service->processChunkBatchForTesting(0, 0),
          "incomplete delta-gap batch completes");
  world.tick();
  auto incomplete = combinedForTesting(*service, 0, 0);
  require(incomplete && !incomplete->protocolReady,
          "incomplete middle revision is drained but not ready");
  auto incompletePackets = drainPlay(session);
  const auto incompleteUpdate = packetOf(
      incompletePackets, data.packet("play", "clientbound", "light_update"));
  if (!incompleteUpdate.empty()) {
    Reader onlyReadyLayer(incompleteUpdate);
    require(onlyReadyLayer.varInt() ==
                    data.packet("play", "clientbound", "light_update") &&
                onlyReadyLayer.varInt() == 0 &&
                onlyReadyLayer.varInt() == 0,
            "incomplete layer packet has the light-update header");
    (void)readBitSet(onlyReadyLayer);
    require(readBitSet(onlyReadyLayer).empty(),
            "incomplete block layer sends no data mask");
    (void)readBitSet(onlyReadyLayer);
    require(readBitSet(onlyReadyLayer).empty(),
            "incomplete block layer sends no empty mask");
  }

  // T changes only in the next ready revision. Its ready-to-ready delta must
  // still include the earlier S change that the client never received.
  require(world.setBlock({3, 8, 3}, Blocks::AIR.defaultState()) &&
              world.setBlock({2, 24, 2},
                             Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "unknown removal and later visible change enter");
  require(service->processChunkBatchForTesting(0, 0),
          "ready delta-gap batch completes");
  world.tick();
  auto current = combinedForTesting(*service, 0, 0);
  require(current && current->protocolReady &&
              current->deltaBaseRevision == initial->revision,
          "ready delta spans the drained incomplete revision");
  auto packets = drainPlay(session);
  auto body = packetOf(packets, data.packet("play", "clientbound", "light_update"));
  require(!body.empty(), "ready delta-gap update is sent");
  Reader update(body);
  require(update.varInt() == data.packet("play", "clientbound", "light_update") &&
              update.varInt() == 0 && update.varInt() == 0,
          "delta-gap update has the exact 775 header");
  verifyLightData(update, *current, initial.get(), "ready/incomplete/ready");
  service->resumeForTesting();
}

void evictedReadyBaseFallsBack(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(), "evicted-base initial batch");
  service->drainPublished();
  auto initial = combinedForTesting(*service, 0, 0);
  require(initial && initial->protocolReady, "evicted-base initial snapshot");
  ServerOptions options;
  options.chunks.push_back({0, 0});
  options.spawn = {8.5, 70, 8.5};
  options.viewDistance = 2;
  Session session(world, data, options);
  world.lightPublicationObserver =
      [&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        session.lightPublished(std::move(snapshot));
      };
  (void)join(session, data);

  service->pauseForTesting();
  require(world.setBlock({2, 8, 2},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")) &&
              service->processChunkBatchForTesting(0, 0),
          "ready B with S completes without drain");
  require(world.setBlock({3, 8, 3},
                         BlockState("minecraft:unmapped_evicted_base")) &&
              service->processChunkBatchForTesting(0, 0),
          "incomplete C replaces undrained B");
  world.tick();
  auto incompletePackets = drainPlay(session);
  const auto incompleteUpdate = packetOf(
      incompletePackets, data.packet("play", "clientbound", "light_update"));
  if (!incompleteUpdate.empty()) {
    Reader onlyReadyLayer(incompleteUpdate);
    require(onlyReadyLayer.varInt() ==
                    data.packet("play", "clientbound", "light_update") &&
                onlyReadyLayer.varInt() == 0 &&
                onlyReadyLayer.varInt() == 0,
            "incomplete C packet has the light-update header");
    (void)readBitSet(onlyReadyLayer);
    require(readBitSet(onlyReadyLayer).empty(),
            "incomplete block C sends no data mask");
    (void)readBitSet(onlyReadyLayer);
    require(readBitSet(onlyReadyLayer).empty(),
            "incomplete block C sends no empty mask");
  }
  require(!session.closed(), "incomplete C does not close the session");

  require(world.setBlock({3, 8, 3}, Blocks::AIR.defaultState()) &&
              world.setBlock({2, 24, 2},
                             Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")) &&
              service->processChunkBatchForTesting(0, 0),
          "ready D with T completes");
  world.tick();
  auto current = combinedForTesting(*service, 0, 0);
  require(current && current->protocolReady &&
              current->deltaBaseRevision > initial->revision &&
              current->deltaBaseRevision != initial->revision,
          "D refers to the evicted ready B base");
  auto packets = drainPlay(session);
  auto body = packetOf(packets, data.packet("play", "clientbound", "light_update"));
  require(!body.empty() && !session.closed(),
          "session falls back to a full D delta instead of disconnecting");
  Reader update(body);
  require(update.varInt() == data.packet("play", "clientbound", "light_update") &&
              update.varInt() == 0 && update.varInt() == 0,
          "evicted-base fallback has an exact light_update header");
  verifyLightData(update, *current, initial.get(), "evicted ready base");
  service->resumeForTesting();
}

void managedResidencyProviderDuringHandshake(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.loadChunk(2, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(),
          "handshake provider light batch");
  service->drainPublished();
  ServerOptions options;
  options.chunks = {{0, 0}};
  options.spawn = {8.5, 70, 8.5};
  options.viewDistance = 2;
  Session session(world, data, options);
  session.setManagedResidencyProvider(
      [](int x, int z) { return x == 0 && z == 0; },
      [] { return std::vector<std::pair<int, int>>{{0, 0}}; });
  session.managedChunkResidency(1, 0, true);
  session.managedChunkResidency(1, 0, false);
  session.managedChunkResidency(2, 0, true);
  require(drain(session).empty(),
          "provider install and configuration residency emit no Play packet");
  world.lightPublicationObserver =
      [&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        session.lightPublished(std::move(snapshot));
      };
  auto joined = join(session, data);
  require(!joined.empty(), "managed join emits packets");
  Reader first(joined.front());
  require(first.varInt() == data.packet("play", "clientbound", "login"),
          "Play login is the first managed join packet");
  bool sawCenter = false, sawChunk = false;
  int joinedChunks = 0;
  for (const auto &body : joined) {
    Reader packet(body);
    const int id = packet.varInt();
    sawCenter |= id == data.packet("play", "clientbound",
                                   "set_chunk_cache_center");
    if (id == data.packet("play", "clientbound", "level_chunk_with_light"))
      ++joinedChunks;
    sawChunk |= joinedChunks != 0;
  }
  require(sawCenter && sawChunk,
          "managed join sends center and chunk after login");
  require(joinedChunks == 2,
          "join includes the provider snapshot and configuration residency");
}

void managedResidencyAvailability(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(),
          "managed residency light batch");
  service->drainPublished();
  ServerOptions options;
  options.chunks = {{0, 0}};
  options.spawn = {8.5, 70, 8.5};
  options.viewDistance = 2;
  Session session(world, data, options);
  world.lightPublicationObserver =
      [&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        session.lightPublished(std::move(snapshot));
      };
  auto initial = join(session, data);
  require(!packetOf(initial,
                    data.packet("play", "clientbound",
                                "level_chunk_with_light")).empty(),
          "already resident managed option chunks enter initial view");

  session.managedChunkResidency(1, 0, false);
  require(!session.hasManagedChunk(1, 0),
          "nonresident unload is an idempotent availability erase");
}

void explicitWorldLifecycleIsolation(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(), "lifecycle initial batch");
  service->drainPublished();
  auto initial = combinedForTesting(*service, 0, 0);
  require(initial && initial->protocolReady, "lifecycle initial snapshot");
  ServerOptions options;
  options.chunks.push_back({0, 0});
  options.spawn = {8.5, 70, 8.5};
  options.viewDistance = 2;
  Session session(world, data, options);
  world.lightPublicationObserver =
      [&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        session.lightPublished(std::move(snapshot));
      };
  world.lightChunkLifecycleObserver = [&](int x, int z, bool loaded) {
    session.lightChunkLifecycle(x, z, loaded);
  };
  (void)join(session, data);

  require(world.setBlock({4, 8, 4},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "lifecycle fixture change enters");
  require(service->waitIdleForTesting(), "lifecycle old revision completes");
  service->drainPublished();
  world.unloadChunk(0, 0);
  auto packets = drainPlay(session);
  require(packetOf(packets, data.packet("play", "clientbound",
                                        "light_update")).empty(),
          "explicit unload cancels queued old dynamic light");
  require(!packetOf(packets, data.packet("play", "clientbound",
                                         "forget_level_chunk")).empty(),
          "explicit unload sends forget without moving the player");

  world.loadChunk(0, 0);
  require(service->waitIdleForTesting(), "reloaded light batch completes");
  service->drainPublished();
  session.tick();
  world.unloadChunk(0, 0);
  packets = drainPlay(session);
  int started = 0, chunks = 0, finished = 0, finishedCount = -1, forgotten = 0;
  for (int index = 0; index < static_cast<int>(packets.size()); ++index) {
    Reader packet(packets[index]);
    const int id = packet.varInt();
    if (id == data.packet("play", "clientbound", "chunk_batch_start")) {
      started = index + 1;
    } else if (id == data.packet("play", "clientbound",
                                 "level_chunk_with_light")) {
      ++chunks;
    } else if (id == data.packet("play", "clientbound",
                                 "chunk_batch_finished")) {
      finished = index + 1;
      finishedCount = packet.varInt();
    } else if (id == data.packet("play", "clientbound",
                                 "forget_level_chunk")) {
      forgotten = index + 1;
    }
  }
  require(started && chunks == 1 && finishedCount == chunks &&
              started < finished && finished < forgotten,
          "explicit unload preserves submitted first-packet FIFO count");

  Writer ack;
  ack.f32(9);
  session.receive(frame(
      data.packet("play", "serverbound", "chunk_batch_received"), ack.data));
  world.loadChunk(0, 0);
  require(service->waitIdleForTesting(), "final reload light completes");
  service->drainPublished();
  session.tick();
  packets = drainPlay(session);
  auto reload = packetOf(packets, data.packet("play", "clientbound",
                                               "level_chunk_with_light"));
  require(!reload.empty(), "same-view reload sends the new epoch first packet");
  Reader reloaded(reload);
  require(reloaded.varInt() == data.packet("play", "clientbound",
                                          "level_chunk_with_light") &&
              reloaded.i32() == 0 && reloaded.i32() == 0,
          "same-view reload first packet has exact coordinates");
  auto latest = combinedForTesting(*service, 0, 0);
  require(latest && latest->protocolReady &&
              latest->lifecycleEpoch > initial->lifecycleEpoch,
          "same-view reload uses a newer lifecycle epoch");
  verifyChunkLight(reloaded, *latest);
}

void protocolFailureIsConnectionLocal(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(), "connection-error initial batch");
  service->drainPublished();
  auto initial = combinedForTesting(*service, 0, 0);
  require(initial && initial->protocolReady, "connection-error initial snapshot");
  ServerOptions options;
  options.chunks.push_back({0, 0});
  options.viewDistance = 2;
  Session failing(world, data, options);
  Session healthy(world, data, options);
  (void)join(failing, data);
  (void)join(healthy, data);

  auto wrongLifetime = std::make_shared<LightChunkSnapshot>(*initial);
  wrongLifetime->lifecycleEpoch = initial->lifecycleEpoch + 1;
  wrongLifetime->revision = initial->revision + 1;
  wrongLifetime->deltaBaseRevision = initial->revision;
  wrongLifetime->layer = LightLayer::Block;
  wrongLifetime->changedSections = {{0, false, true}};
  failing.lightPublished(wrongLifetime);
  require(failing.closed(),
          "cross-lifetime light error is contained to its connection");
  require(!healthy.closed(), "a different connection remains open");

  require(world.setBlock({5, 8, 5},
                         Blocks::REDSTONE_LAMP.defaultState().with("lit", "true")),
          "healthy connection update enters");
  require(service->waitIdleForTesting(), "healthy connection light completes");
  service->drainPublished();
  healthy.lightPublished(
      service->latestLayerChunk(LightLayer::Block, 0, 0));
  healthy.lightPublished(
      service->latestLayerChunk(LightLayer::Sky, 0, 0));
  auto packets = drainPlay(healthy);
  require(!packetOf(packets, data.packet("play", "clientbound", "light_update"))
               .empty(),
          "healthy connection continues receiving light updates");
}

std::shared_ptr<LightChunkSnapshot>
denseSnapshot(const LightChunkSnapshot &base, std::uint64_t revision,
              std::uint8_t seed) {
  auto result = std::make_shared<LightChunkSnapshot>(base);
  result->revision = revision;
  result->deltaBaseRevision = base.revision;
  result->sections.clear();
  result->changedSections.clear();
  std::array<std::uint8_t, kLightVolume> values{};
  for (int index = 0; index < kLightVolume; ++index)
    values[static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((index + seed) & 15);
  for (const auto &source : base.sections) {
    auto section = std::make_shared<LightSectionSnapshot>(*source);
    section->layers[static_cast<std::size_t>(LightLayer::Sky)] =
        LightLayerSnapshot::mixed(values);
    section->layers[static_cast<std::size_t>(LightLayer::Block)] =
        LightLayerSnapshot::mixed(values);
    result->sections.push_back(std::move(section));
    result->changedSections.push_back({source->sectionY, true, true});
  }
  return result;
}

void lightBackpressure(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(), "backpressure light load");
  service->drainPublished();
  auto ready = combinedForTesting(*service, 0, 0);
  require(ready && ready->protocolReady, "backpressure light snapshot ready");
  ServerOptions options;
  options.chunks.push_back({0, 0});
  options.viewDistance = 2;
  Session session(world, data, options);
  (void)join(session, data);
  (void)drain(session);

  for (std::uint64_t update = 1; update <= 512 && !session.closed(); ++update) {
    session.lightPublished(denseSnapshot(*ready, ready->revision + update,
                                         static_cast<std::uint8_t>(update)));
  }
  require(session.closed(),
          "light updates alone hit the per-connection retained-byte budget");
  require(session.error() == "Connection output backlog exceeded",
          "light backpressure reports the bounded output failure");
}

void noSkyUsesExplicitZero() {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, false, false});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(), "no-sky batch completes");
  service->drainPublished();
  auto missing = combinedForTesting(*service, 0, 0);
  require(missing && missing->protocolReady && !missing->skyLightEnabled,
          "no-sky snapshot is protocol ready without fabricated brightness");
  auto missingPlan = prepareLightUpdate(missing);
  require(missingPlan.sky.dataSections.empty() &&
              missingPlan.sky.emptySections.empty(),
          "missing no-sky layers are omitted rather than sent as full light");

  // Java's disabled empty DataLayer is a stored uniform zero, distinct from a
  // missing layer even though both query as brightness zero.
  auto zero = std::make_shared<LightChunkSnapshot>(*missing);
  zero->sections.clear();
  for (const auto &section : missing->sections) {
    auto copy = std::make_shared<LightSectionSnapshot>(*section);
    copy->layers[static_cast<std::size_t>(LightLayer::Sky)] =
        LightLayerSnapshot::uniform(0);
    zero->sections.push_back(std::move(copy));
  }
  auto zeroPlan = prepareLightUpdate(zero);
  require(zeroPlan.sky.dataSections.empty() &&
              zeroPlan.sky.emptySections.size() ==
                  static_cast<std::size_t>(zero->lightSectionCount()),
          "stored no-sky zero layers use empty masks and no bytes");
  auto newerLifetime = std::make_shared<LightChunkSnapshot>(*zero);
  ++newerLifetime->lifecycleEpoch;
  bool rejected = false;
  try {
    (void)prepareLightUpdate(newerLifetime, zero);
  } catch (const ProtocolError &) {
    rejected = true;
  }
  require(rejected, "incremental light cannot cross a load/reload lifecycle");

  auto withSky = std::make_shared<LightChunkSnapshot>(*zero);
  withSky->revision = zero->revision + 1;
  auto skySection = std::make_shared<LightSectionSnapshot>(*withSky->sections[1]);
  skySection->layers[static_cast<std::size_t>(LightLayer::Sky)] =
      LightLayerSnapshot::mixedPacked(std::array<std::uint8_t, kLightLayerBytes>{});
  withSky->sections[1] = skySection;
  withSky->changedSections = {{skySection->sectionY, true, false}};
  auto withoutSky = std::make_shared<LightChunkSnapshot>(*withSky);
  withoutSky->revision = withSky->revision + 1;
  auto missingSection = std::make_shared<LightSectionSnapshot>(*skySection);
  missingSection->layers[static_cast<std::size_t>(LightLayer::Sky)] =
      LightLayerSnapshot::missing();
  withoutSky->sections[1] = missingSection;
  withoutSky->changedSections = {{missingSection->sectionY, true, false}};
  withoutSky->deltaBaseRevision = withSky->revision;
  auto missingDelta = prepareLightUpdate(withoutSky, withSky);
  require(missingDelta.sky.dataSections.empty() &&
              missingDelta.sky.emptySections.empty(),
          "raw Missing sky layer sets neither mask, unlike stored zero");
}

void nativeServerFixturePublishesEveryChunk() {
  World world;
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z) {
      world.loadChunk(x, z);
      for (int bx = 0; bx < 16; ++bx)
        for (int bz = 0; bz < 16; ++bz)
          world.rawSetBlock({x * 16 + bx, 64, z * 16 + bz},
                            Blocks::SMOOTH_STONE.defaultState());
    }
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(),
          "native server fixture light batch completes");
  service->drainPublished();
  for (int x = -1; x <= 1; ++x)
    for (int z = -1; z <= 1; ++z) {
      auto snapshot = combinedForTesting(*service, x, z);
      require(snapshot && snapshot->protocolReady,
              "every native server fixture chunk is protocol ready");
    }
}

void unpublishedLightDefersJoin(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.enableLighting(LightWorkerOptions{1000, 4096, true, true});
  auto *service = world.lightService();
  require(service && combinedForTesting(*service, 0, 0) == nullptr,
          "paused worker has no published initial light");
  ServerOptions options;
  options.chunks.push_back({0, 0});
  options.spawn = {8.5, 70, 8.5};
  options.viewDistance = 2;
  Session session(world, data, options);
  world.lightPublicationObserver =
      [&](std::shared_ptr<const LightChunkSnapshot> snapshot) {
        session.lightPublished(std::move(snapshot));
      };
  auto before = join(session, data);
  require(packetOf(before, data.packet("play", "clientbound",
                                       "level_chunk_with_light")).empty() &&
              packetOf(before, data.packet("play", "clientbound",
                                           "player_position")).empty(),
          "join neither invents light nor sends position before a chunk");

  service->resumeForTesting();
  require(service->waitIdleForTesting(), "deferred initial light completes");
  world.tick();
  session.tick();
  auto after = drainPlay(session);
  auto chunkIndex = std::find_if(after.begin(), after.end(), [&](const Bytes &body) {
    Reader reader(body);
    return reader.varInt() == data.packet("play", "clientbound",
                                          "level_chunk_with_light");
  });
  auto positionIndex = std::find_if(after.begin(), after.end(), [&](const Bytes &body) {
    Reader reader(body);
    return reader.varInt() == data.packet("play", "clientbound",
                                          "player_position");
  });
  require(chunkIndex != after.end() && positionIndex != after.end() &&
              chunkIndex < positionIndex,
          "complete chunk light precedes the deferred player position");
}

void unknownOpticsRemainUnpublished(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({1, 8, 1}, BlockState("minecraft:unmapped_light_test"));
  world.enableLighting(LightWorkerOptions{1000, 4096, false, true});
  auto *service = world.lightService();
  require(service && service->waitIdleForTesting(), "unknown-optics batch completes");
  service->drainPublished();
  auto incomplete = combinedForTesting(*service, 0, 0);
  require(incomplete && !incomplete->protocolReady &&
              !incomplete->propagationComplete,
          "unknown optical metadata never becomes protocol ready");
  ServerOptions options;
  options.chunks.push_back({0, 0});
  options.viewDistance = 2;
  Session session(world, data, options);
  auto packets = join(session, data);
  require(packetOf(packets, data.packet("play", "clientbound",
                                        "level_chunk_with_light")).empty() &&
              packetOf(packets, data.packet("play", "clientbound",
                                            "player_position")).empty(),
          "incomplete optics defer both light and player position");
}
void independentLayerDeltaUsesWriteMetadata() {
  auto makeView = [](std::uint64_t revision) {
    auto view = std::make_shared<LightChunkSnapshot>();
    view->chunkX = 0;
    view->chunkZ = 0;
    view->minBlockSectionY = 0;
    view->blockSectionCount = 1;
    view->lifecycleEpoch = 7;
    view->revision = revision;
    view->layer = LightLayer::Block;
    view->blockLightComplete = true;
    view->protocolReady = true;
    for (int sectionY = -1; sectionY <= 1; ++sectionY)
      view->sections.push_back(
          std::make_shared<LightSectionSnapshot>(sectionY, true));
    return view;
  };
  auto first = makeView(1);
  std::array<std::uint8_t, kLightVolume> values{};
  values[lightIndex(3, 4, 5)] = 9;
  values[lightIndex(6, 7, 8)] = 1;
  auto mixed = LightLayerSnapshot::mixed(values);
  auto firstSection =
      std::make_shared<LightSectionSnapshot>(*first->sections[1]);
  firstSection->layers[static_cast<std::size_t>(LightLayer::Block)] = mixed;
  first->sections[1] = firstSection;

  auto initial = prepareLayerLightUpdate(first, LightLayer::Block);
  require(initial.block.dataSections == std::vector<int>{0} &&
              initial.sky.dataSections.empty() &&
              initial.sky.emptySections.empty(),
          "block initial plan reads only the block layer");
  require(initial.snapshot->section(0)
                  ->layers[static_cast<std::size_t>(LightLayer::Sky)]
                  .isMissing(),
          "block protocol source is not a combined section");

  auto second = makeView(2);
  second->sections[1] = first->sections[1];
  second->deltaBaseRevision = 1;
  second->changedSections = {{0, false, true}};
  mixed.writeCell(lightIndex(3, 4, 5), 2);
  const auto firstVersion = lightLayerVersion(*first);
  auto delta =
      prepareLayerLightUpdate(second, LightLayer::Block, &firstVersion);
  require(second->section(0)
                  ->layers[static_cast<std::size_t>(LightLayer::Block)]
                  .shared() == mixed.packedPtr() &&
              delta.block.dataSections == std::vector<int>{0} &&
              delta.sky.dataSections.empty() &&
              delta.sky.emptySections.empty(),
          "same mutable page is sent from dirty metadata, not pointer/content diff");

  second->changedSections.clear();
  auto noChange =
      prepareLayerLightUpdate(second, LightLayer::Block, &firstVersion);
  require(noChange.block.dataSections.empty() &&
              noChange.block.emptySections.empty(),
          "an unmarked mutable-page revision produces an empty block delta");

  bool wrongLayerRejected = false;
  try {
    (void)prepareLayerLightUpdate(second, LightLayer::Sky);
  } catch (const ProtocolError &) {
    wrongLayerRejected = true;
  }
  require(wrongLayerRejected,
          "layer delta refuses a view carrying the other layer tag");
}

} // namespace

int main(int argc, char **argv) {
  try {
    require(argc == 2, "light_protocol_tests needs assets/protocol775");
    ProtocolData data(argv[1]);
    strictInitialDynamicClearAndReload(data);
    readyIncompleteReadyDelta(data);
    evictedReadyBaseFallsBack(data);
    managedResidencyProviderDuringHandshake(data);
    managedResidencyAvailability(data);
    explicitWorldLifecycleIsolation(data);
    protocolFailureIsConnectionLocal(data);
    noSkyUsesExplicitZero();
    independentLayerDeltaUsesWriteMetadata();
    nativeServerFixturePublishesEveryChunk();
    unpublishedLightDefersJoin(data);
    unknownOpticsRemainUnpublished(data);
    lightBackpressure(data);
    if (const char *path = std::getenv("REDSTONE_PLAY_FIXTURE")) {
      std::ofstream file(path, std::ios::binary);
      for (const auto &body : records) {
        Writer record;
        record.i32(static_cast<int>(body.size()));
        record.raw(body);
        file.write(reinterpret_cast<const char *>(record.data.data()),
                   record.data.size());
      }
      require(file.good(), "writes optional official-codec fixture");
    }
    std::cout << "light protocol tests passed: " << records.size()
              << " packets\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "light protocol test failure: " << error.what() << '\n';
    return 1;
  }
}
