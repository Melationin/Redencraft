$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).ProviderPath
$vanilla = Join-Path $root 'validation/vanilla26_1'
$jar = Join-Path $vanilla 'server.jar'
$classesJar = Join-Path $vanilla 'versions/26.1/server-26.1.jar'
$sha = (Get-FileHash $jar -Algorithm SHA1).Hash.ToLowerInvariant()
if ($sha -ne '3872a7f07a1a595e651aef8b058dfc2bb3772f46') { throw 'Wrong server jar' }
$classesSha = (Get-FileHash $classesJar -Algorithm SHA256).Hash.ToLowerInvariant()
if ($classesSha -ne 'a7fed6f7d88379349e35ae0c6e9881d4484605132f6f620376a3868eea6cce52') {
  throw 'Wrong extracted server classes jar'
}
$work = Join-Path $root 'build-light-validation/block-tags-probe/classes'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$libs = Get-ChildItem (Join-Path $vanilla 'libraries') -Filter '*.jar' -Recurse | ForEach-Object FullName
$javaHome = 'C:/Program Files/Zulu/zulu-26/bin'
& "$javaHome/javac.exe" -cp ((@($classesJar) + @($libs)) -join ';') -d $work (Join-Path $PSScriptRoot 'BlockTagsProbe.java')
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }
$output = Join-Path $root 'assets/block26_1/block_tags.tsv'
& "$javaHome/java.exe" -cp ((@($work, $classesJar) + @($libs)) -join ';') BlockTagsProbe $output
if ($LASTEXITCODE -ne 0) { throw 'BlockTagsProbe failed' }
$manifest = [ordered]@{ version = '26.1'; jar_sha1 = $sha
  classes_sha256 = $classesSha
  tags_sha256 = (Get-FileHash $output -Algorithm SHA256).Hash.ToLowerInvariant()
  rows = @(Get-Content $output).Count }
[IO.File]::WriteAllText((Join-Path $root 'assets/block26_1/tags_manifest.json'),
  ($manifest | ConvertTo-Json) + "`n", [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json
