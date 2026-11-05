# Captures a Tracy profile of deengine without the GUI and summarizes it as text.
#
# Usage (from the repo root, after building the dev config):
#   tools/profile.ps1                                  # 10 s of msvc-dev
#   tools/profile.ps1 -Seconds 30 -Preset clang-cl     # longer, another build
#   tools/profile.ps1 -EngineArgs '--deadzone','0.2'   # extra deengine arguments
#   tools/profile.ps1 -Seconds 20 -EngineArgs '--no-validation','--window','2560x1440'
#                                                      # GPU numbers: validation off (dev builds have it
#                                                      # on), at the size that matters. Keep vsync on: it
#                                                      # holds the refresh rate, the load the target is for.
#                                                      # Uncapped, the same scene has run at 240 or 440 fps
#                                                      # with different clocks, and read 1.8 or 2.3 ms.
#
# Writes build/profiles/<timestamp>/:
#   trace.tracy      the capture (open it in tracy-profiler for the timeline)
#   zones.csv        per zone: count, total, mean, min, max (ns)
#   zones_self.csv   the same with self time (children excluded)
#   events.csv       every zone event and plot value with its timestamp and thread
#   summary.csv      per zone and plot, without the start: count, median, p95, p99, min, max, mean
#   run.txt          what was run: the arguments, the swapchain, the frames counted
#   gpu.csv          nvidia-smi's clock, power, temperature and P-state each second, where it's installed
# and prints the top zones by total time, the same figures for each plot (e.g. "heap allocations/frame"
# should stay at 0), and the GPU's clocks. The first -SkipSeconds from the first frame aren't in those figures:
# the GPU ramping up and things first used would otherwise be counted, and a median is what to compare
# between runs. A zone's self time is over the whole capture.
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
$engineStartTime = Get-Date
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
function Stop-Started($process) {
    if ($process -and -not $process.HasExited) { $process.Kill() }
}
# The engine's own log says why it stopped: it writes to stderr.
function Get-EngineErrors {
    (Get-Content (Join-Path $out 'engine.err') -ErrorAction SilentlyContinue | Select-String -Pattern 'error' | Select-Object -First 5) -join "`n"
}
# The capture waits for its client, so an engine that's gone won't ever finish it.
$deadline = (Get-Date).AddSeconds($Seconds + 30)
while (-not $captureProcess.HasExited) {
    if ($engineProcess.HasExited -and -not (Test-Path $trace)) {
        Start-Sleep -Milliseconds 500
        if (-not $captureProcess.HasExited) {
            Stop-Started $captureProcess
            Stop-Started $gpuLogProcess
            throw "deengine stopped before the capture connected; see $out\engine.err`n$(Get-EngineErrors)"
        }
    }
    if ((Get-Date) -gt $deadline) {
        Stop-Started $captureProcess
        Stop-Started $engineProcess
        Stop-Started $gpuLogProcess
        throw "tracy-capture did not finish; see $out\capture.log"
    }
    Start-Sleep -Milliseconds 200
}
Stop-Started $engineProcess
Stop-Started $gpuLogProcess
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
$firstGpu = [double]::MaxValue
$firstCpu = [double]::MaxValue
$last = 0.0
$rows = [System.Collections.Generic.List[string[]]]::new()
while ($null -ne ($line = $reader.ReadLine())) {
    # A comma in a name would move every field along and quietly lose the row: stop instead.
    $fields = $line.Split(',')
    if ($fields.Count -ne $header.Count) { throw "events.csv: a name holds a comma, which this can't read: $line" }
    $at = [double]$fields[$timeColumn]
    $name = $fields[$nameColumn].Trim('"')
    # From the first frame's numbers: anything earlier is loading, whatever it takes. Without GPU timers, its
    # first CPU frame.
    if ($name -eq 'gpu frame ms' -and $at -lt $firstGpu) { $firstGpu = $at }
    if ($name -eq 'rhi begin_frame' -and $at -lt $firstCpu) { $firstCpu = $at }
    if ($at -gt $last) { $last = $at }
    $rows.Add($fields)
}
$reader.Close()
if ($firstCpu -eq [double]::MaxValue) { throw "no frames in this capture; see $out\engine.err`n$(Get-EngineErrors)" }
if ($firstGpu -eq [double]::MaxValue) { Write-Host "No GPU timers in this capture: CPU figures only." }
$from = [math]::Min($firstGpu, $firstCpu) + $SkipSeconds * 1e9
if ($firstGpu -ne [double]::MaxValue) { $from = $firstGpu + $SkipSeconds * 1e9 }
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
if (-not $zoneTimes.ContainsKey('rhi begin_frame')) {
    throw "no frames past the first $SkipSeconds s: capture for longer than that (-Seconds $Seconds)"
}

function Get-Figures([string]$kind, [string]$name, [System.Collections.Generic.List[double]]$values) {
    $values.Sort()
    # The value with that share of them at or below it.
    $percentile = { param($p) $values[[math]::Max([int][math]::Ceiling($p * $values.Count) - 1, 0)] }
    $middle = [int][math]::Floor($values.Count / 2)
    $median = if ($values.Count % 2 -eq 1) { $values[$middle] } else { ($values[$middle - 1] + $values[$middle]) / 2 }
    $total = 0.0
    foreach ($value in $values) { $total += $value }
    [pscustomobject]@{
        kind   = $kind
        name   = $name
        count  = $values.Count
        median = [math]::Round($median, 3)
        p95    = [math]::Round((& $percentile 0.95), 3)
        p99    = [math]::Round((& $percentile 0.99), 3)
        min    = [math]::Round($values[0], 3)
        max    = [math]::Round($values[$values.Count - 1], 3)
        mean   = [math]::Round($total / $values.Count, 3)
        total  = $total
    }
}
# What this was, so two runs can't be compared without seeing they differ.
$swapchain = (Get-Content (Join-Path $out 'engine.err') -ErrorAction SilentlyContinue | Select-String -Pattern 'swapchain \d' | Select-Object -First 1)
$frames = $zoneTimes['rhi begin_frame'].Count
$seconds = ($last - $from) / 1e9
$runLine = "args: $($EngineArgs -join ' ')`n$swapchain`nframes past the skip: $frames in $([math]::Round($seconds, 1)) s, $([math]::Round($frames / [math]::Max($seconds, 0.001))) fps"
Set-Content (Join-Path $out 'run.txt') $runLine
Write-Host $runLine

$zoneFigures = @($zoneTimes.GetEnumerator() | ForEach-Object { Get-Figures 'zone (us)' $_.Key $_.Value })
$plotFigures = @($plotValues.GetEnumerator() | Sort-Object Key | ForEach-Object { Get-Figures 'plot' $_.Key $_.Value })
($zoneFigures + $plotFigures) | Select-Object kind, name, count, median, p95, p99, min, max, mean |
    Export-Csv (Join-Path $out 'summary.csv') -NoTypeInformation

Write-Host ""
$selfTimes = @{}
foreach ($zone in (Import-Csv (Join-Path $out 'zones_self.csv'))) { $selfTimes[$zone.name] = [double]$zone.total_ns }
Write-Host "Top zones by total time, past the first $SkipSeconds s from the first frame, in us ($out):"
$zoneFigures | Sort-Object total -Descending | Select-Object -First $Top |
    Select-Object @{ n = 'zone'; e = { $_.name } }, count, @{ n = 'total_ms'; e = { [math]::Round($_.total / 1e3, 2) } },
        @{ n = 'self_ms_all'; e = { [math]::Round($selfTimes[$_.name] / 1e6, 2) } }, median, p95, p99, max, mean |
    Format-Table -AutoSize | Out-String | Write-Host

Write-Host "Plots, past the first $SkipSeconds s from the first frame:"
$plotFigures | Select-Object @{ n = 'plot'; e = { $_.name } }, @{ n = 'samples'; e = { $_.count } }, median, p95, p99, min, max, mean |
    Format-Table -AutoSize | Out-String | Write-Host

# The GPU log over the same span: launch, exit and saving the trace would otherwise set its ranges. The engine's
# clock starts about when its process does, so this lines up to within a second or so, which is all a once-a-second
# log can tell. nvidia-smi writes the same format whatever the machine's locale.
$gpuLog = Join-Path $out 'gpu.csv'
if ((Test-Path $gpuLog) -and (Get-Item $gpuLog).Length -gt 0) {
    $invariant = [System.Globalization.CultureInfo]::InvariantCulture
    $samples = @(Import-Csv $gpuLog)
    $names = if ($samples.Count -gt 0) { $samples[0].PSObject.Properties.Name } else { @() }
    $timeName = $names | Where-Object { $_ -like '*timestamp*' } | Select-Object -First 1
    $since = $engineStartTime.AddSeconds($from / 1e9)
    $until = $engineStartTime.AddSeconds($last / 1e9)
    $kept = @($samples | Where-Object {
            $when = [datetime]::MinValue
            [datetime]::TryParse($_.$timeName, $invariant, [System.Globalization.DateTimeStyles]::None, [ref]$when) -and
                $when -ge $since -and $when -le $until
        })
    if ($kept.Count -gt 0) {
        # Some GPUs and laptops report [N/A] here, and the last line can be cut short: skip what isn't a number.
        $range = {
            param($match)
            $column = $names | Where-Object { $_ -like $match } | Select-Object -First 1
            $numbers = [System.Collections.Generic.List[double]]::new()
            foreach ($sample in $kept) {
                $number = 0.0
                if ([double]::TryParse([string]$sample.$column, [System.Globalization.NumberStyles]::Float, $invariant, [ref]$number)) {
                    $numbers.Add($number)
                }
            }
            if ($numbers.Count -eq 0) { return 'not reported' }
            $figures = Get-Figures 'gpu' $column $numbers
            "{0} (from {1} to {2})" -f $figures.median, $figures.min, $figures.max
        }
        $stateName = $names | Where-Object { $_ -like '*pstate*' } | Select-Object -First 1
        $states = (@($kept | ForEach-Object { ([string]$_.$stateName).Trim() }) | Sort-Object -Unique) -join ', '
        Write-Host ("GPU past the first {0} s, median: {1}; graphics clock {2} MHz; power {3} W; {4} C; busy {5} %" -f $SkipSeconds, $states,
            (& $range '*clocks.current.graphics*'), (& $range '*power.draw*'), (& $range '*temperature.gpu*'), (& $range '*utilization.gpu*'))
    }
}
