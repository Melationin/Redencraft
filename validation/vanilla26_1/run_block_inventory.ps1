$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).ProviderPath
$vanilla = Join-Path $root 'validation/vanilla26_1'
$officialJar = Join-Path $vanilla 'server.jar'
$classesJar = Join-Path $vanilla 'versions/26.1/server-26.1.jar'
$sha = (Get-FileHash $officialJar -Algorithm SHA1).Hash.ToLowerInvariant()
if ($sha -ne '3872a7f07a1a595e651aef8b058dfc2bb3772f46') { throw 'Wrong server jar' }
$work = Join-Path $root 'build-light-validation/block-inventory-probe/classes'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$libs = Get-ChildItem (Join-Path $vanilla 'libraries') -Filter '*.jar' -Recurse | ForEach-Object FullName
$javaHome = 'C:/Program Files/Zulu/zulu-26/bin'
$compileClasspath = (@($classesJar) + @($libs)) -join ';'
& "$javaHome/javac.exe" -cp $compileClasspath -d $work (Join-Path $PSScriptRoot 'BlockInventoryProbe.java')
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }
$outputDir = Join-Path $root 'assets/block26_1'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$output = Join-Path $outputDir 'block_inventory.tsv'
$runClasspath = (@($work, $classesJar) + @($libs)) -join ';'
& "$javaHome/java.exe" -cp $runClasspath BlockInventoryProbe $output
if ($LASTEXITCODE -ne 0) { throw 'BlockInventoryProbe failed' }
$rows = @(Get-Content $output)
$protocol = @(Get-Content (Join-Path $root 'assets/protocol775/blocks.tsv'))
if ($rows.Count -ne $protocol.Count) { throw 'inventory and protocol block counts differ' }
for ($i = 0; $i -lt $rows.Count; ++$i) {
  $fields = $rows[$i].Split("`t")
  if ($fields.Count -ne 7 -or "$($fields[0])`t$($fields[1])" -ne $protocol[$i]) {
    throw "inventory and protocol registry differ at row $i"
  }
}
$manifest = [ordered]@{
  version = '26.1'; jar_sha1 = $sha
  extracted_classes_jar_sha256 = (Get-FileHash $classesJar -Algorithm SHA256).Hash.ToLowerInvariant()
  inventory_sha256 = (Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
  rows = $rows.Count
  columns = @('block_id', 'name', 'java_class', 'default_properties', 'state_count', 'random_tick_state_count', 'block_entity_state_count')
}
[IO.File]::WriteAllText((Join-Path $outputDir 'manifest.json'),
  ($manifest | ConvertTo-Json) + "`n", [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json
