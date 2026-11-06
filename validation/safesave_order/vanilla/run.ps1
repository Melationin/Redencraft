$ErrorActionPreference='Stop'
$here=$PSScriptRoot
$vanilla=Resolve-Path (Join-Path $here '../../vanilla26_1')
$jar=Join-Path $vanilla 'versions/26.1/server-26.1.jar'
$libs=Get-ChildItem (Join-Path $vanilla 'libraries') -Filter '*.jar' -Recurse | ForEach-Object { $_.FullName }
$cp=(@($here, $jar)+@($libs)) -join ';'
$java='C:/Program Files/Zulu/zulu-26/bin'
& "$java/javac.exe" -cp $cp (Join-Path $here 'VanillaOrderProbe.java')
if ($LASTEXITCODE -ne 0) { throw 'javac failed' }
$output=& "$java/java.exe" -cp $cp VanillaOrderProbe
if ($LASTEXITCODE -ne 0) { throw 'probe failed' }
$line=@($output | Where-Object { $_ -match '\{"fixture_ticks"' })
if ($line.Count -ne 1) { throw 'Expected one JSON probe result' }
$json=$line[0].Substring($line[0].IndexOf('{'))
$json | ConvertFrom-Json | Out-Null
[IO.File]::WriteAllText((Join-Path $here 'vanilla-order.json'), $json + "`n", [Text.UTF8Encoding]::new($false))
Write-Output $json
