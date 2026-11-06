param([string]$VerifyPlayPackets = '')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$vanilla = Resolve-Path (Join-Path $here '../vanilla26_1')
$jar = Join-Path $vanilla 'versions/26.1/server-26.1.jar'
$libs = Get-ChildItem (Join-Path $vanilla 'libraries') -Filter '*.jar' -Recurse | ForEach-Object { $_.FullName }
$classes = Join-Path $here 'classes'
New-Item -ItemType Directory -Force $classes | Out-Null
$cp = (@($classes, $jar) + @($libs)) -join ';'
$java = 'C:/Program Files/Zulu/zulu-26/bin'
& "$java/javac.exe" -cp $cp -d $classes (Join-Path $here 'ExportProtocol.java') (Join-Path $here 'VerifyPackets.java')
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }
$arguments = @('-cp', $cp, 'ExportProtocol', (Join-Path $here '../../assets/protocol775'))
if ($VerifyPlayPackets) { $arguments += (Resolve-Path $VerifyPlayPackets).Path }
& "$java/java.exe" @arguments
if ($LASTEXITCODE -ne 0) { throw 'protocol export failed' }
$assetDir = Resolve-Path (Join-Path $here '../../assets/protocol775')
$hashes = [ordered]@{}
Get-ChildItem -LiteralPath $assetDir -File | Where-Object { $_.Name -ne 'manifest.json' } | Sort-Object Name | ForEach-Object {
    $hashes[$_.Name] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
}
$manifest = [ordered]@{
    version = '26.1'; protocol = 775; data_version = 4786
    source = 'Official unmodified Mojang dedicated server, vanilla pack, no experiments or external datapacks'
    bundler_sha1 = (Get-FileHash -LiteralPath (Join-Path $vanilla 'server.jar') -Algorithm SHA1).Hash.ToLowerInvariant()
    server_inner_sha256 = (Get-FileHash -LiteralPath $jar -Algorithm SHA256).Hash.ToLowerInvariant()
    format = 'configuration.bin records: u32 big-endian byte length, VarInt packet id + payload; no TCP framing/compression'
    configuration = 'Enabled features, all full-content synchronized registries (known packs empty), complete vanilla tags; finish_configuration sent separately'
    hashes_sha256 = $hashes
}
[IO.File]::WriteAllText((Join-Path $assetDir 'manifest.json'), ($manifest | ConvertTo-Json -Depth 5) + "`n", [Text.UTF8Encoding]::new($false))
