# 引擎控制通道（--control）设计

日期：2026-10-10

## 目的

此前我（以及任何脚本）控制引擎只有一种方式：带一串命令行参数（`--scene/--state/--frames/--capture/--drive/--photo` 等）冷启动一次，跑固定帧数后退出，再读截图和日志。每改一个参数就要重启，大场景（GTA、BeamNG）单是加载就几十秒；也无法在运行中查询状态。

控制通道让外部客户端连接**正在运行的编辑器**，在两帧之间执行命令：改相机、改渲染设置、等场景就绪、截图、读帧时间、开车、读遥测、读日志，全在同一个进程里反复做。

## 结构

```
客户端 (mectl.py / mcp_server.py / 任意 TCP)  ──一行一个 JSON──►  ControlServer（socket 线程）
                                                                    │ TakeRequests()
                                                                    ▼
                                         主线程帧循环：DrawFrame → ControlSession::Update → ……
```

- `engine/application/control_server.*`：127.0.0.1 上的 TCP 服务器。接收线程 + 每个连接一个读线程，只搬运文本，不碰引擎状态。只绑定回环地址，其他机器无法连接；Windows 上设 `SO_EXCLUSIVEADDRUSE`。
- `engine/application/control_session.*`：命令表。`Update()` 每帧在 `DrawFrame()` 之后、在主线程上运行，和编辑器面板修改同一份状态的位置一致（渲染线程只读帧包，不读 `State()`）。
- 需要等若干帧的命令（`frames`、`wait_scene`、`photo`）登记为等待者，每帧轮询，完成后再回答；这期间帧循环照常运行，其他请求照常回答。超时会返回错误。

## 协议

请求：`{"id": 7, "cmd": "camera.set", "args": {"position": [0, 2, 5]}}`
回答：`{"id": 7, "ok": true, "result": {...}}` 或 `{"id": 7, "ok": false, "error": "..."}`

`help` 列出全部命令。第一版的命令：

| 命令 | 作用 |
|---|---|
| `ping` / `status` | 版本、pid、后端、帧号；场景、是否加载中、视口、相机、是否在开车 |
| `frames {count}` | 再画 count 帧后回答（最小化时跳过的帧不计） |
| `wait_scene {settle}` | 场景、纹理、光追场景、streaming 都就绪后，重启时域效果，再等 settle 帧 |
| `scene.load {path}` | 等同 File > Open |
| `camera.get` / `camera.set` | position、yaw、pitch、look_at、fov、exposure_ev100（会关闭自动曝光）、auto_exposure |
| `render.get {prefix}` / `render.set {values}` | 渲染设置，键与设置文件相同（`group.key`），通过 `VisitRenderDebugFields` 自动覆盖所有字段；外加 tone_mapper、dlss_mode、dlss_preset、gbuffer_view 和 `camera.*` 自适应参数。类型检查，全部生效或全部不生效 |
| `viewport.set {width,height}` | 固定渲染尺寸（同 `--viewport-size`），0 表示交还给面板 |
| `capture {path}` | 视口最后一帧写成 PNG |
| `photo {...}` | Photo Mode，写完才回答 |
| `timings` | 最近若干帧的 CPU、主线程、各 pass 的 GPU 平均时间（新增 `IRenderBackend::GetFrameTimings`，`LogFrameTimings` 改为基于它） |
| `entities.list` / `entity.set` | 列出实体、按 id 或名字移动实体 |
| `drive.start/stop/reset/controls/pause/status` | 开车；controls 接管油门、转向、刹车、手刹；status 带 `wheels: true` 时输出每个轮子的力和滑移 |
| `log {lines, after}` | 引擎日志最近的行（新增 spdlog 环形缓冲 sink，保留 4096 行，带序号，可增量读取） |
| `quit` | 本帧后退出（不弹未保存场景的确认框） |

## 启用

- `--control [PORT]`（默认 47811），或环境变量 `MINIENGINE_CONTROL_PORT`。
- `--read-only-settings`（或 `MINIENGINE_READ_ONLY_SETTINGS=1`）：`miniengine.settings.json` 和 `imgui.ini` 只读不写，并且和脚本运行一样不保存视图设置。这样测试运行不会覆盖用户的设置和布局（以前的 headless 运行会改写窗口开关和 dock 布局）。

## 客户端

`tools/engine_control/`：

- `engine_control.py`：`EngineClient`（持久连接，`call(cmd, **args)`）和 `launch()`。`launch()` 以 detached 方式从仓库根目录启动 `miniengine_app --control PORT --read-only-settings`，设置 `MINIENGINE_VIRTUAL_DESKTOP=launcher`，控制台输出写到 `out/control/engine-PORT.log`，等 `ping` 成功后返回。
- `mectl.py`：命令行，例如 `mectl.py launch -- --scene assets/scenes/vehicle_studio.yaml`、`mectl.py camera.set position=[0,1.5,6] look_at=[0,0.5,0]`。
- `mcp_server.py`：stdio MCP 服务器，在仓库根目录的 `.mcp.json` 里注册为 `miniengine`，不依赖 SDK。工具：`engine_launch`、`engine_call`、`engine_capture`（把截图作为图片直接返回）、`engine_quit`。

## 第二阶段（同日）：A/B、确定性、场景读写、崩溃报告、RenderDoc、UI 注入

### 确定性帧
- `RendererSharedState::fixedFrameSeconds`（原来只有环境变量 `MINIENGINE_FIXED_FRAME_SECONDS`，现在是运行时设置）：每帧固定步长，0 冻结时间（云、昼夜、动画、物理都不动），加载期间不走时间。
- 命令 `deterministic {frame_seconds, exposure_ev100, auto_white_balance}`：同时把曝光钉在当前值、关自动白平衡、重启时域历史；`{enabled:false}` 恢复。命令行 `--deterministic`、`--exposure EV`。
- `restart_temporal {full:true}`（新计数器 `fullRestart`，不改变旧 `temporalRestart` 的行为，`--wait-for-scene` 的收敛测量不受影响）：除了时域历史，还清空 DDGI 探针、DDGI 射线旋转序号归零、太阳阴影级联缓存全部重画。
- 实测（R34 摄影棚，64 帧）：只重启时域历史时 A 对 A 的噪声底是 PSNR 50 dB、0.77% 像素变化；加上完整重启后是 101.8 dB、最大差 2/255、0.00%。

### A/B 与图像比较
- `tools/engine_control/image_compare.py`：PSNR、NVIDIA FLIP（`pip install flip-evaluator`，可选）、平均/最大差、超过阈值的像素比例、16×9 网格里差异最大的 8 块区域，输出热力图、FLIP 图和 A|B|热力图拼图。
- `engine_control.ab_compare()`（`mectl.py ab`、MCP `engine_ab`）：冻结时间 → A → B → A，每次完整重启后渲 N 帧再截图；A 对 B 是改动，A 对 A 是噪声底；结束后恢复 A 并关闭确定性模式。整个过程约 3 秒。

### 场景读写
- `scene.get {path}` / `scene.set {values}`：复用场景文件的 YAML 序列化（`SerializeEditorSceneData` / 新的 `ParseEditorSceneData`），路径用点分隔，数组可以用下标或 tag（`lights.Key box.intensity`）。类型必须和原值一致，全部生效或全部不生效。可写的部分：environment、lights、实体的 tag 与 transform、drive_paths、minimap；模型路径等需要重新加载的字段会报错。
- 顺带修了 `entity.set` 没有 `MarkTransformDirty` 的问题。

### 崩溃报告
- `engine/platform/crash`：未处理异常过滤器、`std::terminate`、`abort`、purecall、CRT 无效参数，都交给一个专门的报告线程（崩溃线程的栈可能已经溢出），写 minidump 和文本报告到 `%LOCALAPPDATA%/MiniEngine/crashes`：原因、DbgHelp 符号化的调用栈（函数、文件、行号）、日志最后 300 行。
- RenderDoc 和部分驱动加载时会换掉进程的异常过滤器，所以设备创建后 `Reassert()` 再装一次。
- Release 现在也生成 PDB（`/Z7` + 链接 `/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO`），否则 Release 崩溃只有地址。
- 客户端连接断开时自动找崩溃报告，把原因和栈顶附在错误里。`debug.crash {kind}` 用来验证（report / access_violation / abort / throw）。

### RenderDoc
- `engine/platform/renderdoc`（`third_party/renderdoc/renderdoc_app.h`，MIT）：`--renderdoc` 或 `MINIENGINE_RENDERDOC=1` 在创建设备之前加载 RenderDoc，关掉叠加层和抓帧热键。
- `renderdoc.capture {frames}` 等 `.rdc` 写完后回答；`renderdoc.list`、`renderdoc.open`（打开 RenderDoc UI）。
- 加载了 RenderDoc 时，GPU 计时器的每个 `Mark` 都会插一个调试标记 `end: <pass>`，抓到的帧按 pass 分好段。
- `tools/engine_control/rdc_summary.py`（MCP `renderdoc_summary`）：用 `renderdoccmd convert` 转成 XML，流式解析出每个 pass 的 draw、dispatch、barrier、管线切换数。R34 摄影棚一帧：34 个 pass，约 40 秒。

### UI 输入注入
- vcpkg `imgui[test-engine]`（ocornut/imgui_test_engine v1.92.9b，和 ImGui 同版本）。测试引擎在第一次 `ui.run` 时创建（创建后要等两帧才能接测试），每帧 `ImGui::Render` 之后调用 `PostSwap`。
- `ui.windows` 列出窗口（包括 docking 进去的、是否是选中的标签）；`ui.run {steps}` 用模拟输入操作编辑器：click、check、menu、key、type、drag、input、info、list 等，引用写 ImGui 路径（`//Graphics Debug/**/Ray traced reflections`）。只把输入送给 ImGui，不动真实鼠标，窗口隐藏或在另一个虚拟桌面也能用。
- 限制：`list` 列不出弹出菜单里的项（菜单本身可以用路径点）；视口里的相机操作走 SDL 输入，不归 ImGui 管。
- Dear ImGui Test Engine 的许可证不是 MIT：个人、非商业和小公司免费，其他情况要买授权。

## 没做的 / 以后

- 材质参数的读写（材质不在场景文件里，`scene.set` 改不到）。
- 控制通道不做鉴权：只监听回环地址，本机任何进程都能连接。
