#include "Audio/AudioClip.h"

#include <cmath>
#include <cstring>

namespace audio
{
namespace
{
uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t readU32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float readF32(const uint8_t* p)
{
    const uint32_t bits = readU32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void appendU16(std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void appendU32(std::vector<uint8_t>& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

void appendTag(std::vector<uint8_t>& out, const char* tag)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(tag[i]));
}
} // namespace

bool loadWavFromMemory(const void* data, std::size_t size, AudioClip& out, std::string& error)
{
    out = AudioClip{};
    error.clear();

    if (data == nullptr || size < 44)
    {
        error = "WAV 数据太短";
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0)
    {
        error = "不是 RIFF/WAVE 文件";
        return false;
    }

    uint16_t formatTag = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    const uint8_t* sampleData = nullptr;
    uint32_t sampleBytes = 0;

    std::size_t offset = 12;
    while (offset + 8 <= size)
    {
        const uint8_t* chunk = bytes + offset;
        const uint32_t chunkSize = readU32(chunk + 4);
        const std::size_t payload = offset + 8;
        if (payload + chunkSize > size)
            break; // 截断的文件：能解析多少算多少

        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16)
        {
            formatTag = readU16(bytes + payload);
            channels = readU16(bytes + payload + 2);
            sampleRate = readU32(bytes + payload + 4);
            bitsPerSample = readU16(bytes + payload + 14);
            // WAVE_FORMAT_EXTENSIBLE：真正的格式在 SubFormat GUID 的前两个字节
            if (formatTag == 0xFFFE && chunkSize >= 40)
                formatTag = readU16(bytes + payload + 24);
        }
        else if (std::memcmp(chunk, "data", 4) == 0)
        {
            sampleData = bytes + payload;
            sampleBytes = chunkSize;
        }

        offset = payload + chunkSize + (chunkSize & 1u); // 块按偶数字节对齐
    }

    if (sampleData == nullptr || channels == 0 || sampleRate == 0 || bitsPerSample == 0)
    {
        error = "缺少 fmt 或 data 块";
        return false;
    }
    if (channels > 2)
    {
        error = "只支持单声道与立体声";
        return false;
    }

    const bool isFloat = (formatTag == 3);
    if (!isFloat && formatTag != 1)
    {
        error = "只支持 PCM 与 IEEE float";
        return false;
    }

    const uint32_t bytesPerSample = bitsPerSample / 8u;
    if (bytesPerSample == 0 || (bitsPerSample != 8 && bitsPerSample != 16 && bitsPerSample != 24 &&
                                bitsPerSample != 32))
    {
        error = "不支持的位深";
        return false;
    }
    if (isFloat && bitsPerSample != 32)
    {
        error = "只支持 32 位 float";
        return false;
    }

    const uint64_t frames = sampleBytes / (bytesPerSample * channels);
    out.sampleRate = sampleRate;
    out.channels = channels;
    out.samples.resize(static_cast<std::size_t>(frames) * channels);

    const uint8_t* cursor = sampleData;
    for (std::size_t i = 0; i < out.samples.size(); ++i, cursor += bytesPerSample)
    {
        float value = 0.0f;
        if (isFloat)
        {
            value = readF32(cursor);
        }
        else if (bitsPerSample == 8)
        {
            // 8 位 PCM 是无符号的，中点 128
            value = (static_cast<int>(cursor[0]) - 128) / 128.0f;
        }
        else if (bitsPerSample == 16)
        {
            const int16_t s = static_cast<int16_t>(readU16(cursor));
            value = static_cast<float>(s) / 32768.0f;
        }
        else if (bitsPerSample == 24)
        {
            int32_t s = static_cast<int32_t>(cursor[0]) | (static_cast<int32_t>(cursor[1]) << 8) |
                        (static_cast<int32_t>(cursor[2]) << 16);
            if ((s & 0x00800000) != 0)
                s |= ~0x00FFFFFF; // 符号扩展
            value = static_cast<float>(s) / 8388608.0f;
        }
        else
        {
            const int32_t s = static_cast<int32_t>(readU32(cursor));
            value = static_cast<float>(static_cast<double>(s) / 2147483648.0);
        }
        out.samples[i] = value;
    }

    if (out.samples.empty())
    {
        error = "data 块为空";
        return false;
    }
    return true;
}

AudioClip makeSineClip(float hz, float seconds, uint32_t sampleRate, float gain)
{
    AudioClip clip;
    clip.sampleRate = sampleRate;
    clip.channels = 2;

    const uint32_t frames = static_cast<uint32_t>(seconds * static_cast<float>(sampleRate));
    clip.samples.resize(static_cast<std::size_t>(frames) * 2);

    const uint32_t fadeFrames = static_cast<uint32_t>(0.01f * static_cast<float>(sampleRate));
    const float step = 6.283185307179586f * hz / static_cast<float>(sampleRate);
    for (uint32_t i = 0; i < frames; ++i)
    {
        float envelope = 1.0f;
        if (i < fadeFrames)
            envelope = static_cast<float>(i) / static_cast<float>(fadeFrames);
        else if (i + fadeFrames >= frames)
            envelope = static_cast<float>(frames - i) / static_cast<float>(fadeFrames);

        const float value = std::sin(step * static_cast<float>(i)) * gain * envelope;
        clip.samples[i * 2 + 0] = value;
        clip.samples[i * 2 + 1] = value;
    }
    return clip;
}

std::vector<uint8_t> encodeWav16(const AudioClip& clip)
{
    std::vector<uint8_t> out;
    if (!clip.valid())
        return out;

    const uint32_t channels = clip.channels;
    const uint32_t sampleRate = clip.sampleRate;
    const uint16_t bitsPerSample = 16;
    const uint32_t dataBytes = static_cast<uint32_t>(clip.samples.size() * sizeof(int16_t));

    out.reserve(44 + dataBytes);
    appendTag(out, "RIFF");
    appendU32(out, 36 + dataBytes);
    appendTag(out, "WAVE");
    appendTag(out, "fmt ");
    appendU32(out, 16);
    appendU16(out, 1); // PCM
    appendU16(out, static_cast<uint16_t>(channels));
    appendU32(out, sampleRate);
    appendU32(out, sampleRate * channels * bitsPerSample / 8);
    appendU16(out, static_cast<uint16_t>(channels * bitsPerSample / 8));
    appendU16(out, bitsPerSample);
    appendTag(out, "data");
    appendU32(out, dataBytes);

    for (float sample : clip.samples)
    {
        float clamped = sample;
        if (clamped > 1.0f)
            clamped = 1.0f;
        if (clamped < -1.0f)
            clamped = -1.0f;
        const int16_t value = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        appendU16(out, static_cast<uint16_t>(value));
    }
    return out;
}
}
