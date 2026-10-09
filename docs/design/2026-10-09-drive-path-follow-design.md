# 地图轨迹曲线与自动驾驶测试

日期：2026-10-09

## 背景

悬挂、轮胎、ABS/TC、物理步长一直在改，每次验证“开起来怎么样”都靠手开或者一次性的 probe 脚本（GTASA、BeamNG、坡道各一份）。需要一个能反复跑、能比较的驾驶测试：

- 在地图上画一条轨迹曲线，车按设定速度自动沿着它开（闭环）；
- 把一次驾驶的操作原样录下来再重放（开环），改了车之后 A/B 对比；
- 两种方式都能在编辑器里点按钮跑，也能命令行无头跑，输出逐帧 CSV 和一行总结。

评估过直接接 Lua：引擎里没有任何脚本基础设施，而难点（曲线、跟随控制器、遥测、可复现）用不用 Lua 都要做；控制器要读每帧遥测，放在 C++ 里简单、确定、可测。所以这次做原生功能，控制器写成纯函数，以后接 Lua 时它就是要暴露的 API。

## 数据：场景里的轨迹

场景级（和 minimap、streaming 一样，不是实体组件），`SerializedSceneData::drivePaths`，`IEditorWorld::GetDrivePaths/SetDrivePaths`。

```yaml
drive_paths:
  - name: lane_change
    closed: false          # 闭合：跑圈
    speed_kmh: 80          # 没写速度的点用它
    points:
      - [62.1, 100.0, -20.8]          # x, y, z（米，世界坐标）
      - [62.1, 100.0, -60.0, 120]     # 第四个数：到这个点的目标速度 km/h
```

- 点用 `glm::dvec3`，和双精度物理一致（GTA 地图到 ±9000 m）。
- 曲线是**向心 Catmull-Rom**（α = 0.5）：一定经过每个控制点，不会打结、不会过冲，在地图上点哪儿就过哪儿。开放曲线两端用镜像的虚拟点。
- 采样成 0.5 m 间距的折线（`DrivePathTrack`），每个采样点有弧长 s、切向、曲率、目标速度。

## 速度曲线

1. 每个控制点的速度（没写就用 `speed_kmh`）沿弧长线性插值到采样点；开放曲线终点速度为 0（停车）。
2. 可选横向抓地上限 `lateralGrip`（g，默认 0 = 关）：v ≤ √(a·g/κ)。用来防止设了离谱速度直接飞出去；测极限时关掉。
3. 反向扫一遍，限制减速度 `brakingDecel`（默认 0.8 g）：v_i ≤ √(v_{i+1}² + 2·a·Δs)，车会提前刹到下一个点的速度。闭合曲线扫两圈让首尾接上。
4. 正向扫一遍限制加速度 `accelLimit`（默认 0 = 不限，车自己能加多快就多快）。

## 跟随控制器（纯函数）

`engine/editor/services/vehicle_path_follower.*`：

```cpp
VehicleControls ComputePathFollowControls(
    const DrivePathTrack& track, const PathFollowerSettings& settings,
    const PathFollowerInput& input, PathFollowerState& state, float deltaSeconds,
    PathFollowerOutput* output);
```

- **投影**：车（后轴中心）投到折线上，从上一帧的位置附近找（窗口 ±50 m），闭合曲线跨过终点时圈数 +1。得到弧长 s、横向误差（右正）、航向误差。
- **转向：pure pursuit**。预瞄距离 L = clamp(L0 + T·v, Lmin, Lmax)，默认 L0 = 3 m，T = 0.8 s，范围 4–40 m。目标点在 s + L，转到车身坐标里得到夹角 α，前轮转角 δ = atan(2·轴距·sin α / L)，除以最大转角得到 −1…1。再加转向速率限制（默认 2.5 个满舵/秒，像人手）。不经过转向辅助：测的是车，不是辅助。
- **横向误差积分**：pure pursuit 按不打滑的几何转向，车一转向不足就会在弯里持续偏外。所以再加横向误差的积分（增益 0.008 rad/(m·s)，上限 6°），只在车速 > 2 m/s 时累积。增益 0.03 时会以约 8 s 周期振荡 ±0.8 m；0.004–0.012 都能收敛到约 2 cm。
- **速度：PI**。目标速度取前方 v·0.3 s 处（提前一点反应），误差 e = v_target − v：
  - 油门 = clamp(kp·e + I, 0, 1)，积分只在不刹车时累积，有上下限；
  - 刹车 = clamp(−kb·(e + 死区), 0, 1)，刹车时积分清零；
  - 油门和刹车不同时给。走 `VehicleControls::brake`，不用负油门（那会挂倒挡）。
- **结束**：开放曲线到终点且速度 < 0.5 m/s → 完成；闭合曲线跑满 `laps` 圈 → 完成。横向误差超过 `abortDistance`（默认 8 m）或车翻了 → 失败，并记下原因。

单元测试用运动学自行车模型（带一阶转向滞后和简单纵向模型）跑圆、双移线、闭合圈：误差收敛、速度跟得上、终点停住、偏离太远判失败。

## 实测（R34，BeamNG 网格地图，Release）

| 测试 | 结果 |
|---|---|
| 双移线（偏移 3.5 m），60 km/h | 完成，横向误差 RMS 0.23 m，最大 0.60 m，停在终点 |
| 半径 30 m 圆，50 km/h，2 圈 | 完成，RMS 0.26 m（只用 pure pursuit 时 1.08 m），横向 0.69 g |
| 同一条路径跑两次 | 逐帧完全相同 |
| 重放圆的日志 | 1825 帧每一列都完全相同（按控制重放，已改为按时间片播放） |
| 播放双移线的日志（2026-10-09） | 932 帧，同一时刻位置最大差 0.6 mm（插值和 4 位小数），终点相同 |

无头跑时用 `--viewport-size 320x180`：全分辨率下光追预热时每帧 1–1.7 s，一次要 6 分钟；小视口只要 24 s。

## 驾驶日志（CSV）与重放

`engine/editor/services/vehicle_drive_log.*`：

- 每帧一行：`time, dt, steps, x, y, z, yaw_deg, speed_kmh, target_kmh, path_m, lap, lateral_error_m, heading_error_deg, throttle, brake, steering, hand_brake, gear_shifts, clutch_pedal, manual_gearbox, gear, rpm, long_g, lat_g, yaw_rate_dps, body_slip_deg, abs, tc, wheels_on_ground`，接着是车身 `qw, qx, qy, qz, pitch_deg, roll_deg, right_kmh, vertical_ms`，再是四个轮子（前缀 `fl_ fr_ rl_ rr_`）各 28 列：位姿 `x y z qw qx qy qz`（世界坐标，旋转含滚动）、`contact`、接地点 `cx cy cz`、地面法向 `nx ny nz`、接地纵向/横向 `fx fy fz sx sy sz`、胎体 `carcass_x carcass_y carcass_twist carcass_bend`、`load_n`、`travel_mm`、`slip_ratio`、`slip_deg`。列由 `vehicle_drive_log.cpp` 里的一张列表生成，写和读用同一张表；读时按列名对应，不认识的列跳过。
- 文件头注释行 `# start x y z qw qx qy qz` 记下放车时的精确位姿（不是读回的位姿），`# step_seconds` 记物理步长，`# car`、`# path` 记车和轨迹名，文件末尾 `# summary` 是总结。
- 加速度用两帧世界速度差分（位姿差 / dt），再投到车身前向和右向；垂直速度用两帧高度差。
- **重放 = 按时间片播放**（2026-10-09 改）：不再跑物理，而是按日志的 `time` 列在相邻两帧之间插值（位置线性、旋转 slerp，其余取后一帧），把车身和四个轮子直接放到记录的位置；HUD、镜头、手柄反馈读的也是这一帧（`ShownPose/ShownWheels/ShownTelemetry`）。物理的车在起点等着，重放停止时把物理的车放到播放到的位置。这样回放一定沿原来的线走，和物理是否确定性无关。旧格式（没有车身旋转和轮子列）的日志不能播放，报错让重录。
- 以前按控制重放的问题：车 Reset 后不是全新状态（轮胎温度/磨损、ABS/TC 计时、Jolt 暖启动冲量和接触缓存、每步改写的轮子设置都留着），开环控制一点偏差就越滚越大。`PhysicsWorld::ResetVehicle` 现在把这些全部恢复成新车（测试 `miniengine.vehicle_reset`：开过一段再 Reset 的车和新车跑同一串控制，逐帧位置完全相同；Recover 保留轮胎温度和磨损）。
- 录制从按下按钮时 Reset 开始（车回起点）；如果先开始写日志再点跟随/重放，日志在放车时重新开始，起点是跟随的起点。

## 接入驾驶服务

`VehicleDriveState` 新增：

- `pathFollow`：要跟的轨迹名和设置；会话里放构建好的 `DrivePathTrack`、控制器状态、结果。开始跟随时把车放到曲线起点、朝向切向（`PlaceVehicle`，和 Recover 一样找地面），并把它设为 Reset 的起点。
- `replay`：读进来的帧、播放到的时间和那一刻的帧。
- `logPath`：有就写 CSV（手开、跟随、重放都能写）。
- `fixedFrameStep`：命令行跑时每帧固定 1/60 s，不受帧率影响（和 `--drive-controls` 一样）；编辑器里按真实时间跑。

控制优先级：跟随 > `--drive-controls` > 键盘手柄；重放时不跑物理，控制只用来显示。

结束时日志一行总结：完成/失败、用时、圈数、横向误差 RMS/最大、最大横向/纵向 g、最高速度。

## 命令行

- `--follow-path NAME`：配合 `--drive TAG`，场景加载后开始跟随这条轨迹。
- `--replay-drive FILE.csv`：配合 `--drive TAG`，重放一次驾驶。
- `--drive-log FILE.csv`：把这次驾驶写成 CSV。
- `--path-speed-scale S`：所有目标速度乘 S，用来扫速度。
- 跟随或重放结束后程序退出（即使 `--frames` 还没到），退出码 0 = 完成，3 = 失败。

## 编辑器

- **Drive Paths 面板**：列出场景的轨迹（新建、删除、改名、闭合、默认速度、圈数），点表（位置、速度可编辑、插入、删除），“在车的位置加点”，“录线”（手开一段，每 5 m 记一个点和当时的速度，停止后存成轨迹），“Follow”（正在开的车开始跟随；没在开就先用选中的模型开始驾驶），“Record Log / Replay Log”，跟随时显示进度、实时误差，结束后显示总结。
- **视口**：画出所有轨迹（选中的高亮），控制点画圆点，选中的点有平移 gizmo 可以拖；跟随时画出投影点和预瞄点。

## 不做的事（以后）

- 视口里点击地面放点（需要对场景三角形做射线检测，现在只有物理世界在驾驶时才有）；目前用“录线”和“在车的位置加点”+ gizmo。
- Lua：等出现条件序列类需求（第几圈开 ABS、批量跑一组测试）再接，跟随控制器和日志就是要暴露的接口。
- ISO 标准工况（双移线 ISO 3888、定圆 ISO 4138）的现成生成器：先手画，常用了再加。
