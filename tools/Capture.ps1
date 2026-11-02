[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [int]$Frame = 120,
    [string]$Out = "",
    [string]$RenderDocCmd = ""
)

# ---------------------------------------------------------------------------
# P1-2: capture one frame with RenderDoc (out-of-process CLI path).
#
# This uses RenderDoc's own CLI (renderdoccmd), which injects RenderDoc into a
# freshly launched process. Combined with the engine flag --capture-frame N the
# in-app hook triggers the capture exactly on frame N.
#
# Requirements:
#   * RenderDoc installed (renderdoccmd on PATH, or pass -RenderDocCmd <path>).
#   * The engine must be built with -DSENTINEL_ENABLE_RENDERDOC=ON (otherwise
#     --capture-frame is a no-op).
#
# ASCII-only on purpose (PowerShell 5.1 reads .ps1 without BOM as ANSI).
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

$exe = Join-Path $repoRoot (Join-Path $BuildDir (Join-Path $Config "VulkanEngine.exe"))
if (-not (Test-Path -LiteralPath $exe)) { throw "Executable not found: $exe (build first)" }

if ([string]::IsNullOrWhiteSpace($RenderDocCmd)) {
    $cmd = Get-Command renderdoccmd -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { $RenderDocCmd = $cmd.Source }
}
if ([string]::IsNullOrWhiteSpace($RenderDocCmd)) {
    $candidates = @(
        "C:\Program Files\RenderDoc\renderdoccmd.exe",
        "C:\Program Files (x86)\RenderDoc\renderdoccmd.exe"
    )
    foreach ($c in $candidates) { if (Test-Path -LiteralPath $c) { $RenderDocCmd = $c; break } }
}
if ([string]::IsNullOrWhiteSpace($RenderDocCmd) -or -not (Test-Path -LiteralPath $RenderDocCmd)) {
    Write-Host "[capture] renderdoccmd not found."
    Write-Host "[capture] Install RenderDoc, or pass -RenderDocCmd <path to renderdoccmd.exe>."
    Write-Host "[capture] Then rebuild with: cmake -S . -B $BuildDir -DSENTINEL_ENABLE_RENDERDOC=ON"
    exit 2
}

if ([string]::IsNullOrWhiteSpace($Out)) {
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $dir = Join-Path $repoRoot (Join-Path $BuildDir "captures")
    New-Item -ItemType Directory -Force $dir | Out-Null
    $Out = Join-Path $dir "$stamp.rdc"
}

$frames = [Math]::Max($Frame + 5, 20)
Write-Host "[capture] $RenderDocCmd"
Write-Host "[capture] frame $Frame -> $Out"
& $RenderDocCmd capture --opt-hook-children --wait-for-exit -c "$Out" "$exe" --selftest "$frames" --capture-frame "$Frame"
$code = $LASTEXITCODE

if (Test-Path -LiteralPath $Out) {
    Write-Host ("[capture] capture written: {0} ({1} bytes)" -f $Out, (Get-Item -LiteralPath $Out).Length)
} else {
    Write-Host "[capture] no .rdc produced (RenderDoc may not have injected)."
}
exit $code