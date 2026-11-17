#include "Scene/SkyCamera.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace scene
{
namespace
{

constexpr float kDefaultSkyScale = 0.0625f;
constexpr float kMinSkyScale = 1.0e-4f;

bool expect(bool condition, const char* message)
{
    if (!condition)
        std::cerr << "[Sky] selfTest FAIL: " << message << '\n';
    return condition;
}

bool nearVec(const glm::vec3& a, const glm::vec3& b, float epsilon = 1.0e-4f)
{
    return std::fabs(a.x - b.x) <= epsilon && std::fabs(a.y - b.y) <= epsilon &&
        std::fabs(a.z - b.z) <= epsilon;
}

glm::vec3 sanitizeOrigin(const glm::vec3& origin)
{
    const bool finite = std::isfinite(origin.x) && std::isfinite(origin.y) &&
        std::isfinite(origin.z);
    return finite ? origin : glm::vec3(0.0f);
}

float sanitizeScale(float scale)
{
    if (!std::isfinite(scale) || scale <= kMinSkyScale)
        return kDefaultSkyScale;
    return scale;
}

} // namespace

glm::mat4 skyCameraMatrix(const SkyCameraSettings& settings, const glm::vec3& cameraPosition)
{
    const glm::vec3 origin = sanitizeOrigin(settings.origin);
    const float scale = sanitizeScale(settings.scale);
    const float inverseScale = 1.0f / scale;

    const glm::mat4 toOrigin = glm::translate(glm::mat4(1.0f), -origin);
    const glm::mat4 enlarge = glm::scale(glm::mat4(1.0f), glm::vec3(inverseScale));
    const glm::mat4 toCamera = glm::translate(glm::mat4(1.0f), cameraPosition);
    return toCamera * enlarge * toOrigin;
}

glm::mat4 skyEntityMatrix(const SkyCameraSettings& settings, const glm::vec3& cameraPosition,
                          const glm::mat4& worldMatrix)
{
    return skyCameraMatrix(settings, cameraPosition) * worldMatrix;
}

bool skyCameraSelfTest()
{
    bool ok = true;

    SkyCameraSettings settings;
    const glm::vec3 camera(3.0f, 4.0f, -5.0f);

    // 1) origin 正好映射到相机位置：天空世界的原点就是玩家脚下。
    {
        const glm::mat4 m = skyCameraMatrix(settings, camera);
        const glm::vec3 mapped = glm::vec3(m * glm::vec4(settings.origin, 1.0f));
        ok &= expect(nearVec(mapped, camera), "origin must map to the camera position");
    }

    // 2) 默认 1/16：离 origin 一个单位，渲染在 16 个单位外。
    {
        const glm::mat4 m = skyCameraMatrix(settings, camera);
        const glm::vec3 point = settings.origin + glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 mapped = glm::vec3(m * glm::vec4(point, 1.0f));
        ok &= expect(nearVec(mapped, camera + glm::vec3(16.0f, 0.0f, 0.0f)),
                     "1/16 scale must enlarge one unit to sixteen");
    }

    // 3) 换一个比例：1/2 放大成 2 倍。
    {
        SkyCameraSettings half = settings;
        half.scale = 0.5f;
        const glm::mat4 m = skyCameraMatrix(half, camera);
        const glm::vec3 point = half.origin + glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::vec3 mapped = glm::vec3(m * glm::vec4(point, 1.0f));
        ok &= expect(nearVec(mapped, camera + glm::vec3(0.0f, 2.0f, 0.0f)),
                     "half scale must enlarge one unit to two");
    }

    // 4) 相机平移时整块天空跟着平移（没有视差、不会走近）。
    {
        const glm::mat4 a = skyCameraMatrix(settings, camera);
        const glm::mat4 b = skyCameraMatrix(settings, camera + glm::vec3(10.0f, 0.0f, 0.0f));
        const glm::vec3 point = settings.origin + glm::vec3(2.0f, 1.0f, 0.5f);
        const glm::vec3 mappedA = glm::vec3(a * glm::vec4(point, 1.0f));
        const glm::vec3 mappedB = glm::vec3(b * glm::vec4(point, 1.0f));
        ok &= expect(nearVec(mappedB - mappedA, glm::vec3(10.0f, 0.0f, 0.0f)),
                     "moving the camera must translate the sky by the same amount");
    }

    // 5) 退化输入（0 / 负数 / NaN）回退默认比例，结果保持有限。
    {
        SkyCameraSettings broken = settings;
        broken.scale = 0.0f;
        broken.origin = glm::vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
        const glm::mat4 m = skyCameraMatrix(broken, camera);
        const glm::vec3 mapped = glm::vec3(m * glm::vec4(glm::vec3(1.0f, 2.0f, 3.0f), 1.0f));
        const bool finite = std::isfinite(mapped.x) && std::isfinite(mapped.y) && std::isfinite(mapped.z);
        ok &= expect(finite, "degenerate settings must stay finite");

        const glm::vec3 originPoint = glm::vec3(m * glm::vec4(glm::vec3(0.0f), 1.0f));
        ok &= expect(nearVec(originPoint, camera), "degenerate origin must fall back to zero");
    }

    // 6) 实体矩阵：天空实体自身的世界矩阵先作用，再套相机锁定。
    {
        const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        const glm::mat4 m = skyEntityMatrix(settings, camera, world);
        const glm::vec3 originPoint = glm::vec3(m * glm::vec4(settings.origin, 1.0f));
        ok &= expect(nearVec(originPoint, camera + glm::vec3(0.0f, 0.0f, 16.0f), 1.0e-3f),
                     "entity matrix must apply the world transform before the sky lock");

        const glm::mat3 normalMatrix = glm::mat3(glm::transpose(glm::inverse(m)));
        const glm::vec3 up = normalMatrix * glm::vec3(0.0f, 1.0f, 0.0f);
        ok &= expect(nearVec(glm::normalize(up), glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-3f),
                     "uniform sky scale must keep normals pointing up");
    }

    if (ok)
        std::cout << "[Sky] cameraSelfTest PASS | cases=6 scale=1/16 parallax=locked\n";
    return ok;
}

} // namespace scene
