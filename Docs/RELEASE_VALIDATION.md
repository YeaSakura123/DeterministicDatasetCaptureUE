# 1.0.0 正式版验收 / Release validation

2026-09-06，UE 5.7.4 CL 51494982 / Windows Editor / DX12 SM6。交付对象是数据捕获插件及其可验证数据流程；CNN、Transformer 和实时推理按根项目后续阶段推进。

[正式版下载](https://github.com/YeaSakura123/DeterministicDatasetCaptureUE/releases/tag/v1.0.0) · [机器可读结果](Validation/release-1.0.0.json) · [需求核对](RELEASE_PLAN.md)

## 已完成的数据交付

| 划分 | 地图与轨迹 | 帧数 | 采样时长 |
|---|---|---:|---:|
| Train | Gallery、Courtyard、Workshop，各 8 条轨迹 | 14,400 | 480 s |
| Validation | 同三地图各一条独立轨迹 | 1,800 | 60 s |
| Test | 未进入训练/验证的 AtriumTest，3 条轨迹 | 1,800 | 60 s |

总计 30 段 × 600 连续帧，30 fps，10 分钟。真实 Main View LR 为 960×540，原生 HR 为 1920×1080；每地图首条轨迹另保存 16 样本冻结 reference，共 2,400 帧。其余 15,600 帧使用原生 HR 作为 GT，具体以每段 `trainingInputMapping` 为准。

完整语料通过 **3,945,814 / 3,945,814** 检查，按 `nr-sr-data-v2` 准入。已生成 **144 个训练分片，264,333,998,080 bytes（246.18 GiB）**，并重新读取全部 18,000 个样本：分片 SHA-256、所有样本文件 SHA-1、模态完整性、连续帧、clip/split/reset 均通过。母版保留全部诊断文件；`temporal-sr` 分片保留训练模态及完整时序元数据。

完整母版与上述分片为本地交付。公共 Release 包含 32 帧分片示例和完整语料的准入报告，不将小示例冒充完整语料，也不包含作者私有项目资产。

## 功能证据

| 验收项 | 实测结果 |
|---|---|
| 相机/物体/混合、cut、首人称等核心回归 | 8 组共 472 帧及独立几何检查通过 |
| 重复重放与反序附加捕获 | 各 1,792 文件对字节一致；保存 192 张实际差异 heatmap |
| 相机与物体同速 | 物体运动残差 <0.00364 display px；背景解析误差 <0.000080 px |
| 冻结 reference | 支持 1/16/32/64；最终包 16 样本独立平均与面积 resolve 两帧误差均为 0；真实纹理收敛和 LR history 隔离通过 |
| Main View/SceneCapture 像素对照 | 最终包 8 相位 2,072/2,072；全图原有 5% 归一化 MAE、20 dB PSNR 门槛保留 |
| 异步写盘 | 有界 raw 队列与回压；渲染线程零磁盘写入；错误传播和原子清单发布通过；同步 GPU 读回仍保留 |
| FG 三进程、项目 WPO/骨骼/材质证据 | 随包独立复验 199/199、192/192；六类缺项/伪造负例全部拒绝 |
| 最终插件构建与独立安装 | BuildPlugin 成功；空项目安装、独立 Demo 各 1,912/1,912；两项引擎自动测试通过 |
| 32 帧训练 profile | 四地图全部读回，训练图像字节和时序元数据与母版一致 |

`ValidationEvidence` 包可直接运行其中 README 的命令，复算 FG 证据、Main View 像素对照和实际 reference 子样本，无需原作者项目。原始 Shooter 光照下的材质失败、旧像素方向检查失败、写盘故障和坏材质前检也保留，未以降低阈值替代修复。

## 来源与限制

完整语料在发布候选二进制上采集，清单仍保留原始 `1.0.0-rc.1` 及其二进制哈希；最终 1.0.0 打包的 22 个 C++/Shader 文件与采集源码一致，等价报告包含双方二进制哈希。没有修改旧数据来源来伪装重新采集。

全量准入锁定 validator v23，其精确源码在 [基线压缩包](Validation/validator-v23-baseline.zip)。当前 v24 修正可选测试夹具的不透明像素方向评分；完整语料未开启该选项，四地图 32 帧的普通门槛与统计完全等价。[详细说明](Validation/README.md) 记录原失败和修正范围。Python 源码统一 LF，报告中的来源哈希可跨平台保留。

受支持范围为严格场景控制、固定拓扑、最多 255 个稳定组件 ID 的 UE Editor 流程。FG 只在完整项目证据通过后授予报告声明的范围准入。透明/reactive 区域不能作为不透明表面对应真值；无法确认逐表面身份的动态自遮挡保持 invalid。任意 GI 历史、自定义仿真和外部随机状态仍不自动确定。Cooked game、stereo、split-screen 不在已验证支持范围。

## 下一目标

按根执行计划第 4 节进入非神经时间重建基线：读取这些分片，完成 jitter-aware 重采样、运动重投影、深度拒绝、neighborhood clamp、历史权重与 cut/reset；输出 current-only、history、遮挡、权重和历史年龄，并在三个训练地图及未见测试地图上评估收敛、错位与鬼影。此阶段通过后再训练 CNN，无需先扩展 Chaos 或 Niagara 专项能力。
