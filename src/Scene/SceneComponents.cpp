#include "Scene/SceneComponents.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace scene
{
namespace
{

template <typename T>
PropertyDescriptor boolProperty(std::string name, std::string label, bool T::*member,
                               std::string tooltip)
{
    PropertyDescriptor property = makeProperty<T>(
        std::move(name), std::move(label), PropertyType::Bool,
        [member](const T& component) { return PropertyValue::fromBool(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            component.*member = value.asBool();
        });
    return withTooltip(std::move(property), std::move(tooltip));
}

template <typename T>
PropertyDescriptor floatProperty(std::string name, std::string label, float T::*member,
                                 float minimum, float maximum, float dragSpeed,
                                 std::string tooltip)
{
    PropertyDescriptor property = makeProperty<T>(
        std::move(name), std::move(label), PropertyType::Float,
        [member](const T& component) { return PropertyValue::fromFloat(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            component.*member = value.asFloat();
        });
    property = withRange(std::move(property), minimum, maximum, dragSpeed);
    return withTooltip(std::move(property), std::move(tooltip));
}

template <typename T>
PropertyDescriptor vec3Property(std::string name, std::string label, glm::vec3 T::*member,
                                std::string tooltip)
{
    PropertyDescriptor property = makeProperty<T>(
        std::move(name), std::move(label), PropertyType::Vec3,
        [member](const T& component) { return PropertyValue::fromVec3(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            component.*member = value.asVec3();
        });
    return withTooltip(std::move(property), std::move(tooltip));
}

template <typename T>
PropertyDescriptor colorProperty(std::string name, std::string label, glm::vec4 T::*member,
                                 std::string tooltip)
{
    PropertyDescriptor property = makeProperty<T>(
        std::move(name), std::move(label), PropertyType::Color,
        [member](const T& component) { return PropertyValue::fromColor(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            component.*member = value.asColor();
        });
    return withTooltip(std::move(property), std::move(tooltip));
}

template <typename T>
PropertyDescriptor intProperty(std::string name, std::string label, int T::*member,
                               int minimum, int maximum, std::string tooltip)
{
    PropertyDescriptor property = makeProperty<T>(
        std::move(name), std::move(label), PropertyType::Int,
        [member](const T& component) { return PropertyValue::fromInt(component.*member); },
        [member, minimum, maximum](T& component, const PropertyValue& value)
        {
            component.*member = std::clamp(value.asInt(), minimum, maximum);
        });
    property = withRange(std::move(property), static_cast<float>(minimum),
                         static_cast<float>(maximum), 1.0f);
    return withTooltip(std::move(property), std::move(tooltip));
}

// SSAO 核大小必须是 4..64 的偶数，否则采样循环里会出现 0 长度或半采样。
template <typename T>
PropertyDescriptor kernelProperty(std::string name, std::string label, int T::*member)
{
    PropertyDescriptor property = makeProperty<T>(
        std::move(name), std::move(label), PropertyType::Int,
        [member](const T& component) { return PropertyValue::fromInt(component.*member); },
        [member](T& component, const PropertyValue& value)
        {
            int kernel = std::clamp(value.asInt(), 4, 64);
            if ((kernel % 2) != 0)
                ++kernel;
            component.*member = kernel;
        });
    property = withRange(std::move(property), 4.0f, 64.0f, 1.0f);
    return withTooltip(std::move(property),
                       "采样核大小（4..64 偶数）。越大越平滑，开销也越高");
}

ComponentOptions sceneScopeOptions()
{
    ComponentOptions options;
    options.scope = ComponentScope::Scene;
    return options;
}

} // namespace

void registerSceneComponents(ComponentRegistry& registry)
{
    const ComponentOptions sceneOptions = sceneScopeOptions();

    registry.add(makeComponentDescriptor<SceneEnvironmentComponent>(
        "scene.environment", "场景环境", "场景环境",
        {
            boolProperty<SceneEnvironmentComponent>("enabled", "启用", &SceneEnvironmentComponent::enabled,
                                                   "关闭后曝光与方向光回到引擎默认值"),
            floatProperty<SceneEnvironmentComponent>("exposure", "曝光", &SceneEnvironmentComponent::exposure,
                                                    0.1f, 6.0f, 0.01f,
                                                    "整体亮度。Bloom 在曝光之后，调这里不会让辉光跟着跳"),
            floatProperty<SceneEnvironmentComponent>("renderScale", "渲染分辨率", &SceneEnvironmentComponent::renderScale,
                                                    0.5f, 1.0f, 0.005f,
                                                    "动态分辨率：0.5 = 一半像素渲染再上采样，掉帧时先降这个"),
            vec3Property<SceneEnvironmentComponent>("lightDirection", "方向光方向", &SceneEnvironmentComponent::lightDirection,
                                                    "写回渲染器前会归一化；全零会被忽略"),
            colorProperty<SceneEnvironmentComponent>("lightColor", "方向光颜色", &SceneEnvironmentComponent::lightColor,
                                                     "太阳/主光颜色。Alpha 不使用，强度在下一项"),
            floatProperty<SceneEnvironmentComponent>("lightIntensity", "方向光强度", &SceneEnvironmentComponent::lightIntensity,
                                                     0.0f, 4.0f, 0.01f, "主光强度；0 等于只剩环境光"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneShadowsComponent>(
        "scene.shadows", "阴影", "光照",
        {
            boolProperty<SceneShadowsComponent>("enabled", "启用", &SceneShadowsComponent::enabled,
                                                "关闭后场景不再投射方向光阴影"),
            floatProperty<SceneShadowsComponent>("distance", "阴影距离", &SceneShadowsComponent::distance,
                                                 20.0f, 200.0f, 1.0f,
                                                 "方向光阴影的覆盖半径。越大覆盖越远，单像素精度越低"),
            floatProperty<SceneShadowsComponent>("strength", "阴影强度", &SceneShadowsComponent::strength,
                                                 0.0f, 1.0f, 0.01f,
                                                 "0 = 无阴影，1 = 全黑；默认留一点环境光避免贴纸感"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneBloomComponent>(
        "scene.bloom", "Bloom 辉光", "后处理",
        {
            boolProperty<SceneBloomComponent>("enabled", "启用", &SceneBloomComponent::enabled,
                                              "高亮区域溢出的柔光；关闭后亮部边缘更硬"),
            floatProperty<SceneBloomComponent>("threshold", "阈值", &SceneBloomComponent::threshold,
                                               0.0f, 8.0f, 0.05f, "高于该亮度的像素参与辉光"),
            floatProperty<SceneBloomComponent>("knee", "膝部", &SceneBloomComponent::knee,
                                               0.0f, 1.0f, 0.01f, "阈值附近的柔化范围，避免硬切"),
            floatProperty<SceneBloomComponent>("intensity", "强度", &SceneBloomComponent::intensity,
                                               0.0f, 2.0f, 0.005f, "辉光叠加量"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneSsaoComponent>(
        "scene.ssao", "SSAO 环境遮蔽", "后处理",
        {
            boolProperty<SceneSsaoComponent>("enabled", "启用", &SceneSsaoComponent::enabled,
                                             "屏幕空间环境遮蔽：缝隙/接触处变暗"),
            floatProperty<SceneSsaoComponent>("radius", "半径", &SceneSsaoComponent::radius,
                                              0.05f, 2.0f, 0.01f, "世界空间采样半径"),
            floatProperty<SceneSsaoComponent>("bias", "偏移", &SceneSsaoComponent::bias,
                                              0.0f, 0.2f, 0.001f, "深度比较偏移，太小会出现自遮蔽噪点"),
            kernelProperty<SceneSsaoComponent>("kernelSize", "采样核", &SceneSsaoComponent::kernelSize),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneSsrComponent>(
        "scene.ssr", "SSR 屏幕空间反射", "后处理",
        {
            boolProperty<SceneSsrComponent>("enabled", "启用", &SceneSsrComponent::enabled,
                                            "近场屏幕内反射；屏幕外仍交给平面反射/环境图"),
            floatProperty<SceneSsrComponent>("maxDistance", "最大距离", &SceneSsrComponent::maxDistance,
                                             50.0f, 500.0f, 1.0f, "超过该距离的反射不再计算"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneMotionBlurComponent>(
        "scene.motionBlur", "运动模糊", "后处理",
        {
            boolProperty<SceneMotionBlurComponent>("enabled", "启用", &SceneMotionBlurComponent::enabled,
                                                   "按相机/物体速度做屏幕空间拖影"),
            floatProperty<SceneMotionBlurComponent>("strength", "强度", &SceneMotionBlurComponent::strength,
                                                    0.0f, 3.0f, 0.01f, "拖影长度倍数"),
            floatProperty<SceneMotionBlurComponent>("maxRadius", "最大半径", &SceneMotionBlurComponent::maxRadius,
                                                    0.0f, 0.2f, 0.001f, "屏幕空间拖影上限，避免快速转身糊成一片"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneTaaComponent>(
        "scene.taa", "TAA 时域抗锯齿", "后处理",
        {
            boolProperty<SceneTaaComponent>("enabled", "启用", &SceneTaaComponent::enabled,
                                            "亚像素抖动 + 历史帧混合；远处几何边缘和树叶闪烁主要靠它"),
            floatProperty<SceneTaaComponent>("blend", "历史混合", &SceneTaaComponent::blend,
                                             0.0f, 0.98f, 0.005f, "历史帧权重；越高越稳，运动时可能拖影"),
            floatProperty<SceneTaaComponent>("jitterScale", "抖动缩放", &SceneTaaComponent::jitterScale,
                                             0.0f, 1.5f, 0.005f, "亚像素抖动幅度；0 等于关闭时域累积"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneWaterComponent>(
        "scene.water", "水面", "水体",
        {
            boolProperty<SceneWaterComponent>("enabled", "启用", &SceneWaterComponent::enabled,
                                              "Lighting 之后的透明通道；只有地形/体素存在时才绘制"),
            floatProperty<SceneWaterComponent>("level", "海平面高度", &SceneWaterComponent::level,
                                               -10.0f, 20.0f, 0.05f,
                                               "世界 Y。地形默认 0~25m，2m 会在低洼处形成湖泊"),
            floatProperty<SceneWaterComponent>("waveStrength", "波浪强度", &SceneWaterComponent::waveStrength,
                                               0.0f, 2.0f, 0.01f, "0 = 静止镜面"),
            floatProperty<SceneWaterComponent>("waveSpeed", "波浪速度", &SceneWaterComponent::waveSpeed,
                                               0.0f, 2.0f, 0.01f, "波浪动画速度"),
            boolProperty<SceneWaterComponent>("planarReflection", "平面反射", &SceneWaterComponent::planarReflection,
                                              "半分辨率镜像反射，远水能映出对岸；关闭后只用环境贴图"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneFogComponent>(
        "scene.fog", "距离雾", "后处理",
        {
            boolProperty<SceneFogComponent>("enabled", "启用", &SceneFogComponent::enabled,
                                            "远处地形/水面/玻璃向地平线颜色淡出"),
            floatProperty<SceneFogComponent>("start", "起始距离", &SceneFogComponent::start,
                                             0.0f, 500.0f, 1.0f, "雾开始出现的距离；近处保持完全清晰"),
            floatProperty<SceneFogComponent>("density", "密度", &SceneFogComponent::density,
                                             0.0f, 0.003f, 0.00001f,
                                             "1/m。0.00045 约 1km 处 34%、4km 处 83%；0 等于关闭"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneCloudsComponent>(
        "scene.clouds", "体积云", "大气",
        {
            boolProperty<SceneCloudsComponent>("enabled", "启用", &SceneCloudsComponent::enabled,
                                               "在 Lighting 之后、透明通道之前混入 HDR 的云层"),
            floatProperty<SceneCloudsComponent>("coverage", "云量", &SceneCloudsComponent::coverage,
                                                0.0f, 1.0f, 0.005f,
                                                "天空被云覆盖的比例。0.45 约一半天空有云，1 接近全阴"),
            floatProperty<SceneCloudsComponent>("density", "密度", &SceneCloudsComponent::density,
                                                0.0f, 3.0f, 0.01f,
                                                "消光强度。越大云越厚、越不透光；0 等于没有云"),
            floatProperty<SceneCloudsComponent>("baseHeight", "云底高度", &SceneCloudsComponent::baseHeight,
                                                10.0f, 800.0f, 1.0f,
                                                "云层底部的世界高度（米）。相机需要在这个高度以下才能看到云顶"),
            floatProperty<SceneCloudsComponent>("thickness", "云层厚度", &SceneCloudsComponent::thickness,
                                                5.0f, 500.0f, 1.0f,
                                                "云底到云顶的距离（米）。越厚越容易形成高耸的积云"),
            floatProperty<SceneCloudsComponent>("speed", "风速", &SceneCloudsComponent::speed,
                                                0.0f, 40.0f, 0.1f,
                                                "云层水平漂移速度（米/秒）。只改变采样偏移，不影响几何"),
            floatProperty<SceneCloudsComponent>("absorption", "光吸收", &SceneCloudsComponent::absorption,
                                                0.0f, 3.0f, 0.01f,
                                                "云对阳光的自吸收。越大背光面越暗，0 表示云层不产生自阴影"),
            floatProperty<SceneCloudsComponent>("ambient", "环境光", &SceneCloudsComponent::ambient,
                                                0.0f, 1.0f, 0.01f,
                                                "云层内部的天空散射光。过大会让云失去体积感"),
            intProperty<SceneCloudsComponent>("steps", "步进次数", &SceneCloudsComponent::steps,
                                              8, 128,
                                              "每像素最多步进次数。越大细节越好，开销也越高；48 是默认平衡点"),
            colorProperty<SceneCloudsComponent>("color", "云色", &SceneCloudsComponent::color,
                                                "云体散射的色调；偏向暖白可模拟夕阳，偏蓝可模拟阴天"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneSunComponent>(
        "scene.sun", "太阳光晕", "大气",
        {
            boolProperty<SceneSunComponent>("enabled", "启用", &SceneSunComponent::enabled,
                                           "在 TAA 之后、Bloom 之前叠加太阳盘与镜头光晕"),
            floatProperty<SceneSunComponent>("intensity", "太阳强度", &SceneSunComponent::intensity,
                                             0.0f, 60.0f, 0.1f,
                                             "太阳盘与光晕的整体亮度；会先被色调映射压到 LDR"),
            floatProperty<SceneSunComponent>("angularRadius", "角半径", &SceneSunComponent::angularRadius,
                                             0.05f, 8.0f, 0.01f,
                                             "太阳盘的角半径（度）。真实太阳约 0.27°，0.8° 更接近镜头里的观感"),
            floatProperty<SceneSunComponent>("glow", "日冕强度", &SceneSunComponent::glow,
                                             0.0f, 3.0f, 0.01f,
                                             "盘外柔和光晕的强度。0 只剩硬边日盘"),
            floatProperty<SceneSunComponent>("flareIntensity", "光晕强度", &SceneSunComponent::flareIntensity,
                                             0.0f, 4.0f, 0.01f,
                                             "鬼影和水平光条的整体强度；0 关闭镜头光晕"),
            intProperty<SceneSunComponent>("flareGhosts", "鬼影数量", &SceneSunComponent::flareGhosts,
                                           0, 8,
                                           "沿屏幕中心-太阳连线镜像的鬼影个数，0..8；越多越像广角镜头"),
            boolProperty<SceneSunComponent>("occlusion", "几何遮挡", &SceneSunComponent::occlusion,
                                            "开启后山体/建筑挡住太阳时，日盘与光晕一起淡出"),
            colorProperty<SceneSunComponent>("color", "太阳颜色", &SceneSunComponent::color,
                                             "日盘与光晕的色调；偏暖白适合日出/日落，纯白适合正午"),
        },
        sceneOptions));

    registry.add(makeComponentDescriptor<SceneVegetationComponent>(
        "scene.vegetation", "植被", "地形",
        {
            boolProperty<SceneVegetationComponent>("enabled", "启用", &SceneVegetationComponent::enabled,
                                                   "在地形上程序化生成草簇与低模树，并写入 G-Buffer"),
            floatProperty<SceneVegetationComponent>("grassDensity", "草密度", &SceneVegetationComponent::grassDensity,
                                                    0.0f, 3.0f, 0.05f,
                                                    "每平方米草簇数。0.8 约等于每 1.1m 一簇；0 关闭草"),
            floatProperty<SceneVegetationComponent>("grassDistance", "草距离", &SceneVegetationComponent::grassDistance,
                                                    10.0f, 80.0f, 1.0f,
                                                    "草围绕相机的生成半径（米）。越小越省，但远处会露出裸地"),
            floatProperty<SceneVegetationComponent>("grassHeight", "草高度", &SceneVegetationComponent::grassHeight,
                                                    0.05f, 2.0f, 0.01f,
                                                    "草簇的世界高度（米）"),
            floatProperty<SceneVegetationComponent>("treeDensity", "树密度", &SceneVegetationComponent::treeDensity,
                                                    0.0f, 8.0f, 0.1f,
                                                    "每 1000 平方米树数。1.2 在 120m 半径内约 50 棵；0 关闭树"),
            floatProperty<SceneVegetationComponent>("treeDistance", "树距离", &SceneVegetationComponent::treeDistance,
                                                    20.0f, 200.0f, 5.0f,
                                                    "树围绕相机的生成半径（米）。远处的树会逐渐稀疏"),
            floatProperty<SceneVegetationComponent>("treeHeight", "树高度", &SceneVegetationComponent::treeHeight,
                                                    1.0f, 20.0f, 0.1f,
                                                    "树的世界高度（米）"),
            floatProperty<SceneVegetationComponent>("windStrength", "风力", &SceneVegetationComponent::windStrength,
                                                    0.0f, 2.0f, 0.01f,
                                                    "草叶与树冠的摆动幅度；0 完全静止"),
            intProperty<SceneVegetationComponent>("seed", "随机种子", &SceneVegetationComponent::seed,
                                                  0, 999999,
                                                  "改变草的分布与每簇的形态，不影响地形"),
            colorProperty<SceneVegetationComponent>("grassColor", "草色", &SceneVegetationComponent::grassColor,
                                                    "草叶基色；根部会自动压暗"),
            colorProperty<SceneVegetationComponent>("treeColor", "树冠颜色", &SceneVegetationComponent::treeColor,
                                                    "树冠的基色；树冠内部会根据法线产生明暗"),
            colorProperty<SceneVegetationComponent>("trunkColor", "树干颜色", &SceneVegetationComponent::trunkColor,
                                                    "树干与枝条的基色"),
        },
        sceneOptions));
}

void clearSceneComponents(entt::registry& registry, entt::entity settingsEntity)
{
    if (!registry.valid(settingsEntity))
        return;
    registry.remove<SceneEnvironmentComponent>(settingsEntity);
    registry.remove<SceneShadowsComponent>(settingsEntity);
    registry.remove<SceneBloomComponent>(settingsEntity);
    registry.remove<SceneSsaoComponent>(settingsEntity);
    registry.remove<SceneSsrComponent>(settingsEntity);
    registry.remove<SceneMotionBlurComponent>(settingsEntity);
    registry.remove<SceneTaaComponent>(settingsEntity);
    registry.remove<SceneWaterComponent>(settingsEntity);
    registry.remove<SceneFogComponent>(settingsEntity);
    registry.remove<SceneCloudsComponent>(settingsEntity);
    registry.remove<SceneSunComponent>(settingsEntity);
    registry.remove<SceneVegetationComponent>(settingsEntity);
}

void resetSceneComponentsFromSettings(entt::registry& registry, entt::entity settingsEntity,
                                      const EngineSettings& settings)
{
    if (!registry.valid(settingsEntity))
        return;
    clearSceneComponents(registry, settingsEntity);

    SceneEnvironmentComponent environment;
    environment.exposure = settings.exposure;
    environment.renderScale = settings.renderScale;
    environment.lightDirection = glm::vec3(settings.directionalLightDir);
    environment.lightColor = glm::vec4(glm::vec3(settings.directionalLightColor), 1.0f);
    environment.lightIntensity = settings.directionalLightColor.w;
    registry.emplace<SceneEnvironmentComponent>(settingsEntity, environment);

    SceneShadowsComponent shadows;
    shadows.enabled = settings.shadowEnabled;
    shadows.distance = settings.shadowDistance;
    shadows.strength = settings.shadowStrength;
    registry.emplace<SceneShadowsComponent>(settingsEntity, shadows);

    SceneBloomComponent bloom;
    bloom.enabled = settings.bloomEnabled;
    bloom.threshold = settings.bloomThreshold;
    bloom.knee = settings.bloomKnee;
    bloom.intensity = settings.bloomIntensity;
    registry.emplace<SceneBloomComponent>(settingsEntity, bloom);

    SceneSsaoComponent ssao;
    ssao.enabled = settings.ssaoEnabled;
    ssao.radius = settings.ssaoRadius;
    ssao.bias = settings.ssaoBias;
    ssao.kernelSize = settings.ssaoKernelSize;
    registry.emplace<SceneSsaoComponent>(settingsEntity, ssao);

    SceneSsrComponent ssr;
    ssr.enabled = settings.ssrEnabled;
    ssr.maxDistance = settings.ssrMaxDistance;
    registry.emplace<SceneSsrComponent>(settingsEntity, ssr);

    SceneMotionBlurComponent motionBlur;
    motionBlur.enabled = settings.motionBlurEnabled;
    motionBlur.strength = settings.motionBlurStrength;
    motionBlur.maxRadius = settings.motionBlurMaxRadius;
    registry.emplace<SceneMotionBlurComponent>(settingsEntity, motionBlur);

    SceneTaaComponent taa;
    taa.enabled = settings.taaEnabled;
    taa.blend = settings.taaBlend;
    taa.jitterScale = settings.taaJitterScale;
    registry.emplace<SceneTaaComponent>(settingsEntity, taa);

    SceneWaterComponent water;
    water.enabled = settings.waterEnabled;
    water.level = settings.waterLevel;
    water.waveStrength = settings.waterWaveStrength;
    water.waveSpeed = settings.waterWaveSpeed;
    water.planarReflection = settings.waterPlanarReflection;
    registry.emplace<SceneWaterComponent>(settingsEntity, water);

    SceneFogComponent fog;
    fog.enabled = settings.fogEnabled;
    fog.start = settings.fogStart;
    fog.density = settings.fogDensity;
    registry.emplace<SceneFogComponent>(settingsEntity, fog);

    SceneCloudsComponent clouds;
    clouds.enabled = settings.cloudEnabled;
    clouds.coverage = settings.cloudCoverage;
    clouds.density = settings.cloudDensity;
    clouds.baseHeight = settings.cloudBaseHeight;
    clouds.thickness = settings.cloudThickness;
    clouds.speed = settings.cloudSpeed;
    clouds.absorption = settings.cloudAbsorption;
    clouds.ambient = settings.cloudAmbient;
    clouds.steps = settings.cloudSteps;
    clouds.color = settings.cloudColor;
    registry.emplace<SceneCloudsComponent>(settingsEntity, clouds);

    SceneSunComponent sun;
    sun.enabled = settings.sunEnabled;
    sun.intensity = settings.sunIntensity;
    sun.angularRadius = settings.sunAngularRadius;
    sun.glow = settings.sunGlow;
    sun.flareIntensity = settings.sunFlareIntensity;
    sun.flareGhosts = settings.sunFlareGhosts;
    sun.occlusion = settings.sunOcclusion;
    sun.color = settings.sunColor;
    registry.emplace<SceneSunComponent>(settingsEntity, sun);

    SceneVegetationComponent vegetation;
    vegetation.enabled = settings.vegetationEnabled;
    vegetation.grassDensity = settings.vegetationGrassDensity;
    vegetation.grassDistance = settings.vegetationGrassDistance;
    vegetation.grassHeight = settings.vegetationGrassHeight;
    vegetation.treeDensity = settings.vegetationTreeDensity;
    vegetation.treeDistance = settings.vegetationTreeDistance;
    vegetation.treeHeight = settings.vegetationTreeHeight;
    vegetation.windStrength = settings.vegetationWindStrength;
    vegetation.seed = settings.vegetationSeed;
    vegetation.grassColor = settings.vegetationGrassColor;
    vegetation.treeColor = settings.vegetationTreeColor;
    vegetation.trunkColor = settings.vegetationTrunkColor;
    registry.emplace<SceneVegetationComponent>(settingsEntity, vegetation);
}

void syncSceneSettings(entt::registry& registry, entt::entity settingsEntity,
                       EngineSettings& settings)
{
    if (!registry.valid(settingsEntity))
        return;

    const EngineSettings defaults;

    // 先把本函数拥有的字段全部恢复默认，再让“存在的组件”覆盖。
    // 这样移除组件不是留下脏值，而是确定性地关闭对应功能。
    settings.exposure = defaults.exposure;
    settings.renderScale = defaults.renderScale;
    settings.directionalLightDir = defaults.directionalLightDir;
    settings.directionalLightColor = defaults.directionalLightColor;

    settings.shadowEnabled = false;
    settings.shadowDistance = defaults.shadowDistance;
    settings.shadowStrength = defaults.shadowStrength;

    settings.bloomEnabled = false;
    settings.bloomThreshold = defaults.bloomThreshold;
    settings.bloomKnee = defaults.bloomKnee;
    settings.bloomIntensity = defaults.bloomIntensity;

    settings.ssaoEnabled = false;
    settings.ssaoRadius = defaults.ssaoRadius;
    settings.ssaoBias = defaults.ssaoBias;
    settings.ssaoKernelSize = defaults.ssaoKernelSize;

    settings.ssrEnabled = false;
    settings.ssrMaxDistance = defaults.ssrMaxDistance;

    settings.motionBlurEnabled = false;
    settings.motionBlurStrength = defaults.motionBlurStrength;
    settings.motionBlurMaxRadius = defaults.motionBlurMaxRadius;

    settings.taaEnabled = false;
    settings.taaBlend = defaults.taaBlend;
    settings.taaJitterScale = defaults.taaJitterScale;

    settings.waterEnabled = false;
    settings.waterLevel = defaults.waterLevel;
    settings.waterWaveStrength = defaults.waterWaveStrength;
    settings.waterWaveSpeed = defaults.waterWaveSpeed;
    settings.waterPlanarReflection = defaults.waterPlanarReflection;

    settings.fogEnabled = false;
    settings.fogStart = defaults.fogStart;
    settings.fogDensity = defaults.fogDensity;

    settings.cloudEnabled = defaults.cloudEnabled;
    settings.cloudCoverage = defaults.cloudCoverage;
    settings.cloudDensity = defaults.cloudDensity;
    settings.cloudBaseHeight = defaults.cloudBaseHeight;
    settings.cloudThickness = defaults.cloudThickness;
    settings.cloudSpeed = defaults.cloudSpeed;
    settings.cloudAbsorption = defaults.cloudAbsorption;
    settings.cloudAmbient = defaults.cloudAmbient;
    settings.cloudSteps = defaults.cloudSteps;
    settings.cloudColor = defaults.cloudColor;

    settings.sunEnabled = defaults.sunEnabled;
    settings.sunIntensity = defaults.sunIntensity;
    settings.sunAngularRadius = defaults.sunAngularRadius;
    settings.sunGlow = defaults.sunGlow;
    settings.sunFlareIntensity = defaults.sunFlareIntensity;
    settings.sunFlareGhosts = defaults.sunFlareGhosts;
    settings.sunOcclusion = defaults.sunOcclusion;
    settings.sunColor = defaults.sunColor;

    settings.vegetationEnabled = defaults.vegetationEnabled;
    settings.vegetationGrassDensity = defaults.vegetationGrassDensity;
    settings.vegetationGrassDistance = defaults.vegetationGrassDistance;
    settings.vegetationGrassHeight = defaults.vegetationGrassHeight;
    settings.vegetationTreeDensity = defaults.vegetationTreeDensity;
    settings.vegetationTreeDistance = defaults.vegetationTreeDistance;
    settings.vegetationTreeHeight = defaults.vegetationTreeHeight;
    settings.vegetationWindStrength = defaults.vegetationWindStrength;
    settings.vegetationSeed = defaults.vegetationSeed;
    settings.vegetationGrassColor = defaults.vegetationGrassColor;
    settings.vegetationTreeColor = defaults.vegetationTreeColor;
    settings.vegetationTrunkColor = defaults.vegetationTrunkColor;

    if (const auto* environment = registry.try_get<SceneEnvironmentComponent>(settingsEntity);
        environment != nullptr && environment->enabled)
    {
        settings.exposure = environment->exposure;
        settings.renderScale = environment->renderScale;
        glm::vec3 direction = environment->lightDirection;
        const float length = glm::length(direction);
        if (length > 1e-4f)
            direction /= length;
        else
            direction = glm::vec3(defaults.directionalLightDir);
        settings.directionalLightDir = glm::vec4(direction, 0.0f);
        settings.directionalLightColor = glm::vec4(
            environment->lightColor.r, environment->lightColor.g,
            environment->lightColor.b, environment->lightIntensity);
    }

    if (const auto* shadows = registry.try_get<SceneShadowsComponent>(settingsEntity);
        shadows != nullptr && shadows->enabled)
    {
        settings.shadowEnabled = true;
        settings.shadowDistance = shadows->distance;
        settings.shadowStrength = shadows->strength;
    }

    if (const auto* bloom = registry.try_get<SceneBloomComponent>(settingsEntity);
        bloom != nullptr && bloom->enabled)
    {
        settings.bloomEnabled = true;
        settings.bloomThreshold = bloom->threshold;
        settings.bloomKnee = bloom->knee;
        settings.bloomIntensity = bloom->intensity;
    }

    if (const auto* ssao = registry.try_get<SceneSsaoComponent>(settingsEntity);
        ssao != nullptr && ssao->enabled)
    {
        settings.ssaoEnabled = true;
        settings.ssaoRadius = ssao->radius;
        settings.ssaoBias = ssao->bias;
        settings.ssaoKernelSize = ssao->kernelSize;
    }

    if (const auto* ssr = registry.try_get<SceneSsrComponent>(settingsEntity);
        ssr != nullptr && ssr->enabled)
    {
        settings.ssrEnabled = true;
        settings.ssrMaxDistance = ssr->maxDistance;
    }

    if (const auto* motionBlur = registry.try_get<SceneMotionBlurComponent>(settingsEntity);
        motionBlur != nullptr && motionBlur->enabled)
    {
        settings.motionBlurEnabled = true;
        settings.motionBlurStrength = motionBlur->strength;
        settings.motionBlurMaxRadius = motionBlur->maxRadius;
    }

    if (const auto* taa = registry.try_get<SceneTaaComponent>(settingsEntity);
        taa != nullptr && taa->enabled)
    {
        settings.taaEnabled = true;
        settings.taaBlend = taa->blend;
        settings.taaJitterScale = taa->jitterScale;
    }

    if (const auto* water = registry.try_get<SceneWaterComponent>(settingsEntity);
        water != nullptr && water->enabled)
    {
        settings.waterEnabled = true;
        settings.waterLevel = water->level;
        settings.waterWaveStrength = water->waveStrength;
        settings.waterWaveSpeed = water->waveSpeed;
        settings.waterPlanarReflection = water->planarReflection;
    }

    if (const auto* fog = registry.try_get<SceneFogComponent>(settingsEntity);
        fog != nullptr && fog->enabled)
    {
        settings.fogEnabled = true;
        settings.fogStart = fog->start;
        settings.fogDensity = fog->density;
    }

    if (const auto* clouds = registry.try_get<SceneCloudsComponent>(settingsEntity);
        clouds != nullptr && clouds->enabled)
    {
        settings.cloudEnabled = true;
        settings.cloudCoverage = clouds->coverage;
        settings.cloudDensity = clouds->density;
        settings.cloudBaseHeight = clouds->baseHeight;
        settings.cloudThickness = clouds->thickness;
        settings.cloudSpeed = clouds->speed;
        settings.cloudAbsorption = clouds->absorption;
        settings.cloudAmbient = clouds->ambient;
        settings.cloudSteps = clouds->steps;
        settings.cloudColor = clouds->color;
    }

    if (const auto* sun = registry.try_get<SceneSunComponent>(settingsEntity);
        sun != nullptr && sun->enabled)
    {
        settings.sunEnabled = true;
        settings.sunIntensity = sun->intensity;
        settings.sunAngularRadius = sun->angularRadius;
        settings.sunGlow = sun->glow;
        settings.sunFlareIntensity = sun->flareIntensity;
        settings.sunFlareGhosts = sun->flareGhosts;
        settings.sunOcclusion = sun->occlusion;
        settings.sunColor = sun->color;
    }

    if (const auto* vegetation = registry.try_get<SceneVegetationComponent>(settingsEntity);
        vegetation != nullptr && vegetation->enabled)
    {
        settings.vegetationEnabled = true;
        settings.vegetationGrassDensity = vegetation->grassDensity;
        settings.vegetationGrassDistance = vegetation->grassDistance;
        settings.vegetationGrassHeight = vegetation->grassHeight;
        settings.vegetationTreeDensity = vegetation->treeDensity;
        settings.vegetationTreeDistance = vegetation->treeDistance;
        settings.vegetationTreeHeight = vegetation->treeHeight;
        settings.vegetationWindStrength = vegetation->windStrength;
        settings.vegetationSeed = vegetation->seed;
        settings.vegetationGrassColor = vegetation->grassColor;
        settings.vegetationTreeColor = vegetation->treeColor;
        settings.vegetationTrunkColor = vegetation->trunkColor;
    }
}

} // namespace scene
