# 硬件路径追踪（实时 + 静止时累积参考图）

## 背景（2026-10-07）

硬件光追基础（`2026-10-07-hardware-ray-tracing-design.md`）和光追效果（`2026-10-07-ray-traced-effects-design.md`）
之后，用户要求实现路径追踪，并定下两点：**两种模式都要**（实时 + 相机静止时累积成参考图），
**主顶点取自 G-buffer**（不追主光线）。编辑器菜单里早已有 Render > Pipeline > Path Tracing，
但渲染器背后什么都没有。

ReSTIR PT Enhanced（`2026-10-07-restir-pt-enhanced-design.md`，分支 `claude/restir-pt-enhanced-6d02ce`）
是另一项暂停的工作；本文的设置结构与它兼容（同为 `render_debug.path_tracing.*`），之后的重采样可以
建在这里的路径追踪之上。

## 总体结构

- **直接光不变**：光照 pass 照旧算太阳和局部光（光追阴影、LTC 面光源）。路径追踪只替换**所有环境项**：
  DDGI 漫反射、天空 split-sum 镜面、SSR/光追反射、AO、屏幕空间 GI、清漆和 sheen 的环境项。
  这样主表面的直接光与混合管线逐像素相同，路径追踪只负责其余的光。
- **开关**：`RenderDebugSettings::pathTracing`（`PathTracingSettings`），即 `render_debug.path_tracing.*`：
  `enabled`、`max_bounces`（默认 3）、`firefly_clamp`（默认 32，HDR 目标单位；0 = 参考模式）、
  `light_candidates`（默认 8）、`accumulate`、`motion_frames`（默认 32）、`max_frames`（默认 2048）、`denoise`。
  只在硬件光线 + 光追场景就绪 + deferred 顺序 + 非 Khronos 参考视图时运行，否则自动退回混合管线。
- **编辑器**：Render > Pipeline 三个模式现在都接上了设置：Path Tracing = `pathTracing.enabled`，
  Hybrid = `hardwareRayTracing`（光追效果），Rasterization = 两者都关；Ray Tracing 开关即
  `hardwareRayTracing`。GPU 没有 ray query 时 Hybrid / Path Tracing 灰掉。Graphics Debug 新增
  “Path tracing” 一节（各参数 + 状态行：静止了多少帧 / 已收敛 / 为什么没运行 / 由 DLSS RR 降噪）。
- 路径追踪模式下 AO、探针遮挡、光追/屏幕空间反射、屏幕空间 GI 都不运行。DDGI 仍更新：forward 着色的
  透明/传输材质还用它的环境光。

## Pass：`VulkanPathTracePass`（`ScenePassId::PathTrace`，SsrResolve 之后、Lighting 之前）

输出借用两个已有目标：**SceneGi = 漫反射通道，SceneReflections = 镜面通道**（都是全分辨率 RGBA16F，
光照 pass 本来就绑着）。路径追踪模式下 SSR resolve 先写的反射被覆盖，GI resolve 不再写 SceneGi
（于是 “间接漫反射” 调试视图和 `--reference` 都能读到路径追踪结果）。不新增常驻 VRAM：pass 自己的
中间图（原始路径一对、三对历史）在第一次路径追踪时才创建（1080p 约 100 MiB），目标重建时释放。

### 1. trace（`path_trace.comp`，ray query）

- 每像素两条路径：一条从漫反射瓣（余弦采样），一条从镜面瓣（GGX 可见法线采样，按反照率在基底与清漆间选）。
  两条都在一个循环里调用同一个 `TracePath`，着色器只实例化一次（寄存器/占用率与一条路径相同）：
  这一改动把赛道 1080p 的 trace 从 6.3 ms 降到 2.35 ms。
- **解调**：`PathTraceLobesOf`（`pbr_common.glsl`）给出主表面各瓣的方向反照率（与环境项相同的 DFG
  split-sum、能量补偿，以及 sheen/清漆对基底的衰减）。trace 除以它，光照 pass 乘回，两边用同一函数，
  不可能不一致；降噪器过滤的是光而不是纹理。镜面与清漆共用一个通道。
- 每个后续顶点（`ray_hit_common.glsl` 的命中材质：基础色、金属度、粗糙度、自发光）：
  - 自发光；离开场景时取天空（预滤波环境 mip 0，含云，不含太阳盘）+ Ambient/Hemisphere 灯；
  - NEE：每个方向光一条阴影光线（太阳大小的圆盘，乘云影）；局部光用 RIS 从若干候选中选一个
    （点在视锥内时从 cluster 网格的列表里选，只有照得到它的灯），一条阴影光线指向光源球面/矩形上一点；
    贡献用引擎自己的 `EvaluateSceneLight`（与光照 pass 同一 BRDF、LTC 面光源）。解析光源对路径不可见，
    不会重复计数，也就不需要 MIS；
  - 下一方向：按反照率在漫反射/镜面瓣之间选；第 3 个表面起 Russian roulette。
- **firefly 控制**（`firefly_clamp > 0` 时）：每个顶点贡献的预曝光亮度上限；加上简单的路径正则化
  （Kaplanyan 2013）：漫反射反弹之后的表面粗糙度至少 0.3。灯光经近镜面反射到漫反射表面的焦散，
  NEE 只能偶然找到，正则化后由散落亮点变成柔和光晕。`firefly_clamp = 0` 时两者都关：无偏参考。
- 纹理 alpha：只有 “锐利” 路径（镜面瓣粗糙度 < 0.3，且还没经过漫反射）用纹理测试（树叶形状）；其余光线
  用覆盖率哈希，平均光照相同。`rayTexturedAlpha`（`ray_tracing_common.glsl`）默认 true，其它着色器不变。
  Liberty City 一次反弹 10.5 → 8.3 ms。
- 纹理 LOD 用 ray cone，漫反射反弹展宽 0.3 rad。

### 2. temporal（`path_trace_temporal.comp`）

按运动矢量重投影，双线性 4 个 tap，每个 tap 只在上一帧的同一表面上才用（历史里存了上一帧的视深与法线：
视深差 < 3%，法线点积 > 0.85）。新值 = mix(历史, 本帧, 1 / 帧数)，帧数上限为本帧的历史上限。历史随
预曝光缩放（与 TAA 相同的 `TaaHistoryScale`）。相机或场景在动时，镜面通道的历史按粗糙度缩短
（光滑反射不随表面移动）。

### 3. filter（`path_trace_filter.comp`）

三次 à-trous（5×5 B 样条，步长 1、2、4）：平面距离、法线、亮度（以 3×3 邻域标准差缩放）作边缘停止，
镜面通道还要求粗糙度相近。像素历史超过 `motion_frames` 后逐渐淡出（到 8 倍时完全关闭），所以静止画面
收敛后就是纯累积：参考图。

DLSS 光线重构运行时，temporal 和 filter 都不跑，trace 直接写目标，RR 拿原始噪声降噪
（和光追阴影的做法一致）。

## 静止检测与累积（CPU，`engine/renderer/path_tracing.h`，单元测试 `miniengine.path_tracing`）

`PathTraceAccumulation` 精确比较（不像 DDGI 光照纪元那样有容差）：view、未抖动的投影、渲染尺寸、
全部 `RenderDebugSettings`、所有选中的灯（5 个 vec4）和天空参数（太阳、大气、HDRI），以及
“场景变化”（有实例移动：`DdgiMovingInstances::MovedThisFrame`；内容安装或 BLAS 完成：几何纪元；
`contentChanged`）。云每帧漂移，不计入，静止画面会把云的变化平均进去。

`PathTraceHistoryCap`：在动时为 `motion_frames`，之后每静止一帧加一，直到 `max_frames`（最多 2048，
半精度帧数能精确递增的上限）。不清空历史：一有变化，每个像素的帧数上限立刻回到 32。

## 验证

- **与 CPU 参考路径追踪器比较**（`--reference`，原本用于 DDGI，单位都是 irradiance / π）：路径追踪帧里
  读 SceneGi 的漫反射通道。`bounce_box`：path tracer / reference = **0.656**，`cornell_box` 0.67。
  差异来自**引擎的漫反射 BRDF**：直接光用 Frostbite 归一化的 Burley（粗糙度 1 时能量因子 1/1.51 ≈ 0.66），
  参考器用 Lambert。把 NEE 临时换成 Lambert 后 `bounce_box` 为 **0.931**（DDGI 同场景 1.10）。
  路径追踪保持与引擎材质模型一致（主表面直接光就是 Burley），不为了对齐参考器改 BRDF。
- Cornell box：16 次反弹、不 clamp 为 0.666，默认（3 次反弹、clamp 32）为 0.620：默认设置只少拿约 7% 的能量。
- Debug（验证层开）：赛道静止、移动相机、DLSS RR（只有 NGX 自己的 `nv.ngx.dlssd.resource` 首次布局消息，
  与 RR 落地时相同）、Cornell、灯光场景：无验证错误。
- 截图（`out/rt/rt_capture.py`，隐藏窗口、关手柄后端）：赛道隧道内比 DDGI 暗（DDGI 漏光）；Liberty City 静止
  收敛干净，阴影面略暗；移动中（0.05 m/帧）自有降噪与 DLSS RR 都干净；灯光场景局部光高光、球间互反射正确。
- 单元测试：`path_tracing`、`scene_pass`（新顺序）、`command_registry` 通过；整套 113 个中失败 2 个，均与本工作
  无关、主干上已存在：`vehicle_overlay`（d530fab 把刷子轮胎默认 rib 改成 50，测试仍期望 10）、
  `asset_browser_window`（已知的 ImGui 字体合并断言）。

## 结果（RTX 4070 笔记本，Release）

| 场景 | 分辨率 | PathTrace pass | 其中 trace | 整帧 GPU（路径追踪 / 混合） |
|---|---|---|---|---|
| ddgi_track | 1920×1080 原生 | 3.6–4.1 ms | 2.35 ms（temporal ≈0.5，filter ≈0.85） | 6.7 / 5.8 ms |
| Liberty City 300 m | 1920×1080 原生 | ≈16 ms | ≈16 ms（1 次反弹 8.3 ms） | ≈26 / 10.4 ms |
| Liberty City 300 m | 1080p 输出，DLSS Performance + RR（960×540） | 4.4 ms | — | **10.4 / 6.6 ms** |

- 路径追踪模式下光照 pass 从 1.1–2.1 ms 降到 0.3–0.5 ms（不再算 DDGI/天空环境项）；AO/SSR/GI 的 1.7–2.9 ms 不再需要。
- 大场景原生 1080p 不是实时的；与业界做法相同，实时路径追踪配合 DLSS 超分 + 光线重构使用。
- 计时时另一个会话的 `lag_fixed.exe` 断续占用 GPU（50–70%），表中数字取 GPU 空闲时的多次交错测量。

## 已知限制 / 后续

- 后续顶点的材质只有基础层：没有法线贴图、清漆、sheen、各向异性、透射；主表面的各向异性按各向同性采样，
  sheen 自身瓣的间接光不计。
- Blend/透射材质仍是 forward 光栅（用 DDGI 环境光）；路径追踪的光线把它们当作覆盖率哈希的表面。
- 自发光网格只能被 BSDF 采样命中（无 NEE）；小而亮的自发光物体收敛慢。
- 雾、大气透视只作用于主表面，不作用于次级光线。
- 去噪器没有方差缓冲（SVGF 的完整形式），用 3×3 空间方差近似；快速移动时镜面历史较短，噪声更明显。
- 后续可做：ReSTIR（DI 用于局部光，或续上 ReSTIR PT Enhanced 分支）、自发光三角形 NEE、半分辨率选项、
  镜面通道的命中距离给 DLSS RR。
