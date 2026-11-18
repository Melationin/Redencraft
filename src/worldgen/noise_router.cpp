#include "redstone/worldgen/noise_router.hpp"
#include "redstone/worldgen/feature.hpp"
#include "redstone/worldgen/jigsaw.hpp"
#include "redstone/worldgen/structure.hpp"
#include "redstone/worldgen/surface_rules.hpp"

#include "picojson.h"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace redstone::worldgen {
namespace {

std::string normalizeId(std::string_view raw) {
  if (raw.find(':') == std::string_view::npos)
    return "minecraft:" + std::string(raw);
  return std::string(raw);
}

template <class Map, class Fn>
auto lookupWithNormalizedView(const Map &map, std::string_view raw, Fn &&fn) {
  if (raw.find(':') != std::string_view::npos)
    return fn(map, raw);
  constexpr std::string_view kPrefix = "minecraft:";
  if (kPrefix.size() + raw.size() <= 128) {
    char buf[128];
    std::memcpy(buf, kPrefix.data(), kPrefix.size());
    if (!raw.empty())
      std::memcpy(buf + kPrefix.size(), raw.data(), raw.size());
    return fn(map, std::string_view(buf, kPrefix.size() + raw.size()));
  }
  std::string norm;
  norm.reserve(kPrefix.size() + raw.size());
  norm.append(kPrefix);
  norm.append(raw);
  return fn(map, std::string_view(norm));
}

template <class Ptr>
Ptr findPtrInHash(
    const FlatHashMap<std::string_view, Ptr, TransparentStringHash,
                      std::equal_to<>> &idx,
    std::string_view id) {
  return lookupWithNormalizedView(idx, id,
                                  [](const auto &m, std::string_view key) -> Ptr {
                                    auto it = m.find(key);
                                    return it == m.end() ? nullptr : it->second;
                                  });
}

std::string readFileText(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("Failed to open worldgen JSON: " + path.string());
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

picojson::value parseJsonFile(const std::filesystem::path &path) {
  const std::string text = readFileText(path);
  picojson::value val;
  const std::string err = picojson::parse(val, text);
  if (!err.empty()) {
    throw std::runtime_error("JSON parse error in " + path.string() + ": " +
                             err);
  }
  return val;
}

const picojson::value &requireField(const picojson::object &obj,
                                    const char *key, std::string_view ctx) {
  auto it = obj.find(key);
  if (it == obj.end())
    throw std::runtime_error("Missing field '" + std::string(key) + "' in " +
                             std::string(ctx));
  return it->second;
}

double asNumber(const picojson::value &v, std::string_view ctx) {
  if (!v.is<double>())
    throw std::runtime_error("Expected number in " + std::string(ctx));
  return v.get<double>();
}

int asInt(const picojson::value &v, std::string_view ctx) {
  return static_cast<int>(asNumber(v, ctx));
}

bool asBool(const picojson::value &v, std::string_view ctx) {
  if (!v.is<bool>())
    throw std::runtime_error("Expected bool in " + std::string(ctx));
  return v.get<bool>();
}

const std::string &asString(const picojson::value &v, std::string_view ctx) {
  if (!v.is<std::string>())
    throw std::runtime_error("Expected string in " + std::string(ctx));
  return v.get<std::string>();
}

const picojson::object &asObject(const picojson::value &v,
                                 std::string_view ctx) {
  if (!v.is<picojson::object>())
    throw std::runtime_error("Expected object in " + std::string(ctx));
  return v.get<picojson::object>();
}

NoiseParameters parseNoiseParameters(const picojson::value &v,
                                     std::string_view ctx) {
  const auto &obj = asObject(v, ctx);
  NoiseParameters params;
  params.firstOctave = asInt(requireField(obj, "firstOctave", ctx), ctx);
  const auto &ampsVal = requireField(obj, "amplitudes", ctx);
  if (!ampsVal.is<picojson::array>())
    throw std::runtime_error("Expected amplitudes array in " +
                             std::string(ctx));
  for (const auto &el : ampsVal.get<picojson::array>())
    params.amplitudes.push_back(asNumber(el, ctx));
  return params;
}

ClimateParameter parseClimateParameter(const picojson::value &v,
                                       std::string_view ctx) {
  if (v.is<double>()) {
    return ClimateParameter::point(static_cast<float>(v.get<double>()));
  }
  if (v.is<picojson::array>()) {
    const auto &arr = v.get<picojson::array>();
    if (arr.size() != 2)
      throw std::runtime_error("Expected 2-element climate interval in " +
                               std::string(ctx));
    return ClimateParameter::span(
        static_cast<float>(asNumber(arr[0], ctx)),
        static_cast<float>(asNumber(arr[1], ctx)));
  }
  const auto &obj = asObject(v, ctx);
  return ClimateParameter::span(
      static_cast<float>(asNumber(requireField(obj, "min", ctx), ctx)),
      static_cast<float>(asNumber(requireField(obj, "max", ctx), ctx)));
}

ClimateParameterPoint parseClimateParameterPoint(const picojson::value &v,
                                                 std::string_view ctx) {
  const auto &obj = asObject(v, ctx);
  return ClimateParameterPoint::make(
      parseClimateParameter(requireField(obj, "temperature", ctx), ctx),
      parseClimateParameter(requireField(obj, "humidity", ctx), ctx),
      parseClimateParameter(requireField(obj, "continentalness", ctx), ctx),
      parseClimateParameter(requireField(obj, "erosion", ctx), ctx),
      parseClimateParameter(requireField(obj, "depth", ctx), ctx),
      parseClimateParameter(requireField(obj, "weirdness", ctx), ctx),
      static_cast<float>(asNumber(requireField(obj, "offset", ctx), ctx)));
}

BlockState parseBlockStateJsonImpl(const picojson::value &v, std::string_view ctx) {
  const auto &obj = asObject(v, ctx);
  const std::string name =
      normalizeId(asString(requireField(obj, "Name", ctx), ctx));
  BlockState st(name);
  if (auto it = obj.find("Properties");
      it != obj.end() && it->second.is<picojson::object>()) {
    for (const auto &[k, val] : it->second.get<picojson::object>()) {
      if (k == "falling" && st.block().isLiquid() && !st.hasProperty(k))
        continue;
      st = st.with(k, asString(val, ctx));
    }
  }
  return st;
}

VerticalAnchor parseVerticalAnchor(const picojson::value &v,
                                   std::string_view ctx) {
  const auto &obj = asObject(v, ctx);
  if (auto it = obj.find("absolute"); it != obj.end())
    return VerticalAnchor::absolute(asInt(it->second, ctx));
  if (auto it = obj.find("above_bottom"); it != obj.end())
    return VerticalAnchor::aboveBottom(asInt(it->second, ctx));
  if (auto it = obj.find("below_top"); it != obj.end())
    return VerticalAnchor::belowTop(asInt(it->second, ctx));
  throw std::runtime_error("Unknown VerticalAnchor in " + std::string(ctx));
}

SurfaceConditionSourcePtr parseSurfaceCondition(const picojson::value &v,
                                                std::string_view ctx) {
  const auto &obj = asObject(v, ctx);
  const std::string type =
      normalizeId(asString(requireField(obj, "type", ctx), ctx));
  if (type == "minecraft:biome") {
    const auto &arrVal = requireField(obj, "biome_is", ctx);
    if (!arrVal.is<picojson::array>())
      throw std::runtime_error("Expected biome_is array in " + std::string(ctx));
    std::vector<std::string> biomes;
    for (const auto &el : arrVal.get<picojson::array>())
      biomes.push_back(normalizeId(asString(el, ctx)));
    return surface_rules::biome(std::move(biomes));
  }
  if (type == "minecraft:noise_threshold") {
    return surface_rules::noiseThreshold(
        normalizeId(asString(requireField(obj, "noise", ctx), ctx)),
        asNumber(requireField(obj, "min_threshold", ctx), ctx),
        asNumber(requireField(obj, "max_threshold", ctx), ctx));
  }
  if (type == "minecraft:vertical_gradient") {
    return surface_rules::verticalGradient(
        normalizeId(asString(requireField(obj, "random_name", ctx), ctx)),
        parseVerticalAnchor(requireField(obj, "true_at_and_below", ctx), ctx),
        parseVerticalAnchor(requireField(obj, "false_at_and_above", ctx), ctx));
  }
  if (type == "minecraft:y_above") {
    return surface_rules::yAbove(
        parseVerticalAnchor(requireField(obj, "anchor", ctx), ctx),
        asInt(requireField(obj, "surface_depth_multiplier", ctx), ctx),
        asBool(requireField(obj, "add_stone_depth", ctx), ctx));
  }
  if (type == "minecraft:water") {
    return surface_rules::water(
        asInt(requireField(obj, "offset", ctx), ctx),
        asInt(requireField(obj, "surface_depth_multiplier", ctx), ctx),
        asBool(requireField(obj, "add_stone_depth", ctx), ctx));
  }
  if (type == "minecraft:temperature")
    return surface_rules::temperature();
  if (type == "minecraft:steep")
    return surface_rules::steep();
  if (type == "minecraft:hole")
    return surface_rules::hole();
  if (type == "minecraft:above_preliminary_surface")
    return surface_rules::abovePreliminarySurface();
  if (type == "minecraft:stone_depth") {
    const std::string surfaceType =
        asString(requireField(obj, "surface_type", ctx), ctx);
    return surface_rules::stoneDepth(
        asInt(requireField(obj, "offset", ctx), ctx),
        asBool(requireField(obj, "add_surface_depth", ctx), ctx),
        asInt(requireField(obj, "secondary_depth_range", ctx), ctx),
        surfaceType == "ceiling");
  }
  if (type == "minecraft:not") {
    return surface_rules::invert(
        parseSurfaceCondition(requireField(obj, "invert", ctx), ctx));
  }
  throw std::runtime_error("Unsupported surface condition type '" + type +
                           "' in " + std::string(ctx));
}

SurfaceRuleSourcePtr parseSurfaceRule(const picojson::value &v,
                                      std::string_view ctx) {
  const auto &obj = asObject(v, ctx);
  const std::string type =
      normalizeId(asString(requireField(obj, "type", ctx), ctx));
  if (type == "minecraft:bandlands")
    return surface_rules::bandlands();
  if (type == "minecraft:block")
    return surface_rules::block(
        parseBlockStateJsonImpl(requireField(obj, "result_state", ctx), ctx));
  if (type == "minecraft:sequence") {
    const auto &arrVal = requireField(obj, "sequence", ctx);
    if (!arrVal.is<picojson::array>())
      throw std::runtime_error("Expected sequence array in " + std::string(ctx));
    std::vector<SurfaceRuleSourcePtr> rules;
    for (const auto &el : arrVal.get<picojson::array>())
      rules.push_back(parseSurfaceRule(el, ctx));
    return surface_rules::sequence(std::move(rules));
  }
  if (type == "minecraft:condition") {
    return surface_rules::condition(
        parseSurfaceCondition(requireField(obj, "if_true", ctx), ctx),
        parseSurfaceRule(requireField(obj, "then_run", ctx), ctx));
  }
  throw std::runtime_error("Unsupported surface rule type '" + type + "' in " +
                           std::string(ctx));
}

BiomeClimateProperties parseBiomeClimate(const picojson::value &v,
                                         std::string_view ctx) {
  const auto &obj = asObject(v, ctx);
  BiomeClimateProperties props;
  if (auto it = obj.find("has_precipitation"); it != obj.end())
    props.hasPrecipitation = asBool(it->second, ctx);
  if (auto it = obj.find("temperature"); it != obj.end())
    props.temperature = static_cast<float>(asNumber(it->second, ctx));
  if (auto it = obj.find("downfall"); it != obj.end())
    props.downfall = static_cast<float>(asNumber(it->second, ctx));
  if (auto it = obj.find("temperature_modifier"); it != obj.end()) {
    const std::string &mod = asString(it->second, ctx);
    if (mod == "frozen")
      props.temperatureModifier = BiomeTemperatureModifier::Frozen;
    else
      props.temperatureModifier = BiomeTemperatureModifier::None;
  }
  return props;
}

class DensityParser {
public:
  DensityParser(
      const std::map<std::string, picojson::value, std::less<>> &rawDensityJson,
      std::map<std::string, DensityNodePtr, std::less<>> &parsedDensity)
      : rawDensityJson_(rawDensityJson), parsedDensity_(parsedDensity) {}

  DensityNodePtr resolveRef(std::string_view rawId) {
    const std::string id = normalizeId(rawId);
    if (auto it = parsedDensity_.find(id); it != parsedDensity_.end())
      return it->second;
    auto rawIt = rawDensityJson_.find(id);
    if (rawIt == rawDensityJson_.end())
      throw std::runtime_error("Unknown density_function reference: " + id);
    if (!inStack_.emplace(id, true).second)
      throw std::runtime_error("Cycle in density_function reference: " + id);
    DensityNodePtr node = parse(rawIt->second, id);
    inStack_.erase(id);
    parsedDensity_.emplace(id, node);
    return node;
  }

  DensityNodePtr parse(const picojson::value &v, const std::string &ctx) {
    if (v.is<double>())
      return density_functions::constant(v.get<double>());
    if (v.is<std::string>())
      return resolveRef(v.get<std::string>());
    const auto &obj = asObject(v, ctx);
    const std::string type =
        normalizeId(asString(requireField(obj, "type", ctx), ctx));

    if (type == "minecraft:constant") {
      return density_functions::constant(
          asNumber(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:abs") {
      return density_functions::mapped(
          MappedOp::Abs, parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:square") {
      return density_functions::mapped(
          MappedOp::Square, parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:cube") {
      return density_functions::mapped(
          MappedOp::Cube, parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:half_negative") {
      return density_functions::mapped(
          MappedOp::HalfNegative,
          parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:quarter_negative") {
      return density_functions::mapped(
          MappedOp::QuarterNegative,
          parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:invert") {
      return density_functions::mapped(
          MappedOp::Invert, parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:squeeze") {
      return density_functions::mapped(
          MappedOp::Squeeze, parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:clamp") {
      return density_functions::clamp(
          parse(requireField(obj, "input", ctx), ctx),
          asNumber(requireField(obj, "min", ctx), ctx),
          asNumber(requireField(obj, "max", ctx), ctx));
    }
    if (type == "minecraft:add") {
      return density_functions::twoArg(
          TwoArgOp::Add, parse(requireField(obj, "argument1", ctx), ctx),
          parse(requireField(obj, "argument2", ctx), ctx));
    }
    if (type == "minecraft:mul") {
      return density_functions::twoArg(
          TwoArgOp::Mul, parse(requireField(obj, "argument1", ctx), ctx),
          parse(requireField(obj, "argument2", ctx), ctx));
    }
    if (type == "minecraft:min") {
      return density_functions::twoArg(
          TwoArgOp::Min, parse(requireField(obj, "argument1", ctx), ctx),
          parse(requireField(obj, "argument2", ctx), ctx));
    }
    if (type == "minecraft:max") {
      return density_functions::twoArg(
          TwoArgOp::Max, parse(requireField(obj, "argument1", ctx), ctx),
          parse(requireField(obj, "argument2", ctx), ctx));
    }
    if (type == "minecraft:y_clamped_gradient") {
      return density_functions::yClampedGradient(
          asInt(requireField(obj, "from_y", ctx), ctx),
          asInt(requireField(obj, "to_y", ctx), ctx),
          asNumber(requireField(obj, "from_value", ctx), ctx),
          asNumber(requireField(obj, "to_value", ctx), ctx));
    }
    if (type == "minecraft:range_choice") {
      return density_functions::rangeChoice(
          parse(requireField(obj, "input", ctx), ctx),
          asNumber(requireField(obj, "min_inclusive", ctx), ctx),
          asNumber(requireField(obj, "max_exclusive", ctx), ctx),
          parse(requireField(obj, "when_in_range", ctx), ctx),
          parse(requireField(obj, "when_out_of_range", ctx), ctx));
    }
    if (type == "minecraft:noise") {
      return density_functions::noise(
          normalizeId(asString(requireField(obj, "noise", ctx), ctx)),
          asNumber(requireField(obj, "xz_scale", ctx), ctx),
          asNumber(requireField(obj, "y_scale", ctx), ctx));
    }
    if (type == "minecraft:shifted_noise") {
      return density_functions::shiftedNoise(
          parse(requireField(obj, "shift_x", ctx), ctx),
          parse(requireField(obj, "shift_y", ctx), ctx),
          parse(requireField(obj, "shift_z", ctx), ctx),
          asNumber(requireField(obj, "xz_scale", ctx), ctx),
          asNumber(requireField(obj, "y_scale", ctx), ctx),
          normalizeId(asString(requireField(obj, "noise", ctx), ctx)));
    }
    if (type == "minecraft:shift_a") {
      return density_functions::shiftA(
          normalizeId(asString(requireField(obj, "argument", ctx), ctx)));
    }
    if (type == "minecraft:shift_b") {
      return density_functions::shiftB(
          normalizeId(asString(requireField(obj, "argument", ctx), ctx)));
    }
    if (type == "minecraft:shift") {
      return density_functions::shift(
          normalizeId(asString(requireField(obj, "argument", ctx), ctx)));
    }
    if (type == "minecraft:weird_scaled_sampler") {
      const std::string &mapperName =
          asString(requireField(obj, "rarity_value_mapper", ctx), ctx);
      RarityMapperKind kind = RarityMapperKind::Type1;
      if (mapperName == "type_1")
        kind = RarityMapperKind::Type1;
      else if (mapperName == "type_2")
        kind = RarityMapperKind::Type2;
      else
        throw std::runtime_error("Unknown rarity_value_mapper: " + mapperName);
      return density_functions::weirdScaledSampler(
          parse(requireField(obj, "input", ctx), ctx),
          normalizeId(asString(requireField(obj, "noise", ctx), ctx)), kind);
    }
    if (type == "minecraft:old_blended_noise") {
      return density_functions::oldBlendedNoise(
          asNumber(requireField(obj, "xz_scale", ctx), ctx),
          asNumber(requireField(obj, "y_scale", ctx), ctx),
          asNumber(requireField(obj, "xz_factor", ctx), ctx),
          asNumber(requireField(obj, "y_factor", ctx), ctx),
          asNumber(requireField(obj, "smear_scale_multiplier", ctx), ctx));
    }
    if (type == "minecraft:spline") {
      return density_functions::spline(
          parseSpline(requireField(obj, "spline", ctx), ctx));
    }
    if (type == "minecraft:end_islands") {
      return density_functions::endIslands(0);
    }
    if (type == "minecraft:find_top_surface") {
      return density_functions::findTopSurface(
          parse(requireField(obj, "density", ctx), ctx),
          parse(requireField(obj, "upper_bound", ctx), ctx),
          asInt(requireField(obj, "lower_bound", ctx), ctx),
          asInt(requireField(obj, "cell_height", ctx), ctx));
    }
    if (type == "minecraft:blend_alpha")
      return density_functions::blendAlpha();
    if (type == "minecraft:blend_offset")
      return density_functions::blendOffset();
    if (type == "minecraft:blend_density") {
      return density_functions::blendDensity(
          parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:beardifier")
      return density_functions::beardifier();
    if (type == "minecraft:interpolated") {
      return density_functions::marker(
          MarkerKind::Interpolated,
          parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:flat_cache") {
      return density_functions::marker(
          MarkerKind::FlatCache,
          parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:cache_2d") {
      return density_functions::marker(
          MarkerKind::Cache2D, parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:cache_once") {
      return density_functions::marker(
          MarkerKind::CacheOnce,
          parse(requireField(obj, "argument", ctx), ctx));
    }
    if (type == "minecraft:cache_all_in_cell") {
      return density_functions::marker(
          MarkerKind::CacheAllInCell,
          parse(requireField(obj, "argument", ctx), ctx));
    }
    throw std::runtime_error("Unsupported DensityFunction type '" + type +
                             "' in " + ctx);
  }

  CubicSplinePtr parseSpline(const picojson::value &v, const std::string &ctx) {
    if (v.is<double>())
      return CubicSpline::constant(static_cast<float>(v.get<double>()));
    const auto &obj = asObject(v, ctx);
    DensityNodePtr coord =
        parse(requireField(obj, "coordinate", ctx), ctx + ".coordinate");
    const auto &ptsVal = requireField(obj, "points", ctx);
    if (!ptsVal.is<picojson::array>())
      throw std::runtime_error("Expected points array in spline " + ctx);
    const auto &arr = ptsVal.get<picojson::array>();
    std::vector<float> locations;
    std::vector<CubicSplinePtr> values;
    std::vector<float> derivatives;
    locations.reserve(arr.size());
    values.reserve(arr.size());
    derivatives.reserve(arr.size());
    for (const auto &ptVal : arr) {
      const auto &ptObj = asObject(ptVal, ctx + ".point");
      locations.push_back(static_cast<float>(
          asNumber(requireField(ptObj, "location", ctx), ctx)));
      values.push_back(
          parseSpline(requireField(ptObj, "value", ctx), ctx + ".point.value"));
      derivatives.push_back(static_cast<float>(
          asNumber(requireField(ptObj, "derivative", ctx), ctx)));
    }
    return CubicSpline::multipoint(std::move(coord), std::move(locations),
                                   std::move(values), std::move(derivatives));
  }

private:
  const std::map<std::string, picojson::value, std::less<>> &rawDensityJson_;
  std::map<std::string, DensityNodePtr, std::less<>> &parsedDensity_;
  std::map<std::string, bool, std::less<>> inStack_;
};

std::string jsonStemId(const std::filesystem::path &baseDir,
                       const std::filesystem::path &filePath) {
  std::filesystem::path rel = std::filesystem::relative(filePath, baseDir);
  rel.replace_extension();
  return "minecraft:" + rel.generic_string();
}

} // namespace

BlockState parseBlockStateJson(const picojson::value &v, std::string_view ctx) {
  return parseBlockStateJsonImpl(v, ctx);
}

NoiseRouter NoiseRouter::mapAll(
    const density_functions::NodeTransform &nodeVisitor,
    const std::function<NoiseHolder(const NoiseHolder &)> &noiseVisitor) const {
  auto wrap = [&](const DensityNodePtr &fn) {
    return density_functions::mapAll(fn, nodeVisitor, noiseVisitor);
  };
  return NoiseRouter{
      .barrierNoise = wrap(barrierNoise),
      .fluidLevelFloodednessNoise = wrap(fluidLevelFloodednessNoise),
      .fluidLevelSpreadNoise = wrap(fluidLevelSpreadNoise),
      .lavaNoise = wrap(lavaNoise),
      .temperature = wrap(temperature),
      .vegetation = wrap(vegetation),
      .continents = wrap(continents),
      .erosion = wrap(erosion),
      .depth = wrap(depth),
      .ridges = wrap(ridges),
      .preliminarySurfaceLevel = wrap(preliminarySurfaceLevel),
      .finalDensity = wrap(finalDensity),
      .veinToggle = wrap(veinToggle),
      .veinRidged = wrap(veinRidged),
      .veinGap = wrap(veinGap),
  };
}

void ClimateSampler::computeYDependentMask() noexcept {
  std::uint8_t mask = 0;
  if (density_functions::dependsOnY(temperature.get()))
    mask |= 1u << 0u;
  if (density_functions::dependsOnY(humidity.get()))
    mask |= 1u << 1u;
  if (density_functions::dependsOnY(continentalness.get()))
    mask |= 1u << 2u;
  if (density_functions::dependsOnY(erosion.get()))
    mask |= 1u << 3u;
  if (density_functions::dependsOnY(depth.get()))
    mask |= 1u << 4u;
  if (density_functions::dependsOnY(weirdness.get()))
    mask |= 1u << 5u;
  yDependentMask = mask;
}

ClimateTargetPoint ClimateSampler::sample(int quartX, int quartY,
                                          int quartZ) const {
  const int blockX = quartX * 4;
  const int blockY = quartY * 4;
  const int blockZ = quartZ * 4;
  if (flat) {
    double values[6];
    flat->evaluate(
        DensityFunctionContext{blockX, blockY, blockZ, nullptr, nullptr},
        values);
    return ClimateTargetPoint::make(
        static_cast<float>(values[0]), static_cast<float>(values[1]),
        static_cast<float>(values[2]), static_cast<float>(values[3]),
        static_cast<float>(values[4]), static_cast<float>(values[5]));
  }
  DensityEvalCache cache;
  const DensityFunctionContext ctx{blockX, blockY, blockZ, nullptr, &cache};
  return ClimateTargetPoint::make(
      temperature ? static_cast<float>(temperature->compute(ctx)) : 0.0f,
      humidity ? static_cast<float>(humidity->compute(ctx)) : 0.0f,
      continentalness
          ? static_cast<float>(continentalness->compute(ctx))
          : 0.0f,
      erosion ? static_cast<float>(erosion->compute(ctx)) : 0.0f,
      depth ? static_cast<float>(depth->compute(ctx)) : 0.0f,
      weirdness ? static_cast<float>(weirdness->compute(ctx)) : 0.0f);
}

Pos ClimateSampler::findSpawnPosition() const {
  return ClimateSpawnFinder::findSpawnPosition(spawnTarget, *this);
}

WorldgenRegistries WorldgenRegistries::loadFromDirectory(
    const std::filesystem::path &worldgenDir) {
  WorldgenRegistries reg;

  const auto noiseDir = worldgenDir / "noise";
  if (std::filesystem::exists(noiseDir)) {
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(noiseDir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const std::string id = jsonStemId(noiseDir, entry.path());
      reg.noises_.emplace(id, parseNoiseParameters(parseJsonFile(entry.path()), id));
    }
  }

  std::map<std::string, picojson::value, std::less<>> rawDensity;
  const auto dfDir = worldgenDir / "density_function";
  if (std::filesystem::exists(dfDir)) {
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(dfDir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const std::string id = jsonStemId(dfDir, entry.path());
      rawDensity.emplace(id, parseJsonFile(entry.path()));
    }
  }

  DensityParser parser(rawDensity, reg.densityFunctions_);
  for (const auto &[id, _] : rawDensity)
    (void)parser.resolveRef(id);

  const auto carverDir = worldgenDir / "configured_carver";
  if (std::filesystem::exists(carverDir)) {
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(carverDir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const std::string id = jsonStemId(carverDir, entry.path());
      reg.carvers_.emplace(
          id, parseConfiguredCarverJson(parseJsonFile(entry.path()), id));
    }
  }

  const auto biomeDir = worldgenDir / "biome";
  if (std::filesystem::exists(biomeDir)) {
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(biomeDir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const std::string id = jsonStemId(biomeDir, entry.path());
      const picojson::value root = parseJsonFile(entry.path());
      reg.biomeClimates_.emplace(id, parseBiomeClimate(root, id));

      BiomeGenerationSettings bgi;
      if (root.is<picojson::object>()) {
        const auto &obj = root.get<picojson::object>();
        if (auto it = obj.find("carvers"); it != obj.end()) {
          if (it->second.is<std::string>()) {
            bgi.carvers.push_back(normalizeId(it->second.get<std::string>()));
          } else if (it->second.is<picojson::array>()) {
            for (const auto &el : it->second.get<picojson::array>()) {
              bgi.carvers.push_back(
                  normalizeId(asString(el, id + ".carvers")));
            }
          }
        }
        if (auto it = obj.find("features");
            it != obj.end() && it->second.is<picojson::array>()) {
          const auto &steps = it->second.get<picojson::array>();
          for (std::size_t s = 0;
               s < std::min<std::size_t>(steps.size(), bgi.features.size());
               ++s) {
            if (!steps[s].is<picojson::array>())
              continue;
            for (const auto &f : steps[s].get<picojson::array>()) {
              bgi.features[s].push_back(
                  normalizeId(asString(f, id + ".features")));
            }
          }
        }
      }
      reg.biomeGenerations_.emplace(id, std::move(bgi));
    }
  }

  const auto biomeTagDir = worldgenDir / "tags" / "biome";
  if (std::filesystem::exists(biomeTagDir)) {
    std::map<std::string, std::vector<std::string>, std::less<>> rawTags;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(biomeTagDir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const std::string id = jsonStemId(biomeTagDir, entry.path());
      const picojson::value root = parseJsonFile(entry.path());
      std::vector<std::string> entries;
      if (root.is<picojson::object>()) {
        const auto &obj = root.get<picojson::object>();
        if (auto it = obj.find("values");
            it != obj.end() && it->second.is<picojson::array>()) {
          for (const auto &v : it->second.get<picojson::array>()) {
            if (v.is<std::string>()) {
              entries.push_back(v.get<std::string>());
            } else if (v.is<picojson::object>()) {
              const auto &vo = v.get<picojson::object>();
              if (auto idIt = vo.find("id");
                  idIt != vo.end() && idIt->second.is<std::string>()) {
                entries.push_back(idIt->second.get<std::string>());
              }
            }
          }
        }
      }
      rawTags.emplace(id, std::move(entries));
    }
    auto resolveTag = [&](auto &self, const std::string &tagId,
                          std::vector<std::string> &visiting)
        -> const std::vector<std::string> & {
      if (auto it = reg.biomeTags_.find(tagId); it != reg.biomeTags_.end())
        return it->second;
      std::vector<std::string> resolved;
      if (std::find(visiting.begin(), visiting.end(), tagId) != visiting.end()) {
        return reg.biomeTags_.emplace(tagId, std::move(resolved)).first->second;
      }
      visiting.push_back(tagId);
      if (auto rawIt = rawTags.find(tagId); rawIt != rawTags.end()) {
        for (const std::string &item : rawIt->second) {
          if (!item.empty() && item.front() == '#') {
            const std::string nested = normalizeId(item.substr(1));
            const auto &sub = self(self, nested, visiting);
            resolved.insert(resolved.end(), sub.begin(), sub.end());
          } else {
            resolved.push_back(normalizeId(item));
          }
        }
      }
      visiting.pop_back();
      std::sort(resolved.begin(), resolved.end());
      resolved.erase(std::unique(resolved.begin(), resolved.end()),
                     resolved.end());
      return reg.biomeTags_.emplace(tagId, std::move(resolved)).first->second;
    };
    std::vector<std::string> visiting;
    for (const auto &[tagId, _] : rawTags)
      (void)resolveTag(resolveTag, tagId, visiting);
  }

  const auto nsDir = worldgenDir / "noise_settings";
  if (std::filesystem::exists(nsDir)) {
    for (const auto &entry : std::filesystem::directory_iterator(nsDir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const std::string id = jsonStemId(nsDir, entry.path());
      const picojson::value rootVal = parseJsonFile(entry.path());
      const auto &obj = asObject(rootVal, id);

      NoiseGeneratorSettings s;
      s.id = id;
      s.aquifersEnabled =
          asBool(requireField(obj, "aquifers_enabled", id), id);
      s.oreVeinsEnabled =
          asBool(requireField(obj, "ore_veins_enabled", id), id);
      s.disableMobGeneration =
          asBool(requireField(obj, "disable_mob_generation", id), id);
      s.legacyRandomSource =
          asBool(requireField(obj, "legacy_random_source", id), id);
      s.seaLevel = asInt(requireField(obj, "sea_level", id), id);

      const auto &defBlock = asObject(requireField(obj, "default_block", id), id);
      s.defaultBlockName =
          normalizeId(asString(requireField(defBlock, "Name", id), id));
      s.defaultBlockState = parseBlockStateJson(requireField(obj, "default_block", id), id);
      const auto &defFluid = asObject(requireField(obj, "default_fluid", id), id);
      s.defaultFluidName =
          normalizeId(asString(requireField(defFluid, "Name", id), id));
      s.defaultFluidState = parseBlockStateJson(requireField(obj, "default_fluid", id), id);

      const auto &noiseCfg = asObject(requireField(obj, "noise", id), id);
      s.minY = asInt(requireField(noiseCfg, "min_y", id), id);
      s.height = asInt(requireField(noiseCfg, "height", id), id);
      s.sizeHorizontal =
          asInt(requireField(noiseCfg, "size_horizontal", id), id);
      s.sizeVertical = asInt(requireField(noiseCfg, "size_vertical", id), id);

      const auto &routerObj =
          asObject(requireField(obj, "noise_router", id), id);
      auto field = [&](const char *key) {
        return parser.parse(requireField(routerObj, key, id),
                            id + ".noise_router." + key);
      };
      s.noiseRouter = NoiseRouter{
          .barrierNoise = field("barrier"),
          .fluidLevelFloodednessNoise = field("fluid_level_floodedness"),
          .fluidLevelSpreadNoise = field("fluid_level_spread"),
          .lavaNoise = field("lava"),
          .temperature = field("temperature"),
          .vegetation = field("vegetation"),
          .continents = field("continents"),
          .erosion = field("erosion"),
          .depth = field("depth"),
          .ridges = field("ridges"),
          .preliminarySurfaceLevel = field("preliminary_surface_level"),
          .finalDensity = field("final_density"),
          .veinToggle = field("vein_toggle"),
          .veinRidged = field("vein_ridged"),
          .veinGap = field("vein_gap"),
      };
      if (auto it = obj.find("spawn_target");
          it != obj.end() && it->second.is<picojson::array>()) {
        for (const auto &el : it->second.get<picojson::array>()) {
          s.spawnTarget.push_back(
              parseClimateParameterPoint(el, id + ".spawn_target"));
        }
      }
      if (auto it = obj.find("surface_rule"); it != obj.end()) {
        s.surfaceRule = parseSurfaceRule(it->second, id + ".surface_rule");
      }
      reg.noiseSettings_.emplace(id, std::move(s));
    }
  }

  auto loadJsonDir = [&](const char *sub, auto &&onEntry) {
    const auto dir = worldgenDir / sub;
    if (!std::filesystem::exists(dir))
      return;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator(dir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const std::string id = jsonStemId(dir, entry.path());
      onEntry(id, parseJsonFile(entry.path()));
    }
  };
  loadJsonDir("processor_list", [&](const std::string &id, const picojson::value &v) {
    reg.processorLists_.emplace(id, parseProcessorListJson(v, id));
  });
  loadJsonDir("template_pool", [&](const std::string &id, const picojson::value &v) {
    reg.templatePools_.emplace(id, parseTemplatePoolJson(v, id));
  });
  loadJsonDir("structure", [&](const std::string &id, const picojson::value &v) {
    reg.structures_.emplace(id, parseStructureDefinitionJson(v, id));
  });
  loadJsonDir("structure_set", [&](const std::string &id, const picojson::value &v) {
    reg.structureSets_.emplace(id, parseStructureSetJson(v, id));
  });
  loadJsonDir("configured_feature",
              [&](const std::string &id, const picojson::value &v) {
                reg.configuredFeatures_.emplace(
                    id, parseConfiguredFeatureJson(v, id));
              });
  loadJsonDir("placed_feature",
              [&](const std::string &id, const picojson::value &v) {
                reg.placedFeatures_.emplace(id, parsePlacedFeatureJson(v, id));
              });
  reg.templateCache_ = std::make_shared<StructureTemplateCache>(
      worldgenDir / "structure_nbt");
  reg.rebuildLookupIndices();
  return reg;
}

WorldgenRegistries::WorldgenRegistries(const WorldgenRegistries &other)
    : noises_(other.noises_), densityFunctions_(other.densityFunctions_),
      noiseSettings_(other.noiseSettings_), biomeClimates_(other.biomeClimates_),
      biomeGenerations_(other.biomeGenerations_), carvers_(other.carvers_),
      biomeTags_(other.biomeTags_), structures_(other.structures_),
      structureSets_(other.structureSets_),
      processorLists_(other.processorLists_),
      templatePools_(other.templatePools_),
      templateCache_(other.templateCache_),
      placedFeatures_(other.placedFeatures_),
      configuredFeatures_(other.configuredFeatures_) {
  rebuildLookupIndices();
}

WorldgenRegistries &
WorldgenRegistries::operator=(const WorldgenRegistries &other) {
  if (this == &other)
    return *this;
  noises_ = other.noises_;
  densityFunctions_ = other.densityFunctions_;
  noiseSettings_ = other.noiseSettings_;
  biomeClimates_ = other.biomeClimates_;
  biomeGenerations_ = other.biomeGenerations_;
  carvers_ = other.carvers_;
  biomeTags_ = other.biomeTags_;
  structures_ = other.structures_;
  structureSets_ = other.structureSets_;
  processorLists_ = other.processorLists_;
  templatePools_ = other.templatePools_;
  templateCache_ = other.templateCache_;
  placedFeatures_ = other.placedFeatures_;
  configuredFeatures_ = other.configuredFeatures_;
  rebuildLookupIndices();
  return *this;
}

void WorldgenRegistries::rebuildLookupIndices() {
  auto indexMap = [](const auto &src, auto &dst) {
    dst.clear();
    dst.reserve(src.size());
    for (const auto &[k, v] : src)
      dst[std::string_view(k)] = &v;
  };
  auto indexPtrMap = [](const auto &src, auto &dst) {
    dst.clear();
    dst.reserve(src.size());
    for (const auto &[k, v] : src)
      dst[std::string_view(k)] = v.get();
  };

  indexMap(noises_, noiseIdx_);
  indexMap(densityFunctions_, densityFunctionIdx_);
  indexMap(noiseSettings_, noiseSettingsIdx_);
  indexMap(biomeClimates_, biomeClimateIdx_);
  indexMap(biomeGenerations_, biomeGenerationIdx_);
  indexMap(carvers_, carverIdx_);
  indexMap(biomeTags_, biomeTagIdx_);
  indexMap(structures_, structureIdx_);
  indexMap(structureSets_, structureSetIdx_);
  indexMap(processorLists_, processorListIdx_);
  indexMap(templatePools_, templatePoolIdx_);
  indexPtrMap(placedFeatures_, placedFeatureIdx_);
  indexPtrMap(configuredFeatures_, configuredFeatureIdx_);

  // 预解析所有 BiomeGenerationSettings 中的 carver 指针，消除 applyCarvers 内的字符串查找
  for (auto &[biomeKey, gen] : biomeGenerations_) {
    (void)biomeKey;
    gen.resolvedCarvers.clear();
    gen.resolvedCarvers.reserve(gen.carvers.size());
    for (const std::string &carverId : gen.carvers)
      gen.resolvedCarvers.push_back(findCarver(carverId));
  }

  // 注册稠密整数 BiomeId（0..N-1），按持久化 string 键地址建索引
  biomeNames_.clear();
  biomeIdByName_.clear();
  auto registerBiome = [&](std::string_view name) -> std::uint16_t {
    if (auto it = biomeIdByName_.find(name); it != biomeIdByName_.end())
      return it->second;
    const std::uint16_t id = static_cast<std::uint16_t>(biomeNames_.size());
    biomeNames_.push_back(name);
    biomeIdByName_[name] = id;
    return id;
  };
  for (const auto &[k, _] : biomeClimates_)
    (void)registerBiome(k);
  for (const auto &[k, _] : biomeGenerations_)
    (void)registerBiome(k);
  for (const auto &[tagKey, list] : biomeTags_) {
    (void)tagKey;
    for (const std::string &b : list)
      (void)registerBiome(b);
  }

  biomeClimatesById_.assign(biomeNames_.size(), nullptr);
  biomeGenerationsById_.assign(biomeNames_.size(), nullptr);
  for (std::size_t i = 0; i < biomeNames_.size(); ++i) {
    biomeClimatesById_[i] = findPtrInHash(biomeClimateIdx_, biomeNames_[i]);
    biomeGenerationsById_[i] = findPtrInHash(biomeGenerationIdx_, biomeNames_[i]);
  }

  biomeTagStrideWords_ = (biomeNames_.size() + 63u) / 64u;
  biomeTagMasks_.clear();
  biomeTagMasks_.reserve(biomeTags_.size());
  for (const auto &[tagKey, list] : biomeTags_) {
    std::vector<std::uint64_t> mask(biomeTagStrideWords_, 0ULL);
    for (const std::string &b : list) {
      const std::uint16_t bid = findBiomeId(b);
      if (bid != kInvalidBiomeId)
        mask[static_cast<std::size_t>(bid) >> 6u] |= (1ULL << (bid & 63u));
    }
    biomeTagMasks_[std::string_view(tagKey)] = std::move(mask);
  }
}

const NoiseParameters *
WorldgenRegistries::findNoise(std::string_view id) const {
  return findPtrInHash(noiseIdx_, id);
}

const DensityNodePtr *
WorldgenRegistries::findDensityFunction(std::string_view id) const {
  return findPtrInHash(densityFunctionIdx_, id);
}

const NoiseGeneratorSettings *
WorldgenRegistries::findNoiseSettings(std::string_view id) const {
  return findPtrInHash(noiseSettingsIdx_, id);
}

const BiomeClimateProperties *
WorldgenRegistries::findBiomeClimate(std::string_view id) const {
  return findPtrInHash(biomeClimateIdx_, id);
}

const BiomeGenerationSettings *
WorldgenRegistries::findBiomeGeneration(std::string_view id) const {
  return findPtrInHash(biomeGenerationIdx_, id);
}

const ConfiguredCarver *
WorldgenRegistries::findCarver(std::string_view id) const {
  return findPtrInHash(carverIdx_, id);
}

const std::vector<std::string> *
WorldgenRegistries::findBiomeTag(std::string_view tagId) const {
  if (!tagId.empty() && tagId.front() == '#')
    tagId.remove_prefix(1);
  return findPtrInHash(biomeTagIdx_, tagId);
}

const StructureDefinition *
WorldgenRegistries::findStructure(std::string_view id) const {
  return findPtrInHash(structureIdx_, id);
}

const StructureSetDefinition *
WorldgenRegistries::findStructureSet(std::string_view id) const {
  return findPtrInHash(structureSetIdx_, id);
}

const StructureProcessorList *
WorldgenRegistries::findProcessorList(std::string_view id) const {
  return findPtrInHash(processorListIdx_, id);
}

const StructureTemplatePool *
WorldgenRegistries::findTemplatePool(std::string_view id) const {
  return findPtrInHash(templatePoolIdx_, id);
}

const PlacedFeature *
WorldgenRegistries::findPlacedFeature(std::string_view id) const {
  return findPtrInHash(placedFeatureIdx_, id);
}

const ConfiguredFeature *
WorldgenRegistries::findConfiguredFeature(std::string_view id) const {
  return findPtrInHash(configuredFeatureIdx_, id);
}

std::uint16_t
WorldgenRegistries::findBiomeId(std::string_view id) const noexcept {
  return lookupWithNormalizedView(
      biomeIdByName_, id,
      [](const auto &m, std::string_view key) -> std::uint16_t {
        auto it = m.find(key);
        return it == m.end() ? kInvalidBiomeId : it->second;
      });
}

const std::vector<std::uint64_t> *
WorldgenRegistries::findBiomeTagMask(std::string_view tagId) const {
  if (!tagId.empty() && tagId.front() == '#')
    tagId.remove_prefix(1);
  return lookupWithNormalizedView(
      biomeTagMasks_, tagId,
      [](const auto &m,
         std::string_view key) -> const std::vector<std::uint64_t> * {
        auto it = m.find(key);
        return it == m.end() ? nullptr : &it->second;
      });
}

bool WorldgenRegistries::buildBiomeSelectorMask(
    std::span<const std::string> selectors,
    std::vector<std::uint64_t> &outWords) const {
  outWords.assign(biomeTagStrideWords_, 0ULL);
  bool anyMatched = false;
  for (const std::string &sel : selectors) {
    if (!sel.empty() && sel.front() == '#') {
      if (const auto *mask = findBiomeTagMask(sel)) {
        for (std::size_t w = 0; w < outWords.size() && w < mask->size(); ++w) {
          outWords[w] |= (*mask)[w];
          if ((*mask)[w] != 0ULL)
            anyMatched = true;
        }
      }
    } else {
      const std::uint16_t bid = findBiomeId(sel);
      if (bid != kInvalidBiomeId) {
        const std::size_t w = static_cast<std::size_t>(bid) >> 6u;
        if (w < outWords.size()) {
          outWords[w] |= (1ULL << (bid & 63u));
          anyMatched = true;
        }
      }
    }
  }
  return anyMatched;
}

bool WorldgenRegistries::biomeMatches(
    std::string_view biomeId, std::string_view biomeOrTagSelector) const {
  const std::uint16_t bid = findBiomeId(biomeId);
  if (!biomeOrTagSelector.empty() && biomeOrTagSelector.front() == '#') {
    if (bid == kInvalidBiomeId)
      return false;
    const auto *mask = findBiomeTagMask(biomeOrTagSelector);
    return mask && biomeMatchesMask(bid, *mask);
  }
  if (bid != kInvalidBiomeId) {
    const std::uint16_t targetId = findBiomeId(biomeOrTagSelector);
    if (targetId != kInvalidBiomeId)
      return bid == targetId;
  }
  return lookupWithNormalizedView(
      biomeIdByName_, biomeId,
      [&](const auto &, std::string_view lhsNorm) {
        return lookupWithNormalizedView(
            biomeIdByName_, biomeOrTagSelector,
            [&](const auto &, std::string_view rhsNorm) {
              return lhsNorm == rhsNorm;
            });
      });
}

RandomState::~RandomState() = default;
RandomState::RandomState(RandomState &&) noexcept = default;
RandomState &RandomState::operator=(RandomState &&) noexcept = default;

RandomState RandomState::create(const NoiseGeneratorSettings &settings,
                                const WorldgenRegistries &registries,
                                std::int64_t seed) {
  return RandomState(settings, registries, seed);
}

struct RandomState::CacheMutexes {
  std::mutex surfaceMutex;
  std::mutex noiseMutex;
  std::mutex randomMutex;
};

SurfaceSystem &
RandomState::surfaceSystem(const NoiseGeneratorSettings &settings) {
  std::lock_guard<std::mutex> lock(cacheMutexes_->surfaceMutex);
  if (!surfaceSystem_) {
    const BlockState defBlock =
        settings.defaultBlockState.id() != 0 ||
                settings.defaultBlockName == "minecraft:air"
            ? settings.defaultBlockState
            : Blocks::STONE.defaultState();
    surfaceSystem_ = std::make_unique<SurfaceSystem>(
        *this, defBlock, settings.seaLevel, random_);
  }
  return *surfaceSystem_;
}

RandomState::RandomState(const NoiseGeneratorSettings &settings,
                         const WorldgenRegistries &registries,
                         std::int64_t seed)
    : seed_(seed), registries_(&registries),
      cacheMutexes_(std::make_unique<CacheMutexes>()) {
  if (settings.legacyRandomSource) {
    LegacyRandomSource base(seed);
    random_ = base.forkPositional();
  } else {
    XoroshiroRandomSource base(seed);
    random_ = base.forkPositional();
  }
  aquiferRandom_ = random_.fromHashOf("minecraft:aquifer").forkPositional();
  oreRandom_ = random_.fromHashOf("minecraft:ore").forkPositional();

  const bool useLegacyInit = settings.legacyRandomSource;

  auto wireNoise = [&](const NoiseHolder &nh) -> NoiseHolder {
    if (nh.noiseId == "minecraft:nether/temperature") {
      const NoiseParameters *p = registries_->findNoise(nh.noiseId);
      if (!p)
        throw std::runtime_error("Missing NoiseParameters: " + nh.noiseId);
      LegacyRandomSource lr(seed_);
      return NoiseHolder{nh.noiseId, std::make_shared<NormalNoise>(
                                         NormalNoise::createLegacyNetherBiome(
                                             lr, *p))};
    }
    if (nh.noiseId == "minecraft:nether/vegetation") {
      const NoiseParameters *p = registries_->findNoise(nh.noiseId);
      if (!p)
        throw std::runtime_error("Missing NoiseParameters: " + nh.noiseId);
      LegacyRandomSource lr(seed_ + 1);
      return NoiseHolder{nh.noiseId, std::make_shared<NormalNoise>(
                                         NormalNoise::createLegacyNetherBiome(
                                             lr, *p))};
    }
    return NoiseHolder{nh.noiseId, getOrCreateNoise(nh.noiseId)};
  };

  auto wireNode = [&](const DensityNodePtr &node) -> DensityNodePtr {
    if (!node)
      return nullptr;
    if (node->kind == DensityNodeKind::OldBlendedNoise) {
      AnyRandomSource terrainRng =
          useLegacyInit ? AnyRandomSource::legacy(seed_)
                        : random_.fromHashOf("minecraft:terrain");
      auto bn = std::make_shared<BlendedNoise>(
          terrainRng, node->xzScale, node->yScale, node->xzFactor,
          node->yFactor, node->smearScaleMultiplier);
      return density_functions::oldBlendedNoise(
          node->xzScale, node->yScale, node->xzFactor, node->yFactor,
          node->smearScaleMultiplier, std::move(bn));
    }
    if (node->kind == DensityNodeKind::EndIslands) {
      return density_functions::endIslands(seed_);
    }
    return node;
  };

  router_ = settings.noiseRouter.mapAll(wireNode, wireNoise);

  auto stripMarkers = [](const DensityNodePtr &node) -> DensityNodePtr {
    if (node && node->kind == DensityNodeKind::Marker)
      return node->arg1;
    return node;
  };
  sampler_ = ClimateSampler{
      .temperature = density_functions::mapAll(router_.temperature, stripMarkers),
      .humidity = density_functions::mapAll(router_.vegetation, stripMarkers),
      .continentalness =
          density_functions::mapAll(router_.continents, stripMarkers),
      .erosion = density_functions::mapAll(router_.erosion, stripMarkers),
      .depth = density_functions::mapAll(router_.depth, stripMarkers),
      .weirdness = density_functions::mapAll(router_.ridges, stripMarkers),
      .spawnTarget = settings.spawnTarget,
  };
  sampler_.computeYDependentMask();
  const std::array<DensityNodePtr, 6> climateRoots{
      sampler_.temperature,     sampler_.humidity, sampler_.continentalness,
      sampler_.erosion,         sampler_.depth,    sampler_.weirdness};
  sampler_.flat = FlatDensityFunction::compile(climateRoots);
}

std::shared_ptr<const NormalNoise>
RandomState::getOrCreateNoise(std::string_view noiseId) {
  std::lock_guard<std::mutex> lock(cacheMutexes_->noiseMutex);
  std::shared_ptr<const NormalNoise> hit;
  std::string normKey;
  lookupWithNormalizedView(noiseInstances_, noiseId,
                           [&](const auto &m, std::string_view key) {
                             if (auto it = m.find(key); it != m.end()) {
                               hit = it->second;
                             } else {
                               normKey = std::string(key);
                             }
                             return 0;
                           });
  if (hit)
    return hit;
  const NoiseParameters *params = registries_->findNoise(normKey);
  if (!params)
    throw std::runtime_error("Missing NoiseParameters: " + normKey);
  AnyRandomSource rng = random_.fromHashOf(normKey);
  auto instance =
      std::make_shared<NormalNoise>(NormalNoise::create(rng, *params));
  noiseInstances_[normKey] = instance;
  return instance;
}

PositionalRandomFactory
RandomState::getOrCreateRandomFactory(std::string_view name) {
  std::lock_guard<std::mutex> lock(cacheMutexes_->randomMutex);
  std::optional<PositionalRandomFactory> hit;
  std::string normKey;
  lookupWithNormalizedView(positionalRandoms_, name,
                           [&](const auto &m, std::string_view key) {
                             if (auto it = m.find(key); it != m.end()) {
                               hit = it->second;
                             } else {
                               normKey = std::string(key);
                             }
                             return 0;
                           });
  if (hit)
    return *hit;
  PositionalRandomFactory factory =
      random_.fromHashOf(normKey).forkPositional();
  positionalRandoms_[normKey] = factory;
  return factory;
}

} // namespace redstone::worldgen
