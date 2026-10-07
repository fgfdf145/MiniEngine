# 进程分配：优先级与 CPU 核心

日期：2026-10-07

## 目标

让引擎自己决定在哪些逻辑处理器上跑、以什么优先级跑，不必每次开任务管理器手动“设置相关性”和“设置优先级”。默认高优先级，全部核心。

## 模块

`engine/platform/process/process_allocation.{h,cpp}`（`engine_platform`）：

- `ProcessPriority`：Below Normal / Normal / Above Normal / High，对应 Windows 的 `*_PRIORITY_CLASS`。不提供 Realtime：它会压住系统自己的输入和音频线程，而且要管理员权限。
- `CpuSelection`：
  - `All`：全部逻辑处理器。
  - `Performance`：混合架构 CPU 上效率等级最高的那一档（Intel 的 P 核）；非混合 CPU 上等于 All，界面里置灰。
  - `Custom`：`customCpus` 列出的处理器编号（与任务管理器的编号一致）。
- `QueryProcessorTopology()`：`GetLogicalProcessorInformationEx(RelationProcessorCore)`，每个逻辑处理器记下所属物理核心和 `EfficiencyClass`（越大越快）。只取处理器组 0：进程亲和掩码只能覆盖一个组，64 个逻辑处理器以内的机器全在组 0。
- `ApplyProcessAllocation()`：`SetPriorityClass` + `SetProcessAffinityMask`。进程亲和掩码对已有线程立即生效，之后创建的线程继承它。失败时结果里带错误文字，并读回进程实际所在的处理器。
- Linux：`setpriority` 设 nice（High = -10，需要 `CAP_SYS_NICE`）与 `sched_setaffinity`；macOS 只有优先级，选部分核心会报告做不到。这两条路径没有在本机验证。

## 设置与命令行

`miniengine.settings.json` 新增：

```json
"process": {
  "priority": "high",
  "cpus": "all",
  "custom_cpus": []
}
```

缺这一节、或值不认识时保持默认（高优先级、全部核心）。

命令行覆盖本次运行（不写回设置）：

- `--priority below-normal|normal|above-normal|high`
- `--cpus all|performance|0,2,4-7`

## 生效时机

1. `EditorApplication::Run()` 一开始（`EnginePaths::Initialize` 之后、任务系统启动之前）读设置、叠加命令行、应用。
2. 任务系统按整台机器的处理器数减二创建工作线程（`--task-threads` 仍然优先），其中按应用后得到的处理器数减二的那些是活动的，其余停放。所以只给 8 个 P 核时 22 个工作线程里 6 个干活，不会 22 个线程挤 8 个核。
3. Preferences 窗口的 Process 一节改动后当帧应用（`EditorUiFrameResult::processAllocation` → `ApplyUiActions`），并保存设置，同时 `TaskSystem::SetActiveWorkerThreads` 把活动工作线程数改成新的处理器数减二，不用重启。

### 运行中改核心数（2026-10-08 修正）

最初的版本里工作线程数只在启动时定一次。亲和掩码会把所有已有线程（物理、任务工作线程）移到新核心上，但线程数不变：启动时全核、运行中改成 8 个 P 核，仍有 22 个工作线程和 7 个物理排空线程挤 8 个核；反过来启动时 8 核、之后改全核，物理一直只有 6 个工作线程可用。

修正：
- 停放：enkiTS 的工作线程没活要睡眠时会调 `profilerCallbacks.waitForNewTaskSuspendStart`，引擎在这里让编号超出活动数的工作线程在条件变量上等，直到活动数再变大或任务系统关闭。这个位置在任务之外（`WaitforTask` 里等待的线程不走这里），所以停放不会卡住它参与的工作；enkiTS 把它算作睡眠中，发给它的唤醒信号最多让别的线程多醒一次。
- 物理：`TaskJobSystem::GetMaxConcurrency` 在整个生命周期内不变（Jolt 一步里多次读它来切分任务，中途变化会让切分对不上），每次开工时启动的排空线程数按 `TaskSystem::ActiveThreadCount() - 1` 取，与上限取小。
- `TaskSystem::Shutdown` 先放开所有停放的线程，否则 enkiTS 等不到它们退出。

## 界面

Edit > Preferences > Process：

- Priority 下拉框。
- CPUs 下拉框：`All (24)`、`Performance Cores (8)`、`Custom`。切到 Custom 时以当前在用的核心为起点。
- Custom 时按“Performance cores / Efficiency cores”两组列出复选框；最后一个勾选的核心不能取消。
- `Now:` 一行显示实际生效的状态（例如 `High priority, CPUs 0-7 (8 of 24)`），失败时附原因；命令行覆盖时标 `(command line)`。

## 测试

- `miniengine.process_allocation`：CPU 列表解析/格式化、键名往返、混合拓扑上的三种选择、真实地对测试进程设置亲和与优先级并用 `GetProcessAffinityMask`/`GetPriorityClass` 读回。
- `miniengine.command_registry` 里的设置往返：保存/读回、旧文件取默认、未知优先级保持 High。
- `miniengine.preferences_window`：无窗口绘制 Preferences 的 All / Performance / Custom / 只剩一个核心四种状态，检查禁用作用域配对；设 `MINIENGINE_UI_SNAPSHOT_DIR` 时输出 PNG。
