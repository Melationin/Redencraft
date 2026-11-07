#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "redstone/litematic.hpp"
#include "redstone/net/session.hpp"
#include "redstone/net/outbox.hpp"
#include "redstone/net/tick_timing.hpp"
#include "redstone/storage.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>

using namespace redstone;
using namespace redstone::net;
namespace {
volatile std::sig_atomic_t stopRequested = 0;
void stop(int) { stopRequested = 1; }
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
            Bytes produced = packet.deferred ? packet.deferred()
                : packet.packetId >= 0 ? frame(packet.packetId, packet.bytes)
                                       : std::move(packet.bytes);
            if (produced.size() > maxQueuedOutputBytes ||
                connection.pending.size() - connection.sent > maxQueuedOutputBytes - produced.size()) {
              connection.dead.store(true, std::memory_order_release);
              break;
            }
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
} // namespace
int runServer(int argc, char **argv) {
  try {
    std::filesystem::path assets = REDSTONE_PROTOCOL_DIR, worldPath,
                          schematicPath;
    int port = 25565, maxTicks = 0, forceMargin = 2;
    bool explicitSpawn = false, explicitOffset = false;
    LitematicOptions schematicOptions;
    ServerOptions options;
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--help") {
        std::cout
            << "redstone_vanilla_server [--port 25565] [--assets "
               "directory] [--world directory | --schematic file]\n"
               "  [--chunk x,z ...] [--ticks N] [--spawn x,y,z]\n"
               "  [--offset x,y,z] [--view-distance 3] [--force-margin 2]\n"
               "Local offline protocol 775 creative editor. Fixed loaded "
               "simulation region; no saving. Litematic: anvils become "
               "obsidian, signs are omitted. Tick time sent to chat.\n";
        return 0;
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
      else if (arg == "--world")
        worldPath = utf8Path(value);
      else if (arg == "--schematic")
        schematicPath = utf8Path(value);
      else if (arg == "--view-distance")
        options.viewDistance = integer(value, 2, maxViewDistance);
      else if (arg == "--force-margin")
        forceMargin = integer(value, 0, 16);
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
    if (!worldPath.empty() && !schematicPath.empty())
      throw ProtocolError("choose --world or --schematic");
    if (schematicPath.empty() && explicitOffset)
      throw ProtocolError("--offset requires --schematic");
    ProtocolData data(assets);
    World world;
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
    } else if (!worldPath.empty()) {
      auto loaded =
          loadAnvilWorld(worldPath, defaultRegionDirectory(worldPath));
      importWorld(loaded, world);
      if (options.chunks.empty())
        options.chunks = loaded.chunks;
      else
        for (auto [x, z] : loaded.chunks) {
          if (std::find(options.chunks.begin(), options.chunks.end(),
                        std::pair{x, z}) == options.chunks.end())
            world.unloadChunk(x, z);
        }
      for (auto [x, z] : options.chunks)
        if (!world.isChunkLoaded(x, z))
          throw ProtocolError("requested chunk absent from save");
    } else {
      if (options.chunks.empty())
        for (int x = -1; x <= 1; ++x)
          for (int z = -1; z <= 1; ++z)
            options.chunks.emplace_back(x, z);
      // Explicit in-memory demonstration fixture, not a world generator.
      for (auto [cx, cz] : options.chunks) {
        world.loadChunk(cx, cz);
        for (int x = 0; x < 16; ++x)
          for (int z = 0; z < 16; ++z)
            world.rawSetBlock({cx * 16 + x, 64, cz * 16 + z},
                              defaultState("smooth_stone"));
      }
    }
    std::sort(options.chunks.begin(), options.chunks.end());
    options.chunks.erase(
        std::unique(options.chunks.begin(), options.chunks.end()),
        options.chunks.end());
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
              << std::endl;
    std::vector<std::shared_ptr<Connection>> connections;
    // Payloads/snapshots are collected on the main thread; framing, chunk
    // encoding and socket writes happen on the writer thread.
    NetworkWriter writer;
    writer.start();
    world.blockUpdateObserver = [&](Pos p, const BlockState &s, int) {
      for (auto &c : connections)
        if (!c->dead)
          c->session.blockUpdate(p, s);
    };
    world.blockEventObserver = [&](const BlockEvent &e) {
      for (auto &c : connections)
        if (!c->dead) {
          c->session.blockEvent(e);
          writer.submit(c, c->session.takeOutputItems());
        }
    };
    world.blockUpdatesBroadcastObserver = [&] {
      for (auto &c : connections)
        if (!c->dead) {
          c->session.flushBlockUpdates();
          writer.submit(c, c->session.takeOutputItems());
        }
    };
    world.entityTrackingBroadcastObserver = [&] {
      for (auto &c : connections)
        if (!c->dead) {
          c->session.broadcastEntities();
          writer.submit(c, c->session.takeOutputItems());
        }
    };
    world.entityAddedObserver = [&](const Entity &entity) {
      for (auto &c : connections)
        if (!c->dead) {
          c->session.entityAdded(entity);
          writer.submit(c, c->session.takeOutputItems());
        }
    };
    world.entityRemovedObserver = [&](int id) {
      for (auto &c : connections)
        if (!c->dead) {
          c->session.entityRemoved(id);
          writer.submit(c, c->session.takeOutputItems());
        }
    };
    world.fallingBlockLandObserver = [&](int entityId, Pos pos,
                                         const BlockState &state) {
      for (auto &c : connections)
        if (!c->dead) {
          c->session.fallingBlockLanded(entityId, pos, state);
          writer.submit(c, c->session.takeOutputItems());
        }
    };
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    auto nextTick =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    int ticks = 0;
    TickTimingWindow timingWindow;
    double totalTickMilliseconds = 0, maxTickMilliseconds = 0;
    while (!stopRequested && (!maxTicks || ticks < maxTicks)) {
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
      timeout.tv_usec = 5000;
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
          sessionOptions.playerId +=
              ticks * 8 + static_cast<int>(connections.size());
          connections.push_back(std::make_shared<Connection>(
              std::move(socket), world, data, std::move(sessionOptions)));
          writer.add(connections.back());
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
      std::erase_if(connections, [](const auto &c) {
        return c->dead.load() || (c->closing.load() && c->flushed.load());
      });
      auto now = std::chrono::steady_clock::now();
      if (now >= nextTick) {
        const auto tickStart = std::chrono::steady_clock::now();
        world.tick();
        for (auto &c : connections)
          if (!c->dead.load()) c->session.tick();
        world.log.clear();
        const auto tickEnd = std::chrono::steady_clock::now();
        const double milliseconds =
            std::chrono::duration<double, std::milli>(tickEnd - tickStart)
                .count();
        totalTickMilliseconds += milliseconds;
        maxTickMilliseconds = std::max(maxTickMilliseconds, milliseconds);
        const auto timingReport = timingWindow.record(tickEnd, milliseconds);
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
                    << std::endl;
        nextTick += std::chrono::milliseconds(50);
        if (tickEnd - nextTick > std::chrono::seconds(1))
          nextTick = tickEnd + std::chrono::milliseconds(50);
      }
    }
    writer.stop();
    std::cout << "STOPPED ticks=" << ticks
              << " mean_ms=" << (ticks ? totalTickMilliseconds / ticks : 0)
              << " max_ms=" << maxTickMilliseconds
              << " backpressure_disconnects=" << writer.backpressureDisconnects() << '\n';
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
