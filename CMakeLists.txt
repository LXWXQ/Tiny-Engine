cmake_minimum_required(VERSION 3.15)
project(VulkanEngine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

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
set(SENTINEL_REQUIRED_DEPENDENCY_FILES
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
foreach(DEPENDENCY_FILE IN LISTS SENTINEL_REQUIRED_DEPENDENCY_FILES)
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
#   VulkanEngine   运行时模式（行为与引入编辑器之前完全一致）
#   SentinelEditor 编辑器模式（等价于 VulkanEngine --editor）
#
# 为什么用 OBJECT 库而不是把源码列两遍：后者会让每个 .cpp 被编译两次，
# 整个工程的构建时间直接翻倍。OBJECT 库让两个 exe 复用同一批 .obj，
# 只有 main.cpp 各编一次。
# ---------------------------------------------------------------------------
add_library(sentinel_core OBJECT ${SOURCES})

add_executable(${PROJECT_NAME} main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE sentinel_core)

# 编辑器版：同一个 main.cpp，只多一个编译期开关，让 --editor 变成默认值。
# 运行时仍然可以显式传 --editor / --screenshot-viewport 等参数。
add_executable(SentinelEditor main.cpp)
target_link_libraries(SentinelEditor PRIVATE sentinel_core)
target_compile_definitions(SentinelEditor PRIVATE SENTINEL_DEFAULT_EDITOR=1)

target_include_directories(sentinel_core PUBLIC 
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
# 资源路径注入：在配置阶段把绝对路径写进生成的 AssetPaths.h，
# 让引擎不再依赖"工作目录恰好是 build/Debug"这个隐式约定。
# ---------------------------------------------------------------------------
set(SENTINEL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${SENTINEL_GENERATED_DIR}")

# W1：配置文件也放在工程根下的 config/ 目录，理由和资源路径一样 ——
# 不能依赖工作目录。放工程根（而不是 build/）是为了让 Debug / Release
# 共享同一份用户设置，而且这份文件是要进版本库的（作为默认预设）。
set(SENTINEL_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/config")
file(MAKE_DIRECTORY "${SENTINEL_CONFIG_DIR}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AssetPaths.h.in"
    "${SENTINEL_GENERATED_DIR}/AssetPaths.h"
    @ONLY
)
target_include_directories(sentinel_core PUBLIC "${SENTINEL_GENERATED_DIR}")

# tinygltf 内部默认会展开一份 stb_image 实现；
# 而 Application.cpp 里已经定义了 STB_IMAGE_IMPLEMENTATION，两者会重复定义 stbi_* 符号。
# 因此关闭它自带的图片解码，改由我们注册自定义解码器（见 GLTFLoader.cpp）。
# 这两个宏必须对所有包含 tiny_gltf.h 的翻译单元一致，所以放在这里统一注入。
target_compile_definitions(sentinel_core PUBLIC
    TINYGLTF_NO_STB_IMAGE
    TINYGLTF_NO_STB_IMAGE_WRITE
)

# ---------------------------------------------------------------------------
# VMA 泄漏排查开关。
#
# 打开之后：
#   1) VMA 在销毁分配器时把每条未释放的分配逐条打出来
#      （默认只有一句断言，不说是谁）；
#   2) sentinel_vmaNameFlag() 会给分配附上名字，于是日志里直接显示
#      "Name: SSAOUbo" 而不是 "Name: vma_empty"。
#
# 默认关：它会记录每次分配的调用点字符串，Debug 下明显变慢，而且它报告的
# 内容本身就意味着"代码有 bug"，不该是常态。
#   cmake -S . -B build -DSENTINEL_VMA_LEAK_CHECK=ON
# ---------------------------------------------------------------------------
option(SENTINEL_VMA_LEAK_CHECK "打开 VMA 的详细泄漏日志（排查用，会变慢）" OFF)
if(SENTINEL_VMA_LEAK_CHECK)
    target_compile_definitions(sentinel_core PUBLIC SENTINEL_VMA_LEAK_CHECK)
endif()

# PUBLIC：两个可执行文件都要拿到这些链接依赖与使用要求。
target_link_libraries(sentinel_core PUBLIC
    Vulkan::Vulkan
    glfw
    Threads::Threads
)

# ---------------------------------------------------------------------------
# 调试 / 剖析能力开关（P0）。
#
# 原则（见 DEBUG_PROFILE_PLAN.md §1.5）：能力「随用随开」，关闭时零开销。
#   * 是否「编译进来」= 下面的 CMake option（-> SENTINEL_ENABLE_* 宏）；
#   * 是否「本次开启」= 命令行 flag（如 --gpu-timing）。
# 重量级 / 带外部依赖的能力默认 OFF（P1 的 Tracy / RenderDoc 走 SENTINEL_ENABLE_PROFILER）。
# ---------------------------------------------------------------------------
option(SENTINEL_ENABLE_GPU_TIMING "编译入 --gpu-timing（每 pass GPU 计时）" ON)
option(SENTINEL_ENABLE_VALIDATION_FEATURES "编译入 --validation-features（best practices / sync）" ON)
option(SENTINEL_ENABLE_BUILD_INFO "编译入 --build-info（能力清单自报）" ON)
option(SENTINEL_ENABLE_SCREENSHOT "编译入 --screenshot（无头截图 + 视觉回归）" ON)
option(SENTINEL_ENABLE_TRACY "编译入 Tracy 时间线剖析（需要 3rdParty/tracy）" OFF)
option(SENTINEL_ENABLE_RENDERDOC "编译入 RenderDoc 抓帧挂钩（需要 3rdParty/renderdoc/renderdoc_app.h）" OFF)

if(SENTINEL_ENABLE_GPU_TIMING)
    target_compile_definitions(sentinel_core PUBLIC SENTINEL_ENABLE_GPU_TIMING)
endif()
if(SENTINEL_ENABLE_VALIDATION_FEATURES)
    target_compile_definitions(sentinel_core PUBLIC SENTINEL_ENABLE_VALIDATION_FEATURES)
endif()
if(SENTINEL_ENABLE_BUILD_INFO)
    target_compile_definitions(sentinel_core PUBLIC SENTINEL_ENABLE_BUILD_INFO)
endif()
if(SENTINEL_ENABLE_SCREENSHOT)
    target_compile_definitions(sentinel_core PUBLIC SENTINEL_ENABLE_SCREENSHOT)
endif()

# P1-1：Tracy 需要 3rdParty/tracy；源不存在时强制关闭，保证默认（OFF）构建不受影响。
if(SENTINEL_ENABLE_TRACY)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tracy/public/tracy/Tracy.hpp" OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tracy/tracy/Tracy.hpp")
        target_compile_definitions(sentinel_core PUBLIC SENTINEL_ENABLE_TRACY TRACY_ENABLE TRACY_ON_DEMAND)
        target_include_directories(sentinel_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tracy/public")
        target_sources(sentinel_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/tracy/public/TracyClient.cpp")
        message(STATUS "Tracy profiling enabled")
    else()
        message(WARNING "SENTINEL_ENABLE_TRACY=ON but 3rdParty/tracy not found; Tracy disabled. See tools/SetupDependencies.ps1")
    endif()
endif()

# P1-2：RenderDoc in-app 抓帧需要 renderdoc_app.h；缺失时强制关闭。
if(SENTINEL_ENABLE_RENDERDOC)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/renderdoc/renderdoc_app.h")
        target_compile_definitions(sentinel_core PUBLIC SENTINEL_ENABLE_RENDERDOC)
        target_include_directories(sentinel_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/renderdoc")
        message(STATUS "RenderDoc capture hook enabled")
    else()
        message(WARNING "SENTINEL_ENABLE_RENDERDOC=ON but 3rdParty/renderdoc/renderdoc_app.h not found; disabled.")
    endif()
endif()
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
    list(APPEND SHADER_OUTPUTS "${SHADER_OUTPUT}")
endforeach()

# 两个可执行文件共享同一批 .spv，所以 shader 只编一次。
add_custom_target(SentinelShaders ALL DEPENDS ${SHADER_OUTPUTS})
add_dependencies(${PROJECT_NAME} SentinelShaders)
add_dependencies(SentinelEditor SentinelShaders)