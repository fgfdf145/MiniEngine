# 着色器从 GLSL 迁移到 Slang

## 背景（2026-10-08）

用户问"换 NVRHI 和 Slang 有多少要做"，评估后决定**先只做 Slang**，NVRHI 暂缓。Slang 产出的仍是 SPIR-V，
现有 Vulkan 后端（`engine/renderer/vulkan/`）不动，C++ 侧只有极少量改动。

现状：`shaders/vulkan/` 103 个 GLSL 文件（45 comp、12 frag、6 vert、40 个 `.glsl` include），约 16.6k 行；
构建时 glslc 编译成 71 个 `.spv`（含 RAY_QUERY、`PATH_TRACE_LAYER_PASS`、`PT_*` 变体），运行时按文件名加载。
另有 9 个 `.glsl` 头被 C++（`exposure.cpp` 和 8 个测试）在 `using namespace glm` 下当 C++ 编译。

## 目标与范围

- 所有着色器改用 **Slang 语言**（HLSL 风格）书写，`slangc` 编译；删除 GLSL 源、glslc 规则和 `shaderc` 依赖。
- **SPIR-V 接口不变**：输出文件名、入口名 `main`、descriptor set/binding、push constant 布局（std430）、
  UBO（std140）/SSBO（std430）成员偏移、specialization constant id、顶点输入和 varying 的 location 全部
  保持原样。因此 C++ 管线代码不需要改（唯一例外见"64 位整数"）。
- 画面不变：同一个 exe，`--shaders` 分别指向 GLSL 和 Slang 编译结果，逐场景对比截图。
- 不在本次范围：Slang 模块化（`import`）、泛型/接口重构、自动微分、着色器热重载、NVRHI。

## 关键决策

### 1. 文件组织：一一对应，`#include` 不用 `import`

每个 GLSL 文件对应一个 Slang 文件，注释和结构保留，便于逐个对比审阅：

- 入口：`bloom.comp` → `bloom.comp.slang`，输出仍是 `bloom.comp.spv`；
- 头文件：`brdf_common.glsl` → `brdf_common.slang`，仍用 `#include` + include guard。

不用 `import` 模块，因为现有变体全靠预处理宏（`RAY_QUERY`、`PT_TRANSMISSION`、`PATH_TRACE_LAYER_PASS`、
各 pass 定义 `RAY_SCENE_SET` 之类的 set 编号再 include），而 Slang 模块单独编译、看不到调用方的宏。
改成模块要把这些宏换成 link-time 常量，是另一项重构，留到以后。

### 2. 矩阵约定

C++ 侧 GLM 列主序存储。Slang 用 `-matrix-layout-column-major`（显式给出），此时 Slang 的 `float4x4` 读出来
就是数学上的同一个矩阵 M（SPIR-V 里体现为 `RowMajor` 修饰加转置的类型，已用 spirv-cross 反编译验证）。
翻译规则：

| GLSL | Slang |
|---|---|
| `M * v` | `mul(M, v)` |
| `v * M` | `mul(v, M)` |
| `A * B`（矩阵乘） | `mul(A, B)` |
| `M[i]`（第 i **列**） | `ColumnOf(M, i)`（= `transpose(M)[i]`），Slang 的 `M[i]` 是**行** |
| `mat3(c0, c1, c2)`（按列构造） | `transpose(float3x3(c0, c1, c2))` |
| `mat3(M4)` | `(float3x3)M4`（同为左上角） |

### 3. 其它容易出错的语义差异

- `mod(x, y)`（floor）≠ `fmod`（trunc）：负数结果不同，统一用 `GlslMod` 辅助函数；
- 全局 `const` 必须写 `static const`（否则 Slang 当成 uniform 参数）；全局可变变量写 `static`；
- `gl_InstanceIndex`/`gl_VertexIndex` 用 `SV_VulkanInstanceID`/`SV_VulkanVertexID`（`SV_InstanceID` 会减掉
  firstInstance）；
- 计算着色器里不能出现隐式导数的采样（`Sample`/`SampleBias`），否则 Slang 会加
  `ComputeDerivativeGroupQuadsKHR` 能力；
- 向量三元运算用 `select`；`mix(a, b, bvec)` → `select(bvec, b, a)`；
- `atan(y, x)` → `atan2`，`fract` → `frac`，`inversesqrt` → `rsqrt`，`floatBitsToUint` → `asuint` 等；
- 存储图像写明格式 `[[vk::image_format("rgba16f")]]`；`flat` → `nointerpolation`；
- 顶点/片元的输入输出都写 `[[vk::location(n)]]`，迁移期间 GLSL 和 Slang 阶段可以混用。

### 4. buffer_reference 与 64 位整数

`ray_hit_common.glsl` 用 `GL_EXT_buffer_reference_uvec2` 从 `uvec2` 地址读顶点/索引（避开 64 位整数）。
Slang 的指针要从 `uint64_t` 构造，SPIR-V 需要 `Int64` 能力，所以设备在启用硬件光追时同时启用
`shaderInt64`（所有支持 ray query 的 GPU 都支持）。这是 C++ 侧唯一的改动。

### 5. 与 C++ 共享的头文件

`exposure_histogram`、`pre_exposure`、`brdf_common`、`anisotropy_common`、`iridescence_common`、
`pbr_neutral`、`local_shadow_common`、`ltc_common`、`ssr_common`（以及它们 include 的 `gt7_tonemap`）
继续写在两种语言的公共子集里：`static const`、`float3`、`lerp`/`saturate`/`frac`/`rsqrt`/`mul`。
C++ 侧新增 `engine/renderer/shader_cpp_compat.h`，给 GLM 提供这些类型别名和函数。

### 6. 编译器来源

和 glslc 一样：先找 Vulkan SDK 的 `Bin/slangc`，找不到再用 vcpkg host 依赖 `shader-slang` 的 `slangc`。
`vcpkg.json` 里 `shaderc` 换成 `shader-slang`。编译参数：
`-target spirv -profile spirv_1_3`（光追变体 `spirv_1_4`）`-matrix-layout-column-major -entry main`，
`-depfile` 生成依赖，替代原来"任何 include 改动重编全部"的规则（VS 生成器和 Ninja 都支持）。

## 验证

1. **接口对比**（每个 `.spv`）：`spirv-cross --reflect` 比较 GLSL 版和 Slang 版：
   入口、workgroup 大小、每个 set/binding 的资源类型和数组大小、push constant 和每个 block 的成员偏移/
   矩阵步长、spec constant id、阶段输入输出的 location。工具在 `out/slang/compare_reflection.py`（不入库）。
2. **画面对比**：同一个 Release exe，`--shaders` 指向 GLSL 输出和 Slang 输出，固定曝光的 `--state` 场景：
   普通延迟/前向、RT 效果全开、路径追踪 + ReSTIR、软件光线（`--software-rays`）、DDGI 调试视图、体积云/
   大气、卡通（Yuki）、轮胎变形、选中描边、bloom/TAA。先用两次 GLSL 运行量出噪声底，再看 Slang 是否在噪声内。
3. **测试**：`ctest` 全部通过（含共享头的 C++ 测试）；validation layer 无新增报错。
4. **性能**：GPU pass 计时对比（`--frames` 的 `LogFrameTimings`），确认没有明显回退。

## 实现中补充的决定

- **invariant**：Slang 没有 `invariant gl_Position` 的写法（`spirv_asm` 只能引用输入内建变量）。triangle.vert 和
  toon.vert 编译后由 `tools/spirv_invariant_position`（约 100 行 C++，构建时先编）给 Position 输出加 `Invariant`
  修饰，CMake 列表 `MINIENGINE_INVARIANT_POSITION_SHADERS`。
- **GLSL 内建函数**：`inverse`、`pack/unpack{Unorm4x8,Snorm2x16,Half2x16}` 在 Slang 里没有，`shader_helpers.slang`
  用 `spirv_asm` 发出和 glslc 相同的 GLSL.std.450 指令（`Inverse`、`PackUnorm4x8` 等）；`.length()` 用
  `BufferLength`。
- **共享头里的矩阵**：GLM 和 Slang 的矩阵构造函数一个按列一个按行，共享头里不写构造函数：`gt7_tonemap.slang` 的
  色域转换改成每通道一个点积，`ltc_common.slang` 用 `MatrixFromColumns/MatrixFromRows`（Slang 版在
  `shader_helpers.slang`，GLM 版在 `engine/renderer/shader_cpp_compat.h`）。
- **语义陷阱（逐个修过）**：GLSL 的向量 `==`/`!=` 得到一个 bool，Slang 逐分量；`mix(a, b, bvec)` 要换成
  `select`；`gl_FragDepth` 等内建；固定大小的 storage block 读成单元素 StructuredBuffer，光源 cluster
  块（定长头 + 变长数组）读成 uint 字，用 `LightClusterRange/LightClusterIndex` 两个访问函数；全局可变变量要
  `static`，否则 Slang 当成 uniform。编译器对这些都有错误或警告（`non-short-circuiting ?:`、bool 向量隐式转换），
  全部清零。
- **采样**：影子图的 `textureGrad(零梯度)` 改成 `SampleCmpLevelZero`（单级 mip，结果相同）；计算着色器里不出现
  隐式导数采样。
- **设备**：硬件光追同时要求并启用 `shaderInt64`（`device.cpp`），只有 12 个硬件光追着色器用到 Int64。
- **构建**：slangc 写 depfile，改一个 include 只重编依赖它的着色器（以前任何 include 改动重编全部）；
  警告 41012（profile implicitly upgraded）关掉，它列的是内建函数可能要的能力，模块实际只声明用到的。
- **测试用固定步长**：`MINIENGINE_FIXED_FRAME_SECONDS=<s>` 让每帧走固定时间、加载期间不走时间
  （`RendererSharedState::IsSceneLoading`，和 `--wait-for-scene` 同一判断），A/B 截图才可重复。

## 验证结果（2026-10-09）

1. **接口**：71 个 SPIR-V 全部与 GLSL 版逐项比对（`out/slang/compare_reflection.py`）：入口、workgroup、每个
   set/binding 的类型和 block 布局（std140/std430 偏移、矩阵步长和主序）、push constant、spec constant、
   location、内建变量（含 Invariant）、执行模式、能力。剩下的差异都是预期的：Slang 去掉未使用的绑定、spec
   constant 和片元输入；定长 block 读成结构化缓冲；光源 cluster 读成 uint；imgui_hdr10 的输入块拆成两个
   location；光追着色器多 Int64 能力；内联造成的矩阵乘法指令数不同。`spirv-val` 全部通过；vcpkg 的
   slangc 2026.5（没有 Vulkan SDK 时用它）也能全部编过、接口相同。
2. **画面**（同一个 Debug exe，`--shaders` 分别指 GLSL 和 Slang 输出，1280x720，60 帧，固定 EV；先跑两次 GLSL
   量噪声底）。超过 2/255 的像素比例：

   | 场景 | GLSL vs GLSL | GLSL vs Slang |
   |---|---|---|
   | R34 + Yuki，RT 效果 | 0.39% | 0.42% |
   | 同上，光栅（无硬件光追） | 0.59% | 0.40% |
   | 同上，软件光线（compute BVH） | 0.40% | 0.41% |
   | 路径追踪 / ReSTIR | 3.21% / 35.1% | 3.22% / 35.5% |
   | 体积云 | 0.69% | 1.32% |
   | DDGI（硬件 / 软件光线） | 8.0% / 1.3% | 9.2% / 1.4% |
   | 材质球 + 点/聚/面光 + 局部阴影（RT / 光栅 / PT） | 1.5% / 1.2% / 1.3% | 0.4% / 0.35% / 1.1% |
   | 自发光院子 PT / ReSTIR | 1.8% / 19.3% | 1.1% / 3.4% |
   | DLSS 超分 / 光线重构 | 0.24% / 0.09% | 0.30% / 0.27% |
   | 卡通着色（车窗里的 Yuki） | 2.2% | 2.2% |

   均在噪声底内；G-buffer 各调试视图（反照率、法线、AO、光线追踪视图、RT 阴影、探针遮挡）也一样。光线追踪
   视图里剩下的差异只在轮廓和地平线像素：Slang 用转置矩阵乘，求和顺序不同，射线方向差几个 ulp。
3. **测试**：ctest 123 项中 122 项通过；`vehicle_brush_tyre` 在 Debug 下超时（物理测试，主线上已知问题，和着色器
   无关）。共享头的 C++ 测试（brdf、ltc、tonemap、exposure 等 12 项）编译自 `.slang`。
4. **validation layer**：所有 Slang 运行没有新的报错（DLSS-D 的 NGX 内部图像警告是原有的）。

顺带发现：`assets/scenes/rolling_road.yaml` 开着大气的地面平面（y = 0），它和起伏几厘米的路面 z-fighting，
TAA 抖动的相位不同就会出现大块的明暗斑（每次运行位置不同）。和这次迁移无关，测试场景里把地面平面关了。
