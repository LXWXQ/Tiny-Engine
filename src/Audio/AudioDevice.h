#pragma once

#include "Audio/AudioMixer.h"

#include <cstdint>
#include <memory>
#include <string>

namespace audio
{
// 播放令牌：主线程拿它来后续改参数/停止。0 表示无效。
// （它不是 AudioMixer 的声部号 —— 声部号只有音频线程知道。）
using PlayRequest = uint64_t;
inline constexpr PlayRequest kInvalidPlayRequest = 0;

// 音频设备：把 AudioMixer 接到系统音频后端（Windows 上是 XAudio2）。
//
// 这里只做三件事：开设备、按固定块长把混音结果推给后端、把主线程的播放请求
// 安全地送进音频线程。混音本身全在 AudioMixer 里，所以 --audio-test 不需要设备。
//
// 线程模型：
//   * 主线程：addClip / play / updateVoice / stop / stopAll / setMasterVolume（全部进命令环）
//   * 音频线程：消费命令环、调 mixer.render、提交缓冲
// 片段表是固定槽位数组（kMaxClips），运行期注册不会让 vector 重新分配，
// 所以音频线程读它不需要加锁。
class AudioDevice
{
public:
    AudioDevice();
    ~AudioDevice();

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // 打开设备并启动音频线程；失败时返回 false，lastError() 给出原因。
    bool start(uint32_t sampleRate = 48000);
    void shutdown();

    bool running() const;
    uint32_t sampleRate() const;
    uint32_t bufferFrames() const;

    // 注册片段。运行期也可以调用（走 AddClip 命令，写进固定槽位）。
    ClipId addClip(std::shared_ptr<const AudioClip> clip);

    // 播放请求先进命令环，由音频线程在下一块开始时消费。
    // 返回设备侧令牌，后续用它 updateVoice / stop。
    PlayRequest play(ClipId clip, const VoiceParams& params);

    // 改运行期参数；令牌已失效（声部播完/被抢）时返回 false。
    bool updateVoice(PlayRequest request, const VoiceParams& params);

    void stop(PlayRequest request);
    void stopAll();
    void setMasterVolume(float volume);

    MixStats stats() const;
    const std::string& lastError() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
} // namespace audio
