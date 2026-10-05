# 限位块零点核实 + 驱动力经半轴反作用（anti-squat 走轮心轨迹）

日期：2026-10-05。来源：悬挂代码审查表里的第 1、2 条。

## 1. BUMPSTOP_UP / BUMPSTOP_DN 的零点：核实，代码不改

疑问：有 `ROD_LENGTH` 时车静止在设计位置之外（R34 前 −20.1 mm、后 +52.6 mm），限位块应从设计位置量还是从静止位置量。

证据：

- R34 自己的 `suspensions.ini`（data.acd 解密）里 Kunos 的注释：
  `BUMPSTOP_UP=0.060 ; meters to upper bumpstop from the 0 design of the suspension`，`BUMPSTOP_DN` 同样写着 “from the 0 design”。
  社区资料的说法也一样。
- 已安装的车里，有 `ROD_LENGTH` 的独立悬挂一共 347 个车轴。按我们的读法（静止行程 = 载荷/k − L，限位块从设计位置量起）：
  静止行程中位数 1.9 mm（绝大多数车静止位置几乎就在设计位置，两种读法分不出来）；
  到上限位块的余量中位数 59.9 mm，为负的 3 个；到下限位的余量中位数 60.3 mm，为负的 5 个。
  R34 后轴是少数离设计位置很远的车之一。

结论：现在的代码（`Curve::Stop(BUMPSTOP_UP)`，硬限位从设计位置量起）与数据的定义一致，不改。
R34 后轮静止时离上限位块约 22–25 mm，前轮离下垂限位约 30 mm，这是数据本身的结果（按 AC 数据为准的原则不改）。

`PACKER_RANGE` 两种读法在 346 个车轴上的对比（按 (a) 实现，见第 4 节）

| 读法 | 静止时离 packer 余量中位数 | 为负（静止就压在 packer 上） | < 10 mm |
|---|---|---|---|
| (a) 弹簧压缩量 z + L 达到 P 时碰 packer | 46.1 mm | 5 | 8 |
| (b) 从全下垂 −DN 起算的行程达到 P 时碰 packer | 30.4 mm | 72 | 108 |

(a) 对几乎所有车都说得通，(b) 不行。按 (a)，R34 前轮静止时离 packer 20 mm、后轮 45.5 mm
（`2026-10-05-rod-length-rest-at-model-design.md` 当时以“前悬 20 mm 就碰”为由没做的就是这个读法）。用户确认按 (a) 实现。

## 2. 驱动力的反作用走半轴：加速时的悬挂载荷按轮心轨迹

原来：轮胎的纵向力一律当作作用在接地点、全部由转向节承受（`CornerInput::load.moment` 一直是 0）。
这只对制动成立：卡钳在转向节上，制动力矩由转向节承受，所以按接地点轨迹算（抗点头 / 抗抬尾）。
驱动力矩则来自车身上差速器经半轴传来的扭矩：车轮绕旋转轴的力矩平衡由半轴完成，转向节只受轮心处的力，
应按轮心轨迹算（抗下蹲）。发动机制动同理。

做法：`KnuckleSpinMomentRelief(geometry, force, brakeTorque)`（`engine/physics/vehicle_suspension.*`）：

- τ = ((P − W) × F) · a，即路面力绕旋转轴 a 的力矩；
- 转向节最多承受 ±制动力矩（`GetBrakeInput() × mMaxBrakeTorque + GetHandBrakeInput() × mMaxHandBrakeTorque`，ABS 调过的那个值）；
- 返回 (clamp(τ, ±制动力矩) − τ) a，作为 `load.moment` 加进去。不踩刹车时等于把车轮平面内的力移到轮心；
  制动足够时不变；左脚刹车加油门时在两者之间线性过渡。
- 沿旋转轴方向的分力（来自前束、外倾）绕其它轴的力矩由轴承传给转向节，不受影响。
- 实心桥：差速器在桥壳里，转向节（桥壳）承受全部力矩，不做修正（`TORQUE_REACTION` 另算）。
- 几何用上一步的位姿，和力一样滞后一步。车身受到的总力不变（Jolt 仍在接地点施加轮胎力），只改变悬挂行程和车身之间的分配。

## 3. 结果

单元测试 `TestDriveForceLoadsTheLinkageAtTheWheelCentre`（GT-R 后轮，压缩 20 mm，2.5 kN 驱动力）：
悬挂载荷 59.0 N = 轮心轨迹的预测（平面内分力走轮心、轴向分力走接地点）59.0 N；制动时 −886.6 N = 接地点轨迹；
制动只能承受一半力矩时取中间值。

R34 无头测试（满油门，在 40–90 km/h 区间取平均；随后 0.7 刹车）：

| | 修改前 | 修改后 | 几何预测的变化 |
|---|---|---|---|
| 加速度 | 0.41 g | 0.41 g | |
| 车身俯仰（抬头 +） | +0.748° | +0.840° | |
| 前轮行程 | −16.6 mm | −16.0 mm | +0.6 mm |
| 后轮行程 | +13.8 mm | +18.6 mm | +5.0 mm |
| 刹车 0.79 g 时俯仰 | −1.398° | −1.397° | 0 |
| 刹车时前/后行程 | +29.8 / −27.4 mm | +29.8 / −27.4 mm | 0 |

预测来自 K&C 台架：R34 后轮接地点轨迹角 −0.18°，轮心轨迹角 +6.72°（anti-squat −58 %，即这套几何本来就会助长下蹲）；
每个后轮驱动力 1874 N × (tan 6.72° − tan(−0.18°)) / 45000 N/m = 5.0 mm。前轮轨迹角差 0.92°，0.6 mm。
测量与预测一致，刹车完全不变。

## 4. PACKER_RANGE（读法 (a)）

packer 是减振器杆上的垫块，作用是让缓冲块提前接触。所以 packer 就是把缓冲块的起点提前，刚度沿用数据里的 `BUMP_STOP_RATE`：

- 缓冲块起点（设计位置起量的行程）= min(`BUMPSTOP_UP`, `PACKER_RANGE` − `ROD_LENGTH`)（`VehicleBumpStopStart`）；
  只在有 `ROD_LENGTH` 的独立悬挂上读（与弹簧的读法同源），没有 rod length 时不读。
- 起点可以在设计位置下方；单侧限位曲线（`Curve::Stop`，sign ≠ 0）改为沿自己一侧量重叠量，所以负起点也能正常工作
  （对原有起点 ≥ 0 的情况结果完全一样）。
- 硬限位（`BUMPSTOP_UP` + 0.04 m）不变：packer 是橡胶，不是刚性的。
- 346 个车轴里有 236 个是 packer 先于缓冲块起作用（中位数早 28 mm）。

数据链路：`VehicleSuspensionAxle::packerRange`，读 AC 的 `PACKER_RANGE`，glTF 的 `MINIENGINE_vehicle` 里存为 `packerRange`
（kn5 导入写出、加载读回）。R34 资产已补上前 0.12、后 0.11（备份 `out/backup/skyline_r34_vspec.gltf.before-packer-range.backup`）。

R34：前轮 packer 在设计位置（0.12 − 0.12），即静止位置上方 20.1 mm，早于 `BUMPSTOP_UP` 的 60 mm；
后轮 0.11 − 0.015 = 95 mm，晚于 `BUMPSTOP_UP` 的 75 mm，所以后轮不变。

无头测试（同一程序，packer 关 / 开）：

| | packer 关 | packer 开 |
|---|---|---|
| 0.7 刹车（0.79 g）俯仰 | −1.397° | −1.265° |
| 刹车时前轮行程 | +29.8 mm | +23.5 mm |
| 0.68 g 稳态弯：侧倾 / 外侧前轮行程 | 2.22° / +21.9 mm | 2.21° / +20.8 mm |
| 0.95 g 稳态弯：侧倾 / 外侧前轮行程 / 俯仰 | 3.08° / +31.9 mm / −0.191° | 2.87° / +25.8 mm / −0.095° |
| 满油门加速 | 不变 | 不变 |

测试：`TestPackersBringTheBumpStopIn`（起点、提前、负起点、不带 rod length 时不读）、kn5 导入测试中 `PACKER_RANGE` 的往返。
