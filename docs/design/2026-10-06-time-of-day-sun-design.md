# 时间驱动的太阳（北半球轨迹）

日期：2026-10-06

## 目标

场景新增“时间”：给定一年中的第几天、当地太阳时和纬度，太阳（场景里的方向光）沿北半球的真实轨迹移动——东升、正午过正南、西落；光线的颜色和强度随太阳高度变化，低空变红，落到地平线下熄灭。

## 设置（`TimeOfDaySettings`，`SceneEnvironment::timeOfDay`）

| 字段 | 默认 | 说明 |
|---|---|---|
| `enabled` | false（旧场景）；新场景 true | 关闭时太阳保持手动旋转 |
| `hours` | 14.0 | 当地太阳时，[0, 24)，12 点太阳在正南最高 |
| `dayOfYear` | 279（10 月初） | 1..365 |
| `latitudeDegrees` | 35.7（东京） | 钳制到 [0, 90]：轨迹永远是北半球的 |
| `northDegrees` | 0 | 世界里北的方向：0 时北 = -Z、东 = +X、南 = +Z（默认相机身后）；正值绕 +Y 旋转 |
| `timeScale` | 0 | 每真实秒推进的场景秒数，0 = 时间静止；3600 = 一秒一小时 |

YAML 键：`environment.time_of_day.{enabled, hours, day_of_year, latitude_degrees, north_degrees, time_scale}`。没有该节点的旧场景读成关闭，太阳位置不变。

默认值选成 14:00 / 10 月初 / 东京：太阳高度约 40°、方位约 220°（西南），接近原来手摆的“35° 高、正南、在默认相机身后”。

## 太阳位置（`engine/scene/sun_position.h`）

- 赤纬：Cooper 余弦拟合 δ = −23.44° · cos(2π (N + 10) / 365)，误差约 0.5°。
- 时角 H = 15° · (hours − 12)。
- 东-北-天坐标中的太阳方向（球形地球，不含大气折射与时差方程）：
  - E = −cos δ sin H
  - N = cos φ sin δ − sin φ cos δ cos H
  - U = sin φ sin δ + cos φ cos δ cos H
- 世界坐标：东轴 = R_y(north)·(+X)，北轴 = R_y(north)·(−Z)，天 = +Y。
- 方向光的欧拉角（XYZ，与 `BuildLightRotation` 一致，光沿局部 −Y 照射）：Rx(x)·Rz(z)·(0,−1,0) = (sin z, −cos z cos x, −cos z sin x)，解得 z = asin(d.x)，x = atan2(−d.z, −d.y)，y = 0。

“北半球钳制”就是纬度钳制到 [0, 90]：负纬度当作赤道。纬度 ≥ 23.44° 时正午太阳一定在正南；在热带（0–23.44°）夏季正午太阳可以在北边，这是北半球的真实情况，不做额外处理。

## 谁转动太阳

`EditorScene::ApplyTimeOfDay()`：时间开启时，把最亮的方向光（即渲染器用来照亮天空、投射阴影的那个）的 `rotationDegrees` 设成上面的欧拉角。调用点：

- `SetEnvironment`（面板修改、时间推进都走这里）；
- `ApplySceneData`（加载场景）；
- `AddDefaultSunAndSky`（新场景默认开启）。

时间开启时手动旋转太阳会在下一次时间变化时被覆盖。`timeScale > 0` 时 `EditorRenderBackendBase::TickSharedFrame` 每帧推进时钟（单帧最多计 0.25 s，卡顿不会让太阳跳过半个天空），在 24 h 处回绕。

## 光线（颜色/强度）

不在实体上改颜色/强度——光的强度仍是大气层顶的照度。渲染器已有的逻辑在 Atmosphere 模式下乘以相机高度处到太空的透射率（`ComputeTransmittanceToSpace`）：太阳低时变红变暗，地平线以下为 0。现在时间开启时，None / HDRI 模式也乘这个透射率（用场景的大气参数），所以没有画天空也一样会有日落和夜晚。天空、云、高度雾、环境探针都已经跟随太阳方向。

## 已知限制

- 太阳时而非钟表时间（无经度/时区/时差方程，误差最多约 ±16 分钟）。
- 无月光：夜里只剩天空残光与自动曝光。
- 时间推进时环境探针每帧重新捕获（与手动旋转太阳相同的已有行为）。

## 测试

`tests/scene_environment_tests.cpp`：赤纬两至点、正午高度 = 90 − φ + δ 且方位 180°、上下午关于子午线对称、午夜下中天、北极圈夏季午夜太阳、纬度/时间/日期的钳制与回绕、世界坐标映射（北 = −Z，东 = +X，旋转 north）、欧拉角往返、启动场景太阳随时间转动、关闭后不动、加载场景按时钟放置、YAML 往返与缺节点兼容。
