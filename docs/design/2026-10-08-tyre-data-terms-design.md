# 轮胎数据的逐步项（AC 轮胎模型 V10）

日期：2026-10-08

## 背景

共享轮胎库（`2026-10-08-tyre-library-design.md`）之后的第二阶段：把 tyres.ini 里刷子轮胎模型能直接承接的参数接进物理。约定是每一项接之前先确认 AC 的公式。

社区资料（论坛、mod 的 ini 注释）只有零散说法，彼此还矛盾（例如 DCAMBER 有人写成 `D·(1 − (…))`）。游戏目录里的 `acs.exe` 附带了完整的 `acs.pdb`，所以公式直接从游戏自己的代码读：

- 用 dbghelp 列出符号：`Tyre::addTyreForcesV10`、`SCTM::solve`、`SCTM::getPureFY`、`Tyre::addGroundContact`、`Tyre::getDX/getDY`、`Tyre::getCorrectedD`、`Tyre::initCompounds`、`Tyre::setCompound` 等。
- 用 dumpbin 反汇编，并把字符串和常量标注出来；从 `initCompounds` 得到 ini 键 → `TyreCompoundDef` 偏移，再经 `setCompound` 得到运行时偏移（`Tyre` 里的模型数据比 def 偏 0x10，SCTM 对象在 `Tyre+0x660`）。
- 每条公式至少有两处互相印证（读取处的键名、使用处的偏移）。脚本在会话草稿目录，没有进仓库。

## 从游戏代码确认的公式（V10）

记 α 为滑移角（向左滑为正，路面把胎推向右），κ 为滑移率，γ 为外倾（顶部向右为正），Fz 为载荷，ω 为轮速，R 为半径。

| 键 | 游戏里的做法 | 位置 |
| --- | --- | --- |
| CAMBER_GAIN | 外倾推力折成滑移角：α' = α + sin(γ)·CAMBER_GAIN | `SCTM::solve` |
| DCAMBER_0 / _1 | 只作用于横向抓地：Dy / (1 + K0·g − K1·g²)，和小于等于 −1 时取 −0.9；g = ±\|γ\|，外倾和 α' 同向（顺着力的方向倾斜）时取负。最佳外倾 g = K0/(2·K1)，RX-7 是 −2.6°，此时 +2.8%。有 DCAMBER_LUT 时改查表（按角度） | `SCTM::solve`、`Tyre::getCamberedDy` |
| SPEED_SENSITIVITY | Dx、Dy 都除以 1 + SS·v_slide，v_slide = √((V sinα)² + (V cosα·κ)²)，即接地点的滑动速度 | `SCTM::solve`（SCTM+0x2C ← SPEED_SENSITIVITY） |
| BRAKE_DX_MOD | 读入时存成 1 + 值；κ < 0（制动）时 Dx 乘它 | `initCompounds`、`SCTM::solve` |
| ROLLING_RESISTANCE_0 / _1 / _SLIP | 力矩 = −Fz·0.001·(RR0 + RR1·v²)·sign(v)·p·(1 + 0.001·RR_SLIP·clamp(s/s_peak, 0, 1))·R，v = R·ω；\|ω\| > 1 才有，滑移项只在 \|ω\| > 20 时加；p = 1 + (P_ideal/P − 1)·PRESSURE_RR_GAIN | `addTyreForcesV10` |
| RADIUS_ANGULAR_K | 读入时 ×0.001（毫米→米）；R = RADIUS + K·\|ω\| | `initCompounds`、`addGroundContact` |
| FRICTION_LIMIT_ANGLE / FLEX_GAIN | 峰值滑移（理论滑移的 tan）：FZ0 处 tan(FLA)，2·FZ0 处 tan(FLA·(1 + FLEX_GAIN))，中间按载荷线性插值；再乘 (1 + 0.75·(D − 1))，D 是温度/胎压/磨损的抓地系数 | `initCompounds`、`SCTM::solve` |
| FALLOFF_LEVEL / FALLOFF_SPEED | 峰后：L + (1 − L)/(1 + FALLOFF_SPEED·(s − s_peak))，s 是归一化的组合理论滑移；峰前是刷子多项式 (1 − t)²·k·s + t²(3 − 2t) | `SCTM::getPureFY` |
| COMBINED_FACTOR | 两个方向理论滑移的 p-范数指数（默认 2，即欧氏） | `SCTM::solve` |
| CX_MULT | 纵向的滑移刚度倍数 | `SCTM::solve`（SCTM+0x3C） |
| LS_EXPX/Y、DX/DY_REF、FZ0 | D = coef·Fz^(LS_EXP − 1)，coef 由 REF 在 FZ0 处换算（与导入器一致） | `Tyre::getDX/getDY`、`calcLoadSensMult` |
| PRESSURE_D_GAIN / IDEAL | D / (1 + \|P − P_ideal\|·D_GAIN) | `Tyre::getCorrectedD` |
| PRESSURE_SPRING_GAIN | 竖向刚度 = RATE + (P − P_ref)·SPRING_GAIN；胎压到轮辋时刚度取 200000 N/m | `Tyre::getDynamicK`、`addGroundContact` |
| BLISTER | D / (1 + 0.2·clamp(blister%/100, 0, 1)) | `getDX/getDY`、`SCTM::solve` |
| FLEX、XMU | 只给 V10 以前的 `BrushSlipProvider` 用（版本 ≥ 5 时 XMU 直接置 0），V10 不用 | `initCompounds` |

另外发现几个可选键，导入器原来不认识：ROLLING_RESISTANCE_SA / _SR（老版本的滑移阻力）、DX_CURVE / DY_CURVE（直接给载荷→抓地曲线）、DCAMBER_LUT / DCAMBER_LUT_SMOOTH、[ADDITIONAL1] 的 PRESSURE_TEMPERATURE_GAIN、BLANKETS_TEMP、CAMBER_TEMP_SPREAD_K。本机 178 台车都没有用到前两组（审计时没有未识别的内联曲线），先记下。

## 这次接进物理的

只接不需要新状态、并且和刷子轮胎的斜率曲线无关的项。实现是一个纯函数 `ComputeTyreStepTerms(tyre, motion)`（physics/vehicle_settings），每一步、每个轮位在刷子轮胎求解前调用：

1. **CAMBER_GAIN**：把外倾推力折算成侧向速度，v_y' = |v_x|·tan(α + sin γ·CG)。有 CAMBER_GAIN 的胎把刷子自己的外倾转向滑移（`camberSpinShare`）关掉，避免重复计算。
2. **DCAMBER_0/1**：作为横向摩擦的倍数（刷子输入新增 `axisFrictionScale`）。
3. **SPEED_SENSITIVITY**：两个方向的摩擦都除以 1 + SS·v_slide。
4. **BRAKE_DX_MOD**：胎面比地面慢时，纵向摩擦乘以 1 + MOD。
5. **ROLLING_RESISTANCE_0/1/SLIP**：RR0 作为刷子轮胎的滚阻系数（RX-7 是 12 → 0.012，和原来的默认值相同）；RR1 的速度平方项、RR_SLIP 的滑移项按上式每步算，加到滚阻系数上。滑移项用的 s/s_peak 按游戏的理论滑移和 FLEX_GAIN 的峰值插值来算。
6. **RADIUS_ANGULAR_K**：刷子轮胎的滚动半径（有效半径、接地长度）加上 K·|ω|（刷子输入新增 `radiusGrowth`）。

胎压按"等于理想胎压"处理（PRESSURE_RR_GAIN 的系数为 1，PRESSURE_D_GAIN 不减抓地）：游戏里冷胎从静态胎压出发、热了以后接近理想值，而这台引擎还没有胎温。胎温和胎压是第三阶段。

## 没接、以及原计划要改的地方

原计划里有几项读了代码以后发现不对：

- **FALLOFF_SPEED 不是滑动速度的 Stribeck 曲线**，是峰后按归一化滑移衰减的速度；**FLEX_GAIN** 是峰值滑移随载荷的变化。两者都属于游戏自己的斜率曲线（SCTM），刷子轮胎的峰后衰减和峰值滑移是由刷毛和接地长度算出来的，硬套会偏离游戏。
- **FLEX、XMU** V10 根本不用。
- **WIDTH** 在 V10 的力计算里不出现（只给温度分区用），刷子轮胎的宽度仍取 3D 模型的轮子。
- **COMBINED_FACTOR、CX_MULT** 的含义是 SCTM 里的组合滑移范数和纵向刚度倍数；刷子轮胎已经用 CX_MULT 做刷毛的纵横刚度比，COMBINED_FACTOR 只用在滚阻滑移项的 s 上。

要让这些也和游戏一致，办法是把 SCTM 本身实现成一个可选的轮胎模型（和刷子并列），由用户决定。

## 测试

- `TestTyreDataTermsFollowTheGame`（vehicle_physics_tests）：用 RX-7 半热熔胎的数据逐项手算核对：外倾推力、顺/逆倾斜的 DCAMBER（最佳外倾 +2.85%、反向 −7.7%）、滑动速度、制动、滚阻的速度项和滑移项（峰值处 6.065 倍、一半处按比例、20 rad/s 以下没有滑移项、1 rad/s 以下只有 RR0）、半径增长。没有这些数据的胎完全不受影响。
- 原有的车辆物理、刷子轮胎（Release）、转向辅助、悬挂测试照常通过（测试用的车不带这些键，行为不变）。
- 实车对比探针：`MINIENGINE_TYRE_PROBE=<car.gltf> miniengine_kn5_import_tests`，平地、刷子轮胎，同一台车去掉/带上这些项：

| | RX-7 去掉 → 带上 | R34 去掉 → 带上 |
| --- | --- | --- |
| 0–100 km/h | 5.80 → 5.90 s | 6.67 → 6.83 s |
| 120 km/h 空挡滑行 10 s | 97.0 → 95.2 km/h | 102.4 → 100.4 km/h |
| 100 km/h 刹停 | 35.1 → 34.1 m | 约 35.5 → 34.5 m（按车速平方折算） |
| 70 km/h、四分之一方向盘稳态转弯 | 1.01 → 0.99 g | 0.88 → 0.87 g |

起步慢一点是因为打滑时 SPEED_SENSITIVITY 降抓地、RR_SLIP 加滚阻；滑行慢一点是 RR_1 的速度平方项；刹车短一点是 BRAKE_DX_MOD（RX-7 +5%）。没有出现不稳定。
