# AWD2 中央耦合、四轮转向控制器、车身碰撞体（Skyline R34）

日期：2026-10-04。目标车：Assetto Corsa `ks_nissan_skyline_r34`（用户此后只做 R34）。
关联：`2026-09-30-ac-car-data-design.md`（data.acd 导入）、`2026-09-30-realistic-vehicle-dynamics-design.md`。

## 0. 现状与问题

导入 R34 后对照原目录（2026-10-04）：

| 数据 | 原文件 | 导入前的处理 |
|---|---|---|
| ATTESA E-TS | `drivetrain.ini` `TYPE=AWD2`、`[AWD2]` | 只知道是四驱；物理固定前后 50/50，前后差速锁都用 `[DIFFERENTIAL]` 的 0.5 |
| Super HICAS | `ctrl_4ws.ini`（三个控制器，内联 LUT） | 没读 |
| 车身碰撞 | `colliders.ini`（三个盒子）、`collider.kn5`（74 顶点外壳） | 没读；驾驶时用模型包围盒拟合一个盒子 |

R34 的原始数据：

```
[AWD2]  FRONT_DIFF_POWER=0.03 COAST=0.03 PRELOAD=0
        CENTRE_RAMP_TORQUE=100.0  CENTRE_MAX_TORQUE=1000.0
        REAR_DIFF_POWER=0.60 COAST=0.50 PRELOAD=10
ctrl_4ws.ini  STEER_DEG  ADD  (-90=-0.0015 -25=-0.0010 -10=0 0=0 10=0 25=0.0010 90=0.0015)  FILTER 0.99
              OVERSTEER_FACTOR MULT (-1.6=-2 -1.2=1 0=1 1.2=1 1.5=2)  FILTER 0.99
              SPEED_KMH MULT (0=1 130=1 150=0.2)  FILTER 0.99      上下限 ±1
colliders.ini 0: CENTRE 0,-0.23,-0.9  SIZE 1.75,0.15,3.0   GROUND_ENABLE=1
              1: CENTRE 0,-0.26, 1.1  SIZE 1.75,0.15,1.0   GROUND_ENABLE=1
              2: CENTRE 0,-0.38, 1.8  SIZE 1.57,0.15,0.35  GROUND_ENABLE=1
collider.kn5  1 个网格 74 顶点 98 三角形，模型坐标 x ±0.913，y 0.115..1.337，z -2.313..2.286
```

## 1. AWD2：中央耦合

### 1.1 含义（含假设）

Kunos 没有公开 `CENTRE_RAMP_TORQUE` / `CENTRE_MAX_TORQUE` 的定义。能确认的：

- 论坛（overtake.gg「AWD / AWD2 Setting Torque Split?」）：AWD2 不是固定比例的中央差速器，而是按控制器把扭矩送到前轴的离合器。
- 已安装车里，带 `ctrl_awd2.ini` 的车（R35 GT-R、R8、保时捷等）的控制器输出是锁止扭矩（上限 10000~1000000），
  R35 的 `difflock_gear_mult.lut` 一档 15.0 = 一档速比 4.056 × 主减 3.7：扭矩在分动箱（变速箱输出）处计算。
- R34、155 V6、Celica ST185 没有控制器，只有这两个数。

**假设**：没有控制器时，中央离合器是装在分动箱上的粘滞式耦合，传递扭矩

    T_c = clamp(CENTRE_RAMP_TORQUE · Δω_shaft, -CENTRE_MAX_TORQUE, +CENTRE_MAX_TORQUE)
    Δω_shaft = i_final · (ω̄_rear − ω̄_front)        （分动箱两端的转速差，rad/s）

前轴得到 `+T_c · i_final`，后轴失去同样多（车轮处）。R34：后轮比前轮快 1 rad/s 时传 354 N·m（分动箱），
上限 1000 N·m 即车轮处 3545 N·m；一档满扭矩时车轮扭矩约 4490 N·m，所以最多约 80% 可到前轴，
实际受前后转速差限制。R34 的发动机扭矩只经后差速器（后驱），前轴只靠耦合得到扭矩：与 ATTESA 平时 0:100、打滑时向前分配的行为一致。

`[AWD]`（`TYPE=AWD`）是固定比例的中央差速器：`FRONT_SHARE` 为前轴份额，`CENTRE_DIFF_*` 为中央锁止。

带控制器的 AWD2 车（`ctrl_awd2.ini`）：控制器原样保存，物理暂不运行（按无控制器的耦合处理），留给后续。

### 1.2 数值稳定

耦合相当于两轴平均转速之间的阻尼 `c = RAMP · i_final²`（车轮处 N·m/(rad/s)）。显式施加，每步改变平均转速差
`Δ ← Δ (1 − c·dt·(1/(2I_f) + 1/(2I_r)))`。为不过冲，`c` 截到 `1 / (dt · (1/(2I_f) + 1/(2I_r)))`。
R34：I = 1.62 kg·m²，dt = 1 ms，上限 1620，数据 c = 1257，不截。

### 1.3 前后差速器

AWD/AWD2 的前、后差速器各有自己的 POWER、PRELOAD。沿用现有的离合片限滑模型（`CoupleDifferentialWheels`），
锁止份额和预紧按轴取：

- 后轴：`limit = preload_r + lock_r · (经后差速器的驱动扭矩)/2`；
- 前轴（AWD2）：`limit = preload_f + lock_f · |前轴耦合扭矩|/2`；
- 预紧低于现有的 40 N·m 时取 40 N·m（与其它车一致，滑行时不让两轮各转各的）。COAST 仍不使用（现状）。

## 2. 四轮转向控制器（ctrl_4ws.ini）

### 2.1 控制器运算（与 ERS、气动、四驱控制器同一套）

gro-ove ac-torque-helper `acController.jsx`：值从 0 开始，每个控制器 `x = LUT(input)`，
ADD: `v += x`，MULT: `v *= x`，随后截到 `[DOWN_LIMIT, UP_LIMIT]`。

**假设**：FILTER 是每个控制器对其 `x` 的一阶低通，AC 物理步 333 Hz 下的系数；本引擎 1 kHz 下
`a = FILTER^(dt·333)`。参考实现不做滤波（它是静态曲线工具）。

### 2.2 输出的单位与符号（假设）

保时捷 991：STEER_DEG ±140° → ∓0.01，低速乘 -2、80 km/h 以上乘 +0.85，总上限 ±0.011。
真实的 991 后轮转向低速反相、高速同相，所以：**输出是后轮转角（弧度），与 STEER_DEG 同号表示反相**。
0.011 rad = 0.63°，与真实系统（1~2.8°）同一量级。

R34：方向盘 90° 时 0.0015 rad = 0.086° 反相（入弯更灵敏），转向过度因子超过 ±1.5 时 ×2，
130 km/h 起减到 0.2。

### 2.3 输入

| 输入 | 定义 |
|---|---|
| STEER_DEG | 方向盘角度 = 转向输入 × 方向盘锁角（右正） |
| SPEED_KMH | 车身前向速度 |
| GAS / BRAKE | 油门、刹车输入 0..1 |
| LATG | 车身横向加速度 / g（左正，与车辆坐标 +X 一致） |
| GEAR | 当前档位 |
| SLIPANGLE_FRONT/REAR_AVERAGE/MAX | 前/后轮侧偏角（度），平均或绝对值最大 |
| OVERSTEER_FACTOR | **假设**：后轴平均侧偏角绝对值 − 前轴平均侧偏角绝对值（度） |

其它输入值取 0。

### 2.4 作用

后轮的前向向量绕车辆竖直轴转 `δ_r`（同相为正，与前轮同向）。多体悬挂和简单悬挂都在每步设置车轮的
`mWheelForward` 时施加，只作用于后轮；它不经过悬挂运动学（R34 的 HICAS 推的是后轮拉杆，转角极小，忽略其对外倾的影响）。

## 3. 车身碰撞体

- `colliders.ini` 的盒子：中心相对质心，轴同车辆坐标（+X 左、+Y 上、+Z 前），尺寸为全长。R34 三个盒子底面离地
  0.22 / 0.19 / 0.07 m（质心离地 0.527 m）。`GROUND_ENABLE` 原样保存，本引擎的静态网格不区分地面和墙，全部参与碰撞。
- `collider.kn5`：模型坐标的车身外壳（AC 用于车与车碰撞）。取其顶点做凸包，**顶点 y 抬到地面盒子的最高顶面**，
  使地面只接触 `colliders.ini` 的盒子（同 AC），外壳负责墙和翻车时的车顶。
- 有盒子时，车身形状 = 盒子 + 外壳的复合形状，替代包围盒拟合的盒子；质心按数据放置（`OffsetCenterOfMassShape`
  按复合形状自身的质心求偏移）。惯量仍来自 `inertiaBox`。

## 4. 数据与接口

- `VehicleController`：通用控制器（原 `VehicleErsController` 改名，ERS 档位也用它）。
- `VehicleAllWheelDrive`：AWD/AWD2 的全部数（前/中/后差速器、FRONT_SHARE、RAMP/MAX、中央控制器）。
- `VehicleCarSpec`：`allWheelDrive`、`rearSteerControllers`、`colliders`（`VehicleColliderBox`）、`colliderHull`（模型坐标）。
- `VehicleSettings`：`centreDrive`（Differential/Coupling）、`frontTorqueShare`、`centreCouplingRampTorque/MaxTorque`、
  `axleDifferentials`（每轴 lock/preload，lock < 0 用 `limitedSlipLock`）、`rearSteerControllers`、`steeringWheelLockDegrees`、
  `carColliders`、`chassisHull`（车辆坐标，由驾驶服务从模型坐标换算）。
- LUT 内联写法 `(|x=y|x=y|)` 在 `LutPoints` 中解析。

## 5. 验证

- 导入：R34 式 fixture（AWD2、内联 LUT 的 ctrl_4ws、colliders.ini、collider.kn5），glTF 往返。
- 中央耦合：公式与截断的单元测试；整车：后轮打滑时前轮得到扭矩，后驱时得不到。
- 控制器：R34 数据在 90°、100 km/h 时稳态 0.0015 rad 反相，150 km/h 时 0.0003；滤波时间常数。
- 后轮转向：后轮前向向量按 δ_r 转动。
- 碰撞体：车身形状包含盒子；车翻过来停在外壳上，不穿地。

## 6. 未做

- `ctrl_awd2.ini` 控制器的运行（数据已存）；`CENTRE_DIFF_*` 对 AWD 的中央锁止。
- 差速器 COAST。HICAS 对外倾/前束运动学的影响。GROUND_ENABLE=0 盒子的区分。

## 7. 结果（2026-10-04）

测试（`miniengine_vehicle_physics_tests`、`miniengine_kn5_import_tests`）：

- 控制器：R34 HICAS 90°/100 km/h → 0.0015，150 km/h → 0.0003，转向过度 1.5 时 ×2；滤波 0.3 s 到 1−1/e。
- 中央耦合：1 rad/s 车轮转差 354.5 N·m，上限 1000 N·m，前轮更快时反向。
- 整车（Boxster 车身 + R34 的 AWD2 数）起步 2 s：后驱前轮 −110 N（只滚动），AWD2 前轮 +1697 N，耦合平均 328 N·m，
  速度 15.7 m/s 对 12.2 m/s。
- HICAS 60 km/h、方向盘 45°：后轮 −0.065°（预期 −0.066°，反相），横摆角速度 0.2495 对 0.2433 rad/s。
- 车身：盒子 + 外壳的范围 y 0.127..1.4，只有盒子时到 0.307；静止时质心高度与包围盒车身相差 < 1 mm（盒子不碰地）。
- 导入：R34 式 fixture 解析、glTF 往返（含 collider.kn5 外壳的每个点）；已安装 178 辆车全部可读。

用户的 R34 资产（只补了 `allWheelDrive`、`rearSteerControllers`、`colliders`、`colliderHull`，其余逐项不变，
备份 `out/backup/skyline_r34_vspec.gltf.before-awd2-hicas-colliders.backup`），按编辑器的驾驶流程无界面运行：

| | 之前（50/50、无 HICAS、包围盒） | 之后 |
|---|---|---|
| 车身最低点（车辆坐标） | 0.261 m | 0.084 m（前唇盒子） |
| 静止质心高度 | 0.528 m | 0.528 m |
| 起步前 2 s 前轮驱动力份额 | 49% | 34%（耦合峰值 450 N·m） |
| 0–100 km/h | 6.45 s | 6.47 s |
| 转向 0.15、122 km/h 的后轮转角 | 0 | +0.131°（同相：此时转向不足，前轴侧偏角大 1.6° 以上，数据的 ×(−2)） |

0–100 km/h 6.5 s 远慢于 Kunos 标的 4.9 s，与本工作无关（牵引力控制、轮胎），另行处理。
