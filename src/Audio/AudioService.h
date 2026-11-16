#pragma once

#include "Audio/AudioDevice.h"

#include <string>
#include <unordered_map>

namespace audio
{
// 进程级音频服务：懒启动设备、按路径缓存片段、把空间参数转发给设备。
//
// 为什么是单例：加载点分散在 Scene / 编辑器 / 命令行，和 assets::mounts() 的理由一样。
// 线程约定：这些方法都在主线程调用；真正的混音在音频线程。
class AudioService
{
public:
    static AudioService& instance();

    // 第一次 play 时懒启动；失败只记录原因并返回 false（引擎继续无声运行）。
    bool ensureStarted();
    bool available() const;
    const std::string& lastError() const;

    // 按路径加载并缓存片段（走资产包，失败返回 kInvalidClip）。
    ClipId loadClip(const std::string& path);

    PlayRequest play(ClipId clip, const VoiceParams& params);
    bool updateVoice(PlayRequest request, const VoiceParams& params);
    void stop(PlayRequest request);
    void stopAll();
    void setMasterVolume(float volume);

    MixStats stats() const;
    uint32_t sampleRate() const;

    void shutdown();

private:
    AudioService() = default;
    AudioService(const AudioService&) = delete;
    AudioService& operator=(const AudioService&) = delete;

    std::unique_ptr<AudioDevice> device_;
    std::unordered_map<std::string, ClipId> clipCache_;
    std::string error_;
    bool startAttempted_ = false;
};
} // namespace audio
