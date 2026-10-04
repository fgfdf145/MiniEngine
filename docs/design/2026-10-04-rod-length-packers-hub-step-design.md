# ROD_LENGTH、PACKER_RANGE、RIM_OFFSET、防倾杆单位、轮毂梯形积分

日期：2026-10-04。原则：严格按 AC 数据（用户要求），即还原 AC 如何解读它自己的数据，不向真车数值修正。
关联：`2026-10-02-vehicle-suspension-integration-and-rigs-design.md`、`2026-10-04-unsprung-corner-integration-notes.md`。

## 1. 证据

Kunos 没有公开这些字段的完整定义。依据：

1. 已安装 178 辆车 `suspensions.ini` 里 Kunos 写的注释（统计次数）：
   - `ROD_LENGTH`：「push rod length in meters. positive raises ride height, negative lowers ride height.」(161)
   - `BUMPSTOP_UP`：「range of suspension travel in bump is bump+rodlength」(27)
   - `PACKER_RANGE`：「Total suspension movement range, before hitting packers」(161)，「suspension travel in meters left by packers.」(6)
   - `[ARB] FRONT/REAR`：「antiroll bar stiffness. in Nm」(176)，与 `SPRING_RATE`「Wheel rate stiffness in Nm」同样把 N/m 写成 Nm；
     4 辆写「incl. 20% Bushings」。
   - `RIM_OFFSET`：注释是复制错的（「Front sprung mass」「masse sospese anteriori」），无信息。
2. 社区（非官方）：
   - overtake.gg「BaseY-Rod Steward-Pickup-CG-Ride Height」：车轮先从设计零点下移 rod_length，再被簧上角重 / 轮端刚度压回
     （mclarenf1papa、prodarwin、Stereo）。
   - 搜索摘要（Steam 设置指南、overtake.gg 等）：「rod length represents the amount of preload at design height」；
     「packer range is the distance from the current suspension position (unloaded) to … a bumpstop」；
     「rod length decreases the packer range」；packer 是「垫片，让缓冲块在想要的车高被碰到」。
   - `RIM_OFFSET`：「若坐标从轮毂安装面量起，就填真实轮辋偏距」。

## 2. 解读与实现

| 字段 | 解读 | 实现 |
|---|---|---|
| `ROD_LENGTH` (L) | 设计位置时弹簧被压缩 L：`F = k (z + L) + kp (z + L)²/2`（z 压缩为正，从设计位置算）；静止位置 `z = 载荷/k − L` | `VehicleSuspensionAxle::rodLength`（可选）；有时弹簧预压 = `k L + kp L²/2`，刚度 `k + kp L`（`BuildVehicleCorner`）。没有该字段的车仍按「设计位置承载静载」 |
| `PACKER_RANGE` (P) | 从无负载位置算起到 packer 的行程；packer 让缓冲块（`BUMP_STOP_RATE`）提前作用：缓冲块从 `min(BUMPSTOP_UP, P − L)` 开始（至少 1 mm）。**推断** | `packerRange`；`VehicleBumpStopTravel` |
| `RIM_OFFSET` | 方向不明：R34 的 TRACK（1.48/1.49 m）就是车轮中心轮距，而 0.03 m 像真车 ET+30；AC 把车轮往里还是往外移无法判定 | 只导入、保存（`rimOffset`），不参与计算 |
| `[ARB]` | N/m：连接左右车轮的弹簧，每侧受 `k (z_本侧 − z_对侧)`（现状不变） | 不改 |

推杆长度和 packer 只用于独立悬挂；整体桥（AXLE）的刚度在弹簧处，含义不同，保持原样。

### 统计检验（已安装车，355 个车轴）

按上面的解读：静止位置超过 `BUMPSTOP_UP` 的 3 个；packer 在静止位置或其后的 5 个；packer 平均在静止位置上方 51 mm；
245 个车轴的 packer 早于 `BUMPSTOP_UP`（与「packer 让缓冲块提前接触」一致）。

## 3. 台架

- `BuildCarModel` 在 `BalanceCar` 之后保留推杆长度的预压；七柱台架构造时若车身不平衡，先静置（至多 5 s）到静止位置，
  `Heave/Pitch/Roll` 返回相对静止位置的位移（`RestHeave/RestPitch/RestRoll` 为静止位置），阶跃、气动、Live Rig 都以此为基准。
- K&C 的汇总、侧倾和转向扫描都在静止行程上取（轮台力等于该角静载的位置），在设计位置平衡的车不变。

## 4. 轮毂梯形积分

`StepUnsprungCorner` 和台架的游戏格式由向后欧拉改为梯形法：
`Δv = dt (F + ½ dt K v) / (m − ½ dt D − ¼ dt² K)`，`Δz = dt (v + ½ Δv)`。
最硬的轮毂模态（轮胎 325 kN/m + 弹簧 45 kN/m + 缓冲块 100 kN/m，55 kg）在 1 ms 下 ωΔt ≈ 0.09，梯形法不 L-稳定的缺点不起作用。

轻阻尼测试车（轮毂阻尼比约 0.24）1 ms 对 50 µs 参考：轮毂位移 ≤ 3.0%，车轮跳动峰 −0.4%（向后欧拉 −13%），轮载（相对峰值）≤ 3.3%，
车身 ≤ 7.0%（0.25 ms 时 1.9%：车身加速度滞后一步的耦合误差，一阶）。回归测试 `TestGameHubSchemeAtTheGameStep` 已按此收紧。

## 5. R34 结果

资产补了 `rodLength/packerRange/rimOffset`（前 0.12/0.12/0.03，后 0.015/0.11/0.03），备份
`out/backup/skyline_r34_vspec.gltf.before-rod-packer.backup`。按编辑器驾驶流程：

| | 之前 | 按 AC 数据 |
|---|---|---|
| 静止行程 前 / 后 | +0.1 / −0.3 mm | −20.7 / +53.0 mm |
| 俯仰 | +0.09° | −1.50°（车尾低） |
| 质心高度 | 0.528 m | 0.516 m |
| 前缓冲块位置 | 60 mm | 1 mm（P − L = 0） |
| 100 km/h 刹车时前轴最大压缩 | +39.8 mm | +13.0 mm |
| 起步时后轴最大压缩 | +35 mm | +82.5 mm（上缓冲块） |
| 0–100 km/h | 5.93 s | 5.94 s |

台架报告（静止位置）：轮端刚度 39.95 / 44.27 N/mm（数据 40 / 45），侧倾刚度前 56.7%，阶跃超调 0.5%，6000 N 气动下压前降 29.5 mm、后降 32.2 mm。

后轴静止在设计位置下 51 mm（静态压缩 66 mm，推杆只有 15 mm），离缓冲块 22 mm。这是对 AC 数据的直接计算；若 AC 实际不是这样，
最可能错的是 `PACKER_RANGE` 的解读和 `BUMPSTOP_UP` 的起算点。

## 6. 未做

- `RIM_OFFSET` 的作用；`[HEAVE_FRONT/REAR]`（第三弹簧，部分赛车）；悬挂损坏（`[DAMAGE]`）；`GRAPHICS_OFFSETS`。
