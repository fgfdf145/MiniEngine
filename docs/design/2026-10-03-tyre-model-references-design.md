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

## 5. 建议的实施顺序

1. 自己的轮胎力，替换 Jolt 的约束钳位：组合滑移 + 载荷敏感 + 外倾，读 `tyres.ini`（解决第 1 节 1–4）。
2. 松弛长度 + 低速混合（5、7）。
3. M_z / 拖距（6），作为方向盘力反馈的来源。
4. 虚拟平带台：无头扫 kappa、alpha、Fz、gamma，断言峰值、侧偏刚度、摩擦椭圆。
5. 以后：Stocco 刷子模型作为可选模型；胎温、磨损。
