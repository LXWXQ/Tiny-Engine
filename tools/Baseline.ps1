[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateSet('Save','Diff')][string]$Action,
    [string]$Name = "default",
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [int]$Frames = 150,
    [int]$Frame = 90,
    [int]$WarmupFrames = 30,
    [double]$MaxMeanDiff = 2.0,
    [double]$MaxPassRegressionPct = 25.0,
    [double]$MaxPassRegressionMs = 0.5,
    [int]$MaxWarningsIncrease = 0,
    [string]$VulkanSdk = $env:VULKAN_SDK
)

# ---------------------------------------------------------------------------
# P3-2: baseline snapshot + automatic three-dimension regression diff.
#
# One engine run yields all three dimensions at once:
#   * GPU timing : --gpu-timing --gpu-timing-csv <csv>        -> per-pass mean ms
#   * validation : stdout "[Validation] errors=N warnings=M"
#   * visual     : --screenshot <png> --screenshot-frame <n>  -> mean pixel diff
#
#   tools/Baseline.ps1 -Action Save -Frames 150
#   tools/Baseline.ps1 -Action Diff -Frames 150
#
# Diff exits non-zero if any dimension regresses beyond the thresholds.
# Absolute GPU ms are Debug-build and noisy: this is RELATIVE regression
# detection, so thresholds are loose and the first WarmupFrames are dropped.
#
# IMPORTANT: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1
# without a BOM as ANSI, and a non-ASCII comment can corrupt the next line.
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

# Start-Process -ArgumentList joins an array with spaces and does NOT quote the
# elements. This repo may live in a path containing spaces ("Vs code Demo"), so
# any argument that is a path has to be quoted explicitly here, otherwise the
# child process receives it split into several arguments.
function Quote-NativeArg([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}
if (-not [string]::IsNullOrWhiteSpace($VulkanSdk)) { $env:VULKAN_SDK = $VulkanSdk }

# Accept an absolute -BuildDir as well as a repo-relative one.
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repoRoot $BuildDir }
$exe = Join-Path $buildRoot (Join-Path $Config "TinyEngine.exe")
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe (build first)" }

# The screenshot frame must fall inside the selftest range and after warm-up.
if ($Frame -ge $Frames) { $Frame = [Math]::Max(1, $Frames - 10) }
if (($Frame -le $WarmupFrames) -and ($Frames -gt $WarmupFrames)) { $Frame = [Math]::Min($Frames, $WarmupFrames + 1) }

function Get-PassMeans([string]$csv) {
    $samples = New-Object System.Collections.ArrayList
    foreach ($line in (Get-Content -LiteralPath $csv -Encoding UTF8)) {
        $f = $line.Split(',')
        if ($f.Length -lt 3) { continue }
        $fr = 0
        if (-not [int]::TryParse($f[0], [ref]$fr)) { continue }   # also skips the header
        if ($fr -le $WarmupFrames) { continue }                   # drop warm-up frames
        $ms = 0.0
        if (-not [double]::TryParse($f[2], [ref]$ms)) { continue }
        [void]$samples.Add([pscustomobject]@{ pass = $f[1]; ms = $ms })
    }
    if ($samples.Count -eq 0) { return @() }
    return @($samples | Group-Object pass | ForEach-Object {
        $vals = @($_.Group | ForEach-Object { $_.ms })
        [pscustomobject]@{ pass = $_.Name; avg_ms = [math]::Round((($vals | Measure-Object -Average).Average), 4) }
    } | Sort-Object pass)
}

function Invoke-Collect([string]$dir) {
    New-Item -ItemType Directory -Force $dir | Out-Null
    $csv = Join-Path $dir "gpu_timing.csv"
    $png = Join-Path $dir "screenshot.png"
    $stdout = Join-Path $dir "run.stdout.log"
    $stderr = Join-Path $dir "run.stderr.log"

    $engineArgs = @("--selftest", "$Frames", "--gpu-timing", "--gpu-timing-csv", $csv,
                    "--screenshot", $png, "--screenshot-frame", "$Frame")
    Write-Host "[baseline] running: TinyEngine.exe --selftest $Frames --gpu-timing --screenshot(frame $Frame)"
    $argLine = ($engineArgs | ForEach-Object { Quote-NativeArg $_ }) -join " "
    $p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru -NoNewWindow

    $outText = [string](Get-Content -LiteralPath $stdout -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
    $errors = -1; $warnings = -1
    $m = [regex]::Match($outText, '\[Validation\] errors=(\d+) warnings=(\d+)')
    if ($m.Success) { $errors = [int]$m.Groups[1].Value; $warnings = [int]$m.Groups[2].Value }

    $passes = @()
    if (Test-Path -LiteralPath $csv) { $passes = @(Get-PassMeans $csv) }

    return [pscustomobject]@{
        exitCode    = $p.ExitCode
        stderrBytes = (Get-Item -LiteralPath $stderr).Length
        errors      = $errors
        warnings    = $warnings
        passes      = $passes
        pngPath     = $png
    }
}

Add-Type -AssemblyName System.Drawing
function Get-ImageMeanDiff([string]$a, [string]$b) {
    $ba = [System.Drawing.Bitmap]::FromFile((Resolve-Path $a).Path)
    $bb = [System.Drawing.Bitmap]::FromFile((Resolve-Path $b).Path)
    try {
        if ($ba.Width -ne $bb.Width -or $ba.Height -ne $bb.Height) {
            throw "size mismatch $($ba.Width)x$($ba.Height) vs $($bb.Width)x$($bb.Height)"
        }
        $step = 2; $sum = 0.0; $n = 0
        for ($y = 0; $y -lt $ba.Height; $y += $step) {
            for ($x = 0; $x -lt $ba.Width; $x += $step) {
                $c1 = $ba.GetPixel($x, $y); $c2 = $bb.GetPixel($x, $y)
                $sum += ([Math]::Abs($c1.R - $c2.R) + [Math]::Abs($c1.G - $c2.G) + [Math]::Abs($c1.B - $c2.B)) / 3.0
                $n++
            }
        }
        return [math]::Round($sum / [Math]::Max($n, 1), 4)
    }
    finally { $ba.Dispose(); $bb.Dispose() }
}

$baseDir = Join-Path $buildRoot "baseline\$Name"

if ($Action -eq 'Save') {
    if (Test-Path -LiteralPath $baseDir) { Remove-Item -LiteralPath $baseDir -Recurse -Force }
    $r = Invoke-Collect $baseDir
    if ($r.exitCode -ne 0 -or $r.stderrBytes -ne 0) { throw "baseline run failed (exit=$($r.exitCode) stderr=$($r.stderrBytes))" }
    if (-not (Test-Path -LiteralPath (Join-Path $baseDir "screenshot.png"))) {
        throw "baseline screenshot missing (frame $Frame of $Frames frames)"
    }
    if (@($r.passes).Count -eq 0) { throw "baseline gpu timing is empty (frame $Frame of $Frames frames)" }

    $json = [ordered]@{
        timestamp    = (Get-Date -Format "yyyyMMdd-HHmmss")
        frames       = $Frames
        frame        = $Frame
        warmupFrames = $WarmupFrames
        validation   = [ordered]@{ errors = $r.errors; warnings = $r.warnings }
        passes       = $r.passes
        reference    = "screenshot.png"
    }
    $json | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $baseDir "baseline.json") -Encoding UTF8
    Write-Host "[baseline] saved '$Name' -> $baseDir"
    Write-Host ("[baseline]   validation errors={0} warnings={1}  passes={2}" -f $r.errors, $r.warnings, @($r.passes).Count)
    exit 0
}

$baseJsonPath = Join-Path $baseDir "baseline.json"
if (-not (Test-Path -LiteralPath $baseJsonPath)) { throw "no baseline '$Name' at $baseDir (run: -Action Save)" }
$base = Get-Content -LiteralPath $baseJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
$refPng = Join-Path $baseDir $base.reference

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$curDir = Join-Path $buildRoot "baseline\_current_$stamp"
$cur = Invoke-Collect $curDir

$problems = @()
if ($cur.exitCode -ne 0) { $problems += "current run exit=$($cur.exitCode)" }
if ($cur.stderrBytes -ne 0) { $problems += "current run stderr=$($cur.stderrBytes) bytes" }

# --- validation ---
if ($cur.errors -lt 0) { $problems += "no [Validation] summary in current run" }
elseif ($cur.errors -gt $base.validation.errors) { $problems += "validation errors $($base.validation.errors) -> $($cur.errors)" }
if ($cur.warnings -ge 0 -and $cur.warnings -gt ($base.validation.warnings + $MaxWarningsIncrease)) {
    $problems += "validation warnings $($base.validation.warnings) -> $($cur.warnings)"
}

# --- GPU timing (per pass) ---
$baseByPass = @{}
foreach ($p in @($base.passes)) { if ($null -ne $p) { $baseByPass[$p.pass] = [double]$p.avg_ms } }

$gpuRows = @()
foreach ($p in @($cur.passes)) {
    if ($null -eq $p) { continue }
    $c = [double]$p.avg_ms
    if (-not $baseByPass.ContainsKey($p.pass)) {
        $gpuRows += [pscustomobject]@{ pass = $p.pass; base_ms = $null; cur_ms = $c; delta_ms = $null; pct = $null; flag = "new" }
        continue
    }
    $b = $baseByPass[$p.pass]
    $delta = [math]::Round($c - $b, 4)
    $pct = if ($b -gt 0) { [math]::Round(100.0 * ($c - $b) / $b, 2) } else { 0 }
    $limit = [math]::Max($MaxPassRegressionMs, $b * $MaxPassRegressionPct / 100.0)
    $flag = if ($delta -gt $limit) { "REGRESS" } else { "ok" }
    $gpuRows += [pscustomobject]@{ pass = $p.pass; base_ms = $b; cur_ms = $c; delta_ms = $delta; pct = $pct; flag = $flag }
}
foreach ($row in $gpuRows) {
    if ($row.flag -eq "REGRESS") { $problems += ("gpu pass {0}: {1} -> {2} ms (+{3}%)" -f $row.pass, $row.base_ms, $row.cur_ms, $row.pct) }
}

# --- visual ---
$visualDiff = $null
if ((Test-Path -LiteralPath $refPng) -and (Test-Path -LiteralPath $cur.pngPath)) {
    $visualDiff = Get-ImageMeanDiff $refPng $cur.pngPath
    if ($visualDiff -gt $MaxMeanDiff) { $problems += "visual mean diff $visualDiff > $MaxMeanDiff" }
} else { $problems += "screenshot missing for visual diff" }

$diff = [ordered]@{
    timestamp      = $stamp
    baseline       = $Name
    validation     = [ordered]@{ base_errors = $base.validation.errors; base_warnings = $base.validation.warnings; cur_errors = $cur.errors; cur_warnings = $cur.warnings }
    gpuTiming      = $gpuRows
    visualMeanDiff = $visualDiff
    problems       = $problems
}
$diff | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $curDir "diff.json") -Encoding UTF8

Write-Host ""
Write-Host ("GPU timing (base -> cur, ms)   [flag if +>{0}% and +>{1} ms]" -f $MaxPassRegressionPct, $MaxPassRegressionMs)
foreach ($row in ($gpuRows | Sort-Object { if ($null -ne $_.delta_ms) { [double]$_.delta_ms } else { 0 } } -Descending)) {
    $pctTxt = if ($null -ne $row.pct) { "{0,7}%" -f $row.pct } else { "      -" }
    Write-Host ("  {0,-18} {1,8} -> {2,8}  {3,8} ms {4}  {5}" -f $row.pass, $row.base_ms, $row.cur_ms, $row.delta_ms, $pctTxt, $row.flag)
}
Write-Host ("validation: errors {0}->{1}  warnings {2}->{3}" -f $base.validation.errors, $cur.errors, $base.validation.warnings, $cur.warnings)
Write-Host ("visual    : mean diff = {0}  (limit {1})" -f $visualDiff, $MaxMeanDiff)
Write-Host ("[baseline] diff report -> {0}\diff.json" -f $curDir)

if ($problems.Count -gt 0) {
    Write-Host "[baseline] REGRESSION:"
    $problems | ForEach-Object { Write-Host "   $_" }
    exit 1
}
Write-Host "[baseline] DIFF PASS (no regression)"
exit 0