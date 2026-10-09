# Direct3D 12 后端与 HDR 输出

## 背景（2026-10-09）

NVRHI 迁移（`2026-10-08-nvrhi-backend-design.md`）当时只编 Vulkan 后端。用户随后要求“继续 RHI 的移植，做到完整支持
DX12 和 HDR 输出；UI 里要有 Vulkan 和 DX12 的切换选项；DX12 也要完整的 path tracing 和光线重构（DLSS RR）”。
所以这一轮的范围是：

- 同一个渲染器（`VulkanRenderer`，名字沿用）在 NVRHI 的 D3D12 后端上跑通**全部**功能：光栅、延迟/前向、阴影、
  云/雾/大气、TAA、SSR、DDGI、硬件光追效果（RT 阴影、RTAO、RT 反射、探针遮蔽）、路径追踪、ReSTIR PT、
  DLSS 超分 / DLAA / 光线重构、Photo Mode、视频回读、ImGui 编辑器；
- HDR10 输出在 D3D12 上和 Vulkan 一样（PQ / Rec.2020）；
- Preferences 和 Render 菜单里切换图形 API（下次启动生效），`--backend vulkan|d3d12` 单次覆盖；
- Vulkan 结果不能变（以 main 为基准 A/B）。

## 结构

```
GpuDevice / GpuSwapchain (gpu_device.h)
 ├─ VulkanGpuDevice   (vulkan_gpu_device.cpp)  VulkanInstance/VulkanDevice + nvrhi::vulkan
 └─ D3D12GpuDevice    (d3d12_gpu_device.cpp)   DXGI + ID3D12Device5 + nvrhi::d3d12
VulkanRenderer 只拿 GpuDevice::Get() 的 nvrhi::IDevice 录制和建资源；
仍是 API 专有的少数几处（光追加速结构、NGX、网格缓冲的 heap 视图）各有两份实现。
```

- **设备**：高性能适配器上的 feature level 12_1 设备，Agility SDK 619（`app/main.cpp` 导出 `D3D12SDKVersion` /
  `D3D12SDKPath ".\\D3D12\\"`，构建时把 `D3D12Core.dll`、`d3d12SDKLayers.dll` 拷到 `app/D3D12/`）。NVRHI 描述符堆：
  SRV/UAV/CBV 1,000,000（流式地图的贴图 + 光追 bindless 表 + 网格缓冲视图），采样器 2048，RTV 4096。
  `MINIENGINE_NVRHI_VALIDATION=1` 同时开 NVRHI validation 和 D3D12 debug layer，debug layer 的消息走
  `ID3D12InfoQueue1::RegisterMessageCallback` 进引擎日志（过滤掉“清屏没有优化清除值”和“缓冲初始状态被忽略”两条建议）。
- **交换链**：DXGI flip-discard，3 张，frame-latency waitable（最大延迟 = 在飞帧数），`Present(0, 0)`。
  SDR 是 `B8G8R8A8_UNORM` + G22/P709；HDR10 是 `R10G10B10A2_UNORM` + `RGB_FULL_G2084_NONE_P2020`。
  后备缓冲在 NVRHI 里从 `Present` 状态开始跟踪（Vulkan 是 `Common`）。
- **选择后端**：`EngineGraphicsSettings::backend`（设置文件 `"graphics": {"backend": "vulkan" | "d3d12"}`）。
  `--backend` 优先；保存的后端在这台机器上不可用时退回 Vulkan 并记日志。render_ab 的 cur 变体总是显式带
  `--backend`，保存的选择不会影响截图对比。

## 着色器：同一份 Slang 出 DXIL

CMake 对每个 `.slang` 再编一份 DXIL（`slangc -target dxil -profile sm_6_8 -DMINIENGINE_DXIL`，PATH 指向 vcpkg 的
dxcompiler）。`CreateNvrhiShader` 在 D3D12 上把 `.spv` 换成 `.dxil`。要点：

1. **寄存器 = Vulkan 绑定**：每个声明同时写 `[[vk::binding(b, s)]]` 和 `register(<类>b, space s)`
   （`d3d_register.slang` 的 `D3D_REGISTER`），NVRHI 用 `registerSpaceIsDescriptorSet`、零偏移，两边一一对应；
   push constant 是该 layout 所在 space 的 `b0`。`tools/render_ab/build_all.py --dxil` 逐个核对。
2. **裁剪空间**：着色器按 Vulkan（y 向下）写。`shader_helpers.slang` 的 `ClipPosition()` 在 DXIL 下翻转 y，所有
   顶点着色器都经它输出；视口在 D3D12 上不再用负高度（`SetNativeViewportConvention`）。投影矩阵两边相同。
3. **阶段签名**：D3D12 按签名位置匹配 VS 输出和 PS 输入，所以每个像素着色器的输入结构与顶点着色器的输出
   结构字段顺序完全一致（多出的字段也要声明）。
4. **特化常量**：DXIL 没有。材质片段着色器（4 个常量）和 toon（1 个）按位掩码预编出全部排列
   `xxx_s<mask>.dxil`，`SpecializeShader` 在 D3D12 上加载对应排列。
5. **结构化缓冲**：D3D12 的视图要元素大小，帧集合等缓冲改成 `StructuredBuffer_SRV/UAV` 并在建缓冲时给 `structStride`。
6. **导数**：HLSL 的 `ddx/ddy` 在 DXIL 里是 coarse（2×2 一个值），NVIDIA Vulkan 的 `OpDPdx` 是 fine。
   specular AA 的法线变化量改用 `ddx_fine/ddy_fine`（Vulkan 结果逐像素不变）。
7. **光追命中着色**：DXIL 没有指针。`ray_hit_common.slang` 的 DXIL 分支用 `RayMeshGeometry.x` 作为 shader-visible
   堆里一个原始（ByteAddressBuffer）视图的下标，经 `ResourceDescriptorHeap[]`（SM 6.6，`DescriptorHandle`）读顶点和索引。

## 资源状态

所有纹理、缓冲都 `keepInitialState`，静止状态由用途决定（采样的 → ShaderResource，只做存储的 → UnorderedAccess，
其余 RenderTarget / DepthWrite；网格缓冲是 ShaderResource|VertexBuffer|IndexBuffer|AccelStructBuildInput）。
NVRHI 自动插 barrier，原生 layout tracker 不再参与。CPU 可见缓冲不能有 UAV（D3D12 上传堆不允许）。
清屏统一走 `ClearTextureFloat/ClearBufferUInt/ClearDepth`，它们先切到正确状态。

## 硬件光追（`d3d12_ray_acceleration.cpp`）

`IRayAcceleration` 抽出 Vulkan 版本的接口，`D3D12RayAcceleration` 用原生 DXR 按同样的思路实现：

- **底层 AS**：从 64 MiB、处于 `RAYTRACING_ACCELERATION_STRUCTURE` 状态的大缓冲里按 256 字节对齐子分配
  （`BlockSuballocator`），超过块 1/4 的单独一个缓冲。流式地图有几万个 BLAS，不能每个一个 committed resource。
- 三角形顶点按叶子顺序写进上传堆（与 Vulkan 相同的 `BvhTriangle` 展开），一次 Prepare 的所有新网格共用一个上传缓冲；
  蒙皮 / 可变形网格直接用位置缓冲的 GPU 地址 + 叶子顺序的索引，`ALLOW_UPDATE`，每帧 `PERFORM_UPDATE` 重拟合。
- **压缩**：构建时 `EmitRaytracingAccelerationStructurePostbuildInfo(COMPACTED_SIZE)` → readback；该帧槽下次轮到时
  读回，节省超过 5% 的才 `CopyRaytracingAccelerationStructure(COMPACT)` 到新范围，旧范围随帧槽退休。
- **顶层**：每个帧槽一个 TLAS（实例上传缓冲 + 存储 + scratch），实例描述只改变化的；`D3D12_RAYTRACING_INSTANCE_DESC`
  与 Vulkan 实例结构同布局，掩码 / 标志 / custom index 规则不变。
- NVRHI 只拿到 TLAS 的 GPU 地址：overlay port 的 `d3d12-fixes.patch` 加了
  `nvrhi::d3d12::IDevice::createHandleForNativeAccelStruct(gpuVA, desc)`，绑定集里按地址建 SRV，不跟踪状态。
- **网格缓冲视图**（`d3d12_buffer_views.cpp`）：开了光追时每个网格的顶点 / 索引缓冲在 NVRHI 的 SRV 堆里各分一个原始视图
  （CPU 堆和 shader-visible 堆都写），下标放进 `RayMeshGeometry`；缓冲销毁时释放。
- NVRHI patch 还让每个根签名带 `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`，深度目标之外不加 `DENY_SHADER_RESOURCE`，
  状态跟踪对无 dataBuffer 的 AS 判空。端口版本 5，改 patch 后要 reconfigure 才会重编 NVRHI。

光追场景是否就绪原来检查 Vulkan 的 descriptor set，D3D12 上永远为空，导致 RT 效果、路径追踪全被关掉；
改为检查 NVRHI 的纹理表。

## DLSS / 光线重构

`VulkanDlss` 多了 D3D12 构造（`NVSDK_NGX_D3D12_Init_with_ProjectID`）。feature 用 NVRHI 自己开的 command list
创建（`NGX_D3D12_CREATE_DLSS_EXT` / `NGX_D3D12_CREATE_DLSSD_EXT`），执行后 `waitForIdle`；评估在帧的 command list 上，
资源是 NVRHI 纹理的 `ID3D12Resource`（`DlssImage::texture`）。评估前颜色、深度、运动矢量、引导图都切到 ShaderResource，
输出切到 UnorderedAccess；NGX 改了描述符堆和根签名，之后 `clearState()` 让 NVRHI 重新绑定（它会重新设置堆）。

## HDR 输出

渲染器只看 `GpuSwapchain::IsHdr()`：HDR10 时色调映射输出 PQ（`hdr_output.slang`），ImGui 用 `IMGUI_HDR10` 变体把
sRGB UI 转到 PQ。D3D12 交换链只在窗口所在显示器处于 HDR 模式时才建 HDR10（`IDXGIOutput6::GetDesc1` 的色彩空间是
G2084/P2020），否则给出警告并用 SDR，**不改用户的显示设置**。`MINIENGINE_FORCE_HDR10=1` 只用于对比测试：在 SDR 显示器上
也建 HDR10 交换链，配合 `MINIENGINE_CAPTURE_WINDOW` 抓取 PQ 码值（10 位按高 8 位写 PNG）。

## 编辑器

- Preferences → Graphics API：Backend 下拉（Vulkan / Direct3D 12），显示当前运行的 API，选的和运行的不同时提示重启。
- Render → Graphics API (Restart) → Vulkan / Direct3D 12，与 Preferences 是同一个值（`EditorCommandState::graphicsBackend`），
  随编辑器设置保存。

## 验证方法

- `tools/render_ab/ab.py`：Vulkan 当前分支对 main 的逐像素 A/B（43 个用例），保证 Vulkan 不变。
- `tools/render_ab/backend_ab.py`：每个用例在本分支上分别用 Vulkan 和 D3D12 渲染，统计差异像素比例、两边日志里的 validation 错误数
  （`MINIENGINE_NVRHI_VALIDATION=1`）。
- `tools/render_ab/backend_case.py <用例> <名字> '<渲染设置>'`：单个用例加覆盖设置（例如 G-buffer 调试视图）对比两边，出差异掩码；
  用来定位 RT 阴影 / AO / 反射 / GI 各自是否一致。
- `tools/render_ab/backend_hdr.py`：hdr_output 打开，两边抓窗口（PQ），比较。

## 已知差异

- DDGI 的收敛依赖构建完成的帧，两边各自的多次运行之间就有差异（`road_ddgi_view` 在 Vulkan 自身的重复运行之间也不同），
  不是后端问题。
- ReSTIR PT 的噪声样式不同（帧平均亮度一致到 0.1%），D3D12 自己多次运行之间也有少量差异。
- `transmission_rt` 远处近乎镜面的地面：specular AA 对插值法线的浮点噪声求导，Vulkan 表现为抖动点、D3D12 表现为同心圆纹；
  两边都是同一个精度伪影（关掉 specular AA 后两边一致）。
