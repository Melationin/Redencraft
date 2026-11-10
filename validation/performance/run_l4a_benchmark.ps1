param(
  [string]$Baseline = "build-l1-baseline/redstone_tick_benchmark.baseline.exe",
  [string]$Candidate = "build-f7-nmake/redstone_tick_benchmark.exe",
  [string]$Workload = "build-review-f2/workload.litematic",
  [string]$OutputDir = "",
  [int]$Ticks = 1000,
  [int]$Groups = 5
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$baselinePath = (Resolve-Path (Join-Path $root $Baseline)).Path
$candidatePath = (Resolve-Path (Join-Path $root $Candidate)).Path
$workloadPath = (Resolve-Path (Join-Path $root $Workload)).Path
if (!$OutputDir) {
  $OutputDir = Join-Path $root ("build-l4a/" + (Get-Date -Format "yyyyMMdd-HHmmss"))
} elseif (![IO.Path]::IsPathRooted($OutputDir)) {
  $OutputDir = Join-Path $root $OutputDir
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

$expectedBaseline = "260273D92C323E983FE4AA92B03E9066E36F74CFE6FCE854462CC039EF17A907"
$expectedWorkload = "7AD76AC215039E66594A81FEB45DAE70896827C768328133D19B86D3E8D91E52"
$baselineHash = (Get-FileHash $baselinePath -Algorithm SHA256).Hash
$workloadHash = (Get-FileHash $workloadPath -Algorithm SHA256).Hash
$candidateHash = (Get-FileHash $candidatePath -Algorithm SHA256).Hash
if ($baselineHash -ne $expectedBaseline) { throw "baseline hash mismatch: $baselineHash" }
if ($workloadHash -ne $expectedWorkload) { throw "workload hash mismatch: $workloadHash" }

function Invoke-Benchmark {
  param(
    [string]$Name,
    [string]$Executable,
    [string[]]$Arguments,
    [long]$Affinity
  )
  $psi = [Diagnostics.ProcessStartInfo]::new()
  $psi.FileName = $Executable
  foreach ($argument in $Arguments) { $psi.ArgumentList.Add([string]$argument) }
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  $process = [Diagnostics.Process]::new()
  $process.StartInfo = $psi
  if (!$process.Start()) { throw "failed to start $Name" }
  try { $process.ProcessorAffinity = [IntPtr]$Affinity } catch { $process.Kill(); throw }
  try { $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::High } catch {}
  $actualAffinity = $process.ProcessorAffinity.ToInt64()
  $actualPriority = $process.PriorityClass.ToString()
  $stdoutTask = $process.StandardOutput.ReadToEndAsync()
  $stderrTask = $process.StandardError.ReadToEndAsync()
  $process.WaitForExit()
  $stdout = $stdoutTask.Result
  $stderr = $stderrTask.Result
  $stdoutPath = Join-Path $OutputDir "$Name.log"
  $stderrPath = Join-Path $OutputDir "$Name.err"
  [IO.File]::WriteAllText($stdoutPath, $stdout)
  [IO.File]::WriteAllText($stderrPath, $stderr)
  if ($process.ExitCode -ne 0) {
    throw "$Name failed with exit $($process.ExitCode); see $stderrPath"
  }
  $meta = [ordered]@{
    name = $Name
    executable = $Executable
    arguments = $Arguments
    requested_process_affinity = ("0x{0:x}" -f $Affinity)
    actual_process_affinity = ("0x{0:x}" -f $actualAffinity)
    priority = $actualPriority
    exit_code = $process.ExitCode
    stdout = $stdoutPath
    stderr = $stderrPath
  }
  [IO.File]::WriteAllText((Join-Path $OutputDir "$Name.json"), ($meta | ConvertTo-Json))
  return [pscustomobject]@{ Name = $Name; Text = $stdout; Meta = $meta }
}

function Numeric($Text, [string]$Pattern) {
  $match = [regex]::Match($Text, $Pattern)
  if (!$match.Success) { return $null }
  return [double]::Parse($match.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Parse-Run($Run) {
  $text = $Run.Text
  $total = [regex]::Match($text, "TOTAL ticks=(\d+) total_ms=([0-9.]+) mean_ms=([0-9.]+) median_ms=([0-9.]+) p95_ms=([0-9.]+)(?: p99_ms=([0-9.]+) max_ms=([0-9.]+) over50=(\d+))?")
  if (!$total.Success) { throw "cannot parse TOTAL from $($Run.Name)" }
  $final = [regex]::Match($text, "FINAL .*").Value
  $lightTick = [regex]::Match($text, "LIGHT_TICK .*")
  $lightDrain = [regex]::Match($text, "LIGHT_DRAIN .*")
  $lightInitial = [regex]::Match($text, "LIGHT_INITIAL .*")
  # Diagnostic light fingerprints deliberately share the FINGERPRINT suffix.
  # Only the four simulation checkpoints participate in simulation equality.
  $fingerprints = [regex]::Matches($text, "(?m)^FINGERPRINT tick=\d+ blocks=.*") | ForEach-Object { $_.Value }
  $lightFingerprints = [regex]::Matches($text, "(?m)^LIGHT_FINGERPRINT .*") | ForEach-Object { $_.Value }
  return [ordered]@{
    name = $Run.Name
    mean_ms = [double]::Parse($total.Groups[3].Value, [Globalization.CultureInfo]::InvariantCulture)
    median_ms = [double]::Parse($total.Groups[4].Value, [Globalization.CultureInfo]::InvariantCulture)
    p95_ms = [double]::Parse($total.Groups[5].Value, [Globalization.CultureInfo]::InvariantCulture)
    p99_ms = if ($total.Groups[6].Success) { [double]::Parse($total.Groups[6].Value, [Globalization.CultureInfo]::InvariantCulture) } else { $null }
    max_ms = if ($total.Groups[7].Success) { [double]::Parse($total.Groups[7].Value, [Globalization.CultureInfo]::InvariantCulture) } else { $null }
    over50 = if ($total.Groups[8].Success) { [int]$total.Groups[8].Value } else { $null }
    main_cpu_ms = Numeric $text "CPU loop_thread_ms=([0-9.]+)"
    final = $final
    fingerprints = @($fingerprints)
    light_fingerprints = @($lightFingerprints)
    light_initial = if ($lightInitial.Success) { $lightInitial.Value } else { $null }
    light_tick = if ($lightTick.Success) { $lightTick.Value } else { $null }
    light_drain = if ($lightDrain.Success) { $lightDrain.Value } else { $null }
    process_affinity = $Run.Meta.actual_process_affinity
    priority = $Run.Meta.priority
    stdout = $Run.Meta.stdout
    stderr = $Run.Meta.stderr
  }
}

$baseArgs = @($workloadPath, [string]$Ticks)
$offArgs = @($workloadPath, [string]$Ticks, "--no-lighting", "--process-affinity", "0x4", "--main-affinity", "0x4")
$onArgs = @($workloadPath, [string]$Ticks, "--lighting", "--process-affinity", "0x14", "--main-affinity", "0x4", "--light-affinity", "0x10")

$warmupTicks = [Math]::Min(100, $Ticks)
(Invoke-Benchmark "warmup-base" $baselinePath @($workloadPath, [string]$warmupTicks) 0x4) | Out-Null
(Invoke-Benchmark "warmup-off" $candidatePath @($workloadPath, [string]$warmupTicks, "--no-lighting", "--process-affinity", "0x4", "--main-affinity", "0x4") 0x4) | Out-Null
(Invoke-Benchmark "warmup-on" $candidatePath @($workloadPath, [string]$warmupTicks, "--lighting", "--process-affinity", "0x14", "--main-affinity", "0x4", "--light-affinity", "0x10") 0x14) | Out-Null

$results = @()
for ($group = 1; $group -le $Groups; ++$group) {
  $order = @("base", "off", "on")
  for ($rotation = 1; $rotation -lt $group; ++$rotation) {
    $order = @($order[1..2] + $order[0])
  }
  foreach ($mode in $order) {
    $name = "g{0:D2}-{1}" -f $group, $mode
    if ($mode -eq "base") {
      $run = Invoke-Benchmark $name $baselinePath $baseArgs 0x4
    } elseif ($mode -eq "off") {
      $run = Invoke-Benchmark $name $candidatePath $offArgs 0x4
    } else {
      $run = Invoke-Benchmark $name $candidatePath $onArgs 0x14
    }
    $results += [pscustomobject](Parse-Run $run)
  }
}

$fingerprintBaseArgs = @($workloadPath, [string]$Ticks, "--fingerprint")
$fingerprintOffArgs = @($workloadPath, [string]$Ticks, "--fingerprint", "--no-lighting", "--process-affinity", "0x4", "--main-affinity", "0x4")
$fingerprintOnArgs = @($workloadPath, [string]$Ticks, "--fingerprint", "--lighting", "--diagnose-light", "--process-affinity", "0x14", "--main-affinity", "0x4", "--light-affinity", "0x10")
$fpBase = Parse-Run (Invoke-Benchmark "fingerprint-base" $baselinePath $fingerprintBaseArgs 0x4)
$fpOff = Parse-Run (Invoke-Benchmark "fingerprint-off" $candidatePath $fingerprintOffArgs 0x4)
$fpOn = Parse-Run (Invoke-Benchmark "fingerprint-on" $candidatePath $fingerprintOnArgs 0x14)
$simulationFingerprintMatch = (($fpBase.fingerprints -join "`n") -eq ($fpOff.fingerprints -join "`n")) -and (($fpBase.fingerprints -join "`n") -eq ($fpOn.fingerprints -join "`n"))
$lightFingerprintValues = @($fpOn.light_fingerprints | ForEach-Object { ($_ -split " ")[2..3] -join " " } | Select-Object -Unique)

$pairs = for ($group = 1; $group -le $Groups; ++$group) {
  $base = $results | Where-Object name -eq ("g{0:D2}-base" -f $group)
  $off = $results | Where-Object name -eq ("g{0:D2}-off" -f $group)
  $on = $results | Where-Object name -eq ("g{0:D2}-on" -f $group)
  [ordered]@{
    group = $group
    final_match = ($base.final -eq $off.final) -and ($base.final -eq $on.final)
    off_cpu_delta_pct = 100.0 * ($off.main_cpu_ms - $base.main_cpu_ms) / $base.main_cpu_ms
    on_cpu_delta_pct = 100.0 * ($on.main_cpu_ms - $base.main_cpu_ms) / $base.main_cpu_ms
    off_mean_delta_pct = 100.0 * ($off.mean_ms - $base.mean_ms) / $base.mean_ms
    on_mean_delta_pct = 100.0 * ($on.mean_ms - $base.mean_ms) / $base.mean_ms
  }
}
function Get-Median([double[]]$Values) {
  $sorted = @($Values | Sort-Object)
  if ($sorted.Count % 2) { return $sorted[[int][Math]::Floor($sorted.Count / 2)] }
  return ($sorted[$sorted.Count / 2 - 1] + $sorted[$sorted.Count / 2]) / 2.0
}
$summary = [ordered]@{
  baseline = $baselinePath
  baseline_sha256 = $baselineHash
  candidate = $candidatePath
  candidate_sha256 = $candidateHash
  workload = $workloadPath
  workload_sha256 = $workloadHash
  ticks = $Ticks
  groups = $Groups
  pairs = @($pairs)
  median_on_cpu_delta_pct = Get-Median @($pairs | ForEach-Object { $_.on_cpu_delta_pct })
  median_on_mean_delta_pct = Get-Median @($pairs | ForEach-Object { $_.on_mean_delta_pct })
  all_final_match = ($pairs | Where-Object { !$_.final_match }).Count -eq 0
  simulation_fingerprint_match = $simulationFingerprintMatch
  distinct_light_fingerprints = $lightFingerprintValues.Count
  fingerprint_base = $fpBase
  fingerprint_off = $fpOff
  fingerprint_on = $fpOn
  runs = $results
}
[IO.File]::WriteAllText((Join-Path $OutputDir "SUMMARY.json"), ($summary | ConvertTo-Json -Depth 8))
$summary | ConvertTo-Json -Depth 4
Get-ChildItem $OutputDir -File | Get-FileHash -Algorithm SHA256 | ForEach-Object { "$($_.Hash.ToLower())  $([IO.Path]::GetFileName($_.Path))" } | Set-Content (Join-Path $OutputDir "HASHES.sha256")
