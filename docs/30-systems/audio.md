---
id: systems/audio
title: 音频：混音器与设备后端
type: system
scope: src/Audio/AudioClip.*, src/Audio/AudioMixer.*, src/Audio/AudioDevice.*
read-when: 要播放声音、调音量/声部数，或排查"没声音 / 有爆音 / 声部被抢"
size: ~140 行
updated: 2026-11-16
---

# 音频：混音器与设备后端

引擎的音频是 C4 阶段从零引入的（之前完全没有声音）。分成两层：

| 层 | 文件 | 职责 | 依赖 |
| --- | --- | --- | --- |
| `audio::AudioMixer` | `src/Audio/AudioMixer.*` | 声部池、抢占、算子链、限幅、统计 | 纯 C++，不碰系统 API |
| `audio::AudioDevice` | `src/Audio/AudioDevice.*` | 打开 XAudio2、按块拉混音、线程安全投递 | Windows + `xaudio2` |

这样分层的原因是：混音逻辑可以用 `--audio-test` 在**没有声卡、没有窗口**的环境里完整自检；
设备层只负责"把 float 缓冲喂给系统"。

## 1. 数据模型

`AudioClip` 是"解码后的 PCM"，不持有任何设备资源：

```cpp
struct AudioClip
{
    uint32_t sampleRate = 48000;
    uint32_t channels = 2;
    std::vector<float> samples;   // 交错存放，范围 [-1, 1]
};
```

- `loadWavFromMemory()` 支持 PCM 8/16/24/32 与 IEEE float32、单/立体声、`WAVE_FORMAT_EXTENSIBLE`；
- `encodeWav16()` 反向写 WAV，自检里用它做"编码 → 解析"往返；
- `makeSineClip(hz, seconds, rate, gain)` 造测试音，首尾各 10ms 淡入淡出（否则片段边界会咔哒）。

WAV 解析器放在这里而不是设备里，是因为下一步要让它从 `.tpack` 资产包里读（见 `asset-pack` 文档）。

## 2. 混音器

### 2.1 声部池与抢占

- 池大小 `kMaxVoices = 64`，片段表 `kMaxClips = 32`；
- 池满时挑"优先级最低、其次最轻（`gain * volume`）"的声部作为受害者；
- 若新请求的 `priority` **低于**受害者，直接丢弃（`dropped++`），不抢占 —— 脚步声不该掐掉音乐；
- 抢占时用 `captureTail()` 把受害者按"当前状态 + 5ms 线性淡出"渲染进尾巴槽，
  与新闻部的前 5ms 淡入交叉，所以听感上没有咔哒（自检第 3 组专门测这个）。

### 2.2 算子链

每个声部固定一条链：`Pitch → Lowpass → Gain → Fade`（对应 Source 的 `CSoundOperator`，但先不做任意拼装）。

| 算子 | 参数 | 说明 |
| --- | --- | --- |
| Pitch | `params.pitch` | 采样步长 = `pitch * clipRate / outRate`，2.0 即高八度且时长减半 |
| Lowpass | `params.lowpassHz` | 一阶低通；`0` 表示旁路（远距离闷声用它） |
| Gain | `params.volume` | 与淡入淡出包络相乘 |
| Fade | `fadeInSeconds` / `fadeOutSeconds` | 默认各 5ms；`stop()` 走淡出而不是硬停 |

### 2.3 限幅与统计

所有声部混完统一 `clamp(-1, 1)`，`MixStats::peak` 记录**本次 render** 的峰值
（注意：不是历史最大值，所以"播完之后的下一块"峰值会回到 0）。

`MixStats` 字段：`activeVoices / playedEvents / stolen / dropped / finished / peak`。

## 3. 设备后端（XAudio2）

- 一个 mastering voice（2 声道、48kHz）+ 一个 float32 立体声 source voice；
- 双缓冲，每块 `kBufferFrames = 1024` 帧（48kHz 下约 21ms）；
- `IXAudio2VoiceCallback::OnBufferEnd` 在音频线程里渲染下一块并重新提交；
- 主线程通过**单生产者单消费者命令环**（256 项）投递 `Play / StopAll / MasterVolume`，
  音频线程在每块开头 `drainCommands()`；
- 打开失败不致命：打印 `[Audio] device start failed: ...（引擎继续无声运行）`，游戏照常跑。

### 3.1 线程模型与一条硬约束

| 线程 | 允许做的事 |
| --- | --- |
| 主线程 | `addClip`（**必须在 `start()` 之前**）、`play` / `stopAll` / `setMasterVolume`（进命令环） |
| 音频线程 | `drainCommands`、`mixer.render`、`SubmitSourceBuffer` |

`addClip` 必须在 `start()` 之前，是因为音频线程直接读片段表，运行期 `push_back` 会让
`std::vector` 重新分配。运行期再注册会打印
`[Audio] addClip must be called before start(), ignored` 并返回 `kInvalidClip`。
（要做运行期加载，得把片段表换成"命令环 + 固定槽位"或加锁，留给 C4b。）

## 4. 命令行

```powershell
# 不需要声卡：混音器契约自检
.\build\Debug\TinyEngine.exe --audio-test

# 需要声卡：真的播一段 440Hz，验证设备链路
.\build\Debug\TinyEngine.exe --audio-play
```

`--audio-test` 稳定输出：

```text
[Audio] mixerSelfTest PASS | cases=10 voices=64 stolen=36 dropped=1 no-click=ok
```

`--audio-play` 稳定输出：

```text
[Audio] play test | sampleRate=48000 played=1 peak=0.169706 active=1
```

## 5. 自检覆盖了什么（10 组）

1. 100 个事件 → 声部数封顶 64、抢占 36 次、事件计数 100；
2. 优先级保护：池满时低优先级请求被丢弃，池仍满；
3. 抢占点无爆音：先量"没有抢占时"的自然斜率，再要求抢占时不超过它的 1.5 倍；
4. 64 个满幅声部叠加后峰值被限幅在 `1.0`；
5. `loop` 片段 1s 后仍在响；
6. 非 `loop` 片段自然结束时走淡出并计入 `finished`；
7. `pitch = 2.0` 时 0.1s 片段约 0.05s 放完；
8. `stop()` 不是立刻断，淡完才释放；
9. `encodeWav16` → `loadWavFromMemory` 往返一致（16 位误差 ≤ 1/32000）；
10. 同一串事件两次渲染逐位相同（FNV 哈希）。

## 6. 已知限制

- **没有 3D 定位**：只有 `pan` 与 `lowpassHz` 两个手工参数，没有听者/衰减/多普勒；
- **没有总线**：只有 `masterVolume`，没有分组（音乐/音效/UI）与 ducking；
- **片段必须在 `start()` 之前注册**（见 3.1）；
- **没有流式解码**：整段 PCM 常驻内存（一个 3 分钟立体声 48kHz 片段约 69MB），
  长音乐要等 Ogg/MP3 解码与分块流送；
- **重采样是线性插值**：`pitch != 1` 或片段采样率与输出不一致时会有混叠，够用但不发烧；
- **编辑器还没接**：目前只有命令行入口，`Scene` / `EditorLayer` 里还没有"播放某个事件"的组件。

## 7. 下一步（C4b）

1. `AudioSourceComponent` + `AudioListenerComponent`，在 `SystemPhase` 里按世界坐标算距离衰减与声像；
2. 总线与音量分组（Master / Music / SFX / UI）；
3. 从 `.tpack` 里 `loadWavFromMemory`（`asset-pack` 已经就绪）；
4. 片段表运行期注册（命令环 + 固定槽位）；
5. 长音频流式播放。
