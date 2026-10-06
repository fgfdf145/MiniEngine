# GTA V 车辆导入（gen9 .yft → glTF）

日期：2026-10-06

## 目标

把 `C:\Project\GTA5 Cars` 里从 GTA V Enhanced 解出来的车（`.yft` + `.ytd` + `.meta`）导入引擎，
和 kn5 导入一样一次性转成 glTF 包（`<name>.gltf`、`buffers/<name>.bin`、`textures/*.png`），
导入后就是普通 glTF：能看、能挂到场景、四个轮子和方向盘被引擎认出来，可以直接开。

不做（这一版）：handling.meta → `MINIENGINE_vehicle`（开车用引擎按轮子位置拟合的默认参数）、
旧版 PC（gen8）格式、车损/脏污/灯光发光、改装件（vehiclemods）、碰撞体。

## 数据来源

- 解包目录：`base/`（本体 + update）、`dlc/<pack>/`、`manifest.tsv`。资源是 RSC7 容器：16 字节头
  （magic、版本、system/graphics 页标志），后面一整段 raw deflate。版本号说明是 gen9：yft 171、ytd 5
  （旧版 PC 是 162 / 13）。gen9 的 yft 没有 graphics 页，顶点/索引都在 system 页。
- 结构布局参照 CodeWalker 的 gen9 实现（dexyfex/CodeWalker，`CodeWalker.Core/GameFiles/Resources`）。
  它的源码是“仅供学习”，没有开源许可，所以只当格式文档看，代码是自己写的。
- 引擎里没有 zlib 依赖，用 stb_image 自带的 `stbi_zlib_decode_noheader_buffer` 解 raw deflate。
- 调色板：carcols.ymt 是二进制，解包里没有。161 色的颜色值和类型（金属/哑光/拉丝/镀铬……）取自
  DurtyFree/gta-v-data-dumps 的 `vehicleColors.json`，编进 `gta5_game_data.cpp`。

## gen9 和旧格式的差别（读的时候踩到的）

| 部分 | gen9 |
|---|---|
| 纹理 | `rage::sga::Texture`：0x18 宽高深、0x1F 格式（按 DXGI 编号）、0x22 mip 数、0x28 名字、0x38 数据。数据是线性的（没有 swizzle），只取 base level 解成 PNG。 |
| 着色器 | 参数不再是数组，而是一张 `(名字哈希, 位域)` 表：类型 2 位（0 纹理 / 3 常量）、纹理槽或 cbuffer 号、偏移 12 位、长度 12 位。参数名换了（`DiffuseTex`、`SpecularTex`、`DiffuseColor`……），用 JenkinsHash 对名字匹配。 |
| 顶点格式 | 52 个语义槽，每槽 offset / stride / 格式（DXGI 编号）。车只用到 float3 位置法线、float4 切线、unorm8 颜色和骨骼权重、uint8 骨骼索引、float2 UV。 |
| 索引 | 0x0C 是索引宽度（2 或 4）。 |

骨架、模型、几何体、fragment 和 physics LOD 的布局和旧版一样。

## 转换规则

### 坐标与节点

GTA 是 +X 右、+Y 前、+Z 上；glTF 是 +X 右、-Z 前、+Y 上。根节点矩阵把 (x, y, z) 变成 (x, z, -y)，行列式 +1，
绕序不变（实测三角形是 CCW，和法线一致）。

GTA 车的原点在底盘中心（大约轮轴高度），直接放进场景会陷进地里一半；根节点再往上抬到最低的轮子顶点，
导入后的车原点就在轮胎接地处，和 kn5 车一样站在地面上。

骨架一根骨头一个节点（相对父节点的 TRS）。车身网格是 skinned 的，但每个顶点只绑一根骨头（全部 1427 辆 `_hi`
统计过，没有多权重顶点），所以按骨头拆开，顶点变换到骨头自己的坐标系，挂在骨头节点下：车门、引擎盖、车轮都是能动的节点。

### 轮子

GTA 的骨头名本来就是 `wheel_lf/rf/lr/rr`，引擎按名字认轮子。轮子的网格不在车身里，而在 fragment 的子件
（physics child）的 drawable 里，坐标是轮子本地的。大多数车只带 `wheel_lf`（203 辆另带 `wheel_lr`），
所以：每个 `wheel_*` 骨头用自己的，没有就用后轮的（非前轮时）、再没有用左前的；左右不同侧时绕 Z 转半圈
（不是镜像，绕序不用翻）。网格挂在 `wheel_xx_mesh` 子节点上。

方向盘：在 `steeringwheel` 骨头下加一个 `STEER_HR` 节点（绕 X 转 -90°，让它的 +Z 对着骨头的 +Y，也就是方向盘轴），
方向盘的网格挂在它下面。

### 材质

| GTA | glTF |
|---|---|
| `DiffuseTex` | baseColorTexture（整张图只有一种颜色时折成 baseColorFactor，比如 4×4 的白色 `smallspecmap`） |
| `BumpTex` | normalTexture；GTA 法线图 B 恒为 255，按 X/Y 重建 Z；`Bumpiness` → scale |
| `SpecularTex` | G 通道（光泽度）→ roughness = 1 − 0.85·g（下限 0.08），写成 metallicRoughness 图 |
| `Specular`（Blinn 指数） | 没有高光图时用 kn5 同一个换算 |
| `DiffuseColor` | x = 2 时 y 是颜色槽：1 主色、2 副色、3 珠光、4 轮毂、6 内饰、7 仪表；5 是默认（不染色）。其它值是普通 tint |
| render bucket | 1、2（透明 / 贴花）→ BLEND，3 → MASK |
| `vehicle_vehglass*` | BLEND，roughness 0.05 |
| `vehicle_licenseplate` | `PlateBgTex` / `PlateBgBumpTex`（没有字） |
| `vehicle_paint3+` 的 `DiffuseTex2` | 涂装，按 alpha 盖在车漆颜色上烘成一张图（UV1），每个颜色组合一张 |

车漆按调色板的类型给材质：金属 metallic 0.4 / roughness 0.35 + 清漆；普通 roughness 0.4 + 清漆；
哑光 0.65 无清漆；旧漆 0.6 + 薄清漆；拉丝 / 镀铬 / 金是全金属（调色板存的是漫反射色，金属的反射率提亮到合理值）。
内饰、仪表的颜色是 0（默认）时不染色，否则整个内饰被染成黑色。

颜色组合：DLC 车的 `carvariations.meta` 里有 `colors`，每组变成一个 `KHR_materials_variants` 变体；
本体车的 carvariations 是二进制 ymt，解包里没有，给 6 组通用颜色（白、黑、银、红、蓝、黄）。

### 纹理从哪找

依次：fragment 自带的（`_hi.yft`、再普通 `.yft`），然后 `<txd>+hi.ytd`、`<txd>.ytd`，再按 vehicles.meta 的
`txdRelationships` 往上找父字典（比如 adder → vehicles_supergt_interior → vehshare），最后 vehshare。
同名文件优先同一个 DLC 包，其次补丁 > update > DLC > 本体。字典只在还有纹理没找到时才读。

meta 的优先级：本体 common < DLC < DLC 补丁包（`dlc/patch*`）< update < update 的 dlc_patch；后读的覆盖先读的。

### LOD

只要最高细节：`<name>_hi.yft` 存在就用它（只有一层 High LOD），否则用 `<name>.yft` 的 High。

## 验证

`tests/gta5_import_tests.cpp`：哈希、页大小、越界读、颜色槽、调色板、XML、meta 优先级；设了
`MINIENGINE_GTA5_CARS=C:/Project/GTA5 Cars` 时导入 adder（本体）和 coquette5（DLC），再用 ModelLoader 读回，
要求找到四个轮子、方向盘、颜色变体，轮子左右前后对、半径合理，轮胎接地（minBounds.y ≈ 0）。
另外在引擎里渲染看过：adder（红色变体）和 coquette5（Race Yellow 变体）外观、内饰、轮子位置都对。

结果（Debug）：

| 车 | 三角形 | 网格 | 材质 | 图片 | 颜色组 | 缺失纹理 |
|---|---|---|---|---|---|---|
| adder | 82,949 | 37 | 38 | 24 | 6 | 0 |
| coquette5 | 124,164 | 51 | 94 | 37 | 11 | 0 |

第一次读解包目录要建索引（6.4 万个文件、50 多个 meta），Debug 下十几秒，之后同一次运行内复用。

## 没做 / 以后

- handling.meta → `MINIENGINE_vehicle`（质量、驱动分配、档位、刹车、转向角、悬挂）。GTA 的 handling 是街机式参数，
  需要单独设计换算。
- 法线图绿通道方向没有确认（`kFlipNormalGreen`），要看渲染结果定。
- 镀铬件（保险杠、轮毂盖）现在是暗色塑料的样子：GTA 靠环境反射做镀铬，漫反射很暗，按 PBR 非金属就发黑。
- 脏污、损伤、灯光发光、仪表、车牌文字、`DetailTex`（皮革细节法线）、extras 的取舍（现在全保留）。
- 旧版 PC（gen8）的 yft/ytd。
