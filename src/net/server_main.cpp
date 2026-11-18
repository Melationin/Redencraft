#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "redstone/command.hpp"
#include "redstone/dimension.hpp"
#include "redstone/entity.hpp"
#include "redstone/litematic.hpp"
#include "redstone/net/session.hpp"
#include "redstone/net/outbox.hpp"
#include "redstone/net/tick_timing.hpp"
#include "redstone/server_worlds.hpp"
#include "redstone/storage.hpp"
#include "redstone/tickets.hpp"
#include "redstone/world_chunk_manager.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <tuple>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

using namespace redstone;
using namespace redstone::net;
namespace {
volatile std::sig_atomic_t stopRequested = 0;
void stop(int) { stopRequested = 1; }

// English strings for the keys /tick emits, copied from the 26.1.2 client's
// assets/minecraft/lang/en_us.json. A connected client resolves the same keys
// itself; this table is only for the server console, which has no language
// files.
const char *consoleText(const std::string &key) {
  static const std::map<std::string, const char *> table{
      {"commands.tick.query.percentiles",
       "Percentiles: P50: %sms P95: %sms P99: %sms. Sample: %s"},
      {"commands.tick.query.rate.running",
       "Target tick rate: %s per second.\nAverage time per tick: %sms "
       "(Target: %sms)"},
      {"commands.tick.query.rate.sprinting",
       "Target tick rate: %s per second (ignored, reference only).\nAverage "
       "time per tick: %sms"},
      {"commands.tick.rate.success", "Set the target tick rate to %s per second"},
      {"commands.tick.sprint.report",
       "Sprint completed with %s ticks per second, or %s ms per tick"},
      {"commands.tick.sprint.stop.fail", "No tick sprint in progress"},
      {"commands.tick.sprint.stop.success",
       "Interrupted the current tick sprint"},
      {"commands.tick.status.frozen", "The game is frozen"},
      {"commands.tick.status.lagging",
       "The game is running, but can't keep up with the target tick rate"},
      {"commands.tick.status.running", "The game is running normally"},
      {"commands.tick.status.sprinting", "The game is sprinting"},
      {"commands.tick.step.fail",
       "Unable to step the game - the game must be frozen first"},
      {"commands.tick.step.stop.fail", "No tick step in progress"},
      {"commands.tick.step.stop.success",
       "Interrupted the current tick step"},
      {"commands.tick.step.success", "Stepping %s tick(s)"},
      {"commands.time.pause", "Paused clock %s"},
      {"commands.time.query.absolute", "Clock %s is at %s tick(s)"},
      {"commands.time.query.gametime", "The game time is %s tick(s)"},
      {"commands.time.query.timeline", "Timeline %s is at %s tick(s)"},
      {"commands.time.query.timeline.repetitions",
       "Timeline %s has passed %s repetition(s)"},
      {"commands.time.rate", "Clock %s will now advance at %sx normal rate"},
      {"commands.time.resume", "Resumed clock %s"},
      {"commands.time.set.absolute", "Set %s to %s tick(s)"},
      {"commands.time.set.time_marker", "Set %s to time marker %s"},
      {"commands.time.no_time_marker_found",
       "Time marker %s does not exist for clock %s"},
      {"commands.time.wrong_timeline_for_clock",
       "Timeline %s is not valid for clock %s"},
  };
  const auto it = table.find(key);
  return it == table.end() ? nullptr : it->second;
}
// Component.translatable rendering for the console: substitute the arguments for
// "%s" in order, like the client's formatter does.
std::string formatConsole(const command::Message &message) {
  const char *format = consoleText(message.key);
  if (!format)
    return message.key + (message.args.empty() ? "" : " " + message.args.front());
  std::string out = format;
  for (const auto &argument : message.args) {
    const auto at = out.find("%s");
    if (at == std::string::npos)
      break;
    out.replace(at, 2, argument);
  }
  return out;
}

// Reads complete lines off the server stdin. The reader runs on its own thread
// so a blocking read never stalls the tick loop; the loop drains the queue.
class ConsoleInput {
public:
  explicit ConsoleInput(bool enabled = true) {
#ifndef _WIN32
    if (enabled) {
      if (::pipe(wakePipe_) != 0)
        wakePipe_[0] = wakePipe_[1] = -1;
    }
#endif
    if (enabled)
      worker_ = std::thread([this] { run(); });
  }
  ~ConsoleInput() {
    done_ = true;
#ifndef _WIN32
    if (wakePipe_[1] >= 0) {
      const char wake = 1;
      while (::write(wakePipe_[1], &wake, 1) < 0 && errno == EINTR) {
      }
    }
#endif
    if (worker_.joinable())
      worker_.join();
#ifndef _WIN32
    if (wakePipe_[0] >= 0)
      ::close(wakePipe_[0]);
    if (wakePipe_[1] >= 0)
      ::close(wakePipe_[1]);
#endif
  }
  // Returns false when no complete line is pending.
  bool next(std::string &line) {
    std::lock_guard lock(mutex_);
    if (pending_.empty())
      return false;
    line = std::move(pending_.front());
    pending_.pop_front();
    return true;
  }

private:
  void pushLine(std::string &buffer) {
    if (buffer.empty())
      return;
    {
      std::lock_guard lock(mutex_);
      pending_.push_back(std::move(buffer));
    }
    buffer.clear();
  }
  void run() {
    std::string buffer;
#ifndef _WIN32
    if (wakePipe_[0] >= 0) {
      while (!done_) {
        struct pollfd fds[2]{
            {STDIN_FILENO, POLLIN, 0},
            {wakePipe_[0], POLLIN, 0},
        };
        const int ready = ::poll(fds, 2, -1);
        if (ready < 0) {
          if (errno == EINTR)
            continue;
          break;
        }
        if (done_ || (fds[1].revents & (POLLIN | POLLERR | POLLHUP)))
          break;
        if (fds[0].revents & (POLLIN | POLLHUP)) {
          char ch = 0;
          const ssize_t n = ::read(STDIN_FILENO, &ch, 1);
          if (n == 0)
            break;
          if (n < 0) {
            if (errno == EINTR)
              continue;
            break;
          }
          if (ch != '\n' && ch != '\r') {
            buffer.push_back(ch);
            continue;
          }
          pushLine(buffer);
          continue;
        }
        if (fds[0].revents & (POLLERR | POLLNVAL))
          break;
      }
      pushLine(buffer);
      return;
    }
#endif
    for (int c; !done_ && (c = std::fgetc(stdin)) != EOF;) {
      if (c != 10 && c != 13) {
        buffer.push_back(static_cast<char>(c));
        continue;
      }
      pushLine(buffer);
    }
    pushLine(buffer);
    // stdin hit EOF (a piped script or a closed console): the server keeps
    // running instead of treating it as a stop request.
  }
  std::mutex mutex_;
  std::deque<std::string> pending_;
  std::atomic<bool> done_{false};
#ifndef _WIN32
  int wakePipe_[2]{-1, -1};
#endif
  std::thread worker_;
};


#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle invalidSocket = INVALID_SOCKET;
void closeSocket(SocketHandle s) { closesocket(s); }
bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
void nonblocking(SocketHandle s) {
  u_long one = 1;
  if (ioctlsocket(s, FIONBIO, &one))
    throw ProtocolError("nonblocking socket failed");
}
struct SocketSystem {
  SocketSystem() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data))
      throw ProtocolError("WSAStartup failed");
  }
  ~SocketSystem() { WSACleanup(); }
};
#else
using SocketHandle = int;
constexpr SocketHandle invalidSocket = -1;
void closeSocket(SocketHandle s) { close(s); }
bool wouldBlock() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
void nonblocking(SocketHandle s) {
  if (fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) == -1)
    throw ProtocolError("nonblocking socket failed");
}
struct SocketSystem {};
#endif
class Socket {
public:
  explicit Socket(SocketHandle value = invalidSocket) : handle_(value) {}
  ~Socket() {
    if (handle_ != invalidSocket)
      closeSocket(handle_);
  }
  Socket(const Socket &) = delete;
  Socket &operator=(const Socket &) = delete;
  Socket(Socket &&other) noexcept
      : handle_(std::exchange(other.handle_, invalidSocket)) {}
  SocketHandle get() const { return handle_; }

private:
  SocketHandle handle_;
};
struct Connection {
  Socket socket;
  Session session;
  // One main-thread producer, one network-thread consumer. Session hands owned
  // payloads/snapshots over through NetworkWriter::submit; only the writer
  // touches encoding/pending buffers or compacts already-sent bytes.
  OutboundQueue<> outbox;
  OutboundQueue<>::Batch encoding;
  std::size_t nextPacket = 0;
  Bytes pending;
  std::size_t sent = 0;
  std::atomic<bool> dead{false};
  std::atomic<bool> closing{false};
  // Set by the writer once a closing connection's last byte was sent.
  std::atomic<bool> flushed{false};
  std::array<char, 256> diagnosticMessage{};
  std::atomic<bool> diagnosticReady{false};
  Connection(Socket accepted, World &world, const ProtocolData &data,
             ServerOptions options)
      : socket(std::move(accepted)), session(world, data, std::move(options)) {
    session.diagnostic = [this](std::string_view message) {
      // One bounded message slot: diagnostic IO must not stall a world callback.
      if (diagnosticReady.load(std::memory_order_acquire)) return;
      const auto length = std::min(message.size(), diagnosticMessage.size() - 1);
      std::copy_n(message.data(), length, diagnosticMessage.data());
      diagnosticMessage[length] = '\0';
      diagnosticReady.store(true, std::memory_order_release);
    };
  }
};

// Matches Connection.sendPacket's event-loop handoff: immutable work is queued
// in order without a producer mutex. Socket readiness/encoding never run on the
// world thread. A slow connection owns its own bounded queue.
class NetworkWriter {
public:
  ~NetworkWriter() { stop(); }
  void start() { thread_ = std::thread([this] { run(); }); }
  void stop() {
    stopping_.store(true, std::memory_order_release);
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
  }
  // Only connection acceptance uses this lock, outside the simulation tick.
  void add(const std::shared_ptr<Connection> &connection) {
    {
      std::lock_guard lock(mutex_);
      added_.push_back(connection);
    }
    wake_.notify_one();
  }
  void submit(const std::shared_ptr<Connection> &connection,
              std::vector<OutPacket> packets) noexcept {
    if (packets.empty() || connection->dead.load(std::memory_order_acquire)) return;
    if (!connection->outbox.tryPush(packets)) {
      if (!connection->dead.exchange(true, std::memory_order_acq_rel))
        ++backpressureDisconnects_;
      return;
    }
    wake_.notify_one();
  }
  std::size_t backpressureDisconnects() const noexcept { return backpressureDisconnects_; }
private:
  static void discard(Connection &connection) {
    if (!connection.encoding.packets.empty())
      connection.outbox.complete(connection.encoding);
    while (connection.outbox.tryPop(connection.encoding))
      connection.outbox.complete(connection.encoding);
    connection.nextPacket = 0;
  }
  void run() {
    std::vector<std::shared_ptr<Connection>> active, added;
    for (;;) {
      {
        std::lock_guard lock(mutex_);
        added.swap(added_);
      }
      for (auto &connection : added) active.push_back(std::move(connection));
      added.clear();
      if (stopping_.load(std::memory_order_acquire)) {
        for (auto &connection : active) discard(*connection);
        break;
      }
      for (auto &ptr : active) {
        auto &connection = *ptr;
        if (connection.diagnosticReady.load(std::memory_order_acquire)) {
          std::fprintf(stderr, "connection: %s\n", connection.diagnosticMessage.data());
          connection.diagnosticReady.store(false, std::memory_order_release);
        }
        if (connection.dead.load(std::memory_order_acquire)) {
          discard(connection);
          continue;
        }
        if (connection.sent == connection.pending.size()) {
          connection.pending.clear();
          connection.sent = 0;
        } else if (connection.sent >= 65536 &&
                   connection.sent >= connection.pending.size() / 2) {
          connection.pending.erase(connection.pending.begin(),
                                   connection.pending.begin() + connection.sent);
          connection.sent = 0;
        }
        // Netty-style writable watermark: retain ordered work rather than
        // repeatedly encoding more data for a socket that cannot drain.
        if (connection.pending.size() - connection.sent >= 1024 * 1024) continue;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        for (int count = 0; count < 64; ++count) {
          if (connection.encoding.packets.empty()) {
            if (!connection.outbox.tryPop(connection.encoding)) break;
            connection.nextPacket = 0;
          }
          auto &packet = connection.encoding.packets[connection.nextPacket];
          try {
            if (packet.stillValid && !packet.stillValid()) {
              if (++connection.nextPacket == connection.encoding.packets.size())
                connection.outbox.complete(connection.encoding);
              continue;
            }
            Bytes produced = packet.deferred ? packet.deferred()
                : packet.packetId >= 0 ? frame(packet.packetId, packet.bytes)
                                       : std::move(packet.bytes);
            if (produced.size() > maxQueuedOutputBytes ||
                connection.pending.size() - connection.sent > maxQueuedOutputBytes - produced.size()) {
              connection.dead.store(true, std::memory_order_release);
              break;
            }
            if (!produced.empty())
              connection.pending.insert(connection.pending.end(), produced.begin(), produced.end());
          } catch (...) {
            connection.dead.store(true, std::memory_order_release);
            break;
          }
          if (++connection.nextPacket == connection.encoding.packets.size())
            connection.outbox.complete(connection.encoding);
          if (connection.pending.size() - connection.sent >= 1024 * 1024 ||
              std::chrono::steady_clock::now() >= deadline) break;
        }
      }
      fd_set writes;
      FD_ZERO(&writes);
      SocketHandle largest = invalidSocket;
      bool waiting = false;
      for (const auto &connection : active) {
        if (connection->dead.load() || connection->sent == connection->pending.size()) continue;
        FD_SET(connection->socket.get(), &writes);
        largest = largest == invalidSocket ? connection->socket.get()
                  : std::max(largest, connection->socket.get());
        waiting = true;
      }
      if (waiting) {
        timeval timeout{};
        timeout.tv_usec = 2000;
        (void)select(static_cast<int>(largest + 1), nullptr, &writes, nullptr, &timeout);
      }
      for (auto &connection : active) {
        if (connection->dead.load() || connection->sent == connection->pending.size() ||
            !FD_ISSET(connection->socket.get(), &writes)) continue;
        const int count = static_cast<int>(::send(connection->socket.get(),
            reinterpret_cast<const char *>(connection->pending.data() + connection->sent),
            static_cast<int>(connection->pending.size() - connection->sent), 0));
        if (count > 0) connection->sent += static_cast<std::size_t>(count);
        else if (!wouldBlock()) connection->dead.store(true, std::memory_order_release);
      }
      std::erase_if(active, [](const auto &connection) {
        if (connection->dead.load()) {
          discard(*connection);
          return true;
        }
        if (connection->closing.load(std::memory_order_acquire) &&
            connection->sent == connection->pending.size() &&
            connection->encoding.packets.empty() && connection->outbox.empty()) {
          connection->flushed.store(true, std::memory_order_release);
          return true;
        }
        return false;
      });
      std::unique_lock lock(mutex_);
      wake_.wait_for(lock, std::chrono::milliseconds(2), [&] {
        if (stopping_.load(std::memory_order_acquire) || !added_.empty()) return true;
        return std::any_of(active.begin(), active.end(), [](const auto &connection) {
          return connection->dead.load() || connection->closing.load() ||
                 ((!connection->outbox.empty() || !connection->encoding.packets.empty()) &&
                  connection->pending.size() - connection->sent < 1024 * 1024);
        });
      });
    }
  }
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<std::shared_ptr<Connection>> added_;
  std::atomic<bool> stopping_{false};
  std::size_t backpressureDisconnects_ = 0; // world-thread only
  std::thread thread_;
};

int integer(const std::string &value, int lo, int hi) {
  std::size_t consumed;
  int n = std::stoi(value, &consumed);
  if (consumed != value.size() || n < lo || n > hi)
    throw ProtocolError("invalid numeric argument");
  return n;
}
std::filesystem::path utf8Path(const std::string &value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
class TicketedWorldLock {
public:
  explicit TicketedWorldLock(const std::filesystem::path &directory) {
#if defined(__linux__)
    const auto path = directory / "session.lock";
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd_ < 0)
      throw ProtocolError("cannot open session.lock: " + path.string());
    struct flock lock{};
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (::fcntl(fd_, F_SETLK, &lock) != 0) {
      ::close(fd_);
      fd_ = -1;
      throw ProtocolError("world is already locked: " + path.string());
    }
#else
    (void)directory;
    throw ProtocolError(
        "--ticketed-world write mode is currently supported only on Linux");
#endif
  }
  ~TicketedWorldLock() {
#if defined(__linux__)
    if (fd_ >= 0)
      ::close(fd_);
#endif
  }
  TicketedWorldLock(const TicketedWorldLock &) = delete;
  TicketedWorldLock &operator=(const TicketedWorldLock &) = delete;
private:
#if defined(__linux__)
  int fd_ = -1;
#endif
};

// These callbacks capture the NetworkWriter and connection list. Detach them
// before either dependency leaves scope on normal shutdown or exception unwind.
class NetworkTeardownGuard {
public:
  NetworkTeardownGuard() = default;
  void track(World *world, WorldChunkManager *manager) {
    if (world)
      worlds_.push_back(world);
    if (manager)
      managers_.push_back(manager);
  }
  ~NetworkTeardownGuard() { detach(); }
  NetworkTeardownGuard(const NetworkTeardownGuard &) = delete;
  NetworkTeardownGuard &operator=(const NetworkTeardownGuard &) = delete;
  void detach() noexcept {
    for (World *world : worlds_) {
      world->lightPublicationObserver = {};
      world->lightChunkLifecycleObserver = {};
      world->blockUpdateObserver = {};
      world->blockEventObserver = {};
      world->blockUpdatesBroadcastObserver = {};
      world->entityTrackingBroadcastObserver = {};
      world->entityAddedObserver = {};
      world->entityRemovedObserver = {};
      world->explosionObserver = {};
      world->fallingBlockLandObserver = {};
    }
    for (WorldChunkManager *manager : managers_) {
      manager->managedChunkInstalled = {};
      manager->managedChunkRemoved = {};
    }
  }
private:
  std::vector<World *> worlds_;
  std::vector<WorldChunkManager *> managers_;
};

class FixtureCreeper final : public LivingEntity {
public:
  FixtureCreeper() : LivingEntity("minecraft:creeper") {}
  bool hurtServer(World &world, const DamageSource &source,
                  float amount) override {
    if (source.kind == DamageKind::Cramming)
      return false;
    return LivingEntity::hurtServer(world, source, amount);
  }
};

} // namespace
int runServer(int argc, char **argv) {
  try {
    std::filesystem::path assets = REDSTONE_PROTOCOL_DIR, worldPath,
                          schematicPath, ticketedWorldPath, worldgenDirectory;
    bool worldgenDisabled = false;
    std::string requestedDimension;
    std::vector<std::filesystem::path> blockTagPacks;
    int port = 25565, maxTicks = 0, forceMargin = 2;
    int simulationDistance = 10;
    int entityCramCount = 100;
    bool explicitSpawn = false, explicitOffset = false;
    bool consoleEnabled = true, fluidFixture = false, entityFixture = false,
         singlePlatform = false;
    LitematicOptions schematicOptions;
    AnvilLoadOptions anvilOptions;
    ServerOptions options;
    options.spawnPlayerEntity = true;
    LightWorkerOptions lightOptions;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help") {
        std::cout
            << "redstone_vanilla_server [--port 25565] [--assets directory] "
               "[--world directory | --schematic file | "
               "--ticketed-world directory]\n"
               "  [--dimension namespace:path] [--chunk x,z ...] [--ticks N] "
               "[--spawn x,y,z] [--offset x,y,z] [--view-distance 3] "
               "[--force-margin 2]\n"
               "  [--allow-unsupported] [--skip-non-full-chunks]\n"
               "  [--fluid-basin] [--single-platform] [--no-console] "
               "[--light-workers 1] [--ticket-view-distance 3] "
               "[--simulation-distance 10] [--command-permission-level 0] "
               "[--data-pack path ...] [--worldgen directory | --no-worldgen]\n"
               "Offline --world/--schematic modes do not save. "
               "--ticketed-world opens the overworld region and session.lock "
               "for writes; missing chunks are generated only when a "
               "26.1 worldgen asset directory is available.\n";
        return 0;
      }
      if (arg == "--no-console") {
        consoleEnabled = false;
        continue;
      }
      if (arg == "--no-worldgen") {
        worldgenDisabled = true;
        continue;
      }
      if (arg == "--allow-unsupported") {
        // Preview only: unimplemented schematic blocks become smooth_stone and
        // are reported on stdout as SUBSTITUTED lines.
        schematicOptions.substituteUnsupported = true;
        continue;
      }
      if (arg == "--skip-non-full-chunks") {
        anvilOptions.skipNonFullChunks = true;
        continue;
      }
      if (arg == "--fluid-basin") {
        // Validation fixture for validation/protocol775/backpressure_test.py:
        // a set of sealed 1x1 shafts whose falling water touches several Y
        // sections. It is not a world generator and does not run in normal use.
        fluidFixture = true;
        continue;
      }
      if (arg == "--entity-fixture") {
        entityFixture = true;
        continue;
      }
      if (arg == "--entity-cram-count") {
        if (i + 1 >= argc)
          throw ProtocolError("argument needs a value");
        entityCramCount = integer(argv[++i], 1, 5000);
        entityFixture = true;
        continue;
      }
      if (arg == "--single-platform") {
        singlePlatform = true;
        continue;
      }
      if (i + 1 >= argc)
        throw ProtocolError("argument needs a value");
      const std::string value = argv[++i];
      if (arg == "--port")
        port = integer(value, 0, 65535);
      else if (arg == "--ticks")
        maxTicks = integer(value, 1, 10000000);
      else if (arg == "--assets")
        assets = utf8Path(value);
      else if (arg == "--data-pack")
        blockTagPacks.push_back(utf8Path(value));
      else if (arg == "--worldgen")
        worldgenDirectory = utf8Path(value);
      else if (arg == "--dimension")
        requestedDimension = normalizeDimensionId(value);
      else if (arg == "--world")
        worldPath = utf8Path(value);
      else if (arg == "--ticketed-world")
        ticketedWorldPath = utf8Path(value);
      else if (arg == "--schematic")
        schematicPath = utf8Path(value);
      else if (arg == "--view-distance")
        options.viewDistance = integer(value, 2, maxViewDistance);
      else if (arg == "--ticket-view-distance")
        options.ticketViewDistance = integer(value, 2, 32);
      else if (arg == "--simulation-distance")
        simulationDistance = integer(value, 2, 32);
      else if (arg == "--force-margin")
        forceMargin = integer(value, 0, 16);
      else if (arg == "--light-workers") {
        if (integer(value, 1, 2) != 1)
          throw ProtocolError("--light-workers only supports one worker");
      } else if (arg == "--command-permission-level")
        options.commandPermissionLevel = integer(value, 0, 4);
      else if (arg == "--offset") {
        std::istringstream in(value);
        char a, b;
        if (!(in >> schematicOptions.offset.x >> a >>
              schematicOptions.offset.y >> b >> schematicOptions.offset.z) ||
            a != ',' || b != ',' || !in.eof() ||
            std::abs(static_cast<long long>(schematicOptions.offset.x)) >
                29999984 ||
            std::abs(static_cast<long long>(schematicOptions.offset.z)) >
                29999984)
          throw ProtocolError(
              "offset must be integer x,y,z within world bounds");
        explicitOffset = true;
      } else if (arg == "--chunk") {
        const auto comma = value.find(',');
        if (comma == std::string::npos)
          throw ProtocolError("chunk must be x,z");
        options.chunks.emplace_back(
            integer(value.substr(0, comma), -1874999, 1874999),
            integer(value.substr(comma + 1), -1874999, 1874999));
      } else if (arg == "--spawn") {
        explicitSpawn = true;
        std::istringstream in(value);
        char a, b;
        if (!(in >> options.spawn.x >> a >> options.spawn.y >> b >>
              options.spawn.z) ||
            a != ',' || b != ',' || !in.eof() ||
            !std::isfinite(options.spawn.x) ||
            !std::isfinite(options.spawn.y) ||
            !std::isfinite(options.spawn.z) ||
            std::abs(options.spawn.x) > 29999984 ||
            std::abs(options.spawn.z) > 29999984 ||
            std::abs(options.spawn.y) > 20000000)
          throw ProtocolError("spawn must be finite x,y,z");
      } else
        throw ProtocolError("unknown argument " + arg);
    }
    const int sourceModes = int(!worldPath.empty()) +
                            int(!schematicPath.empty()) +
                            int(!ticketedWorldPath.empty());
    if (sourceModes > 1)
      throw ProtocolError(
          "choose exactly one of --world, --schematic, --ticketed-world");
    if (!ticketedWorldPath.empty()) {
      if (fluidFixture || explicitOffset || !options.chunks.empty())
        throw ProtocolError(
            "--ticketed-world is incompatible with --chunk, --offset and "
            "--fluid-basin");
    }
    if (schematicPath.empty() && explicitOffset)
      throw ProtocolError("--offset requires --schematic");
    if (anvilOptions.skipNonFullChunks && worldPath.empty())
      throw ProtocolError("--skip-non-full-chunks requires --world");
    if (worldgenDisabled)
      worldgenDirectory.clear();
    else if (worldgenDirectory.empty()) {
      const std::filesystem::path candidate =
          std::filesystem::path(assets).parent_path() / "worldgen26_1";
      if (std::filesystem::is_directory(candidate))
        worldgenDirectory = candidate;
    }
    ProtocolData data(assets);
    std::unique_ptr<World> standaloneWorld;
    std::optional<ServerWorldSet> worldSet;
    const auto explicitBlockTagPacks = blockTagPacks;
    auto selectPacks = [&](const std::filesystem::path &path,
                           const nbt::RootTag &root) {
      auto selection = selectWorldDataPacks(path, root);
      for (const auto &message : selection.diagnostics)
        std::cerr << "BLOCK_TAGS: " << message << std::endl;
      blockTagPacks = std::move(selection.paths);
      blockTagPacks.insert(blockTagPacks.end(), explicitBlockTagPacks.begin(),
                           explicitBlockTagPacks.end());
    };
    auto configureDimensionOptions = [&](const DimensionStem &stem,
                                         const std::vector<DimensionStem> &all) {
      options.dimensionId = stem.id;
      options.dimensionTypeId =
          stem.type.id.empty() ? stem.typeId : stem.type.id;
      options.seaLevel = (stem.id == "minecraft:the_nether")
                             ? 32
                             : ((stem.id == "minecraft:the_end") ? 0 : 63);
      options.availableDimensions.clear();
      for (const auto &entry : all)
        options.availableDimensions.push_back(entry.id);
    };
    auto createStandaloneWorld = [&](const DimensionTypeSpec &spec,
                                     std::string_view dimId) {
      standaloneWorld =
          std::make_unique<World>(spec, 1000000, dimId);
    };
    auto loadBlockTagsInto = [&](World &targetWorld) {
      if (blockTagPacks.empty())
        return;
      const auto reload = targetWorld.reloadBlockTags(blockTagPacks);
      for (const auto &message : reload.diagnostics)
        std::cerr << "BLOCK_TAGS: " << message << std::endl;
      if (!reload.applied)
        throw ProtocolError("cannot load block tag packs");
      std::cout << "BLOCK_TAGS loaded packs=" << blockTagPacks.size()
                << std::endl;
    };
    const auto ticketRecordLess = [](const PersistentTicketRecord &a,
                                     const PersistentTicketRecord &b) {
      return std::tie(a.x, a.z, a.type, a.level, a.ticksLeft) <
             std::tie(b.x, b.z, b.type, b.level, b.ticksLeft);
    };
    // The lock outlives the world set so exception unwinding still protects the
    // managers' best-effort final save.
    std::unique_ptr<TicketedWorldLock> ticketedLock;
    DimensionLevel *primaryTicketedLevel = nullptr;
    if (!ticketedWorldPath.empty()) {
      ticketedLock = std::make_unique<TicketedWorldLock>(ticketedWorldPath);
      const std::string targetDim = requestedDimension.empty()
                                        ? std::string("minecraft:overworld")
                                        : requestedDimension;
      ServerWorldOptions worldOpts;
      worldOpts.withChunkManagers = true;
      worldOpts.viewDistance = options.ticketViewDistance;
      worldOpts.simulationDistance = simulationDistance;
      worldOpts.extraBlockTagPacks = explicitBlockTagPacks;
      worldOpts.worldgenDirectory = worldgenDirectory;
      worldSet = ServerWorldSet::open(ticketedWorldPath, worldOpts);
      std::cout << "TICKETED_WORLDGEN "
                << (worldgenDirectory.empty()
                        ? std::string("disabled")
                        : worldgenDirectory.string())
                << std::endl;
      for (const auto &message : worldSet->dataPackSelection().diagnostics)
        std::cerr << "BLOCK_TAGS: " << message << std::endl;
      blockTagPacks = worldSet->blockTagPacks();
      if (!blockTagPacks.empty())
        std::cout << "BLOCK_TAGS loaded packs=" << blockTagPacks.size()
                  << std::endl;
      primaryTicketedLevel = worldSet->find(targetDim);
      if (!primaryTicketedLevel || !primaryTicketedLevel->chunkManager) {
        throw ProtocolError("--ticketed-world has no region for " + targetDim);
      }
      configureDimensionOptions(primaryTicketedLevel->stem,
                                worldSet->metadata().dimensions);
      World &pw = *primaryTicketedLevel->world;
      std::cout << "TICKETED_GAME_RULES advanceTime="
                << worldSet->metadata().gameRules.advanceTime
                << " tntExplodes=" << pw.tntExplodes
                << " waterSourceConversion=" << pw.waterSourceConversion
                << " lavaSourceConversion=" << pw.lavaSourceConversion
                << " randomTickSpeed=" << pw.randomTickSpeed
                << std::endl;
      for (const BuiltinWorldClock clock :
           {BuiltinWorldClock::Overworld, BuiltinWorldClock::TheEnd}) {
        const WorldClockState &state =
            worldSet->metadata().worldClocks.state(clock);
        std::cout << "TICKETED_WORLD_CLOCK "
                  << (clock == BuiltinWorldClock::Overworld
                          ? "minecraft:overworld"
                          : "minecraft:the_end")
                  << " total_ticks=" << state.totalTicks
                  << " partial_tick=" << state.partialTick
                  << " rate=" << state.rate
                  << " paused=" << (state.paused ? 1 : 0) << std::endl;
      }
      std::sort(primaryTicketedLevel->initialTickets.begin(),
                primaryTicketedLevel->initialTickets.end(), ticketRecordLess);
      if (!primaryTicketedLevel->initialTickets.empty()) {
        std::cout << "TICKETED_TICKETS_LOADED "
                  << primaryTicketedLevel->initialTickets.size() << std::endl;
      }
      std::cout << "TICKETED_DIMENSIONS count=" << worldSet->levels().size()
                << " initial=" << primaryTicketedLevel->stem.id << std::endl;
    } else if (!worldPath.empty()) {
      const WorldMetadata meta = loadWorldMetadata(worldPath);
      const std::string targetDim = requestedDimension.empty()
                                        ? std::string("minecraft:overworld")
                                        : requestedDimension;
      const DimensionStem *stem = meta.findDimension(targetDim);
      if (!stem)
        throw ProtocolError("--world does not contain dimension " + targetDim);
      createStandaloneWorld(stem->type, stem->id);
      configureDimensionOptions(*stem, meta.dimensions);
      selectPacks(worldPath, meta.levelRoot);
      loadBlockTagsInto(*standaloneWorld);
    } else {
      DimensionStem stem{"minecraft:overworld", "minecraft:overworld",
                         overworldDimensionType()};
      if (!requestedDimension.empty()) {
        const auto builtin = findBuiltinDimensionType(requestedDimension);
        if (!builtin)
          throw ProtocolError("unknown builtin dimension for standalone mode: " +
                              requestedDimension);
        stem.id = requestedDimension;
        stem.typeId = requestedDimension;
        stem.type = *builtin;
      }
      createStandaloneWorld(stem.type, stem.id);
      configureDimensionOptions(stem, {stem});
      loadBlockTagsInto(*standaloneWorld);
    }
    World &world = worldSet ? *primaryTicketedLevel->world : *standaloneWorld;
    if (!schematicPath.empty()) {
      if (!options.chunks.empty())
        throw ProtocolError(
            "--schematic loads its complete simulation region; omit --chunk");
      // A schematic generator's DataVersion is not trusted. Check every used
      // state against the actual 26.1 registry before mutating the world.
      schematicOptions.validateState = [&](const BlockState &state) {
        data.registry.stateId(state);
      };
      auto imported = loadLitematic(schematicPath, schematicOptions);
      importWorld(imported.world, world);
      const int minChunkX =
          static_cast<int>(std::floor(imported.min.x / 16.0)) - forceMargin;
      const int minChunkZ =
          static_cast<int>(std::floor(imported.min.z / 16.0)) - forceMargin;
      const int maxChunkX =
          static_cast<int>(std::floor((imported.maxExclusive.x - 1) / 16.0)) +
          forceMargin;
      const int maxChunkZ =
          static_cast<int>(std::floor((imported.maxExclusive.z - 1) / 16.0)) +
          forceMargin;
      // Explicit empty forced chunks allow movement across the schematic edge;
      // this does not generate terrain or add loading tickets.
      for (int x = minChunkX; x <= maxChunkX; ++x)
        for (int z = minChunkZ; z <= maxChunkZ; ++z) {
          world.loadChunk(x, z);
          options.chunks.emplace_back(x, z);
        }
      if (!explicitSpawn) {
        options.spawn = {
            imported.min.x +
                std::min(8.5, imported.maxExclusive.x - imported.min.x - 0.5),
            std::min(double(world.maxY() - 1), imported.maxExclusive.y + 4.0),
            imported.min.z +
                std::min(8.5, imported.maxExclusive.z - imported.min.z - 0.5)};
      }
      std::cout << "SCHEMATIC regions=" << imported.regionCount
                << " blocks=" << imported.world.blocks.size()
                << " anvils_replaced=" << imported.replacedAnvils
                << " signs_skipped=" << imported.skippedSigns
                << " sign_block_entities_skipped="
                << imported.skippedSignBlockEntities
                << " entities=" << world.entities.size()
                << " independent_entity_uuids="
                << imported.regeneratedEntityUuids
                << " source_data_version=" << imported.sourceDataVersion
                << " forced_chunks=" << options.chunks.size()
                << " offset=" << schematicOptions.offset.x << ','
                << schematicOptions.offset.y << ',' << schematicOptions.offset.z
                << " spawn=" << options.spawn.x << ',' << options.spawn.y << ','
                << options.spawn.z << std::endl;
      for (const auto &[name, count] : imported.substitutedBlocks)
        std::cout << "SUBSTITUTED " << name << " x" << count
                  << " -> minecraft:smooth_stone" << std::endl;
      for (const auto &[name, count] : imported.skippedUnsupportedBlockTicks)
        std::cout << "SKIPPED unsupported_block_tick=" << name
                  << " count=" << count << std::endl;
      for (const auto &[name, count] : imported.skippedUnsupportedFluidTicks)
        std::cout << "SKIPPED unsupported_fluid_tick=" << name
                  << " count=" << count << std::endl;
      if (imported.skippedUnsupportedBlockEntities ||
          imported.skippedUnsupportedEntities)
        std::cout << "SKIPPED unsupported_block_entities="
                  << imported.skippedUnsupportedBlockEntities
                  << " unsupported_entities="
                  << imported.skippedUnsupportedEntities << std::endl;
    } else if (!worldPath.empty()) {
      std::set<std::pair<int, int>> requestedChunks(options.chunks.begin(),
                                                    options.chunks.end());
      if (!requestedChunks.empty()) {
        anvilOptions.chunkFilter = [&requestedChunks](int cx, int cz) {
          return requestedChunks.contains({cx, cz});
        };
      }
      const std::filesystem::path regionDir =
          dimensionRegionDirectory(worldPath, world.dimensionId());
      if (!std::filesystem::is_directory(regionDir)) {
        throw ProtocolError("--world has no region directory for " +
                            world.dimensionId() + ": " + regionDir.string());
      }
      const AnvilWorldImportSummary summary =
          loadAnvilWorldInto(worldPath, regionDir, world, anvilOptions);
      if (options.chunks.empty()) {
        options.chunks = summary.loadedChunks;
      } else {
        for (auto [x, z] : options.chunks)
          if (!world.isChunkLoaded(x, z))
            throw ProtocolError("requested chunk absent from save");
      }
      std::cout << "WORLD loaded_chunks=" << summary.loadedChunks.size()
                << " skipped_non_full_chunks=" << summary.skippedNonFullChunks
                << " block_ticks=" << summary.loadedBlockTicks
                << " fluid_ticks=" << summary.loadedFluidTicks
                << " block_entities=" << summary.loadedBlockEntities
                << " entities=" << summary.loadedEntities << std::endl;
    } else if (worldSet) {
      // Opt-in async managed world. startup read only metadata; region reads
      // happen on each dimension's lifecycle worker.
    } else {
      if (options.chunks.empty()) {
        const int radius = singlePlatform ? options.viewDistance : 1;
        for (int x = -radius; x <= radius; ++x)
          for (int z = -radius; z <= radius; ++z)
            options.chunks.emplace_back(x, z);
      }
      // Explicit in-memory demonstration fixture, not a world generator.
      for (auto [cx, cz] : options.chunks) {
        world.loadChunk(cx, cz);
        if (!singlePlatform || (cx == 0 && cz == 0)) {
          for (int x = 0; x < 16; ++x)
            for (int z = 0; z < 16; ++z)
              world.rawSetBlock({cx * 16 + x, 64, cz * 16 + z},
                                defaultState("smooth_stone"));
        }
      }
      if (fluidFixture) {
        std::size_t columns = 0;
        for (auto [cx, cz] : options.chunks) {
          const int bx = cx * 16 + 8, bz = cz * 16 + 8;
          // A sealed vertical shaft: floor at y=48, air from y=49..63, and a
          // stone ring around each shaft cell. Falling water therefore writes
          // into the lower section (y=48..63) as well as the source section.
          world.rawSetBlock({bx, 48, bz}, defaultState("smooth_stone"));
          for (int y = 49; y <= 63; ++y) {
            world.rawSetBlock({bx, y, bz}, Blocks::AIR.defaultState());
            for (int dx = -1; dx <= 1; ++dx)
              for (int dz = -1; dz <= 1; ++dz)
                if (dx || dz)
                  world.rawSetBlock({bx + dx, y, bz + dz},
                                    defaultState("smooth_stone"));
          }
          for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz)
              if (dx || dz)
                world.rawSetBlock({bx + dx, 64, bz + dz},
                                  defaultState("smooth_stone"));
          world.rawSetBlock({bx, 64, bz}, Blocks::AIR.defaultState());
          // A small surface pool in the same chunk writes within the source
          // section (y=64..79), while the shaft writes into y=48..63.
          for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz) {
              world.rawSetBlock({bx + 4 + dx, 63, bz + 4 + dz},
                                defaultState("smooth_stone"));
              world.rawSetBlock({bx + 4 + dx, 64, bz + 4 + dz},
                                Blocks::AIR.defaultState());
            }
          // Normal placement path: onPlace queues the first fluid tick.
          world.setBlock({bx, 64, bz},
                         defaultState("water").with("level", "0"));
          world.setBlock({bx + 4, 64, bz + 4},
                         defaultState("water").with("level", "0"));
          ++columns;
        }
        std::cout << "FLUID_FIXTURE columns=" << columns
                  << " y=48..64 source_section=4 lower_section=3"
                  << std::endl;
      }
      if (entityFixture) {
        auto buildGlassCage = [&](int cx, int cz) {
          for (int y = 65; y <= 67; ++y) {
            world.setBlock({cx - 1, y, cz}, defaultState("glass"), 0);
            world.setBlock({cx + 1, y, cz}, defaultState("glass"), 0);
            world.setBlock({cx, y, cz - 1}, defaultState("glass"), 0);
            world.setBlock({cx, y, cz + 1}, defaultState("glass"), 0);
          }
          world.setBlock({cx, 68, cz}, defaultState("glass"), 0);
        };
        // Cage 1 at (4, 65, 8): N creepers ignoring cramming damage.
        buildGlassCage(4, 8);
        for (int i = 0; i < entityCramCount; ++i) {
          auto creeper = std::make_shared<FixtureCreeper>();
          const double ox = 4.5 + ((i % 5) - 2) * 0.04;
          const double oz = 8.5 + (((i / 5) % 5) - 2) * 0.04;
          creeper->setPos(ox, 65.0, oz);
          world.addEntity(creeper);
        }
        // Cage 2 at (12, 65, 8): 200 non-pushable items + 20 armor stands + 2 creepers.
        buildGlassCage(12, 8);
        for (int i = 0; i < 200; ++i) {
          auto item = std::make_shared<ItemEntity>(
              world, Vec3{12.5, 65.0, 8.5}, ItemStack{"minecraft:cobblestone", 64});
          item->setDeltaMovement(0.0, 0.0, 0.0);
          item->pickupDelay = 32767;
          item->age = -32768;
          world.addEntity(item);
        }
        for (int i = 0; i < 20; ++i) {
          auto stand = std::make_shared<ArmorStand>(world, Vec3{12.5, 65.0, 8.5});
          world.addEntity(stand);
        }
        for (int i = 0; i < 2; ++i) {
          auto c = std::make_shared<FixtureCreeper>();
          c->setPos(12.4 + i * 0.2, 65.0, 8.5);
          world.addEntity(c);
        }
        // Track + Minecarts + Boat + Step-up slabs around z=12..14.
        for (int x = 6; x <= 10; ++x)
          world.setBlock({x, 65, 12}, defaultState("rail"), 0);
        world.spawnMinecart({7.5, 65.0625, 12.5});
        world.spawnMinecart({9.5, 65.0625, 12.5});
        auto boat = std::make_shared<Boat>(world, Vec3{12.5, 65.0, 12.5},
                                           "minecraft:oak_boat",
                                           "minecraft:oak_boat");
        world.addEntity(boat);
        world.setBlock({7, 65, 14}, defaultState("stone_slab").with("type", "bottom"), 0);
        world.setBlock({8, 65, 14}, defaultState("smooth_stone"), 0);
        std::cout << "ENTITY_FIXTURE creepers=" << entityCramCount
                  << " cage1=(4.5,65,8.5) cage2=(12.5,65,8.5) "
                  << "total_entities=" << world.entities.size() << std::endl;
      }
    }
    std::sort(options.chunks.begin(), options.chunks.end());
    options.chunks.erase(
        std::unique(options.chunks.begin(), options.chunks.end()),
        options.chunks.end());
    // The native protocol path uses published light rather than placeholder
    // brightness. Initial chunk sends wait asynchronously for the first complete
    // worker snapshot; no connection callback waits on the light worker.
    if (worldSet)
      worldSet->enableLighting(lightOptions);
    else
      world.enableLighting(lightOptions);
    SocketSystem socketSystem;
    Socket listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.get() == invalidSocket)
      throw ProtocolError("socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener.get(), reinterpret_cast<sockaddr *>(&address),
             sizeof(address)))
      throw ProtocolError("bind failed (port in use?)");
    if (listen(listener.get(), 8))
      throw ProtocolError("listen failed");
    nonblocking(listener.get());
    int addressSize = sizeof(address);
#ifdef _WIN32
    getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address),
                &addressSize);
#else
    socklen_t addressLength = addressSize;
    getsockname(listener.get(), reinterpret_cast<sockaddr *>(&address),
                &addressLength);
#endif
    std::cout << "LISTENING 127.0.0.1:" << ntohs(address.sin_port)
              << " protocol=775 allocator=" << REDSTONE_ALLOCATOR_NAME
              << " workers=1" << std::endl;
    std::vector<std::shared_ptr<Connection>> connections;
    TicketSourceManager ticketSources(
        std::clamp(options.ticketViewDistance,
                   TicketSourceManager::MinViewDistance,
                   TicketSourceManager::MaxViewDistance),
        simulationDistance);
    // Payloads/snapshots are collected on the main thread; framing, chunk
    // encoding and socket writes happen on the writer thread.
    NetworkWriter writer;
    writer.start();
    NetworkTeardownGuard teardown;
    std::size_t ticketedInstalls = 0;
    const auto bindDimensionObservers = [&](World &targetWorld,
                                            WorldChunkManager *manager) {
      teardown.track(&targetWorld, manager);
      World *wp = &targetWorld;
      if (manager) {
        manager->managedChunkInstalled = [&, wp](const ChunkTicketPos &p) {
          ++ticketedInstalls;
          for (auto &c : connections)
            if (!c->dead && &c->session.world() == wp)
              c->session.managedChunkResidency(p.x, p.z, true);
        };
        manager->managedChunkRemoved = [&, wp](const ChunkTicketPos &p) {
          for (auto &c : connections)
            if (!c->dead && &c->session.world() == wp)
              c->session.managedChunkResidency(p.x, p.z, false);
        };
      }
      targetWorld.lightPublicationObserver =
          [&, wp](std::shared_ptr<const LightChunkSnapshot> snapshot) {
            for (auto &c : connections)
              if (!c->dead && &c->session.world() == wp) {
                c->session.lightPublished(snapshot);
                writer.submit(c, c->session.takeOutputItems());
              }
          };
      targetWorld.lightChunkLifecycleObserver = [&, wp](int x, int z,
                                                        bool loaded) {
        for (auto &c : connections)
          if (!c->dead && &c->session.world() == wp) {
            c->session.lightChunkLifecycle(x, z, loaded);
            writer.submit(c, c->session.takeOutputItems());
          }
      };
      targetWorld.blockUpdateObserver = [&, wp](Pos p, const BlockState &s,
                                                int) {
        for (auto &c : connections)
          if (!c->dead && &c->session.world() == wp)
            c->session.blockUpdate(p, s);
      };
      targetWorld.blockEventObserver = [&, wp](const BlockEvent &e) {
        for (auto &c : connections)
          if (!c->dead && &c->session.world() == wp) {
            c->session.blockEvent(e);
            writer.submit(c, c->session.takeOutputItems());
          }
      };
      targetWorld.blockUpdatesBroadcastObserver = [&, wp] {
        for (auto &c : connections)
          if (!c->dead && &c->session.world() == wp) {
            c->session.flushBlockUpdates();
            writer.submit(c, c->session.takeOutputItems());
          }
      };
      targetWorld.entityTrackingBroadcastObserver = [&, wp] {
        for (auto &c : connections)
          if (!c->dead && &c->session.world() == wp) {
            c->session.broadcastEntities();
            writer.submit(c, c->session.takeOutputItems());
          }
      };
      targetWorld.entityAddedObserver = [&, wp](const Entity &entity) {
        for (auto &c : connections)
          if (!c->dead && &c->session.world() == wp) {
            c->session.entityAdded(entity);
            writer.submit(c, c->session.takeOutputItems());
          }
      };
      targetWorld.entityRemovedObserver = [&, wp](int id) {
        for (auto &c : connections)
          if (!c->dead && &c->session.world() == wp) {
            c->session.entityRemoved(id);
            writer.submit(c, c->session.takeOutputItems());
          }
      };
      targetWorld.explosionObserver = [&, wp](Vec3 center, float radius,
                                              int blockCount) {
        for (auto &c : connections)
          if (!c->dead && &c->session.world() == wp) {
            c->session.explosion(center, radius, blockCount);
            writer.submit(c, c->session.takeOutputItems());
          }
      };
      targetWorld.fallingBlockLandObserver =
          [&, wp](int entityId, Pos pos, const BlockState &state) {
            for (auto &c : connections)
              if (!c->dead && &c->session.world() == wp) {
                c->session.fallingBlockLanded(entityId, pos, state);
                writer.submit(c, c->session.takeOutputItems());
              }
          };
      targetWorld.entityTakenObserver =
          [&, wp](int collectorId, int targetId, int amount) {
            for (auto &c : connections)
              if (!c->dead && &c->session.world() == wp) {
                c->session.sendTakeItemEntity(collectorId, targetId, amount);
                writer.submit(c, c->session.takeOutputItems());
              }
          };
    };
    if (worldSet) {
      for (const auto &level : worldSet->levels())
        bindDimensionObservers(*level->world, level->chunkManager.get());
    } else {
      bindDimensionObservers(world, nullptr);
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    // MinecraftServer's command dispatcher and tick rate manager. /tick is
    // always available; /time is added when the ticketed world owns clocks.
    command::Dispatcher commands;
    command::registerTickCommand(commands);
    TickRateManager tickRate;
    command::Context commandContext;
    commandContext.ticks = &tickRate;
    if (worldSet) {
      command::registerTimeCommand(commands);
      commandContext.world = &world;
      commandContext.worldClocks = &worldSet->metadata().worldClocks;
      commandContext.worldClockChanged = [&](BuiltinWorldClock clock) {
        for (auto &c : connections)
          if (!c->dead && !c->session.closed()) {
            c->session.sendWorldClockUpdate(clock);
            writer.submit(c, c->session.takeOutputItems());
          }
      };
    }
    std::array<std::int64_t, 100> tickTimes{};
    std::int64_t aggregatedTickNanos = 0;
    std::int64_t tickCount = 0;
    std::uint64_t tickStateRevision = tickRate.stateRevision();
    std::uint64_t tickStepRevision = tickRate.stepRevision();
    auto nextTick =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    int ticks = 0;
    TickTimingWindow timingWindow;
    double totalTickMilliseconds = 0, maxTickMilliseconds = 0;
    // Console front end: the same dispatcher the client uses, with a source at
    // owner level so the local operator can drive /tick without an op list.
    ConsoleInput console(consoleEnabled);
    if (consoleEnabled)
      std::cout << "CONSOLE ready: type /tick ... (stop to shut down)"
                << std::endl;
    while (!stopRequested && (!maxTicks || ticks < maxTicks)) {
      if (worldSet)
        worldSet->pumpAll();
      // Console commands run on the server thread between ticks, so they can
      // never interleave with a running simulation.
      if (consoleEnabled)
        for (std::string line; console.next(line);) {
          while (!line.empty() && line.front() == '/')
            line.erase(line.begin());
          if (line == "stop" || line == "quit" || line == "exit") {
            stopRequested = 1;
            break;
          }
          if (line == "reload") {
            bool allApplied = true;
            if (worldSet) {
              for (const auto &level : worldSet->levels()) {
                const auto reload = level->world->reloadBlockTags(blockTagPacks);
                for (const auto &message : reload.diagnostics)
                  std::cerr << "BLOCK_TAGS: " << message << std::endl;
                if (!reload.applied) {
                  allApplied = false;
                  break;
                }
              }
            } else {
              const auto reload = world.reloadBlockTags(blockTagPacks);
              for (const auto &message : reload.diagnostics)
                std::cerr << "BLOCK_TAGS: " << message << std::endl;
              allApplied = reload.applied;
            }
            if (!allApplied) {
              std::cout << "BLOCK_TAGS reload failed; previous snapshot retained" << std::endl;
            } else {
              for (auto &c : connections)
                if (!c->dead && !c->session.closed()) {
                  c->session.sendBlockTagUpdate();
                  writer.submit(c, c->session.takeOutputItems());
                }
              std::cout << "BLOCK_TAGS reload complete" << std::endl;
            }
            continue;
          }
          if (line.empty())
            continue;
          if (line == "help" || line == "?") {
            std::cout << "Commands: /tick query | /tick rate <1..10000> | "
                         "/tick step [<time>|stop] | /tick sprint "
                         "<time>|stop | /tick freeze | /tick unfreeze"
                      << (worldSet
                              ? " | /time set <duration|marker> | /time add "
                                "<duration> | /time pause|resume|rate <rate> | "
                                "/time query time|gametime|<timeline> "
                                "[repetition] | /time of <clock> ..."
                              : "")
                      << std::endl;
            continue;
          }
          command::Source source;
          source.permissionLevel = 4;
          source.name = "Server";
          if (commands.run(line, source, commandContext) < 0) {
            std::cout << "Unknown or incomplete command: " << line << std::endl;
            continue;
          }
          for (const auto &message : source.messages) {
            std::cout << (message.failure ? "[FAIL] " : "")
                      << formatConsole(message) << std::endl;
            // A broadcast line reaches connected players, like a vanilla server.
            if (message.broadcast)
              for (auto &c : connections)
                if (!c->dead) {
                  c->session.sendCommandMessage(message);
                  writer.submit(c, c->session.takeOutputItems());
                }
          }
        }
      if (stopRequested)
        break;
      // The main thread only waits for readable sockets; the write side lives
      // on NetworkWriter.
      fd_set reads;
      FD_ZERO(&reads);
      FD_SET(listener.get(), &reads);
      SocketHandle largest = listener.get();
      for (auto &c : connections)
        if (!c->dead.load() && !c->session.closed()) {
          FD_SET(c->socket.get(), &reads);
          largest = std::max(largest, c->socket.get());
        }
      timeval timeout{};
      // A sprinting server polls without waiting, like MinecraftServer.run:
      // the loop must spin as fast as it can instead of pacing to the tick rate.
      // When not sprinting, sleep at most until nextTick (capped at 5 ms for
      // console/accept responsiveness) so /tick rate > 200 TPS is not throttled.
      if (tickRate.isSprinting()) {
        timeout.tv_usec = 0;
      } else {
        const auto remainingUs =
            std::chrono::duration_cast<std::chrono::microseconds>(
                nextTick - std::chrono::steady_clock::now())
                .count();
        timeout.tv_usec =
            static_cast<long>(std::clamp<std::int64_t>(remainingUs, 0, 5000));
      }
      if (select(static_cast<int>(largest + 1), &reads, nullptr, nullptr,
                 &timeout) < 0) {
        if (stopRequested)
          break;
        throw ProtocolError("select failed");
      }
      if (FD_ISSET(listener.get(), &reads)) {
        Socket socket(accept(listener.get(), nullptr, nullptr));
        if (socket.get() != invalidSocket && connections.size() < 8) {
          nonblocking(socket.get());
          auto sessionOptions = options;
          sessionOptions.playerId = world.allocateEntityId();
          connections.push_back(std::make_shared<Connection>(
              std::move(socket), world, data, std::move(sessionOptions)));
          writer.add(connections.back());
          connections.back()->session.attachCommands(&commands, &commandContext);
          if (worldSet)
            connections.back()->session.setWorldClockSource(
                worldSet->metadata().worldClocks,
                worldSet->metadata().gameRules.advanceTime);
          if (primaryTicketedLevel && primaryTicketedLevel->chunkManager) {
            auto *manager = primaryTicketedLevel->chunkManager.get();
            auto *session = &connections.back()->session;
            session->setManagedResidencyProvider(
                [manager](int x, int z) {
                  return manager->isManaged({x, z});
                },
                [manager]() {
                  return manager->managedChunks();
                });
          }
        }
      }
      for (auto &c : connections) {
        if (!c->dead.load() && !c->session.closed() && FD_ISSET(c->socket.get(), &reads)) {
          std::uint8_t buffer[65536];
          int count = recv(c->socket.get(), reinterpret_cast<char *>(buffer),
                           sizeof(buffer), 0);
          if (count > 0)
            c->session.receive({buffer, static_cast<std::size_t>(count)});
          else if (!count || !wouldBlock())
            c->dead = true;
        }
        writer.submit(c, c->session.takeOutputItems());
        if (c->session.closed())
          c->closing = true;
      }
      for (const auto &connection : connections) {
        const bool present = ticketPlayerDemandActive(
            connection->session.phase() == Phase::Play,
            connection->dead.load(), connection->closing.load(),
            connection->session.closed());
        const auto position = connection->session.playerPosition();
        const ChunkTicketPos center{
            static_cast<int>(std::floor(position.x)) >> 4,
            static_cast<int>(std::floor(position.z)) >> 4};
        const auto pid = static_cast<std::uint64_t>(connection->session.playerId());
        if (worldSet) {
          for (const auto &level : worldSet->levels()) {
            if (!level->chunkManager)
              continue;
            if (level->world.get() == &connection->session.world())
              (void)level->chunkManager->setPlayer(pid, present, center);
            else
              (void)level->chunkManager->removePlayer(pid);
          }
        } else {
          (void)ticketSources.setPlayer(pid, present, center);
        }
      }
      // One bounded pump per socket iteration keeps completed I/O and
      // a large frozen-world ticket-removal queue moving without doing work
      // once per connection. Held autosave remains server-tick-only.
      if (worldSet)
        worldSet->pumpAll();
      std::erase_if(connections, [&](const auto &c) {
        const bool remove =
            c->dead.load() || (c->closing.load() && c->flushed.load());
        if (remove) {
          const auto pid = static_cast<std::uint64_t>(c->session.playerId());
          if (worldSet) {
            for (const auto &level : worldSet->levels())
              if (level->chunkManager)
                (void)level->chunkManager->removePlayer(pid);
          } else {
            (void)ticketSources.removePlayer(pid);
          }
        }
        return remove;
      });
      auto now = std::chrono::steady_clock::now();
      const auto syncTickState = [&]() {
        const bool stateChanged = tickRate.stateRevision() != tickStateRevision;
        const bool stepChanged = tickRate.stepRevision() != tickStepRevision;
        if (!stateChanged && !stepChanged)
          return;
        tickStateRevision = tickRate.stateRevision();
        tickStepRevision = tickRate.stepRevision();
        const auto targetInterval =
            std::chrono::nanoseconds(tickRate.nanosecondsPerTick());
        if (stateChanged && !tickRate.isSprinting() &&
            nextTick - now > targetInterval)
          nextTick = now + targetInterval;
        for (auto &c : connections)
          if (!c->dead) {
            if (stateChanged)
              c->session.sendTickingState(tickRate);
            if (stepChanged)
              c->session.sendTickingStep(tickRate);
            writer.submit(c, c->session.takeOutputItems());
          }
      };
      // Sync command-driven /tick rate|freeze|step updates before tickRate.tick()
      // decrements the step budget, matching vanilla ServerTickRateManager.
      syncTickState();
      // MinecraftServer.run: a sprinting iteration ticks immediately and
      // restarts the scheduler clock; otherwise the loop waits for the current
      // tick rate.
      bool sprintingThisIteration = false;
      if (tickRate.isSprinting() && tickRate.checkShouldSprintThisTick(now)) {
        sprintingThisIteration = true;
        nextTick = now;
      }
      if (now >= nextTick) {
        const auto tickStart = now;
        tickRate.tick();
        if (worldSet) {
          worldSet->tick(tickRate.runsNormally(), true);
        } else if (tickRate.runsNormally()) {
          world.tick();
        } else {
          world.drainLightPublications();
        }
        for (auto &c : connections)
          if (!c->dead.load()) c->session.tick();
        if (tickRate.runsNormally()) {
          if (worldSet) {
            for (const auto &level : worldSet->levels())
              level->world->log.clear();
          } else {
            world.log.clear();
          }
        }
        const auto tickEnd = std::chrono::steady_clock::now();
        if (sprintingThisIteration)
          tickRate.endTickWork(tickEnd);
        const auto tickNanos =
            std::chrono::duration_cast<std::chrono::nanoseconds>(tickEnd -
                                                                 tickStart)
                .count();
        const double milliseconds =
            std::chrono::duration<double, std::milli>(tickEnd - tickStart)
                .count();
        totalTickMilliseconds += milliseconds;
        maxTickMilliseconds = std::max(maxTickMilliseconds, milliseconds);
        const auto timingReport = timingWindow.record(tickEnd, milliseconds);
        // MinecraftServer.tickServer "tallying": 100-sample ring plus the average
        // over min(100, tickCount) samples used by /tick query.
        ++tickCount;
        const auto index = static_cast<std::size_t>(tickCount % 100);
        aggregatedTickNanos -= tickTimes[index];
        aggregatedTickNanos += tickNanos;
        tickTimes[index] = tickNanos;
        commandContext.averageTickTimeNanos =
            aggregatedTickNanos /
            std::min<std::int64_t>(100, std::max<std::int64_t>(tickCount, 1));
        commandContext.tickTimes = tickTimes;
        if (worldSet && worldSet->cleanupManager()) {
          worldSet->cleanupManager()->recordExternalTickTimeMs(milliseconds);
        } else if (world.cleanupManager()) {
          world.cleanupManager()->recordExternalTickTimeMs(milliseconds);
        }
        // Sent after this tick's simulation and connection synchronization.
        // Idle waiting, socket IO outside the tick and this display are
        // excluded.
        for (auto &c : connections)
          if (!c->dead) {
            if (timingReport)
              c->session.sendTickTime(timingReport->milliseconds, world.gameTime(), timingReport->ticks);
            writer.submit(c, c->session.takeOutputItems());
            if (c->session.closed())
              c->closing = true;
          }
        ++ticks;
        if (ticks == 1 || ticks % 100 == 0)
          std::cout << "TICK tick=" << world.gameTime() << " ms=" << std::fixed
                    << std::setprecision(3) << milliseconds
                    << " entities=" << world.entities.size()
                    << " scheduled=" << world.scheduledTickCount()
                    << " moving_pistons=" << world.pistonEntities.size()
                    << " tick_rate=" << std::setprecision(1)
                    << tickRate.tickrate()
                    << (tickRate.isFrozen() ? " frozen" : "")
                    << std::endl;
        nextTick += std::chrono::nanoseconds(tickRate.nanosecondsPerTick());
        if (tickEnd - nextTick > std::chrono::seconds(1))
          nextTick =
              tickEnd + std::chrono::nanoseconds(tickRate.nanosecondsPerTick());
      }
      // ServerTickRateManager.updateStateToClients / updateStepTicks: also flush
      // state changes triggered at sprint completion (e.g. restoring frozen).
      syncTickState();
      // ServerTickRateManager.finishTickSprint reporting.
      if (const auto report = tickRate.takeSprintReport()) {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << std::fixed << std::setprecision(2)
             << report->millisecondsPerTick;
        std::cout << "SPRINT ticks_per_second=" << report->ticksPerSecond
                  << " ms_per_tick=" << text.str()
                  << std::endl;
        for (auto &c : connections)
          if (!c->dead) {
            c->session.sendCommandMessage(
                command::Message{"commands.tick.sprint.report",
                                 {std::to_string(report->ticksPerSecond),
                                  text.str()},
                                 false, true});
            writer.submit(c, c->session.takeOutputItems());
          }
      }
    }
    if (worldSet) {
      bool allComplete = true;
      bool allWorkersStopped = true;
      std::size_t totalSaved = 0;
      std::size_t totalUnsaved = 0;
      ChunkGenerationStats totalGeneration;
      for (const auto &level : worldSet->levels()) {
        if (!level->chunkManager)
          continue;
        ChunkShutdownResult shutdown = level->chunkManager->close();
        const ChunkGenerationStats generation =
            level->chunkManager->generationStats();
        totalGeneration.requested += generation.requested;
        totalGeneration.completed += generation.completed;
        totalGeneration.failed += generation.failed;
        totalGeneration.bufferRequests += generation.bufferRequests;
        totalGeneration.bufferCompleted += generation.bufferCompleted;
        totalGeneration.evicted += generation.evicted;
        allComplete = allComplete && shutdown.complete;
        allWorkersStopped = allWorkersStopped && shutdown.workerStopped;
        totalSaved += shutdown.savedChunks.size();
        totalUnsaved += shutdown.unsavedChunks.size();
        if (!shutdown.fatalError.empty())
          std::cerr << "TICKETED_WORLD_FATAL [" << level->stem.id << "] "
                    << shutdown.fatalError << std::endl;
        for (const ChunkShutdownIssue &issue : shutdown.unsavedChunks)
          std::cerr << "UNSAVED_CHUNK [" << level->stem.id << "] "
                    << issue.position.x << " " << issue.position.z
                    << " dirty=" << issue.dirtyVersion
                    << " saved=" << issue.savedDirtyVersion
                    << " error=" << issue.error << std::endl;
      }
      std::cout << "TICKETED_WORLD_CLOSED complete=" << (allComplete ? 1 : 0)
                << " saved=" << totalSaved << " unsaved=" << totalUnsaved
                << " worker_stopped=" << (allWorkersStopped ? 1 : 0)
                << " resident_installs=" << ticketedInstalls << std::endl;
      std::cout << "TICKETED_WORLDGEN_STATS requested="
                << totalGeneration.requested
                << " generated=" << totalGeneration.completed
                << " failed=" << totalGeneration.failed
                << " buffer_requests=" << totalGeneration.bufferRequests
                << " buffer_generated=" << totalGeneration.bufferCompleted
                << " evicted=" << totalGeneration.evicted << std::endl;
      if (!allComplete)
        return 1;
      // Terrain has settled under the same world lock. Metadata and tickets
      // follow as separate observable files; no cross-file crash transaction
      // is claimed.
      WorldMetadata &meta = worldSet->metadata();
      const std::int64_t initialTime =
          meta.levelRoot.value.get("Data")->get("Time")->number;
      bool metadataSaved = false;
      if (meta.gameTime != initialTime) {
        setWorldMetadataTime(meta, meta.gameTime);
        saveWorldMetadata(ticketedWorldPath, meta);
        metadataSaved = true;
      }
      const bool clocksSaved = meta.worldClocks.saveIfDirty(
          ticketedWorldPath / "data" / "minecraft" / "world_clocks.dat");
      const auto sameTickets = [](const auto &left, const auto &right) {
        if (left.size() != right.size())
          return false;
        for (std::size_t i = 0; i < left.size(); ++i) {
          const auto &a = left[i];
          const auto &b = right[i];
          if (a.x != b.x || a.z != b.z || a.type != b.type ||
              a.level != b.level || a.ticksLeft != b.ticksLeft)
            return false;
        }
        return true;
      };
      bool ticketsChanged = false;
      for (const auto &level : worldSet->levels()) {
        if (!level->chunkManager)
          continue;
        const auto tickets = level->chunkManager->ticketSources()
                                 .tickets()
                                 .storage()
                                 .persistentTickets();
        std::vector<PersistentTicketRecord> ticketRecords;
        ticketRecords.reserve(tickets.size());
        for (const auto &[position, ticket] : tickets)
          if (ticket.type().name == ticket_types::Forced.name ||
              ticket.type().name == ticket_types::Portal.name)
            ticketRecords.push_back({position.x, position.z,
                                     std::string(ticket.type().name),
                                     ticket.level(), ticket.ticksLeft()});
        std::sort(ticketRecords.begin(), ticketRecords.end(), ticketRecordLess);
        const auto ticketPath = ticketDataPath(level->regionDirectory);
        const bool noTicketFile = !std::filesystem::exists(ticketPath);
        const bool levelChanged =
            !(noTicketFile && ticketRecords.empty()) &&
            !sameTickets(level->initialTickets, ticketRecords);
        if (levelChanged &&
            (!ticketRecords.empty() || std::filesystem::exists(ticketPath))) {
          savePersistentTickets(ticketPath, ticketRecords);
        }
        ticketsChanged = ticketsChanged || levelChanged;
      }
      std::cout << "TICKETED_METADATA_SAVED metadata="
                << (metadataSaved ? 1 : 0)
                << " tickets=" << (ticketsChanged ? 1 : 0)
                << " clocks=" << (clocksSaved ? 1 : 0) << std::endl;
    }
    teardown.detach();
    writer.stop();
    std::cout << "STOPPED ticks=" << ticks
              << " mean_ms=" << (ticks ? totalTickMilliseconds / ticks : 0)
              << " max_ms=" << maxTickMilliseconds
              << " backpressure_disconnects=" << writer.backpressureDisconnects() << '\n';
    // NetworkWriter owns connection handles until its final join. Release
    // Session/World references before the manager and World leave scope.
    connections.clear();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

#ifdef _WIN32
// Preserve Chinese schematic paths regardless of the active ANSI code page.
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> utf8Args;
  utf8Args.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                                   nullptr, 0, nullptr, nullptr);
    if (!size)
      return 1;
    std::string value(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1,
                        value.data(), size, nullptr, nullptr);
    value.pop_back();
    utf8Args.push_back(std::move(value));
  }
  std::vector<char *> pointers;
  for (auto &value : utf8Args)
    pointers.push_back(value.data());
  return runServer(argc, pointers.data());
}
#else
int main(int argc, char **argv) { return runServer(argc, argv); }
#endif
