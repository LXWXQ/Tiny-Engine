#pragma once

// T-06：图级黑板。
//
// 为什么要有它：跨 Pass 交接的东西（本帧曝光、选中项、插值权重、UI 回调）原先
// 全挂在 FrameInfo 上 —— 于是 FrameInfo 成了所有子系统的公共抽屉，加一个特性就
// 多两三个字段，而且谁都能写。黑板把"交接"这件事收拢成一张按类型索引的表：
//
//   1. 生命周期严格一帧：RenderGraph::beginFrame 清空，写方在 beginFrame 之后写，
//      读方在 addToGraph 里读。跨帧的东西不许放这里。
//   2. 只放"要交给别人"的值，不放子系统自己的状态（那些放子系统成员里）。
//   3. 类型即键：同类型只能有一份，读方拿裸指针，nullptr 表示"本帧没人提供"。

#include <cstddef>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace tiny
{

class RenderGraphBlackboard
{
public:
template <typename T, typename... Args>
T& getOrCreate(Args&&... args)
{
const std::type_index key(typeid(T));
auto it = entries.find(key);
if (it == entries.end())
{
T* raw = new T(std::forward<Args>(args)...);
Entry entry{};
entry.object = raw;
entry.destroy = [](void* p) { delete static_cast<T*>(p); };
it = entries.emplace(key, entry).first;
}
return *static_cast<T*>(it->second.object);
}

template <typename T>
T* find()
{
auto it = entries.find(std::type_index(typeid(T)));
return it == entries.end() ? nullptr : static_cast<T*>(it->second.object);
}

template <typename T>
const T* find() const
{
auto it = entries.find(std::type_index(typeid(T)));
return it == entries.end() ? nullptr : static_cast<const T*>(it->second.object);
}

template <typename T>
void set(T value)
{
getOrCreate<T>() = std::move(value);
}

template <typename T>
bool has() const
{
return entries.find(std::type_index(typeid(T))) != entries.end();
}

void clear()
{
for (auto& pair : entries)
{
if (pair.second.destroy != nullptr)
pair.second.destroy(pair.second.object);
}
entries.clear();
}

size_t size() const { return entries.size(); }

private:
struct Entry
{
void* object = nullptr;
void (*destroy)(void*) = nullptr;
};

std::unordered_map<std::type_index, Entry> entries;
};

} // namespace tiny