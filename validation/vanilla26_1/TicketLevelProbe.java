import net.minecraft.server.level.*;
import net.minecraft.SharedConstants;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.TicketStorage;
import net.minecraft.world.level.chunk.status.ChunkPyramid;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.chunk.status.ChunkStep;

public final class TicketLevelProbe {
  private TicketLevelProbe() {}

  private static void type(String name, TicketType type) {
    System.out.printf("TYPE\t%s\t%d\t%d\t%s%s%s%s%s%n", name, type.timeout(), type.flags(),
        type.persist() ? "P" : "-", type.doesLoad() ? "L" : "-",
        type.doesSimulate() ? "S" : "-", type.shouldKeepDimensionActive() ? "K" : "-",
        type.canExpireIfUnloaded() ? "X" : "-");
  }

  public static void main(String[] args) throws Exception {
    SharedConstants.tryDetectVersion();
    Bootstrap.bootStrap();
    ChunkStep full = ChunkPyramid.GENERATION_PYRAMID.getStepTo(ChunkStatus.FULL);
    System.out.printf("CHUNK_LEVEL\t%d\t%d%n", ChunkLevel.MAX_LEVEL,
        ChunkLevel.RADIUS_AROUND_FULL_CHUNK);
    for (int distance = 0; distance <= ChunkLevel.RADIUS_AROUND_FULL_CHUNK; ++distance)
      System.out.printf("FULL_DEP\t%d\t%s%n", distance,
          ChunkLevel.getStatusAroundFullChunk(distance).getName());
    for (int level : new int[]{0, 30, 31, 32, 33, 34, 40, 41, 42})
      System.out.printf("FULL_STATUS\t%d\t%s%n", level,
          ChunkLevel.fullStatus(level).name());
    for (ChunkStatus status : ChunkStatus.getStatusList())
      System.out.printf("STATUS_LEVEL	%s	%d%n", status.getName(),
          ChunkLevel.byStatus(status));

    type("player_spawn", TicketType.PLAYER_SPAWN);
    type("spawn_search", TicketType.SPAWN_SEARCH);
    type("dragon", TicketType.DRAGON);
    type("player_loading", TicketType.PLAYER_LOADING);
    type("player_simulation", TicketType.PLAYER_SIMULATION);
    type("forced", TicketType.FORCED);
    type("portal", TicketType.PORTAL);
    type("ender_pearl", TicketType.ENDER_PEARL);
    type("unknown", TicketType.UNKNOWN);

    Ticket timed = new Ticket(TicketType.UNKNOWN, 42);
    System.out.printf("TIMEOUT\tbefore=%s%n", timed.isTimedOut());
    timed.decreaseTicksLeft();
    System.out.printf("TIMEOUT\tzero=%s\t%s%n", timed.isTimedOut(), timed);
    timed.decreaseTicksLeft();
    System.out.printf("TIMEOUT\tminus=%s\t%s%n", timed.isTimedOut(), timed);

    TicketStorage storage = new TicketStorage();
    System.out.printf("NO_TICKET_SOURCE\t%d\t%d%n",
        storage.getTicketLevelAt(new ChunkPos(0, 0).pack(), false),
        storage.getTicketLevelAt(new ChunkPos(0, 0).pack(), true));
    SimulationChunkTracker simulation = new SimulationChunkTracker(storage);
    System.out.printf("SIM_DEFAULT\t%d%n", simulation.getLevel(new ChunkPos(0, 0)));

    ChunkPos a = new ChunkPos(-2, -3);
    ChunkPos b = new ChunkPos(-1, -2);
    boolean addedA = storage.addTicket(a.pack(), new Ticket(TicketType.FORCED, 31));
    boolean addedB = storage.addTicket(b.pack(), new Ticket(TicketType.FORCED, 30));
    simulation.runAllUpdates();
    System.out.printf("SIM_NEG\t%d\t%d\t%d%n", simulation.getLevel(a),
        simulation.getLevel(b), simulation.getLevel(new ChunkPos(-1, -3)));
    simulation.runAllUpdates();
    System.out.printf("SIM_NO_CHANGE_QUEUE\t%d%n", simulation.getQueueSize());
    boolean removedB = storage.removeTicket(b.pack(), new Ticket(TicketType.FORCED, 30));
    simulation.runAllUpdates();
    System.out.printf("SIM_REMOVE\t%d\t%d\t%d\t%d\t%d%n", addedA ? 1 : 0,
        addedB ? 1 : 0, removedB ? 1 : 0, simulation.getLevel(a),
        simulation.getLevel(b));
    storage.removeTicket(a.pack(), new Ticket(TicketType.FORCED, 31));
    simulation.runAllUpdates();
    System.out.printf("SIM_CLEAR\t%d%n", simulation.getLevel(a));

    Ticket portal = new Ticket(TicketType.PORTAL, 7);
    boolean addedPortal = storage.addTicket(a.pack(), portal);
    portal.decreaseTicksLeft();
    boolean renewed = storage.addTicket(a.pack(), new Ticket(TicketType.PORTAL, 7));
    Ticket stored = storage.getTickets(a.pack()).getFirst();
    System.out.printf("RENEW\t%d\t%d\t%s%n", addedPortal ? 1 : 0,
        renewed ? 1 : 0, stored);
    System.out.printf("SOURCE_LEVELS\t%d\t%d\t%d%n",
        storage.getTicketLevelAt(a.pack(), false),
        storage.getTicketLevelAt(a.pack(), true), storage.getTickets(a.pack()).size());
    boolean removedPortal = storage.removeTicket(a.pack(), new Ticket(TicketType.PORTAL, 7));
    System.out.printf("REMOVE_EXACT\t%d\t%d%n", removedPortal ? 1 : 0,
        storage.getTickets(a.pack()).size());

    TicketStorage eventStorage = new TicketStorage();
    eventStorage.setLoadingChunkUpdatedListener((key, level, decreased) ->
        System.out.printf("LOAD_EVENT\t%d\t%d\t%d%n", key, level,
            decreased ? 1 : 0));
    eventStorage.setSimulationChunkUpdatedListener((key, level, decreased) ->
        System.out.printf("SIM_EVENT\t%d\t%d\t%d%n", key, level,
            decreased ? 1 : 0));
    ChunkPos eventPos = new ChunkPos(6, -9);
    eventStorage.addTicket(eventPos.pack(), new Ticket(TicketType.FORCED, 31));
    eventStorage.addTicket(eventPos.pack(), new Ticket(TicketType.PORTAL, 7));
    eventStorage.addTicket(eventPos.pack(), new Ticket(TicketType.PLAYER_LOADING, 20));
    eventStorage.addTicket(eventPos.pack(), new Ticket(TicketType.PLAYER_SIMULATION, 10));
    eventStorage.addTicket(eventPos.pack(), new Ticket(TicketType.PORTAL, 7));
    eventStorage.removeTicket(eventPos.pack(), new Ticket(TicketType.PORTAL, 7));
  }
}
