#include "redstone/worldgen/biome_source.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>

namespace redstone::worldgen {
namespace {

constexpr int floorDiv4(int v) noexcept {
  return v >= 0 ? (v / 4) : ((v - 3) / 4);
}

constexpr int mod4(int v) noexcept {
  const int m = v % 4;
  return m < 0 ? m + 4 : m;
}

constexpr int floorDiv16(int v) noexcept {
  return v >= 0 ? (v / 16) : ((v - 15) / 16);
}

std::int64_t lcgNext(std::int64_t rval, std::int64_t c) noexcept {
  std::uint64_t u = static_cast<std::uint64_t>(rval);
  u *= u * 6364136223846793005ULL + 1442695040888963407ULL;
  return static_cast<std::int64_t>(u + static_cast<std::uint64_t>(c));
}

double biomeFiddle(std::int64_t rval) noexcept {
  // Equivalent to Math.floorMod(rval >> 24, 1024) / 1024.0
  const std::uint64_t shifted = static_cast<std::uint64_t>(rval >> 24);
  const double uniform = static_cast<double>(shifted & 1023ULL) / 1024.0;
  return (uniform - 0.5) * 0.9;
}

// Standalone SHA-256 matching Guava `Hashing.sha256().hashLong(seed).asLong()`
// (8 little-endian input bytes -> first 8 digest bytes interpreted little-endian).
std::uint32_t rotr32(std::uint32_t x, unsigned n) noexcept {
  return (x >> n) | (x << (32U - n));
}

std::array<std::uint8_t, 32> sha256(const std::uint8_t *data,
                                    std::size_t len) noexcept {
  static constexpr std::array<std::uint32_t, 64> K = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  std::array<std::uint32_t, 8> h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                    0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                    0x1f83d9abU, 0x5be0cd19U};

  std::array<std::uint8_t, 64> block{};
  if (len <= 55) {
    std::memcpy(block.data(), data, len);
    block[len] = 0x80U;
    const std::uint64_t bitLen = static_cast<std::uint64_t>(len) * 8ULL;
    for (int i = 0; i < 8; ++i)
      block[56 + i] = static_cast<std::uint8_t>((bitLen >> (56 - i * 8)) & 0xFFU);
  }

  std::array<std::uint32_t, 64> w{};
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
  std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = hh + S1 + ch + K[i] + w[i];
    const std::uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = S0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;

  std::array<std::uint8_t, 32> digest{};
  for (int i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFFU);
    digest[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFFU);
    digest[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFFU);
    digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFFU);
  }
  return digest;
}

class OverworldBuilderImpl {
public:
  using Sink = std::vector<std::pair<ClimateParameterPoint, std::string>>;

  const ClimateParameter FULL_RANGE = ClimateParameter::span(-1.0f, 1.0f);
  const std::array<ClimateParameter, 5> temperatures = {
      ClimateParameter::span(-1.0f, -0.45f),
      ClimateParameter::span(-0.45f, -0.15f),
      ClimateParameter::span(-0.15f, 0.2f),
      ClimateParameter::span(0.2f, 0.55f),
      ClimateParameter::span(0.55f, 1.0f),
  };
  const std::array<ClimateParameter, 5> humidities = {
      ClimateParameter::span(-1.0f, -0.35f),
      ClimateParameter::span(-0.35f, -0.1f),
      ClimateParameter::span(-0.1f, 0.1f),
      ClimateParameter::span(0.1f, 0.3f),
      ClimateParameter::span(0.3f, 1.0f),
  };
  const std::array<ClimateParameter, 7> erosions = {
      ClimateParameter::span(-1.0f, -0.78f),
      ClimateParameter::span(-0.78f, -0.375f),
      ClimateParameter::span(-0.375f, -0.2225f),
      ClimateParameter::span(-0.2225f, 0.05f),
      ClimateParameter::span(0.05f, 0.45f),
      ClimateParameter::span(0.45f, 0.55f),
      ClimateParameter::span(0.55f, 1.0f),
  };
  const ClimateParameter FROZEN_RANGE = temperatures[0];
  const ClimateParameter UNFROZEN_RANGE =
      ClimateParameter::span(temperatures[1], temperatures[4]);

  const ClimateParameter mushroomFieldsContinentalness =
      ClimateParameter::span(-1.2f, -1.05f);
  const ClimateParameter deepOceanContinentalness =
      ClimateParameter::span(-1.05f, -0.455f);
  const ClimateParameter oceanContinentalness =
      ClimateParameter::span(-0.455f, -0.19f);
  const ClimateParameter coastContinentalness =
      ClimateParameter::span(-0.19f, -0.11f);
  const ClimateParameter inlandContinentalness =
      ClimateParameter::span(-0.11f, 0.55f);
  const ClimateParameter nearInlandContinentalness =
      ClimateParameter::span(-0.11f, 0.03f);
  const ClimateParameter midInlandContinentalness =
      ClimateParameter::span(0.03f, 0.3f);
  const ClimateParameter farInlandContinentalness =
      ClimateParameter::span(0.3f, 1.0f);

  static constexpr const char *OCEANS[2][5] = {
      {"minecraft:deep_frozen_ocean", "minecraft:deep_cold_ocean",
       "minecraft:deep_ocean", "minecraft:deep_lukewarm_ocean",
       "minecraft:warm_ocean"},
      {"minecraft:frozen_ocean", "minecraft:cold_ocean", "minecraft:ocean",
       "minecraft:lukewarm_ocean", "minecraft:warm_ocean"},
  };

  static constexpr const char *MIDDLE_BIOMES[5][5] = {
      {"minecraft:snowy_plains", "minecraft:snowy_plains",
       "minecraft:snowy_plains", "minecraft:snowy_taiga", "minecraft:taiga"},
      {"minecraft:plains", "minecraft:plains", "minecraft:forest",
       "minecraft:taiga", "minecraft:old_growth_spruce_taiga"},
      {"minecraft:flower_forest", "minecraft:plains", "minecraft:forest",
       "minecraft:birch_forest", "minecraft:dark_forest"},
      {"minecraft:savanna", "minecraft:savanna", "minecraft:forest",
       "minecraft:jungle", "minecraft:jungle"},
      {"minecraft:desert", "minecraft:desert", "minecraft:desert",
       "minecraft:desert", "minecraft:desert"},
  };

  static constexpr const char *MIDDLE_BIOMES_VARIANT[5][5] = {
      {"minecraft:ice_spikes", nullptr, "minecraft:snowy_taiga", nullptr,
       nullptr},
      {nullptr, nullptr, nullptr, nullptr,
       "minecraft:old_growth_pine_taiga"},
      {"minecraft:sunflower_plains", nullptr, nullptr,
       "minecraft:old_growth_birch_forest", nullptr},
      {nullptr, nullptr, "minecraft:plains", "minecraft:sparse_jungle",
       "minecraft:bamboo_jungle"},
      {nullptr, nullptr, nullptr, nullptr, nullptr},
  };

  static constexpr const char *PLATEAU_BIOMES[5][5] = {
      {"minecraft:snowy_plains", "minecraft:snowy_plains",
       "minecraft:snowy_plains", "minecraft:snowy_taiga",
       "minecraft:snowy_taiga"},
      {"minecraft:meadow", "minecraft:meadow", "minecraft:forest",
       "minecraft:taiga", "minecraft:old_growth_spruce_taiga"},
      {"minecraft:meadow", "minecraft:meadow", "minecraft:meadow",
       "minecraft:meadow", "minecraft:pale_garden"},
      {"minecraft:savanna_plateau", "minecraft:savanna_plateau",
       "minecraft:forest", "minecraft:forest", "minecraft:jungle"},
      {"minecraft:badlands", "minecraft:badlands", "minecraft:badlands",
       "minecraft:wooded_badlands", "minecraft:wooded_badlands"},
  };

  static constexpr const char *PLATEAU_BIOMES_VARIANT[5][5] = {
      {"minecraft:ice_spikes", nullptr, nullptr, nullptr, nullptr},
      {"minecraft:cherry_grove", nullptr, "minecraft:meadow",
       "minecraft:meadow", "minecraft:old_growth_pine_taiga"},
      {"minecraft:cherry_grove", "minecraft:cherry_grove", "minecraft:forest",
       "minecraft:birch_forest", nullptr},
      {nullptr, nullptr, nullptr, nullptr, nullptr},
      {"minecraft:eroded_badlands", "minecraft:eroded_badlands", nullptr,
       nullptr, nullptr},
  };

  static constexpr const char *SHATTERED_BIOMES[5][5] = {
      {"minecraft:windswept_gravelly_hills",
       "minecraft:windswept_gravelly_hills", "minecraft:windswept_hills",
       "minecraft:windswept_forest", "minecraft:windswept_forest"},
      {"minecraft:windswept_gravelly_hills",
       "minecraft:windswept_gravelly_hills", "minecraft:windswept_hills",
       "minecraft:windswept_forest", "minecraft:windswept_forest"},
      {"minecraft:windswept_hills", "minecraft:windswept_hills",
       "minecraft:windswept_hills", "minecraft:windswept_forest",
       "minecraft:windswept_forest"},
      {nullptr, nullptr, nullptr, nullptr, nullptr},
      {nullptr, nullptr, nullptr, nullptr, nullptr},
  };

  void addBiomes(Sink &out) const {
    out.reserve(7600);
    addOffCoastBiomes(out);
    addInlandBiomes(out);
    addUndergroundBiomes(out);
  }

private:
  void addSurfaceBiome(Sink &out, const ClimateParameter &t,
                       const ClimateParameter &h, const ClimateParameter &c,
                       const ClimateParameter &e, const ClimateParameter &w,
                       float offset, const char *biome) const {
    out.emplace_back(
        ClimateParameterPoint::make(t, h, c, e, ClimateParameter::point(0.0f), w,
                                    offset),
        biome);
    out.emplace_back(
        ClimateParameterPoint::make(t, h, c, e, ClimateParameter::point(1.0f), w,
                                    offset),
        biome);
  }

  void addUndergroundBiome(Sink &out, const ClimateParameter &t,
                           const ClimateParameter &h, const ClimateParameter &c,
                           const ClimateParameter &e, const ClimateParameter &w,
                           float offset, const char *biome) const {
    out.emplace_back(ClimateParameterPoint::make(
                         t, h, c, e, ClimateParameter::span(0.2f, 0.9f), w,
                         offset),
                     biome);
  }

  void addBottomBiome(Sink &out, const ClimateParameter &t,
                      const ClimateParameter &h, const ClimateParameter &c,
                      const ClimateParameter &e, const ClimateParameter &w,
                      float offset, const char *biome) const {
    out.emplace_back(
        ClimateParameterPoint::make(t, h, c, e, ClimateParameter::point(1.1f), w,
                                    offset),
        biome);
  }

  const char *pickMiddleBiome(int ti, int hi, const ClimateParameter &w) const {
    if (w.max < 0)
      return MIDDLE_BIOMES[ti][hi];
    const char *v = MIDDLE_BIOMES_VARIANT[ti][hi];
    return v ? v : MIDDLE_BIOMES[ti][hi];
  }

  const char *pickMiddleBiomeOrBadlandsIfHot(int ti, int hi,
                                             const ClimateParameter &w) const {
    return ti == 4 ? pickBadlandsBiome(hi, w) : pickMiddleBiome(ti, hi, w);
  }

  const char *
  pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(int ti, int hi,
                                              const ClimateParameter &w) const {
    return ti == 0 ? pickSlopeBiome(ti, hi, w)
                   : pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
  }

  const char *maybePickWindsweptSavannaBiome(int ti, int hi,
                                            const ClimateParameter &w,
                                            const char *underlying) const {
    return (ti > 1 && hi < 4 && w.max >= 0) ? "minecraft:windswept_savanna"
                                            : underlying;
  }

  const char *pickShatteredCoastBiome(int ti, int hi,
                                      const ClimateParameter &w) const {
    const char *base =
        w.max >= 0 ? pickMiddleBiome(ti, hi, w) : pickBeachBiome(ti, hi);
    return maybePickWindsweptSavannaBiome(ti, hi, w, base);
  }

  const char *pickBeachBiome(int ti, int /*hi*/) const {
    if (ti == 0)
      return "minecraft:snowy_beach";
    return ti == 4 ? "minecraft:desert" : "minecraft:beach";
  }

  const char *pickBadlandsBiome(int hi, const ClimateParameter &w) const {
    if (hi < 2)
      return w.max < 0 ? "minecraft:badlands" : "minecraft:eroded_badlands";
    return hi < 3 ? "minecraft:badlands" : "minecraft:wooded_badlands";
  }

  const char *pickPlateauBiome(int ti, int hi,
                               const ClimateParameter &w) const {
    if (w.max >= 0) {
      const char *v = PLATEAU_BIOMES_VARIANT[ti][hi];
      if (v)
        return v;
    }
    return PLATEAU_BIOMES[ti][hi];
  }

  const char *pickPeakBiome(int ti, int hi, const ClimateParameter &w) const {
    if (ti <= 2)
      return w.max < 0 ? "minecraft:jagged_peaks" : "minecraft:frozen_peaks";
    return ti == 3 ? "minecraft:stony_peaks" : pickBadlandsBiome(hi, w);
  }

  const char *pickSlopeBiome(int ti, int hi, const ClimateParameter &w) const {
    if (ti >= 3)
      return pickPlateauBiome(ti, hi, w);
    return hi <= 1 ? "minecraft:snowy_slopes" : "minecraft:grove";
  }

  const char *pickShatteredBiome(int ti, int hi,
                                 const ClimateParameter &w) const {
    const char *b = SHATTERED_BIOMES[ti][hi];
    return b ? b : pickMiddleBiome(ti, hi, w);
  }

  void addOffCoastBiomes(Sink &out) const {
    addSurfaceBiome(out, FULL_RANGE, FULL_RANGE, mushroomFieldsContinentalness,
                    FULL_RANGE, FULL_RANGE, 0.0f, "minecraft:mushroom_fields");
    for (int ti = 0; ti < 5; ++ti) {
      addSurfaceBiome(out, temperatures[ti], FULL_RANGE,
                      deepOceanContinentalness, FULL_RANGE, FULL_RANGE, 0.0f,
                      OCEANS[0][ti]);
      addSurfaceBiome(out, temperatures[ti], FULL_RANGE, oceanContinentalness,
                      FULL_RANGE, FULL_RANGE, 0.0f, OCEANS[1][ti]);
    }
  }

  void addInlandBiomes(Sink &out) const {
    addMidSlice(out, ClimateParameter::span(-1.0f, -0.93333334f));
    addHighSlice(out, ClimateParameter::span(-0.93333334f, -0.7666667f));
    addPeaks(out, ClimateParameter::span(-0.7666667f, -0.56666666f));
    addHighSlice(out, ClimateParameter::span(-0.56666666f, -0.4f));
    addMidSlice(out, ClimateParameter::span(-0.4f, -0.26666668f));
    addLowSlice(out, ClimateParameter::span(-0.26666668f, -0.05f));
    addValleys(out, ClimateParameter::span(-0.05f, 0.05f));
    addLowSlice(out, ClimateParameter::span(0.05f, 0.26666668f));
    addMidSlice(out, ClimateParameter::span(0.26666668f, 0.4f));
    addHighSlice(out, ClimateParameter::span(0.4f, 0.56666666f));
    addPeaks(out, ClimateParameter::span(0.56666666f, 0.7666667f));
    addHighSlice(out, ClimateParameter::span(0.7666667f, 0.93333334f));
    addMidSlice(out, ClimateParameter::span(0.93333334f, 1.0f));
  }

  void addPeaks(Sink &out, const ClimateParameter &w) const {
    const auto coastToFar =
        ClimateParameter::span(coastContinentalness, farInlandContinentalness);
    const auto coastToNear =
        ClimateParameter::span(coastContinentalness, nearInlandContinentalness);
    const auto midToFar =
        ClimateParameter::span(midInlandContinentalness, farInlandContinentalness);
    const auto e2to3 = ClimateParameter::span(erosions[2], erosions[3]);

    for (int ti = 0; ti < 5; ++ti) {
      const auto &t = temperatures[ti];
      for (int hi = 0; hi < 5; ++hi) {
        const auto &h = humidities[hi];
        const char *mid = pickMiddleBiome(ti, hi, w);
        const char *midHot = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
        const char *midHotCold =
            pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(ti, hi, w);
        const char *plat = pickPlateauBiome(ti, hi, w);
        const char *shat = pickShatteredBiome(ti, hi, w);
        const char *shatSav = maybePickWindsweptSavannaBiome(ti, hi, w, shat);
        const char *peak = pickPeakBiome(ti, hi, w);

        addSurfaceBiome(out, t, h, coastToFar, erosions[0], w, 0.0f, peak);
        addSurfaceBiome(out, t, h, coastToNear, erosions[1], w, 0.0f,
                        midHotCold);
        addSurfaceBiome(out, t, h, midToFar, erosions[1], w, 0.0f, peak);
        addSurfaceBiome(out, t, h, coastToNear, e2to3, w, 0.0f, mid);
        addSurfaceBiome(out, t, h, midToFar, erosions[2], w, 0.0f, plat);
        addSurfaceBiome(out, t, h, midInlandContinentalness, erosions[3], w,
                        0.0f, midHot);
        addSurfaceBiome(out, t, h, farInlandContinentalness, erosions[3], w,
                        0.0f, plat);
        addSurfaceBiome(out, t, h, coastToFar, erosions[4], w, 0.0f, mid);
        addSurfaceBiome(out, t, h, coastToNear, erosions[5], w, 0.0f, shatSav);
        addSurfaceBiome(out, t, h, midToFar, erosions[5], w, 0.0f, shat);
        addSurfaceBiome(out, t, h, coastToFar, erosions[6], w, 0.0f, mid);
      }
    }
  }

  void addHighSlice(Sink &out, const ClimateParameter &w) const {
    const auto coastToNear =
        ClimateParameter::span(coastContinentalness, nearInlandContinentalness);
    const auto midToFar =
        ClimateParameter::span(midInlandContinentalness, farInlandContinentalness);
    const auto coastToFar =
        ClimateParameter::span(coastContinentalness, farInlandContinentalness);
    const auto e0to1 = ClimateParameter::span(erosions[0], erosions[1]);
    const auto e2to3 = ClimateParameter::span(erosions[2], erosions[3]);

    for (int ti = 0; ti < 5; ++ti) {
      const auto &t = temperatures[ti];
      for (int hi = 0; hi < 5; ++hi) {
        const auto &h = humidities[hi];
        const char *mid = pickMiddleBiome(ti, hi, w);
        const char *midHot = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
        const char *midHotCold =
            pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(ti, hi, w);
        const char *plat = pickPlateauBiome(ti, hi, w);
        const char *shat = pickShatteredBiome(ti, hi, w);
        const char *midSav = maybePickWindsweptSavannaBiome(ti, hi, w, mid);
        const char *slope = pickSlopeBiome(ti, hi, w);
        const char *peak = pickPeakBiome(ti, hi, w);

        addSurfaceBiome(out, t, h, coastContinentalness, e0to1, w, 0.0f, mid);
        addSurfaceBiome(out, t, h, nearInlandContinentalness, erosions[0], w,
                        0.0f, slope);
        addSurfaceBiome(out, t, h, midToFar, erosions[0], w, 0.0f, peak);
        addSurfaceBiome(out, t, h, nearInlandContinentalness, erosions[1], w,
                        0.0f, midHotCold);
        addSurfaceBiome(out, t, h, midToFar, erosions[1], w, 0.0f, slope);
        addSurfaceBiome(out, t, h, coastToNear, e2to3, w, 0.0f, mid);
        addSurfaceBiome(out, t, h, midToFar, erosions[2], w, 0.0f, plat);
        addSurfaceBiome(out, t, h, midInlandContinentalness, erosions[3], w,
                        0.0f, midHot);
        addSurfaceBiome(out, t, h, farInlandContinentalness, erosions[3], w,
                        0.0f, plat);
        addSurfaceBiome(out, t, h, coastToFar, erosions[4], w, 0.0f, mid);
        addSurfaceBiome(out, t, h, coastToNear, erosions[5], w, 0.0f, midSav);
        addSurfaceBiome(out, t, h, midToFar, erosions[5], w, 0.0f, shat);
        addSurfaceBiome(out, t, h, coastToFar, erosions[6], w, 0.0f, mid);
      }
    }
  }

  void addMidSlice(Sink &out, const ClimateParameter &w) const {
    const auto nearToFar =
        ClimateParameter::span(nearInlandContinentalness, farInlandContinentalness);
    const auto nearToMid =
        ClimateParameter::span(nearInlandContinentalness, midInlandContinentalness);
    const auto coastToNear =
        ClimateParameter::span(coastContinentalness, nearInlandContinentalness);
    const auto midToFar =
        ClimateParameter::span(midInlandContinentalness, farInlandContinentalness);
    const auto coastToFar =
        ClimateParameter::span(coastContinentalness, farInlandContinentalness);

    addSurfaceBiome(out, FULL_RANGE, FULL_RANGE, coastContinentalness,
                    ClimateParameter::span(erosions[0], erosions[2]), w, 0.0f,
                    "minecraft:stony_shore");
    addSurfaceBiome(out, ClimateParameter::span(temperatures[1], temperatures[2]),
                    FULL_RANGE, nearToFar, erosions[6], w, 0.0f,
                    "minecraft:swamp");
    addSurfaceBiome(out, ClimateParameter::span(temperatures[3], temperatures[4]),
                    FULL_RANGE, nearToFar, erosions[6], w, 0.0f,
                    "minecraft:mangrove_swamp");

    for (int ti = 0; ti < 5; ++ti) {
      const auto &t = temperatures[ti];
      for (int hi = 0; hi < 5; ++hi) {
        const auto &h = humidities[hi];
        const char *mid = pickMiddleBiome(ti, hi, w);
        const char *midHot = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
        const char *midHotCold =
            pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(ti, hi, w);
        const char *shat = pickShatteredBiome(ti, hi, w);
        const char *plat = pickPlateauBiome(ti, hi, w);
        const char *beach = pickBeachBiome(ti, hi);
        const char *midSav = maybePickWindsweptSavannaBiome(ti, hi, w, mid);
        const char *shatCoast = pickShatteredCoastBiome(ti, hi, w);
        const char *slope = pickSlopeBiome(ti, hi, w);

        addSurfaceBiome(out, t, h, nearToFar, erosions[0], w, 0.0f, slope);
        addSurfaceBiome(out, t, h, nearToMid, erosions[1], w, 0.0f,
                        midHotCold);
        addSurfaceBiome(out, t, h, farInlandContinentalness, erosions[1], w,
                        0.0f, ti == 0 ? slope : plat);
        addSurfaceBiome(out, t, h, nearInlandContinentalness, erosions[2], w,
                        0.0f, mid);
        addSurfaceBiome(out, t, h, midInlandContinentalness, erosions[2], w,
                        0.0f, midHot);
        addSurfaceBiome(out, t, h, farInlandContinentalness, erosions[2], w,
                        0.0f, plat);
        addSurfaceBiome(out, t, h, coastToNear, erosions[3], w, 0.0f, mid);
        addSurfaceBiome(out, t, h, midToFar, erosions[3], w, 0.0f, midHot);
        if (w.max < 0) {
          addSurfaceBiome(out, t, h, coastContinentalness, erosions[4], w, 0.0f,
                          beach);
          addSurfaceBiome(out, t, h, nearToFar, erosions[4], w, 0.0f, mid);
        } else {
          addSurfaceBiome(out, t, h, coastToFar, erosions[4], w, 0.0f, mid);
        }
        addSurfaceBiome(out, t, h, coastContinentalness, erosions[5], w, 0.0f,
                        shatCoast);
        addSurfaceBiome(out, t, h, nearInlandContinentalness, erosions[5], w,
                        0.0f, midSav);
        addSurfaceBiome(out, t, h, midToFar, erosions[5], w, 0.0f, shat);
        addSurfaceBiome(out, t, h, coastContinentalness, erosions[6], w, 0.0f,
                        w.max < 0 ? beach : mid);
        if (ti == 0) {
          addSurfaceBiome(out, t, h, nearToFar, erosions[6], w, 0.0f, mid);
        }
      }
    }
  }

  void addLowSlice(Sink &out, const ClimateParameter &w) const {
    const auto nearToFar =
        ClimateParameter::span(nearInlandContinentalness, farInlandContinentalness);
    const auto midToFar =
        ClimateParameter::span(midInlandContinentalness, farInlandContinentalness);
    const auto e0to1 = ClimateParameter::span(erosions[0], erosions[1]);
    const auto e2to3 = ClimateParameter::span(erosions[2], erosions[3]);
    const auto e3to4 = ClimateParameter::span(erosions[3], erosions[4]);

    addSurfaceBiome(out, FULL_RANGE, FULL_RANGE, coastContinentalness,
                    ClimateParameter::span(erosions[0], erosions[2]), w, 0.0f,
                    "minecraft:stony_shore");
    addSurfaceBiome(out, ClimateParameter::span(temperatures[1], temperatures[2]),
                    FULL_RANGE, nearToFar, erosions[6], w, 0.0f,
                    "minecraft:swamp");
    addSurfaceBiome(out, ClimateParameter::span(temperatures[3], temperatures[4]),
                    FULL_RANGE, nearToFar, erosions[6], w, 0.0f,
                    "minecraft:mangrove_swamp");

    for (int ti = 0; ti < 5; ++ti) {
      const auto &t = temperatures[ti];
      for (int hi = 0; hi < 5; ++hi) {
        const auto &h = humidities[hi];
        const char *mid = pickMiddleBiome(ti, hi, w);
        const char *midHot = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
        const char *midHotCold =
            pickMiddleBiomeOrBadlandsIfHotOrSlopeIfCold(ti, hi, w);
        const char *beach = pickBeachBiome(ti, hi);
        const char *midSav = maybePickWindsweptSavannaBiome(ti, hi, w, mid);
        const char *shatCoast = pickShatteredCoastBiome(ti, hi, w);

        addSurfaceBiome(out, t, h, nearInlandContinentalness, e0to1, w, 0.0f,
                        midHot);
        addSurfaceBiome(out, t, h, midToFar, e0to1, w, 0.0f, midHotCold);
        addSurfaceBiome(out, t, h, nearInlandContinentalness, e2to3, w, 0.0f,
                        mid);
        addSurfaceBiome(out, t, h, midToFar, e2to3, w, 0.0f, midHot);
        addSurfaceBiome(out, t, h, coastContinentalness, e3to4, w, 0.0f, beach);
        addSurfaceBiome(out, t, h, nearToFar, erosions[4], w, 0.0f, mid);
        addSurfaceBiome(out, t, h, coastContinentalness, erosions[5], w, 0.0f,
                        shatCoast);
        addSurfaceBiome(out, t, h, nearInlandContinentalness, erosions[5], w,
                        0.0f, midSav);
        addSurfaceBiome(out, t, h, midToFar, erosions[5], w, 0.0f, mid);
        addSurfaceBiome(out, t, h, coastContinentalness, erosions[6], w, 0.0f,
                        beach);
        if (ti == 0) {
          addSurfaceBiome(out, t, h, nearToFar, erosions[6], w, 0.0f, mid);
        }
      }
    }
  }

  void addValleys(Sink &out, const ClimateParameter &w) const {
    const auto coastToFar =
        ClimateParameter::span(coastContinentalness, farInlandContinentalness);
    const auto inlandToFar =
        ClimateParameter::span(inlandContinentalness, farInlandContinentalness);
    const auto midToFar =
        ClimateParameter::span(midInlandContinentalness, farInlandContinentalness);
    const auto e0to1 = ClimateParameter::span(erosions[0], erosions[1]);
    const auto e2to5 = ClimateParameter::span(erosions[2], erosions[5]);

    addSurfaceBiome(out, FROZEN_RANGE, FULL_RANGE, coastContinentalness, e0to1,
                    w, 0.0f,
                    w.max < 0 ? "minecraft:stony_shore"
                              : "minecraft:frozen_river");
    addSurfaceBiome(out, UNFROZEN_RANGE, FULL_RANGE, coastContinentalness,
                    e0to1, w, 0.0f,
                    w.max < 0 ? "minecraft:stony_shore" : "minecraft:river");
    addSurfaceBiome(out, FROZEN_RANGE, FULL_RANGE, nearInlandContinentalness,
                    e0to1, w, 0.0f, "minecraft:frozen_river");
    addSurfaceBiome(out, UNFROZEN_RANGE, FULL_RANGE, nearInlandContinentalness,
                    e0to1, w, 0.0f, "minecraft:river");
    addSurfaceBiome(out, FROZEN_RANGE, FULL_RANGE, coastToFar, e2to5, w, 0.0f,
                    "minecraft:frozen_river");
    addSurfaceBiome(out, UNFROZEN_RANGE, FULL_RANGE, coastToFar, e2to5, w, 0.0f,
                    "minecraft:river");
    addSurfaceBiome(out, FROZEN_RANGE, FULL_RANGE, coastContinentalness,
                    erosions[6], w, 0.0f, "minecraft:frozen_river");
    addSurfaceBiome(out, UNFROZEN_RANGE, FULL_RANGE, coastContinentalness,
                    erosions[6], w, 0.0f, "minecraft:river");
    addSurfaceBiome(out, ClimateParameter::span(temperatures[1], temperatures[2]),
                    FULL_RANGE, inlandToFar, erosions[6], w, 0.0f,
                    "minecraft:swamp");
    addSurfaceBiome(out, ClimateParameter::span(temperatures[3], temperatures[4]),
                    FULL_RANGE, inlandToFar, erosions[6], w, 0.0f,
                    "minecraft:mangrove_swamp");
    addSurfaceBiome(out, FROZEN_RANGE, FULL_RANGE, inlandToFar, erosions[6], w,
                    0.0f, "minecraft:frozen_river");

    for (int ti = 0; ti < 5; ++ti) {
      const auto &t = temperatures[ti];
      for (int hi = 0; hi < 5; ++hi) {
        const auto &h = humidities[hi];
        const char *midHot = pickMiddleBiomeOrBadlandsIfHot(ti, hi, w);
        addSurfaceBiome(out, t, h, midToFar, e0to1, w, 0.0f, midHot);
      }
    }
  }

  void addUndergroundBiomes(Sink &out) const {
    addUndergroundBiome(out, FULL_RANGE, FULL_RANGE,
                        ClimateParameter::span(0.8f, 1.0f), FULL_RANGE,
                        FULL_RANGE, 0.0f, "minecraft:dripstone_caves");
    addUndergroundBiome(out, FULL_RANGE, ClimateParameter::span(0.7f, 1.0f),
                        FULL_RANGE, FULL_RANGE, FULL_RANGE, 0.0f,
                        "minecraft:lush_caves");
    addBottomBiome(out, FULL_RANGE, FULL_RANGE, FULL_RANGE,
                   ClimateParameter::span(erosions[0], erosions[1]), FULL_RANGE,
                   0.0f, "minecraft:deep_dark");
  }
};

} // namespace

FixedBiomeSource::FixedBiomeSource(std::string biomeId)
    : biomeId_(std::move(biomeId)) {}

std::vector<std::string> FixedBiomeSource::possibleBiomes() const {
  return {biomeId_};
}

std::string_view
FixedBiomeSource::getNoiseBiome(int /*quartX*/, int /*quartY*/, int /*quartZ*/,
                                const ClimateSampler & /*sampler*/) const {
  return biomeId_;
}

std::vector<std::pair<ClimateParameterPoint, std::string>>
OverworldBiomeBuilder::buildEntries() {
  std::vector<std::pair<ClimateParameterPoint, std::string>> out;
  OverworldBuilderImpl impl;
  impl.addBiomes(out);
  return out;
}

std::vector<ClimateParameterPoint> OverworldBiomeBuilder::spawnTarget() {
  const ClimateParameter fullRange = ClimateParameter::span(-1.0f, 1.0f);
  const ClimateParameter inlandToFull =
      ClimateParameter::span(ClimateParameter::span(-0.11f, 0.55f), fullRange);
  const ClimateParameter surfaceDepth = ClimateParameter::point(0.0f);
  return {
      ClimateParameterPoint::make(fullRange, fullRange, inlandToFull, fullRange,
                                  surfaceDepth,
                                  ClimateParameter::span(-1.0f, -0.16f), 0.0f),
      ClimateParameterPoint::make(fullRange, fullRange, inlandToFull, fullRange,
                                  surfaceDepth,
                                  ClimateParameter::span(0.16f, 1.0f), 0.0f),
  };
}

MultiNoiseBiomeSource::MultiNoiseBiomeSource(
    ClimateParameterList<std::string> parameters)
    : parameters_(std::move(parameters)) {}

MultiNoiseBiomeSource MultiNoiseBiomeSource::createOverworld() {
  return createFromList(OverworldBiomeBuilder::buildEntries());
}

MultiNoiseBiomeSource MultiNoiseBiomeSource::createNether() {
  return createFromList({
      {ClimateParameterPoint::make(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
       "minecraft:nether_wastes"},
      {ClimateParameterPoint::make(0.0f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
       "minecraft:soul_sand_valley"},
      {ClimateParameterPoint::make(0.4f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f),
       "minecraft:crimson_forest"},
      {ClimateParameterPoint::make(0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.375f),
       "minecraft:warped_forest"},
      {ClimateParameterPoint::make(-0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.175f),
       "minecraft:basalt_deltas"},
  });
}

MultiNoiseBiomeSource MultiNoiseBiomeSource::createFromList(
    std::vector<std::pair<ClimateParameterPoint, std::string>> list) {
  return MultiNoiseBiomeSource(ClimateParameterList<std::string>(std::move(list)));
}

std::vector<std::string> MultiNoiseBiomeSource::possibleBiomes() const {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  for (const auto &entry : parameters_.entries()) {
    if (seen.insert(entry.second).second)
      out.push_back(entry.second);
  }
  return out;
}

std::string_view
MultiNoiseBiomeSource::getNoiseBiome(int quartX, int quartY, int quartZ,
                                     const ClimateSampler &sampler) const {
  return getNoiseBiome(sampler.sample(quartX, quartY, quartZ));
}

const std::string &
MultiNoiseBiomeSource::getNoiseBiome(const ClimateTargetPoint &target) const {
  return parameters_.findValue(target);
}

std::vector<std::string> TheEndBiomeSource::possibleBiomes() const {
  return {"minecraft:the_end", "minecraft:end_highlands",
          "minecraft:end_midlands", "minecraft:small_end_islands",
          "minecraft:end_barrens"};
}

std::string_view
TheEndBiomeSource::getNoiseBiome(int quartX, int quartY, int quartZ,
                                 const ClimateSampler &sampler) const {
  const int blockX = quartX * 4;
  const int blockY = quartY * 4;
  const int blockZ = quartZ * 4;
  const std::int64_t chunkX = floorDiv16(blockX);
  const std::int64_t chunkZ = floorDiv16(blockZ);
  if (chunkX * chunkX + chunkZ * chunkZ <= 4096LL) {
    return "minecraft:the_end";
  }
  const int weirdBlockX = static_cast<int>((chunkX * 2 + 1) * 8);
  const int weirdBlockZ = static_cast<int>((chunkZ * 2 + 1) * 8);
  const double heightValue =
      sampler.erosion ? sampler.erosion->compute(weirdBlockX, blockY, weirdBlockZ)
                      : 0.0;
  if (heightValue > 0.25)
    return "minecraft:end_highlands";
  if (heightValue >= -0.0625)
    return "minecraft:end_midlands";
  return heightValue < -0.21875 ? "minecraft:small_end_islands"
                                : "minecraft:end_barrens";
}

std::int64_t BiomeManager::obfuscateSeed(std::int64_t worldSeed) noexcept {
  const std::uint64_t u = static_cast<std::uint64_t>(worldSeed);
  std::array<std::uint8_t, 8> leBytes{};
  for (int i = 0; i < 8; ++i)
    leBytes[i] = static_cast<std::uint8_t>((u >> (i * 8)) & 0xFFU);
  const auto digest = sha256(leBytes.data(), leBytes.size());
  std::uint64_t res = 0;
  for (int i = 0; i < 8; ++i)
    res |= static_cast<std::uint64_t>(digest[i]) << (i * 8);
  return static_cast<std::int64_t>(res);
}

double BiomeManager::getFiddledDistance(std::int64_t seed, int xRandom,
                                        int yRandom, int zRandom,
                                        double distanceX, double distanceY,
                                        double distanceZ) noexcept {
  std::int64_t rval = lcgNext(seed, xRandom);
  rval = lcgNext(rval, yRandom);
  rval = lcgNext(rval, zRandom);
  rval = lcgNext(rval, xRandom);
  rval = lcgNext(rval, yRandom);
  rval = lcgNext(rval, zRandom);
  const double fiddleX = biomeFiddle(rval);
  rval = lcgNext(rval, seed);
  const double fiddleY = biomeFiddle(rval);
  rval = lcgNext(rval, seed);
  const double fiddleZ = biomeFiddle(rval);
  const double dz = distanceZ + fiddleZ;
  const double dy = distanceY + fiddleY;
  const double dx = distanceX + fiddleX;
  return dz * dz + dy * dy + dx * dx;
}

int BiomeManager::pickCornerIndex(std::int64_t biomeZoomSeed, int parentX,
                                  int parentY, int parentZ, int absX, int absY,
                                  int absZ) noexcept {
  const double fractX = static_cast<double>(mod4(absX)) / 4.0;
  const double fractY = static_cast<double>(mod4(absY)) / 4.0;
  const double fractZ = static_cast<double>(mod4(absZ)) / 4.0;

  int minI = 0;
  double minFiddledDistance = std::numeric_limits<double>::infinity();
  for (int i = 0; i < 8; ++i) {
    const bool xEven = (i & 4) == 0;
    const bool yEven = (i & 2) == 0;
    const bool zEven = (i & 1) == 0;
    const int cornerX = xEven ? parentX : parentX + 1;
    const int cornerY = yEven ? parentY : parentY + 1;
    const int cornerZ = zEven ? parentZ : parentZ + 1;
    const double dx = xEven ? fractX : fractX - 1.0;
    const double dy = yEven ? fractY : fractY - 1.0;
    const double dz = zEven ? fractZ : fractZ - 1.0;
    const double next =
        getFiddledDistance(biomeZoomSeed, cornerX, cornerY, cornerZ, dx, dy, dz);
    if (minFiddledDistance > next) {
      minI = i;
      minFiddledDistance = next;
    }
  }
  return minI;
}

std::string_view BiomeManager::getBiome(int blockX, int blockY, int blockZ,
                                        const NoiseBiomeReader &source) const {
  const int absX = blockX - 2;
  const int absY = blockY - 2;
  const int absZ = blockZ - 2;
  const int parentX = floorDiv4(absX);
  const int parentY = floorDiv4(absY);
  const int parentZ = floorDiv4(absZ);
  const int minI = pickCornerIndex(biomeZoomSeed_, parentX, parentY, parentZ,
                                   absX, absY, absZ);
  const int biomeX = (minI & 4) == 0 ? parentX : parentX + 1;
  const int biomeY = (minI & 2) == 0 ? parentY : parentY + 1;
  const int biomeZ = (minI & 1) == 0 ? parentZ : parentZ + 1;
  return source(biomeX, biomeY, biomeZ);
}

namespace {

const PerlinSimplexNoise &biomeTemperatureNoise() {
  static const PerlinSimplexNoise kNoise = [] {
    LegacyRandomSource rng(1234LL);
    static constexpr std::array<int, 1> kOctaves{0};
    return PerlinSimplexNoise(rng, kOctaves);
  }();
  return kNoise;
}

const PerlinSimplexNoise &biomeFrozenTemperatureNoise() {
  static const PerlinSimplexNoise kNoise = [] {
    LegacyRandomSource rng(3456LL);
    static constexpr std::array<int, 3> kOctaves{-2, -1, 0};
    return PerlinSimplexNoise(rng, kOctaves);
  }();
  return kNoise;
}

const PerlinSimplexNoise &biomeInfoNoise() {
  static const PerlinSimplexNoise kNoise = [] {
    LegacyRandomSource rng(2345LL);
    static constexpr std::array<int, 1> kOctaves{0};
    return PerlinSimplexNoise(rng, kOctaves);
  }();
  return kNoise;
}

float modifyFrozenTemperature(int blockX, int blockZ,
                              float baseTemperature) noexcept {
  const double groundLarge =
      biomeFrozenTemperatureNoise().getValue(
          static_cast<double>(blockX) * 0.05,
          static_cast<double>(blockZ) * 0.05, false) *
      7.0;
  const double groundEdge = biomeInfoNoise().getValue(
      static_cast<double>(blockX) * 0.2, static_cast<double>(blockZ) * 0.2,
      false);
  if (groundLarge + groundEdge < 0.3) {
    const double groundSmall = biomeInfoNoise().getValue(
        static_cast<double>(blockX) * 0.09, static_cast<double>(blockZ) * 0.09,
        false);
    if (groundSmall < 0.8)
      return 0.2f;
  }
  return baseTemperature;
}

} // namespace

float BiomeClimateProperties::getHeightAdjustedTemperature(
    int blockX, int blockY, int blockZ, int seaLevel) const noexcept {
  const float adjusted =
      temperatureModifier == BiomeTemperatureModifier::Frozen
          ? modifyFrozenTemperature(blockX, blockZ, temperature)
          : temperature;
  const int snowLevel = seaLevel + 17;
  if (blockY > snowLevel) {
    const float v = static_cast<float>(
        biomeTemperatureNoise().getValue(
            static_cast<double>(static_cast<float>(blockX) / 8.0f),
            static_cast<double>(static_cast<float>(blockZ) / 8.0f), false) *
        8.0);
    return adjusted -
           (v + static_cast<float>(blockY) - static_cast<float>(snowLevel)) *
               0.05f / 40.0f;
  }
  return adjusted;
}

bool BiomeClimateProperties::warmEnoughToRain(int blockX, int blockY,
                                              int blockZ,
                                              int seaLevel) const noexcept {
  return getHeightAdjustedTemperature(blockX, blockY, blockZ, seaLevel) >=
         0.15f;
}

bool BiomeClimateProperties::coldEnoughToSnow(int blockX, int blockY,
                                              int blockZ,
                                              int seaLevel) const noexcept {
  return !warmEnoughToRain(blockX, blockY, blockZ, seaLevel);
}

bool BiomeClimateProperties::shouldMeltFrozenOceanIcebergSlightly(
    int blockX, int blockY, int blockZ, int seaLevel) const noexcept {
  return getHeightAdjustedTemperature(blockX, blockY, blockZ, seaLevel) > 0.1f;
}

void fillBiomesFromNoise(ProtoChunk &chunk, const BiomeSource &biomeSource,
                         const ClimateSampler &sampler) {
  const int chunkStartQuartX = chunk.pos().x * 4;
  const int chunkStartQuartZ = chunk.pos().z * 4;
  for (int secIndex = 0; secIndex < chunk.sectionCount(); ++secIndex) {
    const int secStartQuartY = (chunk.minSectionY() + secIndex) * 4;
    auto biomes = chunk.biomes(secIndex, chunk.defaultBiome());
    for (int qy = 0; qy < 4; ++qy) {
      for (int qx = 0; qx < 4; ++qx) {
        for (int qz = 0; qz < 4; ++qz) {
          const std::string_view biomeId = biomeSource.getNoiseBiome(
              chunkStartQuartX + qx, secStartQuartY + qy, chunkStartQuartZ + qz,
              sampler);
          biomes.set(qx, qy, qz, biomeId);
        }
      }
    }
  }
}

std::optional<BiomeSearchResult> BiomeSource::findBiomeHorizontal(
    int originX, int originY, int originZ, int searchRadius,
    const std::function<bool(std::string_view)> &allowed, RandomSource &rng,
    const ClimateSampler &sampler) const {
  return findBiomeHorizontal(originX, originY, originZ, searchRadius, 1,
                             allowed, rng, false, sampler);
}

std::optional<BiomeSearchResult> BiomeSource::findBiomeHorizontal(
    int originX, int originY, int originZ, int searchRadius, int skipSteps,
    const std::function<bool(std::string_view)> &allowed, RandomSource &rng,
    bool findClosest, const ClimateSampler &sampler) const {
  const int noiseCenterX = originX >> 2;
  const int noiseCenterZ = originZ >> 2;
  const int noiseRadius = searchRadius >> 2;
  const int noiseY = originY >> 2;
  std::optional<BiomeSearchResult> result;
  int found = 0;
  const int startRadius = findClosest ? 0 : noiseRadius;
  for (int currentRadius = startRadius; currentRadius <= noiseRadius;
       currentRadius += skipSteps) {
    for (int z = -currentRadius; z <= currentRadius; z += skipSteps) {
      const bool zEdge = std::abs(z) == currentRadius;
      for (int x = -currentRadius; x <= currentRadius; x += skipSteps) {
        if (findClosest) {
          const bool xEdge = std::abs(x) == currentRadius;
          if (!xEdge && !zEdge)
            continue;
        }
        const int noiseX = noiseCenterX + x;
        const int noiseZ = noiseCenterZ + z;
        const std::string_view biome =
            getNoiseBiome(noiseX, noiseY, noiseZ, sampler);
        if (allowed(biome)) {
          if (!result.has_value() || rng.nextInt(found + 1) == 0) {
            Pos p{noiseX << 2, originY, noiseZ << 2};
            result = BiomeSearchResult{p, std::string(biome)};
            if (findClosest)
              return result;
          }
          ++found;
        }
      }
    }
  }
  return result;
}

} // namespace redstone::worldgen
