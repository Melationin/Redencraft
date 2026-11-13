import com.mojang.serialization.JsonOps;
import com.mojang.serialization.Dynamic;
import net.minecraft.SharedConstants;
import net.minecraft.nbt.CompoundTag;
import net.minecraft.nbt.NbtAccounter;
import net.minecraft.nbt.NbtIo;
import net.minecraft.nbt.NbtOps;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.level.Ticket;
import net.minecraft.server.level.TicketType;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.TicketStorage;
import java.nio.file.Files;
import java.nio.file.Path;

public final class TicketPersistenceProbe {
  private TicketPersistenceProbe() {}

  public static void main(String[] args) throws Exception {
    if (args.length != 2)
      throw new IllegalArgumentException("usage: <write|read|level> <file>");
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    Path file = Path.of(args[1]);
    if (args[0].equals("level")) {
      var root = NbtIo.readCompressed(Files.newInputStream(file), NbtAccounter.unlimitedHeap());
      var data = root.getCompoundOrEmpty("Data");
      if (data.getIntOr("DataVersion", 0) != 4786)
        throw new IllegalStateException("level DataVersion mismatch");
      System.out.println("JAVA_LEVEL_OK Time=" + data.getLongOr("Time", Long.MIN_VALUE));
      return;
    }
    if (args[0].equals("write")) {
      var storage = new TicketStorage();
      storage.addTicket(new Ticket(TicketType.FORCED, 31), new ChunkPos(-13, -16));
      var portal = new Ticket(TicketType.PORTAL, 33);
      for (int i = 0; i < 43; ++i)
        portal.decreaseTicksLeft();
      storage.addTicket(portal, new ChunkPos(-14, -16));
      var encoded = TicketStorage.CODEC.encodeStart(NbtOps.INSTANCE, storage).getOrThrow();
      var root = new CompoundTag();
      root.put("data", encoded);
      root.putInt("DataVersion", 4786);
      Files.createDirectories(file.getParent());
      NbtIo.writeCompressed(root, file);
      System.out.println("JAVA_TICKETS_WRITE_OK encoded=" + encoded);
      return;
    }
    var root = NbtIo.readCompressed(Files.newInputStream(file), NbtAccounter.unlimitedHeap());
    TicketStorage storage = TicketStorage.CODEC.parse(new Dynamic<>(NbtOps.INSTANCE, root.get("data"))).getOrThrow();
    storage.activateAllDeactivatedTickets();
    var forcedPosition = new ChunkPos(-13, -16);
    var portalPosition = new ChunkPos(-14, -16);
    int forcedLoading = storage.getTicketLevelAt(forcedPosition.pack(), false);
    int portalLoading = storage.getTicketLevelAt(portalPosition.pack(), false);
    int forcedSimulation = storage.getTicketLevelAt(forcedPosition.pack(), true);
    int portalSimulation = storage.getTicketLevelAt(portalPosition.pack(), true);
    if (forcedLoading != 31 || portalLoading != 33 ||
        forcedSimulation != 31 || portalSimulation != 33)
      throw new IllegalStateException("ticket level roundtrip mismatch");
    var forcedTicket = storage.getTickets(forcedPosition.pack()).getFirst();
    var portalTicket = storage.getTickets(portalPosition.pack()).getFirst();
    if (!forcedTicket.toString().contains("minecraft:forced") ||
        !portalTicket.toString().contains("257 ticks left"))
      throw new IllegalStateException("ticket payload mismatch: " +
          forcedTicket + " / " + portalTicket);
    System.out.println("JAVA_TICKETS_READ_OK count=2");
    System.out.println("JAVA_TICKET forced=-13,-16 loading=" + forcedLoading +
        " simulation=" + forcedSimulation + " " + forcedTicket);
    System.out.println("JAVA_TICKET portal=-14,-16 loading=" + portalLoading +
        " simulation=" + portalSimulation + " " + portalTicket);
  }
}
