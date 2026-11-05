# Captures a Tracy profile of deengine without the GUI and summarizes it as text.
#
# Usage (from the repo root, after building the dev config):
#   tools/profile.ps1                                  # 10 s of msvc-dev
#   tools/profile.ps1 -Seconds 30 -Preset clang-cl     # longer, another build
#   tools/profile.ps1 -EngineArgs '--deadzone','0.2'   # extra deengine arguments
#   tools/profile.ps1 -Seconds 20 -EngineArgs '--no-validation','--no-vsync','--window','2560x1440'
#                                                      # GPU numbers: validation off (dev builds have it
#                                                      # on), vsync off, at the size that matters
#
# Writes build/profiles/<timestamp>/:
#   trace.tracy      the capture (open it in tracy-profiler for the timeline)
#   zones.csv        per zone: count, total, mean, min, max (ns)
#   zones_self.csv   the same with self time (children excluded)
#   events.csv       every zone event and plot value with its timestamp and thread
#   summary.csv      per zone and plot, without the start: count, median, p95, p99, min, max, mean
#   gpu.csv          nvidia-smi's clock, power, temperature and P-state each second, where it's installed
# and prints the top zones by total time, the same figures for each plot (e.g. "heap allocations/frame"
# should stay at 0), and the GPU's clocks. The first -SkipSeconds of the capture aren't in those figures:
# compiling shaders and allocating would otherwise be counted, and a median is what to compare between runs.
#
# Needs the Tracy 0.14.1 tools (tracy-capture, tracy-csvexport), the same version as the
# client in vcpkg; `winget install wolfpld.tracy` provides them.

param(
    [string]$Preset = 'msvc',
    [string]$Config = 'RelWithDebInfo', # the dev build: optimized, with Tracy compiled in
    [int]$Seconds = 10,
    [int]$Top = 15,
    [double]$SkipSeconds = 2,
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

# What the GPU was doing: a laptop's clocks can move a number more than a change being measured.
$gpuLogProcess = $null
if (Get-Command 'nvidia-smi' -ErrorAction SilentlyContinue) {
    $gpuLogProcess = Start-Process -FilePath 'nvidia-smi' -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $out 'gpu.csv') `
        -RedirectStandardError (Join-Path $out 'gpu.err') -ArgumentList @(
            '--query-gpu=timestamp,pstate,clocks.gr,clocks.mem,power.draw,temperature.gpu,utilization.gpu',
            '--format=csv,nounits', '-l', '1')
}

Write-Host "Profiling $Preset/$Config for $Seconds s..."
if (-not $captureProcess.WaitForExit(($Seconds + 30) * 1000)) {
    $captureProcess.Kill()
    $engineProcess.Kill()
    if ($gpuLogProcess) { $gpuLogProcess.Kill() }
    throw "tracy-capture did not finish; see $out\capture.log"
}
if (-not $engineProcess.HasExited) {
    $engineProcess.Kill()
}
if ($gpuLogProcess -and -not $gpuLogProcess.HasExited) {
    $gpuLogProcess.Kill()
}
if (-not (Test-Path $trace)) {
    throw "no trace was written; see $out\capture.log and $out\engine.err"
}

# Through Start-Process, not '>', which would write UTF-16 here; these get large.
function Export-Csv-From-Trace([string[]]$arguments, [string]$file) {
    $process = Start-Process -FilePath $csvexport -ArgumentList ($arguments + "`"$trace`"") -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput (Join-Path $out $file) -RedirectStandardError (Join-Path $out "$file.err")
    if ($process.ExitCode -ne 0) { throw "tracy-csvexport failed; see $out\$file.err" }
}
Export-Csv-From-Trace @() 'zones.csv'
Export-Csv-From-Trace @('-e') 'zones_self.csv'
Export-Csv-From-Trace @('-u', '-p') 'events.csv'

# Each zone event's time and each plot value, past the start. With a StreamReader: Import-Csv takes minutes
# over millions of rows.
$columns = @{}
$reader = [System.IO.StreamReader]::new((Join-Path $out 'events.csv'))
$header = $reader.ReadLine().Split(',')
for ($i = 0; $i -lt $header.Count; ++$i) { $columns[$header[$i]] = $i }
$nameColumn = $columns['name']
$timeColumn = $columns['ns_since_start']
$durationColumn = $columns['exec_time_ns']
$valueColumn = $columns['value']
$zoneTimes = @{}
$plotValues = @{}
$first = [double]::MaxValue
$rows = [System.Collections.Generic.List[string[]]]::new()
while ($null -ne ($line = $reader.ReadLine())) {
    # A name can hold a comma only in quotes, and ours don't: a comma there is a bug to see, not to parse around.
    $fields = $line.Split(',')
    if ($fields.Count -le $durationColumn) { continue }
    $at = [double]$fields[$timeColumn]
    if ($at -lt $first) { $first = $at }
    $rows.Add($fields)
}
$reader.Close()
$from = $first + $SkipSeconds * 1e9
foreach ($fields in $rows) {
    if ([double]$fields[$timeColumn] -lt $from) { continue }
    $name = $fields[$nameColumn].Trim('"')
    $value = if ($null -ne $valueColumn -and $fields.Count -gt $valueColumn) { $fields[$valueColumn] } else { '' }
    if ($value -ne '') {
        if (-not $plotValues.ContainsKey($name)) { $plotValues[$name] = [System.Collections.Generic.List[double]]::new() }
        $plotValues[$name].Add([double]$value)
    } else {
        if (-not $zoneTimes.ContainsKey($name)) { $zoneTimes[$name] = [System.Collections.Generic.List[double]]::new() }
        $zoneTimes[$name].Add([double]$fields[$durationColumn] / 1e3)
    }
}
$rows = $null

function Get-Figures([string]$kind, [string]$name, [System.Collections.Generic.List[double]]$values) {
    $values.Sort()
    $percentile = { param($p) $values[[math]::Min([int]($p * $values.Count), $values.Count - 1)] }
    $total = 0.0
    foreach ($value in $values) { $total += $value }
    [pscustomobject]@{
        kind   = $kind
        name   = $name
        count  = $values.Count
        median = [math]::Round((& $percentile 0.5), 3)
        p95    = [math]::Round((& $percentile 0.95), 3)
        p99    = [math]::Round((& $percentile 0.99), 3)
        min    = [math]::Round($values[0], 3)
        max    = [math]::Round($values[$values.Count - 1], 3)
        mean   = [math]::Round($total / $values.Count, 3)
        total  = $total
    }
}
$zoneFigures = @($zoneTimes.GetEnumerator() | ForEach-Object { Get-Figures 'zone (us)' $_.Key $_.Value })
$plotFigures = @($plotValues.GetEnumerator() | Sort-Object Key | ForEach-Object { Get-Figures 'plot' $_.Key $_.Value })
($zoneFigures + $plotFigures) | Select-Object kind, name, count, median, p95, p99, min, max, mean |
    Export-Csv (Join-Path $out 'summary.csv') -NoTypeInformation

Write-Host ""
Write-Host "Top zones by total time, past the first $SkipSeconds s, in us ($out):"
$zoneFigures | Sort-Object total -Descending | Select-Object -First $Top |
    Select-Object @{ n = 'zone'; e = { $_.name } }, count, median, p95, p99, max, mean |
    Format-Table -AutoSize | Out-String | Write-Host

Write-Host "Plots, past the first $SkipSeconds s:"
$plotFigures | Select-Object @{ n = 'plot'; e = { $_.name } }, @{ n = 'samples'; e = { $_.count } }, median, p95, p99, min, max, mean |
    Format-Table -AutoSize | Out-String | Write-Host

$gpuLog = Join-Path $out 'gpu.csv'
if ((Test-Path $gpuLog) -and (Get-Item $gpuLog).Length -gt 0) {
    $samples = @(Import-Csv $gpuLog)
    if ($samples.Count -gt 0) {
        $names = $samples[0].PSObject.Properties.Name
        $range = {
            param($match)
            $column = $names | Where-Object { $_ -like $match } | Select-Object -First 1
            $values = $samples | ForEach-Object { [double]$_.$column } | Measure-Object -Minimum -Maximum
            "$($values.Minimum) to $($values.Maximum)"
        }
        $states = ($samples | ForEach-Object { $_.$($names | Where-Object { $_ -like '*pstate*' } | Select-Object -First 1).Trim() } | Sort-Object -Unique) -join ', '
        Write-Host ("GPU over the run: {0}; graphics clock {1} MHz; power {2} W; {3} C; busy {4} %" -f $states,
            (& $range '*clocks.current.graphics*'), (& $range '*power.draw*'), (& $range '*temperature.gpu*'), (& $range '*utilization.gpu*'))
    }
}
