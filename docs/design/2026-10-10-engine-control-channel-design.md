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

## 没做的 / 以后

- 输入注入（ImGui 层的点击、键盘）和 RenderDoc 抓帧，作为后续命令接到这里。
- 通用的组件字段读写（目前只有 transform、相机、渲染设置、车辆）。
- 控制通道不做鉴权：只监听回环地址，本机任何进程都能连接。
