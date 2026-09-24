# MiniEngine

MiniEngine 是一个 C++20 编写的 3D 场景编辑器原型，基于 SDL3、Vulkan、Dear ImGui 与 EnTT。编辑器工作流已形成闭环：导入 glTF 模型、摆放实体与灯光、编辑材质，并把场景保存为 YAML。脚本、物理、动画、音频与 Play 模式尚未实现。

依赖：SDL3、Vulkan、Dear ImGui、ImGuizmo、EnTT、yaml-cpp、tinygltf、GLM、spdlog、stb、shaderc（版本见 [vcpkg.json](vcpkg.json)）。

## 功能

**编辑器**

- 停靠式界面：场景面板、资产浏览器、模型处理与预览、材质图、主题、相机与输入监视。
- 视口选择，移动/旋转/缩放 gizmo，拖放放置，键鼠与手柄相机。
- 模型与灯光统一为场景实体；场景格式为 YAML v3，可兼容加载旧的 v1/v2。
- 资产的复制、粘贴、重命名、删除；模型和纹理通过 sidecar 分配 UUID，删除或重命名前会检查引用。
- 模型与场景异步加载，资产在后台导入。

**资产**

- 导入 glTF 2.0（`.gltf` / `.glb`），导入时解包嵌入贴图，并做三角化、法线/切线生成与单位换算。
- 每个材质保存为 `.material.yaml` sidecar，可在 PBR 材质图中编辑。
- 贴图首次加载时压缩为 BC7/BC5 并缓存；`.hdr` / `.exr` 按浮点贴图上传。

**渲染**（Vulkan）

- 不透明物体走 G-Buffer 延迟着色，半透明物体走前向渲染；Cook-Torrance PBR 带多次散射能量补偿，支持 Clearcoat 与 Sheen。
- 五类灯光（方向、点、聚光、矩形面光、环境），最多 1024 盏，局部光按分簇查找；最亮的方向光投射 4 级 CSM 阴影。
- 场景环境可选 Hillaire 大气或 HDRI，提供 SH9 漫反射 IBL 与 split-sum 镜面 IBL。
- VBAO 环境光遮蔽，屏幕空间反射与镜面遮蔽。
- TAA、几何 Specular AA、motion vector。
- 仿 GT7 的成像链：预曝光 HDR、两级自动曝光、自动白平衡、由曝光决定的眩光、GT7 色调映射，以及 HDR10 显示输出。

各功能的实现细节见 [docs/RENDERING.md](docs/RENDERING.md)。

## 构建与运行

需要 C++20 工具链、CMake、Vulkan SDK 和 vcpkg。首次配置前先引导依赖：

```bash
./scripts/bootstrap-deps.sh          # Windows：.\scripts\bootstrap-deps.ps1
```

| 平台 | 配置 / 构建 preset |
| --- | --- |
| Windows (VS 2026) | `vs2026-x64` → `vs2026-x64-debug` / `-release`（x86 同理） |
| Windows (Ninja) | `x64-debug`、`x64-release`、`x86-debug`、`x86-release` |
| macOS | `macos-debug` / `macos-release`（ARM64），`macos-x64-debug` / `macos-x64-release`（Intel） |
| Linux | `linux-debug`（x64），`linux-arm64-debug` |

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug --parallel
ctest --test-dir .\out\build\vs2026-x64 -C Debug --output-on-failure
```

Bash 版 `./scripts/build.sh [preset]` 不传 preset 时会按主机自动选择。Windows 也可以直接打开根目录的 [MiniEngine.slnx](MiniEngine.slnx)。依赖只能安装到 `.deps/vcpkg_installed/<arch>/`。详细规则见 [docs/BUILD.md](docs/BUILD.md)，脚本说明见 [scripts/README.md](scripts/README.md)。

命令行参数：

```text
--backend vulkan     选择后端（目前只有 Vulkan）
--model <path>       启动后加载模型（assets/ 之外的模型会先导入）
--scene <path>       启动后加载场景，替换默认测试场景
--frames <count>     渲染指定帧数后退出
--capture <file.png> 与 --frames 一起用，退出前保存最后一帧视口
```

## 仓库结构

| 路径 | 内容 |
| --- | --- |
| `app/` | 程序入口 `miniengine_app` |
| `engine/core/` | 日志、输入、UUID、版本号 |
| `engine/platform/` | SDL 窗口、文件对话框、界面缩放 |
| `engine/scene/` | 组件、只读场景接口、材质图、世界单位 |
| `engine/logic/` | `IEditorWorld`：实体、选择、场景 YAML 序列化 |
| `engine/asset/` | glTF/贴图加载、模型缓存、资产 UUID 注册表 |
| `engine/editor/` | 编辑器后端基类、UI 面板、编辑服务 |
| `engine/renderer/` | 渲染纯逻辑（`engine_render_core`）、RHI 接口与 Vulkan 后端 |
| `engine/application/` | `EditorApplication` 生命周期与参数解析 |
| `shaders/vulkan/` | GLSL 着色器 |
| `tests/` | 单元测试，以及 vcpkg 布局、格式化两组契约测试 |
| `cmake/`、`scripts/` | CMake 模块、vcpkg overlay port、构建与格式化脚本 |
| `docs/` | 渲染说明、构建细节、开发记录、设计文档 |

## 架构

模块依赖只能由上层指向下层：

```text
app → application → core / platform / renderer
renderer → render_core / editor / logic
editor   → render_core / logic / asset / scene / platform / core
render_core → core / scene / asset;  logic, asset → core / scene;  platform → core
```

- **启动**：`main` 创建 `EditorApplication`，由它创建窗口和 Vulkan 渲染器。`VulkanRenderer` 继承 `EditorRenderBackendBase`，后者负责加载设置、资产注册表和默认场景。
- **每帧**：处理输入与相机 → 处理 UI 请求和异步加载结果 → 更新 CPU Renderable → `DrawFrame()`，按需上传资源，然后录制场景与 ImGui 并提交。
- **数据流**：`IEditorWorld` 是场景数据的唯一来源。实体变更打上 `ModelRenderableDirty` 标记后，`RendererWorld` 按实体增量更新 CPU Renderable；GPU 资源目前仍整批重建。

## 开发约定

- Vulkan UV 原点在左上角，贴图不做垂直翻转，也不做 `1 - v` 补偿。
- 世界单位是米（`engine/scene/world_units.h`）。
- 导入把模型复制到 `assets/models/<bundle>/`，从不修改源文件；贴图路径一律相对于模型文件。
- 资产 UUID 存在 `<文件名>.miniengine_asset.yaml` sidecar 中。场景保存 `entity_uuid`，从不持久化 `entt::entity`。
- `ISceneWorld::Registry()` 只读；所有写入都必须经过场景接口。
- 帧内图像布局转换由 `RenderTargetLayoutTracker` 显式发出。每个 render pass 的附件都必须满足 `initialLayout == finalLayout`。
- [CMakePresets.json](CMakePresets.json) 是构建参数的唯一来源；`.slnx` / `.vcxproj` 只是包装层。

## 已知限制

- RHI 只有 Vulkan 一个后端，并且与编辑器基类耦合。
- GPU 缓冲、贴图和描述符在内容变化时整批重建，显存按 submesh 分别分配。
- 没有视锥剔除；只有一盏方向光投射阴影。
- 材质中的贴图引用只存路径，重命名贴图会打断引用。
- glTF 还不支持额外 UV 集、sampler wrap 和 `KHR_texture_transform`。
- 脚本、动画、物理、音频和 Play 模式尚未实现。

## 路线图

1. 分清编辑器数据与运行时数据的边界。
2. 做更完整的 RHI 与资源生命周期：GPU 资源细粒度更新，降低 Vulkan 后端对编辑器基类的耦合。

## 维护说明

- 源码、各模块的 `CMakeLists.txt` 与 `CMakePresets.json` 优先于文档；[docs/PROJECT_SUMMARY.md](docs/PROJECT_SUMMARY.md) 只是历史资料。
- 功能或限制变化时，同步更新本 README 的清单。实现细节写进 [docs/RENDERING.md](docs/RENDERING.md)；决策与踩坑写进 [docs/DEVLOG.md](docs/DEVLOG.md)。
- 不要覆盖用户的 `imgui.ini`，也不要改动既有的 `docs/superpowers/plans/`。
- 分清验证类型：构建和 CTest 证明编译与自动化回归，`--frames 60` 只验证启动路径，画面效果仍需人工确认。
