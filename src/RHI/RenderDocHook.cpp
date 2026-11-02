#include "RenderDocHook.h"
#include <iostream>

#ifdef SENTINEL_ENABLE_RENDERDOC
#  include "renderdoc_app.h"
#  include <windows.h>

namespace
{
    RENDERDOC_API_1_6_0* g_renderDocApi = nullptr;
}
#endif

void RenderDocHook::init(bool enabled, int frame)
{
    captureFrame = frame;

#ifndef SENTINEL_ENABLE_RENDERDOC
    enabled = false;
    (void)frame;
#endif

    if (!enabled)
        return;

#ifdef SENTINEL_ENABLE_RENDERDOC
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (module == nullptr)
        module = LoadLibraryA("renderdoc.dll");
    if (module == nullptr)
    {
        std::cout << "[Diag] RenderDoc not injected; --capture-frame is a no-op\n";
        return;
    }

    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
    if (getApi == nullptr)
        return;

    if (getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_renderDocApi)) != 1 ||
        g_renderDocApi == nullptr)
    {
        std::cout << "[Diag] RENDERDOC_GetAPI failed\n";
        return;
    }

    active = true;
    std::cout << "[Diag] RenderDoc hook active; will capture frame " << captureFrame << "\n";
#endif
}

void RenderDocHook::destroy()
{
#ifdef SENTINEL_ENABLE_RENDERDOC
    g_renderDocApi = nullptr;
#endif
    active = false;
}

void RenderDocHook::triggerCapture()
{
#ifdef SENTINEL_ENABLE_RENDERDOC
    if (active && g_renderDocApi != nullptr)
        g_renderDocApi->TriggerCapture();
#endif
}