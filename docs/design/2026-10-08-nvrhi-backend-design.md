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
- DDGI 的两个 layout 转换并进已有的那两个 `vkCmdPipelineBarrier`。分开记成两个独立的 barrier（就挨着原来的，
  lavapipe 会把相邻的 barrier 合并成一次 stall，所以执行上应当完全一样）时，Cornell DDGI 场景确定性地变了一点
  （均值 0.157，最大 31，3.4% 的像素 > 2），其中任何一个单独加都不变，也和 layout 本身无关（`GENERAL→GENERAL`
  同样会变），基线在 CPU 满载下跑也不变。像是某处依赖了未初始化的内存（命令流形状不同→主机分配不同），原因未查，
  记为后续。
- `CreateNvrhiImage/Buffer` 的 debug name 原来就是失败信息本身（"Failed to create a DDGI atlas"），改成去掉
  "Failed to create (the|a|an)" 的部分。

验证：A/B 对比 A1 之前的基线，9 个场景逐像素相同，validation 无报告；ctest 与之前相同。

### 验证工具

`tools/render_ab/`：`ab.py`（A/B 截图，`AB_MODE=exe` 对比 `out/baseline_src` 里编的基线 exe；基线 = 改动前的
提交单独开 worktree 用同一 preset 编 `miniengine_app`）、`compare_reflection.py`（两组 SPIR-V 的接口比对）、
`build_all.py`（不经 CMake 编全部 Slang 着色器）、`scenes/`（关了地面平面的测试场景）。跑之前备份
`miniengine.settings.json`（运行会改写它）。
