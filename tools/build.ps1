[CmdletBinding()]
param(
    [string]$BuildDir = "build-ninja",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Debug",
    [string]$Target = "",
    [int]$Jobs = 0,
    [switch]$Reconfigure,
    [switch]$Clean
)

# ---------------------------------------------------------------------------
# Canonical build entry point (Ninja Multi-Config).
#
# Why a script instead of a bare `cmake --build`:
#
#   * MSVC reports /showIncludes as localized text and CMake records the prefix
#     in rules.ninja at configure time. If the console code page is not UTF-8
#     (e.g. CP936 on a Chinese Windows), the detected prefix ends up mojibake
#     and Ninja silently records ZERO header dependencies -- editing a header
#     then does not rebuild anything. Running configure under `chcp 65001`
#     keeps both sides in UTF-8. The prefix is baked in on first configure, so
#     this only matters for a fresh build directory; the script also detects a
#     stale/broken prefix and reconfigures.
#
#   * Ninja needs cl.exe on PATH (a plain shell has it only after vcvars).
#     The script reports that instead of failing with a cryptic ninja error.
#
# ASCII-only on purpose (PowerShell 5.1 reads .ps1 without BOM as ANSI).
# ---------------------------------------------------------------------------

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

try { chcp 65001 > $null } catch { }

function Get-NinjaPath {
    $cmd = Get-Command ninja -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { return $cmd.Source }
    $guess = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (Test-Path -LiteralPath $guess) { return $guess }
    foreach ($ed in @("Community", "Enterprise", "BuildTools")) {
        $guess = Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\$ed\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
        if (Test-Path -LiteralPath $guess) { return $guess }
    }
    throw "ninja.exe not found. Install Ninja or the Visual Studio CMake tools."
}

function Get-ClPath {
    $cmd = Get-Command cl.exe -ErrorAction SilentlyContinue
    if ($null -ne $cmd) { return $cmd.Source }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($vs)) { return $null }
    $hit = Get-ChildItem -LiteralPath (Join-Path $vs "VC\Tools\MSVC") -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName "bin\Hostx64\x64\cl.exe" } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    return $hit
}

# The byte sequence cl actually writes for the /showIncludes prefix. Read as
# Latin-1 so the bytes survive the trip through .NET strings unchanged.
# ISO-8859-1 is byte-transparent: one char per byte, no lossy code page lookup.
# (Encoding.Latin1 only exists on .NET 5+, this has to work on Windows PowerShell 5.1.)
$latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Get-ShowIncludesPrefixBytes {
    param([string]$ClPath, [string]$WorkDir)
    New-Item -ItemType Directory -Force $WorkDir | Out-Null
    $probe = Join-Path $WorkDir "probe.cpp"
    [System.IO.File]::WriteAllText($probe, "#include <cstddef>`n")
    $out = Join-Path $WorkDir "out.txt"
    if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
    $bat = Join-Path $WorkDir "probe.bat"
    $batText = "@echo off`r`n`"$ClPath`" /nologo /c /showIncludes probe.cpp > out.txt 2>&1`r`n"
    [System.IO.File]::WriteAllText($bat, $batText, [System.Text.Encoding]::ASCII)
    Push-Location $WorkDir
    try { & cmd /c "probe.bat" | Out-Null } finally { Pop-Location }
    if (-not (Test-Path -LiteralPath $out)) { return $null }
    $text = $latin1.GetString([System.IO.File]::ReadAllBytes($out))
    foreach ($line in ($text -split "`r?`n")) {
        if ($line -match '^(.*:[ \t]+)[A-Za-z]:[/\\]') {
            return $latin1.GetBytes($Matches[1])
        }
    }
    return $null
}

function Trim-TrailingSpace {
    param([byte[]]$Bytes)
    if ($null -eq $Bytes) { return $null }
    $n = $Bytes.Length
    while ($n -gt 0 -and ($Bytes[$n - 1] -eq 0x20 -or $Bytes[$n - 1] -eq 0x09)) { $n-- }
    if ($n -eq $Bytes.Length) { return $Bytes }
    $out = New-Object byte[] $n
    [Array]::Copy($Bytes, 0, $out, 0, $n)
    return $out
}

function Get-RulesNinjaPrefixBytes {
    param([string]$RulesFile)
    if (-not (Test-Path -LiteralPath $RulesFile)) { return $null }
    # rules.ninja holds the localized prefix as UTF-8: read it byte-transparently (Latin-1)
    # instead of decoding to chars and encoding back (non-Latin chars would become '?').
    $bytes = [System.IO.File]::ReadAllBytes($RulesFile)
    $text = $latin1.GetString($bytes)
    $marker = "msvc_deps_prefix = "
    $at = $text.IndexOf($marker)
    if ($at -lt 0) { return $null }
    $start = $at + $marker.Length
    $end = $start
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0x0D -and $bytes[$end] -ne 0x0A) { $end++ }
    $len = $end - $start
    if ($len -le 0) { return $null }
    $out = New-Object byte[] $len
    [Array]::Copy($bytes, $start, $out, 0, $len)
    return $out
}

function Test-BytesEqual {
    param([byte[]]$A, [byte[]]$B)
    $A = Trim-TrailingSpace $A
    $B = Trim-TrailingSpace $B
    if ($null -eq $A -or $null -eq $B) { return $false }
    if ($A.Length -ne $B.Length) { return $false }
    for ($i = 0; $i -lt $A.Length; $i++) { if ($A[$i] -ne $B[$i]) { return $false } }
    return $true
}

$ninja = Get-NinjaPath
$cl = Get-ClPath
if ([string]::IsNullOrWhiteSpace($cl)) {
    throw "cl.exe not found. Run from a 'Developer PowerShell for VS 2022' or install the MSVC toolset."
}

$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
$rulesFile = Join-Path $BuildDir "CMakeFiles\rules.ninja"

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Write-Host "[build] removing $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

$needConfigure = $Reconfigure -or -not (Test-Path -LiteralPath $cacheFile)
if (-not $needConfigure) {
    $expected = Get-ShowIncludesPrefixBytes -ClPath $cl -WorkDir (Join-Path $BuildDir "CMakeFiles\prefix_probe")
    $actual = Get-RulesNinjaPrefixBytes -RulesFile $rulesFile
    if (-not (Test-BytesEqual $expected $actual)) {
        Write-Host "[build] /showIncludes prefix mismatch in $rulesFile -- reconfiguring."
        Write-Host "[build] (expected UTF-8 bytes for the compiler's localized message, see header comment)"
        $needConfigure = $true
    }
}

if ($needConfigure) {
    Write-Host "[build] configure: Ninja Multi-Config -> $BuildDir"
    & cmake -S . -B $BuildDir -G "Ninja Multi-Config" `
        -DCMAKE_CONFIGURATION_TYPES="Debug;Release" `
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
}

$buildArgs = @("--build", $BuildDir, "--config", $Config)
if (-not [string]::IsNullOrWhiteSpace($Target)) { $buildArgs += @("--target", $Target) }
if ($Jobs -gt 0) { $buildArgs += @("--parallel", "$Jobs") }

$sw = [System.Diagnostics.Stopwatch]::StartNew()
& cmake @buildArgs
$code = $LASTEXITCODE
$sw.Stop()

if ($code -ne 0) { throw "build failed ($code) after $([int]$sw.Elapsed.TotalSeconds)s" }

Write-Host ("[build] {0} ok in {1:N1}s -> {2}" -f $Config, $sw.Elapsed.TotalSeconds, (Join-Path $BuildDir $Config))
exit 0
