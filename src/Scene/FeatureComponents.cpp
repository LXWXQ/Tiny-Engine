#include "Scene/FeatureComponents.h"

#include "Scene/Components.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

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

} // namespace

void registerFeatureComponents(ComponentRegistry& registry)
{
    registry.add(makeComponentDescriptor<SpinComponent>(
        "effects.spin", "Spin", "Effects",
        { withTooltip(makeSpinAxisProperty(), "局部旋转轴；零向量表示不旋转"),
          withTooltip(withRange(makeSpinSpeedProperty(), -3600.0f, 3600.0f, 0.5f),
                      "局部空间自转速度，单位度/秒；负值反向"),
          withTooltip(makeSpinEnabledProperty(), "关闭后系统跳过该组件，但保留设置") }));

    registry.add(makeComponentDescriptor<OrbitComponent>(
        "effects.orbit", "Orbit", "Effects",
        { withTooltip(makeOrbitCenterProperty(), "父空间里的轨道中心"),
          withTooltip(withRange(makeOrbitRadiusProperty(), 0.0f, 1000.0f, 0.05f),
                      "父空间轨道半径；0 表示绕中心原地旋转"),
          withTooltip(withRange(makeOrbitSpeedProperty(), -3600.0f, 3600.0f, 0.5f),
                      "绕 Y 轴公转速度，单位度/秒；负值反向"),
          withTooltip(withRange(makeOrbitAngleProperty(), -360.0f, 360.0f, 0.5f),
                      "当前公转角，单位度；系统会按 360° 回绕"),
          withTooltip(makeOrbitEnabledProperty(), "关闭后系统跳过该组件，但保留设置") }));

    registry.add(makeComponentDescriptor<BobComponent>(
        "effects.bob", "Bob", "Effects",
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
        "effects.pulse", "Pulse", "Effects",
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
        "effects.sway", "Sway", "Effects",
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
        "effects.lookAt", "Look At", "Motion",
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
        "effects.follow", "Follow", "Motion",
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
}

} // namespace scene
