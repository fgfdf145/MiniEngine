# MiniEngine

MiniEngine 是一个以 C++20 编写、基于 SDL3、Vulkan、Dear ImGui 与 EnTT 的 3D 场景编辑器原型。编辑器工作流已经形成闭环；脚本、物理、动画、音频与 Play 模式等完整运行时闭环尚未形成。

本文件面向 AI 助手和长期维护者。当前源码、各模块 `CMakeLists.txt`、`CMakePresets.json` 与 `vcpkg.json` 是架构事实的优先来源；[docs/PROJECT_SUMMARY.md](docs/PROJECT_SUMMARY.md) 仅是历史阶段性分析材料，不能替代当前源码。

## 1. 项目定位

项目提供一个桌面 3D 编辑器原型：在场景中导入和摆放 glTF 模型，编辑实体、变换、灯光与材质，并将场景持久化为 YAML。当前关注的是编辑器数据流、资源管理和 Vulkan 渲染，不把它描述为已具备完整游戏运行时的通用引擎。

- 语言标准：C++20。
- 构建体系：CMake + vcpkg manifest；根目录 [MiniEngine.slnx](MiniEngine.slnx) 是 Visual Studio 的 CMake 包装入口。
- 当前依赖：SDL3、Vulkan、Dear ImGui、ImGuizmo、EnTT、yaml-cpp、tinygltf、GLM、spdlog、stb 和 shaderc。

## 2. 已实现功能

以下项目应以当前源代码和测试目标为准，而不是作为本轮运行或 GUI 验收的声明。

- 编辑器停靠界面与视口交互：场景面板、资产浏览器、模型处理与预览、材质图、主题、相机与输入监视；支持选择、gizmo、拖放放置、键鼠和手柄相机控制。
- 统一场景：模型和灯光共享稳定的编辑器顺序；`ModelComponent`、`LightComponent`、变换、包围盒与 `SceneEntityIdComponent` 构成场景实体。场景采用 YAML v3，并保留旧 v1/v2 的加载兼容路径。
- 资产工作流：资产浏览、复制、粘贴、重命名、删除与批量操作；资产树为可注册模型和纹理维护 UUID sidecar。
- glTF 2.0：导入 `.gltf` 与 `.glb`、复制模型包与关联资源、三角化/法线/切线后处理、单位换算；每个已导入材质可保存 `.material.yaml` sidecar，并可编辑 PBR 材质图与材质贴图。
- 渲染：Cook-Torrance PBR、材质贴图、场景视口、多类型灯光（Directional、Point、Spot、Area、Ambient）及灯光 gizmo。
- 后台任务：模型和场景使用异步加载状态机，资产导入在后台执行；主线程在逐帧阶段泵送结果并刷新 UI 或 CPU Renderable。
- 编辑器设置：`miniengine.settings.json` 保存界面缩放、窗口可见性和主题等设置。

## 3. 架构与运行流程

### CMake 目标边界

目标依赖遵循由应用层指向基础层的方向；下层不得反向包含上层头文件。当前主要关系为：

```text
miniengine_app
  -> engine_application
engine_application -> engine_core / engine_platform / engine_renderer
engine_renderer -> engine_render_core / engine_editor / engine_logic (private)
engine_editor -> engine_render_core / engine_logic / engine_asset / engine_scene / engine_platform / engine_core
engine_render_core -> engine_core / engine_scene / engine_asset
engine_logic -> engine_core / engine_scene
engine_asset -> engine_core / engine_scene
engine_platform -> engine_core
```

`engine_scene` 保持场景数据和只读查询接口；`engine_logic` 实现 `IEditorWorld` 的实体生命周期、选择和 YAML 序列化；`engine_asset` 负责模型、贴图、缓存与资产注册；`engine_editor` 负责面板与编辑器请求；`engine_renderer` 提供 RHI 工厂和 Vulkan 实现；`engine_application` 管理窗口、参数解析与主循环。

### 启动与逐帧链路

1. `app/main.cpp` 初始化日志，解析参数，创建 `EditorApplication`。
2. `EditorApplication` 创建 SDL `Window`、共享 `RendererSharedState`，由 RHI 工厂创建指定后端；当前可用值为 Vulkan。
3. `VulkanRenderer` 继承 `EditorRenderBackendBase`。基类先加载编辑器设置、初始化 `AssetRegistry`、创建 `IEditorWorld`、加载默认场景，并建立初始 CPU Renderable。
4. 每帧先轮询 SDL 事件并更新输入/相机，再处理 UI 请求、异步模型或场景加载及后台导入；必要时更新 CPU Renderable。
5. `VulkanRenderer::DrawFrame()` 在内容变更时调用 `UploadSceneResources()`，随后记录场景和 ImGui 命令并提交交换链显示。

### 场景、CPU 与 GPU 数据流

`IEditorWorld` 是场景真相来源：稳定的 `GetSceneOrder()` 用于编辑器列表与序列化，运行时的高频查询使用 EnTT view。`ISceneWorld::Registry()` 仅返回 `const entt::registry&`；创建、销毁、组件写入和脏标记必须经过场景接口。

模型或材质变更会以 `ModelRenderableDirty` 标记实体。服务层将受影响实体转换为 `RendererWorld` 中的 `CpuRenderSubmesh`，可按实体替换或移除，避免把 CPU Renderable 增量更新误写成全场景重建。当前 Vulkan 端仍会在 Renderable 集合改变时重建并整批上传 GPU 缓冲、贴图和描述符资源；这是已知的资源生命周期边界。

`IRenderBackend` 是很薄的接口，当前只有 Vulkan 实现。`VulkanRenderer` 仍继承 `EditorRenderBackendBase`，说明编辑器帧循环与后端尚未完全解耦；不要把现状描述成完整的多后端 RHI。

## 4. 仓库结构

| 路径 | 维护职责 |
| --- | --- |
| `app/` | 最薄的程序入口和 `miniengine_app`。 |
| `cmake/` | vcpkg、编译选项、依赖发现与 IDE 组织的公共 CMake 模块。 |
| `engine/core/` | 日志、输入、共享 UUID 与后端类型。 |
| `engine/platform/` | SDL 窗口、文件对话框、界面缩放。 |
| `engine/scene/` | 组件、场景只读接口、材质图和世界单位。 |
| `engine/logic/` | `IEditorWorld`、实体/选择管理、场景 YAML 序列化。 |
| `engine/asset/` | glTF/贴图加载、模型缓存、资产 UUID 注册表。 |
| `engine/editor/` | 编辑器后端基类、UI 面板和编辑服务。 |
| `engine/renderer/` | `engine_render_core`、RHI 接口和 Vulkan 后端。 |
| `engine/application/` | `EditorApplication` 生命周期和命令行解析。 |
| `assets/` | 项目资产、导入模型包、材质 sidecar 和默认场景。 |
| `shaders/` | Vulkan 着色器源文件。 |
| `tests/` | 场景身份/YAML 兼容回归测试，见 [tests/CMakeLists.txt](tests/CMakeLists.txt)。 |
| `scripts/` | 依赖引导、构建和解决方案生成脚本，详见 [scripts/README.md](scripts/README.md)。 |
| `docs/` | 设计和历史资料；`PROJECT_SUMMARY.md` 只作为历史阶段性分析。 |
| `miniengine.settings.json` | 编辑器设置持久化文件。 |

## 5. 数据、资源与实现约定

这些约定直接影响资源、场景和渲染正确性；修改相关代码前先核对对应模块。

- **Vulkan UV**：贴图加载不做垂直翻转；UV 原点为左上角，行 0 对应 `v0`。不要引入 OpenGL 风格的全局翻转或 `1 - v` 补偿。
- **单位**：世界单位为米，常量在 `engine/scene/world_units.h`；导入和编辑器 UI 都以此为基准。
- **模型导入**：导入将模型复制到 `assets/models/<bundle>/`，并生成自身资源与 `.material.yaml` sidecar；不修改源模型文件，已有目标文件也不会被导入流程覆盖。
- **资产 UUID**：可注册资产限于 `assets/` 根下的模型和纹理。sidecar 命名为 `<完整文件名>.miniengine_asset.yaml`，资产浏览和场景扫描会忽略该后缀。场景保存 `source_path` 与 `source_uuid`：加载时 UUID 优先，保存时路径优先；重复 UUID 通过 sidecar 的 `file` 与实际文件名仲裁，副本获得新 UUID。
- **场景身份**：场景 YAML v3 写 `entity_uuid` 和 `selected_entity_uuid`。`entt::entity` 仅在 registry 生命周期内有效，不能持久化或作为跨加载引用；旧 v1/v2 可按旧模型索引加载后升级。
- **场景写入边界**：`ISceneWorld::Registry()` 对外只读。实体生命周期、组件编辑、变换刷新和 Renderable 脏标记必须调用场景接口，以保持顺序、选择、UUID 索引和缓存同步。
- **CMake 与 IDE**：依赖方向不可反转。根 [MiniEngine.slnx](MiniEngine.slnx) 与 `MiniEngine.vcxproj` 只是 IDE/Makefile 包装层；[CMakePresets.json](CMakePresets.json) 是构建参数、依赖和输出目录的唯一事实来源。

## 6. 构建、运行与验证

前提：C++20 工具链、CMake、Vulkan SDK/运行环境，以及可用的 vcpkg。依赖版本和 feature 以 [vcpkg.json](vcpkg.json) 为准；首次配置前先引导本地依赖。

### vcpkg 磁盘布局

仓库内依赖只允许安装到 `.deps/vcpkg_installed/<architecture>/`；当前 Windows preset 分别使用 `x64/` 和 `x86/`。不要把 `VCPKG_INSTALLED_DIR` 指向 `out/`、`cmake-build-*`、仓库根 `vcpkg_installed/` 或其他仓库内目录。确需共享依赖时，使用仓库外的绝对路径。CMake 会在安装依赖前拒绝不符合约束的路径。

```powershell
# Windows：引导 vcpkg manifest 依赖
.\scripts\bootstrap-deps.ps1

# 配置和构建 Visual Studio 2026 x64
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --parallel

# 脚本入口（preset 可替换）
.\scripts\build.ps1 vs2026-x64-debug

# 生成按 CMake 目标展开的解决方案
.\scripts\generate-sln.ps1 -Preset vs2026-x64
```

Windows 的根 [MiniEngine.slnx](MiniEngine.slnx) 对应 Debug/Release × x64/Win32，并委托 `vs2026-x64` 或 `vs2026-x86` preset。不要在 `.vcxproj` 中复制 CMake 的编译选项或依赖逻辑。Ninja 的 `x64-debug`/`x64-release`、`x86-debug`/`x86-release` 仍可用于直接 CMake 工作流；x64 与 x86 的 vcpkg 安装根目录隔离。

macOS 使用 `macos-debug` 或 `macos-release`，Linux 使用 `linux-debug`。这些 preset 是当前配置入口，不代表本轮在对应平台完成了构建或 GUI 验收。

运行参数由 `EditorApplication::ParseArgs()` 提供：

```text
--backend vulkan    选择 Vulkan 后端（当前唯一实现）
--model <path>      启动后请求加载指定模型
--frames <count>    渲染指定正整数帧后退出
```

代码改动的自动化验证入口如下；60 帧进程退出和 CTest 不能替代人工 GUI/视觉确认。本文档改动只进行静态核验，不把以下命令的历史或建议用法表述为本轮运行结果。

```powershell
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

## 7. 当前能力边界

- RHI 目前只有 Vulkan 后端，且后端仍承载 `EditorRenderBackendBase` 的编辑器流程。
- glTF 尚未完整处理额外 UV 集、sampler wrap 和 `KHR_texture_transform`。
- 启动默认场景配置与部分引用刷新仍以路径为主，未覆盖所有 UUID 解析路径。
- CPU Renderable 支持按实体增量更新；Vulkan GPU 资源仍在内容变化时整批上传。
- 脚本、动画、物理、音频、Play 模式和完整运行时分层未实现。

## 8. 路线图

已完成的阶段包括：编辑器/资产模块结构拆分、模型与灯光的统一实体模型、EnTT P0/P1/P2 数据路径和只读 Registry 边界，以及资产 UUID/sidecar 注册。

后续工作只聚焦两条已有方向：

1. 明确编辑器数据与运行时数据的边界，避免把编辑器状态直接等同于未来运行时状态。
2. 设计更完整的 RHI 与资源生命周期，使 GPU 缓冲、贴图和描述符可以细粒度更新，并逐步降低 Vulkan 后端对编辑器基类的耦合。

## 9. AI 与维护规则

1. 改动前阅读本 README，并继续阅读所涉模块的 CMake 和实现文件；当前源码优先于历史资料。
2. 遵守 UV、米制单位、模型不改源文件、UUID/场景序列化和模块依赖方向等硬性约定。
3. 保留工作区中与任务无关的未提交修改，尤其不要覆盖、暂存或混入用户的 `imgui.ini`。
4. 实质性代码变更后，同步 README 的当前状态和开发记录；大型专项材料放入 `docs/`，README 保留可检索的结论和约束。
5. 区分验证类型：构建和 CTest 证明编译/自动化回归，60 帧冒烟验证基本启动路径，GUI 和视觉效果仍需人工确认。没有实际执行的项目不得写成已验证。

## 10. 决策与开发记录

以下为历史记录，不是本轮验证结果；保留它们是为了说明仍影响维护决策的原因与踩坑。

### 2026-07-22 — vcpkg 磁盘布局边界

- 两个依赖引导脚本都显式推导 triplet 的架构前缀，并把 manifest 安装根传给 `.deps/vcpkg_installed/<architecture>/`。
- CMake 在安装依赖前拒绝仓库内错误的 `VCPKG_INSTALLED_DIR`，避免构建目录或仓库根目录成为安装根。
- `tests/vcpkg_layout_contract.cmake` 覆盖 x64、x86、arm64、本仓库外共享根、错误目录和未知架构前缀的契约。
- 已定义一次性清理过时依赖副本的范围；`.deps/vcpkg/{downloads,packages,buildtrees}` 仅保留为可重建缓存，不再作为安装根。实际回收将在验证后记录。

### 2026-07-17 — Visual Studio 2026 `.slnx` 与并行构建

- 根 `MiniEngine.slnx`/`.vcxproj` 提供 CMake 的稳定 IDE 包装入口；完整目标图仍由 `out/build/<preset>/MiniEngine.slnx` 提供。
- 根包装层把 Debug/Release × x64/Win32 映射到既有 preset，并显式使用 `cmake --build --parallel`；CMake 配置仍是唯一事实来源。
- x64/x86 的 vcpkg 安装树隔离，避免切换 triplet 时移除另一架构依赖；Win32 在缺少 32 位 SDK loader 时使用 vcpkg 的 x86 Vulkan loader。

### 2026-07-16 — Vulkan 剔除、验证层、资源生命周期与同步

- 默认管线使用背面剔除和 `FRONT_FACE_COUNTER_CLOCKWISE`；投影的 Y 翻转已经抵消 Vulkan 帧缓冲方向差异。未来若支持负缩放镜像，需要按对象处理绕向。
- Debug 路径接入 Khronos 验证层和 debug messenger；第三方隐式层可能注入 swapchain flag 并造成与引擎无关的警告，排查时需隔离隐式层。
- 贴图销毁必须在等待在途帧之后进行；资源池 cache key 与贴图集合必须同步，否则重建会提前销毁仍在使用的贴图。`renderFinishedSemaphore` 按交换链镜像分配，避免 present 仍占用时复用。
- UBO 结构仅使用 16 字节倍数成员（`mat4`/`vec4`），以锁定 Vulkan 布局；不要重新引入依赖 include 顺序的默认对齐宏。

### 2026-07-16 — EnTT P0/P1/P2 与场景 UUID

- 统一模型与灯光的场景顺序，编辑器列表/YAML 使用稳定顺序，高频系统使用 EnTT view；批量清场先清理编辑器侧顺序和选择，避免销毁回调产生 O(N²) 操作。
- `ModelComponent` 拆分出包围盒与编辑器元数据；`ModelRenderableDirty` 支持单实体 CPU Renderable 刷新，`WorldTransformComponent` 批量刷新变换缓存。
- `SceneEntityIdComponent` 是持久身份，重复 UUID 由后加载实体重分配；`Registry()` 只读以保护场景不变量。EnTT 3.16 的单组件 view 默认 `swap_and_pop` 存储应使用 `size()`，多组件 view 才使用 `size_hint()`。

### 2026-07-15 — 资产 UUID、模块拆分、UV、光照与场景保存

- `AssetRegistry` 用线程安全 UUID sidecar 跟踪资产，重命名、删除和复制后保持注册表一致；复制资产时要移除复制来的 sidecar，使原件保持身份、复制件获得新 UUID。
- `engine/editor` 从渲染器职责中拆出，渲染器分为 `engine_render_core` 和 `engine_renderer`；模型缓存置于资产层。
- Vulkan UV 采用不翻转贴图、左上原点；多光源 PBR、灯光 gizmo 与场景保存是在此演进过程中形成的编辑器能力。
