# Assetto Corsa 车辆声音（FMOD bank → miniaudio）

日期：2026-10-10

## 目标

把 R34（`ks_nissan_skyline_r34`）的原厂声音导入引擎，用 miniaudio 播放：开车时有引擎、换挡、涡轮、限转器、回火、轮胎滑移、风声、胎噪、车身异响，并且像 AC 一样按转速/油门交叉淡化、按转速变调。

## AC 的声音格式

每台车一个 FMOD Studio bank：`content/cars/<car>/sfx/<car>.bank`。事件名不在 bank 里，在 `content/sfx/GUIDs.txt`（`{guid} event:/cars/<car>/engine_ext`）。

bank 是 RIFF `FEV ` 文件：前半是事件布局（chunk 树，LIST 带 4 字节类型，chunk 按偶数字节对齐），最后的 `SND ` chunk 里是 FSB5 采样库。格式没有公开文档，以下是从 Kunos 的 bank 逆向得到的（`engine/audio/fmod_bank.cpp`）。

### FSB5

- 头 60 字节（version 1）：样本数、样本头总长、名字表长、数据长、mode。R34 是 mode 2 = PCM16，44 个样本，全部 44.1 kHz（频率码 8）。
- 每个样本头 64 位：bit0 有扩展块，bit1-4 频率码，bit5 双声道，bit6-33 数据偏移（×16 字节），bit34-63 帧数。扩展块：类型 1 声道数、2 采样率。
- 只读 PCM（8/16/float）；Vorbis 等压缩 bank 报错（Kunos 的 bank 都是 PCM16）。

### 布局记录

列表的编码：16 位 `count*2+1`，非空时再跟 16 位元素大小，然后是元素；空列表只有 2 字节 `0x0001`。

| chunk | 内容 |
|---|---|
| `PRMB` | 参数：guid、名字（偏移 21 的 u16 长度 + 字符串）、min、max、default、seek speed（第 5 个 float） |
| `WAV ` | waveform guid → FSB 样本序号（偏移 22） |
| `LIST WAIT` = `WAIB` + `INST` | 单乐器：WAIB = 乐器 guid + waveform guid |
| `LIST MUIT` = `MUIB` + `PLST` + `INST` | 多乐器（随机挑一个）：PLST 偏移 8 的列表 = 成员乐器 guid + 权重 |
| `INST` | 16 音量 dB，20 音高半音，24 循环次数（-1 循环），53 自动变调参数 guid，69 参考值，73 最小音高，83 输出总线 guid |
| `LIST GBUS/MBUS` = `GBSB/MBSB` + `BUS ` | 组总线/事件主总线：GBSB 偏移 18 父总线；BUS 倒数第 16 字节是静态音量 dB |
| `PMLB` | 参数表（parameter sheet）：参数 guid、事件 guid、偏移 48 列表（乐器 guid、起点、长度） |
| `TLNB` | 时间线：时间线 guid、事件 guid，偏移 32 起两个乐器列表（乐器 guid、起点、长度，单位是 48 kHz 帧）；同一 `LIST TMLN` 里有 `LIST TRAN` 就是循环区 |
| `CTRL` | 自动化：控制器 guid、目标（乐器或总线）、参数、曲线、属性（0 音量、1 音高、4 淡入淡出） |
| `CURV` | 曲线点：x、y、shape |
| `PROP` + `MAP ` | 控制器的取值映射：音量是 FMOD 推子刻度（-42→-80 dB、-20→-20 dB 之上线性），音高是 ±1 → ±24 半音 |
| `EVTB` | 事件 guid（时间线、主总线等） |

不读：效果器（`BEFX` 等）、调制器（`MODU`）、快照。

### 验证

- R34 的 16 个事件全部读出（engine_ext/int、gear_ext/int、turbo、limiter、backfire_ext/int、skid_ext/int、wind、wheel、bodywork、horn、door、gear_grind）。
- engine_ext 在 `rpms`（0–20000）上放 22 个循环样本，相邻样本用属性 4 曲线交叉淡化；油门通过组总线的音量曲线在 on（进气）/off（排气）两组之间切换。
- 自动变调参考值的验证：把每个引擎循环原速播放测点火频率，RB26 六缸 = rpm/20 Hz，19 个样本全部与参考转速一致（误差 1–2%，录音本身的偏差），例如 idle 参考 1359 rpm → 实测 69.5 Hz（期望 68）。
- 0→8000 rpm 扫频渲染：基频全程跟随转速（比值 1.02–1.03 恒定），电平在交叉淡化处平滑（±2 dB），没有断档。

## 导入后的格式

`<模型>.sounds.yaml`（模型 glTF 旁边）+ `sounds/*.wav`（PCM16 原样写出，R34 约 20 MB）。YAML 是 `SoundBank`（`engine/audio/sound_bank.h`）：事件 → 参数、总线（父、音量、自动化）、乐器（clips、所在参数表或时间线、区间、循环、音量、音高、自动变调、总线、自动化）。曲线的映射表随自动化一起写出，运行时不需要知道 FMOD 的刻度。

导入途径：
- kn5 导入车时自动导入（`ConvertToGltf` 读完 data.acd 之后，找 `sfx/*.bank` 和 `../../sfx/GUIDs.txt`），报告里 `carSounds` / `carSoundsProblem`。
- 已导入的车：`miniengine_ac_sounds import <AC 车目录> <模型.gltf>`（用户的 R34 就是这样补上的）。
- 工具还有 `dump`（列出事件）和 `sweep`（离线渲染转速扫频 WAV，用来听交叉淡化）。

## 播放（`SoundEventInstance`）

按 FMOD 的语义：
- 参数表上的乐器：参数在区间内时播放（区间半开，触到参数上限的区间包含上限）。循环乐器进入时开始、离开时停止；一次性乐器每次进入触发一次（事件开始时就在区间里也触发）。
- 时间线乐器：事件开始后到起点就播放；有循环区的时间线，乐器循环播放直到事件停止。
- 每帧：音量 dB = 乐器音量 + clip 音量 + 乐器与各级总线的音量自动化 + 各级总线静态音量；线性增益再乘所有淡入淡出曲线和事件音量。音高 = 2^(半音/12) × 自动变调（最小值到参考值之间线性：`min + (1-min)·(v-pmin)/(ref-pmin)`）× 事件音高。
- 曲线 shape 的公式 FMOD 没公开，用 `t^(4^shape)`：两端不变，正值先慢后快、负值先快后慢；FMOD 默认 ±0.25 的交叉淡化因此接近等功率。这是唯一的猜测项。
- 参数有 seek speed 时按速度逼近目标。
- 关掉多普勒：听者是跟车的相机，没有速度，否则车速会让声音变尖。

## 开车时（`VehicleSounds`，`engine/editor/services/vehicle_sounds.*`）

开始驾驶时，若模型旁有 `.sounds.yaml` 且有音频输出就加载（日志 `sounds from ...`）。每帧在相机放好之后更新：

| 参数 | 来源 |
|---|---|
| `rpms` | 遥测转速 |
| `throttle` / `brake` | 驾驶者的踏板 |
| `boost` | 遥测涡轮增压（0–1） |
| `speed` | 车速 km/h（风、胎噪） |
| `susp_travel_speed` | 四轮悬挂行程的最大速度（m/s，封顶 1） |
| `decay` | 距上次碰限转器的秒数（限转器 pop 只在前 20 ms） |
| `Event Cone Angle` / `Distance` | 车头方向与到相机方向的夹角、距离 |
| `inflation` 1、`suspension_damage` 0、`air_pressure` 1.1 | 完好的车、无尾流 |

- 驾驶舱视角用 `_int` 事件（不定位），其他视角用 `_ext`（在车的位置）。turbo、limiter、wind、wheel、bodywork 全程播放。
- 挡位变化 → gear 事件，`state` 1 升挡 / 0 降挡。
- 6500 rpm 这类高转（≥ 4500）下 0.35 s 内从 ≥ 0.7 油门松到 ≤ 0.1 → backfire（冷却 0.6 s）。AC 什么时候触发回火没有数据，这是近似。
- 轮胎滑移：任一着地轮的侧偏角 5°→12° 或滑移率 0.12→0.42 映射到 0→1，作为 skid 事件的音量（音高 0.9–1.1），为 0 时停止。
- 暂停时全部静音但保持播放位置。
- horn、door、gear_grind 没有触发源，不加载。

## 测试

- `miniengine_sound_bank_tests`：曲线/映射、YAML 往返、合成正弦的事件播放（交叉淡化、自动变调、一次性触发、循环时间线、seek speed）；装了 AC 时读 R34 bank（44 样本、16 事件、22 个转速乐器、idle 参考 1359 rpm 等）并做全转速扫频不断档检查。`MINIENGINE_AC_ROOT` 可指定 AC 目录。
- `miniengine_vehicle_sounds_tests`：遥测→参数与触发（挡位、回火、限转器、滑移、悬挂、cone angle），以及 R34 的 `VehicleSounds` 在内外视角和暂停下的电平。

## 未做

- 效果器（EQ、距离滤波、空间化器参数）和调制器（随机音高、AHDSR）。
- 喇叭/车门输入、错挡（gear_grind）、轮胎爆胎与悬挂损坏。
- 引擎声的定位用车身原点，AC 的 `sounds.ini` 里有发动机/排气位置可以以后读。
