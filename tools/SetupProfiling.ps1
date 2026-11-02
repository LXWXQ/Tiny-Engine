[CmdletBinding()]
param(
    [string]$DependenciesRoot = "",
    [string]$TracyVersion = "v0.14.1",
    [switch]$SkipTracy,
    [switch]$SkipRenderDoc,
    [switch]$Force
)

# ---------------------------------------------------------------------------
# Optional P1 profiling dependencies (Tracy + RenderDoc in-app header).
#
# These are NOT required to build/run the engine. They only enable the
# opt-in profiling capabilities (SENTINEL_ENABLE_TRACY / SENTINEL_ENABLE_RENDERDOC),
# which stay compile-time OFF by default.
#
#   * Tracy source  -> 3rdParty/tracy/public   (tracy/Tracy.hpp, TracyClient.cpp)
#   * Tracy binaries-> 3rdParty/tracy/bin      (tracy-profiler / tracy-capture / tracy-csvexport)
#   * RenderDoc app header -> 3rdParty/renderdoc/renderdoc_app.h
#
# ASCII-only on purpose (PowerShell 5.1 reads .ps1 without BOM as ANSI).
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($DependenciesRoot)) {
    $DependenciesRoot = Join-Path $repoRoot "3rdParty"
}
$cacheDir = Join-Path $DependenciesRoot ".cache"
New-Item -ItemType Directory -Force $cacheDir | Out-Null

function Download([string]$Url, [string]$Dest) {
    if ((Test-Path -LiteralPath $Dest) -and -not $Force) { Write-Host "[profiling] cached $([IO.Path]::GetFileName($Dest))"; return }
    Write-Host "[profiling] downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing
}

$tracyVer = $TracyVersion.TrimStart("v")

if (-not $SkipTracy) {
    $tracyDir = Join-Path $DependenciesRoot "tracy"
    $tracyHeader = Join-Path $tracyDir "public/tracy/Tracy.hpp"
    if ((Test-Path -LiteralPath $tracyHeader) -and -not $Force) {
        Write-Host "[profiling] Tracy source already present"
    } else {
        $zip = Join-Path $cacheDir "tracy-$TracyVersion-src.zip"
        Download "https://github.com/wolfpld/tracy/archive/refs/tags/$TracyVersion.zip" $zip
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ("sentinel-tracy-" + [Guid]::NewGuid().ToString("N"))
        Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
        $hit = Get-ChildItem -LiteralPath $tmp -Recurse -File -Filter "Tracy.hpp" |
               Where-Object { $_.FullName -match "\\public\\tracy\\Tracy\.hpp$" } | Select-Object -First 1
        if ($null -eq $hit) { throw "Tracy source archive layout unexpected" }
        $publicSrc = Split-Path (Split-Path $hit.FullName -Parent) -Parent
        if (Test-Path -LiteralPath $tracyDir) { Remove-Item -LiteralPath $tracyDir -Recurse -Force }
        New-Item -ItemType Directory -Force $tracyDir | Out-Null
        Copy-Item -LiteralPath $publicSrc -Destination (Join-Path $tracyDir "public") -Recurse -Force
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "[profiling] Tracy source -> 3rdParty/tracy/public"
    }

    # Prebuilt server binaries (profiler / capture / csvexport).
    $binDir = Join-Path $tracyDir "bin"
    $captureExe = Join-Path $binDir "tracy-capture.exe"
    if ((Test-Path -LiteralPath $captureExe) -and -not $Force) {
        Write-Host "[profiling] Tracy binaries already present"
    } else {
        $zip = Join-Path $cacheDir "tracy-$TracyVersion-win.zip"
        Download "https://github.com/wolfpld/tracy/releases/download/$TracyVersion/windows-$tracyVer.zip" $zip
        $tmp = Join-Path ([IO.Path]::GetTempPath()) ("sentinel-tracybin-" + [Guid]::NewGuid().ToString("N"))
        Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force
        $hit = Get-ChildItem -LiteralPath $tmp -Recurse -File -Filter "tracy-capture.exe" | Select-Object -First 1
        if ($null -eq $hit) { throw "Tracy binary archive layout unexpected" }
        if (Test-Path -LiteralPath $binDir) { Remove-Item -LiteralPath $binDir -Recurse -Force }
        New-Item -ItemType Directory -Force $binDir | Out-Null
        Get-ChildItem -LiteralPath (Split-Path $hit.FullName -Parent) -File | Copy-Item -Destination $binDir -Force
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "[profiling] Tracy binaries -> 3rdParty/tracy/bin"
    }
}

if (-not $SkipRenderDoc) {
    $rdDir = Join-Path $DependenciesRoot "renderdoc"
    $rdHeader = Join-Path $rdDir "renderdoc_app.h"
    if ((Test-Path -LiteralPath $rdHeader) -and -not $Force) {
        Write-Host "[profiling] renderdoc_app.h already present"
    } else {
        New-Item -ItemType Directory -Force $rdDir | Out-Null
        Download "https://raw.githubusercontent.com/baldurk/renderdoc/master/renderdoc/api/app/renderdoc_app.h" $rdHeader
        Write-Host "[profiling] renderdoc_app.h -> 3rdParty/renderdoc/renderdoc_app.h"
    }
}

Write-Host "[profiling] done. Enable with:"
Write-Host "    cmake -S . -B build -DSENTINEL_ENABLE_TRACY=ON -DSENTINEL_ENABLE_RENDERDOC=ON"