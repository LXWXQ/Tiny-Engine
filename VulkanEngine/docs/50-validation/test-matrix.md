---
id: validation/test-matrix
title: 回归测试矩阵
type: validation
scope: tools/Validate.ps1
read-when: 加/改回归项，或某项失败需要知道它在测什么
size: ~200 行
updated: 2026-10-30
---

# 回归测试矩阵

`tools/Validate.ps1` 默认跑 **18 项引擎用例 + 2 项工具自测 = 20 项**；
加 `-EnableProfiling` 时多一项（19 + 2 = 21）。

## 1. 每项的判据（统一）

| 判据 | 说明 |
|---|---|
| 退出码 = 0 | 非 0 即失败 |
| **stderr = 0 字节** | 任何 stderr 输出都算失败（验证层已不写 stderr） |
| 关键 stdout 标记存在 | 每项有自己的 `MustContain` |
| `[Validation] errors=0 warnings=0` | 除 4 项不建设备的用例，其余都必须出现 |

## 2. 引擎用例

| # | 名称 | 命令 | 关键标记 | 测什么 |
|---|---|---|---|---|
| 1 | `jobtest` | `--jobtest` | `[JobSystem] selfTest PASS` | worker pool、parallelFor、嵌套、异常传播 |
| 2 | `td09-config-migration` | `--config-test` | `[Config] selfTest PASS` | v0->v2 迁移、备份、损坏/未来版本保护 |
| 3 | `selftest-default` | `--selftest 120` | `[Scene] ECS/spatial selfTest PASS`、`[TD07] ComputeProbe disabled...` | 主路径 |
| 4 | `selftest-no-bloom` | `--no-bloom` | `[TD06] Bloom disabled: skipping Bright + Down + Up passes` | Bloom 关闭分支真跳过 |
| 5 | `selftest-no-ssao` | `--no-ssao` | `[TD07] ...` | SSAO 关闭分支 |
| 6 | `selftest-no-bloom-no-ssao` | 两者 | 同上两者 | 组合分支 |
| 7 | `td02-cross-queue` | `--cross-queue-test` | `[TD02] compute->graphics verified`、`[TD02] graphics->compute verified` | 跨队列双方向 |
| 8 | `td07-compute-probe` | `--compute-test` | `[D5] async compute probe verified` | GPU 写 + CPU 读回 |
| 9 | `td03-window-resize` | `--resize-test` | `[TD03] Swapchain recreated`、`[TD03] Render targets resized` | 缩放/最小化/恢复 |
| 10 | `td03-render-scale` | `--render-scale 0.5` | `[TD03] Render targets resized` | 动态分辨率 |
| 11 | `td04-material-slot-stress` | `--material-stress` | `[TD04] material slot stress PASS` | 延迟回收 150 轮 |
| 12 | `td05-subresource-probe` | `--subresource-test` | `[TD05] subresource probe active` | mip/layer + buffer 区间正例 |
| 13 | `td05-subresource-error` | `--subresource-error-test` | `[TD05] subresource semantic error PASS` | 反例：必须被拒 |
| 14 | `p0-build-info` | `--build-info` | `[BuildInfo] compiled:`、`[BuildInfo] policy:` | 能力清单自报 |
| 15 | `p0-diagnostics-selftest` | `--diagnostics-test <path>` | `[Diag] diagnostics selfTest PASS` | 合成消息 -> 计数 + JSONL |
| 16 | `p0-gpu-timing` | `--gpu-timing --gpu-timing-csv <path>` | `[Diag] gpu_timing.csv written:`、`[Diag] zero-overhead guard SKIP` | 每 pass 计时 |
| 17 | `p1-screenshot` | `--screenshot <png> --screenshot-frame 30` | `[Diag] screenshot written:` | 无头截图 |
| 18 | `p2-device-fault` | `--device-fault-test` | `[DeviceFault] selfTest PASS` | `VK_EXT_device_fault` 能力就绪 |

## 3. 工具自测

| # | 名称 | 内容 |
|---|---|---|
| 19 | `p3-baseline-diff` | 先 `Baseline.ps1 -Action Save` 再 `-Action Diff`，三维度（GPU 时间 / 验证层 / 视觉）自动判回归 |
| 20 | `p3-debug-bundle` | `DebugBundle.ps1` 轻量版（跳过深度校验与基线 diff）产出证据包 |
| 21 | `p1-profiling-enabled` | 仅 `-EnableProfiling` 时：`--capture-frame 10` 抓帧路径 |

## 4. 文档一致性检查

`Validate.ps1` 还会调用 `tools/DocCheck.ps1`：

| 检查 | 失败条件 |
|---|---|
| 根目录只有 `README.md` | 出现其它 `.md` |
| front-matter 完整 | 缺字段 / 类型非法 / `id` 与路径不匹配 |
| 父索引覆盖 | 新增文档未在父 `README.md` 登记 |
| 相对链接可达 | 链接目标不存在 |
| 单文档 <= 600 行 | 超长未拆 |

## 5. 加一项回归

1. 确认它能输出一个**唯一、稳定**的 stdout 标记（不要用会变的内容如帧率）；
2. 在 `tools/Validate.ps1` 的 `$tests` 数组里加一项：
   `@{ Name = "..."; Args = @(...); MustContain = @("可选的唯一标记") }`；
3. 若它不创建 Vulkan 设备，要加进"跳过零验证层检查"的白名单；
4. 在本文件 §2/§3 补一行；
5. 跑一次 `tools/Validate.ps1` 确认全绿。
