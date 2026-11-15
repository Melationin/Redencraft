$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).ProviderPath
$vanilla = Join-Path $root 'validation/vanilla26_1'
$officialJar = Join-Path $vanilla 'server.jar'
$classesJar = Join-Path $vanilla 'versions/26.1/server-26.1.jar'
$expectedJarSha1 = '3872a7f07a1a595e651aef8b058dfc2bb3772f46'
$actualJarSha1 = (Get-FileHash $officialJar -Algorithm SHA1).Hash.ToLowerInvariant()
if ($actualJarSha1 -ne $expectedJarSha1) {
  throw "server jar SHA-1 $actualJarSha1 != $expectedJarSha1"
}

$names = Join-Path $root 'assets/light26_1/block_names.txt'
$source = Get-Content -Raw (Join-Path $root 'src/blocks_registry.inc')
$matches = [regex]::Matches(
  $source,
  'const\s+Block\s+[A-Za-z0-9_]+\s*\{\s*"(minecraft:[^"]+)"')
$blockNames = @($matches | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
[IO.File]::WriteAllLines($names, $blockNames, [Text.UTF8Encoding]::new($false))

$work = Join-Path $root 'build-light-validation/light-metadata-probe'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$classes = Join-Path $work 'classes'
New-Item -ItemType Directory -Force -Path $classes | Out-Null
$libs = Get-ChildItem (Join-Path $vanilla 'libraries') -Filter '*.jar' -Recurse | ForEach-Object { $_.FullName }
$cp = (@($classesJar) + @($libs)) -join ';'
$javaHome = 'C:/Program Files/Zulu/zulu-26/bin'
& "$javaHome/javac.exe" -cp $cp -d $classes (Join-Path $PSScriptRoot 'LightMetadataProbe.java')
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

$output = Join-Path $root 'assets/light26_1/block_light.tsv'
$runCp = (@($classes, $classesJar) + @($libs)) -join ';'
& "$javaHome/java.exe" -cp $runCp LightMetadataProbe $names $output
if ($LASTEXITCODE -ne 0) { throw 'LightMetadataProbe failed' }

$manifest = [ordered]@{
  jar_sha1 = $actualJarSha1
  extracted_classes_jar_sha256 = (Get-FileHash $classesJar -Algorithm SHA256).Hash.ToLowerInvariant()
  java = "$javaHome/java.exe"
  names_sha256 = (Get-FileHash $names -Algorithm SHA256).Hash.ToLowerInvariant()
  tsv_sha256 = (Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
  rows = (Get-Content $output).Count
}
$faces = Join-Path $root 'assets/light26_1/block_light_faces.tsv'
if (Test-Path $faces) {
  $manifest.faces_sha256 =
    (Get-FileHash $faces -Algorithm SHA256).Hash.ToLowerInvariant()
  $manifest.face_rows = (Get-Content $faces).Count
}
[IO.File]::WriteAllText(
  (Join-Path $root 'assets/light26_1/manifest.json'),
  ($manifest | ConvertTo-Json) + "`n",
  [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json
