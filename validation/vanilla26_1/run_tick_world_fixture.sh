#!/usr/bin/env bash
set -euo pipefail
repo=/home/zhddsj/cpp/redstone-cpp
run=/mnt/f/source/minecraft-merged-07da9a845a-26.1-sources/redstone-cpp/build-fluid-validation/fluid-runs/20261109-005810-34968-9660049e
jar="$run/versions/26.1/server-26.1.jar"
libs="$run/libraries"
java_home=/home/zhddsj/.jdks/azul-26.0.2
mode="${1:-write}"
out="${TICK_FIXTURE_OUT:-/home/zhddsj/.local/state/redstone-cpp/wsl-w3a-tick-fixture-20261112}"
classes="$out/classes"
region="${2:-$out/region}"
if [[ "$mode" == "write" ]]; then
  rm -rf "$out"
  mkdir -p "$classes" "$region"
else
  [[ -d "$region" ]] || { echo "missing fixture region: $region" >&2; exit 1; }
fi
classpath="$jar"
while IFS= read -r lib; do
  classpath="$classpath:$lib"
done < <(find "$libs" -type f -name '*.jar' | sort)
"$java_home/bin/javac" -cp "$classpath" -d "$classes" \
  "$repo/validation/vanilla26_1/TerrainTickWorldCycleFixtureProbe.java" \
  "$repo/validation/vanilla26_1/TerrainTickReadProbe.java"
if [[ "$mode" == "write" ]]; then
  "$java_home/bin/java" -cp "$classes:$classpath" \
    TerrainTickWorldCycleFixtureProbe "$region"
else
  "$java_home/bin/java" -cp "$classes:$classpath" \
    TerrainTickReadProbe "$region"
fi
