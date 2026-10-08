# 路径追踪剩余工作：自发光三角形 NEE、局部光世界网格、DLSS RR 镜面命中距离、性能

## 背景（2026-10-08）

接 `2026-10-08-path-tracing-missing-effects-design.md`（b3efc13 / 905e7c1）。用户要求按顺序做四项，每项做完实测
画面和 GPU 时间再进行下一项：

1. 自发光三角形的 NEE（车灯、霓虹、夜间发光窗），与 BSDF 命中做 MIS；
2. 局部光的光源瓦片：视锥外的点不再从全部局部灯里均匀抽候选；
3. 给 DLSS RR 镜面命中距离和反射运动矢量；
4. 性能：透明层关掉时 trace 仍比之前慢；透明层半分辨率选项。

ReSTIR PT 的分层材质和雾不在本次范围内。

### 测试场景

GTA SA 的流式地图本身**没有任何局部光和自发光材质**（729 个 cell，KHR_lights_punctual 0 个、emissiveFactor
非零的材质 0 个）。R34 唯一的自发光材质是车内中控屏 `INT_MFD`（Mask，约 29894 nit，72 个三角形）。所以测试用
生成的夜景层（`out/rt/make_lights_scene.py`，worktree 内，不入库）：

- `gta_night` 场景（23:00，EV 2）+ 以 Grove Street 为中心 360 m 见方、18 m 间距的 441 盏点光（6000 lm，range 40 m，
  投射阴影，三种色温）；
- `neon.gltf`：一块 3×1 m 青色面板（400 nit）、四根 6 cm 粗 2.4 m 高的粉色灯管（3000 nit，盒子）、一块条纹纹理
  面板（600 nit × 条纹贴图，一半纹素为黑），全部双面；
- 变体：只有霓虹（无路灯）、只有面板、只有灯管、无车、粗灯管（30 cm）。

相机 2481,13.6,1656,147,-8（与之前相同）。计时期间其它会话的 miniengine_app / 测试进程间歇占用 GPU，计时一律
交替跑三次取中位数，并在跑之前看 tasklist。

## 1. 自发光三角形的 NEE

### 光源列表（`VulkanPathTraceLights`，`path_trace_lights.*`）

- **CPU**：`VulkanRayScene::GetEmissiveSubmeshes()`——安装的内容里材质 `emissiveFactor` 非零、不是 Blend、不是
  远 LOD（Skip）的 submesh，每个 draw slot 一条（slot, 三角形数）；安装或材质变化时 `GetEmissiveGeneration()`
  加一。每个帧槽在自己的帧到来时把它展开成每个光源一条 (slot, slot 内叶序三角形号) 的列表和每个 slot 的首光源号
  （host visible 缓冲），最多 4^10 = 1048576 个三角形。
- **GPU 每帧**（`emissive_lights.comp`，路径追踪 pass 开头，GPU 计时段 `PathTraceLights`）：
  1. 每个实例一线程：未 Skip 的实例把自己的序号写给它的 slot（实例顺序每帧随顶层重建变化，移动过的实例旧叶子被
     Skip）；
  2. 每个叶子一线程：用实例的 worldToObject 求逆，把三角形变到世界空间；顺便预先算好采样要的一切——发光一侧
     （与命中判定 `frontFace` 的规则一致，镜像变换也对）、emission 贴图在三个角的 UV（已经过材质的纹理变换，
     仿射所以可以先变换再插值）、贴图 LOD 基准、emissiveFactor——采样时只读这一条记录和一次贴图；
     功率 = 三角形中心按“三角形约一个纹素”的 mip 取的 emission 亮度 × 面积（双面 ×2）；
  3. 四叉功率树逐层求和（每个节点的四个孩子是一个 vec4），根在元素 0（总功率、深度、数量）。
- 52 个三角形时整个构建 0.04 ms。

### 采样与 MIS（`path_trace.comp`、`emissive_lights_common.glsl`）

- 选光源：沿功率树走一遍（每层一个 vec4，`u` 复用），概率 = 叶子 / 根；三角形上均匀取点。
- **G-buffer 表面**：一个点、一条阴影光线，同时给两个通道：漫反射按 Lambert（与漫反射路径的余弦采样一致，
  去调制后权重 1）、镜面按 GGX 基础层 + 清漆，各自与本通道路径的方向密度做 power heuristic MIS
  （漫反射 cos/π，镜面是基础层与清漆 VNDF 的混合密度）。
- **路径顶点**：只在第一个路径顶点做（`PT_EMISSIVE_NEE_BOUNCES = 1`），分层材质完整求值，MIS 用该顶点各 lobe
  按权重混合的方向密度；最后一次反弹的顶点不再采 BSDF，NEE 权重为 1。更深的顶点只靠路径命中（previousDensity
  = 0 → 全部算上）。透射瓣、体积出射之后同样全部算上。
- **路径命中自发光面**：查 slot 的首光源号 + 叶序号得到光源，按 `P × t² / (面积 × |cos|)` 求 NEE 的密度做 MIS；
  不在列表里的（Blend、功率为 0）权重 1。功率估计为 0 的三角形只是不被 NEE 选中，结果仍无偏。
- **阴影光线俄罗斯轮盘**：预曝光亮度 < 1/64 的样本按比例概率追踪、除以概率（无偏），车内中控屏这类被遮住的
  光源因此几乎不花阴影光线。

### 遇到的问题

- **掠射角自遮挡**（bug，已修）：阴影光线最初在采样点前 1 cm 停止。GTA 坐标约 2500 m，float 精度约 0.25 mm，
  掠射角下光线最后一段与发光面的距离在舍入误差内，打中发光面自己 → 灯杆、高架桥底面这种侧向看面板的地方
  NEE 几乎全被遮挡，而 MIS 又把 BSDF 命中的权重压得很低（掠射时光源密度极大），结果比只用 BSDF 暗。
  改成先用 `OffsetRayOrigin` 把终点抬离平面又过头了：256 ulp 在这里约 6 cm，把 6 cm 粗灯管背面的采样点抬到了
  正面之外，背面被当成可见，亮了 20%。最终：终点沿光线后退 `(1 mm + 4e-7·|坐标|) / max(cosθ, 0.01)`。
- **验证方法**：截图是 8 位、经过色调映射的，曲线是凹的，噪声大的图（只用 BSDF）均值偏低（Jensen）；fp16 的
  滑动平均对重尾信号也不可靠。最终用 tone_mapper = 2（线性）+ max_frames 512 对比：
  - 只有青色面板，1 次反弹：开/关各区域差 +1.1 ~ +2.8%；3 次反弹：−2.2 ~ +3.4%（8 位量化级别）；
  - 细灯管：开 = 只用 NEE（另一个估计器）±3%，两者比只用 BSDF 高 5~11%，随参考噪声降低（色调映射→线性、
    细→粗灯管 +4~7%）差距单调缩小；只用 BSDF 的参考在远处是稀疏的饱和亮点（见 `cmp_bl_house.png`）。

### 结果

- 画面：霓虹照亮地面、车身侧面和远处的墙，不再只是 BSDF 偶然命中的亮点；车漆上的斑点噪声消失。
- GPU（1080p，三次中位数，有外部负载）：

  | 场景 | 关 | 开 | 差 |
  |---|---|---|---|
  | 白天 GTA + R34（只有中控屏发光） | 19.38 ms | 19.74 ms | +0.4 ms |
  | 夜景 441 路灯 + 霓虹 | 26.95 ms | 32.68 ms | +5.7 ms |

  消融（单次）：只在 G-buffer 表面做 +2.4 ms；第一个路径顶点再加约 5.6 ms，其中阴影光线约 2.9 ms、采样与分层
  BRDF 求值约 2.7 ms；更深顶点（已去掉）约 1 ms。
- 开关：`render_debug.path_tracing.emissive_lights`（默认开），Graphics Debug “Emissive surfaces as lights”。

## 2. 局部光的世界网格（光源瓦片）

### 问题

`DirectLight` 的局部光 RIS：顶点在视锥内时从 cluster grid 的列表里抽候选（只列出范围够得着的灯），视锥外
（反射到相机背后、漫反射弹到屏幕外）从**全部**局部灯里均匀抽 `light_candidates`（8）个。441 盏 range 40 m 的灯，
一个点通常只有十几盏够得着，8 个候选里经常一个有效的都没有。

### 做法（`light_grid.comp`、`light_grid_common.glsl`）

RTXDI 的 light tiles 是按功率预采样的全局候选表，解决的是“功率差别大”；这里的问题是空间上的，所以做的是
ReGIR 的最简形式——以相机为中心的世界网格：

- 64 × 16 × 64 个 8 m 的格子（512 × 128 × 512 m），原点按格对齐（格子随相机平移不变）；每格存“够得着的局部灯
  总数 n”和最多 32 个灯号（16 位两个一字），每帧在 GPU 上重建（每格一线程，灯 64 个一批经共享内存；
  球与格子 AABB 相交测试）。n > 32 时用 Algorithm R 蓄水池抽样留下均匀随机的 32 个，每帧重新抽。
- `DirectLight`：顶点在网格内时从格子里均匀抽候选，每个候选代表 n / min(n, 32) 份（RIS 权重乘 n）——每盏够得着
  的灯的边缘概率都是 1/n，无偏；格子里没有灯就不做局部光 NEE（正确：没有灯够得着）。网格外照旧
  cluster grid / 全部灯。视锥内也优先用网格：远处的 froxel 很深，列表里多数灯够不着具体的点。
- 缓冲每帧槽 4.5 MB（第一次需要时才创建），与发光三角形共用 set 4（binding 5）。
- 开关 `render_debug.path_tracing.light_grid`（默认开），Graphics Debug “Light grid”。

### 结果（只有 441 盏路灯，无霓虹，max_frames 512）

| 视角 | 单帧相对 RMSE 关 → 开 | 收敛均值 开/关 |
|---|---|---|
| A：默认机位（多数顶点在视锥内） | 0.046 → 0.047 | 1.001 |
| B：40° 窄视角看车身侧面，反射相机背后的街 | 0.076 → 0.058（−24%；上半部 0.211 → 0.161） | 1.001 |

GPU（路灯 + 霓虹，三次中位数）：trace 32.60 → 33.43 ms（+0.8 ms，有效候选多了，选中后的阴影光线也多了），
网格构建 0.13 ~ 0.26 ms。

## 3. DLSS RR 的镜面命中距离和反射运动矢量

- `path_trace.comp`：普通路径追踪器在 RR 下（不累积不降噪，`frame.pathTraceHitDistance`）把镜面路径第一段的
  命中距离写进镜面结果的 alpha（光照 pass 在路径追踪模式下不读它）；射出场景记 10000 m，没有镜面路径记 0。
- `dlss_rr_guides.comp`（TAA pass，RR 时）：读 `SceneReflections.a`，写两张新的 guide：
  - 命中距离（R16F）→ `pInSpecularHitDistance`；
  - 反射运动矢量（RG16F）→ `pInMotionVectorsReflections`：虚像点 = 表面点沿视线再往后走命中距离，用上一帧的
    view-proj 重投影（平面镜 + 静止场景时精确），格式与普通运动矢量相同（当前 → 上一帧，渲染像素）。
- `DlssEvaluateInputs` 加两个可选输入；ReSTIR PT 和混合模式的 RT 反射不提供（不变）。
- 开关 `render_debug.path_tracing.reflection_guides`（默认开）。

### 验证

- 命中距离调试视图正常（天空白、近地面暗、粗糙路面噪声）；故意给错的 guide（×20）时 RR 输出变化像素
  1.4%，正常开/关之间 0.4%（RR 自身的时间抖动）——NGX 确实读了这两张图。
- R34 侧面车漆，相机横移 4 cm/帧和 10 cm/帧，与静止参考对比：车身区域 RMSE 开 0.0239/0.0236、关
  0.0241/0.0237，**差别在跑与跑之间的波动以内**；两种情况都看不到明显拖影。R34 的车漆反射比较糊，这个场景
  测不出改善；更光滑的表面（镜面、湿地面）可能更明显，没有现成场景验证。

## 4. 性能

### 变体（`PT_TRANSMISSION`、`PT_LAYERED`）

`path_trace.comp` 编成四个变体（CMake `MINIENGINE_HARDWARE_RAY_DEFINE_VARIANTS`）：宏为 0 时透射处理（透射瓣、
体积出射）或次级顶点的分层材质（清漆/sheen/specular/法线贴图）在编译期折叠掉。每帧选择：透明层关掉时用
无透射变体（路径只按覆盖率遇到透射面，与透明层之前一样）；光线场景里没有任何带层或自带法线贴图的材质时用
无分层变体（`VulkanRayScene::HasLayeredMaterials`）。

白天 GTA + R34，透明层关（三次中位数，trace）：

| 变体 | trace |
|---|---|
| 全部 | 18.77 ms |
| 无透射 | 18.54 ms |
| 无分层 | 17.85 ms |
| 都没有 | 17.93 ms |

R34 有清漆，所以这个场景实际只用得上“无透射”（−0.2 ms）；没有分层材质的场景还能再省约 0.9 ms。

### 和改之前比

同一条件下（透明层关）：上一轮的 Release 17.99 ms，本轮 18.86 ~ 19.09 ms（+0.9 ~ 1.1 ms）：其中约 0.4 ms 是
发光三角形 NEE（白天只有车内中控屏发光，几乎全被俄罗斯轮盘跳过阴影光线），其余是 shader 变大。
原先说的 13.5 → 16.1 ms 那 2.6 ms，变体只拿回了透射那 0.2 ms；寄存器压力主要来自分层材质（0.9 ms），
而 R34 正需要它。

### 透明层半分辨率

`render_debug.path_tracing.forward_surfaces_half_resolution`（默认关），Graphics Debug 里 “Half resolution”：

- 层的 trace / temporal / filter 在半分辨率网格上跑（`pathTrace.gbufferShift = 1`），读层 G-buffer 每个 2 × 2 块
  左上角的像素（`PtGBufferPixel`，三个 shader 里所有 G-buffer 读取都经过它）；历史与结果图像按半分辨率创建，
  raw 对与不透明 trace 共用（用左上角）；forward pass 按 `pixel >> textureParams.z` 取结果。
- 切换时等所有帧、销毁层图像，再按新尺寸创建（一次性停顿）。
- 结果：层 trace 5.03 → 1.45 ms（−3.6 ms），层的 G-buffer pre-pass 不变（约 0.9 ms）；画面与全分辨率相对 RMSE
  2.7%，树叶看不出差别。

## 验证

- 验证层（`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`）：路灯 + 霓虹 + 半分辨率层、DLSS RR + guides、
  ReSTIR PT + RR 三种运行，引擎自己 0 条消息（只有 NGX 自己图像的已知 09600）。

- ctest（Release，24 并行）122/122 通过。

### 帧节奏（跳帧与帧时间突增）

新加诊断 `MINIENGINE_FRAME_TIMES=<文件>`：每帧 present 时写一行（steady clock 微秒、CPU 录制、CPU 等待、
最近返回的一帧 GPU 时间），`VulkanGpuTimer::GetLastFrameMs`。脚本 `out/rt/pacing.py`（worktree 内）。

相机沿 Grove Street 移动（`--camera-velocity`），同一构建里新功能全开 vs 全关（emissive_lights、light_grid、
reflection_guides）：

| 运行 | present 间隔 中位/p99/最大 | > 1.5× 中位的帧 | GPU 中位/p99/最大 |
|---|---|---|---|
| 白天 PT，900 帧，0.3 m/帧，开 | 39.2 / 77.4 / 100.1 ms | 43 | 30.4 / 64.5 / 88.6 ms |
| 同上，关 | 38.1 / 81.4 / 113.8 ms | 41 | 29.5 / 69.1 / 100.8 ms |
| 路灯 + 霓虹，开 | 83.2 / 120.0 / 150.7 ms | 6 | 60.3 / 87.8 / 110.4 ms |
| 同上，关 | 77.0 / 142.0 / 148.5 ms | 38 | 53.8 / 117.0 / 122.8 ms |
| DLSS RR，开 | 43.9 / 77.6 / 93.5 ms | 44 | 34.2 / 66.6 / 81.9 ms |
| 同上，关 | 42.9 / 75.8 / 78.2 ms | 40 | 33.2 / 65.5 / 67.4 ms |
| 白天 PT，1800 帧取后 1200，0.12 m/帧，开 | 39.7 / 61.1 / 63.5 ms | 12 | 31.1 / 52.1 / 54.4 ms |
| 同上，关 | 41.0 / 66.6 / 74.2 ms | 16 | 32.0 / 57.6 / 64.4 ms |

- 开关两边的突增数量和幅度相同，新功能没有带来新的卡顿。
- 前 15 秒的 CPU 卡顿（流式加载 `CommitContent` 100 ~ 200 ms、`RayInstances` 约 100 ms 连续三帧）两边都有，
  发生在流式半径从 150 m 涨到 6422 m 的启动阶段；稳态（后 1200 帧）一次 “Slow frame” 都没有。
- 稳态的 GPU 上升是视角相关的：两次运行都在相机经过同样位置时（第 410 ~ 426、1140 ~ 1153 帧）平滑升到
  55 ~ 64 ms 再回落，不是单帧尖峰。

## 遗留

- 发光三角形 NEE 只在 G-buffer 表面和第一个路径顶点做；更深的顶点、ReSTIR PT 都还没有。
- 发光三角形不进世界网格：功率树是全局的，霓虹很多、彼此很远的场景需要 ReGIR 式的空间选择。
- 世界网格固定 8 m / 512 m 范围；网格外回到 cluster grid / 全部灯。
- RR 的 guides 在 R34 车漆上测不出改善；没有镜面 / 湿地面场景验证。
- 半分辨率层是最近邻上采样（每个 2 × 2 块取左上角的光），树叶边缘可能有块状，没有做深度感知的上采样。
- 启动阶段的流式卡顿（`CommitContent`、`RayInstances`）是之前就有的。
