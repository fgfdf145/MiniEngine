# NVIDIA DLSS 超分辨率与 DLAA

日期：2026-10-07

## 目标

在 Vulkan 渲染器里接入 NVIDIA DLSS（Super Resolution 和 DLAA），通过 NGX SDK 直接调用，不经过 Streamline。DLSS 放在引擎原来 TAA 的位置：输入是渲染分辨率的 HDR 图像、深度和运动向量，输出是视口分辨率的抗锯齿图像，之后接 Bloom、曝光、色调映射。机器不支持 DLSS（没有 SDK、不是 RTX 显卡、驱动过旧、Mac）时，自动退回自研 TAA，其他行为不变。

本期不做帧生成、Reflex 和 Ray Reconstruction，原因见 2026-10-05 关于“引擎缺什么”的分析：帧生成要先有不带编辑器 UI 的游戏模式，Ray Reconstruction 要先有硬件光追。

## 依赖

- SDK：github.com/NVIDIA/DLSS，固定版本 v310.9.1。`scripts/fetch-dlss-sdk.sh` 只下载需要的文件到 `.deps/dlss`（头文件、`nvsdk_ngx_d.lib` / `_d_dbg.lib`、release 版 `nvngx_dlss.dll`，Linux 是 `.a` 和 `.so`），不提交到 git。
- `cmake/MiniEngineDlss.cmake`：检测到 SDK 时创建 `miniengine_ngx` 接口目标，定义 `MINIENGINE_WITH_DLSS=1`。Debug 链接 `_dbg` 版本（/MDd），其他配置链接 `_d` 版本（/MD）。`miniengine_copy_dlss_runtime(target)` 在构建后把 DLL 复制到 exe 旁边，NGX 默认在这里找它。
- 没有 SDK 时，`dlss.cpp` 编译成桩实现，界面上显示 “built without the DLSS SDK”。
- 不使用开发版 DLL（`dev/`）：它带水印和调试叠加层，不能分发。

## NGX 封装（`engine/renderer/vulkan/dlss.{h,cpp}`）

- 扩展：建 instance 前用 `NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements` 查询需要的 instance 扩展，选定物理设备后用 `...DeviceExtensionRequirements` 查询 device 扩展。`VulkanInstance` 和 `VulkanDevice` 新增“可选扩展”参数：设备支持就开启，有任何一个缺失就记下来，此时 DLSS 不可用，引擎其余部分照常运行。RTX 4070 Laptop + 驱动 616.92 实测需要的 device 扩展：`VK_NVX_binary_import`、`VK_NVX_image_view_handle`、`VK_KHR_buffer_device_address`、`VK_KHR_push_descriptor`。列表里有 buffer_device_address 时，同时开启 `bufferDeviceAddress` 特性。
- 初始化：`NVSDK_NGX_VULKAN_Init_with_ProjectID`，engine type 用 CUSTOM，project ID 是一个固定的 GUID。日志和缓存写到 `%TEMP%/MiniEngine/ngx`。通过 capability parameters 检查 `SuperSampling.Available` 和 `NeedsUpdatedDriver`，把不可用的原因记到 `Status()`，在 Graphics Debug 窗口里显示。
- 渲染尺寸：`RenderExtentFor(output, mode)` 调用 `NGX_DLSS_GET_OPTIMAL_SETTINGS`；DLAA 的渲染尺寸等于输出尺寸。上一次的查询结果会缓存。
- Feature：`EnsureFeature(render, output, mode)` 在尺寸或模式变化时重建 feature，用单独的命令缓冲提交并等待完成。创建失败会记住这一组参数，之后不再每帧重试。释放前先 `vkDeviceWaitIdle`，因为还在飞的帧可能正在使用它。
- 创建标志：`IsHDR | MVLowRes | DepthInverted | AutoExposure`。
  - IsHDR：`SceneHdr` 是预曝光的线性 HDR。
  - DepthInverted：深度是 reverse-Z。
  - MVLowRes：运动向量是渲染分辨率，不含抖动，所以不设 `MVJittered`。
  - AutoExposure：曝光让 DLSS 自己测光。Preset L/M 本来就总是自动曝光；输入已经是眼睛适应后的值，所以不传 pre-exposure。

## 渲染分辨率和输出分辨率分开

`SceneRenderTargets` 原来只有一个 extent，现在拆成两个：

- `GetExtent()`：渲染尺寸。G-buffer、光照、AO / SSR / GI、体积云、前向和透明 pass 都用这个尺寸。
- `GetOutputExtent()`：输出尺寸。`SceneTaa`、`SceneLdr`、`SelectionDepth` 和 `SelectionOutline` 用这个尺寸（`TargetDescription::outputSized`）。

跨尺寸的地方：

| 位置 | 处理 |
|---|---|
| Bloom | mip 链和合成按输出尺寸 |
| 曝光直方图 | 遍历输出像素；深度按 `pixel * depthSize / hdrSize` 取 |
| 色调映射 | 输出尺寸。G-buffer 调试视图本来就按 UV 采样，不受影响 |
| 选中描边 | 两张图都是输出尺寸；可见性测试读场景深度时按比例换算像素坐标 |
| SSR | 从 TAA 历史按 UV 取反射颜色，与尺寸无关；DLSS 时历史图来自 DLSS 输出（见下文） |
| `--capture` / 录像 | 读 `SceneLdr`，所以用输出尺寸 |

`ScenePassFrameContext` 新增 `outputExtent`。`extent` 仍然是渲染尺寸。

尺寸由谁决定：编辑器计算视口尺寸，作为输出尺寸，渲染线程在 `SyncSceneTargets` 里调用 `ResolveSceneExtents` 决定渲染尺寸。

- DLSS 生效时（模式不是 Off、不是 forward-only、设备可用、feature 建成），渲染尺寸取 DLSS 的最佳尺寸，编辑器按 100% 计算视口像素，Render scale 滑条变灰。
- 否则渲染尺寸等于输出尺寸，Render scale 照旧缩小整条管线。

TAA 历史、AO / GI / SSR 历史在尺寸或模式变化时重置，DLSS 的 `InReset` 也跟着置位。

## TAA pass 里的 DLSS 路径（`VulkanTaaPass::RecordDlss`）

DLSS 生效时，原 TAA pass 不再执行 resolve，改为下面四步：

1. **运动向量**（`dlss_motion_vectors.comp`，RG16F，渲染尺寸）：几何 pass 写入的是 `当前 UV − 上一帧 UV`，转换成 DLSS 需要的 `(上一帧 − 当前)`，单位是渲染像素。背景像素没有写速度，用相机矩阵重投影：用带抖动的 `invViewProj` 求出这个像素看到的方向，用 `prevViewProj` 投影到上一帧，当前位置要减去本帧抖动，结果才不含抖动。
2. **评估 DLSS**：color = `SceneHdr`，depth = `SceneDepth`（只读布局），mv = 第 1 步的结果，输出 = `SceneTaa`（GENERAL）。NGX 会先清空输出，所以 `SceneTaa` 加了 TRANSFER_DST 用途。
3. 把 `SceneTaa` 复制到 TAA 历史图里本帧要写的那一张。下一帧 SSR 从这里取颜色，和 TAA 时一样。
4. 屏障：NGX 的工作阶段未知，第一个作用域用 ALL_COMMANDS。复制完成后接 compute / fragment 阶段的屏障，与布局跟踪器之后的转换形成执行依赖链。

## 抖动

- 序列不变，仍是 Halton(2,3)。周期改为 `TaaJitterPhaseCount = 8 × 输出像素数 / 渲染像素数`（指南 3.7.1.1：Quality 18、Performance 32、Ultra Performance 72），不缩放时仍是 8。
- DLSS 生效时总是抖动，不看 TAA 开关。抖动只加在投影矩阵上，运动向量和编辑器矩阵都不带抖动。
- 符号：`JitterProjection` 让画面平移 +jitter 像素（x 向右，y 向下），传给 DLSS 的就是这个值。实测（Performance 模式，静态画面，与原生 TAA 对比的 PSNR）：(+x,+y) 38.65 dB，(−x,−y) 35.33，(+x,−y) 34.66，(−x,+y) 35.71。(+,+) 明显最好，梯度能量也最高，说明画面最锐，确认符号正确。

## 纹理 mip bias

指南 3.5：`bias = log2(渲染宽 / 输出宽) − 1`，Performance 是 −2，DLAA 是 −1。

- 函数是 `UpscaleTextureMipBias`，结果放在相机 UBO 末尾新增的 `textureParams.x`。
- 着色器里新增宏 `MaterialTexture(s, uv)`，等于 `texture(s, uv, MATERIAL_MIP_BIAS)`。gbuffer.frag、triangle.frag、material_layers.glsl、detail_layers.glsl 里的 46 处材质采样都改用这个宏。用宏而不是函数，是因为 scene_common.glsl 也被 compute shader 包含，而 compute shader 里不能用带 bias 的 `texture`。
- 不用 DLSS 时 bias 为 0，画面与原来逐像素一致。

## 设置与界面

- `RenderDebugSettings::dlssMode`（`DlssMode`：Off / DLAA / Quality / Balanced / Performance / Ultra Performance），存为 engine settings 的 `render.dlss_mode`，capture state 的 `render_debug.dlss_mode`。
- Graphics Debug 窗口里 Render scale 下方新增 “DLSS” 下拉框。设备不支持时下拉框变灰，并显示原因。DLSS 生效时，Render scale 和 TAA 开关变灰。

## 验证（RTX 4070 Laptop，驱动 616.92，1920×1080，R34 rolling road 场景）

- Quality：1280×720 → 1920×1080。静态画面与原生 TAA 的 PSNR 为 42.1 dB，平均亮度差 < 0.3%。细节（Brembo 字样、雨刮、格栅）与原生相当或更清晰。
- 相机每帧平移 6 mm（`--camera-velocity 0.006,0,-0.006`），Quality 和 Performance 下车身轮廓、尾翼、轮毂都没有拖影。
- Debug 构建开着验证层，没有我们这边的报错。Performance（Preset M）的第一帧验证层报告 NGX 自己的内部图像（`nv.ngx.dlss.resource`）布局是 UNDEFINED，只出现一次，属于 NGX 内部问题。
- 单元测试：`miniengine.taa_jitter` 新增相位数和 mip bias 的测试。

## 已知限制 / 后续

- Blend 材质（玻璃、粒子）不写运动向量，沿用下层表面的速度，快速运动时可能有轻微拖影。之后可以用 reactive / transparency mask 处理。
- DLSS 只在 deferred 管线下生效，forward-only 对照管线没有运动向量。
- 没有接 OTA 更新（`NVSDK_NGX_UpdateFeature`）。
- Preset 可以在 Graphics Debug 的 “DLSS preset” 里选（Default / J / K / L / M，存为 `dlss_preset`），对所有档位生效，换 preset 会重建 feature 并清掉历史。Default 交给 DLSS：SDK 310.9 下 DLAA/Quality/Balanced 是 K，Performance 是 M，Ultra Performance 是 L。NGX 没有接口读回实际在用的 preset，所以 Default 后面括号里的名字是按 SDK 头文件的注释写的。
- 帧生成、Reflex 和 Ray Reconstruction 见目标一节。
