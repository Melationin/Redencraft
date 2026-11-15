#include "redstone/shapes.hpp"
#include <iostream>
#include <stdexcept>
using namespace redstone;
void require(bool c, const char *s) {
  if (!c)
    throw std::runtime_error(s);
}
int main() {
  try {
    World w;
    const auto &stableShape = collisionShape(defaultState("smooth_stone"));
    const auto *stableAddress = &stableShape;
    const auto extendedState = defaultState("bedrock");
    collisionShape(extendedState);
    require(&collisionShape(defaultState("smooth_stone")) == stableAddress &&
                stableShape.size() == 1 && stableShape[0].x1 == 16,
            "cached collision shape reference survives cache growth");
    for (int x = -1; x <= 1; ++x)
      for (int z = -1; z <= 1; ++z)
        w.loadChunk(x, z);
    auto wall = defaultState("cobblestone_wall");
    auto stoneWall = defaultState("stone_brick_wall");
    auto gate = defaultState("oak_fence_gate");
    auto gateClosed = collisionShape(gate);
    require(gateClosed.size() == 1 && gateClosed[0].x1 == 16 &&
                gateClosed[0].z0 == 6 && gateClosed[0].z1 == 10 &&
                gateClosed[0].y1 == 24, "north gate collision");
    require(collisionShape(gate.with("open", "true")).empty(),
            "open gate has no collision");
    auto eastGate = collisionShape(gate.with("facing", "east"));
    require(eastGate[0].x0 == 6 && eastGate[0].x1 == 10 &&
                eastGate[0].z1 == 16, "east gate collision rotates");
    auto gateOutline = outlineShape(gate.with("in_wall", "true"));
    require(gateOutline.size() == 1 && gateOutline[0].y0 == 0 &&
                gateOutline[0].y1 == 13, "in-wall gate outline");
    w.rawSetBlock({1, 0, 0}, gate);
    auto joinedWall = wallUpdateShape(w, {0, 0, 0}, wall, Direction::East);
    require(joinedWall.get("east") != "none", "wall connects to gate across its axis");
    w.rawSetBlock({1, 0, 0}, gate.with("facing", "east"));
    joinedWall = wallUpdateShape(w, {0, 0, 0}, joinedWall, Direction::East);
    require(joinedWall.get("east") == "none", "wall does not connect to gate along its axis");
    w.rawSetBlock({1, 0, 0}, airState());
    require(isWallBlock(stoneWall) && stoneWall.get("waterlogged") == "false",
            "stone brick wall shares wall defaults");
    Pos p{0, 0, 0};
    w.rawSetBlock(p, wall);
    w.rawSetBlock({0, 0, -1}, defaultState("smooth_stone"));
    auto s = wallUpdateShape(w, p, wall, Direction::North);
    require(s.get("north") == "low" && s.get("up") == "true",
            "wall connects to full block");
    auto stoneConnected = wallUpdateShape(w, p, stoneWall, Direction::North);
    require(stoneConnected.properties() == s.properties(),
            "stone brick wall has the same connection properties as cobblestone");
    w.rawSetBlock({1, 0, 0}, stoneWall);
    auto crossConnected = wallUpdateShape(w, p, wall, Direction::East);
    require(crossConnected.get("east") == "low",
            "cobblestone connects to stone brick wall tag member");
    w.rawSetBlock({1, 0, 0}, defaultState("air"));
    auto stoneShape = collisionShape(stoneConnected);
    auto cobbleShape = collisionShape(s);
    require(stoneShape.size() == cobbleShape.size(), "wall collision part counts");
    for (size_t i = 0; i < stoneShape.size(); ++i) {
      auto a = stoneShape[i], b = cobbleShape[i];
      require(a.x0 == b.x0 && a.y0 == b.y0 && a.z0 == b.z0 &&
                  a.x1 == b.x1 && a.y1 == b.y1 && a.z1 == b.z1,
              "stone wall collision matches original wall including 24/16 height");
    }
    w.rawSetBlock({0, 0, 1}, defaultState("slime_block"));
    s = wallUpdateShape(w, p, s, Direction::South);
    require(s.get("south") == "low" && s.get("up") == "false",
            "straight wall removes post");
    w.rawSetBlock({0, 1, 0}, defaultState("smooth_stone"));
    s = wallUpdateShape(w, p, s, Direction::Up);
    require(s.get("north") == "tall" && s.get("south") == "tall" &&
                s.get("up") == "false",
            "covered straight wall is tall without post");
    w.rawSetBlock({0, 0, -1}, defaultState("honey_block"));
    s = wallUpdateShape(w, p, s, Direction::North);
    require(s.get("north") == "none", "honey inset face does not connect");
    require(faceSturdy(defaultState("slime_block"), Direction::Up),
            "slime sturdy top");
    require(!faceSturdy(defaultState("honey_block"), Direction::Up),
            "honey inset top");
    auto piston =
        defaultState("piston").with("facing", "up").with("extended", "true");
    require(!faceSturdy(piston, Direction::Up) &&
                faceSturdy(piston, Direction::Down),
            "extended piston face geometry");
    w.rawSetBlock({0, -1, 0}, defaultState("glass"));
    require(coralCanSurvive(w, p, defaultState("dead_tube_coral_fan")),
            "coral supported by glass");
    w.rawSetBlock({0, -1, 0}, defaultState("honey_block"));
    require(!coralCanSurvive(w, p, defaultState("dead_tube_coral_fan")),
            "coral cannot stand on honey");
    auto carpet = defaultState("blue_carpet");
    auto carpetShape = collisionShape(carpet);
    require(carpetShape.size() == 1 && carpetShape[0].y0 == 0 &&
                carpetShape[0].y1 == 1 && carpetShape[0].x1 == 16 &&
                carpetShape[0].z1 == 16,
            "carpet collision height");
    require(!faceSturdy(carpet, Direction::Up), "carpet is not sturdy on top");
    const Direction directions[] = {Direction::Down, Direction::Up,
                                    Direction::North, Direction::South,
                                    Direction::West, Direction::East};
    for (auto d : directions) {
      require(faceSturdy(defaultState("smooth_stone"), d),
              "full cube is sturdy on every face");
      require(!faceSturdy(defaultState("air"), d),
              "empty shape is never sturdy");
      require(!faceSturdy(defaultState("honey_block"), d),
              "inset honey is not sturdy on any face");
      require(faceSturdy(carpet, d) == (d == Direction::Down),
              "carpet supports only its bottom face");
      require(faceSturdy(w, p, piston, d) == faceSturdy(piston, d),
              "world and cached shape agree for ordinary blocks");
    }
    require(faceSturdy(piston, Direction::North) == false &&
                faceSturdy(piston, Direction::South) == false &&
                faceSturdy(piston, Direction::East) == false &&
                faceSturdy(piston, Direction::West) == false,
            "extended upward piston side faces are incomplete");
    require(!faceSturdy(w, p, defaultState("moving_piston"), Direction::Up),
            "moving piston without block entity has empty collision");
    auto head = defaultState("piston_head").with("facing", "up");
    require(faceSturdy(head, Direction::Up) &&
                !faceSturdy(head, Direction::Down),
            "multi-box piston head has a sturdy outer face only");

    // Stair corner shapes: inner/outer left/right, plus half mismatch stays straight.
    {
      World sw;
      sw.loadChunk(0, 0);
      const Pos sp{8, 64, 8};
      const auto northBottom =
          defaultState("oak_stairs").with("facing", "north").with("half", "bottom");
      sw.rawSetBlock(sp, northBottom);
      sw.rawSetBlock(sp.relative(Direction::North),
                     defaultState("oak_stairs").with("facing", "west").with("half", "bottom"));
      require(stairUpdateShape(sw, sp, sw.getBlock(sp)).getView("shape") == "outer_left",
              "behind west-facing bottom stair creates outer_left");
      sw.rawSetBlock(sp.relative(Direction::North),
                     defaultState("oak_stairs").with("facing", "east").with("half", "bottom"));
      require(stairUpdateShape(sw, sp, sw.getBlock(sp)).getView("shape") == "outer_right",
              "behind east-facing bottom stair creates outer_right");
      sw.rawSetBlock(sp.relative(Direction::North),
                     defaultState("oak_stairs").with("facing", "west").with("half", "top"));
      require(stairUpdateShape(sw, sp, sw.getBlock(sp)).getView("shape") == "straight",
              "mismatched half ignores behind stair");
      sw.rawSetBlock(sp.relative(Direction::North), airState());
      sw.setBlock(sp.relative(Direction::South),
                  defaultState("oak_stairs").with("facing", "west").with("half", "bottom"));
      require(sw.getBlock(sp).getView("shape") == "inner_left",
              "neighbor setBlock updates stair to inner_left");
      sw.setBlock(sp.relative(Direction::South),
                  defaultState("oak_stairs").with("facing", "east").with("half", "bottom"));
      require(sw.getBlock(sp).getView("shape") == "inner_right",
              "neighbor setBlock updates stair to inner_right");
    }

    // Fence and iron bars / glass pane connection rules.
    {
      World fw;
      fw.loadChunk(0, 0);
      const Pos fp{8, 64, 8};
      fw.rawSetBlock(fp, defaultState("oak_fence"));
      fw.setBlock(fp.relative(Direction::North), defaultState("spruce_fence"));
      fw.setBlock(fp.relative(Direction::East), defaultState("nether_brick_fence"));
      fw.setBlock(fp.relative(Direction::South),
                  defaultState("oak_fence_gate").with("facing", "east"));
      fw.setBlock(fp.relative(Direction::West), defaultState("oak_leaves"));
      const auto f = fw.getBlock(fp);
      require(f.getView("north") == "true", "wooden fence connects to wooden fence");
      require(f.getView("east") == "false",
              "wooden fence does not connect to nether_brick_fence");
      require(f.getView("south") == "true",
              "fence connects to perpendicular fence gate");
      require(f.getView("west") == "false",
              "fence does not connect to leaves exception");

      const Pos gp{4, 64, 4};
      fw.rawSetBlock(gp, defaultState("glass_pane"));
      fw.setBlock(gp.relative(Direction::North), defaultState("iron_bars"));
      fw.setBlock(gp.relative(Direction::East), defaultState("cobblestone_wall"));
      fw.setBlock(gp.relative(Direction::South), defaultState("smooth_stone"));
      fw.setBlock(gp.relative(Direction::West), defaultState("barrier"));
      const auto g = fw.getBlock(gp);
      require(g.getView("north") == "true" && g.getView("east") == "true" &&
                  g.getView("south") == "true" && g.getView("west") == "false",
              "glass pane connects to bars, walls, sturdy blocks, and skips barrier");
      require(fw.getBlock(gp.relative(Direction::East)).getView("west") == "low",
              "cobblestone wall connects to glass pane / bars");
    }
    std::cout << "shape tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
