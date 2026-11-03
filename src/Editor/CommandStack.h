#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <string>

namespace editor
{

// ===========================================================================
// 撤销 / 重做
//
// 为什么第一版就要有：编辑器的每一次操作（移动物体、改材质、删实体）
// 都必须是可撤销的。如果先做功能再加撤销，就得把每个功能重写一遍 ——
// 因为"可撤销"要求操作被建模成对象，而不是直接改数据。
//
// 所以规则是：**任何修改场景的代码都必须经过 CommandStack**。
// 面板里禁止直接写 registry.get<TransformComponent>(e).translation = x;
// 必须构造一个 SetTransformCommand。
//
// 事件（Event）与命令（Command）的区别：
//   - Command 可以被撤销（DeleteEntity、SetTransform）
//   - Event 不可撤销（Play、Save、Reload）——它们不进命令栈，直接执行
// ===========================================================================

class Command
{
public:
    virtual ~Command() = default;

    // 执行（首次执行与 redo 都调它）
    virtual void apply() = 0;

    // 撤销
    virtual void revert() = 0;

    // 显示在"编辑"菜单里的名字，例如 "Move Entity"
    virtual const char* name() const = 0;

    // 把相邻的同类命令合并（用于拖拽滑块：一次拖拽 = 一条命令）
    //
    // 为什么需要：ImGui 的 DragFloat 在拖动过程中每帧都会变化，
    // 如果每次都压栈，撤销一次只能退回一帧的数值，用户要按几百次 Ctrl+Z。
    // 返回 true 时，CommandStack 会把连续同类型命令合并成一条。
    virtual bool mergeWith(const Command& previous) const { (void)previous; return false; }

    // 合并时由**上一条命令**吸收新命令的目标值，然后重新 apply。
    // 这样撤销回到拖拽开始，重做则到拖拽结束（只把新命令 apply 一次，
    // 上一条命令里的 new* 仍是第一帧的值，重做会退回第一帧）。
    virtual void absorb(const Command& newer) { (void)newer; }
};

class CommandStack
{
public:
    void execute(std::unique_ptr<Command> command);
    void undo();
    void redo();

    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }

    const char* undoName() const {
        return undoStack.empty() ? "" : undoStack.back()->name();
    }
    const char* redoName() const {
        return redoStack.empty() ? "" : redoStack.back()->name();
    }

    void clear();

    // 正在撤销/重做时置位。命令的 apply/revert 里若需要区分
    // "用户操作" 与 "撤销操作"（例如不重复记日志），检查这个。
    bool isReplaying() const { return replaying; }

    size_t undoCount() const { return undoStack.size(); }
    size_t redoCount() const { return redoStack.size(); }

    // E4-d：撤销历史面板要"看得见每一步"，所以把两个栈只读地暴露出去。
    // 面板只能读名字，真正跳转必须走 jumpTo() —— 否则绕过 replaying 标记会把日志打重复。
    const std::deque<std::unique_ptr<Command>>& undoEntries() const { return undoStack; }
    const std::deque<std::unique_ptr<Command>>& redoEntries() const { return redoStack; }

    // 跳到指定撤销深度：targetUndoCount = 要保留多少条"已执行"的命令。
    // 小于当前 = 连续 undo，大于当前 = 连续 redo。超出范围时 clamp。
    void jumpTo(size_t targetUndoCount);

private:
    std::deque<std::unique_ptr<Command>> undoStack;
    std::deque<std::unique_ptr<Command>> redoStack;
    bool replaying = false;

    // 合并窗口：上一条命令执行后多久之内允许合并。
    // 拖拽滑块时每帧都会产生命令，用时间窗口区分"同一次拖拽"与"两次独立编辑"。
    double lastExecuteTime = 0.0;
    static constexpr double kMergeWindowSeconds = 0.35;
};

} // namespace editor
