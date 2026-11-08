import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

import net.minecraft.SharedConstants;
import java.util.Comparator;

import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.resources.Identifier;

public final class LightMetadataProbe {
  private LightMetadataProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 2) {
      throw new IllegalArgumentException("usage: LightMetadataProbe <names> <output>");
    }
    Set<String> wanted = new HashSet<>();
    for (String line : Files.readAllLines(Path.of(args[0]), StandardCharsets.UTF_8)) {
      line = line.strip();
      if (!line.isEmpty()) wanted.add(line);
    }
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();

    var rows = new ArrayList<String>();
    for (Block block : BuiltInRegistries.BLOCK) {
      Identifier id = BuiltInRegistries.BLOCK.getKey(block);
      if (!wanted.contains(id.toString())) continue;
      for (BlockState state : block.getStateDefinition().getPossibleStates()) {
        TreeMap<String, String> properties = new TreeMap<>();
        state.getValues().forEach(value ->
            properties.put(value.property().getName(), propertyValue(value)));
        StringJoinerLite props = new StringJoinerLite();
        for (var entry : properties.entrySet())
          props.add(entry.getKey() + "=" + entry.getValue());
        rows.add(String.join("\t", id.toString(), props.value(),
            Integer.toString(state.getLightEmission()),
            Integer.toString(state.getLightDampening()),
            state.canOcclude() ? "1" : "0",
            state.useShapeForLightOcclusion() ? "1" : "0",
            state.propagatesSkylightDown() ? "1" : "0"));
      }
    }
    rows.sort(Comparator.naturalOrder());
    Files.writeString(Path.of(args[1]), String.join("\n", rows) + "\n",
        StandardCharsets.UTF_8);
  }

  @SuppressWarnings({"rawtypes", "unchecked"})
  private static String propertyValue(Property.Value<?> value) {
    Property property = value.property();
    return property.getName(value.value());
  }

  private static final class StringJoinerLite {
    private final StringBuilder value = new StringBuilder();
    void add(String text) {
      if (!value.isEmpty()) value.append(',');
      value.append(text);
    }
    String value() { return value.toString(); }
  }
}
