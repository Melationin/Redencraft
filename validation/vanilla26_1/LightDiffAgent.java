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

import net.minecraft.core.BlockPos;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.NoteBlock;
import net.minecraft.world.level.block.state.BlockState;

public final class LightDiffAgent {
  private static final int[][] CELLS = cells();
  private static volatile MinecraftServer server;
  private static int emittedStage = -1;
  private static int observedStage = -1;
  private static long observedTick = -1;
  private static long tickCounter = 0;

  private LightDiffAgent() {}

  public static void premain(String args, Instrumentation instrumentation) {
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
                              ClassDesc.of("LightDiffAgent"), callback,
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
      BlockState control = level.getBlockState(new BlockPos(0, 250, 0));
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
      if (tickCounter - observedTick < 2 ||
          level.getLightEngine().hasLightWork())
        return;
      var listener = level.getLightEngine().getLayerListener(LightLayer.BLOCK);
      var output = new StringBuilder();
      output.append("BLOCKLIGHT_JSON {\"stage\":")
          .append(stage)
          .append(",\"game_time\":")
          .append(level.getGameTime())
          .append(",\"block_light\":[");
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
      System.out.println("BLOCKLIGHT_FAILED " + failure);
      System.out.flush();
    }
  }

  private static int[][] cells() {
    var values = new java.util.ArrayList<int[]>();
    for (int x = -15; x <= 15; ++x)
      values.add(new int[]{x, 200, 0});
    for (int y = 198; y <= 202; ++y)
      values.add(new int[]{0, y, 0});
    for (int z = -1; z <= 1; ++z)
      values.add(new int[]{5, 200, z});
    return values.toArray(int[][]::new);
  }
}
