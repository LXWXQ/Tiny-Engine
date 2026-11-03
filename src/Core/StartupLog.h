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
}
