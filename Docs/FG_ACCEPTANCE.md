# FG 数据准入与已验证范围

插帧数据由三个独立进程产生：正向端点、反向端点和真实中间帧。装配器保存各自真实投影/jitter、双向运动、深度、有效性掩码及独立 UI。反向场由渲染得到，中间帧不进入端点历史。

`AssembleFrameGenerationDataset.py` 不自行认证数据。缺少项目证据时清单保留 `experimental_uncertified` 和 `missingRequirements`；证据齐全时为 `pending_dataset_validation`，`frameGenerationCertified` 仍为 false。随后运行验证器，全部检查通过且证据齐全，报告才输出 `certificationGate=supported_scope_pass`、`frameGenerationCertified=true` 和明确的 `certificationScope`。完整性 PASS 与这一准入结果是两个不同字段。

## 当前实测

2026-09-05，同一 UE 5.7 Windows Editor 插件二进制：

| 项目 | 结果 |
|---|---|
| 语义测试场景装配、证据和准入 | 199/199 |
| 原创 Gallery WPO 普通场景装配、证据和准入 | 192/192 |
| 项目 AnimBP 正/反向原始捕获 | 各 519/519 |
| 项目 AnimBP 反向对照 | 598/598 |
| 受控 Gallery 光照中的项目动态材质反向对照 | 1897/1897 |
| WPO 独立物理验证 | 正反运动最大误差 <0.00374 display pixel；中点轮廓误差 <0.264 render pixel |
| 准入负例 | 缺少证据、修改证据哈希、伪造物理结果、关闭严格前检、生产端自行认证、缺少栅格信息，6/6 拒绝 |

项目材质在 Shooter 地图的历史光照下曾有三个颜色对照项失败，原始失败数据保留。本表的通过结果对应 Gallery 受控光照，不据此保证任意时间 GI/反射历史都可反向重现。

## 使用证据

先运行 [中文说明](USAGE_ZH.md) 中的三进程捕获及各自的 `ValidateDataset.py`。基础装配命令可用于检查普通场景，项目证据为可选附加参数：

```powershell
python Scripts/AssembleFrameGenerationDataset.py `
  --endpoints Captures/forward --reverse-endpoints Captures/reverse `
  --intermediate Captures/midpoint --output Delivery/fg `
  --project-wpo-replays Proof/WPO/forward Proof/WPO/reverse Proof/WPO/midpoint `
  --project-skeletal-forward Proof/Skeletal/forward `
  --project-skeletal-reverse Proof/Skeletal/reverse `
  --project-animated-material-forward Proof/Material/forward `
  --project-animated-material-reverse Proof/Material/reverse
python Scripts/ValidateFrameGenerationDataset.py Delivery/fg
```

WPO 证据使用 `Config/FGProjectWPO` 与原创 Gallery 资产；验证器读取随交付包保存的深度、基础色、运动及覆盖率文件，重新计算正反运动和中点轮廓。骨骼与动态材质的配置模板在 `Config/job.nonfixture-skeletal-*.json`、`Config/job.project-animated-material-*.json`，其中项目资源路径必须改为自己项目里的真实资产。它们需要独立通过 `--compare-mode skeletal-reverse` / `material-reverse`，并将报告命名为 `validation_report_skeletal_reverse.json` / `validation_report_material_reverse.json`，保存在正向输出目录。骨骼证据还包含实际使用的姿态缓存文件。

所有附加证据须使用与主数据相同的插件二进制。装配保留原始清单、报告及哈希；验证器检查报告与源清单、当前验证器版本的绑定。升级验证器后，应重跑源验证与比较，再装配到新目录。发布包若包含已装配示例，可以直接验证它，无需安装作者项目中的资产。

## 训练时必须遵守的范围

固定拓扑、严格场景前检、逻辑时间/曝光/jitter 锁定、最多 255 个组件 ID。运动及可见性判定只在对应 validity=1 时有效；透明和 reactive 区域应从不透明表面对应损失中排除。无法确认同一组件内部表面身份的自遮挡继续输出无效判定。验证通过不会把这些未知像素变成真值，也不认证任意自定义仿真、外部随机状态、任意 WPO 或任意历史光照。

可对随包的完整示例运行 `TestFrameGenerationAdmission.py <dataset> --output <new-negative-evidence-directory>`，确认失败数据不能获得准入结果。
