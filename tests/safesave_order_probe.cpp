#include "redstone/engine.hpp"
#include "redstone/piston.hpp"
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>
using namespace redstone;
namespace {
void load(World &w) {
  for (int c : {-1, 0, 1, 511, 512})
    for (int z : {-1, 0})
      w.loadChunk(c, z);
}
void pos(Pos p) { std::cout << '[' << p.x << ',' << p.y << ',' << p.z << ']'; }
void positions(const std::vector<Pos> &v) {
  std::cout << '[';
  bool first = true;
  for (auto p : v) {
    if (!first)
      std::cout << ',';
    first = false;
    pos(p);
  }
  std::cout << ']';
}
std::vector<Pos> recorded(World &w, const char *kind) {
  std::vector<Pos> out;
  for (const auto &e : w.log)
    if (e.kind == kind)
      out.push_back(e.pos);
  return out;
}
} // namespace
int main() {
  std::cout << "{\n\"fixture_ticks\":";
  World fixture;
  load(fixture);
  fixture.traceUpdates = true;
  int n = 0;
  for (int b : {0, 16, 8192})
    for (int x : {b, b - 1, b + 1, b - 2}) {
      Pos p{x, 82, 5};
      fixture.rawSetBlock(p, defaultState("smooth_stone"));
      fixture.scheduledTicks.push_back({p, "minecraft:smooth_stone", 2,
                                        n % 2 == 0 ? -1 : 0,
                                        static_cast<std::int64_t>(n * 2)});
      ++n;
    }
  fixture.tick();
  fixture.tick();
  positions(recorded(fixture, "scheduled_tick"));
  std::cout << ",\n\"fixture_events\":";
  World events;
  load(events);
  events.traceUpdates = true;
  std::map<Pos, int> data;
  n = 0;
  for (int b : {0, 16, 8192})
    for (int x : {b, b - 1, b + 1, b - 2}) {
      Pos p{x, 82, 4};
      events.rawSetBlock(p, defaultState("note_block"));
      data[p] = ++n;
      events.blockEvent(p, "note_block", 0, n);
      events.blockEvent(p, "note_block", 0, n);
    }
  events.tick();
  std::cout << '[';
  bool first = true;
  for (const auto &e : events.log)
    if (e.kind == "block_event") {
      if (!first)
        std::cout << ',';
      first = false;
      std::cout << '[' << e.pos.x << ',' << e.pos.y << ',' << e.pos.z << ",0,"
                << data[e.pos] << ']';
    }
  std::cout << ']';
  std::cout << ",\n\"dedup_ticks\":";
  World dedup;
  load(dedup);
  dedup.scheduleTick({2, 87, 2}, "smooth_stone", 50, 1);
  dedup.scheduleTick({2, 87, 2}, "smooth_stone", 4, -1);
  dedup.scheduleTick({3, 87, 2}, "smooth_stone", 60, 0);
  // Reproduce explicit vanilla suborders 999 and 1000 after schedule's dedup.
  dedup.scheduledTicks[0].order = 999;
  dedup.scheduledTicks[1].order = 1000;
  std::sort(dedup.scheduledTicks.begin(), dedup.scheduledTicks.end(),
            [](const auto &a, const auto &b) { return a.pos < b.pos; });
  std::cout << '[';
  first = true;
  for (const auto &t : dedup.scheduledTicks) {
    if (!first)
      std::cout << ',';
    first = false;
    std::cout << '[' << t.pos.x << ',' << t.pos.y << ',' << t.pos.z << ','
              << t.trigger << ',' << t.priority << ',' << t.order << ']';
  }
  std::cout << ']';
  std::cout << ",\n\"overdue_ticks\":";
  World overdue;
  load(overdue);
  overdue.traceUpdates = true;
  overdue.setGameTime(10);
  overdue.scheduledTicks = {{{0, 82, 5}, "minecraft:smooth_stone", 5, 2, 0},
                            {{1, 82, 5}, "minecraft:smooth_stone", 6, -3, 1},
                            {{16, 82, 5}, "minecraft:smooth_stone", 8, 0, 2},
                            {{8192, 82, 5}, "minecraft:smooth_stone", 8, 0, 3}};
  overdue.tick();
  positions(recorded(overdue, "scheduled_tick"));
  std::cout << ",\n\"negative_orders\":";
  World negative;
  load(negative);
  negative.traceUpdates = true;
  negative.setGameTime(9);
  negative.scheduledTicks = {{{16, 82, 5}, "minecraft:smooth_stone", 10, 0, 0},
                             {{-1, 82, 5},
                              "minecraft:smooth_stone",
                              10,
                              0,
                              static_cast<std::int64_t>(-2LL)}};
  negative.tick();
  positions(recorded(negative, "scheduled_tick"));
  std::cout << ",\n\"observer_reschedule\":";
  World observer;
  load(observer);
  observer.traceUpdates = true;
  observer.rawSetBlock({0, 82, 5},
                       defaultState("observer").with("facing", "east"));
  observer.scheduleTick({0, 82, 5}, "observer", 1);
  std::cout << '[';
  for (int i = 1; i <= 4; i++) {
    if (i > 1)
      std::cout << ',';
    observer.tick();
    std::cout << '[' << i << ','
              << (observer.getBlock({0, 82, 5}).get("powered") == "true" ? 1
                                                                         : 0)
              << ','
              << (observer.hasScheduledTick({0, 82, 5}, "observer") ? 1 : 0)
              << ']';
  }
  std::cout << ']';
  std::cout << ",\n\"moving_entities\":";
  World moving;
  load(moving);
  moving.traceUpdates = true;
  std::vector<Pos> coords;
  for (int x : {1, -1, 17, 15, 8193, 8191}) {
    Pos p{x, 84, -3};
    coords.push_back(p);
    moving.rawSetBlock(p, defaultState("moving_piston").with("facing", "east"));
    auto entity =
        std::make_shared<PistonMovingBlockEntity>(PistonMovingBlockEntity{
            p, defaultState("slime_block"), Direction::East, true, false});
    entity->progress = .5f;
    entity->progressO = 0;
    entity->lastTicked = 0;
    moving.setPistonEntity(p, entity);
  }
  std::cout << '[';
  for (int tick = 1; tick <= 3; tick++) {
    if (tick > 1)
      std::cout << ',';
    moving.tick();
    std::cout << '[';
    first = true;
    for (auto p : coords) {
      if (!first)
        std::cout << ',';
      first = false;
      std::cout << '[' << p.x << ',' << p.y << ',' << p.z << ',';
      auto it = moving.pistonEntities.find(p);
      if (it == moving.pistonEntities.end())
        std::cout << "null,null,null";
      else
        std::cout << it->second->progress << ',' << it->second->progressO << ','
                  << it->second->lastTicked;
      std::cout << ",\"" << moving.getBlock(p).name << "\"]";
    }
    std::cout << ']';
  }
  std::cout << ']';
  std::cout << ",\n\"moving_completion_order\":";
  positions(recorded(moving, "piston_entity_remove"));
  std::cout << "\n}\n";
}
