import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.TreeMap;
import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.properties.Property;

// Read registry data only; does not start a server or create a world.
public final class BlockInventoryProbe {
  @SuppressWarnings({"rawtypes", "unchecked"})
  private static String properties(net.minecraft.world.level.block.state.BlockState state) {
    var values = new TreeMap<String, String>();
    state.getValues().forEach(v -> values.put(v.property().getName(),
        ((Property)v.property()).getName((Comparable)v.value())));
    return String.join(",", values.entrySet().stream()
        .map(e -> e.getKey() + "=" + e.getValue()).toList());
  }

  public static void main(String[] args) throws Exception {
    if (args.length != 1) throw new IllegalArgumentException("expected output path");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    if (!SharedConstants.getCurrentVersion().name().equals("26.1"))
      throw new IllegalStateException("requires official 26.1");
    var rows = new ArrayList<String>();
    for (Block block : BuiltInRegistries.BLOCK) {
      var states = block.getStateDefinition().getPossibleStates();
      long random = states.stream().filter(s -> s.isRandomlyTicking()).count();
      long entities = states.stream().filter(s -> s.hasBlockEntity()).count();
      rows.add(String.join("\t", Integer.toString(BuiltInRegistries.BLOCK.getId(block)),
          BuiltInRegistries.BLOCK.getKey(block).toString(), block.getClass().getName(),
          properties(block.defaultBlockState()), Integer.toString(states.size()),
          Long.toString(random), Long.toString(entities)));
    }
    Files.writeString(Path.of(args[0]), String.join("\n", rows) + "\n", StandardCharsets.UTF_8);
  }
}
