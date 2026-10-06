# 动态云层与场景风

## 目标

云层原来是静止的：羽流图（plume map）只在启动时生成一次，噪声采样位置与时间无关。这次让它动起来：

1. **随风移动。** 整个云层被风带着走。风是场景级的设置，并作为 API 暴露（`engine/scene/wind.h`），以后烟、旗子、草、粒子都向它询问同一个风。
2. **动态生成。** 每朵羽流都有自己的生命周期：从云底长出、成熟、消散，再在别处长出新的。
3. **翻滚。** 云块（billows）相对羽流向上升，像对流中的积云表面那样翻涌。

## 1. 风（`WindSettings`，`engine/scene/wind.h`）

`SceneEnvironment::wind`：

- `speed`：10 m 参考高度上的平均风速（m/s，气象标准高度），默认 5，范围 [0, 70]。
- `fromDegrees`：风的来向，从北顺时针的方位角（0 北风，90 东风，180 南风，270 西风，默认 270 西风，吹向东）。北在世界中的方向沿用 `TimeOfDaySettings::northDegrees`（0 时北为 -Z、东为 +X），与太阳轨迹共用一个北。

风速随高度按中性大气的幂律增长：`v(h) = v10 (h / 10 m)^(1/7)`（开阔地），到边界层顶（1000 m）后保持不变，即 1.93 倍 v10；低于 0.5 m 时按 0.5 m 算。API：

```cpp
WindSettings ClampWindSettings(const WindSettings&);
glm::vec3 WindDirection(const WindSettings&, float northDegrees);   // 吹向的世界方向（水平单位向量）
float     WindSpeedAt(const WindSettings&, float heightMeters);       // 某高度的风速
glm::vec3 WindVelocity(const SceneEnvironment&, float heightMeters); // 某高度的风速矢量，m/s
```

GPU 侧：`EnvironmentUniformData::wind`（`ubo.wind`）xyz 为吹向的世界方向，w 为 10 m 风速；着色器按同一幂律换算高度。

编辑器 Scene 面板新增 **Wind** 折叠区（速度、来向，并显示世界方向和云层中部的风速）；场景 YAML 新增 `environment.wind { speed, from_degrees }`，旧场景没有这个节点时用默认的微风。

## 2. 云的运动状态（`CloudMotion`）

渲染器每帧用 `packet.deltaSeconds` 推进一个 `CloudMotion`（`VulkanRenderer::m_cloudMotion`）：

- `windKm`：风累计吹动云层的世界 x、z 距离（double）。速度取云层中部高度（base + thickness/2）的风，默认设置下约 9.7 m/s。
- `riseKm`：大云块相对羽流上升的累计距离，速度 `CloudSettings::updraft`（默认 1.5 m/s）；小云块升得快一倍（`kCloudDetailRiseScale = 2`），两层之间的相对运动就是翻滚。
- `lives`：四个羽流尺度各自的生命时钟（以“生命”为单位，只保留小数部分）。
- `stepMeters`：本帧风位移，给时间重建用。

积分而不是用 `时间 × 速度`：中途改风速、风向、上升速度或寿命时，云只是改变运动，不会跳变。长卡顿最多按 0.25 s 算（与时间轴相同）。`CloudSettings::timeScale` 让云的时钟比真实时间快（默认 1，0 冻结），用来做延时摄影。

上传时（`SetCloudMotion`）位移换算成各自纹理的平铺单位并在 double 中取 fract，所以时钟跑多久都不会损失精度：

- `cloudShapeMotion.xyz` / `cloudDetailMotion.xyz`：大/小云块体积纹理的偏移（风 + 上升）；
- `cloudShapeMotion.w`、`cloudDetailMotion.w`：羽流图的 u、v 偏移（只有风）；
- `cloudLife`：四个尺度的生命相位；
- `cloudMotionStep`：xy 本帧风位移（米），w 云时钟（秒）。

`EnvironmentUniformData` 由 27 个 vec4 增加到 32 个。着色器在 `p * freq - offset` 处采样，图案即随 +offset 移动；云影图每帧重算，自动跟着动。

## 3. 羽流的生命周期

每朵羽流（`cloud_weather.comp`，C++ 镜像 `CloudWeatherTexel`）的相位 = 所属尺度的时钟 + 一个随机起点（新的随机槽 `slot + 7`），生命曲线

```
L(p) = smoothstep(0, 0.3, p) · (1 - smoothstep(0.55, 1, p))
```

前 30 % 从云底长到全高，站到 55 %，再在剩下的 45 % 里沉回去。实现方式是把抛物面整体下沉：`dome = h (L - (d/r)²)`。抛物面穿过云底下沉时，底面足迹随之收缩，所以长出时是一个塔从小底面升起、变宽；消散时高度和底面一起缩小到无。L = 0 时整个穹顶都在云底以下，不可见；相位回绕处 L 两侧都是 0，连续。

不同尺度寿命不同（`kCloudPlumeLifeScale = {2.0, 1.4, 1.0, 0.7}` × `CloudSettings::lifetime`，默认 15 分钟）：大云团 30 分钟，最小的羽流约 10 分钟。按默认值，大云团的塔顶上升约 4–5 m/s，与晴天积云/浓积云的观测量级一致。云团的每个小塔（turret）在云团的生命内还以两倍频率在 60 %–100 % 高度之间起伏，相位各不相同，所以塔是一个个冒出来的，而不是整团同步升降。

### 重建与成本

羽流图在生命相位变化时重建（`VulkanAtmosphere::Record` 比较上次构建的相位，云关闭时不重建）。

- **羽流表。** 直接把原来的生成器每帧跑一遍要 ~1.9 ms（720p 测得）：每个纹素要对 36 个候选格子重新做几百次哈希（聚集噪声、位置、形状、小塔）。这些随机量与时间无关，所以改成 CPU 启动时算一次羽流表（`BuildCloudPlumeTable`，每个尺度每个格子一项：中心、半径、高度、生命起点、小塔，9 个 vec4，共 21 760 项、3.1 MB），上传为设备本地存储缓冲（atmosphere set 1 binding 14，staging 拷贝在第一帧，之后释放）。每帧的 `cloud_weather.comp` 只做穹顶和平滑最大值。C++ 镜像 `CloudWeatherTexel` 用同一个 `CloudPlumeCellAt`，GPU 和测试读的是同一份数据。重建降到 ~0.75 ms。
- **刷新间隔。** 塔顶升降最快约 7 m/s，所以羽流图只在云时钟每过 0.1 s 时刷新（`kCloudMapRefreshSeconds`，`CloudMotion::mapLives`）：两次之间顶部移动不到 1 m，在 1.5 km 外不到一个像素。实时速度下约每 6 帧一次，摊到每帧 ~0.03 ms（Atmosphere 计时：冻结 0.18–0.20 ms，实时 0.20–0.22 ms）；延时摄影（时间倍速高）下每帧都刷新。

### 覆盖率

羽流处在不同阶段，平均高度只有满高的约 62 %，同样的偏移下覆盖率会下降。`CloudCoverageOffset` 的节点重新测量：在 512² 采样、四组不同的生命相位上合并统计。任意时刻测得的覆盖率与要求的一致：5 % 时偏差不超过 0.03，30 % 以上偏差约 0.002（`CoverageOffsetMatchesTheMap` 在另外两组相位上验证 ±0.04）。

视觉上的变化：同样 0.45 的覆盖率下，天空里是各阶段的积云混在一起（有正在长的小塔，也有正在塌的扁云），不再全是成熟的大塔。

## 4. 时间重建与环境探针

- **重投影。** `cloud_resolve.comp` 把当前像素的云位置减去本帧风位移，再投到上一帧：被风吹动的云不会拖影。云块上升不是刚体运动，不计入；它比风慢得多，由 3×3 邻域钳制吸收。
- **环境探针。** 它的 `CaptureKey` 去掉每帧都变的运动字段，改用 `floor(云时钟 / 2 s)`：静止的太阳下，探针每 2 秒（云时间）重拍一次，而不是每帧。默认风下云在两次之间移动约 20 m，从 1.5 km 下看约 0.01°。

## 5. 设置

`CloudSettings` 新增（YAML：`updraft`、`lifetime`、`time_scale`），编辑器 Clouds 区新增 Motion 小节：

| 字段 | 默认 | 范围 | 含义 |
|---|---|---|---|
| `updraft` | 1.5 m/s | [0, 10] | 云块相对羽流上升的速度 |
| `lifetime` | 15 min | [1, 240] | 中等羽流的寿命 |
| `timeScale` | 1 | [0, 3600] | 云的时钟倍速，0 冻结 |

## 6. 验证

- `tests/volumetric_clouds_tests.cpp`：`PlumeLives`（曲线端点、平滑、回绕，羽流图随相位变化、整周期后复原、仍然平铺），`Motion`（风速随高度、一秒的位移和上升、各尺度时钟、卡顿钳制、timeScale 0 冻结、长时间运行相位仍在 [0, 1)），`Wind`（参考高度、幂律、边界层顶、方位约定、北向旋转、钳制），覆盖率在其他时刻的复测。
- `tests/scene_environment_tests.cpp`：风和云运动字段的 YAML 往返。
- 画面（`out/cloudcap`，720p，EV 13 固定）：实时速度下连续 20 帧的逐帧平均差 0.5–1.25（8 位灰度），与冻结时的噪声底 0.45–1.1 相同，没有刷新带来的跳变；而 19 帧累计变化 6.96，冻结时 0.58。60 倍速下相隔数秒的三帧，云整体漂移并有新的羽流长出、旧的消散。

## 7. 未做

- 消散阶段只做了下沉收缩；真实积云消散时更像边缘蒸发、变薄、变破碎，可以在消散段同时降低密度。
- 风向不随高度偏转（Ekman 螺旋），也没有阵风；需要时可在 `WindVelocity` 里加。
- 羽流图刷新是整张重建，每 ~6 帧有一帧多 ~0.75 ms；需要更平稳时可以分块（每帧 1/6）更新。
- 延时倍速很高时环境探针按云时钟每 2 s 重拍，等于几乎每帧重拍（~1 ms）；可以改为同时按真实时间限频。
