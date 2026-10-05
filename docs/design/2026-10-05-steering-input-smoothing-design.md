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

## 可调参数（面板）

Stick Response Curve、Steer Time、Return Time、Smoothing、Speed-Sensitive Lock（Corner Grip、Full Lock Below、
Least Lock Share）、Counter-Steer Assist（Slide Dead Zone）。关掉总开关即恢复原来的直接映射。
