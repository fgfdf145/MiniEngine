# 转向输入平滑（GT7 风格手柄转向辅助）

日期：2026-10-05

## 目标

手柄摇杆直接驱动转向齿条时，高速下轻推摇杆就会把前轮打到远超轮胎峰值侧偏角的位置，车只会推头或甩尾；
摇杆抖动也会直接传到齿条。GT7 对手柄做了三件事：限制转向速度、按车速缩小可用转角、在车滑动时把转向范围
对准车的行进方向（反打辅助）。这里在编辑器的驾驶服务里实现同样的思路。

## 位置

- `engine/editor/services/vehicle_steering_assist.{h,cpp}`：纯函数 `ComputeAssistedSteering`，状态在
  `VehicleSteeringAssistState` 里逐帧携带，不依赖物理世界，单测 `tests/steering_assist_tests.cpp`。
- `VehicleDriveService::Tick`：读完输入（键盘缓动 + 摇杆）后、交给物理前调用；脚本驾驶（`--drive-controls`）不经过它。
  暂停时以 dt = 0 调用，保持当前值。
- `VehicleTelemetry::rightSpeed`：车身速度在车身右方向的分量，用来算车身侧偏角。
- Vehicle 面板新增 “Steering Assist” 一节，默认开启。

## 算法

1. **摇杆曲线**：`x(1-s) + x³ s`，s 默认 0.5，中心更细腻、两端不变。
2. **限速 + 平滑**：请求值朝目标以 `1/steerSeconds`（默认 0.25 s 打满）移动，回中或反向用 `1/returnSeconds`
   （默认 0.15 s）；再加 0.04 s 一阶低通，去掉阶跃。
3. **随速转角**：满舵对应的前轮角
   `δ_lim = atan(L · a / v²) + α_peak`，a = `cornerGrip` g（默认 1 g），L 为轴距，α_peak 为前胎峰值侧偏角
   （车数据里的值，没有时 7°）。v 取 `max(|v|, fullLockSpeed=5 m/s)`，低速时 δ_lim 超过最大转角即为全锁。
   比例 `δ_lim / δ_max` 下限 `minLockShare` = 0.1。
   例：R34（L = 2.665 m，δ_max = 35°），100 km/h 时 δ_lim = 1.94° + 7° = 8.94°，即全锁的 25.5%。
4. **反打辅助**：车身侧偏角 β = atan2(rightSpeed, forwardSpeed)，去掉 2° 死区后除以 δ_max 作为范围中心
   （0.08 s 低通，前进速度 1.5–3 m/s 间渐入）。最终输出 `clamp(centre + request · share, -1, 1)`。
   松开摇杆时前轮指向车的行进方向，前轮无侧偏，等价于自动反打；正常过弯 β 在死区内，不受影响。
5. **前轮侧偏闭环限制**（2026-10-07 加）：第 3 步是按车速算的开环值，假设 1 g 抓地，看不到刹车、
   载荷转移、低附着或车已经在推头。这一步每帧读车：前轴中心的速度方向
   θ_f = atan2(frontAxleRightSpeed, forwardSpeed)（`VehicleTelemetry::frontAxleRightSpeed`，
   Jolt `GetPointVelocity` 取前轴两悬挂安装点中点，含横摆），前轮侧偏角就是 δ − θ_f。把 δ 限制在
   `θ_f ± α_peak · slipLimitShare` 之内，0.04 s 低通 θ_f。只减舵、不加舵：上限取 `max(θ_f + α, 0)`，
   下限取 `min(θ_f − α, 0)`，所以甩尾时不会自己把前轮打向反打方向（那是第 4 步的事，关掉反打辅助就
   不该有反打）。在 `fullLockSpeed` 到 2 倍之间渐入，倒车不限。
   - 遥测核对：GT-R 实车测试里 |δ − θ_f| 与轮胎自己算的侧偏角相差 < 0.3°。
   - `slipLimitShare` 默认 1.1：数据里的峰值角（AC `FRICTION_LIMIT_ANGLE`）是静载下的，外侧重载轮
     峰值更靠后，加上 Ackermann。R34 扫描（满舵阶跃，平均横向 g）：60 km/h 0.8→0.91 g、1.0→1.11、
     1.1→1.17、1.2→1.17、1.4→1.16，不限 1.15；120 km/h 刹车 0.4：1.0→1.04、1.1→1.06，不限 1.03。

### 实测（满舵阶跃，开环限角之外加/不加闭环）

| 车 / 工况 | 前轮平均侧偏 | 横向 g | 掉速 |
|---|---|---|---|
| R34 60 km/h 干地（推头） | 12.4° → 7.5°（峰值 7.53°，share 1.0） | 1.15 → 1.11（share 1.1 时 1.17） | 7.8 → 4.4 km/h |
| R34 120 km/h 刹车 0.4 | 10.9° → 7.8° | 1.03 → 1.04 | 42.7 → 41.2 km/h |
| GT-R 60 km/h 干地（share 1.1，单测） | 8.36° → 6.43°（峰值 6.04°） | 1.43 → 1.45 | 4.4 → 1.5 km/h |

120 km/h 满舵带油门时两台车都是转向过度甩尾（β 到 −11°～−19°），前轮在反打一侧，闭环基本不起作用，
这是预期：它只管推头。

## 可调参数（面板）

Stick Response Curve、Steer Time、Return Time、Smoothing、Full Lock Below、Speed-Sensitive Lock（Corner Grip、
Least Lock Share）、Front Slip Limit（Peak Slip Share）、Counter-Steer Assist（Slide Dead Zone）。关掉总开关即恢复
原来的直接映射。
