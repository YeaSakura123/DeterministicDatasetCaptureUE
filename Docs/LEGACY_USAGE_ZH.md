> 历史存档：以下版本号、进度与旧命令不代表当前发布状态。当前入口为 [使用说明](USAGE_ZH.md) 和 [README](../README.md)。

# Deterministic Dataset Capture UE 使用说明

本文面向需要从 Unreal Engine 5.7 自动生成超分辨率、时序超分辨率和帧生成训练数据的使用者。当前版本为 0.17.0、validator v22；本轮修正、连续数据与独立验证见 [时序数据验收](TEMPORAL_ACCEPTANCE.md)。下文版本里程碑中的旧检查数量是历史记录，不代表旧数据通过了 v22。

## 1. 当前进度

截至 2026-08-24，当前项目已经完成以下状态检查：

- `UnrealCodeDemo.uproject` 已启用 `SuperResolutionDataset`。
- 插件版本为 `0.16.0`，描述符版本为 `21`。
- UE 5.7 `Development Editor` 编译成功，本项目已生成 `UnrealEditor-SuperResolutionDataset.dll`。
- `SuperResolutionDataset.Job.Validation` 自动化测试成功，进程退出码为 `0`。
- 语义采集回归通过 `583/583` 项检查。
- Chaos 刚体缓存的记录和回放分别通过 `499/499` 项检查，专用跨进程门禁通过 `2360/2360` 项检查。
- GitHub `main` 当前包含 0.16.0，合并提交为 `dc05e28`。

能力状态如下：

| 功能 | 当前状态 | 可以怎样使用 |
|---|---|---|
| 同状态 HR、LR PNG 与线性深度 | 已实现，`spatial-sr-data-v1` 已认证 | 可直接用于空间超分辨率数据生产 |
| Native LR、Main View HDR、Motion、Depth、GBuffer、Object ID、透明度/Reactive | 已实现并通过夹具验证，仍为实验性 | 可用于研究和内部数据实验，但不能宣称 `nr-sr-data-v2` 已认证 |
| 固定和动态组件实例 ID | 已实现并验证 | 可生成组件级 Object ID；当前载体为 uint8，最多 255 个生命周期身份 |
| `SRDatasetControllable` 私有状态记录/回放 | 已实现并验证 | 用于游戏逻辑、第三方 VFX 和项目自定义状态 |
| Niagara CPU/GPU Sim Cache | 已实现固定拓扑记录/回放 | 保存一份权威 `.srncache` 后进行可验证回放 |
| Chaos StaticMesh 刚体 | 已实现固定拓扑可见姿态记录/回放 | 权威控制世界平移、旋转和固定组件缩放 |
| 骨骼姿态和 AnimBP 端点回放 | 已实现实验性缓存路径 | 用于独立 FG 端点验证 |
| 双向 FG 端点和真实中间帧装配 | 已实现但未认证 | 只能输出 `experimental_uncertified` 数据集 |
| 完整 Chaos 求解器快照 | 未实现 | 速度、力、约束、接触、休眠/岛状态和完整 solver restart 仍需专用适配器 |

“绝对控制”在本插件中指一套可审计协议：插件拥有固定时钟、采集顺序和已接入系统的状态；未接入协议的网络输入、音频驱动状态、外部 Niagara Data Interface、异步任务、MPC、自定义 WPO 或完整 Chaos 内部状态不会被自动声称为确定性。

## 2. 推荐的使用路线

第一次使用时按以下顺序进行：

1. 编译并确认插件已加载。
2. 使用一帧 smoke job 验证安装和输出路径。
3. 复制生产模板，设置自己的地图、相机、帧范围和分辨率。
4. 先以报告模式运行场景控制前检。
5. 为未受控系统加入 Sequencer、`SRDatasetControllable` 或原生缓存。
6. 打开严格前检并执行正式采集。
7. 使用 validator 检查单次采集。
8. 在新的 UE 进程中重复同一任务并做跨进程比较。
9. 只有验证报告通过的数据才进入训练集索引。

空间 SR 的最短路径是 `DownsampleFromHR`。如果目标模型需要真实低分辨率 Mip、LOD、Main View 时序缓冲或渲染器原生 LR 输入，应使用 `NativeRender`。

## 3. 环境要求

- Unreal Engine 5.7。
- Windows；仓库提供的自动运行脚本使用 PowerShell。
- D3D12 GPU；Main View/RDG 验证路径按 D3D12 测试。
- C++ Unreal 项目。Blueprint-only 项目可先创建一个空 C++ 类以生成编译目标。
- Python 3.10 或更高版本，用于离线 validator 和 FG 装配器。
- 足够磁盘空间。EXR、参考 HR 和长序列会显著放大数据体积。

Python 验证依赖：

```powershell
python -m pip install -r '.\Plugins\SuperResolutionDataset\Scripts\requirements-validation.txt'
```

如果插件目录名为 `DeterministicDatasetCaptureUE`，将后续命令中的 `Plugins\SuperResolutionDataset` 替换成该目录名。插件描述符和 C++ 模块名始终是 `SuperResolutionDataset`。

## 4. 安装和编译

### 4.1 安装到项目

将仓库复制到项目插件目录，例如：

```text
YourProject/
  Plugins/
    SuperResolutionDataset/
      SuperResolutionDataset.uplugin
      Config/
      Content/
      Docs/
      Scripts/
      Shaders/
      Source/
```

然后：

1. 打开 `.uproject`。
2. 在 **Edit → Plugins** 中搜索 **Super Resolution Dataset**。
3. 确认插件已启用。
4. 重新生成项目文件，或直接构建 `Development Editor`。

描述符会同时启用 `Niagara` 和 `ChaosCaching` 依赖。

### 4.2 命令行编译

```powershell
& '<UE_ROOT>\Engine\Build\BatchFiles\Build.bat' `
  YourProjectEditor Win64 Development `
  '-Project=D:\Path\To\YourProject.uproject' `
  -WaitMutex -NoHotReloadFromIDE
```

当前示例项目的目标名是 `UnrealCodeDemoEditor`。成功后应能看到：

```text
Plugins/SuperResolutionDataset/Binaries/Win64/UnrealEditor-SuperResolutionDataset.dll
```

编译成功不等于数据契约通过；正式数据仍必须经过离线 validator。

## 5. 第一次采集

### 5.1 先复制 smoke job

不要直接修改仓库提供的验证配置。复制 [`job.smoke.json`](../Config/job.smoke.json) 为自己的文件，例如 `Config/job.my-smoke.json`，至少修改：

- `expectedMap`
- `jobName`
- `outputDirectory`

smoke job 默认只有一帧，HR 为 `128x72`，LR 为 `64x36`。它只用于快速检查插件是否能工作，不能代表训练分辨率。

### 5.2 使用 PowerShell runner

从 Unreal 项目根目录运行：

```powershell
$DatasetPlugin = '.\Plugins\SuperResolutionDataset'

& "$DatasetPlugin\Scripts\RunDatasetCapture.ps1" `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job "$DatasetPlugin\Config\job.smoke.json" `
  -Project '.\UnrealCodeDemo.uproject'
```

对其他项目：

```powershell
$DatasetPlugin = '.\Plugins\SuperResolutionDataset'

& "$DatasetPlugin\Scripts\RunDatasetCapture.ps1" `
  -Map '/Game/Maps/YourCaptureMap' `
  -Job "$DatasetPlugin\Config\job.production-2x.json" `
  -Project '.\YourProject.uproject'
```

runner 会：

- 从 `.uproject` 的 `EngineAssociation` 定位 Unreal Editor；
- 以 `-game -RenderOffscreen -unattended` 启动独立进程；
- 传入 `-SRDatasetJob=<absolute-json-path>`；
- Main View 任务使用 HR 大小的离屏 viewport；
- 等待进程结束；
- 检查本次运行是否写出了新的 `manifest.json`；
- 要求 manifest 的 `state` 为 `Completed`；
- 成功返回退出码 `0`，失败返回非零退出码。

未注册的引擎或源码版引擎可以显式传入：

```powershell
-Editor 'D:\UE\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
```

可选 DDC 参数：

- `-UseMemoryDDC`：只用于短时隔离验证。
- `-UseWorkspaceLocalDDC`：将 DDC 放到项目 `Saved/DerivedDataCache`。
- 两者不能同时使用。

### 5.3 确认结果

成功时终端会打印：

```text
Dataset capture completed: <N> sample(s); manifest=<path>
```

默认输出位于项目目录下的：

```text
Saved/SRDataset/<job-name-or-configured-output>/
```

## 6. 一个可用于空间 SR 的生产配置

以下示例生成 300 个 2× 配对样本。LR 从 HR 确定性缩小，适合建立第一版空间 SR 基线：

```json
{
  "contractVersion": "spatial-sr-data-v1",
  "jobName": "city_spatial_sr_2x_v001",
  "expectedMap": "/Game/Maps/CityCapture",
  "sequence": "/Game/Cinematics/LS_CityCapture.LS_CityCapture",
  "startFrame": 0,
  "endFrame": 299,
  "frameStep": 1,
  "captureFrameOffset": 0,
  "captureFrameRateNumerator": 30,
  "captureFrameRateDenominator": 1,
  "warmupFrames": 32,
  "hRResolution": { "x": 1920, "y": 1080 },
  "lRResolution": { "x": 960, "y": 540 },
  "lRMode": "DownsampleFromHR",
  "resizeFilter": "CubicMitchell",
  "bCaptureDepth": true,
  "outputDirectory": "Saved/SRDataset/city_spatial_sr_2x_v001",
  "bResume": false,
  "bWriteManifestEveryFrame": true,
  "randomSeed": 1337,
  "bControlNiagara": true,
  "bForceNiagaraDeterminism": true,
  "bEnableChaosDeterminism": true,
  "bDisableMotionBlur": true,
  "bLockExposure": true,
  "bForceSynchronousRendering": true,
  "bRunSceneControlPreflight": true,
  "bRequireSceneControlPreflight": false,
  "bBlockOnStreamingBeforeCapture": true,
  "streamingWaitSeconds": 120.0,
  "bAutoQuit": true
}
```

注意 UE JSON 转换器使用的字段名是 `hRResolution` 和 `lRResolution`，不要自行改成 `hrResolution` 或 `lrResolution`。

如果需要真实渲染器 LR、Main View 时序输入和参考 HR，请从 [`job.production-2x.json`](../Config/job.production-2x.json) 开始。该模板默认：

- Native LR：`960x540`；
- HR 输出网格：`1920x1080`；
- 参考源渲染：`3840x2160`，之后用 Lanczos4 回到 `1920x1080`；
- 固定 60 Hz；
- 64 帧 warmup；
- 120 秒 streaming barrier。

该模板会生成研究用时序输入，但并不把 `nr-sr-data-v2` 自动变成已认证契约。

## 7. LR、HR 和参考 HR 的选择

| 模式 | 生成方式 | 优点 | 限制 |
|---|---|---|---|
| `DownsampleFromHR` | 渲染一次 HR，再在 CPU 缩小 | HR/LR 几何和状态配对最严格，适合空间 SR 基线 | 不体现低分辨率渲染时的 Mip、LOD、阴影和时序差异 |
| `NativeRender` | 在同一逻辑状态分别渲染 HR 与 LR | 保留真实低分辨率渲染行为，是时序诊断的必需模式 | 成本更高；颜色可能出现符合数值门禁但非字节完全相同的边界差异 |
| `bCaptureReferenceHR` | 在 HR 的 2×～4× 尺寸渲染，再缩回固定 HR 网格 | 可生成更高质量空间参考 | 显存、渲染时间和磁盘成本显著增加 |

过滤器选项为：

- `Box`
- `Bilinear`
- `CubicMitchell`
- `Lanczos4`

一般建议：空间 SR 初版使用 `DownsampleFromHR + CubicMitchell`；研究真实渲染器退化时使用 `NativeRender`；高质量参考使用 `ReferenceHRScale=2 + Lanczos4`。

## 8. Job 字段说明

完整字段定义以 [`SRDatasetTypes.h`](../Source/SuperResolutionDataset/Public/SRDatasetTypes.h) 为准。本节列出实际配置时需要理解的全部字段组。

### 8.1 身份、回放和相机

| 字段 | 说明 |
|---|---|
| `contractVersion` | 当前可直接生产的是 `spatial-sr-data-v1` |
| `jobName` | 任务和 manifest 身份；建议包含场景、倍率和数据版本 |
| `sequence` | 可选 Level Sequence 软对象路径 |
| `replayPass` | `Standard`、正向 FG 端点、反向 FG 端点或 FG 中间帧 |
| `expectedMap` | 地图包名保护；实际地图不匹配时立即失败 |
| `cameraActorTag` | 未使用确定性相机时的候选相机标签 |
| `bUseDeterministicCameraTransform` | 生成并锁定明确的玩家相机 |
| `deterministicCameraLocationCm` | 确定性相机世界坐标，单位厘米 |
| `deterministicCameraRotationDegrees` | Pitch/Yaw/Roll，单位度 |
| `deterministicCameraFOVDegrees` | 相机水平视场设置 |
| `deterministicCameraTranslationPerLogicalFrameCm` | 每逻辑帧增加的世界位移 |

相机选择顺序是：Sequencer camera cut、带指定 tag 的相机、Player Camera 0；打开确定性相机覆盖后以显式相机为准。

### 8.2 帧和输出

| 字段 | 说明 |
|---|---|
| `startFrame` / `endFrame` | 闭区间；总模拟帧包含两端 |
| `frameStep` | 每帧仍进行仿真，但只写出每 N 帧 |
| `captureFrameOffset` | `frameStep` 内的采样相位，范围为 `[0, frameStep)` |
| `captureFrameRateNumerator/Denominator` | 有理帧率，例如 `30000/1001` |
| `warmupFrames` | 正式起始帧前的确定性稳定帧，不写入样本 |
| `hRResolution` / `lRResolution` | HR/LR 输出尺寸 |
| `lRMode` | `DownsampleFromHR` 或 `NativeRender` |
| `resizeFilter` | HR→LR 过滤器 |
| `auxiliaryCaptureOrder` | 正常使用 `HighResolutionFirst`；另一顺序主要用于不变性验证 |
| `bCaptureDepth` | 输出空间基线深度 |
| `bCaptureTemporalDiagnostics` | 开启时序 HDR、Motion、Depth、mask、GBuffer 等实验输出 |
| `bCaptureMainViewTemporalDiagnostics` | 从真实 Player Main View 获取时序输入 |
| `bCaptureReferenceHR` | 开启空间超采样参考 HR |
| `referenceHRScale` | 每轴 2～4 倍源渲染 |
| `referenceResizeFilter` | 参考 HR 回到固定 HR 网格时的过滤器 |
| `bCaptureMainViewHUDlessColor` | 输出 after-tonemap、before-Slate 的无 HUD 场景颜色 |
| `bCaptureUIColorAlpha` | 单独输出 Slate/UMG RGBA |
| `outputDirectory` | 相对路径从项目根目录解析；也可使用绝对路径 |
| `bResume` | 默认 false；true 仅允许校验并复用完整空间数据，保留原始 manifest。配置变化、哈希损坏、部分输出和时间历史恢复均拒绝 |
| `bWriteManifestEveryFrame` | 每个样本后原子更新 manifest，建议保持开启 |

### 8.3 实例身份和时序验证

| 字段 | 说明 |
|---|---|
| `bAssignStableInstanceIds` | warmup/streaming 后按组件路径分配稳定 uint8 ID |
| `bAllowDynamicInstanceIdTopology` | 允许受控生成/销毁组件；新 ID 单调分配且永不复用 |
| `bCaptureSceneCaptureLRComparison` | 在同一阶段提取 native-LR SceneCapture 作为 Main View 像素域对照 |
| `bValidateMainViewSceneCapturePixelDomain` | 在受控静态夹具中执行硬像素域门禁 |
| `bLockTemporalJitterToLogicalFrame` | 将 TAA/TSR phase 和 View frame index 锁到逻辑帧 |
| `temporalJitterSequenceLength` | 夹具验证序列长度，UE TSR 基础序列可用 8 |
| `temporalJitterPhaseOffset` | 每个 clip 的稳定 phase 偏移 |
| `bSuppressMainViewOnUncapturedFrames` | FG 专用，避免未采样帧污染 Main View history |
| `bUseLastCapturedEndpointTransforms` | FG 专用，将上一采样端点作为 previous transform |

稳定实例 ID 的 `0` 保留为背景。当前编码最多支持 255 个从不复用的生命周期身份；它是组件身份，不是 triangle/surface 身份。

### 8.4 确定性和场景控制

| 字段 | 说明 |
|---|---|
| `randomSeed` | 全局稳定种子 |
| `bControlNiagara` | 插件主动发现并按逻辑时间驱动 Niagara |
| `bForceNiagaraDeterminism` | 临时强制系统 deterministic/fixed tick 设置 |
| `bEnableChaosDeterminism` | 开启 Chaos enhanced determinism CVar |
| `bDisableMotionBlur` | 关闭视图相关运动模糊 |
| `bLockExposure` | 关闭眼适应，稳定曝光 |
| `bForceSynchronousRendering` | 关闭可能扰动完成顺序的并行/异步路径 |
| `bLockMaterialTimeToLogicalFrame` | 用逻辑帧覆盖 Game Time，并冻结 Real Time |
| `bRejectVisibleWidgetComponents` | 拒绝可见 world-space `UWidgetComponent` 污染无 HUD 场景颜色 |
| `bBlockOnStreamingBeforeCapture` | warmup 后等待请求的渲染资源稳定 |
| `streamingWaitSeconds` | streaming barrier 超时；超时会失败而不是继续采集 |

### 8.5 场景控制前检

| 字段 | 说明 |
|---|---|
| `bRunSceneControlPreflight` | warmup 前写出 `scene_control_preflight.json` |
| `bRequireSceneControlPreflight` | 发现未分类对象时在第 0 帧前失败 |
| `bRequireControllableState` | 每个 `SRDatasetControllable` 必须返回非空规范状态 |
| `sceneControlAllowedTickingActorClassPaths` | 已人工审计的 Actor 类路径 |
| `sceneControlAllowedTickingComponentClassPaths` | 已人工审计的组件类路径 |
| `sceneControlAllowedNiagaraDataInterfaceClassPaths` | 已人工审计的 Niagara DI 类路径 |
| `sceneControlAllowedMaterialExpressionClassPaths` | 已人工审计的材质表达式类路径 |

建议分两步使用前检：

1. `bRunSceneControlPreflight=true`、`bRequireSceneControlPreflight=false`，只生成报告。
2. 处理全部未分类记录后，将 `bRequireSceneControlPreflight` 改为 `true`。

allowlist 规则区分大小写，默认是精确类路径。仅允许在非空类名前缀末尾放一个 `*` 做前缀匹配；`/Script/Engine.*` 之类过宽规则会被拒绝。allowlist 只表示人工审计，不会自动控制该类。

### 8.6 状态和原生缓存

| 字段 | 说明 |
|---|---|
| `bCacheSkeletalAnimationPosesForReplay` | 记录/应用 component-space 骨骼姿态 |
| `skeletalPoseCacheInputFile/OutputFile` | 骨骼姿态 artifact，输入和输出按任务角色配置 |
| `bCacheControllableStatesForReplay` | 记录/应用每逻辑帧的规范私有状态 |
| `controllableStateCacheInputFile/OutputFile` | 适配器私有状态 artifact；包含原始状态，应按 save data 保护 |
| `bCacheNiagaraSimForReplay` | 固定拓扑 Niagara CPU/GPU 原生 Sim Cache |
| `niagaraSimCacheInputFile/OutputFile` | `.srncache` 输入或输出，只能二选一 |
| `bCacheChaosRigidBodyTransformsForReplay` | 固定拓扑 StaticMesh 刚体可见姿态缓存 |
| `chaosRigidBodyCacheInputFile/OutputFile` | `.srcache` 输入或输出，只能二选一 |

记录 artifact 的任务通常要求：

- `replayPass=Standard`；
- `bResume=false`；
- 只设置 output，不设置 input；
- 所有逻辑帧都被完整记录。

回放任务只设置 input。正式数据应保留并版本化经过审查的 artifact；重新运行 live GPU Niagara 或物理仿真相当于创建新的源状态版本。

### 8.7 验证夹具字段

以下字段只应用于插件验证，不应直接带入生产数据任务：

- `bEnableSemanticValidationFixture`
- `semanticMotionScenario`
- `bValidateTemporalJitterSignCoverage`
- `bValidateDynamicInstanceIdTopology`
- `bValidateControllableStateCache`
- `bValidateNiagaraSimCache`
- `bValidateChaosRigidBodyCache`
- `bValidateNonFixtureSkeletalAnimation`
- `nonFixtureSkeletalValidationActorClass`
- `bValidateProjectAnimatedMaterial`
- `projectAnimatedMaterialValidationMaterial`

`bAutoQuit` 控制命令行任务结束行为。runner 还会传入 `-SRDatasetAutoQuit`，因此独立 Unreal 采集进程在成功或失败后退出是正常行为。

## 9. PIE 控制台和 Blueprint 使用

### 9.1 PIE 控制台

必须先点击 **Play** 进入 PIE 或 Game World，然后在游戏内控制台输入：

```text
SRDataset.Start "D:/Jobs/city_train.json"
SRDataset.Status
SRDataset.Cancel
```

重要说明：

- 这些是 Unreal 控制台命令，不是 PowerShell 或 `cmd.exe` 命令。
- `SRDataset.Start` 需要有效的 PIE/Game World；在纯 Editor World 中会报错。
- 命令结果写入 Unreal **Output Log**：**Window → Developer Tools → Output Log**。
- 控制台输入框不会持续显示运行日志，所以“没有终端输出”不代表命令没有执行。
- PIE 交互任务不会因为 JSON 中的 `bAutoQuit=true` 而关闭编辑器。
- 自动化生产推荐使用 `RunDatasetCapture.ps1`，不要依赖手动 PIE。

`SRDataset.Status` 的日志包含：

```text
State=<state> Frame=<frame> Captured=<n> Resumed=<n> Error='<message>' Output='<path>'
```

### 9.2 Blueprint API

Blueprint 可调用：

- `Start Dataset Capture`
- `Get Dataset Capture Status`
- `Cancel Dataset Capture`

传入的 job 类型是 `FSRDatasetCaptureJob`。如果项目系统需要明确的逻辑时间和私有状态控制，实现 `SRDatasetControllable`：

- `DatasetPrepare(RandomSeed, FixedDeltaSeconds)`
- `DatasetEvaluateFrame(FrameNumber, TimeSeconds)`
- `DatasetGetDeterministicState()`
- `DatasetApplyDeterministicState(CanonicalState)`
- `DatasetRestore()`

规范状态必须稳定排序、与进程地址和本地化文本无关，并覆盖所有会影响渲染的私有字段。manifest 默认只保存 Actor 路径、SHA-1 和 UTF-8 字节数；显式状态缓存 artifact 会保存原始字符串。

## 10. 输出目录和数据语义

空间基线输出：

```text
output/
  hr/                       # FinalColorLDR PNG
  lr/                       # 配对缩小或 native FinalColorLDR PNG
  depth/                    # SceneCapture 深度 EXR，单位 Unreal 厘米
  manifest.json
  scene_control_preflight.json   # 启用前检时
  instance_id_map.json           # 启用稳定实例 ID 时
```

Main View 时序路径可以额外输出：

```text
color_lr_scene_hdr/
color_lr_scene_capture_hdr/
color_hr_native_scene_hdr/
color_hr_reference_scene_hdr/
color_main_view_hudless_after_tonemap/
velocity_raw/
velocity_coverage/
motion_full_current_to_previous/
motion_valid/
depth_device_raw/
depth_view_linear_meters/
depth_valid/
depth_previous_reprojected_device/
history_rejection_mask/
history_rejection_valid/
history_rejection_reason/
disocclusion_mask/
disocclusion_valid/
disocclusion_reason/
translucency_after_dof_raw/
transparency_mask/
reactive_mask/
object_id/
normal_world/
base_color_linear/
material_properties/
gbuffer_valid/
ui_color_alpha/
```

关键语义：

```text
previous_pixel = current_pixel + motion_current_to_previous
+X = 向右
+Y = 向下
单位 = display pixel
```

- 空间 `depth/` 为 Unreal 厘米。
- 时序 `depth_view_linear_meters/` 为米。
- `color_lr_scene_hdr` 为 AfterDOF、pre-exposed、线性场景 HDR。
- `hr/` 和 `lr/` PNG 是 tonemapped、display-referred 的 FinalColorLDR，不是线性 HDR。
- `object_id` 为 `[0,255]` 整数，`0` 是背景。
- 所有 manifest 文件条目都有编码后文件 hash。
- 写文件使用 `.part` 临时文件再原子重命名。

## 11. 离线验证

### 11.1 验证单个数据集

```powershell
python '.\Plugins\SuperResolutionDataset\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\city_spatial_sr_2x_v001'
```

成功示例：

```text
PASS: <passed>/<total> checks; report=<path>
```

默认报告写到 `<dataset>/validation_report.json`。需要其他位置时使用 `--report`。

### 11.2 比较两个独立进程

把相同任务的 `outputDirectory` 分别改为 `run_a` 和 `run_b`，在两个新的 Unreal 进程中运行，然后：

```powershell
python '.\Plugins\SuperResolutionDataset\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\run_b' `
  --compare '.\Saved\SRDataset\run_a' `
  --compare-mode exact-replay
```

支持的比较模式：

| 模式 | 用途 |
|---|---|
| `exact-replay` | 普通重复运行 |
| `capture-order` | HR/LR/Depth 不同提交顺序的不变性 |
| `vfx-reverse` | VFX 正反向端点 |
| `skeletal-reverse` | 骨骼正反向端点 |
| `material-reverse` | 动画材质正反向逻辑时间 |
| `state-cache` | `SRDatasetControllable` 状态记录/应用 |
| `niagara-cache` | Niagara Sim Cache 记录/回放 |
| `chaos-cache` | Chaos 刚体姿态记录/回放 |

validator 对几何、深度、Motion、validity、mask、ID 和缓存证据执行严格门禁。颜色和少数量化 GBuffer 边界使用独立的窄数值阈值，并在 hash 不同时生成 heatmap；通过数值门禁不等同于像素文件字节完全相同。

## 12. 场景控制前检的生产用法

先复制 [`job.production-first-person-2x-strict.json`](../Config/job.production-first-person-2x-strict.json) 或生产模板，再执行：

1. 将 `bRunSceneControlPreflight` 设为 `true`。
2. 将 `bRequireSceneControlPreflight` 暂时设为 `false`。
3. 运行一次短任务。
4. 查看 `scene_control_preflight.json` 中的未分类 Actor、组件、Niagara DI 和材质输入。
5. 对每条记录选择真正的控制方式：Sequencer、原生缓存、`SRDatasetControllable`，或经过审核的窄 allowlist。
6. 将 `bRequireSceneControlPreflight` 和需要时的 `bRequireControllableState` 设为 `true`。
7. 再运行正式任务。

不要为了让门禁变绿而加入过宽 allowlist。严格前检的价值就是让训练数据中的外部状态来源显式化。

## 13. `SRDatasetControllable` 状态缓存

记录 job：

```json
{
  "bCacheControllableStatesForReplay": true,
  "controllableStateCacheOutputFile": "Saved/SRDataset/state_record/controllable_state_cache.json",
  "bResume": false
}
```

回放 job：

```json
{
  "bCacheControllableStatesForReplay": true,
  "controllableStateCacheInputFile": "Saved/SRDataset/state_record/controllable_state_cache.json"
}
```

验证：

```powershell
python '.\Plugins\SuperResolutionDataset\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\state_replay' `
  --compare '.\Saved\SRDataset\state_record' `
  --compare-mode state-cache
```

回放在 Actor Tick 后、任何数据渲染提交前调用 `DatasetApplyDeterministicState()`，随后要求读回的规范状态字节完全一致。artifact 包含项目私有原始状态，不应随公开数据集无意发布。

## 14. Niagara CPU/GPU Sim Cache

参考配置：

- [`job.niagara-sim-cache-record-validation.json`](../Config/job.niagara-sim-cache-record-validation.json)
- [`job.niagara-sim-cache-replay-validation.json`](../Config/job.niagara-sim-cache-replay-validation.json)

依次运行两个 job，然后：

```powershell
python '.\Plugins\SuperResolutionDataset\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\niagara_sim_cache_replay_validation' `
  --compare '.\Saved\SRDataset\niagara_sim_cache_record_validation' `
  --compare-mode niagara-cache
```

当前 Niagara 契约：

- 固定组件、系统和 emitter 拓扑；
- 捕获所有属性；
- GPU emitter 使用立即 readback；
- 不使用 rebase、interpolation 或 extrapolation；
- 不保存任意外部/自定义 Data Interface storage；
- 正式工作流保留一份经过审查的 `.srncache` 并从它回放；
- 不承诺重新执行 live GPUComputeSim 后再次录出字节相同的 payload。

## 15. Chaos 刚体姿态缓存

参考配置：

- [`job.chaos-rigid-body-cache-record-validation.json`](../Config/job.chaos-rigid-body-cache-record-validation.json)
- [`job.chaos-rigid-body-cache-replay-validation.json`](../Config/job.chaos-rigid-body-cache-replay-validation.json)

记录：

```powershell
$DatasetPlugin = '.\Plugins\SuperResolutionDataset'

& "$DatasetPlugin\Scripts\RunDatasetCapture.ps1" `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job "$DatasetPlugin\Config\job.chaos-rigid-body-cache-record-validation.json" `
  -Project '.\UnrealCodeDemo.uproject'
```

回放：

```powershell
$DatasetPlugin = '.\Plugins\SuperResolutionDataset'

& "$DatasetPlugin\Scripts\RunDatasetCapture.ps1" `
  -Map '/Game/FirstPerson/Lvl_FirstPerson' `
  -Job "$DatasetPlugin\Config\job.chaos-rigid-body-cache-replay-validation.json" `
  -Project '.\UnrealCodeDemo.uproject'
```

比较：

```powershell
python '.\Plugins\SuperResolutionDataset\Scripts\ValidateDataset.py' `
  '.\Saved\SRDataset\chaos_rigid_body_cache_replay_validation' `
  --compare '.\Saved\SRDataset\chaos_rigid_body_cache_record_validation' `
  --compare-mode chaos-cache
```

当前 Chaos 范围：

- 已注册、启用物理模拟、非 transient、固定拓扑的 `UStaticMeshComponent`；
- 通过 UE 5.7 `ChaosCaching` StaticMesh adapter 记录原生 `UChaosCache`；
- `.srcache` 绑定引擎、地图、帧率、种子、组件/Actor/类/StaticMesh 身份；
- 保存原生 payload SHA-1、初始变换、固定缩放和每逻辑帧精确参考姿态；
- 回放按 `(cacheFrameIndex + 1) * fixedDelta` 随机访问；
- 原生平移和旋转验证阈值为 `0.05 cm` 和 `0.05°`；
- 在任何数据渲染提交前应用经过 artifact 和逐帧 SHA-1 验证的最终可见姿态。

当前明确不包含：

- 线速度和角速度；
- 力和冲量；
- constraint state；
- contact manifold；
- sleeping/island state；
- Geometry Collection；
- deformable；
- 完整 Chaos solver restart。

因此它适合生成视觉上权威一致的刚体训练帧，不适合从中间帧继续一段新的、物理上完全等价的分支仿真。

## 16. 实验性帧生成工作流

FG 必须使用三个独立 Unreal 进程：

```text
正向端点：t0 ------------> t1，采集 motion_1_to_0
反向端点：t0 <------------ t1，采集 motion_0_to_1
真实中间：        t0.5
```

分别运行：

- [`job.fg-endpoint-validation.json`](../Config/job.fg-endpoint-validation.json)
- [`job.fg-reverse-endpoint-validation.json`](../Config/job.fg-reverse-endpoint-validation.json)
- [`job.fg-intermediate-validation.json`](../Config/job.fg-intermediate-validation.json)

验证三个源数据集后装配：

```powershell
python '.\Plugins\SuperResolutionDataset\Scripts\AssembleFrameGenerationDataset.py' `
  --endpoints '.\Saved\SRDataset\fg_endpoint_validation' `
  --reverse-endpoints '.\Saved\SRDataset\fg_reverse_endpoint_validation' `
  --intermediate '.\Saved\SRDataset\fg_intermediate_validation' `
  --output '.\Saved\SRDataset\fg_pair_001'

python '.\Plugins\SuperResolutionDataset\Scripts\ValidateFrameGenerationDataset.py' `
  '.\Saved\SRDataset\fg_pair_001'
```

当前 validator 可能打印 `PASS (UNCERTIFIED)`。这表示文件和已声明的实验性门禁通过，不代表该数据集已经获得完整 FG 训练认证。动态逐表面身份、项目自有 WPO 和更广生产场景覆盖仍是开放项。

## 17. 常见问题

### 17.1 在 UE 控制台输入命令后看不到终端输出

这是正常的输出位置差异。`SRDataset.Start/Status/Cancel` 把信息写到 Unreal Output Log，不会写入启动 Codex 或 PowerShell 的终端。打开：

```text
Window → Developer Tools → Output Log
```

并搜索 `SRDataset`。如果需要可脚本化终端结果，使用 `RunDatasetCapture.ps1`。

### 17.2 输入命令后游戏或独立窗口立即关闭

runner 会传入 `-SRDatasetAutoQuit`，任务结束后 Unreal 进程自动退出是设计行为。先检查：

1. `Saved/SRDataset/<output>/manifest.json` 是否存在；
2. `state` 是否为 `Completed`；
3. `capturedSamples` 是否符合预期；
4. runner 是否打印完成信息；
5. `Saved/Logs/` 中是否有 crash stack 或 `LogSRDataset` 错误。

有 `Completed` manifest 且 runner 返回 0 时不是闪退。没有新 manifest 或 manifest 为 `Failed` 才按故障处理。

### 17.3 两张图片分辨率非常低

检查 job 文件。仓库中的 smoke、semantic、Niagara 和 Chaos 验证任务故意使用很低的分辨率以加快回归，例如：

- smoke：HR `128x72`、LR `64x36`；
- 部分语义/FG 夹具：HR `256x144`、LR `128x72`；
- Chaos 验证：HR `512x288`、LR `256x144`。

它们不是插件上限。训练任务应改用：

```json
{
  "hRResolution": { "x": 1920, "y": 1080 },
  "lRResolution": { "x": 960, "y": 540 }
}
```

或者 4× 空间基线：

```json
{
  "hRResolution": { "x": 3840, "y": 2160 },
  "lRResolution": { "x": 960, "y": 540 }
}
```

### 17.4 `SRDataset.Start` 报没有有效 World

必须先进入 PIE 或 Game World。Editor viewport 本身不是受支持的采集 World。无人值守任务改用 PowerShell runner。

### 17.5 runner 找不到 Unreal Engine

确认 `.uproject` 有有效 `EngineAssociation`，或传入完整 `-Editor` 路径。

### 17.6 没有生成 manifest

优先检查：

- map 参数是否存在；
- `expectedMap` 是否与实际 map 完全一致；
- job JSON 是否能被解析；
- 插件是否已编译并启用；
- 输出目录是否可写；
- Unreal 日志中是否有 shader、RHI 或模块加载错误。

runner 会把“没有新 manifest”视为失败并返回退出码 1。

### 17.7 strict preflight 在第 0 帧前失败

查看 `scene_control_preflight.json`。不要立刻加入宽 allowlist；先判断对象是否应该由 Sequencer、`SRDatasetControllable`、Niagara/Chaos cache 或项目专用适配器控制。

### 17.8 重跑后部分帧被跳过

`bResume=true` 只会复用同版本插件/引擎/关卡、相同规范化配置及原始文件哈希全部匹配的完整空间数据；仅 `bResume` 本身允许不同，原 manifest 字节和来源信息不变。它不证明磁盘上的当前关卡资产仍与历史版本相同，而是取回已验证的原数据。时间数据和不完整数据必须在新的空输出目录中重新采集。独立重放也必须使用空目录与 `bResume=false`。

状态、Niagara 或 Chaos 的 artifact 录制任务不能用 resume 拼接成看似完整的缓存。

### 17.9 两次颜色 hash 不同

先运行 validator，不要只比较文件 SHA-1。渲染器浮点、量化和边界像素可能产生极小差异；validator 对颜色使用版本化数值门禁并生成 heatmap。Depth、Motion、ID、validity、缓存拓扑和 artifact 证据仍执行各自的严格门禁。

### 17.10 Main View 输出尺寸不正确

不要直接用任意窗口大小启动 Main View 任务。PowerShell runner 会从 `hRResolution` 设置 `-ResX/-ResY/-ForceRes`，并从 LR/HR 计算内部 render fraction。

## 18. 正式数据生产检查表

采集前：

- [ ] 使用独立、版本化 job 文件。
- [ ] 固定 UE 版本、项目 commit、插件版本、GPU、驱动和 RHI。
- [ ] `expectedMap` 与实际 map 一致。
- [ ] 相机和 Sequencer 路径已经审查。
- [ ] 生产分辨率不是 smoke/fixture 分辨率。
- [ ] 曝光、Motion Blur、动态分辨率和 streaming 策略已经明确。
- [ ] 场景控制前检无未处理记录。
- [ ] 所有外部状态已接入控制协议或缓存。
- [ ] 输出目录为空，或已明确使用 resume 的风险。

采集后：

- [ ] manifest `state=Completed`。
- [ ] `capturedSamples` 与预期数量一致。
- [ ] 单数据集 validator 通过。
- [ ] 第二个干净进程的重复运行通过比较门禁。
- [ ] heatmap 和 dataset profile 已人工检查。
- [ ] cache artifact 与数据集一起版本化，但私有状态 artifact 未被误公开。
- [ ] 训练索引只引用通过验证的数据根目录。

## 19. 进一步阅读

- [英文 README](../README.md)
- [架构与确定性契约](ARCHITECTURE.md)
- [开发路线图](ROADMAP.md)
- [生产 2× 模板](../Config/job.production-2x.json)
- [First Person 严格 2× 模板](../Config/job.production-first-person-2x-strict.json)
- [validator 源码](../Scripts/ValidateDataset.py)
