#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audio
{
// 一段解码后的音频：交错存放的浮点采样，范围 [-1, 1]。
// 引擎内部所有混音都在 float 域完成，只在送设备前转一次格式。
struct AudioClip
{
    uint32_t sampleRate = 0;
    uint32_t channels = 0;          // 1 = 单声道，2 = 立体声
    std::vector<float> samples;     // 交错：L R L R ...

    bool valid() const
    {
        return sampleRate > 0 && channels > 0 && !samples.empty();
    }

    uint64_t frameCount() const
    {
        return channels > 0 ? static_cast<uint64_t>(samples.size()) / channels : 0;
    }

    float durationSeconds() const
    {
        return sampleRate > 0
                   ? static_cast<float>(frameCount()) / static_cast<float>(sampleRate)
                   : 0.0f;
    }
};

// 解析内存里的 WAV（RIFF/WAVE）。
// 支持 PCM 8/16/24/32 位与 IEEE float 32 位，单声道与立体声；
// 其它格式返回 false 并把原因写进 error（调用方负责打印）。
bool loadWavFromMemory(const void* data, std::size_t size, AudioClip& out, std::string& error);

// 生成一段正弦波：没有音频资源时也能听到东西的占位音，自检里也用它。
// 首尾各带 10ms 淡入淡出，避免片段边界产生咔哒声。
AudioClip makeSineClip(float hz, float seconds, uint32_t sampleRate = 48000, float gain = 0.25f);

// 编码成 16 位 PCM WAV。自检里用来验证解析器；将来导出音效也走这里。
std::vector<uint8_t> encodeWav16(const AudioClip& clip);
}
