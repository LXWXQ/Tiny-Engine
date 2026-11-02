[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [int]$Frame = 60,
    [string]$Out = "",
    [string]$Golden = "",
    [double]$MaxMeanDiff = 2.0,
    [string]$VulkanSdk = $env:VULKAN_SDK
)

# ---------------------------------------------------------------------------
# P1-4: headless screenshot + optional golden-image regression.
#
#   - Runs the engine with --screenshot to dump one frame to PNG.
#   - With -Golden <png>, compares against a reference image and reports a
#     numeric diff score (mean absolute per-channel difference, 0..255).
#
# ASCII-only on purpose: Windows PowerShell 5.1 reads .ps1 without a BOM as ANSI.
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

if ([string]::IsNullOrWhiteSpace($Out)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $dir = Join-Path $repoRoot (Join-Path $BuildDir "screenshots")
    New-Item -ItemType Directory -Force $dir | Out-Null
    $Out = Join-Path $dir "$stamp.png"
}

$stdout = [IO.Path]::ChangeExtension($Out, ".stdout.log")
$stderr = [IO.Path]::ChangeExtension($Out, ".stderr.log")
$frames = [Math]::Max($Frame + 5, 20)

Write-Host "[screenshot] running: --selftest $frames --screenshot --screenshot-frame $Frame"
$shotArgs = @("--selftest", "$frames", "--screenshot", $Out, "--screenshot-frame", "$Frame")
$shotArgLine = ($shotArgs | ForEach-Object { Quote-NativeArg $_ }) -join " "
$p = Start-Process -FilePath $exe -ArgumentList $shotArgLine `
     -WorkingDirectory $repoRoot -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru -NoNewWindow

$stderrBytes = (Get-Item -LiteralPath $stderr).Length
$pngOk = Test-Path -LiteralPath $Out
Write-Host ("[screenshot] exit={0} stderr={1} bytes png={2}" -f $p.ExitCode, $stderrBytes, $pngOk)
if ($p.ExitCode -ne 0 -or $stderrBytes -ne 0 -or -not $pngOk) {
    Get-Content -LiteralPath $stdout -Tail 30
    exit 1
}
Write-Host "[screenshot] result -> $Out"

if ([string]::IsNullOrWhiteSpace($Golden)) { exit 0 }
if (-not (Test-Path -LiteralPath $Golden)) { throw "Golden image not found: $Golden" }

Add-Type -AssemblyName System.Drawing
$b1 = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Golden).Path)
$b2 = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Out).Path)
try {
    if ($b1.Width -ne $b2.Width -or $b1.Height -ne $b2.Height) {
        throw "Size mismatch: golden $($b1.Width)x$($b1.Height) vs new $($b2.Width)x$($b2.Height)"
    }

    $step = 2
    $sum = 0.0; $max = 0; $count = 0; $over8 = 0
    for ($y = 0; $y -lt $b1.Height; $y += $step) {
        for ($x = 0; $x -lt $b1.Width; $x += $step) {
            $c1 = $b1.GetPixel($x, $y); $c2 = $b2.GetPixel($x, $y)
            $d = ([Math]::Abs($c1.R - $c2.R) + [Math]::Abs($c1.G - $c2.G) + [Math]::Abs($c1.B - $c2.B)) / 3.0
            $sum += $d; if ($d -gt $max) { $max = $d }; if ($d -gt 8) { $over8++ }; $count++
        }
    }
    $mean = [Math]::Round($sum / [Math]::Max($count, 1), 4)
    $pct = [Math]::Round(100.0 * $over8 / [Math]::Max($count, 1), 3)
    Write-Host ("[screenshot] diff: mean={0} max={1} pct_pixels_over_8={2}% (samples={3}, step={4})" -f $mean, [Math]::Round($max,1), $pct, $count, $step)

    if ($mean -gt $MaxMeanDiff) {
        Write-Host ("[screenshot] REGRESSION: mean diff {0} > threshold {1}" -f $mean, $MaxMeanDiff)
        exit 1
    }
    Write-Host "[screenshot] golden compare PASS"
    exit 0
}
finally {
    $b1.Dispose(); $b2.Dispose()
}