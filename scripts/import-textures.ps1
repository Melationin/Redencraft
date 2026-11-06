param(
  [Parameter(Mandatory=$true)][string]$ClientJar,
  [string]$OutputDirectory=(Join-Path $PSScriptRoot '../assets/textures/block')
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$names=@('bedrock','slime_block','honey_block_bottom','honey_block_side','honey_block_top',
 'smooth_stone','white_terracotta','dead_tube_coral_fan','dead_brain_coral_fan',
 'dead_bubble_coral_fan','dead_fire_coral_fan','dead_horn_coral_fan','cobblestone',
 'obsidian','glass','piston_bottom','piston_inner','piston_side','piston_top','piston_top_sticky',
 'observer_back','observer_back_on','observer_front','observer_side','observer_top','note_block',
 'redstone_block','tnt_bottom','tnt_side','tnt_top','grass_block_top','grass_block_side',
 'grass_block_side_overlay','grass_block_snow','dirt')
foreach($color in @('white','orange','magenta','light_blue','yellow','lime','pink','gray',
 'light_gray','cyan','purple','blue','brown','green','red','black')) { $names+="${color}_wool" }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$archive=[IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $ClientJar))
try {
  foreach($name in $names) {
    $entry=$archive.GetEntry("assets/minecraft/textures/block/$name.png")
    if($null -eq $entry) { throw "Missing block texture: $name" }
    [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $OutputDirectory "$name.png"),$true)
  }
} finally { $archive.Dispose() }
Write-Output "Imported $($names.Count) textures into $OutputDirectory"
