#pragma once

// ===========================================================================
// 原生组件的元数据注册
//
// 这些组件在 Scene / Components.h 里早已存在，但过去只有固定 Inspector 面板。
// 这里给它们补上统一注册表条目，让 + Add Component 能列出可安全挂载的类型，
// 同时把“托管”类型（地形/体素/相机）置灰并说明创建入口。
//
// 关键约束：原生组件走 EntityDescription 的专用字段，不能进入 v3 的通用
// components 数组；因此所有条目 serialized=false，由 Scene 的序列化过滤。
// ===========================================================================

namespace scene
{

class ComponentRegistry;

void registerNativeComponents(ComponentRegistry& registry);

} // namespace scene
