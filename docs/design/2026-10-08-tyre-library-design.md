# 共享轮胎库与按轮位装胎

日期：2026-10-08

## 背景

一条轮胎的数据原来散在三处：

- 抓地、滑移、惯量等在 `VehicleTyreSettings`（导入时按默认胎、按静载算好写进 glTF 的 `frontTyres` / `rearTyres`）；
- 竖向刚度、阻尼、半径在悬挂的 `VehicleSuspensionAxle::tyreRate / tyreDamping / tyreRadius`；
- 载荷指数、轮辋半径、胎压、松弛长度、CX_MULT 在 `ApplyCarSpec` 里再从内嵌的 `tyreCompounds` 原始键值里取。

三处各自去 tyres.ini 里找“默认胎”，cce074a 修的就是其中一处取错了套。物理也只分前后轴（`wheelIndex < 2 ? frontTyres : rearTyres`），四个轮子不能各装各的胎，一条胎也不能装到别的车上。

## 目标

1. 一条胎（一个胎种、一个轴的尺寸）是一个独立对象 `tyre::TyreSpec`，有类型的字段覆盖 tyres.ini 的全部 53 个物理参数，原样保留不认识的键。
2. 轮胎是资产：`assets/tyres/**.tyre.yaml`，有 uuid sidecar，可以被任何车引用。
3. 车的 glTF 记录四个轮位各装哪条胎（uuid + 路径），物理按轮位取胎。
4. 这一阶段**行为不变**：派生出的物理参数和导入器的算法逐位相同（测试对比）。老资产 glTF 里的 `frontTyres` 是导入时按 4 位小数取整写的，现在改为运行时从完整数据算，差别在 5e-5 以内。

不在这一阶段：把没接的参数接进物理（第二阶段起）、轮胎尺寸跟胎走（物理半径、宽度仍取 3D 模型的轮子，见第二阶段 `WIDTH` / `RADIUS`）。

## TyreSpec（`engine/tyre/tyre_spec.*`）

分组，单位沿用 AC（"AC 数据为准"），非 SI 的字段名带单位：

| 组 | 字段（AC 键） |
| --- | --- |
| size | width, radius, rimRadius, angularInertia, radiusGrowthMm（RADIUS_ANGULAR_K，mm/(rad/s)） |
| vertical | rate, damping |
| grip | referenceLoad（FZ0）, longitudinalReference（DX_REF）, lateralReference（DY_REF）, longitudinalLoadExponent（LS_EXPX）, lateralLoadExponent（LS_EXPY）, dx0, dx1, dy0, dy1, speedSensitivity, brakeLongitudinalMod（BRAKE_DX_MOD）, xmu |
| slip | frictionLimitAngleDegrees, falloffLevel, falloffSpeed, longitudinalStiffnessRatio（CX_MULT）, combinedFactor, relaxationLength |
| carcass | flex, flexGain |
| camber | gain, dcamber0, dcamber1 |
| rolling | resistance0, resistance1, resistanceSlip |
| pressure | staticPsi, idealPsi, springGain, flexGain, rollingResistanceGain, footprintGain（PRESSURE_D_GAIN） |
| thermal | surfaceTransfer, patchTransfer, coreTransfer, internalCoreTransfer, frictionK, rollingK, surfaceRollingK, coolFactor, performanceCurve |
| wear | wearCurve, grainGain, grainGamma, blisterGain, blisterGamma |

- 标量都是 `std::optional<float>`：数据里没有的就是没有，YAML 里也不写。
- 一张键表（组名、YAML 键、AC 键、成员）同时驱动 AC 转换、YAML 读和写，三处不会对不上。
- `extraValues` / `extraCurves` 收不在表里的键（键名同 AC，热区段带 `THERMAL_` 前缀）。
- `TyreSpecFromAc(name, shortName, values, curves)`：从导入器内嵌的原始键值（`VehicleTyreData`）建。

## 从 TyreSpec 到物理

`TyreSettingsFromSpec(spec, staticLoad)`（physics）一次给出原来三处的全部量：抓地（按静载的 `DX_REF·(Fz/FZ0)^(LS_EXPX−1)`，缺参数时 `DX0+DX1`）、峰值滑移角和 tan、峰后比例、惯量、载荷指数（上限 1）、轮辋半径、胎压（psi→Pa）、松弛长度、CX_MULT，以及新加的 `verticalRate` / `verticalDamping`。

- 静载仍按**不含燃油**的质量和前后分配算（和导入器原来一样），保证逐位不变。
- `VehicleSettings::frontTyres / rearTyres` 换成 `std::array<VehicleTyreSettings, 4> tyres`，下标同轮位（0 左前、1 右前、2 左后、3 右后）。物理里所有按轴取胎的地方改成按轮位。
- 竖向：轮位上的胎有 `verticalRate` 时用它，否则退回悬挂轴的 `tyreRate`（老资产）。
- 老资产没有轮胎引用：按 `defaultTyreCompound` 从内嵌数据建 TyreSpec；连内嵌数据都没有的更老资产，用 glTF 里的 `frontTyres` / `rearTyres`。

## 轮胎库（`engine/asset/tyre_library.*`）

- 文件：`assets/tyres/<来源车目录>/<胎种>_<front|rear>.tyre.yaml`，如 `assets/tyres/ks_mazda_rx7_tuned/semislicks_front.tyre.yaml`；前后轴是同一条胎时（R34）只写一个 `<胎种>.tyre.yaml`。`.tyre.yaml` 注册为可引用资产（uuid sidecar），改名、移动后引用由注册表修复。
- 数字按最短的可逐位还原写法输出（`std::to_chars`），读回与写入相等。
- 写入时按内容去重：库里已有一模一样的胎就直接引用，不再写新文件；同名但内容不同的加 `_2`、`_3`。
- `TyreLibrary::List()` 扫描全库，给编辑器下拉框用。

## 车的 glTF

`MINIENGINE_vehicle` 增加：

```json
"tyres": {
  "compounds": [{"name": "Semislicks", "front": {"uuid": "...", "path": "tyres/..."}, "rear": {...}}, ...],
  "wheels": [{"uuid": "...", "path": "..."}, x4]
}
```

`compounds` 是这台车原厂的几套胎（编辑器里排在最前面），`wheels` 是四个轮位现在装的。原来的 `tyreCompounds`、`frontTyres`、`rearTyres` 照写，库文件丢了也能开。

加载时（资产层）按 uuid/路径解析成 TyreSpec 放进 `VehicleCarSpec::wheelTyres`；解析不到的轮位退回内嵌数据，并打日志。

## 导入与迁移

- kn5 导入：读完 data.acd 后，每套胎的前后轴各写一条库胎，`wheels` 填默认胎。
- 已有资产：`miniengine_app --adopt-car-tyres <car.gltf>` 把内嵌的几套胎写进库并补上引用（先备份）。RX-7、R34 用它迁移。

## 编辑器

- Vehicle 面板新增 Tyres 一节：四个轮位各一个下拉框（"This Car" 列本车原厂胎，"Tyre Library" 列其余库胎，悬停显示主要参数），"Both Wheels of an Axle" 勾选时同轴两轮一起换。选择存在 `VehicleSettings::tyreFitment`，下次开始驾驶时生效；换选另一台车时清空。"Save to Car" 把四个轮位写回车的 glTF（先写临时文件再替换）并更新模型缓存；老资产显示 "Add the Car's Tyres to the Library"。
- 逻辑在 `engine/editor/services/vehicle_tyre_fitment.*`（Fit / Save / Adopt），面板只管画。
- Assets 窗口认得 `.tyre.yaml`（类型 Tyre），预览里列名称、来源和主要参数。
- HUD 的胎种缩写取左前、左后轮位的胎。

## 测试

- TyreSpec：AC 键全部映射（表里 53 个 + extra），YAML 往返逐位相同，缺省字段不写。
- 派生值：Boxster 夹具和 R34/RX-7 数据，`TyreSettingsFromSpec` 与原来导入器写的 `frontTyres/rearTyres` 及 `ApplyCarSpec` 的派生量逐位相同。
- 库：去重、同名不同内容加后缀、uuid 解析、改名后仍能解析。
- 导入：glTF 带 `tyres`，加载后四个轮位的 TyreSpec 等于默认胎；删掉库文件后退回内嵌数据。
- 物理：一个轮位换胎后只有那个轮位的物理参数变；原有车辆物理、转向辅助、悬挂测试全部通过。
- 面板（`miniengine_vehicle_tyres_panel_tests`，可出截图）：选胎、同轴、换回原厂胎等于不换、保存后 glTF 其余内容不变、换车清空、老资产收编。

## 迁移结果（2026-10-08）

- RX-7 Tuned：3 套胎 × 前后 = 6 个文件，四轮默认 Semislicks。
- R34：前后同胎，2 个文件（`street`、`semislicks`），四轮默认 Semislicks。
- 两台车的 glTF 除新增 `tyres` 外不变（备份 `out/backup/*.before-tyre-library.backup`）。
