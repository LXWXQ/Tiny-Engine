#include "Editor/SceneSerializer.h"
#include "AssetPaths.h"

#include "Voxel/VoxelTypes.h"

#include <json.hpp>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace editor
{

namespace
{
// v2（2026-11-04）：新增 voxelWorlds（seed + 差分）。
// v3（2026-11-06）：每个实体新增可选 components（元数据注册的扩展组件）。
// 旧文件缺这一段时按空处理，所以读取端不按版本号拒绝，只有写入端跟着升版本。
constexpr int kCurrentVersion = 3;

std::string toRelative(const std::string& absolutePath)
{
    if (absolutePath.empty())
        return {};
    const std::string root = TINY_RESOURCE_DIR;
    if (absolutePath.rfind(root, 0) == 0)
        return absolutePath.substr(root.size());
    return absolutePath;
}

std::string toAbsolute(const std::string& stored)
{
    if (stored.empty())
        return {};
    if (stored.size() > 1 && (stored[1] == ':' || stored[0] == '/' || stored[0] == '\\'))
        return stored;
    return std::string(TINY_RESOURCE_DIR) + stored;
}

json toJson(const glm::vec3& v) { return json::array({ v.x, v.y, v.z }); }
json toJson(const glm::vec4& v) { return json::array({ v.x, v.y, v.z, v.w }); }

glm::vec3 vec3From(const json& j, const glm::vec3& fallback)
{
    if (!j.is_array() || j.size() != 3) return fallback;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

glm::vec4 vec4From(const json& j, const glm::vec4& fallback)
{
    if (!j.is_array() || j.size() != 4) return fallback;
    return glm::vec4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>());
}

const char* propertyTypeName(scene::PropertyType type)
{
    switch (type)
    {
    case scene::PropertyType::Bool:   return "bool";
    case scene::PropertyType::Int:    return "int";
    case scene::PropertyType::Float:  return "float";
    case scene::PropertyType::Vec3:   return "vec3";
    case scene::PropertyType::Color:  return "color";
    case scene::PropertyType::String: return "string";
    }
    return "float";
}

bool propertyTypeFromName(const std::string& name, scene::PropertyType& outType)
{
    if (name == "bool")   { outType = scene::PropertyType::Bool;   return true; }
    if (name == "int")    { outType = scene::PropertyType::Int;    return true; }
    if (name == "float")  { outType = scene::PropertyType::Float;  return true; }
    if (name == "vec3")   { outType = scene::PropertyType::Vec3;   return true; }
    if (name == "color")  { outType = scene::PropertyType::Color;  return true; }
    if (name == "string") { outType = scene::PropertyType::String; return true; }
    return false;
}

json propertyValueToJson(const scene::PropertyValue& value)
{
    switch (value.type)
    {
    case scene::PropertyType::Bool:   return json(value.boolValue);
    case scene::PropertyType::Int:    return json(value.intValue);
    case scene::PropertyType::Float:  return json(value.floatValue);
    case scene::PropertyType::Vec3:   return toJson(value.vec3);
    case scene::PropertyType::Color:  return toJson(value.color);
    case scene::PropertyType::String: return json(value.stringValue);
    }
    return json(nullptr);
}

// 坏属性只跳过自己；组件类型 / 组件名坏掉则跳过整个组件。
bool propertyValueFromJson(const json& j, scene::PropertyType type, scene::PropertyValue& outValue)
{
    switch (type)
    {
    case scene::PropertyType::Bool:
        if (!j.is_boolean()) return false;
        outValue = scene::PropertyValue::fromBool(j.get<bool>());
        return true;
    case scene::PropertyType::Int:
        if (!j.is_number_integer()) return false;
        outValue = scene::PropertyValue::fromInt(j.get<int>());
        return true;
    case scene::PropertyType::Float:
        if (!j.is_number()) return false;
        outValue = scene::PropertyValue::fromFloat(j.get<float>());
        return true;
    case scene::PropertyType::Vec3:
    {
        if (!j.is_array() || j.size() != 3) return false;
        outValue = scene::PropertyValue::fromVec3(
            glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>()));
        return true;
    }
    case scene::PropertyType::Color:
    {
        if (!j.is_array() || j.size() != 4) return false;
        outValue = scene::PropertyValue::fromColor(glm::vec4(
            j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()));
        return true;
    }
    case scene::PropertyType::String:
        if (!j.is_string()) return false;
        outValue = scene::PropertyValue::fromString(j.get<std::string>());
        return true;
    }
    return false;
}

void readTerrainConfig(const json& t, terrain::TerrainConfig& cfg)
{
    cfg.seed = static_cast<uint32_t>(t.value("seed", cfg.seed));
    cfg.resolutionX = static_cast<uint32_t>(t.value("resolutionX", cfg.resolutionX));
    cfg.resolutionZ = static_cast<uint32_t>(t.value("resolutionZ", cfg.resolutionZ));
    cfg.sizeX = t.value("sizeX", cfg.sizeX);
    cfg.sizeZ = t.value("sizeZ", cfg.sizeZ);
    cfg.sampleOriginX = t.value("sampleOriginX", cfg.sampleOriginX);
    cfg.sampleOriginZ = t.value("sampleOriginZ", cfg.sampleOriginZ);
    cfg.baseHeight = t.value("baseHeight", cfg.baseHeight);
    cfg.heightScale = t.value("heightScale", cfg.heightScale);
    cfg.noiseFrequency = t.value("noiseFrequency", cfg.noiseFrequency);
    cfg.octaveCount = static_cast<uint32_t>(t.value("octaveCount", cfg.octaveCount));
    cfg.persistence = t.value("persistence", cfg.persistence);
    cfg.lacunarity = t.value("lacunarity", cfg.lacunarity);
    cfg.ridgeWeight = t.value("ridgeWeight", cfg.ridgeWeight);
    cfg.detailWeight = t.value("detailWeight", cfg.detailWeight);
    cfg.edgeFalloff = t.value("edgeFalloff", cfg.edgeFalloff);
    cfg.heightCurve = t.value("heightCurve", cfg.heightCurve);
    cfg.islandWeight = t.value("islandWeight", cfg.islandWeight);
    cfg.islandRadius = t.value("islandRadius", cfg.islandRadius);
    cfg.ridgeSharpness = t.value("ridgeSharpness", cfg.ridgeSharpness);
    cfg.detailFrequency = t.value("detailFrequency", cfg.detailFrequency);
    cfg.materialDetailStrength = t.value("materialDetailStrength", cfg.materialDetailStrength);
    cfg.uvTiling = t.value("uvTiling", cfg.uvTiling);
    cfg.localColorRange = t.value("localColorRange", cfg.localColorRange);
    cfg.skirtsEnabled = t.value("skirtsEnabled", cfg.skirtsEnabled);
    cfg.chunkGridX = static_cast<uint32_t>(t.value("chunkGridX", cfg.chunkGridX));
    cfg.chunkGridZ = static_cast<uint32_t>(t.value("chunkGridZ", cfg.chunkGridZ));
    cfg.lodLevelCount = static_cast<uint32_t>(t.value("lodLevelCount", cfg.lodLevelCount));
    cfg.lodDistanceScale = t.value("lodDistanceScale", cfg.lodDistanceScale);
    cfg.surfaceStyle = static_cast<terrain::TerrainSurfaceStyle>(
        t.value("surfaceStyle", static_cast<uint32_t>(cfg.surfaceStyle)));
    cfg.materialSetIndex = t.value("materialSetIndex", cfg.materialSetIndex);
}

json writeTerrainConfig(const terrain::TerrainConfig& cfg)
{
    json t;
    t["version"] = 5;
    t["seed"] = cfg.seed;
    t["resolutionX"] = cfg.resolutionX;
    t["resolutionZ"] = cfg.resolutionZ;
    t["sizeX"] = cfg.sizeX;
    t["sizeZ"] = cfg.sizeZ;
    t["sampleOriginX"] = cfg.sampleOriginX;
    t["sampleOriginZ"] = cfg.sampleOriginZ;
    t["baseHeight"] = cfg.baseHeight;
    t["heightScale"] = cfg.heightScale;
    t["noiseFrequency"] = cfg.noiseFrequency;
    t["octaveCount"] = cfg.octaveCount;
    t["persistence"] = cfg.persistence;
    t["lacunarity"] = cfg.lacunarity;
    t["ridgeWeight"] = cfg.ridgeWeight;
    t["detailWeight"] = cfg.detailWeight;
    t["edgeFalloff"] = cfg.edgeFalloff;
    t["heightCurve"] = cfg.heightCurve;
    t["islandWeight"] = cfg.islandWeight;
    t["islandRadius"] = cfg.islandRadius;
    t["ridgeSharpness"] = cfg.ridgeSharpness;
    t["detailFrequency"] = cfg.detailFrequency;
    t["materialDetailStrength"] = cfg.materialDetailStrength;
    t["uvTiling"] = cfg.uvTiling;
    t["localColorRange"] = cfg.localColorRange;
    t["skirtsEnabled"] = cfg.skirtsEnabled;
    t["chunkGridX"] = cfg.chunkGridX;
    t["chunkGridZ"] = cfg.chunkGridZ;
    t["lodLevelCount"] = cfg.lodLevelCount;
    t["lodDistanceScale"] = cfg.lodDistanceScale;
    t["surfaceStyle"] = static_cast<uint32_t>(cfg.surfaceStyle);
    t["materialSetIndex"] = cfg.materialSetIndex;
    return t;
}

scene::SceneDescription fromJson(const json& root)
{
    scene::SceneDescription description;
    description.version = root.value("version", kCurrentVersion);

    if (!root.contains("entities") || !root["entities"].is_array())
        return description;

    for (const json& e : root["entities"])
    {
        scene::EntityDescription entity;
        entity.name = e.value("name", std::string("Entity"));
        entity.translation = vec3From(e.value("translation", json::array()), glm::vec3(0.0f));
        entity.rotation = vec3From(e.value("rotation", json::array()), glm::vec3(0.0f));
        entity.scale = vec3From(e.value("scale", json::array()), glm::vec3(1.0f));
        entity.parentIndex = e.value("parent", -1);
        entity.meshPath = toAbsolute(e.value("mesh", std::string()));
        entity.isSkybox = e.value("skybox", false);

        entity.isGrid = e.value("grid", false);

        entity.isCamera = e.value("camera", false);



        // Terrain exists only as an optional parameter object. Unknown fields
        // and old scene files without it continue to load with defaults.
        if (e.contains("terrain") && e["terrain"].is_object())
        {
            entity.isTerrain = true;
            readTerrainConfig(e["terrain"], entity.terrainConfig);
        }

        if (e.contains("terrainWorld") && e["terrainWorld"].is_object())
        {
            const json& w = e["terrainWorld"];
            entity.isTerrainWorld = true;
            readTerrainConfig(w.value("chunkConfig", json::object()), entity.terrainWorldConfig);
            entity.terrainLoadRadius = w.value("loadRadius", entity.terrainLoadRadius);
            entity.terrainUnloadPadding = w.value("unloadPadding", entity.terrainUnloadPadding);
            entity.terrainMaxLoadsPerFrame = static_cast<uint32_t>(
                w.value("maxLoadsPerFrame", entity.terrainMaxLoadsPerFrame));
        }

        if (e.contains("character") && e["character"].is_object())
        {
            const json& c = e["character"];
            entity.isCharacter = true;
            entity.characterRadius = c.value("radius", entity.characterRadius);
            entity.characterHeight = c.value("height", entity.characterHeight);
            entity.characterMoveSpeed = c.value("moveSpeed", entity.characterMoveSpeed);
            entity.characterJumpSpeed = c.value("jumpSpeed", entity.characterJumpSpeed);
            entity.characterGravity = c.value("gravity", entity.characterGravity);
            entity.characterMaxSlopeAngle = c.value("maxSlopeAngle", entity.characterMaxSlopeAngle);
            entity.characterStepHeight = c.value("stepHeight", entity.characterStepHeight);
            entity.characterGroundSnapDistance = c.value("groundSnapDistance", entity.characterGroundSnapDistance);
        }

        if (e.contains("material") && e["material"].is_object())
        {
            const json& m = e["material"];
            entity.material.present = true;
            entity.material.name = m.value("name", std::string("material"));
            entity.material.baseColorFactor = vec4From(m.value("baseColorFactor", json::array()), glm::vec4(1.0f));
            entity.material.metallicFactor = m.value("metallicFactor", 1.0f);
            entity.material.roughnessFactor = m.value("roughnessFactor", 1.0f);
            entity.material.alphaCutoff = m.value("alphaCutoff", 0.5f);
            entity.material.alphaMode = m.value("alphaMode", 0);
            entity.material.albedoMap = toAbsolute(m.value("albedoMap", std::string()));
            entity.material.normalMap = toAbsolute(m.value("normalMap", std::string()));
            entity.material.metallicRoughnessMap = toAbsolute(m.value("metallicRoughnessMap", std::string()));
        }

        // v3：元数据注册的扩展组件。这里只做语法层的值还原；
        // “这个组件类型是否仍然注册”由 Scene::buildFromDescription() 判断，
        // 未知组件会被安全跳过，不影响场景其余部分。
        if (e.contains("components") && e["components"].is_array())
        {
            for (const json& c : e["components"])
            {
                if (!c.is_object())
                    continue;
                scene::ComponentRecord record;
                record.type = c.value("type", std::string());
                if (record.type.empty())
                    continue;
                if (c.contains("properties") && c["properties"].is_array())
                {
                    for (const json& p : c["properties"])
                    {
                        if (!p.is_object())
                            continue;
                        const std::string propertyName = p.value("name", std::string());
                        if (propertyName.empty())
                            continue;
                        scene::PropertyType type = scene::PropertyType::Float;
                        if (!propertyTypeFromName(p.value("type", std::string()), type))
                            continue;
                        if (!p.contains("value"))
                            continue;
                        scene::NamedProperty property;
                        property.name = propertyName;
                        property.label = propertyName;
                        if (!propertyValueFromJson(p["value"], type, property.value))
                            continue;
                        record.properties.push_back(std::move(property));
                    }
                }
                entity.components.push_back(std::move(record));
            }
        }

        description.entities.push_back(std::move(entity));
    }

    // F3：体素世界（可选字段）。任何一格坏数据都跳过，不让整个场景载入失败。
    if (root.contains("voxelWorlds") && root["voxelWorlds"].is_array())
    {
        for (const json& w : root["voxelWorlds"])
        {
            if (!w.is_object())
                continue;
            scene::VoxelWorldDescription world;
            world.seed = w.value("seed", world.seed);
            world.radiusChunks = w.value("radiusChunks", world.radiusChunks);
            world.baseY = w.value("baseY", world.baseY);
            if (w.contains("chunks") && w["chunks"].is_array())
            {
                for (const json& c : w["chunks"])
                {
                    if (!c.is_object())
                        continue;
                    scene::VoxelChunkDescription chunk;
                    chunk.chunkX = c.value("chunkX", 0);
                    chunk.chunkZ = c.value("chunkZ", 0);
                    if (c.contains("edits") && c["edits"].is_array())
                    {
                        for (const json& e : c["edits"])
                        {
                            if (!e.is_array() || e.size() != 4)
                                continue;
                            scene::VoxelBlockEdit edit;
                            edit.x = e[0].get<int32_t>();
                            edit.y = e[1].get<int32_t>();
                            edit.z = e[2].get<int32_t>();
                            const int block = e[3].get<int>();
                            edit.block = static_cast<uint8_t>(
                                std::clamp(block, 0, static_cast<int>(voxel::kBlockCount) - 1));
                            chunk.edits.push_back(edit);
                        }
                    }
                    if (!chunk.edits.empty())
                        world.chunks.push_back(std::move(chunk));
                }
            }
            description.voxelWorlds.push_back(std::move(world));
        }
    }

    return description;
}

json toJson(const scene::SceneDescription& description)
{
    json root;
    root["version"] = kCurrentVersion;
    json entities = json::array();

    for (const scene::EntityDescription& e : description.entities)
    {
        json j;
        j["name"] = e.name;
        j["translation"] = toJson(e.translation);
        j["rotation"] = toJson(e.rotation);
        j["scale"] = toJson(e.scale);
        j["parent"] = e.parentIndex;

        if (!e.isTerrain && !e.meshPath.empty())
            j["mesh"] = toRelative(e.meshPath);
        if (e.isSkybox) j["skybox"] = true;
        if (e.isGrid)   j["grid"] = true;
        if (e.isCamera) j["camera"] = true;



        if (e.isTerrain)
        {
            j["terrain"] = writeTerrainConfig(e.terrainConfig);
        }
        if (e.isTerrainWorld)
        {
            json w;
            w["chunkConfig"] = writeTerrainConfig(e.terrainWorldConfig);
            w["loadRadius"] = e.terrainLoadRadius;
            w["unloadPadding"] = e.terrainUnloadPadding;
            w["maxLoadsPerFrame"] = e.terrainMaxLoadsPerFrame;
            j["terrainWorld"] = std::move(w);
        }

        if (e.isCharacter)
        {
            json c;
            c["radius"] = e.characterRadius;
            c["height"] = e.characterHeight;
            c["moveSpeed"] = e.characterMoveSpeed;
            c["jumpSpeed"] = e.characterJumpSpeed;
            c["gravity"] = e.characterGravity;
            c["maxSlopeAngle"] = e.characterMaxSlopeAngle;
            c["stepHeight"] = e.characterStepHeight;
            c["groundSnapDistance"] = e.characterGroundSnapDistance;
            j["character"] = std::move(c);
        }

        if (e.material.present)
        {
            json m;
            m["name"] = e.material.name;
            m["baseColorFactor"] = toJson(e.material.baseColorFactor);
            m["metallicFactor"] = e.material.metallicFactor;
            m["roughnessFactor"] = e.material.roughnessFactor;
            m["alphaCutoff"] = e.material.alphaCutoff;
            m["alphaMode"] = e.material.alphaMode;
            if (!e.material.albedoMap.empty())
                m["albedoMap"] = toRelative(e.material.albedoMap);
            if (!e.material.normalMap.empty())
                m["normalMap"] = toRelative(e.material.normalMap);
            if (!e.material.metallicRoughnessMap.empty())
                m["metallicRoughnessMap"] = toRelative(e.material.metallicRoughnessMap);
            j["material"] = m;
        }

        // v3：只写元数据注册的组件。Scene::captureDescription() 已经把
        // 非注册的运行期组件过滤掉了。label 不写盘 —— 它由注册表重新生成。
        if (!e.components.empty())
        {
            json components = json::array();
            for (const scene::ComponentRecord& record : e.components)
            {
                json c;
                c["type"] = record.type;
                json properties = json::array();
                for (const scene::NamedProperty& property : record.properties)
                {
                    json p;
                    p["name"] = property.name;
                    p["type"] = propertyTypeName(property.value.type);
                    p["value"] = propertyValueToJson(property.value);
                    properties.push_back(std::move(p));
                }
                c["properties"] = std::move(properties);
                components.push_back(std::move(c));
            }
            j["components"] = std::move(components);
        }

        entities.push_back(std::move(j));
    }

    root["entities"] = std::move(entities);

    // F3：体素世界 = seed + 与按 seed 重新生成的差分。
    // edits 写成 [x, y, z, block] 四元组，比对象数组省一半体积。
    if (!description.voxelWorlds.empty())
    {
        json worlds = json::array();
        for (const scene::VoxelWorldDescription& world : description.voxelWorlds)
        {
            json w;
            w["seed"] = world.seed;
            w["radiusChunks"] = world.radiusChunks;
            w["baseY"] = world.baseY;
            json chunks = json::array();
            for (const scene::VoxelChunkDescription& chunk : world.chunks)
            {
                if (chunk.edits.empty())
                    continue;
                json c;
                c["chunkX"] = chunk.chunkX;
                c["chunkZ"] = chunk.chunkZ;
                json edits = json::array();
                for (const scene::VoxelBlockEdit& edit : chunk.edits)
                    edits.push_back(json::array({ edit.x, edit.y, edit.z, edit.block }));
                c["edits"] = std::move(edits);
                chunks.push_back(std::move(c));
            }
            w["chunks"] = std::move(chunks);
            worlds.push_back(std::move(w));
        }
        root["voxelWorlds"] = std::move(worlds);
    }
    return root;
}
} // namespace

bool SceneSerializer::saveToFile(const scene::SceneDescription& description,
                                 const std::string& filepath,
                                 std::string& errorMessage)
{
    errno = 0;
    std::ofstream out(filepath, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        const int openError = errno;
        errorMessage = "cannot open for writing: " + filepath + " (errno " +
                       std::to_string(openError) + ": " + std::strerror(openError) + ")";
        return false;
    }
    out << toJson(description).dump(2);
    if (!out.good())
    {
        errorMessage = "write failed: " + filepath;
        return false;
    }
    return true;
}

bool SceneSerializer::loadFromFile(const std::string& filepath,
                                   scene::SceneDescription& outDescription,
                                   std::string& errorMessage)
{
    std::ifstream in(filepath, std::ios::binary);
    if (!in)
    {
        errorMessage = "cannot open: " + filepath;
        return false;
    }

    try
    {
        json root;
        in >> root;
        if (!root.contains("entities"))
        {
            errorMessage = "scene file has no 'entities' array";
            return false;
        }
        outDescription = fromJson(root);
    }
    catch (const std::exception& ex)
    {
        errorMessage = std::string("invalid JSON: ") + ex.what();
        return false;
    }

    return true;
}

bool SceneSerializer::roundTrip(const scene::SceneDescription& input,
                                scene::SceneDescription& output,
                                std::string& errorMessage)
{
    try
    {
        const std::string text = toJson(input).dump(2);
        json parsed = json::parse(text);
        output = fromJson(parsed);
    }
    catch (const std::exception& ex)
    {
        errorMessage = ex.what();
        return false;
    }

    if (output.entities.size() != input.entities.size())
    {
        errorMessage = "entity count mismatch after round trip";
        return false;
    }

    for (size_t i = 0; i < input.entities.size(); ++i)
    {
        const auto& inputComponents = input.entities[i].components;
        const auto& outputComponents = output.entities[i].components;
        if (inputComponents.size() != outputComponents.size())
        {
            errorMessage = "component count mismatch after round trip (entity " +
                           std::to_string(i) + ")";
            return false;
        }
        for (size_t c = 0; c < inputComponents.size(); ++c)
        {
            if (!inputComponents[c].equivalentTo(outputComponents[c]))
            {
                errorMessage = "component value mismatch after round trip (entity " +
                               std::to_string(i) + ", component " +
                               inputComponents[c].type + ")";
                return false;
            }
        }
    }
    return true;
}

} // namespace editor
