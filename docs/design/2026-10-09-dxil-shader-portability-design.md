# 着色器可编 DXIL（为 D3D12 后端做的第一步）

## 背景（2026-10-09）

用户问"RHI 是否完整、能否开始 DX12 迁移"。结论是还不能：`engine/renderer/rhi/` 只是整个渲染器的
`IRenderBackend` 接口，GPU 抽象靠 NVRHI，而 NVRHI 迁移（`2026-10-08-nvrhi-backend-design.md`）停在阶段 3
中途，每帧仍有原生 Vulkan 命令。D3D12 后端要等 NVRHI 主线走完。和 NVRHI 进度无关、现在就能做的是
**着色器的可移植性**，用户选了先做这一步："着色器改到能编 DXIL"。

范围：运行时仍只有 Vulkan（NVRHI 设计文档"不做 D3D12"的决定在运行时层面不变）；着色器源码改成同一份
既编 SPIR-V 也编 DXIL，Vulkan 的画面不变；DXIL 用工具检查，D3D12 后端出来之前跑不了。

起点：71 个着色器输出（`build_all.py` 当时漏了两个 float32 变体，见下）里 29 个能编 DXIL。

## 检查工具：`tools/render_ab/build_all.py --dxil`

- 和 CMake 同一份输出清单，编到 `out/slang/dxil`：`-target dxil -profile sm_6_8`，定义 `MINIENGINE_DXIL`；
  有 specialization constant 的着色器另编一个全开的变体（`*_specialized`）。
- 寄存器检查：每个着色器两种目标各编一次带 `-reflection-json`，要求每个资源的 D3D 寄存器号和 space 等于
  它的 Vulkan binding 和 set，push constant 在 `b0`，且没有两个参数落在同一个寄存器上；另外扫源码，每个
  `[[vk::binding]]` / `[[vk::push_constant]]` 声明都要带 `register(...)` 或 `D3D_REGISTER(...)`。
- 默认用 CMake 用的那个 slangc（vcpkg 的 shader-slang，2026.5，`MINIENGINE_SLANGC_EXECUTABLE`），没有时用
  Vulkan SDK 的（2026.13.1）；两个版本都要过。DXIL 需要 PATH 上的 `dxcompiler.dll`（SDK 的 Bin 有）。
- 顺手修了 CMake 列表的解析：注释里的 `)` 把 `MINIENGINE_SHADER_DEFINE_VARIANTS` 截断了，
  `path_trace_*_float32` 两个变体以前不在清单里（现在 SPIR-V 73 个，DXIL 80 个）。

## 失败原因和做法

| 原因 | 着色器 | 做法 | SPIR-V |
|---|---|---|---|
| 阶段输入输出没有 HLSL 语义（只有 `vk::location`） | 全部 vert/frag | 顶点属性按名字（`POSITION`、`TEXCOORD`、`SECOND_TEXCOORD`…，不带数字结尾，NVRHI 的 D3D 输入布局按名字配），varying 用 `TEXCOORD<location>`，ImGui HDR10 用 ImGui DX12 顶点着色器写的 `COLOR0`/`TEXCOORD0` | 逐字节不变 |
| `SV_VulkanVertexID` / `SV_VulkanInstanceID` | fullscreen、sky、triangle、toon 的 vert | `draw_parameters.slang`：Vulkan 的 VertexIndex/InstanceIndex 含首顶点/首实例（引擎用 firstInstance 当 draw slot），D3D 的 SV_VertexID/SV_InstanceID 不含，DXIL 版加 `SV_StartVertexLocation` / `SV_StartInstanceLocation`（SM 6.8）。用宏而不是带 `Index()` 方法的结构体：结构体会让 SPIR-V 指令重排 | 逐字节不变 |
| specialization constant 在 HLSL 里变成运行时 cbuffer | gbuffer、triangle.frag、toon 三个 | `specialization.slang` 的 `SPECIALIZATION_BOOL(id, name)`：Vulkan 展开成原来的 `[[vk::constant_id]]`，DXIL 是 `static const`，变体用 `-DSPECIALIZATION_<id>=true` 编 | 逐字节不变 |
| `spirv_asm`（Inverse、Pack/Unpack*）——Slang 生成 HLSL 时内部报错 "unexpected IR opcode" | gbuffer、emissive_lights、ReSTIR 等 | `__target_switch`：SPIR-V 仍是 GLSL.std.450 指令，其它目标按 GLSL 规范的定义写（伴随矩阵求逆；round 到最近、第一个分量在低位） | 逐字节不变 |
| 光追命中读顶点/索引缓冲用指针（BDA），DXIL 没有指针 | 所有 ray query 着色器 | DXIL 分支：`RayMeshGeometry` 的 `.x` 是该缓冲 raw SRV 在 shader-visible 描述符堆里的下标，`RayHeapBuffer` 用 Slang 的 `DescriptorHandle<ByteAddressBuffer>`（降成 `ResourceDescriptorHeap[...]`，非一致下标；vcpkg 的 Slang 2026.5 没有 `ResourceDescriptorHeap` 这个名字）。Vulkan 分支原样 | 逐字节不变 |
| 对 `Sampler2D` 调 `Load` / `GetDimensions`——Slang 把采样器塞进参数，生成无效 HLSL（Slang 的 bug） | AO、SSR、RT 反射/阴影、路径追踪、ReSTIR、DDGI 调试视图、描边、卡通、光追材质平均 | **拆采样器**，见下 | 21 个变了 |
| Slang 在 D3D 上不看 `vk::binding`，按声明顺序给寄存器、全在 space 0 | 全部 | **显式 D3D 寄存器**，见下 | 逐字节不变 |

### 拆采样器（C++ 跟着改）

combined image sampler 全部去掉（引擎里不再有 `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`，ImGui 自己的除外）：

- 只 `Load` / 取尺寸的输入改成 `Texture2D`，描述符 `SAMPLED_IMAGE`，不要采样器（描边、卡通两个 pass 原来
  只为 combined 才建的采样器删掉了，AO trace 的也是）；
- 真采样的输入拆成 `Texture2D` + `SamplerState`，采样器在 binding + 64（`kSplitSamplerBindingOffset`，和帧集、
  材质集、NVRHI pass 的约定一样），用原来那个 VkSampler：AO resolve 和 SSR resolve 的历史（线性）、SSR trace
  的深度（最近点）和 TAA 历史（线性）、RT 阴影历史（线性）、DDGI 调试视图的深度/法线/HDR（最近点）、
  路径追踪的多重散射 LUT、光追材质平均的两张贴图（各自的材质采样器）；
- `CreateComputeSetLayout` 多了带采样器 binding 列表的重载，`CreateImageDescriptorPool` 改成
  (sampled, storage, samplers) 三种计数；
- 着色器里删掉了只为 combined 存在的 `TextureSize(Sampler*)`、`TextureLevels(Sampler*)` 和
  `CombinedAtmosphereLut`。

### D3D 寄存器约定

`d3d_register.slang`：**Vulkan 的 binding b、set s ⇔ D3D 寄存器 `<class>b`、`space s`**，class 按资源类型
（t 纹理/只读缓冲/加速结构，u 读写，s 采样器，b 常量缓冲）。这正是 NVRHI 的 binding layout 在 D3D12 上给出的
（slot 即寄存器号，`registerSpace` 即 space；引擎的 binding offset 全是 0、`registerSpaceIsDescriptorSet`），
所以以后同一份 NVRHI layout 两个后端都对得上。410 个资源声明都加了 `: register(t3, space1)`；set 是宏的
（`PATH_TRACE_SET`、`RAY_SCENE_SET`、`EMISSIVE_LIGHT_SET`…，会随包含它的着色器变）用
`D3D_REGISTER(t, 3, PATH_TRACE_SET)` 拼出来；`EmissiveBuffer` 随 `EMISSIVE_LIGHTS_WRITE` 在 SRV/UAV 间变，配了
`EMISSIVE_BUFFER_CLASS`。

push constant：NVRHI 的 D3D12 后端把 `PushConstants(slot)` 放成 root constants，寄存器是 `b<slot>`、space 是
携带它的那个 layout 的 `registerSpace`。已迁到 NVRHI 的 pass 都是 `PushConstants(0)` 放在 pass 自己的 layout
里，所以约定是 **b0、pass 自己那个 set 的 space**，写成 `ConstantBuffer<T> name D3D_PUSH_CONSTANTS(1);`。
这个宏只在 DXIL 构建里展开成 `register(b0, space1)`：编 SPIR-V 时 Slang 把 push constant 上的 register 当成
隐式 binding，报"和 set 的 binding 0 重叠"的警告（45 个着色器），所以 Vulkan 下展开为空。

| space | 着色器 |
|---|---|
| 0 | exposure_histogram、tonemap、skin、tyre_deform（NVRHI 已是如此）；selection_outline、shadow.*、ray_material_average |
| 1 | bloom、environment_prefilter、TAA/DLSS 两个、cloud（大气）、transmission_copy、GI（NVRHI 已是如此）；vbao、SSR；triangle.vert（材质集）；sky（只用帧集，帧集 space 0 有相机 cbuffer b0，所以放 1，一个只装 push constant 的 layout） |
| 2 | DDGI 三个、ReSTIR、rt_reflection_trace、rt_occlusion（`AO_PASS_SET`）、toon（卡通材质集）、emissive_lights / light_grid（`EMISSIVE_LIGHT_SET`） |
| 宏 | path_trace（`PATH_TRACE_SET`：filter/temporal 1，trace 2）、rt_shadow（`RT_SHADOW_SET`） |
| 4 | deferred_lighting：没有自己的 set，光栅版用 0、2，ray query 版用 0–3，取两者都不用的 4 |

还没迁 NVRHI 的 pass 到时 layout 怎么分，可以改这里的 space；寄存器检查会跟着把关。

## 验证

- SPIR-V：除拆采样器的 21 个以外逐字节不变（第一批提交前后、加寄存器前后各比一次，两个 Slang 版本都比了）；
  当前源码用 vcpkg 的 slangc 编出来的 SPIR-V 和 CMake 构建里的完全相同（triangle.vert、toon.vert 两个有
  invariant 后处理，本来就不同）。
- DXIL：80/80（vcpkg Slang 2026.5 和 SDK Slang 2026.13.1 都是），寄存器检查无报告。
- 拆采样器的画面：A/B（`AB_MODE=exe`，基线是第一批提交编的 Debug exe），结果见下。

| 情况 | 结果 |
|---|---|
| 逐像素相同（有一对 A、B 截图完全一样） | road_rt、road_pt、road_ddgi_ray（DDGI 调试视图 14）、toon、transmission_rt、fixture_cornell_rt、fixture_cornell_ddgi、materials_pt、fixture_track_pt |
| 在噪声底内（B 和 A 的差不超过 A 对 A） | road_raster（max 12，底 37）、road_restir（1，底 13）、materials_rt（1，底 2）、emissive_restir（1，底 1）、road_software（1，底 1）、road_ddgi_view（DDGI 视图 15，不重置，底 mean 3.2） |

第一轮里 fixture_cornell_ddgi 两个 B 一致地和 A 不同（mean 3.1），materials_pt、fixture_track_pt 有一两千个像素
差几级。查下来：只把 `ray_material_average` 和它的描述符集换回 combined 的混合版本和基线逐像素相同；但在新、
混合两个版本里把 GPU 算出的光追材质整表打到日志里，逐位相同；去掉调试代码、用最终状态重编后再跑这三个，
全部逐像素相同（`AB_B_RUNS=3`）。第一轮那次差异没能复现，原因不明（那一轮中途在同一个构建目录里重编过一次
全部目标，可能有关）。路径追踪两个 case 的新版本自己两次运行之间也有差（1255 像素），基线在 road_pt 上同样
如此，说明这类小差是运行间的抖动。

描边（没有 A/B case）：`--model` 载入即选中，Debug（Vulkan validation 开着）跑 60 帧，SelectionOutline pass
执行、无 validation 报告。所有 A/B 运行的日志里也没有 validation 报告。

## DX12 后端要接住的约定

- 着色器模型 6.8（`SV_StartVertexLocation` / `SV_StartInstanceLocation`），要 Agility SDK；
- 寄存器 = Vulkan binding，space = set；push constants b0 + 上表的 space（NVRHI layout 按 set 建即可）；
- specialization constant 变成编译变体（`-DSPECIALIZATION_<id>=true`），NVRHI 的 `createShaderSpecialization`
  只有 Vulkan 有；
- 光追：`RayMeshGeometry` 的 `.x` 写顶点/索引缓冲 raw SRV 在 shader-visible 堆里的下标，root signature 要
  `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED`；纹理表（`rayTextures[]` 无界数组，t1 起）占 RAY_TEXTURE_SET 的 t 区间；
- `[[vk::image_format]]` 在 D3D 上不生效，读写 UAV 的格式要设备支持 typed UAV load；
- 跑不了：没有 D3D12 后端之前，DXIL 只经过了 dxc 的编译和验证。

## 下一步

第 2 步是 NVRHI 主线（材质集补丁 → 材质 pass → `nvrhi::rt` → DDGI → 阶段 4/5），之后才是 D3D12 设备、
交换链、NGX D3D12 和 CMake 里的 DXIL 输出。
