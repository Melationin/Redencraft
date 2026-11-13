#include "redstone/command.hpp"
#include "redstone/net/session.hpp"
#include "redstone/storage.hpp"
#include <array>
#include <chrono>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace redstone;
using namespace redstone::net;

namespace {
struct ClockWire {
  int id = -1;
  std::int64_t totalTicks = 0;
  float partialTick = 0;
  float rate = -1;
  bool operator==(const ClockWire &) const = default;
};

struct TimeWire {
  std::int64_t gameTime = 0;
  std::vector<ClockWire> clocks;
};

std::vector<Bytes> frames(Session &session) {
  return session.takeOutput();
}

int packetId(const Bytes &frameBytes) {
  Bytes mutableFrame = frameBytes;
  auto body = popFrame(mutableFrame);
  assert(body);
  Reader in(*body);
  return in.varInt();
}

void hello(Session &session) {
  Writer out;
  out.varInt(775);
  out.string("localhost");
  out.i16(25565);
  out.varInt(2);
  session.receive(frame(0, out.data));
}

std::vector<Bytes> join(Session &session, const ProtocolData &data) {
  hello(session);
  Writer out;
  out.string("WorldClockTester");
  out.raw(Uuid{});
  session.receive(frame(0, out.data));
  assert(session.phase() == Phase::Login);
  auto packets = frames(session);
  assert(packets.size() == 1 && packetId(packets[0]) == 2);
  session.receive(frame(3, {}));
  assert(session.phase() == Phase::Configuration);
  packets = frames(session);
  assert(packets.size() == 1 && packetId(packets[0]) == 14);
  out = {};
  out.varInt(0);
  session.receive(frame(7, out.data));
  packets = frames(session);
  assert(packets.size() == data.configuration.size() + 1);
  assert(packetId(packets.back()) == 3);
  session.receive(frame(3, {}));
  assert(session.phase() == Phase::Play);
  packets = frames(session);
  assert(!packets.empty());
  assert(packetId(packets.front()) == 49);
  return packets;
}

TimeWire findTime(const std::vector<Bytes> &packets, int expectedId) {
  for (const Bytes &framed : packets) {
    Bytes mutableFrame = framed;
    auto body = popFrame(mutableFrame);
    assert(body);
    Reader in(*body);
    if (in.varInt() != expectedId)
      continue;
    TimeWire value;
    value.gameTime = in.i64();
    const int count = in.varInt();
    assert(count >= 0 && count <= 2);
    for (int i = 0; i < count; ++i) {
      ClockWire clock;
      clock.id = in.varInt();
      clock.totalTicks = in.varLong();
      clock.partialTick = in.f32();
      clock.rate = in.f32();
      value.clocks.push_back(clock);
    }
    assert(in.empty());
    return value;
  }
  assert(false && "set_time frame missing");
  return {};
}

std::vector<Bytes> advanceTwenty(Session &session) {
  std::vector<Bytes> output;
  for (int i = 0; i < 20; ++i) {
    session.tick();
    auto part = frames(session);
    output.insert(output.end(), std::make_move_iterator(part.begin()),
                  std::make_move_iterator(part.end()));
  }
  return output;
}
void appendU32(Bytes &out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}
void skipRegistryEntry(Reader &in) {
  const int nameBytes = in.varInt();
  (void)in.raw(static_cast<std::size_t>(nameBytes));
  assert(in.bool_());
  assert(in.u8() == 10); // TAG_Compound
  assert(in.u8() == 0);  // empty WorldClock.DIRECT_CODEC payload
}
Bytes reversedWorldClockConfiguration(const std::filesystem::path &assets) {
  std::ifstream input(assets / "configuration.bin", std::ios::binary);
  assert(input);
  Bytes bytes((std::istreambuf_iterator<char>(input)), {});
  Reader reader(bytes);
  Bytes output;
  bool found = false;
  while (!reader.empty()) {
    const int length = reader.i32();
    assert(length > 0);
    auto payload = reader.raw(static_cast<std::size_t>(length));
    Bytes body(payload.begin(), payload.end());
    Reader scan(body);
    const int id = scan.varInt();
    if (id == 7 && !found) {
      const std::string registry = scan.string();
      if (registry == "minecraft:world_clock") {
        assert(scan.varInt() == 2);
        const std::size_t firstStart = scan.pos;
        skipRegistryEntry(scan);
        const std::size_t secondStart = scan.pos;
        skipRegistryEntry(scan);
        const std::size_t end = scan.pos;
        Bytes swapped(body.begin(), body.begin() + firstStart);
        swapped.insert(swapped.end(), body.begin() + secondStart,
                       body.begin() + end);
        swapped.insert(swapped.end(), body.begin() + firstStart,
                       body.begin() + secondStart);
        swapped.insert(swapped.end(), body.begin() + end, body.end());
        body = std::move(swapped);
        found = true;
      }
    }
    appendU32(output, static_cast<std::uint32_t>(body.size()));
    output.insert(output.end(), body.begin(), body.end());
  }
  assert(found);
  return output;
}
Bytes markerOutsideWorldClockRegistry() {
  Writer packet;
  packet.varInt(7); // official configuration/clientbound/registry_data ID
  packet.string("minecraft:example_registry");
  packet.varInt(1);
  packet.string("minecraft:world_clock");
  packet.bool_(false);
  Bytes output;
  appendU32(output, static_cast<std::uint32_t>(packet.data.size()));
  output.insert(output.end(), packet.data.begin(), packet.data.end());
  return output;
}
std::filesystem::path assetDirectoryWithConfiguration(
    const std::filesystem::path &assets, const Bytes &configuration) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("redstone-world-clock-protocol-" + std::to_string(stamp));
  std::filesystem::create_directories(directory);
  for (const auto &entry : std::filesystem::directory_iterator(assets))
    if (entry.is_regular_file())
      std::filesystem::copy_file(
          entry.path(), directory / entry.path().filename(),
          std::filesystem::copy_options::overwrite_existing);
  std::ofstream output(directory / "configuration.bin", std::ios::binary);
  output.write(reinterpret_cast<const char *>(configuration.data()),
               static_cast<std::streamsize>(configuration.size()));
  assert(output);
  return directory;
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  ProtocolData data(argv[1]);
  assert(data.worldClockIds[0] == 0);
  assert(data.worldClockIds[1] == 1);

  World world;
  world.loadChunk(0, 0);
  world.setGameTime(5000);
  WorldClocks clocks;
  WorldClockState overworld;
  overworld.totalTicks = 90;
  overworld.partialTick = 0.75f;
  overworld.rate = 1.25f;
  clocks.setClock(BuiltinWorldClock::Overworld, overworld);
  WorldClockState end;
  end.totalTicks = 40;
  end.partialTick = 0.25f;
  end.rate = 2.0f;
  end.paused = true;
  clocks.setClock(BuiltinWorldClock::TheEnd, end);
  ServerOptions options;
  options.chunks = {{0, 0}};
  options.commandPermissionLevel = 2;
  Session session(world, data, options);
  command::Dispatcher timeCommands;
  command::registerTimeCommand(timeCommands);
  command::Context timeContext;
  timeContext.world = &world;
  timeContext.worldClocks = &clocks;
  timeContext.worldClockChanged = [&](BuiltinWorldClock clock) {
    session.sendWorldClockUpdate(clock);
  };
  session.attachCommands(&timeCommands, &timeContext);

  session.setWorldClockSource(clocks, true);

  const int setId = data.packet("play", "clientbound", "set_time");
  auto joined = join(session, data);
  auto initial = findTime(joined, setId);
  assert(initial.gameTime == 5000);
  assert(initial.clocks.size() == 2);
  assert(initial.clocks[0].id == 0 && initial.clocks[0].totalTicks == 90);
  assert(initial.clocks[0].partialTick == 0.75f);
  assert(initial.clocks[0].rate == 1.25f);
  assert(initial.clocks[1].id == 1 && initial.clocks[1].totalTicks == 40);
  assert(initial.clocks[1].partialTick == 0.25f);
  assert(initial.clocks[1].rate == 0.0f);

  world.setGameTime(6000);
  auto periodicPackets = advanceTwenty(session);
  auto periodic = findTime(periodicPackets, setId);
  assert(periodic.gameTime == 6000);
  assert(periodic.clocks == initial.clocks);

  session.setWorldClockSource(clocks, false);
  world.setGameTime(7000);
  auto noAdvancePackets = advanceTwenty(session);
  auto noAdvance = findTime(noAdvancePackets, setId);
  assert(noAdvance.gameTime == 7000);
  assert(noAdvance.clocks.size() == 2);
  assert(noAdvance.clocks[0].totalTicks == 90 &&
         noAdvance.clocks[0].partialTick == 0.75f &&
         noAdvance.clocks[0].rate == 0.0f);
  assert(noAdvance.clocks[1].totalTicks == 40 &&
         noAdvance.clocks[1].partialTick == 0.25f &&
         noAdvance.clocks[1].rate == 0.0f);

  Writer command;
  command.string("time set 1.5s");
  session.receive(frame(
      data.packet("play", "serverbound", "chat_command"), command.data));
  auto timeUpdatePackets = frames(session);
  auto timeUpdate = findTime(timeUpdatePackets, setId);
  assert(timeUpdate.gameTime == 7000);
  assert(timeUpdate.clocks.size() == 1);
  assert(timeUpdate.clocks[0].id == 0);
  assert(timeUpdate.clocks[0].totalTicks == 30);
  assert(timeUpdate.clocks[0].partialTick == 0.0f);
  assert(timeUpdate.clocks[0].rate == 0.0f); // advance_time remains false.

  Writer markerCommand;
  markerCommand.string("time set noon");
  session.receive(frame(
      data.packet("play", "serverbound", "chat_command"),
      markerCommand.data));
  auto markerUpdate = findTime(frames(session), setId);
  assert(markerUpdate.clocks.size() == 1);
  assert(markerUpdate.clocks[0].id == 0);
  assert(markerUpdate.clocks[0].totalTicks == 6000);
  assert(markerUpdate.clocks[0].partialTick == 0.0f);

  // The same tree and context at a non-operator level rejects /time without
  // changing the clock or emitting a set_time update.
  ServerOptions nonopOptions = options;
  nonopOptions.commandPermissionLevel = 1;
  Session nonop(world, data, nonopOptions);
  nonop.attachCommands(&timeCommands, &timeContext);
  (void)join(nonop, data);
  const auto beforeDenied = clocks.state(BuiltinWorldClock::Overworld);
  Writer denied;
  denied.string("time set 2d");
  nonop.receive(frame(
      data.packet("play", "serverbound", "chat_command"), denied.data));
  auto deniedPackets = frames(nonop);
  bool deniedSetTime = false;
  for (const Bytes &packetBytes : deniedPackets)
    if (packetId(packetBytes) == setId)
      deniedSetTime = true;
  assert(!deniedSetTime);
  assert(clocks.state(BuiltinWorldClock::Overworld).totalTicks ==
         beforeDenied.totalTicks);
  assert(clocks.state(BuiltinWorldClock::Overworld).partialTick ==
         beforeDenied.partialTick);

  World legacyWorld;
  legacyWorld.loadChunk(0, 0);
  legacyWorld.setGameTime(8000);
  Session legacy(legacyWorld, data, options);
  auto legacyPackets = join(legacy, data);
  auto legacyTime = findTime(legacyPackets, setId);
  assert(legacyTime.gameTime == 8000);
  assert(legacyTime.clocks.empty());

  // Reorder the two entries in the real official registry_data frame. Holder
  // IDs must follow this on-wire order even when registry_ids.tsv disagrees.
  const auto reversedDirectory = assetDirectoryWithConfiguration(
      argv[1], reversedWorldClockConfiguration(argv[1]));
  ProtocolData reversedData(reversedDirectory);
  assert(reversedData.worldClockIds[0] == 1);
  assert(reversedData.worldClockIds[1] == 0);
  World reversedWorld;
  reversedWorld.loadChunk(0, 0);
  reversedWorld.setGameTime(9000);
  Session reversedSession(reversedWorld, reversedData, options);
  reversedSession.setWorldClockSource(clocks, true);
  auto reversedPackets = join(reversedSession, reversedData);
  auto reversedTime = findTime(reversedPackets, setId);
  assert(reversedTime.clocks.size() == 2);
  assert(reversedTime.clocks[0].id == 1); // overworld follows frame position 1.
  assert(reversedTime.clocks[1].id == 0); // the_end follows frame position 0.
  std::filesystem::remove_all(reversedDirectory);

  // A marker string in another registry's payload cannot satisfy the lookup:
  // only a parsed registry_data frame whose registry key is world_clock does.
  const auto markerDirectory = assetDirectoryWithConfiguration(
      argv[1], markerOutsideWorldClockRegistry());
  bool rejectedMarker = false;
  try {
    ProtocolData invalidData(markerDirectory);
    (void)invalidData;
  } catch (const ProtocolError &) {
    rejectedMarker = true;
  }
  assert(rejectedMarker);
  std::filesystem::remove_all(markerDirectory);

  std::cout << "world clock protocol tests passed ids="
            << data.worldClockIds[0] << ',' << data.worldClockIds[1]
            << " reversed=" << reversedData.worldClockIds[0] << ','
            << reversedData.worldClockIds[1]
            << " marker_only_rejected=1 time_command_update=1 time_marker_update=1\n";
  return 0;
}
