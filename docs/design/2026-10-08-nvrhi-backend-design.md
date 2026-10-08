# 渲染后端迁移到 NVRHI（只用 Vulkan 后端）

## 背景（2026-10-08）

着色器迁到 Slang 之后（`2026-10-08-slang-shader-migration-design.md`），用户要求"NVRHI 也接上，能分发 Vulkan
就行，直接输出 SPIR-V 做跨平台"。范围因此定为：

- NVRHI 只编 Vulkan 后端（`NVRHI_WITH_DX11/DX12=OFF`），不做 D3D12；
- 着色器只出 SPIR-V（slangc），不出 DXIL，所以 Slang 的指针（`ray_hit_common.slang`）等 Vulkan 专有写法照用；
- 跨平台靠 Vulkan 本身（Linux 原生，macOS 以后走 MoltenVK），不靠多后端。

现状：`engine/renderer/vulkan/` 122 个文件约 3.5 万行，1073 处 `vk*()` 调用，手写 barrier、render pass、
描述符池/集、管线、内存池（`VulkanMemoryPool`）、secondary command buffer 并行录制、上传批次、光追 BLAS/TLAS、
DLSS（NGX Vulkan）、ImGui Vulkan backend、视频回读、GPU 计时。

## 依赖

vcpkg 有 `nvrhi` 端口（2026-02-26，提交 5410046），但项目的 builtin-baseline 里没有它，端口的 `supports` 也排除
macOS 和 x86，且 Windows 上默认连 D3D11/D3D12 一起编并依赖 `directx-headers`。所以放一份 overlay port
（`cmake/vcpkg-overlay-ports/nvrhi`，仓库已经这样处理 jolt、tinygltf）：同一提交，`NVRHI_WITH_DX11/DX12=OFF`，
去掉 `directx-headers`，`supports` 放开到所有有 Vulkan 的平台。NVRHI 自带 validation 层（`nvrhi::validation`），
Debug 下打开。

## 和 NVRHI 的模型差异（决定迁移顺序的几件事）

1. **没有 combined image sampler**。NVRHI 的绑定类型只有 `Texture_SRV` 和单独的 `Sampler`，所以着色器里每个
   `Sampler2D` 都要拆成 `Texture2D` + `SamplerState`，C++ 侧的描述符也跟着拆。这件事和 NVRHI 无关，可以先在
   现有原生 Vulkan 后端里做完、单独验证。
2. **绑定号**：NVRHI Vulkan 的 binding = slot + 按类型的偏移（`VulkanBindingOffsets`），偏移全设 0 后 slot
   就是着色器里的 `[[vk::binding(n, set)]]`；一个 binding layout 对应一个 descriptor set（`registerSpaceIsDescriptorSet`）。
   同一 set 内不同类型不能同号，所以采样器另占一段 binding（例如 64 起）。
3. **没有 secondary command buffer**：并行录制（`VulkanParallelRecorder`，cb640a3）改成多个 command list
   各自录制，按顺序一起 `executeCommandLists`；每个 list 自己开关 render pass（load op 用 LOAD）。
4. **自动状态跟踪**：NVRHI 按资源跟踪状态、自动插 barrier。过渡期原生 pass 和 NVRHI pass 混用时，在每个
   NVRHI pass 前用 `beginTrackingTextureState/BufferState` 告诉它资源的当前状态，结束后 `setTextureState` 回到原生代码
   期望的布局；全部迁完后删掉 `RecordTransitions` 这一层。
5. **内存**：NVRHI 默认每个资源一次 `vkAllocateMemory`，而流式地图有几万个缓冲、几千张贴图
   （`VulkanMemoryPool` 注释里的教训）。保留现在的块分配策略：64 MiB 的 `nvrhi::IHeap`，资源用 `isVirtual`
   创建后 `bindBufferMemory/bindTextureMemory` 到堆里的偏移。NVRHI 在设备启用 buffer device address 时给堆加
   device address 标志。
6. **Push constants** 最多 128 字节（`nvrhi::c_MaxPushConstantSize`）。阴影 pass 现在推 144 字节
   （`ShadowConstants`：矩阵加五个 vec4），迁这个 pass 时要挪一部分到缓冲里；其它都在 80 字节以内。
   specialization constant 用 `createShaderSpecialization`。
7. **设备要求**：NVRHI 用 synchronization2 的 barrier、dynamic rendering 和 timeline semaphore，所以设备必须是
   Vulkan 1.3 并开这三个特性（`VulkanDevice` 现在要求并启用它们）。
8. **Vulkan 头文件**：vcpkg 用动态 triplet 时 NVRHI 编成一个 DLL，`vulkan.hpp` 的 dispatcher 在 DLL 里、由
   `createDevice` 初始化，所以引擎用 SDK 的头、NVRHI 用 vcpkg 的头互不影响。NVRHI 导出的目标会给使用者加
   `VK_USE_PLATFORM_WIN32_KHR`（让 `vulkan.h` 带进 `<windows.h>` 和 min/max 宏），`find_package` 之后清掉。

## 分阶段

每一阶段都能编译运行，用同一套 A/B 截图（`out/slang/ab.py` 的做法：同场景、固定曝光、
`MINIENGINE_FIXED_FRAME_SECONDS`、关地面平面避免 z-fighting）对比上一阶段的 exe，加上 ctest 和 validation
零报错，再提交。

0. **接入**：overlay port；`nvrhi::vulkan::createDevice` 包住现有的 `VulkanInstance/VulkanDevice`（设备特性、扩展、
   NGX、HDR 交换链逻辑不动）；NVRHI 消息回调进引擎日志；每帧一个 NVRHI command list，原生录制代码通过
   `getNativeObject(VK_CommandBuffer)` 继续往里录；提交走 `executeCommandList`，acquire/present 信号量用
   `queueWaitForSemaphore/queueSignalSemaphore`，帧完成用 event query 代替帧 fence（`VulkanRetireQueue` 跟着改）。
1. **拆采样器**（仍是原生 Vulkan）：所有着色器改用分离的纹理和采样器；帧描述符集、材质集、各 pass 的集按新布局写。
2. **资源走 NVRHI**：`VulkanTexture`、`VulkanBuffer`、场景渲染目标、uniform buffer、内存池（堆）都由 NVRHI 创建，
   对外仍给原生句柄，录制仍是原生的。
3. **pass 逐个迁**：先 compute（bloom、曝光直方图、AO/GI、SSR、TAA、大气/云、DDGI、路径追踪……），再全屏图形 pass
   （光照、tonemap、天空、地面），再材质 pass（几何、阴影、前向、卡通、散射、描边，含并行录制），再光追加速结构
   （`nvrhi::rt`，含 compaction 和分帧构建）。
4. **外围**：ImGui 改用 NVRHI 渲染（含 HDR10 着色器）、交换链图像用 `createHandleForNativeTexture` 包装、DLSS 从
   NVRHI 取原生句柄、视频回读和截图用 staging texture、GPU 计时用 timer query。
5. **清理**：删掉 `VulkanCommandContext`、描述符池、管线/render pass 代码、手写 barrier；剩下的原生 Vulkan 只有
   实例/设备创建、交换链和 present、NGX、显存预算查询。

## 进度

### 阶段 0（2026-10-09）

- overlay port `cmake/vcpkg-overlay-ports/nvrhi`（Vulkan only，动态 triplet 下为 `nvrhi.dll`）；
- `NvrhiDevice`（`engine/renderer/vulkan/nvrhi_device.*`）包住 `VulkanInstance/VulkanDevice`，消息进引擎日志，
  `MINIENGINE_NVRHI_VALIDATION=1` 打开 NVRHI validation 层；
- `VulkanCommandContext` 改用一个 NVRHI command list：每帧 `open`，原生录制代码拿
  `getNativeObject(VK_CommandBuffer)` 往里录，`close` 后 `executeCommandList`；acquire/present 的二值信号量用
  `queueWaitForSemaphore/queueSignalSemaphore`，帧完成用每个帧槽一个 event query（`VulkanRetireQueue` 照旧按提交
  序号回收），present 后 `runGarbageCollection`；
- 设备要求 Vulkan 1.3 + timelineSemaphore、synchronization2、dynamicRendering。

验证：对比基线 exe（Slang 提交 89b5303 单独编的 Debug，`out/baseline_src`），A/B 截图在噪声底内；NVRHI validation
层开着跑无报错；ctest 121 项全过（两个长物理测试另跑）。

### 阶段 1 的做法（定了，未做）

着色器里每个 `Sampler2D x`（binding b）拆成 `Texture2D x`（binding b）和 `SamplerState xSampler`（binding
b + 64），纹理的 binding 号不变；采样写成 `x.SampleLevel(xSampler, uv, lod)`，函数参数成对传。Slang 不允许全局
`static` 变量装资源，所以没有"把两者包成一个结构"的捷径。`MaterialTexture` 宏用 `##Sampler` 拼名字。bindless 的
`rayTextures[]` 改成纹理数组加一个小的采样器表。
