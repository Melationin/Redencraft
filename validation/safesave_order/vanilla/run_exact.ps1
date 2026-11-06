$ErrorActionPreference = 'Stop'
$here=$PSScriptRoot
$base=(Resolve-Path (Join-Path $here '../../vanilla26_1')).Path
$cp=(@((Join-Path $base 'versions/26.1/server-26.1.jar'))+@(Get-ChildItem (Join-Path $base 'libraries') -Recurse -Filter '*.jar' | ForEach-Object FullName)) -join ';'
$java='C:/Program Files/Zulu/zulu-26/bin'
& "$java/javac.exe" -cp $cp (Join-Path $here 'ExactAgent.java')
if($LASTEXITCODE -ne 0){throw 'agent compilation failed'}
$classes=Get-ChildItem -LiteralPath $here -Filter 'ExactAgent*.class' | ForEach-Object Name
Push-Location $here
try { & "$java/jar.exe" cfm exact-agent.jar EXACT-MANIFEST.MF @classes; if($LASTEXITCODE -ne 0){throw 'agent jar failed'} } finally { Pop-Location }
python (Join-Path $here 'run_exact_server.py')
if($LASTEXITCODE -ne 0){throw 'server probe failed'}
