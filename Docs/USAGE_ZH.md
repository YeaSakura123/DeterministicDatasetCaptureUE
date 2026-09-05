# 使用说明

当前正式版为 1.0.0。下载入口见 [GitHub Release](https://github.com/YeaSakura123/DeterministicDatasetCaptureUE/releases/tag/v1.0.0)，实际交付和支持范围见 [正式版验收](RELEASE_VALIDATION.md)。旧 API 与专项缓存说明见 [历史文档](LEGACY_USAGE_ZH.md)。

首次使用可下载独立 Demo，安装 Python 依赖后运行 `CaptureDemo.ps1 -Editor "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"`。它已在干净宿主上完成 8 帧 1080p/ref16 采集和验证。完整本地语料为 30 段、18,000 帧、10 分钟；GitHub 另提供 32 帧训练示例和可离线复验的证据包。

## 安装与采集

支持 Windows / UE 5.7 Editor 目标，包括 `UnrealEditor-Cmd -game -RenderOffscreen`。已在独立 BuildPlugin 包与空白宿主项目上生成场景并采集 1080p 数据。源码放进项目的 `Plugins/SuperResolutionDataset` 后构建 Editor；二进制包需要匹配 UE 构建版本。项目使用 DX12、SM6；当前验证环境启用 Substrate。

在插件目录执行：

```powershell
python -m pip install -r Scripts/requirements-validation.txt
powershell -ExecutionPolicy Bypass -File Scripts/RunDatasetCapture.ps1 `
  -Project "D:\Project\Project.uproject" `
  -Editor "D:\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  -Job "D:\Jobs\capture.json" -UseWorkspaceLocalDDC
python Scripts/ValidateDataset.py "D:/Project/Saved/SRDataset/capture"
```

JSON 的 `expectedMap` 指定关卡，`sequence` 指定镜头轨迹，输出目录必须是新目录。命令行退出码、日志、manifest 的完成状态都参与判定；引擎返回 0 不会掩盖插件失败。

## 时间超分输入

正式时间输入使用 `nr-sr-data-v2`，要求真实 Main View、连续帧、严格场景前检、固定曝光/逻辑时间/jitter、稳定组件 ID、完整 2× 分辨率和无可见世界 Widget。可从 `Config/job.production-2x.json` 开始，填写自己的地图和 Sequence。

- `color_lr_scene_hdr`：Main View AfterDOF、升采样前的 pre-exposed 线性 HDR。
- `color_hr_native_scene_hdr`：同状态的原生 HR。
- `color_hr_reference_scene_hdr`：可选的冻结时间 reference；`referenceTemporalSamples` 支持 16、32、64，默认兼容值 1 只供旧诊断。
- `depth_view_linear_meters`：训练用的视空间米制深度；`depth_device_raw` 是 Reversed-Z。
- `motion_full_current_to_previous`：当前像素指向上一帧、单位 display pixel，已移除几何运动中的 jitter。
- 使用保存的矩阵计算前后栅格 jitter，换算运动到 render pixel 后再采样上一帧；`TemporalGeometry.py` 提供实现。
- 必须处理 `reset`、`motionTrainingUsable`、深度/运动有效性，以及 `history_rejection_valid`。动态同组件自遮挡无法判定的像素会被保守拒绝。

`hr/lr` PNG 是 SDR 预览；旧 `depth` 是厘米，不能直接当训练深度。曝光、相机矩阵、实际提交时间和子样本投影都在 manifest 中。

写盘在工作线程压缩并原子发布图像，内存预算由 `maxPendingImageWriteMB` 控制。GPU 读回仍然同步。manifest 只发布已写完且有哈希的帧，Windows 替换支持短暂读占用。持久写入失败会使任务失败。`bWriteManifestEveryFrame=false` 可避免长片段反复重写增长中的清单。

## 批量数据与训练分片

`GenerateFormalDatasetAssets.py` 在 UE Python 环境生成四个原创测试场景及其轨迹，只使用引擎基础网格与脚本生成的纹理。执行方法见 [README](../README.md)。

```powershell
python Scripts/GenerateFormalCapturePlan.py --output Jobs/v1 --version v1
python Scripts/RunDatasetBatch.py Jobs/v1/plan.json `
  --project "D:/Project/Project.uproject" `
  --editor "D:/UE_5.7/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" `
  --status Jobs/v1/status.json --workspace-ddc
python Scripts/DatasetDelivery.py index Jobs/v1/plan.json `
  --project "D:/Project/Project.uproject" --purpose temporal-sr --output Delivery/v1/index.json
python Scripts/DatasetDelivery.py pack Delivery/v1/index.json --output Delivery/v1/shards --profile temporal-sr
python Scripts/DatasetDelivery.py verify-shards Delivery/v1/shards/shards.json
```

计划按轨迹分离训练/验证集，测试地图不进入训练或验证。生产器只标记待验证；索引工具逐帧检查后才准入。索引与分片绑定 manifest、验证器和所有文件哈希；失败数据不能发布正式索引。`DatasetDelivery.iter_samples()` 逐样本读取分片，并保留 clip、split 与 reset 边界。

`--reuse-completed` 只复用批次收据中完整、未变更的片段。失败片段保留，换新输出目录重新采集。时间采集不支持从中间恢复 history。

## FG 三进程与项目 WPO 检查

运行 `Config/FGProjectWPO/forward.json`、`reverse.json`、`midpoint.json` 前先生成上述四个场景；每个 JSON 用独立进程采集，并分别运行 `ValidateDataset.py`。然后执行：

```powershell
python Scripts/AssembleFrameGenerationDataset.py `
  --endpoints "D:/Project/Saved/SRDataset/ProjectWPO/forward" `
  --reverse-endpoints "D:/Project/Saved/SRDataset/ProjectWPO/reverse" `
  --intermediate "D:/Project/Saved/SRDataset/ProjectWPO/midpoint" `
  --output "D:/Project/Saved/SRDataset/ProjectWPO/assembled"
python Scripts/ValidateFrameGenerationDataset.py "D:/Project/Saved/SRDataset/ProjectWPO/assembled"
python Scripts/VerifyProjectWPO.py `
  "D:/Project/Saved/SRDataset/ProjectWPO/forward" `
  "D:/Project/Saved/SRDataset/ProjectWPO/reverse" `
  "D:/Project/Saved/SRDataset/ProjectWPO/midpoint" --report wpo-physical.json
```

反向运动由反向重放实际渲染，不是正向场取负；中间帧使用独立历史。输出保留真实 jitter/投影、双向运动、深度、遮挡判定有效性、HUD-less Color 和单独 UI Color/Alpha。普通场景不要求存在测试夹具、骨骼或可见 UI。基础装配保留证据缺项；项目证据齐全且独立验证全部通过后，报告才给出限定范围的准入结果。详见 [FG 验收与证据用法](FG_ACCEPTANCE.md)。

## 范围

大规模时间训练分片可使用 `DatasetDelivery.py pack <index.json> --output <new-directory> --profile temporal-sr`，保留契约输入、对应有效性/遮挡掩码和全部时序元数据；诊断预览和重复 HR 继续留在母版。省略 profile 时保留所有模态。打包过程仍核对全部母版文件的哈希。

固定组件拓扑支持最多 255 个实例标签。透明区域输出风险掩码，不承诺透明表面的单层深度/运动真值。严格前检中的精确类白名单是已审计例外，不表示插件控制了该类的任意行为。自定义外部状态、任意 WPO、任意 Niagara DI、完整 Chaos 求解器、cooked IoStore、立体和分屏不在当前已验证范围内。
