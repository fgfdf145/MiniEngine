# 视口录屏

日期：2026-10-04。前置：视口截图（Tools > Capture Viewport，`CaptureViewportWithState`）。

标注约定同前：**[已验证]** = 测试或独立计算核对过；**[未核对]** = 假设，或没有在真机上跑过。

## 0. 范围

- 录的是**视口**：色调映射后的 LDR 场景图（`SceneLdr`），与 Capture Viewport 的 PNG 是同一张图。ImGui 画的东西（gizmo、选中框、车辆叠加层、REC 标志）不在视频里。不录整个窗口，也不录声音。
- 编辑器里用 **Tools > Record Viewport**（`Shift+F12`，工具栏播放按钮右边的按钮）开始和停止。文件写到 `captures/recording_<日期>_<时间>.avi`。
- 脚本运行用 `--record <file.avi>`（可加 `--record-fps <n>`，默认 30），与 `--frames` 配合，用于复现和对比。

## 1. 格式：MJPEG AVI

每帧一张独立的 JPEG，装在 AVI（RIFF，带 `idx1` 索引）里。

- **为什么选它：** 不需要新依赖。JPEG 编码用仓库里已有的 stb_image_write，AVI 容器约 200 行代码。ffmpeg、VLC、Windows 的“电影和电视”、Premiere、Resolve 都能直接打开。H.264/MP4 需要 Media Foundation 或 ffmpeg，而且在这台 Linux 上无法编译和测试，所以以后要做再单独做。
- **代价：** 文件大。1080p、质量 90 大约每帧 150–250 KB，30 fps 时约 5–7 MB/s **[未核对：按 1281×720 的合成图外推]**。
- **4 GB 限制：** 经典 AVI 的大小和偏移都是 32 位，所以单个文件写到 `0xF0000000` 字节（约 3.75 GiB）就关闭，接着写 `<名字>_2.avi`、`_3.avi`……不实现 OpenDML（AVI 2.0）。
- 文件只有在关闭时才会写入索引和最终大小。崩溃留下的文件没有索引，部分播放器打不开（ffmpeg 可以按顺序读）。

## 2. 帧节奏

编辑器的帧率不固定，视频的帧率是固定的。`VideoPacing` 有两种：

- **RealTime（编辑器）：** 视频按实际速度播放。第 n 帧视频覆盖 [n/fps, (n+1)/fps)。某个渲染帧在 t 时刻显示，它就是 `VideoFramesStartedBy(t) − 1` 号视频帧。中间的空档用上一帧重复填满（上一帧在屏幕上停留了那么久）。同一个视频帧时间内又来了一帧，就丢掉。**[已验证：`TestRealTimePacing`]**
- **EveryFrame（`--record`）：** 每个渲染帧就是一个视频帧，与渲染耗时无关。脚本运行的相机每帧走固定的步长，这样录出的视频可以复现。**[已验证：`TestEveryFramePacing`]**

## 3. 线程与 GPU 回读

```
渲染线程                         编码线程（1–4 个）                 写入（同一时刻只有一个线程）
ClaimFrameAt(t) → 是否录这一帧
  命令缓冲末尾：SceneLdr → 回读缓冲
2 帧后，该帧槽的 fence 已等待
  Take() → Submit(帧)  ────────→  转 RGBA8，编码 JPEG  ────────→  按提交顺序写入 AVI
```

- **回读（`VulkanVideoReadback`）：** 每个帧槽（`kMaxFramesInFlight` = 2）一块 host-visible 缓冲。在 ImGui pass 之后把 `SceneLdr` 拷进去（布局 shader-read → transfer-src → shader-read，Capture Viewport 仍然看到原来的布局），再加一个 transfer → host 的缓冲屏障。下次用到这个帧槽时，`AcquireNextImage` 已经等过它的 fence，这时读出。录制不会让 GPU 停顿。内存优先选 `HOST_CACHED`：CPU 要读每一个字节，write-combined 内存读起来慢很多。非 coherent 内存先 invalidate。
- **只回读需要的帧：** `ClaimFrameAt` 在录制命令时占下这一帧对应的视频帧。所以 144 fps 渲染、30 fps 录制时，每个视频帧只拷贝一次，不会因为提交滞后 2 帧而多拷。
- **编码：** 工作线程数为 `min(4, 核数 − 1)`。格式转换（BGRA 交换；HDR 输出时 `RGBA16F` 在 UI 白处截断并做 sRGB 编码，与 `--capture` 相同）和 JPEG 编码都在工作线程上做。编码结果按序号放进 map，谁拿到写锁谁就按顺序写出。
- **背压：** 最多积压 2×工作线程数帧。RealTime 模式下积压满了就丢帧并计数（后面那帧会被重复来补位，时间轴不乱），REC 标志上会显示丢帧数。EveryFrame 模式改为阻塞等待。
- **测速：** 4 核 Linux 容器上，1281×720 编码约 230 fps（90 帧 0.39 s）**[已验证]**。1080p 按像素数估计约 100 fps，高于 30 fps 的录制需要 **[未核对]**。

## 4. 编辑器行为

- **尺寸固定：** 开始录制时，把当前的视口大小写进 `fixedViewportExtent`，停止后恢复原值。录制期间缩放面板或进入全屏，场景仍按原尺寸渲染，再拉伸显示。尺寸不符的帧（开始前已经在途的帧）丢弃。
- **出错：** 写文件失败（例如磁盘满）时，录制自动停止，视口上用红字显示原因，同时写日志。
- **结束：** 停止时先等 GPU 空闲，交出两个帧槽里还没取走的帧（按时间排序），再等编码线程写完并关闭文件。视口上显示“Saved …（时长，大小）”6 秒。退出编辑器（`~VulkanRenderer`）时也会走同样的停止流程。
- **REC 标志：** 视口顶部居中，红点闪烁，显示时长和文件大小，全屏视口下也显示。它由 ImGui 绘制，不进视频。

## 5. 测试

- `miniengine.video_recorder`（只链接 `engine_core`）覆盖：AVI 结构能解析（RIFF 大小、`avih`/`strh` 帧数、`idx1` 每项都指向 `00dc` 块且大小一致、奇数长度帧的补齐）；帧按提交顺序写出（多线程编码）；颜色没有被镜像或交换通道；RealTime 的重复与丢弃；ClaimFrameAt；BGRA 和半精度转换；文件写满后续写到 `_2.avi`，且各文件帧数之和与总字节数正确。ASan/UBSan 下通过，TSan 下连续 5 次通过 **[已验证：Linux, GCC]**。
- 生成的文件 ffprobe 识别为 `mjpeg, 30/1, 90 帧, 3.0 s`，用 ffmpeg 完整解码没有报错，抽出的帧画面正确 **[已验证]**。
- `miniengine.command_registry`：快捷键是 `Shift+F12`，命令在 Tools 菜单里，勾选状态跟随录制，没有后端时命令禁用 **[已验证：Linux 上用 ImGui docking 分支编译运行]**。
- Vulkan 回读和编辑器里的完整流程没有在 GPU 上跑过 **[未核对]**。Windows 上的验证命令：

```powershell
.\miniengine_app.exe --frames 120 --viewport-size 1280x720 --camera-velocity 0.02,0,0 --record captures\test.avi
```
