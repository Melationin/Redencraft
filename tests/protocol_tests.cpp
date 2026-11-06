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
  ids = drain(session, true);
  assert(std::count(ids.begin(), ids.end(), 45) == 2);
  assert(std::count(ids.begin(), ids.end(),
                    data.packet("play", "clientbound", "add_entity")) == 1);
  assert(world.isChunkLoaded(0, 0) && world.isChunkLoaded(12, 0));
  const auto tick = world.gameTime();
  session.sendTickTime(12.34567, 123);
  auto packets = session.takeOutput();
  assert(packets.size() == 1);
  auto body = popFrame(packets[0]);
  Reader timing(*body);
  assert(timing.varInt() == data.packet("play", "clientbound", "system_chat"));
  assert(timing.u8() == 8);
  const auto bytes = timing.raw(timing.i16());
  assert(std::string(bytes.begin(), bytes.end()) == "Tick 123: 12.346 ms");
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
  world.fallingBlockLandObserver = [&](int id, Pos p, const BlockState &state) {
    session.fallingBlockLanded(id, p, state);
  };
  world.tick();
  session.tick();
  const auto ids = drain(session, true);
  const auto block =
      std::find(ids.begin(), ids.end(),
                data.packet("play", "clientbound", "block_update"));
  const auto removed =
      std::find(ids.begin(), ids.end(),
                data.packet("play", "clientbound", "remove_entities"));
  assert(block != ids.end() && removed != ids.end() && block < removed);
  assert(world.getBlock({8, 65, 8}).name() == "minecraft:sand");
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
  chunkViewAndTiming(data);
  fallingLandingPacketOrder(data);
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
  // Event must reach the client while its original push structure exists.
  // Event-induced source removals are dirty until the next chunk broadcast.
  assert(std::find(event + 1, ids.end(), 8) == ids.end());
  assert(!world.pistonEntities.empty());
  for (int i = 0; i < 4; ++i) {
    world.tick();
    session.tick();
    drain(session, true);
  }
  world.spawnItem({9, 67, 9}, "white_carpet");
  world.spawnPrimedTnt({10.5, 67, 10.5}, 80);
  world.spawnMinecart({11.5, 67, 11.5});
  world.spawnFallingBlock({12.5, 67, 12.5}, defaultState("smooth_stone"));
  session.tick();
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
  session.tick();
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
    world.setBlock({6, 65, 11}, defaultState("glass").with("invalid", "true"),
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
