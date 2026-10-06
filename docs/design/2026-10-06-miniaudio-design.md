# miniaudio 音频支持

日期：2026-10-06

## 目标

给引擎加一个声音层，作为以后车辆引擎声、轮胎声、环境声的基础。这一轮只做底座：库接入、引擎内的播放接口、编辑器里的音量与试听，不做任何具体的游戏音效。

## 依赖

- vcpkg 的 `miniaudio` port（0.11.25，manifest baseline 里就是这个版本），只装 `miniaudio.h`。
- 实现在 `engine/audio/miniaudio_impl.cpp` 里编译一次（`MINIAUDIO_IMPLEMENTATION`），按 C++ 编译，免得为一个文件给工程开 C 语言。
- Ogg Vorbis 用 vcpkg `stb` 自带的 `stb_vorbis.c`：按 miniaudio 文档的顺序，先以 `STB_VORBIS_HEADER_ONLY` 包含声明，再包含 miniaudio 实现，最后包含 stb_vorbis 实现。miniaudio 看到 `STB_VORBIS_INCLUDE_STB_VORBIS_H` 才打开 Vorbis 解码。
- 第三方实现文件用 `/W0` 编译，不把它们的警告混进工程。

## 模块

`engine_audio`（`engine/audio/`），公开头文件只用 glm 和 `std::filesystem`，miniaudio 是 private 依赖。

- `audio_file.h`：`IsAudioFilePath`，按扩展名认 `.wav/.mp3/.flac/.ogg`。单独一个编译单元，资产浏览器（`engine_asset`）只链接这个对象，不会把 miniaudio 拖进去。
- `audio_engine.h`：`AudioEngine`，pimpl 包着 `ma_engine`。
  - `AudioOutput::Device`：系统默认播放设备，混音在 miniaudio 的音频线程上跑。
  - `AudioOutput::None`：不开设备，`Render()` 拉多少帧才混多少帧。测试用它得到确定的输出；`--frames` 的脚本运行也用它，不出声。
  - 声音以 `SoundId` 管理（从 1 递增，不复用），`ma_sound` 放在堆上不移动（miniaudio 的节点图持有它们的地址）。
  - `SoundDesc`：spatial / looping / stream / volume / pitch，空间声音另有位置、min/max distance、rolloff、doppler。衰减模型固定为反距离（`ma_attenuation_model_inverse`），1 m 参考距离下增益为 1/d。
  - 非流式声音用 `MA_SOUND_FLAG_DECODE`：资源管理器按路径缓存解码结果，同一文件多次创建只解码一次。流式（`stream = true`）给音乐和长环境声。
  - `PlayOneShot`：一次性播放，`Update()` 在播完后释放。
  - `StartPreview/StopPreview`：资产浏览器的试听槽，一次一个、流式、不定位。
  - 文件路径在 Windows 上走 `ma_sound_init_from_file_w`，非 ASCII 文件名不依赖进程代码页。

### miniaudio 行为上的几个坑

- 一个非循环声音到结尾时，miniaudio 先置 `atEnd`，下一个处理步才把节点状态改成 stopped；`ma_sound_start` 对仍处于 started 的声音什么都不做。所以 `IsPlaying` 是 `is_playing && !at_end`，`Play` 在 `at_end` 时先 `ma_sound_stop` 再 `start`（start 会自己 seek 回开头）。
- 声音的 cursor 是文件读到的位置，比混音输出超前一个处理块（实测 320 帧）。
- 混音图里一个声音都没有时 `ma_engine_read_pcm_frames` 返回 0 帧；`Render` 把没读到的部分补零。
- 流式 Ogg 走 stb_vorbis 的 push 接口，拿不到总长度，`LengthSeconds` 返回 0。整段解码的 Ogg 有长度。

## 编辑器接入

- `EditorApplication::Run` 在建窗口前创建 `AudioEngine`，放在 `RendererSharedState::audio`；设备打不开只记警告，编辑器照常运行，Preferences 里显示原因。`--no-audio` 不创建。
- `EditorRenderBackendBase::TickSharedFrame`：相机更新之后把听者放到相机位置和朝向（`Camera::GetForward`、`worldUp`），然后 `Update()`。驾驶时相机就是追车相机，所以听者跟车。听者速度给 0：自由相机会瞬移，用相机位移算速度会让多普勒乱跳；以后有车声时由声源自己的速度产生多普勒。
- Preferences 窗口新增 Audio 一节：主音量（0–100%）、静音、输出设备。存进 `miniengine.settings.json` 的 `audio.master_volume` / `audio.muted`；旧文件没有这一节时音量为 100%。
- 资产浏览器：新增 `AssetType::Audio`（图标是 Font Awesome 的 `music`，由 `tools/editor_icons/fontawesome_to_svg.py` 生成），双击或点预览区的 “Play / Stop” 试听；再点同一个文件停止。

## 验证

`miniengine_audio_engine_tests`（`miniengine.audio_engine`）全部用 `AudioOutput::None` 跑：

- 扩展名识别；缺失文件、不能解码的文件报错并带文件名。
- 0.5 s、振幅 0.5 的 440 Hz 正弦 WAV：长度 0.5 s；Play 前静音；播放时左右声道 RMS 都是 0.354；音量 0.5 时 0.177；主音量 0 时静音；Stop 后停止并回到开头。
- 非循环声音到结尾自动停止、可再次从头播放；循环声音越过结尾继续发声。
- 空间化：听者朝 -Z，声源在 +X 时右声道明显大于左声道，转身后反过来；2 m 与 8 m 的电平比 4.0（1/d）。
- 一次性播放和试听播完后自动释放。
- `tests/fixtures/audio/` 里 0.25 s 的 OGG/FLAC/MP3 正弦（python soundfile 生成），整段解码和流式各播一次，电平接近 0.354。
- 中文与西里尔字母文件名能打开。
- 机器上有播放设备时再打开一次真实设备并检查格式（没有设备不算失败）。本机：`LG ULTRAGEAR+ (NVIDIA High Definition Audio)`，96 kHz，双声道。

`engine_settings` 的音量/静音往返在 `miniengine_command_registry_tests` 里。

## 以后

- 车辆声音：按转速和负载混合发动机采样（AC 的声音是 FMOD bank，读不了，需要自己的采样），轮胎尖叫按滑移量。
- 声音组件放进场景（实体上的循环环境声）。
- 混响 / 遮挡、多个听者（分屏）暂不考虑。
