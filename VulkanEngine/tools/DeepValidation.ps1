[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [int]$Frames = 60,
    [string]$Modes = "sync,bp,gpuav,printf",
    [switch]$AllowSyncErrors,
    [string]$OutDir = "",
    [string]$VulkanSdk = $env:VULKAN_SDK
)

# ---------------------------------------------------------------------------
# P2-3: deep validation job (vendor-neutral, Khronos validation layer only).
#
# Runs the engine once per validation feature set and collects structured
# reports. This is a SEPARATE job: it never touches the default Validate.ps1
# baseline (17 tests) and is not part of release acceptance.
#
#   sync   -> synchronization validation (hazard detection)   <- gating
#   bp     -> best practices (performance hints, warnings)    <- report only
#   gpuav  -> GPU-assisted validation (shader OOB)            <- report only
#   printf -> debug printf shader logging                     <- report only
#
# Expected non-errors: GPU-AV emits "WARNING-Setting-Limit-Adjusted" notices
# while it force-enables features it needs; that is informational.
#
# ASCII-only (PowerShell 5.1 reads .ps1 without a BOM as ANSI).
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
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe (build first)" }

if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $outDir = Join-Path $repoRoot (Join-Path $BuildDir "deepvalidation\$stamp")
} else {
    $outDir = if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $repoRoot $OutDir }
}
New-Item -ItemType Directory -Force $outDir | Out-Null

$modeList = $Modes -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" }
$results = @()
$failed = @()

foreach ($mode in $modeList) {
    $jl = Join-Path $outDir "$mode.jsonl"
    $stdout = Join-Path $outDir "$mode.stdout.log"
    $stderr = Join-Path $outDir "$mode.stderr.log"
    Write-Host "[deep] mode=$mode frames=$Frames"
    $deepArgs = @("--selftest", "$Frames", "--validation-features", $mode, "--validation-jsonl", $jl)
    $deepArgLine = ($deepArgs | ForEach-Object { Quote-NativeArg $_ }) -join " "
    $p = Start-Process -FilePath $exe `
        -ArgumentList $deepArgLine `
        -WorkingDirectory $repoRoot -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru -NoNewWindow
    $stderrBytes = (Get-Item -LiteralPath $stderr).Length
    $outText = [string](Get-Content -LiteralPath $stdout -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)

    $errors = 0; $warnings = 0
    $m = [regex]::Match($outText, '\[Validation\] errors=(\d+) warnings=(\d+)')
    if ($m.Success) { $errors = [int]$m.Groups[1].Value; $warnings = [int]$m.Groups[2].Value }
    else { $failed += "$mode : no [Validation] summary found" }

    $vuids = @()
    if (Test-Path -LiteralPath $jl) {
        $ids = Get-Content -LiteralPath $jl -Encoding UTF8 | ForEach-Object { ($_ | ConvertFrom-Json).id }
        $vuids = $ids | Group-Object | Sort-Object Count -Descending |
                 Select-Object -First 8 | ForEach-Object { [pscustomobject]@{ id = $_.Name; count = $_.Count } }
    }

    if ($p.ExitCode -ne 0) { $failed += "$mode : exit code $($p.ExitCode)" }
    if ($stderrBytes -ne 0) { $failed += "$mode : stderr $stderrBytes bytes" }
    if ($mode -eq "sync" -and $errors -gt 0 -and -not $AllowSyncErrors) {
        $failed += "$mode : $errors sync hazard(s)"
    }

    $results += [pscustomobject]@{
        mode = $mode; exitCode = $p.ExitCode; stderrBytes = $stderrBytes
        errors = $errors; warnings = $warnings; topVuids = $vuids
    }
}

$summary = [ordered]@{
    timestamp = $stamp
    frames    = $Frames
    modes     = $modeList
    results   = $results
    failed    = $failed
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outDir "summary.json") -Encoding UTF8

Write-Host ""
Write-Host ("{0,-8} {1,8} {2,8} {3,10} {4,10}" -f "mode", "exit", "stderr", "errors", "warnings")
foreach ($r in $results) {
    Write-Host ("{0,-8} {1,8} {2,8} {3,10} {4,10}" -f $r.mode, $r.exitCode, $r.stderrBytes, $r.errors, $r.warnings)
    foreach ($v in $r.topVuids) { if ($v.count -gt 0) { Write-Host ("           {0}x {1}" -f $v.count, $v.id) } }
}
Write-Host ""
Write-Host "[deep] report -> $outDir\summary.json"

if ($failed.Count -gt 0) {
    Write-Host "[deep] FAIL:"
    $failed | ForEach-Object { Write-Host "   $_" }
    exit 1
}
Write-Host "[deep] DEEP VALIDATION PASS"
exit 0