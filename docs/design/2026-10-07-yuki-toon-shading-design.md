# Yuki 完整卡通着色（进行中，2026-10-07 暂停）

目标：C:\Project\Yuki\Yuki.glb 在 viewport 中按 AnimateApp（Unity 6 URP，shader
"Universal Render Pipeline/Anime/Character"，NiloToon 克隆）的方式完整着色。

状态：**已实现**（2026-10-07）。第 1、2 节是研究结论，第 3 节是最初的方案，第 4 节是实际实现与验证结果（与方案不同处以第 4 节为准）。

## 1. 研究产物（都在 C:\Project\Yuki\research）

- `shader_params.txt`：各 pass 公共参数（cbuffer 成员名 → 寄存器）。
- `shader_variant_params.txt`：每个变体额外参数（Unity 6 参数 blob：版本 202012090，
  名字内联；目录项 12 字节 `offset,len,?`）。
- `unity_per_material_layout.txt`：UnityPerMaterial 完整布局（736 B）。
- `annotated/*.asm`：81 个变体的 DXBC，cb/纹理已替换成属性名（脚本在会话 scratchpad，
  `annot.py` 逻辑：common + variant 参数合并，按 cb 顺序映射）。
- 材质关键字与队列（从 bundle 读 m_ValidKeywords）：

| 材质 | 队列 | 关键字 | Forward 变体 |
|---|---|---|---|
| ClothingUpper | 2000 | OCCLUSION SHADING_GRADEMAP SHADOW_COLOR | 18 |
| ClothingLower | 2000 | ADDITIVE_MATCAP ALPHA_BLEND_MATCAP OCCLUSION SHADOW_COLOR | 17 |
| Hair | 2003 | ADDITIVE_MATCAP OCCLUSION OUTLINE_WIDTH_TEXTURE SHADING_GRADEMAP SHADOW_COLOR | 19 |
| Body | 2000 | ALPHA_BLEND_MATCAP SKIN SKIN_MASK SHADOW_COLOR | 16 |
| Head | 2000 | FACE FACE_MASK FACE_SHADOW_GRADIENTMAP ADDITIONAL_LIGHT_FACE_SDF SKIN SHADOW_COLOR | 15 |
| Eyes | 2000 | FACE SKIN SHADOW_COLOR（无描边，写 stencil 77） | 14 |
| EyesShadow | 2450 | ALPHATEST ALPHA_OVERRIDE FACE（无描边，透明混合） | 24 |
| Hair Redraw_on_Brows | 2452 | 同 Hair + ALPHATEST ALPHA_OVERRIDE；stencil == 77，混合 | 25 |
| Hair_Front | 2454 | 同 Hair + ALPHATEST；stencil != 77，混合 | 20 |

所有变体都带 DEPTHTEX_RIMLIGHT_SHADOW、FIX_DOTTED_LINE、ANIME_RECEIVE_SELF_SHADOW（全局）。
`_ControlledByCharacterRenderController` 运行时为 1。Materials.json 的 Color 属性是 gamma 值，
上传 GPU 前要转线性（Vector 如 `_MainLightRamp` 不转）。

## 2. 着色数学（统一 shader，按特性位分支）

- `faceMask = FACE ? (FACE_MASK ? FaceMaskMap.g : 1) : 0`；`skinMask = SKIN ? (SKIN_MASK ? SkinMaskMap.g : 1) : 0`。
- albedo = BaseMap * _BaseColor（* 全局/角色 tint=1）。
- AlphaBlend matcap：`albedo = lerp(albedo, matcap*tint, mask.g(可反相)*strength)`。
- Additive matcap：`pow(matcap, 1+extract)*tint*lerp(1,albedo,mix)*remap(mask.g)*intensity`，加到 albedo；
  alpha = saturate(a + lum(matcap))。matcap UV = 视空间法线：u=0.5+0.5 N·camRight，v=0.5+0.5 N·camUp（Unity UV）。
- Occlusion：`ao = 1 + strength*(remap(occ.g) - 1)`。
- Shading grade：`g = (remap(grade.r 可反相) - 0.5)*strength*applyRange + midOffset`；
  `cel = smoothstep(ramp.x+g, ramp.y+g, N·L)`，再 `lerp(cel,1,_MainLightIgnoreCelShade)`。
- Face SDF：脸坐标 F(前)/U(上)，R=cross(F,U)（引擎镜像坐标里用 cross(U,F)）；光投到 (R,F) 平面归一化，
  侧向 <0 时 uv1.x 镜像（2*mid-u）；`t=1-(fwd*0.5+0.5)`，`sdf=smoothstep(t-soft,t+soft,SDF.g+offset)`，
  `faceShadow=lerp(1,sdf,faceMask*GradientMask.g*intensity)`，`cel=lerp(cel*fs, fs, faceMask*RemoveGeometryShadow)`。
  SDF 用 **TEXCOORD_1**。
- 自阴影：`1 + k*(shadow*ndlFix - 1)`，k = SelfShadowIntensity*lerp(ForNonFace,ForFace,faceMask)；
  引擎里用太阳级联阴影代替角色专用阴影图。ndlFix = smoothstep(saturate((N·Ls-0.1)*10))。
- 深度纹理边缘光/阴影（屏幕空间，线性深度）：宽度 `w = 0.0054*P11/depth*globalMul*matMul*(1-0.33334*faceMask)*(H/W,1)`，
  y 方向乘屏幕底部淡出 smoothstep(saturate(uv.y*20))。
  rim 偏移 = 视空间光方向 xy 归一化 * 0.707w * RimWidthMul；4 个采样（+±x extend, +y extend，extend=0.1）取 min：
  `saturate((d - (self + PerCharZOffset + saturate(0.05+offsets) + ZWriteOffset*faceMask))*10/fadeRange)`。
  阴影偏移 = lerp(光方向, 平滑法线, IgnoreLightDir) * 0.707w * ShadowWidthMul；
  `ds = lerp(1, saturate((d - (self - (0.03-0.02*faceMask)))*50), usage)`，cel *= ds。
- Rim 颜色 = lerp(1,albedo,mix)*_RimLightColor*intensity/6*min(lightColor,2)，乘 rimMask 后相加。
- lit = saturate(1 + overallShadowStrength*(cel*ao - 1))。
- 阴影色：`base = SHADOW_COLOR ? HSV(albedo)*_ShadowTint : albedo`；HSV：hue+=HueOffset*S，
  sat → lerp(s, pow(s,1/(1+3*boost*S)), smoothstep(saturate(4s)))，value*=lerp(1,ValueMul,S)。
  `SKIN: shadow = lerp(base, albedo*lerp(SkinShadowTint,FaceShadowTint,faceMask), skinMask)`。
- color = lerp(shadow, albedo, lit) * lightColor * lerp(DepthTexTint*Bright（脸用 ForFace 版本，按 faceMask 插值）,1,ds)
  * lerp(overallShadowTint,1,lit) + rim + pow(1-N·V, 4)*角色粉色 rim（(1,0.43,0.64)*0.3）。无环境光项。
- 透明：alpha override `a = lerp(a, override.g, strength*lerp(1,pow4(remap(F·camBack)),mode))`；alpha<_Cutoff 丢弃。
- 脸法线修正（Eyes 0.8，Head 0）：N = normalize(lerp(N, lerp(F, pos-head-F, 0.75), amount*faceMask))。
- 描边：沿平滑法线（TEXCOORD7，切线空间）外扩 `width*min(dist*fovDeg,60)*0.00005` 米，
  width = _OutlineWidth*widthTex.g（OUTLINE_WIDTH_TEXTURE），正面剔除；颜色 = 按几何法线 cel 的
  lerp(shadow,albedo,lit)*light*_OutlineTintColor(0.25)，不透明。
- 深度纹理：face 区域写入时向后推 0.04 m（_FaceAreaCameraDepthTextureZWriteOffset），Hair_Front（队列 2454）也在深度纹理里。
- 主光颜色 = light.finalColor（无 clamp）。引擎方案：`E/π * exposure`（等价 Lambert 正入射），沿用曝光。

## 3. 引擎集成方案（待实现）

1. **导出脚本**（C:\Project\Yuki\tools\export_yuki.py，先备份 Yuki.glb）：
   - 新 glTF 材质扩展 `MINIENGINE_toon`：特性位、队列、线性化参数、额外纹理索引、脸 F/U/头位置（模型空间，骨骼 rest pose）。
   - 顶点属性 `_SMOOTH_NORMAL`：TEXCOORD7 用 Unity 切线框架转到网格空间再镜像 X。
   - 注意：场景 rest pose 与 bind pose 差很多（|J·IBM - meshWorld| 达 1.37）。
2. **加载器**：读扩展到 `ModelMaterialData::toon`；`Vertex` 新增 `outlineNormal[3]`（buffer.cpp 属性 6）；
   对有 skin 的节点按 rest pose 做 CPU 蒙皮 + 默认 morph 权重。
3. **纹理**：toon 额外贴图复用 set 1 槽位，按 sRGB/线性选槽：matcap→emissive/secondaryBaseColor（Color），
   mask/SDF/grade→Data 槽。注意 BC7 压缩可能让 SDF 出现色带，需要时对 SDF 关闭压缩。
4. **渲染**：新增 `VulkanToonPass`（ScenePassId::Toon，排在 Forward 之后、TransmissionCopy 之前，两种顺序都有）：
   - 不透明 toon 仍进 geometry pass（深度/法线/运动矢量），材质带 kShadingFlagForward 让 lighting 跳过，
     但 draw item 不标 forwardShaded（不让 triangle.frag 画）；toon Blend 项从普通列表移除。
   - 子步骤 A：私有 D32 + R32F 线性深度（含脸 0.04 推后）+ R8 眼睛掩码（模拟 stencil 77），
     先画不透明 toon（眼睛写 1），再画 Hair_Front 只写深度纹理。
   - 子步骤 B：SceneHdr + SceneDepth：不透明 toon（GE 测试）→ 描边（正面剔除）→ 队列 2450/2452/2454 混合项
     （Hair_Front 在掩码=1 处丢弃，Redraw 只画掩码=1 处）。
   - toon 参数：pass 自己的每帧 SSBO（set 2），push constant 偏移 64 传索引。
   - 应用 aerial perspective；附加光源先不做。

## 4. 实现（2026-10-07）

与第 3 节方案的差别：

- **不做 CPU 蒙皮**。glb 的 bind pose 是自然的 A 字姿势，场景 rest pose 是 T 字；引擎照旧按节点变换画（即 bind pose），
  导出脚本按 bind pose（`meshWorld · inverse(IBM)`）算头部坐标系写进扩展。
- 导出脚本（C:\Project\Yuki	ools\export_yuki.py，旧文件备份在 C:\Project\Yukiackup_2026-10-07）新增：
  - 材质扩展 `MINIENGINE_toon`：`keywords`、`renderQueue`、`disabledPasses`、全部 floats、colors（Color 类型属性转线性，
    Vector 不转）、13 张贴图索引、`head`（position/forward/up，bind pose）、`character`（粉色角色 rim、面部法线修正）。
  - 顶点属性 `_SMOOTH_NORMAL`：TEXCOORD7 按 shader 的规则（单位长度且 z≠0 才走切线框架）解码到网格空间再镜像 X。

引擎：

- `engine/scene/toon_material.h`：`GpuToonMaterial`（29 x vec4，std430）+ 特性位 `kToonFeature*`。
- 加载器 `ReadToonMaterial`：关键字 → 特性位；Unity stencil（Pass=Replace → 写眼睛掩码，Comp=NotEqual/Equal → 测试）；
  贴图放进 PBR 的空闲槽，颜色空间一致：
  `_ShadingGradeMap`→metallic(2)、`_AdditiveMatCapMask`→roughness(3)、`_OcclusionMap`→occlusion(4)、
  `_AdditiveMatCap`→emissive(5, sRGB)、`_AlphaBlendMatCapMask`→clearcoat(13)、`_FaceMaskMap`→clearcoatRoughness(14)、
  `_AlphaBlendMatCap`→sheenColor(15, sRGB)、`_SkinMaskMap`→sheenRoughness(16)、`_FaceShadowGradientMap`→specular(18)、
  `_FaceShadowGradientMaskMap`→iridescence(21)、`_OutlineWidthTexture`→iridescenceThickness(22)、`_AlphaOverrideMap`→transmission(23)。
  对应的 PBR 系数全为 0，几何/光线追踪路径不受影响。材质集改为 vertex+fragment 可见（描边宽度在顶点阶段读）。
- `Vertex` 增加 `outlineNormal[3]`（20 floats；`RAY_VERTEX_FLOATS` 同步改为 20）。只有 toon 管线读 location 6。
- 渲染：
  - 不透明 toon 仍进 geometry pass（深度/法线/运动矢量），材质带 forward 标志让 lighting pass 跳过；
    draw item 不标 `forwardShaded`，triangle.frag 不着色它们。透明 toon（队列 2450/2452/2454）只由 toon pass 画。
  - `ScenePassId::ToonPrepass`、`Toon` 排在 Forward 之后、TransmissionCopy 之前（两种 pass 顺序都有）。
  - ToonPrepass：私有 `ToonDepth` + `ToonLinearDepth`(R32F，脸部推后 0.04 m) + `ToonMask`(R8，眼睛)。透明项只写深度纹理不写掩码。
  - Toon：不透明（GE + depth bias，triangle.vert 与 toon.vert 都声明 `invariant gl_Position`，否则深度对不上会出现成片黑块）
    → 全部描边（正面剔除）→ 透明项（SrcAlpha 混合，眼睛掩码模拟 stencil）。
  - 每帧 toon 材质放在 `VulkanToonMaterials`（每帧槽一个 host-visible SSBO，最多 512 个 draw），push constant 带索引和曝光倍数。
- 光照单位：着色在 Unity 单位里算（主光颜色归一到亮度 1），再乘 `主光照度/π × exposure × 2^toonExposureEv`。
  `RenderDebugSettings::toonExposureEv` 默认 +1 EV（Graphics Debug → Anime characters），因为自动曝光让阳光下的白色漫反射只到纸白的一半左右，
  而原 app 的角色是 display-referred 的。主光取投影的平行光，没有则第一盏平行光，再没有用环境光。
- 自阴影用引擎的太阳级联阴影代替角色专用阴影图；附加光源（点/聚光）未实现。

验证（Debug，1024²，headless）：

- 正面主光、左右 90° 侧光、背光、全身各角度截图：脸部 SDF 在鼻梁处干净分界且左右镜像正确；眼睛/眉毛透过刘海可见；
  描边、matcap、阴影色、裙子 alpha-blend matcap 正常；背光时边缘光在朝光的轮廓上（几像素宽，符合原公式）。
- Vulkan 验证层无警告无错误；GPU 开销 ToonPrepass 0.03 ms + Toon 0.12 ms。
- ctest：除 `asset_browser_window`（main 上已知的 ImGui 字体断言）和 `vehicle_overlay`（main 的 d530fab 把默认 rib 数改成 50 但测试仍断言 10）外全部通过。

复现截图：`--scene <只含 Yuki 的场景> --wait-for-scene --camera x,y,z,yaw,pitch --frames 120 --capture`。
注意 `--model` 会在模型加载后重新取景，`--camera` 会被覆盖；且导入目录已存在时 `--model <外部路径>` 会失败。
平行光旋转是先 Y 后 X 再 Z：侧光要用 Z（如 `[0,0,±75]`），`[45,φ,0]` 的 φ 对朝下的方向不起作用。
