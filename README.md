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
- 渲染：Cook-Torrance PBR（含多次散射能量补偿）、材质贴图、场景视口、多类型灯光（Directional、Point、Spot、Area、Ambient，最多 1024 盏，局部灯按分簇查找）及灯光 gizmo；最亮的方向光投射 4 级级联阴影（CSM，每级 2048²，3×3 双线性 PCF），点光、聚光与面光从 4096² 阴影图集取阴影（每块 512²，共 64 块）。
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
| `tests/` | 场景身份/YAML 兼容、Gizmo 设置、材质 alpha 与渲染器纯逻辑单元测试，外加 vcpkg 布局和格式化两组契约测试，见 [tests/CMakeLists.txt](tests/CMakeLists.txt)。 |
| `scripts/` | 依赖引导、构建和解决方案生成脚本，详见 [scripts/README.md](scripts/README.md)。 |
| `docs/` | 设计和历史资料；`PROJECT_SUMMARY.md` 只作为历史阶段性分析。 |
| `miniengine.settings.json` | 编辑器设置持久化文件。 |

## 5. 数据、资源与实现约定

这些约定直接影响资源、场景和渲染正确性；修改相关代码前先核对对应模块。

- **Vulkan UV**：贴图加载不做垂直翻转；UV 原点为左上角，行 0 对应 `v0`。不要引入 OpenGL 风格的全局翻转或 `1 - v` 补偿。
- **单位**：世界单位为米，常量在 `engine/scene/world_units.h`；导入和编辑器 UI 都以此为基准。
- **模型导入**：导入将模型复制到 `assets/models/<bundle>/`，并生成自身资源与 `.material.yaml` sidecar；不修改源模型文件，已有目标文件也不会被导入流程覆盖。导入还会把模型文件内部的图片（`.glb` 负载、`data:` URI）解包成 `<bundle>/textures/` 下的真实文件；加载只读取，不写盘。因此贴图路径一律相对于模型文件所在目录——外部 URI 和嵌入图片没有例外。`--model` 指向 `assets/` 之外时会先自动导入，再加载导入后的副本。
- **资产 UUID**：可注册资产限于 `assets/` 根下的模型和纹理。sidecar 命名为 `<完整文件名>.miniengine_asset.yaml`，资产浏览和场景扫描会忽略该后缀。场景保存 `source_path` 与 `source_uuid`：加载时 UUID 优先，保存时路径优先；重复 UUID 通过 sidecar 的 `file` 与实际文件名仲裁，副本获得新 UUID。
- **场景身份**：场景 YAML v3 写 `entity_uuid` 和 `selected_entity_uuid`。`entt::entity` 仅在 registry 生命周期内有效，不能持久化或作为跨加载引用；旧 v1/v2 可按旧模型索引加载后升级。
- **场景写入边界**：`ISceneWorld::Registry()` 对外只读。实体生命周期、组件编辑、变换刷新和 Renderable 脏标记必须调用场景接口，以保持顺序、选择、UUID 索引和缓存同步。
- **Vulkan 帧内布局转换**：帧内所有图像布局变更都是显式 `vkCmdPipelineBarrier`，由 `RenderTargetLayoutTracker` 按各 pass 的 `Io()` 声明推出。每个 render pass 的每个附件都必须声明 `initialLayout == finalLayout`，不得让 render pass 在 tracker 背后隐式转换附件布局。新增 pass 时把 `finalLayout` 改成一个顺手的值，就会静默破坏布局跟踪：Vulkan 不报错，验证层也不报，tracker 之后发出的 barrier 起点已经是错的。
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

`cmake/vcpkg-overlay-ports/` 通过所有 preset 的 `VCPKG_OVERLAY_PORTS` 生效，当前有 `tinygltf` 与 `bc7enc-rdo` 两个 port。上游 port 用 `vcpkg_from_github` 拉取 GitHub 自动生成的源码归档并按 SHA512 固定；该归档被重新压缩后哈希不再匹配，port 直接下载失败。覆盖版改用 `vcpkg_from_git` 拉取 `v3.0.0` 对应的提交 `cfcadfa8d14eb489d97b6324838ae100410edcc7`，git 对象按内容寻址，不会像重新压缩的 tarball 那样漂移；除拉取方式外与上游 portfile 逐行一致。

overlay 会一直遮蔽上游同名 port：版本号仍是 `3.0.0`，所以刷新 baseline 后既不会切回上游，也拿不到 3.0.x 的后续修复。上游 port 记录的哈希与服务端一致后，删除 `cmake/vcpkg-overlay-ports/tinygltf/` 即可；overlay 目录清空后一并移除各 preset 的 `VCPKG_OVERLAY_PORTS`。

`bc7enc-rdo` 不是遮蔽：vcpkg 没有这个库的 port。它用 `vcpkg_from_git` 按提交 `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` 拉取 richgel999/bc7enc_rdo，只把 `bc7enc.cpp`、`rgbcx.cpp`、`bc7decomp.cpp` 编成静态库 `unofficial::bc7enc-rdo::bc7enc-rdo`（CMakeLists 随 port 提供），不构建上游的示例程序、ISPC 编码器与 RDO 编码器。

运行参数由 `EditorApplication::ParseArgs()` 提供：

```text
--backend vulkan    选择 Vulkan 后端（当前唯一实现）
--model <path>      启动后请求加载指定模型
--scene <path>      启动后加载指定场景文件，替换默认的双立方体测试场景
--frames <count>    渲染指定正整数帧后退出
--capture <file.png> 与 --frames 一起使用，退出前把最后一帧的视口（色调映射后的 LDR 图）保存为 PNG
```

代码改动的自动化验证入口如下；60 帧进程退出和 CTest 不能替代人工 GUI/视觉确认。本文档改动只进行静态核验，不把以下命令的历史或建议用法表述为本轮运行结果。

```powershell
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
.\out\build\vs2026-x64\app\Debug\miniengine_app.exe --backend vulkan --frames 60
```

## 7. 当前能力边界

- RHI 目前只有 Vulkan 后端，且后端仍承载 `EditorRenderBackendBase` 的编辑器流程。
- glTF 尚未完整处理额外 UV 集、sampler wrap 和 `KHR_texture_transform`。
- 启动默认场景配置与部分引用刷新仍以路径为主，未覆盖所有 UUID 解析路径；材质内的贴图引用是纯路径，没有 UUID 参与，重命名贴图会静默打断引用。
- CPU Renderable 支持按实体增量更新；Vulkan GPU 资源仍在内容变化时整批上传。
- 渲染端已有 `alphaMode` 分类（opaque / mask / blend × 单双面共 6 条管线变体）与半透明 back-to-front 排序；仍没有视锥剔除和抗锯齿；局部光阴影受图集容量限制（最多 64 块，点光和面光各占 6 块），面光按中心点投影、半影不随面积变宽；环境光是均匀环境（split-sum 近似），没有 IBL，显存按每 submesh 独立分配。缺口清单见 2026-07-30 的开发记录，其中管线相关两条已在 2026-09-03 处理，「缺失特性」中无独立 HDR 中间靶、色调映射硬编码在 `triangle.frag` 一条已在 2026-09-12 处理。不透明与 Mask 几何已改走 G-Buffer 延迟着色（第二阶段），Blend 仍走前向并合成在光照结果之上；GB4 实体 id 拾取（第三阶段）尚未实现。
- G-Buffer 带有相机与物体运动的 motion vector（`R16G16_SFLOAT`，当前 UV 减上一帧 UV；上一帧 model 矩阵经 set 0 binding 2 的 SSBO 按 `firstInstance` 索引），可在 Graphics Debug 窗口查看；Blend 表面与背景像素没有速度。目前还没有使用它的功能，TAA 与时域 AO 尚未实现。设计见 [docs/superpowers/specs/2026-09-19-motion-vectors-design.md](docs/superpowers/specs/2026-09-19-motion-vectors-design.md)。
- 材质纹理文件在设备支持时以块压缩格式上传（基础色与自发光 BC7 sRGB、法线 BC5、金属度/粗糙度/AO/混合遮罩 BC7 unorm），首次加载时在 CPU 上生成 mip 并压缩，结果缓存在 `CacheRoot()/textures`，文件路径、大小、修改时间或编码设置变化都会重新压缩；内置默认纹理仍是 RGBA8。NewSponza 的显存占用由约 6.2 GB 降到约 2.0 GB，首次加载压缩约 24 s，之后从缓存加载约 3 s。纹理的解码、压缩与缓存读取在后台工作线程上完成，期间编辑器照常渲染旧画面并在 Scene 面板显示进度，某次场景改动所需的纹理全部就绪后才一次性提交；超过 1 秒的帧会在日志中告警。设计见 [docs/superpowers/specs/2026-09-19-block-compressed-textures-design.md](docs/superpowers/specs/2026-09-19-block-compressed-textures-design.md)。
- `.hdr`（stb）与 `.exr`（tinyexr）按扩展名识别为浮点贴图：解码为线性 RGBA32F，在工作线程打包成 RGBA16F，以 `R16G16B16A16_SFLOAT` 上传并在 GPU 上生成 mip；不做块压缩、不进磁盘缓存，任何材质槽都按线性值采样（不做 sRGB 解码），自发光因此可以取大于 1 的值。软件预览经 `LoadRGBA8` 看到的是截断到 [0, 1] 并 sRGB 编码后的版本。设计见 [docs/superpowers/specs/2026-09-23-float-textures-design.md](docs/superpowers/specs/2026-09-23-float-textures-design.md)。
- 场景环境（Scene 面板 Environment：None / Atmosphere / HDRI，随场景 YAML 的 `environment` 节点保存；旧场景文件没有该节点时按 None 加载，新建的启动场景默认 Atmosphere 并带一盏 120000 lux 的太阳）。Atmosphere 是 Hillaire 2020：透射率 256×64 与多重散射 32×32 两张 LUT 只在大气参数变化时重建，Sky-View 192×108 与空气透视 32×32×32 体纹理每帧更新，全部为 `R16G16B16A16_SFLOAT`、常驻 `GENERAL` 布局、由 `VulkanAtmosphere` 自带屏障排序，经 set 0 binding 3–5 采样。太阳取投射阴影的平行光，其强度视为大气层顶照度，着色时乘以相机高度处沿太阳方向的透射率（CPU 端 `ComputeTransmittanceToSpace`，与 LUT 着色器积分同一介质）；天空在前向 pass 的不透明与半透明物体之间以远平面深度测试绘制，含太阳圆盘与临边昏暗（亮度截断到 65504）；延迟光照与 `triangle.frag` 叠加空气透视，距离缩放可在编辑器里放大以便在小场景里看到。HDRI 在工作线程用 `LoadRGBA32F` 解码，以 RGBA32F（设备不支持其线性过滤时退回 RGBA16F）单 mip 上传到 binding 6，图像中心朝 -Z（默认相机方向），强度单位为每单位纹素值的 cd/m²；加载完成前或失败时按 None 渲染。物理天空（Atmosphere、HDRI）参与自动曝光测光，只有 None 的平面背景被排除。RTX 4070 Laptop Release 实测：每帧 Sky-View 加空气透视约 0.035 ms，参数变化那一帧连同两张静态 LUT 约 0.10 ms。设计见 [docs/superpowers/specs/2026-09-23-sky-atmosphere-design.md](docs/superpowers/specs/2026-09-23-sky-atmosphere-design.md)。
- 漫反射 IBL：Atmosphere 与 HDRI 模式下，环境光项改为天空的 L2 球谐（SH9）辐照度——漫反射取法线方向 E(N)/π，镜面瓣在第四期预滤波之前暂用反射方向的 E(R)/π，两者再乘材质 AO 与 VBAO；场景里的 Ambient 灯仍叠加其上，兜底环境光不再使用；None 模式不变。大气的 SH 每帧在 `VulkanAtmosphere` 里由 `atmosphere_irradiance.comp` 投影（128×64 个等立体角方向，不含太阳圆盘，地平线以下补上被透射后的阳光照亮的地面），写入 set 0 binding 7 的存储缓冲；HDRI 的 SH 在解码线程里按每个纹素的立体角精确投影，每帧在 CPU 上按旋转解析地绕 Y 轴旋转、乘强度后放进相机 UBO。基与常数见 `engine/renderer/spherical_harmonics.h` 与 `shaders/vulkan/spherical_harmonics.glsl`（两边一致，单元测试验证正交归一、常量天空得 πL、旋转等于平移贴图后重新投影）。RTX 4070 Laptop Release：SH 投影每帧约 0.05 ms。尚无天空遮蔽，室内除 VBAO 外按整片天空受光。设计见 [docs/superpowers/specs/2026-09-24-diffuse-ibl-design.md](docs/superpowers/specs/2026-09-24-diffuse-ibl-design.md)。
- 镜面 IBL（split-sum，Karis 2013）：Atmosphere 与 HDRI 模式下，每帧由 `VulkanEnvironmentProbe` 把天空（不含太阳圆盘）以每纹素 2×2 采样捕获进 128² 的辐射度 cube，blit 出完整 mip 链，再用 64 个 GGX 重要性采样（按采样 pdf 选源 mip）预滤波出 6 级 mip 的第二个 cube（第 m 级对应粗糙度 m/5），经 set 0 binding 8 沿反射方向按粗糙度采样；权重来自 64×64 的 DFG 表（binding 9），由 `IntegrateEnvironmentBrdf` 在 CPU 启动时积分（512 个采样、Schlick-Smith k = α/2）并上传。漫反射仍是 SH；None 模式仍用原来的均匀环境光与 Karis 解析拟合。DFG 表的测试以精确性质为准（镜面即 Schlick 菲涅耳、能量守恒、采样收敛）：Karis 的移动端解析拟合在低粗糙度正对时约低估 0.14，只作宽松的同量级比对。RTX 4070 Laptop Release：捕获加预滤波每帧约 0.07 ms（大气模式环境相关计算合计约 0.15 ms）。设计见 [docs/superpowers/specs/2026-09-24-specular-ibl-design.md](docs/superpowers/specs/2026-09-24-specular-ibl-design.md)。
- 多次散射能量补偿（Fdez-Agüera 2019，Filament 的写法）：所有镜面项（四类直接光、天空/HDRI 与均匀环境光）乘以 `1 + F0 (1 / (A + B) − 1)`，A + B 取自镜面 IBL 的 DFG 表（均匀环境光用它自己的 Karis 拟合），漫反射按补偿后的镜面反照率扣能量。粗糙金属明显变亮，电介质在粗糙度 1 时最多约 +6%。直接光的几何项是 k = (r+1)²/8，DFG 表是 k = α/2，所以直接光的补偿是表中波瓣的近似。`miniengine.environment_brdf` 对表中每个纹素验证白炉（F0 = 1 补偿后正好反射 1）且任何 F0 都不超过 1。
- 分簇光照：非环境光进 set 0 binding 10 的存储缓冲（最多 1024 盏，方向光在前，仍按原排序规则截断并告警）。每帧 CPU 上的 `BuildLightClusters`（`engine/renderer/light_clusters.*`）把点光、聚光与面光按 `range` 包围球分进 16×9×24 的视锥体素网格（NDC 等分的 16×9 块，近远平面间 24 个指数深度切片），用块边界所在的过眼平面判定，结果是保守的：光照范围覆盖到的簇一定列出该灯。网格以“每簇 (offset, count) + 一条索引表”（容量 131072，满了丢弃并告警）上传到 binding 11；`ShadeSurface` 先遍历方向光，再只遍历像素所在簇的灯，簇由世界坐标经同一个渲染投影求出。Graphics Debug 有 “Clustered lighting” 开关（关掉即逐灯全遍历，用来对比）与 “Light clusters” 热力图（每簇局部灯数，蓝→红，32 盏以上为红）。`miniengine.light_clusters` 用 200 个随机相机验证保守性。Apple M3、Debug、667×541 的 Sponza 加 256 盏点光：分簇与全遍历的截图差异和同一构建两次运行之间的差异相同（最多 1 个 8 位灰阶，约 0.5% 像素）；整帧 45.0 ms 对 49.6 ms，只画热力图、不着色是 43.7 ms，所以局部光着色约 1.3 ms 对 5.9 ms，整帧主要耗在别处。设计见 [docs/superpowers/specs/2026-09-24-clustered-lighting-design.md](docs/superpowers/specs/2026-09-24-clustered-lighting-design.md)。
- 材质基础：材质参数不再走 push constant，而是每个 draw 一条 `GpuMaterialData`（80 字节：原来的四个 vec4 加一个着色模型 vec4），放在 set 0 binding 12 的存储缓冲里，下标是 `triangle.vert` 本来就拿到的 draw 槽位（`gl_InstanceIndex`，以 flat 变量传给片元着色器）。这个缓冲随每次内容重建由 `VulkanUniformBuffer` 写一次，之后不再改。Push constant 只剩 model 矩阵（64 字节，只有顶点阶段读）。GB2.a 存着色模型 ID（id / 255），目前只有 `DefaultLit = 0`，要到 clearcoat 才有第二种。导入时读取 `KHR_materials_emissive_strength` 写入 `emissiveIntensity`，负数或非数字忽略并告警，由 `miniengine.gltf_material_extensions` 覆盖。已导入的模型保留原有 `.material.yaml`，重新导入才生效。Sponza 截图在延迟与 forward-only 两个顺序下与改动前的差异都在同一构建两次运行的噪声之内。设计见 [docs/superpowers/specs/2026-09-24-material-foundation-design.md](docs/superpowers/specs/2026-09-24-material-foundation-design.md)。
- Clearcoat（`KHR_materials_clearcoat`，两个系数；coat 与 coat roughness 贴图见下方“各向异性与层贴图”，`clearcoatNormalTexture` 自第 2 阶段起支持）：`clearcoatFactor` 与 `clearcoatRoughnessFactor` 走现有材质路径：导入、`.material.yaml` 的 `pbr:`（`clearcoat_factor`、`clearcoat_roughness_factor`，旧文件缺这两个键时按 0）、材质面板的两个滑块，最后进入 `GpuMaterialData.clearcoatFactors`。系数大于 0 的材质用 `ShadingModel::Clearcoat`：geometry pass 把系数与粗糙度写进新的 G-buffer 目标 GB5（`R8G8B8A8_UNORM`，通道含义由像素的着色模型决定，set 2 binding 7）；`ShadeSurface` 按 Khronos glTF sample viewer 的做法，在基底上叠第二个 F0 = 0.04 的 GGX 瓣：直接光逐灯计算（投影光的阴影同样作用于 coat，面光源用同样的代表点近似），环境光沿 coat 反射方向取预滤波天空与 DFG 表，并乘 AO；基底和自发光再按 coat 的菲涅耳整体衰减。coat 法线用几何法线（没有 coat 法线贴图时规范就是这样），粗糙度下限 0.04。无 clearcoat 的场景截图与改动前的差异在噪声之内。Graphics Debug 的 “G-buffer: custom data (clearcoat)” 视图可直接看 GB5。编辑器的软件材质预览不画 coat。设计见 [docs/superpowers/specs/2026-09-24-clearcoat-design.md](docs/superpowers/specs/2026-09-24-clearcoat-design.md)。
- Sheen（`KHR_materials_sheen`，颜色与粗糙度两个系数；两张 sheen 贴图见下方“各向异性与层贴图”）：数据路径与 clearcoat 相同（`.material.yaml` 的 `sheen_color_factor`、`sheen_roughness_factor`，材质面板的颜色与滑块）。颜色不为黑的材质用 `ShadingModel::Sheen`，GB5 的 rgb 存 sheen 颜色、a 存粗糙度。着色用 Filament 的布料模型：Charlie 分布加 Neubelt 可见性，作用在着色法线上，逐灯计算（面光源朝矩形中心近似）；环境光取沿反射方向、按 sheen 粗糙度预滤波的 GGX 天空，乘 sheen 颜色、sheen 反照率和 AO；基底（直接光与环境光）乘 `1 − max(sheenColor) · 反照率`，自发光不受影响。Khronos sample viewer 另用一张 Charlie 预滤波的 cube，这里没有做。Sheen 反照率在启动时由 `IntegrateSheenAlbedo` 在 CPU 上积分，存进 DFG 表空着的蓝色通道；该模型不守恒能量，光滑 sheen 在掠射角会积出大于 1 的值（粗糙度 0.1、N·V 0.05 时为 1.74，已由独立的暴力积分验证），表里截断到 1，保证基底缩放不为负。GB5 只能放一层，所以同时有 clearcoat 与 sheen 的材质只渲染 clearcoat（导入时告警，材质面板有提示）。无 sheen 的场景截图与改动前的差异在噪声之内。设计见 [docs/superpowers/specs/2026-09-24-sheen-design.md](docs/superpowers/specs/2026-09-24-sheen-design.md)。
- TAA（时域抗锯齿，Graphics Debug 的 “Temporal anti-aliasing”，默认开）：投影每帧按 Halton(2, 3) 的 8 个亚像素偏移抖动，只抖动 GPU 光栅化用的矩阵（相机块的 `proj`/`invViewProj` 和分簇光照的分箱）；编辑器矩阵与 `MotionHistory` 仍用未抖动的投影，相机块新增 `viewProjNoJitter` 供 `triangle.vert` 算 motion vector，所以静止相机的速度为零。Forward 之后新增计算 pass `Taa`（`taa_resolve.comp`）：取 3×3 邻域里最近深度像素的速度（背景按相机运动重投影），5 次采样的 Catmull-Rom 读历史，在 YCoCg 里做方差裁剪（均值 ± 1σ），按 `1 / (1 + 曝光后亮度)` 加权 10% 当前、90% 历史。输出写进新的存储目标 `SceneTaa`，曝光直方图与色调映射改读它。历史图由 pass 自己持有（`HistoryImagePair`，与 VBAO 共用新提出的 `compute_pass_util`），由重命名后的 `TemporalHistory` 管理。关掉 TAA 或在 forward-only 顺序下不抖动，pass 原样拷贝，截图与改动前的差异在噪声之内。静止画面里拱门、柱子的锯齿边变平滑；相机每帧转 0.3° 时没有拖影，但纹理细节在运动中会变软（没有做锐化）。比一个像素还窄的高光（coat 粗糙度 0.05 的太阳反射，约 0.1 像素半径）8 个抖动采样也采不到，TAA 不解决，需要 specular AA。Blend 表面没有 motion vector，相机运动时可能拖影。设计见 [docs/superpowers/specs/2026-09-24-taa-design.md](docs/superpowers/specs/2026-09-24-taa-design.md)。
- Specular AA（几何镜面抗锯齿，Tokuyoshi & Kaplanyan 2019，参数取 Filament 的：方差 0.15、阈值 0.2；Graphics Debug 的 “Specular anti-aliasing”，默认开）：`gbuffer.frag` 与 `triangle.frag` 用法线的屏幕空间导数估计像素内法线的变化，加到 α² 上再换回感知粗糙度。基底用带法线贴图的着色法线，写进 GB2 之前就过滤；clearcoat 用几何法线。平坦表面粗糙度完全不变。纯函数 `FilterRoughnessForSpecularAA`（`engine/renderer/specular_aa.*`，`miniengine.specular_aa` 覆盖）与 `specular_aa.glsl` 同一算式，开关和常数经相机块末尾的 vec4 传入。补上了 TAA 解决不了的亚像素高光：coat 粗糙度 0.05 的小球，太阳的 coat 高光原本多数漏采，开启后每个球都能看到（不论 TAA 开关）。关掉时截图与改动前的差异在噪声之内。设计见 [docs/superpowers/specs/2026-09-24-specular-aa-design.md](docs/superpowers/specs/2026-09-24-specular-aa-design.md)。
- 眩光（Bloom 的 mip 链，Jimenez 2014；按 GT7 的做法由曝光决定，见 [docs/references/gt7-rendering-notes.md](docs/references/gt7-rendering-notes.md) 第 3 节；Graphics Debug 的 “Glare (bloom)” 开关与强度倍数，默认开、1.0 即物理值；Camera 面板显示当前光圈）：在 TAA 之后、两种顺序里都有的计算 pass `Bloom`，从半分辨率起建最多 6 级 mip（`BuildBloomMipChain`），第一级 13 点降采样用 Karis 平均防止单点闪烁。光圈由曝光推出：ISO 100、快门 1/125 s 时 `N = sqrt(2^EV / 125)`，限制在 f/1.4–f/22（`GlareFNumberFromEv100`），EV 15.6 约 f/20，EV 5 以下 f/1.4。Fraunhofer 衍射的 Airy 图样在第一圈之外、焦平面半径 ρ 以外的能量约为 2λN/(π²ρ)；以 24 mm 高的全画幅传感器换算像素间距，半径 R 像素以外的能量就是 K/R。第 k 级 mip（纹素宽 2^(k+1) 像素）承担 2^(k+1) 到 2^(k+2) 像素这一环的能量，最后一级承担其外全部，2 像素以内的能量留在原处（`ComputeGlareBands`，按 R 612、G 549、B 465 nm 分通道，所以红色光晕略宽）。升采样时每级乘自己那一环的能量再加上更低一级，合成时 `场景 × (1 − 总量) + 第 0 级`，能量守恒。效果：日光 f/20 下每个像素移走约 2.5% 的能量，室内 f/1.4 只有 0.18%，所以 Sponza 这种 EV 5 的场景几乎看不到眩光，而日光下 2e7 cd/m² 的自发光球有一圈收得很紧、带暖色外缘的光晕（边缘 +106 级灰度，20–30 像素处 +19，90 像素外为 0；原来固定混合 4% 时 90 像素外仍有 +10）。开启 HDR 输出时按 GT7 的做法把显示峰值超出 SDR 250 nit 的部分当作曝光余量，光圈按 `EV − log2(峰值 / 250)` 计算，1000 nit 时眩光减半。关掉时截图与改动前的差异在噪声之内。设计见 [docs/superpowers/specs/2026-09-24-exposure-glare-design.md](docs/superpowers/specs/2026-09-24-exposure-glare-design.md)。
- 预曝光 HDR 缓冲（采用 GT7 的帧缓冲约定，见 [docs/references/gt7-rendering-notes.md](docs/references/gt7-rendering-notes.md)）：HDR 目标与 GB3 自发光不再存物理亮度，而是存「物理亮度 × 本帧预曝光」，单位是 GT7 帧缓冲单位（1.0 = 显示上的 100 cd/m²）。预曝光 `PreExposureFromEv100(EV) = ExposureFromEv100(EV) × 2.5`，在 `UpdateAutoExposure` 之后于 CPU 上算出，经相机块新增的 `exposure`（x 为预曝光，y 为其倒数）交给所有着色器；换算常量在 `shaders/vulkan/pre_exposure.glsl`，由测试与 `exposure.h` 对齐。着色仍在 fp32 的物理单位里进行，只有延迟光照、前向、天空和 G-buffer 在写出时乘一次；延迟光照读 GB3 时先除回物理单位。直方图除以预曝光后按物理亮度计量；色调映射直接使用缓冲里的值（`TonemapFrameBufferRec709`）；TAA 与 Bloom 的 Karis 权重乘 0.4，数值与原来的「曝光后亮度」一致；TAA 历史按「本帧 / 写入历史那帧」的预曝光比缩放（`TaaHistoryScale`），曝光变化时不闪、不拖影。视口纯色背景改成常量 `kViewportBackgroundFrameBuffer`，不再随 EV 变化，EV 上限也不再受 fp16 约束。效果：EV 15.6 下 2e5 与 2e7 cd/m² 的自发光球，以前 bloom 光晕完全相同（都被截在 65504），现在 2e7 的光晕在球边缘亮 166 级灰度，120 像素外仍有 +3；EV 5→7 跳变后 10 帧，与稳定 EV 7 的截图只差 2.6% 像素，不缩放历史则差 67%。代价：存储尺度变了，fp16 的舍入落点随之改变，Sponza 无 TAA 时约 5% 像素差 1 级灰度（最大 1）；有 TAA 时历史逐帧重新舍入，约 13% 像素差 1 级。设计见 [docs/superpowers/specs/2026-09-24-pre-exposed-hdr-design.md](docs/superpowers/specs/2026-09-24-pre-exposed-hdr-design.md)。
- 两级自动曝光（GT7 的做法，见 [docs/references/gt7-rendering-notes.md](docs/references/gt7-rendering-notes.md) 第 4 节；Camera 面板显示长期适应的 EV 与短期范围）：长期一级模拟感受器灵敏度，按帧缓冲直方图、太阳（地面上的直射照度照在 18% 灰卡上）和天空（Atmosphere 模式为 GPU 算出的天空 SH，经每帧槽一份的回读缓冲拿到 CPU；HDRI 为其 SH；None 为环境光；都取 L0 平均亮度）三个参考的加权平均（0.5 / 0.25 / 0.25，缺哪个就按剩下的重新归一）缓慢适应，变亮 0.05/s、变暗 0.02/s。短期一级沿用原来的直方图目标和速度，但只能离长期一级 ±2.5 EV（`StepAutoExposure`），所以白天把镜头压向阴暗处时画面保持偏暗，而不是被拉到中灰。首次计量时两级一起定下来；前 3 秒、以及每次场景内容换新（比如后台加载的场景替换掉启动场景）之后的 3 秒，长期一级按短期速度追赶，免得记住上一个世界的光。效果：Sponza 与日光自发光球的曝光与改动前相同（EV 5.14、15.68）；日光下看向不受光的地面，单级方案会提亮到 EV 1.69，两级方案停在 EV 3.86，暗 2.2 档。设计见 [docs/superpowers/specs/2026-09-25-two-stage-auto-exposure-design.md](docs/superpowers/specs/2026-09-25-two-stage-auto-exposure-design.md)。
- 自动白平衡（GT7 的做法，见 [docs/references/gt7-rendering-notes.md](docs/references/gt7-rendering-notes.md) 第 4.4 节；Camera 面板的 “Auto White Balance”，默认开，显示适应到的色温）：光源参考为太阳（到达地面的照度与颜色）和天空（平均亮度 × π），按亮度加权求色度，再加一盏占 5% 外加 1 lx 的虚拟 D65 光让暗场景也稳定；然后与画面本身的平均色度（直方图 pass 顺带累加每个像素的 rgb/亮度）以 0.25 : 0.75 混合，画面占大头，免得镜头里几乎看不到的太阳把整个室内染色。估计值的相关色温（McCamy）限制在 2800–10000 K（超出就移到对应温度的普朗克轨迹上），白点按 0.5/s 指数适应，最后在色调映射之前以 0.6 的适应程度做 Bradford 色适应到 D65（`WhiteBalanceMatrix`），所以日落仍然偏暖，只是没那么橙。效果：日光大气场景与关闭时相差不到 2 级灰度；Sponza 偏蓝的环境光被中和（蓝通道平均 −5.9 级）；把太阳改成 3000 K 的 Sponza 只是略微变中性（红 −4.9、蓝 −2.6 级）。调参过程：只用光源参考时，大气模式缺少天空导致日光偏蓝，3000 K 太阳的 Sponza 整体变成青蓝色；加上天空回读和画面参考、适应程度从 0.8 降到 0.6 后才正常。设计见 [docs/superpowers/specs/2026-09-25-auto-white-balance-design.md](docs/superpowers/specs/2026-09-25-auto-white-balance-design.md)。
- HDR 显示输出（GT7 的 `initializeAsHDR`，见 [docs/references/gt7-rendering-notes.md](docs/references/gt7-rendering-notes.md) 第 2.3 节；Graphics Debug 的 Output 区 “HDR output” 与 “Display peak (nits)”，默认关、1000 nit）：实例在可用时启用 `VK_EXT_swapchain_colorspace`，打开后交换链改用 `A2B10G10R10` + `HDR10_ST2084`（本机 MoltenVK 提供，macOS 经 EDR 显示），没有就退回 SDR 并告警一次；切换会重建交换链和依赖其格式的一切。场景的 LDR 目标改成 RGBA16F，存以 UI 白（`kUiWhiteNits = 203`，ITU-R BT.2408 的图形白）为 1.0 的显示线性值；色调映射用 GT7 按显示峰值重建的同一条曲线（输出帧缓冲单位，1.0 = 100 nit），再换算到 UI 白单位，所以能超过 1。ImGui 改用 `imgui_hdr10.frag`（后端加了可选的自定义片元着色器），把 UI 与场景图像统一转到 Rec.2020、乘 203 nit、做 PQ 编码，所以 UI 亮度与 SDR 时相同，场景高光可以更亮。`--capture` 遇到浮点 LDR 目标时在 UI 白处截断并做 sRGB 编码，仍输出 SDR PNG。显示峰值无法经 Vulkan 查询，所以做成设置项。HDR 曲线与参考实现的 `initializeAsHDR` 在 250–10000 nit 五个峰值下逐位一致，250 nit 时正好是 SDR 曲线乘 2.5。设计见 [docs/superpowers/specs/2026-09-25-hdr-output-design.md](docs/superpowers/specs/2026-09-25-hdr-output-design.md)。
- 屏幕空间反射与镜面遮蔽（按 GT7 光追镜面 GI 的结构，见 [docs/references/gt7-rendering-notes.md](docs/references/gt7-rendering-notes.md) 第 5 节；Graphics Debug 的 “Screen-space reflections”，默认开，最大粗糙度 0.8、最大距离 30 m；Viewport output 新增 “Screen-space reflections” 视图）：保留 split-sum，只替换预滤波环境那一项。延迟顺序在 AO 之后、光照之前新增两个计算 pass：`SsrTrace` 每像素一条光线，方向由 GGX 可见法线分布采样（Heitz 2018），每帧换一次交错梯度噪声，在屏幕空间按 1/z 透视正确地步进 48 步再二分 6 次，命中点经 `prevViewProj` 重投影到上一帧，从 TAA 的历史图像（上一帧抗锯齿后、bloom 之前）取颜色并换算到本帧预曝光；可信度 = 屏幕边缘淡出 × 粗糙度淡出（0.4 到最大粗糙度）× 光线末端淡出。`SsrResolve` 做随粗糙度放大的 5×5 深度 / 法线感知空间滤波，再按表面运动向量重投影做时间累积，历史先裁剪到当前 3×3 邻域，写入新的 `SceneReflections`（RGBA16F，经 G-buffer 集合的 binding 8 给光照 pass）。着色时镜面项的入射辐亮度 = `mix(环境 × 镜面遮蔽, 屏幕反射, 可信度)`，镜面遮蔽用 Lagarde 2014 的公式替代原来“镜面也乘漫反射 AO”；前向路径没有屏幕反射，只有镜面遮蔽。需要 TAA 历史：TAA 关闭或前向对比顺序下不追踪。效果：光滑地面（粗糙度 0.1）上清楚地映出球体；Sponza 的石材粗糙度多在 0.6–0.8，反射模糊而暗，主要表现为地面对环境光的局部遮蔽，画面干净无噪点。设计见 [docs/superpowers/specs/2026-09-25-ssr-design.md](docs/superpowers/specs/2026-09-25-ssr-design.md)。
- 局部光阴影（Graphics Debug 的 “Local light shadows”，默认开；灯光面板的 “Cast Shadows”，随场景 YAML 存为 `cast_shadows`，旧场景缺该键时按开）：点光、聚光与面光共用一张 4096² 深度图集（D32，64 MiB），切成 64 块 512²。聚光占 1 块（视场为外锥角两倍），点光与面光各占 6 块立方体面（面光从中心投影），外锥角超过 60° 的聚光也按立方体处理。每块视锥外扩 2 个 texel 作保护带，着色器再把查找夹在块内，3×3 双线性 PCF 读不到邻块，立方体各面之间无缝。每帧由纯函数 `PlanLocalShadows`（`engine/renderer/local_shadows.*`，`miniengine.local_shadows` 覆盖）按灯光选择的顺序给视野内、开启投影的局部灯分块，放不下的灯不投影并在日志中报告数量；灯的首块序号写在 `areaRightAxis.w`（序号 + 1，0 表示无阴影），块数据在 set 0 binding 14，图集在 binding 13。投影物按块视锥与包围球剔除，深度偏移与法线偏移沿用级联阴影的做法（法线偏移随到灯距离放大）。聚簇与暴力两条局部光循环都乘阴影，前向路径同样生效。代价：Apple M3、Debug 构建、667×541、NewSponza 带两盏立方体灯和一盏聚光（13 块）时，整帧约 22 ms，关闭局部光阴影约 12 ms；每帧重画全部图集，尚无静态缓存。设计见 [docs/superpowers/specs/2026-09-25-local-light-shadows-design.md](docs/superpowers/specs/2026-09-25-local-light-shadows-design.md)。
- 各向异性与层贴图（`KHR_materials_anisotropy`，以及 clearcoat / sheen 此前导入时只告警的贴图）：导入 `anisotropyStrength`、`anisotropyRotation`（弧度，编辑器里按角度显示）与 `anisotropyTexture`（RG 为切线空间方向，B 乘强度），`.material.yaml` 存为 `anisotropy_strength`、`anisotropy_rotation` 与五个 `*_texture_path`，材质面板列出五张层贴图并有两个各向异性滑块。材质集合（set 1）从 13 个采样器增至 18 个：clearcoat（R）、clearcoat roughness（G）、sheen color（RGB，sRGB）、sheen roughness（A）、anisotropy（RGB），各自乘对应系数，缺省时绑白图（各向异性绑 (1, 0.5, 1)，即沿切线、满强度），只作用于主层。`GpuMaterialData` 增至 8 个 vec4（128 字节）。着色：强度大于 0 时基底 GGX 沿各向异性方向拉伸（`alpha_t = mix(alpha, 1, s²)`，各向异性 GGX D 与 Heitz 2014 高度相关可见性，与 Khronos sample viewer 相同，但去掉了它把可见性夹到 1 的做法），面光在代表点上同样拉伸，环境光沿弯曲法线取预滤波天空；强度为 0 的像素走原来的各向同性路径。G-buffer：着色模型 id 的 bit 2 表示各向异性基底，GB5.ba 存方向角（在仅由着色法线构造的 Duff 2017 正交基中，[0, π) 量化为 8 位，约 0.7°）与强度，可与清漆共存（清漆占 .rg）；光泽占满 GB5，因此光泽材质的各向异性被忽略并告警（第 2 阶段起不再有此限制，见下）。`clearcoatNormalTexture` 仍不支持（GB5 没有空间，第 2 阶段已支持）。SSR 按各向同性追踪，强各向异性表面上的屏幕反射比环境反射更锐。验收：NewSponza 与改动前的差异在两次运行的噪声之内（3.5–3.7% 对 3.9% 的通道）；测试球上高光随强度横向拉长，旋转 90° 与“向上”的各向异性贴图结果一致，条纹清漆贴图、棋盘光泽颜色贴图在延迟与前向路径下一致。设计见 [docs/superpowers/specs/2026-09-25-anisotropy-and-layer-textures-design.md](docs/superpowers/specs/2026-09-25-anisotropy-and-layer-textures-design.md)。
- BRDF 精度（完整 BRDF 计划第 1a 阶段，总计划见 [docs/superpowers/specs/2026-09-25-complete-brdf-program.md](docs/superpowers/specs/2026-09-25-complete-brdf-program.md)）：直射光、清漆、面光与 DFG 表统一使用高度相关 Smith 可见性（Heitz 2014），DFG 表增至每纹素 2048 次采样，均匀环境光与清漆环境光改查 DFG 表而不再用 Karis 拟合；漫反射改为 Burley（Disney，Frostbite 能量归一化形式），面光按矩形中心方向求值；环境镜面反射在几何地平线以下渐隐（地平线遮蔽，屏幕空间反射不受影响）；方向光按环境里的太阳角直径当作圆盘，点光与聚光新增光源半径（灯光面板 “Source Radius (m)”，场景 YAML 存为 `source_radius`，缺省 0 即点光），镜面高光取光源上离反射射线最近的代表点并按 (α/α')² 归一化。修正了一个长期存在的问题：`DistributionGGX` 把 π·d² 下限夹在 1e-4，粗糙度低于约 0.3 的高光峰值都被削弱，粗糙度 0.04 时削弱达五个数量级，光滑黑漆球上的太阳高光因此只剩一团灰雾。面光改用线性变换余弦（LTC，Heitz 2016，第 1b 阶段）：拟合表由 `tools/ltc_fit` 针对本引擎的 GGX + 高度相关 Smith 波瓣自行拟合（64×64，入库为 `engine/renderer/ltc_table.cpp`，set 0 binding 15、16），漫反射积分按地平线裁剪后的余弦（精确辐照度），基底与清漆高光积分拟合波瓣，各向异性基底仍用代表点；与真实波瓣对比平均误差 7.7%，掠射角小灯最大 49%（方法已知的弱点）。设计见 [docs/superpowers/specs/2026-09-25-brdf-accuracy-design.md](docs/superpowers/specs/2026-09-25-brdf-accuracy-design.md)。
- G-buffer 分层、IOR 与 specular（完整 BRDF 计划第 2 阶段）：几何 pass 写满 8 个颜色附件（Apple GPU 上 MoltenVK 的上限，少于 8 个的设备在选择时跳过）：GB5 存电介质 √F0 与 F90，新增 GB6（清漆系数、清漆粗糙度、各向异性角度与强度）和 GB7（光泽颜色与粗糙度），速度目标改为 RGBA16F，`.ba` 存清漆自己的法线；GB0.a 留给第 5 阶段的次表面散射。GB2.a 从“着色模型”改为标志位（1 清漆、2 光泽、4 各向异性、8 自定义 specular、16 清漆法线），同一像素可以任意组合，光照 pass 只读标志位指明的目标，普通像素的计算不变。新增 `KHR_materials_ior`（`ior`，0 表示无穷大）与 `KHR_materials_specular`（`specularFactor`、`specularColorFactor` 及两张贴图）：F0 = min(IOR 的 F0 × 颜色, 1) × specular，F90 = specular，贯穿直射、面光（LTC 权重）与环境光；清漆法线贴图 `clearcoatNormalTexture` 及其 scale。材质面板新增 IOR、Specular、Specular Color、Clearcoat Normal Scale，`.material.yaml` 新增对应键。Graphics Debug 的视图改为 “specular (sqrt F0)”，并新增 “coat and anisotropy” 与 “sheen”。设计见 [docs/superpowers/specs/2026-09-25-gbuffer-layers-ior-specular-design.md](docs/superpowers/specs/2026-09-25-gbuffer-layers-ior-specular-design.md)。
- 前向着色材质与彩虹色薄膜（完整 BRDF 计划第 3 阶段）：G-buffer 放不下的稀有特性让材质带上标志位 32（前向着色）。这类不透明/Mask 物体仍由几何 pass 写入深度、运动矢量、法线与粗糙度（TAA、AO、阴影照常），光照 pass 跳过这些像素，前向 pass 在延迟物体之后、天空与 Blend 之前以 LESS_OR_EQUAL 深度测试在同一深度上着色；绘制顺序为延迟、前向着色、Blend（`BuildMaterialDrawOrder`）。代价是这些表面没有屏幕空间反射。第一个此类特性是 `KHR_materials_iridescence`（Belcour & Barla 2017）：薄膜折射率、厚度范围与两张贴图，薄膜菲涅耳在 N·V 处每像素求值一次，按权重替换直射、LTC 面光、环境光与漫反射权重中的 Schlick 菲涅耳；零厚度时精确退回基底菲涅耳，并逐通道夹到 1。材质面板新增 Iridescence、Iridescence IOR 与薄膜厚度范围。设计见 [docs/superpowers/specs/2026-09-25-forward-materials-iridescence-design.md](docs/superpowers/specs/2026-09-25-forward-materials-iridescence-design.md)。
- 贴图变换、第二套 UV 与无光照材质：`KHR_texture_transform`（平移、旋转、缩放及其 texCoord）与每个 textureInfo 的 `texCoord` 按材质贴图槽导入（金属度与粗糙度共用 metallicRoughness 贴图的变换），存入 `.material.yaml` 的 `texture_transforms`；每个 draw 的变换放在 set 0 binding 17，只有带变换的材质才读取，普通材质的采样不变；旋转过的法线贴图、清漆法线与各向异性方向贴图会按旋转角转回网格的切线空间。顶点新增 `texCoord1`（`TEXCOORD_1`，属性 5）。阴影 pass 的 alpha 测试也按底色贴图的变换采样，push constant 因此增至 144 字节（设备选择时要求）。`KHR_materials_unlit`：底色按显示纸白亮度直接写入 HDR，与曝光无关（经过色调曲线后接近原色，不完全相等），仍投射阴影；材质面板新增 Unlit。设计见 [docs/superpowers/specs/2026-09-26-texture-transform-uv-sets-unlit-design.md](docs/superpowers/specs/2026-09-26-texture-transform-uv-sets-unlit-design.md)。
- 材质变体（`KHR_materials_variants`，完整 BRDF 计划第 3b 阶段）：导入根节点的变体名与每个图元的 mappings（越界的材质或变体索引跳过并告警，同一变体重复映射取第一个）；变体选择放在实体上（`ModelComponent::materialVariant`，按名字，场景里存为 `material_variant`，旧场景没有此键即默认材质），同一模型的两个实体可穿不同变体；场景面板对有变体的模型显示 Material Variant 下拉框。变体材质就是模型材质列表里的普通材质，材质面板照常编辑。设计见 [docs/superpowers/specs/2026-09-26-material-variants-design.md](docs/superpowers/specs/2026-09-26-material-variants-design.md)。
- Khronos 参考对比：Graphics Debug 的 Khronos reference view（命令行 `--khronos-reference`）按 Khronos glTF Sample Viewer 的默认设置渲染——Khronos PBR Neutral 色调映射、HDRI 纹素 1 曝光为 1、关闭自动白平衡、辉光、AO 与 SSR、背景用粗糙度 0.6 的预滤波环境图，并按 viewer 的规则取景（每个图元的 accessor 包围盒经节点变换后扩成外接立方体再求并）。`tools/khronos_reference/` 下载 glTF-Sample-Assets 测试模型、生成场景、批量截图，并把 viewer 截图与引擎截图并排、出差异图和平均差（用法见其 README）。2026-09-26 的首轮结果（26 个场景，平均差 0–255）：底色、金属度、粗糙度、法线、clearcoat、sheen、specular、iridescence、各向异性强度/旋转、emissive strength 与鞋的四个变体均在 2.7–4.6，即噪声底（TextureTransformTest 在补上 glTF sampler 后由 5.6 降到 2.7；CompareAnisotropy 由 6.1 降到 3.8、CompareNormal 由 3.4 降到 2.6，见下条切线修正）；已知差异为 CompareIor / IORTestGrid（12–15，需透射，第 4 阶段）、UnlitTest（5.8，viewer 不对 unlit 做色调映射，引擎有意经过色调曲线）。设计见 [docs/superpowers/specs/2026-09-26-khronos-reference-comparison-design.md](docs/superpowers/specs/2026-09-26-khronos-reference-comparison-design.md)。
- glTF sampler：每个材质贴图槽导入其贴图的 sampler（wrapS/wrapT 的 repeat、clamp_to_edge、mirrored_repeat，放大与缩小过滤，含无 mipmap 的 NEAREST/LINEAR），金属度与粗糙度共用；未指定的过滤沿用引擎默认（线性、线性 mipmap、16x 各向异性，即以前所有贴图的 sampler）。非默认设置存入 `.material.yaml` 的 `texture_samplers`。渲染器用 `VulkanSamplerCache` 按设置去重 `VkSampler`，材质描述符把贴图视图和其槽位的 sampler 配对（同一贴图文件可被不同材质以不同方式采样）；最近邻过滤关闭各向异性，无 mipmap 时只采样第 0 级。阴影 alpha 测试走同一套描述符。设计见 [docs/superpowers/specs/2026-09-26-gltf-samplers-design.md](docs/superpowers/specs/2026-09-26-gltf-samplers-design.md)。
- 生成切线的方向修正（2026-09-26，由 Khronos 对比发现）：glTF 切线空间的 +Y 指向贴图图像的上方，即 v 减小的方向；没有 TANGENT 属性的网格此前生成的 bitangent 是 dP/dv（图像向下），法线贴图和各向异性方向的 Y 因此反了（CompareNormal 的编织纹和标志显示为凹陷，CompareAnisotropy 旋压面的明暗扇区方向不对）。现在取 -dP/dv，与 MikkTSpace 在导出器 V 朝上的 UV 上算出的结果以及 Sample Viewer 一致，镜像 UV 仍得到正确的 w。自带切线的模型（Sponza 与全部验收场景）不受影响。测试见 `tests/tangent_generation_tests.cpp`。
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

### 2026-09-19 — G-Buffer 延迟着色第二阶段：几何 pass、延迟光照与前向对比开关

G-Buffer 延迟着色三阶段的第二阶段，连同 2026-09-18 的光照修正与方向光级联阴影一起作为 0.1.2 提交。不透明与 Mask 几何改走延迟路径，不引入新视觉效果；设计见 [docs/superpowers/specs/2026-09-10-gbuffer-deferred-design.md](docs/superpowers/specs/2026-09-10-gbuffer-deferred-design.md)，与设计的出入已在该文档内以 "Amended after phase two" 记录。

**帧结构**

- 延迟顺序为 Geometry → Lighting → Forward（仅 Blend）→ ExposureHistogram → Tonemap，对比顺序为 Forward（全部）→ ExposureHistogram → Tonemap，由纯函数 `BuildScenePassOrder` 给出。两条顺序共用同一个直方图 pass 与色调映射 pass，对比只隔离着色差异。方向光阴影 pass 不在列表里，仍在两条顺序之前录制。
- pass 所有权收进 `std::vector<std::unique_ptr<IScenePass>>`，按 `ScenePassId` 查找；自动曝光通过一个非拥有指针读直方图。交换链重建时 pass 与两套材质管线整体重建，管线缓存使其廉价，重建后的直方图从空开始，曝光保持两帧；视口 resize 只让每个 pass 跟随新图像。第一阶段记下的"新增 pass 改五处"与 stale framebuffer 窗口随之收掉。
- 前向 pass 持有 clear 与 load 两个 render pass：延迟顺序下必须保留光照结果与几何 pass 的深度。二者只差 `loadOp`，render pass 兼容性不看它，因此共用 framebuffer 与管线。

**G-Buffer 与光照**

- GB0 `R8G8B8A8_SRGB` 反照率，GB1 `R16G16B16A16_SFLOAT`：`.rg` 着色法线、`.ba` 几何法线，均为八面体编码（直接存 [-1, 1]，不做 0.5 映射），GB2 `R8G8B8A8_UNORM` 金属度/粗糙度/AO，GB3 `B10G11R11_UFLOAT_PACK32` 自发光（回退 `R16G16B16A16_SFLOAT`）。几何 pass 的深度 `storeOp` 为 STORE。
- GB1 比设计多出两个通道：前向路径的阴影法线偏移沿插值后、已按背面翻转的几何法线，延迟路径必须用同一条法线，否则法线贴图强的地方阴影边界会移动。每像素每份多 4 字节，1600x900、两份约 11.5 MB。
- 世界坐标由深度与相机 UBO 末尾（阴影块之后）新增的 `invViewProj` 重建，不存储；`invViewProj` 取 `renderProjection`，取错会把位置镜像。深度 1.0 的像素输出 `GetBackgroundRadiance(exposure)`（`kViewportBackgroundExposed / exposure`），由 push constant 传入，与前向清屏走同一个函数。
- 光照 pass 与材质 pass 绑定同一个 set 0，阴影图（binding 1）不需要额外绑定；阴影 pass 的外部依赖覆盖其后所有片元着色器读取。

**着色器**

- 新增三个 include：`pbr_common.glsl`（阴影图声明、BRDF、面光源、阴影查询、均匀环境光与 `ShadeSurface`）、`gbuffer_common.glsl`、`gbuffer_inputs.glsl`；此前已有的 `scene_common.glsl` 加了 include guard。每条着色器编译命令依赖全部 include。
- `gbuffer.frag` 是 `triangle.frag` 的材质半段，含双面背面的 TBN 翻转；`deferred_lighting.frag` 与 `triangle.frag` 调用同一个 `ShadeSurface`，两条路径的光照与阴影算术只有一份。

**调试视图与开关**

相机面板新增 "Forward only (comparison)" 复选框与 G-Buffer 视图下拉框（反照率、着色法线、几何法线、金属度/粗糙度/AO、自发光），设置为 `render_types.h` 的 `RenderDebugSettings`，不持久化。色调映射的 push constant 由曝光扩成曝光加视图编号，着色视图仍是 GT7；自发光视图同样经过曝光与 GT7。对比顺序不写 G-Buffer，渲染器在该顺序下强制关闭调试视图，而不依赖 UI 的禁用状态。

**成像**

按设计像素等价，精度边界如下：反照率经 8 位 sRGB、金属度/粗糙度/AO 经 8 位 unorm、两条法线经 fp16 量化后才参与着色，世界坐标由深度重建，阴影边缘可能有 texel 级差异。光照 pass 对粗糙度重新夹到 [0.04, 1]。

**与计划的出入**

执行中发现并修复了一个计划没有覆盖的同步问题：`RenderTargetLayoutTracker` 只在布局变化时产生屏障，而第二阶段有两处同一目标被连续两个 pass 写入（深度：几何 pass → 前向 pass；HDR：光照 pass → 前向混合 pass），布局不变便没有任何屏障。现在写后写会产生 `oldLayout == newLayout` 的纯内存屏障，读后读仍不产生屏障；原先把"写后写不产生屏障"固定下来的两个测试随之更新。单独提交为 `fix(vulkan): order back-to-back writes of the same render target`，并已记入设计文档的修订。

**验证**

x64 Debug 从全新 configure 构建通过，CTest `37/37`，`check-format` 通过，`git diff --check` 无输出。Debug 开验证层 `--frames 60`（默认场景，走延迟顺序）与带太阳场景 `--frames 300` 均退出码 0，无验证层输出。同步验证仍未开启。

每个任务都用脚本截取了引擎窗口，与改动前的前向截图逐像素比对（比对区域为视口，去掉左上角文字）：
- 任务 1–6 期间着色画面不变：测试场景逐像素相同，缩放窗口触发两次交换链重建后仍相同；NewSponza 只有自动曝光时点带来的 ±1 级噪声，同一构建两次运行之间也会出现。
- 延迟顺序对前向：测试场景 3.4% 的像素差 1 级（8 位目标量化），NewSponza 16% 的像素差 1–2 级、单个像素差 3 级。差值放大 80 倍后是均匀噪声，亮处略强，阴影边缘、轮廓、级联边界和法线贴图细节处没有结构性差异。
- 对比开关：用临时环境变量强制前向顺序（未提交），测试场景与前向基线逐像素相同，NewSponza 只有 ±1 级噪声。
- 调试视图：同样用临时开关在 NewSponza 上截取五个视图，反照率、着色法线、几何法线、金属度/粗糙度/AO、自发光均与计划的预期一致。
- Release、RTX 4070 Laptop、NewSponza、1920×936：延迟顺序有阴影 156.4 FPS、无太阳 451.7 FPS；阴影提交时的前向顺序为 153.5 / 427.8 FPS。

以上截图检查由脚本完成。Task 7 Step 12 的九项人工 GUI 检查（在面板里实际切换开关、拖动视口、载入场景、删除后撤销等）尚未由人确认，第二阶段的视觉验收仍待用户完成。Blend 材质的叠加没有单独验证：测试场景没有 Blend 材质，NewSponza 的差异图里没有局部异常，但不能据此断定其中有 Blend 材质被画到。

**仍未处理**

GB4 实体 id、单像素读回与混合拾取属于第三阶段。八灯上限、IBL、抗锯齿不变；只有主方向光有阴影。同步验证仍未开启。

### 2026-09-18 — 光照修正与方向光级联阴影

来自对光照系统的一次审查。分两次提交：先修光照公式和灯光选择，再加阴影。

**光照修正**

- 点光与聚光的距离衰减由 `window / (d² + 1)` 改为 `window / max(d², 1 cm²)`。原式在 1 m 处只有物理值的一半、0.3 m 处约十分之一，与以流明为单位的强度不符。
- 聚光把流明摊到外锥立体角 `2π(1 − cos outer)`，此前用的是内锥，内外角差越大，实际发出的光通量越多于标称值。
- 环境光按亮度均匀的环境处理，用 Karis 的 split-sum 解析拟合（`EnvironmentBrdfApprox`）：金属不再有 albedo 色的漫反射环境光，所有表面补上了原本缺失的环境镜面项与菲涅尔。对均匀环境而言，除拟合误差外 split-sum 是精确的。
- Ambient 灯在 CPU 上求和（`SelectSceneLights`），不再占着色器的 8 个灯位；兜底环境光只在场景没有任何 Ambient 灯时生效，放一盏强度为 0 的 Ambient 灯即可得到全黑环境。
- 超过 8 盏时不再按 ECS 迭代顺序截断：方向光按强度优先，其余按在相机处的照度 `I / max(d², 1)` 排序，并列保持场景顺序；被丢弃数量变化时打一条警告。纯逻辑在 `engine/renderer/scene_lighting.*`，由 `miniengine.scene_lighting` 覆盖。
- 相机 uniform 块的声明集中到 `shaders/vulkan/scene_common.glsl`。
- 新增启动参数 `--scene <path>`，以异步方式加载场景，替换双立方体测试场景。

**方向光级联阴影**

- 投影光源：选中灯里最亮的方向光（强度大于 0）。每帧由 `BuildShadowCascades`（`engine/renderer/shadow_cascades.*`，纯函数，`miniengine.shadow_cascades` 覆盖）按 λ = 0.8 的对数/均匀混合切 4 级，阴影距离取 80 m 与相机远平面中较小者。每级拟合到视锥切片的包围球（半径量化到 1/16 m），光空间原点对齐到整 texel，相机平移、转向时阴影边缘不爬动。近平面沿光线方向后拉 200 m，视野外的遮挡物也能投影。
- `VulkanShadowPass`（`engine/renderer/vulkan/shadow_pass.*`）持有一张 4 层的 2D array 深度图（优先 D32，退回 D16），设备生命周期，所有在途帧共用一张。它不是 `IScenePass`，也不经过 `RenderTargetLayoutTracker`：每层一个 render pass，从 UNDEFINED 清屏，结束于 SHADER_READ_ONLY；与上一帧的读取、本帧材质 pass 的读取之间的顺序由 render pass 的两条外部依赖保证。没有投影光时仍逐层清屏，因为材质 pass 总会绑定这张图，清屏保证它处于描述符声明的布局。
- 投影物：Opaque 只画深度、无片元着色器；Mask 跑与 `triangle.frag` 相同的双层 alpha 测试；Blend 不投影。不做背面剔除（单面墙从背后也要挡光），用光栅化 slope bias（常数 1、斜率 2）加着色器里 1.5 texel 的法线偏移防止阴影痤疮。每级按包围球剔除投影物；为此子网格新增包围球半径（`ComputeMeshBounds`，与中心一样在加载线程算好缓存）。
- 采样：set 0 binding 1 的 `sampler2DArrayShadow`，比较采样器 LESS_OR_EQUAL，线性过滤可用时 3×3 次采样构成 4×4 texel 平滑滤波；每级最后 10% 深度范围与下一级混合，最后一级淡出到无阴影。查询在非统一控制流里，用零梯度的 `textureGrad`。

**已知限制**

- 只有一盏方向光投影；点光、聚光、面光没有阴影。没有逐灯的 Cast Shadows 开关。
- 开销：Release、RTX 4070 Laptop、NewSponza、1920×936 视口下，有阴影 153.5 FPS，把太阳换成点光 427.8 FPS，阴影约 4.2 ms。NewSponza 的子网格多是大块合并网格，包围球剔除基本剔不掉，每级几乎重画整个场景。可能的后续：更细粒度剔除、远级隔帧更新、减少级数。
- 仅有兜底环境光的场景里，阴影区接近纯黑。这是没有天空光的正常结果，不是阴影本身的问题。
- `docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md` 原按修正前的着色器写成，已于 2026-09-18 按 `4b50e17` 修订（见计划开头的 Revision 一节）：`ShadeSurface` 移入 `pbr_common.glsl` 并带上新的衰减、聚光立体角、split-sum 环境光与阴影采样；GB1 扩为 `R16G16B16A16_SFLOAT`，`.ba` 存几何法线，供延迟路径做与前向相同的阴影法线偏移；光照 pass 经已绑定的 set 0 读取阴影图。

**验证**

x64 Debug 与 Release 构建通过，CTest `37/37`（新增 `miniengine.scene_lighting`、`miniengine.shadow_cascades`），`check-format` 通过。Debug 开验证层运行默认场景 200 帧、含太阳的测试场景 300 帧，均退出码 0，无验证层输出。同步验证仍未开启，跨帧共用阴影图的正确性依赖对 render pass 外部依赖的推理，没有经过同步验证层检查。截取了引擎窗口：测试场景里阴影方向与太阳方向一致，与投影物底部相接，地面无痤疮；NewSponza 中拱廊在地面和柱子上投下的阴影形状正确。截图由脚本拍摄，未经人工 GUI 验收。

### 2026-09-18 — 曝光与自动曝光、GT7 色调映射、双面背面法线、前向深度保存、矩形面光源与版本号

来自一次管线基础审查。LDR 靶跟随交换链格式（UNORM 回退时缺 sRGB 编码）、每资源独立 `vkAllocateMemory` 两条记录在案，未处理；深度回退到带模板格式时采样视图含两个 aspect 一条随自动曝光一并修复（见下）。

**GT7 色调映射**

- Reinhard 换成 Polyphony Digital 公开的 GT7 算子（MIT，2025-08-10 版 1.0），只用 SDR 和默认的 ICtCp。
- `shaders/vulkan/gt7_tonemap.glsl` 用 GLSL 与 C++/GLM 的公共子集编写，同一份源码由 glslc 编进 `tonemap.frag`，也在 `miniengine_tonemap_tests` 里以 C++ 编译。测试与 `tests/third_party/gt7/` 下原样保留的参考实现对拍：10 万组随机输入最大差 0（逐位一致）。这两个文件的写法约束见文件头注释。
- 引擎渲染的是线性 Rec.709，而参考实现的 ICtCp 系数要求线性 Rec.2020，所以进出算子各做一次 BT.2087 原色转换。
- 曝光后的 1.0（传感器饱和点）映射到 GT 的 SDR 纸白 250 cd/m²，即帧缓冲值 2.5。这样中灰的显示值与原 Reinhard 相差不到 0.005，高光则真正收敛到白色。
- 编辑器背景色对应的曝光后值改为对新算子数值求解，放在 `exposure.h` 的 `kViewportBackgroundExposed`，由测试核对仍映射到 {0.08, 0.1, 0.16}。
- 着色器新增 `#include` 支持：`MINIENGINE_SHADER_INCLUDES` 里的文件变化时，全部着色器重新编译。

**自动曝光**

- 新的 compute pass `VulkanExposureHistogramPass` 排在前向 pass 与色调映射之间，把 HDR 像素按 log2 亮度计入 256 个 bin（-12 到 +18 档）。深度为 1.0 的背景像素不计：背景清屏值除以了曝光，计入会形成反馈。直方图写入每帧槽一份的主机可见 buffer。
- CPU 在 `AcquireNextImage` 等完该帧槽的 fence 后读回（结果晚两帧），GPU 从不等待 CPU。
- 测光取 60%–95% 分位的平均 log 亮度，按反射式测光（K = 12.5）换成 EV100，加补偿、限定范围，再按指数方式向目标靠近：变亮 3/s，变暗 1.5/s，与帧率无关。第一次测到结果时直接跳到该值。
- 分箱规则 `exposure_histogram.glsl` 同样由着色器与 `engine/renderer/exposure.cpp` 共用。测光、换算、平滑都是纯函数，由 `miniengine.exposure` 覆盖。
- Camera 面板默认开启 Auto Exposure，可调补偿、EV 范围和两个适应速度；关闭后回到手动 EV 滑块。设置不存盘。
- 为此：布局追踪器的采样读取阶段加上 compute；图形队列族要求同时支持 compute；带模板的深度靶另建只含 depth aspect 的采样视图（`SceneRenderTargets::GetSampledView`）；`RendererSharedState` 新增 `frameDeltaSeconds`。

**矩形面光源**

- 此前面光源在着色器里就是中心处的点光源：`lm / (π · 面积)` 算出亮度后按点光源的 1/d² 衰减，没有乘回面积。所以面积越小越亮，照明形状与点光源无从区分，且向所有方向发光。
- 现在是单面朗伯矩形，几何与视口里已有的 `DrawLightAreaGizmo` 一致：矩形在本地 XY 平面内，宽沿 X、高沿 Y，沿 gizmo 法线箭头方向（本地 -Z）发光，背面不发光。尺寸为 `areaSize` 乘以变换的 X/Y 缩放，与 gizmo 画出的矩形大小一致。方向光和聚光灯仍沿本地 -Y。漫反射用多边形的余弦加权立体角闭式解（Lambert，每条边一项），接收面地平线以下的部分不裁剪，只把负值钳到 0。高光取反射射线落在矩形上的最近点作代表点，GGX 按矩形等面积圆盘的张角乘 `(α/α')²` 归一化。
- 矩形缩小时收敛到沿法线发出 `lm / π` 坎德拉、余弦衰减的点光源。闭式解经 Python 数值核对：小矩形与点近似一致，无限大矩形趋于 π，离轴随机点与 40 万样本蒙特卡洛积分相差 0.02%。
- 半径衰减只保留窗口项（`RangeWindow`），距离衰减由积分本身给出。
- `GpuLightData` 由 4 个 vec4 增为 5 个，新增 `areaRightAxis`；`triangle.vert`、`triangle.frag` 的 `SceneLightData` 同步修改。第一版误把矩形放在本地 XZ 平面、朝 -Y 发光，与已有 gizmo 的朝向垂直，另加的选中轮廓也画成了 XZ；已按 gizmo 的约定改正，并删掉了那个多余的轮廓。

**版本号**

根 `CMakeLists.txt` 的 `project(MiniEngine VERSION ...)` 是唯一来源，只编译进 `engine/core/version/engine_version.cpp`，改版本只重编这一个文件。窗口标题显示为 `MiniEngine v<版本>`，`SDL_SetAppMetadata` 和 `VkApplicationInfo` 的应用与引擎版本也从这里读取（此前分别硬编码为 `0.1.0` 与 `1.0.0`）。本轮改动作为 0.1.1 提交。

**曝光**

- 灯光强度是物理单位（lx、lm），此前辐亮度直接进 Reinhard，默认 1000 lx 方向光照在白色漫反射面上约 318 cd/m²，色调映射后贴近纯白。现在 `Camera::exposureEv100`（默认 8，范围 -2 至 18，相机面板可调）经 `ExposureFromEv100`（ISO 100 饱和度法，`1 / (1.2 * 2^EV100)`）换成乘数，以 push constant 交给 `tonemap.frag`，在 Reinhard 之前乘上。
- EV 不进场景文件，每次启动回到默认值。默认值是按默认太阳选的：同样 1000 lm 的点光源在几米外会暗得多，这是物理上正确的结果，室内场景把 EV 降到 3 左右。
- 环境光统一为亮度单位 cd/m²：Ambient 类型灯光的强度标签由倍数改为 `cd/m^2`，UBO 兜底环境光按默认 EV 的曝光倒数放大，所以没有灯光的场景在默认 EV 下与改动前一致，只差浮点舍入。
- 前向 pass 的清屏值除以曝光，编辑器背景色不随 EV 变化。最高 EV 18 下清屏值约 6.0e4，仍在 fp16 的 65504 以内；上调 `kMaxExposureEv100` 前要先处理这一点。

**双面材质背面**

`triangle.frag` 按 `gl_FrontFacing` 整体翻转切线空间（法线、切线、副切线），与 glTF Sample Viewer 做法一致。副切线必须单独取反：`cross(-N, -T) == cross(N, T)`。单面管线剔除背面，这段代码对它们不起作用。

**前向深度**

`SceneDepth` 的 `storeOp` 由 `DONT_CARE` 改为 `STORE`，后续 pass 采样深度时不会读到未定义内容。

**与 phase two 计划的冲突**

`docs/superpowers/plans/2026-09-13-gbuffer-phase2-deferred-shading.md` 写于本轮之前，照原文执行会回退或破坏上述改动：几何 pass 着色器沿用未翻转的 TBN；`tonemap.frag` 自带一个 `uint32_t` push constant，需要与曝光的 `float` 合并排布；延迟光照在深度 1.0 处输出的背景预除值和前向清屏片段都没有除以曝光；计划中的 `scene_common.glsl` 与 `ShadeSurface` 仍是 4 个 vec4 的 `SceneLightData`，也没有新的面光源函数。计划里的 `tonemap.frag` 仍是 Reinhard，没有 GT7 的 include，场景 pass 列表里也没有直方图 pass。以上各点已在 2026-09-18 的计划修订中处理（见计划开头的 Revision 一节）。

**验证**

x64 Debug 构建通过，CTest `34/34`，`check-format` 通过（新文件另用 clang-format 22 单独核对），`--frames 60` 退出码 0 且开验证层无错误输出；启动时加载 NewSponza（406 个 submesh）同样无验证层错误，从进程读到的窗口标题为 `MiniEngine v0.1.1`。面光源路径只有着色器编译和管线创建经过验证层，没有在含面光源的场景中运行过。自动曝光加入后 CTest `35/35`。验证期间临时加过日志：默认场景约 1.55 万个非背景像素，收敛到 EV 6.53；Sponza 加载后收敛到 5.26。全程无验证层错误，唯一一次例外是窗口被最小化时，交换链以 0×0 重建报错。该问题是既有问题：`HasDrawableArea` 看的是 SDL 窗口尺寸，交换链用的是 surface 的 `currentExtent`，与本轮改动无关，未修。临时日志已删除。新增 `miniengine_exposure_tests`（`miniengine.exposure`），覆盖 EV 换算、每档减半、默认 EV 让默认太阳下的白面不到显示白，以及最高 EV 下清屏值不溢出 fp16；自动曝光的分箱、测光、补偿与平滑也在这个目标里。用户已完成 GUI 验收。

### 2026-09-12 — 资产生命周期加固

同一轮审查里的六条缺陷，分三组，组内耦合、组间独立。

**注册表安全性**

- 资产浏览器预览面板此前**每帧**调 `GetOrCreateUuid`：全局注册表互斥锁 + 一次 `is_regular_file` + 可能写 sidecar。而后台导入线程在同一把锁上跑整棵资产树的 `RescanAssetTree`，UI 线程被它按住。UUID 在文件生命周期内不变，现在只在焦点移动或条目列表重建时算一次。
- 孤儿 sidecar 的判定此前是 `exists()` 返回 false，而它在"无法判断"时同样返回 false——一次瞬时 IO 错误就永久丢掉 UUID。现在要求 `error_code` 干净才算证据。
- `WriteSidecar` 此前截断后直接流式写，中途崩溃留下空文件，读回来就是"没有 UUID"。改成写临时文件 + `rename` 覆盖。临时文件名以 `.tmp` 结尾，不匹配 sidecar 后缀，因此不会被扫描误认。

**缓存生命周期**

- `ModelCache::Get` 此前返回可变 `shared_ptr`，两处调用方直接改缓存里的材质。今天两处都在主线程，没有真的 data race，但接口没这么说，而后台线程随时可以 `Store` 覆盖同一个键。`Get` 改为返回 `shared_ptr<const LoadedModelData>`，新增 `UpdateMaterial`/`UpdateMaterials` 两个显式写入口，在缓存锁内应用，路径缺失或索引越界时是 no-op——正是调用方原先自己带的那两个守卫。
- 缓存此前无上限无淘汰，Sponza 级别的一个包就是几百 MB，进去就常驻到进程退出。现在按字节计量 + LRU 淘汰，预算 1 GiB。**场景仍引用的条目永不淘汰**，无论预算：淘汰它只会在下次重建 renderable 时触发一次同步重新加载，那是体验倒退不是节省。淘汰在 live set 真正可能变化的两个点评估——`RebuildSceneRenderables` 末尾，以及报告了变化的 `RefreshDirtySceneRenderables` 末尾——而不是每帧空跑。
- 预览贴图缓存（解码后的全分辨率 RGBA8，一张 4K 基色图解码就是 32 MB）用同样的计数器 LRU，预算 256 MB，但没有 live set——预览面板对任何一张贴图都没有持久占有。

**引用完整性**

- 点一次 Delete 此前是主线程同步把资产树下每个 ≤64 MB 的 `.gltf`/`.yaml` 全文读进内存，对最多 256 个文件名逐个子串搜索。这对删除已经慢，对重命名（F2 高频操作）根本不可行。
- 扫描提取成 `engine/asset/asset_references.{h,cpp}` 的 `FindReferencesTo`，背后是按 `last_write_time` 索引的文档内容缓存：首次仍走全树，重复扫描只重读变化的文档。时间戳读不到的文档会重读而不是信任索引。
- 重命名由此能用上同一个检查：有引用就弹和删除同款的确认框。材质里的贴图引用是纯路径（见第 7 节），重命名必然打断它们——此前是静默打断。
- 重命名还会 `ModelCache::Invalidate`，此前只有删除路径做了。否则之后在老路径上重新导入的模型会被喂上一个文件的解析数据。

设计见 [docs/superpowers/specs/2026-09-12-asset-lifecycle-hardening-design.md](docs/superpowers/specs/2026-09-12-asset-lifecycle-hardening-design.md)。给材质贴图引用加 UUID 能让重命名经注册表自动修复、彻底不需要扫描，但那要改 `.material.yaml` 格式、`ModelMaterialData` 和解析路径，比其余五项加起来还大，仍是已知缺口。

### 2026-09-12 — 嵌入式贴图改为导入时解包

嵌入图片此前在**加载时**导出到 `.cache/tinygltf/<stem>_<模型绝对路径的 FNV1a>/`。缓存键是路径，所以每次导入、复制或重命名都新建一份全尺寸副本且从不清理——工作区里曾经是同一个模型的五份 101 MB 目录，共 501 MB。导出返回的还是绝对路径，而外部 URI 返回的是模型相对路径：同一个 `ModelMaterialData` 字段承载两种语义，`.material.yaml`（在 `assets/` 下，可提交）于是会持久化指向 gitignored 派生数据的本机路径。

- 解包移到 `ModelLoader::CopyModelWithSortedReferences` 末尾，`.glb` 与 `.gltf` 两条分支合流后统一调 `GltfModelLoader::UnpackEmbeddedTextures`，写进 `<bundle>/textures/`。命名由 `BuildEmbeddedTextureFileName` 一处决定，导入和加载从同一个输入推出同一个名字。
- `ResolveImagePath` 改为推导相对路径并确认存在，`LoadModel` 因此不再写任何文件。解包之前导入的旧包会退化成无贴图材质加一条明确的 warning，而不是指向不存在的路径。
- `.glb` 仍原样保留，是源文件的忠实副本，贴图在磁盘上有两份（包内嵌一份、解包一份）。这是一次性有界成本，不随路径变化增长；把 `.glb` 转写成 `.gltf` 能省掉那一份，但导入产物就不再是副本，且要冒 tinygltf 写回丢 extension、漂精度的风险。
- 解包只在导入时发生，所以 `--model` 指向 `assets/` 外的模型会先自动导入。守卫放在 `editor_backend_base.cpp` 的 `pendingModelLoads` 出口——所有加载请求的唯一收口，一处覆盖 `--model`、UI 加载和批量加载。由此确立不变式：能渲染的模型一定在 `assets/` 下。
- `AssetRegistry` 补上首批测试：UUID 铸造与持久化、边界拒绝、重复 UUID 仲裁（**两种扫描顺序都断言**，因为扫描顺序决定归属正是 sidecar 文件名仲裁要防的事）、孤儿 sidecar 清理、引用解析三级回退、重命名与删除。

`EnginePaths::CacheRoot()` 保留（启动日志仍在报告它），只是 `tinygltf/` 子目录和那套键推导没有了。设计见 [docs/superpowers/specs/2026-09-12-embedded-texture-unpacking-design.md](docs/superpowers/specs/2026-09-12-embedded-texture-unpacking-design.md)。

### 2026-09-12 — Vulkan 帧结构重组：HDR 中间靶、显式 barrier 与独立色调映射 pass

G-Buffer 延迟着色三阶段的第一阶段，只重组帧结构，不引入延迟着色路径。改动集中在 `engine/renderer/vulkan/` 与 `shaders/vulkan/`；设计见 [docs/superpowers/specs/2026-09-10-gbuffer-deferred-design.md](docs/superpowers/specs/2026-09-10-gbuffer-deferred-design.md)。

**HDR 中间靶与色调映射**

- 场景改为渲染到 `R16G16B16A16_SFLOAT` 的 HDR 靶，`triangle.frag` 只输出线性辐亮度。Reinhard 从 uber shader 移出，落到独立的 `tonemap.frag` 与 `VulkanTonemapPass`：全屏三角形由 `fullscreen.vert` 按 `gl_VertexIndex` 生成，无顶点缓冲也无顶点输入状态。算子表达式逐字未改，成像等价的依据就在此。
- 背景清屏值改为预除后的 `(0.086957, 0.111111, 0.190476)`。清屏现在落进 HDR 靶、和其他像素一起被色调映射，而此前它绕过片元着色器直达显示；`c / (1 - c)` 是 Reinhard 的逆，所以成像后仍是原来的 `(0.08, 0.1, 0.16)`。改动清屏值的人必须同时改这个反函数，否则背景色会漂。
- `tonemap.frag` 在算子前用 `min(..., vec3(65504.0))` 夹到 fp16 上限。辐亮度现在是裸存的，`+inf` 会让 `inf / (inf + 1)` 算出 NaN，把极亮像素变成黑而不是白。

**显式 barrier 纪律**

- `SceneRenderTargets` 持有全部离屏图像、视图、内存与 ImGui 纹理绑定；render pass 和 framebuffer 归使用它们的 pass。`VulkanSceneViewport` 删除，职责按这条线拆开。
- 帧内所有布局变更都是显式 `vkCmdPipelineBarrier`，由 `RenderTargetLayoutTracker` 从各 pass 的 `Io()` 声明推出；每个 render pass 的附件都声明 `initialLayout == finalLayout`。原 `VulkanSceneViewport` 的两条 `VK_SUBPASS_EXTERNAL` 依赖随之删除——显式 barrier 已经承担那份次序，留着只是对不再变化的布局重复一遍。这条约定已记入第 5 节，不只存在于已完成阶段的计划里。
- tracker 按帧作用域构造，不是可选的实现细节：它每个靶只存一个布局，而瞬时靶每个副本各有一张图像，跨帧留下的布局描述的是另一张 `VkImage`。`Reset()` 因此在每个命令缓冲开头执行。这不花成本——acquire 已等过该帧槽的 fence，且每个靶在被读取前都已清屏或整幅重写。反过来说，内容需要跨帧存活的靶不能照用这个 tracker。
- 两套索引并存：瞬时靶（深度、HDR）按帧槽索引，各 `kMaxFramesInFlight`（2）份；LDR 靶被 ImGui 采样，而纹理绑定在命令缓冲录制之前就交了出去，因此按交换链图像索引，一图一份。两者都经 `SceneRenderTargets::ResolveIndex` 取用，规则只存在一处；访问器只用 `.at()` 拦越界下标，索引搞混但恰好在范围内是拦不住的。

**描述符集拆分**

原本 14 个绑定的单一布局按更新频率拆成 set 0（逐帧相机 UBO）与 set 1（13 个材质采样器）。相机数据由此每交换链图像写一次，而不再是每图像每材质写一次；材质重载只重建 set 1。色调映射 pass 不绑定相机集——它的 set 0 是自己的 HDR 采样器。

**成像**

Opaque 与 Mask 像素逐位相同：这两档完全覆盖，`triangle.frag` 的算术未动，只是 Reinhard 挪到了后一个 pass。Blend 像素不同：混合现在发生在色调映射之前的线性辐亮度空间，此前是在已被 Reinhard 压过的值上混合，因此半透明像素偏亮。这是本阶段的预期结果而非回归，但本轮未做视觉验收。

**验证**

x64 Debug 构建通过，CTest `33/33`，`check-format` 通过，`--frames 60` 退出码 0 且无验证层输出。新增 `miniengine_scene_pass_tests`（`miniengine.scene_pass`），覆盖 `RenderTargetLayoutTracker` 与 `ChooseFormat` 两个不调用任何 Vulkan 入口的纯逻辑单元；这是本仓库第一个渲染器单元测试目标。未做人工 GUI 验收，上述成像结论来自代码与算子等价性，不是截图比对。

**仍未处理**

五张 G-Buffer 靶、几何与延迟光照 pass、前向对比开关和 GB4 实体 id 拾取都属于第二、三阶段。同步验证（synchronization validation）未开启，是一条独立的待决项。pass 列表目前是 `std::vector<IScenePass*>`，新增一个 pass 要改 `VulkanRenderer` 的五处；改成持有 `std::unique_ptr` 的列表加一个 `ForEachPass` 可以收掉其中四处，记在第一阶段计划的退出准则里作为第二阶段前置项。

### 2026-09-03 — Vulkan 图形管线：动态状态、共享布局与资源寿命分层

承接 2026-07-30 缺口清单中「性能与架构」的前两条。改动集中在 `engine/renderer/vulkan/`、`engine/renderer/material_pipeline.*` 与 `engine/renderer/material.h`，不改变渲染输出。

**管线创建**

- `VulkanPipeline` 类删除。原本 6 个变体各自读一次 SPIR-V、各建一对 shader module、各建一个内容完全相同的 `VkPipelineLayout`；现在 `VulkanPipelineSet` 统一持有一对 module 和一个 layout，6 条管线由单次 `vkCreateGraphicsPipelines` 批量创建。`pipeline.h/cpp` 保留为 `VulkanShaderModule` 这个 RAII 包装。
- 各变体独有的状态放进一个 `PipelineVariantState` 数组就地填充。这些结构体内部互相取址（`pSpecializationInfo`、`pAttachments`），数组一旦被拷贝或移动就会悬空，不要换成可能重新分配的容器。
- viewport/scissor 改为 `VK_DYNAMIC_STATE_VIEWPORT/SCISSOR`，在 `RecordSceneLayer` 每个 pass 设一次。
- `VkPipelineCache` 由 `VulkanRenderer` 持有，与逻辑设备同寿命，跨管线重建复用。

**资源寿命分层**

`VulkanRenderer` 的资源按寿命分成三层，方法名同步改为 `Create/DestroyDeviceResources`、`Create/DestroySwapchainResources`、`Create/DestroyDescriptorResources` 加 `EnsureGraphicsPipelines`：

- 设备级：`VulkanMaterialDescriptorSetLayout` 与 `VkPipelineCache`。描述符集布局由 shader 写死（1 个 UBO + 13 个 combined image sampler），与场景内容和交换链都无关，因此从 `VulkanUniformBuffer` 中提出。这是「内容变化不再重建管线」的关键：管线绑定的是这个常驻布局对象，重建描述符集不会使其失效——是结构上的保证，不是「两个布局定义相同所以兼容」的推理。
- render pass 级：`VulkanPipelineSet`。`VulkanSceneViewport` 的 render pass 只依赖附件格式，因此拆出 `ReleaseFrames()` / `BuildFrames()`；交换链重建时只换逐帧图像、framebuffer 和 ImGui 纹理绑定，render pass 与 sampler 存活，管线不动。只有交换链颜色格式真的变化才整体重建视口并 `m_graphicsPipelines.reset()`。
- 交换链/内容级：`VulkanUniformBuffer` 与视口逐帧资源。
- 顺序约束：`ReleaseFrames()` 会调用 `ImGui_ImplVulkan_RemoveTexture`，必须早于 `m_imguiLayer->DestroyVulkanResources()`，否则释放的是已销毁描述符池里的集合。

**绘制顺序**

`BuildMaterialDrawOrder` 在把 Blend 分区到尾部之后，对非 Blend 前缀按 `GetMaterialPipelineIndex` 再做一次 `stable_sort`。Opaque 与 Mask 都做深度测试且写深度，相对顺序不影响成像，分组可消除交错 submesh 造成的 `vkCmdBindPipeline` 抖动；Blend 段仍严格 back-to-front，正确性优先于批处理。

**交换链 render pass 去掉深度附件**

ImGui 后端的 `depth_info` 是零初始化的，从不做深度测试，这个 pass 的深度附件属于死重量：一张全屏 D32 加每帧一次 clear。删除后顺带消除了「一张深度图被所有 framebuffer 共用、两帧并发同时 clear」的 WAW 隐患；`srcAccessMask = 0` 也不再是问题，交换链图像的可见性本来就由 acquire 信号量保证。

**材质常量清理**

- 删除 `MaterialPipelineState::writeAttachmentAlpha`。它实际是 no-op：视口清屏 alpha 为 1.0，Opaque/Mask 屏蔽 alpha 写入，Blend 的 `dstAlpha = ONE_MINUS_SRC_ALPHA` 算出来恒等于 `srcA + (1 - srcA) × 1 = 1`。现在所有变体统一只写 RGB。`srcAlphaBlendFactor` 等字段不能一并删除——`blendEnable` 为真时 Vulkan 要求它们是合法枚举值。这条 alpha 约束本身必须保持：ImGui 采样视口图像并合成到编辑器上，附件 alpha 一旦小于 1，编辑器背景就会透过 3D 视图。
- `alphaCutoff` 从 `emissiveFactor.a` 提升为独立命名字段。`ObjectPushConstants` 已经卡在 128 字节的 Vulkan 下限，加不了第 5 个 vec4；做法是 C++ 侧拆成 `float emissiveFactor[3]` + `float alphaCutoff`，GLSL 侧拆成 `vec3 emissiveFactor; float alphaCutoff;`——vec3 后跟 float 的打包方式与 vec4 完全一致，GPU 字节布局逐字节不变。已用 `spirv-dis` 核对两个 stage 的成员偏移（`emissiveFactor` 80、`alphaCutoff` 92），并加 `offsetof` 静态断言锁住。写第一版断言时误把 `ObjectPushConstants` 内的偏移当成结构体内偏移，被编译直接挡下：结构体内正确值是 28 与 32。

**验证**

x64 Debug 构建通过，CTest `32/32`，`check-format` 通过。`tests/material_alpha_tests.cpp` 的 draw order 断言按新的分组语义更新，并新增一个 7 个 draw 的交错用例，除断言完整顺序外还直接计数 `vkCmdBindPipeline` 从 7 次降到 6 次；`writeAttachmentAlpha` 的三条断言随字段一并删除。开验证层启动编辑器并程序化多次 resize 窗口，全程无 `[error]` 日志，截图确认视口正常出图。日志可直接佐证寿命分层生效：一整个会话（含 1 次内容重载、5 次交换链重建、3 次视口缩放）只出现一次 `Created 6 material pipeline variants`，改动前同样操作会建 8 次管线组。未做人工 GUI 验收；带真实 MASK/BLEND 材质的视觉比对也未进行——`alphaCutoff` 改动的依据是 SPIR-V 偏移与 `offsetof` 断言证明字节布局未变，不是视觉确认。

**仍未处理**

视锥剔除、显存 suballocator、材质去重与 bindless、灯光排序告警、导入 alpha 平方、负缩放镜像绕向，以及阴影 / IBL / MSAA，均按 2026-07-30 的建议顺序保留。

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
