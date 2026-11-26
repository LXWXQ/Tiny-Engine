cmake_minimum_required(VERSION 3.15)
project(TinyEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# 并行编译（仅 VS 生成器需要）。
#
# VS 生成器把 130+ 个 .cpp 塞进同一个 tiny_core 项目，MSBuild 默认逐个串行编译。
# /MP 让 cl 一次接收多个源文件并按物理核并行；CMake 会把它翻译成
# <MultiProcessorCompilation>true</MultiProcessorCompilation>。实测全量 Debug：93s -> 14s。
#
# Ninja 生成器本来就按「一个源文件一条编译边」并行（靠 -j 控制并发），
# 再叠 /MP 会让每个 cl 各自再开一池线程，属于超订，因此只在非 Ninja 下加。
if(MSVC AND NOT CMAKE_GENERATOR MATCHES "Ninja")
    add_compile_options(/MP)
endif()

# ---------------------------------------------------------------------------
# Ninja + MSVC + 中文区域设置（CP936）的已知坑。
#
# cl 的 /showIncludes 输出是 UTF-8 的「注意: 包含文件: 」，而 CMake 探测该前缀时按当前
# 控制台代码页解码。在 CP936 控制台里探测出的前缀会变成乱码写进 rules.ninja，Ninja 于是
# 一条依赖行都认不出来 —— 症状是改头文件后不重编（增量构建静默漏编），
# `ninja -t deps` 里所有 .obj 都是 "#deps 0"。这是 CMake 侧的编码问题，和工程本身无关，
# 但一旦踩到就是"改了头文件却跑的是旧二进制"，所以这里在配置阶段自行修正。
#
# 做法：让 cl 经 cmd 重定向把 /showIncludes 输出落盘（cmd 写盘的是编译器原始字节，
# 不经 CMake 的代码页解码），再用 file(READ) 按 UTF-8 读回（正好还原出正确字符），
# 切出「盘符路径之前」的那段作为前缀。CMake 写 rules.ninja 时按 UTF-8 编码，
# 于是 Ninja 能按字节匹配上 cl 的输出行。这样在任何控制台代码页下结果都一样。
# ---------------------------------------------------------------------------
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    set(_tiny_inc_dir "${CMAKE_BINARY_DIR}/CMakeFiles/tiny_showinc_probe")
    file(MAKE_DIRECTORY "${_tiny_inc_dir}")
    file(WRITE "${_tiny_inc_dir}/probe.cpp" "#include <cstddef>\n")
    file(WRITE "${_tiny_inc_dir}/probe.bat"
        "@echo off\r\n\"${CMAKE_CXX_COMPILER}\" /nologo /c /showIncludes probe.cpp > out.txt 2>&1\r\n")
    if(EXISTS "${_tiny_inc_dir}/out.txt")
        file(REMOVE "${_tiny_inc_dir}/out.txt")
    endif()
    execute_process(
        COMMAND cmd /c probe.bat
        WORKING_DIRECTORY "${_tiny_inc_dir}"
        RESULT_VARIABLE _tiny_inc_rc
    )
    set(_tiny_inc_prefix "")
    if(EXISTS "${_tiny_inc_dir}/out.txt")
        # out.txt 首行是源文件名，之后才是「<前缀>  C:\Program Files\...」这种行。
        file(READ "${_tiny_inc_dir}/out.txt" _tiny_inc_out)
        if(_tiny_inc_out MATCHES "([^\r\n]*:[ \t]*)[A-Za-z]:[/\\\\]")
            set(_tiny_inc_prefix "${CMAKE_MATCH_1}")
        endif()
    endif()
    if(_tiny_inc_prefix)
        set(CMAKE_CL_SHOWINCLUDES_PREFIX "${_tiny_inc_prefix}")
    else()
        message(WARNING "探测 cl 的 /showIncludes 前缀失败（rc=${_tiny_inc_rc}）：Ninja 的头文件依赖可能失效，改头文件后不会重新编译。可用 tools/build.ps1 重建构建目录。")
    endif()
endif()
# ---------------------------------------------------------------------------

# Vulkan SDK 通过 find_package 定位，不把某一台机器的安装路径写死。
# 如果 CMake 找不到，可设置环境变量 VULKAN_SDK，或传 -DVulkan_ROOT=<SDK 根目录>。
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)

set(GLFW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/glfw")
set(GLM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/glm")
set(TINYOBJ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tinyobjloader")
set(VMA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/VulkanMemoryAllocator")
set(STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/stb")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/imgui")
set(TINYGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tinygltf")
set(ENTT_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/entt")
set(SPDLOG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/spdlog")

# 所有第三方依赖都不进版本库。配置阶段统一检查关键文件，缺失时给出
# 一条可执行的修复命令，避免等到编译期才报 "cannot open ..."。
# spdlog 是可选依赖，当前未启用，因此不在必需清单里。
set(TINY_REQUIRED_DEPENDENCY_FILES
    "${GLFW_DIR}/CMakeLists.txt"
    "${GLFW_DIR}/include/GLFW/glfw3.h"
    "${GLM_DIR}/glm/glm.hpp"
    "${IMGUI_DIR}/imgui.cpp"
    "${STB_DIR}/stb_image.h"
    "${TINYOBJ_DIR}/tiny_obj_loader.h"
    "${VMA_DIR}/vk_mem_alloc.h"
    "${TINYGLTF_DIR}/tiny_gltf.h"
    "${TINYGLTF_DIR}/json.hpp"
    "${ENTT_DIR}/src/entt/entt.hpp"
)
foreach(DEPENDENCY_FILE IN LISTS TINY_REQUIRED_DEPENDENCY_FILES)
    if(NOT EXISTS "${DEPENDENCY_FILE}")
        message(FATAL_ERROR
            "缺少第三方依赖文件：${DEPENDENCY_FILE}\n"
            "请从仓库根目录执行：\n"
            "  powershell -NoProfile -ExecutionPolicy Bypass -File tools/SetupDependencies.ps1")
    endif()
endforeach()

# GLFW：优先使用 3rdParty/glfw 自带源码，避免依赖机器上是否安装过 glfw3。
# 如果以后把 3rdParty/glfw 换成系统包，也仍然保留 find_package 回退路径。
if(EXISTS "${GLFW_DIR}/CMakeLists.txt")
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${GLFW_DIR}" "${CMAKE_BINARY_DIR}/glfw")
else()
    find_package(glfw3 REQUIRED)
endif()
set(GLM_INCLUDE_DIR "${GLM_DIR}")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")

# main.cpp 不进共享库：它是两个可执行文件的唯一差异所在，
# 各自编译一次（几百行，代价可以忽略）。
list(APPEND SOURCES 
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
)

# ---------------------------------------------------------------------------
# 两个可执行文件，共享同一份引擎/编辑器源码
#
#   TinyEngine  运行时模式（行为与引入编辑器之前完全一致）
#   TinyEditor  编辑器模式（等价于 TinyEngine --editor）
#
# 为什么用 OBJECT 库而不是把源码列两遍：后者会让每个 .cpp 被编译两次，
# 整个工程的构建时间直接翻倍。OBJECT 库让两个 exe 复用同一批 .obj，
# 只有 main.cpp 各编一次。
# ---------------------------------------------------------------------------
add_library(tiny_core OBJECT ${SOURCES})

add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE tiny_core)

# 编辑器版：同一个 main.cpp，只多一个编译期开关，让 --editor 变成默认值。
# 运行时仍然可以显式传 --editor / --screenshot-viewport 等参数。
add_executable(TinyEditor main.cpp)
target_link_libraries(TinyEditor PRIVATE tiny_core)
target_compile_definitions(TinyEditor PRIVATE TINY_DEFAULT_EDITOR=1)

target_include_directories(tiny_core PUBLIC 
    ${Vulkan_INCLUDE_DIRS}
    ${GLM_INCLUDE_DIR}
    "${GLFW_DIR}/include"
    "${TINYOBJ_DIR}"
    "${TINYGLTF_DIR}"
    "${ENTT_DIR}/src"
    "${SPDLOG_DIR}"
    "${STB_DIR}"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${VMA_DIR}"
    "src"
)

# ---------------------------------------------------------------------------
# 预编译头（PCH）
#
# 140 个 TU 里 <vector> 被 95 个包含、glm 52 个、<vulkan/vulkan.h> 36 个。这些头每次
# 都要重新解析几万到十几万行，正是"每个 TU 固定 0.6s"的来源。src/pch.h 把它们收在
# 一起只编译一次。全量 Debug 从 16.5s 降到 13~14s，改头文件的增量也一样受益。
#
# 关掉：-DTINY_PCH=OFF。排查"某个头必须在别的头之前包含"这类问题时用得上，
# 或者想看真实 include 树时（PCH 会把 include 链条遮住）。
# 需要 CMake >= 3.16；更老的 CMake 自动跳过并给出提示。详见 docs/40-build/compile-speed.md。
# ---------------------------------------------------------------------------
option(TINY_PCH "预编译头：把 std / glm / vulkan 头预编译一次，加速全量构建" ON)
if(TINY_PCH)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.16)
        target_precompile_headers(tiny_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/pch.h")
        message(STATUS "PCH: src/pch.h（关掉用 -DTINY_PCH=OFF）")
    else()
        message(WARNING "CMake ${CMAKE_VERSION} 不支持 target_precompile_headers，已跳过 PCH（需要 3.16+）。")
    endif()
endif()

# ---------------------------------------------------------------------------
# 资源路径注入：在配置阶段把绝对路径写进生成的 AssetPaths.h，
# 让引擎不再依赖"工作目录恰好是 build/Debug"这个隐式约定。
# ---------------------------------------------------------------------------
set(TINY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${TINY_GENERATED_DIR}")

# W1：配置文件也放在工程根下的 config/ 目录，理由和资源路径一样 ——
# 不能依赖工作目录。放工程根（而不是 build/）是为了让 Debug / Release
# 共享同一份用户设置，而且这份文件是要进版本库的（作为默认预设）。
set(TINY_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config")
file(MAKE_DIRECTORY "${TINY_CONFIG_DIR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AssetPaths.h.in"
    "${TINY_GENERATED_DIR}/AssetPaths.h"
    @ONLY
)
target_include_directories(tiny_core PUBLIC "${TINY_GENERATED_DIR}")

# tinygltf 内部默认会展开一份 stb_image 实现；
# 而 Application.cpp 里已经定义了 STB_IMAGE_IMPLEMENTATION，两者会重复定义 stbi_* 符号。
# 因此关闭它自带的图片解码，改由我们注册自定义解码器（见 GLTFLoader.cpp）。
# 这两个宏必须对所有包含 tiny_gltf.h 的翻译单元一致，所以放在这里统一注入。
target_compile_definitions(tiny_core PUBLIC
    TINYGLTF_NO_STB_IMAGE
    TINYGLTF_NO_STB_IMAGE_WRITE
)

# ---------------------------------------------------------------------------
# VMA 泄漏排查开关。
#
# 打开之后：
#   1) VMA 在销毁分配器时把每条未释放的分配逐条打出来
#      （默认只有一句断言，不说是谁）；
#   2) tiny_vmaNameFlag() 会给分配附上名字，于是日志里直接显示
#      "Name: SSAOUbo" 而不是 "Name: vma_empty"。
#
# 默认关：它会记录每次分配的调用点字符串，Debug 下明显变慢，而且它报告的
# 内容本身就意味着"代码有 bug"，不该是常态。
#   cmake -S . -B build -DTINY_VMA_LEAK_CHECK=ON
# ---------------------------------------------------------------------------
option(TINY_VMA_LEAK_CHECK "打开 VMA 的详细泄漏日志（排查用，会变慢）" OFF)
if(TINY_VMA_LEAK_CHECK)
    target_compile_definitions(tiny_core PUBLIC TINY_VMA_LEAK_CHECK)
endif()

# PUBLIC：两个可执行文件都要拿到这些链接依赖与使用要求。
target_link_libraries(tiny_core PUBLIC
    Vulkan::Vulkan
    glfw
    Threads::Threads
)

# 音频后端：Windows 自带的 XAudio2（不引入第三方依赖）。
if(WIN32)
    target_link_libraries(tiny_core PUBLIC xaudio2)
endif()

# ---------------------------------------------------------------------------
# 调试 / 剖析能力开关（P0）。
#
# 原则（见 DEBUG_PROFILE_PLAN.md §1.5）：能力「随用随开」，关闭时零开销。
#   * 是否「编译进来」= 下面的 CMake option（-> TINY_ENABLE_* 宏）；
#   * 是否「本次开启」= 命令行 flag（如 --gpu-timing）。
# 重量级 / 带外部依赖的能力默认 OFF（P1 的 Tracy / RenderDoc 走 TINY_ENABLE_PROFILER）。
# ---------------------------------------------------------------------------
option(TINY_ENABLE_GPU_TIMING "编译入 --gpu-timing（每 pass GPU 计时）" ON)
option(TINY_ENABLE_VALIDATION_FEATURES "编译入 --validation-features（best practices / sync）" ON)
option(TINY_ENABLE_BUILD_INFO "编译入 --build-info（能力清单自报）" ON)
option(TINY_ENABLE_SCREENSHOT "编译入 --screenshot（无头截图 + 视觉回归）" ON)
option(TINY_ENABLE_TRACY "编译入 Tracy 时间线剖析（需要 3rdParty/tracy）" OFF)
option(TINY_ENABLE_RENDERDOC "编译入 RenderDoc 抓帧挂钩（需要 3rdParty/renderdoc/renderdoc_app.h）" OFF)

if(TINY_ENABLE_GPU_TIMING)
    target_compile_definitions(tiny_core PUBLIC TINY_ENABLE_GPU_TIMING)
endif()
if(TINY_ENABLE_VALIDATION_FEATURES)
    target_compile_definitions(tiny_core PUBLIC TINY_ENABLE_VALIDATION_FEATURES)
endif()
if(TINY_ENABLE_BUILD_INFO)
    target_compile_definitions(tiny_core PUBLIC TINY_ENABLE_BUILD_INFO)
endif()
if(TINY_ENABLE_SCREENSHOT)
    target_compile_definitions(tiny_core PUBLIC TINY_ENABLE_SCREENSHOT)
endif()

# P1-1：Tracy 需要 3rdParty/tracy；源不存在时强制关闭，保证默认（OFF）构建不受影响。
if(TINY_ENABLE_TRACY)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tracy/public/tracy/Tracy.hpp" OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tracy/tracy/Tracy.hpp")
        target_compile_definitions(tiny_core PUBLIC TINY_ENABLE_TRACY TRACY_ENABLE TRACY_ON_DEMAND)
        target_include_directories(tiny_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tracy/public")
        target_sources(tiny_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tracy/public/TracyClient.cpp")
        message(STATUS "Tracy profiling enabled")
    else()
        message(WARNING "TINY_ENABLE_TRACY=ON but 3rdParty/tracy not found; Tracy disabled. See tools/SetupDependencies.ps1")
    endif()
endif()

# P1-2：RenderDoc in-app 抓帧需要 renderdoc_app.h；缺失时强制关闭。
if(TINY_ENABLE_RENDERDOC)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/renderdoc/renderdoc_app.h")
        target_compile_definitions(tiny_core PUBLIC TINY_ENABLE_RENDERDOC)
        target_include_directories(tiny_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/renderdoc")
        message(STATUS "RenderDoc capture hook enabled")
    else()
        message(WARNING "TINY_ENABLE_RENDERDOC=ON but 3rdParty/renderdoc/renderdoc_app.h not found; disabled.")
    endif()
endif()
# ---------------------------------------------------------------------------
# TM-12：构建期反射工具。
#
# 把 .spv 里的 set / binding / 块成员偏移与总大小导出成一个 C++ 头文件，让 C++
# 侧的常量与 GLSL 只有一份来源。它是宿主机工具（不链 Vulkan、不链 glm），因此
# 不会拖慢引擎本身的构建；产物是可 diff 的文本，改一个 binding 会在 diff 里看见。
# ---------------------------------------------------------------------------
add_executable(SpirvReflect tools/SpirvReflect.cpp)

# shader 编译器同样由 Vulkan SDK 的 CMake 包给出，避免第二处硬编码路径。
set(GLSL_VALIDATOR "${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}")
if(NOT GLSL_VALIDATOR OR NOT EXISTS "${GLSL_VALIDATOR}")
    find_program(GLSL_VALIDATOR NAMES glslangValidator glslangValidator.exe)
endif()
if(NOT GLSL_VALIDATOR)
    message(FATAL_ERROR "找不到 glslangValidator。请安装 Vulkan SDK，或传 -DGLSL_VALIDATOR=<路径>。")
endif()

# P0-3：SPIR-V 静态校验器（glslangValidator 只保证能生成，不保证合法）。
get_filename_component(GLSLANG_DIR "${GLSL_VALIDATOR}" DIRECTORY)
find_program(SPIRV_VAL_EXECUTABLE NAMES spirv-val spirv-val.exe HINTS "${GLSLANG_DIR}")
if(SPIRV_VAL_EXECUTABLE)
    message(STATUS "spirv-val found: ${SPIRV_VAL_EXECUTABLE}")
else()
    message(WARNING "找不到 spirv-val，跳过 SPIR-V 静态校验。")
endif()

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(OUTPUT_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")

file(MAKE_DIRECTORY ${OUTPUT_SHADER_DIR})

# TM-12：构建期生成的反射头（<shader>.spv.reflect.h）落在这里，引擎源码直接 include。
target_include_directories(tiny_core PUBLIC "${OUTPUT_SHADER_DIR}")

file(GLOB SHADER_FILES CONFIGURE_DEPENDS "${SHADER_DIR}/*.vert" "${SHADER_DIR}/*.frag" "${SHADER_DIR}/*.comp")
foreach(SHADER_FILE ${SHADER_FILES})
    get_filename_component(FILE_NAME ${SHADER_FILE} NAME)
    set(SHADER_OUTPUT "${OUTPUT_SHADER_DIR}/${FILE_NAME}.spv")

    # P0-3：glslangValidator 生成 SPIR-V 后，用 spirv-val 做静态合法性校验。
    if(SPIRV_VAL_EXECUTABLE)
        set(SPIRV_VAL_STEP COMMAND ${SPIRV_VAL_EXECUTABLE} "${SHADER_OUTPUT}")
    else()
        set(SPIRV_VAL_STEP "")
    endif()

    # 用 OUTPUT 依赖而不是 PRE_BUILD：PRE_BUILD 每次构建都会重编**全部** shader，
    # 而 OUTPUT 形式只在对应 .vert/.frag/.comp 真的变了时才重编那一个。
    # 反射头与 .spv 一起作为同一条规则的产物：改 GLSL 时两者一起更新，
    # 不会出现「.spv 是新的、反射头是旧的」这种半新半旧的状态。
    set(REFLECT_OUTPUT "${OUTPUT_SHADER_DIR}/${FILE_NAME}.reflect.h")

    add_custom_command(
        OUTPUT "${SHADER_OUTPUT}"
        # --target-env vulkan1.3 隐含 SPIR-V 1.6：compute 会改用 LocalSizeId 执行模式，
        # 不再使用 SPIR-V 1.6 已弃用的 WorkgroupSize 内建（否则 best-practices 每次报 2 条 warning）。
        COMMAND ${GLSL_VALIDATOR} -V --target-env vulkan1.3 "${SHADER_FILE}" -o "${SHADER_OUTPUT}"
        ${SPIRV_VAL_STEP}
        DEPENDS "${SHADER_FILE}"
        COMMENT "自动编译并校验 Shader: ${FILE_NAME}"
        VERBATIM
    )
    # 反射头单独一条规则：Visual Studio 生成器对「一条规则多个 OUTPUT」的增量判断
    # 不可靠（.spv 已存在、.h 缺失时不会重跑），拆成两条就没有这个坑。
    add_custom_command(
        OUTPUT "${REFLECT_OUTPUT}"
        COMMAND $<TARGET_FILE:SpirvReflect> "${SHADER_OUTPUT}" "${REFLECT_OUTPUT}" "${FILE_NAME}"
        # 反射程序本身也是依赖：改了 SpirvReflect.cpp（比如多生成一批常量）而 GLSL 没动时，
        # 头文件必须重写，否则会留下「旧反射头 + 新 C++ 断言」这种只在 MSBuild 增量树里出现的编译错。
        DEPENDS "${SHADER_OUTPUT}" $<TARGET_FILE:SpirvReflect>
        COMMENT "反射 Shader: ${FILE_NAME} -> ${FILE_NAME}.reflect.h"
        VERBATIM
    )

    list(APPEND SHADER_OUTPUTS "${SHADER_OUTPUT}" "${REFLECT_OUTPUT}")
endforeach()

# 两个可执行文件共享同一批 .spv，所以 shader 只编一次。
add_custom_target(TinyShaders ALL DEPENDS ${SHADER_OUTPUTS})
add_dependencies(TinyShaders SpirvReflect)
# TM-12：引擎源码 include 生成的反射头，所以编译前必须先把 shader 与反射头生成出来。
add_dependencies(tiny_core TinyShaders)
add_dependencies(${PROJECT_NAME} TinyShaders)
add_dependencies(TinyEditor TinyShaders)
