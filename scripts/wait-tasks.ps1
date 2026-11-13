param(
  [string]$Task,
  [string]$AfterCommit,
  [ValidateRange(1, 3600)][int]$PollSeconds = 60
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$tasksPath = Join-Path $repo 'task/TASKS.md'
$agentsPath = Join-Path $repo 'AGENTS.md'

function Resolve-Commit([string]$Hash) {
  $commit = git -C $repo rev-parse --verify --quiet "$Hash^{commit}"
  if ($LASTEXITCODE -eq 0) { return $commit }
}

$previousCommit = if ($AfterCommit) {
  $resolved = Resolve-Commit $AfterCommit
  if (!$resolved) { throw "无法解析已审提交 $AfterCommit" }
  $resolved
}

function Read-Handoff {
  $lines = [IO.File]::ReadAllLines($tasksPath)
  $agents = [IO.File]::ReadAllText($agentsPath)
  $section = [regex]::Match($agents, '(?ms)^###\s+3\.1\s+.*?(?=^###\s+3\.2\s)')
  $rows = [regex]::Matches($section.Value, '(?m)^\| `(?<state>[^`]+)` \| (?<meaning>[^|]+) \| (?<writer>coder|reviewer) \|\r?$')
  $states = @($rows | ForEach-Object { $_.Groups['state'].Value })
  $ready = @($rows | Where-Object {
    $_.Groups['writer'].Value -eq 'coder' -and $_.Groups['meaning'].Value -match '等待审查|待审'
  })
  if ($ready.Count -ne 1) { return 'AGENTS.md 中找不到唯一的 coder 待审状态定义' }

  $bare = @($lines | Where-Object { $states -ccontains $_ })
  if ($bare.Count -ne 1 -or $lines[-1] -cne $bare[0]) {
    return "TASKS.md 状态行无效（裸状态行数 $($bare.Count)）"
  }

  $heading = [regex]::Match(($lines -join "`n"), '(?m)^##\s+(?<id>[A-Z][A-Z0-9]*-\d+)\b')
  $taskId = $heading.Groups['id'].Value
  $state = $bare[0]
  if (!$taskId) { return 'TASKS.md 缺少任务标题' }
  if (($Task -and $taskId -cne $Task) -or $state -cne $ready[0].Groups['state'].Value) {
    return "$taskId · $state"
  }

  $pattern = '^\s*-\s*(?:主提交|返工[^：:\r\n]*提交)[：:]\s*`(?<hash>[0-9a-fA-F]{7,64})`'
  $entries = @($lines | Select-String -Pattern $pattern)
  if (!$entries.Count) { return "$taskId · $state · 等待填写提交号" }

  $hash = $entries[-1].Matches[0].Groups['hash'].Value
  $commit = Resolve-Commit $hash
  if (!$commit) { return "$taskId · $state · Git 尚无法验证 $hash" }
  if ($commit -eq $previousCommit) { return "$taskId · $state · 等待新提交" }

  return [pscustomobject]@{ Task = $taskId; State = $state; Commit = $commit }
}

$lastMessage = $null
while ($true) {
  try {
    $result = Read-Handoff
  } catch [IO.IOException] {
    $result = "暂时无法读取交接文件 · $($_.Exception.Message)"
  }

  if ($result -isnot [string]) {
    Write-Output $result
    break
  }
  if ($result -cne $lastMessage) {
    Write-Host "$(Get-Date -Format 'HH:mm:ss') $result"
    $lastMessage = $result
  }
  Start-Sleep -Seconds $PollSeconds
}
