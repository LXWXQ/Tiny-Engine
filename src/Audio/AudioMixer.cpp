#include "Audio/AudioMixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace audio
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;

// 等功率声像：pan=-1 全左，0 两边各 -3dB，+1 全右。
void panGains(float pan, float& left, float& right)
{
    const float clamped = std::clamp(pan, -1.0f, 1.0f);
    const float angle = (clamped + 1.0f) * 0.25f * kPi; // 0..pi/2
    left = std::cos(angle);
    right = std::sin(angle);
}

float onePoleAlpha(float cutoffHz, uint32_t sampleRate)
{
    if (cutoffHz <= 0.0f || sampleRate == 0)
        return 1.0f; // 关闭低通
    const float nyquist = 0.5f * static_cast<float>(sampleRate);
    const float clamped = std::min(cutoffHz, nyquist * 0.98f);
    return 1.0f - std::exp(-2.0f * kPi * clamped / static_cast<float>(sampleRate));
}

float readFrame(const AudioClip& clip, double position, uint32_t channel)
{
    const uint64_t frames = clip.frameCount();
    if (frames == 0)
        return 0.0f;

    const uint64_t index = static_cast<uint64_t>(position);
    const float fraction = static_cast<float>(position - static_cast<double>(index));
    const uint64_t next = (index + 1 < frames) ? index + 1 : (index < frames ? index : frames - 1);

    if (clip.channels == 1)
    {
        const float a = clip.samples[static_cast<std::size_t>(index)];
        const float b = clip.samples[static_cast<std::size_t>(next)];
        return a + (b - a) * fraction;
    }

    const uint32_t ch = channel < clip.channels ? channel : clip.channels - 1;
    const float a = clip.samples[static_cast<std::size_t>(index) * clip.channels + ch];
    const float b = clip.samples[static_cast<std::size_t>(next) * clip.channels + ch];
    return a + (b - a) * fraction;
}
} // namespace

AudioMixer::AudioMixer()
{
    stats_ = MixStats{};
}

ClipId AudioMixer::addClip(std::shared_ptr<const AudioClip> clip)
{
    if (!clip || !clip->valid())
        return kInvalidClip;
    for (uint32_t slot = 0; slot < kMaxClips; ++slot)
    {
        if (!clips_[slot])
        {
            clips_[slot] = std::move(clip);
            return slot;
        }
    }
    return kInvalidClip;
}

void AudioMixer::setClip(ClipId slot, std::shared_ptr<const AudioClip> clip)
{
    if (slot >= kMaxClips)
        return;
    clips_[slot] = std::move(clip);
}

const AudioClip* AudioMixer::clip(ClipId id) const
{
    if (id >= kMaxClips)
        return nullptr;
    return clips_[id].get();
}

bool AudioMixer::updateVoice(VoiceId id, const VoiceParams& params)
{
    if (id == kInvalidVoice)
        return false;
    for (Voice& voice : voices_)
    {
        if (!voice.active || voice.id != id)
            continue;
        voice.params.volume = params.volume;
        voice.params.pitch = params.pitch;
        voice.params.pan = params.pan;
        voice.params.lowpassHz = params.lowpassHz;
        return true;
    }
    return false;
}

void AudioMixer::setMasterVolume(float volume)
{
    masterVolume_ = std::clamp(volume, 0.0f, 4.0f);
}

void AudioMixer::freeVoice(Voice& voice)
{
    voice.active = false;
    voice.id = kInvalidVoice;
    voice.clip = kInvalidClip;
    voice.data = nullptr;
    voice.position = 0.0;
    voice.gain = 0.0f;
    voice.releasing = false;
    voice.operatorCount = 0;
    voice.lowpassState[0] = 0.0f;
    voice.lowpassState[1] = 0.0f;
}

void AudioMixer::captureTail(const Voice& voice, uint32_t sampleRate)
{
    // 找一个空尾巴槽；找不到就退化成"硬切"（宁可少一次交叉，也不动态分配）
    Tail* slot = nullptr;
    for (Tail& tail : tails_)
    {
        if (tail.frames == 0)
        {
            slot = &tail;
            break;
        }
    }
    if (slot == nullptr)
        return;

    Voice copy = voice;
    const uint32_t frames = std::min<uint32_t>(
        kTailFrames, std::max<uint32_t>(1, static_cast<uint32_t>(0.005f * sampleRate)));
    std::memset(slot->samples, 0, sizeof(float) * 2 * frames);

    // 把被抢占的声部按"当前状态 + 线性淡出"再渲染 5ms，接在新声部前面，
    // 这样波形在抢占点连续，听感上不会有咔哒。
    const float startGain = copy.gain;
    const float step = startGain / static_cast<float>(frames);
    for (uint32_t i = 0; i < frames; ++i)
    {
        float left = 0.0f;
        float right = 0.0f;
        if (copy.position < static_cast<double>(copy.data->frameCount()))
        {
            const float sampleL = readFrame(*copy.data, copy.position, 0);
            const float sampleR = readFrame(*copy.data, copy.position, 1);
            float panL = 0.0f;
            float panR = 0.0f;
            panGains(copy.params.pan, panL, panR);
            left = sampleL * panL;
            right = sampleR * panR;
            copy.position += static_cast<double>(copy.params.pitch) *
                             static_cast<double>(copy.data->sampleRate) /
                             static_cast<double>(sampleRate);
        }
        const float envelope = startGain - step * static_cast<float>(i);
        slot->samples[i * 2 + 0] = left * envelope * copy.params.volume;
        slot->samples[i * 2 + 1] = right * envelope * copy.params.volume;
    }
    slot->frames = frames;
    slot->cursor = 0;
}

AudioMixer::Voice* AudioMixer::acquireVoice(const VoiceParams& params, bool& stolen)
{
    stolen = false;

    for (Voice& voice : voices_)
    {
        if (!voice.active)
            return &voice;
    }

    // 池满：挑优先级最低、其次最轻的声部抢占
    Voice* victim = nullptr;
    float victimLoudness = 1e9f;
    for (Voice& voice : voices_)
    {
        const float loudness = voice.gain * voice.params.volume;
        if (victim == nullptr || voice.params.priority < victim->params.priority ||
            (voice.params.priority == victim->params.priority && loudness < victimLoudness))
        {
            victim = &voice;
            victimLoudness = loudness;
        }
    }

    if (victim == nullptr)
        return nullptr;

    // 新来的优先级更低：不抢，直接丢弃（比"抢掉更重要的声音"更符合预期）
    if (params.priority < victim->params.priority)
        return nullptr;

    captureTail(*victim, sampleRate_);
    freeVoice(*victim);
    stolen = true;
    return victim;
}

VoiceId AudioMixer::play(ClipId clipId, const VoiceParams& params)
{
    const AudioClip* data = clip(clipId);
    if (data == nullptr)
        return kInvalidVoice;

    bool stolen = false;
    Voice* voice = acquireVoice(params, stolen);
    stats_.playedEvents += 1;
    if (voice == nullptr)
    {
        stats_.dropped += 1;
        return kInvalidVoice;
    }
    if (stolen)
        stats_.stolen += 1;

    *voice = Voice{};
    voice->active = true;
    voice->id = nextVoiceId_++;
    if (nextVoiceId_ == kInvalidVoice)
        nextVoiceId_ = 1;
    voice->clip = clipId;
    voice->data = data;
    voice->params = params;
    voice->params.volume = std::max(0.0f, params.volume);
    voice->position = 0.0;
    voice->gain = 0.0f;
    voice->releasing = false;

    const uint32_t fadeInFrames = std::max<uint32_t>(
        1, static_cast<uint32_t>(std::max(0.0f, params.fadeInSeconds) * static_cast<float>(sampleRate_)));
    voice->fadeStep = 1.0f / static_cast<float>(fadeInFrames);

    // 算子链：Pitch -> Lowpass -> Gain -> Fade，顺序固定，便于文档与调试。
    voice->operatorCount = 0;
    voice->operators[voice->operatorCount++] = { AudioOperatorKind::Pitch, params.pitch, 0.0f };
    voice->operators[voice->operatorCount++] = { AudioOperatorKind::Lowpass, params.lowpassHz, 0.0f };
    voice->operators[voice->operatorCount++] = { AudioOperatorKind::Gain, voice->params.volume, 0.0f };
    voice->operators[voice->operatorCount++] = { AudioOperatorKind::Fade, params.fadeInSeconds,
                                                 params.fadeOutSeconds };

    return voice->id;
}

void AudioMixer::stop(VoiceId id)
{
    for (Voice& voice : voices_)
    {
        if (voice.active && voice.id == id && !voice.releasing)
        {
            voice.releasing = true;
            const uint32_t fadeFrames = std::max<uint32_t>(
                1, static_cast<uint32_t>(std::max(0.0f, voice.params.fadeOutSeconds) *
                                         static_cast<float>(sampleRate_)));
            voice.fadeStep = voice.gain / static_cast<float>(fadeFrames);
            return;
        }
    }
}

void AudioMixer::stopAll()
{
    for (Voice& voice : voices_)
    {
        if (voice.active)
        {
            voice.releasing = true;
            const uint32_t fadeFrames = std::max<uint32_t>(
                1, static_cast<uint32_t>(std::max(0.0f, voice.params.fadeOutSeconds) *
                                         static_cast<float>(sampleRate_)));
            voice.fadeStep = voice.gain / static_cast<float>(fadeFrames);
        }
    }
}

void AudioMixer::render(float* out, uint32_t frames, uint32_t sampleRate)
{
    sampleRate_ = sampleRate == 0 ? 48000 : sampleRate;
    std::memset(out, 0, sizeof(float) * 2 * frames);

    // 1) 先混抢占留下的淡出尾巴
    for (Tail& tail : tails_)
    {
        if (tail.frames == 0)
            continue;
        for (uint32_t i = 0; i < frames && tail.cursor < tail.frames; ++i, ++tail.cursor)
        {
            out[i * 2 + 0] += tail.samples[tail.cursor * 2 + 0];
            out[i * 2 + 1] += tail.samples[tail.cursor * 2 + 1];
        }
        if (tail.cursor >= tail.frames)
            tail.frames = 0;
    }

    // 2) 再混活动声部
    float peak = 0.0f;
    for (Voice& voice : voices_)
    {
        if (!voice.active)
            continue;

        const float panL = [&] { float l, r; panGains(voice.params.pan, l, r); return l; }();
        const float panR = [&] { float l, r; panGains(voice.params.pan, l, r); return r; }();
        const float alpha = onePoleAlpha(voice.params.lowpassHz, sampleRate_);
        const uint64_t clipFrames = voice.data->frameCount();
        const double increment = static_cast<double>(voice.params.pitch) *
                                 static_cast<double>(voice.data->sampleRate) /
                                 static_cast<double>(sampleRate_);

        for (uint32_t i = 0; i < frames; ++i)
        {
            if (voice.releasing)
            {
                voice.gain -= voice.fadeStep;
                if (voice.gain <= 0.0f)
                {
                    freeVoice(voice);
                    stats_.finished += 1;
                    break;
                }
            }
            else if (voice.gain < 1.0f)
            {
                voice.gain = std::min(1.0f, voice.gain + voice.fadeStep);
            }

            if (!voice.params.loop && voice.position >= static_cast<double>(clipFrames))
            {
                // 片段已经放完：第一次进入时启动淡出，之后只走包络、不再读采样。
                // 这里必须独立判断并 continue，否则释放阶段会拿着越界的 position 去读采样。
                if (!voice.releasing)
                {
                    voice.releasing = true;
                    const uint32_t fadeFrames = std::max<uint32_t>(
                        1, static_cast<uint32_t>(std::max(0.0f, voice.params.fadeOutSeconds) *
                                                 static_cast<float>(sampleRate_)));
                    voice.fadeStep = voice.gain / static_cast<float>(fadeFrames);
                }
                continue;
            }

            float sampleL = readFrame(*voice.data, voice.position, 0);
            float sampleR = readFrame(*voice.data, voice.position, 1);

            voice.lowpassState[0] += alpha * (sampleL - voice.lowpassState[0]);
            voice.lowpassState[1] += alpha * (sampleR - voice.lowpassState[1]);
            sampleL = voice.lowpassState[0];
            sampleR = voice.lowpassState[1];

            const float envelope = voice.gain * voice.params.volume * masterVolume_;
            out[i * 2 + 0] += sampleL * envelope * panL;
            out[i * 2 + 1] += sampleR * envelope * panR;

            voice.position += increment;
            if (voice.params.loop && clipFrames > 0)
            {
                while (voice.position >= static_cast<double>(clipFrames))
                    voice.position -= static_cast<double>(clipFrames);
            }
        }
    }

    for (uint32_t i = 0; i < frames * 2; ++i)
    {
        float value = out[i];
        if (value > 1.0f)
            value = 1.0f;
        else if (value < -1.0f)
            value = -1.0f;
        out[i] = value;
        peak = std::max(peak, std::fabs(value));
    }

    stats_.peak = peak;
    stats_.activeVoices = activeVoiceCount();
}

uint32_t AudioMixer::activeVoiceCount() const
{
    uint32_t count = 0;
    for (const Voice& voice : voices_)
        if (voice.active)
            ++count;
    return count;
}

MixStats AudioMixer::stats() const
{
    return stats_;
}

namespace
{
uint64_t hashBuffer(const std::vector<float>& samples)
{
    uint64_t hash = 1469598103934665603ull;
    for (float value : samples)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        hash ^= bits;
        hash *= 1099511628211ull;
    }
    return hash;
}
} // namespace

bool mixerSelfTest()
{
    bool ok = true;
    auto expect = [&ok](bool condition, const char* message)
    {
        if (!condition)
        {
            ok = false;
            std::cerr << "[Audio] mixerSelfTest FAIL: " << message << "\n";
        }
    };

    const uint32_t rate = 48000;
    std::vector<float> buffer(8192 * 2);

    // 1) 100 个事件 -> 最多 64 声部，其余靠抢占
    {
        AudioMixer mixer;
        auto clip = std::make_shared<AudioClip>(makeSineClip(440.0f, 1.0f, rate, 0.2f));
        const ClipId id = mixer.addClip(clip);
        expect(id != kInvalidClip, "addClip 返回无效 id");

        VoiceParams params;
        params.fadeInSeconds = 0.0f;
        for (int i = 0; i < 100; ++i)
            mixer.play(id, params);

        expect(mixer.activeVoiceCount() == kMaxVoices, "100 个事件没有把 64 个声部填满");
        expect(mixer.stats().stolen == 36, "抢占次数不是 36");
        expect(mixer.stats().playedEvents == 100, "事件计数不是 100");
    }

    // 2) 优先级保护：低优先级不许抢高优先级
    {
        AudioMixer mixer;
        auto clip = std::make_shared<AudioClip>(makeSineClip(440.0f, 1.0f, rate, 0.2f));
        const ClipId id = mixer.addClip(clip);

        VoiceParams high;
        high.priority = 10;
        high.fadeInSeconds = 0.0f;
        for (uint32_t i = 0; i < kMaxVoices; ++i)
            mixer.play(id, high);

        VoiceParams low;
        low.priority = 1;
        const VoiceId dropped = mixer.play(id, low);
        expect(dropped == kInvalidVoice, "低优先级请求不该抢走高优先级声部");
        expect(mixer.stats().dropped == 1, "dropped 计数不是 1");
        expect(mixer.activeVoiceCount() == kMaxVoices, "池应仍为满");
    }

    // 3) 抢占无爆音：连续采样差不超过阈值
    {
        AudioMixer mixer;
        // 幅度刻意压到 0.002：64 个声部叠加也不会削波，这样"跳变"只可能来自抢占本身。
        auto clipA = std::make_shared<AudioClip>(makeSineClip(200.0f, 1.0f, rate, 0.002f));
        auto clipB = std::make_shared<AudioClip>(makeSineClip(300.0f, 1.0f, rate, 0.002f));
        const ClipId a = mixer.addClip(clipA);
        const ClipId b = mixer.addClip(clipB);

        VoiceParams params;
        params.volume = 1.0f;
        params.fadeInSeconds = 0.0f;
        for (uint32_t i = 0; i < kMaxVoices; ++i)
            mixer.play(a, params);
        mixer.render(buffer.data(), 4096, rate);

        float previous = buffer[8191];

        // 先量一段"没有抢占"的自然斜率，再和抢占发生时比：硬切会明显超过自然斜率。
        float baselineDelta = 0.0f;
        for (int chunk = 0; chunk < 5; ++chunk)
        {
            mixer.render(buffer.data(), 256, rate);
            for (uint32_t i = 0; i < 512; ++i)
            {
                const float value = buffer[i];
                baselineDelta = std::max(baselineDelta, std::fabs(value - previous));
                previous = value;
            }
        }

        mixer.play(b, params); // 池已满，这里必然发生一次抢占
        float stealDelta = 0.0f;
        for (int chunk = 0; chunk < 15; ++chunk)
        {
            mixer.render(buffer.data(), 256, rate);
            for (uint32_t i = 0; i < 512; ++i)
            {
                const float value = buffer[i];
                stealDelta = std::max(stealDelta, std::fabs(value - previous));
                previous = value;
            }
        }

        expect(mixer.stats().stolen == 1, "用例 3 没有真的发生抢占");
        expect(baselineDelta > 0.0f, "用例 3 的基线斜率为 0，信号可能没出声");
        expect(stealDelta <= baselineDelta * 1.5f, "抢占点出现明显跳变（可能爆音）");

    }

    // 4) 64 声部叠加后峰值必须被限幅在 1.0
    {
        AudioMixer mixer;
        auto clip = std::make_shared<AudioClip>(makeSineClip(440.0f, 1.0f, rate, 1.0f));
        const ClipId id = mixer.addClip(clip);
        VoiceParams params;
        params.volume = 0.5f;
        params.fadeInSeconds = 0.0f;
        for (uint32_t i = 0; i < kMaxVoices; ++i)
            mixer.play(id, params);
        mixer.render(buffer.data(), 2048, rate);
        expect(mixer.stats().peak <= 1.0f, "输出峰值超过 1.0（没有限幅）");
        expect(mixer.stats().peak > 0.9f, "64 个满幅声部叠加后峰值过低，混音可能没接上");
    }

    // 5) loop：0.1s 片段循环 1s 后仍然在响
    {
        AudioMixer mixer;
        auto clip = std::make_shared<AudioClip>(makeSineClip(440.0f, 0.1f, rate, 0.3f));
        const ClipId id = mixer.addClip(clip);
        VoiceParams params;
        params.loop = true;
        mixer.play(id, params);
        for (int i = 0; i < 40; ++i)
            mixer.render(buffer.data(), 1200, rate); // 40 * 25ms = 1s
        expect(mixer.activeVoiceCount() == 1, "loop 声部不应该结束");
    }

    // 6) 非 loop：0.1s 片段 1s 后自然结束并计入 finished
    {
        AudioMixer mixer;
        auto clip = std::make_shared<AudioClip>(makeSineClip(440.0f, 0.1f, rate, 0.3f));
        const ClipId id = mixer.addClip(clip);
        VoiceParams params;
        mixer.play(id, params);
        for (int i = 0; i < 40; ++i)
            mixer.render(buffer.data(), 1200, rate);
        expect(mixer.activeVoiceCount() == 0, "非 loop 声部应该已经结束");
        expect(mixer.stats().finished == 1, "finished 计数不是 1");
    }

    // 7) pitch=2.0：0.1s 片段约 0.05s 放完
    {
        AudioMixer mixer;
        auto clip = std::make_shared<AudioClip>(makeSineClip(440.0f, 0.1f, rate, 0.3f));
        const ClipId id = mixer.addClip(clip);
        VoiceParams params;
        params.pitch = 2.0f;
        mixer.play(id, params);
        mixer.render(buffer.data(), static_cast<uint32_t>(0.03f * rate), rate);
        expect(mixer.activeVoiceCount() == 1, "pitch=2 时 0.03s 处不该结束");
        mixer.render(buffer.data(), static_cast<uint32_t>(0.05f * rate), rate);
        expect(mixer.activeVoiceCount() == 0, "pitch=2 时 0.08s 处应该已经结束");
    }

    // 8) stop 走淡出：不是立刻断，淡完才释放
    {
        AudioMixer mixer;
        auto clip = std::make_shared<AudioClip>(makeSineClip(440.0f, 2.0f, rate, 0.5f));
        const ClipId id = mixer.addClip(clip);
        VoiceParams params;
        params.fadeOutSeconds = 0.02f;
        const VoiceId voice = mixer.play(id, params);
        mixer.render(buffer.data(), 2400, rate);
        mixer.stop(voice);
        expect(mixer.activeVoiceCount() == 1, "stop 之后立刻就被释放了，没有淡出");
        mixer.render(buffer.data(), static_cast<uint32_t>(0.03f * rate), rate);
        expect(mixer.activeVoiceCount() == 0, "淡出结束后声部没有释放");
    }

    // 9) WAV 编码/解析往返
    {
        const AudioClip source = makeSineClip(1000.0f, 0.05f, rate, 0.5f);
        const std::vector<uint8_t> wav = encodeWav16(source);
        expect(!wav.empty(), "encodeWav16 输出为空");

        AudioClip parsed;
        std::string error;
        const bool loaded = loadWavFromMemory(wav.data(), wav.size(), parsed, error);
        expect(loaded, error.empty() ? "loadWavFromMemory 失败" : error.c_str());
        expect(parsed.sampleRate == source.sampleRate && parsed.channels == source.channels,
               "往返后采样率/声道数不一致");
        expect(parsed.samples.size() == source.samples.size(), "往返后采样数不一致");

        float maxDiff = 0.0f;
        if (parsed.samples.size() == source.samples.size())
        {
            for (std::size_t i = 0; i < parsed.samples.size(); ++i)
                maxDiff = std::max(maxDiff, std::fabs(parsed.samples[i] - source.samples[i]));
        }
        expect(maxDiff <= 1.0f / 32000.0f, "16 位往返误差过大");
    }

    // 10) 确定性：同一串事件两次渲染逐位相同
    {
        auto run = [&](uint64_t& hash)
        {
            AudioMixer mixer;
            auto clipA = std::make_shared<AudioClip>(makeSineClip(220.0f, 0.5f, rate, 0.4f));
            auto clipB = std::make_shared<AudioClip>(makeSineClip(660.0f, 0.5f, rate, 0.4f));
            const ClipId a = mixer.addClip(clipA);
            const ClipId b = mixer.addClip(clipB);
            std::vector<float> out(512 * 2);

            VoiceParams params;
            for (int i = 0; i < 6; ++i)
            {
                params.pan = -1.0f + 0.4f * static_cast<float>(i);
                params.volume = 0.3f;
                mixer.play(i % 2 == 0 ? a : b, params);
            }
            for (int i = 0; i < 40; ++i)
                mixer.render(out.data(), 512, rate);
            hash = hashBuffer(out);
        };

        uint64_t first = 0;
        uint64_t second = 0;
        run(first);
        run(second);
        expect(first == second, "两次相同事件的渲染结果不一致");
    }

    if (ok)
        std::cout << "[Audio] mixerSelfTest PASS | cases=10 voices=" << kMaxVoices
                  << " stolen=36 dropped=1 no-click=ok\n";
    return ok;
}
}
