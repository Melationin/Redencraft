#pragma once
#include "redstone/tick_rate.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace redstone::command {
// One chat line a command produced. Vanilla sends a translatable component and
// lets the client resolve the key, so only the key and its arguments travel.
struct Message {
  std::string key;
  std::vector<std::string> args;
  bool failure = false;
  // CommandSourceStack.sendSuccess(message, broadcast): true is shown to every
  // player that may see the command, false only to the caller.
  bool broadcast = false;
};
// Server state /tick query needs. The caller refreshes it every tick.
struct Context {
  TickRateManager *ticks = nullptr;
  std::int64_t averageTickTimeNanos = 0;
  // MinecraftServer.tickTimesNanos: 100-entry ring, zero-filled at startup.
  std::array<std::int64_t, 100> tickTimes{};
};
struct Source {
  // 0 all, 2 gamemasters, 3 admins, 4 owners (Commands.LEVEL_*).
  int permissionLevel = 0;
  std::string name;
  std::vector<Message> messages;
  void success(std::string key, std::vector<std::string> args = {},
               bool broadcast = false);
  void failure(std::string key, std::vector<std::string> args = {});
};
// CommandDispatcher.getCompletionSuggestions result: the caller writes the
// replacement range and the candidate texts into ClientboundCommandSuggestions.
struct Completion {
  int start = 0;
  int length = 0;
  std::vector<std::string> texts;
};
struct Node {
  enum class Kind { Root, Literal, Argument };
  // Argument types /tick uses. The wire export maps these onto the vanilla
  // parser ids (brigadier:float, minecraft:time).
  enum class ArgumentKind { Float, Time };

  Kind kind = Kind::Root;
  std::string name;
  // CommandNode.getRequirement: a node is only reachable when this passes.
  std::function<bool(const Source &)> requirement;
  ArgumentKind argumentKind = ArgumentKind::Float;
  double minimum = 0, maximum = 0;
  // ArgumentCommandNode.getCustomSuggestions: the fixed examples vanilla offers
  // for an argument (/tick rate -> "20", step/sprint time -> "1t", "1s", ...).
  std::vector<std::string> suggestions;
  std::function<int(const Context &, Source &,
                    const std::map<std::string, double> &)>
      executes;
  std::vector<std::unique_ptr<Node>> children;

  bool executable() const { return static_cast<bool>(executes); }
  bool restricted() const { return static_cast<bool>(requirement); }
};

class Dispatcher {
public:
  Dispatcher();
  Node &root();
  const Node &root() const;
  // Parses and runs a command line without the leading slash. Returns the
  // command result, or -1 when no node accepts the line (unknown or incomplete
  // command, or the source failed a requirement).
  int run(std::string_view line, Source &, const Context &) const;
  // CommandDispatcher.getCompletionSuggestions: the candidates for the last
  // (possibly empty) word of a partial command line, without the leading slash.
  Completion suggestions(std::string_view line, const Source &) const;

private:
  std::unique_ptr<Node> root_;
};

// Registers /tick exactly as TickCommand.register does.
void registerTickCommand(Dispatcher &);
} // namespace redstone::command
