# 拍照模式：离线路径追踪、完整 DLSS、保存文件夹

日期：2026-10-09。接 `2026-10-09-photo-mode-design.md`、`2026-10-09-photo-tiles-design.md` 和
`2026-10-09-path-tracing-offline-mode-design.md`。

## 需求

用户："Photo Mode 也接上离线路径追踪，保证能用完整 DLSS"；随后加了"照片模式能选择保存文件夹还能一键打开"。

之前拍照视图和四机位视图一样：不走路径追踪（光栅 + 光追效果），不走 DLSS（引擎自己的 TAA）。原因有两个：路径追踪的
累积状态（`PathTraceAccumulation`、几何纪元）是渲染器上只给视口的一份；NGX 的 DLSS 特性也只有一个，按视口的尺寸建。

## 做法

### 每个视图自己的状态

- `VulkanSceneView` 多了路径追踪的静止检测（`pathTraceAccumulation`、`pathTraceGeometryEpoch`）、这一帧的离线进度
  （`offlineProgress`），和 DLSS 的 `dlssSlot` / `dlssActive` / `dlssResetPending`。`ResetHistories` 一并重置它们，
  所以分块换块时（`SceneCaptureView::resetHistory`）累积、DLSS 历史都从头开始。渲染器上原来的
  `m_pathTraceAccumulation`、`m_pathTraceGeometryEpoch`、`m_dlssResetPending` 删掉，视口用 `m_view` 上的那一份。
- `UpdatePathTracing` 改成按视图：静止检测用这个视图自己的矩阵和它实际渲染的设置；状态行仍只写视口的。

### 两个 DLSS 特性

`VulkanDlss` 的特性按槽位（`DlssFeatureSlot::Viewport`、`Photo`）各一份：`RenderExtentFor`、`EnsureFeature`、
`ReleaseFeature`、`HasFeature`、`HasRayReconstruction`、`Evaluate` 都带槽位（默认视口的，原调用不变）。NGX 的参数
对象共用，特性句柄、尺寸、失败记录、最优尺寸缓存各槽位一份。帧上下文多了 `dlssSlot`，TAA pass 用它 evaluate。

拍照视图（`SyncCaptureViews` → `EnsurePhotoDlss`）：按照片设置的模式取 DLSS 的渲染尺寸、建 Photo 槽位的特性
（预设跟视口的 `dlss_preset`），视图的目标按 render / output 两个尺寸建（DLAA 相同，Quality 等更小再放大）；建不出
特性时退回 TAA。模式、预设、光线重构有变化时重置视图历史；没有拍照视图时释放 Photo 槽位的特性。
所以照片可以用 DLAA、Quality 到 Ultra Performance 的超分，以及光线重构，和视口用什么无关，两个特性同时存在。

### 拍照视图跑离线路径追踪

`SceneCaptureView` 多了 `offlinePathTracing`、`samplesPerPixel`、`targetSamples`、`dlssMode`、`dlssPreset`、
`dlssRayReconstruction`、`photoTile`。`PrepareView` 对拍照视图：打开硬件光追、路径追踪和离线模式，样本数取照片的，
反弹数、clamp、候选数等取视口的 `path_tracing_offline` 设置，再经 `EffectivePathTracing` 展开；DLSS 能力取 Photo
槽位的。透明层、32 位累积、停住（hold）等都和视口的离线模式相同（同一个 pass、同一组共享管线）。

### 什么时候算一块拍完

渲染线程的反馈多了 `PhotoViewReport`（这一帧画的是第几块、离线进度、用什么解析），主线程 `ReportPhotoView` 存下。
`PhotoTileDone`：

- 不走路径追踪：照旧渲染 `warmupFrames` 帧；
- 走路径追踪：反馈说**这一块**的样本已满（`OfflineProgress::done`）以后，再渲染 `warmupFrames` 帧让 DLSS/TAA 在
  静止的输入上收敛；反馈里的块号必须是当前块，避免上一块的"已完成"提前结束下一块；
- 路径追踪没运行（没有 ray query、光追场景没就绪）：预热帧数后照光栅结果拍，记警告；
- 场景一直在动（开车、时间推移让太阳动）累积会不断重来：最多 `PhotoPathTraceFrameLimit` = 样本所需帧数 × 4 + 预热帧，
  到了就照当前结果拍，记警告。

### 显存

分块规划在视口的每像素开销（`viewBytesPerPixel`，目标 × 1.5）上加 `PhotoExtraBytesPerPixel`：离线路径追踪 64 B/px，
光线重构 48 B/px。实测：2176x1336 的拍照视图（路径追踪 + RR，DLAA）占 1548 MB，即 532 B/px，而不加时估计约 450 B/px。

### 设置与界面

`PhotoModeSettings`（`photo_mode` 设置）：`offline_path_tracing`（默认开）、`samples_per_pixel`（每帧，1–16，默认 2）、
`target_samples`（每像素总数，默认 1024）、`dlss_mode`（默认 DLAA）、`dlss_ray_reconstruction`（默认开）、`folder`（空 =
项目的 captures/）。每帧样本数上限 16：4K 一帧太多样本时编辑器会卡顿，超过 Windows 的 2 秒还会丢设备。

Photo Mode 窗口：

- **Rendering** 一节：离线路径追踪开关和两个样本数、DLSS 模式下拉（Off (TAA) / DLAA / Quality / Balanced /
  Performance / Ultra Performance）、光线重构开关；没有 ray query / DLSS / RR 时对应控件灰掉并说明。
- 进度条显示"第几块、第几帧、多少 spp"，下面一行"Resolved with DLSS ray reconstruction"等。
- **Save To** 一节：当前文件夹、Choose Folder...（Windows 用 shell 的文件夹选择框 `IFileOpenDialog` +
  `FOS_PICKFOLDERS`，其它平台 SDL 的 `SDL_ShowOpenFolderDialog`，都不可用时退回输入路径）、恢复 captures/、
  **Open Folder**（不存在就先建）、**Open Last Photo**。打开用新的 `OpenInFileBrowser`（`SDL_OpenURL` 的 file URL，
  README 的打开也改用它）。

命令行：`--photo-path-tracing on|off`、`--photo-spp N`、`--photo-samples N`、`--photo-dlss <模式>`、`--photo-rr on|off`
（README 的命令行一节）。

## 验证（RTX 4070 笔记本，Debug 构建；测试时用户自己的 Release 版在跑，GPU 时间不可信）

- R34 道路，1920x1080 照片，4 spp/帧到 256 spp，DLAA + RR：日志 "DLSS ray reconstruction: 1920x1080 -> 1920x1080"、
  "Photo: resolves with DLSS ray reconstruction"，按完成信号结束（无"场景没静止"警告），validation 无报告。
- 同场景 DLSS Quality + RR：1280x720 渲染 → 1920x1080 输出，画面均值与 DLAA 版相差 0.1/255。
- 视口开 DLSS RR（路径追踪）的同时拍 RR 照片：两个特性并存，视口截图均值与没拍照时相同（203.95 / 203.92）。
- 4K 强制切 8 块（`--photo-max-view-pixels 2000000`），路径追踪 + RR：看不到接缝；接缝两侧的跳变 0.13–0.52/255，
  离接缝 10 像素处 0.05–0.65/255（行接缝 1.9 对 1.3）。
- 光栅照片（`--photo-path-tracing off --photo-dlss off`）照旧。
- 单元测试：`miniengine.photo_mode` 新增请求、帧数、显存、文件夹，设置往返包括新字段；`miniengine.photo_mode_panel`
  新增光栅/无光追两张快照；整套 125 项通过（两个长的车辆物理测试另算）。
- 文件夹选择框和打开文件夹要在桌面上手动点（会弹窗口，不在自动测试里）。

## 限制

- 拍照期间场景照常运动；路径追踪照片在开车时会一直重新累积，到帧数上限才拍。暂停模拟仍是后续一步。
- 路径追踪照片期间视口和照片在同一帧里渲染，编辑器帧率会降（每帧样本数越多越明显）。
