#include "Scene/ViewModel.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <iostream>

namespace scene
{
namespace
{
bool expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "[ViewModel] selfTest FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool near(float a, float b, float tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

bool finite(const glm::vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite(const glm::mat4& m)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (!std::isfinite(m[column][row]))
                return false;
        }
    }
    return true;
}

// 与 VulkanCamera::setPerspectiveProjection 同一套写法（右手视图空间、
// Vulkan 的 0..1 深度、Y 轴翻转），保证 viewmodel 与场景的裁剪约定一致。
glm::mat4 projectionMatrix(float fovy, float aspect, float nearPlane, float farPlane)
{
    const float tanHalfFovy = std::tan(fovy * 0.5f);
    glm::mat4 projection{ 0.0f };
    projection[0][0] = 1.0f / (aspect * tanHalfFovy);
    projection[1][1] = 1.0f / tanHalfFovy;
    projection[2][2] = farPlane / (farPlane - nearPlane);
    projection[2][3] = 1.0f;
    projection[3][2] = -(farPlane * nearPlane) / (farPlane - nearPlane);
    projection[1][1] *= -1.0f;
    return projection;
}

// 视图空间里距相机 depth 米处的点的 NDC z（Vulkan 0..1）。
float ndcDepth(const glm::mat4& projection, float depth)
{
    const glm::vec4 clip = projection * glm::vec4(0.0f, 0.0f, depth, 1.0f);
    if (clip.w == 0.0f)
        return 0.0f;
    return clip.z / clip.w;
}

float ndcX(const glm::mat4& projection, float x, float depth)
{
    const glm::vec4 clip = projection * glm::vec4(x, 0.0f, depth, 1.0f);
    if (clip.w == 0.0f)
        return 0.0f;
    return clip.x / clip.w;
}
} // namespace

glm::mat4 viewModelProjection(float fovDegrees, float aspectRatio, float nearPlane,
                              float farPlane)
{
    float fov = fovDegrees;
    if (!std::isfinite(fov))
        fov = 54.0f;
    fov = glm::clamp(fov, 1.0f, 179.0f);

    float aspect = aspectRatio;
    if (!std::isfinite(aspect) || aspect <= 0.0f)
        aspect = 1.0f;

    float nearValue = nearPlane;
    if (!std::isfinite(nearValue) || nearValue <= 0.0f)
        nearValue = 0.01f;

    float farValue = farPlane;
    if (!std::isfinite(farValue) || farValue <= nearValue)
        farValue = nearValue * 1000.0f;

    return projectionMatrix(glm::radians(fov), aspect, nearValue, farValue);
}

glm::mat4 viewModelOffsetMatrix(const ViewModelSettings& settings)
{
    glm::vec3 offset = settings.offset;
    glm::vec3 rotation = settings.rotation;
    glm::vec3 scale = settings.scale;
    if (!finite(offset))
        offset = glm::vec3(0.30f, -0.26f, 0.62f);
    if (!finite(rotation))
        rotation = glm::vec3(0.0f);
    if (!finite(scale) || scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f)
        scale = glm::vec3(0.16f);

    glm::mat4 matrix{ 1.0f };
    matrix = glm::translate(matrix, offset);
    const glm::vec3 radians = glm::radians(rotation);
    matrix = glm::rotate(matrix, radians.y, glm::vec3(0.0f, 1.0f, 0.0f));
    matrix = glm::rotate(matrix, radians.x, glm::vec3(1.0f, 0.0f, 0.0f));
    matrix = glm::rotate(matrix, radians.z, glm::vec3(0.0f, 0.0f, 1.0f));
    matrix = glm::scale(matrix, scale);
    return matrix;
}

bool viewModelSelfTest()
{
    bool ok = true;

    // 1) 深度映射：近平面 → 0，远平面 → 1，中间单调递增（Vulkan 的 0..1）。
    {
        const glm::mat4 projection = viewModelProjection(54.0f, 16.0f / 9.0f, 0.01f, 10.0f);
        const float atNear = ndcDepth(projection, 0.01f);
        const float atFar = ndcDepth(projection, 10.0f);
        const float mid = ndcDepth(projection, 2.0f);
        ok = expect(near(atNear, 0.0f, 1.0e-3f), "near plane should map to depth 0") && ok;
        ok = expect(near(atFar, 1.0f, 1.0e-3f), "far plane should map to depth 1") && ok;
        ok = expect(mid > atNear && mid < atFar, "depth should increase with distance") && ok;
    }

    // 2) 视场角：更窄的 fov 让同一个点投影到更大的 |NDC x|（拉近感更强）。
    {
        const glm::mat4 narrow = viewModelProjection(30.0f, 16.0f / 9.0f, 0.01f, 10.0f);
        const glm::mat4 wide = viewModelProjection(90.0f, 16.0f / 9.0f, 0.01f, 10.0f);
        const float narrowX = std::fabs(ndcX(narrow, 0.1f, 0.62f));
        const float wideX = std::fabs(ndcX(wide, 0.1f, 0.62f));
        ok = expect(narrowX > wideX, "narrow fov should enlarge ndc x") && ok;
    }

    // 3) 宽高比：更宽的画布让同一个点的 |NDC x| 变小。
    {
        const glm::mat4 square = viewModelProjection(54.0f, 1.0f, 0.01f, 10.0f);
        const glm::mat4 wide = viewModelProjection(54.0f, 21.0f / 9.0f, 0.01f, 10.0f);
        const float squareX = std::fabs(ndcX(square, 0.1f, 0.62f));
        const float wideX = std::fabs(ndcX(wide, 0.1f, 0.62f));
        ok = expect(squareX > wideX, "wider aspect should shrink ndc x") && ok;
    }

    // 4) 偏移矩阵：先平移、再 Y→X→Z 旋转、最后缩放。
    {
        ViewModelSettings settings;
        settings.offset = glm::vec3(0.25f, -0.2f, -0.6f);
        settings.rotation = glm::vec3(0.0f, 90.0f, 0.0f);
        settings.scale = glm::vec3(2.0f);
        const glm::mat4 matrix = viewModelOffsetMatrix(settings);
        const glm::vec3 origin = glm::vec3(matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        const glm::vec3 unitX = glm::vec3(matrix * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        ok = expect(near(origin.x, 0.25f, 1.0e-5f) && near(origin.y, -0.2f, 1.0e-5f) &&
                        near(origin.z, -0.6f, 1.0e-5f),
                    "offset matrix should place the origin at the offset") && ok;
        // 绕 Y 轴 90°：局部 +X 转到相机空间的 -Z，再乘 2 倍缩放。
        ok = expect(near(unitX.x, 0.25f, 1.0e-4f) && near(unitX.y, -0.2f, 1.0e-4f) &&
                        near(unitX.z, -2.6f, 1.0e-4f),
                    "offset matrix should apply rotation then scale") && ok;
    }

    // 5) 独立近平面：武器距相机 5cm，用场景的近平面（0.1m）会被裁掉（负深度），
    //    用 viewmodel 自己的近平面（0.01m）则稳稳落在 0..1 之间。
    {
        const glm::mat4 viewModel = viewModelProjection(54.0f, 16.0f / 9.0f, 0.01f, 10.0f);
        const glm::mat4 world = viewModelProjection(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
        const float viewModelDepth = ndcDepth(viewModel, 0.05f);
        const float worldDepth = ndcDepth(world, 0.05f);
        ok = expect(viewModelDepth >= 0.0f && viewModelDepth <= 1.0f,
                    "viewmodel depth should stay inside 0..1 at 5cm") && ok;
        ok = expect(worldDepth < 0.0f, "world near plane should clip a 5cm weapon") && ok;
    }

    // 6) 退化输入：全部夹紧、结果仍然有限。
    {
        const glm::mat4 projection = viewModelProjection(0.0f, 0.0f, -1.0f, 0.0f);
        ok = expect(finite(projection), "degenerate projection must stay finite") && ok;
        ViewModelSettings bad;
        bad.offset = glm::vec3(std::nanf(""), 0.0f, 0.0f);
        bad.rotation = glm::vec3(std::nanf(""));
        bad.scale = glm::vec3(0.0f);
        ok = expect(finite(viewModelOffsetMatrix(bad)), "degenerate offset must stay finite") && ok;
    }

    if (ok)
    {
        std::cout << "[ViewModel] selfTest PASS | cases=6 fov=54 depth=0.01..10 aspect=ok\n";
    }
    return ok;
}

} // namespace scene