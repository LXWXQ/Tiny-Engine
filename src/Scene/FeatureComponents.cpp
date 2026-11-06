#include "Scene/FeatureComponents.h"

#include "Scene/Components.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>

#include <cmath>

namespace scene
{

namespace
{

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
            component.degreesPerSecond = value.asFloat();
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
            component.radius = std::max(0.0f, value.asFloat());
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
            component.degreesPerSecond = value.asFloat();
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

} // namespace

void registerFeatureComponents(ComponentRegistry& registry)
{
    registry.add(makeComponentDescriptor<SpinComponent>(
        "effects.spin", "Spin", "Effects",
        { makeSpinAxisProperty(), makeSpinSpeedProperty(), makeSpinEnabledProperty() }));

    registry.add(makeComponentDescriptor<OrbitComponent>(
        "effects.orbit", "Orbit", "Effects",
        { makeOrbitCenterProperty(), makeOrbitRadiusProperty(), makeOrbitSpeedProperty(),
          makeOrbitAngleProperty(), makeOrbitEnabledProperty() }));
}

void registerFeatureSystems(SystemScheduler& scheduler)
{
    scheduler.add("effects.spin", SystemPhase::Simulation, 100, &spinSystem);
    scheduler.add("effects.orbit", SystemPhase::Simulation, 110, &orbitSystem);
}

} // namespace scene
