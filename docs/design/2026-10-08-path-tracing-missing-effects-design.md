# 路径追踪补缺：玻璃与透明层、次级顶点材质、次级光线的雾与大气、夜景亮度

## 背景（2026-10-08）

`2026-10-07-path-tracing-design.md` 的“已知限制”里列了路径追踪下缺的画面效果。用户要求先做前四项：

1. 玻璃、透射、半透明（Blend）不参与路径追踪；
2. 次级顶点材质只有基础层（无法线贴图、清漆、sheen）；
3. 雾和大气透视只作用于主表面；
4. 夜景比光栅暗很多。

以下各节对应这四项。第 4 项查下来是光栅（DDGI）的错，不是路径追踪的。

## 1. 玻璃与 forward 着色的表面

### 1a. 主表面：路径追踪的透明层（`VulkanPathTraceLayerPass`）

R34 的车窗、GTA 的树叶卡片、路面贴花、栅栏都是 Blend 材质，由 forward pass 着色，环境光一直来自 DDGI 和天空，
路径追踪只管 G-buffer 里的不透明表面。现在每个像素上**最近的一层** forward 表面（Blend、透射、forward 着色的
Opaque/Mask）也被路径追踪：

- **`ScenePassId::PathTraceLayer`**（deferred 顺序里紧接 `PathTrace` 之前），用 gbuffer.frag 画两遍 forward 物件
  （`frame.PathTraceLayerDrawItems()`：从 `forwardShadedDrawItemBegin` 到结尾）：
  - **深度 pass**（`path_trace_layer_depth.frag.spv`，`PATH_TRACE_LAYER_PASS=1`）：把片元深度写进 R32F，
    颜色混合 `MAX` 保留最近者（reverse-Z）；深度附件是场景深度，只测试（`GREATER_OR_EQUAL`）不写。
    Blend 物件 alpha < 0.05 的片元不算覆盖。
  - **表面 pass**（`path_trace_layer_surface.frag.spv`，`=2`）：只保留深度等于上一步结果的片元，写 G-buffer 的
    albedo / 法线 / surface / 速度四张（格式与 G-buffer 相同，其余槽位 `VK_ATTACHMENT_UNUSED`）。着色标志只留
    UNLIT：层按纯基础层追踪，forward pass 再乘回完整材质的 lobe。匹配判断放在 shader 最后，所有导数之后。
- **`VulkanPathTracePass` 第二遍**：同一套 trace / temporal / filter，换一个描述符集（层的 G-buffer、层自己的三对
  历史、结果对），raw 对两遍共用作草稿。层**总是**自己累积和降噪（读设置里的 accumulate/denoise，不受 DLSS RR
  影响，因为 RR 看不到它），两种路径追踪器（普通、ReSTIR PT）下都运行。GPU 计时单独一段
  （`PathTraceOpaque` = 不透明，`PathTrace` = 层）。
- **forward pass**（triangle.frag）：相机块 `textureParams.y` 表示本帧有层；片元深度与 set 0 binding 29（层深度）
  **按位相等**时（triangle.vert 的 `invariant gl_Position` 保证三条管线深度一致），取 binding 30/31 的漫反射 / 镜面
  结果作 `pathTracedIndirect`。`ShadeSurface` 的路径追踪分支补上透射：透射材质的漫反射份额照旧换成透射复制里
  身后的光（漫透射的背面辐照度仍取探针）。层后面的表面保持 DDGI + 天空。
- **图像**：层 pre-pass 五张 + 层历史/结果八张，第一次路径追踪帧才创建（1080p 约 190 MiB，DLSS 性能档渲染分辨率
  约 1/4）。创建时等所有帧、一次性把图像转到 `SHADER_READ_ONLY_OPTIMAL`（`VulkanUploadBatch`）再改写各帧
  set 0；之后每帧结束都回到这个布局。分辨率重建时 set 0 改回占位（DFG 表）。
- **开关**：`render_debug.path_tracing.forward_surfaces`（默认开），Graphics Debug “Glass and blended surfaces”。
  关掉时与之前完全一样（也不让次级光线看见 Blend）。

### 1b. 次级光线看见玻璃

- **Blend 实例**进入 TLAS，`kRayInstanceBlend` 标志 + 新实例掩码 `kRayMaskBlend`（0x4）；只有路径追踪的
  `RAY_MASK_PATH` 包含它，阴影、AO、反射、DDGI 探针、CPU 参考追踪仍然看不见（与之前一致）。
- 只有**镜面类路径**（`sharp`：至今都是粗糙度 < 0.3 的镜面瓣）的光线遇到 Blend：反射里能看到车窗、贴花；
  漫反射等宽瓣光线照旧穿过。（让所有路径都遇到时，GTA 的树叶卡片使不透明 trace 多了约 13 ms。）
- 覆盖按材质 alpha 随机决定：镜面路径读纹理 alpha（`RAY_MATERIAL_ALPHA_BLEND`），其它用平均覆盖率哈希。
- **KHR_materials_transmission**（`RAY_MATERIAL_TRANSMISSION`）：路径光线整体命中它（`rayMeetsTransmission`），
  由 TracePath 自己处理：透射瓣占漫反射的 `transmission` 份额，按 albedo 着色；薄壁（无厚度或双面）直穿，
  体积则在入射面按粗糙度微表面折射、出射（背面命中）时按 Beer-Lambert 衰减后按 Fresnel 折射或反射回去
  （全反射时全部反射），出射界面不计反弹次数（每条路径最多 4 次）。阴影光线仍按覆盖率哈希。

## 2. 次级顶点的材质

`ray_hit_common.glsl` / `path_trace.comp`：

- **法线贴图**：光线纹理表每个 draw slot 从 4 张增加到 5 张（`RAY_TEXTURE_NORMAL`），有自己法线贴图的材质在光线
  材质里标 `RAY_MATERIAL_NORMAL_MAP`；`RayHitMappedNormal` 用插值的顶点法线和切线（按法线方式变换到世界空间，
  再投影到法线平面）建 TBN，背面镜像整个切线框架，与 gbuffer.frag 相同。只在镜面类路径上取（宽瓣后面细节被平均掉）。
- **清漆、sheen、介电 specular**（`RayHitLayersOf`）：只用材质因子（贴图省略）。`PathTraceLobesOf` 给出四个瓣的
  反照率，下一方向在透射 / 漫反射 / 基础镜面 / 清漆镜面之间按反照率选；清漆瓣用 GGX 可见法线采样，权重
  `coat.factor·F_coat·G2/G1`。直接光按 ShadeSurface 的方式组合：基础 × sheen 缩放 + sheen，再 × (1 − 清漆 Fresnel)
  + 清漆。局部光 RIS 的目标仍用基础层（便宜），只对选中的那盏灯求完整分层贡献。
- 仍未做：混合图（blend graph）第二层、细节层、各向异性、清漆/sheen 的贴图。ReSTIR PT 的次级顶点没改
  （它的 shift 需要储存的重连顶点材质与重算一致，改材质要连同储备池格式一起改）。

## 3. 次级光线经过大气和高度雾（`RayMedium`）

每段路径光线（击中或离开场景）都乘透射率、加内散射，和光照 pass 给 G-buffer 表面做的一样（先大气、再雾）：

- **大气**：从光线起点（相机的大气坐标加上世界偏移 × 距离缩放）沿光线用 Hillaire 的积分
  （`IntegrateScatteredLuminance`，带多次散射 LUT，新绑定 18），每 0.5 km 一步、1–8 步；短于 0.2 km 的光线跳过
  （晴天近地面每公里消光不到 4%）。离开场景的光线不加：天空的光已经包含大气。
- **高度雾**：`HeightFogOpticalDepth` 改成从任意起点（没有起始距离），离开场景时积到无穷远（向下为不透明）；
  内散射与主表面相同（天空平均 + 太阳的 HG 瓣，不算雾内阴影），不透明度上限 `max_opacity` 同样适用。
- 开关 `render_debug.path_tracing.ray_media`（默认开），Graphics Debug “Air and fog along the paths”。
- 效果：雾里阴影被雾的内散射补亮，反射里的物体被雾化，与混合管线（DDGI 含雾前的光）更接近。代价可忽略
  （GTA 1080p 下 < 0.5 ms，测不出来）。

## 4. 夜景亮度：其实是 DDGI 留着白天的光

GTA Grove Street，23:00（月光 0.267 lux），EV 0：路径追踪整图均值 10，混合 20；路面 13.7 对 34。

- 关掉 DDGI 的混合路面 14.6 ≈ 路径追踪；关掉光追探针遮挡的混合**整幅过曝**（均值 214），探针遮挡只是把它
  遮住了一部分；DDGI hysteresis 设 0 时混合正常（路面 16.7）。所以是 DDGI 探针留着旧光。
- 原因：场景文件的太阳（120000 lux）在头几帧生效，随后时间系统换成月亮，开新的光照纪元。纪元变化时探针
  “保留一半”，之后按 1/(n+1) 平均——白天比夜里亮约 10^5 倍，n 次更新后仍剩 10^5/(n+1)；而且探针光线的反弹
  读邻居探针，没轮到的邻居还亮着，刚重来的探针又被它们染亮。
- 修正：
  - `ddgi_update.comp`：变化（新纪元或 4σ 检测）且这次更新的平均亮度与历史均值相差超过 2 倍（`RESTART_RATIO`）
    时，不保留旧值；
  - 渲染器：新纪元的光照总量（方向光照度 + 环境 + HDRI）与上一纪元相差超过 2 倍（`DdgiLightingJumped`，
    `kDdgiLightingJump`）时清空整个探针体积（同改布局）；日落那样的渐变每个纪元只差 0.5%，不受影响。
- 结果：夜里混合整图 10.4、路面 14.4，路径追踪 9.9 / 13.7；白天混合与修改前逐像素差约 1/255。
- 路径追踪这边的萤火虫钳制在此场景夜里不再有可见影响（钳制 0 与 32 的路面 13.8 对 13.7）。

## 验证

- Vulkan 验证层：普通路径追踪、DLSS RR + 路径追踪、ReSTIR PT + RR 均无错误（只有 NGX 自己图像的已知警告）。
  曾出现的 binding 29 布局错误（深度 pass 静态引用自己的附件）通过拆成两个 SPIR-V 变体解决。
- 调试时一度看到玻璃上沿三角形边的黑线：是调试 shader 在分支里提前 `return`、后面还有求导代码造成的未定义
  行为，正式代码没有（用很亮的常数结果验证过整片玻璃均匀）。
- 单元测试 122 个全过；新增 `ddgi_volume` 的 `LightingJumpsClearTheProbes`，`scene_pass` 的顺序改为 26 个。

## 性能（RTX 4070 笔记本，Release，GTA Grove Street 300 m + R34 + Yuki，1920×1080 原生，三次交替取中位数）

| 配置 | 不透明 trace（含 temporal/filter） | 层 pre-pass | 层 trace | 合计 |
|---|---|---|---|---|
| 修改前 | 13.5 ms | — | — | 13.5 ms |
| 新代码，`forward_surfaces` 关 | 16.1 ms | — | — | 16.1 ms |
| 新代码，全开 | 17.4 ms | 0.9 ms | 4.6 ms | 22.9 ms |

- 关闭层时多出的约 2.6 ms 来自更大的 trace shader（分层材质、透射、介质），逐项关掉都只差 0.3–1 ms，单次运行的
  噪声有 ±1–2 ms，没法再细分。
- 层的代价与 forward 表面覆盖的像素数成正比；GTA 里树叶卡片、路面贴花占画面很大一块。
- 测量开始时用户自己的引擎在跑，GPU 满载，那段数字（50–60 ms）作废，表中是 GPU 空闲后重测的。

## 仍未做

- 自发光三角形的 NEE、局部光的光源瓦片、给 DLSS RR 的镜面命中距离（原清单第 5–7 项）。
- 层只有最近的一层；层后面的 Blend 表面、alpha < 0.05 的部分仍用 DDGI + 天空。
- ReSTIR PT 没有分层材质和次级介质。
