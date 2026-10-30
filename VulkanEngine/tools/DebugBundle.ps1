[CmdletBinding()]
param(
    [string]$Name = "",
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [int]$Frames = 150,
    [int]$ScreenshotFrame = 90,
    [int]$WarmupFrames = 30,
    [int]$DeepFrames = 40,
    [switch]$SkipDeepValidation,
    [switch]$SkipBaselineDiff,
    [switch]$SkipScreenshot,
    [string]$VulkanSdk = $env:VULKAN_SDK
)

# ---------------------------------------------------------------------------
# P3-1: one-click full evidence bundle.
#
#   tools/DebugBundle.ps1 -Frames 150
#
# Output: <BuildDir>/bundles/<timestamp>/  (or .../<Name>/ when -Name is given)
#   manifest.json          machine-readable index (steps, counts, artifact list)
#   summary.md             human/LLM-readable overview
#   build_info.txt         engine --build-info (compiled-in capabilities)
#   env.txt                vulkaninfo --summary
#   run/                   main run: stdout/stderr, gpu_timing.csv, screenshot.png
#   validation/            validation.jsonl (only if the layer emitted messages)
#   deep/                  deep validation job report (sync/bp/gpuav/printf)
#   baseline_diff/         regression diff vs the stored baseline (if one exists)
#
# Every step records its own status; the bundle is a superset of what
# tools/Profile.ps1 produces (Profile.ps1 is the light version).
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

$buildRoot = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $repoRoot $BuildDir }
$exe = Join-Path $buildRoot (Join-Path $Config "VulkanEngine.exe")
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe (build first)" }

# Keep the screenshot frame inside the selftest range and after warm-up.
if ($ScreenshotFrame -ge $Frames) { $ScreenshotFrame = [Math]::Max(1, $Frames - 10) }
if (($ScreenshotFrame -le $WarmupFrames) -and ($Frames -gt $WarmupFrames)) {
    $ScreenshotFrame = [Math]::Min($Frames, $WarmupFrames + 1)
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$bundleName = if ([string]::IsNullOrWhiteSpace($Name)) { $stamp } else { $Name }
$bundleDir = Join-Path $buildRoot "bundles\$bundleName"
if (Test-Path -LiteralPath $bundleDir) { Remove-Item -LiteralPath $bundleDir -Recurse -Force }
$runDir = Join-Path $bundleDir "run"
$valDir = Join-Path $bundleDir "validation"
$deepDir = Join-Path $bundleDir "deep"
$diffDir = Join-Path $bundleDir "baseline_diff"
foreach ($d in @($bundleDir, $runDir, $valDir, $deepDir, $diffDir)) {
    New-Item -ItemType Directory -Force $d | Out-Null
}

$steps = New-Object System.Collections.ArrayList
function Add-Step([string]$name, [string]$status, [string]$detail) {
    [void]$steps.Add([pscustomobject]@{ name = $name; status = $status; detail = $detail })
    Write-Host ("[bundle] {0,-20} {1,-8} {2}" -f $name, $status, $detail)
}

# ---------------- 1) build info ----------------
$biLog = Join-Path $bundleDir "build_info.txt"
$biErr = Join-Path $bundleDir "build_info.err.log"
$p = Start-Process -FilePath $exe -ArgumentList "--build-info" -WorkingDirectory $repoRoot `
    -RedirectStandardOutput $biLog -RedirectStandardError $biErr -Wait -PassThru -NoNewWindow
if ($p.ExitCode -eq 0) { Add-Step "build_info" "ok" "build_info.txt" }
else { Add-Step "build_info" "fail" "exit=$($p.ExitCode)" }

# ---------------- 2) environment snapshot ----------------
$envTxt = Join-Path $bundleDir "env.txt"
$vulkaninfo = $null
if (-not [string]::IsNullOrWhiteSpace($env:VULKAN_SDK)) {
    $sdkInfo = Join-Path $env:VULKAN_SDK "Bin\vulkaninfoSDK.exe"
    if (Test-Path -LiteralPath $sdkInfo) { $vulkaninfo = $sdkInfo }
}
if ($null -eq $vulkaninfo) {
    $cmd = Get-Command vulkaninfo -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { $vulkaninfo = $cmd.Source }
}
if ($null -ne $vulkaninfo) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $vulkaninfo --summary 2>$null | Out-File -LiteralPath $envTxt -Encoding utf8
    $ErrorActionPreference = $prev
    Add-Step "environment" "ok" "env.txt ($([IO.Path]::GetFileName($vulkaninfo)))"
} else {
    "vulkaninfo not found (set VULKAN_SDK for the SDK copy)" | Set-Content -LiteralPath $envTxt
    Add-Step "environment" "skip" "vulkaninfo not found"
}

# ---------------- 3) main run: gpu timing + screenshot + validation jsonl ----------------
$csv = Join-Path $runDir "gpu_timing.csv"
$png = Join-Path $runDir "screenshot.png"
$valJsonl = Join-Path $valDir "validation.jsonl"
$stdout = Join-Path $runDir "stdout.log"
$stderr = Join-Path $runDir "stderr.log"

$engineArgs = @("--selftest", "$Frames")
if (-not $SkipScreenshot) {
    $engineArgs += @("--screenshot", $png, "--screenshot-frame", "$ScreenshotFrame")
}
$engineArgs += @("--gpu-timing", "--gpu-timing-csv", $csv, "--validation-jsonl", $valJsonl)

Write-Host "[bundle] running: VulkanEngine.exe $($engineArgs -join ' ')"
$argLine = ($engineArgs | ForEach-Object { Quote-NativeArg $_ }) -join " "
$p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $repoRoot `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru -NoNewWindow
$stderrBytes = (Get-Item -LiteralPath $stderr).Length
$outText = [string](Get-Content -LiteralPath $stdout -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)

$valErrors = -1; $valWarnings = -1
$m = [regex]::Match($outText, '\[Validation\] errors=(\d+) warnings=(\d+)')
if ($m.Success) { $valErrors = [int]$m.Groups[1].Value; $valWarnings = [int]$m.Groups[2].Value }

if ($p.ExitCode -eq 0 -and $stderrBytes -eq 0) {
    Add-Step "main_run" "ok" "frames=$Frames stderr=0"
} else {
    Add-Step "main_run" "fail" "exit=$($p.ExitCode) stderr=$stderrBytes"
}

if ($valErrors -ge 0) {
    $jsonlNote = if (Test-Path -LiteralPath $valJsonl) { "validation.jsonl" } else { "no messages (jsonl not created)" }
    Add-Step "validation" "ok" "errors=$valErrors warnings=$valWarnings; $jsonlNote"
} else {
    Add-Step "validation" "fail" "no [Validation] summary in stdout"
}

# ---------------- 4) gpu timing summary ----------------
$gpuSummary = @()
if (Test-Path -LiteralPath $csv) {
    $samples = New-Object System.Collections.ArrayList
    foreach ($line in (Get-Content -LiteralPath $csv -Encoding UTF8)) {
        $f = $line.Split(',')
        if ($f.Length -lt 3) { continue }
        $fr = 0
        if (-not [int]::TryParse($f[0], [ref]$fr)) { continue }
        if ($fr -le $WarmupFrames) { continue }
        $ms = 0.0
        if (-not [double]::TryParse($f[2], [ref]$ms)) { continue }
        [void]$samples.Add([pscustomobject]@{ pass = $f[1]; ms = $ms })
    }
    if ($samples.Count -gt 0) {
        $gpuSummary = @($samples | Group-Object pass | ForEach-Object {
            $vals = @($_.Group | ForEach-Object { $_.ms })
            [pscustomobject]@{
                pass   = $_.Name
                avg_ms = [math]::Round((($vals | Measure-Object -Average).Average), 4)
                frames = $vals.Count
            }
        } | Sort-Object avg_ms -Descending)
    }
    Add-Step "gpu_timing" "ok" "$(($gpuSummary | Measure-Object).Count) passes"
} else {
    Add-Step "gpu_timing" "fail" "gpu_timing.csv missing"
}

# ---------------- 5) deep validation ----------------
if ($SkipDeepValidation) {
    Add-Step "deep_validation" "skip" "-SkipDeepValidation"
} else {
    $deepScript = Join-Path $PSScriptRoot "DeepValidation.ps1"
    $deepLog = Join-Path $deepDir "deep.log"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $deepScript -OutDir $deepDir -Frames $DeepFrames -BuildDir $BuildDir -Config $Config *> $deepLog
    $deepExit = $LASTEXITCODE
    $ErrorActionPreference = $prev
    if ($deepExit -eq 0) { Add-Step "deep_validation" "ok" "deep/summary.json" }
    else { Add-Step "deep_validation" "fail" "exit=$deepExit (see deep/deep.log)" }
}

# ---------------- 6) baseline diff ----------------
$baselineJson = Join-Path $buildRoot "baseline\default\baseline.json"
if ($SkipBaselineDiff) {
    Add-Step "baseline_diff" "skip" "-SkipBaselineDiff"
} elseif (-not (Test-Path -LiteralPath $baselineJson)) {
    Add-Step "baseline_diff" "skip" "no baseline 'default' (run tools/Baseline.ps1 -Action Save)"
} else {
    $baselineScript = Join-Path $PSScriptRoot "Baseline.ps1"
    $diffLog = Join-Path $diffDir "baseline.log"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $baselineScript -Action Diff -Name "default" -BuildDir $BuildDir -Config $Config -Frames $Frames *> $diffLog
    $diffExit = $LASTEXITCODE
    $ErrorActionPreference = $prev
    $latest = Get-ChildItem -LiteralPath (Join-Path $buildRoot "baseline") -Directory -Filter "_current_*" -ErrorAction SilentlyContinue |
              Sort-Object Name -Descending | Select-Object -First 1
    if ($null -ne $latest) {
        $src = Join-Path $latest.FullName "diff.json"
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination (Join-Path $diffDir "diff.json") -Force }
    }
    if ($diffExit -eq 0) { Add-Step "baseline_diff" "ok" "baseline_diff/diff.json" }
    else { Add-Step "baseline_diff" "fail" "regression detected (see baseline_diff/baseline.log)" }
}

# ---------------- 7) manifest + summary ----------------
$artifacts = @(Get-ChildItem -LiteralPath $bundleDir -Recurse -File | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{
        path  = $_.FullName.Substring($bundleDir.Length + 1)
        bytes = $_.Length
    }
})

$failedSteps = @($steps | Where-Object { $_.status -eq "fail" })
$ok = ($failedSteps.Count -eq 0)

$manifest = [ordered]@{
    timestamp       = $stamp
    name            = $bundleName
    config          = $Config
    frames          = $Frames
    screenshotFrame = $ScreenshotFrame
    warmupFrames    = $WarmupFrames
    validation      = [ordered]@{ errors = $valErrors; warnings = $valWarnings }
    steps           = $steps
    gpuTiming       = $gpuSummary
    artifacts       = $artifacts
    ok              = $ok
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $bundleDir "manifest.json") -Encoding UTF8

$md = New-Object System.Collections.ArrayList
[void]$md.Add("# Debug bundle $bundleName")
[void]$md.Add("")
[void]$md.Add("- config: $Config")
[void]$md.Add("- frames: $Frames (warm-up skipped: $WarmupFrames)")
[void]$md.Add("- screenshot frame: $ScreenshotFrame")
[void]$md.Add("- validation: errors=$valErrors warnings=$valWarnings")
[void]$md.Add("- overall: $(if ($ok) { 'OK' } else { 'FAILED' })")
[void]$md.Add("")
[void]$md.Add("## Steps")
[void]$md.Add("")
[void]$md.Add("| step | status | detail |")
[void]$md.Add("|---|---|---|")
foreach ($s in $steps) { [void]$md.Add("| $($s.name) | $($s.status) | $($s.detail) |") }
[void]$md.Add("")
[void]$md.Add("## GPU timing (avg ms, warm-up skipped)")
[void]$md.Add("")
[void]$md.Add("| pass | avg_ms | frames |")
[void]$md.Add("|---|---|---|")
foreach ($g in $gpuSummary) { [void]$md.Add("| $($g.pass) | $($g.avg_ms) | $($g.frames) |") }
[void]$md.Add("")
[void]$md.Add("## Artifacts")
[void]$md.Add("")
foreach ($a in $artifacts) { [void]$md.Add("- ``$($a.path)`` ($($a.bytes) bytes)") }
$md -join "`n" | Set-Content -LiteralPath (Join-Path $bundleDir "summary.md") -Encoding UTF8

Write-Host ""
Write-Host "[bundle] -> $bundleDir"
Write-Host "[bundle] manifest.json / summary.md written; ok=$ok"
if (-not $ok) {
    $failedSteps | ForEach-Object { Write-Host "   FAILED: $($_.name) ($($_.detail))" }
    exit 1
}
exit 0