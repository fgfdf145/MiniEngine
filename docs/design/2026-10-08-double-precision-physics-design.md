# 物理世界改用双精度坐标

日期：2026-10-08

## 问题

Jolt 原来按单精度编译：世界坐标（`RVec3`）是 float。float 在 x 处的间距约为 |x|·1.2e-7：

| 离原点 | 相邻坐标间距 | 1 ms 一步能移动的最小速度（间距的一半 / 1 ms） |
| --- | --- | --- |
| 300 m | 3e-5 m | 0.015 m/s |
| 2000 m | 1.2e-4 m | 0.06 m/s |
| 6000 m（GTA 地图里 VC 的偏移） | 4.9e-4 m | 0.24 m/s |

低于这个速度，一步的位移直接被舍掉：车身速度不为零，位置却不动，轮子按车速转。看起来就是“车停着、轮子在慢慢转”。速度略高时位移又被量化成整格，车会一顿一顿地走。

## 做法

1. **Jolt 双精度编译**：`cmake/vcpkg-overlay-ports/joltphysics` 是上游 5.5.0 的 port，加了 `-DDOUBLE_PRECISION=ON`（port-version 1）。Jolt 导出的 `JPH_DOUBLE_PRECISION` 只作用于私有链接 Jolt 的 `engine_physics`。
   - 世界坐标 `RVec3`/`RMat44` 是 double。
   - 速度、旋转、形状的局部坐标和接触仍是 float。它们都是相对量，精度不受离原点远近影响。
2. **物理接口的世界坐标改成 double**（`physics_world.h`）：
   - `PhysicsPose::position`、`VehicleWheelState::mount` / `contactPosition`、`VehicleLinkage` 的各个点改为 `glm::dvec3`；
   - `AddStaticBox(center)`、`FindGroundBelow` 也改成 double；
   - 方向、力、车身坐标系里的量仍是 float；
   - 内部 `FromJoltPosition` / `ToJoltPosition` 负责转换。原来的 `FromJolt(RVec3)` 会把坐标截成 float，已经删掉。
3. **静态网格用“double 原点 + float 局部顶点”**：
   - `AddStaticMesh(vertices, indices, grip, origin)` 的顶点相对于 `origin`，Jolt 的静态体就放在 `origin`；
   - 驾驶服务把每个实体的平移当作 origin，顶点只乘旋转缩放（`mat3`），不再在 float 里变换到世界坐标。原来 VC 区域的碰撞顶点在送进物理之前就只剩 0.5 mm 的精度。
4. **与 float 世界的边界**：场景（`TransformComponent`）、相机和渲染仍是 float。在边界上，先用 double 算完，最后一步再转成 float：
   - 车的实体矩阵（`ComposeMatrix`）、追尾相机与车内相机（相对偏移是 float，加到 double 车位上以后再转）、四机位录制相机、小地图、物理叠加层（在进入 `Painter` 时取整）。
   - 两个世界坐标相减（轮子相对车身、轮子在哪一侧），先在 double 里相减，再转成 float。

## 没改的

- **水面**仍用 float 世界坐标：它只决定浮力，0.5 mm 的误差不影响。
- **场景、相机和渲染**仍是 float。渲染时车的位置在 6000 m 处是 0.5 mm 一格，肉眼看不出；物理本身不再丢运动。要彻底消除远处的顶点抖动，需要相机相对渲染（另一个工程：场景变换、TLAS 实例、DDGI、流式加载都要改）。

## 注意

GLM 默认允许 `dvec3` 隐式转成 `vec3`，会悄悄丢精度。写新代码时，世界坐标一律用 `dvec3`；转 float 只在渲染边界做，并且要写成显式的 `glm::vec3(...)`。

## 验证

- `miniengine.vehicle_brush_tyre` 新增“far from the origin”测试：车以 0.5 m/s 放手，分别在原点和 (6000, 6, -6000) 各跑一次，起步位移和滚动距离都要相同（相差 < 1 mm）。
- 其余车辆、轮胎、叠加层、相机、录制测试全部通过。
