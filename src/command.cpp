#include "redstone/command.hpp"
#include "redstone/storage.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
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
// Brigadier's StringReader.readFloat consumes only digits, '-', and '.'. It
// does not accept exponents, hexadecimal floats, signs like '+', NaN or
// Infinity. Parse the same lexical prefix, then require a finite float.
bool parseNumberPrefix(std::string_view token, float &out,
                       std::size_t &consumed) {
  consumed = 0;
  while (consumed < token.size()) {
    const char c = token[consumed];
    if ((c < '0' || c > '9') && c != '-' && c != '.')
      break;
    ++consumed;
  }
  if (consumed == 0)
    return false;
  const std::string text(token.substr(0, consumed));
  char *end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0' || !std::isfinite(value))
    return false;
  out = value;
  return true;
}
bool parseFloat(std::string_view token, float &out) {
  std::size_t consumed = 0;
  return parseNumberPrefix(token, out, consumed) && consumed == token.size();
}
bool normalizeIdentifier(std::string_view token, std::string &out) {
  if (token.empty())
    return false;
  const auto colon = token.find(':');
  if (colon != std::string_view::npos &&
      token.find(':', colon + 1) != std::string_view::npos)
    return false;
  const auto valid = [](char c, bool path) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '.' || (path && c == '/');
  };
  std::string_view ns, path;
  if (colon == std::string_view::npos) {
    ns = "minecraft";
    path = token;
  } else {
    ns = token.substr(0, colon);
    path = token.substr(colon + 1);
  }
  if (ns.empty() || path.empty() ||
      !std::all_of(ns.begin(), ns.end(), [&](char c) { return valid(c, false); }) ||
      !std::all_of(path.begin(), path.end(), [&](char c) { return valid(c, true); }))
    return false;
  out.assign(ns);
  out.push_back(':');
  out.append(path);
  return true;
}
int javaRoundFloat(float value) {
  if (std::isnan(value))
    return 0;
  const double rounded = std::floor(static_cast<double>(value) + 0.5);
  if (rounded <= std::numeric_limits<int>::min())
    return std::numeric_limits<int>::min();
  if (rounded >= std::numeric_limits<int>::max())
    return std::numeric_limits<int>::max();
  return static_cast<int>(rounded);
}
// TimeArgument: a float followed by "d", "s", "t" or nothing. The multiply
// remains float and Math.round(float) uses floor(value + 0.5), including its
// tie direction for negative values and int saturation.
bool parseTime(std::string_view token, int &out) {
  float value = 0;
  std::size_t consumed = 0;
  if (!parseNumberPrefix(token, value, consumed))
    return false;
  const std::string unit(token.substr(consumed));
  const int factor = unit == "d" ? 24000
                     : unit == "s" ? 20
                     : unit == "t" ? 1
                     : unit.empty() ? 1
                                    : 0;
  if (factor == 0)
    return false;
  out = javaRoundFloat(value * static_cast<float>(factor));
  return true;
}
bool resourceValue(const Node &node, std::string_view token, int &out) {
  std::string key(token);
  auto found = node.resourceValues.find(key);
  if (found == node.resourceValues.end() && key.find(':') == std::string::npos)
    found = node.resourceValues.find("minecraft:" + key);
  if (found == node.resourceValues.end())
    return false;
  out = found->second;
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
struct TimelineDefinition {
  const char *id;
  BuiltinWorldClock clock;
  int periodTicks; // zero means no period (absolute ticks)
};
constexpr std::array<TimelineDefinition, 4> kTimelines{{
    {"minecraft:day", BuiltinWorldClock::Overworld, 24000},
    {"minecraft:early_game", BuiltinWorldClock::Overworld, 0},
    {"minecraft:moon", BuiltinWorldClock::Overworld, 24000 * 8},
    {"minecraft:villager_schedule", BuiltinWorldClock::Overworld, 24000},
}};
struct MarkerDefinition {
  const char *id;
  BuiltinWorldClock clock;
  int ticks;
  int periodTicks;
  bool showInCommands;
};
// All six registered 26.1 markers are supplied by the overworld day timeline.
constexpr std::array<MarkerDefinition, 6> kTimeMarkers{{
    {"minecraft:day", BuiltinWorldClock::Overworld, 1000, 24000, true},
    {"minecraft:noon", BuiltinWorldClock::Overworld, 6000, 24000, true},
    {"minecraft:night", BuiltinWorldClock::Overworld, 13000, 24000, true},
    {"minecraft:midnight", BuiltinWorldClock::Overworld, 18000, 24000, true},
    {"minecraft:wake_up_from_sleep", BuiltinWorldClock::Overworld, 0, 24000,
     false},
    {"minecraft:roll_village_siege", BuiltinWorldClock::Overworld, 18000,
     24000, false},
}};
BuiltinWorldClock clockFrom(const std::map<std::string, double> &values) {
  const auto found = values.find("clock");
  const int index = found == values.end() ? 0 : static_cast<int>(found->second);
  return index == 1 ? BuiltinWorldClock::TheEnd : BuiltinWorldClock::Overworld;
}
const char *clockName(BuiltinWorldClock clock) {
  return clock == BuiltinWorldClock::TheEnd ? "minecraft:the_end"
                                             : "minecraft:overworld";
}
void clockChanged(const Context &context, BuiltinWorldClock clock) {
  if (context.worldClockChanged)
    context.worldClockChanged(clock);
}
int wrapTime(std::int64_t value) {
  return static_cast<int>(value % 2147483647LL);
}
const TimelineDefinition &timelineFrom(
    const std::map<std::string, double> &values) {
  return kTimelines.at(static_cast<std::size_t>(values.at("timeline")));
}
const MarkerDefinition *markerById(std::string_view id,
                                   BuiltinWorldClock clock) {
  for (const MarkerDefinition &marker : kTimeMarkers)
    if (marker.clock == clock && marker.id == id)
      return &marker;
  return nullptr;
}
std::int64_t moveToMarker(std::int64_t totalTicks,
                          const MarkerDefinition &marker) {
  if (marker.periodTicks == 0)
    return marker.ticks;
  const std::int64_t from = totalTicks % marker.periodTicks;
  const std::int64_t forward = marker.ticks - from;
  const std::int64_t offset =
      forward > 0 ? forward : marker.periodTicks + forward;
  // ClockTimeMarker.resolveTimeToMoveTo uses Java long addition. Preserve its
  // modulo-2^64 wrap instead of invoking C++ signed-overflow UB.
  return static_cast<std::int64_t>(
      static_cast<std::uint64_t>(totalTicks) +
      static_cast<std::uint64_t>(offset));
}
std::vector<std::string> visibleMarkerSuggestions(
    const std::map<std::string, double> &values) {
  const BuiltinWorldClock clock = clockFrom(values);
  std::vector<std::string> result;
  for (const MarkerDefinition &marker : kTimeMarkers)
    if (marker.clock == clock && marker.showInCommands)
      result.emplace_back(marker.id);
  return result;
}
std::vector<std::string> timelineSuggestions(
    const std::map<std::string, double> &values) {
  const BuiltinWorldClock clock = clockFrom(values);
  std::vector<std::string> result;
  for (const TimelineDefinition &timeline : kTimelines)
    if (timeline.clock == clock)
      result.emplace_back(timeline.id);
  return result;
}
int timeSetMarker(const Context &context, Source &source,
                  const std::map<std::string, double> &values,
                  const std::map<std::string, std::string> &strings) {
  if (!context.worldClocks)
    return 0;
  const BuiltinWorldClock clock = clockFrom(values);
  const std::string &id = strings.at("timemarker");
  const MarkerDefinition *marker = markerById(id, clock);
  if (!marker) {
    source.failure("commands.time.no_time_marker_found",
                   {"ResourceKey[minecraft:clock_time_marker / " + id + "]",
                    clockName(clock)});
    return 0;
  }
  WorldClockState state = context.worldClocks->state(clock);
  state.totalTicks = moveToMarker(state.totalTicks, *marker);
  state.partialTick = 0.0f;
  (void)context.worldClocks->setClock(clock, state);
  clockChanged(context, clock);
  source.success("commands.time.set.time_marker",
                 {clockName(clock), id}, true);
  return wrapTime(state.totalTicks);
}
int timeQueryTimeline(const Context &context, Source &source,
                      const std::map<std::string, double> &values) {
  if (!context.worldClocks)
    return 0;
  const BuiltinWorldClock clock = clockFrom(values);
  const TimelineDefinition &timeline = timelineFrom(values);
  if (timeline.clock != clock) {
    source.failure("commands.time.wrong_timeline_for_clock",
                   {timeline.id, clockName(clock)});
    return 0;
  }
  const std::int64_t total = context.worldClocks->state(clock).totalTicks;
  const std::int64_t current =
      timeline.periodTicks ? total % timeline.periodTicks : total;
  source.success("commands.time.query.timeline",
                 {timeline.id, std::to_string(current)});
  return wrapTime(current);
}
int timeQueryTimelineRepetitions(
    const Context &context, Source &source,
    const std::map<std::string, double> &values) {
  if (!context.worldClocks)
    return 0;
  const BuiltinWorldClock clock = clockFrom(values);
  const TimelineDefinition &timeline = timelineFrom(values);
  if (timeline.clock != clock) {
    source.failure("commands.time.wrong_timeline_for_clock",
                   {timeline.id, clockName(clock)});
    return 0;
  }
  std::int64_t repetitions = 0;
  if (timeline.periodTicks) {
    const std::int64_t quotient =
        context.worldClocks->state(clock).totalTicks / timeline.periodTicks;
    // Timeline.getPeriodCount returns an int narrowed from the long quotient;
    // queryTimelineRepetitions then sign-extends that int back to long.
    const auto bits = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(quotient));
    repetitions = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(bits));
  }
  source.success("commands.time.query.timeline.repetitions",
                 {timeline.id, std::to_string(repetitions)});
  return wrapTime(repetitions);
}
int timeQueryGameTime(const Context &context, Source &source) {
  if (!context.world)
    return 0;
  const std::int64_t time = context.world->gameTime();
  source.success("commands.time.query.gametime", {std::to_string(time)});
  return wrapTime(time);
}
int timeQuery(const Context &context, Source &source,
              const std::map<std::string, double> &values) {
  if (!context.worldClocks)
    return 0;
  const BuiltinWorldClock clock = clockFrom(values);
  const std::int64_t total = context.worldClocks->state(clock).totalTicks;
  source.success("commands.time.query.absolute",
                 {clockName(clock), std::to_string(total)});
  return wrapTime(total);
}
int timeSet(const Context &context, Source &source,
            const std::map<std::string, double> &values) {
  if (!context.worldClocks)
    return 0;
  const BuiltinWorldClock clock = clockFrom(values);
  WorldClockState state = context.worldClocks->state(clock);
  const int ticks = static_cast<int>(values.at("time"));
  state.totalTicks = ticks;
  state.partialTick = 0.0f;
  (void)context.worldClocks->setClock(clock, state);
  clockChanged(context, clock);
  source.success("commands.time.set.absolute",
                 {clockName(clock), std::to_string(ticks)}, true);
  return ticks;
}
int timeAdd(const Context &context, Source &source,
            const std::map<std::string, double> &values) {
  if (!context.worldClocks)
    return 0;
  const BuiltinWorldClock clock = clockFrom(values);
  WorldClockState state = context.worldClocks->state(clock);
  const int delta = static_cast<int>(values.at("time"));
  state.totalTicks = static_cast<std::int64_t>(
      static_cast<std::uint64_t>(state.totalTicks) +
      static_cast<std::uint64_t>(static_cast<std::int64_t>(delta)));
  if (state.totalTicks < 0)
    state.totalTicks = 0;
  (void)context.worldClocks->setClock(clock, state);
  clockChanged(context, clock);
  source.success("commands.time.set.absolute",
                 {clockName(clock), std::to_string(state.totalTicks)}, true);
  return wrapTime(state.totalTicks);
}
int timePause(const Context &context, Source &source,
              const std::map<std::string, double> &values, bool paused) {
  if (!context.worldClocks)
    return 0;
  const BuiltinWorldClock clock = clockFrom(values);
  WorldClockState state = context.worldClocks->state(clock);
  state.paused = paused;
  (void)context.worldClocks->setClock(clock, state);
  clockChanged(context, clock);
  source.success(paused ? "commands.time.pause" : "commands.time.resume",
                 {clockName(clock)}, true);
  return 1;
}
std::string floatText(float value) {
  char buffer[64];
  const auto converted = std::to_chars(
      buffer, buffer + sizeof buffer, value, std::chars_format::general);
  std::string text(buffer, converted.ptr);
  const auto exponent = text.find_first_of("eE");
  if (exponent == std::string::npos) {
    if (text.find('.') == std::string::npos)
      text += ".0";
    return text;
  }
  std::string mantissa = text.substr(0, exponent);
  if (mantissa.find('.') == std::string::npos)
    mantissa += ".0";
  std::string power = text.substr(exponent + 1);
  const bool negative = !power.empty() && power.front() == '-';
  if (!power.empty() && (power.front() == '-' || power.front() == '+'))
    power.erase(power.begin());
  while (power.size() > 1 && power.front() == '0')
    power.erase(power.begin());
  return mantissa + "E" + (negative ? "-" : "") + power;
}
int timeRate(const Context &context, Source &source,
             const std::map<std::string, double> &values) {
  if (!context.worldClocks)
    return 0;
  const BuiltinWorldClock clock = clockFrom(values);
  WorldClockState state = context.worldClocks->state(clock);
  state.rate = static_cast<float>(values.at("rate"));
  (void)context.worldClocks->setClock(clock, state);
  clockChanged(context, clock);
  source.success("commands.time.rate",
                 {clockName(clock), floatText(state.rate)}, true);
  return 1;
}
Node &ensureLiteral(Node &parent, const std::string &name) {
  for (auto &child : parent.children)
    if (child->kind == Node::Kind::Literal && child->name == name)
      return *child;
  return literal(parent, name);
}
void addClockActions(Node &parent) {
  Node &set = ensureLiteral(parent, "set");
  Node &setTime = argument(set, "time", Node::ArgumentKind::Time, 0);
  setTime.suggestions = {"0d", "0s", "0t", "0"};
  setTime.executes = [](const Context &context, Source &source,
                        const auto &values) {
    return timeSet(context, source, values);
  };
  Node &marker = argument(set, "timemarker",
                          Node::ArgumentKind::Identifier, 0);
  marker.dynamicSuggestions = visibleMarkerSuggestions;
  marker.executesWithStrings =
      [](const Context &context, Source &source, const auto &values,
         const auto &strings) {
        return timeSetMarker(context, source, values, strings);
      };

  Node &add = ensureLiteral(parent, "add");
  Node &addTime =
      argument(add, "time", Node::ArgumentKind::Time,
               static_cast<double>(std::numeric_limits<int>::min()));
  addTime.suggestions = {"0d", "0s", "0t", "0"};
  addTime.executes = [](const Context &context, Source &source,
                        const auto &values) {
    return timeAdd(context, source, values);
  };

  ensureLiteral(parent, "pause").executes =
      [](const Context &context, Source &source, const auto &values) {
        return timePause(context, source, values, true);
      };
  ensureLiteral(parent, "resume").executes =
      [](const Context &context, Source &source, const auto &values) {
        return timePause(context, source, values, false);
      };

  Node &rate = ensureLiteral(parent, "rate");
  Node &rateValue =
      argument(rate, "rate", Node::ArgumentKind::Float, 1.0e-5, 1000.0);
  rateValue.suggestions = {"1.0"};
  rateValue.executes = [](const Context &context, Source &source,
                          const auto &values) {
    return timeRate(context, source, values);
  };

  Node &query = ensureLiteral(parent, "query");
  ensureLiteral(query, "time").executes =
      [](const Context &context, Source &source, const auto &values) {
        return timeQuery(context, source, values);
      };
  Node &timeline =
      argument(query, "timeline", Node::ArgumentKind::Resource, 0);
  timeline.resourceRegistry = "minecraft:timeline";
  timeline.resourceValues = {
      {"minecraft:day", 0},
      {"minecraft:early_game", 1},
      {"minecraft:moon", 2},
      {"minecraft:villager_schedule", 3},
  };
  timeline.dynamicSuggestions = timelineSuggestions;
  timeline.executes = [](const Context &context, Source &source,
                         const auto &values) {
    return timeQueryTimeline(context, source, values);
  };
  ensureLiteral(timeline, "repetition").executes =
      [](const Context &context, Source &source, const auto &values) {
        return timeQueryTimelineRepetitions(context, source, values);
      };
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
  std::map<std::string, std::string> strings;
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
          float parsed = 0;
          if (!parseFloat(token, parsed) ||
              parsed < static_cast<float>(child->minimum) ||
              parsed > static_cast<float>(child->maximum))
            continue;
          value = parsed;
        } else if (child->argumentKind == Node::ArgumentKind::Time) {
          int ticks = 0;
          if (!parseTime(token, ticks) || ticks < child->minimum)
            continue;
          value = ticks;
        } else if (child->argumentKind == Node::ArgumentKind::Resource) {
          int resource = 0;
          if (!resourceValue(*child, token, resource))
            continue;
          value = resource;
        } else {
          std::string identifier;
          if (!normalizeIdentifier(token, identifier))
            continue;
          strings[child->name] = std::move(identifier);
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
  if (node->executesWithStrings)
    return node->executesWithStrings(context, source, values, strings);
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
  std::map<std::string, double> values;
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
          float value = 0;
          accepted = parseFloat(token, value) &&
                     value >= static_cast<float>(child->minimum) &&
                     value <= static_cast<float>(child->maximum);
        } else if (child->argumentKind == Node::ArgumentKind::Time) {
          int ticks = 0;
          accepted = parseTime(token, ticks) && ticks >= child->minimum;
        } else if (child->argumentKind == Node::ArgumentKind::Resource) {
          int resource = 0;
          accepted = resourceValue(*child, token, resource);
        } else {
          std::string identifier;
          accepted = normalizeIdentifier(token, identifier);
        }
        if (!accepted)
          continue;
        if (child->argumentKind == Node::ArgumentKind::Float ||
            child->argumentKind == Node::ArgumentKind::Time) {
          // The executed command reparses the completed word; suggestions only
          // need it to select the node path.
        } else if (child->argumentKind == Node::ArgumentKind::Resource) {
          int resource = 0;
          (void)resourceValue(*child, token, resource);
          values[child->name] = resource;
        }
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
    // Resource-dependent and fixed suggestions are filtered against the
    // unfinished word. Marker/timeline lists depend on the already selected
    // clock.
    const auto candidates = child->dynamicSuggestions
                                ? child->dynamicSuggestions(values)
                                : child->suggestions;
    for (const auto &suggestion : candidates)
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

void registerTimeCommand(Dispatcher &dispatcher) {
  Node &time = literal(dispatcher.root(), "time");
  // Commands.hasPermission(Commands.LEVEL_GAMEMASTERS).
  time.requirement = [](const Source &source) {
    return source.permissionLevel >= 2;
  };
  addClockActions(time);

  Node &query = ensureLiteral(time, "query");
  ensureLiteral(query, "gametime").executes =
      [](const Context &context, Source &source, const auto &) {
        return timeQueryGameTime(context, source);
      };

  Node &of = literal(time, "of");
  Node &clock = argument(of, "clock", Node::ArgumentKind::Resource, 0);
  clock.resourceRegistry = "minecraft:world_clock";
  clock.resourceValues = {{"minecraft:overworld", 0},
                          {"minecraft:the_end", 1}};
  clock.suggestions = {"minecraft:overworld", "minecraft:the_end"};
  addClockActions(clock);
}
} // namespace redstone::command
