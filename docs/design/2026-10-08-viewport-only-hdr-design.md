# 只在 viewport 里输出 HDR

## 目标

打开 HDR 输出后，只有 viewport 里的场景是 HDR，编辑器 UI 与 SDR 模式逐像素一致（亮度、颜色、半透明混合都一样）。
默认仍然关闭；用户不开就什么都不变。

上次（显示校准分支，已回滚 704d9bc）的教训：

1. UI 白固定 203 nit、GT7 纸白 250 nit，而 Windows 把 SDR 内容放在用户的"SDR 内容亮度"（这台机器 480 nit），
   所以开 HDR 后 UI 和场景都暗了一半。
2. ImGui 直接画进 PQ 交换链，半透明填充在 PQ 空间混合，和 SDR 不一样。
3. GPU 跑满时 HDR 交换链成批归还图像（呈现间隔 1 / 8 / 17 ms 循环），画面抖。
4. 默认改成跟随 Windows 自动开 HDR，改变了用户日常的输出。

## 设计

1. **UI 先画进 SDR 图层。** HDR 模式下 ImGui 用后端自带的着色器画进一张 `B8G8R8A8_UNORM` 离屏图（每张交换链图一张），
   和 SDR 交换链完全相同的格式、清屏色、混合方程，所以 UI 像素与 SDR 模式一字不差。
2. **在 viewport 处打洞。** 渲染线程把绘制数据里采样 viewport 纹理的命令换成回调：用一条关闭混合的管线把该命令的
   裁剪矩形内写成 (0,0,0,0)，随后的 ResetRenderState 恢复 ImGui 状态。之后画的覆盖层（HUD、小地图、选中描边、
   gizmo、文字）照常混合，于是图层的 alpha 正好是覆盖层的覆盖率，颜色是预乘的 sRGB 编码值。
   viewport 图像的屏幕矩形和 UV 从该命令的顶点读出。
3. **合成。** 一个全屏 pass 写交换链：`out = Linear(ui.rgb + Encode(scene) * (1 - ui.a))`，`Encode` 是延伸到 1 以上的
   sRGB 编码。场景不超过 SDR 白时，这就是 SDR 交换链里的那次混合，覆盖层半透明效果与 SDR 相同；超过 1 的高光平滑延伸。
   `out` 以 SDR 白为 1.0，按 Windows 的 SDR 内容亮度输出：
   - 首选 scRGB（`R16G16B16A16_SFLOAT` + `EXTENDED_SRGB_LINEAR`），值 = `out * sdrWhiteNits / 80`，
     与 DWM 合成 SDR 窗口的方式相同；
   - 没有 scRGB 就用 HDR10（`A2B10G10R10` + `ST2084`），值 = `PQ(Rec2020(out) * sdrWhiteNits)`。
4. **显示信息。** Windows 下用 DXGI `GetDesc1`（峰值、是否 HDR）和 `DISPLAYCONFIG_SDR_WHITE_LEVEL`（SDR 内容亮度）
   查询窗口所在显示器，在后台线程每秒轮询一次（Windows 不发事件）。SDR 白只是推常量，改了不需要重建任何东西。
   Windows 报告该显示器没开 HDR 时不建 HDR 交换链（否则 DWM 会把 HDR 压回 SDR）；非 Windows 平台照旧。
5. **色调映射。** 纸白跟随 SDR 白：GT7 的 HDR 曲线按"峰值相对纸白的余量"建，即虚拟峰值
   `peak × 250 / sdrWhite`，输出按 250 nit 读回到以 SDR 白为 1 的单位。曲线的趾部和线性段与峰值无关，
   所以 SDR 肩部以下与 SDR 帧完全相同，只有高光延伸到 `peak / sdrWhite`。峰值默认取显示器报告的值（`hdr_peak_from_display`，默认开），
   关掉后用 `hdr_peak_nits`；有效峰值不低于纸白。眩光的余量是峰值 / 纸白。
6. **四路录制等离屏视图** 始终按 SDR 输出（它们的图像给 UI 预览和视频用）；选中描边的图像始终是 8 位 sRGB。
7. **帧节奏。** 每帧拆成两个命令缓冲、两批提交：工作（场景、HDR 时的 UI 层）不等交换链，只有写交换链图像的
   pass 等 acquire 信号量。这样 GPU 计时里的工作时间不含等显示的时间。HDR 时按工作时间 × 1.08 开始每一帧
   （`hdr_frame_pacing`，默认开），避免成批归还造成的抖动。`MINIENGINE_FRAME_TIMES=<文件>` 记录每次呈现的时间。
8. `imgui_hdr10.frag` 和 `kUiWhiteNits` 删除。

## 验收

自动：
- 色调映射单测：纸白 = 250 时与原 HDR 曲线一致；中间调 HDR / SDR 比值为 1。
- 合成公式单测：a = 1 时等于 UI，a = 0 时等于场景，场景 ≤ 1 时等于 SDR 的编码空间混合。

在用户的显示器上（Desktop Duplication，FP16 scRGB 读回）：
1. UI 区域 HDR 与 SDR 的亮度一致（同一像素比值 ≈ 1）。
2. 场景中间调 HDR / SDR ≈ 1，高光超过 SDR 白。
3. HDR 时帧间隔的标准差与 SDR 相当。

## 实现中的修订

- **纸白**：最初是把场景乘 `sdrWhite / 250` 再进曲线；但 GT7 的趾部在绝对亮度上（0.538 帧缓冲单位），
  放大输入会让阴影比 SDR 亮约 25%。改为按余量建虚拟峰值（见设计 5），单测验证 SDR 肩部以下逐值相同。
- **帧节奏的原因**（PresentMon 2.6.0，RTX 4070 Laptop，240 Hz）：SDR 交换链走 NVIDIA 原生路径
  （`Composed: Copy with GPU GDI`），HDR 交换链被驱动转到 DXGI（窗口 `Composed: Flip`，全屏
  `Hardware: Independent Flip`）。屏幕几乎每次刷新都有新画面，但 HDR 的出帧时刻不均：路径追踪场景里，
  相邻两帧内容的时间步长与显示步长之差平均 5.3 ms（SDR 2.2 ms），全屏也有 4.2 ms。
- **试过不用的方案**：`VK_KHR_present_wait` 等上一帧上屏再开始下一帧，29% 的帧停两个刷新周期，更差；
  换 FIFO / IMMEDIATE、换 HDR10 都不变。按 GPU 时间限帧最初失败有两个原因：整帧只有一批提交，所有写颜色的
  pass 都等 acquire，"GPU 时间"里混进了等显示的时间；GPU 计时的第一个时间戳写在 TOP_OF_PIPE，帧排队时
  它提前写下，把上一帧的尾巴也算了进去。两者都会形成正反馈。现在每帧两批提交、首个时间戳在 BOTTOM_OF_PIPE。
- **最终测量**（PresentMon，窗口 1920×1080，GPU 上没有别的程序）：

  | | 帧率 | 出帧间隔标准差 | 运动抖动 平均 / p95 |
  |---|---|---|---|
  | 轻场景 SDR | 356 | 0.28 ms | 1.48 / 1.97 ms |
  | 轻场景 HDR 限帧 | 313 | 0.37 ms | 1.42 / 2.42 ms |
  | 路径追踪 SDR | 164 | 0.38 ms | 2.16 / 2.79 ms |
  | 路径追踪 HDR 不限帧 | 130 | 5.86 ms | 5.05 / 10.64 ms |
  | 路径追踪 HDR 限帧 | 130 | 1.90 ms | 2.12 / 5.20 ms |

  限帧不损失帧率。HDR 在路径追踪场景比 SDR 慢约 1.6 ms/帧，不限帧时也一样，来源未查（驱动的 DXGI 路径或
  FP16 合成）。限帧后 p95 抖动仍比 SDR 高。
- 自动 HDR（Windows Auto HDR）不作用于 Vulkan，不在本方案内。
