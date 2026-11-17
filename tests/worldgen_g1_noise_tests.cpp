#include "redstone/worldgen/noise.hpp"
#include "redstone/worldgen/random_source.hpp"

#include <bit>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

using namespace redstone::worldgen;

std::uint64_t bitsOf(double v) noexcept {
  return std::bit_cast<std::uint64_t>(v);
}

std::int32_t bitsOf(float v) noexcept {
  return std::bit_cast<std::int32_t>(v);
}

void require(bool cond, const char *msg) {
  if (!cond)
    throw std::runtime_error(msg);
}

void requireCloseBits(std::uint64_t actual, std::uint64_t expected,
                      const char *label) {
  if (actual == expected)
    return;
  const double a = std::bit_cast<double>(actual);
  const double e = std::bit_cast<double>(expected);
  const std::uint64_t ulp =
      actual > expected ? actual - expected : expected - actual;
  if (ulp > 0) {
    std::ostringstream oss;
    oss << label << " mismatch: actual=0x" << std::hex << actual
        << " (" << std::dec << a << ") expected=0x" << std::hex << expected
        << " (" << std::dec << e << ") ulp=" << ulp;
    throw std::runtime_error(oss.str());
  }
}

void testSeed123456789() {
  const std::int64_t seed = 123456789LL;
  const Seed128bit up = random_support::upgradeSeedTo128bit(seed);
  require(up.seedLo == -7278655050202467086LL &&
              up.seedHi == -3669656285675084719LL,
          "upgradeSeedTo128bit(123456789)");

  const Seed128bit md5 = random_support::seedFromHashOf("minecraft:erosion");
  require(md5.seedLo == -3448470997537165352LL &&
              md5.seedHi == 5157273775208757888LL,
          "seedFromHashOf(minecraft:erosion)");

  require(positionalSeedHash(-1234, -64, 5678) == 43301017239116LL,
          "positionalSeedHash(-1234, -64, 5678)");

  XoroshiroRandomSource xr(seed);
  require(xr.nextLong() == 3219654894476264721LL, "xoro nextLong");
  require(xr.nextInt() == 1667009048, "xoro nextInt");
  require(xr.nextInt(17) == 0, "xoro nextInt(17)");
  require(bitsOf(xr.nextFloat()) == 1057634383, "xoro nextFloat");
  requireCloseBits(bitsOf(xr.nextDouble()), 0x3fc7ef8836ff6490ULL,
                   "xoro nextDouble");
  requireCloseBits(bitsOf(xr.nextGaussian()), 0x3fd2c1d1c1ce0c97ULL,
                   "xoro nextGaussian");

  auto xpf = xr.forkPositional();
  auto xrAt = xpf.at(12345, -17, -6789);
  auto xrHash = xpf.fromHashOf("minecraft:aquifer");
  require(xrAt.nextLong() == -3996774547586249417LL, "xoro pos at");
  require(xrHash.nextLong() == 3409950315474334504LL, "xoro pos hash");

  LegacyRandomSource lr(seed);
  require(lr.nextLong() == -6197403153606331135LL, "legacy nextLong");
  require(lr.nextInt() == 1962592967, "legacy nextInt");
  require(lr.nextInt(17) == 16, "legacy nextInt(17)");
  require(bitsOf(lr.nextFloat()) == 1053290614, "legacy nextFloat");
  requireCloseBits(bitsOf(lr.nextDouble()), 0x3fcbb96f9c964030ULL,
                   "legacy nextDouble");
  requireCloseBits(bitsOf(lr.nextGaussian()), 0xbffda4ed19cc1671ULL,
                   "legacy nextGaussian");
}

struct GoldenNoiseCase {
  std::int64_t seed;
  int x;
  int y;
  int z;
  std::uint64_t nn;
  std::uint64_t bn;
  std::uint64_t s2;
  std::uint64_t s3;
  std::uint64_t lp;
};

void testAllNoiseGolden() {
  struct SeedChecks {
    std::int64_t seed;
    std::int64_t upLo, upHi, xrLong, xrPosAt, xrPosHash, lrLong;
  };
  const std::vector<SeedChecks> seedChecks{
      {123456789LL, -7278655050202467086LL, -3669656285675084719LL,
       3219654894476264721LL, -3996774547586249417LL, 3409950315474334504LL,
       -6197403153606331135LL},
      {0LL, 3847398142028685078LL, 7192185014346937746LL,
       3038984756725240190LL, 2757447101434647776LL, 7130261715983522286LL,
       -4962768465676381896LL},
      {-987654321LL, 1764412863008151955LL, 1672175165007966110LL,
       -8813114921905719051LL, 1976336148847836010LL, 6758220704970871740LL,
       -5646896605836454244LL},
  };
  for (const auto &sc : seedChecks) {
    const Seed128bit up = random_support::upgradeSeedTo128bit(sc.seed);
    require(up.seedLo == sc.upLo && up.seedHi == sc.upHi, "seed upgrade");
    XoroshiroRandomSource xr(sc.seed);
    require(xr.nextLong() == sc.xrLong, "xrLong");
    (void)xr.nextInt();
    (void)xr.nextInt(17);
    (void)xr.nextFloat();
    (void)xr.nextDouble();
    (void)xr.nextGaussian();
    auto xpf = xr.forkPositional();
    require(xpf.at(12345, -17, -6789).nextLong() == sc.xrPosAt, "xrPosAt");
    require(xpf.fromHashOf("minecraft:aquifer").nextLong() == sc.xrPosHash,
            "xrPosHash");
    LegacyRandomSource lr(sc.seed);
    require(lr.nextLong() == sc.lrLong, "lrLong");
  }

  const std::vector<GoldenNoiseCase> cases{
      {123456789LL, 0, 64, 0, 0xbfa0adb2aad6f157ULL, 0x3fc2c802f848c1d0ULL,
       0ULL, 0xbfe4deff6966eca5ULL, 0xbfa72490669e5126ULL},
      {123456789LL, -17, -64, 33, 0xbf67177afedf746cULL, 0x3fb0733104ea4b03ULL,
       0x3fa80d4d23aad6f9ULL, 0xbfcf7ba9b027522cULL, 0x3f8a82fcf6f52d30ULL},
      {123456789LL, 1234, 19, -567, 0x3fd1ca81f5f48ac7ULL,
       0x3fc54e065ce11fa8ULL, 0xbfe3306decd44ab6ULL, 0x3fd2067328fcf96eULL,
       0x3fa402b44b268cc9ULL},
      {123456789LL, 33554433, 319, -33554433, 0xbfd8ca2228642722ULL,
       0x3f9736f56ebfba00ULL, 0xbfdb0fb76047990dULL, 0x3fe8c9cd82b83854ULL,
       0x3fbe35d4c8582578ULL},

      {0LL, 0, 64, 0, 0x3fbd896c025ca712ULL, 0x3fc929f95dc76dfaULL, 0ULL,
       0xbfebc6795526bae1ULL, 0xbfd0a4e4f17ea3d7ULL},
      {0LL, -17, -64, 33, 0xbfd6cc0a355b0098ULL, 0x3fddac30c42b333eULL,
       0xbfe0ddcb3aece8b6ULL, 0x3fdf7ba9b0275253ULL, 0xbfca248a27cd2278ULL},
      {0LL, 1234, 19, -567, 0xbfcc1fea12839539ULL, 0x3fb50e43abfb0acfULL,
       0xbfe16dcbef278941ULL, 0xbfc56f7ec62cbd35ULL, 0xbfc7a3001e329df6ULL},
      {0LL, 33554433, 319, -33554433, 0xbfbeaa1c35728161ULL,
       0x3fbc8571c6735158ULL, 0xbf4951755b041275ULL, 0xbfdaedd4e7bfc349ULL,
       0xbfb919e52cdb1c7aULL},

      {-987654321LL, 0, 64, 0, 0x3fe03bc5998324eaULL, 0xbfb94487cab9c5a6ULL,
       0ULL, 0ULL, 0xbfa11c105bc5431dULL},
      {-987654321LL, -17, -64, 33, 0x3f809512a47db97bULL,
       0x3fcfbb5e188e3414ULL, 0x3fe3f4efc3c8c612ULL, 0xbfcf7ba9b027527aULL,
       0xbfb79d1e4b35a55cULL},
      {-987654321LL, 1234, 19, -567, 0x3fdaa9a6814a4c66ULL,
       0xbfa5588b30973d23ULL, 0xbfd386567bec4d72ULL, 0x3f83ae0ab39b68d0ULL,
       0xbf98f2beccf12082ULL},
      {-987654321LL, 33554433, 319, -33554433, 0x3fc42dffe7d74cfdULL,
       0x3fc6a6b3690a57e2ULL, 0x3fdb0962fe8cd333ULL, 0x3fe2bfab6351ce81ULL,
       0xbfb135d18bef45c6ULL},
  };

  for (const auto &c : cases) {
    auto nnRng = XoroshiroRandomSource(c.seed)
                     .forkPositional()
                     .fromHashOf("minecraft:erosion");
    NormalNoise nn = NormalNoise::create(
        nnRng, {-9, {1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 0.0}});
    LegacyRandomSource bnRng(c.seed);
    BlendedNoise bn(bnRng, 0.25, 0.125, 80.0, 160.0, 8.0);
    LegacyRandomSource snRng(c.seed);
    SimplexNoise sn(snRng);
    LegacyRandomSource lpRng(c.seed);
    PerlinNoise lp =
        PerlinNoise::createLegacyForNetherBiome(lpRng, -7, {1.0, 0.5, 0.25});

    requireCloseBits(bitsOf(nn.getValue(c.x, c.y, c.z)), c.nn, "NormalNoise");
    requireCloseBits(bitsOf(bn.compute(c.x, c.y, c.z)), c.bn, "BlendedNoise");
    requireCloseBits(bitsOf(sn.getValue(c.x * 0.125, c.z * 0.125)), c.s2,
                     "SimplexNoise2D");
    requireCloseBits(
        bitsOf(sn.getValue(c.x * 0.0625, c.y * 0.0625, c.z * 0.0625)), c.s3,
        "SimplexNoise3D");
    requireCloseBits(bitsOf(lp.getValue(c.x * 0.1, c.y * 0.1, c.z * 0.1)), c.lp,
                     "LegacyPerlin");
  }
}

} // namespace

int main() {
  testSeed123456789();
  testAllNoiseGolden();
  std::cout << "worldgen_g1_noise tests passed\n";
  return 0;
}
