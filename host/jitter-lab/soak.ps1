# soak.ps1 — start/stop/status of an unattended soak run (jitter work stream).
#
#   pwsh host/jitter-lab/soak.ps1 -Start  -Run RUN-20260828-01 [-Port COM5] [-Host http://p3a.local] [-Every 120] [-Hours 4] [-Note "baseline, Work mix"]
#   pwsh host/jitter-lab/soak.ps1 -Status -Run RUN-20260828-01
#   pwsh host/jitter-lab/soak.ps1 -Stop   -Run RUN-20260828-01        # stops logger+puller, runs analyze.py
#   pwsh host/jitter-lab/soak.ps1 -NewRunId                            # prints the next free RUN-YYYYMMDD-NN
#
# The logger and puller are detached processes (survive the agent session);
# their pids live in runs/<RUN>/pids.json. Raw data is gitignored; the
# committed summary is docs/jitter/runs/<RUN>.md (written by hand from report.md).
param(
    [switch]$Start, [switch]$Stop, [switch]$Status, [switch]$NewRunId,
    [string]$Run,
    [string]$Port = "COM5",
    [string]$DeviceHost = "http://p3a.local",
    [double]$Every = 120,
    [double]$Hours = 0,
    [string]$Note = ""
)
$ErrorActionPreference = "Stop"
$lab = $PSScriptRoot
$runs = Join-Path $lab "runs"
New-Item -ItemType Directory -Force $runs | Out-Null

if ($NewRunId) {
    $d = Get-Date -Format "yyyyMMdd"
    $n = 1
    while (Test-Path (Join-Path $runs ("RUN-{0}-{1:D2}" -f $d, $n))) { $n++ }
    Write-Output ("RUN-{0}-{1:D2}" -f $d, $n)
    exit 0
}
if (-not $Run) { throw "-Run RUN-YYYYMMDD-NN is required" }
$dir = Join-Path $runs $Run
$pids = Join-Path $dir "pids.json"

function Read-Pids { if (Test-Path $pids) { Get-Content $pids -Raw | ConvertFrom-Json } else { $null } }
function Alive($id) { if (-not $id) { return $false }; try { $null -ne (Get-Process -Id $id -ErrorAction Stop) } catch { $false } }

if ($Start) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $existing = Read-Pids
    if ($existing -and (Alive $existing.logger)) { throw "logger already running for $Run (pid $($existing.logger))" }
    # Snapshot settings before anything else
    & python (Join-Path $lab "snapshot_settings.py") save $Run --host $DeviceHost | Out-Null
    try { Invoke-RestMethod -Method Post "$DeviceHost/api/debug/frames/reset" -TimeoutSec 10 | Out-Null } catch { Write-Warning "stats reset failed: $_" }
    $logOut = Join-Path $dir "logger.stdout.log"
    $pullOut = Join-Path $dir "puller.stdout.log"
    $lp = Start-Process -FilePath "python" -ArgumentList @((Join-Path $lab "serial_logger.py"), $Port, $Run) `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $logOut -RedirectStandardError (Join-Path $dir "logger.stderr.log")
    $pullArgs = @((Join-Path $lab "pull_frames.py"), $Run, "--host", $DeviceHost, "--every", $Every, "--from-head")
    if ($Hours -gt 0) { $pullArgs += @("--hours", $Hours) }
    $pp = Start-Process -FilePath "python" -ArgumentList $pullArgs `
            -WindowStyle Hidden -PassThru -RedirectStandardOutput $pullOut -RedirectStandardError (Join-Path $dir "puller.stderr.log")
    @{ run = $Run; logger = $lp.Id; puller = $pp.Id; started = (Get-Date).ToString("s"); port = $Port; host = $DeviceHost; every = $Every; hours = $Hours; note = $Note } |
        ConvertTo-Json | Set-Content $pids -Encoding UTF8
    Write-Output "STARTED $Run logger=$($lp.Id) puller=$($pp.Id) dir=$dir"
    exit 0
}

if ($Status) {
    $p = Read-Pids
    if (-not $p) { Write-Output "no pids.json for $Run"; exit 1 }
    Write-Output ("run={0} started={1} note='{2}'" -f $p.run, $p.started, $p.note)
    Write-Output ("logger pid={0} alive={1}" -f $p.logger, (Alive $p.logger))
    Write-Output ("puller pid={0} alive={1}" -f $p.puller, (Alive $p.puller))
    foreach ($f in @("state.json", "pull_state.json")) {
        $fp = Join-Path $dir $f
        if (Test-Path $fp) { Write-Output "--- $f"; Get-Content $fp }
    }
    $sj = Join-Path $dir "stalls.jsonl"
    if (Test-Path $sj) { Write-Output ("uart stall reports: {0}" -f (Get-Content $sj | Measure-Object -Line).Lines) }
    exit 0
}

if ($Stop) {
    $p = Read-Pids
    if ($p) {
        foreach ($id in @($p.puller, $p.logger)) {
            if (Alive $id) { Stop-Process -Id $id -Force -Confirm:$false; Write-Output "stopped pid $id" }
        }
    }
    # Final pull + analysis
    & python (Join-Path $lab "pull_frames.py") $Run --host $DeviceHost --once 2>&1 | Select-Object -Last 2
    & python (Join-Path $lab "analyze.py") $Run
    Write-Output "report: $(Join-Path $dir 'report.md')"
    exit 0
}

Write-Output "nothing to do: pass -Start, -Stop, -Status or -NewRunId"
