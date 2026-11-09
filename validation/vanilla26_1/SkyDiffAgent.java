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
import java.security.ProtectionDomain;
import java.util.concurrent.CompletableFuture;

import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ThreadedLevelLightEngine;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.NoteBlock;
import net.minecraft.world.level.block.state.BlockState;

public final class SkyDiffAgent {
  private static final int[][] CELLS = cells();
  private static volatile MinecraftServer server;
  private static int emittedStage = -1;
  private static int observedStage = -1;
  private static int scheduledStage = -1;
  private static long observedTick = -1;
  private static long tickCounter = 0;

  private SkyDiffAgent() {}

  public static void premain(String args, Instrumentation instrumentation) {
    System.err.println("LIGHT_AGENT premain");
    instrumentation.addTransformer(new ClassFileTransformer() {
      @Override
      public byte[] transform(Module module, ClassLoader loader, String name,
                              Class<?> previous, ProtectionDomain domain,
                              byte[] bytes) {
        String method;
        String hook;
        int slot;
        if (name.equals("net/minecraft/server/MinecraftServer")) {
          method = "tickServer";
          hook = "serverTick";
          slot = 0;
        } else {
          return null;
        }
        System.err.println("LIGHT_TRANSFORM " + name + " " + method);
        final String callback = hook;
        final int argument = slot;
        var classFile = ClassFile.of();
        return classFile.transformClass(classFile.parse(bytes),
            ClassTransform.transformingMethodBodies(
                code -> code.methodName().equalsString(method),
                new CodeTransform() {
                  @Override
                  public void accept(java.lang.classfile.CodeBuilder builder,
                                     CodeElement element) {
                    if (element instanceof ReturnInstruction) {
                      builder.aload(argument)
                          .invokestatic(
                              ClassDesc.of("SkyDiffAgent"), callback,
                              MethodTypeDesc.of(ConstantDescs.CD_void,
                                                ConstantDescs.CD_Object));
                    }
                    builder.with(element);
                  }
                }));
      }
    }, false);
  }

  public static void serverTick(Object value) {
    server = (MinecraftServer) value;
    ++tickCounter;
    tryEmit();
  }

  private static void tryEmit() {
    MinecraftServer current = server;
    if (current == null)
      return;
    try {
      ServerLevel level = current.overworld();
      if (level == null)
        return;
      BlockState control = level.getBlockState(new BlockPos(-8, 250, -8));
      if (!control.is(Blocks.NOTE_BLOCK))
        return;
      int stage = control.getValue(NoteBlock.NOTE);
      if (stage <= 0 || stage == emittedStage)
        return;
      if (stage != observedStage) {
        observedStage = stage;
        observedTick = tickCounter;
        return;
      }
      if (tickCounter - observedTick < 2 || stage == scheduledStage)
        return;
      if (!(level.getLightEngine() instanceof ThreadedLevelLightEngine light))
        return;
      scheduledStage = stage;
      CompletableFuture<?>[] fences = new CompletableFuture<?>[4];
      int[][] chunks = {{0, 0}, {-1, 0}, {0, -1}, {-1, -1}};
      for (int i = 0; i < chunks.length; ++i)
        fences[i] = light.waitForPendingTasks(chunks[i][0], chunks[i][1]);
      System.out.println("SKY_FENCE stage=" + stage + " chunks=4");
      System.out.flush();
      Object currentObject = current;
      CompletableFuture.allOf(fences).thenRun(
          () -> ((MinecraftServer) currentObject)
                    .execute(() -> emitStage(stage)));
    } catch (Throwable failure) {
      failure.printStackTrace(System.out);
      System.out.println("SKYLIGHT_FAILED " + failure);
      System.out.flush();
    }
  }

  private static void emitStage(int stage) {
    MinecraftServer current = server;
    if (current == null)
      return;
    try {
      ServerLevel level = current.overworld();
      if (level == null)
        return;
      var listener = level.getLightEngine().getLayerListener(LightLayer.SKY);
      var output = new StringBuilder();
      output.append("SKYLIGHT_JSON {\"stage\":")
          .append(stage)
          .append(",\"game_time\":")
          .append(level.getGameTime())
          .append(",\"sky_light\":[");
      for (int i = 0; i < CELLS.length; ++i) {
        if (i != 0)
          output.append(',');
        output.append(listener.getLightValue(
            new BlockPos(CELLS[i][0], CELLS[i][1], CELLS[i][2])));
      }
      output.append("]}");
      System.out.println(output);
      System.out.flush();
      emittedStage = stage;
    } catch (Throwable failure) {
      failure.printStackTrace(System.out);
      System.out.println("SKYLIGHT_FAILED " + failure);
      System.out.flush();
    }
  }

  private static int[][] cells() {
    var values = new java.util.ArrayList<int[]>();
    values.add(new int[]{8, 200, 8});
    values.add(new int[]{0, 200, 8});
    values.add(new int[]{15, 200, 8});
    values.add(new int[]{16, 200, 8});
    values.add(new int[]{8, 200, 0});
    values.add(new int[]{8, 200, 15});
    values.add(new int[]{8, 200, 16});
    values.add(new int[]{8, 202, 8});
    values.add(new int[]{8, 203, 8});
    values.add(new int[]{8, 320, 8});
    values.add(new int[]{0, -62, 0});
    values.add(new int[]{0, -61, 0});
    values.add(new int[]{0, -65, 0});
    values.add(new int[]{0, -60, 0});
    values.add(new int[]{0, 100, 0});
    return values.toArray(int[][]::new);
  }
}
