# 性能需求与架构闭合审查

日期：2026-11-17。此记录是文档/解析预算审查，不是gem5运行或目标达标证据。

## 发现与处理

| 发现 | 原问题/影响 | 本次处理 | 执行状态 |
|---|---|---|---|
| PERF-GAP-01 | P-ID主要是测量定义，数值目标分散在COMP表 | 需求§6.4集中P-01–13数值、条件、证据；§6.5固定profile约束 | 文档闭合；M3执行待实现 |
| PERF-GAP-02 | 默认target128被误读为64B应用吞吐承诺 | 需求§6.6列Read/Write/4KiB的参考上限，区分不可达输入 | 解析已核算，非模拟 |
| PERF-GAP-03 | Fabric raw speedup未计短包Cell膨胀/metadata | 硬件§12.3按Cell算1911.6 GB/s需求，标旧配置不足，给连续/整Cell候选 | 预算闭合；实际bank/仲裁与PPA待验证 |
| PERF-GAP-04 | core/pin/RTT无逐阶段/residual预算 | 硬件§12.2分配70-cycle候选；§12.6明确pin/250ns仍未闭合 | core预算待实现；RTT/pin OPEN |
| PERF-GAP-05 | 保守ordering可能与non-blocking/尾延目标冲突 | 硬件§12.5要求合法scope/越过trace和实测，不剔除ordering损失降低分母 | M3必须验证，未证明 |
| PERF-GAP-06 | 吞吐/尾延基线、样本、默认值与结果混同 | 独立B_ref/R_ref、采样门槛、状态分层及PerformanceCase | M3 checker待实现 |

## 本次实际解析检查

读取HTML内嵌`hardware-sizing.mjs`，通过隔离Node VM运行其现有
`sizeHardware({...hardwareDefaults, ...diff})`；未修改页面、未运行事件
模拟。结果为页面估算，不是完整Gen6 FLIT或芯片实测：

| diff | RTT预算ns | 协议payload上限GB/s/发起端口 | portWidth bit | Fabric bit×通道 | 首限制 |
|---|---:|---:|---:|---|---|
| 默认64B Read | 250 | 99.3684 | 2112 | 1472×9 | Link/FLIT/TLP cut |
| op=write，64B | 250 | 94.4 | 2016 | 1472×9 | Link/FLIT/TLP cut |
| payload=4096 | 250 | 112.7164 | 1504 | 1472×9 | Link/FLIT/TLP cut |
| payload=4096，mrrs=512 | 250 | 112.7164 | 1504 | 1472×9 | Link/FLIT/TLP cut |
| reqPorts=8，target=25 | 250 | 12.4211 | 2112 | 1472×9 | 汇聚cut |

上述默认四组target=128均超过对应页面协议上限；8→1例target=25也超过
每发起端口12.4211的共享cut。这是正确的“目标受限”诊断，不能放宽
硬件信用或缓存来伪造达到需求带宽。

独立手算复核：118 GB/s TLP服务，64B Write的80B TLP→2×72B Cell，
每port1.475 G TLP/s、212.4 GB/s内部服务，9口1911.6 GB/s；20%余量
2293.92 GB/s。连续9×2400-bit/1GHz/85%=2295，整Cell实现须检查lane
量化；9通道每拍5个72B Cell等价2880-bit、85%后2754 GB/s。

Tag例：99.3684×250/64的100%协议上限窗口向上取整389，95%为369；
ACK RTT185ns的raw128、20%余量Replay参考需求111个256B等价槽，取128。
这些均只是条件化下界，真实排队/协议group/额外metadata仍需模型验证。

## 验证边界

本次检查P-ID唯一性、目标/机制映射、算术、链接、文档结构和whitespace。
原63字段、HTML与M0源代码/测试未改；M0状态不升级M1–M5。P-11/12
缺业务目标，P-04/13缺预算/校准，均不能标为已满足。
