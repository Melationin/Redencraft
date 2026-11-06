# Vanilla 26.1 carpet duplication comparison

This test ran the original Mojang dedicated server 26.1 using Zulu Java 26 on localhost only. The downloaded server jar SHA-1 was `3872a7f07a1a595e651aef8b058dfc2bb3772f46`, matching Mojang's version metadata in `version.json`. The user authorized accepting the Minecraft EULA for this isolated local comparison.

The script freezes the game, force-loads the test chunks, and builds an east-facing sticky piston at (0,100,0), with slime at (1,100,0) and (1,101,0). A redstone block at (-1,100,0) powers it; after three ticks both slime blocks are at x=2. It places white carpet at (1,101,0), on the extended piston head, removes power and advances five ticks. Exact commands and server output are in `server-validation.log`.

Observed server output:

- `VALIDATION_EXTENDED_OK` and `VALIDATION_CARPET_PLACED` confirm the setup.
- `VALIDATION_CARPET_MOVED_TO_BASE` confirms a white carpet block at (0,101,0).
- `VALIDATION_LOWER_SLIME_RETRACTED` and `VALIDATION_UPPER_SLIME_RETRACTED` confirm both slime blocks at x=1.
- `data get entity` returned `{count: 1, id: "minecraft:white_carpet"}` and `VALIDATION_WHITE_CARPET_ITEM` confirmed an item entity.

The C++ `carpetDuplicator` regression in `tests/piston_tests.cpp` now uses the same coordinates and tick counts (3 ticks extending, 5 ticks after removing power). It asserts the carpet at (0,101,0), both slime blocks at x=1, and exactly one logged white-carpet item spawn. This compares this specific structure's settled state and item outcome; it does not establish every update sequence as server-differentially verified.

To rerun, download the `server` URL from `version.json` as `server.jar`, verify the SHA-1 above, review https://aka.ms/MinecraftEULA and place `eula=true` in `eula.txt` only if you accept it. Run `python run_validation.py` with Java 26 at the default Zulu path or set `JAVA_26` to the Java executable. The script uses 127.0.0.1:25597 and creates an isolated world in this directory. It overwrites `server-validation.log` and the server properties.
