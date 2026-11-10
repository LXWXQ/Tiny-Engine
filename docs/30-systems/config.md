---
id: systems/config
title: 运行时配置
type: system
scope: src/Core/EngineConfig.h, src/Core/EngineConfig.cpp, config/engine.json
read-when: 增删可调参数、改配置格式、处理配置迁移
size: ~180 行
updated: 2026-10-30
---

# 运行时配置

## 1. 文件位置与格式

`config/engine.json`，**当前 `version: 2`**，进版本库当默认预设。

放工程根（而不是 `build/`）的理由：与资源路径同源——不能依赖工作目录；
且 Debug/Release 共享同一份设置。

## 2. 结构

```json
{
  "version": 2,
  "camera": { "position": [...], "rotation": [...] },
  "engineSettings": {
    "bloom":      { "enabled", "intensity", "knee", "threshold" },
    "lighting":   { "color", "direction", "exposure" },
    "ssao":       { "enabled", "bias", "kernelSize", "radius" },
    "motionBlur": { "enabled", "maxRadius", "strength" },
    "water":      { "enabled", "level", "waveStrength", "waveSpeed" },
    "renderScale": 1.0,
    "windowTitle": "...",
    "planned":    { "shadowEnabled", "ssrEnabled" }
  }
}
```

> `motionBlur` 字段当前**没有渲染实现**（属规划位）；`planned` 里的开关同理。

## 3. 版本迁移（TD-09）

| 版本 | 变化 |
|---|---|
| v0 | 字段平铺在顶层 |
| v1 | 旧顶层字段收进 `engineSettings` |
| v2 | 补齐 `renderScale` / `planned` |

迁移规则：

1. 迁移前把原文件复制为 `.vN.bak`；
2. 迁移成功后**原子写回**（先写 `.tmp` 再 rename）；
3. **未来版本 / 损坏 JSON / 非法 version 一律不覆盖原文件**；
4. `save()` 对不可读的已有配置**直接拒绝**覆盖。

## 4. 容错取向

文件不存在 / JSON 语法错 / 单个字段类型不对，**都不抛异常**：
缺失字段保持编译期默认值并记一条警告。手改坏一个字段不该让引擎起不来。

## 5. 界面文字必须 ASCII

`EngineConfig::getLastStatus()` 返回英文。因为 ImGui 内置字体只有 ASCII 字形，
中文会整条渲染成 `???`。这是刻意的，不是遗漏。见 [imgui](imgui.md)。

## 6. 只持久化"设置"，不持久化"场景"

`EngineSettings` + 相机位姿进配置；场景 / 模型 / 材质**不进**。
后者是**资源**不是**设置**，恢复它们涉及资产路径、加载顺序、失败回退，是另一件事。

F5-06 起，阴影 / Bloom / SSAO / SSR / 运动模糊 / TAA / 水面 / 雾 / 环境这 9 类画质功能由
场景级组件（`.scene.json` v4 顶层 `sceneComponents`）持有；`engine.json` 仍是**启动默认值**，
载入场景后由组件写回 `EngineSettings` 缓存。编辑器里 Save/Reload `engine.json` 会以新默认值
重建当前场景的组件，属于“恢复默认”，不是保存场景。

## 7. 自检

```powershell
.\\build\\Debug\\TinyEngine.exe --config-test
```

输出：`[Config] selfTest PASS: v0 -> v2 migration, backup, corrupt/future config left untouched`

覆盖：v0->v2 迁移、备份内容一致、损坏文件不被覆盖、version 999 不被覆盖。

> 注意：该测试需要 `%TEMP%` 可写。若在受限沙箱里运行，需把 `TEMP`/`TMP`
> 指到可写目录，否则会报 `cannot create temp directory`。

## 8. 加新参数

1. 加进 `EngineSettings`（`src/Core/FrameInfo.h`）；
2. 在 `EngineConfig` 的 serialize / deserialize 两侧同步；
3. 若影响配置结构，**升 `version` 并加迁移函数**，同时更新本文件 §3；
4. 若属于场景画质功能，注册 `scene.*` 组件并接入 `syncSceneSettings()`；仅调试/提交路径开关才留在 ImGui/Render 面板；
5. 在 `--config-test` 里补校验（至少确保往返一致）。
