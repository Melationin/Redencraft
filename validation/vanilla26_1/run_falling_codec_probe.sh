#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 1 ]]; then
  echo "usage: run_falling_codec_probe.sh <cpp-entity-nbt>" >&2
  exit 2
fi
repo=/home/zhddsj/cpp/redstone-cpp
run=/mnt/f/source/minecraft-merged-07da9a845a-26.1-sources/redstone-cpp/build-fluid-validation/fluid-runs/20261109-005810-34968-9660049e
jar="$run/versions/26.1/server-26.1.jar"
libs="$run/libraries"
java_home=/home/zhddsj/.jdks/azul-26.0.2
out=/home/zhddsj/.local/state/redstone-cpp/wsl-w3b2c2-review-20261113/java
if [[ -n "${FALLING_CODEC_OUT:-}" ]]; then out="$FALLING_CODEC_OUT"; fi
classes="$out/classes"
rm -rf "$classes"
mkdir -p "$classes"
classpath="$jar"
while IFS= read -r lib; do
  classpath="$classpath:$lib"
done < <(find "$libs" -type f -name '*.jar' | sort)
"$java_home/bin/javac" -cp "$classpath" -d "$classes"   "$repo/validation/vanilla26_1/FallingBlockCodecProbe.java"
"$java_home/bin/java" -cp "$classes:$classpath"   FallingBlockCodecProbe "$1" "$out/official-rewritten-falling.nbt"   | tee "$out/falling-codec-probe.log"
