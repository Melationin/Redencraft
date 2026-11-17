import java.io.BufferedWriter;
import java.io.IOException;
import java.lang.classfile.ClassFile;
import java.lang.classfile.ClassTransform;
import java.lang.classfile.CodeElement;
import java.lang.classfile.CodeTransform;
import java.lang.classfile.instruction.ReturnInstruction;
import java.lang.constant.ClassDesc;
import java.lang.constant.ConstantDescs;
import java.lang.constant.MethodTypeDesc;
import java.lang.instrument.ClassFileTransformer;
import java.lang.instrument.Instrumentation;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.ProtectionDomain;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;

import net.minecraft.commands.arguments.blocks.BlockStateParser;
import net.minecraft.core.QuartPos;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.levelgen.Heightmap;

public final class WorldgenDiffAgent {
  private static volatile boolean ran = false;

  private WorldgenDiffAgent() {}

  public static void premain(String args, Instrumentation instrumentation) {
    instrumentation.addTransformer(new ClassFileTransformer() {
      @Override
      public byte[] transform(Module module, ClassLoader loader, String name,
                              Class<?> previous, ProtectionDomain domain,
                              byte[] bytes) {
        if (!name.equals("net/minecraft/server/MinecraftServer")) {
          return null;
        }
        var classFile = ClassFile.of();
        return classFile.transformClass(classFile.parse(bytes),
            ClassTransform.transformingMethodBodies(
                code -> code.methodName().equalsString("tickServer"),
                new CodeTransform() {
                  @Override
                  public void accept(java.lang.classfile.CodeBuilder builder,
                                     CodeElement element) {
                    if (element instanceof ReturnInstruction) {
                      builder.aload(0)
                          .invokestatic(
                              ClassDesc.of("WorldgenDiffAgent"), "serverTick",
                              MethodTypeDesc.of(ConstantDescs.CD_void,
                                                ConstantDescs.CD_Object));
                    }
                    builder.with(element);
                  }
                }));
      }
    }, false);
  }

  public static void serverTick(Object serverObj) {
    if (ran)
      return;
    ran = true;
    MinecraftServer server = (MinecraftServer) serverObj;
    boolean ok = false;
    try {
      String specFile = System.getProperty("worldgen.diff.spec");
      String outFile = System.getProperty("worldgen.diff.out");
      long seed = Long.parseLong(System.getProperty("worldgen.diff.seed"));
      if (specFile == null || outFile == null) {
        throw new IllegalStateException(
            "missing worldgen.diff.spec or worldgen.diff.out");
      }

      List<String> lines = Files.readAllLines(Path.of(specFile), StandardCharsets.UTF_8);
      int count = 0;
      try (BufferedWriter writer = Files.newBufferedWriter(
               Path.of(outFile), StandardCharsets.UTF_8)) {
        for (String rawLine : lines) {
          String line = rawLine.trim();
          if (line.isEmpty() || line.startsWith("#"))
            continue;
          String[] parts = line.split("\\s+");
          long reqSeed = Long.parseLong(parts[0]);
          if (reqSeed != seed)
            continue;
          String dimStr = parts[1];
          int cx = Integer.parseInt(parts[2]);
          int cz = Integer.parseInt(parts[3]);
          String[] stages = parts[4].split(",");

          ResourceKey<Level> levelKey = parseDimension(dimStr);
          ServerLevel level = server.getLevel(levelKey);
          if (level == null) {
            throw new IllegalStateException("missing ServerLevel for " + dimStr);
          }

          for (String stageName : stages) {
            if (stageName.isEmpty())
              continue;
            ChunkStatus status = parseStatus(stageName);
            ChunkAccess chunk = level.getChunkSource().getChunk(cx, cz, status, true);
            writeChunkSnapshot(writer, seed, dimStr, cx, cz, stageName, chunk);
            ++count;
          }
        }
      }
      System.out.println("VANILLA_WORLDGEN_DIFF_OK " + count);
      System.out.flush();
      ok = true;
    } catch (Throwable failure) {
      failure.printStackTrace(System.out);
      System.out.println("VANILLA_WORLDGEN_DIFF_FAILED " + failure);
      System.out.flush();
    } finally {
      // Ephemeral diff output is already closed & flushed; skip Saving worlds across 90k+ chunks.
      System.out.flush();
      System.err.flush();
      Runtime.getRuntime().halt(ok ? 0 : 1);
    }
  }

  private static ResourceKey<Level> parseDimension(String dim) {
    return switch (dim) {
      case "minecraft:overworld" -> Level.OVERWORLD;
      case "minecraft:the_nether" -> Level.NETHER;
      case "minecraft:the_end" -> Level.END;
      default -> throw new IllegalArgumentException("unsupported dim: " + dim);
    };
  }

  private static ChunkStatus parseStatus(String stage) {
    return switch (stage) {
      case "BIOMES" -> ChunkStatus.BIOMES;
      case "NOISE" -> ChunkStatus.NOISE;
      case "SURFACE" -> ChunkStatus.SURFACE;
      case "CARVERS" -> ChunkStatus.CARVERS;
      case "FEATURES" -> ChunkStatus.FEATURES;
      case "FULL" -> ChunkStatus.FULL;
      default -> throw new IllegalArgumentException("unknown stage: " + stage);
    };
  }

  private static String jsonEscape(String s) {
    StringBuilder sb = new StringBuilder(s.length() + 4);
    for (int i = 0; i < s.length(); ++i) {
      char c = s.charAt(i);
      switch (c) {
        case '"' -> sb.append("\\\"");
        case '\\' -> sb.append("\\\\");
        case '\n' -> sb.append("\\n");
        case '\r' -> sb.append("\\r");
        case '\t' -> sb.append("\\t");
        default -> sb.append(c);
      }
    }
    return sb.toString();
  }

  private static void appendRle(List<int[]> rle, int idx) {
    if (!rle.isEmpty() && rle.getLast()[0] == idx) {
      ++rle.getLast()[1];
    } else {
      rle.add(new int[]{idx, 1});
    }
  }

  private static void writeChunkSnapshot(BufferedWriter out, long seed,
                                         String dim, int cx, int cz,
                                         String stageName, ChunkAccess chunk)
      throws IOException {
    int minY = chunk.getMinY();
    int height = chunk.getHeight();

    // 1. Quart biomes (qy, qz, qx)
    List<String> biomePalette = new ArrayList<>();
    Map<String, Integer> biomeIdx = new HashMap<>();
    List<int[]> biomeRle = new ArrayList<>();
    int quartMinY = QuartPos.fromBlock(minY);
    int quartHeight = QuartPos.fromBlock(height);
    int quartBaseX = QuartPos.fromBlock(cx << 4);
    int quartBaseZ = QuartPos.fromBlock(cz << 4);
    for (int qy = 0; qy < quartHeight; ++qy) {
      for (int qz = 0; qz < 4; ++qz) {
        for (int qx = 0; qx < 4; ++qx) {
          String biome = chunk.getNoiseBiome(quartBaseX + qx, quartMinY + qy, quartBaseZ + qz)
              .getRegisteredName();
          Integer idx = biomeIdx.get(biome);
          if (idx == null) {
            idx = biomePalette.size();
            biomePalette.add(biome);
            biomeIdx.put(biome, idx);
          }
          appendRle(biomeRle, idx);
        }
      }
    }

    // 2. Blocks (sec -> ly, lz, lx) matches ascending (y, z, x)
    List<String> blockPalette = new ArrayList<>();
    IdentityHashMap<BlockState, Integer> blockIdx = new IdentityHashMap<>();
    List<int[]> blockRle = new ArrayList<>();
    BlockState airState = Blocks.AIR.defaultBlockState();
    Integer airIdx = null;
    LevelChunkSection[] sections = chunk.getSections();
    for (LevelChunkSection sec : sections) {
      if (sec.hasOnlyAir()) {
        if (airIdx == null) {
          airIdx = blockPalette.size();
          blockPalette.add(BlockStateParser.serialize(airState));
          blockIdx.put(airState, airIdx);
        }
        if (!blockRle.isEmpty() && blockRle.getLast()[0] == airIdx) {
          blockRle.getLast()[1] += 4096;
        } else {
          blockRle.add(new int[]{airIdx, 4096});
        }
        continue;
      }
      for (int ly = 0; ly < 16; ++ly) {
        for (int lz = 0; lz < 16; ++lz) {
          for (int lx = 0; lx < 16; ++lx) {
            BlockState st = sec.getBlockState(lx, ly, lz);
            Integer idx = blockIdx.get(st);
            if (idx == null) {
              idx = blockPalette.size();
              blockPalette.add(BlockStateParser.serialize(st));
              blockIdx.put(st, idx);
            }
            appendRle(blockRle, idx);
          }
        }
      }
    }

    // 3. Heightmaps
    Heightmap.Types[] hmTypes = {
        Heightmap.Types.WORLD_SURFACE_WG,
        Heightmap.Types.OCEAN_FLOOR_WG,
        Heightmap.Types.MOTION_BLOCKING,
        Heightmap.Types.WORLD_SURFACE,
    };

    StringBuilder sb = new StringBuilder(16384);
    sb.append("{\"seed\":").append(seed)
      .append(",\"dim\":\"").append(jsonEscape(dim)).append('"')
      .append(",\"cx\":").append(cx)
      .append(",\"cz\":").append(cz)
      .append(",\"stage\":\"").append(jsonEscape(stageName)).append('"')
      .append(",\"min_y\":").append(minY)
      .append(",\"height\":").append(height)
      .append(",\"biome_palette\":[");
    for (int i = 0; i < biomePalette.size(); ++i) {
      if (i > 0) sb.append(',');
      sb.append('"').append(jsonEscape(biomePalette.get(i))).append('"');
    }
    sb.append("],\"biome_rle\":[");
    for (int i = 0; i < biomeRle.size(); ++i) {
      if (i > 0) sb.append(',');
      int[] r = biomeRle.get(i);
      sb.append('[').append(r[0]).append(',').append(r[1]).append(']');
    }
    sb.append("],\"heightmaps\":{");
    for (int t = 0; t < hmTypes.length; ++t) {
      if (t > 0) sb.append(',');
      sb.append('"').append(hmTypes[t].name()).append("\":[");
      boolean first = true;
      for (int lz = 0; lz < 16; ++lz) {
        for (int lx = 0; lx < 16; ++lx) {
          if (!first) sb.append(',');
          first = false;
          sb.append(chunk.getHeight(hmTypes[t], lx, lz));
        }
      }
      sb.append(']');
    }
    sb.append("},\"block_palette\":[");
    for (int i = 0; i < blockPalette.size(); ++i) {
      if (i > 0) sb.append(',');
      sb.append('"').append(jsonEscape(blockPalette.get(i))).append('"');
    }
    sb.append("],\"block_rle\":[");
    for (int i = 0; i < blockRle.size(); ++i) {
      if (i > 0) sb.append(',');
      int[] r = blockRle.get(i);
      sb.append('[').append(r[0]).append(',').append(r[1]).append(']');
    }
    sb.append("]}\n");
    out.write(sb.toString());
  }
}
