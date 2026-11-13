#!/usr/bin/env bash
set -euo pipefail
repo=/home/zhddsj/cpp/redstone-cpp
run=/mnt/f/source/minecraft-merged-07da9a845a-26.1-sources/redstone-cpp/build-fluid-validation/fluid-runs/20261109-005810-34968-9660049e
jar="$run/versions/26.1/server-26.1.jar"
libs="$run/libraries"
java_home=/home/zhddsj/.jdks/azul-26.0.2
out="${WORLD_CYCLE_FIXTURE_OUT:-/home/zhddsj/.local/state/redstone-cpp/wsl-t4b1-world-fixture-20261112}"
classes="$out/classes"
region="$out/region"
rm -rf "$out"
mkdir -p "$classes" "$region"
classpath="$jar"
while IFS= read -r lib; do
  classpath="$classpath:$lib"
done < <(find "$libs" -type f -name '*.jar' | sort)
"$java_home/bin/javac" -cp "$classpath" -d "$classes" \
  "$repo/validation/vanilla26_1/TerrainWorldCycleFixtureProbe.java"
"$java_home/bin/java" -cp "$classes:$classpath" \
  TerrainWorldCycleFixtureProbe "$region"
