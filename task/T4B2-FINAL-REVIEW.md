# T-4b-2 final reviewer notes (2026-11-12)

Waiting for the coder's terminal completion notification; the working-tree delivery and available evidence were inspected without rerunning CTest.

Confirmed: separate read-result ownership and bounded readiness cache; managed provider registration no longer attempts Handshake packets; monotonic player ID; Linux F_SETLK rejects simultaneous native and Java FileChannel writers in isolated probes; real terrain/tick direct tests, and official Java RegionFile/NbtIo parsed the isolated C++ output. An actual native edit survives disk write/reopen. No complete Java server restart is claimed.

Blocking before writable native entry can be accepted:

1. P1, wrong first Play packet. At src/net/session.cpp:534-536 the Configuration finish path calls syncManagedResidency before join. src/net/session.cpp:1458-1459 invokes refreshChunkView and sendChunkBatch there; refresh emits set_chunk_cache_center. join then sends login only at :581. Packet IDs in assets/protocol775/packet_ids.tsv are login=49 and set_chunk_cache_center=94. The headless smoke hardcodes 94 as the first Play packet (validation/protocol775/managed_world_smoke.py:61); tests/light_protocol_tests.cpp:559-570 only counts chunks. In original Java PlayerList.java:171 sends ClientboundLoginPacket; ClientPacketListener.java:511 constructs the level, while :2439 handles center against that level. Make syncManagedResidency data-only before join; leave view update and sending after login in join. Assert login is first, then cache center and real chunk(s); use packet mapping in the TCP smoke.
2. P1, shutdown lifetime/validation. src/net/server_main.cpp:563-564 declares manager before lock, so unwinding closes the lock before destructor fallback saves. The callbacks attached at :760+ capture connections/writer declared after manager; an exception after attachment can destroy those dependencies before manager destructor calls close/drains callbacks. Hold lock through manager destruction on every path, and explicitly detach network observers before captured objects are destroyed. Use a scoped guard or nested handling; avoid production debug output. validation/protocol775/managed_world_smoke.py:43-59 currently accepts a timed-out process killed by the script (and doesn't check returncode); successful terrain write is not evidence of normal process exit. Require zero exit for both rounds, log bounded cleanup only on failing timeout, and verify no process hangs after it prints TICKETED_WORLD_CLOSED. If it still hangs, show where shutdown is waiting, avoid broad lighting changes. No need to re-run CTest or all test suites.

The native lock, terrain disk write, and official codec evidence remain valid independently of these blockers. Persistent tickets, level.dat writes, autosave budget, entities/block entities, generation and complete vanilla restart are later stages.

## Coder follow-up: both P1 fixes verified (2026-11-12)

Play login ordering is fixed without a second resident-set copy. Configuration
completion only refreshes availableChunks_; join() emits login first, then the
cache center, then chunks. The protocol fixture asserts login is the first Play
packet and later packets include the center and both resident chunks. The TCP
smoke uses the packet mapping and asserts the same first packet.

Shutdown ordering is fixed. Normal close runs before network teardown; a guard
detaches every World callback that captures connections or NetworkWriter before
those objects leave scope on normal and exceptional paths. TicketedWorldLock is
declared before WorldChunkManager, so exception unwinding closes the manager
before releasing the world lock. Temporary lock/manager diagnostics are gone.

The smoke treats timeout as failure, requires return 0 in each round, and
checks complete/unsaved/fatal plus the restart readback. The final noninteractive
run returned 0 for two rounds:

    /home/zhddsj/.local/state/redstone-cpp/wsl-reviewfix-final4-smoke.log
    SHA-256 2a39236154bc6fec1fceede9b664dee63ec1b326e8fe986bd2fbc4400897bd78
    final redstone_vanilla_server binary
    SHA-256 d6eec6fbbdfffc8f316f7c680b65df3202a86a4eeac776db1058111466aa79a3

Focused protocol test passed with 249 packets. One earlier interactive-shell
background probe was observed stopped in Tl (job-control suspension), not in a
manager-save deadlock; the noninteractive real run printed STOPPED in both
rounds. CTest and the already accepted ticket/storage/manager/Java probes were
not rerun.

