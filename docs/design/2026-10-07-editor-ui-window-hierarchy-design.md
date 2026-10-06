# 编辑器 UI 窗口继承体系

## 背景

重构前，编辑器 UI 是一个 `EditorUiController` 大类：九个面板、三个浮动窗口和四个模态框都是它的 `Draw*` 成员函数，分散在 `ui/*.cpp` 中；每个窗口的状态（`m_show*Window`、模型处理器的十几个 `m_modelProcessor*`、导入对话框的待定状态……）全部是它的成员。窗口之间互相调用（场景面板打开模型处理器、资产浏览器改写模型处理器的路径、Preferences 打开 Theme）都直接改对方的成员。新增一个面板要改头文件、`Draw()` 里的 if 链、Window 菜单表和停靠布局四处。

## 目标

参照现代引擎编辑器的窗口模型（Unity `EditorWindow`、Unreal `SDockTab` + `FTabManager`、Godot 的 dock）：

- 每个窗口是一个类，自己持有状态，自己画自己。
- 窗口的种类用继承表达：可停靠面板、浮动工具窗口、模态框。
- 一个管理器拥有全部窗口，统一驱动生命周期；窗口之间通过管理器找到彼此。
- 对后端（`EditorRenderBackendBase`、`VulkanRenderer`）的接口不变。

## 类层次

```text
EditorWindow                     ui/framework/editor_window.h
├─ EditorPanel                   ui/framework/editor_panel.h
│  ├─ ScenePanel                 ui/panels/scene_panel.*            停靠：左
│  ├─ ViewportPanel              ui/panels/viewport_panel.*         停靠：中
│  ├─ CameraPanel                ui/panels/camera_panel.*           停靠：右下
│  ├─ GraphicsDebugPanel         ui/panels/graphics_debug_panel.*   停靠：右下
│  ├─ AssetBrowserPanel          ui/panels/asset_browser_panel.*
│  ├─ InputMonitorPanel          ui/panels/input_monitor_panel.*
│  ├─ VehiclePanel               ui/panels/vehicle_panel.*
│  ├─ SuspensionRigsPanel        ui/panels/suspension_rigs_panel.*  （原 SuspensionRigWindow）
│  └─ ThemePanel                 ui/panels/theme_panel.*
├─ ModelProcessorWindow          ui/windows/model_processor_window.*  "Model Preview"
├─ PreferencesWindow             ui/windows/preferences_window.*
├─ KeyboardShortcutsWindow       ui/windows/keyboard_shortcuts_window.*
└─ EditorModal                   ui/framework/editor_modal.h
   ├─ SceneResetModal            ui/modals/scene_reset_modal.*
   ├─ Kn5ImportModal             ui/modals/kn5_import_modal.*
   ├─ ImportConflictModal        ui/modals/import_conflict_modal.*
   └─ AboutModal                 ui/modals/about_modal.*
```

### EditorWindow

- 身份：`GetId()`（命令与设置用，如 `graphics_debug`）、`GetTitle()`（ImGui 窗口名，停靠布局和聚焦按它查找）、`GetIcon()`。
- 打开状态：`Open()` / `Close()` / `IsOpen()`，`OpenFlag()` 给 ImGui 的 `p_open` 和 Window 菜单使用。
- `Draw(EditorContext&)` 是不可重写的模板方法：`ShouldDraw` → `PreBegin` → `BeginWindow` → `OnGui` → `EndWindow` → `PostEnd`。子类只重写钩子：
  - `OnGui`：窗口内容，纯虚。
  - `PreBegin` / `PostEnd`：`SetNextWindow*`、样式压栈与出栈（视口的零内边距、全屏无边框）。
  - `GetWindowFlags`、`GetImGuiName`、`IsClosable`：视口全屏时换成 `Viewport##Fullscreen`、无装饰、无关闭按钮。
  - `BeginWindow` / `EndWindow`：默认是 `ImGui::Begin/End`，模态框换成 `BeginPopupModal/EndPopup`。
- 生命周期：`Tick`（每帧，不论是否打开，用于隐藏时也要做的事）、`OnOpen`、`OnClose`（打开状态变化后由管理器调用，不论是谁改的：命令、标题栏 X、窗口自己）。
- `ShouldDraw` 默认是“打开着”；`DrawsInFullscreen` 默认否。

### EditorPanel

可停靠面板：注册即进入 Window 菜单（顺序即注册顺序），打开状态以 `GetSettingsKey()` 存入 `miniengine.settings.json`（资产浏览器保留旧键 `asset_manager`），构造时给出 `EditorDockSlot`，默认停靠布局（首次运行与 Window > Reset Layout）据此生成，不再在 `editor_dockspace.cpp` 里写死窗口名。

### EditorModal

`Open()` 只是请求，下一次绘制时在管理器的 ID 栈里 `OpenPopup`，居中显示；按钮里调用 `CloseModal()`。弹窗被按钮关闭或被 Esc 关掉后，下一帧管理器看到打开状态变为否并调用 `OnClose`——持有待定选择的模态框（导入冲突、kn5 导入）在这里把未作答的请求当作取消。模态框在全屏视口上也绘制。

### EditorWindowManager

- `Register<T>(args...)`：创建并拥有窗口，每种类型一个；`EditorPanel` 子类同时进入面板列表。
- `Find<T>()` / `Get<T>()` / `FindById()`；`Open<T>(focus)`、`Focus(window)`：聚焦请求在全部窗口绘制完后执行，所以本帧刚打开的窗口也能被聚焦。
- `TickAndDraw(context, fullscreen)`：全部 Tick → 绘制（全屏时只画 `DrawsInFullscreen` 的窗口）→ 对打开状态变化的窗口回调 `OnOpen` / `OnClose` → 执行聚焦请求。
- `BuildPanelMenuEntries()` 生成 Window 菜单条目（原 `EditorPanel` 结构体改名为 `EditorPanelMenuEntry`，名字让给面板基类）；`ApplyOpenState` / `WriteOpenState` / `CapturePanelOpenState` 负责设置的读写与“是否有面板开关”的判断。

### EditorContext 与共享状态

窗口每帧收到一个 `EditorContext`：

| 成员 | 内容 |
| --- | --- |
| `scene`、`camera`、`matrices` | 场景、编辑器相机与视口矩阵 |
| `frame` | `EditorFrameInput`：后端本帧给的错误信息、上传状态、视口纹理、尺寸与后端类型 |
| `result` | `EditorUiFrameResult`：窗口对后端的请求（与重构前相同） |
| `state` | `EditorSharedState`：多个窗口共用的编辑器状态——命令状态、渲染调试设置、音频、车辆设置（`EditorVehicleSettings`）、后端每帧写入的车辆/台架/录像/DLSS/显存状态 |
| `style` | `EditorStyle`：UI 缩放与主题调色板（原控制器里的 base style、默认/内置配色与 `ApplyUiScale`） |
| `windows`、`commands` | 窗口管理器与命令表 |

窗口是视图，数据放在 `EditorSharedState`、`EditorStyle` 或场景里，不放在别的窗口里。只属于一个窗口的状态（材质图画布、输入监视的滚动、悬挂台架的运行）留在该窗口类中。

### 外壳 EditorUiController

保留对后端的全部公开接口（`BeginFrame`、`Draw`、`WriteEngineSettings`、`Set*Status`、`QueueDroppedFile`、`RequestAssetBrowserRefresh`……），内部只剩：命令注册、命令状态与场景/渲染器的双向同步、全屏切换、文件命令（打开/保存/导入）、主菜单、工具栏、命令面板和停靠空间，然后把一帧交给 `EditorWindowManager::TickAndDraw`。`RegisterWindows()` 是全部窗口的注册表，新增面板只需一个类加一行注册。

## 行为变化

逐项保持了原有行为，以下是有意的小差异：

- **导入对话框不再依附资产浏览器窗口。** 原来 kn5 导入和“模型已导入”冲突两个模态框画在 Assets 窗口内部，Assets 窗口折叠或是隐藏的停靠标签时，从 File > Import Model 发起的导入看不到对话框；现在它们是独立模态框，总能弹出，全屏视口上也能弹出。
- **窗口绘制顺序** 改为注册顺序（面板按 Window 菜单顺序，然后浮动窗口，最后模态框）。只影响同一帧内的先后，例如在 Scene 面板点 Edit Materials 时 Model Preview 当帧即出现。
- **关闭悬挂台架窗口时停止实时台架** 在下一帧的 `Tick` 里发出，比原来晚一帧。
- Vehicle 面板的 Suspension Rigs 按钮、Preferences 里的 Open Theme / Open Graphics Debug 统一走 `EditorWindowManager::Open`，聚焦在本帧所有窗口画完后执行。

## 测试

- 新增 `tests/editor_window_framework_tests.cpp`（`miniengine.editor_window_framework`）：注册与查找、Window 菜单条目、Tick/OnGui/OnOpen/OnClose 的时机、全屏过滤、模态框的打开与按钮关闭、设置键的读写、Window 菜单命令与 Show All Panels、打开即聚焦。
- `suspension_rig_window_tests` 改用 `SuspensionRigsPanel::DrawContents`（面板内容可以不经外壳单独绘制测试）；`command_registry_tests` 跟随结构体改名。
