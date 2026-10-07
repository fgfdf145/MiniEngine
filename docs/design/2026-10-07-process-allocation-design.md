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
2. 任务系统的工作线程数按应用后得到的处理器数减二（原来是全部硬件线程减二），所以只给 8 个 P 核时是 6 个工作线程，不会 22 个线程挤 8 个核。`--task-threads` 仍然优先。
3. Preferences 窗口的 Process 一节改动后当帧应用（`EditorUiFrameResult::processAllocation` → `ApplyUiActions`），并保存设置。工作线程数不随之变化，窗口里注明“按启动时的核心数”，要重新分配线程数需重启。

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
