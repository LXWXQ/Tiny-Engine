#pragma once

#include "Audio/AudioClip.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace audio
{
// 声部上限：同时最多 64 个声部在响。超过就按优先级/响度抢占，
// 被抢占的声部用 5ms 淡出尾巴交叉掉，避免"啪"的一声。
inline constexpr uint32_t kMaxVoices = 64;
inline constexpr uint32_t kMaxClips = 32;
inline constexpr uint32_t kMaxOperatorsPerVoice = 4;
inline constexpr uint32_t kTailFrames = 512;          // 抢占时保留的淡出尾巴上限
inline constexpr float kDefaultFadeSeconds = 0.005f;

// 算子种类。Source 的 CSoundOperator 是一棵可组合的树，这里先做
// "每声部一条固定顺序的链"：Pitch -> Lowpass -> Gain -> Fade。
enum class AudioOperatorKind : uint8_t
{
    Gain,
    Lowpass,
    Pitch,
    Fade,
};

struct AudioOperator
{
    AudioOperatorKind kind = AudioOperatorKind::Gain;
    float a = 1.0f;
    float b = 0.0f;
};

using ClipId = uint32_t;
inline constexpr ClipId kInvalidClip = 0xFFFFFFFFu;

using VoiceId = uint32_t;
inline constexpr VoiceId kInvalidVoice = 0xFFFFFFFFu;

struct VoiceParams
{
    float volume = 1.0f;
    float pitch = 1.0f;          // 1 = 原速；2 = 高八度且时长减半
    float pan = 0.0f;            // -1 全左，0 居中，+1 全右（等功率）
    float lowpassHz = 0.0f;      // 0 = 关闭；远距离闷声用它
    bool loop = false;
    int priority = 0;            // 数值越大越不容易被抢占
    float fadeInSeconds = kDefaultFadeSeconds;
    float fadeOutSeconds = kDefaultFadeSeconds;
};

struct MixStats
{
    uint32_t activeVoices = 0;
    uint64_t playedEvents = 0;
    uint64_t stolen = 0;         // 池满时抢占了别人
    uint64_t dropped = 0;        // 池满且对方优先级更高，请求被丢弃
    uint64_t finished = 0;       // 自然播完（含淡出）
    float peak = 0.0f;           // 最近一次 render 的峰值
};

// 纯软件混音器：不碰任何系统 API，因此可以无设备跑自检。
// 线程约定：实例归音频线程所有；主线程通过 AudioDevice 的命令队列投递。
class AudioMixer
{
public:
    AudioMixer();

    ClipId addClip(std::shared_ptr<const AudioClip> clip);
    const AudioClip* clip(ClipId id) const;

    VoiceId play(ClipId clip, const VoiceParams& params);
    void stop(VoiceId voice);
    void stopAll();
    void setMasterVolume(float volume);

    // 渲染交错立体声。frames 是"帧"数（每帧 2 个 float）。
    void render(float* out, uint32_t frames, uint32_t sampleRate);

    MixStats stats() const;
    uint32_t activeVoiceCount() const;

private:
    struct Voice
    {
        bool active = false;
        VoiceId id = kInvalidVoice;
        ClipId clip = kInvalidClip;
        const AudioClip* data = nullptr;
        VoiceParams params;
        AudioOperator operators[kMaxOperatorsPerVoice];
        uint32_t operatorCount = 0;

        double position = 0.0;       // 片段内的帧位置（带小数）
        float gain = 0.0f;           // 当前包络值 0..1
        float fadeStep = 0.0f;       // 每帧的包络增量
        bool releasing = false;
        float lowpassState[2] = { 0.0f, 0.0f };
    };

    struct Tail
    {
        float samples[kTailFrames * 2] = {};
        uint32_t frames = 0;
        uint32_t cursor = 0;
    };

    Voice* acquireVoice(const VoiceParams& params, bool& stolen);
    void captureTail(const Voice& voice, uint32_t sampleRate);
    void freeVoice(Voice& voice);

    std::vector<std::shared_ptr<const AudioClip>> clips_;
    Voice voices_[kMaxVoices];
    Tail tails_[4];

    VoiceId nextVoiceId_ = 1;
    float masterVolume_ = 1.0f;
    MixStats stats_;
    uint32_t sampleRate_ = 48000;
};

// 混音器契约自检（不需要设备/窗口）：100 个事件 -> 最多 64 声部、抢占无爆音。
bool mixerSelfTest();
}
