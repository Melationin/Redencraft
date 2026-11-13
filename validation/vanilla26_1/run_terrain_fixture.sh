#!/usr/bin/env bash
set -euo pipefail
repo=/home/zhddsj/cpp/redstone-cpp
run=/mnt/f/source/minecraft-merged-07da9a845a-26.1-sources/redstone-cpp/build-fluid-validation/fluid-runs/20261109-005810-34968-9660049e
jar="$run/versions/26.1/server-26.1.jar"
libs="$run/libraries"
java_home=/home/zhddsj/.jdks/azul-26.0.2
out="${TERRAIN_FIXTURE_OUT:-/home/zhddsj/.local/state/redstone-cpp/wsl-w2b-terrain-java-20261112}"
classes="$out/classes"
region="$out/region"
scratch="$out/cpp-scratch"
binary=/home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build/terrain_codec_tests
rm -rf "$out"
mkdir -p "$classes" "$region"
classpath="$jar"
while IFS= read -r lib; do
  classpath="$classpath:$lib"
done < <(find "$libs" -type f -name '*.jar' | sort)
"$java_home/bin/javac" -cp "$classpath" -d "$classes" \
  "$repo/validation/vanilla26_1/TerrainFixtureProbe.java" \
  "$repo/validation/vanilla26_1/TerrainEditedProbe.java"
"$java_home/bin/java" -cp "$classes:$classpath" TerrainFixtureProbe "$region" \
  | tee "$out/java-fixture.log"
"$binary" "$scratch" "$region" | tee "$out/cpp-read-java.log"
"$java_home/bin/java" -cp "$classes:$classpath" TerrainEditedProbe \
  "$scratch/cpp-edited" | tee "$out/java-read-cpp.log"
