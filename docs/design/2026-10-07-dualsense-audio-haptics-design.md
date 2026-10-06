# DualSense 音频触觉（USB）

## 目标

驾驶时用 DualSense 的音圈致动器表现发动机、路面纹理和轮胎打滑，替代兼容 rumble 那种粗糙的双马达模拟。先不出声音，只驱动致动器。

## 接口

- DualSense 通过 USB 连接时，Windows 上会出现一个 4 声道、48 kHz 的播放设备 "Speakers (DualSense Wireless Controller)"。第 1、2 声道是手柄扬声器和耳机，**第 3、4 声道分别是左、右致动器**。
- `GamepadHaptics`（engine_audio）：用 miniaudio 按名字找到这个设备，以设备原生格式（4 声道）打开，第 1、2 声道写静音，第 3、4 声道写 `HapticsSynth` 生成的波形。音频线程只用 `try_lock` 取参数，永远不会阻塞。
- effects 报告（`EncodeDualSenseEffects`）：只要设置了 rumble emulation 的 0x01/0x02 位，手柄就会关掉音频触觉。所以 `GamepadFeedback::audioHaptics` 为 true 时这两个位都不设、马达值写 0，扳机效果照常。这一行为与 SDL `SDL_hidapi_ps5.c` 的注释一致："Leaving emulated rumble bits off will restore audio haptics"。
- 蓝牙连接时没有这个音频设备，自动退回兼容 rumble。

## 合成（HapticsSynth）

| 声部 | 波形 | 参数 |
|---|---|---|
| 发动机 | 点火频率的正弦，叠加 0.35 倍二次谐波；每个点火周期的幅度随机 ±12% | 点火频率 = rpm / 60 × 缸数 / 2（R34 的 RB26 是 6 缸：1000 rpm 对应 50 Hz，7000 rpm 对应 350 Hz） |
| 断油 | 发动机以 16 Hz 方波斩断，剩 10% | 转速 ≥ 96% 且油门 > 0.3 |
| 路面 | 白噪声经带通（Q 0.7），中心频率 20 + 6 × 车速(m/s) Hz，上限 250 Hz | 左右两侧独立 |
| 打滑 | 白噪声经带通（Q 2.5），中心频率 70 + 10 × 滑移速度 Hz；再叠 24 Hz 的幅度抖动（模拟粘滑） | 左右两侧独立 |
| 冲击 | 45 Hz 衰减正弦，τ = 35 ms，从过零点起振 | 颠簸、路肩、换挡 |

各电平经 12 ms 平滑过渡，混音后用 tanh 软限幅。

## 车辆映射（ComputeVehicleAudioHaptics）

- 路面粗糙度：每个车轮的压缩量 = 悬挂压缩 + 轮胎变形，逐帧求速度；减去 0.25 s 的慢速分量（车身侧倾和俯仰）后，再取 0.15 s 的 RMS。电平 = 0.10 × 车速/30（沥青颗粒感）+ 0.60 × RMS/0.25。
- 颠簸：残余速度 > 0.25 m/s，且超过当前粗糙度的 2.5 倍时触发一次冲击，每个车轮 80 ms 冷却。车轮离地后再落地也算在内。
- 打滑：刷子轮胎模型用滑动份额（0.15 到 0.75 映射为 0 到 1），其他轮胎模型用滑移率和滑移角；再乘以滑移速度/3 m/s 的渐入系数。
- 左右侧按车轮相对车身右轴（−X）的位置划分。
- Vehicle 面板 → Gamepad Feedback：总开关 "DualSense HD Haptics (USB)"，另有 Engine / Road / Tyre Slip 三个强度滑条（0 到 2）。

## 验证

- `miniengine_haptics_synth_tests`：发动机过零次数、断油包络、左右隔离、冲击衰减、范围，以及只写第 3、4 声道。
- 加 `--device` 参数时，会在连着 USB 的 DualSense 上依次播放各个声部，用手感检查。
- `miniengine_gamepad_feedback_tests`：音频触觉模式下 effects 位为 0x0C；还覆盖颠簸只敲对应一侧、打滑侧、断油和换挡。

## 未做

- 地面类型（草地、砂石）没有单独的纹理：物理层还没有把 per-wheel surface 暴露出来，目前只靠悬挂运动体现。
- 蓝牙下的音频触觉（Sony 私有 HID 报告）。
- 手柄扬声器的声音。
