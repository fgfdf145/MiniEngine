# Yuki 当 R34 的司机

目标：Yuki（C:\Project\Yuki 的 AnimateApp 角色，assets/Yuki/Yuki.glb）坐进 Skyline R34 的驾驶座，
跟着车走；开车时双手握方向盘随转向转动，座舱视角从她的眼睛看出去。

Yuki 的 25 个动画里没有坐姿/驾驶动作，所以姿势是程序化生成的（不需要 Blender 做新动画）：
座椅几何从车模型里测出来，身体用两骨 IK 摆上去。

## 1. 数据与接口

- `ModelComponent::driverVehicleUuid` / `driverSeatOffset`：这个模型坐进哪辆车（车实体的 entity uuid），
  座位偏移（米：车的右、上、前）。场景 yaml 里是实体级的
  `driver: { vehicle: <uuid>, seat_offset: [x, y, z] }`。
- Inspector → ModelComponent → **Driver**：`Drives` 下拉列出场景里有方向盘（STEER_HR）的模型，
  `Seat Offset (m)` 微调座位，坐不进去时显示原因（车不在场景里 / 车没有方向盘 / 没有人形骨架）。
- assets/scenes/rolling_road.yaml 已经加上 Yuki，坐在 R34 里。

## 2. 跟车（VehicleDriverService）

每帧在 VehicleDriveService::Tick 之后、ModelAnimationPlayback::Tick 之前：

- 司机的模型空间 = 车的 vehicle space（+Z 前、+X 车左、+Y 上，原点是车模型原点）：
  `driverMatrix = carModelMatrix * mat4(vehicleToModel)`。Yuki 本来就是 +Z 朝前、+X 是她的左边，
  所以姿势目标直接用 vehicle space 的坐标。车的缩放也作用在司机身上。
- 司机的 transform 每帧被覆盖，gizmo 拖她没用；要挪位置用 Seat Offset。
- 开车时：方向盘转角来自 `VehicleDriveSession::steeringWheelTurn`（从 BuildWheelSubmeshTransforms
  拆出来的 `SteeringWheelTurn`，和方向盘网格转的角度完全一样）。
- 座舱视角：`session->driverEyes` = 她两只眼睛关节的中点，座舱相机放在那里（再加 Vehicle 面板的
  seat offset）；同时把 Head_M 缩到 1e-3，头、头发、眼睛都收进脖子里，从里面看不到。
- 开车时的静态碰撞跳过所有蒙皮网格（角色会动，不该是赛道的一部分）。

## 3. 座椅拟合（FitDriverSeat，每对车/角色模型算一次并缓存）

全部在 vehicle space，射线打车自己的网格（排除车轮、方向盘、玻璃等 Blend 材质、水面，
以及 AC 的安全带 CINTURE_ON —— 它挂在座椅前面，会挡住测座椅靠背的射线，最初量出 5° 的假倾角）：

1. 方向盘：STEER_HR 的中心和轴（轴翻转成指向远离司机，转角符号跟着翻）；握持半径 = 方向盘网格
   离转轴最远的顶点 − 1.2 cm（半个轮缘粗）。R34：0.173 m。
2. 眼睛：车数据的 DRIVEREYES，否则方向盘后 0.59 m、上 0.29 m（R34 两者一样）。
3. 坐垫：从眼睛下 0.2 m、前 0.05 m 往下打 → R34 坐垫面 y = 0.409。
4. 靠背：坐垫上 0.15 m 和 0.40 m 两条往后打的射线 → 位置和倾角，R34 17°。
5. H 点（髋关节中点）：靠背前 0.13 m、坐垫上 0.09 m（SAE H 点人体模型的数）。
6. 地板：H 点前 0.65 m 往下打（要越过坐垫前沿，最初从 0.3 m 打，打中了坐垫前缘，脚被放到座椅下面，
   变成跪姿）→ 0.243；脚踏板/防火墙：从那里往前打 → 0.829。
7. 脚踝：防火墙后 0.22 m、地板上 0.10 m，左右各偏 0.10 m；腿伸直不超过 95%。
8. 手够不着时：先让上身离开靠背往前倾（每步 2°，最多 20°），还不够再把髋往前滑（每步 1 cm），
   直到两只手腕需要的距离都不超过手臂长度的 98%。

Yuki 身高约 1.6 m，手臂（肩到腕）只有 0.48 m，而 AC 的座位是给成年男性司机设的：R34 上结果是
**前倾 20°、往前滑 0.13 m**，手臂基本伸直。只往前滑（不前倾）要滑 0.15 m 以上，膝盖会顶到方向盘下缘
（膝盖比髋高 0.24 m），所以优先前倾。

## 4. 姿势（PoseDriver，engine/asset/model_driver_pose.*）

骨骼按 AdvancedSkeleton 的名字找（Root_M、Spine1_M、Chest_M、Neck_M、Head_M、Hip/Knee/Ankle/Toes、
Scapula/Shoulder/Elbow/Wrist、ElbowPart1/2、*Finger1..3、Eye_L/R，_L/_R 后缀）；没有这些关节的
蒙皮模型只跟车，不摆姿势。静止姿态是 T-pose（手臂平伸、掌心向下、脚平放），关节铰链轴从静止姿态推出：
肘让前臂向前弯，膝让小腿向后弯。

1. 骨盆绕左右轴后仰（靠背倾角），平移到 H 点；脊柱和胸各前倾一半 lean；脖子、头各收回一部分，
   让视线回到水平；头随方向盘转角往弯里看（转角 × 0.1，最多 20°）。
2. 腿：两骨 IK，膝盖朝上并向外撇（让膝盖从方向盘轮缘外侧过去）；用"骨方向 + 铰链轴"两向量对齐
   大腿，铰链不扭；小腿最短弧对准；脚尖沿踏板斜面抬 40°。
3. 裙子（Root_M 下面的 Skirt_* 链）：每条链在第一个关节转动，让链末端沿大腿方向躺在腿上；
   前面全跟，侧面部分跟，后面不动（被座椅挡住）。直接复制大腿旋转会让裙子前片竖起来挡在胸前。
4. 手：握在 3 点和 9 点（转向时跟着轮缘转，最多 ±120°，再多手就在轮缘上滑），手腕在轮缘后 4.5 cm、
   外 3 cm；手臂快伸直时肩胛向目标转最多 15°；两骨 IK 肘部朝下朝外；手掌朝方向盘中心，
   手腕的扭转按 1/3、2/3 分给前臂扭转关节（ElbowPart1/2），避免手腕糖纸扭；
   四指三个关节弯 45°/70°/45°，拇指 10°/20°/20°，绕手的横轴，一次设定。
5. 输出眼睛位置；需要时把头缩掉。

`model_animation` 拆成 RestNodePoses / EvaluateNodePoses / ComputeNodeWorldMatrices /
PaletteFromNodeWorldMatrices 四步，驾驶姿势和动画片段共用后两步。ModelAnimationPlayback 对设了
驾驶姿势的实体用 EvaluateDriverPalette 代替片段。

## 5. 验证

- `miniengine_driver_pose_tests`：合成人形骨架上髋到位、脚踝到位、膝盖朝上、手在轮缘对的一侧、
  肘朝下、右转时左手上右手下、藏头；Inspector 的 Driver 区块无 GPU 截图
  （`MINIENGINE_UI_SNAPSHOT_DIR`）；场景保存/读取保留 driver 字段。设
  `MINIENGINE_DRIVER_MODEL`、`MINIENGINE_DRIVER_CAR` 会对真实模型拟合并打印各关节位置。
- 全部 116 个 ctest（Release）通过。
- 截图（rolling_road.yaml，Release）：侧面车窗里坐姿正常；座舱视角头部消失、相机在眼睛处，
  直行时两只手在画面下沿 3/9 点，左转右手越过顶部，右转左手越过顶部。

## 6. 没做的

- 踏板动作（油门/刹车/离合脚不动）、换挡手。
- 方向盘转过 120° 后手在轮缘上滑动，不做交叉换手。
- 车身加减速/侧倾时身体不晃（刚性跟车）。
- 头发、裙子没有布料物理（与 Yuki 着色文档里的未完成项一样）。
- 座位只拟合一次；换车或角色模型重新载入时才重算。
