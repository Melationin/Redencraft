# 26.1 / protocol 775 数据导出与解码验证

`ExportProtocol.java` 直接调用本地**未修改的 Mojang 26.1 服务端 jar**。不运行 Minecraft 服务端、不创建世界、不启动客户端、不操作桌面。动态注册表由 `RegistryDataLoader` 从原版 vanilla 数据包读取；静态 tags 按 `WorldLoader` 的次序加载并 apply。没有额外数据包或实验功能。

运行环境沿用 `validation/vanilla26_1` 的官方 server bundler 提取结果和 Zulu Java 26：

```powershell
.\validation\protocol775\export.ps1
.\validation\protocol775\export.ps1 -VerifyPlayPackets C:\absolute\packet-records.bin
```

启用 `REDSTONE_BUILD_VANILLA_SERVER` 并构建后，可以从协议测试重建 C++ 发包记录：

```powershell
$env:REDSTONE_PLAY_FIXTURE = "$PWD/build-jemalloc/play-packets.bin"
./build-jemalloc/Release/protocol_tests.exe assets/protocol775
Remove-Item Env:REDSTONE_PLAY_FIXTURE
./validation/protocol775/export.ps1 -VerifyPlayPackets "$PWD/build-jemalloc/play-packets.bin"
```

额外的 `socket_test.py --server <exe>` 启动有限时长的无界面 C++ 服务端，用原始 TCP 验证 status、离线登录、配置、区块、放置/破坏与预测确认。此脚本没有运行 Minecraft 客户端，也没有操作桌面。

脚本会检查版本确实为 26.1 / 775，并重新生成 `assets/protocol775`。源 bundler SHA1、实际类所在内部 jar SHA256、所有资产 SHA256 记录在 `manifest.json`；需要更新版本时必须重新导出，不能只修改 protocol 字段。

## 资产格式

所有 TSV 使用 UTF-8、TAB 分列、无表头；namespaced identifier 均带 `minecraft:`。数值采用十进制。

| 文件 | 列 / 格式 |
|---|---|
| `block_states.tsv` | state ID、block identifier、逗号分隔的 `key=value` 属性（属性名排序）；无属性时第三列为空 |
| `state_flags.tsv` | state ID、isAir、hasFluid、blocksMotion、isLeaves；布尔为 0/1，leaves 在 tags apply 后取得 |
| `blocks.tsv` | block registry ID、block identifier；方块事件使用这个 ID，而非 state ID |
| `items.tsv` | item registry ID、item identifier |
| `entities.tsv` | entity type registry ID、entity type identifier |
| `block_entity_types.tsv` | block entity type registry ID、identifier |
| `biomes.tsv` | biome registry ID、identifier |
| `registry_ids.tsv` | registry identifier、entry ID、entry identifier；也含不向客户端发送的世界生成注册表，仅供索引 |
| `explosion_effects.tsv` | 原版爆炸粒子和声音的名称、注册 ID |
| `packet_ids.tsv` | phase、flow、packet type identifier、packet ID；phase 为 handshake/status/login/configuration/play，flow 为 clientbound/serverbound |
| `entity_data_serializers.tsv` | serializer ID、Java 静态常量名；从真实 serializer 注册表取得，不能从字段声明顺序猜测 |
| `empty_section.bin` | 真实 `LevelChunkSection.write` 的空 section；8 字节 `00 00 00 00 00 00 00 28`，最后一个 VarInt 为 plains biome ID 40 |
| `configuration.bin` | 连续记录，每条为 `u32 BE byteLength`，后接 `VarInt packetId + packetPayload`；无压缩、无 TCP VarInt length |

`configuration.bin` 含 30 个包：enabled features、28 个 synchronized registry、update tags。注册表条目包含完整 NBT 内容，按 accepted known packs 为空构造，客户端无需预置的数据包缓存。服务端自行处理 select_known_packs 握手，并在这些包之后发送 finish_configuration。不能把配置包用 play 阶段 packet ID 发送。

当前 overworld dimension type ID 为 0、plains biome ID 为 40。区块适用原版 overworld 高度 -64..319（24 section）；空 section 的格式包含 `fluidCount`，且 paletted data 的 long 数组无长度前缀。

## 验证边界

导出时每条配置包均由真实 `ConfigurationProtocols.CLIENTBOUND` codec 编码后再解码，并检查无剩余字节。`VerifyPackets.java` 读取与 configuration.bin 相同的记录格式，使用真实 `GameProtocols.CLIENTBOUND_TEMPLATE`、完整原版注册表上下文逐包解码。遇到 chunk 包还会用原版 `LevelChunkSection.read` 解码全部 24 section，检查 section buffer 无剩余字节。错误会指出记录序号和 packet ID 首字节。

该检查验证网络编码结构和区块 palette 解析；不证明客户端渲染、资源加载、实际连接流程或游戏机制与原版一致。没有启动真实客户端测试。26.1.2 与 26.1 的协议号相同，也仍需独立验证静态 ID 和实际连接兼容性；26.2 不在此资产支持范围。
