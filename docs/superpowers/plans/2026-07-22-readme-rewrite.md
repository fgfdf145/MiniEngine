# MiniEngine README Rewrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 全面重写根目录 README，使 AI 助手和长期维护者能从当前仓库事实中理解 MiniEngine 的功能、架构、约束、构建方式与能力边界。

**Architecture:** 只修改根目录 `README.md`，以源码、CMake、presets、manifest、脚本和测试为当前事实来源，以旧 README 为历史决策来源。新版文档按“定位与功能 → 架构与目录 → 硬性约定 → 构建验证 → 能力边界 → 路线与历史”的阅读路径组织，并通过静态检查验证链接、依赖、命令和差异范围。

**Tech Stack:** Markdown、C++20、CMake 3.25+、CMake Presets、vcpkg、PowerShell、Git

## Global Constraints

- 主要读者是 AI 助手和长期维护者，不写成产品宣传或最终用户教程。
- 本次实现只修改根目录 `README.md`；计划文件属于执行流程记录，不修改程序代码、构建配置、测试、资产、`imgui.ini` 或 `docs/PROJECT_SUMMARY.md`。
- 当前源码、各模块 `CMakeLists.txt`、`CMakePresets.json` 与 `vcpkg.json` 的优先级高于旧文档。
- `docs/PROJECT_SUMMARY.md` 只能标注为历史阶段性分析，不能称为当前架构事实来源。
- 使用中文，保留必要的英文类型名、命令和文件路径。
- 不加入没有现成素材的徽章、截图或外部链接。
- 不把本次未运行的构建、测试或 GUI 行为写成“本轮已验证”。
- 保留用户未提交的 `imgui.ini` 修改，不暂存、不覆盖、不提交。

---

### Task 1: 全面重写并静态验证根 README

**Files:**
- Modify: `README.md`
- Reference: `docs/superpowers/specs/2026-07-22-readme-rewrite-design.md`
- Reference: `CMakeLists.txt`
- Reference: `CMakePresets.json`
- Reference: `vcpkg.json`
- Reference: `app/main.cpp`
- Reference: `engine/application/editor_application.cpp`
- Reference: `engine/*/CMakeLists.txt`
- Reference: `engine/scene/scene_components.h`
- Reference: `engine/scene/scene_world.h`
- Reference: `engine/logic/editor_world.h`
- Reference: `engine/editor/editor_ui.h`
- Reference: `engine/editor/editor_backend_base.cpp`
- Reference: `engine/asset/asset_registry.h`
- Reference: `engine/asset/model_loader.h`
- Reference: `engine/renderer/rhi/backend.h`
- Reference: `engine/renderer/renderer_world.h`
- Reference: `tests/CMakeLists.txt`
- Reference: `scripts/README.md`

**Interfaces:**
- Consumes: 当前仓库文件、已确认的 README 重写设计，以及旧 README 中仍有效的技术决策和历史验证记录。
- Produces: 一个自包含的 `README.md`；后续维护者可从固定章节定位项目功能、目标依赖、数据约定、构建命令和限制。

- [ ] **Step 1: 锁定工作区基线和允许修改的文件**

Run:

```powershell
git status --short
git diff --name-only
```

Expected: 输出包含用户已有的 `imgui.ini` 修改；执行过程中不得还原、暂存或提交它。除计划文件外，README 重写前不应出现新的程序源文件改动。

- [ ] **Step 2: 用已确认的信息架构重写 README**

将 `README.md` 重写为以下固定一级章节，顺序不可颠倒：

```markdown
# MiniEngine
## 1. 项目定位
## 2. 已实现功能
## 3. 架构与运行流程
## 4. 仓库结构
## 5. 数据、资源与实现约定
## 6. 构建、运行与验证
## 7. 当前能力边界
## 8. 路线图
## 9. AI 与维护规则
## 10. 决策与开发记录
```

每章必须包含以下明确内容：

- `项目定位`：SDL3 + Vulkan + Dear ImGui + EnTT 的 3D 场景编辑器原型；编辑器闭环已形成，脚本/物理/动画/Play 模式等运行时闭环尚未形成；语言标准为 C++20。
- `已实现功能`：编辑器停靠界面和视口交互、模型/灯光统一场景、YAML v3、资产浏览与批量操作、glTF 2.0 导入、材质 sidecar 与材质图、Cook-Torrance PBR、多类型灯光、异步模型/场景加载与后台导入、设置/主题/输入监视。
- `架构与运行流程`：列出真实 CMake 目标依赖；说明启动、逐帧、场景到 CPU Renderable、CPU 到 Vulkan 资源的流程；注明 RHI 目前仅有 Vulkan，`VulkanRenderer` 仍继承 `EditorRenderBackendBase`。
- `仓库结构`：覆盖 `app/`、`cmake/`、`engine/core`、`platform`、`scene`、`logic`、`asset`、`editor`、`renderer`、`application`、`assets/`、`shaders/`、`tests/`、`scripts/`、`docs/` 与 `miniengine.settings.json`。
- `数据、资源与实现约定`：保留 Vulkan UV/不翻转、米制单位、模型导入不改源文件、UUID sidecar、场景 UUID/YAML v3、Registry 对外只读、CMake 依赖方向和根 `.slnx` 包装层规则。
- `构建、运行与验证`：准确列出 vcpkg manifest 依赖；Windows Visual Studio 2026 x64/Win32、bootstrap/build/generate-sln、macOS/Linux presets、`--backend vulkan`、`--model`、`--frames`、CTest 与 60 帧冒烟命令。
- `当前能力边界`：仅 Vulkan、glTF 额外 UV/sampler wrap/`KHR_texture_transform` 缺口、部分路径引用、GPU 资源整批上传、无完整运行时能力。
- `路线图`：标记已完成的结构拆分、统一实体模型和资产 UUID；后续只写编辑器/运行时边界与 RHI/资源生命周期两条已有方向。
- `AI 与维护规则`：读 README、遵守硬性约定、保留工作区修改、同步文档、区分自动化验证与 GUI/视觉确认。
- `决策与开发记录`：倒序保留 `.slnx`/并行构建、Vulkan 剔除和验证层、资源生命周期与同步、EnTT P0/P1/P2、资产 UUID、模块拆分、UV、光照和场景保存的原因及坑。

依赖列表必须与当前 manifest/CMake 一致，包含 SDL3、Vulkan、Dear ImGui、ImGuizmo、EnTT、yaml-cpp、tinygltf、GLM、spdlog、stb 和 shaderc；不得包含 `nlohmann-json`。

- [ ] **Step 3: 核对依赖、目标、preset 和 CLI 参数**

Run:

```powershell
$requiredTerms = @(
  'SDL3', 'Vulkan', 'Dear ImGui', 'ImGuizmo', 'EnTT', 'yaml-cpp',
  'tinygltf', 'GLM', 'spdlog', 'stb', 'shaderc',
  'engine_application', 'engine_renderer', 'engine_editor', 'engine_render_core',
  'engine_asset', 'engine_logic', 'engine_scene', 'engine_platform', 'engine_core',
  'vs2026-x64', 'vs2026-x86', '--backend', '--model', '--frames'
)
$readme = Get-Content -Raw -Encoding UTF8 README.md
foreach ($term in $requiredTerms) {
  if (-not $readme.Contains($term)) { throw "README missing required term: $term" }
}
if ($readme.Contains('nlohmann-json')) { throw 'README still lists removed dependency nlohmann-json' }
'README_REQUIRED_TERMS_OK'
```

Expected: `README_REQUIRED_TERMS_OK`，无异常。

Run:

```powershell
rg -n '"name": "(vs2026-x64|vs2026-x86|macos-debug|linux-debug)"' CMakePresets.json
rg -n 'argument == "--(backend|model|frames)"' engine/application/editor_application.cpp
```

Expected: 四个 configure preset 和三个 CLI 参数均能在当前配置/源码中找到。

- [ ] **Step 4: 验证 README 中的仓库内链接和关键路径**

Run:

```powershell
$readme = Get-Content -Raw -Encoding UTF8 README.md
$relativeLinks = [regex]::Matches($readme, '\[[^\]]+\]\((?!https?://|#)([^)]+)\)') |
  ForEach-Object { $_.Groups[1].Value.Split('#')[0] } |
  Where-Object { $_ -ne '' } |
  Sort-Object -Unique
foreach ($link in $relativeLinks) {
  if (-not (Test-Path -LiteralPath $link)) { throw "Broken README link: $link" }
}
$requiredPaths = @(
  'MiniEngine.slnx', 'CMakePresets.json', 'vcpkg.json',
  'scripts/bootstrap-deps.ps1', 'scripts/build.ps1', 'scripts/generate-sln.ps1',
  'tests/CMakeLists.txt', 'docs/PROJECT_SUMMARY.md'
)
foreach ($path in $requiredPaths) {
  if (-not (Test-Path -LiteralPath $path)) { throw "Missing referenced path: $path" }
}
'README_PATHS_OK'
```

Expected: `README_PATHS_OK`，无断链或缺失路径。

- [ ] **Step 5: 检查未完成标记、差异质量和修改范围**

Run:

```powershell
$markers = @('T' + 'BD', 'T' + 'ODO', '待补充', '占位' + '符', 'nlohmann-json')
foreach ($marker in $markers) {
  if (Select-String -LiteralPath README.md -SimpleMatch $marker -Quiet) {
    throw "README contains forbidden unfinished marker or stale dependency: $marker"
  }
}
git diff --check -- README.md
git diff --stat -- README.md
git status --short
```

Expected: 禁用词搜索无结果；`git diff --check` 无输出；README 是本任务唯一修改的交付文件，`imgui.ini` 仍保持用户原有的未暂存状态。

- [ ] **Step 6: 人工复核最终 README**

逐章确认：

1. 所有“已实现”条目都能指向当前源码或测试。
2. 所有历史构建/测试结果都位于开发记录中，没有被描述为本轮验证。
3. `docs/PROJECT_SUMMARY.md` 被标注为历史资料。
4. CPU 增量更新与 GPU 整批上传没有混为一谈。
5. Vulkan 后端现状没有被描述成完整多后端 RHI。
6. 旧 README 中的高价值约定和渲染踩坑仍可检索。

Expected: 六项全部满足；如任一项不满足，直接修正 README 并重新执行 Step 3–5。

- [ ] **Step 7: 只提交 README**

Run:

```powershell
git add -- README.md
git diff --cached --name-status
git commit -m "docs: rewrite repository README"
```

Expected: 暂存列表只包含 `M README.md`；提交成功；`imgui.ini` 未进入提交。
