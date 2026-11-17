#include "redstone/worldgen/noise.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace redstone::worldgen {
namespace {

std::pair<int, std::vector<double>>
makeAmplitudesFromOctaveSet(std::span<const int> octaves) {
  if (octaves.empty())
    throw std::invalid_argument("Need some octaves!");
  std::set<int> sorted(octaves.begin(), octaves.end());
  const int lowFreqOctaves = -(*sorted.begin());
  const int highFreqOctaves = *sorted.rbegin();
  const int count = lowFreqOctaves + highFreqOctaves + 1;
  if (count < 1)
    throw std::invalid_argument("Total number of octaves needs to be >= 1");
  std::vector<double> amps(static_cast<std::size_t>(count), 0.0);
  for (int octave : sorted)
    amps[static_cast<std::size_t>(octave + lowFreqOctaves)] = 1.0;
  return {-lowFreqOctaves, std::move(amps)};
}

} // namespace

SimplexNoise::SimplexNoise(RandomSource &random) {
  xo_ = random.nextDouble() * 256.0;
  yo_ = random.nextDouble() * 256.0;
  zo_ = random.nextDouble() * 256.0;
  for (int i = 0; i < 256; ++i)
    p_[static_cast<std::size_t>(i)] = i;
  for (int i = 0; i < 256; ++i) {
    const int offset = random.nextInt(256 - i);
    const int tmp = p_[static_cast<std::size_t>(i)];
    p_[static_cast<std::size_t>(i)] = p_[static_cast<std::size_t>(offset + i)];
    p_[static_cast<std::size_t>(offset + i)] = tmp;
  }
}

double SimplexNoise::cornerNoise3D(int index, double x, double y, double z,
                                   double base) const noexcept {
  double t0 = base - x * x - y * y - z * z;
  if (t0 < 0.0)
    return 0.0;
  t0 *= t0;
  return t0 * t0 * dot(kGradient[static_cast<std::size_t>(index)], x, y, z);
}

double SimplexNoise::getValue(double xin, double yin) const noexcept {
  static const double kSqrt3 = std::sqrt(3.0);
  static const double kF2 = 0.5 * (kSqrt3 - 1.0);
  static const double kG2 = (3.0 - kSqrt3) / 6.0;

  const double s = (xin + yin) * kF2;
  const int i = worldgen_math::floorDivInt(xin + s);
  const int j = worldgen_math::floorDivInt(yin + s);
  const double t = static_cast<double>(i + j) * kG2;
  const double X0 = static_cast<double>(i) - t;
  const double Y0 = static_cast<double>(j) - t;
  const double x0 = xin - X0;
  const double y0 = yin - Y0;
  const int i1 = x0 > y0 ? 1 : 0;
  const int j1 = x0 > y0 ? 0 : 1;
  const double x1 = x0 - static_cast<double>(i1) + kG2;
  const double y1 = y0 - static_cast<double>(j1) + kG2;
  const double x2 = x0 - 1.0 + 2.0 * kG2;
  const double y2 = y0 - 1.0 + 2.0 * kG2;
  const int ii = i & 0xFF;
  const int jj = j & 0xFF;
  const int gi0 = perm(ii + perm(jj)) % 12;
  const int gi1 = perm(ii + i1 + perm(jj + j1)) % 12;
  const int gi2 = perm(ii + 1 + perm(jj + 1)) % 12;
  const double n0 = cornerNoise3D(gi0, x0, y0, 0.0, 0.5);
  const double n1 = cornerNoise3D(gi1, x1, y1, 0.0, 0.5);
  const double n2 = cornerNoise3D(gi2, x2, y2, 0.0, 0.5);
  return 70.0 * (n0 + n1 + n2);
}

double SimplexNoise::getValue(double xin, double yin, double zin) const noexcept {
  constexpr double F3 = 0.3333333333333333;
  constexpr double G3 = 0.16666666666666666;
  const double s = (xin + yin + zin) * F3;
  const int i = worldgen_math::floorDivInt(xin + s);
  const int j = worldgen_math::floorDivInt(yin + s);
  const int k = worldgen_math::floorDivInt(zin + s);
  const double t = static_cast<double>(i + j + k) * G3;
  const double x0 = xin - (static_cast<double>(i) - t);
  const double y0 = yin - (static_cast<double>(j) - t);
  const double z0 = zin - (static_cast<double>(k) - t);

  int i1 = 0, j1 = 0, k1 = 0, i2 = 0, j2 = 0, k2 = 0;
  if (x0 >= y0) {
    if (y0 >= z0) {
      i1 = 1;
      i2 = 1;
      j2 = 1;
    } else if (x0 >= z0) {
      i1 = 1;
      i2 = 1;
      k2 = 1;
    } else {
      k1 = 1;
      i2 = 1;
      k2 = 1;
    }
  } else if (y0 < z0) {
    k1 = 1;
    j2 = 1;
    k2 = 1;
  } else if (x0 < z0) {
    j1 = 1;
    j2 = 1;
    k2 = 1;
  } else {
    j1 = 1;
    i2 = 1;
    j2 = 1;
  }

  const double x1 = x0 - static_cast<double>(i1) + G3;
  const double y1 = y0 - static_cast<double>(j1) + G3;
  const double z1 = z0 - static_cast<double>(k1) + G3;
  const double x2 = x0 - static_cast<double>(i2) + 0.3333333333333333;
  const double y2 = y0 - static_cast<double>(j2) + 0.3333333333333333;
  const double z2 = z0 - static_cast<double>(k2) + 0.3333333333333333;
  const double x3 = x0 - 1.0 + 0.5;
  const double y3 = y0 - 1.0 + 0.5;
  const double z3 = z0 - 1.0 + 0.5;

  const int ii = i & 0xFF;
  const int jj = j & 0xFF;
  const int kk = k & 0xFF;
  const int gi0 = perm(ii + perm(jj + perm(kk))) % 12;
  const int gi1 = perm(ii + i1 + perm(jj + j1 + perm(kk + k1))) % 12;
  const int gi2 = perm(ii + i2 + perm(jj + j2 + perm(kk + k2))) % 12;
  const int gi3 = perm(ii + 1 + perm(jj + 1 + perm(kk + 1))) % 12;
  const double n0 = cornerNoise3D(gi0, x0, y0, z0, 0.6);
  const double n1 = cornerNoise3D(gi1, x1, y1, z1, 0.6);
  const double n2 = cornerNoise3D(gi2, x2, y2, z2, 0.6);
  const double n3 = cornerNoise3D(gi3, x3, y3, z3, 0.6);
  return 32.0 * (n0 + n1 + n2 + n3);
}

PerlinSimplexNoise::PerlinSimplexNoise(RandomSource &random,
                                       std::span<const int> octaves) {
  if (octaves.empty())
    throw std::invalid_argument("PerlinSimplexNoise needs some octaves!");
  std::set<int> sorted(octaves.begin(), octaves.end());
  const int lowFreqOctaves = -(*sorted.begin());
  const int highFreqOctaves = *sorted.rbegin();
  const int count = lowFreqOctaves + highFreqOctaves + 1;
  if (count < 1)
    throw std::invalid_argument("Total number of octaves needs to be >= 1");

  SimplexNoise zeroOctave(random);
  const int zeroOctaveIndex = highFreqOctaves;
  noiseLevels_.resize(static_cast<std::size_t>(count));
  if (highFreqOctaves >= 0 && highFreqOctaves < count && sorted.contains(0))
    noiseLevels_[static_cast<std::size_t>(highFreqOctaves)].emplace(zeroOctave);

  for (int i = highFreqOctaves + 1; i < count; ++i) {
    if (i >= 0 && sorted.contains(zeroOctaveIndex - i))
      noiseLevels_[static_cast<std::size_t>(i)].emplace(random);
    else
      random.consumeCount(262);
  }

  if (highFreqOctaves > 0) {
    const std::int64_t positiveSeed = static_cast<std::int64_t>(
        zeroOctave.getValue(zeroOctave.xo(), zeroOctave.yo(), zeroOctave.zo()) *
        9.223372E18F);
    LegacyRandomSource highFreqRandom(positiveSeed);
    for (int i = zeroOctaveIndex - 1; i >= 0; --i) {
      if (i < count && sorted.contains(zeroOctaveIndex - i))
        noiseLevels_[static_cast<std::size_t>(i)].emplace(highFreqRandom);
      else
        highFreqRandom.consumeCount(262);
    }
  }

  highestFreqInputFactor_ = std::pow(2.0, static_cast<double>(highFreqOctaves));
  highestFreqValueFactor_ =
      1.0 / (std::pow(2.0, static_cast<double>(count)) - 1.0);
}

double PerlinSimplexNoise::getValue(double x, double y,
                                    bool useNoiseStart) const noexcept {
  double value = 0.0;
  double factor = highestFreqInputFactor_;
  double valueFactor = highestFreqValueFactor_;
  for (const auto &level : noiseLevels_) {
    if (level) {
      value += level->getValue(x * factor + (useNoiseStart ? level->xo() : 0.0),
                               y * factor + (useNoiseStart ? level->yo() : 0.0)) *
               valueFactor;
    }
    factor /= 2.0;
    valueFactor *= 2.0;
  }
  return value;
}

ImprovedNoise::ImprovedNoise(RandomSource &random) {
  xo_ = random.nextDouble() * 256.0;
  yo_ = random.nextDouble() * 256.0;
  zo_ = random.nextDouble() * 256.0;
  for (int i = 0; i < 256; ++i)
    p_[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
  for (int i = 0; i < 256; ++i) {
    const int offset = random.nextInt(256 - i);
    std::swap(p_[static_cast<std::size_t>(i)],
              p_[static_cast<std::size_t>(i + offset)]);
  }
}

double ImprovedNoise::noise(double _x, double _y, double _z, double yScale,
                            double yFudge) const noexcept {
  const double x = _x + xo_;
  const double y = _y + yo_;
  const double z = _z + zo_;
  const int xf = worldgen_math::floorDivInt(x);
  const int yf = worldgen_math::floorDivInt(y);
  const int zf = worldgen_math::floorDivInt(z);
  const double xr = x - static_cast<double>(xf);
  const double yr = y - static_cast<double>(yf);
  const double zr = z - static_cast<double>(zf);
  double yrFudge = 0.0;
  if (yScale != 0.0) {
    const double fudgeLimit = (yFudge >= 0.0 && yFudge < yr) ? yFudge : yr;
    yrFudge = static_cast<double>(worldgen_math::floorDivInt(
                  fudgeLimit / yScale + static_cast<double>(1.0E-7F))) *
              yScale;
  }
  return sampleAndLerp(xf, yf, zf, xr, yr - yrFudge, zr, yr);
}

double ImprovedNoise::noiseWithDerivative(
    double _x, double _y, double _z,
    std::array<double, 3> &derivativeOut) const noexcept {
  const double x = _x + xo_;
  const double y = _y + yo_;
  const double z = _z + zo_;
  const int xf = worldgen_math::floorDivInt(x);
  const int yf = worldgen_math::floorDivInt(y);
  const int zf = worldgen_math::floorDivInt(z);
  const double xr = x - static_cast<double>(xf);
  const double yr = y - static_cast<double>(yf);
  const double zr = z - static_cast<double>(zf);
  return sampleWithDerivative(xf, yf, zf, xr, yr, zr, derivativeOut);
}

double ImprovedNoise::sampleAndLerp(int x, int y, int z, double xr, double yr,
                                    double zr,
                                    double yrOriginal) const noexcept {
  static constexpr double kGradDouble[16][3] = {
      {1.0, 1.0, 0.0},  {-1.0, 1.0, 0.0},  {1.0, -1.0, 0.0}, {-1.0, -1.0, 0.0},
      {1.0, 0.0, 1.0},  {-1.0, 0.0, 1.0},  {1.0, 0.0, -1.0}, {-1.0, 0.0, -1.0},
      {0.0, 1.0, 1.0},  {0.0, -1.0, 1.0},  {0.0, 1.0, -1.0}, {0.0, -1.0, -1.0},
      {1.0, 1.0, 0.0},  {0.0, -1.0, 1.0},  {-1.0, 1.0, 0.0}, {0.0, -1.0, -1.0},
  };
  auto gradDot = [](int hash, double dx, double dy, double dz) noexcept {
    const auto &g = kGradDouble[hash & 15];
    return g[0] * dx + g[1] * dy + g[2] * dz;
  };
  const int x0 = perm(x);
  const int x1 = perm(x + 1);
  const int xy00 = perm(x0 + y);
  const int xy01 = perm(x0 + y + 1);
  const int xy10 = perm(x1 + y);
  const int xy11 = perm(x1 + y + 1);
  const double d000 = gradDot(perm(xy00 + z), xr, yr, zr);
  const double d100 = gradDot(perm(xy10 + z), xr - 1.0, yr, zr);
  const double d010 = gradDot(perm(xy01 + z), xr, yr - 1.0, zr);
  const double d110 = gradDot(perm(xy11 + z), xr - 1.0, yr - 1.0, zr);
  const double d001 = gradDot(perm(xy00 + z + 1), xr, yr, zr - 1.0);
  const double d101 = gradDot(perm(xy10 + z + 1), xr - 1.0, yr, zr - 1.0);
  const double d011 = gradDot(perm(xy01 + z + 1), xr, yr - 1.0, zr - 1.0);
  const double d111 = gradDot(perm(xy11 + z + 1), xr - 1.0, yr - 1.0, zr - 1.0);
  const double xAlpha = worldgen_math::smoothstep(xr);
  const double yAlpha = worldgen_math::smoothstep(yrOriginal);
  const double zAlpha = worldgen_math::smoothstep(zr);
  return worldgen_math::lerp3(xAlpha, yAlpha, zAlpha, d000, d100, d010, d110,
                              d001, d101, d011, d111);
}

double ImprovedNoise::sampleWithDerivative(
    int x, int y, int z, double xr, double yr, double zr,
    std::array<double, 3> &derivativeOut) const noexcept {
  const int x0 = perm(x);
  const int x1 = perm(x + 1);
  const int xy00 = perm(x0 + y);
  const int xy01 = perm(x0 + y + 1);
  const int xy10 = perm(x1 + y);
  const int xy11 = perm(x1 + y + 1);
  const auto &g000 =
      SimplexNoise::kGradient[static_cast<std::size_t>(perm(xy00 + z) & 15)];
  const auto &g100 =
      SimplexNoise::kGradient[static_cast<std::size_t>(perm(xy10 + z) & 15)];
  const auto &g010 =
      SimplexNoise::kGradient[static_cast<std::size_t>(perm(xy01 + z) & 15)];
  const auto &g110 =
      SimplexNoise::kGradient[static_cast<std::size_t>(perm(xy11 + z) & 15)];
  const auto &g001 = SimplexNoise::kGradient[static_cast<std::size_t>(
      perm(xy00 + z + 1) & 15)];
  const auto &g101 = SimplexNoise::kGradient[static_cast<std::size_t>(
      perm(xy10 + z + 1) & 15)];
  const auto &g011 = SimplexNoise::kGradient[static_cast<std::size_t>(
      perm(xy01 + z + 1) & 15)];
  const auto &g111 = SimplexNoise::kGradient[static_cast<std::size_t>(
      perm(xy11 + z + 1) & 15)];

  const double d000 = SimplexNoise::dot(g000, xr, yr, zr);
  const double d100 = SimplexNoise::dot(g100, xr - 1.0, yr, zr);
  const double d010 = SimplexNoise::dot(g010, xr, yr - 1.0, zr);
  const double d110 = SimplexNoise::dot(g110, xr - 1.0, yr - 1.0, zr);
  const double d001 = SimplexNoise::dot(g001, xr, yr, zr - 1.0);
  const double d101 = SimplexNoise::dot(g101, xr - 1.0, yr, zr - 1.0);
  const double d011 = SimplexNoise::dot(g011, xr, yr - 1.0, zr - 1.0);
  const double d111 = SimplexNoise::dot(g111, xr - 1.0, yr - 1.0, zr - 1.0);

  const double xAlpha = worldgen_math::smoothstep(xr);
  const double yAlpha = worldgen_math::smoothstep(yr);
  const double zAlpha = worldgen_math::smoothstep(zr);
  const double d1x = worldgen_math::lerp3(xAlpha, yAlpha, zAlpha, g000[0],
                                          g100[0], g010[0], g110[0], g001[0],
                                          g101[0], g011[0], g111[0]);
  const double d1y = worldgen_math::lerp3(xAlpha, yAlpha, zAlpha, g000[1],
                                          g100[1], g010[1], g110[1], g001[1],
                                          g101[1], g011[1], g111[1]);
  const double d1z = worldgen_math::lerp3(xAlpha, yAlpha, zAlpha, g000[2],
                                          g100[2], g010[2], g110[2], g001[2],
                                          g101[2], g011[2], g111[2]);
  const double d2x = worldgen_math::lerp2(yAlpha, zAlpha, d100 - d000,
                                          d110 - d010, d101 - d001, d111 - d011);
  const double d2y = worldgen_math::lerp2(zAlpha, xAlpha, d010 - d000,
                                          d011 - d001, d110 - d100, d111 - d101);
  const double d2z = worldgen_math::lerp2(xAlpha, yAlpha, d001 - d000,
                                          d101 - d100, d011 - d010, d111 - d110);
  derivativeOut[0] += d1x + worldgen_math::smoothstepDerivative(xr) * d2x;
  derivativeOut[1] += d1y + worldgen_math::smoothstepDerivative(yr) * d2y;
  derivativeOut[2] += d1z + worldgen_math::smoothstepDerivative(zr) * d2z;
  return worldgen_math::lerp3(xAlpha, yAlpha, zAlpha, d000, d100, d010, d110,
                              d001, d101, d011, d111);
}

PerlinNoise::PerlinNoise(RandomSource &random, int firstOctave,
                         std::vector<double> amplitudes,
                         bool useNewInitialization)
    : firstOctave_(firstOctave), amplitudes_(std::move(amplitudes)) {
  const int octaves = static_cast<int>(amplitudes_.size());
  const int zeroOctaveIndex = -firstOctave_;
  noiseLevels_.resize(static_cast<std::size_t>(octaves));
  if (useNewInitialization) {
    PositionalRandomFactory positional = random.forkPositional();
    char nameBuf[32] = "octave_";
    for (int i = 0; i < octaves; ++i) {
      if (amplitudes_[static_cast<std::size_t>(i)] != 0.0) {
        const int octave = firstOctave_ + i;
        auto [ptr, ec] =
            std::to_chars(nameBuf + 7, nameBuf + sizeof(nameBuf), octave);
        (void)ec;
        AnyRandomSource rng = positional.fromHashOf(
            std::string_view(nameBuf, static_cast<std::size_t>(ptr - nameBuf)));
        noiseLevels_[static_cast<std::size_t>(i)].emplace(rng);
      }
    }
  } else {
    ImprovedNoise zeroOctave(random);
    if (zeroOctaveIndex >= 0 && zeroOctaveIndex < octaves) {
      if (amplitudes_[static_cast<std::size_t>(zeroOctaveIndex)] != 0.0) {
        noiseLevels_[static_cast<std::size_t>(zeroOctaveIndex)] = zeroOctave;
      }
    }
    for (int ix = zeroOctaveIndex - 1; ix >= 0; --ix) {
      if (ix < octaves && amplitudes_[static_cast<std::size_t>(ix)] != 0.0) {
        noiseLevels_[static_cast<std::size_t>(ix)].emplace(random);
      } else {
        random.consumeCount(262);
      }
    }
    if (zeroOctaveIndex < octaves - 1) {
      throw std::invalid_argument(
          "Positive octaves are disabled in legacy PerlinNoise");
    }
  }
  lowestFreqInputFactor_ = std::pow(2.0, -zeroOctaveIndex);
  lowestFreqValueFactor_ =
      std::pow(2.0, octaves - 1) / (std::pow(2.0, octaves) - 1.0);
  maxValue_ = edgeValue(2.0);
}

PerlinNoise PerlinNoise::create(RandomSource &random, int firstOctave,
                                std::vector<double> amplitudes) {
  return PerlinNoise(random, firstOctave, std::move(amplitudes), true);
}

PerlinNoise PerlinNoise::create(RandomSource &random,
                                std::span<const int> octaves) {
  auto [first, amps] = makeAmplitudesFromOctaveSet(octaves);
  return PerlinNoise(random, first, std::move(amps), true);
}

PerlinNoise PerlinNoise::createLegacyForBlendedNoise(
    RandomSource &random, std::span<const int> octaves) {
  auto [first, amps] = makeAmplitudesFromOctaveSet(octaves);
  return PerlinNoise(random, first, std::move(amps), false);
}

PerlinNoise PerlinNoise::createLegacyForNetherBiome(
    RandomSource &random, int firstOctave, std::vector<double> amplitudes) {
  return PerlinNoise(random, firstOctave, std::move(amplitudes), false);
}

double PerlinNoise::getValue(double x, double y, double z, double yScale,
                             double yFudge) const noexcept {
  double value = 0.0;
  double factor = lowestFreqInputFactor_;
  double valueFactor = lowestFreqValueFactor_;
  for (std::size_t i = 0; i < noiseLevels_.size(); ++i) {
    if (noiseLevels_[i]) {
      const double nv =
          noiseLevels_[i]->noise(wrap(x * factor), wrap(y * factor),
                                 wrap(z * factor), yScale * factor,
                                 yFudge * factor);
      value += amplitudes_[i] * nv * valueFactor;
    }
    factor *= 2.0;
    valueFactor /= 2.0;
  }
  return value;
}

double PerlinNoise::edgeValue(double noiseValue) const noexcept {
  double value = 0.0;
  double valueFactor = lowestFreqValueFactor_;
  for (std::size_t i = 0; i < noiseLevels_.size(); ++i) {
    if (noiseLevels_[i])
      value += amplitudes_[i] * noiseValue * valueFactor;
    valueFactor /= 2.0;
  }
  return value;
}

const ImprovedNoise *PerlinNoise::getOctaveNoise(int i) const noexcept {
  const int idx = static_cast<int>(noiseLevels_.size()) - 1 - i;
  if (idx < 0 || idx >= static_cast<int>(noiseLevels_.size()))
    return nullptr;
  return noiseLevels_[static_cast<std::size_t>(idx)]
             ? &*noiseLevels_[static_cast<std::size_t>(idx)]
             : nullptr;
}

NormalNoise::NormalNoise(RandomSource &random,
                         const NoiseParameters &parameters,
                         bool useNewInitialization)
    : first_(useNewInitialization
                 ? PerlinNoise::create(random, parameters.firstOctave,
                                       parameters.amplitudes)
                 : PerlinNoise::createLegacyForNetherBiome(
                       random, parameters.firstOctave, parameters.amplitudes)),
      second_(useNewInitialization
                  ? PerlinNoise::create(random, parameters.firstOctave,
                                        parameters.amplitudes)
                  : PerlinNoise::createLegacyForNetherBiome(
                        random, parameters.firstOctave, parameters.amplitudes)),
      parameters_(parameters) {
  int minOctave = std::numeric_limits<int>::max();
  int maxOctave = std::numeric_limits<int>::min();
  for (int i = 0; i < static_cast<int>(parameters.amplitudes.size()); ++i) {
    if (parameters.amplitudes[static_cast<std::size_t>(i)] != 0.0) {
      minOctave = std::min(minOctave, i);
      maxOctave = std::max(maxOctave, i);
    }
  }
  valueFactor_ =
      0.16666666666666666 / expectedDeviation(maxOctave - minOctave);
  maxValue_ = (first_.maxValue() + second_.maxValue()) * valueFactor_;
}

NormalNoise NormalNoise::create(RandomSource &random,
                                const NoiseParameters &parameters) {
  return NormalNoise(random, parameters, true);
}

NormalNoise
NormalNoise::createLegacyNetherBiome(RandomSource &random,
                                   const NoiseParameters &parameters) {
  return NormalNoise(random, parameters, false);
}

double NormalNoise::getValue(double x, double y, double z) const noexcept {
  const double x2 = x * kInputFactor;
  const double y2 = y * kInputFactor;
  const double z2 = z * kInputFactor;
  return (first_.getValue(x, y, z) + second_.getValue(x2, y2, z2)) *
         valueFactor_;
}

namespace {

PerlinNoise buildBlendedOctaves(RandomSource &random, int minOctave,
                                int maxOctave) {
  std::vector<int> octaves;
  octaves.reserve(static_cast<std::size_t>(maxOctave - minOctave + 1));
  for (int o = minOctave; o <= maxOctave; ++o)
    octaves.push_back(o);
  return PerlinNoise::createLegacyForBlendedNoise(random, octaves);
}

} // namespace

BlendedNoise::BlendedNoise(RandomSource &random, double xzScale, double yScale,
                           double xzFactor, double yFactor,
                           double smearScaleMultiplier)
    : minLimitNoise_(buildBlendedOctaves(random, -15, 0)),
      maxLimitNoise_(buildBlendedOctaves(random, -15, 0)),
      mainNoise_(buildBlendedOctaves(random, -7, 0)),
      xzMultiplier_(684.412 * xzScale), yMultiplier_(684.412 * yScale),
      xzFactor_(xzFactor), yFactor_(yFactor),
      smearScaleMultiplier_(smearScaleMultiplier),
      maxValue_(minLimitNoise_.maxBrokenValue(yMultiplier_)),
      xzScale_(xzScale), yScale_(yScale) {}

BlendedNoise BlendedNoise::createUnseeded(double xzScale, double yScale,
                                          double xzFactor, double yFactor,
                                          double smearScaleMultiplier) {
  XoroshiroRandomSource zero(0LL);
  return BlendedNoise(zero, xzScale, yScale, xzFactor, yFactor,
                      smearScaleMultiplier);
}

BlendedNoise BlendedNoise::withNewRandom(RandomSource &terrainRandom) const {
  return BlendedNoise(terrainRandom, xzScale_, yScale_, xzFactor_, yFactor_,
                      smearScaleMultiplier_);
}

double BlendedNoise::compute(int blockX, int blockY, int blockZ) const noexcept {
  const double limitX = static_cast<double>(blockX) * xzMultiplier_;
  const double limitY = static_cast<double>(blockY) * yMultiplier_;
  const double limitZ = static_cast<double>(blockZ) * xzMultiplier_;
  const double mainX = limitX / xzFactor_;
  const double mainY = limitY / yFactor_;
  const double mainZ = limitZ / xzFactor_;
  const double limitSmear = yMultiplier_ * smearScaleMultiplier_;
  const double mainSmear = limitSmear / yFactor_;

  double mainNoiseValue = 0.0;
  double pow = 1.0;
  for (int i = 0; i < 8; ++i) {
    if (const ImprovedNoise *n = mainNoise_.getOctaveNoise(i)) {
      mainNoiseValue +=
          n->noise(PerlinNoise::wrap(mainX * pow),
                   PerlinNoise::wrap(mainY * pow),
                   PerlinNoise::wrap(mainZ * pow), mainSmear * pow,
                   mainY * pow) /
          pow;
    }
    pow /= 2.0;
  }

  const double factor = (mainNoiseValue / 10.0 + 1.0) / 2.0;
  const bool isMax = factor >= 1.0;
  const bool isMin = factor <= 0.0;
  double blendMin = 0.0;
  double blendMax = 0.0;
  pow = 1.0;
  for (int i = 0; i < 16; ++i) {
    const double wx = PerlinNoise::wrap(limitX * pow);
    const double wy = PerlinNoise::wrap(limitY * pow);
    const double wz = PerlinNoise::wrap(limitZ * pow);
    const double yScalePow = limitSmear * pow;
    if (!isMax) {
      if (const ImprovedNoise *mn = minLimitNoise_.getOctaveNoise(i))
        blendMin += mn->noise(wx, wy, wz, yScalePow, limitY * pow) / pow;
    }
    if (!isMin) {
      if (const ImprovedNoise *mx = maxLimitNoise_.getOctaveNoise(i))
        blendMax += mx->noise(wx, wy, wz, yScalePow, limitY * pow) / pow;
    }
    pow /= 2.0;
  }
  return worldgen_math::clampedLerp(factor, blendMin / 512.0, blendMax / 512.0) /
         128.0;
}

} // namespace redstone::worldgen
