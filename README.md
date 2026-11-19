# Scene Recorder — Minecraft 场景录制 → Blender

在 Minecraft（NeoForge 1.21.1）里录制**三维场景**，而不是录视频：方块、实体、粒子、物理结构、相机，全部按固定帧率（默认 **200 帧/秒**）记录下来，再用 Blender 插件导入，自己用 Cycles 渲染。

- `mod/` — NeoForge 1.21.1 模组（纯 Java，没有原生库，macOS / Apple Silicon 可用）
- `blender/scenerec_importer/` — Blender 扩展（按 Blender 5.3 的 API 编写，最低 4.4）
- `docs/FORMAT.md` — 录制文件格式

## 原理

### 1. 在"交给显卡"的那一刻截取几何

不管哪个模组，原版渲染管线里的世界几何最后都要经过 `VertexBuffer.upload()`（上传顶点）和 `VertexBuffer.draw()`（绘制）。模组在这两处：

- 上传时复制顶点数据（位置、颜色、UV、光照、法线）；
- 绘制时读取当前着色器的 `ModelViewMat` 和 `ChunkOffset`，求出这次绘制在世界里的真实位置：
  `世界坐标 = 相机位置 + 视角旋转⁻¹ × ModelViewMat × (顶点 + ChunkOffset)`

所以**不需要知道几何是谁画的**。地形区块、实体、方块实体、粒子、雨雪、手里的东西都能截到；**Sable 的物理结构**是通过修改这两个参数来移动和旋转的，也会被自动截到正确的位置和姿态。带 3D 模型的资源包已经烘焙进顶点里了，自然也包含在内。

录制时会做这些调整：

- **视锥剔除关闭**，加载的区块和实体全部渲染，身后的东西也录进去；
- **区块重新编译**：去掉预先烘焙的环境光遮蔽和方向阴影（Cycles 会自己计算），只保留草和树叶的生物群系染色；
- **方块发光等级**写进顶点里一个没用到的填充字节（`0x80 | 发光等级`），导入后光源方块会发光；
- 天空、太阳、云、世界边界、方块选择框、调试线、附魔光效、实体脚下的圆形阴影不录。

### 2. 固定时间步长：再卡也是严格 200 帧

录制期间，游戏时间不再按现实时间走：**每画一帧，游戏时间只前进 1/200 秒**（`DeltaTracker.Timer.advanceGameTime`）。一帧用 5 毫秒还是卡了 3 秒，录出来都只前进 5 毫秒，一帧不丢。

单人游戏里，内置服务端会和客户端**锁步**：客户端每到一个 tick 才放行服务端走一个 tick，并等它算完。所以服务端卡（比如物理模组太重）也不影响录制。电脑跟不上时，录制过程中看到的是慢动作，导出的动画仍是正常速度。

### 3. 模组兼容

| 模组 | 处理方式 |
|---|---|
| **Sable** | 渲染无需适配（见上）。Sable 客户端用现实时间估算服务端的 tick 速度，录制时把这个值改回标准的 50 ms（`ClientSableInterpolationState.receiveInfo`）。 |
| **Create（机械动力）/ Flywheel** | Flywheel 用 GPU 实例化渲染，几何只存在于显卡上。录制时自动切到 `flywheel:off`，结束后恢复。Create 的动态结构（轴承、活塞、矿车、火车……）和旋转零件就会走原版管线被截到。Create 每帧会给这些模型重新乘一遍方向阴影（catnip `SuperByteBuffer.renderInto`），录制时关掉；它缓存的结构模型会在开始和结束时清空重建。 |
| **Create Aeronautics** | 建立在 Sable + Create 之上，以上处理都适用。 |
| 其他模组 | 只要走原版渲染管线（`VertexBuffer`），就自动兼容。 |

## 使用

### 安装

1. 把 `scenerec-1.0.0.jar` 放进 `.minecraft/mods`（需要 NeoForge 1.21.1，21.1.228 或更新）。
2. Blender：编辑 → 偏好设置 → 获取扩展 → 右上角菜单 → **从磁盘安装…** → 选择 `scenerec_importer-1.0.0.zip`。

### 录制

- 按 **F6** 开始/停止（可在按键设置里改），或者用命令 `/scenerec start [fps]`、`/scenerec stop`。
- 开始后会先"预热"：等区块全部重新编译完，聊天栏出现 `SceneRec: recording` 才真正开始。
- 帧率在 `config/scenerec-client.toml` 里改（`fps`，默认 200）。
- 录制文件保存在 `.minecraft/scenerec/<日期_时间>/`：
  - `scene.screc` — 几何和动画（分块压缩）
  - `textures/` — 游戏里实际用到的贴图（从显卡读回来的，资源包效果都在），以及 PBR 贴图
  - `meta.json` — 帧率、帧数、模组列表

### 导入 Blender

文件 → 导入 → **Minecraft Scene Recording (.screc)**，选择 `scene.screc`。导入后：

- 帧率和帧范围自动设好（录制的第 k 帧放在 Blender 的第 k+1 帧）；
- **SceneRec Camera**：录制时的相机，逐帧关键帧，第一人称、第三人称切换都照录。可以直接用，也可以自己另加相机；
- **World Geometry**：地形，每种材质合并成一个物体。区块在录制中被重建的部分由几何节点修改器 `SceneRec Lifetime` 按帧切换；
- **Moving Geometry**：Sable 物理结构（带变换关键帧），以及每帧都在变的几何（实体、粒子、动态结构等）。后者存放在录制目录下的 `frames.usdc`，通过 Blender 的 USD 缓存逐帧从磁盘读取，录很长也不会把内存撑爆；
- **Hand**：第一人称手和手持物品，不需要可以隐藏；
- 节点组 **SceneRec Settings**：统一调整所有材质的发光强度，以及无 PBR 时的默认粗糙度。

插件只负责搭场景，不会渲染。灯光、天空（HDRI）、渲染设置由你自己决定。

## 材质与 PBR

- 基础色 = 贴图 × 顶点颜色（生物群系染色），最近邻采样，保持像素风；
- 透明：镂空类（树叶、实体）用 alpha 裁剪；半透明（水、玻璃、冰）用 alpha 混合，粗糙度较低；附加混合（信标光束、闪电）用发光加透明；
- **发光**：方块按发光等级（火把 14、萤石 15……）自发光，强度在 SceneRec Settings 里调；
- **LabPBR 1.3 材质包**（游戏里装了才有）：
  - `_n`：法线（DirectX 格式，导入时翻转绿色通道转成 Blender 的 OpenGL 格式，由 X、Y 重建 Z）+ 环境光遮蔽；
  - `_s`：光滑度 → 粗糙度 `(1−s)²`；F0 0–229 → IOR；**230–237 是 8 种预设金属**（铁、金、铝、铬、铜、铅、铂、银），用规范给出的复折射率 N、K 精确计算反射颜色；255 为以基础色作反射色的金属；65–255 → 次表面散射；alpha 通道 → 自发光；
  - 图集里没有 PBR 贴图的方块，用默认材质。

## 已知限制

- **多人服务器**无法锁步，只能保证客户端这一侧按固定帧率；服务端卡了，录制里也会卡。严格 200 帧只保证单人游戏。
- 游戏完全卡死（无响应）时没有画面，当然录不了；低帧率、卡顿、服务端掉 TPS 都没问题。
- 动态贴图（水、岩浆、火的贴图动画）目前固定为录制开始时那一帧。
- 在显卡里自己生成几何、或直接调用 OpenGL 绘制的模组截不到（Flywheel 和 Sable 已处理）。Iris 光影包不支持，录制时请关闭。
- 录制期间游戏画面没有环境光遮蔽，并且会变成慢动作（电脑跟不上时），这是正常现象。
- LabPBR 的高度通道（视差）没有使用。
- 第一人称手部在游戏里用独立的视角绘制，放进 3D 场景后位置是对的，但可能穿进墙里。

## 开发

```bash
cd mod && ./gradlew build            # → mod/build/libs/scenerec-1.0.0.jar
# 无人值守的端到端测试（Linux 下可配合 xvfb-run + Mesa 软件渲染）：
./gradlew runAutotest -Pframes=300 -PswitchCamera=true -Pspin=4 \
  "-Pcommands=time set 6000;..." "-PcommandsAtStart=..."
# Blender 无界面导入测试：
blender -b --factory-startup --python blender/tests/import_test.py -- <scene.screc> out.blend
blender --command extension build --source-dir blender/scenerec_importer --output-dir dist
```

自动测试需要在 `mod/run/saves/test` 准备一个存档。把 Create、Sable、Create Aeronautics 的 jar 放进 `mod/run/mods` 就能一起测试。
