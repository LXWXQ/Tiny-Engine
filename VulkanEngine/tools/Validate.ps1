[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$Config = "Debug",
    [int]$Frames = 120,
    [switch]$Incremental,
    [switch]$SkipSetup,
    [switch]$EnableProfiling,
    [string]$VulkanSdk = $env:VULKAN_SDK,
    [string]$CMakePath = ""
)

$ErrorActionPreference = "Stop"

# MSBuild keeps background worker nodes alive by default. With -Wait on a
# redirected process that can block the caller until the reused node exits,
# which shows up as "the validation script hangs after the build step".
# Disabling node reuse makes every build a self-contained process.
$env:MSBUILDDISABLENODEREUSE = "1"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repoRoot

if ([string]::IsNullOrWhiteSpace($CMakePath)) {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($null -eq $cmakeCommand) {
        throw "cmake was not found. Install CMake or pass -CMakePath <path>."
    }
    $CMakePath = $cmakeCommand.Source
}

if (-not $SkipSetup) {
    & (Join-Path $PSScriptRoot "SetupDependencies.ps1")
}

if (-not [string]::IsNullOrWhiteSpace($VulkanSdk)) {
    $env:VULKAN_SDK = $VulkanSdk
}

$BuildDir = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDir))
if (-not $Incremental -and (Test-Path -LiteralPath $BuildDir)) {
    Write-Host "[validate] Removing previous build directory: $BuildDir"
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Force $BuildDir | Out-Null

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$logDir = Join-Path $BuildDir "validation\$stamp"
New-Item -ItemType Directory -Force $logDir | Out-Null

function Quote-NativeArg([string]$Value) {
    if ($Value -notmatch '[\s"]') { return $Value }
    $escaped = $Value -replace '(\\*)"', '$1$1\"'
    $escaped = $escaped -replace '(\\+)$', '$1$1'
    return '"' + $escaped + '"'
}

function Invoke-Native {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$LogStem
    )
    $stdout = Join-Path $logDir "$LogStem.stdout.log"
    $stderr = Join-Path $logDir "$LogStem.stderr.log"
    $argumentLine = ($Arguments | ForEach-Object { Quote-NativeArg $_ }) -join " "
    $process = Start-Process -FilePath $FilePath -ArgumentList $argumentLine `
        -WorkingDirectory $repoRoot -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr -Wait -PassThru -NoNewWindow
    return [pscustomobject]@{
        ExitCode = $process.ExitCode
        StdoutPath = $stdout
        StderrPath = $stderr
        Stdout = (Get-Content -LiteralPath $stdout -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
        Stderr = (Get-Content -LiteralPath $stderr -Raw -Encoding UTF8 -ErrorAction SilentlyContinue)
        StderrBytes = if (Test-Path -LiteralPath $stderr) { (Get-Item -LiteralPath $stderr).Length } else { 0 }
    }
}
# TD-08 dead-file guard: these were removed deliberately and must not return.
$forbiddenPaths = @(
    "src/Core/Descriptor.h",
    "src/Core/Descriptor.cpp",
    "shaders/light_cube.vert",
    "shaders/light_cube.frag",
    "shaders/shader.vert",
    "shaders/shader.frag"
)
foreach ($relative in $forbiddenPaths) {
    if (Test-Path -LiteralPath (Join-Path $repoRoot $relative)) {
        throw "TD-08 dead file returned: $relative"
    }
}
$straySpv = Get-ChildItem -LiteralPath (Join-Path $repoRoot "shaders") -Filter "*.spv" -File
if ($straySpv.Count -gt 0) {
    throw "TD-08: source shader directory contains .spv build artifacts: $($straySpv.Name -join ', ')"
}
Write-Host "[validate] TD-08 dead-file guard PASS"

# DocCheck: the documentation system is a verified artifact (docs/00-meta/doc-rules.md).
# A new conversation has to reconstruct project state from a limited context window,
# so stale/missing docs cost more than a failed build.
Write-Host "[validate] DocCheck"
$docCheck = Invoke-Native -FilePath "powershell" -LogStem "doccheck" -Arguments @(
    "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "DocCheck.ps1")
)
if ($docCheck.ExitCode -ne 0) {
    Get-Content -LiteralPath $docCheck.StdoutPath -Tail 40
    throw "DocCheck failed with exit code $($docCheck.ExitCode). See $($docCheck.StdoutPath)"
}
Write-Host "[validate] DocCheck PASS"

Write-Host "[validate] Configure ($Config)"
$configureArgs = @(
    "-S", ".", "-B", $BuildDir, "-G", "Visual Studio 17 2022", "-A", "x64"
)
if ($EnableProfiling) {
    $configureArgs += @("-DSENTINEL_ENABLE_TRACY=ON", "-DSENTINEL_ENABLE_RENDERDOC=ON")
}
$configure = Invoke-Native -FilePath $CMakePath -LogStem "configure" -Arguments $configureArgs
if ($configure.ExitCode -ne 0) {
    Get-Content -LiteralPath $configure.StdoutPath -Tail 80
    Get-Content -LiteralPath $configure.StderrPath -Tail 80
    throw "CMake configure failed with exit code $($configure.ExitCode)."
}

Write-Host "[validate] Build"
$build = Invoke-Native -FilePath $CMakePath -LogStem "build" -Arguments @(
    "--build", $BuildDir, "--config", $Config
)
if ($build.ExitCode -ne 0) {
    Get-Content -LiteralPath $build.StdoutPath -Tail 120
    Get-Content -LiteralPath $build.StderrPath -Tail 120
    throw "Build failed with exit code $($build.ExitCode)."
}
$buildText = [string]$build.Stdout + "`n" + [string]$build.Stderr
if ($buildText -match '(?im)\bwarning\s+(C|LNK|MSB)\d+') {
    throw "Build emitted compiler/linker warnings. See $($build.StdoutPath)."
}

$exeCandidates = @(
    (Join-Path $BuildDir "$Config\VulkanEngine.exe"),
    (Join-Path $BuildDir "VulkanEngine.exe")
)
$exe = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($null -eq $exe) {
    $exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter VulkanEngine.exe |
        Where-Object { $_.FullName -match [Regex]::Escape($Config) } |
        Select-Object -First 1 -ExpandProperty FullName
}
if ([string]::IsNullOrWhiteSpace($exe)) {
    throw "VulkanEngine.exe was not found under $BuildDir."
}
Write-Host "[validate] Executable: $exe"

$null = New-Item -ItemType Directory -Force (Join-Path $logDir "tests")
$tests = @(
    @{ Name = "jobtest"; Args = @("--jobtest");
       MustContain = @("[JobSystem] selfTest PASS") },
    @{ Name = "td09-config-migration"; Args = @("--config-test");
       MustContain = @("[Config] selfTest PASS") },
    @{ Name = "selftest-default"; Args = @("--selftest", "$Frames");
       MustContain = @("[Scene] ECS/spatial selfTest PASS", "[TD07] ComputeProbe disabled: no probe dispatches or CPU readback", "[SelfTest]") },
    @{ Name = "selftest-no-bloom"; Args = @("--selftest", "$Frames", "--no-bloom");
       MustContain = @("[TD06] Bloom disabled: skipping Bright + Down + Up passes", "[TD07] ComputeProbe disabled: no probe dispatches or CPU readback", "[SelfTest]") },
    @{ Name = "selftest-no-ssao"; Args = @("--selftest", "$Frames", "--no-ssao");
       MustContain = @("[TD07] ComputeProbe disabled: no probe dispatches or CPU readback", "[SelfTest]") },
    @{ Name = "selftest-no-bloom-no-ssao"; Args = @("--selftest", "$Frames", "--no-bloom", "--no-ssao");
       MustContain = @("[TD06] Bloom disabled: skipping Bright + Down + Up passes", "[TD07] ComputeProbe disabled: no probe dispatches or CPU readback", "[SelfTest]") },
    @{ Name = "td02-cross-queue"; Args = @("--selftest", "$([Math]::Max($Frames, 20))", "--cross-queue-test");
       MustContain = @("[TD02] compute->graphics verified", "[TD02] graphics->compute verified", "[TD07] ComputeProbe disabled: no probe dispatches or CPU readback") },
    @{ Name = "td07-compute-probe"; Args = @("--selftest", "$([Math]::Max($Frames, 20))", "--compute-test");
       MustContain = @("[D5] async compute probe verified", "[SelfTest]") },
    @{ Name = "td03-window-resize"; Args = @("--selftest", "$([Math]::Max($Frames, 60))", "--resize-test");
       MustContain = @("[TD03] Swapchain recreated", "[TD03] Render targets resized", "[SelfTest]") },
    @{ Name = "td03-render-scale"; Args = @("--selftest", "$([Math]::Max($Frames, 20))", "--render-scale", "0.5");
       MustContain = @("[TD03] Render targets resized", "[SelfTest]") },
    @{ Name = "td04-material-slot-stress"; Args = @("--selftest", "$([Math]::Max($Frames, 150))", "--material-stress");
       MustContain = @("[TD04] material slot stress PASS", "[SelfTest]") },
    @{ Name = "td05-subresource-probe"; Args = @("--selftest", "$([Math]::Max($Frames, 20))", "--subresource-test");
       MustContain = @("[TD05] subresource probe active", "[SelfTest]") },
    @{ Name = "td05-subresource-error"; Args = @("--subresource-error-test");
       MustContain = @("[TD05] subresource semantic error PASS") },
    # ---- P0: diagnostics / profiling infrastructure ----
    @{ Name = "p0-build-info"; Args = @("--build-info");
       MustContain = @("[BuildInfo] compiled:", "[BuildInfo] policy:") },
    @{ Name = "p0-diagnostics-selftest"; Args = @("--diagnostics-test", (Join-Path $logDir "diagnostics.jsonl"));
       MustContain = @("[Diag] diagnostics selfTest PASS") },
    @{ Name = "p0-gpu-timing"; Args = @("--selftest", "30", "--gpu-timing", "--gpu-timing-csv", (Join-Path $logDir "gpu_timing.csv"));
       MustContain = @("[Diag] gpu_timing.csv written:", "[Diag] zero-overhead guard SKIP") },
    # ---- P1: screenshot ----
    @{ Name = "p1-screenshot"; Args = @("--selftest", "40", "--screenshot", (Join-Path $logDir "screenshot.png"), "--screenshot-frame", "30");
       MustContain = @("[Diag] screenshot written:") },
    # ---- F2 / D-18: terrain CPU + G-Buffer path ----
    @{ Name = "terrain-render"; Args = @("--selftest", "20", "--empty-scene", "--terrain-test");
       MustContain = @("[Terrain] selfTest PASS", "[Terrain] LOD stitching PASS", "[Terrain] GPU mesh assembly selfTest PASS", "[Terrain] GPU selection selfTest PASS", "[Terrain] GPU selection transition PASS", "[Terrain] demo entity created", "chunks=4", "lods=3", "submeshes=12", "[Terrain] async generation PASS", "streaming chunk test PASS | chunks=9 independentModels=9 gpuMesh=9", "[Terrain] spatial region query PASS | chunks=9 regions=5x5", "[Terrain] sculpt PASS | chunks=9 rebuilt=1", "[Terrain] character controller PASS | grounded=1", "[Terrain] character serialization PASS | captured=1", "[Terrain] material sets PASS | resident=4 max=4") },
    # ---- P2 ----
    @{ Name = "p2-device-fault"; Args = @("--device-fault-test");
       MustContain = @("[DeviceFault] selfTest PASS") }
)

# Only meaningful for a profiling build (-EnableProfiling).
if ($EnableProfiling) {
    $tests += @{ Name = "p1-profiling-enabled";
        Args = @("--selftest", "20", "--capture-frame", "10");
        MustContain = @("[Diag] Tracy Vulkan context created", "[Diag] RenderDoc not injected") }
}
$failed = @()
foreach ($test in $tests) {
    Write-Host "[validate] Test: $($test.Name)"
    $result = Invoke-Native -FilePath $exe -Arguments $test.Args -LogStem (Join-Path "tests" $test.Name)
    $stdout = [string]$result.Stdout
    $errorText = [string]$result.Stderr

    $reasons = @()
    if ($result.ExitCode -ne 0) {
        $reasons += "exit code $($result.ExitCode)"
    }
    if ($result.StderrBytes -ne 0) {
        $reasons += "stderr is not empty ($($result.StderrBytes) bytes)"
    }
    foreach ($pattern in $test.MustContain) {
        if ($stdout.IndexOf($pattern, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
            $reasons += "missing stdout marker '$pattern'"
        }
    }

    # P0: every test that creates a Vulkan device must report zero validation messages.
    # (jobtest / config-test / --build-info / --diagnostics-test return before the Vulkan device exists.)
    if ($test.Name -notin @('jobtest', 'td09-config-migration', 'p0-build-info', 'p0-diagnostics-selftest')) {
        if ($stdout.IndexOf("[Validation] errors=0 warnings=0", [StringComparison]::OrdinalIgnoreCase) -lt 0) {
            $reasons += "missing zero-validation marker '[Validation] errors=0 warnings=0'"
        }
    }

    if ($reasons.Count -gt 0) {
        $failed += "$($test.Name): $($reasons -join '; ')"
        Write-Host "[validate] FAIL $($test.Name): $($reasons -join '; ')" -ForegroundColor Red
        if ($errorText.Length -gt 0) {
            Write-Host "--- stderr ($($result.StderrPath)) ---"
            Get-Content -LiteralPath $result.StderrPath -TotalCount 80
        }
        if ($stdout.Length -gt 0) {
            Write-Host "--- stdout tail ($($result.StdoutPath)) ---"
            Get-Content -LiteralPath $result.StdoutPath -Tail 40
        }
    } else {
        Write-Host "[validate] PASS $($test.Name) (stderr=0 bytes)" -ForegroundColor Green
    }
}

# P3-2: baseline + diff self-test (Save then Diff against itself).
Write-Host "[validate] Test: p3-baseline-diff"
$baselineScript = Join-Path $PSScriptRoot "Baseline.ps1"
$psArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $baselineScript)
$scriptArgs = @("-Name", "_selftest", "-BuildDir", $BuildDir, "-Config", $Config, "-Frames", "60")
$saveArgs = (($psArgs + @("-Action", "Save") + $scriptArgs) | ForEach-Object { Quote-NativeArg $_ }) -join " "
$save = Start-Process -FilePath "powershell" -ArgumentList $saveArgs `
    -WorkingDirectory $repoRoot -RedirectStandardOutput (Join-Path $logDir "p3-baseline-save.log") `
    -RedirectStandardError (Join-Path $logDir "p3-baseline-save.err") -Wait -PassThru -NoNewWindow
$diffArgs = (($psArgs + @("-Action", "Diff") + $scriptArgs) | ForEach-Object { Quote-NativeArg $_ }) -join " "
$diff = Start-Process -FilePath "powershell" -ArgumentList $diffArgs `
    -WorkingDirectory $repoRoot -RedirectStandardOutput (Join-Path $logDir "p3-baseline-diff.log") `
    -RedirectStandardError (Join-Path $logDir "p3-baseline-diff.err") -Wait -PassThru -NoNewWindow
if ($save.ExitCode -ne 0) { $failed += "p3-baseline-diff: Save exited $($save.ExitCode)" }
if ($diff.ExitCode -ne 0) { $failed += "p3-baseline-diff: Diff reported regression" }
if ($save.ExitCode -eq 0 -and $diff.ExitCode -eq 0) { Write-Host "[validate] PASS p3-baseline-diff (stderr=0 bytes)" -ForegroundColor Green }

# P3-1: debug bundle self-test (light: skip deep validation and baseline diff).
Write-Host "[validate] Test: p3-debug-bundle"
$bundleScript = Join-Path $PSScriptRoot "DebugBundle.ps1"
$bundleArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $bundleScript,
                "-Name", "_selftest", "-BuildDir", $BuildDir, "-Config", $Config,
                "-Frames", "40", "-SkipDeepValidation", "-SkipBaselineDiff")
$bundleArgsQuoted = ($bundleArgs | ForEach-Object { Quote-NativeArg $_ }) -join " "
$bundle = Start-Process -FilePath "powershell" -ArgumentList $bundleArgsQuoted `
    -WorkingDirectory $repoRoot -RedirectStandardOutput (Join-Path $logDir "p3-debug-bundle.log") `
    -RedirectStandardError (Join-Path $logDir "p3-debug-bundle.err") -Wait -PassThru -NoNewWindow
if ($bundle.ExitCode -ne 0) { $failed += "p3-debug-bundle: exited $($bundle.ExitCode)" }
else { Write-Host "[validate] PASS p3-debug-bundle (stderr=0 bytes)" -ForegroundColor Green }
if ($failed.Count -gt 0) {
    throw ("Validation failed:`n  " + ($failed -join "`n  ") + "`nLogs: $logDir")
}

Write-Host "[validate] ALL PASS | logs: $logDir" -ForegroundColor Green