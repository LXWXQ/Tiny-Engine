#include "Audio/AudioSpatial.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace audio
{
namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << "[Audio] spatialSelfTest FAIL: " << message << "\n";
    return false;
}

bool near(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance;
}
} // namespace

AudioSpatialParams computeSpatialParams(const AudioListener& listener,
                                        const glm::vec3& sourcePosition,
                                        float baseVolume,
                                        const AudioSpatialSettings& settings)
{
    AudioSpatialParams result;

    const float minDistance = std::max(settings.minDistance, 0.01f);
    const float maxDistance = std::max(settings.maxDistance, minDistance + 0.01f);
    const float rolloff = std::max(settings.rolloff, 0.01f);

    const glm::vec3 delta = sourcePosition - listener.position;
    const float distance = glm::length(delta);
    result.distance = distance;

    if (distance >= maxDistance)
    {
        result.audible = false;
        result.volume = 0.0f;
        return result;
    }

    // 反比衰减：minDistance 以内是 1，之后按 min/distance 衰减，rolloff 拉陡。
    const float inverse = minDistance / std::max(distance, minDistance);
    float attenuation = std::pow(std::clamp(inverse, 0.0f, 1.0f), rolloff);

    // 靠近 maxDistance 时再乘一段线性淡出，避免"走到边界突然静音"。
    const float fadeBand = std::max(maxDistance * 0.1f, 0.01f);
    const float edgeFade = std::clamp((maxDistance - distance) / fadeBand, 0.0f, 1.0f);
    attenuation *= edgeFade;

    result.volume = std::max(0.0f, baseVolume * attenuation * std::max(listener.gain, 0.0f));

    if (distance > 1.0e-5f)
    {
        const glm::vec3 direction = delta / distance;
        glm::vec3 forward = listener.forward;
        if (glm::length(forward) < 1.0e-5f)
            forward = glm::vec3(0.0f, 0.0f, -1.0f);
        forward = glm::normalize(forward);

        glm::vec3 up = listener.up;
        if (glm::length(up) < 1.0e-5f)
            up = glm::vec3(0.0f, 1.0f, 0.0f);
        up = glm::normalize(up);

        glm::vec3 right = glm::cross(forward, up);
        if (glm::length(right) < 1.0e-5f)
            right = glm::vec3(1.0f, 0.0f, 0.0f);
        right = glm::normalize(right);

        result.pan = std::clamp(glm::dot(direction, right), -1.0f, 1.0f);
    }

    // 远处的声音高频先没：衰减到 1 以下才开始滤，越远越闷（下限 600Hz）。
    if (attenuation < 0.99f)
        result.lowpassHz = std::max(600.0f, 20000.0f * attenuation);

    return result;
}

bool audioSpatialSelfTest()
{
    bool ok = true;

    AudioListener listener;
    listener.position = glm::vec3(0.0f);
    listener.forward = glm::vec3(0.0f, 0.0f, -1.0f);
    listener.up = glm::vec3(0.0f, 1.0f, 0.0f);

    AudioSpatialSettings settings;   // min 1, max 30, rolloff 1

    // 1) 音源就在听者身上：满音量、居中、不滤、无 NaN。
    {
        const AudioSpatialParams p = computeSpatialParams(listener, glm::vec3(0.0f), 0.8f, settings);
        ok = expect(p.audible, "at listener should be audible") && ok;
        ok = expect(near(p.volume, 0.8f, 1.0e-4f), "at listener should keep base volume") && ok;
        ok = expect(near(p.pan, 0.0f, 1.0e-4f), "at listener should be centered") && ok;
        ok = expect(near(p.lowpassHz, 0.0f, 1.0e-4f), "at listener should not be filtered") && ok;
        ok = expect(std::isfinite(p.volume) && std::isfinite(p.pan), "at listener must be finite") && ok;
    }

    // 2) 正前方 5m：反比衰减到 1/5。
    {
        const AudioSpatialParams p = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -5.0f), 1.0f, settings);
        ok = expect(near(p.distance, 5.0f, 1.0e-3f), "distance should be 5") && ok;
        ok = expect(near(p.volume, 0.2f, 5.0e-3f), "inverse distance should give 1/5") && ok;
        ok = expect(near(p.pan, 0.0f, 1.0e-3f), "front source should be centered") && ok;
    }

    // 3) 正右方 / 正左方：声像符号与幅度。
    {
        const AudioSpatialParams right = computeSpatialParams(listener, glm::vec3(5.0f, 0.0f, 0.0f), 1.0f, settings);
        const AudioSpatialParams left = computeSpatialParams(listener, glm::vec3(-5.0f, 0.0f, 0.0f), 1.0f, settings);
        ok = expect(right.pan > 0.95f, "source on the right should pan right") && ok;
        ok = expect(left.pan < -0.95f, "source on the left should pan left") && ok;
    }

    // 4) 超出 maxDistance：听不见。
    {
        const AudioSpatialParams p = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -40.0f), 1.0f, settings);
        ok = expect(!p.audible, "beyond maxDistance should be inaudible") && ok;
        ok = expect(near(p.volume, 0.0f, 1.0e-6f), "beyond maxDistance should be silent") && ok;
    }

    // 5) 单调：越远越轻。
    {
        const float near5 = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -5.0f), 1.0f, settings).volume;
        const float far15 = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -15.0f), 1.0f, settings).volume;
        const float far25 = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -25.0f), 1.0f, settings).volume;
        ok = expect(near5 > far15 && far15 > far25, "volume must decrease with distance") && ok;
    }

    // 6) rolloff 越大，同一距离越轻。
    {
        AudioSpatialSettings steep = settings;
        steep.rolloff = 2.0f;
        const float normal = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -10.0f), 1.0f, settings).volume;
        const float steeper = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -10.0f), 1.0f, steep).volume;
        ok = expect(steeper < normal, "higher rolloff should be quieter") && ok;
    }

    // 7) 远处低通更狠，但不会低于 600Hz。
    {
        const float nearCut = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -3.0f), 1.0f, settings).lowpassHz;
        const float farCut = computeSpatialParams(listener, glm::vec3(0.0f, 0.0f, -20.0f), 1.0f, settings).lowpassHz;
        ok = expect(farCut < nearCut, "farther source should be more muffled") && ok;
        ok = expect(farCut >= 600.0f, "cutoff should clamp at 600Hz") && ok;
    }

    // 8) 退化输入（听者方向为零、minDistance 非法）不能出 NaN。
    {
        AudioListener degenerate;
        degenerate.forward = glm::vec3(0.0f);
        degenerate.up = glm::vec3(0.0f);
        AudioSpatialSettings bad;
        bad.minDistance = -3.0f;
        bad.maxDistance = 0.0f;
        const AudioSpatialParams p = computeSpatialParams(degenerate, glm::vec3(1.0f, 0.0f, 0.0f), 1.0f, bad);
        ok = expect(std::isfinite(p.volume) && std::isfinite(p.pan) && std::isfinite(p.lowpassHz),
                    "degenerate input must stay finite") && ok;
    }

    if (ok)
        std::cout << "[Audio] spatialSelfTest PASS | cases=8 pan=ok rolloff=ok cutoff=ok\n";
    return ok;
}
} // namespace audio
