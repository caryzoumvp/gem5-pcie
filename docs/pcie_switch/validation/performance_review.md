# 性能需求与架构闭合审查

最新审查：2026-11-19，M0–M3 一键验证的 gem5 运行产物。
2026-11-17 的 HTML 解析预算保留在文末。

## 2026-11-19 gem5 运行审查

证据目录为仓库根下的 `gem5-pcie-validation-fprstupj/`。运行日志记载
gem5 26.0.0.0；两份合同报告记录 Git HEAD 为 `4abd006bea`，工作区
有改动。性能 profile 内的 Base Spec SHA-256 与当前文件一致；
HTML SHA-256 为 `7b9ab025…ce804`，与 Git HEAD 版本一致。
审查时工作区 HTML 为 `63070fa2…4fbb2`，归档的页面验证只适用于
原 SHA-256。复核核对了 `performance-verdicts.json` 和各用例的
`performance.json`、`tail-result.json`、`tail-trial.json`。

68 项 C++ GTest、38 项 Python 测试及 54 项 TestLib 检查通过。
26 个常规性能用例与 2 个尾延用例的数据正确性均为 `PASS`。
性能判定共 21 个 `PASS`、6 个 `FAIL` 和 1 个 `NOT_COMPARABLE`。
6 个 `FAIL` 包含预期的三通道容量反例；其余 5 个是读吞吐目标未达。

### 与现行需求的逐项差距

表中的状态限定在已归档 profile。各档性能通过、未达、待测
或不可比与协议及数据正确性独立。
[需求 §6.4–6.6 和 §11.1](../PCIe6_Switch_Requirements.md)

| 需求与验收线 | 当前证据 | 差距与闭合条件 |
|---|---|---|
| P-01，x16 单向 128 GB/s，9 口 TX raw 1152 GB/s | 通过模型账本复核。九口 Write 方向各为 1,500 个 256 B FLIT / 3 µs，配对回程区分数据和控制 | 1152 GB/s 是九方向 raw 和，缺独立 P-01 报告行；保留与有效载荷、跨拓扑 cut 和 PHY 实测的边界 |
| P-02，空载单跳 `L_core <100 ns` | SW-LAT 八档 PASS，64/256 B 读写同 Slice 10 ns、跨 Slice 12 ns，按合法入口至 TX 接受的最慢 TLP 判定 | 目标在已测 profile 闭合。若用于 COMP 冻结，需逐类汇总 MemRd/MemWr/CplD 的时延和阶段分项 |
| P-03，实际 `L_pin` 分项 | NOT_RUN，无 pin 时间戳。90/100/125/150 ns 为探索输入 | 冻结 RX/TX bit 边界和包型，补 PHY/PCS/FEC、FLIT 等待及 CDC 分项和实际证据 |
| P-04，`RTT_XPU <250 ns` | NOT_COMPARABLE。RTT-64 仅给出合成端点的 53 ns 和 core trace | 给出同测点的实端服务预算，分别量出请求/返回链路、两次 core 过境和端点服务，并闭合互斥 RTT 分项 |
| P-05，≥4 KiB 且 ≥95% 独立 `B_ref` | 4 KiB Write 两种回程、延迟回程 Read 通过；配对回程 Read 为 99.669 GB/s，距 107.081 GB/s 门槛差 **7.411 GB/s** | 固定回程、packing、MPS/MRRS/RCB 与窗口，隔离共享反向串行器上的控制预算；大于 4 KiB 和配对回程下的 MRRS 4096 B 对照未运行 |
| P-06，64 B 读写各达 ≥95% `B_ref` | Write 已测档通过。单流 Read 延迟/配对分别比 60.800 GB/s 门槛少 **12.800/14.805 GB/s**；8→1 有 5/8 流低于 7.600 GB/s，2 流窗口零完成 | 隔离装填与供包限制，加逐流 grant/信用/队列证据，复测单流和 8→1；8→1 配对回程未测 |
| P-07，九口合法置换无额外 core 瓶颈且 ≥95% `B_ref` 总和 | 同/跨 Slice Write 通过。跨 Slice Read 距 547.200 GB/s 门槛差 **259.200 GB/s**；三通道 Write 为预期容量反例 | 同时核对 Request/Cpl grant、bank/metadata 及链路装填，补同 Slice Read 和配对回程 Read；输出聚合门槛和逐流达成率 |
| P-08，70% `R_ref` 加载 P99 ≤2×基线 | 两种回程的非过订阅 64 B Write 均 PASS，10 ns ≤24 ns，各 10,800 样本 | 该 P99 profile 闭合；突发/热点为独立结果。当前未交付针对 Read/混合流的尾延，也未达到 P999 的 100,000 样本门槛 |
| P-09，广播源只注入一份，扫描扇出 2/4/8/16/32 | UNSUPPORTED，归档中没有复制路径 | M5 扫描扇出，分别证明入口一份、出口总字节和慢成员占用守恒 |
| P-10，选定 collective 净收益 >1，扫描 1.2/1.5/2× | UNSUPPORTED，归档中没有闭环 collective 或 INC | M4 建立同拓扑软件算法基线和端点同步/归约成本，M5 对已验证功能做端到端对照 |
| P-11，面积/功耗/温度 | NOT_RUN，产品门槛待定 | 先定义工艺/PVT 和负载口径，再提交独立 PPA 证据 |
| P-12，RAS 恢复 SLA/失败率 | NOT_RUN，已有 CRC/丢 ACK 恢复正确性，SLA 门槛待定 | 产品输入确定故障范围和门槛；另测隔离、恢复时间和失败率 |
| P-13，x16→x16 C/T ≤90 ns | UNSUPPORTED，当前为 S/F，缺 CT-PIN 运行和 PHY 校准 | 实现安全 C/T，取得 COMP-04 包型与 bit 边界，冻结 CT-PIN 并用校准证据复测。§11.1 通用 `L_pin` 为首 bit 到首 bit，checker 的 `pin_first_to_last_bit` 仅为候选口径 |

### 跨用例的验收证据缺口

- **§6.1/§6.6 参考分母。** 当前 `B_ref` 采用连续的 236/256 TLP 区
  与合成端点 cut，可独立复算；尚缺按合法 packing、空闲和控制策略
  逐方向预算的第二层参考，以及 Tag、FC、replay 和 core 约束排序。
  双向读置换每方向同时承载请求和 Completion，独立预算应合计两类
  TLP。按本次对齐头计 12+76 B/笔，理想 1 G 笔/s 需 88 GB/s，
  低于 118 GB/s TLP 区预算。在理想装填假设下，当前端点上限仍为
  64 GB/s。装填和控制开销应作为可比 profile 参数预先冻结，保持分母
  独立于被测 core 实测吞吐。
- **§6.2/§6.3、F-19 和 A-07 输出覆盖。** 归档已有逐流 GB/s/IOPS、
  每方向 P/NP/Cpl TLP/s、FLIT/占线、计数器和部分资源峰值；
  尚缺统一逐流 P50/P99/P999 与样本/不足标记、Tag stall/峰值，
  以及 VOQ/pool/egress/active-VOQ 峰值和容量对照。逐链路信用/
  ACK RTT、H/D backing、replay 需求/配置和完整限制项排序
  也应在同一份结果中关联。P-08 的命中只针对已有 cohort 与 P99。
- **§8.1/§11.1、F-09/F-21 结果身份。** 单次运行有 profile、窗口、
  tick、模式和源哈希，合同 JSON 另存 HEAD/dirty。完整 ResultBundle
  尚需绑定编译版本、实验及校准来源、测点/单位、seed（有随机输入时）
  和硬件适用范围。当前报告的合成端点没有型号/stepping、固件或 PVT，
  只能支撑模型结论；也未生成已冻结的 `competitor_target` 记录。
- **§12 的工作负载性能。** V-35 的 gem5 周期证据已归档，当前样本
  未经过 SimAI/SimCCL。通信/collective 和依赖闭环的模型数据、
  逐 link 热点、step/TTFT/TPOT 与两仓库版本关联仍待
  SIM-02–SIM-16，尤其 SIM-14 的输出合同。

### 测点与分母

- 吞吐在 1 µs 预热后测量 3 µs。统计目标端可见的 Write payload
  与源端完整组装的 Read payload，按整笔应用事务计数。所有吞吐用例
  窗口结束时每流有 64 笔未完成，最终排空且等待图为空。
- 参考分母使用 x16 的 128 GB/s 单向 raw、236/256 B 的 TLP 区、
  对齐头开销及合成端点服务上限。64 B 单目标每 1 ns 一笔，
  上限为 64 GB/s，8→1 的等分目标为每流 8 GB/s。九个独立目标
  的置换上限为合计 576 GB/s。4 KiB 场景上限为 112.716 GB/s。
  目标按每流独立分母的 95% 判定。
- 延迟回程以定时事件交付 ACK/NAK，配对回程让控制 NOP FLIT 与
  反向数据共用串行器。两者的 FC Update 均为协调事件。活跃方向
  的窗口计数均满足原发、重发和控制每个 256 B FLIT 占 2 ns。
  回程策略与装填效率单独审查，参考分母保持一致。

### 目标判定

吞吐为聚合有效载荷，单位 GB/s。多流的逐流约束单列。

| 目标与用例 | 实测 / 参考 | 判定 |
|---|---:|---|
| P-02 SW-LAT，64/256 B Read/Write，八档配对回程 | 同 Slice 10 ns，跨 Slice 12 ns / <100 ns | PASS，合法 core 入口至 TX 接受 |
| P-04 RTT-64 | 合成 Read 提交至可消费 53 ns | NOT_COMPARABLE，缺匹配的 PHY、端点证据和预算闭合 |
| P-05 4 KiB Write，两种回程 | 113.323 / 112.716 | PASS；窗口以整笔事务切分 |
| P-05 4 KiB Read，延迟回程 | 111.957 / 112.716（99.33%） | PASS；MRRS 4096 B、replay 16 格分别对照，吞吐相同 |
| P-05 4 KiB Read，配对回程 | 99.669 / 112.716（88.42%） | FAIL |
| P-06 64 B Write，单流两种回程、8→1 延迟回程 | 64.000 / 64.000 | PASS；8→1 每流 8.000 |
| P-06 64 B Read，单流延迟 / 配对回程 | 48.000 / 64.000；45.995 / 64.000 | 两者 FAIL |
| P-06 64 B Read，8→1 延迟回程 | 48.000 / 64.000 | FAIL；逐流 13.888、13.589、12.651、6.635、0.853、0.384、0、0 |
| P-07 64 B Write，九口同 / 跨 Slice，跨 Slice 配对回程 | 576.000 / 576.000 | PASS；每流 64.000 |
| P-07 64 B Read，九口跨 Slice，延迟回程 | 288.000 / 576.000 | FAIL；每流 32.000 |
| P-07 九口 Write，三通道反例 | 192.000 / 576.000 | 预期 FAIL；每流 21.333 |
| P-08 SW-TAIL，64 B Write，两种回程 | 空载 P99 12 ns，加载 P99 10 ns / ≤24 ns | PASS；两组各 10,800 样本 |

4 KiB Write 的应用计数为 83 笔，目标方向同一窗口接受 338,176 B
TLP payload（112.725 GB/s）。应用完成和链路接受的边界相差
1,792 B。113.323 GB/s 较解析分母高 0.606 GB/s，处于一笔
4 KiB 事务对应的窗口误差量级（约 1.365 GB/s）。该差值不能用来
提高持续吞吐上限。

### 读吞吐与内部服务

- **P-05 大包读的反向控制开销。** 两条反向数据链路均占用全部
  1,500 个 FLIT 时隙。配对回程中分别有 172 和 169 个控制
  FLIT，占用约 11.5% 和 11.3%。切换回程后吞吐下降约 11.0%，
  core 各类 stall 计数为零。该对照把差值定位到当前回程策略的
  链路争用；此结果只覆盖独立 NOP 控制的实现。
- **P-06 短包读的装填效率。** 单流延迟回程中，两段链路的请求和
  Completion 方向均已占满 1,500 个 FLIT 时隙，各传 2,250
  个 TLP；平均请求方向只有 18 B TLP/FLIT，Completion 方向
  114 B/FLIT。配对回程各方向另有 187 个控制时隙。欠填充与
  1 ns flush 窗口和供包节奏一致，当前对照未隔离各自贡献。
- **P-06 8→1 的流间不均衡。** 目标反向链路 100% 占线，正向
  为 70.2% 且记录 15,897 次信用停顿。core 记录 21,377 次
  staging stall 和 738,559 次 admission stall；计数是尝试
  次数，admission 还包含写 bank 忙、队列或资源预约失败等原因。
  两个流在窗口内零完成，最终仍全部排空。聚合带宽和排空结论
  均不足以证明公平性。
- **P-07 九口双向读的授权量。** 3 µs 内有 27,000 个 grant，
  等于 9 个/cycle。每笔 64 B Read 消耗 Request 和 Completion
  两个 grant，当前授权量对应 288 GB/s；要达到 95% 门槛，至少
  需要平均 17.1 个 grant/cycle，同时满足 bank 与链路约束。
  读置换的端口方向混合两类 TLP，正/反向占线约 98.5%/100%，
  实际平均约 88 B TLP/FLIT。扩展授权量后仍须独立复测装填
  和入端服务，当前没有配对回程读置换证据。
- **P-07 三通道对照。** 九口 Write 从 27,000 降到 9,000
  个 grant，聚合吞吐从 576 降到 192 GB/s。两个配置每通道
  单拍分别容许 5 和 3 个 cell；该包型只需 2 个 cell，两档均
  容得下一笔。结果验证了该原生配置下同时授权通道数对吞吐
  的影响。旧解析审查使用 72 B cell，本次 gem5 为 64 B cell；
  两套预算应分别追踪。

### 尾延与剩余边界

SW-TAIL 为九流非过订阅 Write，每流以 1/7 笔/ns 为独立参考，
加载为 0.1 笔/ns（70%）。三个窗口内每流各有 400 笔入核、出核
及目的完成，两组均无删失。加载时每流的 core/端到端积压在
各边界分别保持 1/3 笔。阶段 P99 中，core-ready 至写开始和
TX-ready 至 TX 接受分别由 1 ns 变为 0 ns。该差异只适用于
本次注入间隔和边沿相位；P-08 的门槛是相同测点下不超过两倍
空载 P99。共享出口、突发、Read 及混合流量尾延仍缺测量。

P-01 已有模型串行化计数与独立 raw 预算对照。P-03/P-04/P-13
仍缺 PHY/pin/XPU 证据，P-13 还缺安全 C/T 运行路径。P-09/10
由后续扩展评估，P-11/12 仍缺产品数值。下一轮复核可用独立扰动
分别隔离 flush 与供包速率、按流记录 grant/信用/队列等待，并
在固定分母下验证同时提升 read grant 与入端服务率的效果。
阶段范围与复现入口见 [M3 验收](M3.md) 和 [一键验证](../validation.md)。

## 2026-11-17 原始解析审查

### 当时的发现与处理

| 发现 | 原问题/影响 | 本次处理 | 执行状态 |
|---|---|---|---|
| PERF-GAP-01 | P-ID主要是测量定义，数值目标分散在COMP表 | 需求§6.4集中P-01–13数值、条件、证据；§6.5固定profile约束 | 文档闭合；M3执行待实现 |
| PERF-GAP-02 | 默认target128被误读为64B应用吞吐承诺 | 需求§6.6列Read/Write/4KiB的参考上限，区分不可达输入 | 解析已核算，非模拟 |
| PERF-GAP-03 | Fabric raw speedup未计短包Cell膨胀/metadata | 硬件§12.3按Cell算1911.6 GB/s需求，标旧配置不足，给连续/整Cell候选 | 预算闭合；实际bank/仲裁与PPA待验证 |
| PERF-GAP-04 | core/pin/RTT无逐阶段/residual预算 | 硬件§12.2分配70-cycle候选；§12.6明确pin/250ns仍未闭合 | core预算待实现；RTT/pin OPEN |
| PERF-GAP-05 | 保守ordering可能与non-blocking/尾延目标冲突 | 硬件§12.5要求合法scope/越过trace和实测，不剔除ordering损失降低分母 | M3必须验证，未证明 |
| PERF-GAP-06 | 吞吐/尾延基线、样本、默认值与结果混同 | 独立B_ref/R_ref、采样门槛、状态分层及PerformanceCase | M3 checker待实现 |

### 当时的解析检查

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

### 原始审查边界

本次检查P-ID唯一性、目标/机制映射、算术、链接、文档结构和whitespace。
原63字段、HTML与M0源代码/测试未改；M0状态不升级M1–M5。P-11/12
缺业务目标，P-04/13缺预算/校准，均不能标为已满足。
