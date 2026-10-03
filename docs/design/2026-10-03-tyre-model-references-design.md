# 轮胎模型：现状缺口、文献地图与 MF 验证数据

日期：2026-10-03。状态：调研，未实现。轮胎原计划排在悬挂之后（见 `2026-10-02-suspension-test-rigs-design.md` 的顺序），本文是开工前的资料整理。

## 1. MiniEngine 现有轮胎缺什么

现状：Jolt `WheeledVehicleController` 的轮胎。每轮纵向、侧向各一条 3 点折线摩擦曲线（`physics_world.cpp` 的 `ApplyTyres`），力是速度约束，冲量钳在 `mu(slip) * N * dt`。悬挂模块已补上簧下质量 + 轮胎径向弹簧/阻尼（2b62f2e）和逐帧外倾/前束。

按影响排序的缺口：

1. **力不是滑移的函数。** 稳态滑移由步长与制动扭矩定（制动时约 0.2，`T dt / (I w)`），ABS 加了反而刹得更远（59 -> 70..97 m）。其余各项都依赖一条真正的 `F = f(kappa, alpha, Fz, gamma)`。
2. **无组合滑移（摩擦椭圆）。** 纵横向上限各自独立；`SetTireMaxImpulseCallback` 还没用上。
3. **无载荷敏感性。** 力与载荷成正比；`tyres.ini` 的 `FZ0`、`LS_EXPX/Y` 已导入但未读。
4. **外倾不影响抓地。** 悬挂算出的 camber 没进轮胎力；`CAMBER_GAIN`、`DCAMBER_0/1` 未用。
5. **无松弛长度。** 力瞬时建立；`RELAXATION_LENGTH` 未用。
6. **无回正力矩 M_z / 拖距。** 没有方向盘力反馈来源，后倾/拖距对力无贡献。
7. **低速与静止。** 改成力模型后 `(w r - v) / v` 在 v -> 0 发散，需要低速混合或随速度变化的松弛长度。
8. 次要：滚动阻力、翻转力矩、单点接触无包络（路肩）、胎温/胎压/磨损、路面只有一个 mu。

## 2. Stocco, Biral, Bertolazzi 2024 刷子模型

*A physical tire model for real-time simulations*，Math. Comput. Simul. 223 (2024) 654–676，开放获取（CC BY-NC-ND 4.0，原样收录）。PDF：`docs/references/stocco-2024-physical-tire-model.pdf`。

- 结构：轮胎沿宽度切成 n 根胎肋，各自与局部路面平面求交；垂向单点接触，与切向解耦，`k_z = k_s + k_p p + k_w w + k_g g^2`；接地长 `l = 2 sqrt((2(R - R_l) - rho) rho)`；接地压力为四次多项式（凸度 lambda、重心偏移 delta 的闭式系数）。
- 胎体：中心线 `y = y_c + theta_c (x - x_c) - y_c Psi/2 (x - x_c)^2`，状态 `c = [x_c, y_c, theta_c]`，平衡 `K(c) c - F(c) = 0`，K 对角（结构 + 胎压 + 平滑触底）。
- 切向：Coulomb + Savkoor 摩擦（Stribeck、压力、滑速依赖）；黏着/滑移分界 `|q_t| = mu_s q_z` 是多项式求根（Sturm / Algorithm 748）；黏着积分解析，滑移积分数值。
- 求解：3 个未知数，初始 Jacobian 取 `J_K`；Broyden Combined 最好：5 胎肋平均 8.4 次迭代、55.7 us（最大 264 us），成功率 100%；耗时随胎肋数线性（1 根 19 us，25 根 414 us）。平台 Xeon 4215 2.5 GHz 实时 Linux。
- 验证：侧向力与 MF 5.2 相当，纵向力和 M_z 偏差较大。

对 MiniEngine 的问题：

- 胎体导数 `x_c'` 等出现在滑移定义 (21)(22) 里，但平衡是静态的，离散方式未交代，瞬态要自己加。
- 滑移除以 `|V_r|`，低速未处理。
- 表 2 大多参数标 ⋆（估计值，未拟合）；式 (47) 积分限、图 13 的 alpha = 0.70 是笔误。
- 物理步 1000 Hz 下，4 轮 x 56 us ≈ 每步 0.22 ms（22% 预算）。
- 参数是物理量，与 AC `tyres.ini` 的经验参数对不上。

结论：可作为日后的工程级可选模型，不作为第一个实现。

## 3. 文献地图（手头已有，开工不缺论文）

两本书的 PDF 在 `C:\Users\Lora\Downloads\`（有版权，不入库），用 `pymupdf` 提取文本。

| 缺口 | Pacejka 2006（`epdf.pub_tyre-and-vehicle-dynamics-second-edition.pdf`，PDF 页 = 书页 + 15） | Rill 2012 全本（PDF 页 = 书页 + 31），第 3 章书页 43 起 |
|---|---|---|
| F(滑移)、组合滑移 | 4.3 MF，全套方程 4.E1–4.E78（4.3.2）；3.2.3 刷子组合滑移 | 3.3–3.4 TMeasy |
| 载荷敏感性 | MF 方程内 | 3.6.1 |
| 外倾 | 3.2.4、4.3 | 3.6.3 |
| M_z / 拖距 | 4.3 | 3.3.5 |
| 松弛长度 / 瞬态 | 第 5 章（弦模型）、第 7 章（单接触点瞬态） | 3.7 一阶轮胎动力学 |
| 低速 / 起步 / 驻车 | 8.6 Starting from Standstill；9.2.3 Parking | 3.7.3.3 Parking Torque；4.2 车轮动力学 |
| 滚阻、翻转力矩 | 4.3.5 | 3.3.2–3.3.3 |
| 接触几何、路肩 | 第 10 章（等效路面输入） | 3.2（含动态滚动半径） |
| 刷子物理模型 | 第 3 章、TreadSim（附录 2） | — |
| ABS 验证 | 8.5 | 4.2.3 |

TMeasy（Rill）参数少且直观，能处理静止，风格接近 AC 的 `tyres.ini`，适合作为第一个模型；MF 用于对照和验证。

仍缺：AC 自己的轮胎公式（不公开，只能按字段含义 + 游戏实测反推）；胎温/磨损文献（Farroni / Sakhnevych，后期再找）；FSAE TTC 原始数据（需会员，不需要）。

## 4. MF 验证数据

**参数：** Pacejka 附录 3 的 Table A3.1（书页 629，PDF 第 644 页）是完整 MF + SWIFT 参数集：205/60R15 91V，2.2 bar，ISO 符号，`R0 = 0.313 m`，`Fz0 = 4000 N`，`m0 = 9.3 kg`，`V0 = 16.67 m/s`，含 pCx/pDx/pEx/pKx/pHx/pVx、pCy…pVy、qBz…qHz、rBx…rVy 和 SWIFT 动态参数。参数名即 `.tir` 的 PCX1、PDY1 等字段。**提取注意：** pymupdf 把负号提成 `!`（如 `pEx3 =!0.020` 即 -0.020），录入时逐项核对。附录 3.2 说明这些量已无量纲化。

**参考实现（都需要 MATLAB，本机没有 MATLAB/Octave）：**

- MFeval，https://mathworks.com/matlabcentral/fileexchange/63618-mfeval ，支持 MF 5.2/6.1/6.2，业内常用的参考。
- teasit Magic Formula Tyre Library，https://github.com/teasit/magic-formula-tyre-library ，GPL-3.0，只有 MF 6.1，带一份脱敏的 TTC `.tir` 和测量数据，2026-03 已归档。
- OpenTire Python，https://github.com/opentire/opentirepython ，PAC2002，较旧，未与 MFeval 对照。

**计划：**

1. 零下载：Table A3.1 -> `tests/data` 下的 JSON；C++ 按 4.E1–4.E78 实现；测试用解析断言（零滑移零力、`K_x`/`K_y` 闭式、峰值 ≈ mu Fz、对称性），加上对照 4.3.6 节的曲线图（读图，几个百分点），再用 `tools/` 下的独立 Python 实现交叉对数。
2. 需要逐位对齐时，装 Octave 跑 MFeval，或借 MATLAB 跑一次，把输出存成 CSV 黄金数据（涉及下载，先问用户）。

这条乘用车胎只用来验证实现；GT-R / Boxster 的轮胎参数仍从 AC `tyres.ini` 映射。

### 4.1 第 1 步已完成（2026-10-03）

- `tests/fixtures/tyres/pacejka2006_205_60R15.tir`：Table A3.1 录成标准 `.tir`，对着页面图逐项核过符号。表里没有的系数（rEx、rEy、rHy2、qSy2…）为 0。
- `engine/tyre`（`engine_tyre`，只依赖标准库）：
  - `tyre_magic_formula.{h,cpp}`：`EvaluateMagicFormula`，照 4.E1–4.E78 逐式实现，含纯滑移和组合滑移的 Fx、Fy、Mz，以及 Mx、My；不含转向滑移（zeta 全为 1）。
  - `tyre_tir_file.{h,cpp}`：`.tir` 读取，键名大小写不敏感、不分段，兼容 MF 5.2 的 `LGAY/LGAZ`。
- `tools/tyre_reference/magic_formula.py`：独立的 Python 转写；`reference_points.py` 输出 C++ 参考点，共 67 组，覆盖 3 档载荷下的纯纵滑/纯侧偏、组合滑移、外倾、低速和倒车。
- `tests/tyre_tests.cpp`（`miniengine.tyre`）：
  - 与 Python 对齐到相对 1e-9；
  - `K_x`/`K_y` 等于曲线在零点的数值斜率，且等于闭式解；
  - 峰值等于 `D + S_V`；
  - ISO 符号检查；
  - 拖距 `t = R0 qDz1 cos'alpha`；
  - 组合滑移单调变弱；
  - `M_z` 峰值位置和大小落在合理区间；
  - 载荷敏感性；零载荷返回零。
- 实测数值（Fz0 = 4000 N）：`K_x` = 86.0 kN，`K_y` = -803 N/deg，峰值 Fx 4840 N，峰值 Fy -3780 N，拖距 31 mm，`M_z` 峰值约 50 Nm（出现在 4° 附近）。Release 下每次求值约 250 ns，4 轮 x 1000 Hz 每步约 1 us。
- 变异检查：把 `qDz4 gamma^2` 改成 `qDz4 gamma`，参考点测试会失败。
- 书中方程的已知问题：`mu_y` 的分母是 `1 + pDy3 gamma^2`，表中 `pDy3` = -11.23，所以 `|mu_y|` 随外倾角增大（6° 时增大 14%），并在 |gamma| ≈ 0.3 rad 处发散。这是照书实现的结果，外倾测试只覆盖到 6°。

还没做：接入车辆（目前仍由 Jolt 的约束钳位出力）、松弛长度、低速处理、与 MFeval 的逐位对齐。

## 5. 建议的实施顺序

1. 自己的轮胎力，替换 Jolt 的约束钳位：组合滑移 + 载荷敏感 + 外倾，读 `tyres.ini`（解决第 1 节 1–4）。
2. 松弛长度 + 低速混合（5、7）。
3. M_z / 拖距（6），作为方向盘力反馈的来源。
4. 虚拟平带台：无头扫 kappa、alpha、Fz、gamma，断言峰值、侧偏刚度、摩擦椭圆。
5. 以后：Stocco 刷子模型作为可选模型；胎温、磨损。

## 6. 带柔性胎体的刷子模型已接入车辆（2026-10-03）

用户的验收标准："在模拟器里能开带柔性胎体的刷子模型"。原第 5 节的顺序因此调整：直接实现 Stocco 模型，并作为车辆可选的轮胎模型；MF 保留为对照用。

### 6.1 模型（`engine/tyre/tyre_brush.{h,cpp}`）

- 结构照论文：
  - 胎面横向切成 n 根胎肋（默认 5 根），每根胎肋沿接地长度分成 20 段；
  - 接地长度用 (6) 式，过渡半径 `R_l = 0.45 R0`；
  - 压力用 (7) 式的四次多项式（λ = 4，δ = 0）；
  - 胎体中心线用 (10) 式的抛物线，状态 `c = [x_c, y_c, theta_c]`；
  - 刚度含 (14)(15) 的触底项；
  - 粘着/滑移按 (48) 判断，过了分界点整根胎肋都算滑移；
  - 滑移区摩擦按滑移速度做 Stribeck 衰减，载荷敏感性用 `(Fz/Fz_ref)^(e-1)`。
- **和论文不同的地方**：
  1. **胎体是动态的**：论文求静力平衡 `K c = F(c)`。这里保留 (21)(22) 式里的胎体速度，用后向欧拉解 `K c + D c' = F(c, c')`。好处是松弛长度从胎体刚度里自然得出（实测侧偏角阶跃后 0.14 m 达到 63%），静止时也能像弹簧一样撑住车。`dt <= 0` 时退化为论文的稳态。
  2. **分界点在积分段内插值**：分界点每越过一段，力就跳变一次，牛顿法会卡住。改成在段内线性插值分界位置后，残差变成连续的。
  3. **低速正则化**：胎面通过接地区的速度下限取 `sqrt(V_r^2 + v0^2)`（v0 = 0.3 m/s），滑移方向在 0.03 m/s 内平滑过渡。代价是在持续拉力下会缓慢蠕动：10° 坡上刹死，5 s 内滑动 7–11 mm。
  4. **外倾**：外倾角通过逐根胎肋的压缩量（接地长度、滚动半径）起作用，另外以 `(1-ε)·ω·sinγ` 的转向滑移形式进入，取 1-ε = 0.3。
- **求解**：牛顿法加 Broyden 更新，配合线搜索；有限差分 Jacobian 在步与步之间沿用。平均每步约 2.75 次求值，单个轮胎 Release 下约 5–7 µs。
- **参数**：`MakeBrushTyreParameters` 用峰值摩擦、参考载荷、峰值侧偏角、滑移后剩余抓地比例、半径、宽度、垂向刚度来生成参数：
  - 先按刚性刷子的 `tan α_sl = 3μFz/C_α` 估出刷毛刚度，再用稳态曲线迭代，把峰值放到给定角度；
  - 胎体刚度取横向 `C_α/(0.6 R0)`、纵向 `C_κ/(0.4 R0)`、扭转 `1.5 C_α R0^2`，阻尼按 0.5 ms 时间常数取。

### 6.2 接入车辆（`engine/physics/physics_world.cpp`）

- 新增 `VehicleSettings::tyreModel`（`VehicleTyreModel::PhysicsEngine | Brush`）。默认仍是 PhysicsEngine，原有测试不受影响；编辑器的默认调校（`VehicleDriveService::DefaultTuning`）用 Brush。
- 每个轮胎的参数由该轴的 `VehicleTyreSettings` 生成：纵横向峰值摩擦的均值、`peakSlipAngleDegrees`、`postPeakShare`，加上车轮尺寸、该轴 `tyreRate` 和静载荷。没有数据时用 μ = 1.1、峰值 7°。
- 在 Jolt 车辆约束的 `PostCollideCallback` 里调用：这时 Jolt 已经找到接地点，控制器还没处理发动机和刹车。每个轮胎在这里：
  1. 读取接地坐标系下的速度、上一步的悬架载荷、对地外倾角和路面摩擦；
  2. 推进一步；
  3. 在接地点把 Fx/Fy 加到车身，绕法线加 Mz，并把 `-Fx·R_e` 加上滚动阻力矩作为轮的旋转扭矩。
- Jolt 自己的轮胎冲量上限设为 0（`SetTireMaxImpulseCallback`），刹车和发动机仍由 Jolt 控制器处理。
- 多连杆悬架读取刷子轮胎的力，用于抗点头、抗下蹲和顶升效应。`VehicleWheelState` 新增 `brushTyre`、`aligningTorque`、`slidingShare`、`carcassDeflection`。
- 同时修了两处辅助逻辑：
  - **牵引力控制**：刷子轮胎下按摩擦圆剩余的纵向余量计算，并在驱动轮滑移率超过 0.1 时让离合器进一步打滑；
  - **限滑差速器**：锁止扭矩改为随实际油门变化。原来按全油门算，松油门时差速器也几乎锁死，在弯里白白占用摩擦圆。
- 编辑器 Vehicle 面板：新增 "Tyre Model" 下拉框，驾驶时显示每个轮胎的载荷、Fx、Fy、Mz、滑移比例和胎体变形。

### 6.3 验证

- `miniengine.tyre`（新增 5 项）：
  - 刚性胎体、抛物线压力时，和 Pacejka 3.2.1–3.2.2 刷子模型的解析式误差在 0.3% 以内；
  - 147 组组合滑移和外倾工况下全部收敛，且都不超出摩擦圆；
  - 峰值角标定到 4°/7°/10° ± 0.6°；
  - ISO 符号、外倾推力方向、倒车对称性正确；柔性胎体使侧偏刚度低于刷毛自身的刚度；
  - 阶跃响应单调无过冲，第一毫秒不到 20%；
  - 静止时零力，推动时像弹簧一样撑住，松开后缓慢回弹。
- `miniengine.vehicle_brush_tyre`（Boxster 直弹簧，GT-R 多连杆加簧下质量），刷子轮胎对比 Jolt：

| | Boxster 刷子 | Boxster Jolt | GT-R 刷子 | GT-R Jolt |
|---|---|---|---|---|
| 0-100 km/h | 6.19 s | 6.08 s | 5.73 s | 5.60 s |
| 100-0 km/h | 50.8 m | 52.5 m | 57.4 m | 59.8 m |
| 20 m/s 定圆，转向 0.1 / 0.3 / 0.6 | 0.57 / 1.07 / 1.04 g | 0.58 / 1.26 / 1.17 g | 0.57 / 1.40 / 1.27 g | 0.53 / 1.54 / 1.45 g |
| 平地静止 5 s 位移 | 0 mm | | 0 mm | |
| 10° 坡刹死 5 s 位移 | 11 mm | | 7 mm | |
| 每模拟秒物理耗时（Release） | 96 ms | 61 ms | 109 ms | 72 ms |

  刷子轮胎的极限侧向加速度比 Jolt 低 10–15%，因为纵横向共用一个摩擦圆；Jolt 两个方向互不影响，会凭空多出抓地力。在加入上面的牵引力控制和限滑差速器修正之前，Boxster 在转向 0.3、补油维持车速时会因功率转向过度而甩尾。

### 6.4 还没做

- 胎体参数没有用 AC 的 `FLEX` 和 `RELAXATION_LENGTH` 标定；
- 没做路肩包络（论文的 [73]）；
- 没做胎温和磨损；
- 没有用 MF 曲线对刷子轮胎做系统拟合；
- 方向盘力反馈还没接 Mz。

### 6.5 路面抓地（2026-10-03）

原来的问题有两个：
- 摩擦合成方式不一致。有轮胎数据的车用"轮胎 μ × 路面"，没有数据的车走 Jolt 的 √(轮胎 μ × 路面)，冰面上会得到 0.35；
- 低摩擦路面被高估。GT3 光头胎 μ = 1.6，在雪上会得到 0.48。

现在的做法：

- **`SurfaceGrip`**（`physics_world.h`），每个静态刚体一份，从 `MINIENGINE_collision` 的可选字段读入：
  - `friction`：相对干沥青的比例，乘到轮胎 μ 上，和 AC 的 surfaces.ini 一致；
  - `frictionCap`：μ 的绝对上限。非铺装路面的抓地力由路面决定，用来给冰雪、碎石、土路设上限；
  - `wetSpeedFalloff`：湿路抓地力随车速衰减，按 `exp(-k v)`；
  - `slidingShare`：松散路面上滑动摩擦占峰值的比例；
  - `rollingResistance`：软地面额外的滚动阻力系数。
- **两种轮胎模型统一处理**：
  - 刷子轮胎：`frictionScale`、`frictionCap`、`slidingShare` 和额外滚动阻力都进入 `BrushTyreInput`；
  - Jolt 轮胎：合成回调一律改为乘法并套上上限（去掉 √），软地面的滚动阻力作为轮子上的扭矩施加。
- **测试场数值**（`tools/render_scenes/make_car_test_track.py`，已重新生成 fixture，并复制到 `assets/models`）：比例为 Wong《Theory of Ground Vehicles》表 1.3 的峰值除以干沥青的 0.85，上限取 Wong 的峰值，滑动比例取 Wong 的滑动值除以峰值；滚动阻力按 Bosch《汽车手册》。

| 路面 | 比例 | 上限 | 滑动比例 | 额外滚阻 | 依据 |
|---|---|---|---|---|---|
| 干沥青 / 凸起 | 1.0 | — | 轮胎自身 | — | Wong 0.8–0.9 |
| 混凝土 | 0.95 | — | | — | Burckhardt 1.09 / 1.17 |
| 湿沥青 | 0.95 × e^(−0.0173 v) | — | 0.86 | — | 30 km/h 时为干地的 0.7，100 km/h 时为 0.5（Wong 0.5–0.7） |
| 草地 | 0.53 | 0.45 | 0.9 | 0.06 | 文献 0.4–0.5；田地滚阻 0.1 起 |
| 土路（干） | 0.8 | 0.68 | 0.96 | 0.037 | Wong 0.68 / 0.65；未铺装路面滚阻 0.05 |
| 碎石 | 0.71 | 0.6 | 0.92 | 0.012 | Wong 0.6 / 0.55；压实碎石滚阻 0.02 |
| 烂泥（湿土） | 0.65 | 0.55 | 0.82 | 0.09 | Wong 湿土路 0.55 / 0.4–0.5 |
| 压实雪 | 0.24 | 0.2 | 0.75 | 0.013 | Wong 0.2 / 0.15 |
| 冰 | 0.12 | 0.1 | 0.7 | — | Wong 0.1 / 0.07 |

- **验证**（`miniengine.vehicle_brush_tyre` 的 surfaces 项，测试中关掉车身的线性阻尼）：

| | Boxster | GT-R |
|---|---|---|
| 从 15 m/s 刹车，冰（上限 0.1） | 0.071 g | 0.071 g |
| 从 15 m/s 刹车，雪（上限 0.2） | 0.152 g | 0.152 g |
| Jolt 轮胎在冰上 | 0.099 g | 0.100 g |
| 湿路抱死，9 m/s / 30 m/s | 0.94 / 0.65 g | 1.16 / 0.80 g |
| 滑行，沥青 / 草地 | 0.041 / 0.094 g | 0.034 / 0.089 g |

  冰雪上的数值正好是"上限 × 滑动比例"，因为车轮抱死后进入滑动；Jolt 轮胎没有滑动比例，所以贴着上限本身。Boxster 湿路的理论值是 0.91 / 0.63 g（1.3 × 0.86 × 路面比例），实测与之吻合。
- **未改动**：AC 赛道（Spa）仍只有 surfaces.ini 里的 FRICTION 比例，和 AC 的行为保持一致；AC 的 DAMPING 等字段还没读。Jolt 轮胎不支持滑动比例和湿路随车速的变化，因为它的曲线是按车建的，不是按路面建的。

### 6.6 接地区可视化（2026-10-03）

- 物理叠加层新增 "Brush Contact Patch"（默认打开）和 "Deformation Scale"（默认 10 倍）。每个刷子轮胎会画出：
  - 静止时的接地区（灰框）；
  - 每根胎肋的实际接地长度：从前缘到粘着/滑移分界为绿色，之后为红色，分界处画一条白线；
  - 胎体中心线（黄色）：按 x_c 平移，并按 `y_c + θ_c (x - x_c) - y_c Ψ/2 (x - x_c)^2` 横向偏移、弯曲和扭转。胎体变形只有几毫米，所以按放大倍数绘制。
- 数据链：`BrushTyreOutput::ribs`（每根胎肋的位置、接地长度、从前缘起的粘着长度）→ `VehicleWheelState::brushRibs`、`carcassBendingShape`、`treadRollingForward` → `DrawBrushPatch`（`editor_vehicle_overlay.cpp`）。
- 验证：`miniengine.vehicle_overlay`。GT-R 以 20 m/s 右转（外侧前轮 Fy = −5.6 kN）；断言胎肋长度、粘着长度和胎体侧移方向（与路面对轮胎的力同向，−7.9 mm），并把叠加层软件光栅化后统计绿、红、黄像素。设置 `MINIENGINE_UI_SNAPSHOT_DIR` 时会输出 `brush_contact_patch.png`。
