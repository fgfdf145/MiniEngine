# 轮胎磨损、起粒、起泡（AC 轮胎模型 V10）

日期：2026-10-08

## 背景

共享轮胎库的第四阶段，接在温度和胎压（`2026-10-08-tyre-thermal-design.md`）之后，让 WEAR_CURVE、GRAIN_GAIN/GAMMA、BLISTER_GAIN/GAMMA 和 [VIRTUALKM] USE_LOAD 生效。公式同样读自 `acs.exe`：`Tyre::addTyreForcesV10` 的末尾（虚拟里程）、`Tyre::stepGrainBlister`、`Tyre::getCorrectedD`（磨损曲线）、`Tyre::getDX/getDY` 与 `SCTM::solve`（起泡）、`Tyre::initCompounds`（WEAR_CURVE 的缩放、起粒和起泡的温度阈值）。

## 游戏的做法

- **虚拟里程**：每一步加 `滑动速度 · dt · 磨损倍率 · (USE_LOAD ? Fz/FZ0 : 1) / 1000`，单位 km，是胎面滑过的距离而不是车开过的距离。磨损倍率是游戏的会话设置（1x）。
- **磨损抓地**：WEAR_CURVE（虚拟里程 → %）读入时乘 0.01，抓地乘这个值。
- **窗口**：从 PERFORMANCE_CURVE 求出。起粒阈值是曲线第一次达到 1 的温度，起泡阈值是最后一次达到 1 的温度。RX-7 半热熔胎分别是 75 °C 和 95 °C。比较的是胎芯温度。
- **起粒**（胎芯低于窗口）：`grain += s^GRAIN_GAMMA · dt · (路面抓地 · V · GRAIN_GAIN · (T_窗口起点 − T_芯) · 0.0001)`。
  - s 是理论滑移除以峰值滑移（最多 2.5），V 是轮心对地速度。
  - 要求 V > 2 m/s、路面抓地 ≥ 0.95。
  - 同时一直在磨掉：`grain −= s^GRAIN_GAMMA · dt · V · 路面抓地 · GRAIN_GAIN · 0.00005`。
- **起泡**（胎芯高于窗口）：`blister += s^BLISTER_GAMMA · dt · (路面抓地 · V · BLISTER_GAIN · (T_芯 − T_窗口终点) · 0.0001)`，条件同上，不会恢复。
- 两者都限制在 0–100。
- 作用：
  - 起泡让抓地除以 `1 + 0.2·blister/100`，即 100% 起泡少 20%；
  - 起粒让峰值滑移变大（乘 1 + grain/100），刷子轮胎的峰值滑移由刷毛算出，这一项没有接。

## 实现

- `engine/tyre/tyre_wear.*`：`TyreWearModel`（虚拟里程、起粒、起泡、窗口阈值、抓地）。
- `TyreSpec` 新增 `wear.useLoad`（[VIRTUALKM] USE_LOAD，和 [ADDITIONAL1] 一样对每套胎都适用）。
- `VehicleTyreSettings::wear`，`VehicleSettings::tyreWear`（编辑器默认开），每次开始驾驶都是新胎。
- 物理每一步：
  - 刷子轮胎求解前，抓地乘磨损和起泡的系数；
  - 求解后用这一步的滑动速度、轮心速度、载荷、理论滑移（`VehicleTyreStepTerms::slip`，滚阻的滑移项也用它）、胎芯温度、路面抓地推进。
  - 没有开温度时只累计虚拟里程，不起粒、不起泡，因为没有胎芯温度。
- 遥测 `VehicleTelemetry::wear`；Vehicle 面板驾驶时显示每个轮位的滑行距离、起粒、起泡和剩余抓地，Tyres 一节有 "Tyre Wear" 开关。
- 库里已有的 RX-7、R34 轮胎补上 USE_LOAD：`--adopt-car-tyres <gltf> <游戏的车目录>` 现在可以直接给游戏目录，重新读 data.acd。同一来源的胎原地更新，uuid 不变，所以车的引用不用改。

## 结果

`miniengine_tyre_tests`：
- 窗口阈值（75 / 95 °C）；
- 虚拟里程（载荷计入）；
- 起粒、起泡各一步对公式；
- 慢速和滑路面不起泡；
- 磨损曲线 25 km 处 80%；
- 100% 起泡再除以 1.2。

`miniengine_vehicle_physics_tests`：冷车开一段之后，虚拟里程大于 0，起粒但不起泡。

实车探针，两分钟激烈蛇行：
- RX-7 后轮滑过 0.81 km，起粒 2.7%，磨损抓地 0.9994；
- R34 前轮滑过 0.53 km，起粒 4.1%，没有起泡（胎芯 70 °C，没到 95 °C 的窗口终点）。
