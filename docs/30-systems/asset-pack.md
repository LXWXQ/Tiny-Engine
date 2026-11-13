---
id: systems/asset-pack
title: 资产包（.tpack）
type: system
scope: src/Core/AssetPack.h, src/Core/AssetPack.cpp, src/RHI/Texture.cpp
read-when: 要打包/挂载资产、要给新的加载路径接上资产包、或排查"贴图读不到"
size: ~90 行
updated: 2026-11-13
---

# 资产包（.tpack）

把 `Resources/` 下的散装文件打成一个只读容器，运行时挂载后优先从包内读，未命中自动退回文件系统。

## 为什么

`Resources/` 目前是 129 个文件 85.1MB（Models 89 个 61.6MB、Texture 39 个 25.5MB），
每个贴图/模型都要一次 `open + read + decode`，路径散落在 `src/RHI`、`src/Scene`、`src/Editor`。
资产包解决两件事：文件数（一次 open）与路径（一条挂载点），顺便给"16 字节对齐"和
"包内随机读"留出接口。

## 格式（v1）

| 区段 | 大小 | 内容 |
|---|---|---|
| 包头 | 16B | `magic[4]="TPAK"`、`uint32 version=1`、`uint32 entryCount`、`uint32 reserved` |
| 条目表 | 144B × N | `char name[128]`、`uint64 offset`、`uint64 size` |
| 负载 | 变长 | 每个条目起点 16 字节对齐，尾部补零 |

- 名字归一化：`\` → `/`、去掉循环 `./` 与前导 `/`。
- 无压缩、无加密、无 CRC。这三项一旦写进 v1 就改不掉，所以先留空。
- `open()` 把整包读进内存（85MB 量级可接受）。将来换成 `pread` 时 `read()` 签名不变。

## 命令行

```
TinyEngine.exe --pack-build Resources\Texture build\assets.tpack   # 递归打包一个目录
TinyEngine.exe --pack-test                                          # 契约自检（不需要 Vulkan）
```

`--pack-test` 覆盖：打包→打开→条目数→16B 对齐→逐字节内容→缺失条目→绝对路径后缀命中→
坏 magic/version 被拒→挂载后删源目录仍可读→`unmountAll` 回落到文件系统。

> 受限环境可能禁止写系统临时目录（access denied），自检会退回到 `build/pack_selftest_<时间戳>`。

## 运行时挂载

`main.cpp` 在创建 `Application` 之前检查 `Resources/assets.tpack`，存在就挂上：

```cpp
if (auto pack = assets::AssetPack::open("Resources/assets.tpack", packError))
    assets::mounts().mount(pack);
```

包文件不进仓库（构建产物），需要时用 `--pack-build` 生成。

## 查找规则

`Mounts::readFile(path, out)` 依次尝试：精确名 → "包内名是查询路径的后缀且前一个字符是 `/`"（取最长命中）。
所以代码里传 `C:/.../Resources/Texture/Water/water_normal.png` 也能命中包内 `Texture/Water/water_normal.png`。
多个包按后挂优先（倒序）；全部未命中才回退 `std::filesystem::exists` / 读盘。

## 已接入的读取点

| 位置 | 状态 |
|---|---|
| `src/RHI/Texture.cpp` `loadLDR`/`loadHDR` | 已接入：`stbi_load_from_memory` / `stbi_loadf_from_memory` |
| `src/Scene/GLTFLoader.cpp`（`LoadBinaryFromFile`/`LoadASCIIFromFile`） | 未接入（下一步） |
| `src/RHI/Pipeline.cpp`（SPIR-V） | 未接入 |
| `src/Editor/SceneSerializer.cpp`（场景 JSON） | 未接入 |

## 验证方式

- `--pack-test`：契约自检，14 个断言，输出 `[AssetPack] selfTest PASS | cases=14 align=16 suffix-lookup=ok`。
- 端到端：`--pack-build Resources\Texture Resources\assets.tpack` 后把 `Resources/Texture` 改名，
  再跑 `--selftest 5`：贴图仍能加载、Sponza bounds 与基线一致，说明走的是包内数据。

## 坑

- 条目名 128 字节上限（含结尾 0），超长直接拒绝。
- 空目录打包会失败（`目录里没有文件`），不是静默产出空包。
- 包内视图指针指向 `open()` 读进内存的 blob，`AssetPack` 生命周期必须覆盖使用期。
