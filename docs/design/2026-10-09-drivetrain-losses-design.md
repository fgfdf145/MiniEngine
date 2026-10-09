# 传动系损耗

日期：2026-10-09

## 背景

R34 官方数据是 392 N·m / 280 ps，引擎里的 AC 数据只有 331 N·m。

把装的 178 台 AC 车全部解开（145 台可比），Kunos `ui_car.json` 的标称值除以物理曲线（含涡轮）的峰值，中位数是 1.15（扭矩、功率都是），10%–90% 在 1.04–1.23。R34 是扭矩 ×1.18、功率 ×1.19。

`acs.exe` 带调试符号：
- `Drivetrain::loadINI` 读的 39 个键里没有效率或损耗；
- `step2WD` / `step4WD` 里的常数只有 0.5、1、60、1/2π 等，没有效率系数。

所以 AC 不模拟传动损耗，它的扭矩曲线就是车轮端的（相当于扣掉约 13%），传动系按 100% 传递。引擎之前和它一样。

用户选了方案 B：齿轮啮合损耗、反拖、搅油和轴承阻力都模拟。

## 模型

`VehicleSettings::drivetrainLosses`（`VehicleDrivetrainLosses`），编辑器默认开（`DefaultTuning`），结构体默认关（测试和工具不变）。

| 项 | 默认 | 说明 |
|---|---|---|
| gearboxEfficiency | 0.96 | 间接挡：经副轴两对齿轮 |
| directGearEfficiency | 0.985 | 直接挡（齿比 1，±0.005）：只有轴承和油封 |
| finalDriveEfficiency | 0.96 | 准双曲面主减速 |
| transferEfficiency | 0.97 | 四驱分动箱 |
| spinTorque | 3 Nm | 每根传动系带着转的车桥，车轮端，与负载无关 |
| spinTorquePerSpeed | 0.04 Nm/(rad/s) | 每根车桥，随轮速增长（搅油） |

**啮合效率**
- 某挡的效率 `DrivetrainEfficiency` = 变速箱 × 主减速。
- 中央差速器四驱再乘分动箱；耦合器四驱（R34 的 ATTESA）的分动箱只作用在耦合器传给前轴的那部分。
- 空挡为 1。

**参考效率与扭矩曲线**
- 参考效率 `ReferenceDrivetrainEfficiency` 取间接挡的效率。后驱和 R34 都是 0.9216，中央差速器四驱是 0.894。
- 开启时，`ApplyCarSpec` 把游戏曲线（含 ERS 曲线）除以参考效率，当作曲轴扭矩。这样全油门、间接挡时车轮扭矩和 AC 一样。
- 用户自己调的曲线（不用车数据）本来就是曲轴端，不换算。

## 实现

Jolt 的传动系（`WheeledVehicleController::PostCollide`）是无损的粘性离合器加隐式求解，所以损耗都在 Jolt 之外加。

**啮合损耗：`ApplyDrivetrainLoss`**
- 在 `ApplyEngineCoast` 之后施加到发动机上。
- 净扭矩 = 曲线扭矩 × 油门 − 发动机反拖。
- 驱动时加 `−(1−η)·净扭矩`，车轮得到 η 倍；反拖时加 `(1/η−1)·净扭矩`，车轮要给出净扭矩 / η。
- η 按离合器结合程度混合：`1 − (1−η_挡)·clutchFriction`，离合器打开时不损耗。
- 发动机惯量引起的那部分没有乘 η，误差约 (1−η)·反映到车轮的惯量份额，很小。

**分动箱：`ApplyCentreCoupling`**
- 功率流向哪一侧，哪一侧就只得到 transferEfficiency 倍：驱动前轴时是前轴，前轴反拖后轴时是后轴。

**搅油和轴承：`ApplyDrivetrainSpin`**
- 每根被驱动的车桥受 `spinTorque + spinTorquePerSpeed·|ω|`，两轮平分。
- 按干摩擦处理（`RollingResistanceTorque`），不会把车轮倒转。
- 变速箱自身的阻力算在被驱动车桥里，不区分挡位。

**其他用到传递扭矩的地方**
- LSD 的锁止扭矩（`CoupleDifferentialWheels`）和整体桥的反扭（`ApplyAxleTorqueReaction`）改用损耗后的扭矩，所以 LSD 仍按 AC 口径锁止。

**显示**
- 遥测 `VehicleTelemetry::drivetrainMeshLossKw` / `drivetrainSpinLossKw`。
- Vehicle 面板 Tuning 里有 "Drivetrain Losses" 开关和六个参数。
- 驾驶时 "Car's own data" 一行显示曲轴端峰值扭矩和功率（`PeakCurvePower`）。

## 结果

`miniengine_vehicle_physics_tests`：
- 效率按挡位、反拖和阻力的公式；
- 曲线换算；
- 单挡 8–14 m/s 用时比 0.917（理论 0.9216，差在发动机惯量）；
- 遥测的阻力功率和公式一致；
- 空中反拖多损失 7.5%（直接挡 1/0.9456 加上搅油阻力）。

R34 实车探针（`MINIENGINE_DRIVETRAIN_PROBE=<gltf> miniengine_kn5_import_tests`，平直路面，刷子轮胎，自动挡）：

| | 无损耗 | 有损耗 |
|---|---|---|
| 曲轴峰值 | 331 Nm / 278 PS | 359 Nm / 302 PS |
| 0-100 km/h | 6.83 s | 6.82 s |
| 0-200 km/h | 24.99 s | 25.68 s |
| 极速 | 255.6 km/h（5 挡） | 253.9 km/h（5 挡） |
| 极速收油 5 s | −29.2 km/h | −30.1 km/h |
| 100 km/h 时损耗（2 挡全油门） | — | 啮合 18.2 kW + 阻力 1.1 kW |
| 200 km/h（5 挡） | — | 13.0 + 3.4 kW |
| 极速时 | — | 14.4 + 5.2 kW |

- 0-100 受抓地限制，不变。
- 0-200 慢 0.7 s、极速低 1.7 km/h，主要来自转速相关的阻力：200 km/h 以上加速功率的余量只有几十 kW。
- 曲轴端 359 Nm / 302 PS 低于 Kunos 标称的 392 Nm / 325 bhp：Kunos 的 1.18 倍里还有这套效率没覆盖的部分（四驱实测损耗通常 15–20%，含轮胎在转毂上的损耗）。

## 未做

- 效率与油温、负载、转速的关系。
- 空挡时变速箱输入轴一侧的阻力。
- 0-200 本身偏慢（无损耗也要 25 s），和这次改动无关，另查。
