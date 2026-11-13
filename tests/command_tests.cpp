// /tick and its command skeleton. The dispatcher, the tick rate manager and the
// clientbound command tree are checked against the vanilla TickCommand /
// ServerTickRateManager behaviour this simulator mirrors.
#include "redstone/command.hpp"
#include "redstone/net/wire.hpp"
#include "redstone/storage.hpp"
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace redstone;
using namespace redstone::command;

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
bool has(const Source &source, const std::string &key) {
  for (const auto &message : source.messages)
    if (message.key == key)
      return true;
  return false;
}
const Message &find(const Source &source, const std::string &key) {
  for (const auto &message : source.messages)
    if (message.key == key)
      return message;
  throw std::runtime_error("missing message " + key);
}
Source admin() {
  Source source;
  source.permissionLevel = 4;
  source.name = "tester";
  return source;
}
struct Fixture {
  Dispatcher dispatcher;
  TickRateManager ticks;
  Context context;
  Fixture() {
    registerTickCommand(dispatcher);
    context.ticks = &ticks;
  }
  int run(Source &source, const std::string &line) {
    source.messages.clear();
    return dispatcher.run(line, source, context);
  }
};

void tickRateManager() {
  TickRateManager ticks;
  require(ticks.tickrate() == 20.0f, "default tick rate is 20");
  require(ticks.nanosecondsPerTick() == 50000000,
          "20 tps is 50 ms per tick");
  ticks.setTickRate(0.5f);
  require(ticks.tickrate() == 1.0f, "TickRateManager clamps to 1 tps");
  require(ticks.nanosecondsPerTick() == 1000000000,
          "1 tps is one second per tick");
  ticks.setTickRate(40.0f);
  require(ticks.nanosecondsPerTick() == 25000000, "40 tps is 25 ms per tick");
  require(std::abs(ticks.millisecondsPerTick() - 25.0f) < 1e-4f,
          "millisecondsPerTick matches nanosecondsPerTick");

  // Freeze: no tick runs until /tick step hands out a budget.
  ticks.setFrozen(true);
  ticks.tick();
  require(!ticks.runsNormally(), "a frozen server does not run game elements");
  require(ticks.stepGameIfPaused(2), "step is accepted while frozen");
  require(ticks.isSteppingForward(), "stepping state is visible");
  ticks.tick();
  require(ticks.runsNormally(), "the first stepped tick runs");
  require(ticks.frozenTicksToRun() == 1, "step budget counts down");
  ticks.tick();
  require(ticks.runsNormally(), "the second stepped tick runs");
  ticks.tick();
  require(!ticks.runsNormally(), "stepping ends after the budget");
  require(ticks.stepGameIfPaused(1), "step works while frozen");
  require(ticks.stopStepping(), "stop clears a pending step");
  require(!ticks.stopStepping(), "stopping twice reports failure");
  ticks.setFrozen(false);
  ticks.tick();
  require(ticks.runsNormally(), "unfreezing resumes normal ticks");
  require(!ticks.stepGameIfPaused(1), "step is rejected while running");
}

void tickRateCommands() {
  Fixture fixture;
  Source source = admin();

  require(fixture.run(source, "tick") == -1, "bare /tick is incomplete");
  require(fixture.run(source, "tick query") >= 0, "/tick query runs");
  require(has(source, "commands.tick.status.running"),
          "query reports the running state");
  require(has(source, "commands.tick.query.rate.running"),
          "query reports the rate line");
  const Message &percentiles =
      find(source, "commands.tick.query.percentiles");
  require(percentiles.args.size() == 4 && percentiles.args[3] == "100",
          "percentiles report the 100-sample window");
  require(!percentiles.broadcast, "query output is not broadcast");

  require(fixture.run(source, "tick rate 20.5") == 20,
          "rate returns the integer tick rate");
  require(std::abs(fixture.ticks.tickrate() - 20.5f) < 1e-4f,
          "rate sets the tick rate");
  const Message &rate = find(source, "commands.tick.rate.success");
  require(rate.args.size() == 1 && rate.args[0] == "20.5",
          "rate message carries one decimal");
  require(rate.broadcast, "rate success is broadcast like vanilla");
  require(fixture.run(source, "tick rate 0") == -1,
          "rate below 1 is rejected");
  require(fixture.run(source, "tick rate 10001") == -1,
          "rate above 10000 is rejected");
  require(fixture.run(source, "tick rate abc") == -1,
          "rate rejects non-numbers");

  // step needs a frozen server.
  require(fixture.run(source, "tick step") == 1, "step always returns 1");
  require(find(source, "commands.tick.step.fail").failure,
          "step fails while the server runs");
  require(fixture.run(source, "tick freeze") == 1, "freeze returns 1");
  require(fixture.ticks.isFrozen(), "freeze freezes the manager");
  require(has(source, "commands.tick.status.frozen"),
          "freeze reports the frozen state");

  require(fixture.run(source, "tick step 5s") == 1, "step accepts a time");
  require(fixture.ticks.frozenTicksToRun() == 100,
          "5s is 100 ticks");
  require(find(source, "commands.tick.step.success").args[0] == "100",
          "step message reports the tick count");
  require(fixture.run(source, "tick step 1.5t") == 1,
          "a fractional tick count rounds");
  require(fixture.ticks.frozenTicksToRun() == 2, "1.5t rounds to 2 ticks");
  require(fixture.run(source, "tick step 0") == -1,
          "step rejects zero ticks");
  require(fixture.run(source, "tick step 0d") == -1,
          "step rejects a zero day");
  require(fixture.run(source, "tick step stop") == 1, "step stop works");
  require(fixture.ticks.frozenTicksToRun() == 0, "step stop clears the budget");
  require(fixture.run(source, "tick step stop") == 0,
          "step stop reports failure when nothing is pending");
  require(find(source, "commands.tick.step.stop.fail").failure,
          "the failure is a failure message");

  require(fixture.run(source, "tick unfreeze") == 0, "unfreeze returns 0");
  require(!fixture.ticks.isFrozen(), "unfreeze resumes");
  require(has(source, "commands.tick.status.running"),
          "unfreeze reports running");

  // sprint runs ticks as fast as possible for a budget.
  require(fixture.run(source, "tick sprint 60s") == 1, "sprint starts");
  require(fixture.ticks.isSprinting(), "sprint is visible");
  require(fixture.ticks.scheduledSprintTicks() == 1200, "60s is 1200 ticks");
  require(has(source, "commands.tick.status.sprinting"),
          "sprint reports the sprinting state");
  fixture.ticks.tick();
  require(fixture.ticks.checkShouldSprintThisTick(), "sprint ticks run");
  require(fixture.ticks.remainingSprintTicks() == 1199,
          "the sprint budget counts down");
  require(fixture.run(source, "tick query") >= 0, "query during a sprint");
  require(has(source, "commands.tick.query.rate.sprinting"),
          "query reports the sprinting rate line");
  require(fixture.run(source, "tick sprint stop") == 1, "sprint stop works");
  require(!fixture.ticks.isSprinting(), "sprint stop ends the sprint");
  const auto report = fixture.ticks.takeSprintReport();
  require(report.has_value(), "finishing a sprint produces a report");
  require(report->ticksPerSecond >= 0, "the report carries a tick rate");
  require(fixture.run(source, "tick sprint stop") == 0,
          "stopping a finished sprint fails");
  require(!fixture.ticks.takeSprintReport().has_value(),
          "the report is only handed out once");

  // Freezing cancels a sprint and a pending step, like TickCommand.setFreeze.
  fixture.ticks.setFrozen(false);
  require(fixture.run(source, "tick sprint 1s") == 1, "sprint restarts");
  require(fixture.run(source, "tick freeze") == 1, "freeze cancels a sprint");
  require(!fixture.ticks.isSprinting(), "freeze stopped the sprint");
}

void suggestions() {
  Fixture fixture;
  Source source = admin();
  const auto texts = [&](std::string_view line) {
    Source local = source;
    return fixture.dispatcher.suggestions(line, local);
  };

  // Top level: only /tick, and only for a source that may use it.
  auto empty = texts("");
  require(empty.start == 0 && empty.length == 0,
          "an empty line replaces nothing");
  require(empty.texts.size() == 1 && empty.texts[0] == "tick",
          "the only command is tick");
  auto partial = texts("ti");
  require(partial.start == 0 && partial.length == 2,
          "the range covers the typed word");
  require(partial.texts.size() == 1 && partial.texts[0] == "tick",
          "a prefix narrows to tick");
  Source low;
  low.permissionLevel = 0;
  require(fixture.dispatcher.suggestions("", low).texts.empty(),
          "an unprivileged source gets no suggestions");

  // Subcommands of /tick, including the ones a partial word filters out.
  auto subs = texts("tick ");
  require(subs.start == 5 && subs.length == 0,
          "the range starts after the space");
  require(subs.texts.size() == 6, "tick has six subcommands");
  require(std::find(subs.texts.begin(), subs.texts.end(), "query") !=
              subs.texts.end(),
          "query is suggested");
  auto narrowed = texts("tick f");
  require(narrowed.start == 5 && narrowed.length == 1,
          "the range covers the partial subcommand");
  require(narrowed.texts.size() == 1 && narrowed.texts[0] == "freeze",
          "a prefix narrows the subcommands");

  // Argument nodes offer the vanilla example values.
  auto rate = texts("tick rate ");
  require(rate.texts.size() == 1 && rate.texts[0] == "20",
          "/tick rate suggests 20");
  auto stepTime = texts("tick step ");
  // Literals are listed before the argument example values.
  require(stepTime.texts.size() == 3 && stepTime.texts[0] == "stop" &&
              stepTime.texts[1] == "1t" && stepTime.texts[2] == "1s",
          "/tick step suggests stop plus the 1t/1s examples");
  auto sprintTime = texts("tick sprint ");
  require(sprintTime.texts.size() == 4 && sprintTime.texts[0] == "stop" &&
              sprintTime.texts[1] == "60s" && sprintTime.texts[2] == "1d" &&
              sprintTime.texts[3] == "3d",
          "/tick sprint suggests stop plus 60s, 1d and 3d");
  // "stop" is a literal sibling, so it is offered next to the time examples.
  require(std::find(stepTime.texts.begin(), stepTime.texts.end(), "stop") !=
              stepTime.texts.end(),
          "a literal sibling is suggested beside the argument examples");
  require(texts("tick step st").texts.size() == 1 &&
              texts("tick step st").texts[0] == "stop",
          "a prefix narrows to stop");
  // A partially typed example still matches itself, like brigadier's prefix
  // filter over the argument's own suggestions.
  auto typed = texts("tick rate 2");
  require(typed.texts.size() == 1 && typed.texts[0] == "20",
          "a typed prefix narrows the argument example");
  // A complete literal command has nothing after it.
  require(texts("tick freeze ").texts.empty(),
          "nothing follows a leaf command");

  // Unknown prefixes and unparsable text produce no candidates.
  require(texts("bogus ").texts.empty(), "unknown commands suggest nothing");
  require(texts("tick bogus ").texts.empty(),
          "an unknown subcommand stops the walk");
}

struct TimeFixture {
  Dispatcher dispatcher;
  World world;
  WorldClocks clocks;
  Context context;
  std::vector<BuiltinWorldClock> changed;
  TimeFixture() {
    registerTimeCommand(dispatcher);
    world.setGameTime(1234);
    context.world = &world;
    context.worldClocks = &clocks;
    context.worldClockChanged = [this](BuiltinWorldClock clock) {
      changed.push_back(clock);
    };
  }
  int run(Source &source, const std::string &line) {
    source.messages.clear();
    changed.clear();
    return dispatcher.run(line, source, context);
  }
};

void timeCommands() {
  TimeFixture fixture;
  Source player;
  player.permissionLevel = 1;
  require(fixture.run(player, "time query gametime") == -1,
          "/time requires gamemaster permission");
  require(fixture.changed.empty(), "denied command changes no clock");

  Source source = admin();
  require(fixture.run(source, "time query gametime") == 1234,
          "timestamp return is gameTime modulo int max");
  require(find(source, "commands.time.query.gametime").args ==
              std::vector<std::string>{"1234"},
          "gameTime uses its own query message");
  require(fixture.changed.empty(), "query does not broadcast a clock update");

  WorldClockState overworld;
  overworld.totalTicks = 100;
  overworld.partialTick = 0.75f;
  overworld.rate = 2.0f;
  fixture.clocks.setClock(BuiltinWorldClock::Overworld, overworld);
  require(fixture.run(source, "time query time") >= 0,
          "default time query succeeds");
  require(find(source, "commands.time.query.absolute").args ==
              std::vector<std::string>{"minecraft:overworld", "100"},
          "default query resolves the overworld clock");
  require(fixture.changed.empty(), "clock query has no side effect");

  require(fixture.run(source, "time set 0.5t") == 1,
          "0.5t uses Java Math.round rounding");
  require(fixture.clocks.state(BuiltinWorldClock::Overworld).totalTicks == 1 &&
              fixture.clocks.state(BuiltinWorldClock::Overworld).partialTick ==
                  0.0f,
          "set replaces total and clears partial");
  require(fixture.changed ==
              std::vector<BuiltinWorldClock>{BuiltinWorldClock::Overworld},
          "set broadcasts exactly the changed clock");
  require(find(source, "commands.time.set.absolute").args ==
              std::vector<std::string>{"minecraft:overworld", "1"},
          "set reports the new total");

  overworld = fixture.clocks.state(BuiltinWorldClock::Overworld);
  overworld.partialTick = 0.25f;
  fixture.clocks.setClock(BuiltinWorldClock::Overworld, overworld);
  require(fixture.run(source, "time add -0.5t") >= 0,
          "negative half tick rounds to zero");
  require(fixture.clocks.state(BuiltinWorldClock::Overworld).totalTicks == 1 &&
              fixture.clocks.state(BuiltinWorldClock::Overworld).partialTick ==
                  0.25f,
          "add preserves partial and applies Java negative-half rounding");
  require(fixture.run(source, "time add -100000t") >= 0,
          "large negative add is accepted by the parser");
  require(fixture.clocks.state(BuiltinWorldClock::Overworld).totalTicks == 0 &&
              fixture.clocks.state(BuiltinWorldClock::Overworld).partialTick ==
                  0.25f,
          "add clamps total to zero and keeps partial");

  require(fixture.run(source, "time rate 0.00001") == 1,
          "minimum rate is accepted");
  require(find(source, "commands.time.rate").args[1] == "1.0E-5",
          "rate message uses Java float text");
  require(fixture.clocks.state(BuiltinWorldClock::Overworld).rate == 0.00001f,
          "minimum rate is stored as f32");
  require(fixture.run(source, "time rate 0.000009") == -1,
          "rate below the minimum is rejected");
  require(fixture.run(source, "time rate 1000") == 1,
          "maximum rate is accepted");
  require(find(source, "commands.time.rate").args[1] == "1000.0",
          "integral rate retains Java .0 suffix");
  require(fixture.run(source, "time rate 1000.1") == -1,
          "rate above the maximum is rejected");
  require(fixture.run(source, "time pause") == 1 &&
              fixture.clocks.state(BuiltinWorldClock::Overworld).paused,
          "pause affects the default clock");
  require(fixture.run(source, "time resume") == 1 &&
              !fixture.clocks.state(BuiltinWorldClock::Overworld).paused,
          "resume clears pause");

  require(fixture.run(source, "time of minecraft:the_end set 2d") == 48000,
          "namespace-qualified explicit clock set");
  require(fixture.run(source, "time of the_end add -1s") >= 0,
          "resource shorthand resolves the_end");
  require(fixture.clocks.state(BuiltinWorldClock::TheEnd).totalTicks == 47980,
          "explicit add updates only the_end");
  require(fixture.run(source, "time of minecraft:the_end query time") >= 0 &&
              find(source, "commands.time.query.absolute").args ==
                  std::vector<std::string>{"minecraft:the_end", "47980"},
          "explicit clock query reports the_end");
  require(fixture.run(source, "time of minecraft:overworld query time") >= 0 &&
              find(source, "commands.time.query.absolute").args ==
                  std::vector<std::string>{"minecraft:overworld", "0"},
          "explicit overworld query remains separate");

  require(fixture.run(source, "time set -1") == 0 &&
              has(source, "commands.time.no_time_marker_found"),
          "a negative duration can only fall through to the marker identifier");
  require(fixture.run(source, "time add 1m") == -1,
          "unknown duration unit is rejected");
  require(fixture.run(source, "time of minecraft:bad set 1") == -1,
          "unknown world clock is rejected");
}

void timeMarkersAndTimelines() {
  TimeFixture fixture;
  Source source = admin();
  WorldClockState overworld;
  overworld.totalTicks = 12000;
  overworld.partialTick = 0.75f;
  overworld.rate = 1.0f;
  fixture.clocks.setClock(BuiltinWorldClock::Overworld, overworld);

  require(fixture.dispatcher.suggestions("time set ", source).texts ==
              (std::vector<std::string>{
                  "0d", "0s", "0t", "0", "minecraft:day", "minecraft:noon",
                  "minecraft:night", "minecraft:midnight"}),
          "marker suggestions expose only the four visible built-ins");
  const auto endSuggestions = fixture.dispatcher.suggestions(
      "time of minecraft:the_end set ", source).texts;
  require(std::find(endSuggestions.begin(), endSuggestions.end(),
                    "minecraft:noon") == endSuggestions.end(),
          "the End exposes no Overworld marker suggestions");

  require(fixture.run(source, "time query minecraft:day") == 12000,
          "timeline query returns current ticks");
  require(find(source, "commands.time.query.timeline").args ==
              std::vector<std::string>{"minecraft:day", "12000"},
          "timeline query message args");
  require(fixture.run(source, "time query day repetition") == 0,
          "day timeline repetition count");
  require(find(source, "commands.time.query.timeline.repetitions").args ==
              std::vector<std::string>{"minecraft:day", "0"},
          "timeline repetition message args");

  require(fixture.run(source, "time set noon") == 30000,
          "visible marker moves to its next occurrence");
  require(fixture.clocks.state(BuiltinWorldClock::Overworld).partialTick == 0.0f,
          "marker move clears partial");
  require(fixture.run(source, "time set noon") == 54000,
          "same marker at its position advances one full period");
  require(fixture.run(source, "time set wake_up_from_sleep") == 72000,
          "hidden marker remains directly usable");
  require(fixture.run(source, "time set roll_village_siege") == 90000,
          "hidden village marker remains directly usable");
  require(fixture.run(source, "time set nope") == 0 &&
              has(source, "commands.time.no_time_marker_found"),
          "unknown marker returns the official no-marker failure");

  overworld = fixture.clocks.state(BuiltinWorldClock::Overworld);
  overworld.totalTicks = 192001;
  overworld.partialTick = 0.5f;
  fixture.clocks.setClock(BuiltinWorldClock::Overworld, overworld);
  require(fixture.run(source, "time query moon") == 1,
          "moon timeline current ticks use its 192000 period");
  require(fixture.run(source, "time query moon repetition") == 1,
          "moon timeline repetition count");
  require(fixture.run(source, "time query early_game") == 192001,
          "periodless timeline reports total ticks");
  require(fixture.run(source, "time query early_game repetition") == 0,
          "periodless timeline has zero repetitions");
  require(fixture.run(source, "time of minecraft:overworld query moon") == 1,
          "explicit overworld can query matching timeline");
  require(fixture.run(source, "time query day repetition") == 8,
          "day timeline counts completed periods");

  require(fixture.run(source, "time of minecraft:the_end query day") == 0 &&
              has(source, "commands.time.wrong_timeline_for_clock"),
          "End rejects an Overworld timeline");
  require(find(source, "commands.time.wrong_timeline_for_clock").args ==
              std::vector<std::string>{"minecraft:day", "minecraft:the_end"},
          "wrong timeline error names both values");
  require(fixture.run(source, "time of minecraft:the_end set noon") == 0 &&
              has(source, "commands.time.no_time_marker_found"),
          "End has no Overworld time markers");
  require(find(source, "commands.time.no_time_marker_found").args ==
              (std::vector<std::string>{
                  "ResourceKey[minecraft:clock_time_marker / minecraft:noon]",
                  "minecraft:the_end"}),
          "no-marker error preserves the vanilla ResourceKey text");

  const auto defaultTimelines =
      fixture.dispatcher.suggestions("time query ", source).texts;
  for (const char *timeline :
       {"minecraft:day", "minecraft:early_game", "minecraft:moon",
        "minecraft:villager_schedule"})
    require(std::find(defaultTimelines.begin(), defaultTimelines.end(),
                      timeline) != defaultTimelines.end(),
            "default Overworld timeline suggestions");
  const auto endTimelines =
      fixture.dispatcher.suggestions("time of minecraft:the_end query ", source)
          .texts;
  require(std::find(endTimelines.begin(), endTimelines.end(), "minecraft:day") ==
              endTimelines.end(),
          "End exposes no Overworld timeline suggestions");

  overworld = fixture.clocks.state(BuiltinWorldClock::Overworld);
  overworld.totalTicks = std::numeric_limits<std::int64_t>::max();
  overworld.partialTick = 0.5f;
  fixture.clocks.setClock(BuiltinWorldClock::Overworld, overworld);
  require(fixture.run(source, "time set noon") == -2147461457,
          "marker jump follows Java long wrap at INT64_MAX");
  require(fixture.clocks.state(BuiltinWorldClock::Overworld).totalTicks ==
              -9223372036854753616LL,
          "wrapped marker target matches Java long bits");
  require(fixture.clocks.state(BuiltinWorldClock::Overworld).partialTick ==
              0.0f,
          "wrapped marker jump still clears partial");

  overworld = fixture.clocks.state(BuiltinWorldClock::Overworld);
  overworld.totalTicks = 24000LL * 2147483648LL;
  fixture.clocks.setClock(BuiltinWorldClock::Overworld, overworld);
  require(fixture.run(source, "time query day repetition") == -1,
          "timeline repetition follows Java long-to-int narrowing");
  require(find(source, "commands.time.query.timeline.repetitions").args[1] ==
              "-2147483648",
          "narrowed repetition keeps the signed int value");
}

void numericGrammar() {
  TimeFixture time;
  Source adminSource = admin();
  require(time.run(adminSource, "time rate 1e0") == -1,
          "scientific notation is not Brigadier readFloat grammar");
  require(time.run(adminSource, "time rate 0x1p0") == -1,
          "hexadecimal float is not Brigadier readFloat grammar");
  require(time.run(adminSource, "time rate +1") == -1,
          "plus sign is not Brigadier readFloat grammar");
  require(time.run(adminSource, "time rate 1") >= 0,
          "plain decimal is accepted");
  require(time.run(adminSource, "time set .5s") >= 0,
          "leading decimal point is accepted");
  require(time.run(adminSource, "time set 1.d") >= 0,
          "trailing decimal point is accepted");

  Fixture fixture;
  require(time.run(adminSource, "time add 1e2t") == -1,
          "time argument rejects exponent notation");
  require(time.run(adminSource, "time add 0x1p2t") == -1,
          "time argument rejects hexadecimal floats");
  require(time.run(adminSource, "time add +1t") == -1,
          "time argument rejects plus signs");
  require(time.run(adminSource, "time add 1-2t") == -1,
          "time argument rejects embedded signs");
  require(time.run(adminSource, "time add .5t") >= 0,
          "time argument accepts leading decimal point");
  require(time.run(adminSource, "time add 1.t") >= 0,
          "time argument accepts trailing decimal point");
  require(fixture.run(adminSource, "tick step 1e2t") == -1,
          "/tick step shares the tightened numeric grammar");
}

void permissionAndSyntax() {
  Fixture fixture;
  Source player;
  player.permissionLevel = 2; // gamemasters, not admins
  require(fixture.run(player, "tick freeze") == -1,
          "/tick requires the admin permission level");
  player.permissionLevel = 3;
  require(fixture.run(player, "tick query") >= 0,
          "level 3 is enough for /tick");
  require(fixture.run(player, "bogus") == -1, "unknown command");
  require(fixture.run(player, "tick nonsense") == -1,
          "unknown subcommand");
  require(fixture.run(player, "") == -1, "an empty line is unknown");
  Source low;
  low.permissionLevel = 0;
  require(fixture.run(low, "tick query") == -1,
          "level 0 cannot run /tick");
}

#if REDSTONE_HAVE_PROTOCOL
void commandTreeEncoding() {
  Dispatcher dispatcher;
  registerTickCommand(dispatcher);
  const auto body = net::encodeCommandTree(dispatcher.root());
  net::Reader reader(body);
  const int count = reader.varInt();
  // root, tick, six subcommands, rate/time/step-time/sprint-time/step-stop and
  // sprint-stop depend on the tree, so only the important shape is pinned.
  require(count == 13, "the exported tree has the expected node count");
  struct Entry {
    std::uint8_t flags = 0;
    std::vector<int> children;
    std::string name;
    int parser = -1;
    std::string parserData;
  };
  std::vector<Entry> entries;
  for (int i = 0; i < count; ++i) {
    Entry entry;
    entry.flags = reader.u8();
    const int childCount = reader.varInt();
    for (int c = 0; c < childCount; ++c)
      entry.children.push_back(reader.varInt());
    if ((entry.flags & 3) == 1) {
      entry.name = reader.string();
    } else if ((entry.flags & 3) == 2) {
      entry.name = reader.string();
      entry.parser = reader.varInt();
      const std::size_t before = reader.pos;
      if (entry.parser == 1) {
        (void)reader.u8();
        (void)reader.f32();
        (void)reader.f32();
      } else {
        (void)reader.i32();
      }
      entry.parserData.assign(body.begin() + before, body.begin() + reader.pos);
    }
    entries.push_back(std::move(entry));
  }
  require(reader.varInt() == 0, "the root is node 0");
  require(reader.empty(), "the command packet body is fully consumed");

  require(entries[0].flags == 0, "the root has no type or executable flag");
  require(entries[0].children.size() == 1 && entries[0].children[0] == 1,
          "the root only exposes /tick");
  require(entries[1].name == "tick", "the command is named tick");
  require((entries[1].flags & 0x20) != 0,
          "an admin-only node is marked restricted");
  require((entries[1].flags & 4) == 0, "bare /tick is not executable");
  require(entries[1].children.size() == 6,
          "tick has query/rate/step/sprint/unfreeze/freeze");
  bool sawFloat = false, sawTime = false;
  for (const auto &entry : entries) {
    if (entry.parser == 1) {
      sawFloat = true;
      require(entry.name == "rate", "the float argument is rate");
    } else if (entry.parser == 43) {
      sawTime = true;
      require(entry.name == "time", "the time argument is time");
    }
  }
  require(sawFloat, "brigadier:float is exported for rate");
  require(sawTime, "minecraft:time is exported for step/sprint");
}
#endif

#if REDSTONE_HAVE_PROTOCOL
void timeCommandTreeEncoding() {
  Dispatcher dispatcher;
  registerTickCommand(dispatcher);
  registerTimeCommand(dispatcher);
  const auto body = net::encodeCommandTree(dispatcher.root());
  net::Reader reader(body);
  const int count = reader.varInt();
  struct Entry {
    std::uint8_t flags = 0;
    std::vector<int> children;
    std::string name;
    int parser = -1;
    std::string registry;
  };
  std::vector<Entry> entries;
  for (int i = 0; i < count; ++i) {
    Entry entry;
    entry.flags = reader.u8();
    const int childCount = reader.varInt();
    for (int c = 0; c < childCount; ++c)
      entry.children.push_back(reader.varInt());
    if ((entry.flags & 3) == 1) {
      entry.name = reader.string();
    } else if ((entry.flags & 3) == 2) {
      entry.name = reader.string();
      entry.parser = reader.varInt();
      if (entry.parser == 1) {
        (void)reader.u8();
        (void)reader.f32();
        (void)reader.f32();
      } else if (entry.parser == 43) {
        (void)reader.i32();
      } else if (entry.parser == 46) {
        entry.registry = reader.string();
      } else if (entry.parser == 36) {
        // IdentifierArgument's singleton serializer has no payload.
      } else {
        throw std::runtime_error("unexpected command parser id");
      }
    }
    entries.push_back(std::move(entry));
  }
  require(reader.varInt() == 0 && reader.empty(),
          "time command tree is fully encoded");
  require(entries[0].children.size() == 2,
          "root exposes tick and time");
  int timeIndex = -1;
  for (int child : entries[0].children)
    if (entries[child].name == "time")
      timeIndex = child;
  require(timeIndex >= 0, "time command node exists");
  require((entries[timeIndex].flags & 0x20) != 0,
          "time is restricted to gamemasters");
  bool clockResource = false, timelineResource = false, markerIdentifier = false;
  for (const auto &entry : entries) {
    if (entry.parser == 46 && entry.name == "clock") {
      require(entry.registry == "minecraft:world_clock",
              "clock resource names the world_clock registry");
      clockResource = true;
    } else if (entry.parser == 46 && entry.name == "timeline") {
      require(entry.registry == "minecraft:timeline",
              "timeline resource names the timeline registry");
      timelineResource = true;
    } else if (entry.parser == 36 && entry.name == "timemarker") {
      markerIdentifier = true;
    }
  }
  require(clockResource, "minecraft:resource=46 is exported for clocks");
  require(timelineResource, "minecraft:resource=46 is exported for timelines");
  require(markerIdentifier, "minecraft:resource_location=36 is used for markers");
}
#endif

void messagesCarryTranslateKeys() {
  Fixture fixture;
  Source source = admin();
  fixture.run(source, "tick freeze");
  require(source.messages.size() == 1, "freeze produces one line");
  require(source.messages[0].key == "commands.tick.status.frozen",
          "the line is a translation key the client resolves");
  require(source.messages[0].args.empty(), "the line has no arguments");
  require(!source.messages[0].failure, "it is a success message");
}
} // namespace

int main() {
  try {
    tickRateManager();
    tickRateCommands();
    suggestions();
    timeCommands();
    timeMarkersAndTimelines();
    numericGrammar();
    permissionAndSyntax();
#if REDSTONE_HAVE_PROTOCOL
    commandTreeEncoding();
    timeCommandTreeEncoding();
#endif
    messagesCarryTranslateKeys();
    std::cout << "command tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "command test failed: " << e.what() << '\n';
    return 1;
  }
}
