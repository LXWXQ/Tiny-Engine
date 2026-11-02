#pragma once
#include <cstdint>

// ============================================================================
// P1-2：RenderDoc in-app 抓帧挂钩。
//
// 开关模型：
//   * 编译期：SENTINEL_ENABLE_RENDERDOC 关闭时不加载任何东西（零开销）；
//   * 运行期：只有 --capture-frame N 时才尝试加载 RenderDoc API。
//
// 只有本进程被 RenderDoc 注入（例如经 renderdoccmd capture 启动）时，
// RENDERDOC_GetAPI 才会成功；否则所有调用都是安全的空操作。
// ============================================================================
class RenderDocHook
{
public:
    // 设备创建之后调用。
    void init(bool enabled, int captureFrame);
    void destroy();

    bool isActive() const { return active; }
    bool shouldCapture(uint64_t frameNumber) const
    {
        return active && captureFrame > 0 && frameNumber == static_cast<uint64_t>(captureFrame);
    }
    void triggerCapture();

private:
    bool active = false;
    int captureFrame = -1;
};