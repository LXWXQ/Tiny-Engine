#include "Core/EngineConfig.h"
#include "AssetPaths.h"   // SENTINEL_CONFIG_PATH（CMake 配置阶段生成）

// nlohmann/json 来自 3rdParty/tinygltf（tinygltf 拉取时带下来的），
// 不额外引依赖、也不需要联网。它本身是 header-only 的第三方代码，
// MSVC 的 /W4 会在里面刷出一堆我们用不到的警告，所以用 pragma 屏蔽一段。
#pragma warning(push)
#pragma warning(disable : 4244 4267 4996 5054 4127 4324)
#include <json.hpp>
#pragma warning(pop)

#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>
#include <vector>
#include <exception>
#include <filesystem>
#include <system_error>
#include <stdexcept>

namespace
{
	using nlohmann::json;

	std::string g_lastStatus = "(no config I/O yet)";
	std::string g_configPath = SENTINEL_CONFIG_PATH;
	bool g_suppressExpectedErrorLog = false;
	constexpr int kCurrentConfigVersion = 2;

	void setStatus(const std::string& s)
	{
		g_lastStatus = s;
	}

	// ---- glm 向量 <-> json 数组 ----
	// glm 和 json 之间没有内置转换，写成模板省得为 vec2/vec3/vec4 各来一遍。
	template <typename VecT>
	VecT readVec(const json& node, const char* key, const VecT& fallback)
	{
		const auto it = node.find(key);
		if (it == node.end() || !it->is_array() || it->size() != static_cast<size_t>(VecT::length()))
			return fallback;

		VecT out;
		for (size_t i = 0; i < static_cast<size_t>(VecT::length()); ++i)
		{
			const json& e = (*it)[i];
			// 整数形式的 1 也算合法（手写配置文件时很容易写成 1 而不是 1.0）
			if (!e.is_number()) return fallback;
			out[static_cast<glm::length_t>(i)] = e.get<float>();
		}
		return out;
	}

	template <typename VecT>
	void writeVec(json& node, const char* key, const VecT& v)
	{
		json arr = json::array();
		for (size_t i = 0; i < static_cast<size_t>(VecT::length()); ++i)
			arr.push_back(v[static_cast<glm::length_t>(i)]);
		node[key] = std::move(arr);
	}

	/// 从节点里取一个字段；类型不对时保留 fallback 并记一条说明。
	/// 为什么不用 node.value(key, fallback)：那个在类型不匹配时会抛异常，
	/// 而"用户手改配置文件写错一个字段"不该让引擎起不来。
	template <typename T>
	T read(const json& node, const char* key, const T& fallback, std::vector<std::string>& warnings)
	{
		const auto it = node.find(key);
		if (it == node.end()) return fallback;
		try
		{
			return it->get<T>();
		}
		catch (const std::exception&)
		{
			warnings.push_back(std::string(key) + ": bad type, keeping default");
			return fallback;
		}
	}

	json settingsToJson(const EngineSettings& s)
	{
		json j;
		j["windowTitle"] = s.windowTitle;
		j["renderScale"] = s.renderScale;

		json ssao;
		ssao["enabled"] = s.ssaoEnabled;
		ssao["radius"] = s.ssaoRadius;
		ssao["bias"] = s.ssaoBias;
		ssao["kernelSize"] = s.ssaoKernelSize;
		j["ssao"] = std::move(ssao);

		json bloom;
		bloom["enabled"] = s.bloomEnabled;
		bloom["threshold"] = s.bloomThreshold;
		bloom["knee"] = s.bloomKnee;
		bloom["intensity"] = s.bloomIntensity;
		j["bloom"] = std::move(bloom);

		json motionBlur;
		motionBlur["enabled"] = s.motionBlurEnabled;
		motionBlur["strength"] = s.motionBlurStrength;
		motionBlur["maxRadius"] = s.motionBlurMaxRadius;
		j["motionBlur"] = std::move(motionBlur);

		json light;
		writeVec(light, "direction", s.directionalLightDir);
		writeVec(light, "color", s.directionalLightColor);
		light["exposure"] = s.exposure;
		j["lighting"] = std::move(light);

		json planned;
		planned["ssrEnabled"] = s.ssrEnabled;
		planned["shadowEnabled"] = s.shadowEnabled;
		j["planned"] = std::move(planned);

		return j;
	}

	// 只覆盖 out 里确实存在的字段，缺失的保持 out 的当前值
	void jsonToSettings(const json& j, EngineSettings& out, std::vector<std::string>& warnings)
	{
		out.windowTitle = read<std::string>(j, "windowTitle", out.windowTitle, warnings);
		out.renderScale = read(j, "renderScale", out.renderScale, warnings);

		if (j.contains("ssao") && j["ssao"].is_object())
		{
			const json& n = j["ssao"];
			out.ssaoEnabled = read(n, "enabled", out.ssaoEnabled, warnings);
			out.ssaoRadius = read(n, "radius", out.ssaoRadius, warnings);
			out.ssaoBias = read(n, "bias", out.ssaoBias, warnings);
			out.ssaoKernelSize = read(n, "kernelSize", out.ssaoKernelSize, warnings);
		}

		if (j.contains("bloom") && j["bloom"].is_object())
		{
			const json& n = j["bloom"];
			out.bloomEnabled = read(n, "enabled", out.bloomEnabled, warnings);
			out.bloomThreshold = read(n, "threshold", out.bloomThreshold, warnings);
			out.bloomKnee = read(n, "knee", out.bloomKnee, warnings);
			out.bloomIntensity = read(n, "intensity", out.bloomIntensity, warnings);
		}

		if (j.contains("motionBlur") && j["motionBlur"].is_object())
		{
			const json& n = j["motionBlur"];
			out.motionBlurEnabled = read(n, "enabled", out.motionBlurEnabled, warnings);
			out.motionBlurStrength = read(n, "strength", out.motionBlurStrength, warnings);
			out.motionBlurMaxRadius = read(n, "maxRadius", out.motionBlurMaxRadius, warnings);
		}

		if (j.contains("lighting") && j["lighting"].is_object())
		{
			const json& n = j["lighting"];
			out.directionalLightDir = readVec(n, "direction", out.directionalLightDir);
			out.directionalLightColor = readVec(n, "color", out.directionalLightColor);
			out.exposure = read(n, "exposure", out.exposure, warnings);
		}

		if (j.contains("planned") && j["planned"].is_object())
		{
			const json& n = j["planned"];
			out.ssrEnabled = read(n, "ssrEnabled", out.ssrEnabled, warnings);
			out.shadowEnabled = read(n, "shadowEnabled", out.shadowEnabled, warnings);
		}
	}

	json cameraToJson(const CameraState& c)
	{
		json j;
		writeVec(j, "position", c.position);
		writeVec(j, "rotation", c.rotation);
		return j;
	}

	void jsonToCamera(const json& j, CameraState& out)
	{
		out.position = readVec(j, "position", out.position);
		out.rotation = readVec(j, "rotation", out.rotation);
	}

	std::string makeBackupPath(const std::string& path, int version)
	{
		const std::string base = path + ".v" + std::to_string(version) + ".bak";
		if (!std::filesystem::exists(base))
			return base;
		for (int i = 1; i < 1000; ++i)
		{
			const std::string candidate = base + "." + std::to_string(i);
			if (!std::filesystem::exists(candidate))
				return candidate;
		}
		return {};
	}

	bool backupConfigFile(const std::string& path, int version, std::string& outBackupPath)
	{
		outBackupPath = makeBackupPath(path, version);
		if (outBackupPath.empty())
			return false;
		std::error_code ec;
		std::filesystem::copy_file(path, outBackupPath, std::filesystem::copy_options::none, ec);
		return !ec;
	}

	void migrateV0ToV1(json& root)
	{
		if (!root.contains("engineSettings") || !root["engineSettings"].is_object())
		{
			json settings = json::object();
			const char* legacyKeys[] = { "windowTitle", "renderScale", "ssao", "bloom", "lighting", "planned" };
			for (const char* key : legacyKeys)
			{
				if (root.contains(key))
				{
					settings[key] = std::move(root[key]);
					root.erase(key);
				}
			}
			root["engineSettings"] = std::move(settings);
		}
		root["version"] = 1;
	}

	void migrateV1ToV2(json& root)
	{
		if (!root.contains("engineSettings") || !root["engineSettings"].is_object())
			root["engineSettings"] = json::object();
		json& settings = root["engineSettings"];
		if (!settings.contains("renderScale") || !settings["renderScale"].is_number())
			settings["renderScale"] = 1.0f;
		if (!settings.contains("planned") || !settings["planned"].is_object())
			settings["planned"] = json::object();
		root["version"] = 2;
	}

	/// 原子写：先写 .tmp 再 rename。
	/// 直接覆盖写的话，写到一半崩溃就会留下语法坏掉的配置，下次启动直接报错。
	bool writeRoot(const json& root)
	{
		const std::string path = EngineConfig::getConfigPath();
		const std::string tmp = path + ".tmp";
		const std::string text = root.dump(2) + "\n";

		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out.is_open())
			{
				setStatus("save failed: cannot write " + tmp);
				std::cerr << "[Config] " << g_lastStatus << "\n";
				return false;
			}
			out << text;
			if (!out.good())
			{
				setStatus("save failed: write error");
				std::cerr << "[Config] " << g_lastStatus << "\n";
				return false;
			}
		}

		std::remove(path.c_str());   // Windows 的 rename 不覆盖已存在文件
		if (std::rename(tmp.c_str(), path.c_str()) != 0)
		{
			setStatus("save failed: rename " + tmp + " -> " + path);
			std::cerr << "[Config] " << g_lastStatus << "\n";
			return false;
		}
		return true;
	}

	enum class RootReadStatus
	{
		Ok,
		Missing,
		InvalidJson,
		InvalidRoot,
		InvalidVersion,
		UnsupportedVersion,
		BackupFailed,
		MigrationFailed
	};

	struct RootLoadResult
	{
		RootReadStatus status = RootReadStatus::Missing;
		json root;
		int sourceVersion = 0;
		bool migrated = false;
		std::string backupPath;
		std::string why;
	};

	RootLoadResult loadRootWithMigration()
	{
		RootLoadResult result;

		std::ifstream file(EngineConfig::getConfigPath());
		if (!file.is_open())
		{
			result.status = RootReadStatus::Missing;
			result.why = "config file not found";
			return result;
		}

		std::stringstream buffer;
		buffer << file.rdbuf();
		file.close();
		const std::string text = buffer.str();
		if (text.empty())
		{
			result.status = RootReadStatus::InvalidJson;
			result.why = "config file is empty";
			return result;
		}

		json root = json::parse(text, nullptr, false);
		if (root.is_discarded())
		{
			result.status = RootReadStatus::InvalidJson;
			result.why = "config file is not valid JSON";
			return result;
		}
		if (!root.is_object())
		{
			result.status = RootReadStatus::InvalidRoot;
			result.why = "config root is not an object";
			return result;
		}

		int version = 0;
		if (root.contains("version"))
		{
			if (!root["version"].is_number_integer())
			{
				backupConfigFile(EngineConfig::getConfigPath(), 0, result.backupPath);
				result.status = RootReadStatus::InvalidVersion;
				result.why = "config version is not an integer";
				return result;
			}
			version = root["version"].get<int>();
			if (version < 0)
			{
				backupConfigFile(EngineConfig::getConfigPath(), version, result.backupPath);
				result.status = RootReadStatus::InvalidVersion;
				result.why = "config version is negative";
				return result;
			}
		}

		result.sourceVersion = version;
		if (version > kCurrentConfigVersion)
		{
			if (!backupConfigFile(EngineConfig::getConfigPath(), version, result.backupPath))
			{
				result.status = RootReadStatus::BackupFailed;
				result.why = "unsupported config version and backup failed";
				return result;
			}
			result.status = RootReadStatus::UnsupportedVersion;
			result.why = "config version " + std::to_string(version) +
				" is newer than supported version " + std::to_string(kCurrentConfigVersion);
			return result;
		}

		if (version < kCurrentConfigVersion)
		{
			if (!backupConfigFile(EngineConfig::getConfigPath(), version, result.backupPath))
			{
				result.status = RootReadStatus::BackupFailed;
				result.why = "could not back up config before migration";
				return result;
			}

			try
			{
				int current = version;
				while (current < kCurrentConfigVersion)
				{
					if (current == 0)
						migrateV0ToV1(root);
					else if (current == 1)
						migrateV1ToV2(root);
					else
						throw std::runtime_error("no migration path from version " + std::to_string(current));
					++current;
				}
				root["version"] = kCurrentConfigVersion;
				if (!writeRoot(root))
					throw std::runtime_error("could not write migrated config");
			}
			catch (const std::exception& e)
			{
				result.status = RootReadStatus::MigrationFailed;
				result.why = e.what();
				return result;
			}

			result.migrated = true;
		}

		result.status = RootReadStatus::Ok;
		result.root = std::move(root);
		return result;
	}
}

const std::string& EngineConfig::getConfigPath()
{
	return g_configPath;
}

std::string EngineConfig::getConfigFileName()
{
	// 状态栏只显示文件名：绝对路径可能有五十多个字符，会把 ImGui 面板撑得很宽。
	static const std::string name = []() -> std::string {
		const std::string& full = getConfigPath();
		const size_t slash = full.find_last_of("/\\");
		return (slash == std::string::npos) ? full : full.substr(slash + 1);
	}();
	return name;
}

const std::string& EngineConfig::getLastStatus()
{
	return g_lastStatus;
}

bool EngineConfig::load(EngineSettings& out)
{
	RootLoadResult loaded = loadRootWithMigration();
	if (loaded.status != RootReadStatus::Ok)
	{
		std::string status;
		switch (loaded.status)
		{
		case RootReadStatus::Missing:
			status = loaded.why + " - using compile-time defaults";
			break;
		case RootReadStatus::InvalidJson:
		case RootReadStatus::InvalidRoot:
			status = loaded.why + " - using compile-time defaults (file left untouched)";
			break;
		case RootReadStatus::InvalidVersion:
			status = loaded.why + " - using compile-time defaults";
			if (!loaded.backupPath.empty())
				status += " (backup: " + std::filesystem::path(loaded.backupPath).filename().string() + ")";
			break;
		case RootReadStatus::UnsupportedVersion:
			status = loaded.why + " - using compile-time defaults";
			if (!loaded.backupPath.empty())
				status += " (backup: " + std::filesystem::path(loaded.backupPath).filename().string() + ")";
			break;
		case RootReadStatus::BackupFailed:
		case RootReadStatus::MigrationFailed:
			status = "config migration failed: " + loaded.why + " - using compile-time defaults";
			break;
		default:
			status = "config load failed - using compile-time defaults";
			break;
		}
		setStatus(status);
		std::cout << "[Config] " << g_lastStatus << " -> " << getConfigPath() << "\n";
		return false;
	}

	if (!loaded.root.contains("engineSettings") || !loaded.root["engineSettings"].is_object())
	{
		setStatus("no engineSettings node - using compile-time defaults");
		std::cout << "[Config] " << g_lastStatus << "\n";
		return false;
	}

	std::vector<std::string> warnings;
	jsonToSettings(loaded.root["engineSettings"], out, warnings);

	if (loaded.migrated)
	{
		setStatus("migrated " + getConfigFileName() + " v" + std::to_string(loaded.sourceVersion)
			+ " -> v" + std::to_string(kCurrentConfigVersion)
			+ " (backup: " + std::filesystem::path(loaded.backupPath).filename().string() + ")");
	}
	else if (!warnings.empty())
	{
		setStatus("loaded with " + std::to_string(warnings.size()) + " bad field(s), defaults kept");
	}
	else
	{
		setStatus("loaded " + getConfigFileName());
	}
	for (const std::string& w : warnings)
		std::cout << "[Config] 警告: " << w << "\n";
	std::cout << "[Config] " << g_lastStatus << " -> " << getConfigPath() << "\n";
	return true;
}

bool EngineConfig::load(CameraState& out)
{
	RootLoadResult loaded = loadRootWithMigration();
	if (loaded.status != RootReadStatus::Ok)
		return false;   // load(EngineSettings&) 已经报过状态

	if (!loaded.root.contains("camera") || !loaded.root["camera"].is_object())
		return false;

	jsonToCamera(loaded.root["camera"], out);
	if (loaded.migrated)
	{
		setStatus("migrated " + getConfigFileName() + " v" + std::to_string(loaded.sourceVersion)
			+ " -> v" + std::to_string(kCurrentConfigVersion)
			+ " (backup: " + std::filesystem::path(loaded.backupPath).filename().string() + ")");
		std::cout << "[Config] " << g_lastStatus << "\n";
	}
	return true;
}

bool EngineConfig::save(const EngineSettings& settings)
{
	RootLoadResult loaded = loadRootWithMigration();
	json root;
	if (loaded.status == RootReadStatus::Missing)
	{
		root = json::object();
	}
	else if (loaded.status == RootReadStatus::Ok)
	{
		root = std::move(loaded.root);
	}
	else
	{
		setStatus("save refused: existing config is not writable (file left untouched)");
		if (!g_suppressExpectedErrorLog)
			std::cerr << "[Config] " << g_lastStatus << " -> " << getConfigPath() << "\n";
		return false;
	}

	root["version"] = kCurrentConfigVersion;
	root["engineSettings"] = settingsToJson(settings);

	if (!writeRoot(root))
		return false;

	setStatus("saved render settings -> " + getConfigFileName());
	std::cout << "[Config] " << g_lastStatus << "\n";
	return true;
}

bool EngineConfig::save(const CameraState& camera)
{
	RootLoadResult loaded = loadRootWithMigration();
	json root;
	if (loaded.status == RootReadStatus::Missing)
	{
		root = json::object();
	}
	else if (loaded.status == RootReadStatus::Ok)
	{
		root = std::move(loaded.root);
	}
	else
	{
		setStatus("save refused: existing config is not writable (file left untouched)");
		if (!g_suppressExpectedErrorLog)
			std::cerr << "[Config] " << g_lastStatus << " -> " << getConfigPath() << "\n";
		return false;
	}

	root["version"] = kCurrentConfigVersion;
	root["camera"] = cameraToJson(camera);

	if (!writeRoot(root))
		return false;

	setStatus("saved camera pose -> " + getConfigFileName());
	std::cout << "[Config] " << g_lastStatus << "\n";
	return true;
}

bool EngineConfig::selfTest()
{
	namespace fs = std::filesystem;
	const std::string originalPath = g_configPath;
	const fs::path tempRoot = fs::temp_directory_path() / "sentinel-config-selftest";
	std::error_code ec;
	fs::remove_all(tempRoot, ec);
	fs::create_directories(tempRoot, ec);
	if (ec)
	{
		setStatus("config selfTest FAIL: cannot create temp directory");
		std::cerr << "[Config] " << g_lastStatus << "\n";
		return false;
	}

	auto writeText = [](const fs::path& path, const std::string& text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << text;
		return out.good();
	};
	auto readText = [](const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	};

	bool ok = true;
	auto check = [&](bool condition, const char* message)
	{
		if (!condition)
		{
			ok = false;
			std::cerr << "[Config] selfTest FAIL: " << message << "\n";
		}
	};

	try
	{
		const fs::path legacyPath = tempRoot / "legacy.json";
		const std::string legacyText = R"({
  "version": 0,
  "windowTitle": "Legacy Title",
  "bloom": { "enabled": false },
  "camera": { "position": [1.0, 2.0, 3.0], "rotation": [0.0, 0.0, 0.0] }
})";
		check(writeText(legacyPath, legacyText), "cannot write legacy fixture");
		g_configPath = legacyPath.string();

		EngineSettings settings;
		settings.windowTitle = "Default Title";
		check(load(settings), "legacy config did not load");
		check(settings.windowTitle == "Legacy Title", "v0 top-level windowTitle was not migrated");
		check(!settings.bloomEnabled, "v0 top-level bloom was not migrated");
		check(settings.renderScale == 1.0f, "v2 renderScale default was not inserted");

		json migrated = json::parse(readText(legacyPath), nullptr, false);
		check(migrated.is_object() && migrated["version"] == 2, "config was not rewritten as version 2");
		check(migrated.contains("engineSettings") && migrated["engineSettings"].is_object(),
			"migrated config has no engineSettings object");

		const fs::path legacyBackup = fs::path(legacyPath.string() + ".v0.bak");
		check(fs::exists(legacyBackup), "legacy config backup was not created");
		check(readText(legacyBackup) == legacyText, "legacy backup content mismatch");

		CameraState camera;
		check(load(camera), "legacy camera did not load");
		check(camera.position.x == 1.0f && camera.position.y == 2.0f && camera.position.z == 3.0f,
			"legacy camera pose mismatch");

		const fs::path corruptPath = tempRoot / "corrupt.json";
		const std::string corruptText = "{ this is not json";
		check(writeText(corruptPath, corruptText), "cannot write corrupt fixture");
		g_configPath = corruptPath.string();
		check(!load(settings), "corrupt config unexpectedly loaded");
		g_suppressExpectedErrorLog = true;
		check(!save(settings), "corrupt config was unexpectedly overwritten by save");
		g_suppressExpectedErrorLog = false;
		check(readText(corruptPath) == corruptText, "corrupt config file was modified");

		const fs::path futurePath = tempRoot / "future.json";
		const std::string futureText = R"({ "version": 999, "engineSettings": {} })";
		check(writeText(futurePath, futureText), "cannot write future fixture");
		g_configPath = futurePath.string();
		check(!load(settings), "future config unexpectedly loaded");
		g_suppressExpectedErrorLog = true;
		check(!save(settings), "future config was unexpectedly overwritten by save");
		g_suppressExpectedErrorLog = false;
		check(readText(futurePath) == futureText, "future config file was modified");
		check(fs::exists(fs::path(futurePath.string() + ".v999.bak")), "future config backup was not created");
	}
	catch (const std::exception& e)
	{
		ok = false;
		std::cerr << "[Config] selfTest exception: " << e.what() << "\n";
	}

	g_suppressExpectedErrorLog = false;
	g_configPath = originalPath;
	fs::remove_all(tempRoot, ec);

	if (!ok)
	{
		setStatus("config selfTest FAIL");
		return false;
	}

	setStatus("config migration selfTest PASS");
	std::cout << "[Config] selfTest PASS: v0 -> v2 migration, backup, corrupt/future config left untouched\n";
	return true;
}
