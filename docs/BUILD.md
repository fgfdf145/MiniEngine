# 构建细节

README“构建与运行”一节的补充：vcpkg 磁盘布局、各平台 preset 与 overlay port。

## vcpkg 磁盘布局

仓库内依赖只允许安装到 `.deps/vcpkg_installed/<architecture>/`；当前 Windows preset 分别使用 `x64/` 和 `x86/`。不要把 `VCPKG_INSTALLED_DIR` 指向 `out/`、`cmake-build-*`、仓库根 `vcpkg_installed/` 或其他仓库内目录，也不要使用相对路径。确需共享依赖时，使用同时位于源码树和当前 CMake 二进制树之外的绝对路径。该门禁只在实际 vcpkg toolchain 生效，并会在 `project()` 安装依赖前拒绝不符合约束的路径；无关的自定义 toolchain 不需要 vcpkg 变量。

## 平台 preset

Windows 的根 [MiniEngine.slnx](../MiniEngine.slnx) 对应 Debug/Release × x64/Win32，并委托 `vs2026-x64` 或 `vs2026-x86` preset。不要在 `.vcxproj` 中复制 CMake 的编译选项或依赖逻辑。Ninja 的 `x64-debug`/`x64-release`、`x86-debug`/`x86-release` 仍可用于直接 CMake 工作流；x64 与 x86 的 vcpkg 安装根目录隔离。

macOS ARM64 沿用 `macos-debug`/`macos-release`（`arm64-osx` → `arm64/`），Intel macOS 使用 `macos-x64-debug`/`macos-x64-release`（`x64-osx` → `x64/`）。Linux x64 沿用 `linux-debug`（`x64-linux` → `x64/`），Linux ARM64 使用 `linux-arm64-debug`（`arm64-linux` → `arm64/`）。省略 Bash 构建脚本的 preset 时，会按这四种主机组合选择对应 Debug 入口。上述名称是当前配置入口，不代表本轮在对应平台完成了构建或 GUI 验收。

## vcpkg overlay port

`cmake/vcpkg-overlay-ports/` 通过所有 preset 的 `VCPKG_OVERLAY_PORTS` 生效，当前有 `tinygltf` 与 `bc7enc-rdo` 两个 port。上游 port 用 `vcpkg_from_github` 拉取 GitHub 自动生成的源码归档并按 SHA512 固定；该归档被重新压缩后哈希不再匹配，port 直接下载失败。覆盖版改用 `vcpkg_from_git` 拉取 `v3.0.0` 对应的提交 `cfcadfa8d14eb489d97b6324838ae100410edcc7`，git 对象按内容寻址，不会像重新压缩的 tarball 那样漂移；除拉取方式外与上游 portfile 逐行一致。

overlay 会一直遮蔽上游同名 port：版本号仍是 `3.0.0`，所以刷新 baseline 后既不会切回上游，也拿不到 3.0.x 的后续修复。上游 port 记录的哈希与服务端一致后，删除 `cmake/vcpkg-overlay-ports/tinygltf/` 即可；overlay 目录清空后一并移除各 preset 的 `VCPKG_OVERLAY_PORTS`。

`bc7enc-rdo` 不是遮蔽：vcpkg 没有这个库的 port。它用 `vcpkg_from_git` 按提交 `b9438627eef73a1157e84201b6fa6eb2ffd6d9f0` 拉取 richgel999/bc7enc_rdo，只把 `bc7enc.cpp`、`rgbcx.cpp`、`bc7decomp.cpp` 编成静态库 `unofficial::bc7enc-rdo::bc7enc-rdo`（CMakeLists 随 port 提供），不构建上游的示例程序、ISPC 编码器与 RDO 编码器。
