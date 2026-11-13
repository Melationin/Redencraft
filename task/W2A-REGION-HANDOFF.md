
## W-2a 独立 RegionFile 单槽读写

新增：

- include/redstone/region.hpp
- src/region.cpp
- tests/region_tests.cpp
- validation/vanilla26_1/RegionInteropProbe.java
- CMakeLists.txt 加入 redstone_core/region 与 region_tests；测试使用仓库外隔离 scratch 路径。

API 与定位：

- locateChunk(regionDirectory, chunkX, chunkZ) 使用 floorDiv32/floorMod32，得到 r.<regionX>.<regionZ>.mca 与 slot=localZ*32+localX；负坐标不依赖 Java 的截断除法。
- readChunkNbt 返回 Found/Missing/Corrupt/Unsupported 和原始解压 NBT。它只读取 8 KiB header、目标 4-byte location、目标扇区 header/payload；外部流仅读目标 .mcc。不会把整个 region 或 unrelated payload 读入内存。
- read 支持 compression 1 gzip、2 deflate、3 none、4 LZ4Block stream；128 标志的 .mcc 外部流。gzip/deflate复用 nbt::decompress 的 zlib 安全限流；LZ4Block 采用与现有 storage reader 一致的块检查、长度/上限、XXH32 校验。未知 compression 明确 Unsupported，越界/截断/CRC/校验失败明确 Corrupt，header/slot 缺失明确 Missing。

写入：

- 使用现有 raw decompressed NBT bytes，不再造/parse chunk NBT。写 compression 2（26.1 默认 deflate）。
- 读取 8 KiB header 后从所有 slot（含目标旧 slot）建立占用图；若两个已存 extent 重叠则拒绝写入。目标旧 sectors 在新 header 提交前始终属于旧数据，不能被新载荷原地覆盖。新 payload 由 first-fit 分配未占用扇区；当前仍保留旧 extent 时通常追加，后续写入可复用前次替换留下的未引用空洞。
- 单槽最小 payload 1 sector，>=256 sectors 使用 1-sector external stub + c.x.z.mcc，符合 RegionFile.write。写入只改一个 location/timestamp 项；其他表项和 payload 不重写。
- 每次写不复制整个 region；被替换的旧扇区成为未引用空洞，后续 first-fit 写入可以复用。若本次没有足够空洞，才在尾部扩展。由于目标旧扇区在新 header 前不可复用，连续覆盖同一大槽仍可能增长；不是零增长或恒定大小。
- 外部临时文件由 RAII 清理；target .mcc 已存在但不是普通文件时，在改 header 前抛出明确错误，旧 internal/external slot 保持可读。普通 rename 失败会尝试写回旧双表；恢复成功则旧槽完全保留，恢复失败则抛出说明 header 可能已改变。已成功提交后的旧 .mcc 删除失败不回滚已提交槽，返回 cleanupPending/warning。
- writeAt/createRegionFile 显式检查 flush 与 close 错误；头写异常不会因 ofstream/fstream 析构而静默成功。扩尾使用 resize_file，并检查 error_code。
- 没有 fsync/父目录 fsync，因此不声称进程崩溃或断电原子性。若进程在申请 header 前崩溃，新扇区是未引用 orphan；header 写后旧扇区是未引用 slack。

直接 C++ 验证：

    region tests passed

覆盖 floorDiv32 负坐标、两个独立槽和较大 sector run、单个目标覆盖后相邻 payload 字节不变/表项不变、缺失槽、未知压缩、无效长度、deflate 往返、超过 256 sectors 的 .mcc 往返、缺失 .mcc、外置到内置切换及旧 .mcc 清理。另覆盖非 sector 对齐但 record 完整的原版尾、短于完整双表头的文件、target .mcc 为目录时旧槽/邻槽保留、重叠 extent 拒绝、替换后空洞复用且文件不增长。只写仓库外 scratch。

官方 26.1 互操作证据（未启动服务器/客户端）：

- Java 26.1 RegionFile 直接读取 C++ 写的 r.-1.-1.mca 两个槽，返回 JAVA_READ_CPP -1 -1 -11 cpp-w2a 与 JAVA_READ_CPP -1 0 -10 cpp-neighbor，并以 JAVA_READ_OK 结束。
- JAVA_WRITE_OK 后 C++ 直接解压 Java 写的同一负 region 槽，region tests passed。
- Probe 依据 RegionFile.java:50-72,109-200,206-331 与 RegionFileVersion.java:22-45；只验证 raw 压缩流和 header/Raw NBT round-trip，不验证完整 SerializableChunkData。

日志：

- C++ build：/home/zhddsj/.local/state/redstone-cpp/wsl-w2a-region-20261112-build-review6.log，SHA-256 59324e29f23d68764c863fe201b10255a16449901157a83d0dbfdfaf350b64a0。
- C++ tests：/home/zhddsj/.local/state/redstone-cpp/wsl-w2a-region-20261112-tests-review3.log，SHA-256 6ab3390c58e39bee2b83cf611e56461469be0ebebfdd50b6b6377a34208667e0。
- Interop combined：/home/zhddsj/.local/state/redstone-cpp/wsl-w2a-region-20261112-interop-review.log，SHA-256 5dd6fd99c4b70f4139959d0fc49721bbeaef29373eb626a3a295176fbe103b80。
- Java reads C++：/home/zhddsj/.local/state/redstone-cpp/wsl-w2a-region-java-20261112/java-read-cpp.log，SHA-256 3f0d77e15281ac85c645ea11c43293b0191eea1ddf684bae780a668aeed7dd24。
- C++ reads Java：/home/zhddsj/.local/state/redstone-cpp/wsl-w2a-region-java-20261112/cpp-read-java.log，SHA-256 6ab3390c58e39bee2b83cf611e56461469be0ebebfdd50b6b6377a34208667e0。
- Source hashes：header fc7fe5ce6a6099b1888061aac59f12f95829053d3c0b7efb1025deacb88dea30；source 73d6993bd042b17dc8ab8fa47dd7de54cc3647a4f78345bc78379fd545411602；tests e97de44f4b6282ce43a29abb1990c5be649b823ca6c1215f32073225fdbf4b11；probe ae143ffbbfae529c1dcc4b63bbe9537f86caad87930d8171fecd4c0e68df3b55。

未接在线 World、实际卸载、完整 SerializableChunkData、Anvil 原存档；未修改原存档或光照实现。未跑 CTest、无关测试、正式 benchmark，未 commit/push。下一步只能由 reviewer 放行后再接 T-2b/T-3 或在线 I/O。
