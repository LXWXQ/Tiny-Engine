#include "Scene/FeatureComponents.h"

#include "Scene/Components.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace scene
{

namespace
{

// 简单字段的属性描述。带范围限制的 float 用 makeFloatProperty；
// 需要自定义语义（例如 Orbit.radius 不能为负）时仍写专用 lambda。
template <typename T>
PropertyDescriptor makeVec3Property(const char* name, const char* label,
                                    glm::vec3 T::* member, float dragSpeed = 0.0f,
                                    const char* tooltip = nullptr)
{
    PropertyDescriptor property = makeProperty<T>(
        name, label, PropertyType::Vec3,
        [member](const T& component) { return PropertyValue::fromVec3(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            component.*member = value.asVec3();
        });
    property.dragSpeed = dragSpeed;
    if (tooltip != nullptr)
        property.tooltip = tooltip;
    return property;
}

template <typename T>
PropertyDescriptor makeFloatProperty(const char* name, const char* label,
                                     float T::* member, float minimum, float maximum,
                                     const char* tooltip = nullptr, float dragSpeed = 0.0f)
{
    PropertyDescriptor property = makeProperty<T>(
        name, label, PropertyType::Float,
        [member](const T& component) { return PropertyValue::fromFloat(component.*member); },
        [member, minimum, maximum](T& component, const PropertyValue& value)
        {
            component.*member = std::clamp(value.asFloat(), minimum, maximum);
        });
    property.hasRange = true;
    property.minimum = minimum;
    property.maximum = maximum;
    property.dragSpeed = dragSpeed;
    if (tooltip != nullptr)
        property.tooltip = tooltip;
    return property;
}

template <typename T>
PropertyDescriptor makeBoolProperty(const char* name, const char* label,
                                    bool T::* member, const char* tooltip = nullptr)
{
    PropertyDescriptor property = makeProperty<T>(
        name, label, PropertyType::Bool,
        [member](const T& component) { return PropertyValue::fromBool(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            component.*member = value.asBool();
        });
    if (tooltip != nullptr)
        property.tooltip = tooltip;
    return property;
}

template <typename T>
PropertyDescriptor makeColorProperty(const char* name, const char* label,
                                     glm::vec4 T::*member, const char* tooltip = nullptr)
{
    PropertyDescriptor property = makeProperty<T>(
        name, label, PropertyType::Color,
        [member](const T& component) { return PropertyValue::fromColor(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            component.*member = value.asColor();
        });
    if (tooltip != nullptr)
        property.tooltip = tooltip;
    return property;
}

template <typename T>
PropertyDescriptor makeIntProperty(const char* name, const char* label,
                                   int T::*member, int minimum, int maximum,
                                   const char* tooltip = nullptr, float dragSpeed = 1.0f)
{
    PropertyDescriptor property = makeProperty<T>(
        name, label, PropertyType::Int,
        [member](const T& component) { return PropertyValue::fromInt(component.*member); },
        [member, minimum, maximum](T& component, const PropertyValue& value)
        {
            component.*member = std::clamp(value.asInt(), minimum, maximum);
        });
    property.hasRange = true;
    property.minimum = static_cast<float>(minimum);
    property.maximum = static_cast<float>(maximum);
    property.dragSpeed = dragSpeed;
    if (tooltip != nullptr)
        property.tooltip = tooltip;
    return property;
}

template <typename T>
PropertyDescriptor makeStringProperty(const char* name, const char* label,
                                      std::string T::*member, const char* tooltip = nullptr,
                                      bool hidden = false)
{
    PropertyDescriptor property = makeProperty<T>(
        name, label, PropertyType::String,
        [member](const T& component) { return PropertyValue::fromString(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            component.*member = value.asString();
        });
    if (tooltip != nullptr)
        property.tooltip = tooltip;
    property.hidden = hidden;
    return property;
}

float sinePhase(float timeSeconds, float frequency, float phaseDegrees)
{
    return glm::radians(phaseDegrees) +
           glm::two_pi<float>() * frequency * timeSeconds;
}

bool normalizeAxis(const glm::vec3& axis, glm::vec3& normalized)
{
    const float length = glm::length(axis);
    if (length < 1e-5f)
        return false;
    normalized = axis / length;
    return true;
}

PropertyDescriptor makeSpinAxisProperty()
{
    return makeProperty<SpinComponent>(
        "axis", "Axis", PropertyType::Vec3,
        [](const SpinComponent& component) { return PropertyValue::fromVec3(component.axis); },
        [](SpinComponent& component, const PropertyValue& value)
        {
            component.axis = value.asVec3();
        });
}

PropertyDescriptor makeSpinSpeedProperty()
{
    return makeProperty<SpinComponent>(
        "degreesPerSecond", "Degrees / Second", PropertyType::Float,
        [](const SpinComponent& component)
        {
            return PropertyValue::fromFloat(component.degreesPerSecond);
        },
        [](SpinComponent& component, const PropertyValue& value)
        {
            component.degreesPerSecond = std::clamp(value.asFloat(), -3600.0f, 3600.0f);
        });
}

PropertyDescriptor makeSpinEnabledProperty()
{
    return makeProperty<SpinComponent>(
        "enabled", "Enabled", PropertyType::Bool,
        [](const SpinComponent& component) { return PropertyValue::fromBool(component.enabled); },
        [](SpinComponent& component, const PropertyValue& value)
        {
            component.enabled = value.asBool();
        });
}

PropertyDescriptor makeOrbitCenterProperty()
{
    return makeProperty<OrbitComponent>(
        "center", "Center", PropertyType::Vec3,
        [](const OrbitComponent& component) { return PropertyValue::fromVec3(component.center); },
        [](OrbitComponent& component, const PropertyValue& value)
        {
            component.center = value.asVec3();
        });
}

PropertyDescriptor makeOrbitRadiusProperty()
{
    return makeProperty<OrbitComponent>(
        "radius", "Radius", PropertyType::Float,
        [](const OrbitComponent& component) { return PropertyValue::fromFloat(component.radius); },
        [](OrbitComponent& component, const PropertyValue& value)
        {
            component.radius = std::clamp(value.asFloat(), 0.0f, 1000.0f);
        });
}

PropertyDescriptor makeOrbitSpeedProperty()
{
    return makeProperty<OrbitComponent>(
        "degreesPerSecond", "Degrees / Second", PropertyType::Float,
        [](const OrbitComponent& component)
        {
            return PropertyValue::fromFloat(component.degreesPerSecond);
        },
        [](OrbitComponent& component, const PropertyValue& value)
        {
            component.degreesPerSecond = std::clamp(value.asFloat(), -3600.0f, 3600.0f);
        });
}

PropertyDescriptor makeOrbitAngleProperty()
{
    return makeProperty<OrbitComponent>(
        "angle", "Angle", PropertyType::Float,
        [](const OrbitComponent& component) { return PropertyValue::fromFloat(component.angle); },
        [](OrbitComponent& component, const PropertyValue& value)
        {
            component.angle = value.asFloat();
        });
}

PropertyDescriptor makeOrbitEnabledProperty()
{
    return makeProperty<OrbitComponent>(
        "enabled", "Enabled", PropertyType::Bool,
        [](const OrbitComponent& component) { return PropertyValue::fromBool(component.enabled); },
        [](OrbitComponent& component, const PropertyValue& value)
        {
            component.enabled = value.asBool();
        });
}

void spinSystem(entt::registry& registry, float deltaSeconds)
{
    for (auto [entity, spin, transform] :
         registry.view<SpinComponent, TransformComponent>().each())
    {
        (void)entity;
        if (!spin.enabled || spin.degreesPerSecond == 0.0f)
            continue;

        glm::vec3 axis = spin.axis;
        const float length = glm::length(axis);
        if (length < 1e-5f)
            continue;
        axis /= length;

        const glm::quat delta =
            glm::angleAxis(glm::radians(spin.degreesPerSecond * deltaSeconds), axis);
        transform.setOrientation(glm::normalize(delta * transform.getOrientation()));
    }
}

void orbitSystem(entt::registry& registry, float deltaSeconds)
{
    for (auto [entity, orbit, transform] :
         registry.view<OrbitComponent, TransformComponent>().each())
    {
        (void)entity;
        if (!orbit.enabled || orbit.degreesPerSecond == 0.0f)
            continue;

        orbit.angle += orbit.degreesPerSecond * deltaSeconds;
        // 防止长时间运行后 float 精度下降到“转一圈只前进几步”。
        orbit.angle = std::fmod(orbit.angle, 360.0f);

        const float radians = glm::radians(orbit.angle);
        transform.setTranslation(
            orbit.center + glm::vec3(std::cos(radians) * orbit.radius, 0.0f,
                                     std::sin(radians) * orbit.radius));
    }
}

void bobSystem(entt::registry& registry, float deltaSeconds)
{
    for (auto [entity, bob, transform] :
         registry.view<BobComponent, TransformComponent>().each())
    {
        (void)entity;
        if (!bob.enabled)
            continue;

        bob.time += deltaSeconds;
        const float offset = bob.amplitude *
                             std::sin(sinePhase(bob.time, bob.frequency, bob.phase));
        // 只写“本帧相对上一帧的位移”，用户手动拖动后不会被拉回旧基点。
        const float delta = offset - bob.lastOffset;
        bob.lastOffset = offset;

        glm::vec3 axis;
        if (!normalizeAxis(bob.axis, axis) || std::abs(delta) < 1e-7f)
            continue;

        transform.setTranslation(transform.getTranslation() + axis * delta);
    }
}

void pulseSystem(entt::registry& registry, float deltaSeconds)
{
    for (auto [entity, pulse, transform] :
         registry.view<PulseComponent, TransformComponent>().each())
    {
        (void)entity;
        if (!pulse.enabled)
            continue;

        pulse.time += deltaSeconds;
        float factor = 1.0f + pulse.amplitude *
                                  std::sin(sinePhase(pulse.time, pulse.frequency, pulse.phase));
        factor = std::max(factor, 0.01f);

        if (pulse.lastFactor <= 1e-4f)
            pulse.lastFactor = 1.0f;

        // 乘法增量：保留用户手动设置的缩放，整周期后回到原比例。
        const float ratio = factor / pulse.lastFactor;
        pulse.lastFactor = factor;
        if (std::abs(ratio - 1.0f) < 1e-7f)
            continue;

        transform.setScale(transform.getScale() * ratio);
    }
}

void swaySystem(entt::registry& registry, float deltaSeconds)
{
    for (auto [entity, sway, transform] :
         registry.view<SwayComponent, TransformComponent>().each())
    {
        (void)entity;
        if (!sway.enabled)
            continue;

        sway.time += deltaSeconds;
        const float angle = sway.amplitudeDegrees *
                            std::sin(sinePhase(sway.time, sway.frequency, sway.phase));
        const float delta = angle - sway.lastAngle;
        sway.lastAngle = angle;

        glm::vec3 axis;
        if (!normalizeAxis(sway.axis, axis) || std::abs(delta) < 1e-6f)
            continue;

        const glm::quat deltaRotation = glm::angleAxis(glm::radians(delta), axis);
        transform.setOrientation(
            glm::normalize(deltaRotation * transform.getOrientation()));
    }
}

void lookAtSystem(entt::registry& registry, float deltaSeconds)
{
    for (auto [entity, lookAt, transform] :
         registry.view<LookAtComponent, TransformComponent>().each())
    {
        (void)entity;
        if (!lookAt.enabled)
            continue;

        const glm::vec3 position = transform.getTranslation();
        glm::vec3 direction = lookAt.target - position;
        const float distance = glm::length(direction);
        if (distance < 1e-5f)
            continue;
        direction /= distance;

        glm::vec3 up = lookAt.up;
        if (glm::length(up) < 1e-5f)
            up = glm::vec3(0.0f, 1.0f, 0.0f);
        up = glm::normalize(up);
        // lookAt 在视线与 up 平行时退化；换一个垂直方向兜底。
        if (std::abs(glm::dot(direction, up)) > 0.9995f)
        {
            up = std::abs(direction.y) > 0.9f
                     ? glm::vec3(0.0f, 0.0f, 1.0f)
                     : glm::vec3(0.0f, 1.0f, 0.0f);
        }

        const glm::mat4 view = glm::lookAt(position, lookAt.target, up);
        glm::quat desired = glm::normalize(
            glm::quat_cast(glm::transpose(glm::mat3(view))));

        if (lookAt.turnSpeed <= 0.0f)
        {
            transform.setOrientation(desired);
            continue;
        }

        glm::quat current = transform.getOrientation();
        if (glm::dot(current, desired) < 0.0f)
            desired = -desired;

        const float dot = glm::clamp(std::abs(glm::dot(current, desired)), 0.0f, 1.0f);
        const float angle = 2.0f * std::acos(dot);
        const float maxAngle = glm::radians(lookAt.turnSpeed) * deltaSeconds;
        if (angle <= maxAngle + 1e-5f || angle < 1e-5f)
        {
            transform.setOrientation(desired);
            continue;
        }

        transform.setOrientation(
            glm::normalize(glm::slerp(current, desired, maxAngle / angle)));
    }
}

void followSystem(entt::registry& registry, float deltaSeconds)
{
    for (auto [entity, follow, transform] :
         registry.view<FollowComponent, TransformComponent>().each())
    {
        (void)entity;
        if (!follow.enabled || follow.speed <= 0.0f)
            continue;

        const glm::vec3 position = transform.getTranslation();
        const glm::vec3 toTarget = follow.target - position;
        const float distance = glm::length(toTarget);
        if (distance <= follow.stopDistance + 1e-5f || distance < 1e-5f)
            continue;

        const float step = std::min(distance - follow.stopDistance,
                                    follow.speed * deltaSeconds);
        if (step <= 1e-6f)
            continue;

        transform.setTranslation(position + toTarget / distance * step);
    }
}

// xorshift32：每个发射器自带状态，避免共享 std::rand 导致多个发射器
// 在同一帧拿到完全相同的随机序列。
uint32_t nextRandom(uint32_t& state)
{
    if (state == 0u)
        state = 0x9E3779B9u;
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return state;
}

float random01(uint32_t& state)
{
    return static_cast<float>(nextRandom(state) & 0x00FFFFFFu) / 16777216.0f;
}

glm::vec3 randomUnitVector(uint32_t& state)
{
    const float z = random01(state) * 2.0f - 1.0f;
    const float angle = random01(state) * glm::two_pi<float>();
    const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return glm::vec3(radius * std::cos(angle), z, radius * std::sin(angle));
}

// 在以 axis 为轴、半角 acos(cosMaxAngle) 的圆锥内均匀取方向。
glm::vec3 sampleConeDirection(const glm::vec3& axis, float cosMaxAngle,
                              uint32_t& state)
{
    const glm::vec3 helper = std::abs(axis.y) > 0.99f
                                 ? glm::vec3(1.0f, 0.0f, 0.0f)
                                 : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 tangent = glm::normalize(glm::cross(helper, axis));
    const glm::vec3 bitangent = glm::cross(axis, tangent);

    const float cosTheta = 1.0f - random01(state) * (1.0f - cosMaxAngle);
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = random01(state) * glm::two_pi<float>();
    return glm::normalize(axis * cosTheta +
                          (tangent * std::cos(phi) + bitangent * std::sin(phi)) * sinTheta);
}

// 粒子模拟：推进、淘汰、发射。挂在 PostTransform，保证世界位置/旋转
// 已经由 Scene::updateWorldTransforms 更新；enabled=false 时只推进不发射，
// 移除组件时运行期粒子随组件一起销毁。
void particleSystem(entt::registry& registry, float deltaSeconds)
{
    // 卡顿或断点恢复时不要让粒子一步飞出画面；0.1s 也足够覆盖 10 FPS。
    const float dt = std::clamp(deltaSeconds, 0.0f, 0.1f);

    std::size_t totalAlive = 0;
    for (auto [entity, emitter] : registry.view<ParticleEmitterComponent>().each())
    {
        // 先推进已有粒子，再决定这一帧能发射多少，避免新粒子被本帧淘汰。
        for (std::size_t i = 0; i < emitter.particles.size();)
        {
            Particle& particle = emitter.particles[i];
            particle.age += dt;
            if (particle.age >= particle.lifetime)
            {
                emitter.particles[i] = emitter.particles.back();
                emitter.particles.pop_back();
                continue;
            }

            particle.velocity += emitter.gravity * dt;
            particle.position += particle.velocity * dt;
            ++i;
        }

        totalAlive += emitter.particles.size();
        if (!emitter.enabled)
        {
            emitter.spawnAccumulator = 0.0f;
            continue;
        }
        if (dt <= 0.0f || emitter.emissionRate <= 0.0f || emitter.maxParticles <= 0)
            continue;

        glm::mat4 world(1.0f);
        bool hasWorld = false;
        if (const WorldTransformComponent* worldTransform =
                registry.try_get<WorldTransformComponent>(entity);
            worldTransform != nullptr && worldTransform->valid)
        {
            world = worldTransform->matrix;
            hasWorld = true;
        }

        glm::vec3 origin(0.0f);
        if (hasWorld)
            origin = glm::vec3(world[3]);
        else if (const TransformComponent* transform =
                     registry.try_get<TransformComponent>(entity))
            origin = transform->getTranslation();

        const glm::mat3 rotation = hasWorld ? glm::mat3(world) : glm::mat3(1.0f);
        origin += rotation * emitter.emissionOffset;

        glm::vec3 direction = rotation * emitter.direction;
        if (glm::length(direction) < 1e-5f)
            direction = hasWorld ? rotation * glm::vec3(0.0f, 1.0f, 0.0f)
                                 : glm::vec3(0.0f, 1.0f, 0.0f);
        if (glm::length(direction) < 1e-5f)
            direction = glm::vec3(0.0f, 1.0f, 0.0f);
        direction = glm::normalize(direction);

        emitter.spawnAccumulator += emitter.emissionRate * dt;
        // 长时间被 maxParticles 卡住时不要攒出一次巨大的爆发。
        emitter.spawnAccumulator = std::min(emitter.spawnAccumulator, 256.0f);
        const std::size_t requested =
            static_cast<std::size_t>(std::floor(emitter.spawnAccumulator));
        if (requested == 0u)
            continue;

        const std::size_t particleLimit = static_cast<std::size_t>(emitter.maxParticles);
        const std::size_t perEmitterRoom = emitter.particles.size() < particleLimit
                                               ? particleLimit - emitter.particles.size()
                                               : 0u;
        const std::size_t globalRoom = totalAlive < kMaxParticlesTotal
                                           ? kMaxParticlesTotal - totalAlive
                                           : 0u;
        const std::size_t spawnCount = std::min({ requested, perEmitterRoom, globalRoom });
        if (spawnCount == 0u)
            continue;

        uint32_t& randomState = emitter.randomState;
        if (randomState == 0u)
            randomState = 0x9E3779B9u ^
                          (static_cast<uint32_t>(entity) * 0x85EBCA6Bu + 0xC2B2AE35u);

        const float cosMaxAngle = std::cos(glm::radians(std::clamp(emitter.spreadAngle, 0.0f, 180.0f)));
        const float speedVariation = std::clamp(emitter.speedVariation, 0.0f, 1.0f);
        const float lifetimeVariation = std::clamp(emitter.lifetimeVariation, 0.0f, 1.0f);

        for (std::size_t i = 0; i < spawnCount; ++i)
        {
            Particle particle{};
            particle.position = origin + randomUnitVector(randomState) *
                                             (emitter.emissionRadius *
                                              std::cbrt(random01(randomState)));
            particle.velocity = sampleConeDirection(direction, cosMaxAngle, randomState) *
                                (emitter.initialSpeed *
                                 (1.0f + (random01(randomState) * 2.0f - 1.0f) * speedVariation));
            particle.lifetime = std::max(
                0.02f, emitter.lifetime *
                           (1.0f + (random01(randomState) * 2.0f - 1.0f) * lifetimeVariation));
            particle.sizeScale = 0.85f + random01(randomState) * 0.3f;
            emitter.particles.push_back(particle);
        }

        emitter.spawnAccumulator -= static_cast<float>(spawnCount);
        totalAlive += spawnCount;
    }
}

} // namespace

glm::mat4 FogVolumeComponent::localMatrix() const
{
    glm::mat4 matrix = glm::translate(glm::mat4(1.0f), center);
    const glm::quat orientation =
        TransformComponent::quatFromEuler(glm::radians(rotation));
    matrix *= glm::mat4_cast(orientation);
    // 负尺寸会让行列式变号、盒体翻面；这里给一个最小厚度兜底。
    return glm::scale(matrix, glm::max(size, glm::vec3(0.01f)));
}

void registerFeatureComponents(ComponentRegistry& registry)
{
    registry.add(makeComponentDescriptor<SpinComponent>(
        "effects.spin", "Spin", "效果",
        { withTooltip(makeSpinAxisProperty(), "局部旋转轴；零向量表示不旋转"),
          withTooltip(withRange(makeSpinSpeedProperty(), -3600.0f, 3600.0f, 0.5f),
                      "局部空间自转速度，单位度/秒；负值反向"),
          withTooltip(makeSpinEnabledProperty(), "关闭后系统跳过该组件，但保留设置") }));

    registry.add(makeComponentDescriptor<OrbitComponent>(
        "effects.orbit", "Orbit", "效果",
        { withTooltip(makeOrbitCenterProperty(), "父空间里的轨道中心"),
          withTooltip(withRange(makeOrbitRadiusProperty(), 0.0f, 1000.0f, 0.05f),
                      "父空间轨道半径；0 表示绕中心原地旋转"),
          withTooltip(withRange(makeOrbitSpeedProperty(), -3600.0f, 3600.0f, 0.5f),
                      "绕 Y 轴公转速度，单位度/秒；负值反向"),
          withTooltip(withRange(makeOrbitAngleProperty(), -360.0f, 360.0f, 0.5f),
                      "当前公转角，单位度；系统会按 360° 回绕"),
          withTooltip(makeOrbitEnabledProperty(), "关闭后系统跳过该组件，但保留设置") }));

    registry.add(makeComponentDescriptor<BobComponent>(
        "effects.bob", "Bob", "效果",
        { makeVec3Property<BobComponent>("axis", "Axis", &BobComponent::axis, 0.05f,
                                         "位移轴；零向量表示不动"),
          makeFloatProperty<BobComponent>("amplitude", "Amplitude",
                                          &BobComponent::amplitude, 0.0f, 100.0f,
                                          "单程位移，单位米", 0.005f),
          makeFloatProperty<BobComponent>("frequency", "Frequency (Hz)",
                                          &BobComponent::frequency, 0.0f, 100.0f,
                                          "振荡频率，Hz", 0.01f),
          makeFloatProperty<BobComponent>("phase", "Phase (deg)",
                                          &BobComponent::phase, -720.0f, 720.0f,
                                          "相位偏移，单位度", 0.5f),
          makeBoolProperty<BobComponent>("enabled", "Enabled", &BobComponent::enabled,
                                         "关闭后系统跳过该组件，但保留设置") }));

    registry.add(makeComponentDescriptor<PulseComponent>(
        "effects.pulse", "Pulse", "效果",
        { makeFloatProperty<PulseComponent>("amplitude", "Amplitude",
                                            &PulseComponent::amplitude, -0.95f, 10.0f,
                                            "缩放增量；-0.95 表示最小缩到 5%", 0.005f),
          makeFloatProperty<PulseComponent>("frequency", "Frequency (Hz)",
                                            &PulseComponent::frequency, 0.0f, 100.0f,
                                            "振荡频率，Hz", 0.01f),
          makeFloatProperty<PulseComponent>("phase", "Phase (deg)",
                                            &PulseComponent::phase, -720.0f, 720.0f,
                                            "相位偏移，单位度", 0.5f),
          makeBoolProperty<PulseComponent>("enabled", "Enabled", &PulseComponent::enabled,
                                           "关闭后系统跳过该组件，但保留设置") }));

    registry.add(makeComponentDescriptor<SwayComponent>(
        "effects.sway", "Sway", "效果",
        { makeVec3Property<SwayComponent>("axis", "Axis", &SwayComponent::axis, 0.05f,
                                          "局部旋转轴；零向量表示不摆动"),
          makeFloatProperty<SwayComponent>("amplitudeDegrees", "Amplitude (deg)",
                                           &SwayComponent::amplitudeDegrees, 0.0f, 180.0f,
                                           "左右摆动的最大角度，单位度", 0.5f),
          makeFloatProperty<SwayComponent>("frequency", "Frequency (Hz)",
                                           &SwayComponent::frequency, 0.0f, 100.0f,
                                           "振荡频率，Hz", 0.01f),
          makeFloatProperty<SwayComponent>("phase", "Phase (deg)",
                                           &SwayComponent::phase, -720.0f, 720.0f,
                                           "相位偏移，单位度", 0.5f),
          makeBoolProperty<SwayComponent>("enabled", "Enabled", &SwayComponent::enabled,
                                          "关闭后系统跳过该组件，但保留设置") }));

    registry.add(makeComponentDescriptor<LookAtComponent>(
        "effects.lookAt", "Look At", "运动",
        { makeVec3Property<LookAtComponent>("target", "Target", &LookAtComponent::target, 0.05f,
                                            "父空间目标点；局部 -Z 轴对准它"),
          makeVec3Property<LookAtComponent>("up", "Up", &LookAtComponent::up, 0.05f,
                                            "父空间上方向；与视线平行时自动兜底"),
          makeFloatProperty<LookAtComponent>("turnSpeed", "Turn Speed (deg/s)",
                                             &LookAtComponent::turnSpeed, 0.0f, 3600.0f,
                                             "最大转向速度；0 表示瞬间对准", 0.5f),
          makeBoolProperty<LookAtComponent>("enabled", "Enabled", &LookAtComponent::enabled,
                                            "关闭后系统跳过该组件，但保留设置") }));

    registry.add(makeComponentDescriptor<FollowComponent>(
        "effects.follow", "Follow", "运动",
        { makeVec3Property<FollowComponent>("target", "Target", &FollowComponent::target, 0.05f,
                                            "父空间目标点"),
          makeFloatProperty<FollowComponent>("speed", "Speed",
                                             &FollowComponent::speed, 0.0f, 1000.0f,
                                             "移动速度，单位/秒；0 表示停止", 0.01f),
          makeFloatProperty<FollowComponent>("stopDistance", "Stop Distance",
                                             &FollowComponent::stopDistance, 0.0f, 1000.0f,
                                             "与目标保持的停止距离", 0.01f),
          makeBoolProperty<FollowComponent>("enabled", "Enabled", &FollowComponent::enabled,
                                            "关闭后系统跳过该组件，但保留设置") }));

    // F5：粒子发射器。属性只暴露可序列化的发射/外观参数；particles 数组
    // 是运行期状态，复制组件或撤销时由默认构造重置，不会把旧粒子带过去。
    registry.add(makeComponentDescriptor<ParticleEmitterComponent>(
        "effects.particles", "Particle Emitter", "效果",
        { makeBoolProperty<ParticleEmitterComponent>(
              "enabled", "Enabled", &ParticleEmitterComponent::enabled,
              "关闭后停止发射，已有粒子会走完寿命；移除组件立即消失"),
          makeFloatProperty<ParticleEmitterComponent>(
              "emissionRate", "Emission Rate (per s)", &ParticleEmitterComponent::emissionRate,
              0.0f, 2000.0f, "每秒发射的粒子数；0 表示不发射", 0.5f),
          makeIntProperty<ParticleEmitterComponent>(
              "maxParticles", "Max Particles", &ParticleEmitterComponent::maxParticles,
              0, 4096, "单个发射器同时存在的粒子上限；全局上限 16384", 1.0f),
          makeVec3Property<ParticleEmitterComponent>(
              "direction", "Direction", &ParticleEmitterComponent::direction, 0.02f,
              "发射锥的轴，局部空间；默认 +Y"),
          makeFloatProperty<ParticleEmitterComponent>(
              "spreadAngle", "Spread Angle (deg)", &ParticleEmitterComponent::spreadAngle,
              0.0f, 180.0f, "锥形半角；0 是笔直一束，180 是全向", 0.5f),
          makeFloatProperty<ParticleEmitterComponent>(
              "initialSpeed", "Initial Speed", &ParticleEmitterComponent::initialSpeed,
              0.0f, 100.0f, "初速度，单位/秒", 0.02f),
          makeFloatProperty<ParticleEmitterComponent>(
              "speedVariation", "Speed Variation", &ParticleEmitterComponent::speedVariation,
              0.0f, 1.0f, "初速度的随机浮动比例；0 表示完全一致", 0.005f),
          makeVec3Property<ParticleEmitterComponent>(
              "emissionOffset", "Emission Offset", &ParticleEmitterComponent::emissionOffset,
              0.02f, "相对实体原点的局部偏移，会随实体旋转"),
          makeFloatProperty<ParticleEmitterComponent>(
              "emissionRadius", "Emission Radius", &ParticleEmitterComponent::emissionRadius,
              0.0f, 10.0f, "在球形范围内随机初始化位置；0 表示从一点发射", 0.01f),
          makeVec3Property<ParticleEmitterComponent>(
              "gravity", "Gravity", &ParticleEmitterComponent::gravity, 0.02f,
              "世界空间重力加速度，单位/秒²"),
          makeFloatProperty<ParticleEmitterComponent>(
              "lifetime", "Lifetime (s)", &ParticleEmitterComponent::lifetime,
              0.05f, 30.0f, "粒子存活时间，单位秒", 0.01f),
          makeFloatProperty<ParticleEmitterComponent>(
              "lifetimeVariation", "Lifetime Variation", &ParticleEmitterComponent::lifetimeVariation,
              0.0f, 1.0f, "寿命的随机浮动比例；0 表示完全一致", 0.005f),
          makeFloatProperty<ParticleEmitterComponent>(
              "startSize", "Start Size", &ParticleEmitterComponent::startSize,
              0.001f, 10.0f, "出生时的世界空间直径", 0.005f),
          makeFloatProperty<ParticleEmitterComponent>(
              "endSize", "End Size", &ParticleEmitterComponent::endSize,
              0.0f, 10.0f, "寿命结束时的世界空间直径", 0.005f),
          makeColorProperty<ParticleEmitterComponent>(
              "startColor", "Start Color", &ParticleEmitterComponent::startColor,
              "出生颜色；alpha 决定初始不透明度"),
          makeColorProperty<ParticleEmitterComponent>(
              "endColor", "End Color", &ParticleEmitterComponent::endColor,
              "寿命结束颜色；alpha 通常设 0 让粒子淡出"),
          // 贴图用 Inspector 专用资源选择器绘制（hidden），避免手输绝对路径。
          makeStringProperty<ParticleEmitterComponent>(
              "texture", "Particle Texture", &ParticleEmitterComponent::texture,
              "粒子 sprite，空表示程序化圆形；路径相对 Resources/Texture，启动时预加载",
              true) }));

    // 局部体积雾：placement（center/size/rotation）由组件自己保存，渲染时再乘
    // 实体 WorldTransform。这样 Inspector 可以直接调“位置 / 体积大小”，不再
    // 依赖用户去 Transform 面板找 Scale。专用面板绘制（drawProperties=false）。
    scene::ComponentOptions fogOptions;
    fogOptions.drawProperties = false;
    fogOptions.requiresTransform = true;
    fogOptions.description =
        "可放置的局部体积雾盒：在 Inspector 里调中心、尺寸与旋转；"
        "实体 Transform 可整体移动/旋转雾盒。最多同时渲染 8 个。";
    registry.add(makeComponentDescriptor<FogVolumeComponent>(
        "effects.fogVolume", "体积雾 (Fog Volume)", "体积效果",
        { makeVec3Property<FogVolumeComponent>(
              "center", "Center (local)", &FogVolumeComponent::center, 0.05f,
              "盒心相对实体原点的局部偏移（米）"),
          withRange(makeVec3Property<FogVolumeComponent>(
                        "size", "Size (m)", &FogVolumeComponent::size, 0.1f,
                        "盒体全尺寸（米）；XYZ 可分别调整"),
                    0.05f, 2000.0f, 0.1f),
          withRange(makeVec3Property<FogVolumeComponent>(
                        "rotation", "Rotation (deg)", &FogVolumeComponent::rotation, 0.5f,
                        "局部欧拉角（度）；与 Transform 的旋转顺序一致"),
                    -360.0f, 360.0f, 0.5f),
          makeBoolProperty<FogVolumeComponent>(
              "enabled", "Enabled", &FogVolumeComponent::enabled,
              "关闭后不参与体积雾计算，保留所有设置"),
          makeFloatProperty<FogVolumeComponent>(
              "density", "Density (1/m)", &FogVolumeComponent::density,
              0.0f, 0.2f, "消光系数；越大越浓，20 米厚时 0.02 约 33% 不透", 0.0005f),
          makeColorProperty<FogVolumeComponent>(
              "color", "Scatter Color", &FogVolumeComponent::color,
              "雾的散射色调；alpha 不参与计算"),
          makeFloatProperty<FogVolumeComponent>(
              "heightFalloff", "Height Falloff (1/m)", &FogVolumeComponent::heightFalloff,
              0.0f, 0.5f, "从盒体中心向上衰减；0 表示整盒均匀", 0.001f),
          makeFloatProperty<FogVolumeComponent>(
              "noiseStrength", "Noise Strength", &FogVolumeComponent::noiseStrength,
              0.0f, 1.0f, "0=均匀雾，1=强团块/丝缕", 0.005f),
          makeFloatProperty<FogVolumeComponent>(
              "noiseScale", "Noise Scale (1/m)", &FogVolumeComponent::noiseScale,
              0.005f, 0.5f, "3D 噪声的世界尺度；越小团块越大", 0.001f),
          makeVec3Property<FogVolumeComponent>(
              "wind", "Wind (m/s)", &FogVolumeComponent::wind, 0.01f,
              "世界空间风速；噪声随时间沿该方向移动"),
          makeFloatProperty<FogVolumeComponent>(
              "edgeSoftness", "Edge Softness", &FogVolumeComponent::edgeSoftness,
              0.01f, 0.5f, "盒体边缘淡出宽度（局部空间，0.5=整个盒体）", 0.002f),
          makeFloatProperty<FogVolumeComponent>(
              "anisotropy", "Anisotropy", &FogVolumeComponent::anisotropy,
              -0.9f, 0.9f, "Henyey-Greenstein 相函数；正值让逆光方向更亮", 0.005f),
          makeFloatProperty<FogVolumeComponent>(
              "ambient", "Ambient", &FogVolumeComponent::ambient,
              0.0f, 2.0f, "环境散射强度；影响背光面亮度", 0.005f),
          makeIntProperty<FogVolumeComponent>(
              "steps", "Steps", &FogVolumeComponent::steps,
              1, 32, "每像素光线步数；越大越平滑，也越贵", 0.5f) },
        fogOptions));
}

void registerFeatureSystems(SystemScheduler& scheduler)
{
    scheduler.add("effects.spin", SystemPhase::Simulation, 100, &spinSystem);
    scheduler.add("effects.orbit", SystemPhase::Simulation, 110, &orbitSystem);
    scheduler.add("effects.bob", SystemPhase::Simulation, 120, &bobSystem);
    scheduler.add("effects.sway", SystemPhase::Simulation, 130, &swaySystem);
    scheduler.add("effects.pulse", SystemPhase::Simulation, 140, &pulseSystem);
    scheduler.add("effects.follow", SystemPhase::Simulation, 150, &followSystem);
    scheduler.add("effects.lookAt", SystemPhase::Simulation, 160, &lookAtSystem);
    // PostTransform：世界变换已经算完，粒子位置才能跟随父节点/旋转。
    scheduler.add("effects.particles", SystemPhase::PostTransform, 200, &particleSystem);
}

} // namespace scene
