# MiniEngine

> **本文档是 AI 协作的统一上下文入口。**
> 无论使用哪个 AI 助手（Claude / GPT / Gemini / …），开始工作前请先完整阅读本文档；完成一次有意义的改动后，请按文末「文档维护规则」更新「开发日志」和「当前状态」。

---

## 1. 项目是什么

MiniEngine 是一个基于 **SDL3 + Vulkan + ImGui** 的 3D 场景编辑器原型，正在向轻量级游戏引擎演进。

当前定位（一句话）：

> 已完成「编辑器闭环」（模型导入 → 场景摆放 → 材质编辑 → 场景保存），尚未开始「运行时引擎闭环」（无脚本/物理/动画/Play 模式）。

**技术栈**：C++ / CMake / vcpkg（manifest 模式）
**核心依赖**：SDL3、Vulkan、Dear ImGui、ImGuizmo、EnTT、yaml-cpp、tinygltf、nlohmann-json、stb

详细的架构分析与演进路线见 [docs/PROJECT_SUMMARY.md](docs/PROJECT_SUMMARY.md)。

---

## 2. 硬性约定（AI 必读，违反会引入 bug）

这些是项目主人已经拍板的决定，**不要**在代码中引入相反的做法：

1. **全部按 Vulkan 约定编写**
   - 贴图加载**不做**垂直翻转（`TextureLoader::LoadRGBA8` 的 `flipVertically` 默认 `false`）。
   - UV 原点在左上角（行 0 = v0），与 glTF 一致。
   - 不要引入 OpenGL 风格的 `stbi_set_flip_vertically_on_load` 或 `1 - v` 补偿。

2. **资产 UUID 走 sidecar 方案**（`engine/asset/asset_registry.{h,cpp}`）
   - sidecar 命名为 `<完整文件名>.miniengine_asset.yaml`，复用已有隐藏后缀，浏览器/场景扫描自动忽略。
   - 只注册 assets 根目录下的模型（.gltf/.glb）和纹理；根外文件退化为纯路径引用。
   - 场景 YAML 同时写 `source_path` + `source_uuid`；**加载侧 uuid 优先，保存侧路径优先**（方向相反是有意的，防止保存时把用户刚改的路径覆盖回旧资产）。
   - 重复 uuid 仲裁：比对 sidecar 里的 `file` 字段和实际文件名，不匹配的一方是副本、拿新 uuid。
   - `assets/**/*.miniengine_asset.yaml` 不入 git。

3. **模型导入不改源文件**：导入 glTF 后复制到 `assets/models/<bundle>/`，生成自己的资产清单和 `.material.yaml` 材质 sidecar，源文件保持原样。

4. **单位是米**：导入时自动换算，世界单位常量在 `engine/scene/world_units.h`。

5. **模块依赖方向不可逆**（CMake 目标 DAG）：
   ```
   miniengine_app → engine_application → engine_renderer → engine_editor
     → engine_render_core → (engine_asset / engine_logic / engine_scene / engine_platform / engine_core)
   ```
   下层模块不得反向 include 上层头文件。

6. **场景实体持久身份使用 UUID**（2026-07-16）
   - `entt::entity` 只是在当前 registry 生命周期内有效的临时句柄，不得写入磁盘或作为跨加载引用。
   - 每个模型和灯光实体都必须携带 `SceneEntityIdComponent`；场景 YAML v3 写 `entity_uuid`，选择状态写 `selected_entity_uuid`。
   - 加载旧 v1/v2 场景时自动生成 UUID，并以旧 `selected_entity` 模型索引回退；重复 UUID 由先加载者保留，后续副本自动获得新 UUID。

7. **Registry 对模块外只读**（2026-07-16）
   - `ISceneWorld::Registry()` 只返回 `const entt::registry&`，供 view/query 使用。
   - 创建、销毁、组件写入和脏标记必须通过场景接口，禁止绕过 `EditorScene` 破坏场景顺序、UUID 索引、选择状态或缓存同步。

---

## 3. 目录结构

```
app/                    程序入口（main.cpp，最薄的启动层）
engine/
  application/          应用生命周期与主循环（EditorApplication）
  core/                 日志、输入系统、共享 UUID v4 生成器
  platform/             SDL 窗口、文件对话框
  scene/                场景数据定义（组件、ISceneWorld、单位常量）——纯数据层
  logic/                EditorScene：实体管理、选择、YAML 序列化、gizmo 参数
  asset/                模型缓存（ModelCache）、资产 UUID 注册表（AssetRegistry）
  editor/               编辑器模块
    editor_backend_base 编辑器工作流中枢（帧循环、UI 请求分发）
    ui/                 按面板一文件一 TU（viewport / scene / asset_browser /
                        model_processor / material_graph / model_preview /
                        theme / dock_toolbar / misc）
    services/           EntityEditService / ModelImportService / SceneIoService
                        （namespace 自由函数，首参 RendererSharedState&）
  renderer/             engine_render_core（camera、render_types、rhi 抽象）
                        + engine_renderer（Vulkan 实现、ImGui 集成）
assets/                 资产目录（模型 bundle、材质 sidecar；大文件不入库）
shaders/                着色器
scripts/                构建/引导脚本（bootstrap-deps、build，见 scripts/README.md）
docs/                   PROJECT_SUMMARY.md 等文档
miniengine.settings.json  编辑器设置持久化（UI 缩放、窗口显隐、主题）
```

---

## 4. 构建与验证

**首选构建方式（Windows）**：

```powershell
# 首次：引导 vcpkg 依赖
./scripts/bootstrap-deps.ps1

# 配置 + 构建（Visual Studio 2026 preset）
cmake --preset vs2026-x64
cmake --build --preset vs2026-x64-debug
```

⚠️ 已知问题：`x64-debug`（Ninja）preset 的缓存可能损坏（找不到 ninja），优先使用 `vs2026-x64`。

macOS 用 `macos-debug` preset，Linux 用 `linux-debug`。

**冒烟测试**（改完代码必须跑）：

```powershell
# 运行 60 帧后自动退出，检查退出码为 0 且日志无 error
./out/build/vs2026-x64/app/Debug/miniengine_app.exe --frames 60

# 场景身份/YAML 兼容性回归测试
ctest --test-dir ./out/build/vs2026-x64 -C Debug --output-on-failure
```

支持的启动参数：`--model <path>`（启动即加载模型）、`--frames <count>`（限帧退出，用于自动化验证）。

---

## 5. 当前状态

**已完成的闭环**：
- ✅ 编辑器启动 / SDL 窗口 / Vulkan 渲染 / ImGui 主循环
- ✅ 场景编辑：实体增删改、选择、gizmo（W/E/R 切换，F 聚焦）、YAML 读写
- ✅ 视口交互：点选、包围盒、右键旋转视角、中键平移、WASD、手柄、拖拽放置模型
- ✅ 资产管理：浏览、分类、复制/粘贴/重命名/删除、模型与材质预览
- ✅ glTF 2.0 导入管线：三角化、补法线/切线、单位换算、贴图复制、sidecar 生成
- ✅ 材质 sidecar 编辑：PBR 贴图、secondary layer、blend mask/factor，改动回写并刷新场景
- ✅ 多光源 PBR（LightComponent + 视口光源 gizmo）
- ✅ 统一实体模型：模型与灯光共享单一场景顺序，高频系统按 EnTT view 查询组件组合
- ✅ EnTT P1 数据路径：模型资产/包围盒/编辑器元数据冷热拆分，Renderable 按实体脏标记增量重建，世界矩阵缓存批量刷新
- ✅ EnTT P2 边界与身份：Registry 对外只读，场景实体 UUID 持久化、重复仲裁及选中状态跨加载恢复
- ✅ 资产 UUID 注册表（阶段三）
- ✅ macOS 支持

**已知缺口（可作为后续任务）**：
- glTF 解析：`texture.texCoord` 索引被忽略（恒用 TEXCOORD_0）、sampler wrap 模式被忽略（恒 REPEAT）、`KHR_texture_transform` 未实现
- `EditorScene::LoadConfig`（启动默认场景）不走 uuid 解析；`RefreshReferencedSceneFiles` 只按路径匹配场景
- RHI 只有 Vulkan 一个实现，抽象层还很薄；VulkanRenderer 仍继承 EditorRenderBackendBase（解耦留待 RHI 阶段）
- CPU Renderable 已按实体增量重建，但 Vulkan 资源集合在 Renderable 变化后仍整批上传；细粒度 GPU buffer/descriptor 更新留待 RHI 与资源生命周期重构
- 无运行时能力：脚本、动画、物理、音频、Play 模式均未开始

**路线图**（来自 PROJECT_SUMMARY，按优先级）：
1. ~~阶段一：结构拆分（editor/asset 模块独立、UI 按面板拆分、service 层）~~ ✅ 2026-07-14
2. ~~阶段二：统一实体模型（lights 与 entities 双轨合并）~~ ✅ 2026-07-16
3. ~~阶段三：资产 UUID~~ ✅ 2026-07-15
4. 阶段四：运行时与编辑器边界（Editor-only / Runtime 数据分层）
5. 阶段五：真正的 RHI 抽象（buffer/texture/pipeline/command 接口）

---

## 6. 开发日志

> 倒序记录。每条：日期 + 做了什么 + 关键决定/坑。细节多的可以链到 docs/ 下的专项文档。

### 2026-07-16 — EnTT P2 边界收口 + 稳定场景实体 ID
- 移除公共可变 Registry：外部系统仍可使用 const view 高效查询，但实体生命周期、组件写入与脏标记必须走场景接口，避免绕过顺序、选择和缓存不变量。
- 新增 `SceneEntityIdComponent` 与场景 UUID 索引；模型、灯光和临时预览实体创建时都会获得 UUID，销毁信号同步清理索引。
- 场景 YAML 升级为 v3，实体写 `entity_uuid`、选择写 `selected_entity_uuid`；旧 v1/v2 文件继续按模型索引加载并在下次保存时自动升级。
- 抽取共享线程局部 UUID v4 生成器，资产 UUID 与场景实体 UUID 复用同一实现；重复场景 UUID 由后出现实体自动换新，避免复制实体共享身份。
- 新增 `miniengine.scene_identity` 回归测试，覆盖重复 UUID 仲裁、模型/灯光 UUID 往返、灯光选择恢复和旧版索引升级。

### 2026-07-16 — EnTT P1 数据路径优化
- 将原本臃肿的 `ModelComponent` 拆成运行时资产引用、`ModelBoundsComponent` 和编辑器专用 `EditorModelMetadataComponent`，让渲染/拾取热路径不再携带材质与子网格元数据数组。
- 新增 `ModelRenderableDirty`：单实体模型、贴图或材质变化只重建该实体的 CPU Renderable；构建先落到临时结果，全部成功后再替换，防止异常造成半更新场景。
- 新增 `WorldTransformComponent` + `TransformDirty`：编辑器写 Transform 时只打标，帧内统一刷新缓存；脏标记尚未冲刷时读取会回退到即时计算，保证同帧 gizmo/面板编辑正确。
- 当前 Vulkan 后端仍在 Renderable 集合变化后整批上传 GPU 资源；细粒度 GPU 更新依赖后续资源句柄和 RHI 生命周期设计，不与本轮 ECS 数据优化混在一起。

### 2026-07-16 — EnTT P0 优化 + 统一实体模型（阶段二）
- 模型与灯光不再维护两套实体顺序，统一为稳定的场景顺序；实体类别由 `ModelComponent` / `LightComponent` 的存在性决定，便于后续扩展相机、音源等组件类型。
- 渲染、灯光上传和视口拾取等高频遍历改用 EnTT view；编辑器列表与 YAML 序列化仍使用稳定场景顺序，避免把运行时存储顺序错误地当成用户顺序。
- 批量清场先清空编辑器侧顺序与选择，再清 EnTT registry，避免 `on_destroy` 对每个实体反复线性擦除导致 O(N²)。实体有效性检查同步改为 registry/component 查询。
- EnTT 3.16 注意点：单组件 view 的默认 `swap_and_pop` 存储用 `size()`，多组件 view 才使用 `size_hint()`。

### 2026-07-15 — 资产 UUID 注册表（阶段三）
- 新增 `engine/asset/asset_registry.{h,cpp}`：静态 API + mutex，线程安全，懒初始化。
- UUID sidecar、加载/保存双向自愈、重复仲裁、重命名联动（详见上文「硬性约定」第 2 条）。
- 同期：资产分类、资产系统图标更新、macOS 支持（commit `1df8819`）。

### 2026-07-14 — 阶段一结构拆分 + UV 修复
- `engine/editor` 从 renderer 中独立成模块；UI 按面板拆成一文件一 TU；业务操作整理为 services 下的自由函数。
- renderer 拆为 `engine_render_core` + `engine_renderer` 两个目标。
- 线程安全模型缓存移到 `engine/asset/model_cache.{h,cpp}`。
- 删除一批确认无调用的死代码（DrawModelWireframePreview、旧版材质图节点绘制等）。
- **确立 Vulkan 约定**：贴图不做 V 翻转，UV 原点左上（见「硬性约定」第 1 条）。

### 2026-06-30 — 模型加载器改进
- 改进 model loader，修 UV 问题，清理冗余代码；大体积 Sponza 示例资产加入 .gitignore。

### 2026-06-26 — 光照系统 + 场景保存
- 简单多光源系统：LightComponent、GpuLightData、多光源 PBR shader、场景面板光源 UI、视口光源 gizmo。
- 修 bug，补场景保存。

### 2026-06-24 — 资产系统大更新
- 渲染器可加载 glTF 和贴图（当时尚无 PBR 和光照）；CMake 配置优化，构建更快。

### 2026-04 及更早 — 原型期
- 材质节点、平台抽象、输入监视、ImGui 主题、PBR/UV 系统雏形、移除 assimp 改用 tinygltf、SDL3 适配。

---

## 7. 文档维护规则（对 AI 的要求）

1. **开始工作前**：通读本文档，特别是「硬性约定」和「当前状态」。
2. **完成改动后**：
   - 在「开发日志」**顶部**追加一条：日期 + 做了什么 + 关键决定或踩过的坑。
   - 如果改动影响了「当前状态」「目录结构」「硬性约定」或「构建方式」，同步更新对应章节。
   - 日志写「为什么这么做」比写「改了哪些文件」更有价值——文件改动 git 都有，决策理由只有这里有。
3. **确立新约定时**（用户拍板的技术决定），写入「硬性约定」章节，并注明日期和原因。
4. **不要**把本文档写成 API 文档或代码注释的替代品；它记录的是「代码里看不出来的东西」：决策、约定、坑、路线。
5. 大型专项总结放 `docs/` 目录，本文档只留链接和一句话摘要。
