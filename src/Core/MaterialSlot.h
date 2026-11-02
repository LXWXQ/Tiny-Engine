#pragma once
#include <cstdint>

// 无效的材质描述符槽位。
//
// MaterialComponent::materialSetIndex / SubMesh::materialSlot / Material::descriptorSetIndex
// 统一用它表示"尚未分配"。
//
// 注意不要用 0 当哨兵：0 是合法的槽位号。原代码里默认值就是 0，
// 导致没有材质的物体（天空盒、网格）会悄悄绑到第 0 号材质上 ——
// 只是因为第 0 号恰好已经分配过才没崩。
constexpr uint32_t kInvalidMaterialSlot = UINT32_MAX;
