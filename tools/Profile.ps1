[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [int]$Frames = 300,
    [switch]$ValidationSync,
    [switch]$SkipGpuTiming,
    [string]$VulkanSdk = $env:VULKAN_SDK
)

# ---------------------------------------------------------------------------
# P0-5: produce a debug bundle in one call.
#
#   Output: <BuildDir>/profiles/<timestamp>/
#     env.txt           vulkaninfo --summary environment snapshot
#     gpu_timing.csv    per-pass GPU timings (--gpu-timing)
#     run.stdout.log    stdout
#     run.stderr.log    stderr (must be 0 bytes)
#     manifest.json     session metadata + per-pass average GPU time
#
# Switch model (docs/60-debug-profiling/zero-overhead.md): every debug capability is
# opt-in via a flag; default off and zero overhead when off. This script only
# composes those flags and never changes default engine behaviour.
#
# NOTE: keep this file ASCII-only. Windows PowerShell 5.1 reads .ps1 without a
# BOM as ANSI, so non-ASCII comments break parsing.
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

$exe = Join-Path $repoRoot (Join-Path $BuildDir (Join-Path $Config "VulkanEngine.exe"))
if (-not (Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe (run tools/Validate.ps1 first to build)"
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outDir = Join-Path $repoRoot (Join-Path $BuildDir "profiles\$stamp")
New-Item -ItemType Directory -Force $outDir | Out-Null

# 1) environment snapshot (best effort).
# Prefer the SDK build (vulkaninfoSDK.exe); the Win32 system copy can be older
# than the installed layers and would emit spurious warnings.
$envTxt = Join-Path $outDir "env.txt"
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
    $ErrorActionPreference = "Continue"   # native stderr is not a terminating error
    & $vulkaninfo --summary 2>$null | Out-File -LiteralPath $envTxt -Encoding utf8
    $ErrorActionPreference = $prev
    Write-Host "[profile] env snapshot -> env.txt ($vulkaninfo)"
} else {
    "vulkaninfo not found (set VULKAN_SDK for the SDK copy)" | Set-Content -LiteralPath $envTxt
}

# 2) compose engine arguments
$csvPath = Join-Path $outDir "gpu_timing.csv"
$engineArgs = @("--selftest", "$Frames")
if (-not $SkipGpuTiming) { $engineArgs += @("--gpu-timing", "--gpu-timing-csv", $csvPath) }
if ($ValidationSync) { $engineArgs += @("--validation-features", "sync") }

$stdout = Join-Path $outDir "run.stdout.log"
$stderr = Join-Path $outDir "run.stderr.log"
Write-Host "[profile] running: VulkanEngine.exe $($engineArgs -join ' ')"
$argLine = ($engineArgs | ForEach-Object { Quote-NativeArg $_ }) -join " "
$p = Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $repoRoot `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru -NoNewWindow

$stderrBytes = (Get-Item -LiteralPath $stderr).Length
Write-Host ("[profile] exit={0} stderr={1} bytes" -f $p.ExitCode, $stderrBytes)

# 3) aggregate per-pass average GPU time
$summary = @()
if (Test-Path -LiteralPath $csvPath) {
    $rows = Get-Content -LiteralPath $csvPath -Encoding UTF8 | Select-Object -Skip 1
    $summary = $rows | ForEach-Object {
        $f = $_ -split ','
        if ($f.Count -ge 3) { [pscustomobject]@{ pass = $f[1]; ms = [double]$f[2] } }
    } | Group-Object pass | ForEach-Object {
        [pscustomobject]@{
            pass   = $_.Name
            avg_ms = [math]::Round((($_.Group | Measure-Object ms -Average).Average), 4)
            frames = $_.Count
        }
    } | Sort-Object avg_ms -Descending
}

# 4) manifest
$artifacts = @("env.txt", "run.stdout.log", "run.stderr.log")
if (Test-Path -LiteralPath $csvPath) { $artifacts += "gpu_timing.csv" }
$manifest = [ordered]@{
    timestamp        = $stamp
    buildConfig      = $Config
    frames           = $Frames
    gpuTiming        = (-not $SkipGpuTiming)
    validationSync   = [bool]$ValidationSync
    exitCode         = $p.ExitCode
    stderrBytes      = $stderrBytes
    artifacts        = $artifacts
    gpuTimingSummary = $summary
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $outDir "manifest.json") -Encoding UTF8

Write-Host "[profile] debug bundle -> $outDir"
if ($summary.Count -gt 0) {
    Write-Host "[profile] top GPU passes (avg ms):"
    $summary | Select-Object -First 6 | ForEach-Object { Write-Host ("    {0,-18} {1}" -f $_.pass, $_.avg_ms) }
}

if ($p.ExitCode -ne 0 -or $stderrBytes -ne 0) { exit 1 }
exit 0