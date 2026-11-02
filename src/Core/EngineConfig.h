#pragma once
#include "Core/FrameInfo.h"
#include <glm/glm.hpp>
#include <string>

// ---------------------------------------------------------------------------
// W1: 运行时配置持久化
//
// 为什么需要它（这不是"顺手加个好用的功能"，而是被实际开发节奏逼出来的）：
//   D-2 调 Bloom 参数时，每换一组值都要「改 EngineSettings 默认值 -> 重编译
//   -> 启动 -> 截屏 -> 统计」，一轮几分钟。而 Bloom 的可用区间是靠扫参找到的
//   （阈值 1.0 / 1.8 / 2.2 / 0.2 一共试了四次，每次都要完整重建）。
//   ImGui 面板本来就能实时改这些值，只是改完不落盘 —— 重启就回到编译期默认值。
//
// 所以这里只做一件事：把 ImGui 面板能改的东西存成 json 再读回来。
// 引擎的其它状态（场景、模型）不进配置 —— 那是"资源"，不是"设置"。
//
// 文件位置由 CMake 注入的 SENTINEL_CONFIG_PATH 决定（工程根的 config/ 目录），
// 这样 Debug / Release 两个配置共享同一份用户设置，也不会污染工作目录。
// ---------------------------------------------------------------------------

/// 相机位姿。单独拿出来是因为它不属于 EngineSettings（那是渲染参数），
/// 但同样"改了希望下次启动还在"——调地形/体素时每次重新飞过去太痛苦。
struct CameraState
{
	glm::vec3 position{ 0.0f, 0.0f, 0.0f };
	glm::vec3 rotation{ 0.0f, 0.0f, 0.0f };   // 弧度，和 TransformComponent 一致
};

class EngineConfig
{
public:
	// 读取。文件不存在 / 解析失败 / 字段缺失都不会抛异常：
	// 缺失字段保持 out 的当前值（即编译期默认值），并在控制台说明原因。
	// 返回 true 表示"至少成功读到了一个合法的 json 文件"。
	static bool load(EngineSettings& out);
	static bool load(CameraState& out);

	// TD-09 regression: explicit v0 -> v1 -> v2 migration, backup creation,
	// and refusal to overwrite corrupt/future config files.
	static bool selfTest();

	static bool save(const EngineSettings& settings);
	static bool save(const CameraState& camera);

	// 供 UI 显示的一行结果（例如 "saved engine.json" / "file not found, defaults in use"）。
	//
	// 【为什么要用 ASCII】这条状态是画在 ImGui 面板上的，而 ImGui 用的是内置
	// 默认字体（ProggyClean），只有 ASCII 字形。写中文会整条渲染成 "???"
	// —— 既看不懂又显得像坏了。想要中文状态就得先给 ImGui 挂一份中文字体，
	// 那是另一件事（字体加载 + 图集重建），不该混在配置功能里。
	// 代码注释、commit message、控制台输出仍然用中文，只有这一条是给 UI 看的。
	static const std::string& getLastStatus();
	static const std::string& getConfigPath();
	/// 只取文件名。UI 上显示完整绝对路径会把面板撑宽，而且路径里可能有
	/// 默认字体渲染不出来的字符。
	static std::string getConfigFileName();

private:
	// 把 EngineSettings <-> json 的字段映射集中在一处。
	// 为什么不做成 to_json/from_json 的 ADL 重载：那样会要求所有包含
	// EngineSettings 的翻译单元都看到 nlohmann/json.hpp（一个 1MB 的头），
	// 编译时间会明显变差。集中在这里只影响本文件。
	static std::string readFileToString(bool& outFileExists);
	static bool writeStringToFile(const std::string& text);
};
