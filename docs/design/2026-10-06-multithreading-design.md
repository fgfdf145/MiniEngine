# 多线程：渲染线程、enkiTS 任务系统、并行命令录制

## 现状（2026-10-06）

主循环是单线程的：`EditorApplication::Run` 每帧调用 `VulkanRenderer::DrawFrame`，它按顺序做完一帧的全部工作：

1. `TickSharedFrame`：输入、车辆物理（Jolt）、台架、时间、相机；
2. `ProcessPendingOperations`：异步加载的收尾、世界流式加载；
3. 内容上传（`RequestSceneUpload` / `PumpSceneUpload`）；
4. 交换链检查、`AcquireNextImage`（等 GPU 的栅栏）；
5. ImGui 一整帧（`NewFrame` → 各面板 → `ApplyUiActions` → `Render`）；
6. 灯光、阴影级联、环境、DDGI 调度、绘制项、uniform；
7. 录制唯一一个主命令缓冲、提交、呈现。

多线程只用在局部：Jolt 自己的 `JobSystemThreadPool`（核数 − 1 个线程），纹理准备队列（核数 / 2 个常驻线程），BC 压缩按行分段的 `std::async`，光追场景的后台构建，编辑器的各种 `std::async` 加载，录像编码线程，DDGI 参考图的 CPU 路径追踪线程。这些池互不知道对方，线程总数远超核数。

## 目标

分三个阶段，每个阶段单独提交：

1. **渲染线程**：模拟和 UI 留在主线程，所有 Vulkan 工作挪到渲染线程，两者错开一帧并行。
2. **统一任务系统**：引入 enkiTS，一个调度器服务整个引擎；Jolt、纹理准备、压缩、光追构建等改用它，不再各开各的线程池。
3. **并行命令录制**：绘制多的通道（阴影级联、几何、前向）在 enkiTS 任务里并行录制。

不做：多个逻辑帧排队（只错开一帧）；多队列（异步计算）；ImGui 多视口。

## 阶段 1：渲染线程

### 线程分工

| 主线程 | 渲染线程 |
|---|---|
| SDL 事件、输入、窗口 | 内容上传（纹理暂存、提交新内容） |
| 车辆物理、台架、时间、相机 | 视口目标尺寸同步 |
| 异步加载收尾、世界流式加载 | Acquire、自动曝光、白平衡 |
| ImGui 帧（`NewFrame` 到 `Render`） | 灯光选择、阴影规划、环境、DDGI 调度 |
| 灯光收集、实体变换快照 | 运动矢量、光追实例、绘制项 |
| 交换链重建（独占区内） | 录制、提交、呈现 |

SDL 的窗口与事件函数只能在主线程调用，所以交换链重建也留在主线程（见下）。

### 帧包与交接

主线程每帧末尾把渲染需要的一切写进 `RenderFramePacket`，交给渲染线程：

- 相机（副本）、视口矩阵、`RenderDebugSettings`、视口尺寸、帧间隔；
- 场景环境（`SceneEnvironment` 副本）、小地图路径；
- 收集好的场景灯光（`CollectedSceneLights`：实体灯 + 模型灯，已经套上实体变换）；
- 内容：本帧是否有内容变化，以及子网格列表的快照；
- 实体变换快照：子网格列表里每个实体的模型矩阵，加上子网格局部变换（车轮）；
- ImGui 绘制数据的快照。

**流水线深度为一帧**：`RenderThread::Submit` 等渲染线程做完上一帧才交出新的一帧，所以主线程做第 N+1 帧时渲染线程在录第 N 帧，不会更多。两个帧包轮流使用（主线程写 N+1 时渲染线程读 N），容器的容量逐帧复用，不重新分配。输入到画面多出一帧 CPU 延迟，这是这种结构的固有代价。

`--no-render-thread` 让 `Submit` 在调用线程上直接执行，回到单线程，便于对比和排查。

### 子网格列表快照

`RendererWorld` 的子网格改存 `std::shared_ptr<const CpuRenderSubmesh>`：子网格一旦编号就不再修改，快照只复制指针。`SnapshotRenderSubmeshes()` 在列表变化后第一次调用时生成一个不可变的 `std::shared_ptr<const CpuRenderSubmeshList>`，之后返回同一个，直到下一次修改。十万个子网格的快照是 1.6 MB 的指针复制，且只在内容变化的那一帧发生。

### 实体变换快照

`RenderTransformSnapshot`：按实体索引的稠密数组存完整实体 ID 和模型矩阵，外加子网格局部变换的副本。渲染线程的 `GetSubmeshModelMatrix(entity, ordinal)` 与 `RendererWorld` 的同名函数结果相同。

“实体已被删除”原来用 `IsValidEntity` 判断，现在用“快照里有没有这个实体”：子网格列表里没有的实体立即停止绘制。与原来的差别只在一种情况：实体还在但模型被移除了，原来要等到下一次内容提交才消失，现在当帧消失。

### 反馈（渲染线程 → 主线程）

渲染线程每帧末尾在锁内写 `RenderFeedback`，主线程在下一帧开头读：

- 自动曝光的 EV100、长期 EV100，自动白平衡的色温。渲染线程自己保存曝光状态，自动曝光开着时从自己的值步进，关着时采用帧包里相机的手动值。主线程把结果写回相机，供 UI 显示和保存。
- 内容上传进度（`sceneUploadStatus`）、光追场景是否还在构建、显存不足的报告。
- 场景目标的实际尺寸、小地图是否可用、交换链是否过期。

`--wait-for-scene` 依赖“还在加载”的判断。反馈晚一帧，所以主线程另外记住最后一个带内容变化的帧号，渲染线程还没处理到这一帧时也算加载中，不会在内容提交前误计帧。

### ImGui

ImGui 的上下文只属于主线程。`ImGui::Render()` 之后：

1. **纹理请求**（ImGui 1.92 的 `RendererHasTextures`：字体图集随需要的字形增长，后端在绘制时创建、更新、销毁纹理）由主线程在独占区里调用 `ImGui_ImplVulkan_UpdateTexture` 完成，只在有请求的帧（启动、出现新字形、交换链重建后）发生。这些纹理对象属于主线程的上下文，又要用设备和队列，所以两个线程都不能在对方运行时做。
2. 主线程把 `ImDrawData` 深拷贝（绘制列表的缓冲区逐帧复用），把每个绘制命令的纹理引用解析成具体的 `ImTextureID`，拷贝里不带纹理列表，渲染线程绘制时不会再处理纹理请求。

渲染线程用这份拷贝调用未修改的上游后端 `ImGui_ImplVulkan_RenderDrawData`。它会写 `PlatformIO.Renderer_RenderState`，但主线程从不读写这个字段（只有绘制回调用），不构成数据竞争。

由渲染线程创建、生命周期也由它管理的纹理，UI 里用**哨兵 ID**：

- 视口图像：它按交换链图像索引，只有渲染线程 acquire 之后才知道是哪一张；
- 小地图：渲染线程在路径变化时释放并重建它。

渲染线程录制 ImGui 前把哨兵替换成当前的描述符集，所以已经在路上的帧包永远不会引用被释放的纹理。

### 交换链重建与独占区

`RenderThread::RunExclusive(fn)`：等渲染线程做完所有已提交的帧，然后在调用线程上执行 `fn`。渲染线程此时空闲、不持有任何东西，`fn` 可以独占设备和队列。

交换链重建放在主线程的独占区里，在 ImGui 帧开始之前：窗口尺寸变化、HDR 输出切换、或渲染线程报告 `VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` 时进行。ImGui Vulkan 后端随交换链重建、字体纹理的 ID 随之改变，这些都发生在没有帧包在路上、主线程也没在构建 UI 的时刻，因此无需任何同步。窗口边缘拖动时（`SetLiveResizeHandler`）每一步都走这条路，代价与原来的 `vkDeviceWaitIdle` 相同。

同样走独占区的：截图（`CaptureViewport`、`--capture`）、录像开始与停止、`--reference`、`LogFrameTimings`、析构。

### 线程安全的守卫

渲染线程把自己标记为渲染线程（`thread_local`）。`EditorRenderBackendBase::State()` 在 Debug 构建里断言不在渲染线程上调用，渲染路径漏改的共享状态访问会在第一次运行时暴露。

### 错误

渲染线程捕获异常并保存，主线程在下一次 `Submit` / `RunExclusive` 时重新抛出，行为与原来异常从 `DrawFrame` 抛出一样（程序报错退出）。

### 验证

- 单元测试：`RenderThread`（流水线深度、独占区、异常传递、内联模式），`RenderTransformSnapshot`，子网格快照的共享与失效，ImGui 快照的纹理解析与哨兵替换。
- `--state` 截图与 `--no-render-thread` 的截图逐像素比较（固定曝光）。
- `--frames` 的帧时间：主线程与渲染线程各自的 CPU 时间、帧率，对比单线程。
- 手动：编辑器交互、拖动窗口边缘、HDR 切换、驾驶、录像、GTA SA 地图流式加载。

## 阶段 2：统一任务系统（enkiTS）

`engine/core/threading/task_system.h`：全局唯一的 `enki::TaskScheduler`。

- 线程数：工作线程 = 逻辑核数 − 2（主线程是 0 号线程，渲染线程注册为外部线程）。
- 工作线程命名（`profilerCallbacks.threadStart`），在调试器和 PIX 里可辨认。
- `ParallelFor(count, minRange, fn)` 等小工具；三档优先级：高（帧内的并行，等待者在关键路径上），中（纹理准备、BVH 构建），低（后台加载）。

迁移清单：

| 现在 | 改为 |
|---|---|
| Jolt `JobSystemThreadPool`（核数 − 1 个线程） | 基于 `JPH::JobSystemWithBarrier` 的适配器，作业放进 enkiTS（高优先级） |
| `TexturePreparationQueue` 的常驻线程（核数 / 2） | enkiTS 任务（中优先级），保留现有的排队、取消、限流语义 |
| BC 压缩的按行 `std::async` | `ParallelFor` |
| 光追场景构建的 `std::async` + 内部线程 | enkiTS 任务 + `ParallelFor` |
| DDGI 参考图、kn5 写文件的线程 | `ParallelFor` |
| HDRI 解码 `std::async` | enkiTS 任务（中优先级） |

保留独立线程的：渲染线程、录像编码（阻塞写文件）、编辑器的场景/模型加载（长时间阻塞 IO，放进任务池会占住工作线程，改动收益也小）。

## 阶段 3：并行命令录制

录制顺序不变，拆成三步：

1. **规划**（渲染线程，串行）：按通道顺序跑一遍布局追踪，得到每个通道录制前的屏障列表；为每个通道分配命令缓冲。
2. **录制**（enkiTS，并行）：每个通道一个任务录进自己的主命令缓冲；阴影的每个级联、几何通道、前向通道再按绘制项切成若干段，录进二级命令缓冲（`VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS`），由通道的主命令缓冲执行。
3. **提交**（渲染线程）：按顺序一次提交全部主命令缓冲。

命令池按“帧槽 × 线程”分配，帧槽的栅栏通过后整池重置。GPU 计时的查询按命令缓冲写，顺序不变。

只有绘制项多到值得的时候才切分（阈值按测量定），小场景仍然一个任务录完，避免任务开销大于收益。

验证：GTA SA 地图和 R34 场景的录制 CPU 时间，切分前后对比；截图逐像素一致。
