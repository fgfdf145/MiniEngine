# HDR 输出开关与 HDR 校准

## 背景（2026-10-10）

D3D12 后端合入 main（c3a25f2）后已经能建 HDR10 交换链（`R10G10B10A2_UNORM` + `RGB_FULL_G2084_NONE_P2020`，flip-discard
加帧延迟等待对象），但 Graphics Debug 里没有开关（当时因为 Vulkan 在 NVIDIA 上 HDR 出帧不匀而隐藏），HDR 的亮度也有
2026-10-08 那次（已撤回的 display calibration，704d9bc）查到的老问题：

- UI 白固定在 203 cd/m²、GT7 的纸白固定在 250，而这台机器 Windows 的“SDR 内容亮度”是 480，所以一开 HDR 整个编辑器和
  场景中间调都暗一半；
- 峰值只来自手填的 `hdr_peak_nits`，不读显示器报告的值；
- ImGui 在 PQ 编码空间里做 alpha 混合，所有半透明的底色（按钮、输入框、文字边缘）都比 SDR 暗（实测输入框约 0.5 倍）。

用户要求：“加上开关和 HDR 校准”。

## 范围

只做 HDR（SDR 校准、曝光/饱和度、帧节奏器那几项不做——D3D12 交换链自己的等待对象已经解决了出帧节奏）。默认值不变：
`hdr_output` 默认关，用户自己打开才走 HDR。

## 设计

1. **显示器信息**（`engine/platform/display/display_hdr.h`，从 704d9bc 之前的版本恢复）：DXGI `IDXGIOutput6::GetDesc1`
   给出窗口所在显示器是否在 HDR、峰值/全屏峰值/黑位；`DisplayConfigGetDeviceInfo(SDR_WHITE_LEVEL)` 给 SDR 内容亮度。
   后台线程每 250 ms 做一次廉价检查（窗口所在显示器变了没有、缓存的 DXGI factory 的 `IsCurrent()`——Windows 切 HDR
   或显示器变化时它会失效），只有变了、或收到 SDL 的显示器/窗口换屏/HDR 状态事件时才重新枚举 DXGI；SDR 亮度滑块
   没有任何通知，每秒只读一次 `DisplayConfigGetDeviceInfo`。
2. **设置**（`DisplaySettings`，渲染设置的 `display` 组，存进 engine settings 和 capture state）：`calibrated`、
   `max_luminance`、`max_full_frame_luminance`、`min_luminance`、`ui_white_nits`（0 = 跟随 Windows）、
   `paper_white_nits`（0 = 跟随 UI 白）。开关仍是顶层的 `hdr_output`。旧的 `hdr_peak_nits` 不再读。
   `ResolveDisplayOutput` 把设置和显示器报告合成本帧用的值：未校准时用显示器报告的亮度，UI 白用 Windows 的 SDR 亮度。
3. **场景**（`tonemap.frag`）：HDR 时场景先乘 `纸白 / 250`，让 GT7 的 250 cd/m² 纸白落在校准的纸白上（默认 = UI 白，
   所以中间调和 SDR 一样亮，只有高光更亮），GT7 HDR 曲线的峰值是校准的最大亮度，最后除以 UI 白。眩光的余量是峰值 / 纸白。
   BT.2390 黑位抬升 `E' = E + b(1-E)^4` 不在这里做，而是在 `hdr_ui_encode.frag` 编成 PQ 之后对整帧（场景和 UI）做一次，
   `b = PQ(最小亮度)` 由 CPU 每帧算好（`HdrBlackFloorPq`；显示校准图案时为 0，图案必须是绝对亮度）。
4. **编辑器 UI**（`HdrUiComposite`）：HDR10 时 ImGui 不再直接写 PQ，而是画进一张和交换链一样大的 RGBA16F 图层，写
   sRGB 编码值（大于 1 的场景高光用同一条曲线外推），在这个编码里混合——和 SDR 交换链完全一样；然后
   `hdr_ui_encode.frag` 一次性解码、转 Rec.2020、以 UI 白为 1.0 编成 PQ、做黑位抬升后写进交换链。
5. **HDR 元数据**：D3D12 `IDXGISwapChain4::SetHDRMetaData`（MaxCLL = 峰值，MaxFALL = 全屏峰值，母版亮度 = 峰值/黑位，
   Rec.2020 原色）。Vulkan 不发（Vulkan 的 HDR 不推荐）。
6. **跟随显示器**：开着 HDR 输出时，显示器的 HDR 状态变了（Windows 开关、窗口拖到别的显示器）就重建交换链。
7. **校准界面**（`HdrCalibrationWindow`，Render > HDR Calibration… 和 Graphics Debug 的按钮）：仿 PS5 调整 HDR，视口全屏，
   图案由色调映射 pass 代替场景画出，绝对 cd/m²：
   - 1/5 全屏最大亮度：整屏是试验亮度，中间一个 10000 cd/m² 的圆环，调高到圆环刚好消失；
   - 2/5 10% 窗口最大亮度：黑底，中间 10% 面积的方块是试验亮度，圆环同上；
   - 3/5 最小亮度：整屏试验亮度，圆环 0 cd/m²，调低到圆环刚好融进背景；
   - 4/5 场景峰值（GT 自己的标定，见下）；
   - 5/5 纸白和总览：实时场景，纸白可跟随 UI 白或手调。
   试验亮度按 PQ 等分（左右方向键一格），Enter 下一步，Backspace 上一步，Esc 取消（恢复原值），Finish 保存。
8. **开关**：Graphics Debug 的 Output 段（HDR output、显示器报告、实际使用值、UI 白/纸白、校准按钮、“用显示器自己的数值”）
   和 Render > HDR Output。Vulkan 上开 HDR 时提示 D3D12 更平滑。

### GT 场景峰值标定（2026-10-10 追加）

用户要求：“GT 自己的峰值标定，就是环境里物体的亮度设置”。做法来自 Polyphony 的 *Practical HDR and Wide Color Techniques
in Gran Turismo SPORT*（SIGGRAPH Asia 2018，桌面 PracticalHDRandWCGinGTS_20181222.pdf，“Our Calibration Process”）：
屏幕的一部分是固定 10000 nits 的信号（显示器只能出它自己的峰值），旁边是玩家调的信号，调到两者分不出来，玩家调的那个
已知亮度就是估出来的峰值。只用屏幕的一部分，因为全屏峰值画面在游戏里很少见（显示器的 ABL）。

- 图案 `CalibrationPattern::GtPeak`（4）：黑底，和第 2 步同样的 10% 面积方块，里面 8×8 棋盘格，一半格子 10000 cd/m²、一半试验
  亮度。调高到棋盘格几乎看不出。
- 设置 `display.scene_peak_nits`（`DisplaySettings::scenePeakNits`，0 = 跟随 10% 窗口峰值 maxLuminance），解析到
  `DisplayOutput::scenePeakNits`：GT7 HDR 曲线的峰值和眩光余量（峰值 / 纸白）用它；HDR 元数据仍用显示器的 maxLuminance。
  进入这一步时若还是 0，从 10% 窗口的值开始。
- Graphics Debug 的 Output 段：“Scene peak follows display peak” + 手调。`--display-pattern 4,LEVEL` 可直接显示图案。
- 论文自己也说这个估计在显示器做色调映射（非 HGIG）时偏高（表里 400 实测估成 1100），所以 PS5 的三步保留，场景峰值单独存。

验证：`miniengine.display_calibration`（棋盘格占 10%、两种格子各半、相邻格不同、设置合成）、`miniengine.hdr_calibration_window`
（第 4 步、从 10% 窗口值起步、只改场景峰值、Finish 保存）；D3D12 强制 HDR10 抓窗口：`--display-pattern 4,300` 的格子 PQ 码
255（10000）和 159（300 cd/m²）；日志 “scene peak 600”（跟随）/ “1500”（设置）。

### 顺带修的 D3D12 问题

重建交换链时 `waitForIdle` 只等到最后一个 command list，排在它后面的 Present 还在用旧交换链的图像，debug layer 报
“CORRUPTION … Swapchain image … referenced by GPU operations in-flight”并退出（合入的 main 上也一样，只是不开验证时看不出）。
`D3D12GpuSwapchain` 析构前在队列上再 Signal 一个 fence 并等它。运行中开关 HDR 就是重建交换链，所以必须修。

## 验证

- 单元测试：`miniengine.display_calibration`（PQ 往返、黑位抬升、图案、设置合成）、`miniengine.hdr_calibration_window`
  （无 GPU 画出每一步，按键、取消、完成；`MINIENGINE_UI_SNAPSHOT_DIR` 出 PNG）、settings / capture state 往返。
- `out/hdr_check.py`（D3D12，`MINIENGINE_FORCE_HDR10=1` + `MINIENGINE_CAPTURE_WINDOW`，隐藏窗口，`MINIENGINE_NVRHI_VALIDATION=1`）：
  `--display-pattern 1,300` 抓到 305 cd/m²、`3,0.05` 抓到 0.043（窗口抓图只有 PQ 高 8 位）；同一画面 HDR 对 SDR（SDR 按 480 cd/m²
  白换算）的亮度比：1–300 cd/m² 各段 0.97–1.005，300 以上更亮（最高 610，峰值 600）；验证层 0 错误。
- SDR 不变：road_rt 在 Vulkan 和 D3D12 上与合入的 main 逐像素相同。
