[CmdletBinding()]
param(
    [string]$DependenciesRoot = "",
    [switch]$Force,
    [switch]$CheckOnly
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($DependenciesRoot)) {
    $DependenciesRoot = Join-Path $repoRoot "3rdParty"
}
$DependenciesRoot = [IO.Path]::GetFullPath($DependenciesRoot)
$cacheDir = Join-Path $DependenciesRoot ".cache"

# Fixed source pins. Update this table only together with a clean validation run.
$dependencies = @(
    @{ Name = "glfw"; Version = "3.4"; Target = "glfw";
       Url = "https://github.com/glfw/glfw/archive/refs/tags/3.4.zip";
       SourceEntry = "include/GLFW/glfw3.h";
       Files = @("CMakeLists.txt", "include/GLFW/glfw3.h") },
    @{ Name = "glm"; Version = "1.0.3"; Target = "glm";
       Url = "https://github.com/g-truc/glm/archive/refs/tags/1.0.3.zip";
       SourceEntry = "glm/glm.hpp";
       Files = @("CMakeLists.txt", "glm/glm.hpp") },
    @{ Name = "imgui"; Version = "1.92.6"; Target = "imgui";
       Url = "https://github.com/ocornut/imgui/archive/refs/tags/v1.92.6.zip";
       SourceEntry = "imgui.cpp";
       Files = @("imgui.cpp", "imgui.h", "backends/imgui_impl_vulkan.cpp") },
    @{ Name = "stb"; Version = "f4a71b1"; Target = "stb";
       Url = "https://github.com/nothings/stb/archive/f4a71b13373436a2866c5d68f8f80ac6f0bc1ffe.zip";
       SourceEntry = "stb_image.h";
       Files = @("stb_image.h") },
    @{ Name = "tinyobjloader"; Version = "v2.0.0"; Target = "tinyobjloader";
       Url = "https://github.com/tinyobjloader/tinyobjloader/archive/refs/tags/v2.0.0.zip";
       SourceEntry = "tiny_obj_loader.h";
       Files = @("tiny_obj_loader.h") },
    @{ Name = "VulkanMemoryAllocator"; Version = "v3.4.0"; Target = "VulkanMemoryAllocator";
       Url = "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.zip";
       SourceEntry = "include/vk_mem_alloc.h"; CopySubdir = "include";
       Files = @("vk_mem_alloc.h") },
    @{ Name = "tinygltf"; Version = "b3e0d78"; Target = "tinygltf";
       Url = "https://github.com/syoyo/tinygltf/archive/b3e0d78.zip";
       SourceEntry = "tiny_gltf.h";
       Files = @("tiny_gltf.h", "json.hpp") },
    @{ Name = "entt"; Version = "v3.16.0"; Target = "entt";
       Url = "https://github.com/skypjack/entt/archive/refs/tags/v3.16.0.zip";
       SourceEntry = "src/entt/entt.hpp";
       Files = @("src/entt/entt.hpp") }
)
function Get-DependencyTarget([hashtable]$Dependency) {
    return Join-Path $DependenciesRoot $Dependency.Target
}

function Test-Dependency([hashtable]$Dependency) {
    $target = Get-DependencyTarget $Dependency
    foreach ($relative in $Dependency.Files) {
        if (-not (Test-Path -LiteralPath (Join-Path $target $relative) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

function Find-ArchiveSourceRoot([string]$ExtractRoot, [string]$SourceEntry) {
    $needle = $SourceEntry.Replace("/", "\")
    $leaf = Split-Path -Leaf $needle
    $hits = Get-ChildItem -LiteralPath $ExtractRoot -Recurse -File -Filter $leaf
    foreach ($hit in $hits) {
        if ($hit.FullName.EndsWith($needle, [StringComparison]::OrdinalIgnoreCase)) {
            return $hit.FullName.Substring(0, $hit.FullName.Length - $needle.Length).TrimEnd("\")
        }
    }
    throw "Archive does not contain expected entry '$SourceEntry'."
}

function Install-Dependency([hashtable]$Dependency) {
    $target = Get-DependencyTarget $Dependency
    $fileName = "$($Dependency.Name)-$($Dependency.Version).zip"
    $archive = Join-Path $cacheDir $fileName

    New-Item -ItemType Directory -Force $cacheDir | Out-Null
    if ($Force -and (Test-Path -LiteralPath $archive)) {
        Remove-Item -LiteralPath $archive -Force
    }
    if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
        Write-Host "[deps] Downloading $($Dependency.Name) $($Dependency.Version)"
        Invoke-WebRequest -Uri $Dependency.Url -OutFile $archive -UseBasicParsing
    } else {
        Write-Host "[deps] Reusing cached $fileName"
    }

    $extractRoot = Join-Path ([IO.Path]::GetTempPath()) ("sentinel-deps-" + [Guid]::NewGuid().ToString("N"))
    $staging = Join-Path $DependenciesRoot (".install-" + $Dependency.Name + "-" + [Guid]::NewGuid().ToString("N"))
    try {
        Expand-Archive -LiteralPath $archive -DestinationPath $extractRoot -Force
        $sourceRoot = Find-ArchiveSourceRoot $extractRoot $Dependency.SourceEntry
        $copyFrom = $sourceRoot
        if ($Dependency.ContainsKey("CopySubdir")) {
            $copyFrom = Join-Path $sourceRoot $Dependency.CopySubdir
        }
        if (-not (Test-Path -LiteralPath $copyFrom -PathType Container)) {
            throw "Dependency source directory not found: $copyFrom"
        }

        New-Item -ItemType Directory -Force $staging | Out-Null
        Get-ChildItem -LiteralPath $copyFrom -Force | Copy-Item -Destination $staging -Recurse -Force

        foreach ($relative in $Dependency.Files) {
            if (-not (Test-Path -LiteralPath (Join-Path $staging $relative) -PathType Leaf)) {
                throw "Installed dependency is missing '$relative'."
            }
        }

        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Recurse -Force
        }
        Move-Item -LiteralPath $staging -Destination $target
        Write-Host "[deps] Installed $($Dependency.Name) $($Dependency.Version)"
    }
    finally {
        if (Test-Path -LiteralPath $staging) {
            Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path -LiteralPath $extractRoot) {
            Remove-Item -LiteralPath $extractRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
Write-Host "[deps] Dependency root: $DependenciesRoot"
$missing = @()
foreach ($dependency in $dependencies) {
    if ((Test-Dependency $dependency) -and -not $Force) {
        Write-Host "[deps] OK $($dependency.Name) $($dependency.Version)"
        continue
    }
    if ($CheckOnly) {
        $missing += "$($dependency.Name) $($dependency.Version)"
        continue
    }
    Install-Dependency $dependency
}

if ($CheckOnly -and $missing.Count -gt 0) {
    throw ("Missing dependencies:`n  " + ($missing -join "`n  ") + "`nRun tools/SetupDependencies.ps1 to install them.")
}

# spdlog is intentionally optional and currently unused by the engine. It is
# listed here so a fresh clone does not fail with an unexplained missing include.
Write-Host "[deps] spdlog: optional / not enabled (no action)"

Write-Host "[deps] All required dependencies are present."