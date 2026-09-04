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

- 编辑器停靠界面与视口交互：场景面板、资产浏览器、模型处理与预览、材质图、主题、相机与输入监视；支持选择、组合移动/旋转与缩放 gizmo、拖放放置、键鼠和手柄相机控制。
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

仓库内依赖只允许安装到 `.deps/vcpkg_installed/<architecture>/`；当前 Windows preset 分别使用 `x64/` 和 `x86/`。不要把 `VCPKG_INSTALLED_DIR` 指向 `out/`、`cmake-build-*`、仓库根 `vcpkg_installed/` 或其他仓库内目录，也不要使用相对路径。确需共享依赖时，使用同时位于源码树和当前 CMake 二进制树之外的绝对路径。该门禁只在实际 vcpkg toolchain 生效，并会在 `project()` 安装依赖前拒绝不符合约束的路径；无关的自定义 toolchain 不需要 vcpkg 变量。

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

macOS ARM64 沿用 `macos-debug`/`macos-release`（`arm64-osx` → `arm64/`），Intel macOS 使用 `macos-x64-debug`/`macos-x64-release`（`x64-osx` → `x64/`）。Linux x64 沿用 `linux-debug`（`x64-linux` → `x64/`），Linux ARM64 使用 `linux-arm64-debug`（`arm64-linux` → `arm64/`）。省略 Bash 构建脚本的 preset 时，会按这四种主机组合选择对应 Debug 入口。上述名称是当前配置入口，不代表本轮在对应平台完成了构建或 GUI 验收。

### vcpkg overlay port

`cmake/vcpkg-overlay-ports/` 通过所有 preset 的 `VCPKG_OVERLAY_PORTS` 生效，当前只覆盖 `tinygltf`。上游 port 用 `vcpkg_from_github` 拉取 GitHub 自动生成的源码归档并按 SHA512 固定；该归档被重新压缩后哈希不再匹配，port 直接下载失败。覆盖版改用 `vcpkg_from_git` 拉取 `v3.0.0` 对应的提交 `cfcadfa8d14eb489d97b6324838ae100410edcc7`，git 对象按内容寻址，不会像重新压缩的 tarball 那样漂移；除拉取方式外与上游 portfile 逐行一致。

overlay 会一直遮蔽上游同名 port：版本号仍是 `3.0.0`，所以刷新 baseline 后既不会切回上游，也拿不到 3.0.x 的后续修复。上游 port 记录的哈希与服务端一致后，删除 `cmake/vcpkg-overlay-ports/tinygltf/` 即可；overlay 目录清空后一并移除各 preset 的 `VCPKG_OVERLAY_PORTS`。

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
- 渲染端没有 `alphaMode` 分类与半透明排序、没有视锥剔除、没有阴影与抗锯齿、没有环境镜面/IBL；显存按每 submesh 独立分配。缺口清单见 2026-07-30 的开发记录。
- 脚本、动画、物理、音频、Play 模式和完整运行时分层未实现。

## 8. 路线图

已完成的阶段包括：编辑器/资产模块结构拆分、模型与灯光的统一实体模型、EnTT P0/P1/P2 数据路径和只读 Registry 边界，以及资产 UUID/sidecar 注册。

后续工作只聚焦两条已有方向：

1. 明确编辑器数据与运行时数据的边界，避免把编辑器状态直接等同于未来运行时状态。
2. 设计更完整的 RHI 与资源生命周期，使 GPU 缓冲、贴图和描述符可以细粒度更新，并逐步降低 Vulkan 后端对编辑器基类的耦合。

## 9. AI 与维护规则

1. 改动前阅读本 README，并继续阅读所涉模块的 CMake 和实现文件；当前源码优先于历史资料。
2. 遵守 UV、米制单位、模型不改源文件、UUID/场景序列化和模块依赖方向等硬性约定。
3. 保留工作区中与任务无关的未提交修改，尤其不要覆盖、暂存或混入用户的 `imgui.ini` 和既有 `docs/superpowers/plans/` 内容。
4. 实质性代码变更后，同步 README 的当前状态和开发记录；大型专项材料放入 `docs/`，README 保留可检索的结论和约束。
5. 区分验证类型：构建和 CTest 证明编译/自动化回归，60 帧冒烟验证基本启动路径，GUI 和视觉效果仍需人工确认。没有实际执行的项目不得写成已验证。

## 10. 决策与开发记录

以下为历史记录，不是本轮验证结果；保留它们是为了说明仍影响维护决策的原因与踩坑。

### 2026-07-30 — 渲染管线缺口审查（静态核验，未构建未运行）

本轮只通读了 `engine/renderer/vulkan/` 全部实现与 `shaders/vulkan/triangle.{vert,frag}`，没有构建、没有 CTest、没有 GUI 确认；以下条目是代码事实，不是运行结论。2026-07-16 审查记录的维护结论经复核仍然成立：背面剔除绕向、Debug 验证层、贴图生命周期与 cache key 配对、UBO std140 布局，以及按交换链镜像分配 `renderFinishedSemaphore`，都是已经落实且后续修改必须保持的约束；当时明确留下的负缩放镜像绕向问题仍未解决。mipmap 生成、各向异性采样（`device.cpp` 已在逻辑设备启用 `samplerAnisotropy`，`texture.cpp` 才据此建采样器）、贴图池与批量上传也都在位。其余缺口分三类：

**正确性**

- 没有 `alphaMode` 分类：`pipeline.cpp` 对所有材质无条件开混合且深度写入常开，`renderer.cpp` 按 submesh 插入顺序绘制。后果是 OPAQUE 材质的贴图 alpha 也参与混合（违反 glTF 规范，Sponza 的 `dirt_decal_*_Opacity` 会踩到）、BLEND 材质既不排序又写深度、不透明物体白付混合开销。补法是把 alphaMode 带到渲染端并拆成 opaque / mask / blend 三档变体（叠加单双面后为 6 条管线）。
- 灯光超过 `kMaxSceneLights`（8）在 `uniform_buffer.cpp` 静默截断，既不按贡献排序也不告警。
- 导入的 BLEND/MASK 材质 alpha 被平方：`gltf_model_loader.cpp` 把 glTF alpha 同时写入 `opacity` 与 `baseColor[3]`，而 `scene_renderables.cpp` 取两者乘积。`opacity` 是编辑器独立滑杆（默认 1.0），导入路径不应再写 `baseColor[3]`。
- 环境项只有漫反射（`triangle.frag` 的 `ambient = albedo * ambientAccum * ao`），无环境镜面/IBL，也无 kD 能量分配，metallic=1 的表面在直射高光以外为纯黑。
- 负缩放镜像仍未按对象翻转 `frontFace`（沿用 2026-07-16 记录的已知欠账）。

**性能与架构**

- 全仓库无视锥剔除，`BuildDrawItems` 每帧遍历全部 submesh（Sponza 405 次 draw）；`ModelComponent` 已拆出的包围盒足以支撑 CPU 剔除。
- `buffer.cpp` 每个 submesh 顶点/索引各做一次 `vkAllocateMemory`，Sponza 约 810 次常驻分配，与 `maxMemoryAllocationCount` 同数量级；需要 VMA 或自建 suballocator。
- `VulkanBuffer` 上传后仍持有顶点/索引 CPU 拷贝，与 `RendererWorld` 的 `CpuRenderSubmesh::mesh` 重复存一份。
- `VkDescriptorSetLayout` 由 `VulkanUniformBuffer` 创建，导致每次内容变化都连带重建管线；管线又用静态 viewport/scissor，于是拖拽视口分隔条的每一帧都要 `vkDeviceWaitIdle` + 重建整套管线资源。改用 `VK_DYNAMIC_STATE_VIEWPORT/SCISSOR` 并把布局提为与内容无关的静态对象可消除该卡顿；另外全程无 `VkPipelineCache`。
- 描述符规模按 材质数 × 交换链图像数 线性膨胀（每材质 13 个 combined image sampler），且 `renderer.cpp` 对每个 submesh 无条件新增一份 `MaterialTextureSlots`，相同材质未合并。终点是 bindless（`descriptorIndexing` + 贴图数组 + 材质 SSBO），中间态可先做材质去重。

**缺失特性**

- 无阴影；无抗锯齿（`rasterizationSamples = 1`，也无 FXAA/TAA）；无后处理链与独立 HDR 中间靶，色调映射是 `triangle.frag` 内硬编码的 Reinhard；`uniform_buffer.cpp` 的环境光 `(0.05, 0.05, 0.08)` 硬编码、编辑器不可改。
- 无深度测试的 3D 调试绘制：灯光 gizmo 走 ImGui 2D 投影线，永远浮在最上层；无世界网格、线框模式与包围盒可视化。
- 着色器是单一 uber shader、构建期 glslc 编译，无热重载与变体系统；无 timestamp query 与帧统计，上述各项收益目前无法量化。
- 已知偏差（非缺陷）：视口与编辑器 pass 都用交换链的 `B8G8R8A8_SRGB`，ImGui 顶点颜色会被再编码一次导致 UI 偏亮；场景贴图路径的 sRGB 解码/编码是抵消的。

建议补齐顺序：alphaMode 分类与半透明排序 → 视锥剔除 → 动态 viewport/scissor 与静态描述符布局 → 显存 suballocator → 灯光排序告警与 alpha 平方修正 → 之后才是阴影 / IBL / MSAA。

### 2026-07-26 — 组合移动/旋转 Gizmo

- 模型编辑默认使用一次 `ImGuizmo::Manipulate` 调用呈现 `TRANSLATE | ROTATE`：移动轴、平面手柄和旋转环同时可用；`R` 在 Combined/Scale 间切换，`W`/`E` 不再切换模式，拖拽期间忽略 `R`。Point 与 Ambient 灯光仍限制为仅平移。
- 平移、旋转和缩放继续使用各自的吸附值；吸附族在拖拽开始时锁定到结束。平移与旋转手柄重叠时以平移优先，与 ImGuizmo 的内部命中顺序一致，避免界面操作和吸附步长不匹配。
- YAML 只写 `operation: combined` 或 `operation: scale`；旧场景中的 `translate`、`rotate` 都按 Combined 读取，未知值沿用回退模式。
- 功能分四个提交实现并快进合并到 `main`，最终提交为 `3884c98`。x64 Debug 构建通过，CTest 为 `3/3`，Vulkan 60 帧冒烟测试正常退出；用户完成 GUI 验收后才合并，覆盖组合手柄、模式切换、World/Local、各类吸附和灯光限制。

### 2026-07-22 — vcpkg 磁盘布局边界

- 两个依赖引导脚本都显式推导大小写敏感的 triplet 架构前缀，并把 manifest 安装根传给 `.deps/vcpkg_installed/<architecture>/`；只打印安装根的模式不会检查命令、克隆、引导、下载或安装。
- CMake 在 `project()` 前拒绝相对安装根、仓库内错误目录和当前二进制树内目录；门禁只作用于实际 vcpkg toolchain，Windows 路径比较忽略大小写。
- `tests/vcpkg_layout_contract.cmake` 解析 preset JSON 并执行隔离的配置与脚本探针，覆盖 x64、x86、arm64、外部共享根、错误目录、未知/混合大小写前缀及非 vcpkg toolchain。
- 一次性清理已经完成：7 个禁用路径全部不存在；排除临时 `.worktrees` 后，可比逻辑大小为 `21.335 GiB`，基线为 `43.230 GiB`，逻辑减少 `21.895 GiB`（50.6%）。x64/x86 Debug 构建均通过，每个架构的 CTest 均为 `2/2`；逻辑文件长度不等于精确的物理空间回收量。`.deps/vcpkg/{downloads,packages,buildtrees}` 仅保留为可重建缓存，不作为安装根。

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
