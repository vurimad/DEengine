# Captures a Tracy profile of deengine without the GUI and summarizes it as text.
#
# Usage (from the repo root, after building the dev config):
#   tools/profile.ps1                                  # 10 s of msvc-dev
#   tools/profile.ps1 -Seconds 30 -Preset clang-cl     # longer, another build
#   tools/profile.ps1 -EngineArgs '--deadzone','0.2'   # extra deengine arguments
#
# Writes build/profiles/<timestamp>/:
#   trace.tracy      the capture (open it in tracy-profiler for the timeline)
#   zones.csv        per zone: count, total, mean, min, max (ns)
#   zones_self.csv   the same with self time (children excluded)
#   events.csv       every zone event and plot value with its timestamp and thread
# and prints the top zones by total time, and min / max / mean of each plot
# (e.g. "heap allocations/frame" should stay at 0).
#
# Needs the Tracy 0.14.1 tools (tracy-capture, tracy-csvexport), the same version as the
# client in vcpkg; `winget install wolfpld.tracy` provides them.

param(
    [string]$Preset = 'msvc',
    [string]$Config = 'RelWithDebInfo', # the dev build: optimized, with Tracy compiled in
    [int]$Seconds = 10,
    [int]$Top = 15,
    [string[]]$EngineArgs = @()
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

function Find-TracyTool([string]$name) {
    $command = Get-Command $name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    # winget only links tracy-profiler onto PATH; the other tools sit next to it.
    $profiler = Get-Command 'tracy-profiler' -ErrorAction SilentlyContinue
    if ($profiler) {
        $target = (Get-Item $profiler.Source).Target
        $dir = Split-Path ($(if ($target) { $target } else { $profiler.Source }))
        $candidate = Join-Path $dir "$name.exe"
        if (Test-Path $candidate) { return $candidate }
    }
    $packages = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
    $found = Get-ChildItem $packages -Directory -Filter 'wolfpld.tracy*' -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName "$name.exe" } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) { return $found }
    throw "$name not found: install the Tracy tools with 'winget install wolfpld.tracy'"
}

$capture = Find-TracyTool 'tracy-capture'
$csvexport = Find-TracyTool 'tracy-csvexport'
$engine = Join-Path $repo "build\$Preset\bin\$Config\deengine.exe"
if (-not (Test-Path $engine)) {
    throw "$engine not found: build it first (cmake --build --preset $Preset-dev)"
}

$out = Join-Path $repo ("build\profiles\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $out | Out-Null
$trace = Join-Path $out 'trace.tracy'

# The capture tool waits for the client, so start it first; it stops after -s seconds.
$captureProcess = Start-Process -FilePath $capture -ArgumentList @('-o', "`"$trace`"", '-f', '-s', $Seconds) `
    -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $out 'capture.log') `
    -RedirectStandardError (Join-Path $out 'capture.err')
$engineStart = @{
    FilePath               = $engine
    PassThru               = $true
    RedirectStandardOutput = (Join-Path $out 'engine.log')
    RedirectStandardError  = (Join-Path $out 'engine.err')
}
if ($EngineArgs.Count -gt 0) {
    $engineStart.ArgumentList = $EngineArgs # Start-Process rejects an empty list
}
$engineProcess = Start-Process @engineStart

Write-Host "Profiling $Preset/$Config for $Seconds s..."
if (-not $captureProcess.WaitForExit(($Seconds + 30) * 1000)) {
    $captureProcess.Kill()
    $engineProcess.Kill()
    throw "tracy-capture did not finish; see $out\capture.log"
}
if (-not $engineProcess.HasExited) {
    $engineProcess.Kill()
}
if (-not (Test-Path $trace)) {
    throw "no trace was written; see $out\capture.log and $out\engine.err"
}

& $csvexport $trace > (Join-Path $out 'zones.csv')
& $csvexport -e $trace > (Join-Path $out 'zones_self.csv')
& $csvexport -u -p $trace > (Join-Path $out 'events.csv')

$zones = Import-Csv (Join-Path $out 'zones.csv')
$self = @{}
foreach ($zone in (Import-Csv (Join-Path $out 'zones_self.csv'))) { $self[$zone.name] = [double]$zone.total_ns }

Write-Host ""
Write-Host "Top zones by total time ($out):"
$zones | Sort-Object { [double]$_.total_ns } -Descending | Select-Object -First $Top | ForEach-Object {
    [pscustomobject]@{
        zone      = $_.name
        count     = [int64]$_.counts
        total_ms  = [math]::Round([double]$_.total_ns / 1e6, 2)
        self_ms   = [math]::Round($self[$_.name] / 1e6, 2)
        mean_us   = [math]::Round([double]$_.mean_ns / 1e3, 2)
        max_us    = [math]::Round([double]$_.max_ns / 1e3, 2)
    }
} | Format-Table -AutoSize | Out-String | Write-Host

# Plot rows are the events with a value.
Write-Host "Plots:"
Import-Csv (Join-Path $out 'events.csv') | Where-Object { $_.value -ne '' } | Group-Object name | ForEach-Object {
    $values = $_.Group | ForEach-Object { [double]$_.value } | Measure-Object -Minimum -Maximum -Average
    [pscustomobject]@{
        plot    = $_.Name
        samples = $_.Count
        min     = [math]::Round($values.Minimum, 2)
        max     = [math]::Round($values.Maximum, 2)
        mean    = [math]::Round($values.Average, 2)
    }
} | Format-Table -AutoSize | Out-String | Write-Host
