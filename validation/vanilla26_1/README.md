# Vanilla 26.1 carpet duplication comparison

This test ran the original Mojang dedicated server 26.1 using Zulu Java 26 on localhost only. The downloaded server jar SHA-1 was `3872a7f07a1a595e651aef8b058dfc2bb3772f46`, matching Mojang's version metadata in `version.json`. The user authorized accepting the Minecraft EULA for this isolated local comparison.

The script freezes the game, force-loads the test chunks, and builds an east-facing sticky piston at (0,100,0), with slime at (1,100,0) and (1,101,0). A redstone block at (-1,100,0) powers it; after three ticks both slime blocks are at x=2. It places white carpet at (1,101,0), on the extended piston head, removes power and advances five ticks. Exact commands and server output are in `server-validation.log`.

Observed server output:

- `VALIDATION_EXTENDED_OK` and `VALIDATION_CARPET_PLACED` confirm the setup.
- `VALIDATION_CARPET_MOVED_TO_BASE` confirms a white carpet block at (0,101,0).
- `VALIDATION_LOWER_SLIME_RETRACTED` and `VALIDATION_UPPER_SLIME_RETRACTED` confirm both slime blocks at x=1.
- `data get entity` returned `{count: 1, id: "minecraft:white_carpet"}` and `VALIDATION_WHITE_CARPET_ITEM` confirmed an item entity.

The C++ `carpetDuplicator` regression in `tests/piston_tests.cpp` now uses the same coordinates and tick counts (3 ticks extending, 5 ticks after removing power). It asserts the carpet at (0,101,0), both slime blocks at x=1, exactly one logged white-carpet item spawn, and that the item is launched westward by the retracting slime (`deltaMovement.x < -0.8`). This compares this specific structure's settled state and item outcome; it does not establish every update sequence as server-differentially verified.

## Item trajectory after the slime launch

`item_motion_validation.py` repeats the same structure while frozen and reads the item entity's `Pos`/`Motion` after each tick. The server output is in `item-motion-validation.log`. The deterministic part is the X sequence after the moving slime hits the item: velocity `-1.0`, then `-0.98`, `-0.9604`, ... and positions advancing by that velocity. The hit tick can shift by one when commands/tick steps are scheduled differently, and the spawn Y/Z plus initial Z motion use the world random stream. The C++ focused regression `slimePistonBounce()` asserts the local one-tick launch to `-1.0` and the swept push; it does not assert a single server run's absolute item position.

To rerun, download the `server` URL from `version.json` as `server.jar`, verify the SHA-1 above, review https://aka.ms/MinecraftEULA and place `eula=true` in `eula.txt` only if you accept it. Run `python run_validation.py` and/or `python item_motion_validation.py` with Java 26 at the default Zulu path or set `JAVA_26` to the Java executable. The scripts use 127.0.0.1:25597 and create an isolated world in this directory. They overwrite their respective `*-validation.log` files and the server properties.

## Block-light differential

`run_block_light_validation.py` compiles `LightDiffAgent.java` and starts the same official 26.1 `server.jar` on a unique loopback port in a fresh world under `build-light-validation/block-light-runs/`. For each command-driven stage, the agent submits `ThreadedLevelLightEngine.waitForPendingTasks` POST_UPDATE fences for all four affected chunks. Only after every fence future completes does it queue the read back onto the server main thread; the tick itself never waits. It records 45 block-light cells for each of five stages: the original three stages plus bottom/top padding source insertion and removal. No client is started.

The first successful fence run was promoted to `block-light-golden.json`; a second independent run produced the same candidate. `block_light_differential_test.py` runs the C++ `block_light_probe` and compares all 5 x 45 = 225 observations; the first 39 cells of each original stage retain the old 117-observation fixture order and values. Raw server logs, fence records, candidate JSON, agent JAR, commands and hashes remain in the unique run directory.

## Sky-light differential

`run_sky_light_validation.py` uses `SkyDiffAgent.java` and the same POST_UPDATE fence pattern. Nine command stages cover an open column, a full-chunk roof with an open adjacent chunk, roof opening, reclosing, a negative-height roof and its removal, top-layer water, and a three-chunk A/B/C missing-section edge fixture before/after closing its source. The fixture records 27 exact sky-light cells per stage, including cross-chunk side attenuation, negative y, upper/bottom padding, inherited sparse planes, stored raw layers that are uniform 15, and cross-empty-section copy/clear. Eight positions per stage also record raw-layer presence, including source-side missing and adjacent stored sections. The promoted `sky-light-golden.json` and its independent repeat contain 9 x 27 = 243 sky values plus 72 raw-layer flags; `sky_light_differential_test.py` compares both with `sky_light_probe`. Raw worlds, logs and manifests are under `build-light-validation/sky-light-runs/<unique>/`. No client is started.
