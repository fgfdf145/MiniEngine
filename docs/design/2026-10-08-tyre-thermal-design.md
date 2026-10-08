# 轮胎温度与胎压（AC 轮胎模型 V10）

日期：2026-10-08

## 背景

共享轮胎库的第三阶段。第二阶段（`2026-10-08-tyre-data-terms-design.md`）把胎压固定在理想值、温度不模拟。这一阶段按游戏自己的代码实现温度和胎压，让 tyres.ini 里的 [THERMAL_*] 八个参数、性能曲线和五个胎压参数生效。

公式同样从 `acs.exe`（附带 PDB）读出：`Tyre::stepThermalModel`、`TyreThermalModel::step / addThermalInput / getCurrentCPTemp / buildTyre / init`、`Tyre::step`（胎压）、`Tyre::getCorrectedD`、`Tyre::getDynamicK`、`Tyre::Tyre`（默认值）、`Tyre::initCompounds`（COOL_FACTOR 的换算）。

## 游戏的热模型

结构：胎面是 3 条（一侧、中间、另一侧）× 一圈 12 块，加一个胎芯。轮子转动累计角度，`index = ⌊角度/2π · 12⌋ mod 12` 是此刻贴地的那一块。

每一步（顺序和游戏一致，胎压和抓地用的是上一步的结果）：

1. **热输入**
   - 胎面：`滑动速度 · Fz · D_static(Fz) · FRICTION_K · 路面抓地 + p·SURFACE_ROLLING_K·ω·Fz/1000`，其中 D_static 是还没乘温度和胎压之前的横向摩擦系数，p 是胎压的滚阻系数 `1 + (P_ideal/P − 1)·PRESSURE_RR_GAIN`。
   - 胎芯：`ROLLING_K·p·ω·Fz/1000`。
2. **贴地块的目标温度** = 热输入 + 路面温度（26 °C），按外倾和胎压分到三条：
   - 一侧 `(1 + c − 0.05r)`，中间 `(1 + 0.1r)`，另一侧 `(1 − c − 0.05r)`，三条都再乘 `(1 − 0.5r)`。
   - c = clamp(外倾 · CAMBER_TEMP_SPREAD_K, −1, 1)，r = P/P_ideal − 1。
3. **胎芯**：向 max(胎芯输入, 气温) 靠拢，速率 INTERNAL_CORE_TRANSFER。
4. **每一块胎面**（按游戏的遍历顺序，原地更新）：
   - 有输入且高于气温：向目标靠拢，速率 SURFACE_TRANSFER。
   - 否则向气温冷却，速率 `(1 + v²·(COOL_FACTOR − 1)·0.000324)·SURFACE_TRANSFER`，v 是车速（m/s）；0.000324 是游戏读入 COOL_FACTOR 时乘的换算。
   - 和四个邻块（左右两条、前后两块，一圈首尾相连）交换热量，速率 PATCH_TRANSFER。
   - 和胎芯双向交换，速率 CORE_TRANSFER。
5. **接地温度** = `((1+c)·T一侧 + T中 + (1−c)·T另一侧)/3`；**实用温度** = 胎芯 + 0.25·(接地温度 − 胎芯)；**温度抓地** = PERFORMANCE_CURVE(实用温度)。
6. **胎压** = PRESSURE_STATIC + (胎芯 − 26)·PRESSURE_TEMPERATURE_GAIN。

胎压的作用：

- 抓地 / (1 + |P − P_ideal|·PRESSURE_D_GAIN)；
- 滚阻和滚动生热乘 p；
- 竖向刚度 = RATE + (P − PRESSURE_STATIC)·PRESSURE_SPRING_GAIN。

游戏的默认值（tyres.ini 没有 [ADDITIONAL1] 时；本机 178 台车都没有）：PRESSURE_TEMPERATURE_GAIN 0.16 psi/°C，CAMBER_TEMP_SPREAD_K 1.4，BLANKETS_TEMP 80 °C；气温和路面温度 26 °C；没有 PRESSURE_STATIC 时 26 psi。

以 RX-7 半热熔胎为例：冷胎 28 psi，理想 33 psi，胎芯到 57 °C 胎压才到理想值。冷启动（26 °C）的抓地是曲线的 0.929 再除以 1 + 5 × 0.0045，约 0.909。

## 实现

- `engine/tyre/tyre_thermal.*`：`TyreThermalModel` 逐条照搬上面的步骤。和游戏的两处不同：
  - 滚动生热用 |ω|（游戏用带符号的 ω，倒车时生热为负）；
  - 一圈首尾两块互为邻居（游戏的 buildTyre 只看到一个方向的回绕，影响可以忽略）。
- `TyreSpec` 新增 [ADDITIONAL1] 的三个键（键表的 `thermalSection` 改为节前缀，导入器把 [ADDITIONAL1] 读进每套胎）。
- `VehicleTyreSettings::thermal`、`lateralReference`、`pressureSpringGain`、`pressureGripGain`，由 `TyreSettingsFromSpec` 填。
- `VehicleSettings::tyreTemperatures`（编辑器默认开）和 `tyreStartTemperature`（默认 26 °C）。只对刷子轮胎、并且有 [THERMAL_*] 数据的轮位生效；关掉时和第二阶段一样（抓地 1、理想胎压）。
- 物理每一步：
  - 刷子轮胎求解前，把温度抓地 / 胎压损失乘到两个方向的摩擦上；把 p 交给滚阻项；按胎压更新刷子轮胎和簧下轮胎弹簧的竖向刚度。
  - 求解后用这一步的滑动速度、载荷、外倾、路面抓地和车速推进热模型。
- 遥测 `VehicleTelemetry::tyres`：每个轮位三条胎面温度（内 / 中 / 外，按轮位换算）、胎芯、胎压、温度和胎压留下的抓地。Vehicle 面板驾驶时显示这张表；Tyres 一节有 "Tyre Temperatures" 开关和 "Start Temperature"。

没做的：

- PRESSURE_FLEX_GAIN 改变峰值滑移（属于 SCTM 斜率曲线）；
- 温度抓地对峰值滑移的 0.75 倍关系（刷子轮胎的峰值滑移本来就随摩擦系数变）；
- 胎温毯（BLANKETS_TEMP，可以用 Start Temperature = 80 代替）；
- 环境温度随时间、天气变化；
- 磨损、起粒、起泡见 `2026-10-08-tyre-wear-design.md`。

## 结果

`miniengine_tyre_tests`：
- 每个机制单独一步对公式（胎芯、贴地块三条、冷却、接地温度和实用温度、性能曲线、胎压）；
- 100 km/h 滚一分钟：胎面约 39.5 °C，胎芯 31.8 °C，28.9 psi，抓地 0.94；
- 滑动比单纯滚动热得多；停车后冷却回气温。

`miniengine_vehicle_physics_tests`：
- 冷车从 26 °C、28 psi、抓地 0.909 开始，开动后胎面、胎芯、胎压都上升；
- 关掉时不模拟。

实车探针（`MINIENGINE_TYRE_PROBE`，Release）：

| | RX-7：第二阶段 → 加温度（冷胎起步） | R34：第二阶段 → 加温度 |
| --- | --- | --- |
| 0–100 km/h | 5.90 → 6.30 s | 6.83 → 6.87 s |
| 100 km/h 刹停 | 34.1 → 36.7 m | 34.5 → 36.1 m |
| 稳态转弯 | 0.99 → 0.90 g | 0.87 → 0.82 g |

两分钟激烈蛇行（80–110 km/h）：
- R34 前轮胎面 70 → 105 → 119 → 137 °C，胎芯 32 → 45 → 57 → 70 °C，胎压 29 → 35 psi，抓地 0.93 → 0.997；
- RX-7（后驱漂移车）打滑转圈，后轮胎面到 138 °C，胎芯 61 °C。

冷胎抓地少约 9%、要一两分钟的激烈驾驶才进工作温度，这是游戏数据本来的样子。不想要这个效果可以在 Vehicle 面板关掉温度，或者把起始温度设成 80 °C。
