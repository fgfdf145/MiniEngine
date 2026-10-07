# BeamNG.drive 地图导入（网格地图 v2）

## 目标

用 BeamNG 自带的悬挂测试场地测试 R34 的悬挂：把 `levels/gridmap_v2`（网格地图 v2）原样搬进 MiniEngine，
几何、碰撞与路面抓地力都取 BeamNG 自己的数据。转换器写成 Python 工具（`tools/beamng/`），不进引擎：
和 GTA 地图一样，产物是普通 glTF，引擎侧不需要新代码。

## BeamNG 的关卡格式（实测于本机安装）

- **文件系统**：游戏把 `gameengine.zip`、`content/art_shapes.zip`、`content/assets/**/*.zip` 与
  `content/levels/<关卡>.zip` 都挂在根目录下，路径不区分大小写。`.link` 文件是 JSON 指针
  `{"path": ...}`；指向的贴图常以另一扩展名存放（`x.color.png` 实为 `x.color.dds`）。`bng_vfs.py` 直接读 zip。
- **场景**：`main/**/items.level.json`，每行一个对象（`class`、`__parent`、`position`、`rotationMatrix`、`scale`）。
  `rotationMatrix` 的 9 个数是**按列**写的：按行读时网格地图 v2 的 ramp park 四叶草拼不起来，按列读与
  关卡自带的 minimap 一致。`isRenderEnabled` 在所有 TSStatic 上都是 false（序列化默认值），不表示隐藏；
  `hidden` 才是。
- **预制件**：`.prefab` 是 TorqueScript（`new Class(name) { field = "value"; };` 嵌套），子对象在预制件空间，
  世界变换 = 预制件变换 × 子变换（子位置先乘预制件缩放，Torque 的 `Prefab::_updateChildTransform`）。
- **形状**：COLLADA（`.dae`，旁边的 `.cdae` 是缓存）。`baseXX/startXX` 下的网格名以细节尺寸结尾
  （`wall_a500`、`_L180`），取最大的非负尺寸；负尺寸（`Colmesh_*-1`）是碰撞，`LOS*` 忽略。3ds Max 导出的文件
  把网格放在无名子节点里（继承父节点名），单位可能是厘米（`<unit meter="0.01">`）。V 坐标要翻转。
- **碰撞类型**：TSStatic 的 `collisionType` 缺省为 "Collision Mesh"（用 Colmesh；形状没有就不碰撞，与 BeamNG
  一致），"Visible Mesh (Final)" 用最高 LOD 可见网格，"None" 不碰撞。
- **地形**：`.ter` v9 = 版本字节、尺寸 n、n² 个 u16 高度、n² 个 u8 图层（255 为洞）、材质名表；
  高度 = position.z + h / 65535 × maxHeight。网格地图 v2 的地形大部分是 56 m 处的平面，被 100 m 处的网格地板盖住。
- **地面模型**：`art/groundmodels.json`（带注释的 JSON）。材质的 `groundType`、地形材质的 `groundmodelName`
  经别名表映射到 ASPHALT、DIRT、GRASS 等。

## 转换（`tools/beamng/bng_build.py`）

1. 读场景，展开预制件，跳过隐藏对象；24 进程并行解析 181 个形状。
2. 每个 TSStatic 的最高 LOD 网格变换到世界（BeamNG 坐标），按 256 m 单元 × 材质合并成一个 glTF 网格。
   根节点绕 X 轴转 −90°，所以 BeamNG 的 (x, y, z) 在引擎里是 (x, z, −y)，北为 −Z，网格地板在 100 m。
3. 碰撞三角形按地面模型分组，写成 `MINIENGINE_collision`：`friction` = 该地面的 staticFrictionCoefficient /
   ASPHALT 的 0.98；松软路面（草、土、砂石、泥、沙、雪、冰、湿沥青）再加 `make_car_test_track.py` 的
   GRIP 表里的滚动阻力与滑移比例（滑移比例不低于 BeamNG 的 sliding/static）。
4. 地形按 64×64 格的块构网：块内全部采样共面且只有一种图层时画成围绕中心的扇形，否则保留每个方格
   （棋盘对角线）；两块共面块之间的边只留端点，其它边保留每个采样，因此相邻块总在同一组顶点相接，没有裂缝
   和 T 型顶点（未焊接的接缝会把车轮弹飞，见 rolling road 的经验）。网格地图 v2 的地形由 840 万三角形降到 74 万，
   焊接后每条内部边恰好被两个三角形共享。
5. 材质只取 Stages[0]：1.5 版 PBR（baseColor、roughness+metallic 打包、AO、normal、opacity 并入 alpha，
   `*UseUV` 决定 texCoord）与旧版（colorMap、diffuseColor、roughnessFactor / specularPower）。DDS（BC1–BC7、
   BC4/BC5）用 Pillow 解码成 PNG，BC5 法线重建 Z。地形图层的细节图是以 0.5 为中心的调制图，乘到地形基础色上，
   按 detailSize 平铺（`KHR_texture_transform`）。
6. WaterBlock 的顶面（盒子挂在 position 之下）写成 `MINIENGINE_water`。

`--install <检出目录>` 用硬链接装到 `assets/models/beamng_<关卡>/`，sidecar 的 UUID 由关卡名生成（uuid5），
重建后场景引用不变。

## 结果（gridmap_v2）

- 14,184 个对象（12,712 个 TSStatic），500 万绘制三角形、544 个网格、314 万碰撞三角形，52 张贴图；转换约 20 s。
- `bng_heightmap.py` 从写出的 glTF 读回碰撞画俯视图，整图与关卡 minimap 的分区、环道、四叶草、U 形坡道一致；
  悬挂区的测试带（石块、正弦、坑洼、冲击、鹅卵石、越野方块/正弦/原木、三角、方块、落差、粗糙真实路面）
  都在 BeamNG y 68–212 之间。
- 引擎里：出生点 spawn_suspension 处地板高 100.000 m，R34 四轮着地；0.3 油门直线驶过正弦带，车身起伏到
  +0.22 m，出带后四轮回到平地；物理每模拟秒 150–350 ms。

## 未做

- DecalRoad（路面标线、AI 路径，画在别的表面上的贴花）、GroundCover 草、River、灯光、天空参数。
- 材质的细节图层与多层材质；法线贴图的 Y 方向未与 BeamNG 对比确认（按 glTF/OpenGL 约定写入）。
- 实例颜色（instanceColor）、`.cdae` 独有的形状（本图没有）、森林（本图的岩石是 TSStatic）。
