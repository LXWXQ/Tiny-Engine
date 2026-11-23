[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [int]$Frames = 600,
    [int]$Runs = 3,
    [string]$Scenario = "all",
    [double]$MaxVariancePct = 2.0,
    [switch]$Strict,
    [string]$OutDir = "",
    [string]$VulkanSdk = $env:VULKAN_SDK
)

# ---------------------------------------------------------------------------
# P0: fixed scenarios + one baseline.md (T-10's missing half).
#
# Runs each scenario N times, parses three things from stdout:
#   * [SelfTest] performance / telemetry  -> FPS, draw calls, desc binds, tris
#   * [FrameStats]                        -> passes, barriers, binds, redundancy
#   * [Validation]                        -> errors / warnings
# #   * [FrameTime]  -> median / p95 / max frame time in ms (hitch detector)
#   * [Resources]  -> images created per frame (steady state must be 0)
#   * [Memory]     -> heap0 used MiB, last sample
#   * [SceneGpu]   -> cull summary visibleRatio
# and writes build/bench/<timestamp>/baseline.md.
#
#   tools/bench.ps1                 # all scenarios, 3 runs each
#   tools/bench.ps1 -Scenario lights-512 -Runs 1
#
#
# Why 600 frames and not 120: measured on this machine (RTX 2070, Debug build),
# 120 frames per run is a 0.9..1.6 s window and gives spreads of 13.7% (default),
# 2.9% (voxel-world) and 6.5% (lights-512) - too short to average out scheduling
# jitter. 600 frames is a 4..7 s window and gives 0.91% / 1.21% / 1.88%. A
# variance failure therefore usually means the machine was busy, not that the
# code regressed: re-run before believing it.
# Exit code: 0 when every run exits 0, stderr is empty, validation is 0/0 and
# FPS spread across runs stays under -MaxVariancePct. Anything else is 1.
#
# Scene substitution versus the plan: the plan listed cube-field / lights-512 /
# editor-default. There is no cube-field spawner in the engine, so this script
# uses the voxel world as the geometry-pressure scenario. Recorded as a
# deviation in docs/80-roadmap/ue-borrowing-plan.md.
#
# IMPORTANT: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1
# without a BOM as ANSI, and a non-ASCII comment can corrupt the next line.
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

function Quote-NativeArg([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}
if (-not [string]::IsNullOrWhiteSpace($VulkanSdk)) { $env:VULKAN_SDK = $VulkanSdk }

$buildRoot = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repoRoot $BuildDir }
$exe = Join-Path $buildRoot (Join-Path $Config "TinyEngine.exe")
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe (build first)" }

# Scenario table. Order is fixed so baseline.md rows are comparable across runs.
$scenarios = [ordered]@{
    "default"    = @()
    "voxel-world" = @("--voxel-world", "1")
    "lights-512" = @("--voxel-world", "1", "--light-stress", "512", "--clustered-lighting", "--clustered-gpu-build")
}
if ($Scenario -ne "all") {
    if (-not $scenarios.Contains($Scenario)) { throw "unknown scenario '$Scenario' (default / voxel-world / lights-512 / all)" }
    $only = [ordered]@{}
    $only[$Scenario] = $scenarios[$Scenario]
    $scenarios = $only
}

function Get-Metric([string]$text, [string]$pattern, [int]$group) {
    $m = [regex]::Match($text, $pattern)
    if (-not $m.Success) { return $null }
    return [double]$m.Groups[$group].Value
}

function Invoke-Run([string]$dir, [string]$name, [int]$index, [string[]]$extra) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $stdout = Join-Path $dir ("{0}.run{1}.stdout.log" -f $name, $index)
    $stderr = Join-Path $dir ("{0}.run{1}.stderr.log" -f $name, $index)

    # Fixed establishing shot (Scene::init's Sponza default). Without it the camera
    # comes from config/engine.json, i.e. from whoever last moved it - the same code
    # would then produce different screenshots and different cull ratios.
    $engineArgs = @("--selftest", "$Frames", "--stats-frame") +
        @("--camera", "-4.6485233,5.2119632,0.0523994,0.3507974,-17.3302765") + $extra
    $argLine = ($engineArgs | ForEach-Object { Quote-NativeArg $_ }) -join " "
    $p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru -NoNewWindow

    $out = [string](Get-Content -LiteralPath $stdout -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
    $row = [ordered]@{
        scenario = $name
        run      = $index
        exit     = $p.ExitCode
        stderr   = (Get-Item -LiteralPath $stderr).Length
        fps      = Get-Metric $out '\[SelfTest\] performance: \d+ frames in [\d.]+ s \(([\d.]+) FPS\)' 1
        seconds  = Get-Metric $out '\[SelfTest\] performance: \d+ frames in ([\d.]+) s' 1
        draws    = Get-Metric $out '\[SelfTest\] telemetry: draw calls (\d+)' 1
        binds    = Get-Metric $out '\[SelfTest\] telemetry: draw calls \d+ \| desc binds (\d+)' 1
        tris     = Get-Metric $out '\[SelfTest\] telemetry: .*\| triangles (\d+)' 1
        passes   = Get-Metric $out '\[FrameStats\] frames=\d+ \| passes/frame=([\d.]+)' 1
        barriers = Get-Metric $out 'barriers/frame=([\d.]+) calls' 1
        sameState = Get-Metric $out 'skipped: sameState=([\d.]+) crossQueue' 1
        crossQ   = Get-Metric $out 'crossQueue=([\d.]+)' 1
        bindsPg  = Get-Metric $out 'pipelineBinds/frame=([\d.]+)' 1
        redundant = Get-Metric $out 'redundant=[\d.]+ \(([\d.]+)%\)' 1
        errors   = Get-Metric $out '\[Validation\] errors=(\d+)' 1
        warnings = Get-Metric $out '\[Validation\] errors=\d+ warnings=(\d+)' 1
        medMs    = Get-Metric $out '\[FrameTime\] frames=\d+ \| median=([\d.]+)ms' 1
        p95Ms    = Get-Metric $out 'median=[\d.]+ms p95=([\d.]+)ms' 1
        maxMs    = Get-Metric $out 'p95=[\d.]+ms max=([\d.]+)ms' 1
        images   = Get-Metric $out '\[Resources\] frames=\d+ \| images/frame=([\d.]+)' 1
        heapMb   = Get-Metric $out '\[Memory\] heap0 used first=[\d.]+ last=([\d.]+) peak=' 1
        visible  = Get-Metric $out 'cull summary \| frames=\d+ visibleRatio=([\d.]+)' 1
    }
    if ($null -eq $row.fps) { throw "no [SelfTest] performance line in $stdout (scenario $name run $index)" }
    return [pscustomobject]$row
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = Join-Path $buildRoot "bench\$stamp" }
elseif (-not [IO.Path]::IsPathRooted($OutDir)) { $OutDir = Join-Path $repoRoot $OutDir }
New-Item -ItemType Directory -Force $OutDir | Out-Null

$all = New-Object System.Collections.ArrayList
$problems = @()
$warnings = @()

foreach ($name in $scenarios.Keys) {
    for ($i = 1; $i -le $Runs; $i++) {
        Write-Host ("[bench] {0} run {1}/{2} ..." -f $name, $i, $Runs)
        $row = Invoke-Run $OutDir $name $i $scenarios[$name]
        [void]$all.Add($row)
        if ($row.exit -ne 0) { $problems += ("{0} run {1}: exit={2}" -f $name, $i, $row.exit) }
        if ($row.stderr -ne 0) { $problems += ("{0} run {1}: stderr={2} bytes" -f $name, $i, $row.stderr) }
        if ($row.errors -ne 0) { $problems += ("{0} run {1}: validation errors={2}" -f $name, $i, $row.errors) }
        if ($row.warnings -ne 0) { $problems += ("{0} run {1}: validation warnings={2}" -f $name, $i, $row.warnings) }
    }
}

# Aggregate per scenario: median FPS plus spread as (max-min)/median.
$summary = @()
foreach ($name in $scenarios.Keys) {
    $rows = @($all | Where-Object { $_.scenario -eq $name })
    $fps = @($rows | ForEach-Object { [double]$_.fps } | Sort-Object)
    $median = $fps[[int][Math]::Floor($fps.Count / 2)]
    $spread = if ($median -gt 0) { 100.0 * ($fps[-1] - $fps[0]) / $median } else { 0 }
    $first = $rows[0]
    $summary += [pscustomobject]@{
        scenario  = $name
        runs      = $rows.Count
        fpsMedian = [math]::Round($median, 2)
        fpsMin    = [math]::Round($fps[0], 2)
        fpsMax    = [math]::Round($fps[-1], 2)
        spreadPct = [math]::Round($spread, 2)
        frameMs   = [math]::Round(1000.0 / $median, 2)
        passes    = $first.passes
        barriers  = $first.barriers
        sameState = $first.sameState
        crossQ    = $first.crossQ
        binds     = $first.bindsPg
        redundant = $first.redundant
        draws     = $first.draws
        descBinds = $first.binds
        tris      = $first.tris
        errors    = $first.errors
        warnings  = $first.warnings
        stderr    = $first.stderr
        medMs     = $first.medMs
        p95Ms     = $first.p95Ms
        maxMs     = $first.maxMs
        images    = $first.images
        heapMb    = $first.heapMb
        visible   = $first.visible
    }
    if ($spread -gt $MaxVariancePct) {
        $warnings += ("{0}: FPS spread {1}% > {2}% ({3}..{4}) - machine noise first, re-run before suspecting code" -f $name, [math]::Round($spread, 2), $MaxVariancePct, $fps[0], $fps[-1])
    }
}

$md = New-Object System.Collections.ArrayList
[void]$md.Add("# Baseline (" + $stamp + ")")
[void]$md.Add("")
[void]$md.Add("- frames per run: $Frames")
[void]$md.Add("- runs per scenario: $Runs")
[void]$md.Add("- config: $Config")
[void]$md.Add("- engine: $exe")
[void]$md.Add("")
[void]$md.Add("| scenario | FPS (median) | spread % | frame ms | passes/frame | barriers/frame | sameState | crossQueue | binds/frame | redundant % | draw calls | desc binds | triangles | median ms | p95 ms | max ms | images/frame | heap0 MiB | visible ratio |")
[void]$md.Add("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
foreach ($s in $summary) {
    [void]$md.Add(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} | {14} | {15} | {16} | {17} | {18} |" -f `
        $s.scenario, $s.fpsMedian, $s.spreadPct, $s.frameMs, $s.passes, $s.barriers, $s.sameState, $s.crossQ, `
        $s.binds, $s.redundant, $s.draws, $s.descBinds, $s.tris, `
        $s.medMs, $s.p95Ms, $s.maxMs, $s.images, $s.heapMb, $s.visible))
}
[void]$md.Add("")
[void]$md.Add("| scenario | run | FPS | s | exit | stderr | errors | warnings |")
[void]$md.Add("|---|---|---|---|---|---|---|---|")
foreach ($r in $all) {
    [void]$md.Add(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} |" -f `
        $r.scenario, $r.run, $r.fps, $r.seconds, $r.exit, $r.stderr, $r.errors, $r.warnings))
}
[void]$md.Add("")
if ($problems.Count -eq 0 -and $warnings.Count -eq 0) {
    [void]$md.Add("Result: PASS (all runs exit 0, stderr 0 bytes, validation 0/0, spread under $MaxVariancePct%)")
} elseif ($problems.Count -eq 0) {
    $verdict = if ($Strict) { "FAIL (-Strict)" } else { "PASS with warnings" }
    [void]$md.Add("Result: " + $verdict + " (correctness clean, spread above $MaxVariancePct%)")
    foreach ($w in $warnings) { [void]$md.Add("- WARN " + $w) }
} else {
    [void]$md.Add("Result: FAIL")
    foreach ($p in $problems) { [void]$md.Add("- " + $p) }
    foreach ($w in $warnings) { [void]$md.Add("- WARN " + $w) }
}

$mdPath = Join-Path $OutDir "baseline.md"
[IO.File]::WriteAllText($mdPath, (($md -join "`n") + "`n"), (New-Object Text.UTF8Encoding($false)))
Set-Content -LiteralPath (Join-Path $buildRoot "bench\latest.txt") -Value $mdPath -Encoding ASCII

Write-Host ""
Write-Host ("{0,-13} {1,10} {2,9} {3,8} {4,7} {5,8} {6,9} {7,8}" -f "scenario", "FPS", "spread%", "frame ms", "passes", "barriers", "p95 ms", "max ms")
foreach ($s in $summary) {
    Write-Host ("{0,-13} {1,10} {2,9} {3,8} {4,7} {5,8} {6,9} {7,8}" -f $s.scenario, $s.fpsMedian, $s.spreadPct, $s.frameMs, $s.passes, $s.barriers, $s.p95Ms, $s.maxMs)
}
Write-Host ("[bench] baseline -> {0}" -f $mdPath)

if ($warnings.Count -gt 0) {
    Write-Host "[bench] WARN (variance, re-run before suspecting code):"
    foreach ($w in $warnings) { Write-Host ("   " + $w) }
}
if ($problems.Count -gt 0) {
    Write-Host "[bench] PROBLEMS:"
    foreach ($p in $problems) { Write-Host ("   " + $p) }
    exit 1
}
if ($Strict -and $warnings.Count -gt 0) { Write-Host "[bench] FAIL (-Strict: variance counts as failure)"; exit 1 }
Write-Host "[bench] PASS"
exit 0