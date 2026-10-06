# 编辑器 UI：ImGui 1.92.9 + Claude desktop 外观

日期：2026-10-06。状态：已实现，Release 构建与全部测试通过，实机截图核对过像素值。

---

## 0. 结论

| 项 | 结果 |
|---|---|
| ImGui | 1.92.6 → 1.92.9（上游 v1.92.9b-docking）；ImPlot 0.17 → 1.0；ImGuizmo 2024-05-29 → 1.10。`vcpkg.json` 用 `overrides` 只升这三个，其余依赖仍按原 baseline |
| 后端 | `engine/renderer/imgui` 换成上游 1.92.9b-docking 的原版文件，不再改动。HDR10 的自定义片元着色器改用上游的 `CustomShaderFragCreateInfo`。字体由后端动态上传（`RendererHasTextures`），缩放后的文字和图标不再糊 |
| 灰蒙蒙的根因 | ① 交换链是 `B8G8R8A8_SRGB`，ImGui 的 sRGB 颜色被当成线性光又编码一次：`#151515` 显示成 `#515151`；半透明填充也在线性空间混合，比网页亮得多。② `miniengine.settings.json` 里存着 7 月的整套灰色主题，每次启动都覆盖代码里的配色 |
| 颜色空间 | SDR 交换链改用 `B8G8R8A8_UNORM`（色彩空间仍是 sRGB），ImGui 在 sRGB 空间画和混合，与浏览器一致。场景 LDR 目标显式用 sRGB 格式（色调映射写线性光），ImGui 通过它的 UNORM 视图读字节。小地图按 UNORM 读。HDR10 着色器先把顶点色从 sRGB 解码到线性再 PQ 编码 |
| 主题存档 | `ui.theme` 加 `version: 2`，只存在 Theme 窗口里改过的颜色；没有 version 的旧整套存档不再读取 |
| 配色/圆角/间距/字号/图标 | 全部取自 Claude desktop 安装包里的设计令牌（`--cds-*`，默认密度），并用应用自己的截图核对（见下） |

---

## 1. 令牌来源与核对

令牌从 `Claude.exe` 的 `resources/app.asar` 里的 CSS 解析：基础块 `.cds-root`（默认密度），暗色块 `[data-mode=dark] .cds-root`。界面图标在 claude.ai 前端缓存里，组件名如 `PencilSimpleIcon`、`MagnifyingGlassIcon`，路径是 256×256 viewBox——即 Phosphor Icons（MIT）regular 字重。

截图核对（显示缩放 200%，应用缩放约 108%）：

| 位置 | Claude 像素 | 令牌 | 编辑器像素（改后） |
|---|---|---|---|
| 内容区 | 21 | surface-1 `#151515` | 21 |
| 侧栏 | 17 | neutral-30 `#111` | 菜单栏/标签栏/空停靠区 17 |
| 输入框 | 29（侧栏上） | fill-field 白 5% | 33（内容区上，21+5%） |
| 选中行 | 52（侧栏上） | fill-ghost-selected 白 15% | Header 白 15% |
| 分隔线 | 41（侧栏上） | 白 10% | 45（内容区上） |
| 行/输入框高 | 52 设备像素 | h-control 24px | FramePadding 使控件高 24 |
| 圆角 | 约 12 设备像素 | radius 6px | FrameRounding 6 |

## 2. 映射

- 颜色：文字 text-primary / text-muted；按钮 fill-secondary 10%/14%/20%；输入框 fill-field 5%、悬停 7.5%、激活 10%；弹出层 surface-3；强调色是蓝色 fill-accent `rgb(42,120,214)`（勾选、滑块、拖放、焦点），陶土色 fill-brand 只是品牌色，只留给图表悬停；标签页不画彩色上划线。状态色（错误、警告、删除按钮）在 `engine/editor/ui_colors.h`。资源类型颜色用 `--cds-text-tint-*`。
- 圆角：控件 6（--cds-radius），窗口/弹出/子面板 10（--cds-radius-card = radius + 4），滑块把手 4（与外框同心），滚动条全圆。
- 间距：正文 13px（--cds-font-size-body）；控件高 24（--cds-h-control）；FramePadding.x 8（pad-md）；ItemSpacing 8×6（gap-sm × gap-xs）；WindowPadding 12×8（pad-lg × panel-inset）；缩进 18（h-control-nested）；分隔线 1px。
- 字体：Claude 的 `--cds-font-sans` 是 Anthropic Sans，Windows 上回退到 system-ui，即 Segoe UI Variable。Anthropic Sans 是专有字体，不从安装包里取，沿用 Segoe UI Variable。
- 图标：Phosphor regular。行内图标用 `third_party/phosphor/Phosphor.ttf` + `IconsPhosphor.h`（`tools/editor_icons/phosphor_header.py` 生成），比正文大 14/13；资源窗口的大图标是 Phosphor 的原版 SVG（`tools/editor_icons/phosphor_to_svg.py`）。Font Awesome 已移除。

## 3. 没做到的

- ImGui 自带的折叠三角和标签页关闭叉是内部绘制的，没有换成 Phosphor 的 caret/x。
- ImGui 只有一个 Border 颜色：Claude 输入框描边 15%、分隔线 10%，这里统一 10%。
- 悬停在已选中的行上，ImGui 用 HeaderHovered（7.5%），比选中态（15%）浅；Claude 保持 15%。

## 4. 升级时注意

vcpkg 安装的头文件保留上游的修改时间（如 2026-07-31），比已有的 .obj 旧，MSBuild 不会重编译，结果是链接错误（`ImGui::OpenPopup` 返回值从 void 变 bool）。升级后要 `touch` 这些头文件或清理重建。更新 vcpkg 子模块后还要重新 bootstrap vcpkg 工具（旧工具读不了新的 `vcpkg-tools.json`）。
