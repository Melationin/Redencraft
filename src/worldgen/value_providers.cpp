#include "redstone/worldgen/value_providers.hpp"

#include "picojson.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace redstone::worldgen {
namespace {

std::string normalizeProviderId(std::string_view id) {
  if (id.find(':') != std::string_view::npos)
    return std::string(id);
  return "minecraft:" + std::string(id);
}

const picojson::object &asObj(const picojson::value &v, std::string_view ctx) {
  if (!v.is<picojson::object>())
    throw std::runtime_error("Expected JSON object in " + std::string(ctx));
  return v.get<picojson::object>();
}

const picojson::value &reqField(const picojson::object &obj, const char *key,
                                std::string_view ctx) {
  auto it = obj.find(key);
  if (it == obj.end())
    throw std::runtime_error("Missing field '" + std::string(key) + "' in " +
                             std::string(ctx));
  return it->second;
}

double asNum(const picojson::value &v, std::string_view ctx) {
  if (!v.is<double>())
    throw std::runtime_error("Expected number in " + std::string(ctx));
  return v.get<double>();
}

int asInteger(const picojson::value &v, std::string_view ctx) {
  return static_cast<int>(asNum(v, ctx));
}

std::string asStr(const picojson::value &v, std::string_view ctx) {
  if (!v.is<std::string>())
    throw std::runtime_error("Expected string in " + std::string(ctx));
  return v.get<std::string>();
}

const std::array<float, 65536> &sinTable() {
  static const std::array<float, 65536> kSin = []() {
    std::array<float, 65536> tbl{};
    for (int i = 0; i < 65536; ++i) {
      tbl[static_cast<std::size_t>(i)] = static_cast<float>(
          std::sin(static_cast<double>(i) / 10430.378350470453));
    }
    return tbl;
  }();
  return kSin;
}

} // namespace

namespace worldgen_math {

float minecraftSin(double radians) noexcept {
  const auto idx = static_cast<std::size_t>(
      static_cast<std::int64_t>(radians * 10430.378350470453) & 65535LL);
  return sinTable()[idx];
}

float minecraftCos(double radians) noexcept {
  const auto idx = static_cast<std::size_t>(
      static_cast<std::int64_t>(radians * 10430.378350470453 + 16384.0) &
      65535LL);
  return sinTable()[idx];
}

int randomBetweenInclusive(RandomSource &rng, int minInclusive,
                           int maxInclusive) {
  if (minInclusive >= maxInclusive)
    return minInclusive;
  return rng.nextInt(maxInclusive - minInclusive + 1) + minInclusive;
}

float randomBetween(RandomSource &rng, float minInclusive,
                    float maxExclusive) {
  if (minInclusive >= maxExclusive)
    return minInclusive;
  return rng.nextFloat() * (maxExclusive - minInclusive) + minInclusive;
}

} // namespace worldgen_math

float FloatProvider::sample(RandomSource &rng) const {
  switch (kind) {
  case FloatProviderKind::Constant:
    return min;
  case FloatProviderKind::Uniform:
    return worldgen_math::randomBetween(rng, min, max);
  case FloatProviderKind::Trapezoid: {
    const float range = max - min;
    const float plateauStart = (range - plateau) / 2.0f;
    const float plateauEnd = range - plateauStart;
    const float f1 = rng.nextFloat();
    const float f2 = rng.nextFloat();
    return min + f1 * plateauEnd + f2 * plateauStart;
  }
  case FloatProviderKind::ClampedNormal: {
    const float val = mean + static_cast<float>(rng.nextGaussian()) * deviation;
    return std::clamp(val, min, max);
  }
  }
  return min;
}

FloatProvider parseFloatProviderJson(const picojson::value &v,
                                     std::string_view ctx) {
  if (v.is<double>())
    return FloatProvider::constant(static_cast<float>(v.get<double>()));
  const auto &obj = asObj(v, ctx);
  if (auto it = obj.find("value"); it != obj.end() && it->second.is<double>()) {
    return FloatProvider::constant(
        static_cast<float>(it->second.get<double>()));
  }
  const std::string type =
      normalizeProviderId(asStr(reqField(obj, "type", ctx), ctx));
  const auto *body = &obj;
  if (auto valIt = obj.find("value");
      valIt != obj.end() && valIt->second.is<picojson::object>()) {
    body = &valIt->second.get<picojson::object>();
  }
  if (type == "minecraft:constant") {
    return FloatProvider::constant(
        static_cast<float>(asNum(reqField(*body, "value", ctx), ctx)));
  }
  if (type == "minecraft:uniform") {
    FloatProvider p;
    p.kind = FloatProviderKind::Uniform;
    p.min =
        static_cast<float>(asNum(reqField(*body, "min_inclusive", ctx), ctx));
    p.max =
        static_cast<float>(asNum(reqField(*body, "max_exclusive", ctx), ctx));
    return p;
  }
  if (type == "minecraft:trapezoid") {
    FloatProvider p;
    p.kind = FloatProviderKind::Trapezoid;
    p.min = static_cast<float>(asNum(reqField(*body, "min", ctx), ctx));
    p.max = static_cast<float>(asNum(reqField(*body, "max", ctx), ctx));
    p.plateau =
        static_cast<float>(asNum(reqField(*body, "plateau", ctx), ctx));
    return p;
  }
  if (type == "minecraft:clamped_normal") {
    FloatProvider p;
    p.kind = FloatProviderKind::ClampedNormal;
    p.mean = static_cast<float>(asNum(reqField(*body, "mean", ctx), ctx));
    p.deviation =
        static_cast<float>(asNum(reqField(*body, "deviation", ctx), ctx));
    p.min = static_cast<float>(asNum(reqField(*body, "min", ctx), ctx));
    p.max = static_cast<float>(asNum(reqField(*body, "max", ctx), ctx));
    return p;
  }
  throw std::runtime_error("Unsupported FloatProvider '" + type + "' in " +
                           std::string(ctx));
}

int IntProvider::sample(RandomSource &rng) const {
  switch (kind) {
  case IntProviderKind::Constant:
    return minInclusive;
  case IntProviderKind::Uniform:
    return worldgen_math::randomBetweenInclusive(rng, minInclusive,
                                                 maxInclusive);
  case IntProviderKind::BiasedToBottom: {
    const int span = maxInclusive - minInclusive + 1;
    if (span <= 1)
      return minInclusive;
    return minInclusive + rng.nextInt(rng.nextInt(span) + 1);
  }
  case IntProviderKind::Clamped:
    if (child.empty())
      return minInclusive;
    return std::clamp(child[0].sample(rng), minInclusive, maxInclusive);
  case IntProviderKind::ClampedNormal: {
    const float v = mean + static_cast<float>(rng.nextGaussian()) * deviation;
    return std::clamp(static_cast<int>(std::round(v)), minInclusive,
                      maxInclusive);
  }
  case IntProviderKind::WeightedList: {
    if (weighted.empty())
      return 0;
    int totalWeight = 0;
    for (const auto &e : weighted)
      totalWeight += e.weight;
    if (totalWeight <= 0)
      return weighted[0].provider[0].sample(rng);
    int r = rng.nextInt(totalWeight);
    for (const auto &e : weighted) {
      r -= e.weight;
      if (r < 0)
        return e.provider[0].sample(rng);
    }
    return weighted.back().provider[0].sample(rng);
  }
  case IntProviderKind::Trapezoid: {
    if (plateau == 0 && maxInclusive == -minInclusive) {
      const int a = rng.nextInt(maxInclusive + 1);
      const int b = rng.nextInt(maxInclusive + 1);
      return a - b;
    }
    const int range = maxInclusive - minInclusive;
    if (plateau == range) {
      return worldgen_math::randomBetweenInclusive(rng, minInclusive,
                                                   maxInclusive);
    }
    const int plateauStart = (range - plateau) / 2;
    const int plateauEnd = range - plateauStart;
    const int r1 =
        worldgen_math::randomBetweenInclusive(rng, 0, plateauEnd);
    const int r2 =
        worldgen_math::randomBetweenInclusive(rng, 0, plateauStart);
    return minInclusive + r1 + r2;
  }
  }
  return minInclusive;
}

IntProvider parseIntProviderJson(const picojson::value &v,
                                 std::string_view ctx) {
  if (v.is<double>())
    return IntProvider::constant(asInteger(v, ctx));
  const auto &obj = asObj(v, ctx);
  const std::string type =
      normalizeProviderId(asStr(reqField(obj, "type", ctx), ctx));
  const auto *body = &obj;
  if (auto valIt = obj.find("value");
      valIt != obj.end() && valIt->second.is<picojson::object>()) {
    body = &valIt->second.get<picojson::object>();
  }
  if (type == "minecraft:constant") {
    return IntProvider::constant(
        asInteger(reqField(*body, "value", ctx), ctx));
  }
  if (type == "minecraft:uniform") {
    IntProvider p;
    p.kind = IntProviderKind::Uniform;
    p.minInclusive = asInteger(reqField(*body, "min_inclusive", ctx), ctx);
    p.maxInclusive = asInteger(reqField(*body, "max_inclusive", ctx), ctx);
    return p;
  }
  if (type == "minecraft:biased_to_bottom") {
    IntProvider p;
    p.kind = IntProviderKind::BiasedToBottom;
    p.minInclusive = asInteger(reqField(*body, "min_inclusive", ctx), ctx);
    p.maxInclusive = asInteger(reqField(*body, "max_inclusive", ctx), ctx);
    return p;
  }
  if (type == "minecraft:clamped") {
    IntProvider p;
    p.kind = IntProviderKind::Clamped;
    p.minInclusive = asInteger(reqField(*body, "min_inclusive", ctx), ctx);
    p.maxInclusive = asInteger(reqField(*body, "max_inclusive", ctx), ctx);
    p.child.push_back(
        parseIntProviderJson(reqField(*body, "source", ctx), ctx));
    return p;
  }
  if (type == "minecraft:clamped_normal") {
    IntProvider p;
    p.kind = IntProviderKind::ClampedNormal;
    p.mean = static_cast<float>(asNum(reqField(*body, "mean", ctx), ctx));
    p.deviation =
        static_cast<float>(asNum(reqField(*body, "deviation", ctx), ctx));
    p.minInclusive = asInteger(reqField(*body, "min_inclusive", ctx), ctx);
    p.maxInclusive = asInteger(reqField(*body, "max_inclusive", ctx), ctx);
    return p;
  }
  if (type == "minecraft:weighted_list") {
    IntProvider p;
    p.kind = IntProviderKind::WeightedList;
    const auto &dist = reqField(*body, "distribution", ctx);
    if (!dist.is<picojson::array>())
      throw std::runtime_error("Expected distribution array in " +
                               std::string(ctx));
    for (const auto &el : dist.get<picojson::array>()) {
      const auto &eo = asObj(el, ctx);
      WeightedIntEntry we;
      we.weight = asInteger(reqField(eo, "weight", ctx), ctx);
      we.provider.push_back(
          parseIntProviderJson(reqField(eo, "data", ctx), ctx));
      p.weighted.push_back(std::move(we));
    }
    return p;
  }
  if (type == "minecraft:trapezoid") {
    IntProvider p;
    p.kind = IntProviderKind::Trapezoid;
    p.minInclusive = asInteger(reqField(*body, "min", ctx), ctx);
    p.maxInclusive = asInteger(reqField(*body, "max", ctx), ctx);
    if (auto pit = body->find("plateau"); pit != body->end()) {
      p.plateau = asInteger(pit->second, ctx);
    }
    return p;
  }
  throw std::runtime_error("Unsupported IntProvider '" + type + "' in " +
                           std::string(ctx));
}

VerticalAnchor parseVerticalAnchorJson(const picojson::value &v,
                                       std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  if (auto it = obj.find("absolute"); it != obj.end())
    return VerticalAnchor::absolute(asInteger(it->second, ctx));
  if (auto it = obj.find("above_bottom"); it != obj.end())
    return VerticalAnchor::aboveBottom(asInteger(it->second, ctx));
  if (auto it = obj.find("below_top"); it != obj.end())
    return VerticalAnchor::belowTop(asInteger(it->second, ctx));
  throw std::runtime_error("Unknown VerticalAnchor in " + std::string(ctx));
}

int HeightProvider::sample(RandomSource &rng,
                           const WorldGenerationContext &ctx) const {
  const int min = minInclusive.resolveY(ctx.minGenY, ctx.genDepth);
  const int max = maxInclusive.resolveY(ctx.minGenY, ctx.genDepth);
  switch (kind) {
  case HeightProviderKind::Constant:
    return min;
  case HeightProviderKind::Uniform:
    if (min > max)
      return min;
    return worldgen_math::randomBetweenInclusive(rng, min, max);
  case HeightProviderKind::BiasedToBottom: {
    if (max - min - inner + 1 <= 0)
      return min;
    const int limit = rng.nextInt(max - min - inner + 1);
    return rng.nextInt(limit + inner) + min;
  }
  case HeightProviderKind::VeryBiasedToBottom: {
    if (max - min - inner + 1 <= 0)
      return min;
    const int upperInclusive =
        worldgen_math::randomBetweenInclusive(rng, min + inner, max);
    const int biasedUpper =
        worldgen_math::randomBetweenInclusive(rng, min, upperInclusive - 1);
    return worldgen_math::randomBetweenInclusive(rng, min,
                                                 biasedUpper - 1 + inner);
  }
  case HeightProviderKind::Trapezoid: {
    if (min > max)
      return min;
    const int range = max - min;
    if (plateau >= range)
      return worldgen_math::randomBetweenInclusive(rng, min, max);
    const int plateauStart = (range - plateau) / 2;
    const int plateauEnd = range - plateauStart;
    const int a = worldgen_math::randomBetweenInclusive(rng, 0, plateauEnd);
    const int b = worldgen_math::randomBetweenInclusive(rng, 0, plateauStart);
    return min + a + b;
  }
  case HeightProviderKind::WeightedList: {
    if (weighted.empty())
      return 0;
    int totalWeight = 0;
    for (const auto &e : weighted)
      totalWeight += e.weight;
    if (totalWeight <= 0)
      return weighted[0].provider[0].sample(rng, ctx);
    int r = rng.nextInt(totalWeight);
    for (const auto &e : weighted) {
      r -= e.weight;
      if (r < 0)
        return e.provider[0].sample(rng, ctx);
    }
    return weighted.back().provider[0].sample(rng, ctx);
  }
  }
  return min;
}

HeightProvider parseHeightProviderJson(const picojson::value &v,
                                       std::string_view ctx) {
  const auto &obj = asObj(v, ctx);
  if (obj.find("type") == obj.end()) {
    // Implicit ConstantHeight shorthand as VerticalAnchor object.
    HeightProvider p;
    p.kind = HeightProviderKind::Constant;
    p.minInclusive = parseVerticalAnchorJson(v, ctx);
    p.maxInclusive = p.minInclusive;
    return p;
  }
  const std::string type =
      normalizeProviderId(asStr(reqField(obj, "type", ctx), ctx));
  if (type == "minecraft:constant") {
    HeightProvider p;
    p.kind = HeightProviderKind::Constant;
    p.minInclusive =
        parseVerticalAnchorJson(reqField(obj, "value", ctx), ctx);
    p.maxInclusive = p.minInclusive;
    return p;
  }
  if (type == "minecraft:uniform") {
    return HeightProvider::uniform(
        parseVerticalAnchorJson(reqField(obj, "min_inclusive", ctx), ctx),
        parseVerticalAnchorJson(reqField(obj, "max_inclusive", ctx), ctx));
  }
  if (type == "minecraft:biased_to_bottom") {
    HeightProvider p;
    p.kind = HeightProviderKind::BiasedToBottom;
    p.minInclusive =
        parseVerticalAnchorJson(reqField(obj, "min_inclusive", ctx), ctx);
    p.maxInclusive =
        parseVerticalAnchorJson(reqField(obj, "max_inclusive", ctx), ctx);
    if (auto it = obj.find("inner"); it != obj.end())
      p.inner = asInteger(it->second, ctx);
    return p;
  }
  if (type == "minecraft:very_biased_to_bottom") {
    HeightProvider p;
    p.kind = HeightProviderKind::VeryBiasedToBottom;
    p.minInclusive =
        parseVerticalAnchorJson(reqField(obj, "min_inclusive", ctx), ctx);
    p.maxInclusive =
        parseVerticalAnchorJson(reqField(obj, "max_inclusive", ctx), ctx);
    if (auto it = obj.find("inner"); it != obj.end())
      p.inner = asInteger(it->second, ctx);
    return p;
  }
  if (type == "minecraft:trapezoid") {
    HeightProvider p;
    p.kind = HeightProviderKind::Trapezoid;
    p.minInclusive =
        parseVerticalAnchorJson(reqField(obj, "min_inclusive", ctx), ctx);
    p.maxInclusive =
        parseVerticalAnchorJson(reqField(obj, "max_inclusive", ctx), ctx);
    if (auto it = obj.find("plateau"); it != obj.end())
      p.plateau = asInteger(it->second, ctx);
    return p;
  }
  if (type == "minecraft:weighted_list") {
    HeightProvider p;
    p.kind = HeightProviderKind::WeightedList;
    const auto &dist = reqField(obj, "distribution", ctx);
    if (!dist.is<picojson::array>())
      throw std::runtime_error("Expected distribution array in " +
                               std::string(ctx));
    for (const auto &el : dist.get<picojson::array>()) {
      const auto &eo = asObj(el, ctx);
      WeightedHeightEntry we;
      we.weight = asInteger(reqField(eo, "weight", ctx), ctx);
      we.provider.push_back(
          parseHeightProviderJson(reqField(eo, "data", ctx), ctx));
      p.weighted.push_back(std::move(we));
    }
    return p;
  }
  throw std::runtime_error("Unsupported HeightProvider '" + type + "' in " +
                           std::string(ctx));
}

} // namespace redstone::worldgen
