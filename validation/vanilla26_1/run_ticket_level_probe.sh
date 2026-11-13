#!/usr/bin/env bash
set -euo pipefail
repo=/home/zhddsj/cpp/redstone-cpp
run=/mnt/f/source/minecraft-merged-07da9a845a-26.1-sources/redstone-cpp/build-fluid-validation/fluid-runs/20261109-005810-34968-9660049e
jar="$run/versions/26.1/server-26.1.jar"
libs="$run/libraries"
java_home=/home/zhddsj/.jdks/azul-26.0.2
out=/home/zhddsj/.local/state/redstone-cpp/wsl-ticket-t1-20261112
classes="$out/classes"
rm -rf "$classes"
mkdir -p "$classes"
classpath="$jar"
while IFS= read -r lib; do
  classpath="$classpath:$lib"
done < <(find "$libs" -type f -name '*.jar' | sort)
"$java_home/bin/javac" -cp "$classpath" -d "$classes" \
  "$repo/validation/vanilla26_1/TicketLevelProbe.java"
"$java_home/bin/java" -cp "$classes:$classpath" \
  TicketLevelProbe | tee "$out/ticket-level-probe.log"
