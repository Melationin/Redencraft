#include "redstone/command.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace redstone::command {
namespace {
// Locale-independent "%.1f": the process never calls setlocale, but String.format
// uses Locale.ROOT for exactly this reason and the values reach the client.
std::string oneDecimal(double value) {
  char buffer[64];
  std::snprintf(buffer, sizeof buffer, "%.1f", value);
  return buffer;
}
std::string nanosToMilis(std::int64_t nanos) {
  return oneDecimal(static_cast<double>(nanos) / 1.0e6);
}
bool parseFloat(std::string_view token, double &out) {
  const std::string text(token);
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(value))
    return false;
  out = value;
  return true;
}
// TimeArgument: a float followed by "d", "s", "t" or nothing, scaled to ticks.
bool parseTime(std::string_view token, int &out) {
  const std::string text(token);
  char *end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || !std::isfinite(value))
    return false;
  const std::string unit(end);
  const int factor = unit == "d" ? 24000
                     : unit == "s" ? 20
                     : unit == "t" ? 1
                     : unit.empty() ? 1
                                    : 0;
  if (factor == 0)
    return false;
  const double ticks = std::round(value * factor);
  if (ticks < std::numeric_limits<int>::min() ||
      ticks > std::numeric_limits<int>::max())
    return false;
  out = static_cast<int>(ticks);
  return true;
}
Node &literal(Node &parent, std::string name) {
  parent.children.push_back(std::make_unique<Node>());
  Node &node = *parent.children.back();
  node.kind = Node::Kind::Literal;
  node.name = std::move(name);
  return node;
}
Node &argument(Node &parent, std::string name, Node::ArgumentKind kind,
               double minimum, double maximum = 0) {
  parent.children.push_back(std::make_unique<Node>());
  Node &node = *parent.children.back();
  node.kind = Node::Kind::Argument;
  node.name = std::move(name);
  node.argumentKind = kind;
  node.minimum = minimum;
  node.maximum = maximum;
  return node;
}

// TickCommand.setTickingRate.
int setTickingRate(const Context &context, Source &source, float rate) {
  context.ticks->setTickRate(rate);
  source.success("commands.tick.rate.success", {oneDecimal(rate)}, true);
  return static_cast<int>(rate);
}
// TickCommand.tickQuery.
int tickQuery(const Context &context, Source &source) {
  const TickRateManager &manager = *context.ticks;
  const std::string busyTime = nanosToMilis(context.averageTickTimeNanos);
  const std::string tickRateString = oneDecimal(manager.tickrate());
  if (manager.isSprinting()) {
    source.success("commands.tick.status.sprinting");
    source.success("commands.tick.query.rate.sprinting",
                   {tickRateString, busyTime});
  } else {
    if (manager.isFrozen())
      source.success("commands.tick.status.frozen");
    else if (manager.nanosecondsPerTick() < context.averageTickTimeNanos)
      source.success("commands.tick.status.lagging");
    else
      source.success("commands.tick.status.running");
    source.success("commands.tick.query.rate.running",
                   {tickRateString, busyTime,
                    nanosToMilis(manager.nanosecondsPerTick())});
  }
  auto samples = context.tickTimes;
  std::sort(samples.begin(), samples.end());
  source.success("commands.tick.query.percentiles",
                 {nanosToMilis(samples[samples.size() / 2]),
                  nanosToMilis(samples[static_cast<std::size_t>(
                      samples.size() * 0.95)]),
                  nanosToMilis(samples[static_cast<std::size_t>(
                      samples.size() * 0.99)]),
                  std::to_string(samples.size())});
  return static_cast<int>(manager.tickrate());
}
// TickCommand.sprint.
int sprint(const Context &context, Source &source, int time) {
  if (context.ticks->requestGameToSprint(time))
    source.success("commands.tick.sprint.stop.success", {}, true);
  source.success("commands.tick.status.sprinting", {}, true);
  return 1;
}
// TickCommand.setFreeze.
int setFreeze(const Context &context, Source &source, bool freeze) {
  TickRateManager &manager = *context.ticks;
  if (freeze) {
    if (manager.isSprinting())
      manager.stopSprinting();
    if (manager.isSteppingForward())
      manager.stopStepping();
  }
  manager.setFrozen(freeze);
  source.success(freeze ? "commands.tick.status.frozen"
                        : "commands.tick.status.running",
                 {}, true);
  return freeze ? 1 : 0;
}
// TickCommand.step.
int step(const Context &context, Source &source, int advance) {
  if (context.ticks->stepGameIfPaused(advance))
    source.success("commands.tick.step.success", {std::to_string(advance)},
                   true);
  else
    source.failure("commands.tick.step.fail");
  return 1;
}
int stopStepping(const Context &context, Source &source) {
  if (context.ticks->stopStepping()) {
    source.success("commands.tick.step.stop.success", {}, true);
    return 1;
  }
  source.failure("commands.tick.step.stop.fail");
  return 0;
}
int stopSprinting(const Context &context, Source &source) {
  if (context.ticks->stopSprinting()) {
    source.success("commands.tick.sprint.stop.success", {}, true);
    return 1;
  }
  source.failure("commands.tick.sprint.stop.fail");
  return 0;
}
} // namespace

void Source::success(std::string key, std::vector<std::string> args,
                     bool broadcast) {
  messages.push_back(
      Message{std::move(key), std::move(args), false, broadcast});
}
void Source::failure(std::string key, std::vector<std::string> args) {
  messages.push_back(Message{std::move(key), std::move(args), true, false});
}

Dispatcher::Dispatcher() : root_(std::make_unique<Node>()) {}
Node &Dispatcher::root() { return *root_; }
const Node &Dispatcher::root() const { return *root_; }

int Dispatcher::run(std::string_view line, Source &source,
                    const Context &context) const {
  std::vector<std::string_view> tokens;
  for (std::size_t i = 0; i < line.size();) {
    while (i < line.size() && line[i] == ' ')
      ++i;
    const std::size_t start = i;
    while (i < line.size() && line[i] != ' ')
      ++i;
    if (i > start)
      tokens.push_back(line.substr(start, i - start));
  }
  if (tokens.empty())
    return -1;
  std::map<std::string, double> values;
  const Node *node = root_.get();
  for (const auto &token : tokens) {
    const Node *next = nullptr;
    for (const auto &child : node->children) {
      if (child->kind == Node::Kind::Literal && child->name == token) {
        next = child.get();
        break;
      }
    }
    if (!next) {
      for (const auto &child : node->children) {
        if (child->kind != Node::Kind::Argument)
          continue;
        if (child->requirement && !child->requirement(source))
          continue;
        double value = 0;
        if (child->argumentKind == Node::ArgumentKind::Float) {
          if (!parseFloat(token, value) || value < child->minimum ||
              value > child->maximum)
            continue;
        } else {
          int ticks = 0;
          if (!parseTime(token, ticks) || ticks < child->minimum)
            continue;
          value = ticks;
        }
        values[child->name] = value;
        next = child.get();
        break;
      }
    }
    if (!next)
      return -1;
    // Brigadier prunes children whose requirement fails, so an unreachable node
    // answers exactly like an unknown command.
    if (next->requirement && !next->requirement(source))
      return -1;
    node = next;
  }
  if (!node->executes)
    return -1;
  return node->executes(context, source, values);
}

Completion Dispatcher::suggestions(std::string_view line,
                                   const Source &source) const {
  // Brigadier parses the whole input, stops at the first error, and offers the
  // children of the deepest reachable node. The builder then keeps the entries
  // that start with the unfinished word.
  Completion completion;
  const std::size_t lastSpace = line.find_last_of(' ');
  const std::size_t wordStart =
      lastSpace == std::string_view::npos ? 0 : lastSpace + 1;
  const std::string_view word = line.substr(wordStart);
  completion.start = static_cast<int>(wordStart);
  completion.length = static_cast<int>(word.size());
  const std::string_view completed = line.substr(0, wordStart);

  std::vector<std::string_view> tokens;
  for (std::size_t i = 0; i < completed.size();) {
    while (i < completed.size() && completed[i] == ' ')
      ++i;
    const std::size_t start = i;
    while (i < completed.size() && completed[i] != ' ')
      ++i;
    if (i > start)
      tokens.push_back(completed.substr(start, i - start));
  }
  const Node *node = root_.get();
  if (node->requirement && !node->requirement(source))
    return completion;
  for (const auto &token : tokens) {
    const Node *next = nullptr;
    for (const auto &child : node->children) {
      if (child->kind != Node::Kind::Literal || child->name != token)
        continue;
      if (child->requirement && !child->requirement(source))
        continue;
      next = child.get();
      break;
    }
    if (!next) {
      for (const auto &child : node->children) {
        if (child->kind != Node::Kind::Argument)
          continue;
        if (child->requirement && !child->requirement(source))
          continue;
        bool accepted = false;
        if (child->argumentKind == Node::ArgumentKind::Float) {
          double value = 0;
          accepted = parseFloat(token, value) && value >= child->minimum &&
                     value <= child->maximum;
        } else {
          int ticks = 0;
          accepted = parseTime(token, ticks) && ticks >= child->minimum;
        }
        if (!accepted)
          continue;
        next = child.get();
        break;
      }
    }
    // An unterminated last word is not parsed here: the suggestion range covers
    // it, and brigadier stops at the first error anyway.
    if (!next)
      return completion;
    node = next;
  }
  for (const auto &child : node->children) {
    if (child->requirement && !child->requirement(source))
      continue;
    if (child->kind == Node::Kind::Literal) {
      if (child->name.starts_with(word))
        completion.texts.push_back(child->name);
      continue;
    }
    // ArgumentCommandNode.getCustomSuggestions: only the fixed examples are
    // offered, and only while the word can still become one of them.
    for (const auto &suggestion : child->suggestions)
      if (suggestion.starts_with(word))
        completion.texts.push_back(suggestion);
  }
  return completion;
}

void registerTickCommand(Dispatcher &dispatcher) {
  Node &tick = literal(dispatcher.root(), "tick");
  // Commands.hasPermission(Commands.LEVEL_ADMINS).
  tick.requirement = [](const Source &source) { return source.permissionLevel >= 3; };

  literal(tick, "query").executes =
      [](const Context &context, Source &source, const auto &) {
        return tickQuery(context, source);
      };

  Node &rate = literal(tick, "rate");
  Node &rateArg =
      argument(rate, "rate", Node::ArgumentKind::Float, 1.0, 10000.0);
  rateArg.suggestions = {"20"};
  rateArg.executes =
      [](const Context &context, Source &source, const auto &values) {
        return setTickingRate(context, source,
                              static_cast<float>(values.at("rate")));
      };

  Node &stepNode = literal(tick, "step");
  stepNode.executes = [](const Context &context, Source &source, const auto &) {
    return step(context, source, 1);
  };
  literal(stepNode, "stop").executes =
      [](const Context &context, Source &source, const auto &) {
        return stopStepping(context, source);
      };
  Node &stepTime = argument(stepNode, "time", Node::ArgumentKind::Time, 1);
  stepTime.suggestions = {"1t", "1s"};
  stepTime.executes =
      [](const Context &context, Source &source, const auto &values) {
        return step(context, source, static_cast<int>(values.at("time")));
      };

  Node &sprintNode = literal(tick, "sprint");
  literal(sprintNode, "stop").executes =
      [](const Context &context, Source &source, const auto &) {
        return stopSprinting(context, source);
      };
  Node &sprintTime =
      argument(sprintNode, "time", Node::ArgumentKind::Time, 1);
  sprintTime.suggestions = {"60s", "1d", "3d"};
  sprintTime.executes =
      [](const Context &context, Source &source, const auto &values) {
        return sprint(context, source, static_cast<int>(values.at("time")));
      };

  literal(tick, "unfreeze").executes =
      [](const Context &context, Source &source, const auto &) {
        return setFreeze(context, source, false);
      };
  literal(tick, "freeze").executes =
      [](const Context &context, Source &source, const auto &) {
        return setFreeze(context, source, true);
      };
}
} // namespace redstone::command
