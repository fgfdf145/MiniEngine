# MiniEngine 工作区磁盘布局治理设计

## 目标

保留当前由 CMake preset 使用的 x64/x86 vcpkg 安装树和 Visual Studio 2026 构建输出，删除已经不再被当前配置引用的旧构建副本，并在依赖引导与 CMake 配置阶段阻止同类重复目录再次生成。

## 保留范围

- `.deps/vcpkg_installed/x64/`：当前 x64 preset 的唯一仓库内安装根。
- `.deps/vcpkg_installed/x86/`：当前 x86 preset 的唯一仓库内安装根。
- `out/build/vs2026-x64/`：当前 Visual Studio 2026 x64 构建输出，但不保留其中旧的嵌套 `vcpkg_installed/`。
- `out/build/vs2026-x86/`：当前 Visual Studio 2026 x86 构建输出。
- `.deps/vcpkg/`：vcpkg checkout、下载缓存、packages 和 buildtrees；本轮不做激进缓存清理。
- `.vs/`、`.cache/`、`assets/`、`.git/`：不属于本轮旧构建副本清理范围。
- 用户已有的 `imgui.ini` 修改和 `docs/superpowers/plans/` 未跟踪内容。

## 删除范围

删除前必须逐项解析绝对路径，确认目标仍位于仓库根目录内，并再次确认保留目录存在。

- `cmake-build-debug/`：旧 CLion 构建树及其独立 vcpkg 安装树。
- `vcpkg_installed/`：旧引导脚本在仓库根目录生成的安装树。
- `out/build/x64-debug/`：旧 Ninja x64 构建树及其嵌套安装树。
- `out/build/vs2026-x64/vcpkg_installed/`：旧 Visual Studio 集成 vcpkg 生成的嵌套安装树；当前顶层 CMake cache 已指向 `.deps/vcpkg_installed/x64`。
- `.deps/vcpkg_installed/x64-windows/`、`.deps/vcpkg_installed/x86-windows/` 和同层旧 `vcpkg/` 元数据：旧的非架构分桶安装根；当前 preset 分别使用 `x64/` 和 `x86/`。

预计删除的逻辑文件大小约为 21.9 GiB。由于 vcpkg 部分文件使用 NTFS 硬链接，实际释放空间以删除后的卷可用空间和目录复测为准。

## 防止再次膨胀

### 依赖引导脚本

PowerShell 与 Bash 引导脚本必须根据 triplet 的架构前缀计算架构分桶，并向 `vcpkg install` 显式传递安装根：

- `x64-*` 使用 `.deps/vcpkg_installed/x64`。
- `x86-*` 使用 `.deps/vcpkg_installed/x86`。
- `arm64-*` 使用 `.deps/vcpkg_installed/arm64`，保持跨平台脚本可扩展性。
- 无法识别架构前缀时立即失败，不允许回退到仓库根 `vcpkg_installed/`。

引导脚本必须打印最终安装根，便于日志审计。

### CMake 配置约束

`MiniEngineVcpkg.cmake` 继续在 `project()` 前选择 toolchain，并提供架构分桶的默认安装根。`project()` 完成后执行布局校验：

- 允许 `.deps/vcpkg_installed/<architecture>/`。
- 允许显式指定的仓库外共享安装根。
- 拒绝仓库根 `vcpkg_installed/`。
- 拒绝 `out/`、`cmake-build-*`、`build/`、`bin/` 或 `obj/` 下的安装根。
- 错误信息必须同时显示错误路径和允许的仓库内路径。

该校验不自动删除目录；它在写入新依赖前停止配置，避免破坏用户数据。

## 自动化验证

增加一个无需编译引擎的 CMake contract test，检查：

- x64/x86 preset 仍分别指向 `.deps/vcpkg_installed/x64` 和 `x86`。
- PowerShell 与 Bash 引导脚本均显式传递安装根。
- CMake 布局校验拒绝构建目录内和仓库根安装树。
- 合法的 x64/x86 分桶以及仓库外自定义安装根通过校验。

测试先以缺少约束的当前实现运行并确认失败，再实施最小修改使其通过。完成后运行 contract test、CMake 配置、现有 CTest，以及仓库磁盘布局审计。

## 文档与维护约束

README 和 `scripts/README.md` 记录唯一允许的仓库内依赖布局、禁止目录、清理方式和缓存边界。`.gitignore` 继续忽略生成目录，但明确说明“忽略”不是允许任意生成依赖副本；实际约束由引导脚本和 CMake 校验执行。

## 验收标准

- 当前 `.deps/vcpkg_installed/x64` 和 `x86` 内容未被删除或重装。
- 当前 `out/build/vs2026-x64`、`vs2026-x86` 保留，旧嵌套依赖被删除。
- 所有列出的旧副本不存在。
- 仓库逻辑大小相较清理前 43.230 GiB 明显下降，实际结果记录在交付说明中。
- 新引导脚本不会在仓库根或构建树内创建 `vcpkg_installed`。
- 非法 CMake 安装根在配置阶段得到明确错误。
- 自动化 contract test、现有构建/CTest 验证均通过；若环境阻止完整构建，必须报告实际阻塞而不能声称通过。
- `imgui.ini` 和既有 `docs/superpowers/plans/` 内容保持不变。
