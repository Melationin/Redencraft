#include "redstone/worldgen/cubic_spline.hpp"
#include "redstone/worldgen/density_function.hpp"
#include "redstone/worldgen/noise_router.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace redstone::worldgen;

std::uint64_t bitsOf(double v) noexcept {
  return std::bit_cast<std::uint64_t>(v);
}

void require(bool cond, const char *msg) {
  if (!cond)
    throw std::runtime_error(msg);
}

void checkBits(double actualVal, std::uint64_t expectedBits,
               const char *dim, std::int64_t seed, int x, int y, int z,
               const char *channel) {
  const std::uint64_t actualBits = bitsOf(actualVal);
  if (actualBits == expectedBits)
    return;
  const double expectedVal = std::bit_cast<double>(expectedBits);
  const std::uint64_t ulp =
      actualBits > expectedBits ? actualBits - expectedBits
                                : expectedBits - actualBits;
  // Compare finite magnitude difference safely in case signs match
  if (ulp > 0) {
    std::ostringstream oss;
    oss << dim << " seed=" << seed << " (" << x << "," << y << "," << z
        << ") " << channel << " actual=0x" << std::hex << actualBits << " ("
        << std::dec << actualVal << ") expected=0x" << std::hex << expectedBits
        << " (" << std::dec << expectedVal << ") ulp=" << ulp;
    throw std::runtime_error(oss.str());
  }
}

void testCubicSplineDirect() {
  auto c = CubicSpline::constant(3.25f);
  require(c->minValue() == 3.25f && c->maxValue() == 3.25f,
          "constant spline min/max");
  require(c->apply(0, 0, 0) == 3.25f, "constant spline apply");

  auto sp = CubicSpline::multipoint(
      density_functions::yClampedGradient(0, 100, 0.0, 100.0),
      {0.0f, 2.0f},
      {CubicSpline::constant(1.0f), CubicSpline::constant(5.0f)},
      {0.5f, -1.0f});

  require(std::abs(sp->apply(0, 0, 0) - 1.0f) < 1e-6f, "spline left knot");
  require(std::abs(sp->apply(0, 2, 0) - 5.0f) < 1e-6f, "spline right knot");
  auto spNeg = CubicSpline::multipoint(
      density_functions::yClampedGradient(-10, 10, -10.0, 10.0),
      {0.0f, 2.0f},
      {CubicSpline::constant(1.0f), CubicSpline::constant(5.0f)},
      {0.5f, -1.0f});
  require(std::abs(spNeg->apply(0, -2, 0) - 0.0f) < 1e-6f,
          "spline left extrapolation");
  require(std::abs(spNeg->apply(0, 4, 0) - 3.0f) < 1e-6f,
          "spline right extrapolation");
  require(std::abs(spNeg->apply(0, 1, 0) - 3.375f) < 1e-6f,
          "spline midpoint hermite");
  require(spNeg->minValue() <= 0.0f && spNeg->maxValue() >= 5.0f,
          "spline min/max bounds encompass extrema");
}

struct GoldenRow {
  const char *settingsId;
  std::int64_t seed;
  int x;
  int y;
  int z;
  std::array<std::uint64_t, 15> expectedBits;
};

constexpr std::array<GoldenRow, 24> kGoldenRows = {{
    {"minecraft:overworld", 123456789LL, 0, 64, 0,
     {0x3fce165c1477d91dULL, 0xbfca1ecb41f4f199ULL, 0x3fdf5306a5ee3c6eULL,
      0xbfd7287cbfd1c721ULL, 0xbf95b0a2bb5b1c91ULL, 0xbfc2f9d824fd1ccfULL,
      0xbfb27ab17c693e6bULL, 0xbfa52d90f9531be5ULL, 0xbfb554cf40000000ULL,
      0xbf8e08aae7962e78ULL, 0x4044000000000000ULL, 0xbfc08ca5957a4479ULL,
      0x0000000000000000ULL, 0xbfb47ae140000000ULL, 0xbfd45d695c1427efULL}},
    {"minecraft:overworld", 123456789LL, -17, -40, 33,
     {0xbfc61b3e92c55f50ULL, 0x3fdad7ce4746ad61ULL, 0x3fc82d4d5b2de6bbULL,
      0x3fcb8015dabde623ULL, 0xbf910b312ff3813bULL, 0xbfc738307636b1d4ULL,
      0xbfae33e29bc7d228ULL, 0xbfaae29315b28945ULL, 0x3fe70681f4000000ULL,
      0xbf7e9c46d78f9bd8ULL, 0x4044000000000000ULL, 0x3fc56827ffc74d10ULL,
      0xbfc85c378d821b4bULL, 0x3fe01601bb9354ecULL, 0x3fd2172694315dc6ULL}},
    {"minecraft:overworld", 123456789LL, 1200, 64, -1200,
     {0x3faeac7afa7cd07fULL, 0x3fb48fdb0d65e7e7ULL, 0x3fe0b91c99d7c18fULL,
      0xbfd0de2e7030ea27ULL, 0xbfb06fa6e23ce78cULL, 0x3fc80ae6f40b37ffULL,
      0xbfeaacb1cab4e1c4ULL, 0xbfc8113da7d98d16ULL, 0xbfccebee40000000ULL,
      0x3f9c5b327d7433f4ULL, 0x4038000000000000ULL, 0xbfd29f25dcd69680ULL,
      0x0000000000000000ULL, 0xbfb47ae140000000ULL, 0xbfb40f0e127ecdfbULL}},
    {"minecraft:overworld", 123456789LL, 2048, 120, -1536,
     {0xbfa057b4a7bf0db6ULL, 0xbfd1f0dd4ce3da90ULL, 0xbfc38c18acb85b3dULL,
      0xbfe1f673d83f2cc5ULL, 0xbfd37ecd7eda7730ULL, 0xbfd57251b4a9117dULL,
      0xbfd31d3285ee0fa6ULL, 0x3fe00135662d1c52ULL, 0xbfe1f5c29c000000ULL,
      0xbfdd6f53475afb83ULL, 0x4040000000000000ULL, 0xbfdd555555555555ULL,
      0x0000000000000000ULL, 0xbfb47ae140000000ULL, 0xbfa37288f6059b5dULL}},
    {"minecraft:nether", 123456789LL, 0, 64, 0,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x3f5c648a7f1a9ddfULL, 0xbfc3328e48a6df09ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x3f7d0091a0cfe40dULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:nether", 123456789LL, -17, -40, 33,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0xbfb53ae46da6c30eULL, 0xbfba35cdb18a9ecaULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x3fdd555555555555ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:nether", 123456789LL, 1200, 64, -1200,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0xbfc678677f468f31ULL, 0x3fcf3b3d427a4c08ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfb17dcd5cf165fbULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:nether", 123456789LL, 2048, 120, -1536,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0xbfe08b410ea7f7fcULL, 0x3fc1762f865ab435ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x3fcd6da6b11dc51cULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:end", 123456789LL, 0, 64, 0,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x3fe2000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbf7ecbc955ab5425ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:end", 123456789LL, -17, -40, 33,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x3fdc1c8880000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfb329fbe76c8b43ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:end", 123456789LL, 1200, 64, -1200,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x3fdb000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfa42212b0faec27ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:end", 123456789LL, 2048, 120, -1536,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0xbfe3000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfdd555555555555ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},

    {"minecraft:overworld", -987654321LL, 0, 64, 0,
     {0x3fd2b47bb6b2fabdULL, 0xbfd6d760737cbcfaULL, 0x3fc6d9a74d12c30aULL,
      0xbfc0999e2227df5fULL, 0x3fdf8430b83b5cafULL, 0x3fc40ce4916057e4ULL,
      0xbfd9195b3ffa7822ULL, 0x3fd1b872b97a3a62ULL, 0xbfbfae14e0000000ULL,
      0x3fbb5c07a886ad4cULL, 0x4040000000000000ULL, 0xbfba40dee09410deULL,
      0x0000000000000000ULL, 0xbfb47ae140000000ULL, 0x3fd2ec3e732a42d1ULL}},
    {"minecraft:overworld", -987654321LL, -17, -40, 33,
     {0x3fd3de6badfa3614ULL, 0xbfbac0a8c75c37e0ULL, 0xbfa797fd59790496ULL,
      0x3fcbd6a75981d10dULL, 0x3fdfaee40d82dd42ULL, 0x3fc27df410ffa5fdULL,
      0xbfd5e3cba1bd0448ULL, 0x3fd147faf1a6b6c4ULL, 0x3fe60a3d64000000ULL,
      0xbfc4d3a18adaa574ULL, 0x4040000000000000ULL, 0x3fc1d19bd11701a5ULL,
      0xbfd610bc04222410ULL, 0x3fe24188ac5ac32eULL, 0xbfd47c690ebbf211ULL}},
    {"minecraft:overworld", -987654321LL, 1200, 64, -1200,
     {0xbfe1266a2024639eULL, 0x3fcef10609b160fcULL, 0x3fe18bf51741ca3bULL,
      0xbfd5a3bc7fb10ffbULL, 0xbfad309644b4668fULL, 0x3fb3ab2e46496bfaULL,
      0x3f79edaa232e5b4fULL, 0x3fc2a1a9f3ffaab0ULL, 0x3f77a9c400000000ULL,
      0xbfd0b8ecdd004156ULL, 0x404c000000000000ULL, 0x3f9c3b6f75b09664ULL,
      0x0000000000000000ULL, 0xbfb47ae140000000ULL, 0x3fb496effc27751dULL}},
    {"minecraft:overworld", -987654321LL, 2048, 120, -1536,
     {0xbf87bac07f5e9d0fULL, 0xbfb02b364b360581ULL, 0xbfd60979bcaeb34cULL,
      0xbfe47549dd4577e0ULL, 0xbfda55c9f782b2fcULL, 0x3fc525b334dd7a8fULL,
      0x3fd66a86191c1cc0ULL, 0xbfb51218231e5712ULL, 0xbfa9bd4900000000ULL,
      0xbfdf471c813cfdeeULL, 0x4058000000000000ULL, 0xbfc1212a6e532084ULL,
      0x0000000000000000ULL, 0xbfb47ae140000000ULL, 0xbfd8dbeea2ed9e51ULL}},
    {"minecraft:nether", -987654321LL, 0, 64, 0,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0xbfd05cf53803beb4ULL, 0x3fcedd5b899c133eULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x3fbe11ac0cf7da82ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:nether", -987654321LL, -17, -40, 33,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0xbfcd10af877d37f5ULL, 0x3fda9a9e458f0778ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x3fdd555555555555ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:nether", -987654321LL, 1200, 64, -1200,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x3fde06d2d1cb8c0dULL, 0x3fe2815dd96f1757ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfb06c1b20c54795ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:nether", -987654321LL, 2048, 120, -1536,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0xbfe142dd31dac0bdULL, 0xbfc49ad45abaab8cULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x3fc66bb7b7988e59ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:end", -987654321LL, 0, 64, 0,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x3fe2000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfbc786cb6322b36ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:end", -987654321LL, -17, -40, 33,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x3fdc1c8880000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfb329fbe76c8b43ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:end", -987654321LL, 1200, 64, -1200,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x3f9ba65800000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfd386a389d055eaULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
    {"minecraft:end", -987654321LL, 2048, 120, -1536,
     {0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0xbfe3f4d500000000ULL, 0x0000000000000000ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0xbfdd555555555555ULL,
      0x0000000000000000ULL, 0x0000000000000000ULL, 0x0000000000000000ULL}},
}};

constexpr std::array<const char *, 15> kChannelNames = {
    "barrier",
    "fluid_level_floodedness",
    "fluid_level_spread",
    "lava",
    "temperature",
    "vegetation",
    "continents",
    "erosion",
    "depth",
    "ridges",
    "preliminary_surface_level",
    "final_density",
    "vein_toggle",
    "vein_ridged",
    "vein_gap",
};

void testRegistriesAndRouters(const std::filesystem::path &assetsDir) {
  const WorldgenRegistries reg = WorldgenRegistries::loadFromDirectory(assetsDir);
  for (const char *preset :
       {"minecraft:overworld", "minecraft:large_biomes", "minecraft:amplified",
        "minecraft:nether", "minecraft:end", "minecraft:caves",
        "minecraft:floating_islands"}) {
    require(reg.findNoiseSettings(preset) != nullptr, preset);
  }
  require(!reg.findNoiseSettings("minecraft:overworld")->legacyRandomSource,
          "overworld uses Xoroshiro");
  require(reg.findNoiseSettings("minecraft:nether")->legacyRandomSource,
          "nether uses LegacyRandomSource");
  require(reg.findNoiseSettings("minecraft:end")->legacyRandomSource,
          "end uses LegacyRandomSource");

  for (const auto &row : kGoldenRows) {
    const auto *settings = reg.findNoiseSettings(row.settingsId);
    require(settings != nullptr, row.settingsId);
    RandomState state = RandomState::create(*settings, reg, row.seed);
    const NoiseRouter &r = state.router();
    const int x = row.x, y = row.y, z = row.z;
    const std::array<double, 15> actual = {
        r.barrierNoise->compute(x, y, z),
        r.fluidLevelFloodednessNoise->compute(x, y, z),
        r.fluidLevelSpreadNoise->compute(x, y, z),
        r.lavaNoise->compute(x, y, z),
        r.temperature->compute(x, y, z),
        r.vegetation->compute(x, y, z),
        r.continents->compute(x, y, z),
        r.erosion->compute(x, y, z),
        r.depth->compute(x, y, z),
        r.ridges->compute(x, y, z),
        r.preliminarySurfaceLevel->compute(x, y, z),
        r.finalDensity->compute(x, y, z),
        r.veinToggle->compute(x, y, z),
        r.veinRidged->compute(x, y, z),
        r.veinGap->compute(x, y, z),
    };
    for (std::size_t i = 0; i < 15; ++i) {
      checkBits(actual[i], row.expectedBits[i], row.settingsId, row.seed, row.x,
                row.y, row.z, kChannelNames[i]);
    }
    const ClimateSampler &s = state.sampler();
    require(bitsOf(s.temperature->compute(x, y, z)) == bitsOf(actual[4]),
            "sampler temperature matches router");
    require(bitsOf(s.humidity->compute(x, y, z)) == bitsOf(actual[5]),
            "sampler humidity matches router");
    require(bitsOf(s.continentalness->compute(x, y, z)) == bitsOf(actual[6]),
            "sampler continentalness matches router");
    require(bitsOf(s.erosion->compute(x, y, z)) == bitsOf(actual[7]),
            "sampler erosion matches router");
    require(bitsOf(s.depth->compute(x, y, z)) == bitsOf(actual[8]),
            "sampler depth matches router");
    require(bitsOf(s.weirdness->compute(x, y, z)) == bitsOf(actual[9]),
            "sampler weirdness matches router");
  }
}


void testFlatDensitySampler(const std::filesystem::path &assetsDir) {
  const WorldgenRegistries reg = WorldgenRegistries::loadFromDirectory(assetsDir);
  int flatSamplers = 0;
  for (const GoldenRow &row : kGoldenRows) {
    const auto *settings = reg.findNoiseSettings(row.settingsId);
    require(settings != nullptr, row.settingsId);
    RandomState state = RandomState::create(*settings, reg, row.seed);
    const ClimateSampler &flat = state.sampler();
    ClimateSampler fallback = flat;
    fallback.flat.reset();
    if (flat.flat)
      ++flatSamplers;
    for (int qx = -48; qx <= 48; qx += 7) {
      for (int qz = -48; qz <= 48; qz += 11) {
        for (int qy = -8; qy <= 8; qy += 4) {
          const ClimateTargetPoint a = flat.sample(qx, qy, qz);
          const ClimateTargetPoint b = fallback.sample(qx, qy, qz);
          if (std::memcmp(&a, &b, sizeof(a)) != 0)
            throw std::runtime_error("flat sampler differs from recursive");
        }
      }
    }
  }
  require(flatSamplers > 0, "no climate sampler compiled a flat evaluator");
}

} // namespace

int main(int argc, char **argv) {
  try {
    const std::filesystem::path assetsDir =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path("assets/worldgen26_1");
    testCubicSplineDirect();
    testRegistriesAndRouters(assetsDir);
    testFlatDensitySampler(assetsDir);
    std::cout << "worldgen_g1_router tests passed\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "worldgen_g1_router test failed: " << ex.what() << "\n";
    return 1;
  }
}
