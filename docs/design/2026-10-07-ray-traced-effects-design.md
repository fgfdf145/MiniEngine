# 硬件光追效果：阴影、AO、DDGI 遮挡、反射、局部光阴影、DLSS 光线重构

## 背景（2026-10-07）

`docs/design/2026-10-07-hardware-ray-tracing-design.md` 接入了 BLAS/TLAS 和 ray query，但只有 DDGI
的探针光线和两个调试视图在用。那份文档的“后续”列了：用硬件光线做新效果（太阳阴影、逐像素 DDGI
遮挡、反射、AO）、BLAS 压缩、BLAS 构建分摊到多帧。用户要求按顺序全部实现（路径追踪除外），并加上
DLSS 光线重构（Ray Reconstruction）。

## 总体原则

- **每个效果都替换现有的屏幕空间 / 阴影贴图版本，而不是叠加**；没有硬件光追（`--no-ray-query`、
  MoltenVK、Graphics Debug 里关掉 “Hardware ray tracing”）时自动回到原来的实现。
- 新效果只有 ray query 版本（`MINIENGINE_HARDWARE_RAY_SHADERS`，`--target-env=vulkan1.2 -DRAY_QUERY`），
  软件 BVH 遍历太慢，不做。
- 开关：`RenderDebugSettings::rayTracing`（`RayTracingSettings`），设置文件和状态文件里是
  `render_debug.ray_tracing.*`：`sun_shadows`、`local_shadows`、`reflections`、`ambient_occlusion`、
  `probe_occlusion`、`occlusion_rays`、`denoise`；再加顶层的 `dlss_ray_reconstruction`。默认全开。
  渲染器每帧把不能运行的效果清掉（没有硬件光线、光追场景未就绪、forward-only、Khronos 参考视图、
  DDGI 关闭时的 `probeOcclusion`），各 pass 只看 `ScenePassFrameContext::rayTracing`。

## 1. 实例掩码：动的物体也要投影

DDGI 把 30 帧内动过的实例排除在探针光线之外（否则开过的车会在探针里留下暗痕）。以前是
`kRayInstanceSkip`，所有光线都看不见它们，车就没有光追阴影。

- 新标志 `kRayInstanceDynamic`：探针光线跳过，逐像素的可见性光线照常命中。
- TLAS 实例掩码：静态 `kRayMaskStatic = 1`，动态 `kRayMaskDynamic = 2`，被跳过的仍是 0。
- shader：`TraceSceneRayMasked(..., rayMask, ...)`；`TraceSceneRay` 保持原义（`RAY_MASK_PROBE`）。
  效果用 `RAY_MASK_VISIBILITY`。软件遍历按同样的规则跳过。
- `RayInstance::data.w` 低 4 位是标志，高位是实例的网格下标（命中着色要用）。

## 2. 命中着色数据（`ray_hit_common.glsl`）

反射和带纹理的 alpha 测试需要在命中点拿到法线、UV、材质和纹理。

- **顶点**：有硬件光追时网格的顶点/索引缓冲多带 `STORAGE | SHADER_DEVICE_ADDRESS`
  （`VulkanBuffer(..., deviceAddressable)`），光追集 binding 6 是每个网格的两个地址，binding 7 是每个
  叶子三角形在网格索引表里的序号（`MeshBvh::sourceTriangles`，和 `meshTriangles` 同布局，每三角形
  4 字节）。shader 用 `GL_EXT_buffer_reference` 读 `Vertex`（17 个 float，C++ 里有 static_assert）。
  每个构建持有它用到的 `VulkanBuffer`，内容被替换前不会释放。
- **材质**：命中实例的 `data.z` 就是 draw slot，直接读 set 0 的材质（binding 12）和纹理变换
  （binding 17），这两个 binding 加上 compute 可见。
- **纹理表**：一个 descriptor indexing 的大数组（`VARIABLE_DESCRIPTOR_COUNT | PARTIALLY_BOUND`），
  每个 draw slot 4 项：base color、metallic、roughness、emissive。在 `SetContent` 里随材质槽写入；
  释放的槽改写成渲染器的 1×1 白纹理，所以仍在安装的旧内容即使命中已释放的槽也读到合法描述符。
  容量按槽数的 1.5 倍预留，超出时重新分配。设备要求 `runtimeDescriptorArray`、
  `shaderSampledImageArrayNonUniformIndexing`、`descriptorBindingPartiallyBound`、
  `descriptorBindingVariableDescriptorCount`（有 ray query 的 GPU 都有）。
- **纹理 LOD**：ray cone（Akenine-Möller 2019），三角形的 UV 面积 / 世界面积给出纹素密度。
- **带纹理的 alpha 测试**：定义 `RAY_TEXTURED_ALPHA` 的 shader 里，`AcceptHit` 对 Mask 材质读
  base color 的 alpha（约 256 像素宽的 mip）和 cutoff 比较；其它部分覆盖的材质仍用覆盖率哈希。
  树叶的阴影因此有叶子的形状。实测在 Liberty City 上几乎没有额外开销。

## 3. AO 与 DDGI 探针遮挡（`rt_occlusion.comp`）

在 AO trace pass 里，半分辨率，每像素 `occlusionRays` 条余弦分布光线：

- `t < AO 半径` 的命中算 AO（`ambientOcclusion` 开时替换 VBAO；关时 VBAO 先跑，这个 pass 读回它的 r）。
- **探针遮挡**（DDGI 设计文档“比探针更细的遮挡”）：按 `DdgiIrradianceAlong` 的淡入规则算出这个像素
  由粗级别回答的份额和那些级别的平均间距 L；穿过 AO 半径的光线里，再走到 L 仍没命中的比例就是
  探针遮挡，按粗级别份额混合。AO 和它分别统计近处和远处的遮挡，相乘不重复计算。
- `AoRaw`/`SceneAo` 改为 RGBA16F：r 是 AO，g 是探针遮挡；VBAO resolve 对两个通道一起做空间和时间滤波。
  光照里 `ddgiProbeOcclusion` 只乘 DDGI 的那部分（漫反射和镜面环境的探针项），不乘回退的天空。
- 调试视图 18 显示 g。

## 4. 太阳阴影（`VulkanRtShadowPass`）

几何 pass 之后、光照之前，三个 dispatch：

1. **trace**（`rt_shadow_trace.comp`，全分辨率）：每像素一条朝太阳圆盘内一点的光线（大小来自环境的
   太阳角直径），any-hit，带纹理 alpha，动的物体也挡光，最长 20 km。记录可见性和遮挡物距离。
2. **temporal**：按运动矢量重投影累积（最多 24 帧）；历史先夹到 3×3 原始可见性的范围里——邻域光线
   全一致（全亮或全暗）的地方历史也必须一致，所以移动的阴影（路上的车影）不留拖影，半影里不受影响。
3. **filter**：按半影宽度（遮挡物距离 × 太阳角直径 / 像素足迹）做 5×5 的深度、法线感知模糊，至少
   一个像素宽，接触阴影保持锐利；样本少时稍宽。已收敛且全亮/全暗的像素直接跳过。

输出 `SceneShadow`（r 可见性，g = 本帧运行过），G-buffer 输入集 binding 12。光照 pass 用 push
constant `debug.y` 打开，`EvaluateDirectionalShadow` 改读它（再乘云影）。forward 着色的透明物体仍
用级联阴影。调试视图 17 显示它。`denoise` 关闭（或 DLSS 光线重构运行）时输出原始光线。

## 5. 反射（`rt_reflection_trace.comp`）

SSR trace pass 的硬件变体，和 SSR 用同样的波瓣（`ssr_lobe.glsl`，车漆用清漆层）、同样的半分辨率和
`SsrRaw` 格式，所以 SSR resolve 和光照完全不变：

- 光线穿过场景（5 km）。未命中 → 天空（预滤波环境的 mip 0 + 场景环境光）。
- 命中点就是屏幕上那个位置可见的表面 → 直接取 TAA 历史（和 SSR 一样，带所有效果）。
- 否则在命中点着色：`RayHitSurfaceOf` + `RayHitShadingOf` 取材质，再调用完整的 `ShadeSurface`
  （DDGI 环境光、所有灯光、一条朝太阳的光追阴影光线）。命中点在视锥外时 `shadeAllLocalLights`
  遍历所有局部光（cluster 网格只覆盖视锥）。单面网格的背面视为黑。
- 只要开了 `reflections`（不依赖 SSR 的开关），但和 SSR 一样需要有效的 TAA 历史。

## 6. 局部光阴影（光照 pass 的 ray query 变体）

`deferred_lighting.frag` 编译第二份（`-DRAY_QUERY`），光照 pass 原本为空的 set 1 绑光追集，set 3
绑纹理表。`ApplyLocalShadow` 在 `tracedLocalShadows` 时对每个有阴影 tile 的局部光发一条光线，指向光源
球面（点光、聚光的 source radius）或矩形（面光）上随机一点，停在灯前 5 cm（和阴影图集的近平面相同，
灯罩不挡自己的光）。有尺寸的光源靠 TAA 平均出软阴影。

## 7. DLSS 光线重构

- `scripts/fetch-dlss-sdk.sh` 多取 `nvngx_dlssd.dll`（layout 3），CMake 拷贝所有 DLSS 运行库。
- `VulkanDlss` 检查 `SuperSamplingDenoising.Available`，`EnsureFeature(..., rayReconstruction)` 创建
  DLSS-D（`DLUnified`、粗糙度打包在法线 w、硬件深度，其余标志和 SR 相同）。
- TAA pass 在评估前写三张引导图（`dlss_rr_guides.comp`）：漫反射反照率、split-sum 镜面反照率
  （Karis 近似）、世界空间法线 + 粗糙度；评估时附带 world-to-view 和无抖动的 view-to-clip。
- 光线重构运行时，太阳阴影自己的滤波让位，直接给它原始光线。
- Graphics Debug 里 DLSS 下拉框下面的 “DLSS ray reconstruction”（默认开，只在 DLSS 运行时生效）。

## 8. BLAS 压缩与分帧构建

- BLAS 带 `ALLOW_COMPACTION` 构建；每个帧槽一个查询池，构建后写入压缩尺寸，下次轮到该帧槽（栅栏已过）
  时读出，缩小超过 5% 的就建一个压缩后大小的结构，`COMPACT` 拷贝，并立刻替换（原结构随这一帧退休）。
  地址变了，所有 TLAS 重写。
- 每帧最多构建 2M 三角形 / 1024 个结构；还没构建的网格在 TLAS 里是非活动实例，构建后下一帧加入。
  某次内容的最后一批构建完成时，DDGI 的几何纪元加一（探针重新看场景），和安装时一样。
- 每个命中都会读的小缓冲（实例、顶层节点、光线材质、网格地址、源三角形、TLAS 实例）在有完整
  resizable BAR 堆时放进可写的显存。

## 9. 流式加载时不中断（2026-10-07 修复）

以前每次 `SetContent`（流式加载进出一个格子）都把光追场景标成未就绪，直到新的层次结构构建装好：
这期间所有光追效果退回光栅版本，DDGI 整帧不运行。开车穿过 GTA 地图时内容几秒一变，阴影成片消失、
闪烁，间接光也跟着闪。现在已经装好的内容在新构建期间继续追踪：它的每个子网格按 draw slot + 网格
对应到新内容里的同一个绘制，跟着新的模型矩阵走；新内容里已经没有的（流出的格子）光线跳过；新流入
的格子在它的构建装好之前不投影（远处的格子先进来，影响很小）。

## 验证

- Debug（验证层开）：赛道场景（所有效果 + 局部光）、Liberty City 流式加载（分帧构建 + 压缩）均无
  验证错误。DLSS-D 只有 NGX 自己的 `nv.ngx.dlssd.resource` 首次使用布局消息，和 SR 一样。
  压缩最初有一个生命周期错误（内容释放后排队的压缩目标被销毁），由验证层发现并修复。
- 单元测试 109/109（pass 顺序测试已更新）。
- 截图对比（`out/rt/rt_capture.py`，隐藏窗口、关手柄后端）：光追阴影与级联阴影布局一致，树叶阴影
  有叶形并随遮挡距离变软；铬球反射出天空、道路、树和自己的阴影（SSR 几乎为空）；局部光不再从车底
  漏光；光线重构输出干净。

## 结果（RTX 4070 笔记本，Release，1280×720）

| 场景 | 光追阴影 | AO + 探针遮挡 | 反射 | 整帧 GPU |
|---|---|---|---|---|
| ddgi_track（5 千三角形） | 0.34 ms | 0.05 ms | 0.09 ms | 2.9 ms |
| Liberty City 300 m（7.7 千网格、138 万三角形） | 0.84 ms | 0.46 ms | 0.03 ms | 5.06 ms（全部光追关闭 6.06 ms，其中软件 DDGI 2.49 ms） |

- 阴影的开销几乎全在 trace（关掉滤波 0.86 ms，关掉纹理 alpha 也是 0.89 ms），约 11 亿条阴影光线/秒。
- BLAS 压缩：Liberty City 98.5 → 38.6 MiB（−61%）。
- 分帧：Liberty City 首次加载的 7252 个 BLAS 分 8 帧完成。

## 未做 / 后续

- 用户在玩游戏，没有跑 GTA 全图（约 4.5 GB 显存）的完整测量；Liberty City 子集代替。
- forward 着色的透明物体仍用级联阴影；局部光只给有阴影 tile 的灯追踪。
- 反射命中点不做 aerial perspective；没有多次反射。
- 光线重构没有提供镜面命中距离等可选输入。
- 蒙皮 / 变形网格的 BLAS 更新（目前只有刚体实例）。
