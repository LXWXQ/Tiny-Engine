#pragma once

#include <string>

namespace tiny
{
    // UX-04：致命错误不能只写 stderr —— 双击启动的用户看不到控制台，
    // 窗口一闪就没了，用户拿不到任何可行动的信息。
    // 把「阶段 + 消息 + 工作目录 + 提示」追加到 startup-error.log，返回写入成功的路径；
    // 写不进去返回空串（调用方仍然把消息打到 stderr）。
    // directory 为空时写当前工作目录；自检用它指到临时目录，避免污染仓库根。
    std::string writeStartupErrorLog(const std::string& stage,
                                     const std::string& message,
                                     const std::string& directory = std::string());

    // 把致命错误整理成给人看的正文：阶段、原因、日志路径、下一步。
    // 单独成函数是为了能自检文本，而不必真的弹一个模态框。
    std::string formatStartupErrorMessage(const std::string& stage,
                                          const std::string& message,
                                          const std::string& logPath);

    // UX-04：原生 MessageBox。只在交互式启动失败时调用；无人值守的
    // --selftest / 截图 / 回归必须保持静默，否则会卡在等人点“确定”。
    // 非 Windows 平台退化为空操作（错误仍已写 stderr 与 startup-error.log）。
    void showStartupErrorBox(const std::string& title, const std::string& message);
}
