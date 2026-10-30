#include "Editor/CommandStack.h"
#include <chrono>

namespace editor
{

namespace
{
double nowSeconds()
{
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}
} // namespace

void CommandStack::execute(std::unique_ptr<Command> command)
{
    if (!command)
        return;

    const double now = nowSeconds();
    const bool withinMergeWindow = (now - lastExecuteTime) <= kMergeWindowSeconds;

    // 合并条件：栈非空、时间窗口内、且两条命令自认为可以合并。
    // 合并时**不再压新命令**，只让上一条命令重新 apply 一次 ——
    // 因为命令对象里存的是"旧值 + 新值"，上一条的 apply 用的是最新的目标值。
    if (withinMergeWindow && !undoStack.empty() && command->mergeWith(*undoStack.back()))
    {
        command->apply();
        lastExecuteTime = now;
        redoStack.clear();
        return;
    }

    command->apply();
    undoStack.push_back(std::move(command));
    redoStack.clear();
    lastExecuteTime = now;

    // 限制历史长度。编辑器长时间运行不可能无限存命令
    // （每条命令可能持有实体快照），超过上限丢弃最老的。
    static constexpr size_t kMaxHistory = 256;
    while (undoStack.size() > kMaxHistory)
        undoStack.pop_front();
}

void CommandStack::undo()
{
    if (undoStack.empty())
        return;

    replaying = true;
    std::unique_ptr<Command> command = std::move(undoStack.back());
    undoStack.pop_back();
    command->revert();
    redoStack.push_back(std::move(command));
    replaying = false;

    // 撤销后打断合并窗口，否则紧接着的编辑会被合并进"上一条"（其实已经不在了）
    lastExecuteTime = 0.0;
}

void CommandStack::redo()
{
    if (redoStack.empty())
        return;

    replaying = true;
    std::unique_ptr<Command> command = std::move(redoStack.back());
    redoStack.pop_back();
    command->apply();
    undoStack.push_back(std::move(command));
    replaying = false;

    lastExecuteTime = 0.0;
}

void CommandStack::clear()
{
    undoStack.clear();
    redoStack.clear();
    lastExecuteTime = 0.0;
}

} // namespace editor
