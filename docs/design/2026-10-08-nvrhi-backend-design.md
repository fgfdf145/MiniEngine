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

### 顺序调整（2026-10-09）

NVRHI 的管线要求每个 set 都是 NVRHI 的 binding layout，而几乎所有 pass 都绑帧描述符集（set 0）；引擎里纹理的
"通货"又是 `TextureDescriptorBinding {VkImageView, VkSampler}`，NVRHI 的 binding set 要的是 `ITexture` 和
`ISampler`。所以在拆采样器之前先做 **阶段 A：资源归 NVRHI 所有**——纹理、缓冲、采样器都由 NVRHI 创建，还没迁的
代码拿它们的原生句柄（`getNativeObject` / `getNativeView`）继续用；然后共享描述符集（帧、材质、G-buffer、光追）
改成 NVRHI binding set（原生 pass 用它的原生 `VkDescriptorSet`/layout），同时给读这些集的着色器拆采样器；再逐个
pass 迁移（pass 自己的集在迁它时拆采样器）。这样每个 pass 的描述符代码只改一次。

阶段 A 分四步：A1 采样器，A2 网格缓冲 + 材质纹理 + 内存池放到 NVRHI 堆上，A3 渲染目标和各 pass 的图像，
A4 其余缓冲（uniform、大气、DDGI、光追、回读……）。

**A1 完成（2026-10-09）**：
- NVRHI 打了第二个补丁 `sampler-lod-and-comparison.patch`：`SamplerDesc` 加 `comparisonFunc`、`minLod`、`maxLod`
  （上游比较固定为 LESS、LOD 不能限，阴影要 LESS_OR_EQUAL，glTF 不带 mip 的过滤要 maxLod 0.25）；
- 材质采样器缓存（`VulkanSamplerCache`）改成 NVRHI 采样器，`BuildTextureSamplerDesc` 代替
  `BuildTextureSamplerInfo`；原生描述符写入用 `GetNative`；
- 各 pass 自己的采样器全部由 NVRHI 创建：`CreateClampSampler(nvrhi::IDevice*, VkFilter)` 用
  `BuildClampSamplerDesc`（clamp、只读基础层 maxLod 0——原来 `VkSamplerCreateInfo{}` 清零的 maxLod 就是 0），
  atmosphere（云噪声 repeat）、environment_probe（三线性、maxLod 9）、shadow / local_shadow（比较
  LESS_OR_EQUAL，级联阴影 clamp-to-border 白边）、transmission（三线性、maxLod 11）逐项照原来的参数写成
  `nvrhi::SamplerDesc`；pass 构造函数多一个 `nvrhi::IDevice*`，成员是 `nvrhi::SamplerHandle`，原生描述符写入用
  `NativeSampler()`（`nvrhi_native.h`，`ToNative<VkHandle>` 兼顾 32 位目标上非 dispatchable 句柄是整数）。
  引擎里不再有 `vkCreateSampler`。`texture_sampler` 测试加了 `BuildClampSamplerDesc` 的用例。

验证（Linux，见下一节）：A/B 对比改动前的提交，9 个场景逐像素相同。

**A2 完成（2026-10-09）**：网格缓冲、材质纹理和内存池。
- NVRHI 第三个补丁 `heap-memory-type-and-native.patch`：`HeapDesc::memoryTypeBits`（上游的堆取第一个满足属性的
  内存类型，不看资源的 `memoryTypeBits`），`IHeap::getNativeObject(VK_DeviceMemory)`（还原生绑定的加速结构要用）；
- `VulkanMemoryPool` 的 64 MiB 块和 16 MiB 以上的独占分配都是 NVRHI 堆，内存类型照旧由引擎按资源的
  requirements 选（`memoryTypeBits = 1 << index`），堆在设备有 buffer device address 时带 device address 标志
  （NVRHI 给它建的每个缓冲都加 `SHADER_DEVICE_ADDRESS`，所以普通缓冲也必须绑在这样的内存上）。
  `VulkanPooledMemory` 多了 `heap`（引用计数），NVRHI 资源 `bindBufferMemory/bindTextureMemory(heap, offset)`，
  原生资源仍 `vkBind*Memory(memory, offset)`。`NvrhiDevice` 注册/注销池（注销时释放所有块；还在用的块连同块对象
  一起泄漏并记日志，避免之后的 `Free` 访问悬空指针）；`VulkanDevice` 不再管池。
- `VulkanBuffer` 的六个设备缓冲（顶点、索引、位置流、绑定姿势、蒙皮、上一帧位置）是 NVRHI 的 virtual 缓冲，
  绑到池里；device address 取 `getGpuVirtualAddress()`。没用到的立方体构造函数删了。
- `VulkanTexture` 的图像是 NVRHI 的 virtual 纹理（`initialState = ShaderResource, keepInitialState`：上传完是
  `SHADER_READ_ONLY_OPTIMAL`），绑到池里；上传、mip 生成、视图仍是原生的。

验证：A/B 对比 A1 之前的基线（A1 与它逐像素相同），9 个场景逐像素相同；validation 无报告，退出时池里没有残留的块。

**A3 完成（2026-10-09）**：渲染目标和各 pass 的图像。
- `nvrhi_resources.{h,cpp}` 的 `CreateNvrhiImage`：把原生代码本来就写好的 `VkImageCreateInfo` 原样说成
  `nvrhi::TextureDesc`（格式用 `ToNvrhiFormat`：反查 `nvrhi::vulkan::convertFormat`；1D/2D/3D/立方体/数组、层数、
  mip、usage→`isShaderResource/isUAV/isRenderTarget`、`MUTABLE_FORMAT`→`isTypeless`），NVRHI 照旧给每张图独立的
  device-local 内存（原来也是每张一次 `vkAllocateMemory`）；NVRHI 说不出的参数（别的 tiling、sharing、create flag、
  usage）直接抛异常而不是悄悄造一张不同的图。NVRHI 给每张图都加两个 transfer usage。
- `SceneRenderTargets`、`HistoryImagePair`（AO/GI/SSR/TAA/光追阴影/路径追踪的历史）、大气 LUT/云噪声/云阴影/云目标、
  bloom 链、DDGI 图集、环境探针立方体、级联阴影和局部阴影图集、路径追踪层、散射预通道、TAA 的 DLSS 引导图和运动
  矢量、透射拷贝：成员从 `VkDeviceMemory` 换成 `nvrhi::TextureHandle`，视图和所有 barrier 仍是原生的。引擎里
  只剩 `VulkanTexture`（A2，virtual + 池）这一处图像不是这样建的。

验证：A/B 对比 A1 之前的基线，在 A4 一起跑了全部 9 个场景（见 A4）；单独的 A3 编译通过，两个场景逐像素相同。

**A4 完成（2026-10-09）**：其余缓冲。
- `CreateNvrhiBuffer`：同样从 `VkBufferCreateInfo` 加内存属性说成 `nvrhi::BufferDesc`：device local → `cpuAccess
  None`；host visible + coherent → `Write`（NVRHI 取第一个 host-visible 类型，桌面驱动上都是 coherent，NVRHI 自己的
  上传也这样假设）；host cached → `Read`。要映射的直接 `mapBuffer` 常驻映射，释放时随内存一起解除。
- 帧描述符集的缓冲（相机 UBO、上一帧模型矩阵、光源、cluster、局部阴影 tile、材质、纹理变换）、大气（SH、羽流表和
  它的 staging、回读）、DDGI、路径追踪的发光三角形表、ReSTIR PT、曝光直方图、蒙皮调色板、卡通材质。
- 还是原生的（各有去处）：光追场景和加速结构的缓冲（ReBAR 内存类型的选择 NVRHI 说不出；阶段 3 随 `nvrhi::rt`
  一起换）、上传批次的 staging 分块和回退 staging（阶段 3/4 换 NVRHI 的上传）、视频回读和截图（阶段 4 用
  staging texture）。

验证（A3 + A4）：A/B 对比 A1 之前的基线，9 个场景逐像素相同；validation 无报告，退出时池里没有残留的块。

### Linux 上的验证（2026-10-09）

云端会话是 Linux、没有 GPU，所以 `linux-debug` 修到能编能跑（`fix(build)` 提交：GCC 的几处兼容、静态 NVRHI 的
vulkan.hpp 调度器、vcpkg 的 SDL3/Vulkan loader 特性），用 Mesa 的 lavapipe（CPU 上的 Vulkan 1.4，有 ray query
和加速结构）在 Xvfb 里跑。lavapipe 的结果是确定的（同一 exe 跑两次逐像素相同），所以 A/B 的噪声底是 0，
任何差异都是真的。很慢（320x180 一帧几秒到二十秒），所以用 `AB_SIZE=320x180 AB_FRAMES=6`，场景是不依赖仓库外资产的
`fixture_*`（`tests/fixtures/render_scenes`，`scripts/install-render-scenes.sh` 装到 `AB_ASSETS`）和 materials：

```bash
AB_PRESET=linux-debug AB_MODE=exe AB_SIZE=320x180 AB_FRAMES=6 AB_ASSETS=<装了 fixtures 的目录> \
  DISPLAY=:99 python3 tools/render_ab/ab.py fixture_spheres_sun fixture_track_rt ...
```

基线在 `out/baseline_src`（改动前提交的 worktree，同样打上 Linux 构建修复，`-DVCPKG_MANIFEST_INSTALL=OFF` 指向主
checkout 的 vcpkg_installed）。这不能代替 Windows + NVIDIA 上的 A/B（DLSS、真实 GPU 的格式/内存类型），那一部分留给本机验收。

### 拆采样器的做法

着色器里每个 `Sampler2D x`（binding b）拆成 `Texture2D x`（binding b）和 `SamplerState xSampler`（binding
b + 64，`FRAME_SAMPLER(b)` / `kFrameSamplerBindingOffset`），纹理的 binding 号不变；采样写成
`x.SampleLevel(xSampler, uv, lod)`。Slang 不允许全局 `static`/`static const` 变量装资源，所以没有"把两者包成一个
全局结构"的捷径（试过）。既被拆开的集又被还没拆的集调用的函数（大气的 `SampleTransmittance`、
`SampleMultipleScattering`、`IntegrateScatteredLuminance`：帧集的透射 LUT 拆了，大气 pass 自己的集还没拆）改成
泛型，参数是 `IAtmosphereLut`，两个实现 `CombinedAtmosphereLut`（`Sampler2D`）和 `SeparateAtmosphereLut`
（纹理 + 采样器），Slang 全部内联，生成的 SPIR-V 合法。只 `Load` 的纹理（路径追踪层、云目标）不声明采样器。
`MaterialTexture` 宏到材质集时用 `##Sampler` 拼名字。bindless 的 `rayTextures[]` 改成纹理数组加一个小的采样器表。

**B1 完成（2026-10-09）**：帧描述符集（set 0）的 23 个 combined image sampler 拆成 SAMPLED_IMAGE（b）和
SAMPLER（b + 64），仍是原生 Vulkan：布局按同样的规则展开，`UpdateFrameDescriptorSets` 把每个 combined 写拆成两个
写（同一个 `VkDescriptorImageInfo`：Vulkan 对 SAMPLED_IMAGE 忽略 sampler、对 SAMPLER 忽略 view），描述符池
按拆后的类型分配。着色器：`atmosphere_sampling`、`pbr_common`（级联阴影和局部阴影图集用
`SamplerComparisonState` 和 `SampleCmpLevelZero`）、`ddgi_common`、`volumetric_clouds`、`cloud_shadow`、
`triangle.frag`、`gbuffer.frag`、`sky.frag` 和各处 `prefilteredEnvironment` 的采样；`TextureSize` 补了
`Texture2D/Texture2DArray` 的重载。

验证：71 个 SPIR-V 全部 `spirv-val` 通过，用帧集的着色器在 set 0 里不再有 combined image sampler；A/B 对比 A1
之前的基线，9 个场景逐像素相同，validation 无报告；ctest 与之前相同（120/123，三个已知失败与此无关）。

**B2a 完成（2026-10-09）**：帧集里的图像平时都在 `SHADER_READ_ONLY_OPTIMAL`。NVRHI 的 binding set 把 SRV 一律
写成这个 layout（`vulkan-resource-bindings.cpp`），而原来有 11 张帧集图像一直放在 `GENERAL`：大气的透射/天空视图
LUT、空气透视体积、云噪声三张、云阴影、解析后的云，环境探针的预滤波立方体，DDGI 的两个图集。现在它们只在写的
那段是 `GENERAL`（`BeginFrameImageWrites` / `EndFrameImageWrites`，`compute_pass_util`），写完回到
`SHADER_READ_ONLY_OPTIMAL`；大气 pass 自己的集里透射 LUT 的采样描述符跟着改。只在本 pass 内部用的图像
（多次散射 LUT、云的 march 目标和历史、探针的辐射立方体）照旧是 `GENERAL`。
- DDGI 的更新在写第 L 层的同时采样第 L+1 层（新探针从更粗一层起步），而帧集的视图包含所有层、要求全是
  `SHADER_READ_ONLY_OPTIMAL`，所以更新改从 DDGI 自己的集（binding 6 到 9，`GENERAL`）读图集
  （`DDGI_UPDATE_ATLASES`）。
- DDGI 的两个 layout 转换并进已有的那两个 `vkCmdPipelineBarrier`。分开记成两个独立的 barrier 时，Cornell DDGI
  场景变了一点（均值 0.157，最大 31，3.4% 的像素 > 2）。当时以为是某处读了未初始化的内存；后来 B3c 一个与 DDGI
  无关的改动给出一模一样的另一张图，对比日志才看清是 A/B 本身的竞争：场景加载完后新网格的光追场景在工作线程上
  建，渲染线程哪一帧装上它不固定，`--wait-for-scene` 开始数帧之前的预热帧数因此差一帧（日志里 10 帧对 11 帧），
  DDGI 会跨帧累积，所以结果有两个稳定的取值。别的场景不累积，不受影响。这是原有的不确定性，不是这次的改动造成的；
  要彻底消除，得让等待以渲染线程装好光追场景的那一帧为准（记为后续）。
- `CreateNvrhiImage/Buffer` 的 debug name 原来就是失败信息本身（"Failed to create a DDGI atlas"），改成去掉
  "Failed to create (the|a|an)" 的部分。

验证：A/B 对比 A1 之前的基线，9 个场景逐像素相同，validation 无报告；ctest 与之前相同。

**B2b 完成（2026-10-09）**：帧描述符集是 NVRHI 的了。
- `VulkanFrameDescriptorSetLayout` 用 `nvrhi::BindingLayoutDesc` 建：visibility 是 vertex + pixel + compute
  （原来各 binding 的 stage 并集），`VulkanBindingOffsets` 全清零（NVRHI 默认把 sampler 挪到 128、CB 到 256、
  UAV 到 384），所以 slot 就是 binding；相机块是 `ConstantBuffer`，着色器的 `StructuredBuffer` 用
  `RawBuffer_SRV`（Vulkan 上都是 storage buffer；`StructuredBuffer_SRV` 要求 `structStride`，这些缓冲没有），纹理
  `Texture_SRV` 加 b + 64 的 `Sampler`。原生管线布局照旧拿 `GetHandle()`（NVRHI 的 `VkDescriptorSetLayout`）。
- `VulkanUniformBuffer` 每个交换链图像一个 `nvrhi::BindingSetHandle`，原生 pass 绑的是它的
  `VK_DescriptorSet`；自己的描述符池没了。NVRHI 的 binding set 不能改写，所以 `SetEnvironmentMap` 等四个
  setter 改成整组重建（调用方本来就先等了所有帧）。缺 NVRHI 纹理、采样器或缓冲的 binding 直接抛异常。
- `TextureDescriptorBinding` 多了 `texture` 和 `nvrhiSampler`（`BindTexture(view, texture, sampler)` 两边一起
  填）；NVRHI 自己给整张纹理建视图，帧集的每个来源（级联/局部阴影、大气 8 张、探针、DDGI、透射拷贝、散射、
  路径追踪层、HDRI、DFG/LTC 表）原生视图本来就是整张图，所以两边一致。环境里的两个缓冲（大气 SH、DDGI 探针
  状态）改成 `nvrhi::IBuffer*`。`UpdateFrameDescriptorSets`（B1 的拆写）删了。

验证：A/B 对比 A1 之前的基线，9 个场景逐像素相同，validation 无报告；ctest 与之前相同。

下一步（B3）：G-buffer 集、材质集（`MaterialTexture` 宏用 `##Sampler`）、光追集（`rayTextures[]` 拆成纹理数组加
采样器表）照同样的路子：先让它们的图像平时都在 `SHADER_READ_ONLY_OPTIMAL`，再拆采样器，再换成 NVRHI 的
binding set。之后才是阶段 3 的逐 pass 迁移。

**B3a 完成（2026-10-09）**：材质集（几何、前向、卡通、路径追踪层 pass 的 set 1，级联和局部阴影 pass 的 set 0）
拆了采样器，仍是原生的：32 个纹理在 b，各自的采样器在 b + 64（`kMaterialSamplerBindingOffset`、
`MATERIAL_SAMPLER`），`VulkanMaterialSetCache` 每个 combined 写拆成两个。着色器：`gbuffer.frag`、
`triangle.frag`、`material_layers`、`detail_layers`、`toon_common`（及 `toon.frag/vert`、`toon_prepass.frag`
里的直接采样）、`shadow.frag`；`MaterialTexture(t, uv)` 展开成 `t.SampleBias(t##Sampler, uv, bias)`。
材质集换成 NVRHI binding set 先不做：NVRHI 每个 binding set 自建一个描述符池，而这个缓存为大地图的流式内容
专门做了成批的池（每池 1024 个集），照搬可能变慢；等阶段 3 看是走 NVRHI 的 descriptor table 还是别的办法。

验证：A/B 9 个场景逐像素相同，validation 无报告（卡通管线在启动时按新布局创建，也无报告；但这里没有
卡通场景的资产，卡通的画面没有对比）；ctest 与之前相同。

**B3c 完成（2026-10-09）**：G-buffer 输入集（光照、GI 合成、tonemap 调试视图的 set 2）一步到位：14 个纹理都是
场景渲染目标，本来就在 `SHADER_READ_ONLY_OPTIMAL`，读的都是同一个 nearest 采样器，所以拆成 14 个 `Texture2D`
加一个 `gbufferSampler`（binding 64），布局和每个帧槽的集直接用 NVRHI 的（`SceneRenderTargets::GetTexture`
给出每份拷贝的 NVRHI 纹理；深度加模板的目标 NVRHI 取深度那一面，和原来的 `GetSampledView` 一样）。
`OnTargetsRebuilt` 重建集，旧集随之释放它们引用的纹理。

验证：A/B 9 个场景逐像素相同，validation 无报告；ctest 与之前相同。Cornell DDGI 在整批 A/B 里有一次落在另一个
稳定取值上（预热少一帧，见 B2a 那条），同一个 exe 单独重跑三次都和基线逐像素相同。

**B3b 完成（2026-10-09，本机）**：光追纹理表（光线查询着色器的 set 3）拆成采样器表加纹理数组。binding 0 是
`SamplerState raySamplers[108]`：`VulkanSamplerCache` 能造的全部材质采样器（wrapS、wrapT、mag、min、mip 五个
枚举的组合 3·3·2·2·3 = 108 个，`VulkanSamplerCache::SamplerAt` 编号，默认采样器是 0），每个纹理表分配时写一次；
binding 1 是 `Texture2D rayTextures[]`（可变数量必须是最后一个 binding，所以纹理从 0 挪到 1），标志照旧
（partially bound、variable count、update unused while pending）。每个槽 5 张纹理各用哪个采样器，记在这个槽的
光追材质里：`RayMaterial` 加 `uint4 samplers`（x 低到高四个字节是基色、金属度、粗糙度、自发光，y 是法线），
由平均材质的 dispatch 从 push constant 原样写进去，所以 `RayMaterial` 从 32 字节变 48 字节
（`kRayMaterialBytes`，CPU 参考路径追踪器的回读跟着改）。`ray_hit_common` 的 `RayTextureSampleLevel` 统一采样，
命中着色、带纹理的覆盖测试、发光三角形表都用它。

纹理表仍是原生的：NVRHI 的 bindless 布局（`createBindlessLayout`）既没有可变数量，也没有
update-unused-while-pending，而流式地图靠后者在帧还在飞的时候往空槽里写新纹理。要换成 NVRHI 得给它打补丁，
留到阶段 3 迁光追 pass 时再定。

验证：71 个 SPIR-V `spirv-val` 通过；A/B 见下面“Windows 上的验证”。

### Windows + NVIDIA 上的验证（2026-10-09，本机）

云端会话在 lavapipe 上验过的 A1 到 B3c（`fb64ee6`）拿到本机，Debug，`AB_MODE=exe` 对比 `8b6cb8b`（A1 的前半）
跑了 `ab.py` 的全部 40 个场景（1280x720，90 帧，R34 道路、材质球、发光测试、卡通、DLSS、DLSS RR、路径追踪、
ReSTIR PT、软件光线、DDGI、各个 G-buffer 调试视图和 10 个 fixture）：

- 11 个场景 A、B 逐像素相同（10 个 fixture 里的 8 个，调试视图 5、15、18），另两个 fixture 和噪声底完全一样；
- 其余道路场景都在噪声底内，只有 5 个第一次高于噪声底：`materials_rt`、调试视图 1、2、13、14。每个 B 跑三次
  重测，`materials_rt`、视图 1、13 落回噪声底内；视图 2（法线）只差在轮廓边缘，三次 B 之间也互不相同——是
  TAA 抖动相位（预热帧数）不同，不是改动造成的（两次 A 恰好帧数相同）。日志里 `materials_rt` 的 A 两次画了 101
  帧、B 画了 104 帧。
- ctest 121/121 通过（两个长的车辆物理测试另算），Linux 上那三个已知失败在 Windows 上没有。

### 阶段 3：NVRHI 和原生录制混用（2026-10-09）

逐个 pass 迁的过渡期里，同一个命令缓冲里一段是原生的、一段是 NVRHI 的。做法：

- 帧的命令列表**关掉自动 barrier**（`VulkanCommandContext` 每帧 `open` 后 `setEnableAutomaticBarriers(false)`）：
  原生代码改了图像布局 NVRHI 看不见，自动 barrier 会按错的状态插。NVRHI pass 自己在每个 dispatch 前
  `setTextureState` / `setBufferState` 再 `commitBarriers`。
- `NvrhiPassScope`（`nvrhi_pass.h`）包住一个 NVRHI pass：开始时 `clearState()`（原生命令已经重新绑过管线，NVRHI
  缓存的绑定状态作废），对和原生 pass 共用的图像 `beginTrackingTextureState(原生代码给它的状态)`；结束时把它们
  `setTextureState` 回同一状态（同为 UnorderedAccess 时 NVRHI 插一个 UAV barrier，写对后面的原生 pass 可见）、
  `commitBarriers`、再 `clearState`。只在原生的读布局里读、不改状态的图像（`SHADER_READ_ONLY_OPTIMAL` 的
  场景目标）不用告诉 NVRHI。
- pass 自己的图像交给 NVRHI 跟踪：`keepInitialState` + `initialState`（例如 bloom 链、DLSS 的输入都停在
  ShaderResource），每帧开头的“丢弃内容”barrier 就没了。
- 每个绑定布局用 `registerSpace` 写出自己是第几个 set，并设 `registerSpaceIsDescriptorSet`（帧集 0、G-buffer 集 2
  也改了）：NVRHI 的 validation 只靠 register space 区分两个布局的 binding，Vulkan 上非零 register space 又必须
  带这个标志，而且同一管线里所有布局要一致。`ShaderBindingOffsets()` 代替三处手写的全零偏移。
- 着色器里只 `Load` 的输入改成不带采样器的 `Texture2D`；采样的照拆采样器的规矩放在 b + 64。
- 绑定光追场景集（含原生的加速结构）的 pass 要等加速结构迁到 `nvrhi::rt` 以后才能用 NVRHI 管线：NVRHI 管线的
  每个 set 都得是 NVRHI 布局。所以先迁不碰光追集的 pass。
- NVRHI 没有 blit（透射拷贝用 `vkCmdBlitImage` 生成 mip）、也没有“主机读”状态（曝光直方图的设备到主机
  barrier 仍是原生的）。
- 布局里声明了 push constants 的管线，每次 dispatch 前都要 `setPushConstants`（不用的 pass 给零）：NVRHI 的
  validation 层发现没设就**直接丢掉这次 dispatch**，画面会错（大气整片天空没了）。
- 在 NVIDIA 上，RGBA32F 的 GI 历史经 SRV（`SHADER_READ_ONLY_OPTIMAL`，从 GENERAL 转过去）读出来和在 GENERAL
  里读的结果不同（GI 视图暗约 9%，确定性的，不是竞争）；改成和写入的那张一样当存储图像读（一直在 GENERAL）
  就和原来逐像素相同。没查到底层原因，同一 pass 读写的历史一律留在 GENERAL。
- 被 pass 自己写的图像不要在同一个 dispatch 里再以 SRV 声明：Vulkan 的 validation 按“静态使用”查描述符布局。
  多重散射 LUT 的 pass 本来声明了自己（`useMultiScattering` 为 false 时不采样），现在把透射 LUT 传给那个参数。

已迁：bloom（链是 NVRHI 纹理，每对层级一个 binding set）；曝光直方图（NVRHI 清零缓冲，主机 barrier 原生）；
TAA 解析、DLSS 运动矢量、光线重建引导图（NGX 的 evaluate 和之后拷进历史的那一步仍是原生，阶段 4 再说；
运动矢量和引导图是 NVRHI 纹理，NGX 拿 `getNativeObject(VK_Image)` / `getNativeView` 的原生句柄）。
环境探针（捕获和预滤波走 NVRHI；mip 链的 blit 仍原生，夹在 NVRHI 设的 copy 状态之间；`NvrhiSharedTexture` 加了
`exitState`，新建的立方体从 UNDEFINED 出来）、GI 的 trace 和 resolve（输入都只 Load，不带采样器；历史读写都是
存储图像，见上）、大气（下）。

大气：所有管线共用的 set 1 换成一个 NVRHI 绑定布局（15 个 binding 照旧，三个采样的拆出 b + 64 的采样器，缓冲是
raw buffer），每个视图一个 binding set。图像的“停放状态”和原来一样：set 0 采样的（透射、天空视图、云噪声、云
阴影、空气透视、解析后的云）停在 ShaderResource，多重散射 LUT（路径追踪自己的集在 GENERAL 里读）和云的 march
目标停在 GENERAL，云的历史只有本 pass 读，停在 ShaderResource；第一次 Record 从 UNDEFINED 清零（NVRHI 的
`clearTextureFloat` / `clearBufferUInt` / `copyBuffer`），每个 dispatch 前设好它读写的状态。云的历史拷贝用
NVRHI 的 `copyTexture`，SH 回读用 `copyBuffer`，之后到主机的 barrier 原生。`NvrhiPassScope` 加了缓冲
（`NvrhiSharedBuffer`）。DDGI 的更新没迁：它写图集一层的同时在 GENERAL 里采样同一张图的下一层，NVRHI 的 SRV
表达不了（要改着色器里的取样方式）。

验证：探针 + GI + 大气一起对比 TAA 那个提交跑了全部场景：除了 DDGI 场景（不重置，A 对 A 也不同）和 DLSS（NGX
自己两次运行就差几级），全部逐像素相同；`MINIENGINE_NVRHI_VALIDATION=1` 和 Vulkan validation 无报告；ctest 121/121。

透射拷贝（第 0 级的拷贝走 NVRHI，mip 链的 blit 原生，同探针）；`ab.py` 加了 `transmission_rt`（缩小的 GTA 水面
放在材质球前，唯一有透射绘制的场景）。蒙皮（`skin.comp`、`tyre_deform.comp`）：每个网格一个 NVRHI binding set
（`RenderSubmesh::skinningSet` 是 `nvrhi::BindingSetHandle`，NVRHI 在用到它的帧结束前一直持有，原来的描述符池、
2048 个的上限和延迟释放都不要了），前后两个全局 barrier 仍原生（逐缓冲的状态要一个网格一个网格写）。

图形 pass：GI 合成和色调映射。NVRHI 图形管线 + framebuffer（每份拷贝一个）+ NVRHI 的 dynamic rendering；
目标图像在 layout tracker 给的 `COLOR_ATTACHMENT_OPTIMAL` 里进出。NVRHI 把视口一律转成 Direct3D 的朝向（从
maxY 起的负高度）：`NativeViewportState` 把矩形上下颠倒交给它，正好抵消，裁剪矩形单独给，着色器看到的仍是
Vulkan 的视口。验证同上，逐像素相同。

### 下一步（2026-10-09）

剩下的都要先定一件大事：
- 材质集（几何、前向、阴影、局部阴影、卡通、散射、路径追踪层的 set 1）换成 NVRHI：NVRHI 每个 binding set
  自建一个描述符池，大地图流式内容有几万个材质集（现在的缓存每池 1024 个）；要么给 NVRHI 打补丁让 binding set
  从共享的池列表分配，要么先量一下几万个小池的创建开销和显存。定了以后这些 pass（含天空、地面平面这两个夹在
  前向/几何 pass 里的全屏绘制）才能迁。
- 光追：加速结构换成 `nvrhi::rt`（含 compaction、分帧构建、ReBAR 内存类型的选择），光追场景集变成 NVRHI 的；
  纹理表要 NVRHI 补丁（可变数量 + update-unused-while-pending）或保持原生。之后 AO、SSR、光追阴影、反射、DDGI
  trace、路径追踪、ReSTIR、光照 pass 才能迁。
- DDGI 更新：改成写一层时只读另一张图（或把采样的那层先拷出来），才能用 NVRHI 的 SRV。
- 阶段 4：ImGui、DLSS（evaluate）、视频回读和截图、GPU 计时。

A/B 的预热帧数：同一个 exe 两次运行，`--wait-for-scene` 开始数帧前画的帧数会差一两帧（渲染线程装好场景的
时机不定），TAA 的抖动相位和历史把它带进截图，所以“A 对 A”有时就不同（B2a 那条说的 DDGI 也是这个）。
`ab.py` 现在 B 跑两次（`AB_B_RUNS`），只要有一对 A、B 截图逐像素相同就报 `same`——这能证明改动不改画面；
没有相同的一对时再拿最接近的一对和噪声底比。`AB_AUTO_EXPOSURE=1` 打开自动曝光和白平衡，让截图也检验曝光
直方图；`AB_BUILD` / `AB_BASELINE_BUILD` 指定 B、A 的构建目录（同一个 worktree 里留几份构建对比相邻提交）。

根治：`--wait-for-scene` 开始数帧的那一帧，应用把 `RendererSharedState::temporalRestart` 加一，随帧包带到渲染
线程，`VulkanRenderer::RestartTemporalEffects` 让每个视图的时间性效果从头开始：所有历史
（`ResetHistories`）、TAA 抖动序号、AO 噪声序号、云的重建历史和 2x2 步进、路径追踪的累积、DLSS 的历史。
于是截图只取决于开始数帧以后的帧，和加载用了几帧无关。自动曝光和 DDGI 的探针不重置（脚本截图一直靠加载
期间收敛的曝光；DDGI 场景照旧看噪声底）。

整体验证（有了下面的“根治”之后）：B3b + bloom + 直方图 + TAA 一起对比 `fb64ee6` 加时间性重启的基线，40 个场景里
道路、材质球、路径追踪、ReSTIR、软件光线、fixture 绝大多数逐像素相同；DDGI 场景（不重置）和 DLSS（NGX 本身两次
运行就不同）看噪声底；`road_dlss` 与 `fixture_cornell_ddgi` 第一次不同，待多跑几次复核。
之前单步验证：bloom、直方图、TAA 各自对比前一个提交：fixture 场景（TAA 开）`same`，打开自动曝光的两个 fixture
`same`；道路的光栅、DLSS、DLSS RR 在噪声底内；`MINIENGINE_NVRHI_VALIDATION=1` 无报告。

### 验证工具

`tools/render_ab/`：`ab.py`（A/B 截图，`AB_MODE=exe` 对比 `out/baseline_src` 里编的基线 exe；基线 = 改动前的
提交单独开 worktree 用同一 preset 编 `miniengine_app`）、`compare_reflection.py`（两组 SPIR-V 的接口比对）、
`build_all.py`（不经 CMake 编全部 Slang 着色器）、`scenes/`（关了地面平面的测试场景）。跑之前备份
`miniengine.settings.json`（运行会改写它）。
