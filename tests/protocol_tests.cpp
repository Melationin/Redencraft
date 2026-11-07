#include "redstone/net/session.hpp"
#include "redstone/piston.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>

using namespace redstone;
using namespace redstone::net;
namespace {
std::vector<Bytes> allPlay;
std::vector<int> drain(Session &session, bool capture = false) {
  std::vector<int> ids;
  for (auto &bytes : session.takeOutput()) {
    auto body = popFrame(bytes);
    assert(body && bytes.empty());
    Reader in(*body);
    ids.push_back(in.varInt());
    if (capture)
      allPlay.push_back(std::move(*body));
  }
  return ids;
}
void hello(Session &session, int next = 2, int protocol = 775) {
  Writer out;
  out.varInt(protocol);
  out.string("localhost");
  out.i16(25565);
  out.varInt(next);
  session.receive(frame(0, out.data));
}
void join(Session &session, const ProtocolData &data) {
  hello(session);
  Writer out;
  out.string("Tester");
  out.raw(Uuid{});
  session.receive(frame(0, out.data));
  assert(session.phase() == Phase::Login);
  assert(drain(session) == std::vector<int>{2});
  session.receive(frame(3, {}));
  assert(session.phase() == Phase::Configuration);
  assert(drain(session) == std::vector<int>{14});
  out = {};
  out.varInt(0);
  session.receive(frame(7, out.data));
  auto ids = drain(session);
  assert(ids.size() == data.configuration.size() + 1 && ids.back() == 3);
  session.receive(frame(3, {}));
  assert(session.phase() == Phase::Play);
  ids = drain(session, true);
  assert(ids.front() == 49);
  auto start = std::find(ids.begin(), ids.end(), 12),
       chunk = std::find(ids.begin(), ids.end(), 45),
       end = std::find(ids.begin(), ids.end(), 11);
  assert(start < chunk && chunk < end);
  out = {};
  out.varInt(1);
  session.receive(frame(0, out.data));
}
void use(Session &s, Pos clicked, Direction face, int seq = 1) {
  Writer out;
  out.varInt(0);
  out.position(clicked);
  out.varInt(int(face));
  out.f32(.5);
  out.f32(.5);
  out.f32(.5);
  out.bool_(false);
  out.bool_(false);
  out.varInt(seq);
  s.receive(frame(66, out.data));
}
void select(Session &s, int slot) {
  Writer out;
  out.i16(static_cast<short>(slot));
  s.receive(frame(53, out.data));
}
void creative(Session &s, const ProtocolData &data, int slot,
              const std::string &name) {
  Writer out;
  out.i16(static_cast<short>(36 + slot));
  out.varInt(64);
  out.varInt(data.registry.itemId("minecraft:" + name));
  out.varInt(0);
  out.varInt(0);
  s.receive(frame(56, out.data));
}
void viewDistanceTen(const ProtocolData &data) {
  World world;
  ServerOptions options;
  options.viewDistance = 10;
  for (int x : {0, 9, 10, 11}) {
    world.loadChunk(x, 0);
    options.chunks.emplace_back(x, 0);
  }
  Session session(world, data, options);
  const auto begin = allPlay.size();
  join(session, data);
  bool radiusSeen = false;
  std::set<std::pair<int, int>> chunks;
  for (auto i = begin; i < allPlay.size(); ++i) {
    Reader in(allPlay[i]);
    const auto id = in.varInt();
    if (id == data.packet("play", "clientbound", "set_chunk_cache_radius")) {
      assert(in.varInt() == 10 && in.empty());
      radiusSeen = true;
    } else if (id == data.packet("play", "clientbound", "level_chunk_with_light")) {
      const int x = in.i32(), z = in.i32();
      chunks.emplace(x, z);
    }
  }
  assert(radiusSeen);
  assert((chunks == std::set<std::pair<int, int>>{{0, 0}, {9, 0}, {10, 0}}));
}
void chunkViewAndTiming(const ProtocolData &data) {
  World world;
  ServerOptions options;
  options.viewDistance = 2;
  options.spawn.x = 15.5;
  for (int x = -2; x <= 16; ++x)
    for (int z = -2; z <= 2; ++z) {
      world.loadChunk(x, z);
      options.chunks.emplace_back(x, z);
    }
  world.spawnItem({1, 65, 1}, "white_carpet");
  world.spawnItem({201, 65, 1}, "white_carpet");
  world.rawSetBlock({16, 65, 8}, defaultState("glass"));
  Session session(world, data, options);
  join(session, data); // Large worlds send at most two, never all 95.
  Writer destroy;
  destroy.varInt(0);
  destroy.position({16, 65, 8});
  destroy.u8(1);
  destroy.varInt(1);
  session.receive(
      frame(data.packet("play", "serverbound", "player_action"), destroy.data));
  assert(world.getBlock({16, 65, 8}).name() ==
         "minecraft:glass"); // Nearby but not sent.
  drain(session, true);
  session.tick();
  auto ids = drain(session, true);
  assert(std::count(ids.begin(), ids.end(), 45) == 0); // Awaiting ACK.
  Writer ack;
  ack.f32(1000); // A client's rate request cannot exceed the large-world cap.
  session.receive(frame(
      data.packet("play", "serverbound", "chunk_batch_received"), ack.data));
  session.tick();
  ids = drain(session, true);
  assert(std::count(ids.begin(), ids.end(), 45) == 2);
  session.blockUpdate({201, 65, 1}, defaultState("glass"));
  session.blockEvent({{201, 65, 1}, "minecraft:note_block", 0, 0});
  session.flushBlockUpdates();
  assert(drain(session).empty());
  Writer move;
  move.f64(201.5);
  move.f64(66);
  move.f64(8.5);
  move.u8(0);
  session.receive(
      frame(data.packet("play", "serverbound", "move_player_pos"), move.data));
  session.tick();
  session.broadcastEntities();
  ids = drain(session, true);
  assert(std::count(ids.begin(), ids.end(), 37) == 4); // Unload prior view.
  assert(std::count(ids.begin(), ids.end(), 45) ==
         0); // In-flight ACK still required.
  assert(std::count(ids.begin(), ids.end(),
                    data.packet("play", "clientbound", "remove_entities")) ==
         1);
  session.receive(frame(
      data.packet("play", "serverbound", "chunk_batch_received"), ack.data));
  session.tick();
  session.broadcastEntities();
  ids = drain(session, true);
  assert(std::count(ids.begin(), ids.end(), 45) == 2);
  assert(std::count(ids.begin(), ids.end(),
                    data.packet("play", "clientbound", "add_entity")) == 1);
  assert(world.isChunkLoaded(0, 0) && world.isChunkLoaded(12, 0));
  const auto tick = world.gameTime();
  session.sendTickTime(12.34567, 123, 20);
  auto packets = session.takeOutput();
  assert(packets.size() == 1);
  auto body = popFrame(packets[0]);
  Reader timing(*body);
  assert(timing.varInt() == data.packet("play", "clientbound", "system_chat"));
  assert(timing.u8() == 8);
  const auto bytes = timing.raw(timing.i16());
  assert(std::string(bytes.begin(), bytes.end()) == "Tick 123: 12.346 ms (1s avg, 20 ticks)");
  assert(!timing.bool_() && timing.empty());
  assert(world.gameTime() ==
         tick); // Observational reporting never advances simulation.
  allPlay.push_back(*body);
}
void fallingLandingPacketOrder(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.rawSetBlock({8, 64, 8}, defaultState("smooth_stone"));
  world.spawnFallingBlock({8.5, 65, 8.5}, defaultState("sand"));
  ServerOptions options;
  options.chunks = {{0, 0}};
  Session session(world, data, options);
  join(session, data);
  world.blockUpdateObserver = [&](Pos p, const BlockState &state, int) {
    session.blockUpdate(p, state);
  };
  world.blockUpdatesBroadcastObserver = [&] { session.flushBlockUpdates(); };
  world.entityTrackingBroadcastObserver = [&] { session.broadcastEntities(); };
  world.entityRemovedObserver = [&](int id) { session.entityRemoved(id); };
  world.fallingBlockLandObserver = [&](int id, Pos p, const BlockState &state) {
    session.fallingBlockLanded(id, p, state);
  };
  world.tick();
  session.tick();
  auto ids = drain(session, true);
  const auto block =
      std::find(ids.begin(), ids.end(),
                data.packet("play", "clientbound", "block_update"));
  const auto removed = std::find(ids.begin(), ids.end(),
      data.packet("play", "clientbound", "remove_entities"));
  assert(block != ids.end() && removed != ids.end() && block < removed);
  assert(world.getBlock({8, 65, 8}).name() == "minecraft:sand");
}
void tntPacketOrder(const ProtocolData &data) {
  const auto packet = [&](std::string_view name) {
    return data.packet("play", "clientbound", name);
  };
  World world;
  world.loadChunk(-1, 0);
  world.spawnPrimedTnt({-0.5, 70, 8.5}, PrimedTnt::defaultFuse);
  auto &tnt = static_cast<PrimedTnt &>(*world.entities.front());
  const int entityId = tnt.id;
  assert(world.entitiesIn(tnt.boundingBox()) == std::vector<Entity *>{&tnt});
  ServerOptions options;
  options.spawn = {-0.5, 70, 8.5};
  options.chunks = {{-1, 0}};
  Session session(world, data, options);
  join(session, data);
  Reader bundleStart(allPlay[allPlay.size() - 3]);
  Reader spawn(allPlay[allPlay.size() - 2]);
  Reader bundleEnd(allPlay.back());
  assert(bundleStart.varInt() == packet("bundle_delimiter") && bundleStart.empty());
  assert(spawn.varInt() == packet("add_entity") && spawn.varInt() == entityId);
  assert(bundleEnd.varInt() == packet("bundle_delimiter") && bundleEnd.empty());
  session.broadcastEntities(); // First movement update, interval index zero.
  drain(session, true);

  auto begin = allPlay.size();
  tnt.position.x -= 64.5 / 4096;
  tnt.yaw = 0.9f; // Mth.packDegrees floors this to zero.
  tnt.setDeltaMovement(0, 0.25, 0);
  --tnt.fuse;
  session.broadcastEntities();
  const auto ids = drain(session, true);
  assert((ids == std::vector{packet("set_entity_motion"), packet("move_entity_pos"),
                             packet("set_entity_data")}));
  Reader motion(allPlay[begin]);
  assert(motion.varInt() == ids[0] && motion.varInt() == entityId);
  const auto low = motion.u8();
  const auto mid = motion.u8();
  const std::uint64_t bits = low | (std::uint64_t{mid} << 8) |
      (std::uint64_t{static_cast<std::uint32_t>(motion.i32())} << 16);
  assert((bits & 7) == 1 && ((bits >> 18) & 0x7fff) == 20479 && motion.empty());
  Reader pos(allPlay[begin + 1]);
  assert(pos.varInt() == ids[1] && pos.varInt() == entityId);
  assert(pos.i16() == -64 && pos.i16() == 0 && pos.i16() == 0);
  assert(!pos.bool_() && pos.empty());
  Reader values(allPlay[begin + 2]);
  assert(values.varInt() == ids[2] && values.varInt() == entityId);
  assert(values.u8() == 8 && values.varInt() == 1 && values.varInt() == 79);
  assert(values.u8() == 255 && values.empty());

  const Pos chained{-2, 70, 8};
  world.rawSetBlock(chained, defaultState("tnt"));
  world.entityRemovedObserver = [&](int id) { session.entityRemoved(id); };
  world.entityAddedObserver = [&](const Entity &e) { session.entityAdded(e); };
  world.explosionObserver = [&](Vec3 center, float radius, int count) {
    session.explosion(center, radius, count);
  };
  world.blockUpdateObserver = [&](Pos p, const BlockState &state, int) {
    session.blockUpdate(p, state);
  };
  tnt.fuse = 1;
  begin = allPlay.size();
  world.tick();
  const auto blast = drain(session, true);
  assert((blast == std::vector{packet("remove_entities"), packet("bundle_delimiter"),
                               packet("add_entity"), packet("set_entity_data"),
                               packet("bundle_delimiter"), packet("explode")}));
  Reader removed(allPlay[begin]);
  assert(removed.varInt() == blast[0] && removed.varInt() == 1);
  assert(removed.varInt() == entityId && removed.empty());
  Reader added(allPlay[begin + 2]);
  assert(added.varInt() == blast[2]);
  const int chainedId = added.varInt();
  assert(chainedId != entityId);
  Reader chainedValues(allPlay[begin + 3]);
  assert(chainedValues.varInt() == blast[3] && chainedValues.varInt() == chainedId);
  assert(chainedValues.u8() == 8 && chainedValues.varInt() == 1);
  const auto fuse = chainedValues.varInt();
  assert(fuse >= 10 && fuse < 30);
  assert(chainedValues.u8() == 255 && chainedValues.empty());
  Reader effect(allPlay[begin + 5]);
  assert(effect.varInt() == blast[5]);
  assert(effect.f64() == -0.5 - 64.5 / 4096 && effect.f64() > 70 && effect.f64() == 8.5);
  assert(effect.f32() == 4 && effect.i32() >= 1 && !effect.bool_());
  assert(effect.varInt() == data.explosionEffectIds.at("explosion_emitter"));
  assert(world.getBlock(chained).isAir());
  session.flushBlockUpdates();
  assert(drain(session, true) == std::vector{packet("block_update")});
}
void sectionPacketOrder(const ProtocolData &data) {
  World world;
  constexpr int chunkX = 2048, chunkZ = -2048;
  constexpr Pos origin{chunkX * 16, -16, chunkZ * 16};
  const Pos upper{origin.x, 0, origin.z};
  world.loadChunk(chunkX, chunkZ);
  ServerOptions options;
  options.spawn = {origin.x + 0.5, 66, origin.z + 0.5};
  options.chunks = {{chunkX, chunkZ}};
  Session session(world, data, options);
  join(session, data);
  const auto glass = defaultState("glass");
  world.rawSetBlock(upper, glass);
  session.blockUpdate(upper, glass);
  // Crosses the first rehash at 25 entries and includes the special zero key.
  for (int n = 0; n < 26; ++n) {
    const int local = n * 151;
    const Pos p{origin.x + (local >> 8), origin.y + (local & 15),
                origin.z + ((local >> 4) & 15)};
    world.rawSetBlock(p, glass);
    session.blockUpdate(p, glass);
    session.blockUpdate(p, glass);
  }
  session.flushBlockUpdates();
  const auto begin = allPlay.size();
  const auto ids = drain(session, true);
  assert((ids == std::vector{data.packet("play", "clientbound", "section_blocks_update"),
                             data.packet("play", "clientbound", "block_update")}));
  Reader section(allPlay[begin]);
  assert(section.varInt() == ids[0]);
  const auto key = (std::int64_t{chunkX} << 42) |
                   ((chunkZ & 0x3fffffLL) << 20) | 0xfffffLL;
  assert(section.i64() == key);
  // Iteration recorded from the 26.1 server's fastutil ShortOpenHashSet.
  constexpr int order[]{0, 1208, 2416, 3624, 2869, 453, 1963, 906, 2114, 1359,
                        151, 1661, 3171, 604, 1812, 1057, 3473, 2718, 302, 3775,
                        3322, 2567, 3020, 2265, 1510, 755};
  assert(section.varInt() == std::size(order));
  for (int local : order)
    assert(section.varLong() == (std::int64_t{data.registry.stateId(glass)} << 12 | local));
  assert(section.empty());
  Reader block(allPlay[begin + 1]);
  assert(block.varInt() == ids[1] && block.position() == upper);
  assert(block.varInt() == data.registry.stateId(glass) && block.empty());
  session.blockEvent({{origin.x, 130, origin.z}, "minecraft:note_block", 0, 0});
  assert(drain(session).empty()); // Loaded and sent, but outside 64 blocks.
}
void entityBroadcastPhase(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  world.spawnItem({8, 70, 8}, "white_carpet");
  ServerOptions options;
  options.chunks = {{0, 0}};
  Session session(world, data, options);
  join(session, data);
  Entity *entity = world.entities.front().get();
  const int entityId = entity->id;
  entity->position.x += 1;
  world.entityTrackingBroadcastObserver = [&] { session.broadcastEntities(); };
  world.entityAddedObserver = [&](const Entity &e) { session.entityAdded(e); };
  world.entityRemovedObserver = [&](int id) { session.entityRemoved(id); };
  int newEntityId = 0;
  world.blockEntityTicker = [&](World &w) {
    entity->discard();
    w.spawnItem({10, 70, 8}, "white_carpet");
    newEntityId = w.entities.back()->id;
    w.entities.back()->discard();
  };
  world.tick();
  auto packets = session.takeOutput();
  int moveIndex = -1, removeIndex = -1, spawnIndex = -1;
  int spawnedRemoveIndex = -1, index = 0;
  for (auto &bytes : packets) {
    auto body = popFrame(bytes);
    Reader in(*body);
    const int id = in.varInt();
    if (id == data.packet("play", "clientbound", "move_entity_pos")) {
      assert(in.varInt() == entityId);
      assert(in.i16() == 4096);
      moveIndex = index;
    }
    if (id == data.packet("play", "clientbound", "remove_entities")) {
      assert(in.varInt() == 1);
      const int removedId = in.varInt();
      if (removedId == entityId) removeIndex = index;
      if (removedId == newEntityId) spawnedRemoveIndex = index;
    }
    if (id == data.packet("play", "clientbound", "add_entity")) {
      assert(in.varInt() == newEntityId);
      spawnIndex = index;
    }
    ++index;
  }
  assert(moveIndex >= 0 && removeIndex > moveIndex && spawnIndex > moveIndex &&
         spawnedRemoveIndex > spawnIndex);
  // A second broadcast at the same connection tick does not repeat removal
  // or spawn for entities already updated by the lifecycle callbacks.
  session.broadcastEntities();
  auto ids = drain(session, true);
  assert(std::find(ids.begin(), ids.end(), data.packet("play", "clientbound", "remove_entities")) == ids.end());
  assert(std::find(ids.begin(), ids.end(), data.packet("play", "clientbound", "add_entity")) == ids.end());
  world.spawnItem({11, 70, 8}, "white_carpet");
  drain(session, true);
  world.entities.back()->discard();
  session.broadcastEntities();
  ids = drain(session, true);
  assert(std::find(ids.begin(), ids.end(), data.packet("play", "clientbound", "remove_entities")) != ids.end());
}
void sessionOutputLimit(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  ServerOptions options;
  options.chunks = {{0, 0}};
  Session slow(world, data, options), other(world, data, options);
  join(slow, data);
  join(other, data);
  for (std::size_t i = 0; i < maxQueuedOutputPackets; ++i)
    slow.sendTickTime(1.0, static_cast<std::int64_t>(i));
  assert(!slow.closed());
  slow.sendTickTime(1.0, maxQueuedOutputPackets);
  assert(slow.closed());
  assert(slow.error() == "Connection output backlog exceeded");
  assert(slow.takeOutputItems().empty());
  bool completed = false;
  world.blockEntityTicker = [&](World &) { completed = true; };
  world.tick();
  other.sendTickTime(1.0, 1);
  assert(completed && !other.closed());
  assert(other.takeOutputItems().size() == 1);
}
void placementRules(const ProtocolData &data) {
  World world;
  world.loadChunk(0, 0);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      world.rawSetBlock({x, 64, z}, defaultState("smooth_stone"));
  ServerOptions options;
  options.chunks = {{0, 0}};
  Session session(world, data, options);
  join(session, data);
  // Gate opens away from a player facing its back, then closes without
  // changing the powered property. Placement uses horizontal player facing.
  world.rawSetBlock({7, 65, 8}, defaultState("birch_fence_gate"));
  use(session, {7, 65, 8}, Direction::Up);
  assert(world.getBlock({7, 65, 8}).get("open") == "true" &&
         world.getBlock({7, 65, 8}).get("facing") == "south");
  use(session, {7, 65, 8}, Direction::Up);
  assert(world.getBlock({7, 65, 8}).get("open") == "false" &&
         world.getBlock({7, 65, 8}).get("powered") == "false");
  world.rawSetBlock({7, 65, 8}, airState());
  creative(session, data, 0, "oak_fence_gate");
  select(session, 0);
  use(session, {6, 64, 8}, Direction::Up);
  assert(world.getBlock({6, 65, 8}).is(Blocks::OAK_FENCE_GATE) &&
         world.getBlock({6, 65, 8}).get("facing") == "south");
  world.rawSetBlock({6, 65, 8}, airState());
  drain(session, true);
  select(session, 5);
  use(session, {6, 64, 6}, Direction::Up);
  assert(world.getBlock({6, 65, 6}).name() == "minecraft:observer");
  assert(!world.hasScheduledTick({6, 65, 6}, "minecraft:observer"));
  world.setBlock({6, 65, 7}, defaultState("glass"));
  assert(world.hasScheduledTick({6, 65, 6}, "minecraft:observer"));
  drain(session, true);
  // Unsupported rails/carpet fail before mutation, producing no free drops.
  world.rawSetBlock({7, 66, 8}, defaultState("smooth_stone"));
  for (const auto *name : {"rail", "powered_rail", "white_carpet"}) {
    creative(session, data, 0, name);
    select(session, 0);
    const auto count = world.entities.size();
    use(session, {7, 66, 8}, Direction::West);
    assert(isAirState(world.getBlock({6, 66, 8})) &&
           world.entities.size() == count);
    drain(session, true);
  }
  creative(session, data, 0, "glass");
  // The player's body blocks full-cube placement, but the block underfoot is
  // legal. Item entities don't block building; minecarts do.
  use(session, {8, 66, 8}, Direction::Up);
  assert(isAirState(world.getBlock({8, 66, 8})));
  world.spawnMinecart({10.5, 65, 8.5});
  use(session, {10, 64, 8}, Direction::Up);
  assert(isAirState(world.getBlock({10, 65, 8})));
  world.spawnItem({11, 65, 8}, "white_carpet");
  use(session, {11, 64, 8}, Direction::Up);
  assert(world.getBlock({11, 65, 8}).name() == "minecraft:glass");
  drain(session, true);
  // Right-click tunes a note block; shift bypasses use and permits placement.
  world.rawSetBlock({7, 65, 9}, defaultState("note_block"));
  use(session, {7, 65, 9}, Direction::Up);
  assert(world.getBlock({7, 65, 9}).get("note") == "1");
  Writer input;
  input.u8(32);
  session.receive(frame(43, input.data));
  use(session, {7, 65, 9}, Direction::Up);
  assert(world.getBlock({7, 66, 9}).name() == "minecraft:glass");
  drain(session, true);
  // Negative sequence must be rejected before it can destroy a block.
  Session invalid(world, data, options);
  join(invalid, data);
  Writer action;
  action.varInt(0);
  action.position({11, 65, 8});
  action.u8(1);
  action.varInt(-1);
  invalid.receive(frame(41, action.data));
  assert(invalid.closed());
  assert(world.getBlock({11, 65, 8}).name() == "minecraft:glass");
}
} // namespace
int main(int argc, char **argv) {
  assert(argc == 2);
  ProtocolData data(argv[1]);
  viewDistanceTen(data);
  chunkViewAndTiming(data);
  fallingLandingPacketOrder(data);
  tntPacketOrder(data);
  sectionPacketOrder(data);
  entityBroadcastPhase(data);
  sessionOutputLimit(data);
  placementRules(data);
  {
    const Uuid expected{0xb5, 0x0a, 0xd3, 0x85, 0x82, 0x9d, 0x31, 0x41,
                        0xa2, 0x16, 0x7e, 0x7d, 0x75, 0x39, 0xba, 0x7f};
    // Mojang offline UUID for Notch, independently checked against Java.
    assert(offlineUuid("Notch") == expected);
  }
  World world;
  world.loadChunk(0, 0);
  for (int x = 0; x < 16; ++x)
    for (int z = 0; z < 16; ++z)
      world.rawSetBlock({x, 64, z}, defaultState("smooth_stone"));
  ServerOptions options;
  options.chunks = {{0, 0}};
  Session session(world, data, options);
  world.blockUpdateObserver = [&](Pos p, const BlockState &s, int) {
    session.blockUpdate(p, s);
  };
  world.blockEventObserver = [&](const BlockEvent &e) {
    session.blockEvent(e);
  };
  world.blockUpdatesBroadcastObserver = [&] { session.flushBlockUpdates(); };
  world.entityTrackingBroadcastObserver = [&] { session.broadcastEntities(); };
  join(session, data);
  assert(session.inventory()[1].id == "minecraft:sticky_piston");
  use(session, {8, 64, 8}, Direction::Up);
  assert(world.getBlock({8, 65, 8}).name() == "minecraft:smooth_stone");
  auto ids = drain(session, true);
  assert(ids.back() == 4 && ids.front() == 8);
  Writer out;
  out.varInt(0);
  out.position({8, 65, 8});
  out.u8(1);
  out.varInt(2);
  session.receive(frame(41, out.data));
  assert(world.getBlock({8, 65, 8}).name() == "minecraft:air");
  drain(session, true);
  // Far-away or unloaded interactions cannot create chunks or modify blocks.
  use(session, {100, 64, 100}, Direction::Up, 3);
  assert(!world.isChunkLoaded(6, 6));
  drain(session, true);
  // Creative inventory, orientation, wall coral and support updates.
  select(session, 1);
  use(session, {8, 64, 8}, Direction::Up, 4);
  assert(world.getBlock({8, 65, 8}).get("facing") == "north");
  drain(session, true);
  select(session, 6);
  use(session, {8, 65, 8}, Direction::East, 5);
  assert(world.getBlock({9, 65, 8}).name() ==
         "minecraft:dead_tube_coral_wall_fan");
  assert(world.getBlock({9, 65, 8}).get("facing") == "east");
  drain(session, true);
  creative(session, data, 2, "glass");
  select(session, 2);
  use(session, {10, 64, 8}, Direction::Up, 6);
  assert(world.getBlock({10, 65, 8}).name() == "minecraft:glass");
  drain(session, true);
  // Unsupported creative blocks are explicitly rejected and corrected.
  creative(session, data, 2, "repeater");
  assert(session.inventory()[2].empty());
  drain(session, true);
  // Piston event is delivered after its server callbacks, then entity/BE
  // motion continues in World::tick; protocol never reorders core work.
  world.setBlock({8, 65, 7}, defaultState("slime_block"));
  world.setBlock({7, 65, 8}, defaultState("redstone_block"));
  world.tick();
  session.tick();
  ids = drain(session, true);
  auto event = std::find(ids.begin(), ids.end(), 7);
  assert(event != ids.end());
  const int sectionUpdate = data.packet("play", "clientbound", "section_blocks_update");
  const auto isBlockUpdate = [&](int id) { return id == 8 || id == sectionUpdate; };
  // Event must reach the client while its original push structure exists.
  // Event-induced source removals are dirty until the next chunk broadcast.
  assert(std::any_of(ids.begin(), event, isBlockUpdate));
  assert(std::none_of(event + 1, ids.end(), isBlockUpdate));
  assert(!world.pistonEntities.empty());
  for (int i = 0; i < 4; ++i) {
    world.tick();
    session.tick();
    ids = drain(session, true);
    if (i == 0)
      assert(std::any_of(ids.begin(), ids.end(), isBlockUpdate));
  }
  world.spawnItem({9, 67, 9}, "white_carpet");
  world.spawnPrimedTnt({10.5, 67, 10.5}, 80);
  world.spawnMinecart({11.5, 67, 11.5});
  world.spawnFallingBlock({12.5, 67, 12.5}, defaultState("smooth_stone"));
  session.broadcastEntities();
  ids = drain(session, true);
  assert(std::count(ids.begin(), ids.end(), 1) == 4);
  world.tick();
  session.tick();
  ids = drain(session, true);
  // ServerEntity delta tracking: moving entities send move_entity_pos(_rot)
  // against the VecDeltaCodec base instead of a full teleport every tick.
  const int movePos = data.packet("play", "clientbound", "move_entity_pos");
  const int movePosRot =
      data.packet("play", "clientbound", "move_entity_pos_rot");
  assert(std::count(ids.begin(), ids.end(), movePos) +
             std::count(ids.begin(), ids.end(), movePosRot) ==
         4);
  assert(std::count(ids.begin(), ids.end(),
                    data.packet("play", "clientbound", "teleport_entity")) ==
         0);
  world.entities.front()->discard();
  session.broadcastEntities();
  ids = drain(session, true);
  assert(std::find(ids.begin(), ids.end(), 77) != ids.end());
  // Initial chunk snapshot includes a moving piston block entity too.
  world.rawSetBlock({13, 65, 13},
                    defaultState("moving_piston").with("facing", "east"));
  auto moving = std::make_shared<PistonMovingBlockEntity>();
  moving->pos = {13, 65, 13};
  moving->movedState = defaultState("slime_block");
  moving->direction = Direction::East;
  moving->extending = true;
  moving->source = false;
  world.setPistonEntity(moving->pos, moving);
  Session later(world, data, options);
  join(later, data);
  {
    Session status(world, data, options);
    hello(status, 1, 123);
    status.receive(frame(0, {}));
    assert(drain(status) == std::vector<int>{0});
    out = {};
    out.i64(123456789);
    status.receive(frame(1, out.data));
    assert(status.closed());
    auto bytes = status.takeOutput();
    auto body = popFrame(bytes[0]);
    Reader r(*body);
    assert(r.varInt() == 1 && r.i64() == 123456789 && r.empty());
  }
  {
    Session bad(world, data, options);
    hello(bad, 2, 774);
    assert(bad.closed());
    assert(drain(bad) == std::vector<int>{0});
    Session malformed(world, data, options);
    const Bytes invalid{0xff, 0xff, 0xff, 0x7f};
    malformed.receive(invalid);
    assert(malformed.closed());
  }
  {
    Session early(world, data, options);
    hello(early);
    out = {};
    out.string("X");
    out.raw(Uuid{});
    early.receive(frame(0, out.data));
    drain(early);
    early.receive(frame(3, {}));
    drain(early);
    early.receive(frame(3, {}));
    assert(early.closed());
  }
  {
    // Repeated writes coalesce per section and encode the latest state at
    // broadcast time, including on a subsequent broadcast with reused storage.
    const Pos a{5, 65, 10}, b{6, 65, 10};
    for (int pass = 0; pass < 2; ++pass) {
      session.blockUpdate(b, defaultState("smooth_stone"));
      session.blockUpdate(a, defaultState("smooth_stone"));
      session.blockUpdate(b, defaultState("smooth_stone"));
      world.rawSetBlock(a, defaultState("smooth_stone"));
      world.rawSetBlock(b, defaultState("glass"));
      session.flushBlockUpdates();
      auto packets = session.takeOutput();
      assert(packets.size() == 1);
      auto body = popFrame(packets[0]);
      assert(body && packets[0].empty());
      Reader in(*body);
      assert(in.varInt() == data.packet("play", "clientbound", "section_blocks_update"));
      (void)in.i64();
      assert(in.varInt() == 2);
      const auto first = in.varLong(), second = in.varLong();
      assert((first & 0xfff) == ((5 << 8) | (10 << 4) | 1));
      assert((first >> 12) == data.registry.stateId(defaultState("smooth_stone")));
      assert((second & 0xfff) == ((6 << 8) | (10 << 4) | 1));
      assert((second >> 12) == data.registry.stateId(defaultState("glass")));
      assert(in.empty());
      allPlay.push_back(std::move(*body));
    }
  }
  {
    // A dirty position records no stale state. Multiple changes coalesce;
    // sendBlockUpdated must not prematurely replace the client's piston input.
    world.setBlock({6, 65, 10}, defaultState("smooth_stone"), 2);
    world.setBlock({6, 65, 10}, defaultState("glass"), 2);
    drain(session, true);
    session.flushBlockUpdates();
    auto packets = session.takeOutput();
    int found = 0;
    for (auto &bytes : packets) {
      auto body = popFrame(bytes);
      Reader in(*body);
      int id = in.varInt();
      if (id == 8 && in.position() == Pos{6, 65, 10}) {
        assert(in.varInt() == data.registry.stateId(defaultState("glass")));
        ++found;
      }
      allPlay.push_back(std::move(*body));
    }
    assert(found == 1);
    // A failed network mapping disconnects that connection without skipping
    // World::tick's remaining events/entity/block-entity stages.
    world.setBlock({6, 65, 11}, BlockState("minecraft:unmapped_test_state"),
                   2);
    bool completed = false;
    world.blockEntityTicker = [&](World &) { completed = true; };
    world.tick();
    assert(completed && session.closed());
  }
  if (const char *path = std::getenv("REDSTONE_PLAY_FIXTURE")) {
    std::ofstream file(path, std::ios::binary);
    for (const auto &body : allPlay) {
      Writer record;
      record.i32(static_cast<int>(body.size()));
      record.raw(body);
      file.write(reinterpret_cast<const char *>(record.data.data()),
                 record.data.size());
    }
    assert(file.good());
  }
  std::cout << "protocol state, interaction, events and entity tests passed\n";
}
