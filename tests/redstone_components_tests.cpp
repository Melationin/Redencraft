#include "redstone/engine.hpp"
#include "redstone/piston.hpp"
#include "redstone/redstone.hpp"
#include "redstone/shapes.hpp"

#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace redstone;

namespace {

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void setupFloor(World &world) {
  for (int cx = -1; cx <= 1; ++cx)
    for (int cz = -1; cz <= 1; ++cz)
      world.loadChunk(cx, cz);
  for (int x = -16; x <= 15; ++x)
    for (int z = -16; z <= 15; ++z)
      world.rawSetBlock({x, 63, z}, Blocks::SMOOTH_STONE.defaultState());
}

void stepTicks(World &world, int ticks) {
  for (int i = 0; i < ticks; ++i)
    world.tick();
}

int wirePower(const World &world, Pos pos) {
  const BlockState s = world.getBlock(pos);
  require(s.is(Blocks::REDSTONE_WIRE), "expected redstone wire");
  return std::stoi(s.get("power", "0"));
}

void placeWire(World &world, Pos pos) {
  world.setBlock(pos, redstoneWirePlacementState(world, pos), 3);
}

void placeRepeater(World &world, Pos pos, Direction facingToOutput,
                   int delay = 1) {
  // facing property points toward INPUT (opposite of output direction).
  BlockState s =
      repeaterPlacementState(world, pos, facingToOutput)
          .with(PropertyKind::Delay, static_cast<std::uint8_t>(delay));
  world.setBlock(pos, s, 3);
  diodeSetPlacedBy(world, pos, s);
}

void placeComparator(World &world, Pos pos, Direction facingToOutput,
                     bool subtract = false) {
  BlockState s = comparatorPlacementState(facingToOutput);
  if (subtract)
    s = s.with("mode", "subtract");
  world.setBlock(pos, s, 3);
  diodeSetPlacedBy(world, pos, s);
}

void testWireDecayAndNonSelfLoop() {
  World world;
  setupFloor(world);

  // Place 16 blocks of redstone wire going East from x=0..15 at (x, 64, 0).
  for (int x = 0; x <= 15; ++x)
    placeWire(world, {x, 64, 0});

  // Power x=0 directly from a redstone block at (-1, 64, 0).
  world.setBlock({-1, 64, 0}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  for (int x = 0; x <= 14; ++x)
    require(wirePower(world, {x, 64, 0}) == 15 - x,
            "wire power must decay linearly by 1 per block");
  require(wirePower(world, {15, 64, 0}) == 0,
          "16th wire block must have power 0");

  // Remove the power source: wire must not feed itself into a non-zero loop.
  world.setBlock({-1, 64, 0}, airState(), 3);
  for (int x = 0; x <= 15; ++x)
    require(wirePower(world, {x, 64, 0}) == 0,
            "removing source must depressurize entire wire line to 0");
}

void testWireStrongPowerVsWireIsolation() {
  World world;
  setupFloor(world);

  // Redstone block -> wire -> stone conductor -> wire.
  // A wire strongly powers the conductor it points into, which powers repeaters/
  // lamps, EXCEPT DefaultRedstoneWireEvaluator sets shouldSignal=false while
  // computing block signals for wires so a second wire cannot read through the stone!
  placeWire(world, {1, 64, 0});
  placeWire(world, {2, 64, 0});
  world.setBlock({3, 64, 0}, Blocks::STONE.defaultState(), 3);
  placeWire(world, {4, 64, 0});
  world.setBlock({3, 64, 1}, Blocks::REDSTONE_LAMP.defaultState(), 3);
  placeRepeater(world, {4, 64, 0}, Direction::East); // actually replace {4,64,0} test first

  // Put lamp at {3,64,1} and wire at {4,64,0}; wait {4,64,0} is after stone {3,64,0}.
  world.setBlock({0, 64, 0}, Blocks::REDSTONE_BLOCK.defaultState(), 3);

  require(wirePower(world, {1, 64, 0}) == 15, "input wire is 15");
  require(wirePower(world, {2, 64, 0}) == 14, "wire entering stone is 14");
  // Wait: {4,64,0} currently has a Repeater because we called placeRepeater there!
  // Let's re-test wire behind stone cleanly at z=4.
  for (int x = 0; x <= 4; ++x)
    world.rawSetBlock({x, 64, 4}, airState());
  placeWire(world, {1, 64, 4});
  placeWire(world, {2, 64, 4});
  world.setBlock({3, 64, 4}, Blocks::STONE.defaultState(), 3);
  placeWire(world, {4, 64, 4});
  world.setBlock({3, 64, 5}, Blocks::REDSTONE_LAMP.defaultState(), 3);
  world.setBlock({0, 64, 4}, Blocks::REDSTONE_BLOCK.defaultState(), 3);

  require(world.getSignal({3, 64, 4}, Direction::West) == 14,
          "wire strongly powers the conductor block it runs into (lamp/repeater sees 14)");
  require(world.getBlock({3, 64, 5}).get("lit") == "true",
          "lamp beside strongly-powered stone turns on");
  require(wirePower(world, {4, 64, 4}) == 0,
          "wire behind strongly-powered stone cannot be powered by wire");

  // Meanwhile a repeater pointing out of {3, 64, 4} DOES turn on after 1+2 ticks.
  stepTicks(world, 3);
  require(world.getBlock({4, 64, 0}).get("powered") == "true",
          "repeater behind wire-powered conductor turns on");
}

void testWireDiagonalCutAndCrossDotToggle() {
  World world;
  setupFloor(world);

  // Wire at (0, 64, 0) steps UP onto stone block at (1, 64, 0) -> wire at (1, 65, 0).
  world.setBlock({1, 64, 0}, Blocks::STONE.defaultState(), 3);
  placeWire(world, {0, 64, 0});
  placeWire(world, {1, 65, 0});
  world.setBlock({-1, 64, 0}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  require(wirePower(world, {1, 65, 0}) == 14,
          "wire must step up onto block with air above lower wire");

  // Putting opaque STONE above (0, 64, 0) at (0, 65, 0) cuts diagonal wire.
  world.setBlock({0, 65, 0}, Blocks::STONE.defaultState(), 3);
  require(wirePower(world, {1, 65, 0}) == 0,
          "opaque conductor above lower wire must cut step-up connection");
  require(world.getBlock({0, 64, 0}).get("east") == "side",
          "step-up east connection drops from up to straight-line side when cut by conductor");

  // Replacing with GLASS (non-conductor) restores step-up connection.
  world.setBlock({0, 65, 0}, Blocks::GLASS.defaultState(), 3);
  require(wirePower(world, {1, 65, 0}) == 14,
          "glass above lower wire must allow step-up connection");

  // Cross <-> Dot toggle on isolated powered wire at (5, 64, 5).
  world.setBlock({5, 65, 5}, airState(), 3);
  world.setBlock({6, 64, 5}, Blocks::STONE.defaultState(), 3);
  world.setBlock({6, 64, 6}, Blocks::REDSTONE_LAMP.defaultState(), 3);
  placeWire(world, {5, 64, 5}); // default placed is cross ("side" on N/E/S/W)
  world.setBlock({5, 63, 5}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  require(world.getBlock({6, 64, 6}).get("lit") == "true",
          "cross wire powers east stone block strongly");

  require(redstoneWireUse(world, {5, 64, 5}, world.getBlock({5, 64, 5})),
          "right-click toggles cross to dot");
  require(world.getBlock({5, 64, 5}).get("east") == "none",
          "toggled wire becomes dot");
  stepTicks(world, 4);
  require(world.getBlock({6, 64, 6}).get("lit") == "false",
          "dot wire no longer strongly powers adjacent east stone block");
}

void testRedstoneTorchInversionWallAndBurnout() {
  World world;
  setupFloor(world);

  // Standing torch on stone at (0, 64, 0), stone directly above at (0, 66, 0) (air at 65)
  // Actually block directly above torch is (0, 65, 0).
  world.setBlock({0, 64, 0}, Blocks::REDSTONE_TORCH.defaultState(), 3);
  world.setBlock({0, 65, 0}, Blocks::STONE.defaultState(), 3);
  world.setBlock({1, 64, 0}, Blocks::STONE.defaultState(), 3);
  placeWire(world, {1, 65, 0});
  require(world.getBlock({0, 64, 0}).get("lit") == "true", "torch defaults lit");
  require(wirePower(world, {1, 65, 0}) == 15,
          "standing torch strongly powers solid block directly above");

  // Redstone block only emits weak power, so adjacent stone (0, 63, 0) stays
  // at getDirectSignalTo == 0 and torch stays lit.
  world.setBlock({-1, 63, 0}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  stepTicks(world, 3);
  require(world.getBlock({0, 64, 0}).get("lit") == "true",
          "redstone block beside base stone does not strongly power it to invert torch");

  // Power the base block (0, 63, 0) strongly via attached wall lever -> 2-tick delay.
  world.setBlock({-1, 63, 0},
                 Blocks::LEVER.defaultState()
                     .with("face", "wall")
                     .with("facing", "west")
                     .with("powered", "false"),
                 3);
  leverUse(world, {-1, 63, 0}, world.getBlock({-1, 63, 0}));
  require(world.getBlock({0, 64, 0}).get("lit") == "true",
          "torch stays lit on same tick before 2-tick delay");
  stepTicks(world, 1);
  require(world.getBlock({0, 64, 0}).get("lit") == "true",
          "torch stays lit after 1 tick");
  stepTicks(world, 1);
  require(world.getBlock({0, 64, 0}).get("lit") == "false",
          "torch turns off after 2 ticks");
  require(wirePower(world, {1, 65, 0}) == 0,
          "wire above unlit torch turns off");

  // Wall torch on East face of wall at (5, 64, 5) -> wall torch at (6, 64, 5) facing East.
  world.setBlock({5, 64, 5}, Blocks::STONE.defaultState(), 3);
  world.setBlock({6, 64, 5},
                 Blocks::REDSTONE_WALL_TORCH.defaultState().with("facing", "east"),
                 3);
  world.setBlock({6, 65, 5}, Blocks::STONE.defaultState(), 3);
  world.setBlock({7, 64, 5}, Blocks::STONE.defaultState(), 3);
  placeWire(world, {7, 65, 5});
  require(wirePower(world, {7, 65, 5}) == 15,
          "wall torch strongly powers block above it");
  require(world.getSignal({6, 64, 5}, Direction::East) == 0,
          "wall torch does not power the wall it is attached to");

  // Power the wall at (5, 64, 5) via lever -> wall torch turns off in 2 ticks.
  world.setBlock({4, 64, 5},
                 Blocks::LEVER.defaultState()
                     .with("face", "wall")
                     .with("facing", "west")
                     .with("powered", "false"),
                 3);
  leverUse(world, {4, 64, 5}, world.getBlock({4, 64, 5}));
  stepTicks(world, 2);
  require(world.getBlock({6, 64, 5}).get("lit") == "false",
          "wall torch inverts when attached wall is powered");

  // Burnout test: 1-tick rapid self-loop feedback:
  // Torch at (10, 64, 10) on stone (10, 63, 10).
  // Stone above at (10, 65, 10) powers wire at (11, 65, 10) -> stone (11, 64, 10)? No:
  // Wire at (11, 65, 10) powers block below it (11, 64, 10) and wire at (11, 64, 11) -> (10, 64, 11)?
  // Classic 1-torch burnout: stone (10, 64, 10)? Wait:
  // Block at (10, 63, 10) [stone], Torch on North side of (10, 64, 10) -> torch at (10, 64, 9) facing North.
  // Actually simpler: standing torch at (10, 64, 10) on stone (10, 63, 10):
  // Block above torch: (10, 65, 10) [stone] -> wire at (10, 64, 11) (Wait: (10, 65, 11) wire -> powers (10, 64, 11) -> NO:
  // Classic burnout clock in Vanilla:
  // Torch at (10, 65, 10) attached to North face of (10, 65, 11) [stone], OR standing torch at (10, 64, 10):
  // Wire at (11, 63, 10)? Wait: if torch is at (10, 64, 10), block above torch is (10, 65, 10) [stone],
  // wire next to (10, 65, 10) is on top of (11, 64, 10) [wire at (11, 65, 10)]? No, wire at (10, 65, 11)
  // doesn't power (10, 63, 10)! Put dust at (10, 63, 10)... wait dust can't be in same block as stone.
  // Put wire at (11, 64, 10) on stone (11, 63, 10): wait (10, 65, 10) has wire on TOP at (10, 66, 10)?
  // Easiest 1-torch burnout in Vanilla:
  // Torch on top of stone (10, 64, 10) -> torch at (10, 65, 10).
  // Stone above torch at (10, 66, 10)? No: put wire on TOP of (11, 64, 10) [wire at (11, 65, 10)]:
  // wait (10, 65, 10) is a lit torch -> powers wire at (11, 65, 10)!
  // Wait (11, 64, 10) is next to (10, 64, 10) -> wait if (11, 65, 10) is wire on stone (11, 64, 10),
  // put another wire at (11, 64, 11) -> (10, 64, 11) -> Wait if (10, 64, 11) is wire pointing at stone (10, 64, 10),
  // IT STRONGLY POWERS (10, 64, 10), WHICH TURNS OFF THE TORCH AT (10, 65, 10)!
  // Let's trace: Torch at (10, 65, 10) on stone (10, 64, 10).
  // Block above torch: (10, 66, 10) [stone] powers wire at (11, 66, 10) on stone (11, 65, 10) -> steps down to
  // wire at (11, 65, 11) -> steps down to (10, 65, 11)? No! Even simpler:
  // Torch at (10, 64, 10) on stone (10, 63, 10). No! Wall torch!
  // Wall torch at (10, 65, 9) facing North attached to (10, 65, 10) [stone].
  // Stone above wall torch at (10, 66, 9).
  // Wait: put wire at (10, 66, 10) on top of (10, 65, 10)!
  // Let's trace THAT!
  // Wall torch at (10, 65, 9) facing North -> strongly powers (10, 66, 9) [AIR? Wait: NO! Put STONE at (10, 66, 9)?
  // Wait if (10, 66, 9) is STONE, it powers wire at (10, 66, 10)? No, wire is at y=67 on stone (10, 66, 10) ->
  // wire at (10, 66, 10) rests on STONE (10, 65, 10)!!
  // Wait: if STONE (10, 66, 9) is adjacent to wire (10, 66, 10), and wire (10, 66, 10) is ON TOP of STONE (10, 65, 10),
  // then STONE (10, 66, 9) powers WIRE (10, 66, 10), which powers STONE (10, 65, 10), which turns off TORCH (10, 65, 9)!
  // 2 ticks later torch turns OFF -> stone (10, 66, 9) unpowers -> wire (10, 66, 10) unpowers -> stone (10, 65, 10) unpowers -> 2 ticks later torch turns ON -> REPEATS EVERY 2 TICKS UNTIL BURNOUT!
  world.setBlock({10, 65, 10}, Blocks::STONE.defaultState(), 3);
  world.setBlock({10, 66, 9}, Blocks::STONE.defaultState(), 3);
  world.setBlock({10, 65, 9},
                 Blocks::REDSTONE_WALL_TORCH.defaultState().with("facing", "north"),
                 3);
  placeWire(world, {10, 66, 10});

  int offCount = 0;
  for (int t = 0; t < 50; ++t) {
    stepTicks(world, 1);
    if (world.getBlock({10, 65, 9}).get("lit") == "false")
      ++offCount;
  }
  require(offCount > 10, "clock must oscillate and burn out in off state");
  require(world.getBlock({10, 65, 9}).get("lit") == "false",
          "burned out torch must remain unlit during cooldown");

  // Burnout restart delay (160 ticks) when neighborSignal drops AFTER the
  // burnout tick (so scheduleTick(160) is not superseded inside setBlock).
  world.setBlock({15, 64, 15}, Blocks::REDSTONE_TORCH.defaultState(), 3);
  world.setBlock({14, 63, 15},
                 Blocks::LEVER.defaultState()
                     .with("face", "wall")
                     .with("facing", "west")
                     .with("powered", "false"),
                 3);
  for (int i = 0; i < 8; ++i) {
    leverUse(world, {14, 63, 15}, world.getBlock({14, 63, 15})); // ON
    stepTicks(world, 2); // torch turns OFF (counts toggle #i+1)
    if (i < 7) {
      leverUse(world, {14, 63, 15}, world.getBlock({14, 63, 15})); // OFF
      stepTicks(world, 2); // torch turns back ON
    }
  }
  require(world.getBlock({15, 64, 15}).get("lit") == "false",
          "8th toggle in 60 ticks burns out torch");
  // Release lever AFTER burnout tick scheduled 160-tick restart.
  leverUse(world, {14, 63, 15}, world.getBlock({14, 63, 15}));
  stepTicks(world, 159);
  require(world.getBlock({15, 64, 15}).get("lit") == "false",
          "burned out torch stays off until tick 159");
  stepTicks(world, 1);
  require(world.getBlock({15, 64, 15}).get("lit") == "true",
          "burned out torch relights at exact 160-tick restart delay");
}

void testRepeaterDelaysPulseLengthAndLocking() {
  World world;
  setupFloor(world);

  // Repeater delay=3 (6 ticks) at (1, 64, 0) pointing East.
  placeRepeater(world, {1, 64, 0}, Direction::East, 3);
  world.setBlock({2, 64, 0}, Blocks::REDSTONE_LAMP.defaultState(), 3);

  // Input source at (0, 64, 0): 1-tick pulse (on for 2 ticks? Actually 2-tick pulse!)
  world.setBlock({0, 64, 0}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  stepTicks(world, 2);
  world.setBlock({0, 64, 0}, airState(), 3); // removed after 2 ticks (< 6 tick delay)
  stepTicks(world, 3);
  require(world.getBlock({1, 64, 0}).get("powered") == "false",
          "delay-3 repeater still unpowered at tick 5");
  stepTicks(world, 1);
  require(world.getBlock({1, 64, 0}).get("powered") == "true",
          "delay-3 repeater powers on at tick 6 even though input already dropped");
  require(world.getBlock({2, 64, 0}).get("lit") == "true",
          "downstream lamp lights when repeater turns on");
  // Repeater lengthens output pulse to 6 ticks!
  stepTicks(world, 5);
  require(world.getBlock({1, 64, 0}).get("powered") == "true",
          "repeater holds output for full 6 ticks");
  stepTicks(world, 1);
  require(world.getBlock({1, 64, 0}).get("powered") == "false",
          "repeater turns off 6 ticks after turning on");

  // Diode Locking: side repeater at (1, 64, -1) facing South into (1, 64, 0).
  world.setBlock({1, 64, -2}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  placeRepeater(world, {1, 64, -1}, Direction::South, 1);
  stepTicks(world, 2); // locking repeater turns on at (1, 64, -1)
  require(world.getBlock({1, 64, 0}).get("locked") == "true",
          "side active repeater locks target repeater");

  // Power main input while locked -> output must stay OFF.
  world.setBlock({0, 64, 0}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  stepTicks(world, 10);
  require(world.getBlock({1, 64, 0}).get("powered") == "false",
          "locked repeater ignores input turning on");

  // Verify side redstone wire does NOT lock a repeater.
  world.setBlock({1, 64, -2}, airState(), 3);
  stepTicks(world, 3);
  require(world.getBlock({1, 64, 0}).get("locked") == "false",
          "repeater unlocks when side repeater turns off");
  stepTicks(world, 6);
  require(world.getBlock({1, 64, 0}).get("powered") == "true",
          "repeater updates to powered once unlocked");

  placeWire(world, {1, 64, 1});
  world.setBlock({1, 64, 2}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  require(world.getBlock({1, 64, 0}).get("locked") == "false",
          "side redstone wire must NOT lock repeater");
}

void testComparatorCompareSubtractAndAnalogRead() {
  World world;
  setupFloor(world);

  // Composter level=6 behind stone conductor at (0, 64, 0), stone at (1, 64, 0),
  // comparator at (2, 64, 0) pointing East, wire at (3, 64, 0)..(12, 64, 0).
  world.setBlock({0, 64, 0}, Blocks::COMPOSTER.defaultState().with("level", "6"),
                 3);
  world.setBlock({1, 64, 0}, Blocks::STONE.defaultState(), 3);
  placeComparator(world, {2, 64, 0}, Direction::East, false);
  for (int x = 3; x <= 10; ++x)
    placeWire(world, {x, 64, 0});

  // Trigger comparator update (1 tick from diodeSetPlacedBy).
  stepTicks(world, 2);
  require(world.getBlock({2, 64, 0}).get("powered") == "true",
          "comparator powers on reading composter through stone conductor");
  require(world.comparatorOutput({2, 64, 0}) == 6,
          "comparator output matches composter level 6");
  require(wirePower(world, {3, 64, 0}) == 6,
          "output wire receives analog strength 6");
  require(wirePower(world, {8, 64, 0}) == 1, "wire at distance 6 is 1");
  require(wirePower(world, {9, 64, 0}) == 0, "wire at distance 7 is 0");

  // Side wire at (2, 64, 1) with strength 4 -> in compare mode 6 >= 4 -> output stays 6.
  placeWire(world, {2, 64, 1});
  placeWire(world, {3, 64, 1});
  world.setBlock({4, 64, 1}, Blocks::REDSTONE_BLOCK.defaultState(), 3);
  // 4->redstone_block => (3,64,1)=15, wait place further so side wire is 4!
  world.setBlock({4, 64, 1}, airState(), 3);
  // Side source: copper_bulb lit? Wait wire length or composter+comparator on side!
  world.setBlock({2, 64, 2}, Blocks::CAULDRON.defaultState(), 3);
  world.setBlock({2, 64, 2},
                 Blocks::WATER_CAULDRON.defaultState().with("level", "2"), 3);
  world.setBlock({2, 64, 1}, airState(), 3);
  world.setBlock({3, 64, 1}, airState(), 3);
  placeComparator(world, {2, 64, 1}, Direction::North, false);
  stepTicks(world, 4);
  require(world.comparatorOutput({2, 64, 1}) == 2, "side comparator outputs 2");
  require(world.comparatorOutput({2, 64, 0}) == 6,
          "compare mode (6 >= 2) keeps output 6");

  // Toggle main comparator to subtract mode -> 6 - 2 = 4!
  require(comparatorUse(world, {2, 64, 0}, world.getBlock({2, 64, 0})),
          "right-click comparator toggles subtract mode");
  require(world.comparatorOutput({2, 64, 0}) == 4,
          "subtract mode immediately outputs 6 - 2 = 4");
  require(wirePower(world, {3, 64, 0}) == 4,
          "output wire updates to subtracted strength 4");

  // Raise side signal to lava cauldron + copper bulb? Test side > rear (15 > 6 -> 0).
  world.setBlock({2, 64, 2}, Blocks::COPPER_BULB.defaultState().with("lit", "true"),
                 3);
  stepTicks(world, 4);
  require(world.comparatorOutput({2, 64, 1}) == 15,
          "lit copper bulb gives analog output 15");
  require(world.comparatorOutput({2, 64, 0}) == 0,
          "when side > rear, comparator output drops to 0");
  require(world.getBlock({2, 64, 0}).get("powered") == "false",
          "comparator unpowers when output is 0");

  // Breaking comparator clears ComparatorBlockEntity state.
  world.setBlock({2, 64, 2}, Blocks::WATER_CAULDRON.defaultState().with("level", "2"),
                 3);
  stepTicks(world, 4);
  require(world.comparatorOutput({2, 64, 0}) == 4, "restored output 4");
  world.removeBlock({2, 64, 0});
  require(world.comparatorOutput({2, 64, 0}) == 0,
          "removing comparator clears stored output signal");
}

void testButtonsCopperBulbAndPistons() {
  World world;
  setupFloor(world);

  // Stone button (20 ticks) and oak button (30 ticks).
  world.setBlock({0, 64, 0}, Blocks::STONE.defaultState(), 3);
  const BlockState sBtn = Blocks::STONE_BUTTON.defaultState()
                              .with("face", "wall")
                              .with("facing", "west");
  world.setBlock({-1, 64, 0}, sBtn, 3);
  world.setBlock({1, 64, 0}, Blocks::COPPER_BULB.defaultState(), 3);

  require(buttonUse(world, {-1, 64, 0}, sBtn), "press stone button");
  require(world.getBlock({1, 64, 0}).get("lit") == "true",
          "copper bulb toggles lit=true on rising edge");
  require(world.getBlock({1, 64, 0}).get("powered") == "true",
          "copper bulb powered=true while button pressed");
  stepTicks(world, 19);
  require(world.getBlock({-1, 64, 0}).get("powered") == "true",
          "stone button remains pressed at tick 19");
  stepTicks(world, 1);
  require(world.getBlock({-1, 64, 0}).get("powered") == "false",
          "stone button releases at tick 20");
  require(world.getBlock({1, 64, 0}).get("lit") == "true",
          "copper bulb remains lit after falling edge");
  require(world.getBlock({1, 64, 0}).get("powered") == "false",
          "copper bulb powered=false after falling edge");

  // Second press toggles copper bulb off; wire activates piston.
  require(buttonUse(world, {-1, 64, 0}, world.getBlock({-1, 64, 0})),
          "second press");
  require(world.getBlock({1, 64, 0}).get("lit") == "false",
          "copper bulb toggles lit=false on second rising edge");

  // Piston powered by wire.
  world.setBlock({5, 64, 5}, Blocks::PISTON.defaultState().with("facing", "east"),
                 3);
  placeWire(world, {4, 64, 5});
  world.setBlock({3, 64, 5}, Blocks::LEVER.defaultState().with("face", "floor"),
                 3);
  leverUse(world, {3, 64, 5}, world.getBlock({3, 64, 5}));
  stepTicks(world, 4);
  require(world.getBlock({5, 64, 5}).get("extended") == "true",
          "lever -> redstone wire extends piston");
  require(world.getBlock({6, 64, 5}).is(Blocks::PISTON_HEAD),
          "piston head extended via redstone wire");
}

} // namespace

int main() {
  testWireDecayAndNonSelfLoop();
  testWireStrongPowerVsWireIsolation();
  testWireDiagonalCutAndCrossDotToggle();
  testRedstoneTorchInversionWallAndBurnout();
  testRepeaterDelaysPulseLengthAndLocking();
  testComparatorCompareSubtractAndAnalogRead();
  testButtonsCopperBulbAndPistons();
  std::cout << "redstone_components_tests passed\n";
  return 0;
}
