# 路径追踪离线模式

## 背景（2026-10-09）

用户要求："添加 path tracing 离线模式，这个模式默认开启全部 path tracing 功能，能设置 spp，降噪继续沿用光线重构；
现在正在向 Slang 和 NVRHI 迁移，按照这个标准做，可以复用现在的 path tracing 功能，没有的功能自己加上。"

现有的实时路径追踪（`2026-10-07-path-tracing-design.md` 及两份后续文档）只追**间接光**：主表面取自 G-buffer，直接光
仍由光照 pass 算（解析灯 + 光追阴影一条光线），每像素每帧 2 条路径，相机静止时最多累积 2048 帧（半精度历史）。
离线模式要的是"慢一点没关系，画面要对"：

- 全部路径追踪功能打开：透明/混合表面层（全分辨率）、空气与雾、发光三角形 NEE、光源网格、RR 反射引导、
  透射和多层材质变体；ReSTIR 关闭（离线要无偏累积）。
- 主表面的**直接光也由路径追踪器算**（太阳圆盘、局部光、面光源的阴影光线随 spp 一起采样），光照 pass 只加自发光和
  空气透视；光追阴影 pass 不跑。
- 每像素每帧 `samples_per_pixel` 个样本；相机和场景静止时累积到 `target_samples` 个样本后停止追踪（"完成"），
  之后每帧只把累积结果拷出去。
- 降噪：有 DLSS 光线重构时用 RR（输入是累积后的结果，越来越干净）；没有 RR 时退回自带的 à-trous（随收敛淡出）。
- 编辑器：Render > Pipeline > Path Tracing (Offline)（Ctrl+4）；Graphics Debug 的 Path tracing 一节里 "Offline mode"
  开关、六个设置和进度条，离线模式开着时实时的那些开关不显示（它们的值保留）。

## 设置

`PathTracingSettings::offline`（`OfflinePathTracingSettings`，设置文件和 `--state` 里是 `render_debug.path_tracing_offline.*`）：

| 字段 | 默认 | 说明 |
|---|---|---|
| `enabled` | false | 离线模式；需要 `path_tracing.enabled`（Render > Pipeline > Path Tracing (Offline) 同时打开两者） |
| `samples_per_pixel` | 4 | 每像素每帧的样本数（1–64） |
| `target_samples` | 4096 | 静止时累积到这么多样本停止；0 = 不停 |
| `max_bounces` | 8 | 主表面之后的反弹数（1–16） |
| `light_candidates` | 32 | 局部光 RIS 候选数 |
| `firefly_clamp` | 64 | 实时的两倍；0 = 不 clamp（参考） |
| `path_regularization` | true | 漫反射反弹之后的表面粗糙度至少 0.3（实时模式里它跟着 clamp 走，这里单独一个开关）；关 = 无偏参考 |

`EffectivePathTracing(PathTracingSettings)`（`engine/renderer/path_tracing.h`）把离线模式展开成实时设置的一组值：
离线的反弹数、候选数和 clamp，`restir = false`、`forward_surfaces = true`、`forward_surfaces_half_resolution = false`、`ray_media`、
`emissive_lights`、`light_grid`、`reflection_guides`、`accumulate`、`denoise` 全开，`motion_frames = 1`（离线模式在动时
不做时间累积：拖尾的、相关的噪声会被 RR 当作纹理留下，见 ReSTIR PT 的教训），`max_frames = ceil(target / spp)`。
渲染器在 `PrepareView` 开头、`ResolveRenderFeatures` 内部都用展开后的设置，所以面板灰掉的控件和渲染器实际跑的一致；
用户自己的实时设置不被改写，关掉离线模式就回到原样。

## 渲染管线里的改动

### 特性（`ResolveRenderFeatures`）

- `offlinePathTracing` = 普通路径追踪在跑且离线模式开；
- 离线时光追太阳/局部光阴影不跑（直接光由路径追踪器算）；
- `pathTraceAccumulate` 在离线时即使有 RR 也为真（累积的是静止帧，RR 拿到的是越来越干净的输入）；`pathTraceDenoise`
  仍是 "没有 RR 时"。

### trace（`path_trace.comp.slang`）

- push constant 加 `samplesPerPixel`、`emissiveNeeBounces`（64 → 80 字节，NVRHI 上限 128）。每个样本重新选两条路径的
  方向、主表面的发光 NEE 和直接光；`TracePath` 仍只在一个循环里调用一次（不展开，寄存器同单样本）。
- `PT_FLAG_DIRECT_LIGHT`：主表面的直接光（每个方向光一条阴影光线、局部光 RIS 选一个、面光源 LTC × 阴影光线到矩形上
  随机一点，与路径顶点的 `DirectLight` 同一函数），按 G-buffer 的各向异性求值，再拆成漫反射和镜面两份解调进两个通道：
  `pbr_common.slang` 的 `EvaluateBRDF`/`EvaluateAreaLight` 把各自的漫反射项记在 `lastLightDiffuse`（静态变量，没人读的
  着色器里被消掉），sheen 的直接光进反照率较大的那个通道。光照 pass 的乘回与解调用同一个 `PathTraceLobesOf`，所以拆分
  只影响累积/降噪的质量，不影响能量。
- 发光三角形 NEE 的顶点数改成 push constant：实时 1（原来的常数），离线等于反弹数。
- 透明层（forward 表面）照旧只追间接光（它们的直接光是 forward pass 的），但同样按 spp 采样。

### 光照 pass

`debug.w = 3`：路径追踪的光替换环境项**和直接光**（`pathTracedDirect`，`ShadeSurface` 跳过方向光和局部光两个循环），
只剩自发光和空气透视。ReSTIR 的分支改成只认 `w == 2`。

### 累积（`path_trace_temporal.comp.slang`）

- 离线模式的两个历史是 **RGBA32F**（`PT_HISTORY_FLOAT32` 变体：`path_trace_temporal_float32.comp`、
  `path_trace_filter_float32.comp`）：半精度的滑动平均到两千多帧就停住了；32 位的帧数在 2^24 内精确，上限
  `kOfflinePathTraceMaxFrames = 65536` 帧。切换模式时 `Prepare` 按格式重建历史（并重置）。
- `PT_FLAG_HOLD`：达到目标后不再追踪，temporal 只把上一帧的历史原样（乘预曝光比）拷进这一帧的历史，表面历史一起拷，
  所以乒乓、`RecordBarrier` 的逻辑不变；GPU 每帧只剩两次拷贝。
- RR 的镜面命中距离：trace 照旧写进原始镜面的 alpha，temporal 存进表面历史的 w（停住时随历史拷贝），filter 的拷贝
  步骤把它放回 SceneReflections 的 alpha（`dlss_rr_guides` 从那里读）。所以离线 + RR 也有反射引导。

### 进度

`PathTraceAccumulation` 的静止帧数照旧决定历史上限；渲染器另记累积开始的时刻，状态行显示
"Offline: 1234 / 4096 spp, 12.3 s, about 28 s left" / "Offline: done, 4096 spp in 41.0 s"，Graphics Debug 的离线一节
画进度条。

## Slang 与 NVRHI

- 新着色器代码都是 Slang（变体由 CMake 的 `MINIENGINE_HARDWARE_RAY_DEFINE_VARIANTS` / `MINIENGINE_SHADER_DEFINE_VARIANTS`
  编译）。
- 新的 GPU 资源（32 位历史）走 `HistoryImagePair` → `CreateNvrhiImage`，归 NVRHI 所有（阶段 A3 的做法）。
- 路径追踪 pass 的录制仍是原生的：trace 绑定光追场景集（原生加速结构），temporal/filter 与它共用 pass 的集。按 NVRHI
  迁移文档阶段 3 的规则，绑定光追集的 pass 等加速结构迁到 `nvrhi::rt` 时一起迁；这里不提前拆，免得和迁移分支打架。
- 本工作建在 NVRHI 分支（`claude/brave-ride-4txc74`）之上；NVRHI 合入 main 后（2026-10-09）又合了一次 main，无冲突。

## 验证

RTX 4070 笔记本，Debug 构建（着色器与 Release 相同），1280×720，`out/offline/run.py`（工作区里，被 gitignore）
的脚本化截图，固定曝光。

- **validation**：Vulkan validation 开着跑材质球、R34 道路、静止和移动相机、有无 RR：无报告。第一次跑时 trace
  着色器经 `StorePathTracePair` 的分支静态用到了 binding 10/11（格式 rgba16f，而离线模式的图是 rgba32f），
  validation 报格式不符；trace 改用只写原始/结果两对的 `StoreTraced` 后消失。
- **停住**：4 spp、目标 256 spp（64 帧），GPU 每帧 ~50 ms（8 次反弹、全部功能）；达到目标后每帧 ~1.7 ms
  （只剩 temporal 拷贝和其它 pass）。
- **与实时模式的能量一致**：材质球场景（太阳 + 点/聚光/面光源），离线（直接光由路径追踪器算）对实时
  （光照 pass 的解析直接光 + 光追阴影，同样 8 次反弹、clamp 64、32 个候选，累积 400 帧），线性输出（tone mapper
  None，EV 11，98% 的像素不截断）：整体均值 **1.005**，阴影区 1.020，球 1.013，天空 1.000。差异图只有噪声和
  半影边缘（实时的光追阴影有自己的降噪）。
- **firefly clamp 的选择**：同一场景，256 spp，以 16 spp × 256 帧（4096 spp）为参考：不 clamp 0.994（但阴影里
  满是太阳经光滑球面反射的一次反弹焦散亮点，4096 spp 也还有），32 → 0.979，**64 → 0.983**（干净，RMSE 最低之一），
  256 → 0.990（仍有细颗粒）。默认取 64。路径正则化打开后亮点变小但不消失，所以两者都要。
- **RR**：R34 道路，离线 + RR、实时 + RR、离线自带滤波三者在地面、阴影、车身、引擎盖、天空的均值相差 < 2%；
  相机以 0.02 m/帧移动时（离线模式不累积，RR 拿每帧 4 spp）画面干净。
- **单元测试**：`miniengine.path_tracing` 新增四组（展开的设置、32 位历史上限、进度、特性规则），
  `miniengine.graphics_debug_panel` 新增离线模式两张快照（有无 RR）；整套 125 项通过（两个长的车辆物理测试另算）。

## 已知限制

- 边缘像素：G-buffer 每帧按抖动光栅化，轮廓上的像素在前景和背景之间来回换，表面检查拒绝历史，所以边缘只累积
  到几帧；RR 处理这些像素（它本来就按抖动重建），没有 RR 时由自带滤波。
- 透明层的直接光仍是 forward pass 的（阴影贴图），离线模式只给它们加了 spp 和 32 位累积。
- 截图/Photo Mode 的视图不跑路径追踪（`PrepareView` 对非视口关掉它）；离线路径追踪拍照是下一步。
- trace 与它的 temporal/filter 仍是原生录制，随 `nvrhi::rt` 一起迁（见 NVRHI 文档"下一步"）。
