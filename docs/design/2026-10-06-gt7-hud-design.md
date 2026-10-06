# GT7 驾驶 HUD（SVG 矢量）设计

日期：2026-10-06

## 目标

驾驶时在 Viewport 底部显示与 Gran Turismo 7 一致的仪表条（以用户录制的 GT7 画面为准）：

- 左：路面积水括号、四轮胎 + 车身俯视图 + 胎种缩写（SS/SM…）、油量半圆表 + 里程表、ABS / 手刹 / 自动刹车 / 自动转向 指示灯、刹车条
- 中：转速梳齿条（上半转速区间点亮红色，断油点闪烁）、方向盘红点、车速（公里/小时）、档位、建议档位框、自动挡/手动挡、盲区圆环、左右箭头
- 右：油门条、车灯 / 反打辅助 / TCS / 稳定控制 指示灯、增压表（-1..2 ×100 kPa，仅涡轮车）

所有文字、数字、图标都是 SVG，按几何体绘制，任何缩放都锐利；条、弧、刻度用 ImGui 抗锯齿图元。

## 结构

| 部分 | 位置 |
| --- | --- |
| SVG 字体（SVG 1.1 `<font>`/`<glyph>`） | `engine/asset/svg_font.*`，字形用 `SvgIcon::FromPaths` 填充 |
| SVG 解析小工具 | `engine/asset/svg_document.h`（`Attribute`/`Tags`/`ParseNumber`） |
| HUD 绘制 | `engine/editor/ui/editor_gt7_hud.*`，`DrawGt7Hud(drawList, origin, size, Gt7HudInput)` |
| SVG 资源 | `engine/editor/ui/gt7_hud/{fonts,icons}`，CMake 嵌入可执行文件 |
| 生成器 | `tools/gt7_hud/make_gt7_hud_svgs.py`（shapely 描边→填充；fontTools 取 Noto Sans SC 子集） |

- 仪表数字（`gt7_meter.svg`）：宽扁细线、外角圆角，按笔画中心线设计后 shapely 合并为填充轮廓。
- 文字（`gt7_sans*.svg`）：Noto Sans SC（OFL 1.1，`fonts/OFL.txt`），ASCII + “公里/小时 自动挡 手动挡”。
- 坐标直接使用录制画面的像素（2000×356 裁剪，所在帧约 2270×1277，中心 x=1135、底边 y=356），按 viewport 高度（窄于 16:9 时按宽度）缩放。
- `SvgIcon` 网格缓存按尺寸最多 24 个，避免拖动 viewport 时无限增长。

## 数据

`VehicleDriveStatus` 新增：本帧控制量、手动挡、断油转速、ABS/TCS 是否装备、反打辅助、是否涡轮、前后胎种缩写（AC tyres.ini SHORT_NAME）、里程（物理步进时间积分）。
`VehicleTelemetry` 新增 `absActive`（有轮子被 ABS 放松）、`tractionControlCut`（TC 断油）。

没有数据的项保持“未点亮”：积水、胎温/磨损、车灯、稳定控制、自动驾驶辅助、盲区、建议档位。

## 开关

View > Driving HUD（Alt+H），默认开。开启时小地图移到右上角（GT7 赛道图的位置），全屏时不再显示旧的“km/h 档位”文字。

## 验证

`miniengine_gt7_hud_tests`：SVG 字体解析/实体/UTF-8/测量；HUD 字体覆盖所有用到的字符；按录制比例渲染并裁剪成与录制相同的 2000×356（`MINIENGINE_UI_SNAPSHOT_DIR` 下 `gt7_hud_reference.png`），以及三种状态图。
