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
$work = Join-Path $root 'build-light-validation/light-occlusion-probe'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$classes = Join-Path $work 'classes'
New-Item -ItemType Directory -Force -Path $classes | Out-Null
$libs = Get-ChildItem (Join-Path $vanilla 'libraries') -Filter '*.jar' -Recurse |
  ForEach-Object FullName
$compileClasspath = (@($classesJar) + @($libs)) -join ';'
$javaHome = 'C:/Program Files/Zulu/zulu-26/bin'
& "$javaHome/javac.exe" -cp $compileClasspath -d $classes `
  (Join-Path $PSScriptRoot 'LightOcclusionProbe.java')
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }

$output = Join-Path $root 'assets/light26_1/block_light_faces.tsv'
$runClasspath = (@($classes, $classesJar) + @($libs)) -join ';'
& "$javaHome/java.exe" -cp $runClasspath LightOcclusionProbe $names $output
if ($LASTEXITCODE -ne 0) { throw 'LightOcclusionProbe failed' }

$manifestPath = Join-Path $root 'assets/light26_1/manifest.json'
$manifest = Get-Content -Raw $manifestPath | ConvertFrom-Json
$manifest.jar_sha1 = $actualJarSha1
$manifest.extracted_classes_jar_sha256 =
  (Get-FileHash $classesJar -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest | Add-Member -NotePropertyName faces_sha256 -Force -NotePropertyValue `
  (Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest | Add-Member -NotePropertyName face_rows -Force -NotePropertyValue `
  (Get-Content $output).Count
[IO.File]::WriteAllText(
  $manifestPath,
  ($manifest | ConvertTo-Json) + "`n",
  [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json
