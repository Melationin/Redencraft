import java.io.BufferedWriter;
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

import net.minecraft.core.Holder;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.chunk.ChunkGenerator;

public final class BiomeTreeBenchAgent {
  private static volatile boolean ran = false;

  private BiomeTreeBenchAgent() {}

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
                              ClassDesc.of("BiomeTreeBenchAgent"), "serverTick",
                              MethodTypeDesc.of(ConstantDescs.CD_void,
                                                ConstantDescs.CD_Object));
                    }
                    builder.with(element);
                  }
                }));
      }
    }, false);
  }

  private static long nextCoord(long[] state) {
    state[0] = state[0] * 6364136223846793005L + 1442695040888963407L;
    return ((state[0] >>> 33) % 20001L) - 10000L;
  }

  public static void serverTick(Object serverObj) {
    if (ran)
      return;
    ran = true;
    try {
      String outFile = System.getProperty("biome.bench.out");
      if (outFile == null)
        throw new IllegalStateException("missing -Dbiome.bench.out");
      MinecraftServer server = (MinecraftServer) serverObj;
      ServerLevel level = server.overworld();
      ChunkGenerator generator = level.getChunkSource().getGenerator();
      if (!(generator.getBiomeSource() instanceof MultiNoiseBiomeSource source))
        throw new IllegalStateException("overworld biome source is not MultiNoiseBiomeSource");

      // 直接取到 ParameterList（overworld 使用 preset，私有的 parameters() 会展开）。
      java.lang.reflect.Method paramsMethod =
          MultiNoiseBiomeSource.class.getDeclaredMethod("parameters");
      paramsMethod.setAccessible(true);
      @SuppressWarnings("unchecked")
      Climate.ParameterList<Holder<Biome>> list =
          (Climate.ParameterList<Holder<Biome>>) paramsMethod.invoke(source);

      final int n = 200000;
      final int warmup = 300000;
      final int measured = 1000000;
      final int reps = 5;
      long[] targets = new long[n * 6];
      long[] state = {0x123456789ABCDEFL};
      for (int i = 0; i < targets.length; ++i)
        targets[i] = nextCoord(state);

      long sink = 0;
      for (int i = 0; i < warmup; ++i) {
        int k = (i % n) * 6;
        sink += list.findValue(new Climate.TargetPoint(
            targets[k], targets[k + 1], targets[k + 2],
            targets[k + 3], targets[k + 4], targets[k + 5])).hashCode();
      }
      for (int r = 0; r < reps; ++r) {
        long t0 = System.nanoTime();
        for (int i = 0; i < measured; ++i) {
          int k = (i % n) * 6;
          sink += list.findValue(new Climate.TargetPoint(
              targets[k], targets[k + 1], targets[k + 2],
              targets[k + 3], targets[k + 4], targets[k + 5])).hashCode();
        }
        long t1 = System.nanoTime();
        System.out.println("BIOME_TREE_REP_NS " + ((t1 - t0) / (double) measured));
      }

      try (BufferedWriter writer = Files.newBufferedWriter(
               Path.of(outFile), StandardCharsets.UTF_8)) {
        for (int i = 0; i < n; ++i) {
          int k = i * 6;
          Holder<Biome> biome = list.findValue(new Climate.TargetPoint(
              targets[k], targets[k + 1], targets[k + 2],
              targets[k + 3], targets[k + 4], targets[k + 5]));
          String id = biome.unwrapKey()
                          .map(key -> key.identifier().toString())
                          .orElse("[unregistered]");
          writer.write(Long.toString(targets[k]) + " " +
                       Long.toString(targets[k + 1]) + " " +
                       Long.toString(targets[k + 2]) + " " +
                       Long.toString(targets[k + 3]) + " " +
                       Long.toString(targets[k + 4]) + " " +
                       Long.toString(targets[k + 5]) + " " + id + "\n");
        }
      }
      System.out.println("BIOME_TREE_BENCH_OK sink=" + sink);
      System.out.flush();
      Runtime.getRuntime().halt(0);
    } catch (Throwable failure) {
      failure.printStackTrace(System.out);
      System.out.println("BIOME_TREE_BENCH_FAILED " + failure);
      System.out.flush();
      Runtime.getRuntime().halt(1);
    }
  }
}
