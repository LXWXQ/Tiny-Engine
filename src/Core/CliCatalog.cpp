#include "Core/CliCatalog.h"

#include <cstring>

namespace tiny::cli
{
namespace
{
const std::vector<UsageLine> kCatalog = {
    { "运行模式", "--selftest [N]", "跑 N 帧后退出（默认 600），无人值守回归用" },
    { "运行模式", "--editor", "以场景编辑器模式启动（TinyEditor.exe 默认开启）" },
    { "运行模式", "--empty-scene", "不加载 Sponza / 天空盒，只留相机" },
    { "运行模式", "--camera <px,py,pz,pitch,yaw>", "覆盖 config 里的相机位姿（基准场景与截图要固定视角）" },
    { "运行模式", "--runtime-mode", "编辑器启动后直接进入运行模式（角色 + 第三人称相机）" },
    { "窗口", "--hidden / --visible", "不显示 / 强制显示窗口（自检与截图默认 --hidden）" },
    { "窗口", "--minimized", "启动后最小化（最小化会跳过渲染）" },
    { "截图", "--screenshot <path>", "把第 --screenshot-frame 帧写成 PNG" },
    { "截图", "--screenshot-frame <n>", "截图/抓帧的目标帧号" },
    { "截图", "--screenshot-viewport", "截图只拍编辑器离屏视口（纯 3D，不含 UI）" },
    { "截图", "--capture-frame <n>", "RenderDoc 在第 n 帧抓帧（需注入 RenderDoc）" },
    { "截图", "--render-scale <f>", "动态分辨率初始倍率 0.25..1.0" },
        { "剖析", "--print-assets", "打印可执行文件目录与 shader / 资源 / 配置三个根的解析结果（TM-16）" },
        { "剖析", "--print-events", "打印 latched 事件的当前值与订阅者数量（TM-17）" },
        { "剖析", "--probe-transient", "探测惰性分配（transient attachment）能否在本机启用，打印后退出（TM-18）" },
        { "剖析", "--hot-reload", "每帧检查 .spv 的修改时间，变了就重建对应管线（改 binding 数量需重启）" },
        { "渲染", "--bloom", "强制打开 Bloom（默认跟随场景里的 Bloom 组件；量测/演示用）" },
        { "调试", "--print-formats", "打印本机对引擎用到的格式的能力表（颜色/深度/采样/存储/blit/线性）后退出" },
    { "渲染", "--no-bloom / --no-ssao / --no-taa / --no-water / --no-ssr", "关掉 Bloom / SSAO / TAA / 水面 / 水面 SSR（同 UI 复选框）" },
    { "渲染", "--taa-matrix-reprojection", "TAA 重投影改回「上一帧矩阵 × 当前世界位置」：A/B 与回退（P5-2）" },
    { "渲染", "--velocity-probe", "每 30 帧读回一次速度图与矩阵重投影的像素差（P5-2 验收）" },
    { "渲染", "--taa-classic-kernel", "TAA 退回 3x3 全收邻域，不做薄几何/去遮挡过滤（P5-3 回退）" },
    { "渲染", "--motion-blur-matrix", "运动模糊退回全屏相机速度，忽略逐像素速度图（P5-4 回退/A-B）" },
    { "渲染", "--gpu-scene / --no-gpu-scene", "强制开 / 关 GPU 驱动提交" },
    { "渲染", "--gpu-scene-min-px <px>", "GPU 提交的屏幕空间剔除阈值（像素）" },
    { "渲染", "--gpu-scene-external <n>", "外部实例数（粒子/体素共用的提交入口）" },
    { "渲染", "--no-merged-buffers", "不合并静态网格缓冲" },
    { "渲染", "--no-hiz", "关掉 Hi-Z 遮挡剔除" },
    { "渲染", "--no-shadows", "关掉方向光级联阴影（排查 / 对比用）" },
    { "渲染", "--no-terrain-batch", "关掉地形合批（逐 chunk 绘制）" },
    { "地形", "--terrain-test", "启动时创建持久地形，用于地形回归" },
    { "地形", "--streaming-world [r]", "流送世界，半径 r（1..24，默认 3）" },
    { "地形", "--voxel-world [r]", "体素世界（32x32x32 方块 chunk），半径 r（1..12，默认 1）" },
    { "地形", "--voxel-glass-demo", "在体素世界出生点前摆交叉玻璃（V-01 透明排序目测）" },
    { "地形", "--particle-demo", "在原点放一个粒子喷泉（前向透明 billboard 目测）" },
    { "地形", "--cloud-demo", "启用体积云并把相机放到云层下方（建议配 --streaming-world）" },
    { "地形", "--sun-demo", "启用太阳盘与镜头光晕（建议配 --streaming-world）" },
    { "地形", "--vegetation-demo", "在地形上种草与树，相机贴近地面（建议配 --streaming-world 3）" },
    { "地形", "--fog-demo", "在相机前方放一个局部体积雾盒（建议配 --streaming-world 3）" },
    { "地形", "--streaming-fly [speed]", "沿 +X 直线飞行，用于流送/卸载回归" },
    { "地形", "--streaming-fly-radius <r>", "绕圆飞行半径（0 = 直线）" },
    { "地形", "--streaming-budget <ms>", "每帧地形生成预算（毫秒）" },
    { "地形", "--no-origin-rebase", "关掉浮动原点（默认开启）" },
    { "地形", "--origin-threshold <u>", "浮动原点触发距离（世界单位，最低 64）" },
    { "自检", "--cross-queue-test", "TD-02 跨队列探针" },
    { "自检", "--resize-test", "自检中连续缩放窗口并切换动态分辨率（需配 --selftest）" },
    { "自检", "--material-stress", "每帧分配/延迟归还材质槽位（TD-04）" },
    { "自检", "--subresource-test / --subresource-error-test", "TD-05 正向 / 反例探针" },
    { "自检", "--compute-test", "D-5 compute probe 独立回归" },
    { "自检", "--config-test", "配置迁移 / 损坏配置保护自检" },
    { "自检", "--diagnostics-test [path]", "诊断管线自检（合成消息 -> 计数 + JSONL）" },
    { "自检", "--device-fault-test", "VK_EXT_device_fault 自检" },
    { "自检", "--jobtest", "JobSystem 自检" },
    { "自检", "--handle-test", "实体句柄契约自检（不需要 Vulkan / 窗口）" },
    { "自检", "--trace-test", "射线契约自检（不需要 Vulkan / 窗口）" },
    { "自检", "--gltf-node-test", "glTF 节点树契约自检（不需要 Vulkan / 窗口）" },
    { "资产", "--pack-test", "资产包契约自检（不需要 Vulkan / 窗口）" },
    { "资产", "--pack-build <dir> <out>", "把目录递归打成 .tpack 资产包" },
        { "序列化", "--material-json-test", "材质 JSON 只存非默认字段的自检（不需要 Vulkan / 窗口）" },
        { "地形", "--terrain-sculpt-test", "地形雕刻差分与 JSON 往返的自检（不需要 Vulkan / 窗口）" },
        { "渲染", "--texture-budget <MB>", "纹理显存预算（MB；0 = 不限制，超限时降 mip / 减半分辨率）" },
        { "自检", "--texture-budget-test", "纹理预算降级策略自检（不需要 Vulkan / 窗口）" },
        { "自检", "--tick-test", "固定步长模拟时钟契约自检（不需要 Vulkan / 窗口）" },
        { "自检", "--render-interp-test", "渲染插值（TRS 分解 + slerp）契约自检（不需要 Vulkan / 窗口）" },
        { "自检", "--character-collision-test", "角色扫掠盒/滑动/上台阶契约自检（不需要 Vulkan / 窗口）" },
        { "自检", "--replay-test", "角色步进纯函数与输入录制/回放一致性自检（不需要 Vulkan / 窗口）" },
        { "自检", "--viewmodel-test", "viewmodel 投影与偏移矩阵自检（不需要 Vulkan / 窗口）" },
        { "自检", "--sky3d-test", "3D 天空盒相机锁定矩阵自检（不需要 Vulkan / 窗口）" },
        { "自检", "--particle-bench", "粒子上限下的模拟耗时测量（不需要 Vulkan / 窗口）" },
    { "音频", "--audio-test", "音频混音器契约自检（不需要声卡 / 窗口）" },
    { "音频", "--audio-play", "打开声卡播一段 440Hz 测试音（需要音频设备）" },
    { "音频", "--audio-spatial-test", "3D 音源空间参数（衰减/声像/低通）契约自检（不需要声卡）" },
    { "剖析", "--build-info", "打印本二进制的调试能力后退出" },
    { "剖析", "--gpu-timing", "每 pass GPU 计时，退出时写 gpu_timing.csv" },
    { "剖析", "--gpu-timing-csv <path>", "指定计时 CSV 路径（隐含 --gpu-timing）" },
    { "剖析", "--validation-features <list>", "验证层增强：bp / sync / all" },
    { "剖析", "--validation-jsonl <path>", "验证层消息写 JSONL" },
    { "剖析", "--update-profile", "每 60 帧打印 Scene::update 分段耗时" },
    { "剖析", "--stats-pipelines", "退出时打印管线数量、建管线耗时与状态去重（TM-06）" },
    { "剖析", "--stats-scratch", "退出时打印每帧暂存区的分配次数、字节数、扩容与刷新次数（TM-07）" },
    { "剖析", "--alias-report", "退出时打印渲染图资源别名候选对与预估可省显存（TM-08，只读报告）" },
{ "剖析", "--parallel-prepare", "Pass 的准备阶段（访问去重 + 校验 + 组装附件描述）交给作业系统并行（TM-10）" },
{ "剖析", "--stats-rendergraph", "退出时打印 Pass 准备 / 录制各自的每帧耗时（TM-10）" },
{ "剖析", "--stats-frame", "退出时打印屏障 / 绑定 / 帧时间分布 / 资源创建 / 显存 / 剔除汇总（T-10）" },
    { "剖析", "--no-pipeline-cache", "关掉管线绑定缓存（T-09 的整体回退；关掉后每次绑定都照发）" },
        { "剖析", "--pass-cull", "打开 Pass 剔除（T-05）。默认关：本引擎的 Pass 在关闭时就不进图，实测没有「输出无人读」的 Pass" },
    { "剖析", "--no-pass-cull", "关掉 Pass 剔除（T-05 的整体回退；关掉后没人读输出的 Pass 也照跑）" },
    { "剖析", "--pass-cull-dry-run", "只打印本帧哪些 Pass 会被剔除，不真的跳过（T-05）" },
        { "剖析", "--rt-pool", "启用跨帧渲染目标池（T-04；默认关，只在尺寸换回旧值时命中）" },
                    { "剖析", "--gpu-scene-persist", "打开持久原语/实例缓冲的镜像（T-08 骨架；默认关，暂无消费者，打开会多 10~20% 重建成本）" },
                    { "剖析", "--no-gpu-scene-persist", "显式关掉持久原语/实例缓冲（--gpu-scene-persist 的回退）" },
        { "剖析", "--stats-queue", "退出时打印图形 / 计算队列各自的忙碌窗口与重叠时长（T-07 第一步）" },
        { "剖析", "--no-async-split", "退回单次图形提交：不做 G0 -> C0 -> G1 拆分（T-07 整体回退开关）" },
{ "剖析", "--cube-field <n>", "铺 n 个立方体排成立方格点，走外部实例注入路径（P2 的入场尺子）" },
                    { "剖析", "--rebuild-structure <n>", "每 n 帧强制重建一次实例结构，用来取摊平的稳态样本（0 = 关）" },
        { "剖析", "--cube-field-motion <n>", "立方体阵列每帧整体旋转 n 微弧度，用来量「动态实例」的每帧摊平成本（配合 --cube-field）" },
        { "剖析", "--no-instance-cull", "关掉实例级剔除（P2 第三步的整体回退：GPU 写恒等 remap，画面应与默认逐像素一致）" },
        { "剖析", "--no-single-frame-targets", "回到每帧 slot 各一份目标（T-04b 的整体回退开关，默认开的是合成一张）" },
        { "剖析", "--serialize-frames", "本帧图形工作等上一帧完成（T-04b；会让 GPU 不再重叠两帧）" },
    { "剖析", "--light-stress", "铺 N 盏点光源做压力场（配合 --clustered-lighting 看聚簇收益）" },
    { "剖析", "--clustered-lighting", "片元按所在簇取灯表，而不是遍历全部点光源（TM-13）" },
    { "剖析", "--stats-clusters", "退出时打印聚簇构建统计：使用簇数 / 最大簇 / 丢弃数（TM-13）" },
    { "剖析", "--clustered-gpu-build", "建簇改在 GPU 上跑一次 dispatch（定长槽位，索引占 3.4 MiB/帧槽）" },
        { "剖析", "--fireflies <n>", "区域内点亮 n 个萤火虫式光源（形状 + 颜色 + 照亮地面），默认 0 = 关闭" },
        { "剖析", "--fireflies-radius <m>", "萤火虫铺开的半径（米，默认 12）；决定这群灯占多大一片地" },
    { "剖析", "--light-stress-radius <m>", "压力场铺开的半径（米，默认 18）；配合 --light-stress 看稀疏分布下的聚簇收益" },
{ "剖析", "--auto-exposure", "自动曝光：每帧统计 HDR 平均亮度并缓慢适应（默认关，不改默认画面）" },
    { "帮助", "--help / -h", "打印这张表并退出（退出码 0）" },
    { "帮助", "--list-options", "打印这张选项表后退出（与编辑器「帮助 → 命令行选项」同源）" },
};
} // namespace

const std::vector<UsageLine>& catalog()
{
    return kCatalog;
}

void printCatalog(std::FILE* out, const char* exeName)
{
    if (out == nullptr)
        return;

    const char* displayName = "TinyEngine";
    if (exeName != nullptr)
    {
        const char* slash = std::strrchr(exeName, '/');
        const char* backslash = std::strrchr(exeName, '\\');
        const char* base = slash != nullptr ? slash + 1 : exeName;
        if (backslash != nullptr && backslash + 1 > base)
            base = backslash + 1;
        displayName = base;
    }

    std::fprintf(out, "Tiny Engine - %s\n\n", displayName);
    std::fprintf(out, "\u7528\u6cd5: TinyEngine.exe [\u9009\u9879]\n\n");
    const char* currentGroup = nullptr;
    for (const UsageLine& line : kCatalog)
    {
        if (currentGroup == nullptr || std::strcmp(currentGroup, line.group) != 0)
        {
            std::fprintf(out, "%s\n", line.group);
            currentGroup = line.group;
        }
        std::fprintf(out, "  %-34s %s\n", line.flags, line.text);
    }
    std::fprintf(out, "\n\u672a\u77e5\u53c2\u6570\u4f1a\u4ee5\u9000\u51fa\u7801 2 \u7ed3\u675f\uff0c\u5e76\u5728 stderr \u6253\u5370\u4e00\u884c\u8bf4\u660e\u3002\n");
    std::fprintf(out, "\u672c\u8868\u5171 %zu \u884c\uff1b\u7f16\u8f91\u5668\u300c\u5e2e\u52a9 -> \u547d\u4ee4\u884c\u9009\u9879\u300d\u9762\u677f\u4e0e\u6b64\u540c\u6e90\u3002\n", kCatalog.size());
}

bool selfTest()
{
    if (kCatalog.size() < 70)
    {
        std::printf("[Cli] selfTest FAIL | \u9009\u9879\u8868\u53ea\u6709 %zu \u884c\uff08\u9884\u671f >= 70\uff09\n", kCatalog.size());
        return false;
    }

    for (size_t i = 0; i < kCatalog.size(); ++i)
    {
        const UsageLine& line = kCatalog[i];
        if (line.group == nullptr || line.group[0] == '\0' || line.flags == nullptr ||
            line.flags[0] == '\0' || line.text == nullptr || line.text[0] == '\0')
        {
            std::printf("[Cli] selfTest FAIL | \u7b2c %zu \u884c\u7f3a\u5b57\u6bb5\n", i);
            return false;
        }
        for (size_t j = i + 1; j < kCatalog.size(); ++j)
        {
            if (std::strcmp(kCatalog[i].flags, kCatalog[j].flags) == 0)
            {
                std::printf("[Cli] selfTest FAIL | \u91cd\u590d\u7684\u5f00\u5173 %s\n", kCatalog[i].flags);
                return false;
            }
        }
    }

    bool hasRadius = false;
    bool hasList = false;
    for (const UsageLine& line : kCatalog)
    {
        if (std::strstr(line.flags, "--light-stress-radius") != nullptr)
            hasRadius = true;
        if (std::strcmp(line.flags, "--list-options") == 0)
            hasList = true;
    }
    if (!hasRadius || !hasList)
    {
        std::printf("[Cli] selfTest FAIL | \u65b0\u589e\u5f00\u5173\u672a\u767b\u8bb0\n");
        return false;
    }

    std::printf("[Cli] selfTest PASS | \u9009\u9879\u8868 %zu \u884c + \u5206\u7ec4\u975e\u7a7a + \u65e0\u91cd\u590d\n", kCatalog.size());
    return true;
}

} // namespace tiny::cli
