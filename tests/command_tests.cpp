// /tick and its command skeleton. The dispatcher, the tick rate manager and the
// clientbound command tree are checked against the vanilla TickCommand /
// ServerTickRateManager behaviour this simulator mirrors.
#include "redstone/command.hpp"
#include "redstone/net/wire.hpp"
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
    permissionAndSyntax();
#if REDSTONE_HAVE_PROTOCOL
    commandTreeEncoding();
#endif
    messagesCarryTranslateKeys();
    std::cout << "command tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "command test failed: " << e.what() << '\n';
    return 1;
  }
}
