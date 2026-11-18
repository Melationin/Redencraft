#include "redstone/worldgen/jigsaw.hpp"

#include "picojson.h"
#include "redstone/block_tags.hpp"
#include "redstone/worldgen/biome_source.hpp"
#include "redstone/worldgen/noise_chunk_generator.hpp"
#include "redstone/worldgen/noise_router.hpp"
#include "redstone/worldgen/surface_rules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace redstone::worldgen {
namespace {

std::string normId(std::string_view id) {
  if (id.find(':') != std::string_view::npos)
    return std::string(id);
  return "minecraft:" + std::string(id);
}

std::string stripNamespace(std::string_view id) {
  const auto colon = id.find(':');
  return std::string(colon == std::string_view::npos ? id
                                                     : id.substr(colon + 1));
}

const picojson::object &asObj(const picojson::value &v, std::string_view ctx) {
  if (!v.is<picojson::object>())
    throw std::runtime_error("Expected object in " + std::string(ctx));
  return v.get<picojson::object>();
}

const picojson::value &req(const picojson::object &obj, const char *key,
                           std::string_view ctx) {
  auto it = obj.find(key);
  if (it == obj.end())
    throw std::runtime_error("Missing '" + std::string(key) + "' in " +
                             std::string(ctx));
  return it->second;
}

std::string asStr(const picojson::value &v, std::string_view ctx) {
  if (!v.is<std::string>())
    throw std::runtime_error("Expected string in " + std::string(ctx));
  return v.get<std::string>();
}

double asNum(const picojson::value &v, std::string_view ctx) {
  if (!v.is<double>())
    throw std::runtime_error("Expected number in " + std::string(ctx));
  return v.get<double>();
}

int asInt(const picojson::value &v, std::string_view ctx) {
  return static_cast<int>(asNum(v, ctx));
}

Direction rotateDirection(Direction d, Rotation rot) noexcept {
  if (d == Direction::Up || d == Direction::Down)
    return d;
  auto cw = [](Direction dir) noexcept {
    switch (dir) {
    case Direction::North:
      return Direction::East;
    case Direction::East:
      return Direction::South;
    case Direction::South:
      return Direction::West;
    case Direction::West:
      return Direction::North;
    default:
      return dir;
    }
  };
  switch (rot) {
  case Rotation::None:
    return d;
  case Rotation::Clockwise90:
    return cw(d);
  case Rotation::Clockwise180:
    return opposite(d);
  case Rotation::Counterclockwise90:
    return cw(opposite(d));
  }
  return d;
}

Direction mirrorDirection(Direction d, Mirror mirror) noexcept {
  if (mirror == Mirror::FrontBack &&
      (d == Direction::East || d == Direction::West)) {
    return opposite(d);
  }
  if (mirror == Mirror::LeftRight &&
      (d == Direction::North || d == Direction::South)) {
    return opposite(d);
  }
  return d;
}

Rotation mirrorRotationAdjustment(Direction facing, Mirror mirror) noexcept {
  if (mirror == Mirror::LeftRight &&
      (facing == Direction::North || facing == Direction::South)) {
    return Rotation::Clockwise180;
  }
  if (mirror == Mirror::FrontBack &&
      (facing == Direction::East || facing == Direction::West)) {
    return Rotation::Clockwise180;
  }
  return Rotation::None;
}

void parseFrontAndTop(std::string_view orientation, Direction &front,
                      Direction &top) noexcept {
  const auto sep = orientation.find('_');
  if (sep == std::string_view::npos) {
    front = Direction::North;
    top = Direction::Up;
    return;
  }
  front = directionFromString(orientation.substr(0, sep));
  top = directionFromString(orientation.substr(sep + 1));
}

std::string formatFrontAndTop(Direction front, Direction top) {
  return directionName(front) + "_" + directionName(top);
}

int mirrorIndexedRotation(int rot, int steps, Mirror mirror) noexcept {
  const int half = steps / 2;
  const int corr = rot > half ? rot - steps : rot;
  if (mirror == Mirror::LeftRight)
    return (half - corr + steps) % steps;
  if (mirror == Mirror::FrontBack)
    return (steps - corr) % steps;
  return rot;
}

std::string rotateRailShape(std::string_view shape, Rotation rot) {
  if (rot == Rotation::None)
    return std::string(shape);
  if (rot == Rotation::Clockwise180) {
    if (shape == "ascending_east")
      return "ascending_west";
    if (shape == "ascending_west")
      return "ascending_east";
    if (shape == "ascending_north")
      return "ascending_south";
    if (shape == "ascending_south")
      return "ascending_north";
    if (shape == "south_east")
      return "north_west";
    if (shape == "south_west")
      return "north_east";
    if (shape == "north_west")
      return "south_east";
    if (shape == "north_east")
      return "south_west";
    return std::string(shape);
  }
  if (rot == Rotation::Counterclockwise90) {
    if (shape == "ascending_east")
      return "ascending_north";
    if (shape == "ascending_west")
      return "ascending_south";
    if (shape == "ascending_north")
      return "ascending_west";
    if (shape == "ascending_south")
      return "ascending_east";
    if (shape == "south_east")
      return "north_east";
    if (shape == "south_west")
      return "south_east";
    if (shape == "north_west")
      return "south_west";
    if (shape == "north_east")
      return "north_west";
    if (shape == "north_south")
      return "east_west";
    if (shape == "east_west")
      return "north_south";
    return std::string(shape);
  }
  // Clockwise90
  if (shape == "ascending_east")
    return "ascending_south";
  if (shape == "ascending_west")
    return "ascending_north";
  if (shape == "ascending_north")
    return "ascending_east";
  if (shape == "ascending_south")
    return "ascending_west";
  if (shape == "south_east")
    return "south_west";
  if (shape == "south_west")
    return "north_west";
  if (shape == "north_west")
    return "north_east";
  if (shape == "north_east")
    return "south_east";
  if (shape == "north_south")
    return "east_west";
  if (shape == "east_west")
    return "north_south";
  return std::string(shape);
}

BlockState rotateSingleState(BlockState state, Rotation rot) {
  if (rot == Rotation::None || state.isAir())
    return state;

  if (state.hasProperty("axis")) {
    state = rotatePillar(state, rot);
  }
  if (state.hasProperty("facing")) {
    const Direction f = directionFromString(state.getView("facing", "north"));
    const std::string rf = directionName(rotateDirection(f, rot));
    if (state.block().stateDefinition().find("facing")) {
      std::uint32_t dummy = 0;
      if (state.block().stateDefinition().find("facing")->indexOf(rf, dummy)) {
        state = state.with("facing", rf);
      }
    }
  }
  if (state.hasProperty("orientation")) {
    Direction f = Direction::North;
    Direction t = Direction::Up;
    parseFrontAndTop(state.getView("orientation", "north_up"), f, t);
    const std::string next = formatFrontAndTop(rotateDirection(f, rot),
                                               rotateDirection(t, rot));
    if (auto *prop = state.block().stateDefinition().find("orientation")) {
      std::uint32_t dummy = 0;
      if (prop->indexOf(next, dummy)) {
        state = state.with("orientation", next);
      }
    }
  }
  if (state.hasProperty("rotation")) {
    const int idx = std::stoi(std::string(state.getView("rotation", "0")));
    int delta = 0;
    if (rot == Rotation::Clockwise90)
      delta = 4;
    else if (rot == Rotation::Clockwise180)
      delta = 8;
    else if (rot == Rotation::Counterclockwise90)
      delta = 12;
    state = state.with("rotation", std::to_string((idx + delta) % 16));
  }
  if (state.hasProperty("north") && state.hasProperty("south") &&
      state.hasProperty("east") && state.hasProperty("west")) {
    const std::string n(state.getView("north"));
    const std::string e(state.getView("east"));
    const std::string s(state.getView("south"));
    const std::string w(state.getView("west"));
    if (rot == Rotation::Clockwise90) {
      state = state.with("north", w).with("east", n).with("south", e).with(
          "west", s);
    } else if (rot == Rotation::Clockwise180) {
      state = state.with("north", s).with("east", w).with("south", n).with(
          "west", e);
    } else if (rot == Rotation::Counterclockwise90) {
      state = state.with("north", e).with("east", s).with("south", w).with(
          "west", n);
    }
  }
  if (state.is(Blocks::RAIL) || state.is(Blocks::POWERED_RAIL) ||
      state.is(Blocks::DETECTOR_RAIL) || state.is(Blocks::ACTIVATOR_RAIL)) {
    if (state.hasProperty("shape")) {
      const std::string next = rotateRailShape(state.getView("shape"), rot);
      if (auto *prop = state.block().stateDefinition().find("shape")) {
        std::uint32_t dummy = 0;
        if (prop->indexOf(next, dummy)) {
          state = state.with("shape", next);
        }
      }
    }
  }
  return state;
}

BlockState mirrorSingleState(BlockState state, Mirror mirror) {
  if (mirror == Mirror::None || state.isAir())
    return state;

  if (state.hasProperty("facing")) {
    const Direction f = directionFromString(state.getView("facing", "north"));
    const Rotation adj = mirrorRotationAdjustment(f, mirror);
    if (adj != Rotation::None) {
      state = rotateSingleState(state, adj);
      if (state.hasProperty("hinge")) {
        const std::string_view h = state.getView("hinge", "left");
        state = state.with("hinge", h == "left" ? "right" : "left");
      }
      if (state.hasProperty("shape")) {
        const std::string_view s = state.getView("shape", "straight");
        if (s == "inner_left")
          state = state.with("shape", "inner_right");
        else if (s == "inner_right")
          state = state.with("shape", "inner_left");
        else if (s == "outer_left")
          state = state.with("shape", "outer_right");
        else if (s == "outer_right")
          state = state.with("shape", "outer_left");
      }
    }
  } else if (state.hasProperty("rotation")) {
    const int idx = std::stoi(std::string(state.getView("rotation", "0")));
    state = state.with("rotation",
                       std::to_string(mirrorIndexedRotation(idx, 16, mirror)));
  }

  if (state.hasProperty("orientation")) {
    Direction f = Direction::North;
    Direction t = Direction::Up;
    parseFrontAndTop(state.getView("orientation", "north_up"), f, t);
    const std::string next = formatFrontAndTop(mirrorDirection(f, mirror),
                                               mirrorDirection(t, mirror));
    if (auto *prop = state.block().stateDefinition().find("orientation")) {
      std::uint32_t dummy = 0;
      if (prop->indexOf(next, dummy)) {
        state = state.with("orientation", next);
      }
    }
  }

  if (state.hasProperty("north") && state.hasProperty("south") &&
      state.hasProperty("east") && state.hasProperty("west")) {
    if (mirror == Mirror::LeftRight) {
      const std::string n(state.getView("north"));
      const std::string s(state.getView("south"));
      state = state.with("north", s).with("south", n);
    } else if (mirror == Mirror::FrontBack) {
      const std::string e(state.getView("east"));
      const std::string w(state.getView("west"));
      state = state.with("east", w).with("west", e);
    }
  }
  return state;
}

BlockState transformBlockState(BlockState state, Mirror mirror, Rotation rot) {
  return rotateSingleState(mirrorSingleState(state, mirror), rot);
}

void jigsawOrientation(const BlockState &state, Direction &front,
                       Direction &top) noexcept {
  parseFrontAndTop(state.getView("orientation", "north_up"), front, top);
}

bool jigsawCanAttach(const JigsawBlockInfo &source,
                     const JigsawBlockInfo &target) noexcept {
  Direction sf = Direction::North, st = Direction::Up;
  Direction tf = Direction::North, tt = Direction::Up;
  jigsawOrientation(source.info.state, sf, st);
  jigsawOrientation(target.info.state, tf, tt);
  const bool rollable = (source.joint == "rollable");
  return sf == opposite(tf) && (rollable || st == tt) &&
         source.target == target.name;
}

std::vector<Rotation> shuffledRotations(RandomSource &rng) {
  std::vector<Rotation> rots = {Rotation::None, Rotation::Clockwise90,
                                Rotation::Clockwise180,
                                Rotation::Counterclockwise90};
  for (int i = static_cast<int>(rots.size()) - 1; i > 0; --i) {
    const int j = rng.nextInt(i + 1);
    std::swap(rots[static_cast<std::size_t>(i)],
              rots[static_cast<std::size_t>(j)]);
  }
  return rots;
}

template <class T> void javaShuffle(std::vector<T> &vec, RandomSource &rng) {
  for (int i = static_cast<int>(vec.size()); i > 1; --i) {
    const int j = rng.nextInt(i);
    std::swap(vec[static_cast<std::size_t>(i - 1)],
              vec[static_cast<std::size_t>(j)]);
  }
}

RulePredicate parseRulePredicate(const picojson::value &v,
                                 std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  RulePredicate pred;
  const std::string type =
      normId(asStr(req(obj, "predicate_type", ctx), ctx));
  if (type == "minecraft:always_true") {
    pred.kind = RulePredicate::Kind::AlwaysTrue;
  } else if (type == "minecraft:block_match") {
    pred.kind = RulePredicate::Kind::BlockMatch;
    pred.blockOrTag = normId(asStr(req(obj, "block", ctx), ctx));
  } else if (type == "minecraft:block_state_match") {
    pred.kind = RulePredicate::Kind::BlockStateMatch;
    pred.matchState = parseBlockStateJson(req(obj, "block_state", ctx), ctx);
  } else if (type == "minecraft:tag_match") {
    pred.kind = RulePredicate::Kind::TagMatch;
    pred.blockOrTag = normId(asStr(req(obj, "tag", ctx), ctx));
  } else if (type == "minecraft:random_block_match") {
    pred.kind = RulePredicate::Kind::RandomBlockMatch;
    pred.blockOrTag = normId(asStr(req(obj, "block", ctx), ctx));
    pred.probability = static_cast<float>(asNum(req(obj, "probability", ctx), ctx));
  } else if (type == "minecraft:random_block_state_match") {
    pred.kind = RulePredicate::Kind::RandomBlockStateMatch;
    pred.matchState = parseBlockStateJson(req(obj, "block_state", ctx), ctx);
    pred.probability = static_cast<float>(asNum(req(obj, "probability", ctx), ctx));
  } else {
    pred.kind = RulePredicate::Kind::AlwaysTrue;
  }
  return pred;
}

StructureProcessor parseSingleProcessor(const picojson::value &v,
                                        std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  StructureProcessor proc;
  const std::string type =
      normId(asStr(req(obj, "processor_type", ctx), ctx));
  if (type == "minecraft:rule") {
    proc.kind = StructureProcessor::Kind::Rule;
    const auto &arr = req(obj, "rules", ctx).get<picojson::array>();
    for (const auto &r : arr) {
      const auto &ro = asObj(r, ctx);
      ProcessorRule rule;
      rule.inputPredicate =
          parseRulePredicate(req(ro, "input_predicate", ctx), ctx);
      rule.locationPredicate =
          parseRulePredicate(req(ro, "location_predicate", ctx), ctx);
      rule.outputState =
          parseBlockStateJson(req(ro, "output_state", ctx), ctx);
      proc.rules.push_back(std::move(rule));
    }
  } else if (type == "minecraft:block_rot") {
    proc.kind = StructureProcessor::Kind::BlockRot;
    if (auto it = obj.find("integrity"); it != obj.end())
      proc.integrity = static_cast<float>(asNum(it->second, ctx));
    if (auto it = obj.find("rottable_blocks");
        it != obj.end() && it->second.is<std::string>()) {
      proc.rotTag = it->second.get<std::string>();
    }
  } else if (type == "minecraft:protected_blocks") {
    proc.kind = StructureProcessor::Kind::ProtectedBlocks;
    if (auto it = obj.find("value");
        it != obj.end() && it->second.is<std::string>()) {
      proc.protectedTag = it->second.get<std::string>();
    }
  } else if (type == "minecraft:capped") {
    proc.kind = StructureProcessor::Kind::Capped;
    const IntProvider lim = parseIntProviderJson(req(obj, "limit", ctx), ctx);
    proc.capLimit = lim.maxInclusive;
    proc.delegate.push_back(
        parseSingleProcessor(req(obj, "delegate", ctx), ctx));
  } else if (type == "minecraft:blackstone_replace") {
    proc.kind = StructureProcessor::Kind::BlackstoneReplace;
  } else if (type == "minecraft:lava_submerged_block") {
    proc.kind = StructureProcessor::Kind::LavaSubmerged;
  } else {
    proc.kind = StructureProcessor::Kind::Nop;
  }
  return proc;
}

StructurePoolElement parsePoolElement(const picojson::value &v,
                                      std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  StructurePoolElement el;
  const std::string type = normId(asStr(req(obj, "element_type", ctx), ctx));
  if (auto it = obj.find("projection"); it != obj.end() && it->second.is<std::string>()) {
    el.projectionRigid = (it->second.get<std::string>() != "terrain_matching");
  }
  if (type == "minecraft:empty_pool_element") {
    el.kind = StructurePoolElement::Kind::Empty;
  } else if (type == "minecraft:single_pool_element" ||
             type == "minecraft:legacy_single_pool_element") {
    el.kind = (type == "minecraft:legacy_single_pool_element")
                  ? StructurePoolElement::Kind::LegacySingle
                  : StructurePoolElement::Kind::Single;
    el.location = normId(asStr(req(obj, "location", ctx), ctx));
    if (auto it = obj.find("processors"); it != obj.end()) {
      if (it->second.is<std::string>()) {
        el.processorListId = normId(it->second.get<std::string>());
      } else if (it->second.is<picojson::object>()) {
        const auto pl = parseProcessorListJson(it->second, ctx);
        el.inlineProcessors = pl.processors;
      }
    }
  } else if (type == "minecraft:list_pool_element") {
    el.kind = StructurePoolElement::Kind::List;
    if (auto it = obj.find("elements");
        it != obj.end() && it->second.is<picojson::array>()) {
      for (const auto &c : it->second.get<picojson::array>()) {
        el.children.push_back(parsePoolElement(c, ctx));
      }
    }
  } else if (type == "minecraft:feature_pool_element") {
    el.kind = StructurePoolElement::Kind::Feature;
    if (auto it = obj.find("feature");
        it != obj.end() && it->second.is<std::string>()) {
      el.location = normId(it->second.get<std::string>());
    }
  }
  return el;
}

// 解析带属性的方块状态字符串（等价于 26.1 BlockStateParser.parseForBlock）：
// 例如 jigsaw 的 final_state 可能是
// "minecraft:oak_stairs[facing=east,half=bottom,shape=straight,waterlogged=false]"。
// 只按方块名查表会把整串当成方块名，从而造出一个 unsupported 假方块（光学属性缺失，
// 既不挡光也不参与方块逻辑），所以这里必须拆出方括号里的属性再逐条应用。
BlockState parseBlockStateString(std::string_view text) {
  const std::size_t bracket = text.find('[');
  if (bracket == std::string_view::npos)
    return defaultState(normId(std::string(text)));
  BlockState state =
      defaultState(normId(std::string(text.substr(0, bracket))));
  // 属性区只到第一个 ']' 为止。NBT 的 final_state 可能带尾随空白/回车：只在
  // “整串最后一个字符是 ]”时才去右括号会把 "false]" 当成属性值（100x100 生成时
  // 在 minecraft:acacia_fence 上触发过）。
  const std::size_t close = text.find(']', bracket);
  if (close != std::string_view::npos)
    text = text.substr(0, close);
  const auto trimEntry = [](std::string_view value) {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t'))
      value.remove_prefix(1);
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r' || value.back() == '\n'))
      value.remove_suffix(1);
    return value;
  };
  std::size_t pos = bracket + 1;
  while (pos < text.size()) {
    const std::size_t comma = text.find(',', pos);
    const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
    const std::string_view entry = trimEntry(text.substr(pos, end - pos));
    const std::size_t eq = entry.find('=');
    if (eq != std::string_view::npos) {
      const std::string key(trimEntry(entry.substr(0, eq)));
      const std::string value(trimEntry(entry.substr(eq + 1)));
      if (state.hasProperty(key))
        state = state.with(key, value);
    }
    if (comma == std::string_view::npos)
      break;
    pos = comma + 1;
  }
  return state;
}

BlockState readBlockStateFromNbt(const nbt::Tag &tag) {
  const nbt::Tag *nameTag = tag.get("Name");
  const std::string name =
      nameTag ? normId(nameTag->text) : std::string("minecraft:air");
  const nbt::Tag *propsTag = tag.get("Properties");
  if (!propsTag || propsTag->compound.empty()) {
    return defaultState(name);
  }
  std::map<std::string, std::string> props;
  for (const auto &[k, val] : propsTag->compound) {
    props.emplace(k, val.text);
  }
  return BlockState(name, std::move(props));
}

bool isFullCubeForTemplateSort(const BlockState &state) noexcept {
  if (state.isAir() || state.block().isLiquid())
    return false;
  return state.block().metadata().conductor;
}

std::string resolvePoolAlias(const StructureDefinition &def,
                             std::string_view poolId,
                             const std::map<std::string, std::string, std::less<>> &aliases) {
  const std::string key = normId(poolId);
  if (auto it = aliases.find(key); it != aliases.end())
    return it->second;
  (void)def;
  return key;
}

void resolveAliasEntry(const StructureDefinition::PoolAliasEntry &entry,
                       RandomSource &rng,
                       std::map<std::string, std::string, std::less<>> &out) {
  using Kind = StructureDefinition::PoolAliasEntry::Kind;
  if (entry.kind == Kind::Direct) {
    out[entry.alias] = entry.target;
  } else if (entry.kind == Kind::Random) {
    int total = 0;
    for (const auto &w : entry.weighted)
      total += w.weight;
    if (total <= 0)
      return;
    int roll = rng.nextInt(total);
    for (const auto &w : entry.weighted) {
      roll -= w.weight;
      if (roll < 0) {
        out[entry.alias] = w.target;
        break;
      }
    }
  } else if (entry.kind == Kind::RandomGroup) {
    int total = 0;
    for (const auto &g : entry.groups)
      total += g.weight;
    if (total <= 0)
      return;
    int roll = rng.nextInt(total);
    for (const auto &g : entry.groups) {
      roll -= g.weight;
      if (roll < 0) {
        for (const auto &sub : g.entries)
          resolveAliasEntry(sub, rng, out);
        break;
      }
    }
  }
}

std::vector<StructurePoolElement>
shuffledPoolTemplates(const StructureTemplatePool &pool, RandomSource &rng) {
  std::vector<StructurePoolElement> expanded;
  for (const auto &we : pool.elements) {
    for (int i = 0; i < we.weight; ++i)
      expanded.push_back(we.element);
  }
  javaShuffle(expanded, rng);
  return expanded;
}

std::optional<StructurePoolElement>
pickRandomPoolTemplate(const StructureTemplatePool &pool, RandomSource &rng) {
  int total = 0;
  for (const auto &we : pool.elements)
    total += we.weight;
  if (total <= 0)
    return std::nullopt;
  int roll = rng.nextInt(total);
  for (const auto &we : pool.elements) {
    roll -= we.weight;
    if (roll < 0)
      return we.element;
  }
  return std::nullopt;
}

BoundingBox elementBoundingBox(const StructurePoolElement &el,
                               const StructureTemplateCache &templates,
                               Pos pos, Rotation rot) {
  if (el.kind == StructurePoolElement::Kind::Single ||
      el.kind == StructurePoolElement::Kind::LegacySingle) {
    if (const auto *tmpl = templates.getOrLoad(el.location)) {
      return tmpl->boundingBox(pos, rot, Pos{0, 0, 0}, Mirror::None);
    }
  } else if (el.kind == StructurePoolElement::Kind::List &&
             !el.children.empty()) {
    return elementBoundingBox(el.children.front(), templates, pos, rot);
  }
  return BoundingBox{pos.x, pos.y, pos.z, pos.x, pos.y, pos.z};
}

std::vector<JigsawBlockInfo>
elementShuffledJigsaws(const StructurePoolElement &el,
                       const StructureTemplateCache &templates, Pos pos,
                       Rotation rot, RandomSource &rng) {
  std::vector<JigsawBlockInfo> jigsaws;
  if (el.kind == StructurePoolElement::Kind::Single ||
      el.kind == StructurePoolElement::Kind::LegacySingle) {
    if (const auto *tmpl = templates.getOrLoad(el.location)) {
      jigsaws = tmpl->getJigsaws(pos, rot);
    }
  } else if (el.kind == StructurePoolElement::Kind::Feature) {
    JigsawBlockInfo jb;
    jb.info.pos = pos;
    jb.info.state = Blocks::JIGSAW.defaultState().with("orientation", "down_south");
    jb.name = "minecraft:bottom";
    jb.target = "minecraft:empty";
    jb.pool = "minecraft:empty";
    jb.finalState = "minecraft:air";
    jb.joint = "rollable";
    jigsaws.push_back(std::move(jb));
  } else if (el.kind == StructurePoolElement::Kind::List &&
             !el.children.empty()) {
    jigsaws =
        elementShuffledJigsaws(el.children.front(), templates, pos, rot, rng);
  }
  javaShuffle(jigsaws, rng);
  std::stable_sort(jigsaws.begin(), jigsaws.end(),
                   [](const JigsawBlockInfo &a, const JigsawBlockInfo &b) {
                     return a.selectionPriority > b.selectionPriority;
                   });
  return jigsaws;
}

int elementMaxYSpan(const StructureTemplatePool &pool,
                    const StructureTemplateCache &templates) {
  int maxSpan = 0;
  for (const auto &we : pool.elements) {
    if (we.element.kind == StructurePoolElement::Kind::Empty)
      continue;
    const BoundingBox bb =
        elementBoundingBox(we.element, templates, Pos{0, 0, 0}, Rotation::None);
    maxSpan = std::max(maxSpan, bb.ySpan());
  }
  return maxSpan;
}

// 精确等价 Java `VoxelShape`：`outerBox \ carvedBoxes` 的无碰撞测试。
// `targetBB`（按 0.25 收缩后的内盒）不越出 `outerBox`，也不与任一 `carvedBoxes` 重叠。
struct FreeRegion {
  BoundingBox outerBox{};
  std::vector<BoundingBox> carvedHoles;

  bool fitsWithoutCollision(const BoundingBox &target) const noexcept {
    if (target.minX < outerBox.minX || target.maxX > outerBox.maxX ||
        target.minY < outerBox.minY || target.maxY > outerBox.maxY ||
        target.minZ < outerBox.minZ || target.maxZ > outerBox.maxZ) {
      return false;
    }
    for (const BoundingBox &hole : carvedHoles) {
      if (target.intersects(hole))
        return false;
    }
    return true;
  }

  void carve(const BoundingBox &box) { carvedHoles.push_back(box); }
};

void applyProcessorRulePass(const StructureProcessor &proc,
                            WorldGenRegion &region, Pos piecePos, Pos refPos,
                            std::vector<StructureBlockInfo> &processed,
                            const std::vector<StructureBlockInfo> &originals,
                            std::vector<bool> &removed,
                            RandomSource &rng,
                            bool useRngForBlockRot = false) {
  switch (proc.kind) {
  case StructureProcessor::Kind::Rule: {
    for (std::size_t i = 0; i < processed.size(); ++i) {
      if (removed[i])
        continue;
      auto &info = processed[i];
      LegacyRandomSource posRng(positionalSeedHash(info.pos.x, info.pos.y, info.pos.z));
      const BlockState existing =
          region.getBlockState(info.pos.x, info.pos.y, info.pos.z);
      for (const auto &rule : proc.rules) {
        const bool inOk = rule.inputPredicate.test(info.state, posRng);
        const bool locOk = rule.locationPredicate.test(existing, posRng);
        if (inOk && locOk) {
          info.state = rule.outputState;
          break;
        }
      }
    }
    break;
  }
  case StructureProcessor::Kind::BlockRot: {
    for (std::size_t i = 0; i < processed.size(); ++i) {
      if (removed[i])
        continue;
      auto &info = processed[i];
      if (!proc.rotTag.empty() && !blockHasTag(info.state, proc.rotTag).value_or(false))
        continue;
      if (proc.integrity < 1.0f) {
        if (useRngForBlockRot) {
          if (rng.nextFloat() > proc.integrity)
            removed[i] = true;
        } else {
          LegacyRandomSource posRng(
              positionalSeedHash(info.pos.x, info.pos.y, info.pos.z));
          if (posRng.nextFloat() >= proc.integrity)
            removed[i] = true;
        }
      }
    }
    break;
  }
  case StructureProcessor::Kind::ProtectedBlocks: {
    for (std::size_t i = 0; i < processed.size(); ++i) {
      if (removed[i] || proc.protectedTag.empty())
        continue;
      const BlockState existing =
          region.getBlockState(processed[i].pos.x, processed[i].pos.y,
                               processed[i].pos.z);
      if (blockHasTag(existing, proc.protectedTag).value_or(false)) {
        removed[i] = true;
      }
    }
    break;
  }
  case StructureProcessor::Kind::BlackstoneReplace: {
    for (std::size_t i = 0; i < processed.size(); ++i) {
      if (removed[i])
        continue;
      const std::string_view name = processed[i].state.nameView();
      if (name == "minecraft:cobblestone")
        processed[i].state = BlockState("minecraft:blackstone",
                                        processed[i].state.properties());
      else if (name == "minecraft:mossy_cobblestone")
        processed[i].state = BlockState("minecraft:blackstone",
                                        processed[i].state.properties());
      else if (name == "minecraft:stone_bricks")
        processed[i].state = BlockState("minecraft:polished_blackstone_bricks",
                                        processed[i].state.properties());
      else if (name == "minecraft:cracked_stone_bricks")
        processed[i].state = BlockState(
            "minecraft:cracked_polished_blackstone_bricks",
            processed[i].state.properties());
      else if (name == "minecraft:cobblestone_stairs")
        processed[i].state = BlockState("minecraft:blackstone_stairs",
                                        processed[i].state.properties());
      else if (name == "minecraft:cobblestone_slab")
        processed[i].state = BlockState("minecraft:blackstone_slab",
                                        processed[i].state.properties());
      else if (name == "minecraft:cobblestone_wall")
        processed[i].state = BlockState("minecraft:blackstone_wall",
                                        processed[i].state.properties());
    }
    break;
  }
  case StructureProcessor::Kind::Capped: {
    if (proc.delegate.empty() || proc.capLimit <= 0 || processed.empty())
      break;
    const int n = static_cast<int>(processed.size());
    std::vector<int> indices(static_cast<std::size_t>(n));
    std::iota(indices.begin(), indices.end(), 0);
    javaShuffle(indices, rng);
    const int maxReplace = std::min(proc.capLimit, n);
    int replaced = 0;
    const auto &del = proc.delegate.front();
    for (int idx : indices) {
      if (replaced >= maxReplace)
        break;
      const auto ui = static_cast<std::size_t>(idx);
      if (removed[ui])
        continue;
      if (del.kind == StructureProcessor::Kind::Rule) {
        auto &info = processed[ui];
        LegacyRandomSource posRng(positionalSeedHash(info.pos.x, info.pos.y, info.pos.z));
        const BlockState existing =
            region.getBlockState(info.pos.x, info.pos.y, info.pos.z);
        for (const auto &rule : del.rules) {
          if (rule.inputPredicate.test(info.state, posRng) &&
              rule.locationPredicate.test(existing, posRng)) {
            if (!(info.state == rule.outputState)) {
              info.state = rule.outputState;
              ++replaced;
            }
            break;
          }
        }
      }
    }
    break;
  }
  case StructureProcessor::Kind::LavaSubmerged: {
    for (std::size_t i = 0; i < processed.size(); ++i) {
      if (removed[i])
        continue;
      const BlockState existing =
          region.getBlockState(processed[i].pos.x, processed[i].pos.y,
                               processed[i].pos.z);
      if (existing.is(Blocks::LAVA) && !processed[i].state.isAir()) {
        processed[i].state = existing;
      }
    }
    break;
  }
  case StructureProcessor::Kind::Nop:
    break;
  }
  (void)piecePos;
  (void)refPos;
  (void)originals;
}

} // namespace

Pos StructureTemplate::rotatedSize(Rotation rotation) const noexcept {
  if (rotation == Rotation::Clockwise90 ||
      rotation == Rotation::Counterclockwise90) {
    return Pos{size.z, size.y, size.x};
  }
  return size;
}

Pos StructureTemplate::transformPos(Pos p, Mirror mirror, Rotation rotation,
                                    Pos pivot) noexcept {
  int x = p.x;
  const int y = p.y;
  int z = p.z;
  bool mirrored = true;
  if (mirror == Mirror::LeftRight) {
    z = -z;
  } else if (mirror == Mirror::FrontBack) {
    x = -x;
  } else {
    mirrored = false;
  }
  const int px = pivot.x;
  const int pz = pivot.z;
  switch (rotation) {
  case Rotation::Counterclockwise90:
    return Pos{px - pz + z, y, px + pz - x};
  case Rotation::Clockwise90:
    return Pos{px + pz - z, y, pz - px + x};
  case Rotation::Clockwise180:
    return Pos{px + px - x, y, pz + pz - z};
  case Rotation::None:
    return mirrored ? Pos{x, y, z} : p;
  }
  return p;
}

Pos StructureTemplate::getZeroPositionWithTransform(Pos zeroPos, Mirror mirror,
                                                    Rotation rotation,
                                                    int sizeX,
                                                    int sizeZ) noexcept {
  --sizeX;
  --sizeZ;
  const int mx = (mirror == Mirror::FrontBack) ? sizeX : 0;
  const int mz = (mirror == Mirror::LeftRight) ? sizeZ : 0;
  switch (rotation) {
  case Rotation::Counterclockwise90:
    return Pos{zeroPos.x + mz, zeroPos.y, zeroPos.z + (sizeX - mx)};
  case Rotation::Clockwise90:
    return Pos{zeroPos.x + (sizeZ - mz), zeroPos.y, zeroPos.z + mx};
  case Rotation::Clockwise180:
    return Pos{zeroPos.x + (sizeX - mx), zeroPos.y, zeroPos.z + (sizeZ - mz)};
  case Rotation::None:
    return Pos{zeroPos.x + mx, zeroPos.y, zeroPos.z + mz};
  }
  return zeroPos;
}

BoundingBox StructureTemplate::boundingBox(Pos anchor, Rotation rotation,
                                           Pos pivot,
                                           Mirror mirror) const noexcept {
  const Pos delta{size.x - 1, size.y - 1, size.z - 1};
  const Pos c1 = transformPos(Pos{0, 0, 0}, mirror, rotation, pivot);
  const Pos c2 = transformPos(delta, mirror, rotation, pivot);
  return BoundingBox::fromCorners(c1, c2).moved(anchor.x, anchor.y, anchor.z);
}

std::vector<JigsawBlockInfo> StructureTemplate::getJigsaws(Pos pos,
                                                           Rotation rotation) const {
  std::vector<JigsawBlockInfo> out;
  if (palettes.empty())
    return out;
  std::size_t palIdx = 0;
  if (palettes.size() > 1) {
    LegacyRandomSource r(positionalSeedHash(pos.x, pos.y, pos.z));
    palIdx = static_cast<std::size_t>(
        r.nextInt(static_cast<std::int32_t>(palettes.size())));
  }
  const auto &src =
      palettes[palIdx].jigsawsByRotation[static_cast<std::size_t>(rotation)];
  if (pos.x == 0 && pos.y == 0 && pos.z == 0)
    return src;
  out = src;
  for (auto &j : out) {
    j.info.pos.x += pos.x;
    j.info.pos.y += pos.y;
    j.info.pos.z += pos.z;
  }
  return out;
}

StructureTemplate
StructureTemplate::loadFromGzipNbtFile(const std::filesystem::path &p) {
  std::ifstream in(p, std::ios::binary | std::ios::ate);
  if (!in.is_open())
    throw std::runtime_error("Cannot open structure template: " + p.string());
  const auto sz = in.tellg();
  std::vector<std::uint8_t> compressed(static_cast<std::size_t>(sz));
  in.seekg(0);
  in.read(reinterpret_cast<char *>(compressed.data()), sz);

  const auto raw = nbt::decompress(compressed, 31);
  const auto root = nbt::parseRoot(raw);

  StructureTemplate tmpl;
  if (const nbt::Tag *sizeTag = root.value.get("size");
      sizeTag && sizeTag->list.size() >= 3) {
    tmpl.size = Pos{static_cast<int>(sizeTag->list[0].number),
                    static_cast<int>(sizeTag->list[1].number),
                    static_cast<int>(sizeTag->list[2].number)};
  }

  const nbt::Tag *blocksTag = root.value.get("blocks");
  auto loadPalette = [&](const nbt::Tag &paletteTag) {
    std::vector<BlockState> palStates;
    palStates.reserve(paletteTag.list.size());
    for (const auto &stTag : paletteTag.list)
      palStates.push_back(readBlockStateFromNbt(stTag));

    std::vector<StructureBlockInfo> fullBlocks;
    std::vector<StructureBlockInfo> entityBlocks;
    std::vector<StructureBlockInfo> otherBlocks;
    std::vector<JigsawBlockInfo> jigsaws;

    if (blocksTag) {
      for (const auto &bTag : blocksTag->list) {
        const nbt::Tag *posTag = bTag.get("pos");
        const nbt::Tag *stIdxTag = bTag.get("state");
        if (!posTag || posTag->list.size() < 3 || !stIdxTag)
          continue;
        Pos bpos{static_cast<int>(posTag->list[0].number),
                 static_cast<int>(posTag->list[1].number),
                 static_cast<int>(posTag->list[2].number)};
        const auto idx = static_cast<std::size_t>(stIdxTag->number);
        BlockState state =
            idx < palStates.size() ? palStates[idx] : BlockState{};
        const nbt::Tag *nbtPtr = bTag.get("nbt");
        StructureBlockInfo info{bpos, state,
                                nbtPtr ? std::optional<nbt::Tag>(*nbtPtr)
                                       : std::nullopt};
        if (state.is(Blocks::JIGSAW) && nbtPtr) {
          JigsawBlockInfo jb;
          jb.info = StructureBlockInfo{bpos, state, std::nullopt};
          if (const auto *t = nbtPtr->get("name"))
            jb.name = normId(t->text);
          if (const auto *t = nbtPtr->get("target"))
            jb.target = normId(t->text);
          if (const auto *t = nbtPtr->get("pool"))
            jb.pool = normId(t->text);
          if (const auto *t = nbtPtr->get("final_state"))
            jb.finalState = t->text;
          if (const auto *t = nbtPtr->get("joint"))
            jb.joint = t->text;
          if (const auto *t = nbtPtr->get("placement_priority"))
            jb.placementPriority = static_cast<int>(t->number);
          if (const auto *t = nbtPtr->get("selection_priority"))
            jb.selectionPriority = static_cast<int>(t->number);
          jigsaws.push_back(std::move(jb));
        }

        if (nbtPtr) {
          entityBlocks.push_back(std::move(info));
        } else if (isFullCubeForTemplateSort(state)) {
          fullBlocks.push_back(std::move(info));
        } else {
          otherBlocks.push_back(std::move(info));
        }
      }
    }

    auto yxzOrder = [](const StructureBlockInfo &a,
                       const StructureBlockInfo &b) {
      if (a.pos.y != b.pos.y)
        return a.pos.y < b.pos.y;
      if (a.pos.x != b.pos.x)
        return a.pos.x < b.pos.x;
      return a.pos.z < b.pos.z;
    };
    std::sort(fullBlocks.begin(), fullBlocks.end(), yxzOrder);
    std::sort(otherBlocks.begin(), otherBlocks.end(), yxzOrder);
    std::sort(entityBlocks.begin(), entityBlocks.end(), yxzOrder);

    StructurePalette sp;
    sp.blocks.reserve(fullBlocks.size() + otherBlocks.size() +
                      entityBlocks.size());
    sp.blocks.insert(sp.blocks.end(),
                     std::make_move_iterator(fullBlocks.begin()),
                     std::make_move_iterator(fullBlocks.end()));
    sp.blocks.insert(sp.blocks.end(),
                     std::make_move_iterator(otherBlocks.begin()),
                     std::make_move_iterator(otherBlocks.end()));
    sp.blocks.insert(sp.blocks.end(),
                     std::make_move_iterator(entityBlocks.begin()),
                     std::make_move_iterator(entityBlocks.end()));
    sp.jigsaws = std::move(jigsaws);
    constexpr std::array<Rotation, 4> kAllRotations{
        Rotation::None, Rotation::Clockwise90, Rotation::Clockwise180,
        Rotation::Counterclockwise90};
    for (std::size_t r = 0; r < 4; ++r) {
      const Rotation rot = kAllRotations[r];
      auto &dst = sp.jigsawsByRotation[r];
      dst.reserve(sp.jigsaws.size());
      for (const auto &j : sp.jigsaws) {
        JigsawBlockInfo rj = j;
        rj.info.pos = transformPos(j.info.pos, Mirror::None, rot, Pos{0, 0, 0});
        rj.info.state = rotateSingleState(j.info.state, rot);
        dst.push_back(std::move(rj));
      }
    }
    tmpl.palettes.push_back(std::move(sp));
  };

  if (const nbt::Tag *palettesTag = root.value.get("palettes")) {
    for (const auto &pTag : palettesTag->list)
      loadPalette(pTag);
  } else if (const nbt::Tag *paletteTag = root.value.get("palette")) {
    loadPalette(*paletteTag);
  }
  return tmpl;
}

const StructureTemplate *
StructureTemplateCache::getOrLoad(std::string_view templateId) const {
  const std::string key = normId(templateId);
  {
    std::lock_guard lock(mutex_);
    if (auto it = cache_.find(key); it != cache_.end())
      return it->second.get();
  }
  const std::filesystem::path file =
      nbtRoot_ / (stripNamespace(key) + ".nbt");
  if (!std::filesystem::exists(file))
    return nullptr;
  auto loaded =
      std::make_shared<StructureTemplate>(StructureTemplate::loadFromGzipNbtFile(file));
  std::lock_guard lock(mutex_);
  auto [it, inserted] = cache_.emplace(key, std::move(loaded));
  return it->second.get();
}

bool RulePredicate::test(const BlockState &state, RandomSource &rng) const {
  switch (kind) {
  case Kind::AlwaysTrue:
    return true;
  case Kind::BlockMatch:
    return state.nameView() == blockOrTag;
  case Kind::BlockStateMatch:
    return state == matchState;
  case Kind::TagMatch:
    return blockHasTag(state, blockOrTag).value_or(false);
  case Kind::RandomBlockMatch:
    return state.nameView() == blockOrTag && rng.nextFloat() < probability;
  case Kind::RandomBlockStateMatch:
    return state == matchState && rng.nextFloat() < probability;
  }
  return false;
}

StructureProcessorList parseProcessorListJson(const picojson::value &v,
                                              std::string_view id) {
  const auto &obj = asObj(v, id);
  StructureProcessorList out;
  out.id = std::string(id);
  if (auto it = obj.find("processors");
      it != obj.end() && it->second.is<picojson::array>()) {
    for (const auto &p : it->second.get<picojson::array>()) {
      out.processors.push_back(parseSingleProcessor(p, id));
    }
  }
  return out;
}

StructureTemplatePool parseTemplatePoolJson(const picojson::value &v,
                                            std::string_view id) {
  const auto &obj = asObj(v, id);
  StructureTemplatePool out;
  out.id = std::string(id);
  if (auto it = obj.find("fallback");
      it != obj.end() && it->second.is<std::string>()) {
    out.fallback = normId(it->second.get<std::string>());
  }
  if (auto it = obj.find("elements");
      it != obj.end() && it->second.is<picojson::array>()) {
    for (const auto &el : it->second.get<picojson::array>()) {
      const auto &eo = asObj(el, id);
      StructureTemplatePool::WeightedElement we;
      we.weight = asInt(req(eo, "weight", id), id);
      we.element = parsePoolElement(req(eo, "element", id), id);
      out.elements.push_back(std::move(we));
    }
  }
  return out;
}

std::optional<StructureStart> generateJigsawStructureStart(
    const StructureDefinition &def, const NoiseBasedChunkGenerator &gen,
    RandomState &rs, const ChunkGeneratorStructureState &structState,
    std::int64_t worldSeed, ChunkTicketPos chunkPos, int minY, int height) {
  (void)structState;
  const auto &regs = gen.registries();
  const auto *templates = regs.templateCache();
  if (!templates)
    return std::nullopt;

  LegacyRandomSource random(0);
  worldgen_random::setLargeFeatureSeed(random, worldSeed, chunkPos.x,
                                       chunkPos.z);

  const WorldGenerationContext wgCtx{minY, height};
  const int startY = def.startHeight.sample(random, wgCtx);
  const Pos startPos{chunkPos.x << 4, startY, chunkPos.z << 4};

  // PoolAliasLookup.create
  std::map<std::string, std::string, std::less<>> aliases;
  if (!def.poolAliases.empty()) {
    LegacyRandomSource base(worldSeed);
    PositionalRandomFactory posFactory = base.forkPositional();
    AnyRandomSource aliasRng = posFactory.at(startPos.x, startPos.y, startPos.z);
    for (const auto &entry : def.poolAliases)
      resolveAliasEntry(entry, aliasRng, aliases);
  }

  const Rotation centerRot =
      static_cast<Rotation>(random.nextInt(4));
  const std::string resolvedStartPool =
      resolvePoolAlias(def, def.startPool, aliases);
  const auto *centerPool = regs.findTemplatePool(resolvedStartPool);
  if (!centerPool)
    return std::nullopt;
  const auto centerElementOpt = pickRandomPoolTemplate(*centerPool, random);
  if (!centerElementOpt ||
      centerElementOpt->kind == StructurePoolElement::Kind::Empty) {
    return std::nullopt;
  }
  const StructurePoolElement &centerElement = *centerElementOpt;

  Pos anchoredPos = startPos;
  if (!def.startJigsawName.empty()) {
    const auto jigsaws = elementShuffledJigsaws(centerElement, *templates,
                                                startPos, centerRot, random);
    bool found = false;
    for (const auto &jb : jigsaws) {
      if (jb.name == def.startJigsawName) {
        anchoredPos = jb.info.pos;
        found = true;
        break;
      }
    }
    if (!found)
      return std::nullopt;
  }

  const Pos localAnchor{anchoredPos.x - startPos.x,
                        anchoredPos.y - startPos.y,
                        anchoredPos.z - startPos.z};
  Pos adjustedPos{startPos.x - localAnchor.x, startPos.y - localAnchor.y,
                  startPos.z - localAnchor.z};

  BoundingBox box =
      elementBoundingBox(centerElement, *templates, adjustedPos, centerRot);
  const int centerX = (box.maxX + box.minX) / 2;
  const int centerZ = (box.maxZ + box.minZ) / 2;
  const int bottomY =
      def.projectStartToHeightmap.has_value()
          ? startPos.y +
                gen.getBaseHeight(centerX, centerZ,
                                  *def.projectStartToHeightmap, worldSeed)
          : adjustedPos.y;
  const int groundLevelDelta = 1;
  const int oldAbsGroundY = box.minY + groundLevelDelta;
  const int dyShift = bottomY - oldAbsGroundY;
  adjustedPos.y += dyShift;
  box = box.moved(0, dyShift, 0);

  if (def.dimensionPaddingBottom != 0 || def.dimensionPaddingTop != 0) {
    const int minWithPad = minY + def.dimensionPaddingBottom;
    const int maxWithPad = (minY + height - 1) - def.dimensionPaddingTop;
    if (box.minY < minWithPad || box.maxY > maxWithPad)
      return std::nullopt;
  }

  const int centerY = bottomY + localAnchor.y;
  // Structure.isValidBiome 检查结构起点的生物群系是否符合 `def.biomeSelectors`。
  {
    const auto biome = gen.biomeSource().getNoiseBiome(
        centerX >> 2, centerY >> 2, centerZ >> 2, rs.sampler());
    bool allowed = def.biomeSelectors.empty();
    for (const auto &sel : def.biomeSelectors) {
      if (regs.biomeMatches(biome, sel)) {
        allowed = true;
        break;
      }
    }
    if (!allowed)
      return std::nullopt;
  }

  auto makePiece = [&](const StructurePoolElement &el, Pos pos, int gDelta,
                       Rotation rot, BoundingBox bb) {
    StructurePiece piece;
    piece.kind = StructurePiece::Kind::PoolElement;
    piece.boundingBox = bb;
    piece.groundLevelDelta = gDelta;
    piece.rigidProjection = el.projectionRigid;
    piece.rotation = rot;
    piece.mirror = Mirror::None;
    piece.position = pos;
    piece.pivot = Pos{0, 0, 0};
    piece.liquidSettings = def.liquidSettings;
    if (el.kind == StructurePoolElement::Kind::Single ||
        el.kind == StructurePoolElement::Kind::LegacySingle) {
      piece.templateId = el.location;
      piece.processorListId = el.processorListId;
      piece.overwriteAirOnly =
          (el.kind == StructurePoolElement::Kind::LegacySingle);
    } else if (el.kind == StructurePoolElement::Kind::Feature) {
      piece.templateId = el.location;
    } else if (el.kind == StructurePoolElement::Kind::List &&
               !el.children.empty()) {
      piece.templateId = el.children.front().location;
      piece.processorListId = el.children.front().processorListId;
      piece.overwriteAirOnly =
          (el.children.front().kind == StructurePoolElement::Kind::LegacySingle);
    }
    return piece;
  };

  StructureStart result;
  result.structureId = def.id;
  result.chunkPos = chunkPos;
  result.terrainAdjustment = def.terrainAdjustment;
  result.pieces.push_back(
      makePiece(centerElement, adjustedPos, groundLevelDelta, centerRot, box));

  if (def.size > 0) {
    auto outerFree = std::make_shared<FreeRegion>();
    outerFree->outerBox = BoundingBox{
        centerX - def.maxDistanceHorizontal,
        std::max(centerY - def.maxDistanceVertical,
                 minY + def.dimensionPaddingBottom),
        centerZ - def.maxDistanceHorizontal,
        centerX + def.maxDistanceHorizontal,
        std::min(centerY + def.maxDistanceVertical,
                 minY + height - 1 - def.dimensionPaddingTop),
        centerZ + def.maxDistanceHorizontal};
    outerFree->carve(box);

    struct PieceState {
      std::size_t pieceIdx = 0;
      StructurePoolElement element;
      std::shared_ptr<FreeRegion> free;
      int depth = 0;
    };
    std::map<int, std::deque<PieceState>, std::greater<int>> placing;

    auto tryChildren = [&](const PieceState &srcState) {
      const StructurePiece srcPieceCopy = result.pieces[srcState.pieceIdx];
      const BoundingBox srcBB = srcPieceCopy.boundingBox;
      const int srcBoxY = srcBB.minY;
      const bool srcRigid = srcState.element.projectionRigid;
      std::shared_ptr<FreeRegion> insideFree;

      for (const auto &srcJigsaw :
           elementShuffledJigsaws(srcState.element, *templates,
                                  srcPieceCopy.position, srcPieceCopy.rotation,
                                  random)) {
        Direction srcDir = Direction::North, srcTop = Direction::Up;
        jigsawOrientation(srcJigsaw.info.state, srcDir, srcTop);
        const Pos srcJigPos = srcJigsaw.info.pos;
        const Pos targetJigPos = srcJigPos.relative(srcDir);
        const int srcJigLocalY = srcJigPos.y - srcBoxY;
        int cachedBaseHeight = std::numeric_limits<int>::min();

        const std::string targetPoolId =
            resolvePoolAlias(def, srcJigsaw.pool, aliases);
        const auto *targetPool = regs.findTemplatePool(targetPoolId);
        if (!targetPool)
          continue;
        if (targetPool->elements.empty() && targetPoolId != "minecraft:empty")
          continue;
        const auto *fallbackPool = regs.findTemplatePool(targetPool->fallback);
        if (!fallbackPool && targetPool->fallback != "minecraft:empty")
          continue;

        const bool attachInside = srcBB.isInside(targetJigPos.x, targetJigPos.y,
                                                 targetJigPos.z);
        std::shared_ptr<FreeRegion> childrenFree;
        if (attachInside) {
          if (!insideFree) {
            insideFree = std::make_shared<FreeRegion>();
            insideFree->outerBox = srcBB;
          }
          childrenFree = insideFree;
        } else {
          childrenFree = srcState.free;
        }

        std::vector<StructurePoolElement> candidates;
        if (srcState.depth != def.size) {
          auto mainCands = shuffledPoolTemplates(*targetPool, random);
          candidates.insert(candidates.end(),
                            std::make_move_iterator(mainCands.begin()),
                            std::make_move_iterator(mainCands.end()));
        }
        if (fallbackPool) {
          auto fbCands = shuffledPoolTemplates(*fallbackPool, random);
          candidates.insert(candidates.end(),
                            std::make_move_iterator(fbCands.begin()),
                            std::make_move_iterator(fbCands.end()));
        }

        bool placedThisJigsaw = false;
        for (const auto &targetEl : candidates) {
          if (placedThisJigsaw ||
              targetEl.kind == StructurePoolElement::Kind::Empty) {
            break;
          }
          for (Rotation targetRot : shuffledRotations(random)) {
            if (placedThisJigsaw)
              break;
            const auto targetJigsaws = elementShuffledJigsaws(
                targetEl, *templates, Pos{0, 0, 0}, targetRot, random);
            const BoundingBox hackBox =
                elementBoundingBox(targetEl, *templates, Pos{0, 0, 0}, targetRot);
            int expandTo = 0;
            if (def.useExpansionHack && hackBox.ySpan() <= 16) {
              for (const auto &tj : targetJigsaws) {
                Direction td = Direction::North, tt = Direction::Up;
                jigsawOrientation(tj.info.state, td, tt);
                const Pos adj = tj.info.pos.relative(td);
                if (!hackBox.isInside(adj.x, adj.y, adj.z))
                  continue;
                const std::string cPoolId =
                    resolvePoolAlias(def, tj.pool, aliases);
                if (const auto *cp = regs.findTemplatePool(cPoolId)) {
                  expandTo = std::max(expandTo, elementMaxYSpan(*cp, *templates));
                  if (const auto *cfb = regs.findTemplatePool(cp->fallback)) {
                    expandTo =
                        std::max(expandTo, elementMaxYSpan(*cfb, *templates));
                  }
                }
              }
            }

            for (const auto &targetJig : targetJigsaws) {
              if (!jigsawCanAttach(srcJigsaw, targetJig))
                continue;
              const Pos targetJigLocal = targetJig.info.pos;
              const Pos rawBoxPos{targetJigPos.x - targetJigLocal.x,
                                  targetJigPos.y - targetJigLocal.y,
                                  targetJigPos.z - targetJigLocal.z};
              const BoundingBox rawBB =
                  elementBoundingBox(targetEl, *templates, rawBoxPos, targetRot);
              const bool targetRigid = targetEl.projectionRigid;
              const Pos step = Pos{0, 0, 0}.relative(srcDir);
              const int deltaY =
                  srcJigLocalY - targetJigLocal.y + step.y;
              int targetBoxY = 0;
              if (srcRigid && targetRigid) {
                targetBoxY = srcBoxY + deltaY;
              } else {
                if (cachedBaseHeight == std::numeric_limits<int>::min()) {
                  cachedBaseHeight = gen.getBaseHeight(
                      srcJigPos.x, srcJigPos.z, HeightmapType::WorldSurfaceWG,
                      worldSeed);
                }
                targetBoxY = cachedBaseHeight - targetJigLocal.y;
              }

              const int yOff = targetBoxY - rawBB.minY;
              BoundingBox targetBB = rawBB.moved(0, yOff, 0);
              const Pos targetBoxPos{rawBoxPos.x, rawBoxPos.y + yOff,
                                     rawBoxPos.z};
              if (expandTo > 0) {
                const int newSize =
                    std::max(expandTo + 1, targetBB.maxY - targetBB.minY);
                targetBB.encapsulate(
                    Pos{targetBB.minX, targetBB.minY + newSize, targetBB.minZ});
              }

              if (!childrenFree->fitsWithoutCollision(targetBB))
                continue;
              childrenFree->carve(targetBB);

              const int srcGDelta =
                  result.pieces[srcState.pieceIdx].groundLevelDelta;
              const int targetGDelta =
                  targetRigid ? (srcGDelta - deltaY) : 1;
              const std::size_t newIdx = result.pieces.size();
              result.pieces.push_back(
                  makePiece(targetEl, targetBoxPos, targetGDelta, targetRot,
                            targetBB));

              int junctionY = 0;
              if (srcRigid) {
                junctionY = srcBoxY + srcJigLocalY;
              } else if (targetRigid) {
                junctionY = targetBoxY + targetJigLocal.y;
              } else {
                if (cachedBaseHeight == std::numeric_limits<int>::min()) {
                  cachedBaseHeight = gen.getBaseHeight(
                      srcJigPos.x, srcJigPos.z, HeightmapType::WorldSurfaceWG,
                      worldSeed);
                }
                junctionY = cachedBaseHeight + deltaY / 2;
              }

              result.pieces[srcState.pieceIdx].junctions.push_back(
                  JigsawJunction{targetJigPos.x,
                                 junctionY - srcJigLocalY + srcGDelta,
                                 targetJigPos.z, deltaY, targetRigid});
              result.pieces[newIdx].junctions.push_back(
                  JigsawJunction{srcJigPos.x,
                                 junctionY - targetJigLocal.y + targetGDelta,
                                 srcJigPos.z, -deltaY, srcRigid});

              if (srcState.depth + 1 <= def.size) {
                placing[srcJigsaw.placementPriority].push_back(
                    PieceState{newIdx, targetEl, childrenFree,
                               srcState.depth + 1});
              }
              placedThisJigsaw = true;
              break;
            }
          }
        }
      }
    };

    tryChildren(PieceState{0, centerElement, outerFree, 0});
    while (!placing.empty()) {
      auto topIt = placing.begin();
      auto state = std::move(topIt->second.front());
      topIt->second.pop_front();
      if (topIt->second.empty())
        placing.erase(topIt);
      tryChildren(state);
    }
  }

  return result;
}

std::optional<StructureStart> generateStructureStart(
    const StructureDefinition &def, const NoiseBasedChunkGenerator &gen,
    RandomState &rs, const ChunkGeneratorStructureState &structState,
    std::int64_t worldSeed, ChunkTicketPos chunkPos, int minY, int height) {
  const auto &regs = gen.registries();
  const auto *templates = regs.templateCache();
  if (def.type == StructureType::Jigsaw) {
    return generateJigsawStructureStart(def, gen, rs, structState, worldSeed,
                                        chunkPos, minY, height);
  }

  LegacyRandomSource random(0);
  worldgen_random::setLargeFeatureSeed(random, worldSeed, chunkPos.x,
                                       chunkPos.z);

  auto checkBiomeAt = [&](int bx, int by, int bz) -> bool {
    const auto biome =
        gen.biomeSource().getNoiseBiome(bx >> 2, by >> 2, bz >> 2, rs.sampler());
    for (const auto &sel : def.biomeSelectors) {
      if (regs.biomeMatches(biome, sel))
        return true;
    }
    return def.biomeSelectors.empty();
  };

  if (def.type == StructureType::BuriedTreasure) {
    const int bx = (chunkPos.x << 4) + 8;
    const int bz = (chunkPos.z << 4) + 8;
    const int by =
        gen.getBaseHeight(bx, bz, HeightmapType::OceanFloorWG, worldSeed) - 1;
    if (!checkBiomeAt(bx, by, bz))
      return std::nullopt;
    StructureStart start;
    start.structureId = def.id;
    start.chunkPos = chunkPos;
    start.terrainAdjustment = def.terrainAdjustment;
    StructurePiece p;
    p.kind = StructurePiece::Kind::SimpleBoxFill;
    const Pos chestPos{(chunkPos.x << 4) + 9, by, (chunkPos.z << 4) + 9};
    p.boundingBox = BoundingBox{chestPos.x, chestPos.y, chestPos.z,
                                chestPos.x, chestPos.y, chestPos.z};
    p.position = chestPos;
    p.fillBlock = Blocks::CHEST.defaultState();
    start.pieces.push_back(std::move(p));
    return start;
  }

  if (def.type == StructureType::Igloo && templates) {
    const int bx = (chunkPos.x << 4) + 8;
    const int bz = (chunkPos.z << 4) + 8;
    const int by =
        gen.getBaseHeight(bx, bz, HeightmapType::WorldSurfaceWG, worldSeed) - 1;
    if (!checkBiomeAt(bx, by, bz))
      return std::nullopt;
    const Rotation rot = static_cast<Rotation>(random.nextInt(4));
    const Pos anchor{chunkPos.x << 4, 90, chunkPos.z << 4};
    StructureStart start;
    start.structureId = def.id;
    start.chunkPos = chunkPos;
    start.terrainAdjustment = def.terrainAdjustment;

    auto addIglooPiece = [&](std::string_view tid, Pos pivot, Pos pos) {
      if (const auto *t = templates->getOrLoad(tid)) {
        StructurePiece p;
        p.kind = StructurePiece::Kind::TemplatePlacement;
        p.templateId = std::string(tid);
        p.rotation = rot;
        p.pivot = pivot;
        p.position = pos;
        p.liquidSettings = LiquidSettings::IgnoreWaterlogging;
        p.boundingBox = t->boundingBox(pos, rot, pivot, Mirror::None);
        start.pieces.push_back(std::move(p));
      }
    };
    if (random.nextDouble() < 0.5) {
      const int depth = random.nextInt(8) + 4;
      addIglooPiece("minecraft:igloo/bottom", Pos{3, 6, 7},
                    Pos{anchor.x, anchor.y - 3 - depth * 3, anchor.z - 2});
      for (int i = 0; i < depth - 1; ++i) {
        addIglooPiece("minecraft:igloo/middle", Pos{1, 3, 1},
                      Pos{anchor.x + 2, anchor.y - 3 - i * 3, anchor.z + 4});
      }
    }
    addIglooPiece("minecraft:igloo/top", Pos{3, 5, 5}, anchor);
    if (!start.pieces.empty())
      return start;
  }

  if (def.type == StructureType::NetherFossil && templates) {
    const int bx = (chunkPos.x << 4) + random.nextInt(16);
    const int bz = (chunkPos.z << 4) + random.nextInt(16);
    const WorldGenerationContext wgCtx{minY, height};
    const int y = def.fossilHeight.sample(random, wgCtx);
    if (y <= gen.settings().seaLevel || !checkBiomeAt(bx, y, bz))
      return std::nullopt;
    const int idx = random.nextInt(14) + 1;
    const std::string tid = "minecraft:nether_fossils/fossil_" + std::to_string(idx);
    const Rotation rot = static_cast<Rotation>(random.nextInt(4));
    if (const auto *t = templates->getOrLoad(tid)) {
      StructureStart start;
      start.structureId = def.id;
      start.chunkPos = chunkPos;
      start.terrainAdjustment = def.terrainAdjustment;
      StructurePiece p;
      p.kind = StructurePiece::Kind::TemplatePlacement;
      p.templateId = tid;
      p.rotation = rot;
      p.position = Pos{bx, y, bz};
      p.boundingBox = t->boundingBox(p.position, rot, Pos{0, 0, 0}, Mirror::None);
      start.pieces.push_back(std::move(p));
      return start;
    }
  }

  // TODO(G-3): 其余独立程序/半程序结构类型（Mineshaft、Stronghold、Fortress、
  // OceanMonument、WoodlandMansion、EndCity、RuinedPortal、Shipwreck、OceanRuin、
  // DesertPyramid、JungleTemple、SwampHut）当前尚未逐子类移植完整随机子件构造，
  // 待后续增量补全，此处不伪造假方块返回 std::nullopt。
  return std::nullopt;
}

namespace {

bool placeTemplateCore(std::string_view templateId,
                       std::string_view processorListId, Pos position,
                       Rotation rotation, Mirror mirror, Pos pivot,
                       const BoundingBox &clipBounds,
                       LiquidSettings liquidSettings, bool overwriteAirOnly,
                       bool rigidProjection,
                       const StructureTemplateCache &templates,
                       const WorldgenRegistries &registries,
                       WorldGenRegion &region, RandomSource &rng,
                       bool useRngForBlockRot, int flags) {
  const auto *tmpl = templates.getOrLoad(templateId);
  if (!tmpl || tmpl->palettes.empty())
    return false;

  std::size_t palIdx = 0;
  if (tmpl->palettes.size() > 1) {
    palIdx = static_cast<std::size_t>(
        rng.nextInt(static_cast<std::int32_t>(tmpl->palettes.size())));
  }

  const auto &srcBlocks = tmpl->palettes[palIdx].blocks;
  std::vector<StructureBlockInfo> originals;
  std::vector<StructureBlockInfo> processed;
  originals.reserve(srcBlocks.size());
  processed.reserve(srcBlocks.size());

  for (const auto &b : srcBlocks) {
    if (b.state.is(Blocks::STRUCTURE_BLOCK))
      continue;
    const Pos tp =
        StructureTemplate::transformPos(b.pos, mirror, rotation, pivot);
    const Pos worldPos{tp.x + position.x, tp.y + position.y, tp.z + position.z};
    if (!clipBounds.isInside(worldPos.x, worldPos.y, worldPos.z))
      continue;

    StructureBlockInfo orig = b;
    orig.state = transformBlockState(orig.state, mirror, rotation);
    originals.push_back(orig);

    StructureBlockInfo proc = orig;
    proc.pos = worldPos;
    if (proc.state.is(Blocks::JIGSAW)) {
      std::string finalName = "minecraft:air";
      if (proc.nbt) {
        if (const auto *fs = proc.nbt->get("final_state"))
          finalName = fs->text;
      }
      proc.state = parseBlockStateString(normId(finalName));
    }
    processed.push_back(std::move(proc));
  }

  std::vector<bool> removed(processed.size(), false);
  if (overwriteAirOnly) {
    for (std::size_t i = 0; i < processed.size(); ++i) {
      if (processed[i].state.is(Blocks::STRUCTURE_VOID) ||
          processed[i].state.isAir()) {
        removed[i] = true;
      }
    }
  } else {
    for (std::size_t i = 0; i < processed.size(); ++i) {
      if (processed[i].state.is(Blocks::STRUCTURE_VOID))
        removed[i] = true;
    }
  }

  if (!rigidProjection) {
    for (std::size_t i = 0; i < processed.size(); ++i) {
      if (removed[i])
        continue;
      const int h = region.getHeight(HeightmapType::WorldSurfaceWG,
                                     processed[i].pos.x, processed[i].pos.z);
      // `ProtoChunk::getHeight` 已是 `firstAvailable - 1`，等价于 Java `getHeight(WORLD_SURFACE_WG) - 1`。
      processed[i].pos.y = h + originals[i].pos.y;
    }
  }

  if (!processorListId.empty() && processorListId != "minecraft:empty") {
    if (const auto *pl = registries.findProcessorList(processorListId)) {
      for (const auto &proc : pl->processors) {
        applyProcessorRulePass(proc, region, position, position, processed,
                               originals, removed, rng, useRngForBlockRot);
      }
    }
  }

  bool anyPlaced = false;
  for (std::size_t i = 0; i < processed.size(); ++i) {
    if (removed[i])
      continue;
    const auto &info = processed[i];
    if (!clipBounds.isInside(info.pos.x, info.pos.y, info.pos.z))
      continue;
    BlockState targetState = info.state;
    if (liquidSettings == LiquidSettings::ApplyWaterlogging &&
        targetState.hasProperty("waterlogged")) {
      const FluidState prevFluid =
          region.getFluidState(info.pos.x, info.pos.y, info.pos.z);
      if (prevFluid.isSourceOfType(Fluids::water())) {
        targetState = targetState.with("waterlogged", "true");
      }
    }
    if (region.setBlockState(info.pos.x, info.pos.y, info.pos.z, targetState,
                             flags)) {
      anyPlaced = true;
    }
  }
  return anyPlaced;
}

} // namespace

void placeStructurePieceBlocks(const StructurePiece &piece,
                               const StructureTemplateCache &templates,
                               const WorldgenRegistries &registries,
                               WorldGenRegion &region,
                               const BoundingBox &chunkBounds,
                               RandomSource &rng) {
  if (!piece.boundingBox.intersects(chunkBounds))
    return;

  if (piece.kind == StructurePiece::Kind::SimpleBoxFill) {
    if (chunkBounds.isInside(piece.position.x, piece.position.y,
                           piece.position.z)) {
      region.setBlockState(piece.position.x, piece.position.y, piece.position.z,
                           piece.fillBlock, 2);
    }
    return;
  }

  (void)placeTemplateCore(
      piece.templateId, piece.processorListId, piece.position, piece.rotation,
      piece.mirror, piece.pivot, chunkBounds, piece.liquidSettings,
      piece.overwriteAirOnly, piece.rigidProjection, templates, registries,
      region, rng, false, 18);
}

bool placeStructureTemplateWithProcessors(
    std::string_view templateId, std::string_view processorListId,
    Pos position, Rotation rotation, Mirror mirror, Pos pivot,
    const BoundingBox &clipBounds, LiquidSettings liquidSettings,
    const StructureTemplateCache &templates,
    const WorldgenRegistries &registries, WorldGenRegion &region,
    RandomSource &rng, bool useRngForBlockRot, int flags) {
  return placeTemplateCore(templateId, processorListId, position, rotation,
                           mirror, pivot, clipBounds, liquidSettings, false,
                           true, templates, registries, region, rng,
                           useRngForBlockRot, flags);
}

} // namespace redstone::worldgen
