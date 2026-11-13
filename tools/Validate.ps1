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

# M1-A2 ECS destruction gate: teardown has to go through Scene::destroyEntity, which
# retires terrain/voxel arena slices, hierarchy links, spatial index entries and model
# resources (src/Scene/Scene.cpp:880-977). A raw registry.destroy() skips all of it, so
# every exception below is spelled out with its reason; anything else fails the run.
$destroyExempt = @{
    "src\Scene\Scene.cpp"        = "Scene::destroyEntity itself + scene self tests"
    "src\Scene\EntityHandle.cpp" = "handle contract self test owns a throwaway registry"
    "src\Editor\Commands.cpp"    = "scene == nullptr fallback used by editor self tests"
    "src\Editor\EditorLayer.cpp" = "editor self tests own a standalone registry (B-06)"
    "src\Core\Application.cpp"   = "not ECS: gpuTimer / renderDoc / screenshot objects"
}
$destroyViolations = @()
foreach ($sourceFile in (Get-ChildItem -LiteralPath (Join-Path $repoRoot "src") -Recurse -Filter "*.cpp" -File)) {
    $relative = $sourceFile.FullName.Substring($repoRoot.Length).TrimStart("\", "/")
    if ($destroyExempt.ContainsKey($relative)) { continue }
    $hits = Select-String -LiteralPath $sourceFile.FullName -Pattern "registry(\.|->)destroy\("
    foreach ($hit in $hits) {
        $destroyViolations += ("{0}:{1}" -f $relative, $hit.LineNumber)
    }
}
if ($destroyViolations.Count -gt 0) {
    throw ("M1-A2: raw registry.destroy() outside Scene::destroyEntity:`n  " + ($destroyViolations -join "`n  ") +
           "`nRoute destruction through Scene::destroyEntity, or add the file to `$destroyExempt with a reason.")
}
Write-Host "[validate] M1-A2 destruction gate PASS"

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
    $configureArgs += @("-DTINY_ENABLE_TRACY=ON", "-DTINY_ENABLE_RENDERDOC=ON")
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
    (Join-Path $BuildDir "$Config\TinyEngine.exe"),
    (Join-Path $BuildDir "TinyEngine.exe")
)
$exe = $exeCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if ($null -eq $exe) {
    $exe = Get-ChildItem -LiteralPath $BuildDir -Recurse -Filter TinyEngine.exe |
        Where-Object { $_.FullName -match [Regex]::Escape($Config) } |
        Select-Object -First 1 -ExpandProperty FullName
}
if ([string]::IsNullOrWhiteSpace($exe)) {
    throw "TinyEngine.exe was not found under $BuildDir."
}
Write-Host "[validate] Executable: $exe"
$editorExe = Join-Path (Split-Path -Parent $exe) "TinyEditor.exe"
if (-not (Test-Path -LiteralPath $editorExe -PathType Leaf)) {
    $editorExe = ""
}
Write-Host "[validate] Editor executable: $editorExe"

$null = New-Item -ItemType Directory -Force (Join-Path $logDir "tests")
$tests = @(
    @{ Name = "jobtest"; Args = @("--jobtest");
       MustContain = @("[JobSystem] selfTest PASS") },
    @{ Name = "td09-config-migration"; Args = @("--config-test");
       MustContain = @("[Config] selfTest PASS") },
    @{ Name = "m1-handle-contract"; Args = @("--handle-test");
       MustContain = @("[Handle] selfTest PASS") },
    @{ Name = "m1-trace-contract"; Args = @("--trace-test");
       MustContain = @("[Trace] selfTest PASS") },
    @{ Name = "m1-b1-gltf-nodes"; Args = @("--gltf-node-test");
       MustContain = @("[Model] nodeSelfTest PASS") },
    @{ Name = "m1-b2-tpack"; Args = @("--pack-test");
       MustContain = @("[AssetPack] selfTest PASS") },
    @{ Name = "m1-b3-material-json"; Args = @("--material-json-test");
       MustContain = @("[Material] jsonSelfTest PASS") },
    @{ Name = "m1-b4-terrain-sculpt"; Args = @("--terrain-sculpt-test");
       MustContain = @("[Terrain] sculptDeltaSelfTest PASS", "[Terrain] sculptJsonSelfTest PASS") },
    @{ Name = "m1-b5-texture-budget"; Args = @("--texture-budget-test");
       MustContain = @("[Texture] budgetSelfTest PASS") },
    @{ Name = "selftest-default"; Args = @("--selftest", "$Frames");
       MustContain = @("[Scene] ECS/spatial selfTest PASS", "[Scene] ECS component selfTest PASS", "[TD07] ComputeProbe disabled: no probe dispatches or CPU readback", "[SelfTest]") },
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
       MustContain = @("[Terrain] selfTest PASS", "[Terrain] LOD stitching PASS", "[Terrain] GPU mesh assembly selfTest PASS", "[Terrain] GPU selection selfTest PASS", "[Terrain] GPU selection transition PASS", "[Terrain] demo entity created", "chunks=4", "lods=3", "submeshes=12", "[Terrain] async generation PASS", "[Terrain] build task retirement PASS", "[Window] presentation=hidden", "streaming chunk test PASS | chunks=9 independentModels=9 gpuMesh=9", "[Terrain] spatial region query PASS | chunks=9 regions=5x5", "[Terrain] sculpt PASS | chunks=9 rebuilt=1", "[Terrain] character controller PASS | grounded=1", "[Terrain] character serialization PASS | captured=1", "[Terrain] material sets PASS | resident=4 max=4") },
    # ---- P2 ----
    @{ Name = "f1-gpu-scene"; Args = @("--selftest", "$([Math]::Max($Frames, 40))", "--gpu-scene");
       MustContain = @("[SceneGpu] GPU batch ready", "[SceneGpu] indirect batch | groups=", "[Validation] errors=0 warnings=0") },
    @{ Name = "f1b-merged-buffers"; Args = @("--selftest", "$([Math]::Max($Frames, 40))", "--gpu-scene");
       MustContain = @("[SceneGpu] merged buffers built", "[SceneGpu] indirect batch | groups=", "[Validation] errors=0 warnings=0") },
    @{ Name = "f1b-merged-off"; Args = @("--selftest", "$([Math]::Max($Frames, 40))", "--gpu-scene", "--no-merged-buffers");
       MustContain = @("[SceneGpu] GPU batch ready", "[Validation] errors=0 warnings=0") },
    @{ Name = "f1c-hiz-off"; Args = @("--selftest", "$([Math]::Max($Frames, 40))", "--gpu-scene", "--no-hiz");
       MustContain = @("[SceneGpu] cull stats |", "hizEnabled=0", "[Validation] errors=0 warnings=0") },
    @{ Name = "f1d-stable-path"; Args = @("--selftest", "$([Math]::Max($Frames, 120))", "--gpu-scene");
       MustContain = @("[SceneGpu] cull stats |", "stable=", "rebuilds=", "flattenUs=", "[Validation] errors=0 warnings=0") },
    @{ Name = "f1d-sync-validation"; Args = @("--selftest", "60", "--gpu-scene", "--validation-features", "sync");
       MustContain = @("[Diag] validation features enabled: synchronization", "[Validation] errors=0 warnings=0") },
    @{ Name = "f1e-screen-space"; Args = @("--selftest", "$([Math]::Max($Frames, 40))", "--gpu-scene", "--gpu-scene-min-px", "8");
       MustContain = @("[SceneGpu] cull stats |", "screenTests=", "[Validation] errors=0 warnings=0") },
    @{ Name = "f1e-external-instances"; Args = @("--selftest", "$([Math]::Max($Frames, 40))", "--gpu-scene", "--gpu-scene-external", "4");
       MustContain = @("[SceneGpu] external instances | count=4", "external=4", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2-streaming-world"; Args = @("--selftest", "$([Math]::Max($Frames, 90))", "--streaming-world", "2");
       MustContain = @("[Streaming] world ready | radius=2", "[Streaming] chunks=25", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2-streaming-fly"; Args = @("--selftest", "$([Math]::Max($Frames, 150))", "--streaming-world", "2", "--streaming-fly", "60");
       MustContain = @("[Streaming] chunks=", "unloaded=", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2b-terrain-batch"; Args = @("--selftest", "$([Math]::Max($Frames, 130))", "--streaming-world", "2");
       MustContain = @("[Terrain] mesh arena ready", "[Terrain] batched draw |", "heightCaptured=1 heightMin=0 heightRange=12 heightMixed=0", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2b-terrain-batch-off"; Args = @("--selftest", "$([Math]::Max($Frames, 60))", "--streaming-world", "2", "--no-terrain-batch");
       MustContain = @("[Terrain] terrain batching disabled |", "[Terrain] chunk shading | heightCaptured=1 heightMin=0 heightRange=12 heightMixed=0", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2c-origin-rebase"; Args = @("--selftest", "$([Math]::Max($Frames, 240))", "--streaming-world", "2", "--streaming-fly", "60", "--origin-threshold", "64");
       MustContain = @("[Origin] rebased |", "[Origin] stats |", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2c-origin-off"; Args = @("--selftest", "$([Math]::Max($Frames, 240))", "--streaming-world", "2", "--streaming-fly", "60", "--origin-threshold", "64", "--no-origin-rebase");
       MustContain = @("[Origin] stats | rebases=0", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2d-streaming-radius"; Args = @("--selftest", "$([Math]::Max($Frames, 150))", "--streaming-world", "8");
       MustContain = @("[Streaming] chunks=289", "[Terrain] mesh arena ready |", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2d-streaming-budget"; Args = @("--selftest", "$([Math]::Max($Frames, 120))", "--streaming-world", "8", "--streaming-budget", "0.2");
       MustContain = @("budgetHits=", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2e-terrain-picking"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] streamed terrain PASS", "picking=OK", "[Editor] selfTest PASS") },
    @{ Name = "f2f-arena-slots"; Args = @("--selftest", "180", "--streaming-world", "8");
       MustContain = @("[Terrain] mesh arena ready | vertexMB=", "arenaAllocs=", "[Validation] errors=0 warnings=0") },
    @{ Name = "f2h-large-radius"; Args = @("--selftest", "400", "--streaming-world", "16");
       MustContain = @("mesh arena resized | radius=16", "[Streaming] chunks=1089", "[Validation] errors=0 warnings=0") },
    @{ Name = "e1-batch-commands"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("batch delete/duplicate", "[Editor] prefab selfTest PASS", "[Editor] selfTest PASS") },
    @{ Name = "e2-multi-transform"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("multi transform", "[Editor] selfTest PASS") },
    @{ Name = "e3-material-undo"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("material/render tag", "[Editor] selfTest PASS") },
    @{ Name = "e3-play-doc-lock"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("play document lock selfTest PASS") },
    @{ Name = "e4-unsaved-prompt"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("unsaved changes prompt selfTest PASS") },
    @{ Name = "e4-preferences"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] preferences selfTest PASS", "preferences file selfTest PASS") },
    @{ Name = "e4-shortcut-sheet"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("shortcut sheet selfTest PASS") },
    @{ Name = "e4-history-jump"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("history jump selfTest PASS") },
    @{ Name = "e5-toolbar-snap"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("snap selfTest PASS", "[Editor] selfTest PASS") },
    # UX-02（B1）：帮助与未知参数。这两条是错误路径用例，显式声明退出码并允许 stderr ——
    # 默认的「退出码 0 + stderr 0 字节」没有放宽，只是给这两条开了口子。
    @{ Name = "b1-help"; Exe = "engine"; Args = @("--help"); SkipValidationMarker = $true;
       MustContain = @("Tiny Engine -", "--selftest [N]", "--validation-features <list>", "--update-profile") },
    @{ Name = "b1-unknown-arg"; Exe = "engine"; Args = @("--not-a-flag"); ExpectedExitCode = 2;
       AllowStderr = $true; SkipValidationMarker = $true;
       MustContainStderr = @("unknown argument: --not-a-flag") },
    @{ Name = "b1-window-fit"; Exe = "engine"; Args = @("--selftest", "5", "--hidden");
       MustContain = @("[Window] fit selfTest PASS", "[Startup] error log selfTest PASS", "[Startup] error dialog text selfTest PASS", "[Window] sizing") },
    @{ Name = "e5-render-settings"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("render settings selfTest PASS") },
    @{ Name = "b3-notices"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] notices selfTest PASS") },
    @{ Name = "b6-console-search"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] console filter selfTest PASS", "[Editor] terrain create request selfTest PASS", "entries=20 conflicts=0") },
    @{ Name = "b5-language-console"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] status bar selfTest PASS") },
    @{ Name = "b4-shortcuts"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] shortcut sheet selfTest PASS", "conflicts=0") },
    @{ Name = "async-terrain-create"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Scene] async terrain create selfTest PASS") },
    @{ Name = "f6-shadows"; Exe = "engine"; Args = @("--selftest", "20"); MustContain = @("[Shadow] selfTest PASS", "[Shadow] first frame | cascades=3") },
    @{ Name = "f3-voxel"; Exe = "engine"; Args = @("--selftest", "20", "--voxel-world", "1");
       MustContain = @("[Voxel] atlas=512 tile=64 pitch=128 gutter=32", "[Voxel] selfTest PASS", "[Voxel] scene selfTest PASS", "arenaRebuilt=1", "async submitMs=", "[Voxel] world build complete | chunks=9 arena=9", "[Voxel] arena batch | chunks=9") },
    # V-04：最大体素半径 12 = 625 块，必须整块进 arena 并由一条 indirect 提交；
    # 这条同时守住 256 -> 1024 的材质槽位容量（625 块每块一个实体槽位）。
    @{ Name = "f3-voxel-arena-625"; Exe = "engine"; Args = @("--selftest", "200", "--voxel-world", "12");
       MustContain = @("[Voxel] world build complete | chunks=625 arena=625", "[Voxel] scene selfTest PASS", "arenaRebuilt=1", "[Voxel] arena batch |", "[Validation] errors=0 warnings=0") },
    # V-01：玻璃单独走 Lighting 之后的混合 pass。断言 glass 面确实被拆出来、
    # 排序后被一次 draw 画掉（drawcalls=1），且验证层仍为零消息。
    @{ Name = "f3-voxel-glass"; Exe = "engine"; Args = @("--selftest", "20", "--voxel-world", "1", "--voxel-glass-demo");
       MustContain = @("[Voxel] glass demo placed | blocks=", "[Glass] sorted=", "drawcalls=1", "[Voxel] selfTest PASS") },

    # TAA：抖动 + 历史混合。开关两条路径都要保持验证层零消息，
    # 关闭时 Pass 仍运行（只输出当前帧），否则下游 Bloom/Present 会读到没人写的图。
    @{ Name = "f3v-taa"; Exe = "engine"; Args = @("--selftest", "40", "--voxel-world", "1");
       MustContain = @("[TAA] initialized |", "[Validation] errors=0 warnings=0") },
    @{ Name = "f3v-taa-off"; Exe = "engine"; Args = @("--selftest", "20", "--no-taa");
       MustContain = @("[TAA] disabled", "[Validation] errors=0 warnings=0") },
    # 水面：有地形/体素时注册透明 Pass，并能被 --no-water 关掉。两条路径
    # 都要保持验证层零消息（关掉时 RenderGraph 不能留下没人写的目标）。
    @{ Name = "f3v-water"; Exe = "engine"; Args = @("--selftest", "20", "--voxel-world", "1");
       MustContain = @("[Tiny] Water normal map loaded |", "[Water] initialized |", "[Water] first draw |", "[Validation] errors=0 warnings=0") },
    # C1：半分辨率平面反射。镜像相机重画不透明几何，水面从 binding12 采样；
    # 开关默认开启，渲染图注册两个反射 pass 且验证层仍为零消息。
    @{ Name = "f3v-water-reflection"; Exe = "engine"; Args = @("--selftest", "20", "--voxel-world", "1");
       MustContain = @("[Reflection] planar reflection initialized |", "[Reflection] first draw |", "planarReflection=1", "[Validation] errors=0 warnings=0") },
    @{ Name = "f3v-water-off"; Exe = "engine"; Args = @("--selftest", "20", "--voxel-world", "1", "--no-water");
       MustContain = @("[Water] disabled", "[Validation] errors=0 warnings=0") },
    # F6-01：水面近场屏幕空间反射。半分辨率追踪 pass 注册在水之前，
    # 命中结果通过 binding13 混进水面反射；关掉后水面回落到平面反射/IBL。
    @{ Name = "f3v-ssr"; Exe = "engine"; Args = @("--selftest", "20", "--voxel-world", "1");
       MustContain = @("[SSR] initialized |", "[SSR] first draw |", "[Water] first draw |", "ssr=1", "[Validation] errors=0 warnings=0") },
    @{ Name = "f3v-ssr-off"; Exe = "engine"; Args = @("--selftest", "20", "--voxel-world", "1", "--no-ssr");
       MustContain = @("[SSR] disabled", "[Water] first draw |", "ssr=0", "[Validation] errors=0 warnings=0") },
  @{ Name = "f3-voxel-persist"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("capture + json round trip", "[Editor] voxel edit selfTest PASS") },
  @{ Name = "e8-editor-mode"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] editor mode selfTest PASS", "entries=20 conflicts=0", "[Editor] example presets selfTest PASS", "voxel-world meshes=9 bounds=9 parented=9") },
  @{ Name = "e9-runtime-character"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] runtime character selfTest PASS | camera follows feet, exit restores pose | skinned=1 animations=18", "clips=Idle/Walk/Run/Jump/Jump_Idle/Jump_Land active=Idle") },
    @{ Name = "e6-asset-and-textures"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] asset drop selfTest PASS", "[Editor] texture library selfTest PASS") },
    @{ Name = "b2-empty-state"; Exe = "editor"; Args = @("--selftest", "5", "--empty-scene");
       MustContain = @("[Editor] empty state selfTest PASS") },
    @{ Name = "f2i-update-profile"; Args = @("--selftest", "240", "--streaming-world", "8", "--update-profile");
       MustContain = @("[Update] avg us |", "[Validation] errors=0 warnings=0") },
    @{ Name = "p2-device-fault"; Args = @("--device-fault-test");
       MustContain = @("[DeviceFault] selfTest PASS") },
    # F5-08：粒子发射器 + Kenney CC0 sprite。空场景建一个喷泉，启动时预加载
    # Texture/Particles，CPU 模拟后由 ParticlePass 前向透明绘制；断言贴图预加载、
    # 发射器、存活粒子与一次 draw，验证层仍为零消息。
    @{ Name = "f6-particles"; Exe = "engine"; Args = @("--selftest", "60", "--empty-scene", "--particle-demo");
       MustContain = @("[Particle] textures=", "status=ready", "[Particle] demo emitter created |", "texture=Particles/Kenney/fire_01.png", "[Particle] emitters=1 alive=", "drawcalls=1", "[Validation] errors=0 warnings=0") },
    # F6-05：体积云。空场景创建 scene.clouds，启动生成 64³ 3D 噪声，
    # CloudPass 在 Lighting 后 raymarch 并预乘混合进 HDR；断言初始化、演示参数、
    # 首帧绘制与验证层零消息。
    @{ Name = "f6-clouds"; Exe = "engine"; Args = @("--selftest", "30", "--empty-scene", "--cloud-demo");
       MustContain = @("[Cloud] initialized | noise=64^3", "[Cloud] demo scene ready |", "[Cloud] first draw |", "[Validation] errors=0 warnings=0") },
    # F6-06：太阳盘 + 镜头光晕。空场景创建 scene.sun，日盘按世界空间射线绘制，
    # 鬼影沿“太阳->屏幕中心”镜像；断言初始化、演示参数、首帧绘制与验证层零消息。
    @{ Name = "f6-sun"; Exe = "engine"; Args = @("--selftest", "30", "--empty-scene", "--sun-demo");
       MustContain = @("[Sun] initialized |", "[Sun] demo scene ready |", "[Sun] first draw |", "[Validation] errors=0 warnings=0") },
    # F6-07：地形植被。空场景 + 流送地形生成草/树，VegetationPass 在 Geometry 后
    # SSAO 前写 G-Buffer；断言网格、演示参数、放置、首帧与验证层零消息。
    @{ Name = "f6-vegetation"; Exe = "engine"; Args = @("--selftest", "90", "--empty-scene", "--streaming-world", "3", "--vegetation-demo");
       MustContain = @("[Vegetation] meshes |", "[Vegetation] demo scene ready |", "[Vegetation] placed | grass=", "[Vegetation] first draw |", "[Validation] errors=0 warnings=0") },
    # F6-08：可放置的局部体积雾。组件的 center/size/rotation 定义局部盒体，
    # 再乘实体 WorldTransform；FogVolumePass 在 Cloud 后、Reflection 前全屏
    # raymarch，读 G-Buffer 位置截断并预乘混进 HDR。断言初始化、演示盒
    # placement、首帧与验证层零消息。
    @{ Name = "f6-fog-volume"; Exe = "engine"; Args = @("--selftest", "70", "--empty-scene", "--streaming-world", "3", "--fog-demo");
       MustContain = @("[FogVolume] initialized | noise=32^3", "[FogVolume] demo volume created | density=0.045 center=-6,10,-10 size=30x10x36 steps=24", "[FogVolume] first draw |", "[Validation] errors=0 warnings=0") }
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
    $caseExe = $exe
    if ($test.ContainsKey("Exe") -and $test.Exe -eq "editor") {
        if ([string]::IsNullOrWhiteSpace($editorExe)) {
            $failed += "$($test.Name): TinyEditor.exe was not found next to TinyEngine.exe"
            Write-Host "[validate] FAIL $($test.Name): TinyEditor.exe missing" -ForegroundColor Red
            continue
        }
        $caseExe = $editorExe
    }
    $result = Invoke-Native -FilePath $caseExe -Arguments $test.Args -LogStem (Join-Path "tests" $test.Name)
    # Windows PowerShell 5.1: casting $null to [string] still yields $null, and the
    # later $stdout.IndexOf(...) throws InvokeMethodOnNull - which aborts the whole
    # suite instead of reporting a FAIL (e.g. when the exe writes an empty log).
    $stdout = if ($null -eq $result.Stdout) { "" } else { [string]$result.Stdout }
    $errorText = if ($null -eq $result.Stderr) { "" } else { [string]$result.Stderr }

    # UX-B1：错误路径的用例要能声明期望退出码（例如 --not-a-flag 约定返回 2），
    # 并且允许在 stderr 上写一行可行动的错误提示。默认仍是「退出码 0 + stderr 0 字节」。
    $expectedExit = if ($null -ne $test.ExpectedExitCode) { [int]$test.ExpectedExitCode } else { 0 }
    $allowStderr = [bool]$test.AllowStderr
    $reasons = @()
    if ($result.ExitCode -ne $expectedExit) {
        $reasons += "exit code $($result.ExitCode) (expected $expectedExit)"
    }
    if (-not $allowStderr -and $result.StderrBytes -ne 0) {
        $reasons += "stderr is not empty ($($result.StderrBytes) bytes)"
    }
    foreach ($pattern in $test.MustContain) {
        if ($stdout.IndexOf($pattern, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
            $reasons += "missing stdout marker '$pattern'"
        }
    }

    # 错误路径用例的提示走 stderr（stdout 保持干净），所以标记也要能对 stderr 断言。
    if ($null -ne $test.MustContainStderr) {
        foreach ($pattern in $test.MustContainStderr) {
            if ($errorText.IndexOf($pattern, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
                $reasons += "missing stderr marker '$pattern'"
            }
        }
    }

    # P0: every test that creates a Vulkan device must report zero validation messages.
    # (jobtest / config-test / --build-info / --diagnostics-test return before the Vulkan device exists.)
    $needsValidationMarker = -not $test.SkipValidationMarker -and
        $test.Name -notin @('jobtest', 'td09-config-migration', 'p0-build-info', 'p0-diagnostics-selftest', 'f2e-terrain-picking', 'm1-handle-contract', 'm1-trace-contract', 'm1-b1-gltf-nodes', 'm1-b2-tpack', 'm1-b3-material-json', 'm1-b4-terrain-sculpt', 'm1-b5-texture-budget')
    if ($needsValidationMarker) {
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
        $stderrNote = if ($allowStderr) { "stderr allowed" } else { "stderr=0 bytes" }
        Write-Host "[validate] PASS $($test.Name) ($stderrNote, exit=$($result.ExitCode))" -ForegroundColor Green
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