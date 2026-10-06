# 硬件光追基础：加速结构与 ray query

## 现状（2026-10-07）

引擎里所有“真正的”光线都走 `VulkanRayScene`（`engine/renderer/vulkan/ray_scene.h`）：每个网格一棵 CPU
构建的 BVH（`ray_tracing_bvh.h`，分箱 SAH，叶子 ≤ 4 个三角形），每帧在 CPU 上增量重建的顶层
（`IncrementalTopLevel`），全部放在 storage buffer 里，由 `ray_tracing_common.glsl` 的
`TraceSceneRay` 在 compute shader 里用一个栈手工遍历。用到它的只有 DDGI：探针光线
（`ddgi_trace.comp`）和两个调试视图（`ddgi_debug.comp`，视图 14 光追、15 探针辐照度）。

RTX 4070 笔记本 GPU 有 RT core，一直没用上。DDGI 设计文档里“比探针更细的遮挡”一节也写着要等
硬件 ray query。

## 目标

把硬件光追接进来，作为这条光线路径的后端，而不是另起一套：

1. 设备在支持时启用 `VK_KHR_acceleration_structure` + `VK_KHR_ray_query`（+ `bufferDeviceAddress`），
   不支持（MoltenVK、老显卡）时一切照旧。
2. 每个网格一个 BLAS，按网格缓存，跨内容复用；每个帧槽一个 TLAS。
3. `TraceSceneRay` 多一个 `RAY_QUERY` 变体，函数签名和返回的 `RayHit` 完全不变，所以命中之后的
   一切（材质、法线、覆盖率、阴影光线）不用改。
4. 一个运行时开关（Graphics Debug 的 “Hardware ray tracing”，`--software-rays`）在两条路径之间切换，
   用来对比：两条路径必须找到同样的命中。

不做（留给后续）：BLAS 压缩（compaction）、BLAS 更新/refit（蒙皮、变形）、ray tracing pipeline
（SBT、closest-hit shader）、用硬件光线做新效果（阴影、反射、AO、逐像素 DDGI 遮挡）。

## 设计

### 关键决定：TLAS 的实例就是 `RayInstance`，顺序也一样

软件路径命中后用两个下标查一切：`hit.instance`（`rayInstances[]` 的下标，叶子顺序）和
`hit.triangle`（全局三角形下标 = 实例的三角形偏移 + 网格内的叶子顺序下标）。硬件路径要给出同样的
两个数：

- TLAS 的第 i 个实例就是 `scene.instances[i]`，`instanceCustomIndex = i`（24 位，够 1600 万实例）。
- BLAS 的三角形就是网格 BVH 的三角形（`MeshBvh::triangles`，叶子顺序），每个三角形三个顶点
  `v0, v0+e1, v0+e2`，不带索引。于是 `primitiveIndex` 就是网格内的叶子顺序下标，
  `hit.triangle = rayInstances[i].data.y + primitiveIndex`。
- 重心坐标：ray query 给的是 v1、v2 的权重，和 Möller–Trumbore 的 (u, v) 同义。
- 正反面：不用 API 的朝向约定，直接按软件路径的规则从三角形算：
  `frontFace = dot(物体空间光线方向, cross(e1, e2)) < 0`。

被跳过的实例（Blend 材质、DDGI 认为在动的实例、增量顶层里被移走的旧叶子，都带
`kRayInstanceSkip`）在 TLAS 里是非活动实例：`accelerationStructureReference = 0`、`mask = 0`。

### 覆盖率（Mask、透射）

软件路径对每个候选命中调用 `AcceptHit`（材质覆盖率 < 1 时按 `RayHash(rayId, triangle)` 随机决定）。
硬件路径：

- 材质覆盖率恒为 1（Opaque 且无透射，CPU 侧从 `RayMaterialSource` 就能判断）→ 实例标
  `FORCE_OPAQUE`，硬件直接提交，不回到 shader。
- 其余 → `FORCE_NO_OPAQUE`，候选三角形回到 `rayQueryProceedEXT` 循环，用同一个 `AcceptHit`
  决定是否 `rayQueryConfirmIntersectionEXT`。几何标 `NO_DUPLICATE_ANY_HIT_INVOCATION`。

阴影光线（`anyHit`）用 `gl_RayFlagsTerminateOnFirstHitEXT`。

### BLAS：在后台线程上准备，在第一次安装时构建

`VulkanRayAcceleration::Prepare`（`ray_acceleration.h`）跑在光追场景原有的后台构建任务里，紧接在
网格 BVH 之后：

- 按网格查共享缓存（`MeshData*` → weak_ptr，带互斥锁）。还活着的 BLAS 直接复用；另一个尚未安装的
  构建刚做出来的也算，谁先安装谁构建。
- 其余网格的顶点写进**一个**主机可见的顶点批（一次分配），并在后台线程上完成
  `vkGetAccelerationStructureBuildSizesKHR`、存储 buffer、`vkCreateAccelerationStructureKHR`。
- 存储 buffer 从 `VulkanMemoryPool` 分配（新增 `AddressableBuffer` 类型，块带
  `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`）：GTA 地图有约 4.8 万个网格，每个 BLAS 单独
  `vkAllocateMemory` 会撞上 Windows 4096 个分配的上限。

`Install`（渲染线程，所有帧已空闲）把尚未构建的 BLAS 排进待建列表；下一次 `Record` 分批构建
（每批 scratch ≤ 256 MiB，批间加屏障复用同一块 scratch），顶点批和 scratch 在该帧槽下次轮到时
（栅栏之后）释放。旧内容独有的 BLAS 和旧 BVH 一样在后台任务里释放。

### TLAS：每帧槽一个，只在实例变化且本帧用硬件时构建

`UpdateTopLevel` 在 `UpdateInstances` 里调用：顶层代数（`m_topLevelGeneration`）变了就把实例写进该
帧槽的主机可见实例 buffer（`worldToObject` 求逆得到 3×4 变换，按 `RayMeshRange::nodeOffset` 查 BLAS
地址）。增量顶层把完整构建的实例留在原位，所以每个帧槽记住自己上次写入的 `RayInstance`，逐个比较，
只重写变了的（通常只有移动的那几个）。`Record` 在本帧用硬件时才真正 `vkCmdBuild`（PREFER_FAST_TRACE，
整棵重建）；关掉开关时 TLAS 保持脏标记，再打开时重建。

### 描述符与 shader 变体

- 光追集（set 1）在支持硬件时多一个 binding 5：该帧槽的 TLAS。两个变体共用同一个布局（软件变体
  不声明 binding 5，合法），所以切换不用重建管线布局。
- `ddgi_trace.comp`、`ddgi_debug.comp` 各编译两份：原样，以及 `-DRAY_QUERY --target-env=vulkan1.2`
  的 `*_ray_query.comp.spv`（`engine/renderer/CMakeLists.txt` 的 `MINIENGINE_RAY_QUERY_SHADERS`）。
  `GL_EXT_ray_query` 要 `#version 460`，两个 shader 由 450 改为 460；扩展声明必须在任何声明之前，
  所以写在 shader 开头而不是 include 文件里。
- `VulkanDdgi` / `VulkanDdgiDebugPass` 在支持时多建一条管线，每帧按
  `ScenePassFrameContext::hardwareRays` 选。

### 开关

| 开关 | 作用 |
|---|---|
| Graphics Debug → Hardware ray tracing（`hardware_ray_tracing`，默认开，存进设置/状态文件） | 运行时在硬件与 compute 遍历之间切换；BLAS 照建 |
| `--software-rays` | 以关闭该开关启动（脚本对比用） |
| `--no-ray-query` | 设备根本不启用硬件光追，等同于没有 RT 的 GPU（测回退路径、做基线） |

## 验证

见文末“结果”。方法：

1. **命中一致**：视图 14（每像素一条主光线 + 一条阴影光线）硬件/软件逐像素比较。
2. **确实走了硬件**：把硬件变体的光线截短到 2 m 重新编译，图像必须变。
3. **DDGI**：视图 15 比较探针辐照度（DDGI 调度依赖 GPU→CPU 反馈时序，不是逐位确定的，只比统计）。
4. **大场景**：GTA 地图（三张图，约 4.8 万网格、1170 万三角形）稳态帧时间、DDGI GPU 时间、BLAS 内存，
   硬件 / `--software-rays` / `--no-ray-query` 三组。

## 结果（2026-10-07，RTX 4070 笔记本，Release，1280×720）

**命中一致**（视图 14，硬件 vs `--software-rays`，同一个 exe）：

| 场景 | 不同的像素 | 说明 |
|---|---|---|
| cornell_box | 0 | 逐像素相同 |
| ddgi_track 近景（看台、树卡片、红车、阴影） | 0 | Mask 树卡片的随机覆盖率也逐像素相同 |
| ddgi_track 远景（150 m 外） | 0.14% | 全在远处轮廓和细物体上 |
| GTA 地图 | 0.43% | 轮廓边缘和一根 1–2 像素宽的杆子 |

差异都是一像素宽的轮廓边。来源有两个：Möller–Trumbore 和硬件的 watertight 测试对恰好落在边上的
光线结论不同；视图 14 的主光线带 TAA 抖动，两次运行截图时的帧号差一，相机就差一个亚像素（两次都用
compute 遍历的 Debug 运行之间也有 0.78% 这样的边缘差异）。命中本身不同的像素一个也没有找到。把硬件变体的光线截短到 2 m，cornell 图像 40% 的像素改变，证明运行的确实
是 ray query 管线。

**性能**（GTA 地图三张图全部加载后，4.8 万网格、1170 万三角形，最后 120 帧平均）：

| | 硬件 | compute 遍历 |
|---|---|---|
| 视图 14 全屏光追（每像素 1 条主光线 + 1 条阴影光线） | 0.64 ms | 5.12 ms |
| DDGI 探针（`Ddgi` 通道） | 0.48 ms | 1.51 ms |
| CPU `RayInstances`（稳态） | 2.75 ms | 2.66 ms |

- BLAS：共 1177 万三角形，776 MiB（未压缩，约 66 B/三角形）。
- 安装尖峰：`RayInstall` 最大 259 ms（硬件）/ 288 ms（compute），`--no-ray-query` 基线最大 234 ms，
  即安装本身的老开销；`RayInstallWait`（等 GPU 建完 BLAS）不在最慢之列。
- 第一版每次顶层变化都重写全部 4.8 万个实例（两辆待机的车每帧都在动），稳态 `RayInstances` 比基线多
  约 1.7 ms；改为逐个比较后与 compute 路径持平。
- 每通道 GPU 时间在 CPU 受限的帧里（13–19 ms）噪声很大，笔记本 GPU 降频，差异会落在 TAA、ImGui 这类
  无关通道上；比较光追只看视图 14 的 `DdgiDebug` 这种 GPU 受限的测量。

## 后续

（2026-10-07 已全部完成，见 `docs/design/2026-10-07-ray-traced-effects-design.md`；蒙皮 BLAS 更新除外。）

1. BLAS 压缩：预计 776 → 约 400 MiB；压缩后 BLAS 地址变化，所有帧槽的 TLAS 都要重建，旧存储要等用到
   它的帧都结束。
2. 用硬件光线做新东西：太阳阴影光线、逐像素 DDGI 遮挡（DDGI 设计文档“比探针更细的遮挡”）、
   光追反射/AO。
3. BLAS 构建分摊到多帧（首次加载地图时一帧建 1.5 万个 BLAS）。
4. 蒙皮/变形网格的 BLAS 更新（目前只有刚体实例）。
