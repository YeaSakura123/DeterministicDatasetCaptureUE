# 0.17.0：受控时序数据闭环

2026-09-05。本轮落实根目录 `PLUGIN_REVIEW_2026-09-05.md` 的有界目标。保留现有采集主干，修正数据对应关系，以独立消费者和实际渲染结果验收。

**结论：受控数据闭环通过，完整 M1 尚未完成。** `nr-sr-data-v2` 和 FG 认证均未开启。慢速相机对照未达到预设的图像改进门槛，失败结果保留；不能宣称所有样例全部通过。

## 实现与兼容性

| 问题 | 0.17.0 的行为 | 证据 |
|---|---|---|
| 恢复时给旧像素配上新相机和来源 | 默认 `bResume=false`。true 仅复用同插件/引擎/关卡、完整帧列表、相同规范配置及原始哈希均匹配的空间数据；只允许 `bResume` 字段不同。原 manifest 字节不变 | 相同任务通过；改变相机、损坏文件、未完成 manifest 均拒绝且不覆盖原记录 |
| Standard 隔帧保存却声称 Motion 跨越两个保存帧 | Standard 时间诊断强制 `FrameStep=1`；时间 resume 拒绝 | 启动前负例；旧固定 jitter 错误数据从 v21 的 676/676 PASS 变为 v22 的 676/680 FAIL，真实 previous camera 也被独立识别 |
| 未考虑前后输入栅格的 jitter 差 | display Motion 按两轴实际尺寸缩放，并加入 previous-minus-current render jitter；camera fallback 先从 jittered 深度栅格恢复 unjittered clip；previous depth 用含 AA 的重投影矩阵 | 独立矩阵投影、解析轨迹、显露区域、轴向相机测试 |
| 只在第一帧 reset | 从真实 `FSceneView::bCameraCut` 传回标志；首帧或 renderer cut 清空可用历史，`motionTrainingUsable=false` | 24 帧 Sequencer 片段第 12 帧真实切镜，reset 为 `[0,12]`，该帧全图拒绝历史，消费者不引用上一镜头 |

新的 `historyRejectionSource` 为 `component_identity_and_static_camera_depth_on_jittered_rasters_v3`，并写入 `historyRasterMapping`、`rendererCameraCut` 与 `resetReason`。validator v22 从保存的投影矩阵推导栅格位移，而非复制生产端 jitter 字段运算；还校验实际 GPU Previous View 是否对应上一保存帧。UE 的整数舍入按 `floor(x+0.5)` 复算。

旧 v1/v2 时间输出仍可读作诊断材料，但不能通过当前的时序正确性门槛。修正没有重写旧数据或替旧数据重新认证。完整空间复用取回原始数据及其来源，不证明当前磁盘上的资产与历史版本仍相同；部分采集和时间历史恢复暂不支持。

## 数据与范围

实测 UE 5.7、DX12/SM6、AMD Radeon RX 7900 XTX、30 fps、固定曝光、8 相位 jitter、960×540 Main View LR → 1920×1080 reference。reference 由 3840×2160 的隔离、无 jitter SceneCapture 经 Lanczos4 缩到固定 HR 网格。它是现有空间超采样参考，不是根计划中 16～64 个时间子样本的完整 GT 实现。

`GenerateTemporalAcceptanceAssets.py` 在 `/Game/SRDatasetAcceptance` 创建普通 StaticMesh、无光照棋盘材质、细线、带纹理移动板及 Level Sequence。移动板在全部 64 帧持续移动，未沿用旧夹具“第 2 帧后停止”的运动函数。FirstPerson 使用现有项目场景。正式任务都启用严格前检；精确白名单列于对应 JSON，白名单仍只表示审计例外。

| 片段 | 帧数 / jitter 起始相位 | 独立结论 | validator v22 |
|---|---|---|---|
| Static | 64 / 0 | 几何、辐射尺度、固定参考和零 Motion 通过 | 14887/14887 |
| CameraFast | 64 / 1 | 相机 Y 从 -96 到 +96 cm，重投影通过 | 14887/14887 |
| ObjectOnly | 64 / 3 | 物体 Y 从 -120 到 +120 cm，重投影通过 | 14887/14887 |
| Mixed | 64 / 5 | 相机与物体持续运动，重投影通过 | 14887/14887 |
| CameraCut | 24 / 2 | 第 12 帧切换真实相机，reset 和切镜两侧配对通过 | 5603/5603 |
| FirstPerson | 64 / 6 | 非夹具连续数据、完整性和消费成功；无解析场景真值，不授予像素质量认证 | 15018/15018 |
| CameraOnly | 64 / 1 | 慢速相机：几何通过；图像改进未达 20%，保留 FAIL | 14887/14887 |
| MixedRepeat / MixedReverseAux | 各 64 / 5 | 两个新进程，各 1792 对模态文件与 Mixed 逐字节一致 | 独立字节/矩阵/提交顺序检查通过 |

合计 536 帧正式尺寸材料。短定位用的 512×288/256×144 片段另存，不计入该数量。全部材料仍是验证数据，不是训练集划分，也不是完整 M1 的 10～30 分钟多关卡交付。

## 独立证据与容差

`ValidateTemporalAcceptance.py` 不导入 `ValidateDataset.py`，也不用生产端 rejection mask 筛选重投影评分像素。它用保存的 Color、Motion、Depth、相机与投影矩阵消费连续帧，并用预定轨迹和场景尺寸检查物体位置、轮廓及深度。

| 检查 | 预设门槛 | 结果 |
|---|---|---|
| LR 与 reference 的平坦区域线性 RGB | MAE ≤ 0.002，P99 ≤ 0.01 | 各片段 MAE 约 2.98e-8，P99 5.96e-8；仅代表无光照解析材质区域 |
| reference / LR 轮廓、reference 细线中心 | 对各自栅格 ≤ 0.75 pixel | 最大 reference 轮廓误差 0.7447 HR pixel；LR 0.4984 LR pixel；细线 0.4536 HR pixel |
| 解析射线深度 | 最大误差 ≤ 0.002 m | 最大约 9.73e-5 m |
| 解析相机/物体 Motion | 每帧 P99 ≤ 0.05 display pixel | 最大约 0.00483 display pixel |
| 运动片段重投影 | correct / no-warp ≤ 0.8；correct / wrong-motion ≤ 0.8；correct / omitted-jitter ≤ 0.9 | CameraFast、ObjectOnly、Mixed 均通过 |

重投影比较在同一组稳定可见区域上进行。图像评分显式采用 3×3 box 预滤波与 bilinear warp，以减少点采样棋盘锯齿的干扰；原始 EXR 未改动，同时报告 raw no-warp / raw warp 误差。Motion、Depth、轮廓与平坦辐射检查使用原始数据。

| 评分区域 | correct / no-warp | correct / wrong-motion | correct / omitted-jitter |
|---|---:|---:|---:|
| CameraFast 背景 | 0.466 | 0.265 | 0.818 |
| ObjectOnly 前景 | 0.202 | 0.104 | 0.846 |
| Mixed 前景 | 0.237 | 0.120 | 0.704 |
| CameraOnly 慢速背景 | **0.870，未通过** | 0.570 | 0.818 |

慢速相机约 0.49 LR pixel/frame，图像 L1 改进为 13%，低于预先设定的 20%。其解析 Motion 和配对通过，不能把该结果伪装成算法质量合格。额外 CameraFast 约 1.47 LR pixel/frame，使用同一阈值达到 53% 改进。慢速数据保留作为下一阶段的亚像素失败案例。

静态点采样棋盘存在混叠，插值后的 L1 不保证低于原地比较：本轮静态 correct/no-warp 约 1.107。静态与切镜采用原计划要求的独立条件：零几何 Motion、投影/辐射/轮廓正确、错误 jitter 方向显著更差、cut 后不消费旧历史。没有用放宽 20% 阈值来给慢速运动片段改判。

首次验收脚本有两个被像素证据揭示的问题：把有厚度的立方体当作平面轮廓；把斜视侧面也当成固定正面深度。已分别改为八顶点投影和 ray–box 相交，像素和深度容差均未改变。首次失败 JSON 保留为 `acceptance-initial-v1.json` / `acceptance-flat-depth-model.json`，以便复核。

`VerifyAcceptanceVisibility.py` 不使用输出 Motion 或 Object ID 推导真值，而以已知背景深度、相机位移和前一帧立方体投影计算明确显露区域。ObjectOnly 的 12672 个、Mixed 的 11264 个像素全部得到有效历史拒绝。只评价远离亚像素边界的可判定区域，不声称覆盖动态同实例自遮挡。

`VerifyTemporalReplay.py` 从磁盘重新计算所有文件哈希，并检查相机、场景状态、reset、实际前后矩阵与 jitter。两个比较各 64 帧、28 模态、1792 对文件完全一致。反向辅助提交实际为 `lr → depth → hr → hr_reference → main_view_temporal`，基准为 `hr → hr_reference → lr → depth → main_view_temporal`；不是仅比较配置名称。

另有 8 帧轴向相机前进测试：7 个非 reset 帧各 36864 个 camera-fallback 像素经独立 world-point 重建，Motion 最大误差约 1.62e-5 display pixel，previous-device-depth 最大误差约 8.64e-10。该测试专门覆盖景深改变时的 jitter/clip 映射。

## 复现

插件配置在 `Config/TemporalAcceptance/*.json`，依赖沿用 `Scripts/requirements-validation.txt`。生成资产脚本可重复运行，生成的资产局限于 `/Game/SRDatasetAcceptance`。

```powershell
# 项目根目录；按本机安装位置设置 UE 路径。
$ueEditor = 'D:/Software/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$captureProject = (Resolve-Path 'UnrealCodeDemo.uproject').Path
$assetScript = (Resolve-Path 'Plugins/SuperResolutionDataset/Scripts/GenerateTemporalAcceptanceAssets.py').Path
& $ueEditor $captureProject -run=pythonscript "-script=$assetScript" -EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities,SequencerScripting -unattended -NullRHI -NoSound

# 每个任务启动独立进程；首次采集使用空输出目录。
$captureJob = (Resolve-Path 'Plugins/SuperResolutionDataset/Config/TemporalAcceptance/Mixed.json').Path
& $ueEditor $captureProject /Game/SRDatasetAcceptance/TemporalLab -game -RenderOffscreen -unattended -NoSound -dx12 -ResX=1920 -ResY=1080 -ForceRes "-SRDatasetJob=$captureJob" -SRDatasetAutoQuit

python Plugins/SuperResolutionDataset/Scripts/ValidateDataset.py Saved/SRDataset/TemporalAcceptance/Mixed
python Plugins/SuperResolutionDataset/Scripts/ValidateTemporalAcceptance.py Saved/SRDataset/TemporalAcceptance/Mixed --output Saved/SRDataset/TemporalAcceptance/Mixed/independent
python Plugins/SuperResolutionDataset/Scripts/VerifyAcceptanceVisibility.py Saved/SRDataset/TemporalAcceptance/Mixed --report Saved/SRDataset/TemporalAcceptance/Mixed/visibility.json
python Plugins/SuperResolutionDataset/Scripts/VerifyTemporalReplay.py Saved/SRDataset/TemporalAcceptance/Mixed Saved/SRDataset/TemporalAcceptance/MixedReverseAux --reverse-order --report Saved/SRDataset/TemporalAcceptance/replay-reversed.json
python Plugins/SuperResolutionDataset/Scripts/TestTemporalGeometry.py
```

FirstPerson 的地图参数使用 `/Game/FirstPerson/Lvl_FirstPerson`。Windows 下本机 UE 对启动拒绝记录了 `RequestExitWithStatus(0,1)`，但进程仍可能返回 0；自动化必须同时检查 `LogSRDataset: Error`、完成状态和新鲜输出，不能仅看进程码。

本次原始数据、日志、完整 JSON 结果、首次失败与修正后结果、热图均在项目 `Saved/Diagnostics/SRClosure-20260905/`。修改前备份在该目录 `baseline/`；先前审查负例仍在 `Saved/Diagnostics/SRPluginAudit-20260905/`。本轮没有修改或重认证先前采集数据。

## 下一步的目标

**继续完成 M1 的真实场景 reference 与数据交付验收。** 先在含真实纹理/Mip、光照、遮挡及受控透明材质的短场景中检查固定时间 reference 的质量与 LR 对应，明确空间超采样与根计划高采样 GT 的差异；然后建立按关卡/轨迹隔离的数据划分、机器可读准入清单，并完成异步写盘对照和规定的数据规模。以该证据决定局部补采样或捕获修正。

完成这些要求后进入无网络时间融合 baseline（clamp、history weight/age、反遮挡与 reset），再做 CNN。当前不扩大通用缓存、Chaos 内部状态、Transformer 或通用 FG。当前范围仍不证明跨 GPU、复杂随机光照、宽位实例身份、动态同实例遮挡、加载/分辨率变化 reset 或完整 FG 契约。
