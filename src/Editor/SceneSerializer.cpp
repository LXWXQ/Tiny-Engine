#include "Editor/SceneSerializer.h"
#include "AssetPaths.h"

#include <json.hpp>
#include <cstring>
#include <cerrno>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace editor
{

namespace
{
constexpr int kCurrentVersion = 1;

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

        description.entities.push_back(std::move(entity));
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

        entities.push_back(std::move(j));
    }

    root["entities"] = std::move(entities);
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
    return true;
}

} // namespace editor
