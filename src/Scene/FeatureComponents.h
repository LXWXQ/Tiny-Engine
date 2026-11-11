#pragma once

// ===========================================================================
// 内置效果组件
//
// 这些组件大多只改 Transform，不写渲染代码；世界变换、空间索引、可见性、
// 提交照旧工作。它们也是新组件的模板：组件 + 属性描述 + 可选系统 + 注册。
//
//   effects.spin     绕轴持续旋转
//   effects.orbit    沿水平圆环绕中心公转
//   effects.bob      沿轴上下浮动（保留手动位移）
//   effects.pulse    缩放脉冲（保留手动缩放）
//   effects.sway     绕轴来回摆动
//   effects.lookAt   朝向一个局部空间目标点
//   effects.follow   以固定速度靠近一个局部空间目标点
//   effects.particles 粒子发射器（唯一例外：粒子由 ParticlePass 前向绘制）
//   effects.fogVolume 可放置的局部体积雾（由 FogVolumePass 前向绘制）
//
// LookAt / Follow 的 target 是父空间坐标；实体没有父节点时就是世界坐标。
// ===========================================================================

#include "Scene/ComponentRegistry.h"
#include "Scene/SystemScheduler.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace scene
{

struct SpinComponent
{
    glm::vec3 axis{ 0.0f, 1.0f, 0.0f };
    float degreesPerSecond = 45.0f;
    bool enabled = true;
};

struct OrbitComponent
{
    glm::vec3 center{ 0.0f };
    float radius = 3.0f;
    float degreesPerSecond = 30.0f;
    // 当前相位（度）。它也是属性，用户可以在 Inspector 里设起始角度。
    float angle = 0.0f;
    bool enabled = true;
};

// 沿 axis 做正弦浮动。系统按“相对上一帧的偏移量”增量写 Transform，
// 所以用户手动移动物体后，浮动会从新位置继续，而不是被拉回旧基点。
struct BobComponent
{
    glm::vec3 axis{ 0.0f, 1.0f, 0.0f };
    float amplitude = 0.25f;
    float frequency = 1.0f;   // Hz
    float phase = 0.0f;       // deg
    bool enabled = true;

    // 运行期状态，不参与属性/序列化。
    float time = 0.0f;
    float lastOffset = 0.0f;
};

// 缩放脉冲。与 Bob 一样用增量比例，保留用户手动设置的缩放。
struct PulseComponent
{
    float amplitude = 0.15f;
    float frequency = 1.0f;   // Hz
    float phase = 0.0f;       // deg
    bool enabled = true;

    float time = 0.0f;
    float lastFactor = 1.0f;
};

// 绕 axis 来回摆动。与 Spin 的持续旋转不同，角度是正弦的。
struct SwayComponent
{
    glm::vec3 axis{ 0.0f, 0.0f, 1.0f };
    float amplitudeDegrees = 10.0f;
    float frequency = 1.0f;   // Hz
    float phase = 0.0f;       // deg
    bool enabled = true;

    float time = 0.0f;
    float lastAngle = 0.0f;
};

// 朝向 target。turnSpeed=0 表示瞬时对准；大于 0 时按度/秒插值。
// target / up 都是父空间；顶层实体即世界空间。
struct LookAtComponent
{
    glm::vec3 target{ 0.0f, 0.0f, -5.0f };
    glm::vec3 up{ 0.0f, 1.0f, 0.0f };
    float turnSpeed = 0.0f;   // deg/s, 0 = instant
    bool enabled = true;
};

// 以 speed 靠近 target，停在 stopDistance 外。
struct FollowComponent
{
    glm::vec3 target{ 0.0f };
    float speed = 3.0f;
    float stopDistance = 0.1f;
    bool enabled = true;
};

// 单个存活粒子。位置/速度是世界空间；age/lifetime 决定颜色、尺寸与淘汰。
// 这是运行期数据，不进入属性、不序列化，也不会随组件复制到别的实体。
struct Particle
{
    glm::vec3 position{ 0.0f };
    glm::vec3 velocity{ 0.0f };
    float age = 0.0f;
    float lifetime = 1.0f;
    float sizeScale = 1.0f;
};

// 全局粒子上限（所有发射器加起来）。ParticlePass 的实例缓冲同容量，
// 模拟侧先截断，避免画到一半才发现缓冲区装不下。
inline constexpr std::size_t kMaxParticlesTotal = 16384;

// 实体级粒子发射器：挂在有 Transform 的实体上，按世界空间位置连续发射。
//
// 方向/偏移在局部空间，系统用 WorldTransform 的旋转（含父节点）变换到世界。
// 没有 WorldTransform 时退回 Transform 的局部坐标，方便刚建实体就预览。
// enabled=false 只停止发射，已有粒子会继续走完寿命；移除组件则立即消失。
struct ParticleEmitterComponent
{
    bool enabled = true;

    // 发射
    float emissionRate = 40.0f;          // 个/秒
    int maxParticles = 512;              // 单个发射器上限
    glm::vec3 direction{ 0.0f, 1.0f, 0.0f };
    float spreadAngle = 25.0f;           // 锥形半角（度）
    glm::vec3 emissionOffset{ 0.0f };
    float emissionRadius = 0.1f;

    // 运动与外观
    float lifetime = 2.0f;               // 秒
    float lifetimeVariation = 0.3f;      // 0..1，相对寿命的随机浮动
    float initialSpeed = 2.0f;           // 米/秒
    float speedVariation = 0.5f;         // 0..1，相对速度的随机浮动
    glm::vec3 gravity{ 0.0f, -1.5f, 0.0f };
    float startSize = 0.15f;             // 世界空间直径
    float endSize = 0.02f;
    glm::vec4 startColor{ 1.0f, 0.55f, 0.15f, 1.0f };
    glm::vec4 endColor{ 1.0f, 0.10f, 0.02f, 0.0f };

    // 粒子 sprite。存相对 Resources/Texture/ 的路径（例如
    // "Particles/Kenney/fire_01.png"）；空字符串表示使用程序化圆形。
    // ParticlePass 在启动时预加载 Texture/Particles 下的贴图，新增文件需重启。
    std::string texture;

    // 运行期状态，不参与属性/序列化。
    std::vector<Particle> particles;
    float spawnAccumulator = 0.0f;
    uint32_t randomState = 0;
};

// 场景里可放置的局部体积雾（盒形）。
//
// 盒体由组件自己的 center / size / rotation 定义（局部空间），再乘实体
// WorldTransform。这样即使实体本身没有缩放（或组件挂在一个空实体上），
// 也能在 Inspector 里直接调位置和体积；实体 Transform 仍然可以整体移动/旋转。
// FogVolumePass 每帧把最多 8 个组件打包进 UBO，在 Lighting 之后做全屏
// raymarch。雾只影响渲染，不写深度、不进 G-Buffer，也不参与阴影/SSAO。
struct FogVolumeComponent
{
    bool enabled = true;

    // ---- 放置（局部空间；实体 Transform 之下）----
    glm::vec3 center{ 0.0f, 0.0f, 0.0f };   // 盒心相对实体原点的偏移
    glm::vec3 size{ 20.0f, 8.0f, 20.0f };   // 盒体全尺寸（米）
    glm::vec3 rotation{ 0.0f, 0.0f, 0.0f }; // 欧拉角（度），与 Transform 同一顺序

    // 消光系数（1/米）。20 米厚的雾在 density=0.02 时约 33% 不透。
    float density = 0.02f;
    glm::vec4 color{ 0.78f, 0.84f, 0.92f, 1.0f };

    // 从盒体中心向上的高度衰减（1/米）；0 表示整盒均匀。
    float heightFalloff = 0.08f;
    float noiseStrength = 0.55f;     // 0=均匀雾，1=强团块/丝缕
    float noiseScale = 0.08f;        // 噪声世界尺度（1/米）
    glm::vec3 wind{ 0.35f, 0.0f, 0.12f }; // 米/秒
    float edgeSoftness = 0.18f;      // 局部盒体边缘淡出宽度（0..0.5）
    float anisotropy = 0.55f;        // HG 相函数，越大越向前散射
    float ambient = 0.35f;           // 环境散射强度
    int steps = 12;                  // 每像素步数，1..32

    // T(center) * R(rotation) * S(size)。渲染与视口线框共用，避免两处算法漂移。
    glm::mat4 localMatrix() const;
};

// 注册全部效果组件的元数据。重复调用是安全的：ComponentRegistry::add
// 会拒绝同 id 的第二个描述。
void registerFeatureComponents(ComponentRegistry& registry);

// 注册 effects.* 系统到 Simulation 阶段。
void registerFeatureSystems(SystemScheduler& scheduler);

} // namespace scene
