#pragma once

#include <glm/glm.hpp>

namespace audio
{
// 听者：位置 + 朝向。相机实体的世界矩阵列直接能填出来。
struct AudioListener
{
    glm::vec3 position{ 0.0f };
    glm::vec3 forward{ 0.0f, 0.0f, -1.0f };
    glm::vec3 up{ 0.0f, 1.0f, 0.0f };
    float gain = 1.0f;
};

// 距离衰减设置。minDistance 内不衰减，maxDistance 外听不见。
struct AudioSpatialSettings
{
    float minDistance = 1.0f;
    float maxDistance = 30.0f;
    float rolloff = 1.0f;      // 1 = 反比衰减；越大衰减越快
};

struct AudioSpatialParams
{
    float volume = 1.0f;
    float pan = 0.0f;          // -1 全左，+1 全右
    float lowpassHz = 0.0f;    // 0 = 不滤；远处闷声
    float distance = 0.0f;
    bool audible = true;
};

// 纯函数：算"这个音源相对听者应该怎么响"。不碰设备，所以能无设备自检。
AudioSpatialParams computeSpatialParams(const AudioListener& listener,
                                        const glm::vec3& sourcePosition,
                                        float baseVolume,
                                        const AudioSpatialSettings& settings);

bool audioSpatialSelfTest();
} // namespace audio
