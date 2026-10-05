# 大地图流式加载设计（2026-10-05）

## 背景

GTA SA 全图（5 个区域）一次性载入：31k submesh、19k 贴图、820 万三角形，启动要 45 s，显存 3.6 GB。地图只会越来越大，所以要改成按相机/车辆位置流式加载。

## 目标

- 只把焦点（Play 时是车，否则是相机）附近的格子以高模（HD）载入，其余格子显示游戏自带的 LOD 模型，整张地图始终可见。
- 进出格子不卡帧：模型在后台线程解析，贴图走现有的异步准备队列，GPU 上传只处理增量。
- 开车不依赖流式状态：碰撞和水面始终全图载入（数据量小，Play 启动时构建约 1.3 s）。
- 流式产生的实体只在运行时存在：不存进场景文件、不出现在场景面板、不抢选择。

## 数据（GTASA MAP/tools/engine_bundle.py --stream）

`out/engine/gtasa_stream/`，安装到 `assets/models/gtasa_stream/`：

| 路径 | 内容 |
|---|---|
| `hd/<区域>_<cx>_<cy>.gltf/.bin` | 每 500 m 格子的高模（已按材质合批，同现在的区域模型） |
| `lod/<区域>_<cx>_<cy>.gltf/.bin` | 同一格子的 LOD 模型（游戏 `*_lod` 物体）；每个 LOD 物体归入引用它的高模物体所在的格子，保证同一格子 HD/LOD 互斥时没有缝或重叠 |
| `textures/<txd>/<纹理>.png` | 所有格子共用的贴图（硬链接） |
| `gtasa.stream.yaml` | 清单：每个格子的 hd/lod 路径与世界空间包围盒 |

碰撞另出 `gtasa_collision_<区域>`（只有 `MINIENGINE_collision` 节点、不绘制），作为普通实体放进场景。

## 引擎

### 场景

场景 YAML 增加可选的顶层 `streaming`：

```yaml
streaming:
  - manifest: assets/models/gtasa_stream/gtasa.stream.yaml
    load_radius: 700     # 焦点到格子包围盒的水平距离小于它时载入高模
    unload_radius: 900   # 大于它时退回 LOD（滞回，防止边界来回切换）
```

### WorldStreamingService（engine/editor/services）

每帧：

1. 焦点 = 正在驾驶的车，否则相机。
2. 每个格子按距离决定想要 HD 还是 LOD（带滞回）。
3. 需要但不在模型缓存里的模型交给后台加载线程（一次一个，按距离由近到远；模型加载器不能并发读同一文件）。
4. 模型已在缓存中的格子在主线程换实体：先建新实体（HD 或 LOD），同一帧删掉旧的。CPU renderable 按实体增量更新；渲染端等这次变更的贴图全部准备好后才一次性提交，所以不会看到白模或空洞。
5. 流式实体带 `StreamedComponent`：存档、场景面板、选择都跳过它们；模型缓存按引用自动淘汰不再使用的格子。

`--wait-for-scene` 把"焦点附近的格子还没到位"也算作加载中，截图不会截到一半。

### 渲染端增量上传

`UploadSceneResources` 以前每次变更都重建全部 submesh 缓冲。现在 `RenderSubmesh::buffer` 是共享指针，按 CPU mesh 指针复用已在 GPU 上的缓冲；贴图本来就按 key 复用。描述符集与光追场景仍按变更重建（规模随已载入内容而不是全图）。

## 验收

- 启动时间、显存、三角形数与全图载入对比。
- 相机以固定速度穿越地图（`--camera-velocity`）录像：格子切换没有空洞，记录切换帧的最长帧时间。
- 在流式场景里试驾（`--drive`），车在所有格子上都有碰撞，开进海里照样下沉。
