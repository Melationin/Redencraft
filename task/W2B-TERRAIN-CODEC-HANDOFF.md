# W-2b Terrain Chunk NBT Codec Handoff

Date: 2026-11-12 (Asia/Shanghai)

## Scope and status

This stage adds a standalone 26.1 terrain-chunk NBT codec on top of the W-2a
RegionFile layer. It does not connect the online World, an asynchronous loader,
an unload policy, a Minecraft client/server, or a source save. Nothing was
committed or pushed, and CTest was not run.

The implementation is strict: unknown blocks, non-schema block states, a
different DataVersion, non-FULL chunks, malformed palettes, and malformed
packed storage are rejected explicitly. Unknown blocks are never converted to
air.

## Files

Added:

- include/redstone/terrain_codec.hpp
- src/terrain_codec.cpp
- tests/terrain_codec_tests.cpp
- validation/vanilla26_1/TerrainFixtureProbe.java
- validation/vanilla26_1/TerrainEditedProbe.java
- validation/vanilla26_1/run_terrain_fixture.sh

Changed:

- include/redstone/nbt.hpp
- src/nbt.cpp
- CMakeLists.txt
- src/region.cpp (W-2a residual: validate the complete 8 KiB header before
  consulting the target location)
- tests/region_tests.cpp (all-zero 4 KiB header regression)

The fixture runner accepts TERRAIN_FIXTURE_OUT, so a new run does not erase an
earlier isolated fixture/log directory.

## API and ownership

terrain::decodeChunkNbt(span) parses owned NBT and builds an owned decoded view.
terrain::decodeChunkAt(regionDirectory, chunkX, chunkZ) reads one raw region
slot through W-2a and verifies xPos/zPos against the requested slot.
terrain::patchBlockStates(RootTag&, span<const BlockEdit>) is the production
edit path. It owns the DOM in place, groups edits by section, applies
last-write-wins per cell, and repacks each actually changed section once.
terrain::setBlockState(const DecodedChunk&, ...) is fixture compatibility only:
it clones the complete root as explicitly documented in the header.
terrain::setBlockState(DecodedChunk&&, ...) moves that owned root into the
result without cloning it. terrain::encodeChunkNbt writes a supplied root or a
DecodedChunk.

DecodedChunk retains the complete NBT DOM. Its decoded sections, ticks, block
entities, and entities are convenience views; no World is consulted.

## Decode invariants

- DataVersion must be 4786.
- xPos and zPos must be present as 32-bit ints.
- Status must be minecraft:full or full.
- sections must be a compound list with signed-byte Y values. Duplicate Y
  values are corrupt.
- A block_states palette is a nonempty compound list of at most 4096 entries.
- Every palette name must resolve to the existing C++ support set, and every
  Properties map must map to a registered schema state. Otherwise the result is
  Unsupported.
- A single-entry palette has no nonempty data stream.
- A multi-entry palette has the exact 4096-cell SimpleBitStorage long count.
  Entries are packed without crossing a 64-bit word, matching
  PalettedContainer/SimpleBitStorage on-disc geometry. Every index is range
  checked.
- block_ticks and fluid_ticks use SavedTick keys i/x/y/z/t/p. Priority outside
  -3..3 is clamped to EXTREMELY_HIGH/EXTREMELY_LOW exactly as TickPriority.
- block_entities and entities are retained as raw compound tags.
- Empty compound lists retain their element type.
- Missing, non-FULL, unsupported, and corrupt inputs remain distinct results.

## Edit invariants

- Only an already validated DecodeStatus::Ok chunk can be edited.
- The target block must be supported and isSchemaState() must be true.
- A missing section, out-of-range local index, or full 4096-entry palette is
  rejected before changing the returned root.
- An actual change repacks all 4096 entries when the palette crosses a 4/5/6...
  bit boundary. Cell 4094 is covered at the 4-bit to 5-bit boundary.
- Writing the state already stored at the cell is a semantic no-op: edited is
  false, the root is byte-identical, and isLightOn/Heightmaps are untouched.
- A real change sets existing isLightOn to false and clears existing Heightmaps.
  Explicit absence already means the default-derived state is absent.
- Unedited sections are retained byte-for-byte. The test includes a second
  section with unknown fields and raw SkyLight and compares its serialized
  bytes before and after an edit in section 0.
- Biomes, raw BlockLight/SkyLight, structures, block entities, unknown
  compounds, root name, integer widths, and typed empty lists are retained.
- block_entities/entities are not decoded into gameplay objects in this stage.

## Original source basis

Local source under ../net:

- SerializableChunkData.java:102-106 defines xPos, zPos, Heightmaps,
  isLightOn, sections, and both light array keys.
- SerializableChunkData.java:109-170 performs FULL/status, position, tick,
  entity, block-entity, and block_states parsing. Its paletted-container codec
  is the format followed here.
- SerializableChunkData.java:414-478 writes DataVersion/position/Status,
  sections/block_states/biomes/light, entities and ticks, Heightmaps, and
  structures.
- PalettedContainer.java:48-62 and 175-245 define palette/data packing and
  SimpleBitStorage re-encoding.
- SavedTick.java:28-42 defines the i/x/y/z/t/p codec; TickPriority.java:7-35
  defines and clamps the seven priority values.

This C++ stage mirrors the on-disc subset needed for a FULL terrain chunk. It
does not claim a complete gameplay conversion of every Java type.

## Verification

Build (GCC, Ninja, Release flags visible in the compile line):

    cmake --build /home/zhddsj/.local/state/redstone-cpp/wsl-perf-light-split-20261111/build --target terrain_codec_tests region_tests --parallel 6

Final terrain direct test:

    terrain_codec_tests <scratch> /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-terrain-java-20261112-final/region

Result:

    real_26_1_chunk=-1,-1 sections=1 block_ticks=0 fluid_ticks=0
    terrain codec tests passed

The focused test covers byte-identical round-trip of covered normal-value
fixtures, all integer widths, typed empty lists, 4096-cell unpacking,
4-to-5-bit palette growth, same-value no-op behavior, palette >4096 rejection,
unknown block and invalid property rejection, invalid version/FULL status,
block-entity retention, unedited-section byte identity, coordinate/slot
mismatch, and a real Java RegionFile/NbtIo fixture. Byte identity is limited
to values the Tag DOM represents losslessly; noncanonical IEEE NaN payloads are
not promised. The same suite also covers the post-review in-place batch path.

Official 26.1 Java fixture:

    TERRAIN_FIXTURE_OUT=/home/zhddsj/.local/state/redstone-cpp/wsl-w2b-terrain-java-20261112-final \
      bash validation/vanilla26_1/run_terrain_fixture.sh

The fixture uses SharedConstants/Bootstrap, CompoundTag, NbtIo, ChunkPos, and
RegionFile from the local 26.1 jar. Java writes the chunk; C++ reads and edits
it; Java reads the C++-edited region. Outputs:

    JAVA_FULL_FIXTURE_OK
    real_26_1_chunk=-1,-1 sections=1 block_ticks=0 fluid_ticks=0
    terrain codec tests passed
    JAVA_READ_CPP_EDIT_OK

This is an official-class format interop fixture, not a Minecraft restart and
not a headless generated World. SerializableChunkData.parse/write with a full
server Level was not run.

Region regression after the read-order fix:

    region tests passed

The isolated Java-written region is
/home/zhddsj/.local/state/redstone-cpp/wsl-w2b-terrain-java-20261112-final/region/r.-1.-1.mca
(SHA-256 b693c99d6143d438dd39fb0fd0656f6588c52e0b160fd3c914dc53fe751cba6e).

## Existing fluid world, read only

Strictly scanning the existing fluid-validation Overworld region produced:

    terrain codec test failure: official 26.1 fixture has no supported FULL chunk; missing=3567 nonfull=528 unsupported=1 corrupt=0 last=chunk is not full

The scan is read only. It is evidence that the current loose validation world
does not contain a usable supported FULL chunk under the strict rules. No rule
was relaxed to manufacture success.

## Logs and hashes

- Final build: /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-20261112-final-build3.log
  SHA-256 c59ee053c4b498b44e25890f976123dddd3c1abc6c6769e5cbdbbd8ab494da12
- Terrain direct test: /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-20261112-final-terrain3.log
  SHA-256 df70095f12414f5da75f5c95c4d1c3f51fc14231e670fb8cee169e899a931efb
- Region direct test: /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-20261112-final-region.log
  SHA-256 6ab3390c58e39bee2b83cf611e56461469be0ebebfdd50b6b6377a34208667e0
- Java/C++ interop: /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-20261112-final-java.log
  SHA-256 023a76e5864030ecb38512183bdd83007536860d09727c7bcd1928ff59a747e5
- Strict fluid-world scan: /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-20261112-final-fluid-strict.log
  SHA-256 b5c28169b1f7fcbe998c1897abfe5a3548ecbdfb65677e82da7024667c858c1a

Source hashes:

    include/redstone/terrain_codec.hpp  843ec01d54be2bbf5cfb2e436a295e1750e39fbf1945ad100b8b7a7ce6c3e927
    src/terrain_codec.cpp               bb9043d570617efa5330bac836117545a83d2710da0d939b951f47285faca193
    tests/terrain_codec_tests.cpp       fe4f77f188006e8dc7c3e9fa040b35a67e29de15146b3d6818346246c6f86be9
    include/redstone/nbt.hpp            d1c982e93c638d7beaebe2812a7c5cc5a5d652825d2c3f24974bcd2ec7ef10f8
    src/nbt.cpp                         c608e242cf7aeddbbd9dbe450b0bcbb6220f77e955f1675b51c7910a446dca65
    src/region.cpp                      cbcfb788526f28ea655e8f42c7c96d750bb4fef233fd02a424681fa8c4779aa9
    tests/region_tests.cpp              bc64a0c13a4a14d27201bd02f71ed33927e6f9e9ba1ede438d6104fe28a11a22
    CMakeLists.txt                      13e7b924118f502d6345796f74de3a3c9df890fb48526f234f70cebd547396b4

Test binaries:

    terrain_codec_tests 660fcf1f06c7b330fa9056c1feae5174ee835c3cbd2aa723e425651d9e10c276
    region_tests        936c26a8f023bc8dfa93d9303a710316c9c60c42a7375c8f451a340c15853a1c

## Remaining limits

- No online chunk load/save integration, spawned entity handling, asynchronous
  I/O, unload policy, or source-save write.
- Entity-region NBT is outside this stage; only raw "entities" in the terrain
  chunk, if present, are retained.
- SerializableChunkData.parse against a real Level was not run.
- The strict codec supports only the current C++ block-state subset. Unsupported
  blocks remain an explicit result rather than being approximated.
- The 26.1 fixture is a legal minimal FULL chunk crafted through official NBT
  and RegionFile classes, not an actually generated/generated-and-reloaded
  World.

No CTest, unrelated benchmark, commit, push, client/server launch, lighting
change, ticket change, or World behavior change was performed.

## Post-review bounded correction (current candidate)

Production edits now use:

    PatchResult patchBlockStates(nbt::RootTag &root,
                                 std::span<const BlockEdit> edits);

The root is decoded and owned by its caller, then mutated in place. There is no
complete-root assignment in this path. Edits are grouped by section, duplicate
cell writes use the last value in input order, and each affected section is
repacked once. PatchResult reports changed unique cells and repacked sections.

Focused runtime evidence:

- Three edits in one section: changedCells=3, repackedSections=1.
- Two edits in different sections: changedCells=2, repackedSections=2.
- Three effective no-op targets plus one overwritten same-cell edit:
  changedCells=0, repackedSections=0; serialized bytes, untouched SkyLight
  address, and packed-data address are unchanged.
- Untouched payload and root compound-node addresses remain stable across a
  real in-place edit.
- Unknown fields, other sections, NBT ordering, block entities, and light bytes
  are retained.
- The rvalue compatibility wrapper is exercised.

Necessary transient work for one changed section, processed sequentially:

- 8 KiB uint16_t[4096] unpacked indices, read once during preflight and once
  during the one final repack.
- 512-byte std::bitset<4096> to retain the last edit per cell.
- A vector of up to 4096 selected edit indices in the worst case.
- Final section packed longs, 1-6 KiB depending on bit width, are rewritten in
  that section only.
- Palette maps/entries are limited to the affected section. The full chunk DOM
  is neither copied nor retained twice.

The const wrapper and fixture still copy the complete root. It is explicitly
documented as fixture compatibility; production/save callers use
patchBlockStates or the rvalue wrapper.

NBT writer hardening:

- Strings longer than UINT16_MAX throw instead of truncating.
- Byte/Int/Long arrays and lists enforce both the shared 16,000,000-entry limit
  and the signed INT32_MAX NBT boundary before writing.
- Writer recursion enforces depth 64 and the 4,000,000-node limit.
- Duplicate compoundOrder entries are emitted once; map keys absent from the
  captured order are still emitted once afterward.
- Direct tests cover a 65,536-byte string, excessive nesting, and duplicate
  compoundOrder. The collection-size checks are code-enforced; constructing a
  >INT_MAX element fixture would itself require an unreasonable allocation.

Current direct verification:

    terrain codec tests passed

The Java 26.1 fixture again completed Java write -> C++ decode/edit -> Java
read:

    JAVA_FULL_FIXTURE_OK
    terrain codec tests passed
    JAVA_READ_CPP_EDIT_OK

It remains an official-class format fixture, not a real server restart.

Current logs and hashes:

- /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-review-20261112-build-final.log
  SHA-256 5ca1acca68af68798f5df9fabb5065cc60e2f041e830fb37f861915b0c3dba9b
- /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-review-20261112-terrain-final.log
  SHA-256 df70095f12414f5da75f5c95c4d1c3f51fc14231e670fb8cee169e899a931efb
- /home/zhddsj/.local/state/redstone-cpp/wsl-w2b-review-20261112-java-final.log
  SHA-256 a17ff369e18cac1e759557e5fe18c6ff3da54921148c8b9ed79f46b793acff35

Current source hashes:

    include/redstone/terrain_codec.hpp  44d0fe3def0f78306c56cc0d289e1ec6151436b5b87348f546f4a03f4de3018b
    src/terrain_codec.cpp               468d30d6d3e7bdbb9652d267a03292a304898b3797934174808f8960a5f290f9
    tests/terrain_codec_tests.cpp       e965109b89c302ed0fdbd2c410f201eabb5f45b9a651b618674af91ce7877940
    include/redstone/nbt.hpp            a297fd6c7cb01c4600ba14b1f007b964b3381575ebdc24ca222945e7542c567a
    src/nbt.cpp                         a4d8f696507d9c6affe23390c92f848fe0cadac4bb4d3d0290a58e80765a0d10
    terrain_codec_tests binary          26625f8129510e2e07c03d8f064df837b82521cdbc8d2596549266f1e2bbaccd

Earlier hashes in this handoff describe the pre-review candidate. These hashes
and logs are the current review-fix evidence. No CTest, commit, push, online
World/save integration, lighting change, or ticket change was performed.
