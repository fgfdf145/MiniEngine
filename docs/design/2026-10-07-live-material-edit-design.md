# 实时改材质颜色（Edit Materials）

日期：2026-10-07

## 目标

在编辑器里选中一辆车（或任何模型），直接改车漆颜色，并在视口里实时看到效果；满意后保存，不满意关掉就恢复原样。

## 入口

- Inspector → ModelComponent → **Edit Materials**：打开已有的 Model Preview 窗口（材质图编辑器），默认选中最像车漆的材质槽（名字含 carpaint，其次是不在轮圈/卡钳/内饰上的 paint 或 body）。
- 以前这个窗口在 UI 里没有任何入口（只有窗口自己的 Reload 会打开它）。

## Quick Edit

Model Preview 的材质图上方新增 Quick Edit：

| 控件 | 写到哪里 |
| --- | --- |
| Base Color + Brightness | Output 节点的 baseColorFactor；若 base_factor 连了 Color 节点，则写那个节点 |
| Metallic / Roughness | Output 节点的因子；若连了 Scalar 节点，则写那个节点 |
| Clearcoat / Clearcoat Roughness | Output 节点的因子 |

写在编译（CompileMaterialShaderGraph）读取的位置，保证编译后不会被覆盖。

颜色在 gamma 空间编辑（取色器显示的就是屏幕上的颜色），线性因子 = (颜色 × 亮度)^2.2。亮度可以超过 1：kn5 车漆的 diffuse gain（AC 的 ksDiffuse/ksAmbient）会让因子超过 1，GBuffer 的 R8G8B8A8_SRGB 在写入时截断，和导入时烘焙贴图最后截断的结果一致。

## 原版导入材质（As Imported / Edited）

Quick Edit 上方有 **Material: As Imported / Edited** 切换：

- As Imported：该槽换成导入时的材质，即 glTF 本身，不应用旁边保存的 `.material.yaml`（`ModelLoader::LoadModelAsImported`）。对车来说就是所选涂装的原厂车漆。
- Edited：切回刚才的编辑。切到 As Imported 时编辑被暂存，可以来回对比。
- 在 As Imported 上做任何修改，该槽就变成以导入材质为起点的新编辑。
- 打开窗口时，有自己 sidecar 的槽显示 Edited，其余显示 As Imported。
- Save 时，处于 As Imported 的槽删除自己的 sidecar（按序号，或按名字，与加载规则一致），不写新文件；切换同样实时预览，关窗口不保存就恢复磁盘上的状态。

`ModelLoader::LoadModel` 拆成 `LoadModelAsImported` + `ApplyMaterialDefinitions`，窗口只读一次模型就同时拿到两份材质。

## 实时预览

- 每次修改发出 `previewImportedModelMaterial`：`ModelCache::UpdateMaterial` 只改这一个材质槽，标脏用到该模型的实体，刷新 renderables。不写盘。
- 关闭窗口、Reload From Disk、关掉 Live Preview 时，如果场景里有未保存的预览，发出 `revertImportedModelMaterials`：清掉缓存，从磁盘重新读取（包括 sidecar）。
- Save 只写改过的槽（`<stem>_<index>.material.yaml`），以前会把所有槽都写一遍。
- `MarkModelRenderablesDirtyForSourcePath` 改用 weakly_canonical 比较路径：场景可能存相对路径，窗口用的是绝对路径。

开销（R34，Release，每帧都改一次颜色）：主线程约 2 ms，渲染线程上传 1 ms、descriptor + ray scene 2–3 ms，不重建 BLAS（GPU buffer 按 mesh 指针复用）。

## R34 资产

R34 的车漆被导入器烘焙成了 `Skin_00_paint.png` = 灰色模板 `Skin_00.png` × 常数颜色 (0, 0.25, 0.70)（gamma 空间，逐像素比值的 10%–90% 分位差 < 0.01）。因子只能在这张蓝色贴图上相乘，改不成别的颜色。

曾把 `EXT_Carpaint` 的 base map 换回 `Skin_00.png`、因子设为 (0, 0.047, 0.462)，渲染与原来一致，但用户觉得直接改色效果不好，要用原本导入的车漆，已恢复原始 glTF（资产不在 git 里）。所以 R34 目前的 Quick Edit 只能在烘焙的蓝色上相乘。

## 未做

- kn5 导入器仍然会在 alpha 遮罩或亮度超过 1 时烘焙车漆。新导入的车如果车漆是烘焙的，Quick Edit 只能在烘焙色上相乘。可以改成：车漆是模板的纯色乘积时，写模板 + 因子（允许大于 1）。
